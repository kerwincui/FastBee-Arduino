#pragma once
#include <atomic>
#include <cstdint>
#include "InternalAtomicWord.h"

namespace FastBee {
enum class ReportReplayResult : uint8_t { Accepted = 1, Rejected = 2, InstanceChanged = 3 };

// Boot-local admission identity, never a hash of credentials or an address.
// No wrap/reuse: exhausted or contended allocation returns unknown (zero).
class ReportInstanceSequence {
    InternalAtomicWord _last;
public:
    explicit ReportInstanceSequence(uint32_t last = 0) : _last(last) {}
    uint32_t next() {
        auto* counter = _last.get();
        if (!counter) return 0;
        uint32_t last = counter->load(std::memory_order_relaxed);
        for (unsigned attempt = 0; attempt < 8 && last != UINT32_MAX; ++attempt) {
            if (counter->compare_exchange_weak(last, last + 1, std::memory_order_relaxed))
                return last + 1;
        }
        return 0;
    }
};
inline uint32_t nextReportInstanceId() {
    static ReportInstanceSequence sequence;
    return sequence.next();
}

// Snapshot BEFORE collection/callbacks; release the lease on return. Do not
// pin a protocol object across physical operations, delays, or rule dispatch.
// Busy/absent/test-only clients produce unknown and MUST NOT be rebound later.
template<class Acquire>
uint32_t captureReportInstance(Acquire acquire) {
    auto client = acquire();
    return client ? client->getReportInstanceId() : 0;
}

// Caller MUST keep its MQTT instance lease throughout this check and enqueue.
// Equality only permits replay to the SAME object, not another object with
// matching settings. This is not a complete configuration-revision contract.
template<class Client, class Payload>
ReportReplayResult replayBoundReport(Client& client, uint32_t origin, const Payload& payload) {
    const uint32_t current = client.getReportInstanceId();
    if (!origin || !current) return ReportReplayResult::Rejected;
    if (origin != current) return ReportReplayResult::InstanceChanged;
    if (!client.getIsConnected()) return ReportReplayResult::Rejected;
    return client.queueReportData(payload) ? ReportReplayResult::Accepted : ReportReplayResult::Rejected;
}

enum class BoundReportAdmission : uint8_t { Queued, Retained, Dropped };

// Use the SAME origin for immediate admission and fallback retention. Caller
// holds the current client lease throughout. Retention receives both identities
// so it can discard/account a known mismatch BEFORE evicting any queued report.
// A missing current client is unknown (zero), never evidence of a mismatch.
template<class Client, class Payload, class Retain>
BoundReportAdmission submitBoundReport(Client* client, uint32_t origin,
                                       const Payload& payload, Retain retain) {
    if (client && replayBoundReport(*client, origin, payload) == ReportReplayResult::Accepted)
        return BoundReportAdmission::Queued;
    const uint32_t current = client ? client->getReportInstanceId() : 0;
    return retain(payload, origin, current) ? BoundReportAdmission::Retained : BoundReportAdmission::Dropped;
}
} // namespace FastBee
