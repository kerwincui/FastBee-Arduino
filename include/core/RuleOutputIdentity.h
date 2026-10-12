#pragma once
#include <atomic>
#include <stdint.h>

namespace FastBee {
namespace RuleOutputIdentity {
// Two boot-scoped counters, not an SDK resource registry. 32-bit atomics match
// ESP32's existing UART generation approach; exhaustion is permanent until boot.
inline std::atomic<uint32_t>& epochStorage() { static std::atomic<uint32_t> value{1}; return value; }
inline std::atomic<uint32_t>& actorStorage() { static std::atomic<uint32_t> value{0}; return value; }
inline uint32_t currentEpoch() { return epochStorage().load(std::memory_order_acquire); }
inline bool matches(uint32_t epoch) { return epoch && currentEpoch() == epoch; }
inline void invalidate() {
    uint32_t value = epochStorage().load(std::memory_order_relaxed);
    while (value && !epochStorage().compare_exchange_weak(value, value == UINT32_MAX ? 0 : value + 1,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {}
}
inline uint64_t allocateActor() {
    uint32_t value = actorStorage().load(std::memory_order_relaxed);
    while (value != UINT32_MAX) {
        if (actorStorage().compare_exchange_weak(value, value + 1,
                std::memory_order_acq_rel, std::memory_order_relaxed))
            return static_cast<uint64_t>(value + 1);
    }
    return 0;
}
} // namespace RuleOutputIdentity
} // namespace FastBee

