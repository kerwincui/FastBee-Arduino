#pragma once
#include <cstdint>

namespace FastBee {
// Memory retry/cooldown intervals are below INT32_MAX milliseconds. Compare
// elapsed uint32 ticks so a NORMAL sample after millis wrap still recovers.
inline bool mqttMemoryDeadlineReached(uint32_t now, uint32_t deadline) {
    return !deadline || static_cast<int32_t>(now - deadline) >= 0;
}
} // namespace FastBee
