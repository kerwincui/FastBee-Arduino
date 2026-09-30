#pragma once
#include <algorithm>
#include <cstdint>

namespace FastBee {

// Open-loop containment uses possible position bounds, never an ACK as feedback.
// Stalls and an early stop may shorten a move; they cannot restore travel budget.
struct MotorTravelGuard {
    bool referenced = false;
    int32_t minimum = 0, maximum = 0, lower = 0, upper = 0;

    bool configure(int32_t minValue, int32_t maxValue) {
        if (minValue != minimum || maxValue != maximum) referenced = false;
        minimum = minValue;
        maximum = maxValue;
        if (maximum <= minimum) referenced = false;
        return maximum > minimum;
    }

    bool confirm(int32_t position) {
        referenced = false;
        if (maximum <= minimum || position < minimum || position > maximum) return false;
        lower = upper = position;
        referenced = true;
        return true;
    }

    void invalidate() { referenced = false; }

    uint16_t available(bool forward, uint16_t requested) const {
        if (!referenced || maximum <= minimum || lower < minimum || upper > maximum || lower > upper) return 0;
        const int64_t room = forward ? static_cast<int64_t>(maximum) - upper
                                     : static_cast<int64_t>(lower) - minimum;
        if (room <= 0 || !requested) return 0;
        return static_cast<uint16_t>(std::min<int64_t>(requested, room));
    }

    bool reserve(bool forward, uint16_t pulse) {
        if (!pulse || available(forward, pulse) != pulse) return false;
        if (forward) upper = static_cast<int32_t>(static_cast<int64_t>(upper) + pulse);
        else lower = static_cast<int32_t>(static_cast<int64_t>(lower) - pulse);
        return true;
    }
};

} // namespace FastBee
