#pragma once

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_DS1302
#include "core/AsyncExecTypes.h"
#include "core/PeripheralManager.h"
#include "core/RuntimeFeatureManager.h"
#include "peripherals/drivers/DS1302Driver.h"

namespace FastBee {
// One lock covers lookup, singleton ownership, GPIO and readback. Lifecycle
// operations already use this recursive manager mutex; HTTP and rules must
// join the same lock, never operate on an earlier configuration snapshot.
class RtcAccess {
public:
    explicit RtcAccess(const String& id)
        : _lock(PeripheralManager::getInstance().getMutex(), pdMS_TO_TICKS(50)) {
        if (!_lock.isLocked()) { _error = "DS1302_BUSY"; return; }
        auto& manager = PeripheralManager::getInstance();
        PeripheralConfig config;
        PeripheralRuntimeState state;
        if (!manager.getPeripheralSnapshot(id, config) ||
            config.type != PeripheralType::DS1302_RTC) {
            _error = "DS1302_TARGET_INVALID"; return;
        }
        if (!config.enabled || !manager.getRuntimeStateSnapshot(id, state) ||
            state.memorySuspended ||
            (state.status != PeripheralStatus::PERIPHERAL_INITIALIZED &&
             state.status != PeripheralStatus::PERIPHERAL_RUNNING) ||
            !RuntimeFeatureManager::instance().shouldEnable("ds1302")) {
            _error = "DS1302_NOT_READY"; return;
        }
        auto& driver = DS1302Driver::getInstance();
        if (!driver.isInitialized()) { _error = "DS1302_NOT_INITIALIZED"; return; }
        if (config.pinCount != 3 ||
            !driver.usesPins(config.pins[0], config.pins[1], config.pins[2])) {
            _error = "DS1302_OWNER_MISMATCH"; return;
        }
        _driver = &driver;
    }
    DS1302Driver* driver() const { return _driver; }
    const char* error() const { return _error; }
private:
    RecursiveMutexGuard _lock;
    DS1302Driver* _driver = nullptr;
    const char* _error = "";
};
} // namespace FastBee
#endif
