#pragma once
#include <Arduino.h>
#include <map>
#include "core/ControlOutput.h"
#include "core/RuntimeResourceSnapshot.h"
#include "core/AsyncExecTypes.h"
inline unsigned long controlNow=0;
inline unsigned long controlMillis() { return controlNow; }
#define millis controlMillis
enum class GPIOState { STATE_LOW, STATE_HIGH, STATE_UNDEFINED };
enum class PeripheralType { GPIO_DIGITAL_OUTPUT, GPIO_PWM_OUTPUT };
enum class PeripheralStatus { PERIPHERAL_INITIALIZED, PERIPHERAL_RUNNING, PERIPHERAL_ERROR };
struct PeripheralConfig {
    PeripheralType type=PeripheralType::GPIO_DIGITAL_OUTPUT;
    bool enabled=true, expander=false;
    uint8_t pinCount=1, pins[1]={4};
    struct { struct { bool passive=false,invertLogic=false; GPIOState initialState=GPIOState::STATE_HIGH; } gpio; } params;
    bool isExpanderChannel() const { return expander; }
};
struct PeripheralRuntimeState {
    PeripheralStatus status=PeripheralStatus::PERIPHERAL_INITIALIZED;
    bool memorySuspended=false,controlOutputClaimed=false,controlOutputFault=false;
    bool controlOutputRetiring=false,controlOutputRetirementFailed=false;
    uint32_t controlOutputRetirementAttempts=0;
    struct { struct { GPIOState currentState=GPIOState::STATE_HIGH; } gpio; } state;
    String lastError;
    unsigned long lastActivity=0;
};
class PeripheralManager {
public:
    static PeripheralManager& getInstance() { static PeripheralManager value; return value; }
    bool controlOutput(const String&,const void*,FastBee::ControlOutputCommand,int&,std::atomic<bool>** = nullptr,
                       const FastBee::ResourceConsumerIdentity* = nullptr);
    bool controlOutputIsClaimed(const String& id="") const;
    bool ordinaryDigitalClaimedLocked(const String&) const { return ordinaryClaimed; }
    bool ordinaryClaimed = false;
    bool controlOutputPinIsClaimed(uint8_t) const;
    void retireControlOutputs();
    void snapshotControlOutputResources(FastBee::ControlOutputResourceSnapshot&) const;
    bool isValidPin(uint8_t pin) const { return pin<=48 && valid; }
    bool isReservedPin(uint8_t) const { return reserved; }
    bool isInputOnlyPin(uint8_t) const { return inputOnly; }
    bool checkPinConflict(uint8_t,const String&) const { return conflicting; }
    bool valid=true,reserved=false,inputOnly=false,conflicting=false;
    void* _mutex=&controlMutex;
    std::map<String,PeripheralConfig> peripherals;
    std::map<String,PeripheralRuntimeState> runtimeStates;
    std::map<String,int> actionTickers;
    struct ControlOutputClaim {
        const void* owner=nullptr;
        char id[41]{};
        FastBee::ResourceConsumerIdentity consumer;
        int pin=-1;
        bool fault=false;
        std::atomic<bool> retirement{false};
        bool retirementAttempted=false;
        uint32_t lastRetirementAttempt=0;
    };
    ControlOutputClaim _controlOutputClaims[4]{};
};
