/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:33:27
 */

#include "utils/TimeUtils.h"
#include "utils/TimeSyncValidation.h"
#include "utils/SystemTimeQuality.h"
#include "utils/SystemTimeZone.h"
#include "utils/StringUtils.h"
#include "core/FeatureFlags.h"
#include <Ticker.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <map>
#include <new>
#include <atomic>

// 定时器管理
std::map<int, Ticker*> timers;
int nextTimerId = 1;

namespace {

extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

constexpr time_t MIN_VALID_TLS_TIME = 1577836800;  // 2020-01-01 UTC

std::atomic_flag httpTimeSyncBusy = ATOMIC_FLAG_INIT;
class HttpTimeSyncGuard {
public:
    HttpTimeSyncGuard() : locked_(!httpTimeSyncBusy.test_and_set(std::memory_order_acquire)) {}
    ~HttpTimeSyncGuard() { if (locked_) httpTimeSyncBusy.clear(std::memory_order_release); }
    bool locked() const { return locked_; }
    HttpTimeSyncGuard(const HttpTimeSyncGuard&) = delete;
    HttpTimeSyncGuard& operator=(const HttpTimeSyncGuard&) = delete;
private:
    const bool locked_;
};

// 一次性回调可能递归创建下一个定时器；在用户回调仍执行时禁止回收
// 外层 Ticker，避免底层 esp_timer 回调栈上的自删除。
std::map<int, bool> timerCallbacksRunning;

struct TimerCallbackGuard {
    explicit TimerCallbackGuard(int timerId) : id(timerId) {
        auto it = timerCallbacksRunning.find(id);
        if (it != timerCallbacksRunning.end()) {
            it->second = true;
        }
    }

    ~TimerCallbackGuard() {
        auto it = timerCallbacksRunning.find(id);
        if (it != timerCallbacksRunning.end()) {
            it->second = false;
        }
    }

    int id;
};

// Ticker::once_ms() 完成后对象本身不会自动释放。创建新定时器前回收
// 已经停止的实例，避免周期性 setTimeout() 调用把已完成对象留在 map 中。
void cleanupInactiveTimers() {
    for (auto it = timers.begin(); it != timers.end();) {
        Ticker* ticker = it->second;
        auto callbackIt = timerCallbacksRunning.find(it->first);
        const bool callbackRunning = callbackIt != timerCallbacksRunning.end() && callbackIt->second;
        if (ticker == nullptr || (!ticker->active() && !callbackRunning)) {
            delete ticker;
            if (callbackIt != timerCallbacksRunning.end()) {
                timerCallbacksRunning.erase(callbackIt);
            }
            it = timers.erase(it);
        } else {
            ++it;
        }
    }
}

}

bool TimeUtils::initialize(const TimeZone& timezone, const String& ntpServer) {
    FastBee::SystemTimeQuality::installSntpObserver();
    // 配置时区
    if (!setTimeZone(timezone)) {
        return false;
    }
    
    // 判断是否为 HTTP URL 格式的 NTP 服务器
    if (ntpServer.startsWith("http://") || ntpServer.startsWith("https://")) {
        return syncNTPFromHTTP(ntpServer);
    }
    
    // Capture before configuration; a fast SNTP callback can complete before
    // syncNTP enters its wait loop.
    const uint32_t syncSequence = FastBee::SystemTimeQuality::sequence();
    // 配置NTP
    if (!FastBee::SystemTimeZone::startSntp(nullptr, ntpServer.c_str(),
            "time.nist.gov", "time.google.com")) return false;
    
    // 等待时间同步
    return syncNTP(10000, syncSequence);
}

bool TimeUtils::syncNTP(unsigned long timeout, uint32_t afterSequence) {
    const uint32_t initialSequence = afterSequence == UINT32_MAX
        ? FastBee::SystemTimeQuality::sequence() : afterSequence;
    unsigned long startTime = millis();
    
    while (millis() - startTime < timeout) {
        const auto status = FastBee::SystemTimeQuality::snapshot();
        if (status.synced && status.sequence != initialSequence) {
            return true;
        }
        delay(100);
    }
    
    return false;
}

