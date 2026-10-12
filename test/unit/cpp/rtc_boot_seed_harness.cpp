// Real boot hook, RtcAccess, DS1302 driver and source state machine.
// GPIO, scheduler mutex, manager storage and the platform clock are fakes.
#include "core/PeripheralManager.h"
#include "core/RuntimeFeatureManager.h"
#include "utils/SystemTimeQuality.h"
#include "utils/SystemTimePlatform.h"
#if FASTBEE_ENABLE_DS1302
#include "peripherals/drivers/DS1302Driver.h"
#endif
#include <cassert>
#include <cstring>
#include <string>

using Quality = FastBee::SystemTimeQuality;
using Platform = FastBee::SystemTimePlatform;
static int64_t wall = 0;
static uint64_t mono = 0;
static unsigned clockWrites = 0;
static bool writeOk = true, sntpAtWrite = false, invalidMonoAtWrite = false;
[[maybe_unused]] static bool wallAtMono = false, sourceAtIo = false, delayedIo = false;
static Platform::Observer observer = nullptr;
uint64_t Platform::monotonicMs() {
    if (wallAtMono) { wallAtMono = false; wall = 1709210096000LL; }
    return mono;
}
int64_t Platform::wallMs() { return wall; }
bool Platform::writeMs(int64_t value) {
    ++clockWrites;
    if (!writeOk) return false;
    wall = value;
    if (invalidMonoAtWrite) mono = UINT64_MAX;
    if (sntpAtWrite) { wall += 10000; assert(observer); observer(wall); }
    return true;
}
void Platform::installObserver(Observer value) { observer = value; }
static void expectStatus(const char* expected) {
    assert(std::strcmp(Quality::snapshot().rtcBootStatus, expected) == 0);
}
static void checkQuality(const char* quality, bool synced) {
    const auto status = Quality::snapshot();
    assert(std::strcmp(status.quality, quality) == 0 && status.synced == synced);
}
static void service(const std::string& mode) {
    const int64_t value = 1709210096000LL;
    using Status = Quality::RtcBootStatus;
    Quality::installSntpObserver();
    expectStatus("not_attempted");
    assert(!Quality::seedFromRtcMilliseconds(value, 0));
    assert(!Quality::applyMilliseconds(value, Quality::Source::Rtc, false, 0));
    assert(Quality::claimRtcBootSeed());
    assert(!Quality::claimRtcBootSeed());
    expectStatus("pending");
    Quality::finishRtcBootSeed(Status::Pending);
    Quality::finishRtcBootSeed(Status::Applied);
    expectStatus("pending");
    if (mode == "service-disabled") {
        Quality::finishRtcBootSeed(Status::Disabled);
        assert(!Quality::seedFromRtcMilliseconds(value, 0));
        Quality::finishRtcBootSeed(Status::Rejected);
        expectStatus("disabled"); assert(clockWrites == 0); return;
    }
    if (mode == "service-valid") wall = value;
    if (mode == "service-invalid-mono") mono = UINT64_MAX;
    if (mode == "service-write-failure") writeOk = false;
    if (mode == "service-new-wall") wallAtMono = true;
    if (mode == "service-sntp-race") sntpAtWrite = true;
    if (mode == "service-mono-write") invalidMonoAtWrite = true;
    if (mode == "service-existing-source") { wall = value; observer(wall); wall = 0; }
    const bool good = mode == "service-applied" || mode == "service-stale" ||
        mode == "service-jump" || mode == "service-network";
    assert(Quality::seedFromRtcMilliseconds(
        mode == "service-range" ? 4102444800000LL : value, 0) == good);
    expectStatus(good ? "applied" : "rejected");
    const auto written = clockWrites;
    assert(!Quality::seedFromRtcMilliseconds(value, Quality::sequence()));
    assert(!Quality::claimRtcBootSeed()); assert(clockWrites == written);
    if (!good) {
        const bool reachedWrite = mode == "service-write-failure" ||
            mode == "service-sntp-race" || mode == "service-mono-write";
        assert(clockWrites == (reachedWrite ? 1u : 0u));
        return;
    }
    auto seeded = Quality::snapshot();
    assert(seeded.timeValid && !seeded.synced && !seeded.transportAuthenticated);
    assert(std::strcmp(seeded.source, "rtc") == 0 && seeded.ageMs == 0 && seeded.sequence == 1);
    checkQuality("rtc_unverified", false);
    assert(!Quality::stillCurrent(seeded, 1000));
    if (mode == "service-stale") { mono += 86400001; wall += 86400001; checkQuality("stale", false); }
    if (mode == "service-jump") {
        wall += 5001; checkQuality("clock_changed", false);
        wall -= 5001; checkQuality("clock_changed", false);
    }
    if (mode == "service-network") {
        assert(Quality::applyMilliseconds(value, Quality::Source::Https, true, Quality::sequence()));
        checkQuality("synced_tls", true); expectStatus("applied");
    }
}
int main(int argc, char** argv) {
    assert(argc == 2); const std::string mode(argv[1]);
    if (mode.rfind("service-", 0) == 0) { service(mode); return 0; }
    auto& pm = PeripheralManager::getInstance();
#if FASTBEE_ENABLE_DS1302
    PeripheralConfig config;
    config.id = "rtc_01"; config.type = PeripheralType::DS1302_RTC; config.enabled = true;
    initializeDs1302Defaults(config);
    config.params.ds1302.bootSeed = true; config.params.ds1302.utcOffsetMinutes = 480;
    config.pinCount = 3; config.pins[0] = 5; config.pins[1] = 6; config.pins[2] = 7;
    pm.peripherals.emplace(config.id, config);
    pm.state.status = PeripheralStatus::PERIPHERAL_INITIALIZED;
    auto& rtc = DS1302Driver::getInstance(); assert(rtc.begin(5, 6, 7));
    rtcLoad({0x56, 0x34, 0x12, 0x29, 0x02, 0x04, 0x24, 0});
    auto& chosen = pm.peripherals.begin()->second;
    const char* status = "applied"; bool read = true;
    int64_t expectedWall = 1709181296000LL;
    if (mode == "disabled") { chosen.params.ds1302.bootSeed = false; status = "disabled"; read = false; }
    else if (mode == "peripheral-disabled") { chosen.enabled = false; status = "disabled"; read = false; }
    else if (mode == "none") { pm.peripherals.clear(); status = "disabled"; read = false; }
    else if (mode == "multiple") {
        config.id = "rtc_02"; pm.peripherals.emplace(config.id, config);
        status = "multiple_sources"; read = false;
    } else if (mode == "valid-wall") { wall = 1709210096000LL; status = "skipped_valid"; read = false; }
    else if (mode == "busy") { rtcMutexBusy = true; status = "busy"; read = false; }
    else if (mode == "no-mutex") { pm._mutex = nullptr; status = "busy"; read = false; }
    else if (mode == "no-offset") { chosen.params.ds1302.utcOffsetMinutes = INT16_MIN; status = "invalid_offset"; read = false; }
    else if (mode == "bad-offset") { chosen.params.ds1302.utcOffsetMinutes = 841; status = "invalid_offset"; read = false; }
    else if (mode == "runtime-disabled") { FastBee::RuntimeFeatureManager::instance().allowed = false; status = "unavailable"; read = false; }
    else if (mode == "suspended") { pm.state.memorySuspended = true; status = "unavailable"; read = false; }
    else if (mode == "bad-state") { pm.state.status = PeripheralStatus::PERIPHERAL_ERROR; status = "unavailable"; read = false; }
    else if (mode == "state-missing") { pm.stateFound = false; status = "unavailable"; read = false; }
    else if (mode == "uninitialized") { rtc.end(); status = "unavailable"; read = false; }
    else if (mode == "owner-mismatch") { chosen.pins[0] = 8; status = "unavailable"; read = false; }
    else if (mode == "halted") { rtcLoad({0xd6,0x34,0x12,0x29,0x02,0x04,0x24,0}); status = "stopped"; }
    else if (mode == "invalid-calendar") { rtcLoad({0x56,0x34,0x12,0x29,0x02,0x04,0x23,0}); status = "invalid_clock"; }
    else if (mode == "invalid-bcd") { rtcLoad({255,255,255,255,255,255,255,255}); status = "invalid_clock"; }
    else if (mode == "early-epoch") { rtcLoad({0,0,0,1,1,1,0x20,0}); status = "invalid_epoch"; }
    else if (mode == "late-epoch") {
        chosen.params.ds1302.utcOffsetMinutes = -840;
        rtcLoad({0x59,0x59,0x23,0x31,0x12,0x04,0x99,0}); status = "invalid_epoch";
    } else if (mode == "utc") { chosen.params.ds1302.utcOffsetMinutes = 0; expectedWall = 1709210096000LL; }
    else if (mode == "offset-max") { chosen.params.ds1302.utcOffsetMinutes = 840; expectedWall = 1709159696000LL; }
    else if (mode == "offset-min") { chosen.params.ds1302.utcOffsetMinutes = -840; expectedWall = 1709260496000LL; }
    else if (mode == "midnight") { rtcLoad({0,0,0,1,3,5,0x24,0}); expectedWall = 1709222400000LL; }
    else if (mode == "12hour") { rtcLoad({0x56,0x34,0xb2,0x29,0x02,0x04,0x24,0}); }
    else if (mode == "mono-unavailable") { mono = UINT64_MAX; status = "rejected"; read = false; }
    else if (mode == "source-race") { Quality::installSntpObserver(); sourceAtIo = true; status = "rejected"; }
    else if (mode == "slow-read") { delayedIo = true; status = "rejected"; }
    else if (mode == "write-failure") { writeOk = false; status = "rejected"; }
    else assert(mode == "applied");
    rtcIoObserver = [] {
        assert(rtcLockDepth > 0);
        if (sourceAtIo) { sourceAtIo = false; wall = 1709210096000LL; observer(wall); }
        if (delayedIo) { delayedIo = false; mono += 1001; }
    };
    const auto beforeIo = rtcWrites;
    pm.seedSystemClockFromRtcAtBoot();
    expectStatus(status); assert(rtcLockDepth == 0);
    if (read) { assert(rtcBitIndex == 64); assert(rtcOutputBytes() == std::vector<uint8_t>{0xbf}); }
    else { assert(rtcBitIndex == 0); assert(rtcWrites == beforeIo); }
    const bool applied = std::strcmp(status, "applied") == 0;
    assert(clockWrites == (applied || mode == "write-failure" ? 1u : 0u));
    if (applied) { assert(wall == expectedWall); checkQuality("rtc_unverified", false); }
    const auto beforeWrites = clockWrites; const auto io = rtcWrites;
    pm.seedSystemClockFromRtcAtBoot();
    assert(clockWrites == beforeWrites && rtcWrites == io); expectStatus(status);
    rtcIoObserver = nullptr; rtcMutexBusy = false; rtc.end();
#else
    assert(mode == "not-compiled");
    pm.seedSystemClockFromRtcAtBoot(); expectStatus("not_compiled");
    pm.seedSystemClockFromRtcAtBoot(); assert(clockWrites == 0);
#endif
}
