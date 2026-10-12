#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/ChipConfig.h"
#include "core/CheckedAdc.h"
#if FASTBEE_ENABLE_NEOPIXEL
#include "peripherals/NeoPixelTransport.h"
#endif

bool PeripheralManager::managedPinConflict(const PeripheralConfig& config) const {
    const bool digital = isGpioDigitalInputType(config.type) ||
        config.type == PeripheralType::RADAR_SENSOR ||
        (config.type == PeripheralType::RF_MODULE && config.params.rf.mode == RF_MODE_TX) ||
        config.type == PeripheralType::ENCODER || config.type == PeripheralType::STEPPER_MOTOR ||
        (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT && !config.params.gpio.passive);
    for (uint8_t i = 0; i < config.configuredPinCount(); ++i) {
        const uint8_t pin = config.configuredPinAt(i);
        if (pin == 255) continue;
        if (!isAnalogInputType(config.type) && FastBee::adcPinUnavailable(pin)) return true;
        if (_managedPinIo.conflicts(config.id.c_str(), false, pin) ||
            (_managedPinIo.hasGpio(pin) && !digital) ||
            (_managedPinIo.hasDac(pin) && config.type != PeripheralType::DAC)) return true;
    }
    return false;
}
bool PeripheralManager::managedPinAvailableForAdc(uint8_t pin) const {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || !isValidPin(pin) || isReservedPin(pin) ||
        _managedPinIo.hasGpio(pin) || _managedPinIo.hasDac(pin) ||
        controlOutputPinIsClaimed(pin)) return false;
#if FASTBEE_ENABLE_NEOPIXEL
    if (fastbeeNeoPixelProjectPinRetained(pin)) return false;
#endif
    for (const auto& lease : _ledcResourceLeases) if (lease.pin != 255 && lease.pin == pin) return false;
    return true;
}
bool PeripheralManager::setupManagedDigital(const PeripheralConfig& config) {
    RecursiveMutexGuard lock(_mutex, 0);
    const uint8_t pin = config.getPrimaryPin();
    if (!lock.isLocked() || !isValidPin(pin) || isReservedPin(pin) || controlOutputPinIsClaimed(pin) ||
        checkPinConflict(pin, config.id) || FastBee::adcPinUnavailable(pin)) return false;
    FastBee::ManagedGpioMode mode = FastBee::ManagedGpioMode::Input;
    if (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT) {
        if (config.params.gpio.passive || isInputOnlyPin(pin)) return false;
        mode = FastBee::ManagedGpioMode::Output;
    } else if (isGpioDigitalInputType(config.type)) {
        uint8_t input = config.params.gpio.inputMode;
        if (input == static_cast<uint8_t>(DigitalInputMode::AUTO)) {
            input = config.type == PeripheralType::GPIO_DIGITAL_INPUT_PULLUP
                ? static_cast<uint8_t>(DigitalInputMode::PULL_UP)
                : config.type == PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN
                    ? static_cast<uint8_t>(DigitalInputMode::PULL_DOWN)
                    : static_cast<uint8_t>(DigitalInputMode::FLOATING);
        }
        if (input == static_cast<uint8_t>(DigitalInputMode::PULL_UP)) mode = FastBee::ManagedGpioMode::PullUp;
        else if (input == static_cast<uint8_t>(DigitalInputMode::PULL_DOWN)) mode = FastBee::ManagedGpioMode::PullDown;
        else if (input != static_cast<uint8_t>(DigitalInputMode::FLOATING)) return false;
    } else return false;
    const bool ok = _managedPinIo.configure(config.id.c_str(), false, pin, mode,
                                           config.params.gpio.initialState == GPIOState::STATE_HIGH);
    auto state = runtimeStates.find(config.id);
    if (state != runtimeStates.end()) {
        state->second.lastError = ok ? "" : "GPIO_SETUP_UNCONFIRMED";
        state->second.state.gpio.currentState = ok && mode == FastBee::ManagedGpioMode::Output
            ? config.params.gpio.initialState : GPIOState::STATE_UNDEFINED;
    }
    return ok;
}
bool PeripheralManager::releaseManagedDigital(const PeripheralConfig& config) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || controlOutputIsClaimed(config.id)) return false;
    const bool ok = _managedPinIo.release(config.id.c_str(), false, config.getPrimaryPin());
    auto state = runtimeStates.find(config.id);
    if (state != runtimeStates.end()) {
        state->second.lastError = ok ? "" : "GPIO_RELEASE_UNCONFIRMED";
        if (ok) state->second.state.gpio.interruptAttached = false;
    }
    return ok;
}
bool PeripheralManager::writeManagedDac(const String& id, uint8_t pin, uint8_t value) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || !isValidPin(pin) || isReservedPin(pin) || isInputOnlyPin(pin) ||
        controlOutputPinIsClaimed(pin) || checkPinConflict(pin, id) || FastBee::adcPinUnavailable(pin)) return false;
    const bool ok = _managedPinIo.writeDac(id.c_str(), false, pin, value);
    auto state = runtimeStates.find(id);
    if (state != runtimeStates.end()) state->second.lastError = ok ? "" : "DAC_WRITE_UNCONFIRMED";
    return ok;
}
bool PeripheralManager::releaseManagedDac(const String& id, uint8_t pin) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || controlOutputPinIsClaimed(pin)) return false;
    const bool ok = _managedPinIo.releaseDac(id.c_str(), false, pin);
    auto state = runtimeStates.find(id);
    if (state != runtimeStates.end()) state->second.lastError = ok ? "" : "DAC_RELEASE_UNCONFIRMED";
    return ok;
}
bool PeripheralManager::prepareScriptPin(uint8_t pin, uint8_t kind) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || kind > 2 || !isValidPin(pin) || isReservedPin(pin) || isInputOnlyPin(pin) ||
        controlOutputPinIsClaimed(pin) || checkPinConflict(pin) || _managedPinIo.conflicts("", true, pin) ||
        FastBee::adcPinUnavailable(pin)) return false;
