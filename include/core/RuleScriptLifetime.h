#pragma once

#include <atomic>
#include <cstdint>

namespace FastBee {

// Counts access to the rules AND their mutex. Closing admission is immediate;
// the last holder must unlock before allowing the owner to reclaim resources.
// Starting/cleanup each have one exclusive owner, with no waiting or allocation.
class RuleScriptLifetime {
public:
    bool beginStart() {
        uint32_t expected = 0;
        return state_.compare_exchange_strong(expected, Starting, std::memory_order_acq_rel);
    }
    bool publishStart() {
        uint32_t expected = Starting;
        return state_.compare_exchange_strong(expected, Ready, std::memory_order_acq_rel);
    }
    // Only the initializer calls this, after destroying unpublished resources.
    void discardStart() { state_.store(0, std::memory_order_release); }

    bool acquire() {
        auto state = state_.load(std::memory_order_acquire);
        while ((state & Ready) && !(state & Retiring) && (state & References) != References) {
            if (state_.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel)) return true;
        }
        return false;
    }
    void release() { state_.fetch_sub(1, std::memory_order_acq_rel); }

    void requestStop() {
        auto state = state_.load(std::memory_order_acquire);
        while (state && !(state & (Retiring | Cleaning))) {
            const auto closed = (state & ~Ready) | Retiring;
            if (state_.compare_exchange_weak(state, closed, std::memory_order_acq_rel)) return;
        }
    }
    bool claimCleanup() {
        uint32_t expected = Retiring;
        return state_.compare_exchange_strong(expected, Cleaning, std::memory_order_acq_rel);
    }
    void finishCleanup() { state_.store(0, std::memory_order_release); }
    bool running() const { return (state_.load(std::memory_order_acquire) & Ready) != 0; }
    bool idle() const { return state_.load(std::memory_order_acquire) == 0; }
    bool starting() const { return (state_.load(std::memory_order_acquire) & Starting) != 0; }
    bool retiring() const { return (state_.load(std::memory_order_acquire) & (Retiring | Cleaning)) != 0; }

private:
    static constexpr uint32_t Ready = 1UL << 31;
    static constexpr uint32_t Retiring = 1UL << 30;
    static constexpr uint32_t Starting = 1UL << 29;
    static constexpr uint32_t Cleaning = 1UL << 28;
    static constexpr uint32_t References = (1UL << 28) - 1;
    std::atomic<uint32_t> state_{0};
};

}  // namespace FastBee
