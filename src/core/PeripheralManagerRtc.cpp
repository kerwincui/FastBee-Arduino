#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/RtcAccess.h"
#include "utils/SystemTimeQuality.h"
#include "utils/SystemTimePlatform.h"
#include <cstring>

void PeripheralManager::seedSystemClockFromRtcAtBoot() {
    using Quality = FastBee::SystemTimeQuality;
    using Status = Quality::RtcBootStatus;
    if (!Quality::claimRtcBootSeed()) return;
#if !FASTBEE_ENABLE_DS1302
    Quality::finishRtcBootSeed(Status::NotCompiled);
#else
    RecursiveMutexGuard lock(_mutex, pdMS_TO_TICKS(50));
    if (!lock.isLocked()) { Quality::finishRtcBootSeed(Status::Busy); return; }
    const PeripheralConfig* selected = nullptr;
    for (const auto& entry : peripherals) {
        const auto& config = entry.second;
        if (config.type != PeripheralType::DS1302_RTC || !config.enabled ||
            !config.params.ds1302.bootSeed) continue;
        if (selected) { Quality::finishRtcBootSeed(Status::MultipleSources); return; }
        selected = &config;
    }
    if (!selected) { Quality::finishRtcBootSeed(Status::Disabled); return; }
    const auto before = Quality::snapshot();
    if (std::strcmp(before.quality, "busy") == 0) { Quality::finishRtcBootSeed(Status::Busy); return; }
    if (before.timeValid) { Quality::finishRtcBootSeed(Status::SkippedValid); return; }
    if (before.sequence != 0) { Quality::finishRtcBootSeed(Status::Rejected); return; }
    const int offset = selected->params.ds1302.utcOffsetMinutes;
    if (offset < -840 || offset > 840) { Quality::finishRtcBootSeed(Status::InvalidOffset); return; }
    const auto readStarted = FastBee::SystemTimePlatform::monotonicMs();
    if (!FastBee::SystemTimePlatform::monotonicAvailable(readStarted)) {
        Quality::finishRtcBootSeed(Status::Rejected); return;
    }
    FastBee::RtcAccess access(selected->id);
    if (!access.driver()) { Quality::finishRtcBootSeed(Status::Unavailable); return; }
    const auto clock = access.driver()->readClockSnapshot();
    if (!clock.valid || !clock.dateTime.isValid()) {
        Quality::finishRtcBootSeed(Status::InvalidClock); return;
    }
    if (!clock.running) { Quality::finishRtcBootSeed(Status::Stopped); return; }
    const auto readCompleted = FastBee::SystemTimePlatform::monotonicMs();
    if (!FastBee::SystemTimePlatform::monotonicAvailable(readCompleted) ||
        readCompleted < readStarted || readCompleted - readStarted > 1000) {
        Quality::finishRtcBootSeed(Status::Rejected); return;
    }
    // DS1302 stores a calendar without a timezone. The operator-declared fixed
    // offset is independent of mutable system TZ and of 12/24-hour encoding.
    const int64_t epochMs = (static_cast<int64_t>(clock.dateTime.toUnixTimestamp()) -
        static_cast<int64_t>(offset) * 60) * 1000;
    if (epochMs < 1577836800000LL || epochMs >= 4102444800000LL) {
        Quality::finishRtcBootSeed(Status::InvalidEpoch); return;
    }
    // Service rechecks revision, wall validity, monotonic continuity and write
    // result. It consumes this attempt even on failure and never emits NTP_SYNCED.
    Quality::seedFromRtcMilliseconds(epochMs, before.sequence);
#endif
}
