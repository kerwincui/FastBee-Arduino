#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/MemoryBudget.h"
#include "core/SensorConfigEpoch.h"
#include "core/RuleOutputIdentity.h"
#include "core/ScreenOutputIdentity.h"
#include "core/NativeOutputDispatch.h"
#include "core/ChipConfig.h"
#include <esp_timer.h>
#include <new>

namespace {
namespace OC = FastBee::OrdinaryControl;
using Context = FastBee::OrdinaryControlContext;
using Result = FastBee::OrdinaryControlResult;
using Command = FastBee::DigitalOutputCommand;

bool ordinaryTime(uint64_t& now) {
    const int64_t us = esp_timer_get_time();
    if (us < 0) return false;
    now = static_cast<uint64_t>(us) / 1000ULL;
    return true;
}
bool ordinaryActorCurrent(const OC::Actor& actor) {
    return actor.source != OC::Source::Screen || FastBee::ScreenOutputIdentity::matches(actor.identity);
}
bool nativeOrdinaryTarget(const PeripheralConfig& config) {
    return config.type == PeripheralType::GPIO_DIGITAL_OUTPUT &&
           !config.isExpanderChannel() && !config.isModbusPeripheral() &&
           !config.params.gpio.passive && config.pinCount == 1;
}
bool ioOrdinaryTarget(const PeripheralConfig& config) {
    return config.type == PeripheralType::GPIO_DIGITAL_OUTPUT &&
           config.isExpanderChannel() && !config.isModbusPeripheral() && !config.params.gpio.passive;
}
void fillResult(const OC::Lease& lease, uint64_t now, Result& result) {
    result.holdRemainingMs = 0;
    result.actor = lease.actor;
    result.operation = lease.operation;
    result.generation = lease.generation;
    result.phase = lease.phase;
    if (lease.holdMs && now >= lease.acquiredMs &&
        now - lease.acquiredMs < lease.holdMs)
        result.holdRemainingMs = lease.holdMs - static_cast<uint32_t>(now - lease.acquiredMs);
}
void ordinaryTimerCallback(void* raw) {
    PeripheralManager::getInstance().serviceOrdinaryControlTicker(
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(raw)));
}
}

bool PeripheralManager::beginOrdinaryControl(
    OC::Source source, uint64_t actorIdentity, uint32_t holdMs,
    bool overrideConfirmed, uint64_t reviewedGeneration,
    const String& reviewedTargetId, uint32_t expectedConfigEpoch,
    const FastBee::UartEventOrigin& uartOrigin, Context& context) {
    context = Context{};
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || !OC::validSource(source) || !actorIdentity ||
        _nextOrdinaryControlOperation == UINT64_MAX ||
        !FastBee::SensorConfigEpoch::matches(expectedConfigEpoch) ||
        reviewedTargetId.length() > 40 || reviewedTargetId.length() != strlen(reviewedTargetId.c_str()) ||
        (overrideConfirmed && reviewedTargetId.isEmpty()) ||
        (!overrideConfirmed && !reviewedTargetId.isEmpty()) ||
        !isUartEventOriginCurrent(uartOrigin)) return false;
    OC::Request request;
    request.actor.source = source;
    request.actor.identity = actorIdentity;
    if (!ordinaryActorCurrent(request.actor)) return false;
    request.operation = ++_nextOrdinaryControlOperation;
    request.holdMs = OC::isManual(source) ? holdMs : 0;
    request.overrideConfirmed = overrideConfirmed;
    request.reviewedGeneration = reviewedGeneration;
    OC::Lease idle;
    // Review-dependent validity is evaluated later against the actual target.
    OC::Request validation = request;
    validation.overrideConfirmed = false;
    validation.reviewedGeneration = 0;
    if (OC::inspect(idle, validation, 0, false) != OC::Decision::Acquire ||
        (overrideConfirmed && (!OC::isManual(source) || !reviewedGeneration))) return false;
    context.request = request;
    context.configEpoch = expectedConfigEpoch;
    memcpy(context.reviewedTargetId, reviewedTargetId.c_str(), reviewedTargetId.length() + 1);
    context.uartOrigin = uartOrigin;
    return context.configEpoch != 0;
}

bool PeripheralManager::ordinaryDigitalClaimedLocked(const String& id) const {
    const auto runtime = runtimeStates.find(id);
    if (runtime != runtimeStates.end() && runtime->second.ordinaryControl.phase != OC::Phase::Idle)
        return true;
    // An untagged pre-migration producer has no checked stop acknowledgement.
    // Keep its reservation visible to config/teardown guards until restart.
    const auto config = peripherals.find(id);
    const auto timer = actionTickers.find(id);
    return config != peripherals.end() && FastBee::isOrdinaryScalarTarget(config->second) &&
        timer != actionTickers.end() && timer->second && !timer->second->ordinaryPermit.generation;
}

bool PeripheralManager::ordinaryPermitCurrentLocked(const String& id, const OC::Permit& permit) const {
    uint64_t now = 0;
    const auto runtime = runtimeStates.find(id);
    return ordinaryTime(now) && runtime != runtimeStates.end() && ordinaryActorCurrent(permit.actor) &&
           OC::permitCurrent(runtime->second.ordinaryControl, permit, now);
}

