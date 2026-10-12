#pragma once
#include "core/FeatureInstanceRegistry.h"

namespace FastBee {

// Static-lifetime bookkeeping only. Counter saturation or an unmatched exit is
// permanently unknown, never wraparound to a false idle/stopped observation.
template<uint32_t Maximum = UINT32_MAX - 1>
class FeatureObservationCounter {
public:
    static_assert(Maximum > 0 && Maximum < UINT32_MAX, "Counter must reserve an unknown state");
    void enter() {
        auto current = value_.load(std::memory_order_acquire);
        while (current != UINT32_MAX && !value_.compare_exchange_weak(current,
                current == Maximum ? UINT32_MAX : current + 1,
                std::memory_order_acq_rel, std::memory_order_acquire)) {}
    }
    void leave() {
        auto current = value_.load(std::memory_order_acquire);
        while (current != UINT32_MAX && !value_.compare_exchange_weak(current,
                current ? current - 1 : UINT32_MAX,
                std::memory_order_acq_rel, std::memory_order_acquire)) {}
    }
    FeatureInstanceSnapshot snapshot(FeatureInstanceScope scope) const {
        const auto value = value_.load(std::memory_order_acquire);
        if (value == UINT32_MAX) return FeatureInstanceSnapshot::unavailable(
            FeatureInstanceReason::InvalidSnapshot, scope);
        if (scope == FeatureInstanceScope::ExecutionActivity)
            return {value ? FeatureInstanceState::Running : FeatureInstanceState::Idle, 0, scope};
        return FeatureInstanceSnapshot::lifecycle(value ? FeatureInstanceState::Allocated : FeatureInstanceState::Stopped);
    }
private:
    std::atomic<uint32_t> value_{0};
};

inline FeatureObservationCounter<>& commandScriptExecutionCounter() {
    static FeatureObservationCounter<> counter;
    static const FeatureInstanceObserver observer{"commandScript", [](const void* context) {
        return static_cast<const FeatureObservationCounter<>*>(context)->snapshot(FeatureInstanceScope::ExecutionActivity);
    }, &counter};
    FeatureInstanceRegistry::instance().registerObserver(&observer);
    return counter;
}

class CommandScriptExecutionObservation {
public:
    CommandScriptExecutionObservation() : counter_(commandScriptExecutionCounter()) { counter_.enter(); }
    ~CommandScriptExecutionObservation() { counter_.leave(); }
    CommandScriptExecutionObservation(const CommandScriptExecutionObservation&) = delete;
    CommandScriptExecutionObservation& operator=(const CommandScriptExecutionObservation&) = delete;
private:
    FeatureObservationCounter<>& counter_;
};

} // namespace FastBee
