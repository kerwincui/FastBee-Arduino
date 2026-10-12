#pragma once
#include <atomic>
#include <cstdint>
#include "core/UartTextDiagnostics.h"

namespace FastBee {
// Static observer state: callbacks never retain a manager/driver pointer, acquire
// its lock, allocate, read the port or invoke application code. One non-reused
// receive token is armed per generic controller. The first observed error revokes
// that token atomically; later callbacks cannot revoke a replacement stream.
// This cannot observe an SDK event that was lost, delayed or never delivered.
class UartReceiveFault {
public:
    enum class Error { FifoOverflow, BufferFull, Frame, Parity, Break };
    static void arm(uint8_t port, uint32_t generation) {
        if (port && port < 3) slots()[port].store(generation, std::memory_order_seq_cst);
    }
    static void disarm(uint8_t port, uint32_t generation) {
        if (!port || port >= 3 || !generation) return;
        slots()[port].compare_exchange_strong(generation, 0, std::memory_order_seq_cst);
    }
    static bool healthy(uint8_t port, uint32_t generation) {
        return port && port < 3 && generation &&
               slots()[port].load(std::memory_order_seq_cst) == generation;
    }
    static void observe(uint8_t port, uint32_t generation, Error error) {
        if (!port || port >= 3 || !generation) return;
        UartTextDiagnostics::Event event;
        switch (error) {
            case Error::FifoOverflow: event = UartTextDiagnostics::HardwareFifoFault; break;
            case Error::BufferFull: event = UartTextDiagnostics::HardwareBufferFault; break;
            case Error::Frame: event = UartTextDiagnostics::HardwareFrameFault; break;
            case Error::Parity: event = UartTextDiagnostics::HardwareParityFault; break;
            case Error::Break: event = UartTextDiagnostics::HardwareBreakFault; break;
            default: return;
        }
        // Count first-observed stream faults, not bytes, frames or every SDK event.
        if (slots()[port].compare_exchange_strong(generation, 0, std::memory_order_seq_cst))
            UartTextDiagnostics::record(event);
    }
private:
    static std::atomic<uint32_t>* slots() {
        static std::atomic<uint32_t> values[3]{};
        return values;
    }
};
} // namespace FastBee