bool PeripheralManager::retireOrdinaryDigitalLocked(const String& id, uint64_t cutoff,
                                                    bool keepTickerSlot, bool keepInvocation, bool* attempted) {
    auto runtime = runtimeStates.find(id);
    if (runtime == runtimeStates.end()) return false;
    auto& state = runtime->second;
    auto& lease = state.ordinaryControl;
    const auto ticker = actionTickers.find(id);
    // An unrelated legacy timer is not silently adopted or deleted here.
    if (ticker != actionTickers.end() && ticker->second &&
        ticker->second->ordinaryPermit.generation == 0) return false;
    // PM serialization confirms any synchronous IOX write has returned.
    // Cancellation requires no I2C/clock and never releases RESET or pending bits.
    if (!retireOrdinaryIoProducerLocked(id, keepTickerSlot)) return false;
    uint64_t now = 0;
    if (!ordinaryTime(now)) {
        // Loss of time evidence must be sticky. A recovered clock cannot make
        // an old producer/permit valid again before confirmed retirement.
        lease.phase = OC::Phase::Fault;
        state.lastError = "CONTROL_CLOCK_UNCONFIRMED";
        return false;
    }
    if (state.ordinaryRetryAfterMs && now < state.ordinaryRetryAfterMs) return false;
    lease.phase = OC::Phase::Retiring;
    if (!retireOrdinaryMotionLocked(id, keepTickerSlot, attempted)) {
        lease.phase = OC::Phase::Fault;
        if (state.ordinaryStopFailures != UINT32_MAX) ++state.ordinaryStopFailures;
        state.ordinaryRetryAfterMs = now + 1000ULL;
        state.lastError = "CONTROL_MOTION_STOP_UNCONFIRMED";
        return false;
    }
    if (ticker != actionTickers.end() && ticker->second) {
        if (!ticker->second->ordinaryTimer.stopAndDelete()) {
            lease.phase = OC::Phase::Fault;
            if (state.ordinaryStopFailures != UINT32_MAX) ++state.ordinaryStopFailures;
            state.ordinaryRetryAfterMs = now + 1000ULL;
            state.lastError = "CONTROL_STOP_UNCONFIRMED";
            return false;
        }
        // Queued callbacks contain only the old numeric token.
        delete ticker->second;
        ticker->second = nullptr;
    }
    if (!keepTickerSlot && ticker != actionTickers.end()) actionTickers.erase(ticker);
    // A missing local timer does not prove that a backend has relinquished
    // asynchronous SDK buffers/pins. Confirm quiescence before publishing Idle.
    // Backend implementations must not write zero/black as a fake stop ack.
    if (!retireOrdinaryRfLocked(id, attempted) || !ordinaryBackendQuiescentLocked(id)) {
        lease.phase = OC::Phase::Fault;
        if (state.ordinaryStopFailures != UINT32_MAX) ++state.ordinaryStopFailures;
        state.ordinaryRetryAfterMs = now + 1000ULL;
        state.lastError = "CONTROL_BACKEND_STOP_UNCONFIRMED";
        return false;
    }
    state.ordinaryRetryAfterMs = 0;
    // Expired/stale execution loses permission immediately, but no other actor
    // can enter until the invocation returns its terminal acknowledgement.
    if (keepInvocation && lease.invocationActive) return true;
    const uint64_t retiredThrough =
        cutoff > lease.retiredThrough ? cutoff : lease.retiredThrough;
    lease = OC::Lease{};
    lease.retiredThrough = retiredThrough;
    // Do not clear lastError/currentState or historical failure counters here.
    return true;
}

bool PeripheralManager::ordinaryBackendSupportedLocked(const PeripheralConfig& config) const {
    if (config.type == PeripheralType::LCD)
        return config.pinCount == 2 && config.params.lcd.interface == 2 &&
            !config.isModbusPeripheral() && !config.isExpanderChannel();
    if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637)
        return config.pinCount == 2 && !config.isModbusPeripheral() && !config.isExpanderChannel();
    if (config.type == PeripheralType::RF_MODULE)
        return config.params.rf.mode == RF_MODE_TX && config.pinCount == 1 &&
            !config.isModbusPeripheral() && !config.isExpanderChannel();
    if (config.type == PeripheralType::PWM_SERVO || config.type == PeripheralType::STEPPER_MOTOR)
        return !config.isModbusPeripheral() && !config.isExpanderChannel() &&
            config.pinCount == (config.type == PeripheralType::PWM_SERVO ? 1 : 4);
    if (config.type == PeripheralType::PCA9685_PWM)
        return config.pinCount == 0 && !config.isModbusPeripheral() && !config.isExpanderChannel();
    if (config.type == PeripheralType::NEO_PIXEL)
        return config.pinCount == 1 && !config.isModbusPeripheral() && !config.isExpanderChannel();
    // Each additional backend must explicitly extend both support and its
    // quiescence contract. Unknown backends never inherit a successful stop.
    return nativeOrdinaryTarget(config) || ioOrdinaryTarget(config) ||
        (FastBee::isOrdinaryScalarTarget(config) && config.pinCount == 1);
}

bool PeripheralManager::ordinaryProducerPresentLocked(const String& id) const {
    const auto ticker = actionTickers.find(id);
    return (ticker != actionTickers.end() && ticker->second) || ordinaryIoProducerPresentLocked(id) ||
        ordinaryMotionProducerPresentLocked(id);
}

