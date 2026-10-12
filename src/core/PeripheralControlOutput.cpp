#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include <driver/gpio.h>
#include <cstring>

void PeripheralManager::snapshotControlOutputResources(
        FastBee::ControlOutputResourceSnapshot& result) const {
    result = FastBee::ControlOutputResourceSnapshot{};
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) { result.status = FastBee::ResourceObservation::Busy; return; }
    static_assert(sizeof(_controlOutputClaims) / sizeof(_controlOutputClaims[0]) ==
        FastBee::ControlOutputResourceSnapshot::CAPACITY, "snapshot capacity");
    for (size_t i = 0; i < FastBee::ControlOutputResourceSnapshot::CAPACITY; ++i) {
        const auto& claim = _controlOutputClaims[i];
        if (!claim.owner) continue;
        auto& lease = result.leases[result.count++];
        lease.slot = static_cast<uint8_t>(i);
        lease.pin = claim.pin;
        std::memcpy(lease.id, claim.id, sizeof(lease.id));
        lease.id[sizeof(lease.id) - 1] = '\0';
        lease.fault = claim.fault;
        lease.retiring = claim.retirement.load(std::memory_order_acquire);
        lease.retirementAttempted = claim.retirementAttempted;
        lease.consumer = claim.consumer;
    }
}

bool PeripheralManager::controlOutputIsClaimed(const String& id) const {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) return true; // Unknown cannot authorize a mutation.
    for (const auto& claim : _controlOutputClaims)
        if (claim.owner && (id.isEmpty() || id == claim.id)) return true;
    return false;
}

bool PeripheralManager::controlOutputPinIsClaimed(uint8_t pin) const {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) return true;
    for (const auto& claim : _controlOutputClaims)
        if (claim.owner && claim.pin == pin) return true;
    return false;
}

