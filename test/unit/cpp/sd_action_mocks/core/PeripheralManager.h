#pragma once
#include <Arduino.h>
#include <cassert>
#include <freertos/semphr.h>
enum class PeripheralType { GPIO_DIGITAL_OUTPUT=12, SDIO=37 };
struct PeripheralConfig {
    PeripheralType type=PeripheralType::SDIO;
    bool enabled=true;
    uint8_t pinCount=4, pins[4]={5,6,7,8};
    struct { struct { uint32_t frequency=1000000; } sdcard; } params;
};
struct PeripheralRuntimeState { bool memorySuspended=false; };
class PeripheralManager {
public:
    bool found=true, stateFound=true, hasMutex=true;
    uint32_t version=7;
    PeripheralConfig config;
    PeripheralRuntimeState state;
    static PeripheralManager& getInstance() { static PeripheralManager value; return value; }
    SemaphoreHandle_t getMutex() const { return hasMutex ? reinterpret_cast<void*>(1) : nullptr; }
    bool getPeripheralSnapshot(const String& id, PeripheralConfig& out, uint32_t* capturedVersion=nullptr) {
        assert(rtcLockDepth>0); out=config;if(capturedVersion)*capturedVersion=version;
        return found && id=="sd_01";
    }
    bool getRuntimeStateSnapshot(const String&, PeripheralRuntimeState& out) {
        assert(rtcLockDepth>0); out=state; return stateFound;
    }
};
