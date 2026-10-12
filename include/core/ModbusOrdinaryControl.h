#pragma once
#include "core/NativeOutputControl.h"
#include "core/ModbusOutputFrame.h"
#include "core/SensorConfigEpoch.h"
#include "core/RuleOutputIdentity.h"
#include "core/ScreenOutputIdentity.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace FastBee {
// An ownership key covers the complete slave. Endpoint mapping is a separate
// frozen capability, never an independent owner for another alias/channel.
struct ModbusControlTarget {
    uint32_t instance = 0, configuration = 0;
    uint8_t slave = 0, index = 0, function = 0, count = 0, batchType = 0;
    uint16_t first = 0, batch = 0;
    bool nc = false;
    enum class Kind : uint8_t { Relay, Motor, Frame, Maintenance, Snapshot };
    Kind kind = Kind::Relay;
    uint16_t motorRegisters[5]{};
    uint16_t stopValue = 0, frameQuantity = 0;
    uint8_t frameSlave = 0;
    uint32_t hardwareWindowMs = 0; // Derived from configured timing semantics, never client omission.
    ModbusFrameAccess frameAccess = ModbusFrameAccess::Standard;
    ModbusFrameTransform frameTransform = ModbusFrameTransform::None;
    bool valid() const {
        if (kind == Kind::Snapshot) return instance && configuration && slave && slave <= 247;
        if (kind == Kind::Frame || kind == Kind::Maintenance)
            return instance && configuration && frameQuantity &&
                (kind == Kind::Maintenance ? slave == 0 && frameAccess == ModbusFrameAccess::Maintenance :
                    slave && slave <= 247 && slave == frameSlave && frameAccess != ModbusFrameAccess::Maintenance);
        if (kind == Kind::Motor) return instance && configuration && slave && slave <= 247 &&
            (function == 6 || function == 16);
        return instance && configuration && slave && slave <= 247 && count &&
            (function == 5 || function == 6) && uint32_t(first) + count <= 65536U &&
            (!batch || (batchType <= 1 && (batchType == 1 || count <= 16) &&
                (function != 6 || batch < first || uint32_t(batch) >= uint32_t(first) + count)));
    }
    bool accepts(uint8_t fc, uint16_t address, uint16_t value) const {
        if (!valid() || kind == Kind::Frame || kind == Kind::Maintenance || kind == Kind::Snapshot) return false;
        if (kind == Kind::Motor) {
            if (fc != function) return false;
            for (uint16_t reg : motorRegisters) if (reg == address) return true;
            return false;
        }
        if (fc == function && address >= first && uint32_t(address) < uint32_t(first) + count)
            return fc == 5 ? value == 0 || value == 0xff00 : value <= 1;
        if (batch && fc == 6 && address == batch) {
            const uint16_t maximum = batchType == 1 ? 1 :
                count == 16 ? 0xffff : static_cast<uint16_t>((1U << count) - 1);
            return value == 0 || value == maximum;
        }
        return false;
    }
    bool sameMapping(const ModbusControlTarget& other) const {
        return instance == other.instance && configuration == other.configuration &&
            slave == other.slave && index == other.index && function == other.function &&
            count == other.count && batchType == other.batchType && first == other.first &&
            batch == other.batch && nc == other.nc && kind == other.kind && stopValue == other.stopValue &&
            frameQuantity == other.frameQuantity && frameSlave == other.frameSlave &&
            hardwareWindowMs == other.hardwareWindowMs &&
            frameAccess == other.frameAccess && frameTransform == other.frameTransform &&
            std::memcmp(motorRegisters, other.motorRegisters, sizeof(motorRegisters)) == 0;
    }
    void key(char (&out)[41]) const {
        if (kind == Kind::Maintenance) std::snprintf(out, sizeof(out), "mb:%lu:bus", static_cast<unsigned long>(instance));
        else std::snprintf(out, sizeof(out), "mb:%lu:%u", static_cast<unsigned long>(instance), slave);
    }
};

