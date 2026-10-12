#pragma once
#include <FS.h>
#include <SPI.h>
#include <cassert>
constexpr int CARD_NONE = 0, CARD_MMC = 1, CARD_SD = 2, CARD_SDHC = 3;
constexpr int OUTPUT = 1;
inline int sdPinModes = 0, sdPinWrites = 0;
inline void pinMode(uint8_t, int) { ++sdPinModes; }
inline void digitalWrite(uint8_t, int) { ++sdPinWrites; }
class MockSD {
public:
    bool begin(uint8_t, SPIClass&, uint32_t, const char*, int, bool) {
        ++begins; assert(SPI.active); mounted = mountOk; return mountOk;
    }
    void end() { ++ends; mounted = false; }
    int cardType() { assert(SPI.active); return card; }
    bool exists(const String&) { assert(SPI.active); return directoryExists; }
    bool mkdir(const String&) { assert(SPI.active); return mkdirOk; }
    bool rmdir(const String&) { ++deletes; assert(SPI.active); return deleteOk; }
    bool remove(const String&) { ++deletes; assert(SPI.active); return deleteOk; }
    uint64_t cardSize() { ++capacityReads; assert(SPI.active); return 8192; }
    uint64_t totalBytes() { assert(SPI.active); return total; }
    uint64_t usedBytes() { assert(SPI.active); return used; }
    File open(const String& path, const char*, bool = false) {
        ++opens; assert(SPI.active && mounted); return File(openOk, path == "/fastbee");
    }
    bool mountOk = true, mounted = false, directoryExists = true, mkdirOk = true;
    bool openOk = true, deleteOk = true;
    int deletes = 0;
    int card = CARD_SDHC, begins = 0, ends = 0, opens = 0, capacityReads = 0;
    uint64_t total = 4096, used = 128;
};
inline MockSD SD;
