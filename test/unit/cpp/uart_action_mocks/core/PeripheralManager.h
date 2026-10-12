#pragma once
#include <Arduino.h>
#include <cassert>
#include <freertos/semphr.h>
enum class PeripheralType { UART=1, GPIO_DIGITAL_OUTPUT=12 };
struct PeripheralConfig { PeripheralType type=PeripheralType::UART; bool enabled=true; };
struct PeripheralRuntimeState {
    bool memorySuspended=false;
    struct { struct { bool isConnected=true; } comm; } state;
};
class PeripheralManager {
public:
    bool found=true, stateFound=true, writeOk=true;
    uint32_t version=7;
    unsigned writes=0;
    String sent;
    PeripheralConfig config;
    PeripheralRuntimeState runtime;
    static PeripheralManager& getInstance() { static PeripheralManager value; return value; }
    SemaphoreHandle_t getMutex() const { return reinterpret_cast<void*>(1); }
    bool getPeripheralSnapshot(const String& id, PeripheralConfig& out, uint32_t* generation) {
        assert(rtcLockDepth>0); out=config; *generation=version; return found && id=="uart-a";
    }
    bool getRuntimeStateSnapshot(const String&, PeripheralRuntimeState& out) {
        assert(rtcLockDepth>0); out=runtime; return stateFound;
    }
    bool writeString(const String& id, const String& value) {
        assert(rtcLockDepth>0 && id=="uart-a"); ++writes; sent=value; return writeOk;
    }
};
