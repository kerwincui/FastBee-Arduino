#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/MemoryBudget.h"
#include "core/NativeOutputDispatch.h"
#include "core/SensorConfigEpoch.h"
#include "core/RuleOutputIdentity.h"
#include <cstring>
#include <memory>
#include <new>

namespace {
namespace OC = FastBee::OrdinaryControl;
using Context = FastBee::OrdinaryControlContext;
using Result = FastBee::OrdinaryControlResult;
constexpr uint32_t SERVO_RELEASE_DELAY_MS = 500;
}

bool PeripheralManager::writeServoAngle(const String& id, int angle,
    const Context* context, Result* result) {
    return submitOrdinaryServo(id, angle, false, context, result);
}

bool PeripheralManager::submitOrdinaryServo(const String& id, int64_t value, bool rawDuty,
    const Context* context, Result* aggregate) {
    Result result;
    result.evaluated = true;
    auto finish = [&]() {
        FastBee::rememberOrdinaryControlResult(result, aggregate);
        return result.accepted;
    };
    if (id.length() <= 40) memcpy(result.targetId, id.c_str(), id.length() + 1);
    if (!context) { result.reason = "CONTROL_CONTEXT_REQUIRED"; return finish(); }
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) { result.reason = "CONTROL_BUSY"; return finish(); }
    const auto configIt = peripherals.find(id);
    if (configIt == peripherals.end() || configIt->second.type != PeripheralType::PWM_SERVO ||
        configIt->second.isExpanderChannel() || configIt->second.isModbusPeripheral() ||
        !isServoConfigValid(configIt->second)) {
        result.reason = "CONTROL_TARGET_UNSUPPORTED"; return finish();
    }
    auto& config = configIt->second;
    const uint8_t pin = config.getPrimaryPin();
    if (config.pinCount != 1 || !isValidPin(pin) || isReservedPin(pin) || isInputOnlyPin(pin)) {
        result.reason = "CONTROL_TARGET_UNAVAILABLE"; return finish();
    }
    const uint32_t maximum = (1UL << config.params.servo.pwmResolution) - 1UL;
    if ((rawDuty && (value < 0 || static_cast<uint64_t>(value) > maximum)) ||
        (!rawDuty && (value < config.params.servo.minAngle || value > config.params.servo.maxAngle))) {
        result.reason = "CONTROL_PARAMETER_INVALID"; return finish();
    }
    uint64_t now = 0;
    if (!inspectOrdinaryControlLocked(id, *context, OC::Intent::Write, result, now)) return finish();
    bool inserted = false;
    std::unique_ptr<ServoReleaseTickerData> candidate;
    if (config.params.servo.releaseAfterMove) {
        candidate.reset(prepareServoReleaseLocked(id, inserted));
        if (!candidate) { result.reason = "CONTROL_CAPACITY"; return finish(); }
    }
    auto discardEmpty = [&]() {
        const auto slot = servoReleaseTickers.find(id);
        if (inserted && slot != servoReleaseTickers.end() && !slot->second) servoReleaseTickers.erase(slot);
    };
    OC::Permit permit;
    if (!commitOrdinaryControlLocked(id, *context, OC::Intent::Write, result, now,
                                     config.params.servo.releaseAfterMove, permit)) {
        discardEmpty(); return finish();
    }
    // Retain the cleanup node before an SDK call can partially attach or write.
    ServoReleaseTickerData* release = nullptr;
    if (candidate) {
        auto slot = servoReleaseTickers.find(id);
        if (slot == servoReleaseTickers.end() || slot->second) {
            completeOrdinaryControlLocked(id, now, result, false, "SERVO_RELEASE_SLOT_UNCONFIRMED");
            return finish();
        }
        candidate->ordinaryContext = *context;
        candidate->ordinaryPermit = permit;
        release = candidate.get();
        slot->second = candidate.release();
    }
    result.attempted = true;
    if (!attachServoSignal(config)) {
        completeOrdinaryControlLocked(id, now, result, false, "SERVO_ATTACH_UNCONFIRMED");
        return finish();
    }
    const uint16_t pulse = rawDuty ? 0 : servoAngleToPulseUs(
        static_cast<int>(value), config.params.servo.minAngle, config.params.servo.maxAngle,
        config.params.servo.minPulse, config.params.servo.maxPulse, config.params.servo.inverted);
    const uint32_t duty = rawDuty ? static_cast<uint32_t>(value) : servoPulseUsToDuty(
        pulse, config.params.servo.frequency, config.params.servo.pwmResolution);
    if (!ordinaryPermitCurrentLocked(id, permit) || !ledcWrite(config.getPrimaryPin(), duty)) {
        completeOrdinaryControlLocked(id, now, result, false, "SERVO_WRITE_UNCONFIRMED");
        return finish();
    }
    auto& state = runtimeStates.find(id)->second;
    state.status = PeripheralStatus::PERIPHERAL_RUNNING;
    state.lastActivity = millis();
    state.state.servo.signalAttached = true;
    if (!rawDuty) {
        state.state.servo.currentAngle = static_cast<int16_t>(value);
        state.state.servo.currentPulseUs = pulse;
    }
    const bool accepted = !release || startServoReleaseLocked(*release);
    completeOrdinaryControlLocked(id, now, result, accepted, "SERVO_RELEASE_SCHEDULE_FAILED");
    return finish(); // SDK acknowledgment only, never a measured angle.
}

