#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/Pca9685ConfigSupport.h"
#include "core/RuntimeFeatureManager.h"
#include "core/FeatureBudget.h"
#include "peripherals/PCA9685Provider.h"
#include "peripherals/drivers/H1ControlOutputs.h"
#include <esp_timer.h>
#include <esp_system.h>
#include <memory>
#include <new>
#include <cmath>
#include <atomic>

namespace {
using Provider = FastBee::PCA9685Provider;
// One bounded production slot. Every access is under PeripheralManager::_mutex.
struct PwmSlot {
    String id;
    std::unique_ptr<FastBee::PCA9685NativeGate> gate;
    std::unique_ptr<Provider> provider;
    char token[33] = {};
    uint64_t reviewVersion = 0, expiresMs = 0;
    uint32_t failures = 0;
    uint32_t providerFailures = 0;
    unsigned providerError = 0, providerFirstFailure = 0, sessionFailure = 0;
    bool closed = false;
    String firstError, lastError;
};
PwmSlot& pwmSlot() { static PwmSlot slot; return slot; }
std::atomic<bool>& pwmShutdownRequested() { static std::atomic<bool> requested{false}; return requested; }
uint64_t pwmNowMs() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }
bool pwmAvailable() {
    return !pwmShutdownRequested().load(std::memory_order_relaxed) && FASTBEE_ENABLE_IO_EXPANDER &&
        FastBee::RuntimeFeatureManager::instance().shouldEnable("ioExpander") &&
        !FastBee::FeatureBudgetRegistry::instance().isDenied("ioExpander");
}
bool pwmFailure(PwmSlot& slot, const char* reason, String& error) {
    error = reason; slot.lastError = reason;
    if (slot.firstError.isEmpty()) slot.firstError = reason;
    if (slot.failures != UINT32_MAX) ++slot.failures;
    return false;
}
const char* pwmState(const PwmSlot& slot) {
    if (slot.gate) return slot.token[0] ? "awaiting_power_cycle" : "releasing_oe";
    if (!slot.provider) return slot.closed ? "closed" : "awaiting_review";
    switch (slot.provider->state()) {
    case Provider::State::Waking: return "waking";
    case Provider::State::ReadyDisabled: return "ready_disabled";
    case Provider::State::Enabled: return "enabled";
    case Provider::State::Faulted: return "faulted";
    case Provider::State::Closing: return "closing";
    case Provider::State::Closed: return "closed";
    default: return "not_ready";
    }
}
}

FastBee::FeatureInstanceSnapshot FastBee::pca9685InstanceSnapshotLocked() {
    const auto& slot = pwmSlot();
    if (slot.gate) return {slot.token[0] ? FeatureInstanceState::Busy : FeatureInstanceState::CleanupPending, 0};
    if (!slot.provider) return {FeatureInstanceState::NotInitialized, 0};
    if (slot.provider->ready()) return {FeatureInstanceState::Initialized, 1};
    return {slot.provider->state() == Provider::State::Waking ? FeatureInstanceState::Busy : FeatureInstanceState::CleanupPending, 0};
}

bool PeripheralManager::validatePca9685Dependencies(const PeripheralConfig& config, String& error) const {
    if (!FastBee::validatePca9685Config(config, error)) return false;
    const auto& p = config.params.pca9685;
    const auto bus = peripherals.find(p.busPeripheralId), oe = peripherals.find(p.oePeripheralId);
    if (bus == peripherals.end() || bus->second.type != PeripheralType::I2C ||
        !bus->second.params.i2c.isMaster || bus->second.params.i2c.address || bus->second.pinCount != 2 ||
        (bus->second.params.i2c.frequency != 100000 && bus->second.params.i2c.frequency != 400000) ||
        oe == peripherals.end() || oe->second.type != PeripheralType::GPIO_DIGITAL_OUTPUT ||
        oe->second.isExpanderChannel() || oe->second.pinCount != 1 || oe->second.params.gpio.passive ||
        oe->second.params.gpio.invertLogic || oe->second.params.gpio.initialState != GPIOState::STATE_HIGH ||
        oe->second.pins[0] == bus->second.pins[0] || oe->second.pins[0] == bus->second.pins[1] ||
        !isValidPin(oe->second.pins[0]) || isReservedPin(oe->second.pins[0]) || isInputOnlyPin(oe->second.pins[0]) ||
        (config.enabled && (!bus->second.enabled || !oe->second.enabled))) {
        error = "PCA_DEPENDENCY_INVALID: master I2C and dedicated inactive-HIGH OE required"; return false;
    }
    return true;
}

