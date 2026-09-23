#pragma once
#include <cstdint>
struct TwoWire {
    unsigned begins = 0, ends = 0;
    uint32_t clock = 0;
    uint16_t timeout = 0;
    bool beginOk = true;
    bool begin(int8_t, int8_t) { ++begins; return beginOk; }
    void end() { ++ends; }
    void setClock(uint32_t value) { clock = value; }
    void setTimeOut(uint16_t value) { timeout = value; }
};
inline TwoWire Wire;
