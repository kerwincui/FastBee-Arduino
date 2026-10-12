#pragma once
#include "core/AsyncExecTypes.h"
#include "core/PeripheralManager.h"
#include "peripherals/SDCardManager.h"

namespace FastBee {
// Keep configuration/lifecycle stable through an API storage operation. Order:
// PeripheralManager -> SD mutex -> shared SPI. Lower layers never acquire PM.
class SdAccess {
public:
    SdAccess(const String& id, uint32_t expectedVersion)
        : _lock(PeripheralManager::getInstance().getMutex(), 0) {
        if (!_lock.isLocked()) { _error = "SD_CONFIG_BUSY"; return; }
        auto& manager = PeripheralManager::getInstance();
        PeripheralConfig config;
        PeripheralRuntimeState runtime;
        uint32_t currentVersion = 0;
        if (!manager.getPeripheralSnapshot(id, config, &currentVersion) || config.type != PeripheralType::SDIO) {
            _error = "SD_TARGET_INVALID"; return;
        }
        if (currentVersion != expectedVersion) {
            _error = "SD_CONFIG_CHANGED"; return;
        }
        if (!config.enabled || !manager.getRuntimeStateSnapshot(id, runtime) || runtime.memorySuspended) {
            _error = "SD_NOT_READY"; return;
        }
        auto& storage = SDCardManager::getInstance();
        _status = storage.getStatus();
        if (!_status.statusAvailable && !_status.configurationAvailable) {
            _error = "SD_STATUS_UNAVAILABLE"; return;
        }
        if (!_status.configured || _status.ownerId != id) { _error = "SD_OWNER_MISMATCH"; return; }
        if (config.pinCount != 4 || _status.sckPin != config.pins[0] || _status.mosiPin != config.pins[1] ||
            _status.misoPin != config.pins[2] || _status.csPin != config.pins[3] ||
            _status.frequency != config.params.sdcard.frequency) { _error = "SD_CONFIG_CHANGED"; return; }
        // Unmounted/error media may be explicitly recovered, but a disabled or
        // memory-suspended peripheral cannot reacquire resources from the API.
        _driver = &storage;
    }
    SDCardManager* driver() const { return _driver; }
    const SDCardStatus& status() const { return _status; }
    const char* error() const { return _error; }
private:
    RecursiveMutexGuard _lock;
    SDCardStatus _status;
    SDCardManager* _driver = nullptr;
    const char* _error = "";
};
} // namespace FastBee
