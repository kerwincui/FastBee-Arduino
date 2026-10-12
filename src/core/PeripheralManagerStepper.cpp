#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/CheckedAdc.h"
#include "core/NativeOutputDispatch.h"
#include "core/SensorConfigEpoch.h"
#include "core/RuleOutputIdentity.h"
#include "core/MemoryBudget.h"
#include "PeripheralManagerConfigHelpers.h"
#if FASTBEE_ENABLE_NEOPIXEL
#include "peripherals/NeoPixelTransport.h"
#endif
#include <cstring>
#include <memory>
#include <new>

using namespace FastBee::PeripheralManagerConfigDetail;

namespace {
uint32_t stepperIntervalMs(uint16_t rpm, uint16_t stepsPerRev) {
    rpm = clampStepperSpeed(rpm == 0 ? STEPPER_DEFAULT_RPM : rpm);
    if (stepsPerRev == 0) stepsPerRev = STEPPER_DEFAULT_STEPS_PER_REV;
    uint32_t interval = 60000UL / (static_cast<uint32_t>(rpm) * stepsPerRev);
    if (interval < 2) interval = 2;
    if (interval > 1000) interval = 1000;
    return interval;
}
void stepperTickerCallback(void* token) {
    PeripheralManager::getInstance().serviceStepperTicker(
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(token)));
}
}

bool PeripheralManager::setupStepperHardware(const PeripheralConfig& config) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || !config.enabled || config.type != PeripheralType::STEPPER_MOTOR || config.pinCount != 4 ||
        config.id.isEmpty() || config.id.length() >= FastBee::ResourceConsumerIdentity::ID_CAPACITY ||
        config.id.length() != strlen(config.id.c_str())) return false;
    // Validate the whole bundle before acquiring its first pin.
    for (uint8_t i = 0; i < 4; ++i) {
        const uint8_t pin = config.pins[i];
        if (!isValidPin(pin) || isReservedPin(pin) || isInputOnlyPin(pin) ||
            controlOutputPinIsClaimed(pin) || checkPinConflict(pin, config.id) ||
            FastBee::adcPinUnavailable(pin) || _managedPinIo.hasDac(pin) ||
            _managedPinIo.conflicts(config.id.c_str(), false, pin)) return false;
        for (uint8_t j = 0; j < i; ++j) if (config.pins[j] == pin) return false;
        for (const auto& lease : _ledcResourceLeases) if (lease.pin == pin) return false;
#if FASTBEE_ENABLE_NEOPIXEL
        if (fastbeeNeoPixelProjectPinRetained(pin)) return false;
#endif
    }
    auto slot = stepperTickers.find(config.id);
    if (slot == stepperTickers.end()) {
        if (FastBee::MemoryBudget::dramFree() < 4096) return false;
        std::unique_ptr<StepperTickerData> created(new (std::nothrow) StepperTickerData());
        if (!created) return false;
        created->id = config.id;
        if (created->id != config.id || created->id.length() != config.id.length()) return false;
        try {
            const auto added = stepperTickers.emplace(config.id, created.get());
            if (!added.second || added.first->first != config.id) {
                if (added.second) stepperTickers.erase(added.first);
                return false;
            }
            slot = added.first;
        } catch (const std::bad_alloc&) { return false; }
        created.release();
    }
    auto& data = *slot->second;
    if (data.cleanupPending || (data.retainedMask && memcmp(data.pins, config.pins, sizeof(data.pins))))
        return false;
    if (data.pinsReady) {
        // Repeated initialization must not reset energized coils. Verify only
        // our checked ledger; this is not a physical readback or a global lock.
        for (uint8_t i = 0; i < 4; ++i) {
            FastBee::GpioResourceSnapshot::Lease observed;
            if (!_managedPinIo.observeGpio(data.pins[i], observed) || observed.fault ||
                observed.cleanupPending || observed.mode != FastBee::ManagedGpioMode::Output) return false;
        }
        return true;
    }
    if (data.retainedMask) return false;
    memcpy(data.pins, config.pins, sizeof(data.pins));
    for (uint8_t i = 0; i < 4; ++i) {
        // Remember the attempted pad before IDF can partially change it.
        data.retainedMask |= uint8_t(1U << i);
        if (!_managedPinIo.configure(config.id.c_str(), false, data.pins[i],
                                     FastBee::ManagedGpioMode::Output, false)) {
            data.cleanupPending = true;
            auto state = runtimeStates.find(config.id);
            if (state != runtimeStates.end()) {
                state->second.status = PeripheralStatus::PERIPHERAL_ERROR;
                state->second.lastError = "STEPPER_SETUP_UNCONFIRMED";
            }
            return false;
        }
    }
    data.pinsReady = true;
    return true;
}

