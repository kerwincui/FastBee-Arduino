#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/CheckedAdc.h"
#include <driver/gpio.h>
#include <cstring>

bool PeripheralManager::encoderSlotReady(const EncoderIsrSlot* slot) const {
    if (!slot || !slot->used || !slot->active || slot->fault || slot->cleanupPending ||
        !_managedPinIo.interruptCurrent(slot->id.c_str(), slot->pinA)) return false;
    FastBee::GpioResourceSnapshot::Lease phaseB;
    return _managedPinIo.observeGpio(slot->pinB, phaseB) && !phaseB.commandScript &&
        phaseB.consumer.role == FastBee::ResourceConsumerRole::Peripheral &&
        slot->id == phaseB.consumer.id && phaseB.mode == FastBee::ManagedGpioMode::PullUp &&
        !phaseB.fault && !phaseB.cleanupPending;
}

bool PeripheralManager::setupEncoderHardware(const PeripheralConfig& config) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || config.type != PeripheralType::ENCODER || config.pinCount != 2 ||
        !config.params.encoder.useInterrupt || config.pins[0] == config.pins[1] ||
        controlOutputIsClaimed(config.id) || managedPinConflict(config)) return false;
    for (uint8_t i = 0; i < 2; ++i) {
        const uint8_t pin = config.pins[i];
        if (!isValidPin(pin) || isReservedPin(pin) || controlOutputPinIsClaimed(pin) ||
            checkPinConflict(pin, config.id) || FastBee::adcPinUnavailable(pin)) return false;
    }
    auto* slot = findEncoderSlot(config.id);
    if (slot) {
        // A healthy repeat does not reset counters or replace an ISR argument.
        // Failed/retiring slots can only leave through checked teardown.
        return slot->pinA == config.pins[0] && slot->pinB == config.pins[1] && encoderSlotReady(slot);
    }
    slot = allocEncoderSlot(config.id, config.pins[0], config.pins[1]);
    auto state = runtimeStates.find(config.id);
    if (!slot) {
        if (state != runtimeStates.end()) state->second.lastError = "ENCODER_RESOURCE_EXHAUSTED";
        return false;
    }
    slot->fault = true;
    const bool ok = _managedPinIo.configure(config.id.c_str(), false, slot->pinA,
                                            FastBee::ManagedGpioMode::PullUp, false) &&
        _managedPinIo.configure(config.id.c_str(), false, slot->pinB,
                                FastBee::ManagedGpioMode::PullUp, false) &&
        _managedPinIo.attach(config.id.c_str(), slot->pinA, handleEncoderInterrupt,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(slot->token)), GPIO_INTR_ANYEDGE);
    slot->fault = !ok;
    portENTER_CRITICAL(&_encoderMux);
    slot->active = ok;
    portEXIT_CRITICAL(&_encoderMux);
    if (state != runtimeStates.end()) {
        state->second.lastError = ok ? "" : "ENCODER_SETUP_UNCONFIRMED";
        if (ok) memset(&state->second.state.encoder, 0, sizeof(state->second.state.encoder));
    }
    return ok;
}

bool PeripheralManager::teardownEncoderHardware(const PeripheralConfig& config) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || config.type != PeripheralType::ENCODER || controlOutputIsClaimed(config.id)) return false;
    auto* slot = findEncoderSlot(config.id);
    if (!slot) return true;
    // Close admission before removing a handler. An in-flight old token can
    // neither update a retiring slot nor a later generation at this address.
    portENTER_CRITICAL(&_encoderMux);
    slot->active = false;
    portEXIT_CRITICAL(&_encoderMux);
    slot->cleanupPending = true;
    const bool phaseA = _managedPinIo.release(config.id.c_str(), false, slot->pinA);
    const bool phaseB = _managedPinIo.release(config.id.c_str(), false, slot->pinB);
    const bool ok = phaseA && phaseB;
    slot->fault = !ok;
    auto state = runtimeStates.find(config.id);
    if (state != runtimeStates.end()) state->second.lastError = ok ? "" : "ENCODER_RELEASE_UNCONFIRMED";
    if (ok) releaseEncoderSlot(config.id);
    return ok;
}
