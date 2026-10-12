#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <freertos/semphr.h>
#include <atomic>
#include <cassert>
#include <cstring>
#include <new>
struct MFRC522 {
    enum StatusCode { STATUS_OK, STATUS_ERROR };
    enum { VersionReg = 1, RxGain_max = 7, PICC_CMD_MF_AUTH_KEY_A = 0x60 };
    struct MIFARE_Key { byte keyByte[6]; };
    struct Uid { byte size = 4, uidByte[10] = {0xa1,0xb2,0xc3,0xd4}, sak = 8; } uid;
    static inline bool failAllocation = false, cardPresent = true, serialGood = true;
    static inline bool shortRead = false, corruptRead = false, failReleaseOnProbe = false;
    static inline byte version = 0x92;
    static inline StatusCode authStatus = STATUS_OK, readStatus = STATUS_OK, writeStatus = STATUS_OK;
    static inline int live = 0, initialized = 0, antennaOff = 0, hardwareCalls = 0;
    static inline int halts = 0, cryptoStops = 0, writes = 0;
    static inline byte payload[16] = {};
    static inline std::atomic<bool> holdRead{false}, readEntered{false};
    static void* operator new(size_t size, const std::nothrow_t&) noexcept {
        return failAllocation ? nullptr : ::operator new(size, std::nothrow);
    }
    static void operator delete(void* ptr) noexcept { ::operator delete(ptr); }
    static void operator delete(void* ptr, const std::nothrow_t&) noexcept { ::operator delete(ptr); }
    MFRC522(uint8_t, uint8_t) { ++live; }
    ~MFRC522() { --live; }
    static void io() { assert(SPI.bus()); ++hardwareCalls; }
    void PCD_Init() { io(); ++initialized; }
    byte PCD_ReadRegister(int) {
        io();
        if (failReleaseOnProbe) spiMutexFailTake = true;
        return version;
    }
    void PCD_SetAntennaGain(int) { io(); }
    void PCD_AntennaOff() { io(); ++antennaOff; }
    bool PICC_IsNewCardPresent() { io(); return cardPresent; }
    bool PICC_ReadCardSerial() { io(); return serialGood; }
    static int PICC_GetType(byte) { return 1; }
    static const char* PICC_GetTypeName(int) { return "MIFARE 1K"; }
    void PICC_HaltA() { io(); ++halts; }
    void PCD_StopCrypto1() { io(); ++cryptoStops; }
    StatusCode PCD_Authenticate(int, byte, MIFARE_Key*, Uid*) { io(); return authStatus; }
    static const char* GetStatusCodeName(StatusCode status) { return status == STATUS_OK ? "OK" : "error"; }
    StatusCode MIFARE_Read(byte, byte* data, byte* length) {
        io(); readEntered = true;
        while (holdRead.load()) std::this_thread::yield();
        std::memcpy(data, payload, 16);
        if (corruptRead) data[0] ^= 0xff;
        *length = shortRead ? 15 : 18;
        return readStatus;
    }
    StatusCode MIFARE_Write(byte, byte* data, byte) {
        io(); ++writes;
        if (writeStatus == STATUS_OK) std::memcpy(payload, data, 16);
        return writeStatus;
    }
};
