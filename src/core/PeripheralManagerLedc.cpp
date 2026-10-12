#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/ChipConfig.h"
#include "core/CheckedAdc.h"
#include <cstring>

bool PeripheralManager::attachManagedLedc(const String& id, uint8_t pin,
                                         uint32_t frequency, uint8_t resolution,
                                         uint8_t channel, bool commandScript) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || _managedPinIo.hasGpio(pin) || _managedPinIo.hasDac(pin) || FastBee::adcPinUnavailable(pin) ||
        (!commandScript && id.length() == 0) || id.length() >= 41 ||
        std::strlen(id.c_str()) != id.length() || !isValidPin(pin) ||
        isReservedPin(pin) || isInputOnlyPin(pin) || (!commandScript && channel >= CHIP_MAX_PWM_CH) ||
        (commandScript && (channel != 255 || id.length() != 0)) ||
        frequency == 0 || resolution == 0 || resolution >= 32) return false;
    static_assert(CHIP_MAX_PWM_CH <= FastBee::LedcResourceSnapshot::CAPACITY, "LEDC ledger capacity");
    FastBee::LedcResourceSnapshot::Lease* available = nullptr;
    for (auto& lease : _ledcResourceLeases) {
        if (lease.pin == 255) { if (!available) available = &lease; continue; }
        if (lease.pin == pin) {
            // A failed cleanup retains its exact target until explicit release.
            // No alias, rename, parameter change or automatic recovery here.
            if (lease.commandScript != commandScript || std::strcmp(lease.id, id.c_str()) != 0 || lease.cleanupFailed ||
                lease.channel != channel || lease.requestedFrequency != frequency ||
                lease.requestedResolution != resolution) return false;
            // The SDK rejects a second attach on an attached pin. Reuse this
            // unchanged reservation only; callers must check the ensuing
            // ledcWrite. Do not reattach after an unknown external pin change
            // or treat this branch as a fresh hardware attachment result.
            return true;
        }
        if ((channel != 255 && lease.channel == channel) ||
            (!commandScript && !lease.commandScript && std::strcmp(lease.id, id.c_str()) == 0)) return false;
    }
    if (!available) return false;
    // A slot is available before hardware is touched. Publish only SDK success;
    // no configuration entry or readback value is evidence of an attachment.
    if (!(commandScript ? ledcAttach(pin, frequency, resolution) : ledcAttachChannel(pin, frequency, resolution, channel))) return false;
    available->pin = pin;
    available->channel = channel;
    available->requestedFrequency = frequency;
    available->requestedResolution = resolution;
    available->commandScript = commandScript;
    std::memcpy(available->id, id.c_str(), id.length() + 1);
    return true;
}

bool PeripheralManager::detachManagedLedc(const String& id, uint8_t pin, bool commandScript) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) return false;
    for (auto& lease : _ledcResourceLeases) {
        if (lease.pin == 255 || lease.pin != pin) continue;
        if (lease.commandScript != commandScript || id.length() != std::strlen(id.c_str()) ||
            std::strcmp(lease.id, id.c_str()) != 0)
            return false; // Never detach a different retained owner.
        if (!ledcDetach(pin)) { lease.cleanupFailed = true; return false; }
        lease = FastBee::LedcResourceSnapshot::Lease{};
        return true;
    }
    return true; // No managed successful attachment to release (e.g. failed init).
}

bool PeripheralManager::writeScriptPwm(uint8_t pin, uint32_t duty) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || duty > 255 || !scriptPinReady(pin, 1) ||
        !attachManagedLedc("", pin, 5000, 8, 255, true)) return false;
    if (ledcWrite(pin, duty)) return true;
    detachManagedLedc("", pin, true);
    return false;
}

bool PeripheralManager::releaseScriptPwm(uint8_t pin) {
    // Called before raw GPIO/DAC can implicitly detach LEDC. A native owner or
    // failed cleanup blocks that switch; unknown SDK users are not inferred.
    return detachManagedLedc("", pin, true);
}

void PeripheralManager::snapshotLedcResources(FastBee::LedcResourceSnapshot& result) const {
    result = FastBee::LedcResourceSnapshot{};
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) { result.status = FastBee::ResourceObservation::Busy; return; }
    for (size_t i = 0; i < FastBee::LedcResourceSnapshot::CAPACITY; ++i) {
        if (_ledcResourceLeases[i].pin == 255) continue;
        auto& lease = result.leases[result.count++];
        lease = _ledcResourceLeases[i];
        lease.slot = static_cast<uint8_t>(i);
    }
}
