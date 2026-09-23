#pragma once
#include <Wire.h>
#include <set>
#include <mutex>
namespace FastBee {
class I2CBusManager {
public:
    static I2CBusManager& instance() { static I2CBusManager manager; return manager; }
    bool acquire(const void* owner, int8_t data, int8_t clock, uint32_t frequency, String& error) {
        if (!owners.empty() && (data != _sda || clock != _scl || frequency != _frequency)) {
            error = "I2C_CONFLICT"; return false;
        }
        owners.insert(owner); _sda = data; _scl = clock; _frequency = frequency; return true;
    }
    bool release(const void* owner, uint32_t = 1000) { owners.erase(owner); return true; }
    void deferRelease(const void* owner) { owners.erase(owner); }
    bool lock(uint32_t) { mutex.lock(); ++sensorTestBusLockDepth; return true; }
    void unlock() { --sensorTestBusLockDepth; mutex.unlock(); }
    TwoWire* wire(const void* owner) { return owners.count(owner) ? &Wire : nullptr; }
    int8_t sda() const { return owners.empty() ? -1 : _sda; }
    int8_t scl() const { return owners.empty() ? -1 : _scl; }
    uint32_t frequency() const { return owners.empty() ? 0 : _frequency; }
    size_t clientCount() const { return owners.size(); }
private:
    std::set<const void*> owners;
    std::recursive_mutex mutex;
    int8_t _sda = 21, _scl = 22;
    uint32_t _frequency = 100000;
};
class I2CBusLock {
public:
    explicit I2CBusLock(uint32_t timeout = 1000) : held(I2CBusManager::instance().lock(timeout)) {}
    ~I2CBusLock() { if (held) I2CBusManager::instance().unlock(); }
    bool locked() const { return held; }
private:
    bool held;
};
}