bool PeripheralManager::writeStepperPhase(StepperTickerData& data, uint8_t phase) {
    static const uint8_t sequence[8][4] = {
        {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
        {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1}
    };
    if (!data.pinsReady || data.cleanupPending || data.retainedMask != 0x0F) return false;
    for (uint8_t i = 0; i < 4; ++i)
        if (!_managedPinIo.write(data.id.c_str(), false, data.pins[i], sequence[phase & 7][i] != 0))
            return false; // A partial phase is unknown; never replay or roll it back automatically.
    return true;
}

bool PeripheralManager::stopStepper(const String& id, bool* attempted) {
    RecursiveMutexGuard lock(_mutex);
    if (!lock.isLocked()) return false;
    const auto slot = stepperTickers.find(id);
    if (slot == stepperTickers.end()) {
        const auto config = peripherals.find(id);
        return config != peripherals.end() && config->second.type == PeripheralType::STEPPER_MOTOR;
    }
    auto& data = *slot->second;
    data.running = false; data.direction = 0; data.token = 0;
    if (!data.timer.stopAndDelete()) {
        data.cleanupPending = true; data.pinsReady = false;
        return false; // Retain timer and all four pads until deletion is acknowledged.
    }
    data.pinsReady = false;
    data.cleanupPending = true;
    bool released = true;
    for (uint8_t i = 0; i < 4; ++i) {
        if (!(data.retainedMask & (1U << i))) continue;
        // Use captured pins even if the configuration was removed or replaced.
        // release checks low and input mode, and retains its faulted lease on
        // either failure. All remaining owned pads still get a cleanup attempt.
        if (attempted) *attempted = true;
        if (_managedPinIo.release(data.id.c_str(), false, data.pins[i]))
            data.retainedMask &= uint8_t(~(1U << i));
        else released = false;
    }
    data.cleanupPending = !released;
    const auto state = runtimeStates.find(id);
    if (state != runtimeStates.end()) {
        state->second.status = released ? PeripheralStatus::PERIPHERAL_INITIALIZED : PeripheralStatus::PERIPHERAL_ERROR;
        if (!released) state->second.lastError = "STEPPER_RELEASE_UNCONFIRMED";
        if (released) state->second.lastActivity = millis();
    }
    return released;
}

bool PeripheralManager::teardownStepperHardware(const String& id) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) return false;
    if (!stopStepper(id)) return false;
    const auto slot = stepperTickers.find(id);
    if (slot != stepperTickers.end()) {
        delete slot->second;
        stepperTickers.erase(slot);
    }
    return true;
}

void PeripheralManager::snapshotStepperResources(FastBee::StepperResourceSnapshot& result) const {
    result = FastBee::StepperResourceSnapshot{};
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) { result.status = FastBee::ResourceObservation::Busy; return; }
    for (const auto& item : stepperTickers) {
        const auto* data = item.second;
        if (!data || (!data->retainedMask && !data->running && !data->cleanupPending)) continue;
        if (result.count == result.CAPACITY) { result.truncated = true; break; }
        auto& lease = result.leases[result.count++];
        lease.consumer.assign(FastBee::ResourceConsumerRole::Peripheral, data->id.c_str());
        memcpy(lease.pins, data->pins, sizeof(lease.pins));
        lease.retainedMask = data->retainedMask;
        lease.running = data->running; lease.ready = data->pinsReady;
        lease.cleanupPending = data->cleanupPending; lease.fault = data->cleanupPending;
        for (uint8_t i = 0; i < 4; ++i) {
            if (!(data->retainedMask & (1U << i))) continue;
            FastBee::GpioResourceSnapshot::Lease gpio;
            if (!_managedPinIo.observeGpio(data->pins[i], gpio) || gpio.commandScript ||
                !gpio.consumer.equals(lease.consumer)) { lease.fault = true; lease.ready = false; continue; }
            const uint8_t bit = uint8_t(1U << i);
            lease.gpioOwnedMask |= bit;
            if (gpio.fault) lease.gpioFaultMask |= bit;
            if (gpio.cleanupPending) lease.gpioCleanupPendingMask |= bit;
            if (!gpio.fault && !gpio.cleanupPending && gpio.mode == FastBee::ManagedGpioMode::Output)
                lease.gpioReadyMask |= bit;
            else { lease.fault = true; lease.ready = false; }
        }
    }
}

