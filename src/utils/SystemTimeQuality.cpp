#include "utils/SystemTimeQuality.h"
#include "utils/SystemTimePlatform.h"
#include <atomic>

namespace FastBee {
namespace {
constexpr int64_t MIN_TIME_MS = 1577836800000LL;
constexpr int64_t MAX_TIME_MS = 4102444800000LL; // Project calendar: 2020..2099.
constexpr uint64_t MAX_AGE_MS = 86400000ULL; // Project freshness policy: 24h.
constexpr int64_t MAX_CLOCK_DRIFT_MS = 5000; // Detect untracked wall-clock jumps.
std::atomic_flag recordBusy = ATOMIC_FLAG_INIT;
std::atomic<uint32_t> revision{0};
using RtcBootStatus = SystemTimeQuality::RtcBootStatus;
std::atomic<RtcBootStatus> rtcBootStatus{RtcBootStatus::NotAttempted};
bool validWall(int64_t epochMs) { return epochMs >= MIN_TIME_MS && epochMs < MAX_TIME_MS; }
const char* rtcBootName(RtcBootStatus status) {
    switch (status) {
    case RtcBootStatus::Pending: return "pending";
    case RtcBootStatus::Disabled: return "disabled";
    case RtcBootStatus::NotCompiled: return "not_compiled";
    case RtcBootStatus::Busy: return "busy";
    case RtcBootStatus::SkippedValid: return "skipped_valid";
    case RtcBootStatus::MultipleSources: return "multiple_sources";
    case RtcBootStatus::Unavailable: return "unavailable";
    case RtcBootStatus::InvalidClock: return "invalid_clock";
    case RtcBootStatus::Stopped: return "stopped";
    case RtcBootStatus::InvalidOffset: return "invalid_offset";
    case RtcBootStatus::InvalidEpoch: return "invalid_epoch";
    case RtcBootStatus::Rejected: return "rejected";
    case RtcBootStatus::Applied: return "applied";
    default: return "not_attempted";
    }
}
// A record is evidence only for its exact revision. A callback that cannot
// obtain the guard still advances revision, permanently invalidating the old
// record; an older writer cannot clear a newer callback's invalidation.
uint32_t recordRevision = 0; // Guard-owned, like the source fields below.
constexpr uint32_t EXHAUSTED_REVISION = UINT32_MAX;
SystemTimeQuality::Source lastSource = SystemTimeQuality::Source::None;
uint64_t lastMonotonicMs = 0;
uint64_t lastObservedMonotonicMs = 0;
int64_t lastEpochMs = 0;
bool authenticatedTransport = false;
// Once an observation disproves continuity, returning to the old trajectory
// cannot manufacture new synchronization evidence. Only a new accepted source
// resets this record. Guard protects this flag and the observation watermark.
bool clockDiscontinuity = false;

class Guard {
public:
    Guard() : held(!recordBusy.test_and_set(std::memory_order_acquire)) {}
    ~Guard() { if (held) recordBusy.clear(std::memory_order_release); }
    const bool held;
};
uint64_t monotonicMs() { return SystemTimePlatform::monotonicMs(); }
int64_t wallMs() { return SystemTimePlatform::wallMs(); }
uint32_t observeRevision() {
    uint32_t current = revision.load(std::memory_order_acquire);
    while (current != EXHAUSTED_REVISION) {
        const uint32_t next = current + 1;
        if (revision.compare_exchange_weak(current, next, std::memory_order_acq_rel))
            return next == EXHAUSTED_REVISION ? 0 : next;
    }
    return 0; // Never wrap and reuse old evidence. Reboot required to recover.
}
const char* sourceName(SystemTimeQuality::Source source) {
    switch (source) {
    case SystemTimeQuality::Source::Sntp: return "sntp";
    case SystemTimeQuality::Source::Http: return "http";
    case SystemTimeQuality::Source::Https: return "https";
    case SystemTimeQuality::Source::Mqtt: return "mqtt";
    case SystemTimeQuality::Source::Rtc: return "rtc";
    default: return "unknown";
    }
}
void sntpObserved(int64_t epochMs) {
    // SDK SNTP applies the clock before calling us. Never pretend its write is
    // covered by our HTTP/MQTT mutex. Contention invalidates provenance instead.
    const uint32_t observedRevision = observeRevision();
    Guard guard;
    if (!guard.held || !observedRevision ||
        revision.load(std::memory_order_acquire) != observedRevision ||
        epochMs < MIN_TIME_MS || epochMs >= MAX_TIME_MS) return;
    const uint64_t observedMonotonicMs = monotonicMs();
    if (!SystemTimePlatform::monotonicAvailable(observedMonotonicMs)) return;
    lastSource = SystemTimeQuality::Source::Sntp;
    lastEpochMs = epochMs;
    lastMonotonicMs = observedMonotonicMs; authenticatedTransport = false;
    lastObservedMonotonicMs = lastMonotonicMs; clockDiscontinuity = false;
    recordRevision = observedRevision;
}
} // namespace

void SystemTimeQuality::installSntpObserver() { SystemTimePlatform::installObserver(sntpObserved); }
uint32_t SystemTimeQuality::sequence() { return revision.load(std::memory_order_acquire); }

namespace {
bool applyTrackedMilliseconds(int64_t epochMs, SystemTimeQuality::Source source,
                               bool authenticated, uint32_t expectedSequence, bool rtcSeed) {
    using Source = SystemTimeQuality::Source;
    if (epochMs < MIN_TIME_MS || epochMs >= MAX_TIME_MS || (authenticated && source != Source::Https) ||
        (source != Source::Http && source != Source::Https && source != Source::Mqtt &&
         !(rtcSeed && source == Source::Rtc))) return false;
    Guard guard;
    if (!guard.held || expectedSequence >= EXHAUSTED_REVISION - 1 ||
        SystemTimeQuality::sequence() != expectedSequence) return false;
    if (rtcSeed && (expectedSequence != 0 || recordRevision != 0 ||
        lastSource != Source::None || validWall(wallMs()))) return false;
    const uint64_t beforeWriteMs = monotonicMs();
    if (!SystemTimePlatform::monotonicAvailable(beforeWriteMs)) {
        if (lastSource != Source::None) clockDiscontinuity = true;
        return false;
    }
    // Recheck immediately before writing; another source or an already-valid
    // untracked clock must not be replaced by the RTC boot fallback.
    if (rtcSeed && validWall(wallMs())) return false;
    if (SystemTimeQuality::sequence() != expectedSequence) return false;
    if (!SystemTimePlatform::writeMs(epochMs)) return false;
    const uint64_t appliedMonotonicMs = monotonicMs();
    if (!SystemTimePlatform::monotonicAvailable(appliedMonotonicMs) ||
        appliedMonotonicMs < beforeWriteMs) {
        // The wall clock may already have changed. Retire the old evidence;
        // a later healthy monotonic sample must not revive that old source.
        observeRevision();
        return false;
    }
    const uint32_t appliedRevision = expectedSequence + 1;
    // Commit only if no SDK callback arrived during write or time capture.
    // A callback after this CAS still invalidates the stamped record below.
    if (!revision.compare_exchange_strong(expectedSequence, appliedRevision,
                                         std::memory_order_acq_rel)) return false;
    lastSource = source; lastEpochMs = epochMs; lastMonotonicMs = appliedMonotonicMs;
    authenticatedTransport = authenticated;
    lastObservedMonotonicMs = lastMonotonicMs; clockDiscontinuity = false;
    recordRevision = appliedRevision;
    return SystemTimeQuality::sequence() == appliedRevision;
}
} // namespace

bool SystemTimeQuality::applyMilliseconds(int64_t epochMs, Source source,
                                          bool authenticated, uint32_t expectedSequence) {
    return applyTrackedMilliseconds(epochMs, source, authenticated, expectedSequence, false);
}
bool SystemTimeQuality::claimRtcBootSeed() {
    auto expected = RtcBootStatus::NotAttempted;
    return rtcBootStatus.compare_exchange_strong(expected, RtcBootStatus::Pending,
                                                 std::memory_order_acq_rel);
}
void SystemTimeQuality::finishRtcBootSeed(RtcBootStatus status) {
    if (status == RtcBootStatus::NotAttempted || status == RtcBootStatus::Pending ||
        status == RtcBootStatus::Applied) return;
    auto expected = RtcBootStatus::Pending;
    rtcBootStatus.compare_exchange_strong(expected, status, std::memory_order_acq_rel);
}
bool SystemTimeQuality::seedFromRtcMilliseconds(int64_t epochMs, uint32_t expectedSequence) {
    // Consume before validation or I/O: failed or raced writes never auto-retry.
    auto expected = RtcBootStatus::Pending;
    if (!rtcBootStatus.compare_exchange_strong(expected, RtcBootStatus::Rejected,
                                               std::memory_order_acq_rel)) return false;
    if (!applyTrackedMilliseconds(epochMs, Source::Rtc, false, expectedSequence, true)) return false;
    rtcBootStatus.store(RtcBootStatus::Applied, std::memory_order_release);
    return true;
}

SystemTimeQuality::Snapshot SystemTimeQuality::snapshot() {
    Snapshot result;
    result.rtcBootStatus = rtcBootName(rtcBootStatus.load(std::memory_order_acquire));
    Guard guard;
    if (!guard.held) { result.quality = "busy"; return result; }
    result.sequence = sequence();
    const int64_t now = wallMs();
    result.epochMs = now;
    result.timeValid = now >= MIN_TIME_MS && now < MAX_TIME_MS;
    if (!result.timeValid) {
        if (lastSource != Source::None) clockDiscontinuity = true;
        result.quality = "invalid"; return result;
    }
    if (!recordRevision || result.sequence == EXHAUSTED_REVISION ||
        result.sequence != recordRevision || sequence() != result.sequence ||
        lastSource == Source::None) return result;
    result.source = sourceName(lastSource);
    result.transportAuthenticated = authenticatedTransport;
    const uint64_t mono = monotonicMs();
    if (!SystemTimePlatform::monotonicAvailable(mono)) {
        clockDiscontinuity = true;
        result.quality = "clock_changed"; return result;
    }
    if (mono < lastObservedMonotonicMs) clockDiscontinuity = true;
    else lastObservedMonotonicMs = mono;
    if (mono < lastMonotonicMs) { result.quality = "clock_changed"; return result; }
    result.ageMs = mono - lastMonotonicMs;
    if (clockDiscontinuity) { result.quality = "clock_changed"; return result; }
    if (result.ageMs > MAX_AGE_MS) { result.quality = "stale"; return result; }
    const int64_t drift = now - lastEpochMs - static_cast<int64_t>(result.ageMs);
    if (drift < -MAX_CLOCK_DRIFT_MS || drift > MAX_CLOCK_DRIFT_MS) {
        clockDiscontinuity = true;
        result.quality = "clock_changed"; return result;
    }
    if (sequence() != result.sequence) {
        result.quality = "unknown"; return result;
    }
    result.synced = lastSource != Source::Rtc;
    result.quality = lastSource == Source::Rtc ? "rtc_unverified" :
        (authenticatedTransport ? "synced_tls" : "synced_unverified");
    return result;
}
} // namespace FastBee