bool TimeUtils::syncNTPFromHTTPWithTimestamp(const String& url, long long& outTimestampMs, unsigned long timeout) {
    outTimestampMs = 0;
    if (!timeout || timeout > UINT16_MAX) return false;
    // UI and background sync share clients; do not race their sockets or apply
    // an older response after a newer one. No wait, allocation or network on busy.
    HttpTimeSyncGuard syncGuard;
    if (!syncGuard.locked()) return false;
    // The bounded request interval starts after any TLS bootstrap below.
    uint32_t deviceSendTime = 0;
    String fullUrl = url;
    const bool useTls = fullUrl.startsWith("https://");

    // 截断已有 deviceSendTime 参数，重新拼接当前时间戳
    int idx = fullUrl.indexOf("?deviceSendTime=");
    if (idx >= 0) {
        fullUrl = fullUrl.substring(0, idx + 16);
    } else if (fullUrl.indexOf("?") >= 0) {
        fullUrl += "&deviceSendTime=";
    } else {
        fullUrl += "?deviceSendTime=";
    }

    HTTPClient http;
    static WiFiClient plainClient;
    static WiFiClientSecure secureClient;
    bool beginOk = false;

    if (useTls) {
        // RTC boot seeding provides a calendar, not a network sync record.
        // Bootstrap unknown/RTC/stale time with the existing SNTP flow while
        // preserving the configured POSIX timezone and TLS certificate checks.
        if (getTimestamp() < MIN_VALID_TLS_TIME ||
                !FastBee::SystemTimeQuality::snapshot().synced) {
            FastBee::SystemTimeQuality::installSntpObserver();
            const uint32_t bootstrapSequence = FastBee::SystemTimeQuality::sequence();
            if (!FastBee::SystemTimeZone::startSntp(nullptr, "pool.ntp.org",
                    "time.nist.gov", "time.google.com")) return false;
            if (!syncNTP(timeout, bootstrapSequence)) {
                return false;
            }
        }

        const size_t caBundleSize = static_cast<size_t>(x509_crt_bundle_end - x509_crt_bundle_start);
        if (caBundleSize == 0) {
            return false;
        }
        secureClient.setCACertBundle(x509_crt_bundle_start, caBundleSize);
        secureClient.setHandshakeTimeout(30);
    }
    deviceSendTime = static_cast<uint32_t>(millis());
    const uint32_t syncSequence = FastBee::SystemTimeQuality::sequence();
    fullUrl += String(static_cast<unsigned long>(deviceSendTime));
    beginOk = useTls ? http.begin(secureClient, fullUrl) : http.begin(plainClient, fullUrl);

    if (!beginOk) {
        return false;
    }
    http.setTimeout((int)timeout);
    // HTTPClient::setTimeout() mainly limits response reads. DNS/TCP connect
    // can otherwise keep the scheduler blocked far longer on a weak/no-PSRAM
    // device when the configured NTP host is unavailable.
    http.setConnectTimeout((int)timeout);

    int httpCode = http.GET();
    bool success = false;

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        const uint32_t deviceRecvTime = static_cast<uint32_t>(millis());
        
        double serverRecvTime = 0;
        double serverSendTime = 0;
        
        FastBeeJsonDoc doc;
        DeserializationError parseErr = deserializeJson(doc, payload.c_str(), payload.length());
        if (!parseErr) {
            const auto readTimestamp = [](JsonVariantConst value) -> double {
                if (value.is<double>()) return value.as<double>();
                if (value.is<const char*>()) {
                    const JsonString text = value.as<JsonString>();
                    double parsed = 0;
                    if (FastBee::parseTimeMilliseconds(text.c_str(), text.size(), parsed)) return parsed;
                }
                return 0;
            };
            serverRecvTime = readTimestamp(doc["serverRecvTime"]);
            serverSendTime = readTimestamp(doc["serverSendTime"]);
            // Fallback only for an absent pair, never rescue a partial/invalid pair.
            if (!doc.containsKey("serverRecvTime") && !doc.containsKey("serverSendTime")) {
                serverRecvTime = readTimestamp(doc["data"]["serverTime"]);
                serverSendTime = serverRecvTime;
            }
        }

        if (parseErr) {
            auto extractTag = [&](const String& tag) -> double {
                String open  = "<" + tag + ">";
                String close = "</" + tag + ">";
                int s = payload.indexOf(open);
                int e = payload.indexOf(close, s);
                if (s >= 0 && e > s) {
                    const String token = payload.substring(s + open.length(), e);
                    double parsed = 0;
                    if (FastBee::parseTimeMilliseconds(token.c_str(), token.length(), parsed)) return parsed;
                }
                return 0.0;
            };
            serverRecvTime = extractTag("serverRecvTime");
            serverSendTime = extractTag("serverSendTime");
        }

        long long timestampMs = 0;
        if (FastBee::estimateHttpTimeMilliseconds<time_t>(serverRecvTime, serverSendTime,
                deviceSendTime, deviceRecvTime, timeout, timestampMs)) {
            success = FastBee::SystemTimeQuality::applyMilliseconds(timestampMs,
                useTls ? FastBee::SystemTimeQuality::Source::Https : FastBee::SystemTimeQuality::Source::Http,
                useTls, syncSequence);
            if (success) outTimestampMs = timestampMs;
        }
    }

    http.end();
    return success;
}

