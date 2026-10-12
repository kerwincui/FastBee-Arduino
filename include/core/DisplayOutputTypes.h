#pragma once
#include <stdint.h>
#include <Arduino.h>

namespace FastBee {
// Existing named display actions only; no raw pixel, IR or new transport API.
enum class DisplayOutputCommand : uint8_t { Number, Text, Clear, Brightness, SensorPage };
struct DisplayWriteResult {
    bool attempted = false;
    bool acknowledged = false;
    const char* reason = "DISPLAY_NOT_READY";
};
// Commands already exposed by the legacy LCD/OLED rule, script and HTTP paths.
enum class LcdOutputCommand : uint8_t {
    CustomText, Clear, SensorPage, TextAt, TextLine, SystemInfo, Font
};
struct LcdOutputRequest {
    LcdOutputCommand command = LcdOutputCommand::Clear;
    String text;
    int16_t value = 0; // Sensor page (-1 automatic), line or font.
    uint8_t x = 0, y = 0, align = 1;
};
} // namespace FastBee
