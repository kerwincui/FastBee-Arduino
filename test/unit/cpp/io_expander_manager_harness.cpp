// Compiles the production expander manager methods/branches extracted by the
// Python runner. Peripheral storage, clock and transport are the host seams;
// channel admission, synchronization, maintenance and Pcf8574 remain real code.
#define millis unused_arduino_millis
#define ESP unused_arduino_esp
#define Serial unused_arduino_serial
#include <Arduino.h>
#undef millis
#undef ESP
#undef Serial
#include <freertos/FreeRTOS.h>
#include "peripherals/IoExpander.h"
#include <cassert>
#include <atomic>
#include <memory>
#include <new>

static uint32_t ioNow = 0;
static uint32_t nextProviderWriteCost = 0;
static const char* featureUnavailable = nullptr;
static unsigned shutdownWarnings = 0, liveLeases = 0, destroyedWithLease = 0;
static uint8_t lastDestroyedByte = 0xff;
#define LOG_WARNINGF(...) (++shutdownWarnings)
#define LOG_WARNING(...) (++shutdownWarnings)
std::atomic<bool>& ioShutdownRequested();
unsigned long millis() { return ioNow; }
enum class GPIOState { STATE_UNDEFINED, STATE_LOW, STATE_HIGH };
enum class PeripheralType { GPIO_DIGITAL_INPUT, GPIO_DIGITAL_OUTPUT, IO_EXPANDER };
enum class PeripheralStatus { PERIPHERAL_RUNNING, PERIPHERAL_ERROR, PERIPHERAL_DISABLED };
struct PeripheralConfig {
    String id;
    bool enabled = true;
    PeripheralType type = PeripheralType::IO_EXPANDER;
    struct {
        struct {
            char providerId[41] = {};
            uint8_t channel = 0;
            bool invertLogic = false;
            GPIOState initialState = GPIOState::STATE_LOW;
        } gpio;
        struct { uint8_t address = 0x20; uint16_t sampleInterval = 20; } ioExpander;
    } params;
    bool isGPIOPeripheral() const { return type != PeripheralType::IO_EXPANDER; }
    bool isExpanderChannel() const { return isGPIOPeripheral(); }
};
struct PeripheralRuntimeState {
    PeripheralStatus status = PeripheralStatus::PERIPHERAL_RUNNING;
    String lastError;
    uint32_t lastActivity = 0;
    struct { struct { GPIOState currentState = GPIOState::STATE_UNDEFINED; } gpio; } state;
};
struct RecursiveMutexGuard {
    explicit RecursiveMutexGuard(int, uint32_t = 0) {}
    bool isLocked() const { return true; }
};
static bool digitalInputLogicalLevel(bool high, bool invert) { return high != invert; }
template <size_t N> bool fixedIdEquals(const char (&stored)[N], const String& id) {
    return id.length() < N && std::strncmp(stored, id.c_str(), N) == 0;
}
static const char* ioExpanderUnavailableReason() {
    return ioShutdownRequested().load() ? "IO_SHUTDOWN_REQUESTED" : featureUnavailable;
}
struct TimedTransport : FastBee::IoExpanderTransport {
    bool writeBusy = false, readBusy = false, writeOk = true, readOk = true;
    uint32_t writeCost = nextProviderWriteCost, readCost = 0;
    unsigned writes = 0, reads = 0;
    uint8_t physical = 0xff;
    FastBee::IoExpanderResult writeByte(uint8_t value) override {
        if (writeBusy) return FastBee::IoExpanderResult::Busy;
        ++writes;
        ioNow += writeCost;
        if (!writeOk) return FastBee::IoExpanderResult::Error;
        physical = value;
        return FastBee::IoExpanderResult::Ok;
    }
    FastBee::IoExpanderResult readByte(uint8_t& value) override {
        if (readBusy) return FastBee::IoExpanderResult::Busy;
        ++reads;
        ioNow += readCost;
        value = physical;
        return readOk ? FastBee::IoExpanderResult::Ok : FastBee::IoExpanderResult::Error;
    }
};
namespace FastBee {
struct I2CBusLock {
    explicit I2CBusLock(uint32_t) {}
    bool locked() const { return true; }
};
struct IoExpanderProvider {
    TimedTransport transport;
    Pcf8574 driver{transport};
    bool _closing = false, leased = false;
    uint8_t shutdownByte = 0xff;
    IoExpanderResult result = IoExpanderResult::Error;
    explicit IoExpanderProvider(uint8_t) {}
    ~IoExpanderProvider() {
        lastDestroyedByte = transport.physical;
        if (leased) { ++destroyedWithLease; --liveLeases; }
    }
    bool acquire(uint8_t, uint8_t, String&) { leased = true; ++liveLeases; return true; }
    bool closing() const { return _closing; }
    IoExpanderResult shutdown(uint8_t safeByte) {
        if (!_closing) { _closing = true; shutdownByte = safeByte; driver.end(); }
        if (!leased) return result;
        result = driver.shutdown(shutdownByte);
        if (result != IoExpanderResult::Busy) { leased = false; --liveLeases; }
        return result;
    }
};
}
class PeripheralManager {
public:
    inline static PeripheralManager* active = nullptr;
    static PeripheralManager& getInstance() { assert(active); return *active; }
    std::map<String, PeripheralConfig> peripherals;
    std::map<String, PeripheralRuntimeState> runtimeStates;
    int _mutex = 0;
    bool validateIoDependencies(const PeripheralConfig&, const String&, String&) const;
    bool setupHardware(const PeripheralConfig&);
    bool teardownHardware(const PeripheralConfig&);
    void invalidateSensorConfigurationLocked() {} // Sensor generation has a separate concurrency harness.
    bool setupIoExpander(const PeripheralConfig&);
    bool teardownIoExpander(const String&);
    bool shutdownIoExpanders();
    bool syncIoExpander(const String&, const String& = "");
    void pollIoExpanders();
    GPIOState readGpioStateLocked(const PeripheralConfig&, const String*, const char*);
    bool writeGpioStateLocked(const PeripheralConfig&, const String*, const char*, GPIOState);
    bool togglePin(const String&);
    void stopActionTicker(const String&);
    bool resolveI2cConfig(const PeripheralConfig&, uint8_t& sda, uint8_t& scl,
                          uint32_t& frequency, String&) {
        sda = 21; scl = 22; frequency = 100000; return true;
    }
    GPIOState readPin(const String& id) {
        const auto item = peripherals.find(id);
        return item == peripherals.end() ? GPIOState::STATE_UNDEFINED
            : readGpioStateLocked(item->second, &id, nullptr);
    }
    bool writePin(const String& id, GPIOState state) {
        const auto item = peripherals.find(id);
        return item != peripherals.end() && writeGpioStateLocked(item->second, &id, nullptr, state);
    }
};

