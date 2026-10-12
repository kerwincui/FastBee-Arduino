#include "core/SpiBusInitializer.h"

#include <SPI.h>
#include <cstring>
#include <new>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace FastBee {
namespace {
class InitializationLock {
public:
    explicit InitializationLock(void* mutex, uint32_t timeoutMs = 1000)
        : _mutex(static_cast<SemaphoreHandle_t>(mutex)),
          _locked(_mutex && xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {}
    ~InitializationLock() { if (_locked) xSemaphoreGiveRecursive(_mutex); }
    bool locked() const { return _locked; }
private:
    SemaphoreHandle_t _mutex;
    bool _locked;
};
}  // namespace

SpiBusInitializer& SpiBusInitializer::instance() {
    static SpiBusInitializer initializer;
    return initializer;
}

void SpiBusInitializer::snapshotResources(SpiResourceSnapshot& result) const {
    static_assert(MAX_CLIENTS == SpiResourceSnapshot::CAPACITY, "snapshot capacity");
    result = SpiResourceSnapshot{};
    void* handle = _mutex.load(std::memory_order_acquire);
    if (!handle) return;
    InitializationLock guard(handle, 0);
    if (!guard.locked()) { result.status = ResourceObservation::Busy; return; }
    result.initialized = _initialized;
    result.sck = _sck; result.miso = _miso; result.mosi = _mosi;
    for (size_t i = 0; _clients && i < MAX_CLIENTS; ++i) {
        const auto& client = _clients[i];
        if (!client.owner) continue;
        auto& lease = result.leases[result.count++];
        lease.slot = static_cast<uint8_t>(i);
        lease.cs = client.cs; lease.requestedMiso = client.requestedMiso;
        lease.networkOwner = client.owner == _network.owner;
        std::memcpy(lease.name, client.name, sizeof(lease.name));
        lease.name[sizeof(lease.name) - 1] = '\0';
        lease.consumer = client.consumer;
    }
    result.networkReserved = _network.owner != nullptr;
    result.networkDedicatedHost = _network.dedicatedHost;
    std::memcpy(result.networkPins, _network.pins, sizeof(result.networkPins));
    // _initialized is the manager's record; never query SPI.bus() here.
}

bool SpiBusInitializer::ensureMutex() {
    if (_mutex.load(std::memory_order_acquire)) return true;
    void* created = static_cast<void*>(xSemaphoreCreateRecursiveMutex());
    if (!created) return _mutex.load(std::memory_order_acquire) != nullptr;
    void* expected = nullptr;
    if (!_mutex.compare_exchange_strong(expected, created,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        vSemaphoreDelete(static_cast<SemaphoreHandle_t>(created));
    }
    return true;
}

bool SpiBusInitializer::ensureClientsLocked(String& error) {
    if (_clients) return true;
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
    void* storage = heap_caps_malloc(sizeof(Client) * MAX_CLIENTS,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    void* storage = ::operator new(sizeof(Client) * MAX_CLIENTS, std::nothrow);
#endif
    if (!storage) {
        error = "SPI_CLIENT_STORAGE_UNAVAILABLE: unable to allocate lease storage";
        return false;
    }
    auto* clients = static_cast<Client*>(storage);
    for (size_t i = 0; i < MAX_CLIENTS; ++i) new (&clients[i]) Client{};
    _clients = clients;
    return true;
}

void SpiBusInitializer::freeClientsLocked() {
    if (!_clients || _clientCount != 0 || _network.owner) return;
    for (size_t i = 0; i < MAX_CLIENTS; ++i) _clients[i].~Client();
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
    heap_caps_free(_clients);
#else
    ::operator delete(_clients);
#endif
    _clients = nullptr;
}

bool SpiBusInitializer::begin(int8_t sck, int8_t miso, int8_t mosi,
                              int8_t cs, String& error) {
    if (!ensureMutex()) {
        error = "SPI_MUTEX_UNAVAILABLE: unable to create initialization lock";
        return false;
    }
    InitializationLock guard(_mutex.load(std::memory_order_acquire));
    if (!guard.locked()) {
        error = "SPI_BUSY: initialization lock timeout";
        return false;
    }
    if (chipSelectTaken(cs)) {
        error = "SPI_CS_CONFLICT: chip select belongs to a leased client";
        return false;
    }
    if (!beginLocked(sck, miso, mosi, cs, error)) return false;
    return true;
}

bool SpiBusInitializer::validateBeginLocked(int8_t sck, int8_t miso, int8_t mosi,
                                            int8_t cs, String& error) const {
    error = "";
    if (networkPinsConflict(sck, miso, mosi, cs)) {
        error = "SPI_NETWORK_PIN_CONFLICT: pins reserved by the network transport";
        return false;
    }
    if (sck < 0 || mosi < 0 || miso < -1 || cs < 0 || sck == mosi ||
        (miso >= 0 && (miso == sck || miso == mosi)) ||
        cs == sck || cs == mosi || cs == miso) {
        error = "SPI_PINS_INVALID: explicit non-overlapping pins are required";
        return false;
    }
    if (SPI.bus()) {
        // Arduino begin() returns true for an existing bus without checking pins.
        // Do not claim compatibility with a bus initialized outside this path.
        if (!_initialized) {
            error = "SPI_BUS_UNMANAGED: active bus pin assignment is unknown";
            return false;
        }
        if (_sck != sck || _mosi != mosi || (miso >= 0 && _miso != miso)) {
            error = "SPI_PINS_CONFLICT: active bus cannot be rebound";
            return false;
        }
        return true;
    }
    if (_clientCount != 0) {
        error = "SPI_BUS_LOST: existing clients must stop before reinitialization";
        return false;
    }
    return true;
}

bool SpiBusInitializer::beginLocked(int8_t sck, int8_t miso, int8_t mosi,
                                    int8_t cs, String& error) {
    if (!validateBeginLocked(sck, miso, mosi, cs, error)) return false;
    if (SPI.bus()) return true;
    _initialized = false;
    if (!SPI.begin(sck, miso, mosi, cs)) {
        // Some Arduino pin-attach failures leave bus() non-null. Roll back only
        // this newly created bus, never one that existed on entry.
        SPI.end();
        error = "SPI_INIT_FAILED: bus or pin attachment failed";
        return false;
    }
    _sck = sck;
    _miso = miso;
    _mosi = mosi;
    _initialized = true;
    return true;
}

bool SpiBusInitializer::end(String& error) {
    error = "";
    if (!ensureMutex()) {
        error = "SPI_MUTEX_UNAVAILABLE: unable to create initialization lock";
        return false;
    }
    InitializationLock guard(_mutex.load(std::memory_order_acquire));
    if (!guard.locked()) {
        error = "SPI_BUSY: initialization lock timeout";
        return false;
    }
    if (_clientCount != 0) {
        error = "SPI_IN_USE: active clients retain the bus";
        return false;
    }
    if (SPI.bus() && !_initialized) {
        error = "SPI_BUS_UNMANAGED: refusing to stop an unknown bus";
        return false;
    }
    endLocked();
    return true;
}

void SpiBusInitializer::endLocked() {
    if (SPI.bus()) SPI.end();
    _initialized = false;
    _sck = _miso = _mosi = -1;
    // Explicit end has established no managed clients and stopped the bus.
    // A dedicated network reservation still retains storage until released;
    // ordinary release never invalidates named owner tokens or legacy users.
    freeClientsLocked();
}

int SpiBusInitializer::findOwner(const void* owner) const {
    if (!owner || !_clients) return -1;
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (_clients[i].owner == owner) return static_cast<int>(i);
    }
    return -1;
}

bool SpiBusInitializer::chipSelectTaken(int8_t cs, int except) const {
    for (size_t i = 0; _clients && i < MAX_CLIENTS; ++i) {
        if (static_cast<int>(i) != except && _clients[i].owner && _clients[i].cs == cs) return true;
    }
    return false;
}

bool SpiBusInitializer::acquire(const void* owner, int8_t sck, int8_t miso,
                                int8_t mosi, int8_t cs, String& error,
                                ResourceConsumerRole role, const char* consumerId) {
    error = "";
    ResourceConsumerIdentity consumer;
    if (!consumer.assign(role, consumerId)) {
        error = "SPI_CONSUMER_INVALID";
        return false;
    }
    if (!owner) {
        error = "SPI_OWNER_REQUIRED: lease owner is missing";
        return false;
    }
    if (!ensureMutex()) {
        error = "SPI_MUTEX_UNAVAILABLE: unable to create initialization lock";
        return false;
    }
    InitializationLock guard(_mutex.load(std::memory_order_acquire));
    if (!guard.locked()) {
        error = "SPI_BUSY: initialization lock timeout";
        return false;
    }
    const int existing = findOwner(owner);
    if (existing >= 0 && consumer.specified() && !consumer.equals(_clients[existing].consumer)) {
        error = "SPI_CONSUMER_CHANGED: release the previous lease first";
        return false;
    }
    if (existing >= 0 && (_clients[existing].cs != cs ||
                         _clients[existing].requestedMiso != miso)) {
        error = "SPI_OWNER_CHANGED: release the previous configuration first";
        return false;
    }
    if (chipSelectTaken(cs, existing)) {
        error = "SPI_CS_CONFLICT: chip select belongs to another client";
        return false;
    }
    if (existing < 0 && _clientCount >= MAX_CLIENTS) {
        error = "SPI_CLIENT_LIMIT: fixed client capacity reached";
        return false;
    }
    // Validate before allocating or touching hardware. OOM cannot start a bus
    // without a lease, and an existing table/lease is never replaced.
    if (!validateBeginLocked(sck, miso, mosi, cs, error)) return false;
    if (!ensureClientsLocked(error)) return false;
    if (!beginLocked(sck, miso, mosi, cs, error)) return false;
    if (existing >= 0) return true;
    for (size_t i = 0; _clients && i < MAX_CLIENTS; ++i) {
        auto& client = _clients[i];
        if (!client.owner) {
            client.owner = owner;
            client.cs = cs;
            client.requestedMiso = miso;
            client.consumer = consumer;
            ++_clientCount;
            return true;
        }
    }
    return false; // Capacity was checked while holding the same mutex.
}

bool SpiBusInitializer::release(const void* owner, String& error) {
    error = "";
    if (!owner || !_mutex.load(std::memory_order_acquire)) return true;
    InitializationLock guard(_mutex.load(std::memory_order_acquire));
    if (!guard.locked()) {
        error = "SPI_BUSY: lease retained; retry release";
        return false;
    }
    const int index = findOwner(owner);
    if (index < 0) return true;
    _clients[index] = Client{};
    --_clientCount;
    // Do not automatically end the hardware here. Network ownership is tracked,
    // but legacy begin() callers still have no lease. Explicit end() remains
    // checked against all managed clients.
    // A coordinated explicit end() remains possible after all users stop.
    return true;
}

bool SpiBusInitializer::isAcquired(const void* owner, uint32_t timeoutMs) {
    if (!owner || !_mutex.load(std::memory_order_acquire)) return false;
    InitializationLock guard(_mutex.load(std::memory_order_acquire), timeoutMs);
    return guard.locked() && findOwner(owner) >= 0;
}

bool SpiBusInitializer::validateName(const String& name, String& error) {
    if (name.isEmpty() || name.length() > MAX_NAME_LENGTH ||
        std::strlen(name.c_str()) != name.length()) {
        error = "SPI_OWNER_INVALID: logical ID must contain 1-40 bytes without NUL";
        return false;
    }
    return true;
}

int SpiBusInitializer::findNamed(const String& name) const {
    for (size_t i = 0; _clients && i < MAX_CLIENTS; ++i) {
        if (_clients[i].owner && _clients[i].name[0] &&
            std::strcmp(_clients[i].name, name.c_str()) == 0) return static_cast<int>(i);
    }
    return -1;
}

bool SpiBusInitializer::acquireNamed(const String& name, int8_t sck, int8_t miso,
                                     int8_t mosi, int8_t cs, String& error) {
    error = "";
    if (!validateName(name, error)) return false;
    SpiBusOperation operation;
    if (!operation.locked()) {
        error = "SPI_BUSY: named lease cannot be acquired";
        return false;
    }
    int index = findNamed(name);
    if (index >= 0 && _clients[index].requestedMiso != miso) {
        error = "SPI_OWNER_CHANGED: release the previous MISO configuration first";
        return false;
    }
    // A named owner's token is its slot address. Create the table only after
    // validating the first acquisition, before taking that address.
    if (!_clients) {
        if (!validateBeginLocked(sck, miso, mosi, cs, error)) return false;
        if (!ensureClientsLocked(error)) return false;
    }
    if (index < 0) {
        for (size_t i = 0; _clients && i < MAX_CLIENTS; ++i) {
            if (!_clients[i].owner) { index = static_cast<int>(i); break; }
        }
    }
    if (index < 0) {
        error = "SPI_CLIENT_LIMIT: fixed client capacity reached";
        return false;
    }
    // First free slot is stable while holding the operation lock. Pointer and
    // named leases share the same capacity and CS-conflict checks.
    if (!acquire(&_clients[index], sck, miso, mosi, cs, error,
                 ResourceConsumerRole::Peripheral, name.c_str())) return false;
    std::memcpy(_clients[index].name, name.c_str(), name.length() + 1);
    return true;
}

bool SpiBusInitializer::checkNamed(const String& name, int8_t sck, int8_t miso,
                                   int8_t mosi, int8_t cs, String& error) {
    error = "";
    if (!validateName(name, error)) return false;
    SpiBusOperation operation;
    if (!operation.locked()) {
        error = "SPI_BUSY: named lease cannot be checked";
        return false;
    }
    const int index = findNamed(name);
    if (index < 0) {
        error = "SPI_LEASE_MISSING: peripheral has no active lease";
        return false;
    }
    if (!SPI.bus() || !_initialized) {
        error = "SPI_BUS_LOST: stop before recovering the bus";
        return false;
    }
    if (_clients[index].cs != cs || _clients[index].requestedMiso != miso ||
        _sck != sck || _mosi != mosi ||
        (miso >= 0 && _miso != miso)) {
        error = "SPI_CONFIG_CHANGED: active lease differs from requested pins";
        return false;
    }
    return true;
}

bool SpiBusInitializer::releaseNamed(const String& name, String& error,
                                     int8_t expectedCs, bool* released) {
    error = "";
    if (released) *released = false;
    if (!validateName(name, error)) return false;
    SpiBusOperation operation;
    if (!operation.locked()) {
        error = "SPI_BUSY: named lease retained; retry release";
        return false;
    }
    const int index = findNamed(name);
    if (index < 0) return true;
    if (expectedCs >= 0 && _clients[index].cs != expectedCs) {
        error = "SPI_CONFIG_CHANGED: refusing to release another chip select";
        return false;
    }
    if (!release(&_clients[index], error)) return false;
    if (released) *released = true;
    return true;
}

bool SpiBusInitializer::isChipSelectAcquired(int8_t cs) {
    SpiBusOperation operation;
    if (!operation.locked() || chipSelectTaken(cs)) return true;
    if (_network.owner) {
        for (int8_t pin : _network.pins) if (pin >= 0 && pin == cs) return true;
    }
    return false;
}

bool SpiBusInitializer::networkPinsConflict(int8_t sck, int8_t miso,
                                           int8_t mosi, int8_t cs) const {
    if (!_network.owner) return false;
    const int8_t proposed[]{sck, miso, mosi, cs};
    for (size_t i = 0; i < 4; ++i) {
        if (proposed[i] < 0) continue;
        for (size_t j = 0; j < 6; ++j) {
            if (_network.pins[j] < 0 || proposed[i] != _network.pins[j]) continue;
            // Shared bus wires are allowed only in the same role. Controls and
            // wires on a different hardware host are always exclusive.
            if (_network.dedicatedHost || i >= 3 || j >= 3 || i != j) return true;
        }
    }
    return false;
}

bool SpiBusInitializer::acquireNetwork(const void* owner, bool dedicatedHost,
                                      int8_t sck, int8_t miso, int8_t mosi,
                                      int8_t cs, int8_t reset, int8_t interrupt,
                                      String& error) {
    error = "";
    SpiBusOperation operation;
    if (!operation.locked()) { error = "SPI_BUSY: network lease unavailable"; return false; }
    const int8_t pins[]{sck, miso, mosi, cs, reset, interrupt};
    if (!owner) { error = "SPI_OWNER_REQUIRED: network token missing"; return false; }
    for (size_t i = 0; i < 6; ++i) {
        if (pins[i] < (i < 4 ? 0 : -1)) {
            error = "SPI_PINS_INVALID: network pins must be explicit"; return false;
        }
        if (pins[i] < 0) continue;
        for (size_t j = 0; j < i; ++j) {
            if (pins[i] == pins[j]) {
                error = "SPI_PINS_INVALID: network pins overlap"; return false;
            }
        }
    }
    if (_network.owner) {
        if (_network.owner != owner) {
            error = "SPI_NETWORK_IN_USE: another network owner is active"; return false;
        }
        if (_network.dedicatedHost != dedicatedHost ||
            std::memcmp(_network.pins, pins, sizeof(pins)) != 0) {
            error = "SPI_OWNER_CHANGED: stop the previous network configuration"; return false;
        }
        if (!dedicatedHost && (!SPI.bus() || !_initialized || findOwner(owner) < 0)) {
            error = "SPI_BUS_LOST: network lease must stop before recovery"; return false;
        }
        return true;
    }
    if (SPI.bus() && !_initialized) {
        error = "SPI_BUS_UNMANAGED: cannot verify network pin isolation"; return false;
    }
    if (_clientCount && !SPI.bus()) {
        error = "SPI_BUS_LOST: existing clients must stop before network startup"; return false;
    }
    for (size_t i = 0; i < 6; ++i) {
        if (pins[i] < 0) continue;
        if (chipSelectTaken(pins[i]) || (SPI.bus() && (dedicatedHost || i >= 3) &&
                (pins[i] == _sck || pins[i] == _miso || pins[i] == _mosi))) {
            error = "SPI_NETWORK_PIN_CONFLICT: existing SPI client owns this pin"; return false;
        }
    }
    if (!dedicatedHost && !acquire(owner, sck, miso, mosi, cs, error, ResourceConsumerRole::Ethernet)) return false;
    _network.owner = owner;
    _network.dedicatedHost = dedicatedHost;
    std::memcpy(_network.pins, pins, sizeof(pins));
    return true;
}

bool SpiBusInitializer::releaseNetwork(const void* owner, String& error) {
    error = "";
    SpiBusOperation operation;
    if (!operation.locked()) { error = "SPI_BUSY: network lease retained"; return false; }
    if (!_network.owner) return true;
    if (!owner || _network.owner != owner) {
        error = "SPI_OWNER_MISMATCH: refusing to release another network owner"; return false;
    }
    if (!_network.dedicatedHost && !release(owner, error)) return false;
    _network = NetworkLease{};
    return true;
}

bool SpiBusInitializer::lock(uint32_t timeoutMs) {
    return ensureMutex() && xSemaphoreTakeRecursive(
        static_cast<SemaphoreHandle_t>(_mutex.load(std::memory_order_acquire)),
        pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

bool SpiBusInitializer::tryLockExisting() {
    void* mutex = _mutex.load(std::memory_order_acquire);
    return mutex && xSemaphoreTakeRecursive(static_cast<SemaphoreHandle_t>(mutex), 0) == pdTRUE;
}

void SpiBusInitializer::unlock() {
    if (void* mutex = _mutex.load(std::memory_order_acquire)) {
        xSemaphoreGiveRecursive(static_cast<SemaphoreHandle_t>(mutex));
    }
}
}  // namespace FastBee