void PeripheralManager::serviceStepperTicker(uint32_t token) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || !token) return;
    for (const auto& item : stepperTickers) {
        auto* data = item.second;
        if (!data || data->token != token || !data->running || !data->direction) continue;
        const auto config = peripherals.find(item.first);
        const auto state = runtimeStates.find(item.first);
        const bool sourceCurrent = isUartEventOriginCurrent(data->uartOrigin) &&
            FastBee::SensorConfigEpoch::matches(data->ordinaryContext.configEpoch) &&
            (!data->ordinaryContext.ruleBound ||
             FastBee::RuleOutputIdentity::matches(data->ordinaryContext.ruleEpoch));
        if (!sourceCurrent || !ordinaryPermitCurrentLocked(item.first, data->ordinaryPermit) ||
            config == peripherals.end() || state == runtimeStates.end() ||
            !config->second.enabled || config->second.type != PeripheralType::STEPPER_MOTOR ||
            config->second.pinCount != 4 || memcmp(config->second.pins, data->pins, sizeof(data->pins)) ||
            state->second.memorySuspended || state->second.memoryCleanupPending ||
            (state->second.status != PeripheralStatus::PERIPHERAL_INITIALIZED &&
             state->second.status != PeripheralStatus::PERIPHERAL_RUNNING)) {
            data->running = false; data->direction = 0; data->token = 0;
            if (state != runtimeStates.end()) {
                state->second.lastError = !sourceCurrent ? "CONTROL_SOURCE_STALE" : "STEPPER_TARGET_UNCONFIRMED";
                retireOrdinaryDigitalLocked(item.first, _nextOrdinaryControlOperation, false,
                                            state->second.ordinaryControl.invocationActive);
            }
            return; // No stale phase or implicit replay. Cleanup uses captured pads.
        }
        const uint8_t nextPhase = data->direction > 0 ? ((data->phase + 1) & 7) : ((data->phase + 7) & 7);
        if (!writeStepperPhase(*data, nextPhase)) {
            data->running = false; data->direction = 0; data->token = 0;
            data->timer.stopAndDelete(); // Node and retainedMask survive an unconfirmed stop.
            data->cleanupPending = true; data->pinsReady = false;
            state->second.status = PeripheralStatus::PERIPHERAL_ERROR;
            state->second.lastError = "STEPPER_PHASE_UNCONFIRMED";
            state->second.ordinaryControl.phase = FastBee::OrdinaryControl::Phase::Fault;
            return;
        }
        data->phase = nextPhase;
        state->second.status = PeripheralStatus::PERIPHERAL_RUNNING;
        state->second.lastActivity = millis();
        return;
    }
}