bool PeripheralManager::ordinaryBackendQuiescentLocked(const String& id) const {
    const auto config = peripherals.find(id);
    if (config != peripherals.end() && (config->second.type == PeripheralType::SEVEN_SEGMENT_TM1637 ||
        config->second.type == PeripheralType::LCD))
        return ordinaryDisplayQuiescentLocked(id);
    if (config != peripherals.end() && config->second.type == PeripheralType::RF_MODULE)
        return ordinaryRfQuiescentLocked(id);
    if (config != peripherals.end() && (config->second.type == PeripheralType::PWM_SERVO ||
        config->second.type == PeripheralType::STEPPER_MOTOR)) return ordinaryMotionQuiescentLocked(id);
    if (config != peripherals.end() && config->second.type == PeripheralType::PCA9685_PWM)
        return ordinaryPca9685QuiescentLocked(id);
    if (config != peripherals.end() && config->second.type == PeripheralType::NEO_PIXEL)
        return ordinaryNeoPixelQuiescentLocked(id);
    // Native digital SDK writes are synchronous and the checked timer has
    // already been stopped/deleted by retirement. Preserve the last level.
    if (config != peripherals.end() && config->second.type == PeripheralType::DAC) {
        FastBee::DacResourceSnapshot resources;
        _managedPinIo.snapshot(resources);
        for (uint8_t i = 0; i < resources.count; ++i) {
            const auto& retained = resources.leases[i];
            if (retained.pin == config->second.getPrimaryPin() &&
                (retained.fault || retained.cleanupPending)) return false;
        }
    }
    // LEDC and DAC retain their last output state; there is no queued local
    // write once this lock is held and the checked software producer is gone.
    // This does not claim that duty=0, voltage=0, or a load is physically off.
    return config != peripherals.end() &&
        (nativeOrdinaryTarget(config->second) || FastBee::isOrdinaryScalarTarget(config->second) ||
         (ioOrdinaryTarget(config->second) && !ordinaryIoProducerPresentLocked(id)));
}

void PeripheralManager::updateOrdinaryControlResultLocked(const String& id, uint64_t now,
                                                          Result& result) const {
    const auto runtime = runtimeStates.find(id);
    if (runtime != runtimeStates.end()) fillResult(runtime->second.ordinaryControl, now, result);
}

bool PeripheralManager::completeOrdinaryControlLocked(const String& id, uint64_t now,
    Result& result, bool accepted, const char* failureReason) {
    result.attempted = true;
    result.accepted = accepted;
    result.reason = accepted ? "CONTROL_ACCEPTED_SDK_ONLY" : failureReason;
    const auto runtime = runtimeStates.find(id);
    if (!accepted && runtime != runtimeStates.end()) {
        runtime->second.ordinaryControl.phase = OC::Phase::Fault;
        runtime->second.lastError = failureReason;
    }
    updateOrdinaryControlResultLocked(id, now, result);
    return accepted;
}

bool PeripheralManager::inspectOrdinaryControlLocked(const String& id, const Context& context,
    OC::Intent intent, Result& result, uint64_t& now) {
    if (!ordinaryTime(now)) {
        const auto previous = runtimeStates.find(id);
        if (previous != runtimeStates.end() && previous->second.ordinaryControl.phase != OC::Phase::Idle) {
            previous->second.ordinaryControl.phase = OC::Phase::Fault;
            previous->second.lastError = "CONTROL_CLOCK_UNCONFIRMED";
        }
        result.reason = "CONTROL_CLOCK_UNCONFIRMED"; return false;
    }
    const auto config = peripherals.find(id);
    const auto runtime = runtimeStates.find(id);
    if (id.isEmpty() || id.length() > 40 || id.length() != strlen(id.c_str()) ||
        config == peripherals.end() || runtime == runtimeStates.end() ||
        !ordinaryBackendSupportedLocked(config->second)) {
        result.reason = "CONTROL_TARGET_UNSUPPORTED"; return false;
    }
    auto& state = runtime->second;
    auto& lease = state.ordinaryControl;
    memcpy(result.targetId, id.c_str(), id.length() + 1);
    fillResult(lease, now, result);
    if (!ordinaryActorCurrent(context.request.actor) ||
        !FastBee::SensorConfigEpoch::matches(context.configEpoch) ||
        (context.ruleBound && !FastBee::RuleOutputIdentity::matches(context.ruleEpoch)) ||
        !isUartEventOriginCurrent(context.uartOrigin)) {
        result.reason = "CONTROL_SOURCE_STALE"; return false;
    }
    if (context.request.overrideConfirmed && id != context.reviewedTargetId) {
        result.reason = "CONTROL_REVIEW_TARGET_MISMATCH"; return false;
    }
    if (!config->second.enabled || state.memorySuspended || state.memoryCleanupPending ||
        (state.status != PeripheralStatus::PERIPHERAL_INITIALIZED &&
         state.status != PeripheralStatus::PERIPHERAL_RUNNING)) {
        result.reason = "CONTROL_TARGET_NOT_READY"; return false;
    }
    OC::Request request = context.request;
    request.intent = intent;
    result.decision = OC::inspect(lease, request, now, controlOutputIsClaimed(id));
    if (result.decision == OC::Decision::RetireExpired) {
        result.cleanupConfirmed = retireOrdinaryDigitalLocked(id, _nextOrdinaryControlOperation,
            false, lease.invocationActive, &result.attempted);
        result.reason = result.cleanupConfirmed ? "CONTROL_EXPIRED_RETRY_NEW_OPERATION" :
                                                  "CONTROL_STOP_UNCONFIRMED";
        fillResult(lease, now, result);
        return false;
    }
    if (result.decision == OC::Decision::RetireOwned ||
        result.decision == OC::Decision::RetireForOverride ||
        result.decision == OC::Decision::Acquire || result.decision == OC::Decision::Continue ||
        result.decision == OC::Decision::StopIdle) return true;
    result.reason = result.decision == OC::Decision::DedicatedOwner ? "CONTROL_DEDICATED_OWNER" :
        result.decision == OC::Decision::StaleReview ? "CONTROL_REVIEW_STALE" :
        result.decision == OC::Decision::CleanupPending ?
            (lease.phase == OC::Phase::Retiring && OC::expired(lease, now)
                ? "CONTROL_HOLD_EXPIRED" : "CONTROL_STOP_UNCONFIRMED") :
        result.decision == OC::Decision::StaleOperation ? "CONTROL_OPERATION_STALE" :
        result.decision == OC::Decision::Invalid ? "CONTROL_INVALID" : "CONTROL_CONFLICT";
    return false;
}

