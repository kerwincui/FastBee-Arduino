#pragma once
#include "../../../mocks/Arduino.h"
#include <SPI.h>
constexpr int INPUT = 0, OUTPUT = 1;
inline int spiTestPinModes = 0, spiTestPinWrites = 0;
inline std::map<uint8_t, int> spiTestPinMode;
inline void pinMode(uint8_t pin, int mode) {
    ++spiTestPinModes; spiTestPinMode[pin] = mode;
    spiIoEvents.push_back(mode == INPUT ? "input" : "output");
}
inline void digitalWrite(uint8_t, int value) {
    ++spiTestPinWrites; spiIoEvents.push_back(value == LOW ? "low" : "high");
}
