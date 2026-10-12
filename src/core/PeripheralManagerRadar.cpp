#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/ChipConfig.h"
#include "core/CheckedAdc.h"

bool PeripheralManager::setupRadarHardware(const PeripheralConfig& config) {
    RecursiveMutexGuard lock(_mutex, 0);
    const uint8_t pin = config.getPrimaryPin();
    if (!lock.isLocked() || config.type != PeripheralType::RADAR_SENSOR || !config.enabled ||
        config.pinCount < 1 || config.params.radar.mode != 0 ||
        config.params.radar.debounceMs > 5000 || config.params.radar.holdMs > 60000 ||
        !isValidPin(pin) || isReservedPin(pin) || controlOutputPinIsClaimed(pin) ||
        checkPinConflict(pin, config.id) || FastBee::adcPinUnavailable(pin)) return false;
    auto runtime = runtimeStates.find(config.id);
    if (runtime == runtimeStates.end() || runtime->second.memoryCleanupPending) return false;
    auto& state = runtime->second.state.radar;
    FastBee::GpioResourceSnapshot::Lease owner;
    // Runtime snapshots can be restored by the existing mutation rollback.
    // Only an actual lease can make their filter state current again.
    const bool retained = state.generation != 0 && _managedPinIo.observeGpio(state.pin, owner) &&
        !_managedPinIo.conflicts(config.id.c_str(), false, state.pin);
    if (retained) {
        if (!state.initialized || owner.fault || owner.cleanupPending || owner.interruptAttached ||
            owner.interruptCleanupPending || owner.mode != FastBee::ManagedGpioMode::Input ||
            state.pin != pin || state.activeHigh != config.params.radar.activeHigh ||
            state.debounceMs != config.params.radar.debounceMs || state.holdMs != config.params.radar.holdMs)
            return false;
    }
    // Never adopt another function's live pad, even if it has the same ID.
    if ((!retained && _managedPinIo.hasGpio(pin)) || _managedPinIo.hasDac(pin) || _nextRadarGeneration == UINT32_MAX)
        return false;
    // Every initialization starts a fresh logical session, even when the
    // checked pad configure is idempotent. A restored runtime snapshot cannot
    // resurrect a prior session's debounced/held value over a live same-ID pad.
    state = FastBee::RadarInputState{};
    state.generation = ++_nextRadarGeneration;
    state.pin = pin;
    state.activeHigh = config.params.radar.activeHigh;
    state.debounceMs = config.params.radar.debounceMs;
    state.holdMs = config.params.radar.holdMs;
    state.initialized = _managedPinIo.configure(config.id.c_str(), false, pin, FastBee::ManagedGpioMode::Input, false);
    runtime->second.lastError = state.initialized ? "" : "GPIO_SETUP_UNCONFIRMED";
    return state.initialized;
}

bool PeripheralManager::teardownRadarHardware(const PeripheralConfig& config) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || controlOutputIsClaimed(config.id)) return false;
    auto runtime = runtimeStates.find(config.id);
    uint8_t pin = config.getPrimaryPin();
    if (runtime != runtimeStates.end()) {
        auto& state = runtime->second.state.radar;
        if (state.generation) pin = state.pin; // Release the captured pad, not edited config.
        state.initialized = false; // Retire before any SDK operation, including failure.
        state.candidateKnown = state.stableKnown = state.holdPending = state.present = false;
    }
    const bool released = _managedPinIo.release(config.id.c_str(), false, pin);
    if (runtime != runtimeStates.end()) {
        runtime->second.lastError = released ? "" : "GPIO_RELEASE_UNCONFIRMED";
        if (released) runtime->second.state.radar = FastBee::RadarInputState{};
    }
    return released;
}

bool PeripheralManager::readRadarState(const String& id, bool& detected) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) return false;
    const auto config = getPeripheral(id);
    const auto runtime = runtimeStates.find(id);
    if (!config || !config->enabled || config->type != PeripheralType::RADAR_SENSOR ||
        config->pinCount < 1 || runtime == runtimeStates.end() || runtime->second.memorySuspended ||
        runtime->second.memoryCleanupPending ||
        (runtime->second.status != PeripheralStatus::PERIPHERAL_INITIALIZED &&
         runtime->second.status != PeripheralStatus::PERIPHERAL_RUNNING)) return false;
    auto& state = runtime->second.state.radar;
    if (!state.initialized || !state.generation || state.pin != config->getPrimaryPin() ||
        config->params.radar.mode != 0 || state.activeHigh != config->params.radar.activeHigh ||
        state.debounceMs != config->params.radar.debounceMs || state.holdMs != config->params.radar.holdMs)
        return false;
    bool physicalHigh = false;
    if (!_managedPinIo.read(id.c_str(), state.pin, physicalHigh)) {
        state.initialized = false;
        state.candidateKnown = state.stableKnown = state.holdPending = state.present = false;
        runtime->second.lastError = "GPIO_READ_UNCONFIRMED";
        return false;
    }
    const uint32_t now = millis();
    if (!FastBee::sampleRadarInput(state, state.activeHigh ? physicalHigh : !physicalHigh, now, detected))
        return false; // Existing API/Observation failure path preserves invalidity, never a new zero.
    runtime->second.lastActivity = now;
    return true;
}
