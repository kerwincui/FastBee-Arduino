#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "network/FixedJsonField.h"
#include <cstdint>

namespace FastBee {
inline bool validRtcBootOffset(int value) { return value >= -840 && value <= 840; }

inline void serializeRtcBootSeedConfig(JsonObject params, bool enabled, int16_t offset) {
    setFixedJsonField(params, "bootSeed", enabled);
    // Omission retains a genuinely unspecified offset in old/default configs.
    if (offset != INT16_MIN) setFixedJsonField(params, "utcOffsetMinutes", offset);
}
inline bool parseRtcBootSeedConfig(JsonObjectConst params, bool& enabled,
                                    int16_t& offset, String& error) {
    bool nextEnabled = enabled;
    int16_t nextOffset = offset;
    if (!params["bootSeed"].isUnbound()) {
        if (!params["bootSeed"].is<bool>()) { error = "DS1302_BOOT_BOOLEAN"; return false; }
        nextEnabled = params["bootSeed"].as<bool>();
    }
    if (!params["utcOffsetMinutes"].isUnbound()) {
        const auto value = params["utcOffsetMinutes"];
        if (!value.is<int>() || !validRtcBootOffset(value.as<int>())) {
            error = "DS1302_BOOT_OFFSET"; return false;
        }
        nextOffset = static_cast<int16_t>(value.as<int>());
    }
    if (nextEnabled && !validRtcBootOffset(nextOffset)) {
        error = "DS1302_BOOT_OFFSET_REQUIRED"; return false;
    }
    enabled = nextEnabled; offset = nextOffset;
    return true;
}
inline bool parseRtcBootSeedForm(bool hasEnabled, const String& enabledText,
                                 bool hasOffset, const String& offsetText,
                                 bool& enabled, int16_t& offset, String& error) {
    bool nextEnabled = enabled;
    int16_t nextOffset = offset;
    if (hasEnabled) {
        if (enabledText == "true" || enabledText == "1") nextEnabled = true;
        else if (enabledText == "false" || enabledText == "0") nextEnabled = false;
        else { error = "DS1302_BOOT_BOOLEAN"; return false; }
    }
    if (hasOffset) {
        if (offsetText.isEmpty()) nextOffset = INT16_MIN;
        else {
            const size_t length = offsetText.length();
            const bool negative = offsetText[0] == '-';
            size_t index = negative ? 1 : 0;
            if (index == length || length > 4) { error = "DS1302_BOOT_OFFSET"; return false; }
            int value = 0;
            for (; index < length; ++index) {
                const char c = offsetText[index];
                if (c < '0' || c > '9') { error = "DS1302_BOOT_OFFSET"; return false; }
                value = value * 10 + (c - '0');
            }
            if (negative) value = -value;
            if (!validRtcBootOffset(value)) { error = "DS1302_BOOT_OFFSET"; return false; }
            nextOffset = static_cast<int16_t>(value);
        }
    }
    if (nextEnabled && !validRtcBootOffset(nextOffset)) {
        error = "DS1302_BOOT_OFFSET_REQUIRED"; return false;
    }
    enabled = nextEnabled; offset = nextOffset;
    return true;
}
} // namespace FastBee