bool PeripheralManager::pca9685Owns(const String& id) const {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) return true;
    const auto& slot = pwmSlot();
    return slot.id == id && (slot.gate || (slot.provider && slot.provider->state() != Provider::State::Closed));
}

bool PeripheralManager::preparePca9685(const String& id, JsonObject review, String& error) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) { error = "PCA_BUSY"; return false; }
    auto& slot = pwmSlot();
    auto config = peripherals.find(id);
    if (!pwmAvailable() || config == peripherals.end() ||
        config->second.type != PeripheralType::PCA9685_PWM || !config->second.enabled ||
        !validateIoDependencies(config->second, id, error)) {
        if (error.isEmpty()) error = "PCA_NOT_READY";
        return false;
    }
    if (slot.gate || (slot.provider && slot.provider->state() != Provider::State::Closed)) {
        error = "PCA_STOP_REQUIRED"; return false;
    }
    const auto state = runtimeStates.find(id);
    if (state != runtimeStates.end() &&
        state->second.ordinaryControl.phase != FastBee::OrdinaryControl::Phase::Idle) {
        error = "CONTROL_STOP_UNCONFIRMED"; return false;
    }
    if (state == runtimeStates.end() || state->second.memorySuspended ||
        state->second.lastError.startsWith("CONFIG_INVALID:")) { error = "PCA_NOT_READY"; return false; }
    uint8_t sda, scl; uint32_t frequency;
    if (!resolveI2cConfig(config->second, sda, scl, frequency, error)) return false;
    const String oeId(config->second.params.pca9685.oePeripheralId);
    const auto oeState = runtimeStates.find(oeId);
    if (oeState == runtimeStates.end() || oeState->second.memorySuspended || oeState->second.controlOutputFault ||
        oeState->second.controlOutputRetiring || oeState->second.status == PeripheralStatus::PERIPHERAL_ERROR ||
        oeState->second.lastError.startsWith("CONFIG_INVALID:")) { error = "PCA_OE_NOT_READY"; return false; }
    const bool sameIdentity = slot.id == id;
    slot.id = id;
    if (slot.id.length() != id.length()) { error = "PCA_MEMORY_UNAVAILABLE"; return false; }
    if (!sameIdentity) {
        slot.failures = 0; slot.firstError = ""; slot.lastError = "";
        slot.providerError = slot.providerFirstFailure = slot.providerFailures = slot.sessionFailure = 0;
    }
    slot.provider.reset();
    slot.closed = false;
    if (oeState->second.status != PeripheralStatus::PERIPHERAL_INITIALIZED &&
        oeState->second.status != PeripheralStatus::PERIPHERAL_RUNNING && !initHardware(oeId)) {
        error = "PCA_OE_NOT_READY"; return false;
    }
    slot.gate.reset(new (std::nothrow) FastBee::PCA9685NativeGate);
    if (!slot.gate) { error = "PCA_MEMORY_UNAVAILABLE"; return false; }
    if (!slot.gate->begin(oeId, id) || !slot.gate->disable() || !slot.gate->disabled()) {
        slot.token[0] = 0;
        return pwmFailure(slot, "PCA_OE_UNCONFIRMED", error);
    }
    slot.reviewVersion = getConfigurationReviewVersion();
    if (!slot.reviewVersion) return pwmFailure(slot, "PCA_REVIEW_UNAVAILABLE", error);
    snprintf(slot.token, sizeof(slot.token), "%08lx%08lx%08lx%08lx",
        static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()),
        static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
    slot.expiresMs = pwmNowMs() + 60000;
    review["id"] = id; review["token"] = slot.token; review["validForMs"] = 60000;
    review["oePadHigh"] = true; review["physicalPowerVerified"] = false;
    review["outputsEnabled"] = false;
    review["sda"] = sda; review["scl"] = scl; review["i2cFrequency"] = frequency;
    FastBee::serializePca9685Config(review["params"].to<JsonObject>(), config->second);
    return true;
}

