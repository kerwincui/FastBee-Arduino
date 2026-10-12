#pragma once
#include <stdint.h>

namespace FastBee {
// Trivial runtime-only storage fits inside the existing encoder-sized union.
// No ISR, queue, allocation or independent timer: reads sample the pad.
struct RadarInputState {
    uint32_t generation;
    uint32_t candidateSince;
    uint32_t holdSince;
    uint16_t debounceMs;
    uint16_t holdMs;
    uint8_t pin;
    bool activeHigh;
    bool initialized;
    bool candidateKnown;
    bool candidateHigh;
    bool stableKnown;
    bool stableHigh;
    bool holdPending;
    bool present;
};

inline bool sampleRadarInput(RadarInputState& state, bool active, uint32_t now, bool& present) {
    if (!state.candidateKnown || active != state.candidateHigh) {
        state.candidateKnown = true;
        state.candidateHigh = active;
        state.candidateSince = now;
    }
    if (static_cast<uint32_t>(now - state.candidateSince) >= state.debounceMs &&
        (!state.stableKnown || state.stableHigh != state.candidateHigh)) {
        const bool first = !state.stableKnown;
        state.stableKnown = true;
        state.stableHigh = state.candidateHigh;
        if (state.stableHigh) {
            state.present = true;
            state.holdPending = false;
        } else {
            // The hold starts at CONFIRMED absence, after edge debounce.
            state.holdPending = !first && state.present && state.holdMs != 0;
            state.holdSince = now;
            if (!state.holdPending) state.present = false;
        }
    }
    // A raw active glitch does not renew the hold. Only a debounced active
    // transition above does, including when it coincides with hold expiry.
    if (state.holdPending && static_cast<uint32_t>(now - state.holdSince) >= state.holdMs) {
        state.holdPending = false;
        state.present = false;
    }
    if (!state.stableKnown) return false;
    present = state.present;
    return true;
}
} // namespace FastBee
