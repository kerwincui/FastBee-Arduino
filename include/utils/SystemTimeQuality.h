#pragma once
#include <cstdint>

namespace FastBee {

// Provenance/freshness, not clock accuracy or trust in the operator's server.
// RTC boot adoption is unverified and never counts as synchronization.
class SystemTimeQuality {
public:
    enum class Source : uint8_t { None, Sntp, Http, Https, Mqtt, Rtc };
    enum class RtcBootStatus : uint8_t {
        NotAttempted, Pending, Disabled, NotCompiled, Busy, SkippedValid,
        MultipleSources, Unavailable, InvalidClock, Stopped, InvalidOffset,
        InvalidEpoch, Rejected, Applied
    };
    struct Snapshot {
        const char* source = "unknown";
        const char* quality = "unknown";
        const char* rtcBootStatus = "not_attempted";
        bool timeValid = false;
        bool synced = false;
        bool transportAuthenticated = false;
        uint64_t ageMs = 0;
        int64_t epochMs = 0; // Wall clock captured in the same provenance snapshot.
        uint32_t sequence = 0;
    };
    // Boot hook claims once even when disabled or unsuccessful. No retry,
    // runtime endpoint, RTC write, timezone mutation or sync event is implied.
    static bool claimRtcBootSeed();
    static void finishRtcBootSeed(RtcBootStatus status);
    static bool seedFromRtcMilliseconds(int64_t epochMs, uint32_t expectedSequence);
    static void installSntpObserver();
    static uint32_t sequence();
    // Observed invalid wall time, excessive drift or monotonic regression
    // invalidates the source record until a new accepted synchronization.
    static Snapshot snapshot();
    // Recheck freshness and the elapsed wall/monotonic window, not just the
    // revision: expiry, untracked jumps and stalled actions need no new source.
    static bool stillCurrent(const Snapshot& captured, uint32_t maxElapsedMs) {
        if (!captured.synced || !captured.timeValid) return false;
        const auto current = snapshot();
        if (!current.synced || !current.timeValid || current.sequence != captured.sequence ||
            current.ageMs < captured.ageMs || current.epochMs < captured.epochMs) return false;
        return current.ageMs - captured.ageMs <= maxElapsedMs &&
            static_cast<uint64_t>(current.epochMs - captured.epochMs) <= maxElapsedMs;
    }
    // Reject responses started before another source successfully changed time.
    // False does not promise rollback if the SDK SNTP writer raced this call.
    static bool applyMilliseconds(int64_t epochMs, Source source,
                                  bool transportAuthenticated, uint32_t expectedSequence);
};
} // namespace FastBee
