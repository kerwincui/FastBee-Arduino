#ifndef FASTBEE_I2C_BUS_MANAGER_H
#define FASTBEE_I2C_BUS_MANAGER_H

#include <Arduino.h>
#include <Wire.h>
#include <stddef.h>
#include <stdint.h>
#include <atomic>
#include "RuntimeResourceSnapshot.h"

namespace FastBee {

class I2CBusManager {
public:
    static constexpr size_t MAX_CLIENTS = 16;
    static constexpr uint32_t DEFAULT_LOCK_TIMEOUT_MS = 1000;

    static I2CBusManager& instance();

    bool acquire(const void* owner, int8_t sda, int8_t scl,
                 uint32_t frequency, String& errorMsg,
                 ResourceConsumerRole role = ResourceConsumerRole::Unknown,
                 const char* consumerId = nullptr);
    // Exclusively reserve one 7-bit device address for an existing bus lease.
    // Bus-only owners (configuration/scans) do not reserve an address. Raw Wire
    // access is not intercepted; clients must cooperate with this contract.
    bool claimAddress(const void* owner, uint8_t address, String& errorMsg);
    bool ownsAddress(const void* owner, uint8_t address) const;
    // A failed release keeps ownership so the caller can retry.
    bool release(const void* owner, uint32_t timeoutMs = DEFAULT_LOCK_TIMEOUT_MS);
    // Transfer an active lease from a dying object without waiting on the bus.
    // The next bus operation/unlock retires it before accepting a new owner.
    void deferRelease(const void* owner);
    bool recover(const void* owner, String& errorMsg);

    bool lock(uint32_t timeoutMs = DEFAULT_LOCK_TIMEOUT_MS);
    void unlock();
    TwoWire* wire(const void* owner);

    bool isAcquired(const void* owner) const;
    size_t clientCount() const;
    int8_t sda() const;
    int8_t scl() const;
    uint32_t frequency() const;
    // Zero wait, no mutex creation, Wire access or deferred-release draining.
    void snapshotResources(I2CResourceSnapshot& result) const;

private:
    I2CBusManager() = default;
    I2CBusManager(const I2CBusManager&) = delete;
    I2CBusManager& operator=(const I2CBusManager&) = delete;

    bool ensureMutex() const;
    void* mutexHandle() const { return _mutex.load(std::memory_order_acquire); }
    int findOwner(const void* owner) const;
    void releaseLocked(const void* owner);
    void drainDeferredReleases();

    mutable std::atomic<void*> _mutex{nullptr};
    // Fixed atomic slots let a dying owner mark its own lease for retirement
    // without taking the transaction mutex or allocating a queue node.
    std::atomic<const void*> _owners[MAX_CLIENTS]{};
    std::atomic<const void*> _deferredReleases[MAX_CLIENTS]{};
    uint8_t _addresses[MAX_CLIENTS]{}; // 0 = bus-only; guarded by bus mutex
    ResourceConsumerIdentity _consumers[MAX_CLIENTS]{}; // Same mutex/lifetime as owners.
    size_t _clientCount = 0;
    bool _initialized = false;
    int8_t _sda = -1;
    int8_t _scl = -1;
    uint32_t _frequency = 0;
};

class I2CBusLock {
public:
    explicit I2CBusLock(uint32_t timeoutMs = I2CBusManager::DEFAULT_LOCK_TIMEOUT_MS)
        : _locked(I2CBusManager::instance().lock(timeoutMs)) {}

    ~I2CBusLock() {
        if (_locked) I2CBusManager::instance().unlock();
    }

    bool locked() const { return _locked; }

private:
    bool _locked;
};

}  // namespace FastBee

#endif  // FASTBEE_I2C_BUS_MANAGER_H
