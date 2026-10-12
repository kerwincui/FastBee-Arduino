#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace FastBee {
// Independent boot-local event counters. No payload, peripheral ID, reset API,
// allocation or scheduler lifetime dependency. A snapshot is not transactional.
class UartTextDiagnostics {
public:
    enum Event : size_t { DispatchAttempt, OversizedFrame, InvalidByteFrame,
        ReadFailure, SourceChange, StaleFrame, SerializationFailure,
        HardwareFifoFault, HardwareBufferFault, HardwareFrameFault,
        HardwareParityFault, HardwareBreakFault, Count };
    struct Status { uint32_t values[Count] = {}; bool lowerBounds = false; };
    static void record(Event event) {
        if (event >= Count) return;
        auto& counter = counters()[event];
        uint32_t value = counter.load(std::memory_order_relaxed);
        while (value != UINT32_MAX && !counter.compare_exchange_weak(value, value + 1,
            std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }
    static Status snapshot() {
        Status status;
        for (size_t i = 0; i < Count; ++i) {
            status.values[i] = counters()[i].load(std::memory_order_relaxed);
            if (status.values[i] == UINT32_MAX) status.lowerBounds = true;
        }
        return status;
    }
private:
    static std::atomic<uint32_t>* counters() {
        static std::atomic<uint32_t> values[Count]{};
        return values;
    }
};
} // namespace FastBee