bool TimeUtils::syncNTPFromHTTP(const String& url, unsigned long timeout) {
    long long timestampMs = 0;
    return syncNTPFromHTTPWithTimestamp(url, timestampMs, timeout);
}

time_t TimeUtils::getTimestamp() {
    time_t now;
    time(&now);
    return now;
}

uint64_t TimeUtils::getTimestampMs() {
    struct timeval tv{};
    if (gettimeofday(&tv, nullptr) != 0 || tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000)
        return 0;
    return static_cast<uint64_t>(tv.tv_sec) * 1000ULL + static_cast<uint64_t>(tv.tv_usec) / 1000ULL;
}

unsigned long TimeUtils::getUptime() {
    return millis();
}

String TimeUtils::formatTime(time_t timestamp, TimeFormat format) {
    if (timestamp == 0) {
        timestamp = getTimestamp();
    }
    
    struct tm timeinfo{};
    // A literal Z denotes UTC. Human/date/time formats retain configured local
    // time. Reentrant conversion avoids another task replacing this calendar.
    if (!(format == ISO8601 ? gmtime_r(&timestamp, &timeinfo)
                           : localtime_r(&timestamp, &timeinfo))) return String();
    char buffer[64]{};
    size_t length = 0;
    
    switch (format) {
        case ISO8601:
            length = strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
            break;
        case RFC822:
            length = strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S %Z", &timeinfo);
            break;
        case HUMAN_READABLE:
            length = strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
            break;
        case TIME_ONLY:
            length = strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo);
            break;
        case DATE_ONLY:
            length = strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
            break;
        default:
            length = strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
            break;
    }
    
    return length ? String(buffer) : String();
}

String TimeUtils::formatCurrentTime(TimeFormat format) {
    return formatTime(getTimestamp(), format);
}

time_t TimeUtils::parseTime(const String& timeString, TimeFormat format) {
    // Exact fixed formats only: no trailing suffix, partial parse, normalized
    // invalid date or process-global timezone mutation to parse a UTC Z value.
    const size_t size = timeString.length();
    const char* text = timeString.c_str();
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    const auto digits = [&](size_t at, size_t count, int& value) {
        value = 0;
        if (at + count > size) return false;
        for (size_t i = 0; i < count; ++i) {
            const char ch = text[at + i];
            if (ch < '0' || ch > '9') return false;
            value = value * 10 + (ch - '0');
        }
        return true;
    };
    if (format == TIME_ONLY) {
        if (size != 8 || text[2] != ':' || text[5] != ':' ||
            !digits(0,2,hour) || !digits(3,2,minute) || !digits(6,2,second)) return 0;
        const time_t now = getTimestamp();
        struct tm current{};
        if (!localtime_r(&now, &current)) return 0;
        year = current.tm_year + 1900; month = current.tm_mon + 1; day = current.tm_mday;
    } else {
        if (format != ISO8601 && format != HUMAN_READABLE && format != DATE_ONLY) return 0;
        const size_t expected = format == ISO8601 ? 20 : format == DATE_ONLY ? 10 : 19;
        if (size != expected || text[4] != '-' || text[7] != '-' ||
            !digits(0,4,year) || !digits(5,2,month) || !digits(8,2,day)) return 0;
        if (format != DATE_ONLY &&
            (text[10] != (format == ISO8601 ? 'T' : ' ') || text[13] != ':' || text[16] != ':' ||
             !digits(11,2,hour) || !digits(14,2,minute) || !digits(17,2,second))) return 0;
        if (format == ISO8601 && text[19] != 'Z') return 0;
    }
    if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        day > getDaysInMonth(year, month) || hour > 23 || minute > 59 || second > 59) return 0;
    if (format == ISO8601) {
        // Gregorian days since 1970-01-01; UTC is independent of local DST/TZ.
        const int64_t priorYear = year - 1;
        int64_t days = 365LL * (year - 1970) + priorYear / 4 - 1969 / 4
            - priorYear / 100 + 1969 / 100 + priorYear / 400 - 1969 / 400;
        for (int m = 1; m < month; ++m) days += getDaysInMonth(year, m);
        const int64_t epoch = (days + day - 1) * 86400 + hour * 3600 + minute * 60 + second;
        if (epoch > std::numeric_limits<time_t>::max()) return 0;
        return static_cast<time_t>(epoch);
    }
    struct tm local{};
    local.tm_year = year - 1900; local.tm_mon = month - 1; local.tm_mday = day;
    local.tm_hour = hour; local.tm_min = minute; local.tm_sec = second; local.tm_isdst = -1;
    const time_t result = mktime(&local);
    struct tm roundtrip{};
    if (result < 0 || !localtime_r(&result, &roundtrip) || roundtrip.tm_year != year - 1900 ||
        roundtrip.tm_mon != month - 1 || roundtrip.tm_mday != day || roundtrip.tm_hour != hour ||
        roundtrip.tm_min != minute || roundtrip.tm_sec != second) return 0;
    return result;
}