struct RebootEsp { bool restarted = false; void restart() { restarted = true; } } ESP;
struct RebootSerial : MockSerialClass { void flush() {} } Serial;
struct RestartDiagnostics { static void savePreRestartState(int, const char*) {} };
class SystemRebooter {
public:
    inline static bool _scheduled = false;
    inline static unsigned long _rebootAt = 0;
    inline static const char* _reason = "test";
    inline static int _restartReason = 0;
    static void update();
};

#include "io_manager_under_test.inc"

void PeripheralManager::stopActionTicker(const String& id) { ioBlinks().erase(id); }
static PeripheralConfig parentConfig(const char* id) {
    PeripheralConfig config;
    config.id = id;
    return config;
}
static PeripheralConfig outputConfig(const char* id, uint8_t channel, GPIOState startup) {
    PeripheralConfig config;
    config.id = id;
    config.type = PeripheralType::GPIO_DIGITAL_OUTPUT;
    std::strcpy(config.params.gpio.providerId, "provider");
    config.params.gpio.channel = channel;
    config.params.gpio.initialState = startup;
    return config;
}
static void remember(PeripheralManager& manager, const PeripheralConfig& config) {
    manager.peripherals[config.id] = config;
    manager.runtimeStates[config.id] = {};
}
static void clearRuntime(uint32_t now) {
    ioProviders().clear(); ioPendingSyncs().clear(); ioBlinks().clear();
    ioShutdownRequested().store(false);
    featureUnavailable = nullptr;
    shutdownWarnings = destroyedWithLease = liveLeases = 0;
    ioNow = now;
    nextProviderWriteCost = 0;
}

