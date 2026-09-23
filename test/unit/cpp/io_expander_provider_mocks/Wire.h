#pragma once
#include <cstddef>
#include <cstdint>
struct TwoWire {
    unsigned transmissions = 0, requests = 0, begins = 0, ends = 0;
    uint8_t txStatus = 0, sample = 0xff, lastWrite = 0xff;
    size_t written = 1, requested = 1;
    bool begin(int8_t, int8_t) { ++begins; return true; }
    void end() { ++ends; }
    void setClock(uint32_t) {}
    void setTimeOut(uint16_t) {}
    void beginTransmission(uint8_t) { ++transmissions; }
    size_t write(uint8_t value) { lastWrite = value; return written; }
    size_t write(const uint8_t*, size_t length) { return written ? length : 0; }
    uint8_t endTransmission(bool = true) { return txStatus; }
    size_t requestFrom(uint8_t, uint8_t) { ++requests; return requested; }
    int available() { return static_cast<int>(requested); }
    int read() { return sample; }
};
inline TwoWire Wire;
