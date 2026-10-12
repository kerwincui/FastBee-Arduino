#include <Arduino.h>
#include <ArduinoJson.h>
#include <ctime>
#include <cassert>
#include <atomic>
#include <string>
#include <thread>
#include "utils/TimeSyncValidation.h"
#include "utils/SystemTimeQuality.h"
#include "utils/SystemTimePlatform.h"
#include "utils/SystemTimeZone.h"

using FastBeeJsonDoc = JsonDocument;
struct HttpTestTimeval { time_t tv_sec; long tv_usec; };
using suseconds_t = long;
static uint32_t clockMs = 1000, headerMs = 150, bodyMs = 50;
static int beginCalls = 0, endCalls = 0, getCalls = 0, bodyCalls = 0, setCalls = 0;
static int code = 200, setResult = 0, syncCalls = 0, caCalls = 0;
static bool beginResult = true, syncResult = true, validWallClock = true;
static std::atomic<bool> blockRequest{false}, requestEntered{false}, releaseRequest{false};
static String capturedUrl;
static HttpTestTimeval written{};
static int64_t initialWallMs=1700000000000LL;
static String payload = R"({"serverRecvTime":1700000000000,"serverSendTime":1700000000100})";
static FastBee::SystemTimePlatform::Observer timeObserver=nullptr;
static bool sourceChangedDuringGet=false;
static std::string activeTimezone="CST-8";
static unsigned long testMillis() { return clockMs; }
static int testSetTime(const HttpTestTimeval* value, const void*) { ++setCalls; written = *value; return setResult; }
static constexpr int HTTP_CODE_OK = 200;
static constexpr time_t MIN_VALID_TLS_TIME = 1577836800;
static const uint8_t caBytes[2] = {1, 2};
static const uint8_t* x509_crt_bundle_start = caBytes;
static const uint8_t* x509_crt_bundle_end = caBytes + 2;
struct WiFiClient {};
struct WiFiClientSecure {
    void setCACertBundle(const uint8_t* p, size_t n) { assert(p == caBytes && n == 2); ++caCalls; }
    void setHandshakeTimeout(unsigned n) { assert(n == 30); }
};
struct HTTPClient {
    template<class Client> bool begin(Client&, const String& url) { ++beginCalls; capturedUrl=url; return beginResult; }
    void setTimeout(uint16_t) {}
    void setConnectTimeout(int32_t) {}
    int GET() {
        ++getCalls;
        if (blockRequest.load()) {
            requestEntered.store(true);
            while (!releaseRequest.load()) std::this_thread::yield();
        }
        clockMs += headerMs;
        if (sourceChangedDuringGet) { assert(timeObserver); timeObserver(1700000000000LL); }
        return code;
    }
    String getString() { ++bodyCalls; clockMs += bodyMs; return payload; }
    void end() { ++endCalls; }
};
uint64_t FastBee::SystemTimePlatform::monotonicMs() { return clockMs; }
int64_t FastBee::SystemTimePlatform::wallMs() {
    return written.tv_sec ? written.tv_sec*1000LL+written.tv_usec/1000 : initialWallMs;
}
bool FastBee::SystemTimePlatform::writeMs(int64_t value) {
    const HttpTestTimeval tv{static_cast<time_t>(value/1000),static_cast<long>((value%1000)*1000)};
    return testSetTime(&tv,nullptr)==0;
}
void FastBee::SystemTimePlatform::installObserver(Observer observer) { timeObserver=observer; }
const char* FastBee::SystemTimePlatform::readTimezone() { return activeTimezone.c_str(); }
bool FastBee::SystemTimePlatform::writeTimezone(const char* value) { activeTimezone=value;return true; }
void FastBee::SystemTimePlatform::configureSntp(const char*,const char*,const char*,const char*) {}
bool FastBee::SystemTimePlatform::formatLocal(int64_t,char*,size_t) { assert(false);return false; }
class TimeUtils { public:
    static time_t getTimestamp() { return validWallClock ? MIN_VALID_TLS_TIME + 1 : 0; }
    static bool syncNTP(unsigned long,uint32_t) {
        ++syncCalls;clockMs+=12000;
        if(syncResult && timeObserver)timeObserver(1700000000000LL);
        return syncResult;
    }
    static bool syncNTPFromHTTPWithTimestamp(const String&, long long&, unsigned long = 10000);
};
#define millis testMillis
#define settimeofday testSetTime
#define timeval HttpTestTimeval
#include "time_http_under_test.inc"
#undef millis
#undef settimeofday
#undef timeval

