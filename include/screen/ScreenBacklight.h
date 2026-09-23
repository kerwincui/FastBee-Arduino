#pragma once

#include <cstdint>

namespace FastBee {
namespace Screen {

// Zero is retained as the persisted "minimum" setting for compatibility,
// but it must never turn a live screen completely black.  Explicit sleep is
// the only operation that is allowed to remove the backlight.
constexpr uint8_t MIN_VISIBLE_BRIGHTNESS_PERCENT = 10;

inline uint8_t normalizeBrightnessPercent(uint8_t percent) {
    if (percent > 100) return 100;
    return percent < MIN_VISIBLE_BRIGHTNESS_PERCENT ? MIN_VISIBLE_BRIGHTNESS_PERCENT : percent;
}

// User brightness always remains visible (10..100% electrical brightness).
// Only explicit sleep/shutdown requests extinguish the backlight. Polarity is
// board configuration, including CH32 PWM; never infer it from panel model.
inline uint8_t backlightDuty(uint8_t percent, bool sleeping, bool activeHigh) {
    const uint32_t level = normalizeBrightnessPercent(percent);
    const uint8_t duty = sleeping ? 0 : static_cast<uint8_t>(
        ((1000U + level * 90U) * 255U + 5000U) / 10000U);
    return activeHigh ? duty : static_cast<uint8_t>(255U - duty);
}

}  // namespace Screen
}  // namespace FastBee
