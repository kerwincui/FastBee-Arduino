#pragma once
#include <Arduino.h>
#include <array>
class TwoWire {
public:
    std::array<uint8_t, 22> registers{};
    unsigned transmissions = 0, requests = 0, reads = 0, starts = 0, ends = 0;
    unsigned writeLimit = 255;
    int nextByte = -2;
    bool ack = true, shortRead = false, emptyRead = false, extraRead = false, infinite = false;
    uint8_t pointer = 0;
    int availableBytes = 0;
    std::vector<uint8_t> buffer;
    std::vector<bool> stops;
    TwoWire() { registers[0] = registers[1] = 255; }
    bool begin(int, int) { ++starts; return true; }
    void end() { ++ends; }
    void setClock(uint32_t) {}
    void setTimeOut(uint16_t) {}
    void beginTransmission(uint8_t) { ++transmissions; buffer.clear(); }
    size_t write(uint8_t value) { return write(&value, 1); }
    size_t write(const uint8_t* bytes, size_t length) {
        const size_t size = std::min<size_t>(writeLimit, length);
        buffer.insert(buffer.end(), bytes, bytes + size); return size;
    }
    uint8_t endTransmission(bool stop = true) {
        stops.push_back(stop);
        if (!ack) return 4;
        if (!buffer.empty()) pointer = buffer[0];
        if (buffer.size() == 2 && pointer < registers.size()) registers[pointer] = buffer[1];
        return 0;
    }
    size_t requestFrom(uint8_t, uint8_t count) {
        ++requests; availableBytes = emptyRead ? 0 : (extraRead ? 2 : 1);
        return shortRead ? 0 : count;
    }
    int available() { return infinite ? 1 : availableBytes; }
    int read() {
        ++reads;
        if (!infinite && availableBytes > 0) --availableBytes;
        if (nextByte != -2) return nextByte;
        return pointer < registers.size() ? registers[pointer] : -1;
    }
};
inline TwoWire Wire;