PeripheralManager::ServoReleaseTickerData* PeripheralManager::prepareServoReleaseLocked(
    const String& id, bool& inserted) {
    inserted = false;
    const auto config = peripherals.find(id);
    if (config == peripherals.end() || id.isEmpty() || id.length() > 40 ||
        id.length() != strlen(id.c_str()) || _nextActionTickerToken == UINT32_MAX ||
        FastBee::MemoryBudget::dramFree() < 4096) return nullptr;
    std::unique_ptr<ServoReleaseTickerData> data(new (std::nothrow) ServoReleaseTickerData());
    if (!data) return nullptr;
    data->id = id;
    if (data->id != id || data->id.length() != id.length()) return nullptr;
    data->pin = config->second.getPrimaryPin();
    data->token = ++_nextActionTickerToken;
    try {
        const auto slot = servoReleaseTickers.emplace(id, nullptr);
        if (slot.first->first != id) {
            if (slot.second) servoReleaseTickers.erase(slot.first);
            return nullptr;
        }
        inserted = slot.second;
    } catch (const std::bad_alloc&) { return nullptr; }
    return data.release();
}

bool PeripheralManager::startServoReleaseLocked(ServoReleaseTickerData& data) {
    return data.timer.create(servoReleaseTickerCallback, data.token) &&
           data.timer.startPeriodic(SERVO_RELEASE_DELAY_MS);
}

void PeripheralManager::servoReleaseTickerCallback(void* token) {
    getInstance().serviceServoRelease(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(token)));
}

void PeripheralManager::serviceServoRelease(uint32_t token) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || !token) return;
    for (auto it = servoReleaseTickers.begin(); it != servoReleaseTickers.end(); ++it) {
        auto* data = it->second;
        if (!data || data->token != token) continue;
        const auto state = runtimeStates.find(it->first);
        const auto config = peripherals.find(it->first);
        if (data->ordinaryPermit.generation &&
            (!ordinaryPermitCurrentLocked(it->first, data->ordinaryPermit) ||
             !FastBee::SensorConfigEpoch::matches(data->ordinaryContext.configEpoch) ||
             (data->ordinaryContext.ruleBound &&
              !FastBee::RuleOutputIdentity::matches(data->ordinaryContext.ruleEpoch)) ||
             !isUartEventOriginCurrent(data->ordinaryContext.uartOrigin))) {
            if (state != runtimeStates.end())
                retireOrdinaryDigitalLocked(it->first, _nextOrdinaryControlOperation, false,
                                            state->second.ordinaryControl.invocationActive);
            return;
        }
        const bool sameTarget = config != peripherals.end() && state != runtimeStates.end() &&
            config->second.type == PeripheralType::PWM_SERVO && config->second.getPrimaryPin() == data->pin;
        if (!sameTarget) {
            data->token = 0;
            if (state != runtimeStates.end()) {
                state->second.ordinaryControl.phase = OC::Phase::Fault;
                state->second.lastError = "SERVO_TARGET_UNCONFIRMED";
            }
            return; // Retain evidence; never release a replaced target.
        }
        if (!data->releaseDone && !releaseServoSignal(it->first, data->pin)) {
            if (data->ordinaryPermit.generation) state->second.ordinaryControl.phase = OC::Phase::Fault;
            state->second.lastError = "SERVO_RELEASE_PENDING";
            return; // Retry only the already accepted release, never the move.
        }
        data->releaseDone = true;
        if (!data->timer.stopAndDelete()) {
            if (data->ordinaryPermit.generation) state->second.ordinaryControl.phase = OC::Phase::Fault;
            state->second.lastError = "SERVO_TIMER_STOP_UNCONFIRMED";
            return; // Keep numeric token/node; a later callback may confirm deletion.
        }
        const bool ordinary = data->ordinaryPermit.generation != 0;
        delete data;
        servoReleaseTickers.erase(it);
        if (ordinary) {
            auto& lease = state->second.ordinaryControl;
            if (!lease.invocationActive) {
                if (lease.holdMs) lease.phase = OC::Phase::Held;
                else retireOrdinaryDigitalLocked(config->first, lease.operation);
            }
        }
        return;
    }
}

