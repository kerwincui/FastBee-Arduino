#pragma once
#include "core/PeripheralManager.h"
#include <cstring>

namespace FastBee {
inline bool parseServoRawDuty(const PeripheralConfig& config, const String& text, uint32_t& value) {
    value = 0;
    if (config.type != PeripheralType::PWM_SERVO || text.length() > 10 ||
        text.length() != std::strlen(text.c_str()) ||
        config.params.servo.pwmResolution < 1 || config.params.servo.pwmResolution > 16) return false;
    const uint32_t maximum = (1UL << config.params.servo.pwmResolution) - 1UL;
    for (size_t i = 0; i < text.length(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(c - '0');
        if (digit > maximum || value > (maximum - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    return true; // Empty typed SET_PWM retains its existing zero default.
}
inline bool parseStepperCommandValue(const String& command, const String& text, int& value) {
    value = 0;
    if (text.length() != std::strlen(text.c_str())) return false;
    if (command == "direction") {
        String direction = text;
        if (direction != text) return false;
        direction.toLowerCase();
        if (direction == "reverse" || direction == "backward" || direction == "ccw" || direction == "-1")
            value = -1;
        else if (direction == "forward" || direction == "cw" || direction == "1") value = 1;
        else return false;
        return true;
    }
    const bool speed = command == "speed" || command == "setspeed" || command == "faster" ||
        command == "accelerate" || command == "speedup" || command == "inc" ||
        command == "slower" || command == "decelerate" || command == "speeddown" || command == "dec";
    if (!speed) return text.isEmpty();
    if (text.length() > 5) return false;
    uint32_t parsed = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') return false;
        parsed = parsed * 10U + static_cast<uint32_t>(c - '0');
        if (parsed > UINT16_MAX) return false;
    }
    value = static_cast<int>(parsed);
    return true;
}
} // namespace FastBee