bool PeripheralManager::commitOrdinaryControlLocked(const String& id, const Context& context,
    OC::Intent intent, Result& result, uint64_t& now, bool keepTickerSlot, OC::Permit& permit) {
    permit = OC::Permit{};
    // Candidate allocation may have crossed an expiry boundary. Re-evaluate
    // while holding the same mutex; retirement never replays an expired request.
    if (!inspectOrdinaryControlLocked(id, context, intent, result, now)) return false;
    auto& lease = runtimeStates.find(id)->second.ordinaryControl;
    const bool replace = result.decision == OC::Decision::RetireOwned ||
                         result.decision == OC::Decision::RetireForOverride;
    if (intent != OC::Intent::Stop && result.decision != OC::Decision::Continue &&
        _nextOrdinaryControlGeneration == UINT64_MAX) {
        result.reason = "CONTROL_CAPACITY"; return false;
    }
    if (replace || ordinaryProducerPresentLocked(id)) {
        const uint64_t cutoff = replace ? lease.operation : lease.retiredThrough;
        const OC::Lease continuing = lease;
        if (!retireOrdinaryDigitalLocked(id, cutoff, keepTickerSlot, false, &result.attempted)) {
            result.reason = "CONTROL_STOP_UNCONFIRMED";
            updateOrdinaryControlResultLocked(id, now, result);
            return false;
        }
        if (!replace && continuing.phase != OC::Phase::Idle) {
            const uint64_t watermark = lease.retiredThrough;
            lease = continuing;
            lease.retiredThrough = watermark;
            lease.phase = OC::Phase::Executing;
        }
    }
    if (intent == OC::Intent::Stop) {
        const bool ok = retireOrdinaryDigitalLocked(id, context.request.operation,
                                                    false, lease.invocationActive, &result.attempted);
        result.cleanupConfirmed = ok;
        result.accepted = ok;
        result.reason = ok ? (result.attempted ? "CONTROL_STOPPED_SDK_ONLY" :
            "CONTROL_STOPPED_NO_LEVEL_CHANGE") : "CONTROL_STOP_UNCONFIRMED";
        updateOrdinaryControlResultLocked(id, now, result);
        return ok;
    }
    if (lease.phase == OC::Phase::Idle) {
        const uint64_t watermark = lease.retiredThrough;
        lease = OC::Lease{};
        lease.retiredThrough = watermark;
        lease.actor = context.request.actor;
        lease.operation = context.request.operation;
        lease.generation = ++_nextOrdinaryControlGeneration;
        lease.holdMs = context.request.holdMs;
        lease.acquiredMs = now;
        lease.phase = OC::Phase::Executing;
        lease.invocationActive = true;
    }
    permit.actor = lease.actor;
    permit.operation = lease.operation;
    permit.generation = lease.generation;
    updateOrdinaryControlResultLocked(id, now, result);
    return true; // Admission is not yet an SDK acknowledgement.
}

bool PeripheralManager::getOrdinaryControlSnapshot(const String& id, Result& result) const {
    result = Result{};
    result.evaluated = true;
    RecursiveMutexGuard lock(_mutex, 0);
    uint64_t now = 0;
    if (!lock.isLocked()) { result.reason = "CONTROL_BUSY"; return false; }
    if (!ordinaryTime(now)) { result.reason = "CONTROL_CLOCK_UNCONFIRMED"; return false; }
    const auto config = peripherals.find(id);
    const auto runtime = runtimeStates.find(id);
    if (config == peripherals.end() || runtime == runtimeStates.end() ||
        !ordinaryBackendSupportedLocked(config->second)) {
        result.reason = "CONTROL_TARGET_UNSUPPORTED"; return false;
    }
    if (id.length() > 40 || id.length() != strlen(id.c_str())) return false;
    memcpy(result.targetId, id.c_str(), id.length() + 1);
    fillResult(runtime->second.ordinaryControl, now, result);
    result.reason = controlOutputIsClaimed(id) ? "CONTROL_DEDICATED_OWNER" :
        OC::expired(runtime->second.ordinaryControl, now) ? "CONTROL_EXPIRED_PENDING_RETIREMENT" :
        result.phase == OC::Phase::Fault ? "CONTROL_STOP_UNCONFIRMED" : "CONTROL_STATUS";
    result.cleanupConfirmed = result.phase == OC::Phase::Idle;
    return true;
}

