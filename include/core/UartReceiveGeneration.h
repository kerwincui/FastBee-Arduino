#pragma once
#include <atomic>
#include <cstdint>

namespace FastBee {
// Hardware begin identity, separate from configuration and controller ownership.
// All slot access is serialized by PeripheralManager. Allocation is boot-local,
// shared across owners, never zero and never reused after stop or failed begin.
class UartReceiveGeneration {
public:
    bool begin(uint8_t port) {
        if (!port || port >= 3) return false;
        uint32_t next = counter().load(std::memory_order_relaxed);
        do {
            if (next == UINT32_MAX) return false;
        } while (!counter().compare_exchange_weak(next, next + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed));
        slots_[port].store(next, std::memory_order_release);
        return true;
    }
    void end(uint8_t port) { if (port < 3) slots_[port].store(0, std::memory_order_release); }
    uint32_t current(uint8_t port) const {
        return port < 3 ? slots_[port].load(std::memory_order_acquire) : 0;
    }
    const std::atomic<uint32_t>* handle(uint8_t port) const { return port && port < 3 ? &slots_[port] : nullptr; }
    UartReceiveGeneration() = default;
    UartReceiveGeneration(const UartReceiveGeneration&) = delete;
    UartReceiveGeneration& operator=(const UartReceiveGeneration&) = delete;
private:
    static std::atomic<uint32_t>& counter() {
        static std::atomic<uint32_t> value{1};
        return value;
    }
    std::atomic<uint32_t> slots_[3]{};
};
} // namespace FastBee
