#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/NativeOutputDispatch.h"
#include "core/RfOutputContract.h"
#include "core/SensorConfigEpoch.h"
#include "core/RuleOutputIdentity.h"
#include "core/CheckedAdc.h"
#include <esp_timer.h>
#include <cstring>

namespace {
bool rfTransmitTarget(const PeripheralConfig& config) {
    return config.type == PeripheralType::RF_MODULE && config.params.rf.mode == RF_MODE_TX &&
        config.pinCount == 1 && !config.isExpanderChannel() && !config.isModbusPeripheral();
}
bool rfLeaseMatches(const FastBee::GpioResourceSnapshot::Lease& lease, const PeripheralConfig& config) {
    return lease.pin == config.getPrimaryPin() && !lease.commandScript &&
        lease.mode == FastBee::ManagedGpioMode::Output &&
        lease.consumer.role == FastBee::ResourceConsumerRole::Peripheral &&
        std::strcmp(lease.consumer.id, config.id.c_str()) == 0;
}
}
bool PeripheralManager::setupRfTransmit(const PeripheralConfig& config) {
    RecursiveMutexGuard lock(_mutex, 0);
    const uint8_t pin = config.getPrimaryPin();
    if (!lock.isLocked() || !rfTransmitTarget(config) || !isValidPin(pin) || isReservedPin(pin) ||
        isInputOnlyPin(pin) || controlOutputPinIsClaimed(pin) || checkPinConflict(pin, config.id) ||
        ordinaryDigitalClaimedLocked(config.id) || FastBee::adcPinUnavailable(pin)) return false;
    FastBee::GpioResourceSnapshot::Lease previous;
    if (_managedPinIo.observeGpio(pin, previous)) {
        if (!rfLeaseMatches(previous, config) || previous.fault || previous.cleanupPending) return false;
    } else if (!managedPinAvailableForAdc(pin)) return false;
    const bool ok = _managedPinIo.configure(config.id.c_str(), false, pin,
        FastBee::ManagedGpioMode::Output, !config.params.rf.activeHigh);
    const auto state = runtimeStates.find(config.id);
    if (state != runtimeStates.end() && !ok) state->second.lastError = "RF_SETUP_UNCONFIRMED";
    return ok;
}
bool PeripheralManager::releaseRfTransmit(const PeripheralConfig& config) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || !rfTransmitTarget(config) || controlOutputIsClaimed(config.id) ||
        ordinaryDigitalClaimedLocked(config.id)) return false;
    const bool ok = _managedPinIo.release(config.id.c_str(), false, config.getPrimaryPin(),
                                          !config.params.rf.activeHigh);
    const auto state = runtimeStates.find(config.id);
    if (state != runtimeStates.end() && !ok) state->second.lastError = "RF_RELEASE_UNCONFIRMED";
    return ok;
}
bool PeripheralManager::retireOrdinaryRfLocked(const String& id, bool* attempted) {
    const auto config = peripherals.find(id);
    if (config == peripherals.end() || config->second.type != PeripheralType::RF_MODULE) return true;
    if (!rfTransmitTarget(config->second)) return false;
    FastBee::GpioResourceSnapshot::Lease retained;
    if (!_managedPinIo.observeGpio(config->second.getPrimaryPin(), retained)) return true;
    if (!rfLeaseMatches(retained, config->second)) return false;
    // Synchronous transmission has returned under the same PM lock. A fault
    // only retries inactive/release, never a code/frame. Keep SDK failures.
    if (!retained.fault && !retained.cleanupPending) return true;
    if (attempted) *attempted = true;
    return _managedPinIo.release(id.c_str(), false, retained.pin, !config->second.params.rf.activeHigh);
}
bool PeripheralManager::ordinaryRfQuiescentLocked(const String& id) const {
    const auto config = peripherals.find(id);
    if (config == peripherals.end() || !rfTransmitTarget(config->second)) return false;
    FastBee::GpioResourceSnapshot::Lease retained;
    if (!_managedPinIo.observeGpio(config->second.getPrimaryPin(), retained)) return true;
    return rfLeaseMatches(retained, config->second) && !retained.fault && !retained.cleanupPending;
}
bool PeripheralManager::sendRfCode(const String& id, const String& codeText,
    uint32_t bitLength, uint32_t pulseWidth, uint32_t repeat,
    const FastBee::OrdinaryControlContext* context, FastBee::OrdinaryControlResult* aggregate) {
    namespace OC = FastBee::OrdinaryControl;
    FastBee::OrdinaryControlResult result;
    result.evaluated = true;
    if (id.length() <= 40 && id.length() == std::strlen(id.c_str()))
        std::memcpy(result.targetId, id.c_str(), id.length() + 1);
    auto reject = [&](const char* reason) {
        result.reason = reason; FastBee::rememberOrdinaryControlResult(result, aggregate); return false;
    };
    if (!context) return reject("CONTROL_CONTEXT_REQUIRED");
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) return reject("CONTROL_BUSY");
    const auto config = peripherals.find(id);
    const auto runtime = runtimeStates.find(id);
    if (config == peripherals.end() || runtime == runtimeStates.end() || !rfTransmitTarget(config->second))
        return reject("CONTROL_TARGET_UNSUPPORTED");
    const auto& cfg = config->second;
    FastBee::RfOutputFrame frame;
    if (!FastBee::resolveRfOutputFrame(codeText.c_str(), codeText.length(), bitLength, pulseWidth, repeat,
        cfg.params.rf.bitLength, cfg.params.rf.pulseWidth, cfg.params.rf.repeat, frame))
        return reject("RF_PARAMETERS_INVALID");
    FastBee::GpioResourceSnapshot::Lease pad;
    if (!_managedPinIo.observeGpio(cfg.getPrimaryPin(), pad) || !rfLeaseMatches(pad, cfg) ||
        pad.fault || pad.cleanupPending) return reject("RF_TX_NOT_READY");
    uint64_t now = 0; OC::Permit permit;
    if (!commitOrdinaryControlLocked(id, *context, OC::Intent::Write, result, now, false, permit)) {
        FastBee::rememberOrdinaryControlResult(result, aggregate); return false;
    }
    const auto current = [&]() {
        return ordinaryPermitCurrentLocked(id, permit) && FastBee::SensorConfigEpoch::matches(context->configEpoch) &&
            (!context->ruleBound || FastBee::RuleOutputIdentity::matches(context->ruleEpoch)) &&
            isUartEventOriginCurrent(context->uartOrigin) && cfg.enabled &&
            !runtime->second.memorySuspended && !runtime->second.memoryCleanupPending && !controlOutputIsClaimed(id);
    };
    const uint8_t pin = cfg.getPrimaryPin();
    const auto deadlineReached = [&]() {
        return context->operationDeadlineAt &&
            static_cast<int32_t>(static_cast<uint32_t>(millis()) - context->operationDeadlineAt) >= 0;
    };
    const auto pulse = [&](uint8_t active, uint8_t inactive) {
        result.attempted = true; // Before the first SDK call, including partial side effects on error.
        if (!_managedPinIo.write(id.c_str(), false, pin, cfg.params.rf.activeHigh)) return false;
        delayMicroseconds(uint32_t(frame.pulseUs) * active);
        // Deassert this pulse even if its authority elapsed during its active
        // half. A later frame cannot start without a fresh boundary check.
        if (!_managedPinIo.write(id.c_str(), false, pin, !cfg.params.rf.activeHigh)) return false;
        delayMicroseconds(uint32_t(frame.pulseUs) * inactive);
        return true;
    };
    const char* failure = "RF_TX_WRITE_UNCONFIRMED";
    bool ok = true;
    for (uint8_t repetition = 0; repetition < frame.repeat && ok; ++repetition) {
        if (deadlineReached()) { failure = "RF_OPERATION_DEADLINE_EXCEEDED"; ok = false; break; }
        if (!current()) { failure = "RF_SOURCE_UNCONFIRMED"; ok = false; break; }
        ok = pulse(1, 31);
        for (int bit = frame.bits - 1; bit >= 0 && ok; --bit)
            ok = (frame.code >> bit) & 1U ? pulse(3, 1) : pulse(1, 3);
        if (ok) { delay(1); yield(); }
    }
    if (ok && deadlineReached()) { failure = "RF_OPERATION_DEADLINE_EXCEEDED"; ok = false; }
    if (ok && !current()) { failure = "RF_SOURCE_UNCONFIRMED"; ok = false; }
    const int64_t endUs = esp_timer_get_time();
    if (endUs >= 0) now = static_cast<uint64_t>(endUs) / 1000ULL;
    if (ok) {
        runtime->second.lastActivity = millis();
        auto& sent = runtime->second.state.comm.bytesSent;
        sent = sent > UINT32_MAX - 4U ? UINT32_MAX : sent + 4U;
    }
    if (result.attempted) completeOrdinaryControlLocked(id, now, result, ok, failure);
    else {
        // Admission can lose its clock/source before the first pulse. Retain
        // the rejected invocation until finish, but do not claim an SDK try.
        result.accepted = false; result.reason = failure;
        runtime->second.ordinaryControl.phase = OC::Phase::Fault;
        runtime->second.lastError = failure;
        updateOrdinaryControlResultLocked(id, now, result);
    }
    FastBee::rememberOrdinaryControlResult(result, aggregate);
    return ok; // Only local GPIO SDK acknowledgement, never remote RF load identity/feedback.
}