bool PeripheralManager::submitOrdinaryDigital(
    const String& id, Command command, uint32_t intervalMs,
    const Context& context, Result& result) {
    result = Result{};
    result.evaluated = true;
    RecursiveMutexGuard lock(_mutex, 0);
    uint64_t now = 0;
    if (!lock.isLocked()) { result.reason = "CONTROL_BUSY"; return false; }
    auto config = peripherals.find(id);
    if (config == peripherals.end() ||
        (!nativeOrdinaryTarget(config->second) && !ioOrdinaryTarget(config->second))) {
        result.reason = "CONTROL_TARGET_UNSUPPORTED"; return false;
    }
    if (static_cast<uint8_t>(command) > static_cast<uint8_t>(Command::Stop) ||
        (command == Command::Blink &&
            (intervalMs < (config->second.isExpanderChannel() ? 200U : 1U) || intervalMs > 65535))) {
        result.reason = "CONTROL_PARAMETER_INVALID"; return false;
    }
    const OC::Intent intent = command == Command::Stop ? OC::Intent::Stop :
        command == Command::Blink ? OC::Intent::ReplaceOwnedProducer : OC::Intent::Write;
    if (!inspectOrdinaryControlLocked(id, context, intent, result, now)) return false;
    auto& state = runtimeStates.find(id)->second;
    auto& lease = state.ordinaryControl;

    GPIOState writeValue = command == Command::High ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
    if (command == Command::Toggle) {
        writeValue = readGpioStateLocked(config->second, &id, nullptr);
        if (writeValue == GPIOState::STATE_UNDEFINED) {
            result.reason = "CONTROL_READ_UNCONFIRMED"; return false;
        }
        writeValue = writeValue == GPIOState::STATE_HIGH ? GPIOState::STATE_LOW : GPIOState::STATE_HIGH;
    }

    // Complete all fallible candidate allocations before retiring old output.
    std::unique_ptr<ActionTickerData> candidate;
    const bool ioTarget = config->second.isExpanderChannel();
    bool insertedIoSlot = false;
    bool insertedSlot = false;
    if (command == Command::Blink && ioTarget &&
        !prepareOrdinaryIoProducerLocked(id, insertedIoSlot)) {
        result.reason = "CONTROL_IO_RESERVATION_UNCONFIRMED"; return false;
    }
    if (command == Command::Blink && !ioTarget) {
        if (_nextActionTickerToken == UINT32_MAX ||
            _nextOrdinaryControlGeneration == UINT64_MAX ||
            FastBee::MemoryBudget::dramFree() < FastBee::MemoryBudget::ACTION_TICKER_MIN_DRAM) {
            result.reason = "CONTROL_CAPACITY"; return false;
        }
        candidate.reset(new (std::nothrow) ActionTickerData());
        if (!candidate) { result.reason = "CONTROL_CAPACITY"; return false; }
        candidate->id = id;
        if (candidate->id != id) { result.reason = "CONTROL_CAPACITY"; return false; }
        candidate->pin = config->second.getPrimaryPin();
        candidate->mode = 1;
        candidate->token = ++_nextActionTickerToken;
        candidate->uartOrigin = context.uartOrigin;
        candidate->ordinaryConfigEpoch = context.configEpoch;
        candidate->ordinaryRuleEpoch = context.ruleBound ? context.ruleEpoch : 0;
        try {
            auto slot = actionTickers.emplace(id, nullptr);
            if (slot.first->first != id) {
                if (slot.second) actionTickers.erase(slot.first);
                result.reason = "CONTROL_CAPACITY"; return false;
            }
            insertedSlot = slot.second;
        } catch (const std::bad_alloc&) {
            result.reason = "CONTROL_CAPACITY"; return false;
        }
    }
    auto eraseEmptySlot = [&] {
        discardOrdinaryIoReservationLocked(id, insertedIoSlot);
        const auto slot = actionTickers.find(id);
        if (insertedSlot && slot != actionTickers.end() && !slot->second) actionTickers.erase(slot);
    };
    OC::Permit permit;
    if (!commitOrdinaryControlLocked(id, context, intent, result, now,
                                     command == Command::Blink, permit)) {
        eraseEmptySlot(); return false;
    }
    if (command == Command::Stop) return result.accepted;
    if (command == Command::Blink && ioTarget) {
        result.accepted = startOrdinaryIoProducerLocked(id,
            static_cast<uint16_t>(intervalMs), context, permit);
        if (!result.accepted) eraseEmptySlot();
    } else if (command == Command::Blink) {
        // The map reservation must still exist: fail before any timer/output
        // if retirement removed it. No post-stop allocating fallback.
        auto slot = actionTickers.find(id);
        if (slot == actionTickers.end() || slot->second) {
            lease.phase = OC::Phase::Fault;
            result.reason = "CONTROL_RESERVATION_UNCONFIRMED";
            updateOrdinaryControlResultLocked(id, now, result);
            return false;
        }
        candidate->ordinaryPermit = permit;
        slot->second = candidate.release(); // Retain before fallible SDK calls.
        result.attempted = true;
        result.accepted = slot->second->ordinaryTimer.create(ordinaryTimerCallback, slot->second->token) &&
                          slot->second->ordinaryTimer.startPeriodic(intervalMs);
        if (!result.accepted) {
            lease.phase = OC::Phase::Fault;
            state.lastError = "CONTROL_TIMER_START_UNCONFIRMED";
        }
    } else {
        result.attempted = true;
        result.accepted = writeGpioStateLocked(config->second, &id, nullptr, writeValue, &permit);
        if (!result.accepted) lease.phase = OC::Phase::Fault;
    }
    return completeOrdinaryControlLocked(id, now, result, result.accepted,
        command == Command::Blink ? (ioTarget ? "CONTROL_IO_START_UNCONFIRMED" :
            "CONTROL_TIMER_START_UNCONFIRMED") : "CONTROL_OUTPUT_UNCONFIRMED");
}

