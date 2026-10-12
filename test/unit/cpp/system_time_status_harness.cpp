#include "network/SystemTimeStatus.h"
#include "utils/SystemTimePlatform.h"
#include <cassert>
#include <cstring>
#include <string>
static FastBee::SystemTimeQuality::Snapshot captured;
static std::string statusTimezone="CST-8";
static bool formatOk=true;
static unsigned captures=0,formats=0;
FastBee::SystemTimeQuality::Snapshot FastBee::SystemTimeQuality::snapshot() { ++captures;return captured; }
const char* FastBee::SystemTimePlatform::readTimezone() { return statusTimezone.c_str(); }
bool FastBee::SystemTimePlatform::writeTimezone(const char*) { assert(false);return false; }
void FastBee::SystemTimePlatform::configureSntp(const char*,const char*,const char*,const char*) { assert(false); }
bool FastBee::SystemTimePlatform::formatLocal(int64_t epoch,char* output,size_t capacity) {
    ++formats;assert(epoch==captured.epochMs && capacity>=20);
    FastBee::TimeZoneGuard writer;assert(!writer.held());
    std::strcpy(output,"2026-10-02 18:00:00");return formatOk;
}
int main(int argc,char** argv) {
    assert(argc==2);const std::string mode(argv[1]);
    captured.epochMs=1790935200123LL;captured.timeValid=true;captured.synced=true;
    captured.source="https";captured.quality="synced_tls";captured.transportAuthenticated=true;
    captured.sequence=42;captured.ageMs=1234;
    if(mode=="rtc") {
        captured.source="rtc";captured.quality="rtc_unverified";captured.synced=false;
        captured.transportAuthenticated=false;captured.rtcBootStatus="applied";
    }
    if(mode=="invalid" || mode=="source_busy") {
        captured.timeValid=false;captured.synced=false;
        captured.quality=mode=="invalid"?"invalid":"busy";
    }
    if(mode=="unknown") {captured.synced=false;captured.quality="unknown";captured.source="unknown";captured.transportAuthenticated=false;}
    if(mode=="format_failed")formatOk=false;
    JsonDocument doc;
    doc["datetime"]="old";doc["timestamp"]=123;
    if(mode=="timezone_busy") {
        FastBee::TimeZoneGuard writer;assert(writer.held());FastBee::appendSystemTimeStatus(doc.to<JsonObject>());
    } else FastBee::appendSystemTimeStatus(doc.as<JsonObject>());
    assert(captures==1 && doc["timeSyncSequence"]==42 && doc["timeSyncAgeMs"]==1234);
    assert(doc["synced"].as<bool>()==captured.synced);
    assert(doc["timeValid"].as<bool>()==captured.timeValid);
    assert(doc["timeSource"]==captured.source && doc["timeQuality"]==captured.quality);
    assert(doc["timeSourceAuthenticated"].as<bool>()==captured.transportAuthenticated);
    assert(doc["rtcBootStatus"]==captured.rtcBootStatus);
    const bool display=captured.timeValid && mode!="timezone_busy" && formatOk;
    assert(doc["timeDisplayAvailable"].as<bool>()==display);
    if(display)assert(doc["datetime"]=="2026-10-02 18:00:00");
    else assert(doc["datetime"].isNull());
    if(captured.timeValid)assert(doc["timestamp"].as<int64_t>()==captured.epochMs/1000);
    else assert(doc["timestamp"].isNull());
    assert(formats==(captured.timeValid && mode!="timezone_busy"?1U:0U));
    assert(doc["activeTimezoneAvailable"].as<bool>()==(mode!="timezone_busy"));
    statusTimezone="UTC0"; // JSON owns copied label, not stack/environment storage.
    assert(doc["activeTimezone"]==(mode=="timezone_busy"?"unknown":"CST-8"));
}
