#include "core/I2CBusManager.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace FastBee {

namespace {
constexpr uint32_t MIN_I2C_FREQUENCY = 10000;
constexpr uint32_t MAX_I2C_FREQUENCY = 1000000;
constexpr uint16_t I2C_TRANSACTION_TIMEOUT_MS = 20;

class RecursiveLock {
public:
    RecursiveLock(void* handle, uint32_t timeoutMs)
        : _handle(static_cast<SemaphoreHandle_t>(handle)), _locked(false) {
        if (_handle) {
            _locked = xSemaphoreTakeRecursive(
                _handle, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
        }
    }

    ~RecursiveLock() {
        if (_locked) xSemaphoreGiveRecursive(_handle);
    }

    bool locked() const { return _locked; }

private:
    SemaphoreHandle_t _handle;
    bool _locked;
};
}  // namespace

I2CBusManager& I2CBusManager::instance() {
    static I2CBusManager manager;
    return manager;
}

bool I2CBusManager::ensureMutex() const {
    if (mutexHandle()) return true;
    void* created = static_cast<void*>(xSemaphoreCreateRecursiveMutex());
    if (!created) return mutexHandle() != nullptr;
    void* expected = nullptr;
    if (!_mutex.compare_exchange_strong(expected, created,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        // Allocating outside the lock is safe: exactly one handle is published.
        vSemaphoreDelete(static_cast<SemaphoreHandle_t>(created));
    }
    return true;
}

int I2CBusManager::findOwner(const void* owner) const {
    if (!owner) return -1;
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (_owners[i].load(std::memory_order_acquire) == owner) return static_cast<int>(i);
    }
    return -1;
}

bool I2CBusManager::acquire(const void* owner, int8_t sda, int8_t scl,
                            uint32_t frequency, String& errorMsg) {
    errorMsg = "";
    if (!owner) {
        errorMsg = "I2C owner is required";
        return false;
    }
    if (sda < 0 || scl < 0 || sda == scl) {
        errorMsg = "I2C SDA/SCL pins are invalid";
        return false;
    }
    if (frequency < MIN_I2C_FREQUENCY || frequency > MAX_I2C_FREQUENCY) {
        errorMsg = "I2C frequency must be between 10kHz and 1MHz";
        return false;
    }
    if (!ensureMutex()) {
        errorMsg = "I2C mutex allocation failed";
        return false;
    }

    RecursiveLock guard(mutexHandle(), DEFAULT_LOCK_TIMEOUT_MS);
    if (!guard.locked()) {
        errorMsg = "I2C bus lock timeout";
        return false;
    }

    drainDeferredReleases();
    const int ownerIndex = findOwner(owner);
    if (ownerIndex >= 0) {
        if (_sda != sda || _scl != scl || _frequency != frequency) {
            errorMsg = "I2C owner requested a different bus configuration";
            return false;
        }
        if (_initialized) return true;
    }

    if (ownerIndex < 0 && _clientCount >= MAX_CLIENTS) {
        errorMsg = "I2C client capacity reached";
        return false;
    }

    if (_clientCount > 0) {
        if (_sda != sda || _scl != scl) {
            errorMsg = "I2C bus pins conflict with an active client";
            return false;
        }
        if (_frequency != frequency) {
            errorMsg = "I2C bus frequency conflicts with an active client";
            return false;
        }
    }
    if (!_initialized) {
        if (!Wire.begin(sda, scl)) {
            errorMsg = "I2C bus initialization failed";
            return false;
        }
        Wire.setClock(frequency);
        Wire.setTimeOut(I2C_TRANSACTION_TIMEOUT_MS);
        _initialized = true;
        _sda = sda;
        _scl = scl;
        _frequency = frequency;
    }

    if (ownerIndex < 0) {
        for (auto& slot : _owners) {
            if (!slot.load(std::memory_order_relaxed)) {
                slot.store(owner, std::memory_order_release);
                ++_clientCount;
                break;
            }
        }
    }
    return true;
}

bool I2CBusManager::release(const void* owner, uint32_t timeoutMs) {
    if (!owner || !mutexHandle()) return true;
    RecursiveLock guard(mutexHandle(), timeoutMs);
    if (!guard.locked()) return false;
    drainDeferredReleases();
    releaseLocked(owner);
    return true;
}

void I2CBusManager::deferRelease(const void* owner) {
    if (!owner) return;
    // Each active lease already owns a fixed mailbox: retirement cannot lose
    // its slot under concurrent queue churn. Lease lifetime remains exclusive
    // to its owning object. acquire() drains before an address can be reused.
    const int index = findOwner(owner);
    if (index >= 0) {
        _deferredReleases[index].store(owner, std::memory_order_release);
    }
}

void I2CBusManager::drainDeferredReleases() {
    for (auto& pending : _deferredReleases) {
        if (const void* owner = pending.exchange(nullptr, std::memory_order_acq_rel)) {
            releaseLocked(owner);
        }
    }
}

void I2CBusManager::releaseLocked(const void* owner) {
    const int ownerIndex = findOwner(owner);
    if (ownerIndex < 0) return;
    _owners[ownerIndex].store(nullptr, std::memory_order_release);
    --_clientCount;

    if (_clientCount == 0) {
        if (_initialized) Wire.end();
        _initialized = false;
        _sda = -1;
        _scl = -1;
        _frequency = 0;
    }
}

bool I2CBusManager::recover(const void* owner, String& errorMsg) {
    errorMsg = "";
    if (!owner || !mutexHandle()) {
        errorMsg = "I2C recovery requires an active owner";
        return false;
    }
    RecursiveLock guard(mutexHandle(), DEFAULT_LOCK_TIMEOUT_MS);
    if (guard.locked()) drainDeferredReleases();
    if (!guard.locked() || findOwner(owner) < 0) {
        errorMsg = "I2C recovery requires an active owner and bus lock";
        return false;
    }
    if (_initialized) Wire.end();
    _initialized = false;
    if (!Wire.begin(_sda, _scl)) {
        errorMsg = "I2C recovery initialization failed";
        return false;
    }
    Wire.setClock(_frequency);
    Wire.setTimeOut(I2C_TRANSACTION_TIMEOUT_MS);
    _initialized = true;
    return true;
}

bool I2CBusManager::lock(uint32_t timeoutMs) {
    if (!ensureMutex()) return false;
    if (xSemaphoreTakeRecursive(static_cast<SemaphoreHandle_t>(mutexHandle()),
            pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return false;
    drainDeferredReleases();
    return true;
}

void I2CBusManager::unlock() {
    if (void* handle = mutexHandle()) {
        drainDeferredReleases();
        xSemaphoreGiveRecursive(static_cast<SemaphoreHandle_t>(handle));
    }
}

TwoWire* I2CBusManager::wire(const void* owner) {
    if (!owner || !mutexHandle()) return nullptr;
    RecursiveLock guard(mutexHandle(), DEFAULT_LOCK_TIMEOUT_MS);
    if (guard.locked()) drainDeferredReleases();
    if (!guard.locked() || !_initialized || findOwner(owner) < 0) return nullptr;
    return &Wire;
}

bool I2CBusManager::isAcquired(const void* owner) const {
    if (!owner || !mutexHandle()) return false;
    RecursiveLock guard(mutexHandle(), DEFAULT_LOCK_TIMEOUT_MS);
    if (guard.locked()) const_cast<I2CBusManager*>(this)->drainDeferredReleases();
    return guard.locked() && findOwner(owner) >= 0;
}

size_t I2CBusManager::clientCount() const {
    if (!mutexHandle()) return 0;
    RecursiveLock guard(mutexHandle(), DEFAULT_LOCK_TIMEOUT_MS);
    if (guard.locked()) const_cast<I2CBusManager*>(this)->drainDeferredReleases();
    return guard.locked() ? _clientCount : 0;
}

int8_t I2CBusManager::sda() const {
    if (!mutexHandle()) return -1;
    RecursiveLock guard(mutexHandle(), DEFAULT_LOCK_TIMEOUT_MS);
    if (guard.locked()) const_cast<I2CBusManager*>(this)->drainDeferredReleases();
    return guard.locked() ? _sda : -1;
}

int8_t I2CBusManager::scl() const {
    if (!mutexHandle()) return -1;
    RecursiveLock guard(mutexHandle(), DEFAULT_LOCK_TIMEOUT_MS);
    if (guard.locked()) const_cast<I2CBusManager*>(this)->drainDeferredReleases();
    return guard.locked() ? _scl : -1;
}

uint32_t I2CBusManager::frequency() const {
    if (!mutexHandle()) return 0;
    RecursiveLock guard(mutexHandle(), DEFAULT_LOCK_TIMEOUT_MS);
    if (guard.locked()) const_cast<I2CBusManager*>(this)->drainDeferredReleases();
    return guard.locked() ? _frequency : 0;
}

}  // namespace FastBee