static void pending_retirement_blocks_only_reused_channel() {
    clearRuntime(1000);
    PeripheralManager manager;
    auto parent = parentConfig("provider");
    auto old = outputConfig("old", 0, GPIOState::STATE_HIGH);
    auto sibling = outputConfig("sibling", 1, GPIOState::STATE_LOW);
    remember(manager, parent); remember(manager, old); remember(manager, sibling);
    assert(manager.setupIoExpander(parent));
    auto& provider = *ioProviders().at("provider");
    assert(manager.writePin("old", GPIOState::STATE_LOW));
    assert((provider.transport.physical & 1) == 0);

    provider.transport.writeBusy = true;
    assert(manager.teardownHardware(old));
    manager.peripherals.erase("old"); manager.runtimeStates.erase("old");
    assert(ioPendingSyncs().at("provider") == 1);
    auto replacement = outputConfig("replacement", 0, GPIOState::STATE_HIGH);
    String error;
    assert(!manager.validateIoDependencies(replacement, replacement.id, error));
    assert(error.startsWith("IO_CONFIG_PENDING"));
    auto independent = outputConfig("independent", 2, GPIOState::STATE_HIGH);
    assert(manager.validateIoDependencies(independent, independent.id, error));

    // Also exercise the post-teardown update gate, after preflight has passed.
    remember(manager, replacement);
    const unsigned writes = provider.transport.writes;
    assert(!manager.setupHardware(replacement));
    assert(manager.runtimeStates[replacement.id].lastError == "IO_CONFIG_PENDING");
    assert(provider.transport.writes == writes && (provider.transport.physical & 1) == 0);
    manager.peripherals.erase(replacement.id); manager.runtimeStates.erase(replacement.id);

    provider.transport.writeBusy = false;
    assert(manager.writePin("sibling", GPIOState::STATE_HIGH));
    assert((provider.transport.physical & 2) != 0); // unaffected owner remains usable
    ioNow += 20;
    manager.pollIoExpanders();
    assert(ioPendingSyncs().empty());
    assert(manager.validateIoDependencies(replacement, replacement.id, error));
    remember(manager, replacement);
    assert(manager.setupHardware(replacement));
    assert(manager.readPin("replacement") == GPIOState::STATE_HIGH);
    assert((provider.transport.physical & 3) == 3); // new startup and sibling retained
}

static void maintenance_includes_missing_provider_setup_in_budget(uint32_t start) {
    clearRuntime(start);
    PeripheralManager manager;
    nextProviderWriteCost = 20; // one Wire timeout exceeds the cooperative 8 ms slice
    for (int i = 0; i < 4; ++i) remember(manager, parentConfig((String("provider") + String(i)).c_str()));
    manager.pollIoExpanders();
    assert(ioProviders().size() == 1);
    assert(static_cast<uint32_t>(ioNow - start) == 20);
}

static void blink_retries_busy_and_stale_but_stops_on_real_errors() {
    clearRuntime(10000);
    PeripheralManager manager;
    const auto parent = parentConfig("provider");
    const auto output = outputConfig("blink", 0, GPIOState::STATE_LOW);
    remember(manager, parent); remember(manager, output);
    assert(manager.setupIoExpander(parent));
    auto& provider = *ioProviders().at("provider");
    ioBlinks()[output.id] = {ioNow, 200};

    ioNow += 200;
    provider.transport.readBusy = provider.transport.writeBusy = true;
    manager.pollIoExpanders();
    assert(ioBlinks().count(output.id) == 1 && ioBlinks().at(output.id).last == 10000);
    assert(provider.driver.failures() == 0);
    provider.transport.readBusy = false;
    ioNow += 20;
    manager.pollIoExpanders(); // fresh sample, but toggle write still Busy
    assert(ioBlinks().count(output.id) == 1 && ioBlinks().at(output.id).last == 10000);
    assert((provider.transport.physical & 1) == 0);

    provider.transport.writeBusy = false;
    ioNow += 20;
    manager.pollIoExpanders();
    assert(ioBlinks().count(output.id) == 1 && ioBlinks().at(output.id).last == ioNow);
    assert((provider.transport.physical & 1) != 0);
    const unsigned writes = provider.transport.writes;
    ioNow += 20;
    manager.pollIoExpanders();
    assert(provider.transport.writes == writes); // no catch-up burst

    ioNow += 200;
    provider.transport.writeOk = false;
    manager.pollIoExpanders();
    assert(provider.driver.failures() == 1 && ioBlinks().empty());
}