bool PeripheralManager::startPca9685(const String& id, const String& token, bool confirmed, String& error) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) { error = "PCA_BUSY"; return false; }
    auto& slot = pwmSlot();
    if (slot.id != id || !slot.gate || !slot.token[0] || !confirmed || token != slot.token ||
        pwmNowMs() >= slot.expiresMs || !slot.reviewVersion ||
        getConfigurationReviewVersion() != slot.reviewVersion) { error = "PCA_REVIEW_INVALID"; return false; }
    slot.token[0] = 0; // One attempt only, including failures after this point.
    const auto config = peripherals.find(id);
    const auto state = runtimeStates.find(id);
    if (!pwmAvailable() || config == peripherals.end() ||
        config->second.type != PeripheralType::PCA9685_PWM || !config->second.enabled ||
        state == runtimeStates.end() || state->second.memorySuspended ||
        state->second.lastError.startsWith("CONFIG_INVALID:") ||
        !validateIoDependencies(config->second, id, error)) return pwmFailure(slot, "PCA_NOT_READY", error);
    uint8_t sda, scl; uint32_t frequency;
    if (!resolveI2cConfig(config->second, sda, scl, frequency, error)) return false;
    const auto& p = config->second.params.pca9685;
    slot.provider.reset(new (std::nothrow) Provider(sda, scl, frequency, p.address, std::move(slot.gate)));
    if (!slot.provider) return pwmFailure(slot, "PCA_MEMORY_UNAVAILABLE", error);
    slot.providerError = slot.providerFirstFailure = slot.providerFailures = slot.sessionFailure = 0;
    if (!slot.provider->begin(FastBee::PCA9685OutputSession::ResetEvidence::ConfirmedPowerOn,
                             p.pwmHz, p.clockHz, p.channelMask, p.openDrain))
        return pwmFailure(slot, "PCA_BEGIN_FAILED", error);
    slot.lastError = "";
    return true; // Waking, not ready and never OE-enabled.
}

bool PeripheralManager::shutdownPca9685() {
    pwmShutdownRequested().store(true, std::memory_order_relaxed);
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) return false;
    return stopPca9685(pwmSlot().id);
}

bool PeripheralManager::stopPca9685(const String& id) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) return false;
    auto& slot = pwmSlot();
    if (slot.id != id) return true;
    // Revoke all already-issued operations before touching OE or the bus.
    // An active invocation still owes its terminal acknowledgement.
    invalidateOrdinaryPca9685Locked(id);
    slot.token[0] = 0;
    if (slot.gate) {
        if (!slot.gate->release()) {
            if (slot.lastError != "PCA_OE_RELEASE_UNCONFIRMED") {
                String ignored; pwmFailure(slot, "PCA_OE_RELEASE_UNCONFIRMED", ignored);
            }
            return false;
        }
        slot.gate.reset();
        slot.closed = true;
    }
    if (!slot.provider) {
        if (runtimeStates.find(id) != runtimeStates.end())
            retireOrdinaryDigitalLocked(id, _nextOrdinaryControlOperation, false, true);
        return true;
    }
    const auto result = slot.provider->shutdown();
    if (result != Provider::StopResult::Ok && slot.lastError != "PCA_STOP_UNCONFIRMED") {
        String ignored; pwmFailure(slot, "PCA_STOP_UNCONFIRMED", ignored);
    }
    // Closed means leases retired, even when an earlier I/O failed. That
    // failure stays in diagnostics; it does not promise physical load-off.
    if (slot.provider->state() != Provider::State::Closed) return false;
    slot.providerError = static_cast<unsigned>(slot.provider->error());
    slot.providerFirstFailure = static_cast<unsigned>(slot.provider->firstFailure());
    slot.providerFailures = slot.provider->failures();
    slot.sessionFailure = static_cast<unsigned>(slot.provider->sessionFailure());
    slot.provider.reset(); // Free device/session/gate allocations after leases retire.
    slot.closed = true;
    if (runtimeStates.find(id) != runtimeStates.end())
        retireOrdinaryDigitalLocked(id, _nextOrdinaryControlOperation, false, true);
    return true;
}