bool PeripheralManager::scheduleServoRelease(const String& id) {
    RecursiveMutexGuard lock(_mutex);
    if (!lock.isLocked() || ordinaryDigitalClaimedLocked(id)) return false;
    const auto config = peripherals.find(id);
    const auto state = runtimeStates.find(id);
    if (config == peripherals.end() || state == runtimeStates.end() || !config->second.enabled ||
        config->second.type != PeripheralType::PWM_SERVO || !config->second.params.servo.releaseAfterMove ||
        !state->second.state.servo.signalAttached || state->second.memorySuspended) return false;
    bool inserted = false;
    std::unique_ptr<ServoReleaseTickerData> candidate(prepareServoReleaseLocked(id, inserted));
    if (!candidate) {
        state->second.lastError = "SERVO_RELEASE_SCHEDULE_FAILED"; return false;
    }
    if (!cancelServoRelease(id, true)) return false;
    auto slot = servoReleaseTickers.find(id);
    if (slot == servoReleaseTickers.end() || slot->second) return false;
    slot->second = candidate.release();
    if (startServoReleaseLocked(*slot->second)) return true;
    state->second.lastError = "SERVO_RELEASE_SCHEDULE_FAILED";
    return false; // A created SDK handle remains owned by the retained node.
}

bool PeripheralManager::cancelServoRelease(const String& id, bool keepSlot) {
    RecursiveMutexGuard lock(_mutex);
    if (!lock.isLocked()) return false;
    const auto slot = servoReleaseTickers.find(id);
    if (slot == servoReleaseTickers.end()) return true;
    if (slot->second) {
        slot->second->token = 0; // Late callbacks cannot affect a replacement.
        if (!slot->second->timer.stopAndDelete()) return false;
        delete slot->second; slot->second = nullptr;
    }
    if (!keepSlot) servoReleaseTickers.erase(slot);
    return true; // Timer retirement only, no claim about signal or mechanical position.
}

bool PeripheralManager::releaseServoSignal(const String& id, uint8_t capturedPin, bool* attempted) {
    RecursiveMutexGuard lock(_mutex);
    if (!lock.isLocked()) return false;
    const auto config = peripherals.find(id);
    const auto state = runtimeStates.find(id);
    if (config == peripherals.end() || config->second.type != PeripheralType::PWM_SERVO ||
        !config->second.params.servo.releaseAfterMove || state == runtimeStates.end()) return false;
    const uint8_t pin = capturedPin == 255 ? config->second.getPrimaryPin() : capturedPin;
    if (config->second.getPrimaryPin() != pin || !isValidPin(pin) || isReservedPin(pin) || isInputOnlyPin(pin))
        return false;
    if (!state->second.state.servo.signalAttached) return true;
    bool owned = false;
    for (const auto& lease : _ledcResourceLeases)
        if (lease.pin == pin && !lease.commandScript && !strcmp(lease.id, id.c_str())) owned = true;
    if (!owned) return false;
    if (attempted) *attempted = true;
    if (!ledcWrite(pin, 0) || !detachManagedLedc(id, pin)) return false;
    pinMode(pin, INPUT);
    state->second.status = PeripheralStatus::PERIPHERAL_INITIALIZED;
    state->second.lastActivity = millis();
    state->second.state.servo.signalAttached = false;
    // Preserve prior failures; signal detachment is not mechanical position feedback.
    return true;
}

bool PeripheralManager::ordinaryMotionProducerPresentLocked(const String& id) const {
    const auto servo = servoReleaseTickers.find(id);
    if (servo != servoReleaseTickers.end() && servo->second) return true;
    const auto stepper = stepperTickers.find(id);
    return stepper != stepperTickers.end() && stepper->second &&
        (stepper->second->running || !stepper->second->timer.releaseConfirmed());
}

bool PeripheralManager::retireOrdinaryMotionLocked(const String& id, bool keepSlot, bool* attempted) {
    const auto servo = servoReleaseTickers.find(id);
    if (servo != servoReleaseTickers.end() && servo->second) {
        auto* data = servo->second;
        data->token = 0;
        if (!data->timer.stopAndDelete()) return false;
        if (!data->releaseDone) {
            if (!releaseServoSignal(id, data->pin, attempted)) return false;
            data->releaseDone = true;
        }
        delete data; servo->second = nullptr;
        if (!keepSlot) servoReleaseTickers.erase(servo);
    }
    const auto stepper = stepperTickers.find(id);
    if (stepper != stepperTickers.end() && stepper->second) {
        if (!stopStepper(id, attempted)) return false;
    }
    return true;
}

bool PeripheralManager::ordinaryMotionQuiescentLocked(const String& id) const {
    if (ordinaryMotionProducerPresentLocked(id)) return false;
    for (const auto& lease : _ledcResourceLeases)
        if (lease.cleanupFailed && !lease.commandScript && !strcmp(lease.id, id.c_str())) return false;
    const auto stepper = stepperTickers.find(id);
    return stepper == stepperTickers.end() || !stepper->second ||
        (!stepper->second->retainedMask && !stepper->second->cleanupPending);
}