bool PeripheralManager::controlOutput(const String& id, const void* owner,
                                      FastBee::ControlOutputCommand command, int& pin,
                                      std::atomic<bool>** retirement,
                                      const FastBee::ResourceConsumerIdentity* consumer) {
    using Command = FastBee::ControlOutputCommand;
    if (static_cast<uint8_t>(command) > static_cast<uint8_t>(Command::Release)) return false;
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || !owner || id.isEmpty() || id.length() > 40 ||
        id.length() != std::strlen(id.c_str())) return false;
    auto config = peripherals.find(id);
    auto runtime = runtimeStates.find(id);
    if (config == peripherals.end() || runtime == runtimeStates.end()) return false;
    const auto& cfg = config->second;
    // Reuse a normal native GPIO resource claim, with a deliberately narrow
    // inactive-high contract. No native pin is guessed from a bus/expander ID.
    if (cfg.type != PeripheralType::GPIO_DIGITAL_OUTPUT || !cfg.enabled || cfg.isExpanderChannel() ||
        cfg.pinCount != 1 || cfg.params.gpio.passive || cfg.params.gpio.invertLogic ||
        cfg.params.gpio.initialState != GPIOState::STATE_HIGH || runtime->second.memorySuspended ||
        (runtime->second.status != PeripheralStatus::PERIPHERAL_INITIALIZED &&
         runtime->second.status != PeripheralStatus::PERIPHERAL_RUNNING) ||
        !isValidPin(cfg.pins[0]) || isReservedPin(cfg.pins[0]) || isInputOnlyPin(cfg.pins[0]) ||
        checkPinConflict(cfg.pins[0], id)) return false;
    ControlOutputClaim* slot = nullptr;
    ControlOutputClaim* freeSlot = nullptr;
    for (auto& claim : _controlOutputClaims) {
        if (!claim.owner) { if (!freeSlot) freeSlot = &claim; continue; }
        if (id == claim.id || claim.pin == cfg.pins[0]) slot = &claim;
    }
    if (command == Command::Claim) {
        if (slot || !freeSlot || ordinaryDigitalClaimedLocked(id) ||
            actionTickers.find(id) != actionTickers.end()) return false;
        // Check before reservation, GPIO or handle publication. The permanent
        // manager normally lives in BSS; never assume an arbitrary address is
        // safe for destructor retirement when PSRAM atomics may wait forever.
        if (!FastBee::atomicStorageDoesNotWait(freeSlot->retirement)) return false;
        FastBee::ResourceConsumerIdentity identity;
        if (consumer && !identity.assign(consumer->role, consumer->id)) return false;
        slot = freeSlot;
        slot->owner = owner;
        slot->consumer = identity;
        std::memcpy(slot->id, id.c_str(), id.length() + 1);
        slot->pin = cfg.pins[0];
        slot->fault = true; // Keep the reservation even if GPIO setup fails.
        slot->retirement.store(false, std::memory_order_relaxed);
        slot->retirementAttempted = false;
        slot->lastRetirementAttempt = 0;
        if (retirement) *retirement = &slot->retirement;
        runtime->second.controlOutputClaimed = true;
        runtime->second.controlOutputFault = true;
        runtime->second.controlOutputRetiring = false;
        pin = slot->pin;
        const auto gpio = static_cast<gpio_num_t>(slot->pin);
        // Preload inactive level, then enable pad input for actual local
        // feedback. This is not a downstream RESET/OE wiring/load measurement.
        if (gpio_set_level(gpio, 1) != ESP_OK ||
            gpio_set_direction(gpio, GPIO_MODE_INPUT_OUTPUT) != ESP_OK) {
            runtime->second.state.gpio.currentState = GPIOState::STATE_UNDEFINED;
            runtime->second.lastError = "CONTROL_OUTPUT_INIT_UNCONFIRMED";
            return false;
        }
    } else if (!slot || slot->owner != owner || slot->pin != cfg.pins[0]) return false;
    // A destroyed owner cannot be resurrected, even if a new object receives
    // the same address. Retirement may only confirm inactive HIGH and release.
    if (slot->retirement.load(std::memory_order_acquire) && command != Command::Release) return false;
    pin = slot->pin;
    const auto gpio = static_cast<gpio_num_t>(pin);
    if (command == Command::Owned) return !slot->fault;
    if (command == Command::IsHigh) return !slot->fault && gpio_get_level(gpio) == 1;
    if ((command == Command::Low || command == Command::PulseReset) && slot->fault) return false;
    bool ok = true;
    bool high = command != Command::Low;
    if (command == Command::PulseReset) {
        // Fixed bounded low pulse; never retry a reset. Return HIGH even when
        // the LOW command/readback fails, but retain that original failure.
        if (gpio_get_level(gpio) != 1) ok = false;
        else {
            ok = gpio_set_level(gpio, 0) == ESP_OK;
            ok = (gpio_get_level(gpio) == 0) && ok;
            delayMicroseconds(2);
        }
    }
    const bool written = gpio_set_level(gpio, high ? 1 : 0) == ESP_OK;
    // An explicit HIGH/release may retry a failed initial direction setup;
    // it does not retry RESET or an output-enable command.
    const bool feedbackReady = (command == Command::High || command == Command::Release)
        ? gpio_set_direction(gpio, GPIO_MODE_INPUT_OUTPUT) == ESP_OK : true;
    delayMicroseconds(2);
    ok = written && feedbackReady && (gpio_get_level(gpio) == (high ? 1 : 0)) && ok;
    slot->fault = !ok;
    runtime->second.controlOutputFault = !ok;
    runtime->second.state.gpio.currentState = ok
        ? (high ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW) : GPIOState::STATE_UNDEFINED;
    runtime->second.lastActivity = millis();
    runtime->second.lastError = ok ? "" : "CONTROL_OUTPUT_LEVEL_UNCONFIRMED";
    // Releasing ownership never floats the pin or restores an active LOW.
    // Failed release retains the reservation. A destroyed owner requests
    // bounded maintenance retirement without accessing its former object.
    if (ok && command == Command::Release) {
        slot->owner = nullptr;
        slot->id[0] = '\0';
        slot->consumer = FastBee::ResourceConsumerIdentity{};
        slot->pin = -1;
        slot->fault = false;
        slot->retirement.store(false, std::memory_order_relaxed);
        slot->retirementAttempted = false;
        slot->lastRetirementAttempt = 0;
        runtime->second.controlOutputClaimed = false;
        runtime->second.controlOutputRetiring = false;
    }
    return ok;
}

