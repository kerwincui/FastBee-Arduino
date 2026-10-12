#pragma once
#include "core/PeripheralConfig.h"
#include <mutex>
// Keep the real mutex semantics, with an explicit failed-admission seam.
inline bool spiManagerLockAvailable = true;
class RecursiveMutexGuard {
    std::unique_lock<std::recursive_mutex> guard;
public:
    explicit RecursiveMutexGuard(std::recursive_mutex& mutex, unsigned = 0)
        : guard(mutex, std::defer_lock) {
        if (spiManagerLockAvailable) guard.lock();
    }
    bool isLocked() const { return guard.owns_lock(); }
};
class PeripheralManager {
public:
    bool setupSpiHardware(const PeripheralConfig& config);
    bool teardownSpiHardware(const PeripheralConfig& config);
    bool writeData(const String& id, const uint8_t* data, size_t len);
    bool readData(const String& id, uint8_t* buffer, size_t& len);
    const PeripheralConfig* getPeripheral(const String& id) const {
        auto it = peripherals.find(id); return it == peripherals.end() ? nullptr : &it->second;
    }
    bool isValidPin(uint8_t pin) const { return pin < 64; }
    bool isReservedPin(uint8_t pin) const { return pin == 63; }
    bool isInputOnlyPin(uint8_t pin) const { return pin == 62; }
    struct State { String lastError; };
    std::recursive_mutex _mutex;
    std::map<String, PeripheralConfig> peripherals;
    std::map<String, State> runtimeStates;
};
