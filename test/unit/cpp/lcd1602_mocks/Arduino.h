#pragma once
#define delay lcdUnusedRealDelay
#include "../../../mocks/Arduino.h"
#undef delay
#include <cmath>
inline std::vector<unsigned long> lcdDelaysUs;
inline void delay(unsigned long ms) { lcdDelaysUs.push_back(ms * 1000); }
inline void delayMicroseconds(unsigned int us) { lcdDelaysUs.push_back(us); }
inline char* dtostrf(double value, signed char width, unsigned char precision, char* output) {
    std::sprintf(output, "%*.*f", width, precision, value);
    return output;
}