static void reject(const String& data) {
    payload = data;
    long long output = 888888;
    const int before = setCalls, ends = endCalls;
    assert(!TimeUtils::syncNTPFromHTTPWithTimestamp("http://fixture.invalid/time", output));
    assert(output == 0 && setCalls == before && endCalls == ends + 1);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    long long output = 99999;
    if (mode == "concurrent") {
        blockRequest=true;
        bool first=false; long long firstTime=0;
        std::thread worker([&] { first=TimeUtils::syncNTPFromHTTPWithTimestamp("http://fixture.invalid/time",firstTime); });
        while (!requestEntered.load()) std::this_thread::yield();
        const int begins=beginCalls, gets=getCalls, bodies=bodyCalls, sets=setCalls;
        for (int i=0;i<1000;++i) {
            output=99999;
            assert(!TimeUtils::syncNTPFromHTTPWithTimestamp("https://fixture.invalid/time",output));
            assert(output==0 && beginCalls==begins && getCalls==gets && bodyCalls==bodies && setCalls==sets);
        }
        releaseRequest=true; worker.join();
        assert(first && firstTime==1700000000150LL);
        blockRequest=false;
        assert(TimeUtils::syncNTPFromHTTPWithTimestamp("http://fixture.invalid/time",output));
        assert(output==firstTime && setCalls==2);
        return 0;
    }
    if (mode == "bounds32") {
        assert(FastBee::estimateHttpTimeMilliseconds<int32_t>(2147483647000.,2147483647000.,0,0,5000,output));
        assert(output == 2147483647000LL);
        assert(!FastBee::estimateHttpTimeMilliseconds<int32_t>(2147483648000.,2147483648000.,0,0,5000,output) && output == 0);
        return 0;
    }
    if (mode == "bounds64") {
        assert(FastBee::estimateHttpTimeMilliseconds<int64_t>(2147483648000.,2147483648000.,0,0,5000,output));
        assert(!FastBee::estimateHttpTimeMilliseconds<int64_t>(INFINITY,INFINITY,0,0,5000,output) && output == 0);
        assert(!FastBee::estimateHttpTimeMilliseconds<int64_t>(NAN,NAN,0,0,5000,output) && output == 0);
        const double limit = static_cast<double>(std::numeric_limits<long long>::max());
        assert(!FastBee::estimateHttpTimeMilliseconds<int64_t>(limit,limit,0,0,5000,output) && output == 0);
        return 0;
    }
    if (mode == "strict_parser") {
        double value = 123;
        for (const char* text : {""," ","abc","1700000000000junk","inf","nan","1e999"}) {
            assert(!FastBee::parseTimeMilliseconds(text, std::strlen(text), value) && value == 0);
        }
        const char nul[] = "1700000000000\0junk";
        assert(!FastBee::parseTimeMilliseconds(nul,sizeof(nul)-1,value));
        assert(!FastBee::parseTimeMilliseconds(nullptr,1,value));
        assert(!FastBee::parseTimeMilliseconds("1",41,value));
        const char* good=" 1700000000000.5 \r\n";
        assert(FastBee::parseTimeMilliseconds(good,std::strlen(good),value) && value == 1700000000000.5);
        return 0;
    }
    if (mode == "invalid_samples") {
        for (const char* bad : {"{}","null","[]","{",
            R"({"serverRecvTime":true,"serverSendTime":1700000000100})",
            R"({"serverRecvTime":1700000000000,"serverSendTime":1700000000300})",
            R"({"serverRecvTime":1700000000100,"serverSendTime":1700000000000})",
            R"({"serverRecvTime":1e999,"serverSendTime":1e999})",
            R"({"serverRecvTime":9.3e18,"serverSendTime":9.3e18})",
            R"({"serverRecvTime":-1,"serverSendTime":1700000000100})",
            R"({"serverRecvTime":1000000000000,"serverSendTime":1000000000100})",
            R"({"serverRecvTime":"1700000000000junk","serverSendTime":1700000000100})",
            R"({"serverRecvTime":"1700000000000\u0000junk","serverSendTime":1700000000100})"}) reject(bad);
        return 0;
    }
    if (mode == "partial_pair") {
        reject(R"({"serverRecvTime":0,"serverSendTime":1700000000100,"data":{"serverTime":1700000000000}})");
        reject(R"({"serverRecvTime":null,"data":{"serverTime":1700000000000}})");
        reject(R"({"serverSendTime":1700000000100,"data":{"serverTime":1700000000000}})");
        return 0;
    }
    if (mode == "xml_junk") {
        reject("<r><serverRecvTime>1700000000000bad</serverRecvTime><serverSendTime>1700000000100</serverSendTime></r>");
        reject("<r><serverRecvTime>1700000000000</serverRecvTime></r>");
        return 0;
    }
    bool expected = true;
    unsigned long timeout = 10000;
    String url = "http://fixture.invalid/time";
    long long expectedMs = 1700000000150LL;
    if (mode == "tls_rtc_bootstrap" || mode == "tls_rtc_failed") {
        initialWallMs=0;
        assert(FastBee::SystemTimeQuality::claimRtcBootSeed());
        assert(FastBee::SystemTimeQuality::seedFromRtcMilliseconds(1700000000000LL,0));
        const auto rtc=FastBee::SystemTimeQuality::snapshot();
        assert(rtc.timeValid && !rtc.synced && !rtc.transportAuthenticated);
        assert(std::strcmp(rtc.source,"rtc")==0 && std::strcmp(rtc.quality,"rtc_unverified")==0);
        assert(validWallClock && TimeUtils::getTimestamp()>=MIN_VALID_TLS_TIME);
        setCalls=0; // Separate the boot seed write from the HTTP operation.
    }
    if (mode == "nested") { payload=R"({"data":{"serverTime":1700000000000}})"; expectedMs=1700000000100LL; }
    else if (mode == "strings") payload=R"({"serverRecvTime":"1700000000000","serverSendTime":"1700000000100"})";
    else if (mode == "xml") payload="<r><serverRecvTime>1700000000000</serverRecvTime><serverSendTime>1700000000100</serverSendTime></r>";
    else if (mode == "wrap") clockMs=UINT32_MAX-99;
    else if (mode == "body_delay") { bodyMs=350; expectedMs=1700000000300LL; }
    else if (mode == "set_failed") { setResult=-1; expected=false; }
    else if (mode == "begin_failed") { beginResult=false; expected=false; }
    else if (mode == "http_error") { code=500; expected=false; }
    else if (mode == "redirect") { code=301; expected=false; }
    else if (mode == "expired") { bodyMs=10001; expected=false; }
    else if (mode == "zero_timeout") { timeout=0; expected=false; }
    else if (mode == "oversized_timeout") { timeout=65536; expected=false; }
    else if (mode == "source_changed") {
        FastBee::SystemTimeQuality::installSntpObserver();sourceChangedDuringGet=true;expected=false;
    }
    else if (mode == "tls_rtc_bootstrap" || mode == "tls_rtc_failed" ||
             mode == "tls_unknown_clock" || mode == "tls_known_source") {
        url="https://fixture.invalid/time";
        if (mode == "tls_rtc_failed") { syncResult=false; expected=false; }
        if (mode == "tls_known_source") {
            FastBee::SystemTimeQuality::installSntpObserver();
            assert(timeObserver); timeObserver(1700000000000LL);
            assert(FastBee::SystemTimeQuality::snapshot().synced);
        }
    }
    else if (mode == "tls_bootstrap" || mode == "tls_failed" || mode == "no_bundle") {
        url="https://fixture.invalid/time"; validWallClock=false;
        if (mode == "tls_failed") { syncResult=false; expected=false; }
        if (mode == "no_bundle") { x509_crt_bundle_end=x509_crt_bundle_start; expected=false; }
    } else assert(mode == "valid");
    const bool ok=TimeUtils::syncNTPFromHTTPWithTimestamp(url,output,timeout);
    assert(ok == expected);
    if (expected) {
        assert(output == expectedMs && setCalls == 1 && endCalls == 1);
        assert(written.tv_sec * 1000LL + written.tv_usec / 1000 == output);
        assert(written.tv_usec >= 0 && written.tv_usec < 1000000);
        if (mode == "tls_bootstrap" || mode == "tls_rtc_bootstrap" || mode == "tls_unknown_clock") {
            assert(syncCalls == 1 && caCalls == 1 && capturedUrl.endsWith("deviceSendTime=13000"));
        }
        if (mode == "tls_known_source") {
            assert(syncCalls == 0 && caCalls == 1 && capturedUrl.endsWith("deviceSendTime=1000"));
        }
        if (url.startsWith("https://")) {
            const auto status=FastBee::SystemTimeQuality::snapshot();
            assert(status.synced && status.transportAuthenticated && std::strcmp(status.source,"https")==0);
        }
    } else {
        assert(output == 0 && setCalls == (mode == "set_failed" ? 1 : 0));
        if (beginCalls && beginResult) assert(endCalls == 1);
        if (mode == "redirect" || mode == "http_error") assert(bodyCalls == 0);
        if (mode == "zero_timeout" || mode == "oversized_timeout" || mode == "tls_failed" ||
                mode == "tls_rtc_failed" || mode == "no_bundle")
            assert(beginCalls == 0 && getCalls == 0);
        if (mode == "tls_rtc_failed") {
            const auto status=FastBee::SystemTimeQuality::snapshot();
            assert(syncCalls==1 && caCalls==0 && !status.synced);
            assert(std::strcmp(status.source,"rtc")==0);
        }
        // Every early return must relinquish the nonblocking shared-client guard.
        beginResult=true; setResult=0; code=200; bodyMs=50;sourceChangedDuringGet=false;
        assert(TimeUtils::syncNTPFromHTTPWithTimestamp("http://fixture.invalid/time",output));
        assert(output==1700000000150LL);
    }
}