bool PeripheralManager::writeOrdinaryScalarLocked(const String& id,
    FastBee::ScalarOutputCommand command, uint32_t value, const OC::Permit& permit) {
    const auto config = peripherals.find(id);
    const auto runtime = runtimeStates.find(id);
    if (config == peripherals.end() || runtime == runtimeStates.end() ||
        !FastBee::isOrdinaryScalarTarget(config->second) || !config->second.enabled ||
        config->second.pinCount != 1 || controlOutputIsClaimed(id) ||
        !ordinaryPermitCurrentLocked(id, permit) || runtime->second.memorySuspended ||
        runtime->second.memoryCleanupPending ||
        (runtime->second.status != PeripheralStatus::PERIPHERAL_INITIALIZED &&
         runtime->second.status != PeripheralStatus::PERIPHERAL_RUNNING)) return false;
    const auto& cfg = config->second;
    uint32_t maximum = 0;
    if (!FastBee::ordinaryScalarMaximum(cfg, maximum) || value > maximum) return false;
    const uint8_t pin = cfg.getPrimaryPin();
    bool ok = false;
    if (command == FastBee::ScalarOutputCommand::Dac && cfg.type == PeripheralType::DAC) {
#if CHIP_HAS_DAC
        if (pin != 25 && pin != 26) return false;
        ok = writeManagedDac(id, pin, static_cast<uint8_t>(value));
#else
        return false;
#endif
    } else if (command == FastBee::ScalarOutputCommand::Pwm && cfg.type != PeripheralType::DAC) {
        const bool passive = cfg.type == PeripheralType::GPIO_DIGITAL_OUTPUT && cfg.params.gpio.passive;
        const uint32_t frequency = passive && cfg.params.gpio.pwmFrequency < 100 ? 2700 : cfg.params.gpio.pwmFrequency;
        const uint8_t resolution = passive && !cfg.params.gpio.pwmResolution ? 8 : cfg.params.gpio.pwmResolution;
        bool owned = false;
        for (const auto& ledc : _ledcResourceLeases) {
            if (ledc.pin != pin || ledc.commandScript || ledc.cleanupFailed ||
                strcmp(ledc.id, id.c_str()) != 0 || ledc.channel != cfg.params.gpio.pwmChannel ||
                ledc.requestedFrequency != frequency || ledc.requestedResolution != resolution) continue;
            owned = true; break;
        }
        if (!owned) return false;
        const uint32_t duty = !passive && cfg.params.gpio.aoMode
            ? mapAnalogOutputPercent(value, cfg.params.gpio.aoMinPct, cfg.params.gpio.aoMaxPct,
                                     cfg.params.gpio.pwmResolution) : value;
        ok = ledcWrite(pin, duty); // Retain the SDK boolean acknowledgement.
        // Acknowledge a logical requested level, not a measured pad/voltage.
        // AO zero may map to a nonzero minimum. Failure makes toggle unknown.
        runtime->second.state.gpio.currentState = ok
            ? (value ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW) : GPIOState::STATE_UNDEFINED;
    }
    if (ok) runtime->second.lastActivity = millis();
    return ok;
}

bool PeripheralManager::submitOrdinaryScalar(const String& id,
    FastBee::ScalarOutputCommand command, uint32_t value, const Context& context, Result& result) {
    using Scalar = FastBee::ScalarOutputCommand;
    result = Result{};
    result.evaluated = true;
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) { result.reason = "CONTROL_BUSY"; return false; }
    const auto config = peripherals.find(id);
    if (config == peripherals.end() || !FastBee::isOrdinaryScalarTarget(config->second) ||
        config->second.pinCount != 1) {
        result.reason = "CONTROL_TARGET_UNSUPPORTED"; return false;
    }
    const bool isDac = config->second.type == PeripheralType::DAC;
    const bool breathe = command == Scalar::Breathe;
    const bool blink = command == Scalar::Blink;
    const bool toggle = command == Scalar::Toggle;
    const bool producer = breathe || blink;
    uint32_t maximum = 0;
    if (static_cast<uint8_t>(command) > static_cast<uint8_t>(Scalar::Blink) ||
        (command != Scalar::Stop &&
         (!FastBee::ordinaryScalarMaximum(config->second, maximum) ||
          (isDac && command != Scalar::Dac) || (!isDac && command == Scalar::Dac) ||
          (breathe && config->second.type != PeripheralType::GPIO_PWM_OUTPUT &&
                      config->second.type != PeripheralType::GPIO_ANALOG_OUTPUT) ||
          (blink && config->second.type != PeripheralType::GPIO_PWM_OUTPUT &&
                    config->second.type != PeripheralType::GPIO_DIGITAL_OUTPUT) ||
          (toggle && value != 0) ||
          (producer ? !value || value > 65535 : value > maximum)))) {
        result.reason = "CONTROL_PARAMETER_INVALID"; return false;
    }
#if !CHIP_HAS_DAC
    if (isDac && command != Scalar::Stop) {
        result.reason = "CONTROL_TARGET_UNSUPPORTED"; return false;
    }
