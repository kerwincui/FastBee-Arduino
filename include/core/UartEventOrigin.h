#pragma once
#include <cstdint>

namespace FastBee {
// Boot-local provenance, copied by value through rule queues. Receive generation
// is globally unique until reboot; no peripheral ID allocation is necessary.
// This is an observation token, not a lock or proof of peer identity.
struct UartEventOrigin {
    uint32_t configEpoch = 0;
    uint32_t receiveGeneration = 0;
    bool required = false;

    static UartEventOrigin captured(uint32_t epoch, uint32_t generation) {
        UartEventOrigin value;
        value.configEpoch = epoch;
        value.receiveGeneration = generation;
        value.required = true; // Unknown capture must not become an unbound event.
        return value;
    }
    bool unbound() const { return !required && !configEpoch && !receiveGeneration; }
    bool matches(uint32_t epoch, uint32_t generation) const {
        return required && configEpoch && receiveGeneration &&
            configEpoch == epoch && receiveGeneration == generation;
    }
};
} // namespace FastBee
