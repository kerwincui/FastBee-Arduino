#pragma once
#include <cstdint>
#include <functional>
#include <vector>
struct LcdTransmission { uint8_t address; std::vector<uint8_t> bytes; uint8_t status; };
struct TwoWire {
    unsigned begins = 0, ends = 0;
    size_t failAt = 0, shortAt = 0;
    uint8_t failureStatus = 2, detectedAddress = 0x27;
    bool beginResult = true;
    std::vector<LcdTransmission> transfers;
    std::function<void()> onEnd;
    bool begin(int8_t, int8_t) { ++begins; return beginResult; }
    void end() { ++ends; }
    void setClock(uint32_t) {}
    void setTimeOut(uint16_t) {}
    void beginTransmission(uint8_t address) { transfers.push_back({address, {}, 0}); }
    size_t write(uint8_t value) {
        if (shortAt == transfers.size()) return 0;
        transfers.back().bytes.push_back(value);
        return 1;
    }
    uint8_t endTransmission(bool = true) {
        auto& transfer = transfers.back();
        transfer.status = failAt == transfers.size() ? failureStatus :
            (transfer.address == detectedAddress ? 0 : 2);
        const uint8_t status = transfer.status;
        if (onEnd) onEnd();
        return status;
    }
    void resetTrace() { transfers.clear(); failAt = shortAt = 0; onEnd = nullptr; }
};
inline TwoWire Wire;
