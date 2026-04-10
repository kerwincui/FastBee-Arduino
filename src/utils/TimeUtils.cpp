/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:33:27
 */

#include "utils/TimeUtils.h"
#include "utils/StringUtils.h"
#include "core/FeatureFlags.h"
#include <Ticker.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

// 定时器管理
std::map<int, Ticker*> timers;
int nextTimerId = 1;

bool TimeUtils::initialize(const TimeZone& timezone, const String& ntpServer) {
    // 配置时区
    if (!setTimeZone(timezone)) {
        return false;
    }
    
    // 判断是否为 HTTP URL 格式的 NTP 服务器
    if (ntpServer.startsWith("http://") || ntpServer.startsWith("https://")) {
        return syncNTPFromHTTP(ntpServer);
    }
    
    // 配置NTP
    configTime(timezone.offset * 60, timezone.dst ? 3600 : 0, 
               ntpServer.c_str(), "time.nist.gov", "time.google.com");
    
    // 等待时间同步
    return syncNTP();
}

bool TimeUtils::syncNTP(unsigned long timeout) {
    unsigned long startTime = millis();
    
    while (millis() - startTime < timeout) {
        time_t now = getTimestamp();
        if (now > 1000000000) { // 有效的时间戳
            return true;
        }
        delay(100);
    }
    
    return false;
}

bool TimeUtils::syncNTPFromHTTPWithTimestamp(const String& url, long long& outTimestampMs, unsigned long timeout) {
    float deviceSendTime = (float)millis();
    String fullUrl = url;
    
    if (fullUrl.startsWith("https://")) {
        fullUrl = "http://" + fullUrl.substring(8);
    }
    
    int idx = fullUrl.indexOf("?deviceSendTime=");
    if (idx >= 0) {
        fullUrl = fullUrl.substring(0, idx + 16);
    } else if (fullUrl.indexOf("?") >= 0) {
        fullUrl += "&deviceSendTime=";
    } else {
        fullUrl += "?deviceSendTime=";
    }
    fullUrl += String((unsigned long)deviceSendTime);

    WiFiClient wifiClient;
    HTTPClient http;

    if (!http.begin(wifiClient, fullUrl)) {
        return false;
    }
    http.setTimeout((int)timeout);

    int httpCode = http.GET();
    bool success = false;

    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        float deviceRecvTime = (float)millis();
        String payload = http.getString();
        
        float serverRecvTime = 0, serverSendTime = 0;
        
        FastBeeJsonDoc doc;
        DeserializationError parseErr = deserializeJson(doc, payload);
        if (!parseErr) {
            serverRecvTime = doc["serverRecvTime"] | 0.0f;
            serverSendTime = doc["serverSendTime"] | 0.0f;
            if (serverRecvTime == 0) {
                serverRecvTime = doc["data"]["serverTime"] | 0.0f;
                serverSendTime = serverRecvTime;
            }
        }

        if (serverRecvTime == 0) {
            auto extractTag = [&](const String& tag) -> float {
                String open  = "<" + tag + ">";
                String close = "</" + tag + ">";
                int s = payload.indexOf(open);
                int e = payload.indexOf(close, s);
                if (s >= 0 && e > s) {
                    return payload.substring(s + open.length(), e).toFloat();
                }
                return 0.0f;
            };
            serverRecvTime = extractTag("serverRecvTime");
            serverSendTime = extractTag("serverSendTime");
        }

        if (serverRecvTime > 1000000000000.0f && serverSendTime > 1000000000000.0f) {
            float nowMs = (serverRecvTime + serverSendTime + deviceRecvTime - deviceSendTime) / 2.0f;
            outTimestampMs = (long long)nowMs;
            time_t sec = (time_t)(outTimestampMs / 1000);
            struct timeval tv = { sec, (suseconds_t)((outTimestampMs % 1000) * 1000) };
            settimeofday(&tv, nullptr);
            success = true;
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

unsigned long TimeUtils::getTimestampMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (unsigned long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

unsigned long TimeUtils::getUptime() {
    return millis();
}

String TimeUtils::formatTime(time_t timestamp, TimeFormat format) {
    if (timestamp == 0) {
        timestamp = getTimestamp();
    }
    
    struct tm* timeinfo = localtime(&timestamp);
    char buffer[64];
    
    switch (format) {
        case ISO8601:
            strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", timeinfo);
            break;
        case RFC822:
            strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S %Z", timeinfo);
            break;
        case HUMAN_READABLE:
            strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
            break;
        case TIME_ONLY:
            strftime(buffer, sizeof(buffer), "%H:%M:%S", timeinfo);
            break;
        case DATE_ONLY:
            strftime(buffer, sizeof(buffer), "%Y-%m-%d", timeinfo);
            break;
        default:
            strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
            break;
    }
    
    return String(buffer);
}

String TimeUtils::formatCurrentTime(TimeFormat format) {
    return formatTime(getTimestamp(), format);
}

time_t TimeUtils::parseTime(const String& timeString, TimeFormat format) {
    struct tm timeinfo = {0};
    
    switch (format) {
        case ISO8601:
            sscanf(timeString.c_str(), "%d-%d-%dT%d:%d:%dZ",
                   &timeinfo.tm_year, &timeinfo.tm_mon, &timeinfo.tm_mday,
                   &timeinfo.tm_hour, &timeinfo.tm_min, &timeinfo.tm_sec);
            timeinfo.tm_year -= 1900;
            timeinfo.tm_mon -= 1;
            break;
            
        case HUMAN_READABLE:
        case DATE_ONLY:
            sscanf(timeString.c_str(), "%d-%d-%d",
                   &timeinfo.tm_year, &timeinfo.tm_mon, &timeinfo.tm_mday);
            timeinfo.tm_year -= 1900;
            timeinfo.tm_mon -= 1;
            break;
            
        case TIME_ONLY:
            sscanf(timeString.c_str(), "%d:%d:%d",
                   &timeinfo.tm_hour, &timeinfo.tm_min, &timeinfo.tm_sec);
            // 使用当前日期
            time_t now = getTimestamp();
            struct tm* nowinfo = localtime(&now);
            timeinfo.tm_year = nowinfo->tm_year;
            timeinfo.tm_mon = nowinfo->tm_mon;
            timeinfo.tm_mday = nowinfo->tm_mday;
            break;
    }
    
    return mktime(&timeinfo);
}

struct tm TimeUtils::getTimeStruct(time_t timestamp) {
    if (timestamp == 0) {
        timestamp = getTimestamp();
    }
    
    struct tm timeinfo;
    localtime_r(&timestamp, &timeinfo);
    return timeinfo;
}

bool TimeUtils::setTimeZone(const TimeZone& timezone) {
    // 设置时区环境变量
    String tzString = StringUtils::format("UTC%s%d", 
                                         timezone.offset >= 0 ? "+" : "",
                                         timezone.offset / 60);
    
    if (timezone.dst) {
        tzString += "DST";
    }
    
    setenv("TZ", tzString.c_str(), 1);
    tzset();
    
    return true;
}

TimeUtils::TimeZone TimeUtils::getCurrentTimeZone() {
    // 简化实现，返回UTC
    return TimeZone("UTC", 0, false);
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
    Ticker* ticker = new Ticker();
    int timerId = nextTimerId++;
    
    ticker->once_ms(milliseconds, callback);
    timers[timerId] = ticker;
    
    return timerId;
}

int TimeUtils::setInterval(unsigned long milliseconds, void (*callback)()) {
    Ticker* ticker = new Ticker();
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