struct tm TimeUtils::getTimeStruct(time_t timestamp) {
    if (timestamp == 0) {
        timestamp = getTimestamp();
    }
    
    struct tm timeinfo{};
    if (!localtime_r(&timestamp, &timeinfo)) return {};
    return timeinfo;
}

bool TimeUtils::setTimeZone(const TimeZone& timezone) {
    // POSIX offsets have the opposite sign to a user-facing UTC offset.
    // Preserve minute offsets (e.g. UTC+05:30), and reject before mutation.
    if (timezone.offset < -14 * 60 || timezone.offset > 14 * 60) return false;
    const int minutes = timezone.offset < 0 ? -timezone.offset : timezone.offset;
    char offset[24]{};
    snprintf(offset, sizeof(offset), "UTC%c%d:%02d",
             timezone.offset >= 0 ? '-' : '+', minutes / 60, minutes % 60);
    String tzString(offset);
    
    if (timezone.dst) {
        tzString += "DST";
    }
    
    return FastBee::SystemTimeZone::setPosix(tzString.c_str());
}

TimeUtils::TimeZone TimeUtils::getCurrentTimeZone() {
    FastBee::TimeZoneGuard guard;
    if (!guard.held()) return TimeZone("unknown", 0, false);
    const time_t now = getTimestamp();
    struct tm local{}, utc{};
    if (!localtime_r(&now, &local) || !gmtime_r(&now, &utc)) return TimeZone("unknown", 0, false);
    const auto yearDays = [](int year) { return 365 + (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)); };
    int dayDelta = local.tm_yday - utc.tm_yday;
    if (local.tm_year > utc.tm_year) dayDelta += yearDays(utc.tm_year + 1900);
    if (local.tm_year < utc.tm_year) dayDelta -= yearDays(local.tm_year + 1900);
    const int minutes = dayDelta * 1440 + (local.tm_hour - utc.tm_hour) * 60 + local.tm_min - utc.tm_min;
    const char* active = getenv("TZ");
    // Return an effective fixed-offset snapshot. Do not append a second DST
    // hour when round-tripping it; a bool cannot represent seasonal TZ rules.
    return TimeZone(active ? active : "UTC0", minutes, false);
}

time_t TimeUtils::addSeconds(time_t timestamp, long seconds) {
    return timestamp + seconds;
}

time_t TimeUtils::addMinutes(time_t timestamp, long minutes) {
    return timestamp + minutes * 60;
}

time_t TimeUtils::addHours(time_t timestamp, long hours) {
    return timestamp + hours * 3600;
}

time_t TimeUtils::addDays(time_t timestamp, long days) {
    return timestamp + days * 86400;
}

long TimeUtils::timeDifference(time_t time1, time_t time2) {
    return difftime(time1, time2);
}

bool TimeUtils::isSameDay(time_t time1, time_t time2) {
    struct tm tm1 = getTimeStruct(time1);
    struct tm tm2 = getTimeStruct(time2);
    
    return tm1.tm_year == tm2.tm_year &&
           tm1.tm_mon == tm2.tm_mon &&
           tm1.tm_mday == tm2.tm_mday;
}