#if !CHIP_HAS_DAC
    if (kind == 2) return false;
#else
    if (kind == 2 && pin != 25 && pin != 26) return false;
#endif
    // No implicit SDK detach: a failure retains its old record and prevents
    // the next operation. The caller rechecks UART origin after this stage.
    if (kind != 1 && !releaseScriptPwm(pin)) return false;
    if (kind != 0 && !_managedPinIo.release("", true, pin)) return false;
    if (kind != 2 && !_managedPinIo.releaseDac("", true, pin)) return false;
    return true;
}
bool PeripheralManager::writeScriptGpio(uint8_t pin, bool high) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || !scriptPinReady(pin, 0)) return false;
    return _managedPinIo.configure("", true, pin, FastBee::ManagedGpioMode::Output, high);
}
bool PeripheralManager::writeScriptDac(uint8_t pin, uint8_t value) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || !scriptPinReady(pin, 2)) return false;
    return _managedPinIo.writeDac("", true, pin, value);
}
bool PeripheralManager::scriptPinReady(uint8_t pin, uint8_t kind) const {
    // No cleanup here: source provenance was rechecked after prepareScriptPin.
    // Direct callers must prepare a function switch explicitly too.
    if (kind > 2 || !isValidPin(pin) || isReservedPin(pin) || isInputOnlyPin(pin) ||
        controlOutputPinIsClaimed(pin) || checkPinConflict(pin) || _managedPinIo.conflicts("", true, pin) ||
        FastBee::adcPinUnavailable(pin)) return false;
    if ((kind != 0 && _managedPinIo.hasGpio(pin)) || (kind != 2 && _managedPinIo.hasDac(pin))) return false;
    for (const auto& lease : _ledcResourceLeases)
        if (lease.pin == pin && (kind != 1 || !lease.commandScript || lease.cleanupFailed)) return false;
    return true;
}
void PeripheralManager::snapshotGpioResources(FastBee::GpioResourceSnapshot& result) const {
    result = FastBee::GpioResourceSnapshot{};
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) { result.status = FastBee::ResourceObservation::Busy; return; }
    _managedPinIo.snapshot(result);
}
void PeripheralManager::snapshotDacResources(FastBee::DacResourceSnapshot& result) const {
    result = FastBee::DacResourceSnapshot{};
    result.supported = CHIP_HAS_DAC != 0;
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) { result.status = FastBee::ResourceObservation::Busy; return; }
    _managedPinIo.snapshot(result);
}
