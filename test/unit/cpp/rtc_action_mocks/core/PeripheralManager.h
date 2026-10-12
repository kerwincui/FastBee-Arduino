#pragma once
#include <Arduino.h>
#include <freertos/semphr.h>
#include <cassert>
enum class PeripheralType { GPIO_DIGITAL_OUTPUT=12, DS1302_RTC=50 };
enum class PeripheralStatus { PERIPHERAL_DISABLED, PERIPHERAL_ENABLED,
    PERIPHERAL_INITIALIZED, PERIPHERAL_RUNNING, PERIPHERAL_ERROR };
struct PeripheralConfig {
    PeripheralType type=PeripheralType::DS1302_RTC;
    bool enabled=true;
    uint8_t pinCount=3;
    uint8_t pins[3]={5,6,7};
};
struct PeripheralRuntimeState {
    bool memorySuspended=false;
    PeripheralStatus status=PeripheralStatus::PERIPHERAL_INITIALIZED;
};
class PeripheralManager {
public:
    bool found=true, stateFound=true, hasMutex=true;
    PeripheralConfig config;
    PeripheralRuntimeState state;
    static PeripheralManager& getInstance() { static PeripheralManager p; return p; }
    SemaphoreHandle_t getMutex() const { return hasMutex ? reinterpret_cast<void*>(1) : nullptr; }
    bool getPeripheralSnapshot(const String& id, PeripheralConfig& out) {
        assert(rtcLockDepth>0); out=config; return found && id=="rtc_01";
    }
    bool getRuntimeStateSnapshot(const String&, PeripheralRuntimeState& out) {
        assert(rtcLockDepth>0); out=state; return stateFound;
    }
};