static void invalid_blink_ownership_is_removed_without_waiting_for_deadline() {
    clearRuntime(20000);
    PeripheralManager manager;
    const auto parent = parentConfig("provider");
    auto output = outputConfig("blink", 0, GPIOState::STATE_LOW);
    remember(manager, parent); remember(manager, output);
    assert(manager.setupIoExpander(parent));
    ioBlinks()["deleted"] = {ioNow, 200};
    ioBlinks()[output.id] = {ioNow, 200};
    manager.runtimeStates[output.id].lastError = "CONFIG_INVALID: test";
    manager.pollIoExpanders();
    assert(ioBlinks().empty());
    manager.runtimeStates[output.id].lastError = "";
    manager.peripherals[output.id].enabled = false;
    ioBlinks()[output.id] = {ioNow, 200};
    manager.pollIoExpanders();
    assert(ioBlinks().empty());
}

static void parent_shutdown_restores_enabled_outputs_and_releases_other_bits() {
    clearRuntime(30000);
    PeripheralManager manager;
    const auto parent = parentConfig("provider");
    remember(manager, parent);
    auto low = outputConfig("low", 0, GPIOState::STATE_LOW);
    auto invertedHigh = outputConfig("inverted-high", 1, GPIOState::STATE_HIGH);
    invertedHigh.params.gpio.invertLogic = true;
    auto invertedLow = outputConfig("inverted-low", 2, GPIOState::STATE_LOW);
    invertedLow.params.gpio.invertLogic = true;
    auto disabled = outputConfig("disabled", 3, GPIOState::STATE_LOW);
    disabled.enabled = false;
    auto invalid = outputConfig("invalid", 4, GPIOState::STATE_LOW);
    auto input = outputConfig("input", 5, GPIOState::STATE_LOW);
    input.type = PeripheralType::GPIO_DIGITAL_INPUT;
    for (const auto& config : {low, invertedHigh, invertedLow, disabled, invalid, input}) remember(manager, config);
    manager.runtimeStates[invalid.id].lastError = "CONFIG_INVALID: test";
    assert(manager.setupIoExpander(parent));
    assert(manager.writePin(low.id, GPIOState::STATE_HIGH));
    assert(manager.writePin(invertedHigh.id, GPIOState::STATE_LOW));
    assert(manager.writePin(invertedLow.id, GPIOState::STATE_HIGH));
    ioBlinks()[low.id] = {ioNow, 200};
    assert(manager.teardownHardware(parent));
    assert(lastDestroyedByte == 0xfc); // P0/P1 LOW; inverted P2 and all other pins HIGH
    assert(ioProviders().empty() && liveLeases == 0 && destroyedWithLease == 0 && ioBlinks().empty());
    assert(manager.runtimeStates[low.id].state.gpio.currentState == GPIOState::STATE_UNDEFINED);
    assert(manager.runtimeStates[invalid.id].lastError.startsWith("CONFIG_INVALID:"));
}