bool PeripheralManager::controlStepper(const String& id, const String& action, int value,
    const FastBee::UartEventOrigin& uartOrigin,
    const FastBee::OrdinaryControlContext* context, FastBee::OrdinaryControlResult* aggregate) {
    namespace OC = FastBee::OrdinaryControl;
    FastBee::OrdinaryControlResult result;
    result.evaluated = true;
    if (id.length() <= 40) memcpy(result.targetId, id.c_str(), id.length() + 1);
    auto finish = [&]() {
        FastBee::rememberOrdinaryControlResult(result, aggregate);
        return result.accepted;
    };
    if (!context) { result.reason = "CONTROL_CONTEXT_REQUIRED"; return finish(); }
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) { result.reason = "CONTROL_BUSY"; return finish(); }
    PeripheralConfig* config = getPeripheral(id);
    if (!config || config->type != PeripheralType::STEPPER_MOTOR ||
        config->pinCount != 4 || config->isExpanderChannel() || config->isModbusPeripheral()) {
        result.reason = "CONTROL_TARGET_UNSUPPORTED"; return finish();
    }
    String cmd = action;
    if (cmd != action || id.length() != strlen(id.c_str()) || action.length() != strlen(action.c_str())) {
        result.reason = "CONTROL_PARAMETER_INVALID"; return finish();
    }
    cmd.trim(); cmd.toLowerCase();
    const bool stop = cmd == "stop" || cmd == "off" || cmd == "idle";
    const bool speedCommand = cmd == "setspeed" || cmd == "speed" || cmd == "faster" ||
        cmd == "accelerate" || cmd == "speedup" || cmd == "inc" || cmd == "slower" ||
        cmd == "decelerate" || cmd == "speeddown" || cmd == "dec";
    if ((!stop && !speedCommand && cmd != "forward" && cmd != "cw" && cmd != "start" &&
         cmd != "reverse" && cmd != "backward" && cmd != "ccw" && cmd != "direction") ||
        (speedCommand && (value < 0 || value > UINT16_MAX)) ||
        (cmd == "direction" && value != -1 && value != 1) ||
        (!speedCommand && cmd != "direction" && value != 0)) {
        result.reason = "CONTROL_PARAMETER_INVALID"; return finish();
    }
    // Invalid hardware descriptions must not retire an otherwise valid owner.
    // Cleanup still uses the captured four-pad bundle even after config drift.
    if (!stop) {
        for (uint8_t i = 0; i < 4; ++i) {
            const uint8_t pin = config->pins[i];
            if (!isValidPin(pin) || isReservedPin(pin) || isInputOnlyPin(pin) ||
                controlOutputPinIsClaimed(pin) || checkPinConflict(pin, config->id) ||
                FastBee::adcPinUnavailable(pin) || _managedPinIo.hasDac(pin) ||
                _managedPinIo.conflicts(config->id.c_str(), false, pin)) {
                result.reason = "CONTROL_TARGET_UNAVAILABLE"; return finish();
            }
            for (uint8_t j = 0; j < i; ++j) if (config->pins[j] == pin) {
                result.reason = "CONTROL_PARAMETER_INVALID"; return finish();
            }
            for (const auto& lease : _ledcResourceLeases) if (lease.pin == pin) {
                result.reason = "CONTROL_TARGET_UNAVAILABLE"; return finish();
            }
#if FASTBEE_ENABLE_NEOPIXEL
            if (fastbeeNeoPixelProjectPinRetained(pin)) {
                result.reason = "CONTROL_TARGET_UNAVAILABLE"; return finish();
            }
#endif
        }
    }
    if (context->uartOrigin.required != uartOrigin.required ||
        context->uartOrigin.configEpoch != uartOrigin.configEpoch ||
        context->uartOrigin.receiveGeneration != uartOrigin.receiveGeneration) {
        result.reason = "CONTROL_SOURCE_STALE"; return finish();
    }
    const OC::Intent intent = stop ? OC::Intent::Stop : OC::Intent::ReplaceOwnedProducer;
    uint64_t now = 0;
    if (!inspectOrdinaryControlLocked(id, *context, intent, result, now)) return finish();
    const auto runtime = runtimeStates.find(id);
    auto existing = stepperTickers.find(id);
    uint16_t rpm = clampStepperSpeed(config->params.stepper.speed == 0 ?
        STEPPER_DEFAULT_RPM : config->params.stepper.speed);
    const uint16_t steps = config->params.stepper.stepsPerRevolution == 0 ?
        STEPPER_DEFAULT_STEPS_PER_REV : config->params.stepper.stepsPerRevolution;
    int8_t direction = existing != stepperTickers.end() && existing->second->running ?
        existing->second->direction : 0;
    if (cmd == "setspeed" || cmd == "speed")
        rpm = clampStepperSpeed(static_cast<uint16_t>(value ? value : rpm));
    else if (cmd == "faster" || cmd == "accelerate" || cmd == "speedup" || cmd == "inc") {
        const uint32_t next = static_cast<uint32_t>(rpm) + (value ? static_cast<uint16_t>(value) : 2);
        rpm = next > STEPPER_MAX_RPM ? STEPPER_MAX_RPM : static_cast<uint16_t>(next);
    } else if (cmd == "slower" || cmd == "decelerate" || cmd == "speeddown" || cmd == "dec") {
        const uint16_t delta = value ? static_cast<uint16_t>(value) : 2;
        rpm = rpm > delta ? clampStepperSpeed(static_cast<uint16_t>(rpm - delta)) : STEPPER_MIN_RPM;
    }
    if (cmd == "forward" || cmd == "cw" || cmd == "start") direction = 1;
    else if (cmd == "reverse" || cmd == "backward" || cmd == "ccw") direction = -1;
    else if (cmd == "direction") direction = value < 0 ? -1 : 1;
    if (!stop && direction && (_nextActionTickerToken == UINT32_MAX ||
        FastBee::MemoryBudget::dramFree() < 4096)) {
        result.reason = "CONTROL_CAPACITY"; return finish();
    }
    // Reserve only memory before replacing an owner; configure all four pads after commit.
    bool inserted = false;
    if (!stop && direction && existing == stepperTickers.end()) {
        std::unique_ptr<StepperTickerData> candidate(new (std::nothrow) StepperTickerData());
        if (!candidate) { result.reason = "CONTROL_CAPACITY"; return finish(); }
        candidate->id = id;
        if (candidate->id != id || candidate->id.length() != id.length()) {
            result.reason = "CONTROL_CAPACITY"; return finish();
        }
        try {
            const auto slot = stepperTickers.emplace(id, candidate.get());
            if (!slot.second || slot.first->first != id) {
                if (slot.second) stepperTickers.erase(slot.first);
                result.reason = "CONTROL_CAPACITY"; return finish();
            }
            existing = slot.first; inserted = true; candidate.release();
        } catch (const std::bad_alloc&) { result.reason = "CONTROL_CAPACITY"; return finish(); }
    }
    OC::Permit permit;
    if (!commitOrdinaryControlLocked(id, *context, intent, result, now, false, permit)) {
        if (inserted && existing != stepperTickers.end() && !existing->second->retainedMask &&
            existing->second->timer.releaseConfirmed()) {
            delete existing->second; stepperTickers.erase(existing);
        }
        return finish();
    }
    if (stop) return finish();
    if (!direction) {
        config->params.stepper.speed = rpm;
        result.accepted = true;
        result.reason = "CONTROL_ACCEPTED_CONFIG_ONLY";
        updateOrdinaryControlResultLocked(id, now, result);
        return finish();
    }
    result.attempted = true;
    if (!setupStepperHardware(*config)) {
        completeOrdinaryControlLocked(id, now, result, false, "STEPPER_SETUP_UNCONFIRMED");
        return finish();
    }
    auto* data = stepperTickers.find(id)->second;
    data->token = ++_nextActionTickerToken;
    data->ordinaryContext = *context; data->ordinaryPermit = permit;
    data->uartOrigin = context->uartOrigin;
    data->rpm = rpm; data->stepsPerRev = steps; data->direction = direction;
    data->running = true;
    const bool started = data->timer.create(stepperTickerCallback, data->token) &&
        data->timer.startPeriodic(stepperIntervalMs(rpm, steps));
    if (!started) {
        data->running = false; data->direction = 0; data->token = 0;
        data->cleanupPending = true; data->pinsReady = false;
        runtime->second.status = PeripheralStatus::PERIPHERAL_ERROR;
    } else {
        config->params.stepper.speed = rpm;
        runtime->second.status = PeripheralStatus::PERIPHERAL_RUNNING;
        runtime->second.lastActivity = millis();
    }
    completeOrdinaryControlLocked(id, now, result, started, "STEPPER_TIMER_FAILED");
    return finish();
}
