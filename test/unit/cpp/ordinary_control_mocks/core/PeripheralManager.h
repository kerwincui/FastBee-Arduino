#pragma once
#define millis unused_ordinary_millis
#include <Arduino.h>
#undef millis
#include <cassert>
#include <map>
#include <memory>
#include <cstring>
#include "core/NativeOutputControl.h"
#include "core/CheckedActionTimer.h"
#include "core/SensorConfigEpoch.h"
#include "core/ManagedPinIo.h"
uint32_t mapAnalogOutputPercent(uint32_t, uint8_t, uint8_t, uint8_t);
inline unsigned scalarPwmWrites = 0, scalarDacWrites = 0;
inline uint32_t scalarLastDuty = 0;
inline uint8_t scalarLastDac = 0;
inline bool scalarPwmOk = true, scalarDacOk = true, scalarDacRetainedFault = false;
inline unsigned long millis() { return static_cast<unsigned long>(ordinaryClockUs / 1000); }
inline bool ordinaryMutexBusy = false;
inline unsigned ordinaryLockDepth = 0, nativeWrites = 0, nativeReads = 0;
inline bool nativeWriteOk = true, nativeReadOk = true;
inline bool nativeLevel = false;
enum class GPIOState { STATE_UNDEFINED, STATE_LOW, STATE_HIGH };
enum class PeripheralType { GPIO_DIGITAL_OUTPUT, GPIO_PWM_OUTPUT, GPIO_ANALOG_OUTPUT, IO_EXPANDER, GPIO_DIGITAL_INPUT, NEO_PIXEL, DAC, PCA9685_PWM, PWM_SERVO, STEPPER_MOTOR, RF_MODULE, LCD, SEVEN_SEGMENT_TM1637 };
constexpr uint8_t RF_MODE_TX = 0;
enum class PeripheralStatus { PERIPHERAL_INITIALIZED, PERIPHERAL_RUNNING, PERIPHERAL_ERROR };
struct PeripheralConfig {
    String id = "led";
    PeripheralType type = PeripheralType::GPIO_DIGITAL_OUTPUT;
    bool enabled = true, expander = false, modbus = false;
    uint8_t pinCount = 1;
    struct { struct {
        bool passive = false;
        uint8_t pwmResolution = 8, pwmChannel = 0, aoMinPct = 0, aoMaxPct = 100;
        uint32_t pwmFrequency = 1000;
        bool aoMode = false;
        bool invertLogic = false; char providerId[41] = {}; uint8_t channel = 0;
    } gpio;
    struct { uint8_t pwmResolution = 16; } servo;
    struct { uint8_t interface = 2; } lcd;
    struct { uint8_t mode = RF_MODE_TX; } rf; } params;
    bool isExpanderChannel() const { return expander; }
    bool isModbusPeripheral() const { return modbus; }
    bool isGPIOPeripheral() const { return true; }
    uint8_t getPrimaryPin() const { return type == PeripheralType::DAC ? 25 : 5; }
};
struct PeripheralRuntimeState {
    FastBee::OrdinaryControl::Lease ordinaryControl;
    uint32_t ordinaryStopFailures = 0;
    uint64_t ordinaryRetryAfterMs = 0;
    bool memorySuspended = false, memoryCleanupPending = false;
    PeripheralStatus status = PeripheralStatus::PERIPHERAL_RUNNING;
    String lastError;
    unsigned long lastActivity = 0;
    struct { struct { GPIOState currentState = GPIOState::STATE_LOW; } gpio; } state;
};
struct Ticker {};
// The checked native GPIO backend already has a dedicated SDK/ledger harness.
// Here it is a counted result seam; arbitration and the PM write guard are real.
struct CountedNativeIo {
    void snapshot(FastBee::DacResourceSnapshot& result) const {
        result = FastBee::DacResourceSnapshot{};
        result.supported = true;
        if (scalarDacRetainedFault) {
            result.count = 1;
            result.leases[0].pin = 25;
            result.leases[0].fault = true;
        }
    }
    bool write(const char*, bool, uint8_t, bool high) {
        assert(ordinaryLockDepth); ++nativeWrites;
        if (nativeWriteOk) nativeLevel = high;
        return nativeWriteOk;
    }
};
inline bool ledcWrite(uint8_t pin, uint32_t duty) {
    assert(ordinaryLockDepth && pin == 5); ++scalarPwmWrites;
    if (scalarPwmOk) scalarLastDuty = duty;
    return scalarPwmOk;
}
class PeripheralManager {
public:
    // @ordinary_context@
    static PeripheralManager& getInstance() { static PeripheralManager manager; return manager; }
    bool beginOrdinaryControl(FastBee::OrdinaryControl::Source, uint64_t, uint32_t, bool, uint64_t,
        const String&, uint32_t, const FastBee::UartEventOrigin&, FastBee::OrdinaryControlContext&);
    bool submitOrdinaryDigital(const String&, FastBee::DigitalOutputCommand, uint32_t,
        const FastBee::OrdinaryControlContext&, FastBee::OrdinaryControlResult&);
    bool submitOrdinaryScalar(const String&, FastBee::ScalarOutputCommand, uint32_t,
        const FastBee::OrdinaryControlContext&, FastBee::OrdinaryControlResult&);
    bool writeOrdinaryScalarLocked(const String&, FastBee::ScalarOutputCommand, uint32_t,
        const FastBee::OrdinaryControl::Permit&);
    bool writeManagedDac(const String& id, uint8_t pin, uint8_t value) {
        assert(ordinaryLockDepth && id == "led" && pin == 25);
        ++scalarDacWrites;
        if (!scalarDacOk) scalarDacRetainedFault = true;
        if (scalarDacRetainedFault) return false;
        scalarLastDac = value; return true;
    }
    void finishOrdinaryControl(const FastBee::OrdinaryControlContext&, bool = false);
    void finishOrdinaryModbusLocked(const FastBee::OrdinaryControlContext&, bool) {} // Separate actual PM/bus fixture.
    void serviceOrdinaryModbusLocked() {} // Separate actual PM/bus fixture.
    bool getOrdinaryControlSnapshot(const String&, FastBee::OrdinaryControlResult&) const;
    void serviceOrdinaryControlTicker(uint32_t);
    void serviceOrdinaryControlExpiry();
    bool ordinaryDigitalClaimedLocked(const String&) const;
    bool ordinaryPermitCurrentLocked(const String&, const FastBee::OrdinaryControl::Permit&) const;
    bool ordinaryBackendSupportedLocked(const PeripheralConfig& config) const;
    bool ordinaryProducerPresentLocked(const String& id) const;
    bool ordinaryBackendQuiescentLocked(const String& id) const;
    bool retireOrdinaryRfLocked(const String&, bool*) { return true; } // Actual RF SDK fixture is separate.
    bool ordinaryRfQuiescentLocked(const String&) const { return false; }
    bool ordinaryDisplayQuiescentLocked(const String&) const { return false; } // Separate real display fixture.
    bool ordinaryMotionProducerPresentLocked(const String&) const { return false; }
    bool ordinaryMotionQuiescentLocked(const String&) const { return false; }
    bool retireOrdinaryMotionLocked(const String&, bool, bool*) { return true; } // Real motion has its own fixture.
    bool submitOrdinaryServo(const String&, int64_t, bool, const FastBee::OrdinaryControlContext*,
        FastBee::OrdinaryControlResult*) { assert(false); return false; }
    bool ordinaryNeoPixelQuiescentLocked(const String&) const { return false; } // Separate real NeoPixel fixture.
    bool ordinaryPca9685QuiescentLocked(const String&) const { return false; } // Separate real PCA fixture.
    bool inspectOrdinaryControlLocked(const String& id, const FastBee::OrdinaryControlContext& context,
        FastBee::OrdinaryControl::Intent intent, FastBee::OrdinaryControlResult& result, uint64_t& now);
    bool commitOrdinaryControlLocked(const String& id, const FastBee::OrdinaryControlContext& context,
        FastBee::OrdinaryControl::Intent intent, FastBee::OrdinaryControlResult& result, uint64_t& now,
        bool keepTickerSlot, FastBee::OrdinaryControl::Permit& permit);
    void updateOrdinaryControlResultLocked(const String& id, uint64_t now,
        FastBee::OrdinaryControlResult& result) const;
    bool completeOrdinaryControlLocked(const String& id, uint64_t now,
        FastBee::OrdinaryControlResult& result, bool accepted,
        const char* failureReason = "CONTROL_OUTPUT_UNCONFIRMED");
    bool retireOrdinaryDigitalLocked(const String&, uint64_t, bool = false, bool = false, bool* = nullptr);
    bool writeGpioStateLocked(const PeripheralConfig&, const String*, const char*, GPIOState,
        const FastBee::OrdinaryControl::Permit* = nullptr);
    GPIOState readGpioStateLocked(const PeripheralConfig&, const String*, const char*);
    // IOX bridge: callers hold the PM mutex across allocation, retirement and I2C.
    bool ordinaryIoProducerPresentLocked(const String& id) const;
    bool prepareOrdinaryIoProducerLocked(const String& id, bool& inserted);
    void discardOrdinaryIoReservationLocked(const String& id, bool inserted);
    bool retireOrdinaryIoProducerLocked(const String& id, bool keepSlot = false);
    bool startOrdinaryIoProducerLocked(const String& id, uint16_t interval,
        const FastBee::OrdinaryControlContext& context,
        const FastBee::OrdinaryControl::Permit& permit);
    bool retireOrdinaryIoGroupLocked(const String& providerId);
    void serviceOrdinaryIoProducerLocked(const String& id);

