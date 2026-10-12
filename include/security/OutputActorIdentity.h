#pragma once
#include <atomic>
#include <cstdint>

namespace FastBee {
// Boot-local identity, never an authentication token. Static 32-bit atomic
// storage works on the supported Xtensa and RISC-V targets; exhaustion denies
// new output owners without denying ordinary management authentication.
class OutputActorSequence {
public:
    explicit OutputActorSequence(uint32_t issued = 0) : issued_(issued) {}
    uint64_t issue() {
        uint32_t previous = issued_.load(std::memory_order_relaxed);
        do {
            if (previous == UINT32_MAX) return 0;
        } while (!issued_.compare_exchange_weak(previous, previous + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed));
        return static_cast<uint64_t>(previous) + 1;
    }
private:
    std::atomic<uint32_t> issued_;
};
inline uint64_t nextOutputActorIdentity() {
    static OutputActorSequence sequence;
    return sequence.issue();
}
} // namespace FastBee
