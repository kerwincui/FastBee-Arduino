#pragma once
#include "../generic_spi_mocks/Arduino.h"
inline bool tftAttachOk = true, tftWriteOk = true, tftDetachOk = true;
inline int tftAttaches = 0, tftDetaches = 0, tftDuty = -1;
inline unsigned long tftClock = 0;
inline std::function<void()> tftOnDelay;
inline void tftDelay(unsigned long ms) { tftClock += ms; if (tftOnDelay) tftOnDelay(); }
#define delay tftDelay
inline bool ledcAttachChannel(uint8_t, uint32_t, uint8_t, uint8_t) {
    ++tftAttaches; return tftAttachOk;
}
inline bool ledcWrite(uint8_t, uint32_t duty) {
    if (!tftWriteOk) return false;
    tftDuty = static_cast<int>(duty); return true;
}
inline bool ledcDetach(uint8_t) { ++tftDetaches; return tftDetachOk; }