void PeripheralManager::servicePca9685() {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) return;
    auto& slot = pwmSlot();
    if (!slot.gate && !slot.provider) return;
    const auto config = peripherals.find(slot.id);
    const auto state = runtimeStates.find(slot.id);
    const bool changed = getConfigurationReviewVersion() != slot.reviewVersion;
    if (!pwmAvailable() || config == peripherals.end() ||
        config->second.type != PeripheralType::PCA9685_PWM || !config->second.enabled || changed ||
        state == runtimeStates.end() || state->second.memorySuspended ||
        (slot.gate && (!slot.token[0] || pwmNowMs() >= slot.expiresMs)) ||
        (slot.provider && slot.provider->state() == Provider::State::Closing)) {
        stopPca9685(slot.id); return;
    }
    if (slot.provider) {
        const bool waking = slot.provider->state() == Provider::State::Waking;
        // Ready sessions still need the provider's local OE ownership/pad
        // check. This does not poll chip registers or certify physical loads.
        if ((waking || slot.provider->ready()) && !slot.provider->service()) {
            if (slot.provider->error() == Provider::Error::Busy) return;
            String ignored; pwmFailure(slot, waking ? "PCA_WAKE_FAILED" : "PCA_CONTROL_LOST", ignored);
        }
    }
}

void PeripheralManager::invalidateOrdinaryPca9685Locked(const String& id) {
    const auto runtime = runtimeStates.find(id);
    if (runtime == runtimeStates.end()) return;
    auto& lease = runtime->second.ordinaryControl;
    if (lease.retiredThrough < _nextOrdinaryControlOperation)
        lease.retiredThrough = _nextOrdinaryControlOperation;
    if (lease.phase != FastBee::OrdinaryControl::Phase::Idle)
        lease.phase = FastBee::OrdinaryControl::Phase::Retiring;
}

bool PeripheralManager::ordinaryPca9685QuiescentLocked(const String& id) const {
    const auto& slot = pwmSlot();
    if (slot.id != id) return true;
    if (slot.gate) return false; // Unfinished power-on/OE review is not a stopped producer.
    if (!slot.provider) return true;
    // PM serialization proves the synchronous register operation returned.
    // Preserve the last PWM values/OE; no guessed zero-duty or power action.
    // Ready service only checks gate ownership; it cannot replay a command.
    return slot.provider->ready() || slot.provider->state() == Provider::State::Closed;
}