#endif
    const OC::Intent intent = command == Scalar::Stop ? OC::Intent::Stop :
        producer ? OC::Intent::ReplaceOwnedProducer : OC::Intent::Write;
    uint64_t now = 0;
    if (!inspectOrdinaryControlLocked(id, context, intent, result, now)) return false;
    if (toggle || blink) {
        const auto level = runtimeStates.find(id)->second.state.gpio.currentState;
        if (level != GPIOState::STATE_LOW && level != GPIOState::STATE_HIGH) {
            result.reason = "CONTROL_READ_UNCONFIRMED"; return false;
        }
        uint32_t high = 0;
        if (!FastBee::ordinaryScalarHighValue(config->second, high)) {
            result.reason = "CONTROL_PARAMETER_INVALID"; return false;
        }
        if (toggle) { command = Scalar::Pwm; value = level == GPIOState::STATE_HIGH ? 0 : high; }
        else maximum = high;
    }
    std::unique_ptr<ActionTickerData> candidate;
    bool insertedSlot = false;
    if (producer) {
        if (_nextActionTickerToken == UINT32_MAX || _nextOrdinaryControlGeneration == UINT64_MAX ||
            FastBee::MemoryBudget::dramFree() < FastBee::MemoryBudget::ACTION_TICKER_MIN_DRAM) {
            result.reason = "CONTROL_CAPACITY"; return false;
        }
        candidate.reset(new (std::nothrow) ActionTickerData());
        if (!candidate) { result.reason = "CONTROL_CAPACITY"; return false; }
        candidate->id = id;
        if (candidate->id != id) { result.reason = "CONTROL_CAPACITY"; return false; }
        candidate->pin = config->second.getPrimaryPin();
        candidate->mode = blink ? 3 : 2;
        candidate->token = ++_nextActionTickerToken;
        candidate->uartOrigin = context.uartOrigin;
        candidate->ordinaryConfigEpoch = context.configEpoch;
        candidate->ordinaryRuleEpoch = context.ruleBound ? context.ruleEpoch : 0;
        candidate->maxDuty = maximum;
        const uint32_t steps = value / 40U ? value / 40U : 1U;
        candidate->stepSize = maximum / steps ? maximum / steps : 1U;
        try {
            const auto slot = actionTickers.emplace(id, nullptr);
            if (slot.first->first != id) {
                if (slot.second) actionTickers.erase(slot.first);
                result.reason = "CONTROL_CAPACITY"; return false;
            }
            insertedSlot = slot.second;
        } catch (const std::bad_alloc&) { result.reason = "CONTROL_CAPACITY"; return false; }
    }
    OC::Permit permit;
    if (!commitOrdinaryControlLocked(id, context, intent, result, now, producer, permit)) {
        const auto slot = actionTickers.find(id);
        if (insertedSlot && slot != actionTickers.end() && !slot->second) actionTickers.erase(slot);
        return false;
    }
    if (command == Scalar::Stop) return result.accepted;
    if (!producer) return completeOrdinaryControlLocked(id, now, result,
        writeOrdinaryScalarLocked(id, command, value, permit));
    const auto slot = actionTickers.find(id);
    if (slot == actionTickers.end() || slot->second) {
        runtimeStates.find(id)->second.ordinaryControl.phase = OC::Phase::Fault;
        result.reason = "CONTROL_RESERVATION_UNCONFIRMED";
        updateOrdinaryControlResultLocked(id, now, result);
        return false;
    }
    candidate->ordinaryPermit = permit;
    slot->second = candidate.release(); // Preserve context even after partial SDK failure.
    return completeOrdinaryControlLocked(id, now, result,
        slot->second->ordinaryTimer.create(ordinaryTimerCallback, slot->second->token) &&
        slot->second->ordinaryTimer.startPeriodic(blink ? value : 20), "CONTROL_TIMER_START_UNCONFIRMED");
}

void PeripheralManager::finishOrdinaryControl(const Context& context, bool cancelled) {
    if (!context.request.operation || !context.request.actor.identity ||
        !OC::validSource(context.request.actor.source)) return;
    // Task-context terminal acknowledgement cannot be silently lost on a busy
    // zero-wait read. The recursive PM guard contains only bounded local work.
    RecursiveMutexGuard lock(_mutex);
    uint64_t now = 0;
    if (!lock.isLocked()) return;
    const bool clockConfirmed = ordinaryTime(now);
    finishOrdinaryModbusLocked(context, cancelled);
    for (auto& item : runtimeStates) {
        auto& lease = item.second.ordinaryControl;
        if (!OC::sameActor(lease.actor, context.request.actor) ||
            lease.operation != context.request.operation) continue;
        lease.invocationActive = false;
        if (!clockConfirmed) {
            lease.phase = OC::Phase::Fault;
            item.second.lastError = "CONTROL_CLOCK_UNCONFIRMED";
            continue;
        }
        if (cancelled || lease.phase == OC::Phase::Retiring || lease.phase == OC::Phase::Fault ||
            !ordinaryActorCurrent(context.request.actor) ||
            !FastBee::SensorConfigEpoch::matches(context.configEpoch) ||
            (context.ruleBound && !FastBee::RuleOutputIdentity::matches(context.ruleEpoch)) ||
            !isUartEventOriginCurrent(context.uartOrigin)) {
            retireOrdinaryDigitalLocked(item.first, _nextOrdinaryControlOperation);
            continue;
        }
        if (lease.phase != OC::Phase::Executing) continue;
        if (ordinaryProducerPresentLocked(item.first)) lease.phase = OC::Phase::Producing;
        else if (lease.holdMs) lease.phase = OC::Phase::Held;
        else {
            const uint64_t cutoff = lease.operation;
            retireOrdinaryDigitalLocked(item.first, cutoff);
            continue;
        }
        if (OC::expired(lease, now))
            retireOrdinaryDigitalLocked(item.first, _nextOrdinaryControlOperation);
    }
}