time_t TimeUtils::getDayStart(time_t timestamp) {
    struct tm timeinfo = getTimeStruct(timestamp);
    timeinfo.tm_hour = 0;
    timeinfo.tm_min = 0;
    timeinfo.tm_sec = 0;
    return mktime(&timeinfo);
}

time_t TimeUtils::getDayEnd(time_t timestamp) {
    struct tm timeinfo = getTimeStruct(timestamp);
    timeinfo.tm_hour = 23;
    timeinfo.tm_min = 59;
    timeinfo.tm_sec = 59;
    return mktime(&timeinfo);
}

int TimeUtils::getDayOfWeek(time_t timestamp) {
    struct tm timeinfo = getTimeStruct(timestamp);
    return timeinfo.tm_wday;
}

int TimeUtils::getDayOfMonth(time_t timestamp) {
    struct tm timeinfo = getTimeStruct(timestamp);
    return timeinfo.tm_mday;
}

int TimeUtils::getDayOfYear(time_t timestamp) {
    struct tm timeinfo = getTimeStruct(timestamp);
    return timeinfo.tm_yday + 1;
}

String TimeUtils::formatDuration(unsigned long milliseconds, bool showMilliseconds) {
    unsigned long seconds = milliseconds / 1000;
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;
    unsigned long days = hours / 24;
    
    seconds %= 60;
    minutes %= 60;
    hours %= 24;
    
    // 最大格式如 "999d 23h 59m 59.999s"，32 字节足够
    String result;
    result.reserve(32);
    
    if (days > 0) {
        result += String(days) + "d ";
    }
    if (hours > 0 || !result.isEmpty()) {
        result += String(hours) + "h ";
    }
    if (minutes > 0 || !result.isEmpty()) {
        result += String(minutes) + "m ";
    }
    
    if (showMilliseconds) {
        unsigned long ms = milliseconds % 1000;
        result += String(seconds) + "." + String(ms) + "s";
    } else {
        result += String(seconds) + "s";
    }
    
    return result;
}

int TimeUtils::setTimeout(unsigned long milliseconds, void (*callback)()) {
    cleanupInactiveTimers();

    Ticker* ticker = new (std::nothrow) Ticker();
    if (ticker == nullptr) {
        return -1;
    }
    int timerId = nextTimerId++;
    timerCallbacksRunning[timerId] = false;

    ticker->once_ms(milliseconds, [timerId, callback]() {
        TimerCallbackGuard guard(timerId);
        if (callback != nullptr) {
            callback();
        }
    });
    timers[timerId] = ticker;
    
    return timerId;
}

int TimeUtils::setInterval(unsigned long milliseconds, void (*callback)()) {
    cleanupInactiveTimers();

    Ticker* ticker = new (std::nothrow) Ticker();
    if (ticker == nullptr) {
        return -1;
    }
    int timerId = nextTimerId++;
    
    ticker->attach_ms(milliseconds, callback);
    timers[timerId] = ticker;
    
    return timerId;
}

void TimeUtils::clearTimer(int timerId) {
    auto it = timers.find(timerId);
    if (it != timers.end()) {
        delete it->second;
        timers.erase(it);
    }
    timerCallbacksRunning.erase(timerId);
}

bool TimeUtils::isTimeInRange(time_t checkTime, time_t startTime, time_t endTime) {
    return checkTime >= startTime && checkTime <= endTime;
}

TimeUtils::TimeZone TimeUtils::getTimeZoneByName(const String& name) {
    // 预定义时区
    if (name == "UTC") return TimeZone("UTC", 0);
    if (name == "EST") return TimeZone("EST", -5 * 60);
    if (name == "CST") return TimeZone("CST", -6 * 60);
    if (name == "PST") return TimeZone("PST", -8 * 60);
    if (name == "CET") return TimeZone("CET", 1 * 60, true);
    if (name == "CST8") return TimeZone("CST8", 8 * 60); // 中国标准时间
    
    return TimeZone("UTC", 0);
}

bool TimeUtils::isLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int TimeUtils::getDaysInMonth(int year, int month) {
    static const int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    
    if (month < 1 || month > 12) return 0;
    
    if (month == 2 && isLeapYear(year)) {
        return 29;
    }
    
    return daysInMonth[month - 1];
}

unsigned long TimeUtils::getCPUTime() {
    return micros();
}