bool PeripheralManager::commandPca9685(const String& id, const String& command,
    uint32_t channel, double value, String& error,
    const FastBee::OrdinaryControlContext* context, FastBee::OrdinaryControlResult* output) {
    namespace OC = FastBee::OrdinaryControl;
    FastBee::OrdinaryControlResult result;
    result.evaluated = true;
    if (id.length() <= 40 && id.length() == strlen(id.c_str()))
        memcpy(result.targetId, id.c_str(), id.length() + 1);
    auto publish = [&]() {
        if (!result.accepted && error.isEmpty()) error = result.reason;
        if (output && (!output->evaluated || output->accepted)) *output = result;
        return result.accepted;
    };
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) { error = result.reason = "PCA_BUSY"; return publish(); }
    auto& slot = pwmSlot();
    if (command == "stop") {
        result.attempted = true;
        result.accepted = stopPca9685(id);
        result.cleanupConfirmed = result.accepted;
        result.reason = result.accepted ? "PCA_RESOURCES_RETIRED_OUTPUT_UNVERIFIED" : "PCA_STOP_UNCONFIRMED";
        return publish();
    }
    if (slot.id != id || !slot.provider) { error = result.reason = "PCA_NOT_STARTED"; return publish(); }
    // Emergency gating remains available after config/source changes.
    if (command == "disable") {
        invalidateOrdinaryPca9685Locked(id);
        result.attempted = true;
        result.accepted = slot.provider->disableOutputs();
        result.reason = result.accepted ? "PCA_OE_DISABLED_OUTPUT_UNVERIFIED" : "PCA_DISABLE_UNCONFIRMED";
        if (result.accepted) {
            if (runtimeStates.find(id) != runtimeStates.end())
                result.cleanupConfirmed = retireOrdinaryDigitalLocked(id, _nextOrdinaryControlOperation, false, true);
        } else {
            stopPca9685(id); pwmFailure(slot, "PCA_DISABLE_UNCONFIRMED", error);
        }
        return publish();
    }
    if (!context) { result.reason = "CONTROL_CONTEXT_REQUIRED"; return publish(); }
    const auto config = peripherals.find(id);
    const auto state = runtimeStates.find(id);
    if (!pwmAvailable() || config == peripherals.end() ||
        config->second.type != PeripheralType::PCA9685_PWM || !config->second.enabled ||
        state == runtimeStates.end() || state->second.memorySuspended ||
        getConfigurationReviewVersion() != slot.reviewVersion) {
        stopPca9685(id); pwmFailure(slot, "PCA_CONFIG_CHANGED", error);
        result.reason = "PCA_CONFIG_CHANGED"; return publish();
    }
    if (!slot.provider->ready()) { error = result.reason = "PCA_NOT_READY"; return publish(); }
    const auto& p = config->second.params.pca9685;
    uint16_t ticks = 0;
    if (command != "enable") {
        const bool channelValid = std::isfinite(value) && channel < 16 &&
            (p.channelMask & (uint16_t(1) << channel));
        const bool valueValid = command == "duty" ? FastBee::makePCA9685DutyTicks(value, ticks) :
            command == "pulse" ? FastBee::makePCA9685PulseTicks(value, slot.provider->calculatedFrequencyHz(), ticks) :
            command == "angle" && FastBee::makePCA9685ServoTicks(value, p.minAngle, p.maxAngle,
                p.minPulseUs, p.maxPulseUs, slot.provider->calculatedFrequencyHz(), ticks);
        if (!channelValid || !valueValid) {
            result.reason = "PCA_CHANNEL_VALUE_INVALID"; return publish();
        }
    }
    uint64_t now = 0;
    if (!inspectOrdinaryControlLocked(id, *context, OC::Intent::Write, result, now)) return publish();
    OC::Permit permit;
    if (!commitOrdinaryControlLocked(id, *context, OC::Intent::Write, result, now, false, permit)) return publish();
    if (!ordinaryPermitCurrentLocked(id, permit)) {
        auto& runtime = runtimeStates.find(id)->second;
        runtime.ordinaryControl.phase = OC::Phase::Fault;
        runtime.lastError = result.reason = "CONTROL_PERMIT_STALE";
        updateOrdinaryControlResultLocked(id, now, result);
        return publish();
    }
    // The whole PCA device is one target: channels share frequency and OE.
    const bool success = command == "enable" ? slot.provider->enableOutputs() :
        slot.provider->setTicks(channel, 0, ticks);
    if (!success) pwmFailure(slot, "PCA_COMMAND_FAILED", error);
    else slot.lastError = "";
    completeOrdinaryControlLocked(id, now, result, success, "PCA_COMMAND_FAILED");
    if (success) result.reason = "PCA_COMMAND_ACKNOWLEDGED_OUTPUT_UNVERIFIED";
    return publish();
}

bool PeripheralManager::getPca9685Status(const String& id, JsonObject status) const {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) return false;
    const auto config = peripherals.find(id);
    if (config == peripherals.end() || config->second.type != PeripheralType::PCA9685_PWM) return false;
    const auto& slot = pwmSlot();
    const bool ours = slot.id == id;
    status["state"] = ours ? pwmState(slot) : "awaiting_review";
    status["outputsEnabled"] = ours && slot.provider && slot.provider->state() == Provider::State::Enabled;
    status["requiresFreshPowerCycle"] = !ours || !slot.provider || slot.provider->state() == Provider::State::Closed;
    status["physicalOutputVerified"] = false;
    status["failures"] = ours ? slot.failures : 0;
    status["firstError"] = ours ? slot.firstError : String();
    status["lastError"] = ours ? slot.lastError : String();
    status["providerError"] = ours ? slot.provider ? static_cast<unsigned>(slot.provider->error()) : slot.providerError : 0;
    status["providerFirstFailure"] = ours ? slot.provider ? static_cast<unsigned>(slot.provider->firstFailure()) : slot.providerFirstFailure : 0;
    status["providerFailures"] = ours ? slot.provider ? slot.provider->failures() : slot.providerFailures : 0;
    status["sessionFailure"] = ours ? slot.provider ? static_cast<unsigned>(slot.provider->sessionFailure()) : slot.sessionFailure : 0;
    status["allocatedProviders"] = ours && slot.provider ? 1 : 0;
    status["oeLeasePending"] = ours && static_cast<bool>(slot.gate);
    status["memoryMeasured"] = false;
    status["frequencyCalculatedHz"] = ours && slot.provider ? slot.provider->calculatedFrequencyHz() : 0;
    status["channelMask"] = config->second.params.pca9685.channelMask;
    return true;
}
