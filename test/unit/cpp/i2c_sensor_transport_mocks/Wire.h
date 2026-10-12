#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <algorithm>
struct TwoWire {
    unsigned begins = 0, ends = 0, transmissions = 0, requests = 0, reads = 0;
    uint8_t txStatus = 0, address = 0, requestedLength = 0;
    size_t writeLimit = 65535, returnedCount = 2, offset = 0;
    int failReadAt = -1;
    bool stuckAvailable = false;
    std::vector<uint8_t> written;
    std::vector<bool> stops;
    std::vector<uint8_t> input{0x12, 0x34};
    bool begin(int8_t, int8_t) { ++begins; return true; }
    void end() { ++ends; }
    void setClock(uint32_t) {}
    void setTimeOut(uint16_t) {}
    void beginTransmission(uint8_t value) { ++transmissions; address = value; written.clear(); }
    size_t write(uint8_t value) { return write(&value, 1); }
    size_t write(const uint8_t* data, size_t length) {
        const size_t count = std::min(writeLimit, length);
        if (count) written.insert(written.end(), data, data + count);
        return count;
    }
    uint8_t endTransmission(bool stop = true) { stops.push_back(stop); return txStatus; }
    size_t requestFrom(uint8_t value, uint8_t length) {
        ++requests; address = value; requestedLength = length; offset = 0;
        return returnedCount;
    }
    int available() const {
        return stuckAvailable ? 1 : static_cast<int>(input.size() - offset);
    }
    int read() {
        ++reads;
        if (static_cast<int>(offset) == failReadAt) return -1;
        if (stuckAvailable) return 0;
        return offset < input.size() ? input[offset++] : -1;
    }
};
inline TwoWire Wire;
