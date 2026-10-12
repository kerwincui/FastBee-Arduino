#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
constexpr uint8_t MSBFIRST = 1, LSBFIRST = 0;
constexpr uint8_t SPI_MODE0 = 0;
struct SPISettings {
    uint32_t frequency;
    uint8_t bitOrder, mode;
    SPISettings(uint32_t hz, uint8_t order, uint8_t value)
        : frequency(hz), bitOrder(order), mode(value) {}
};
inline std::vector<std::string> spiIoEvents;
constexpr int8_t SCK = 18;
constexpr int8_t MISO = 19;
constexpr int8_t MOSI = 23;
constexpr uint8_t HSPI = 2;
inline bool spiFailNextBegin = false;
inline int spiObjectConstructs = 0, spiObjectDestroys = 0;
class SPIClass {
public:
    explicit SPIClass(uint8_t value = 0) : host(value) { ++spiObjectConstructs; }
    ~SPIClass() { ++spiObjectDestroys; }
    uint8_t host;
    void* bus() { ++busReads; return active ? this : nullptr; }
    bool begin(int8_t sck, int8_t miso, int8_t mosi, int8_t cs) {
        ++begins;
        if (spiFailNextBegin) { spiFailNextBegin = false; active = true; return false; }
        if (active) return true; // Arduino's hazardous early-success behavior.
        lastSck = sck; lastMiso = miso; lastMosi = mosi; lastCs = cs;
        active = !failBeforeAllocation;
        return !failBeforeAllocation && !failAfterAllocation;
    }
    void end() { ++ends; active = false; }
    void beginTransaction(const SPISettings& settings) {
        ++transactions; lastSettings = settings; spiIoEvents.push_back("begin");
    }
    uint8_t transfer(uint8_t value) {
        ++transfers; sent.push_back(value); spiIoEvents.push_back("transfer");
        if (onTransfer) onTransfer();
        return static_cast<uint8_t>(value ^ 0x5a);
    }
    void endTransaction() { ++transactionEnds; spiIoEvents.push_back("end"); }
    void write(uint8_t value) { (void)transfer(value); }
    void writeBytes(uint8_t* bytes, size_t length) {
        for (size_t i = 0; i < length; ++i) (void)transfer(bytes[i]);
    }
    int transactions = 0, transfers = 0, transactionEnds = 0;
    int busReads = 0;
    SPISettings lastSettings{0, 0, 0};
    std::vector<uint8_t> sent;
    std::function<void()> onTransfer;
    bool active = false, failBeforeAllocation = false, failAfterAllocation = false;
    int begins = 0, ends = 0;
    int8_t lastSck = -1, lastMiso = -1, lastMosi = -1, lastCs = -1;
};
inline SPIClass SPI;