void PeripheralManager::retireControlOutputs() {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) return;
    const uint32_t now = millis();
    // Four permanent slots, at most one inactive-HIGH attempt per slot/second.
    // No I2C traffic, active LOW, RESET, allocation retry or waiting for locks.
    for (auto& claim : _controlOutputClaims) {
        if (!claim.owner || !claim.retirement.load(std::memory_order_acquire)) continue;
        if (claim.retirementAttempted &&
            static_cast<uint32_t>(now - claim.lastRetirementAttempt) < 1000u) continue;
        claim.retirementAttempted = true;
        claim.lastRetirementAttempt = now;
        const String id(claim.id);
        if (id.length() != std::strlen(claim.id)) continue;
        auto runtime = runtimeStates.find(id);
        if (runtime != runtimeStates.end()) runtime->second.controlOutputRetiring = true;
        if (runtime != runtimeStates.end() && runtime->second.controlOutputRetirementAttempts != UINT32_MAX)
            ++runtime->second.controlOutputRetirementAttempts;
        int pin = claim.pin;
        // Same configuration/lifecycle/pin admission as a live owner's release.
        // A drifted/missing configuration stays quarantined with no GPIO write.
        if (!controlOutput(id, claim.owner, FastBee::ControlOutputCommand::Release, pin)) {
            claim.fault = true;
            if (runtime != runtimeStates.end()) {
                runtime->second.controlOutputFault = true;
                runtime->second.controlOutputRetirementFailed = true;
            }
        }
    }
}

namespace FastBee {
ControlOutput::~ControlOutput() {
    if (!end() && _retirement) {
        // The slot cannot be reused until this request is consumed. Store is
        // the last access to the handle: no dangling owner dereference or ABA.
        _retirement->store(true, std::memory_order_release);
    }
}
bool ControlOutput::begin(const String& peripheralId, const String& consumerId,
                          ResourceConsumerRole role) {
    if (!_id.isEmpty() || peripheralId.isEmpty()) return false;
    if (consumerId.length() != std::strlen(consumerId.c_str()) ||
        !_consumer.assign(role, consumerId.c_str())) return false;
    _id = peripheralId;
    if (_id != peripheralId) { _id = ""; return false; }
    const bool accepted = command(ControlOutputCommand::Claim);
    if (!accepted && _pin < 0) _id = "";
    return accepted;
}
bool ControlOutput::command(ControlOutputCommand operation) const {
    return !_id.isEmpty() && PeripheralManager::getInstance().controlOutput(
        _id, this, operation, _pin, &_retirement, &_consumer);
}
bool ControlOutput::owned() const { return command(ControlOutputCommand::Owned); }
bool ControlOutput::isHigh() const { return command(ControlOutputCommand::IsHigh); }
bool ControlOutput::high() { return command(ControlOutputCommand::High); }
bool ControlOutput::low() { return command(ControlOutputCommand::Low); }
bool ControlOutput::pulseReset() { return command(ControlOutputCommand::PulseReset); }
bool ControlOutput::end() {
    if (_id.isEmpty()) return true;
    if (!command(ControlOutputCommand::Release)) return false;
    _id = ""; _pin = -1; _retirement = nullptr; _consumer = ResourceConsumerIdentity{};
    return true;
}
} // namespace FastBee
