#pragma once
#include <Arduino.h>
#include <freertos/semphr.h>
#include "core/PeripheralConfig.h"
#include <map>
#include <cassert>
class PeripheralManager {
public:
    SemaphoreHandle_t _mutex = reinterpret_cast<void*>(1);
    std::map<String, PeripheralConfig> peripherals;
    PeripheralRuntimeState state;
    bool stateFound = true;
    static PeripheralManager& getInstance() { static PeripheralManager manager; return manager; }
    SemaphoreHandle_t getMutex() const { return _mutex; }
    bool getPeripheralSnapshot(const String& id, PeripheralConfig& out) {
        assert(rtcLockDepth > 0);
        const auto found = peripherals.find(id);
        if (found == peripherals.end()) return false;
        out = found->second; return true;
    }
    bool getRuntimeStateSnapshot(const String&, PeripheralRuntimeState& out) {
        assert(rtcLockDepth > 0); out = state; return stateFound;
    }
    void seedSystemClockFromRtcAtBoot();
};