    bool controlOutputIsClaimed(const String&) const { return dedicated; }
    bool isUartEventOriginCurrent(const FastBee::UartEventOrigin& origin) const {
        return origin.unbound() || (uartCurrent && origin.matches(7, 19));
    }
    bool hasPeripheral(const String& id) const { return peripherals.count(id) != 0; }
    bool getPeripheralSnapshot(const String& id, PeripheralConfig& result, uint32_t* epoch = nullptr) const {
        auto found = peripherals.find(id); if (found == peripherals.end()) return false;
        result = found->second; if (epoch) *epoch = FastBee::SensorConfigEpoch::current(); return true;
    }
    PeripheralRuntimeState* getRuntimeStateCStrLocked(const char* id) {
        auto found = runtimeStates.find(String(id)); return found == runtimeStates.end() ? nullptr : &found->second;
    }
    bool writePin(const String& id, GPIOState value);
    bool writePWM(const String&, uint32_t) { assert(false); return false; }
    void stopActionTicker(const String&) { assert(false); }
    int _mutex = 1;
    bool dedicated = false, uartCurrent = true;
    uint64_t _nextOrdinaryControlOperation = 0, _nextOrdinaryControlGeneration = 0;
    uint32_t _nextActionTickerToken = 0;
    CountedNativeIo _managedPinIo;
    FastBee::LedcResourceSnapshot::Lease _ledcResourceLeases[FastBee::LedcResourceSnapshot::CAPACITY]{};
    std::map<String, PeripheralConfig> peripherals;
    std::map<String, PeripheralRuntimeState> runtimeStates;
    std::map<String, ActionTickerData*> actionTickers;
};