static void parent_shutdown_busy_retries_and_errors_remain_unconfirmed() {
    clearRuntime(40000);
    PeripheralManager manager;
    const auto parent = parentConfig("provider");
    const auto output = outputConfig("output", 0, GPIOState::STATE_LOW);
    remember(manager, parent); remember(manager, output);
    assert(manager.setupIoExpander(parent));
    auto& provider = *ioProviders().at(parent.id);
    assert(manager.writePin(output.id, GPIOState::STATE_HIGH));
    provider.transport.writeBusy = true;
    assert(!manager.teardownHardware(parent));
    assert(provider.closing() && liveLeases == 1 && destroyedWithLease == 0);
    assert(manager.runtimeStates[parent.id].lastError.startsWith("IO_SHUTDOWN_UNCONFIRMED"));
    assert(!manager.writePin(output.id, GPIOState::STATE_LOW));
    assert(!manager.setupIoExpander(parent));
    manager.peripherals[parent.id].enabled = false;
    provider.transport.writeBusy = false;
    ioNow += 20;
    manager.pollIoExpanders();
    assert(ioProviders().empty() && liveLeases == 0 && lastDestroyedByte == 0xfe);
    assert(destroyedWithLease == 0);

    clearRuntime(50000);
    manager = PeripheralManager();
    remember(manager, parent); remember(manager, output);
    assert(manager.setupIoExpander(parent));
    ioProviders().at(parent.id)->transport.writeOk = false;
    assert(!manager.teardownHardware(parent));
    assert(ioProviders().empty() && liveLeases == 0 && destroyedWithLease == 0);
    assert(manager.runtimeStates[parent.id].lastError.startsWith("IO_SHUTDOWN_UNCONFIRMED"));
    assert(manager.runtimeStates[output.id].lastError.startsWith("IO_SHUTDOWN_UNCONFIRMED"));
    assert(!manager.shutdownIoExpanders()); // repeated shutdown cannot turn a failed ACK into success
    manager.pollIoExpanders();
    assert(manager.runtimeStates[parent.id].lastError.startsWith("IO_SHUTDOWN_UNCONFIRMED"));
    assert(shutdownWarnings != 0);
}

static void feature_disable_and_actual_rebooter_use_bounded_shutdown() {
    clearRuntime(60000);
    PeripheralManager manager;
    const auto parent = parentConfig("provider");
    const auto output = outputConfig("output", 0, GPIOState::STATE_LOW);
    remember(manager, parent); remember(manager, output);
    assert(manager.setupIoExpander(parent));
    assert(manager.writePin(output.id, GPIOState::STATE_HIGH));
    auto& provider = *ioProviders().at(parent.id);
    provider.transport.writeBusy = true;
    featureUnavailable = "IO_EXPANDER_DISABLED";
    manager.pollIoExpanders();
    assert(liveLeases == 1 && ioProviders().size() == 1);
    assert(manager.runtimeStates[parent.id].lastError.startsWith("IO_SHUTDOWN_UNCONFIRMED"));
    provider.transport.writeBusy = false;
    manager.pollIoExpanders();
    assert(ioProviders().empty() && liveLeases == 0 && lastDestroyedByte == 0xfe);

    for (bool busy : {false, true}) {
        clearRuntime(busy ? 80000 : 70000);
        manager = PeripheralManager();
        remember(manager, parent); remember(manager, output);
        assert(manager.setupIoExpander(parent));
        assert(manager.writePin(output.id, GPIOState::STATE_HIGH));
        ioProviders().at(parent.id)->transport.writeBusy = busy;
        PeripheralManager::active = &manager;
        ESP.restarted = false;
        SystemRebooter::_scheduled = true;
        SystemRebooter::_rebootAt = ioNow;
        SystemRebooter::update(); // production method, ESP.restart is the host seam
        assert(ESP.restarted && !SystemRebooter::_scheduled && ioShutdownRequested().load());
        if (busy) {
            assert(liveLeases == 1 && ioProviders().size() == 1);
            assert(manager.runtimeStates[parent.id].lastError.startsWith("IO_SHUTDOWN_UNCONFIRMED"));
            ioProviders().at(parent.id)->transport.writeBusy = false;
            manager.pollIoExpanders();
        }
        assert(ioProviders().empty() && liveLeases == 0 && lastDestroyedByte == 0xfe);
        ioNow += 2000;
        manager.pollIoExpanders();
        assert(ioProviders().empty()); // restart gate prevents background reinitialization
    }
}

int main() {
    pending_retirement_blocks_only_reused_channel();
    maintenance_includes_missing_provider_setup_in_budget(5000);
    blink_retries_busy_and_stale_but_stops_on_real_errors();
    invalid_blink_ownership_is_removed_without_waiting_for_deadline();
    parent_shutdown_restores_enabled_outputs_and_releases_other_bits();
    parent_shutdown_busy_retries_and_errors_remain_unconfirmed();
    feature_disable_and_actual_rebooter_use_bounded_shutdown();
    maintenance_includes_missing_provider_setup_in_budget(UINT32_MAX - 5);
}