void PeripheralManager::serviceOrdinaryControlTicker(uint32_t token) {
    RecursiveMutexGuard lock(_mutex, 0);
    uint64_t now = 0;
    if (!lock.isLocked() || !token) return;
    for (const auto& item : actionTickers) {
        auto* data = item.second;
        if (!data || data->token != token || !data->ordinaryPermit.generation) continue;
        auto runtime = runtimeStates.find(item.first);
        auto config = peripherals.find(item.first);
        if (runtime == runtimeStates.end() || config == peripherals.end()) return;
        auto& state = runtime->second;
        const bool digitalProducer = data->mode == 1 && nativeOrdinaryTarget(config->second);
        const bool scalarTarget = FastBee::isOrdinaryScalarTarget(config->second);
        const bool scalarBlink = data->mode == 3 && scalarTarget &&
            (config->second.type == PeripheralType::GPIO_PWM_OUTPUT ||
             config->second.type == PeripheralType::GPIO_DIGITAL_OUTPUT);
        const bool scalarProducer = scalarBlink || (data->mode == 2 && scalarTarget &&
            (config->second.type == PeripheralType::GPIO_PWM_OUTPUT ||
             config->second.type == PeripheralType::GPIO_ANALOG_OUTPUT));
        if (!ordinaryTime(now) || !ordinaryActorCurrent(data->ordinaryPermit.actor) ||
            !OC::permitCurrent(state.ordinaryControl, data->ordinaryPermit, now) ||
            !FastBee::SensorConfigEpoch::matches(data->ordinaryConfigEpoch) ||
            (data->ordinaryRuleEpoch && !FastBee::RuleOutputIdentity::matches(data->ordinaryRuleEpoch)) ||
            !isUartEventOriginCurrent(data->uartOrigin) || !config->second.enabled ||
            state.memorySuspended || state.memoryCleanupPending || controlOutputIsClaimed(item.first) ||
            (!digitalProducer && !scalarProducer)) {
            retireOrdinaryDigitalLocked(item.first, _nextOrdinaryControlOperation,
                false, state.ordinaryControl.invocationActive);
            return;
        }
        bool ok = false;
        if (digitalProducer) {
            const GPIOState current = readGpioStateLocked(config->second, &item.first, nullptr);
            ok = current != GPIOState::STATE_UNDEFINED &&
                writeGpioStateLocked(config->second, &item.first, nullptr,
                    current == GPIOState::STATE_HIGH ? GPIOState::STATE_LOW : GPIOState::STATE_HIGH,
                    &data->ordinaryPermit);
        } else if (scalarBlink) {
            const auto level = state.state.gpio.currentState;
            if (level == GPIOState::STATE_HIGH || level == GPIOState::STATE_LOW)
                ok = writeOrdinaryScalarLocked(item.first, FastBee::ScalarOutputCommand::Pwm,
                    level == GPIOState::STATE_HIGH ? 0 : data->maxDuty, data->ordinaryPermit);
        } else {
            const uint32_t remaining = data->increasing ? data->maxDuty - data->duty : data->duty;
            const uint32_t step = data->stepSize < remaining ? data->stepSize : remaining;
            const uint32_t next = data->increasing ? data->duty + step : data->duty - step;
            ok = writeOrdinaryScalarLocked(item.first, FastBee::ScalarOutputCommand::Pwm,
                                            next, data->ordinaryPermit);
            if (ok) {
                data->duty = next;
                if (next == data->maxDuty) data->increasing = false;
                else if (!next) data->increasing = true;
            }
        }
        if (!ok) {
            state.lastError = "CONTROL_TICK_UNCONFIRMED";
            state.ordinaryControl.phase = OC::Phase::Fault;
            retireOrdinaryDigitalLocked(item.first, _nextOrdinaryControlOperation,
                false, state.ordinaryControl.invocationActive);
        }
        return;
    }
}

void PeripheralManager::serviceOrdinaryControlExpiry() {
    RecursiveMutexGuard lock(_mutex, 0);
    uint64_t now = 0;
    if (!lock.isLocked()) return;
    serviceOrdinaryModbusLocked();
    if (!ordinaryTime(now)) {
        for (auto& item : runtimeStates) {
            if (item.second.ordinaryControl.phase == OC::Phase::Idle) continue;
            item.second.ordinaryControl.phase = OC::Phase::Fault;
            item.second.lastError = "CONTROL_CLOCK_UNCONFIRMED";
        }
        return;
    }
    for (auto& item : runtimeStates) {
        const auto& lease = item.second.ordinaryControl;
        if (lease.phase != OC::Phase::Idle && (!ordinaryActorCurrent(lease.actor) ||
            OC::expired(lease, now) || lease.phase == OC::Phase::Retiring || lease.phase == OC::Phase::Fault))
            retireOrdinaryDigitalLocked(item.first, _nextOrdinaryControlOperation, false, lease.invocationActive);
    }
}

