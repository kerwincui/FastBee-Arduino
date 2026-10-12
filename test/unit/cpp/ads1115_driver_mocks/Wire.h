#pragma once
#include <Arduino.h>
#include <algorithm>
#include <vector>
class TwoWire {
public:
    uint16_t config = 0x8583, sample = 0x2000;
    bool muxSamplesEnabled = false;
    uint16_t muxSamples[8] = {0x100,0x200,0x300,0x400,0x1000,0x2000,0x3000,0x4000};
    std::vector<unsigned> startedMuxes;
    unsigned polls = 0, starts = 0, transmissions = 0, requests = 0, idleWrites = 0;
    unsigned failTxAt = 0, writeLimit = 255, busyPolls = 2;
    bool ack = true, shortRead = false, negativeRead = false, stuck = false;
    bool missedBusy = false, configLost = false;
    uint8_t selected = 1;
    size_t offset = 0;
    std::vector<uint8_t> output, input;
    std::vector<bool> stops;
    bool begin(int, int) { return true; }
    void end() {}
    void setClock(uint32_t) {}
    void setTimeOut(uint16_t) {}
    void beginTransmission(uint8_t) { ++transmissions; output.clear(); }
    size_t write(uint8_t value) { return write(&value, 1); }
    size_t write(const uint8_t* bytes, size_t size) {
        const size_t count = std::min<size_t>(size, writeLimit);
        output.insert(output.end(), bytes, bytes + count); return count;
    }
    uint8_t endTransmission(bool stop = true) {
        stops.push_back(stop);
        if (!ack || (failTxAt && transmissions == failTxAt)) return 4;
        if (output.empty()) return 0;
        selected = output[0];
        if (selected == 1 && output.size() == 3) {
            config = static_cast<uint16_t>((output[1] << 8) | output[2]);
            polls = 0;
            if (config & 0x8000) { ++starts; startedMuxes.push_back((config >> 12) & 7); config &= 0x7fff; }
            else ++idleWrites;
        }
        return 0;
    }
    size_t requestFrom(uint8_t, uint8_t length) {
        ++requests; offset = 0;
        uint16_t value = muxSamplesEnabled ? muxSamples[(config >> 12) & 7] : sample;
        if (selected == 1) {
            if (!(config & 0x8000) && (config & 0x0100) && !stuck &&
                (missedBusy || polls++ >= busyPolls)) config |= 0x8000;
            value = config;
            if (configLost && starts) value ^= 0x0200;
        }
        input = {static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
        return shortRead ? 1 : length;
    }
    int available() const { return static_cast<int>(input.size() - offset); }
    int read() {
        if (negativeRead) return -1;
        return offset < input.size() ? input[offset++] : -1;
    }
};
inline TwoWire Wire;