// Fixed PM-owned cells remain alive until reboot. Each transaction advances a
// nonreused 30-bit sequence; low bits are Armed=1, Claimed=2, Revoked=3.
// Retirement ORs Revoked; a bus-boundary CAS consumes Armed exactly once.
struct ModbusWriteTicket {
    ModbusControlTarget target;
    OrdinaryControlContext context;
    std::atomic<uint32_t>* gate = nullptr;
    const std::atomic<uint32_t>* uartGeneration = nullptr;
    uint32_t armed = 0;
    uint8_t function = 0;
    uint16_t address = 0, value = 0;
    bool toggle = false;
    const ModbusOutputFrame* frame = nullptr; // Pins an immutable transaction-local copy.
    uint64_t acquiredMs = 0;
    uint32_t holdMs = 0;
    bool sourceCurrent(uint64_t now, uint32_t operationNow) const {
        return (!context.operationDeadlineAt ||
                static_cast<int32_t>(operationNow - context.operationDeadlineAt) < 0) &&
            now >= acquiredMs && (!holdMs || now - acquiredMs < holdMs) &&
            SensorConfigEpoch::matches(context.configEpoch) &&
            (!context.ruleBound || RuleOutputIdentity::matches(context.ruleEpoch)) &&
            (context.request.actor.source != OrdinaryControl::Source::Screen ||
                ScreenOutputIdentity::matches(context.request.actor.identity)) &&
            (context.uartOrigin.unbound() || (context.uartOrigin.required &&
                context.uartOrigin.configEpoch == context.configEpoch && uartGeneration &&
                uartGeneration->load(std::memory_order_acquire) == context.uartOrigin.receiveGeneration));
    }
    bool claim(uint64_t now, uint32_t operationNow) const {
        if (!gate || (armed & 3U) != 1U || !sourceCurrent(now, operationNow)) return false;
        uint32_t expected = armed;
        if (!gate->compare_exchange_strong(expected, armed + 1U,
                std::memory_order_acq_rel, std::memory_order_acquire)) return false;
        // Epoch readers never acquire PM/Panel/UART owner locks under the bus.
        if (sourceCurrent(now, operationNow)) return true;
        gate->fetch_or(3U, std::memory_order_acq_rel);
        return false;
    }
};

struct ModbusOrdinarySlot {
    ModbusControlTarget target;
    OrdinaryControl::Lease lease;
    OrdinaryControlContext context;
    std::atomic<uint32_t> gate{0};
    uint32_t sequence = 0, failures = 0;
    uint64_t firstFailureOperation = 0;
    const char* firstFailure = nullptr;
    // Hardware timing effects survive source/config retirement and handler replacement.
    uint64_t hardwareUntilMs = 0;
    bool inFlight = false, terminalRequested = false, terminalCancelled = false;
    bool workerReserved = false, workerEmergencyStop = false;
    uint64_t pendingStopOperation = 0, pendingStopGeneration = 0;
    OrdinaryControl::Actor pendingStopActor;
    uint64_t workerGeneration = 0, pendingStopAcquiredMs = 0;
    bool pendingStopTerminal = false, pendingStopCancelled = false;
};

// PM owns this table until reboot. Unused Modbus must not reserve every ticket
// cell in static DRAM. Allocate only at write admission, under the PM mutex;
// observers/retirement iterate an empty range before then. Never release or
// relocate allocated cells: an in-flight ticket may still hold its gate address.
class ModbusOrdinarySlots {
public:
    static constexpr size_t CAPACITY = 16;
    ModbusOrdinarySlots() = default;
    ModbusOrdinarySlots(const ModbusOrdinarySlots&) = delete;
    ModbusOrdinarySlots& operator=(const ModbusOrdinarySlots&) = delete;
    bool ensureAllocated() noexcept {
        if (slots_) return true;
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
        // Gate atomics must stay in internal DRAM even on PSRAM targets.
        void* memory = heap_caps_malloc(sizeof(Storage),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
        void* memory = ::operator new(sizeof(Storage), std::nothrow);
#endif
        if (!memory) return false;
        auto* candidate = new (memory) Storage{};
        slots_ = candidate->slots;
        return true;
    }
    ModbusOrdinarySlot* begin() noexcept { return slots_; }
    ModbusOrdinarySlot* end() noexcept { return slots_ ? slots_ + CAPACITY : nullptr; }
    const ModbusOrdinarySlot* begin() const noexcept { return slots_; }
    const ModbusOrdinarySlot* end() const noexcept { return slots_ ? slots_ + CAPACITY : nullptr; }
    ModbusOrdinarySlot& operator[](size_t index) noexcept { return slots_[index]; }
    const ModbusOrdinarySlot& operator[](size_t index) const noexcept { return slots_[index]; }
private:
    struct Storage { ModbusOrdinarySlot slots[CAPACITY]; };
    ModbusOrdinarySlot* slots_ = nullptr;
};
} // namespace FastBee
