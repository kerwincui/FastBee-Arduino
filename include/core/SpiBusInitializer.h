#pragma once

#include <Arduino.h>
#include <atomic>
#include "RuntimeResourceSnapshot.h"

namespace FastBee {

// Global Arduino SPI initialization plus bounded leases for migrated clients.
// Hardware remains retained until explicit end(). W5500 can share this SPIClass
// or reserve a separate host with disjoint pins; legacy callers remain conservative.
class SpiBusInitializer {
public:
    static constexpr size_t MAX_CLIENTS = 16;
    static constexpr size_t MAX_NAME_LENGTH = 40; // ResourceProfile peripheral ID limit.
    static SpiBusInitializer& instance();
    bool begin(int8_t sck, int8_t miso, int8_t mosi, int8_t cs, String& error);
    bool acquire(const void* owner, int8_t sck, int8_t miso, int8_t mosi,
                 int8_t cs, String& error,
                 ResourceConsumerRole role = ResourceConsumerRole::Unknown,
                 const char* consumerId = nullptr);
    bool release(const void* owner, String& error);
    bool isAcquired(const void* owner, uint32_t timeoutMs = 1000);
    // Named clients own an inline key; temporary config/String addresses are never retained.
    bool acquireNamed(const String& name, int8_t sck, int8_t miso, int8_t mosi,
                      int8_t cs, String& error);
    bool checkNamed(const String& name, int8_t sck, int8_t miso, int8_t mosi,
                    int8_t cs, String& error);
    bool releaseNamed(const String& name, String& error, int8_t expectedCs = -1,
                      bool* released = nullptr);
    // Fail closed when busy: callers must not detach a potentially leased CS.
    bool isChipSelectAcquired(int8_t cs);
    // One network transport, with a stable lifetime token. Shared mode uses the
    // global SPIClass; dedicated mode reserves pins before starting another host.
    bool acquireNetwork(const void* owner, bool dedicatedHost, int8_t sck,
                        int8_t miso, int8_t mosi, int8_t cs, int8_t reset,
                        int8_t interrupt, String& error);
    // Only after the network driver, callbacks and private SPIClass have stopped.
    bool releaseNetwork(const void* owner, String& error);
    // Lock order: client mutex -> this recursive mutex -> Arduino transaction.
    // Callbacks must not enter a client while holding this bus lock.
    bool lock(uint32_t timeoutMs = 1000);
    // Passive diagnostics must never allocate a mutex or wait for hardware users.
    bool hasMutex() const { return _mutex.load(std::memory_order_acquire) != nullptr; }
    bool tryLockExisting();
    void snapshotResources(SpiResourceSnapshot& result) const;
    void unlock();
    // Caller must first establish that no other configured client needs SPI.
    // A busy/unknown bus is retained so the caller can retry safely.
    bool end(String& error);

private:
    SpiBusInitializer() = default;
    bool ensureMutex();
    bool ensureClientsLocked(String& error);
    void freeClientsLocked();
    bool validateBeginLocked(int8_t sck, int8_t miso, int8_t mosi, int8_t cs, String& error) const;
    bool beginLocked(int8_t sck, int8_t miso, int8_t mosi, int8_t cs, String& error);
    int findOwner(const void* owner) const;
    int findNamed(const String& name) const;
    static bool validateName(const String& name, String& error);
    bool chipSelectTaken(int8_t cs, int except = -1) const;
    bool networkPinsConflict(int8_t sck, int8_t miso, int8_t mosi, int8_t cs) const;
    void endLocked();
    struct Client {
        const void* owner = nullptr;
        int8_t cs = -1;
        int8_t requestedMiso = -1;
        char name[MAX_NAME_LENGTH + 1]{};
        ResourceConsumerIdentity consumer;
    };
    // Allocated only for a validated managed acquisition, in internal DRAM.
    // Slot addresses remain stable until explicit end() retires every owner.
    Client* _clients = nullptr;
    size_t _clientCount = 0;
    std::atomic<void*> _mutex{nullptr};
    bool _initialized = false;
    int8_t _sck = -1;
    int8_t _miso = -1;
    int8_t _mosi = -1;
    struct NetworkLease {
        const void* owner = nullptr;
        bool dedicatedHost = false;
        int8_t pins[6]{-1, -1, -1, -1, -1, -1}; // SCK, MISO, MOSI, CS, RST, IRQ
    } _network;
};

class SpiBusOperation {
public:
    explicit SpiBusOperation(uint32_t timeoutMs = 1000)
        : _locked(SpiBusInitializer::instance().lock(timeoutMs)) {}
    ~SpiBusOperation() { if (_locked) SpiBusInitializer::instance().unlock(); }
    SpiBusOperation(const SpiBusOperation&) = delete;
    SpiBusOperation& operator=(const SpiBusOperation&) = delete;
    bool locked() const { return _locked; }
private:
    bool _locked;
};

class SpiBusObservation {
public:
    SpiBusObservation()
        : _unused(!SpiBusInitializer::instance().hasMutex()),
          _locked(!_unused && SpiBusInitializer::instance().tryLockExisting()) {}
    ~SpiBusObservation() { if (_locked) SpiBusInitializer::instance().unlock(); }
    SpiBusObservation(const SpiBusObservation&) = delete;
    SpiBusObservation& operator=(const SpiBusObservation&) = delete;
    bool unused() const { return _unused; }
    bool locked() const { return _locked; }
private:
    bool _unused;
    bool _locked;
};

}  // namespace FastBee
