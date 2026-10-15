// Compiles the production expander manager methods/branches extracted by the
// Python runner. Peripheral storage, clock and transport are the host seams;
// channel admission, synchronization, maintenance and Pcf8574 remain real code.
// Load the real JSON dependency before the Arduino fixture's allocator macros.
#include <ArduinoJson.h>
#define millis unused_arduino_millis
#define ESP unused_arduino_esp
#define Serial unused_arduino_serial
#include <Arduino.h>
#undef millis
#undef ESP
#undef Serial
#include <freertos/FreeRTOS.h>
#include "peripherals/DigitalIoProvider.h"
#include "core/FeatureInstanceRegistry.h"
#include "core/UartEventOrigin.h"
#include "core/PeripheralTypes.h"
#include "core/NativeOutputControl.h"
#include "core/SensorConfigEpoch.h"
#include "core/RuleOutputIdentity.h"
#include "core/H1ParameterContract.h"
#include <cassert>
#include <atomic>
#include <memory>
#include <new>

static uint32_t ioNow = 0;
static uint32_t nextProviderWriteCost = 0;
static const char* featureUnavailable = nullptr;
static unsigned shutdownWarnings = 0, liveLeases = 0, destroyedWithLease = 0;
static uint8_t lastDestroyedByte = 0xff;
static uint8_t retainedNeoPixelPin = 255;
// Only the SDK ownership observation is mocked; setupHardware keeps its real guard.
bool fastbeeNeoPixelProjectPinRetained(uint8_t pin) {
    return pin != 255 && pin == retainedNeoPixelPin;
}
#define LOG_WARNINGF(...) (++shutdownWarnings)
#define LOG_WARNING(...) (++shutdownWarnings)
#define LOG_ERROR(...) (++shutdownWarnings)
std::atomic<bool>& ioShutdownRequested();
unsigned long millis() { return ioNow; }
enum class GPIOState { STATE_UNDEFINED, STATE_LOW, STATE_HIGH };
// The production TU's private ADC helper is equivalent to this public predicate.
static bool isAdcInputType(PeripheralType type) { return isAnalogInputType(type); }
struct PeripheralConfig {
    String id;
    bool enabled = true;
    PeripheralType type = PeripheralType::IO_EXPANDER;
    uint8_t pinCount = 0;
    uint8_t pins[8] = {255, 255, 255, 255, 255, 255, 255, 255};
    // Non-display production semantics; this fixture never contains a screen.
    uint8_t configuredPinCount() const { return pinCount > 8 ? 8 : pinCount; }
    uint8_t configuredPinAt(uint8_t index) const {
        return index < pinCount && index < 8 ? pins[index] : 255;
    }
    struct {
        struct {
            char providerId[41] = {};
            uint8_t channel = 0;
            bool invertLogic = false;
            bool passive = false;
            GPIOState initialState = GPIOState::STATE_LOW;
        } gpio;
        struct {
            char driver[16] = "PCF8574";
            char resetPeripheralId[41] = {};
            uint8_t address = 0x20; uint16_t sampleInterval = 20;
        } ioExpander;
        struct { char oePeripheralId[41] = {}; } pca9685;
    } params;
    bool isGPIOPeripheral() const {
        return type == PeripheralType::GPIO_DIGITAL_INPUT || type == PeripheralType::GPIO_DIGITAL_OUTPUT;
    }
    bool isExpanderChannel() const { return isGPIOPeripheral(); }
    bool isModbusPeripheral() const { return false; }
};
struct PeripheralRuntimeState {
    PeripheralStatus status = PeripheralStatus::PERIPHERAL_RUNNING;
    String lastError;
    bool memorySuspended = false, memoryCleanupPending = false;
    bool controlOutputFault = false, controlOutputRetiring = false;
    FastBee::OrdinaryControl::Lease ordinaryControl;
    uint32_t lastActivity = 0;
    struct { struct { GPIOState currentState = GPIOState::STATE_UNDEFINED; } gpio; } state;
};
static bool observationLockBusy = false;
static uint32_t observationLockWait = 1;
using SemaphoreHandle_t = void*;
static SemaphoreHandle_t observedMutex = nullptr;
struct RecursiveMutexGuard {
    explicit RecursiveMutexGuard(int, uint32_t = 0) {}
    explicit RecursiveMutexGuard(SemaphoreHandle_t mutex, uint32_t wait) { observationLockWait = wait; observedMutex = mutex; }
    bool isLocked() const { return !observationLockBusy; }
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
// PCA runtime is exercised separately by pca9685_manager_harness. These seams
// check its aggregation into the IO manager without silently forcing success.
static FeatureInstanceSnapshot pwmSnapshot{FeatureInstanceState::NotInitialized, 0};
FeatureInstanceSnapshot pca9685InstanceSnapshotLocked() { return pwmSnapshot; }
struct I2CBusLock {
    explicit I2CBusLock(uint32_t) {}
    bool locked() const { return true; }
};
struct IoExpanderProvider : Pcf8574ProviderSession {
    TimedTransport transport;
    bool _closing = false, leased = false;
    uint8_t shutdownByte = 0xff;
    IoExpanderResult result = IoExpanderResult::Error;
    explicit IoExpanderProvider(uint8_t) : Pcf8574ProviderSession(transport) {}
    ~IoExpanderProvider() override {
        lastDestroyedByte = transport.physical;
        if (leased) { ++destroyedWithLease; --liveLeases; }
    }
    bool acquire(uint8_t, uint8_t, String&) { leased = true; ++liveLeases; return true; }
    bool closing() const override { return _closing; }
    IoExpanderResult shutdown(uint16_t safeHigh) override {
        if (!_closing) {
            if (safeHigh > 0xff) return IoExpanderResult::Error;
            _closing = true; shutdownByte = static_cast<uint8_t>(safeHigh); driver.end();
        }
        if (!leased) return result;
        result = driver.shutdown(shutdownByte);
        if (result != IoExpanderResult::Busy) { leased = false; --liveLeases; }
        return result;
    }
};
std::unique_ptr<DigitalIoProvider> createDigitalIoProvider(
        const PeripheralConfig& config, uint8_t sda, uint8_t scl, uint32_t, String& error) {
    // Factory seam for the existing PCF manager harness. The real MCP factory,
    // bus and RESET adapter require their own integration candidate.
    std::unique_ptr<IoExpanderProvider> result(new IoExpanderProvider(config.params.ioExpander.address));
    if (!result->acquire(sda, scl, error)) return nullptr;
    return result;
}
}
class PeripheralManager {
public:
    bool uartCurrent = true;
    bool isUartEventOriginCurrent(const FastBee::UartEventOrigin& origin) const {
        return origin.unbound() || (uartCurrent && origin.matches(7,19));
    }
    inline static PeripheralManager* active = nullptr;
    static PeripheralManager& getInstance() { assert(active); return *active; }
    uint64_t _nextOrdinaryControlOperation = 0;
    bool ordinaryDigitalClaimedLocked(const String&) const { return false; } // No native output instances.
    // This fixture verifies provider lifecycles, not admission policy. The real
    // PM arbiter and PCF writes run together in ordinary_control's IOX cases.
    bool ordinaryPermitCurrentLocked(const String& id, const FastBee::OrdinaryControl::Permit& permit) const {
        return runtimeStates.count(id) && permit.generation == 1 && permit.operation == 1 &&
            permit.actor.source == FastBee::OrdinaryControl::Source::Rule && permit.actor.identity == 1;
    }
    bool retireOrdinaryDigitalLocked(const String& id, uint64_t, bool keepSlot = false, bool = false) {
        return retireOrdinaryIoProducerLocked(id, keepSlot);
    }
    // IOX bridge: callers hold the PM mutex across allocation, retirement and I2C.
    bool ordinaryIoProducerPresentLocked(const String& id) const;
    bool prepareOrdinaryIoProducerLocked(const String& id, bool& inserted);
    void discardOrdinaryIoReservationLocked(const String& id, bool inserted);
    bool retireOrdinaryIoProducerLocked(const String& id, bool keepSlot = false);
    bool startOrdinaryIoProducerLocked(const String& id, uint16_t interval,
        const FastBee::OrdinaryControlContext& context,
        const FastBee::OrdinaryControl::Permit& permit);
    bool retireOrdinaryIoGroupLocked(const String& providerId);
    void serviceOrdinaryIoProducerLocked(const String& id);

    bool controlOutputIsClaimed(const String&) const { return false; } // No native control pins in this harness.
    bool managedPinConflict(const PeripheralConfig&) const { return false; } // No native pin backend in expander-only fixture.
    bool teardownAnalogInput(const PeripheralConfig&) { return false; } // Native ADC is outside expander-only dispatch.
    std::map<String, PeripheralConfig> peripherals;
    std::map<String, PeripheralRuntimeState> runtimeStates;
    int _mutex = 0;
    bool validateIoDependencies(const PeripheralConfig&, const String&, String&) const;
    bool setupHardware(const PeripheralConfig&);
    bool initHardware(const String& id) { return setupHardware(peripherals.at(id)); }
    bool teardownHardware(const PeripheralConfig&);
    void invalidateSensorConfigurationLocked() {} // Sensor generation has a separate concurrency harness.
    bool setupIoExpander(const PeripheralConfig&);
    bool teardownIoExpander(const String&);
    bool shutdownIoExpanders();
    bool pwmShutdownConfirmed = true;
    unsigned pwmShutdownCalls = 0, pwmServiceCalls = 0;
    bool shutdownPca9685() { ++pwmShutdownCalls; return pwmShutdownConfirmed; }
    void servicePca9685() { ++pwmServiceCalls; }
    bool validatePca9685Dependencies(const PeripheralConfig&, String&) const { return true; }
    bool stopPca9685(const String&) { return pwmShutdownConfirmed; }
    bool syncIoExpander(const String&, const String& = "");
    void pollIoExpanders();
    GPIOState readGpioStateLocked(const PeripheralConfig&, const String*, const char*);
    bool writeGpioStateLocked(const PeripheralConfig&, const String*, const char*, GPIOState,
        const FastBee::OrdinaryControl::Permit* = nullptr);
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
        FastBee::OrdinaryControl::Permit fixturePermit;
        fixturePermit.actor.source = FastBee::OrdinaryControl::Source::Rule;
        fixturePermit.actor.identity = fixturePermit.operation = fixturePermit.generation = 1;
        return item != peripherals.end() && writeGpioStateLocked(item->second, &id, nullptr, state, &fixturePermit);
    }
};

struct RebootEsp { bool restarted = false; void restart() { restarted = true; } } ESP;
struct RebootSerial : MockSerialClass { void flush() {} } Serial;
struct RestartDiagnostics { static void savePreRestartState(int, const char*) {} };
// The extracted rebooter stays intact. History/cache/panel are explicit seams;
// this fixture verifies IO shutdown, not persistence or panel confirmation.
namespace FastBee {
struct PointHistory {
    bool flushAccepted = true;
    unsigned flushCalls = 0;
    static PointHistory& instance() { static PointHistory value; return value; }
    bool flushNow(uint32_t timeout) { assert(timeout == 2000); ++flushCalls; return flushAccepted; }
};
}
struct ScreenPanelManager {
    bool restartWithoutHistory = false;
    unsigned saveFailures = 0;
    static ScreenPanelManager& instance() { static ScreenPanelManager value; return value; }
    bool consumeRestartWithoutHistorySave() {
        const bool approved = restartWithoutHistory; restartWithoutHistory = false; return approved;
    }
    void notifyRestartSaveFailure() { ++saveFailures; }
};
struct ConfigStorage {
    inline static bool initialized = true;
    unsigned flushCalls = 0;
    static bool isInitialized() { return initialized; }
    static ConfigStorage& getInstance() { static ConfigStorage value; return value; }
    void flushDirtyEntries(bool force) { assert(force); ++flushCalls; }
};
class SystemRebooter {
public:
    inline static bool _scheduled = false;
    inline static unsigned long _rebootAt = 0;
    inline static const char* _reason = "test";
    inline static int _restartReason = 0;
    static void update();
};

#include "io_manager_under_test.inc"

static FastBee::IoExpanderProvider& pcfFixture(const String& id) {
    return static_cast<FastBee::IoExpanderProvider&>(*ioProviders().at(id));
}

void PeripheralManager::stopActionTicker(const String& id) { ioBlinks().erase(id); }
static PeripheralConfig parentConfig(const char* id) {
    PeripheralConfig config;
    config.id = id;
    return config;
}
static PeripheralConfig wideParentConfig(const char* id) {
    auto config = parentConfig(id);
    // This fixture tests channel-level integration against a 16-bit provider;
    // actual MCP bus/RESET admission is covered by the dedicated MCP harness.
    std::strcpy(config.params.ioExpander.driver, "MCP23017");
    std::strcpy(config.params.ioExpander.resetPeripheralId, "reset");
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
static void fixtureBlink(PeripheralManager& manager, const String& id, uint16_t interval) {
    const auto& config = manager.peripherals.at(id);
    IoBlink blink;
    blink.last = ioNow; blink.interval = interval;
    blink.context.configEpoch = FastBee::SensorConfigEpoch::current();
    blink.permit.actor.source = FastBee::OrdinaryControl::Source::Rule;
    blink.permit.actor.identity = blink.permit.operation = blink.permit.generation = 1;
    blink.provider = ioProviders().at(config.params.gpio.providerId).get();
    blink.channel = config.params.gpio.channel;
    ioBlinks()[id] = blink;
}

static void clearRuntime(uint32_t now) {
    ioProviders().clear(); ioPendingSyncs().clear(); ioBlinks().clear();
    ioShutdownRequested().store(false);
    featureUnavailable = nullptr;
    shutdownWarnings = destroyedWithLease = liveLeases = 0;
    ioNow = now;
    nextProviderWriteCost = 0;
    retainedNeoPixelPin = 255;
    FastBee::PointHistory::instance() = FastBee::PointHistory{};
    ScreenPanelManager::instance() = ScreenPanelManager{};
    ConfigStorage::initialized = true;
    ConfigStorage::getInstance() = ConfigStorage{};
    FastBee::pwmSnapshot = {FastBee::FeatureInstanceState::NotInitialized, 0};
}

static void pca_shutdown_and_service_are_part_of_io_lifecycle() {
    clearRuntime(0);
    PeripheralManager manager;
    manager.pwmShutdownConfirmed = false;
    assert(!manager.shutdownIoExpanders() && manager.pwmShutdownCalls == 1);
    manager.pwmShutdownConfirmed = true;
    assert(manager.shutdownIoExpanders() && manager.pwmShutdownCalls == 2);
    ioShutdownRequested().store(false);
    manager.pollIoExpanders();
    assert(manager.pwmServiceCalls == 1);
}

static void pending_retirement_blocks_only_reused_channel() {
    clearRuntime(1000);
    PeripheralManager manager;
    auto parent = parentConfig("provider");
    auto old = outputConfig("old", 0, GPIOState::STATE_HIGH);
    auto sibling = outputConfig("sibling", 1, GPIOState::STATE_LOW);
    remember(manager, parent); remember(manager, old); remember(manager, sibling);
    assert(manager.setupIoExpander(parent));
    auto& provider = pcfFixture("provider");
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
    auto& provider = pcfFixture("provider");
    fixtureBlink(manager, output.id, 200);

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
    fixtureBlink(manager, output.id, 200);
    manager.runtimeStates[output.id].lastError = "CONFIG_INVALID: test";
    manager.pollIoExpanders();
    assert(ioBlinks().empty());
    manager.runtimeStates[output.id].lastError = "";
    manager.peripherals[output.id].enabled = false;
    fixtureBlink(manager, output.id, 200);
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
    fixtureBlink(manager, low.id, 200);
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
    auto& provider = pcfFixture(parent.id);
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
    pcfFixture(parent.id).transport.writeOk = false;
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
    auto& provider = pcfFixture(parent.id);
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
        pcfFixture(parent.id).transport.writeBusy = busy;
        PeripheralManager::active = &manager;
        ESP.restarted = false;
        SystemRebooter::_scheduled = true;
        SystemRebooter::_rebootAt = ioNow;
        SystemRebooter::update(); // production method, ESP.restart is the host seam
        assert(ESP.restarted && !SystemRebooter::_scheduled && ioShutdownRequested().load());
        if (busy) {
            assert(liveLeases == 1 && ioProviders().size() == 1);
            assert(manager.runtimeStates[parent.id].lastError.startsWith("IO_SHUTDOWN_UNCONFIRMED"));
            pcfFixture(parent.id).transport.writeBusy = false;
            manager.pollIoExpanders();
        }
        assert(ioProviders().empty() && liveLeases == 0 && lastDestroyedByte == 0xfe);
        ioNow += 2000;
        manager.pollIoExpanders();
        assert(ioProviders().empty()); // restart gate prevents background reinitialization
    }
}

static void passive_instance_observation() {
    using FastBee::FeatureInstanceState;
    clearRuntime(90000);
    PeripheralManager manager;
    int existingMutex = 0;
    std::atomic<SemaphoreHandle_t> mutex{&existingMutex};
    const auto observe = [&] { return observeIoExpanderInstances(&mutex); };
    assert(observeIoExpanderInstances(nullptr).state == FeatureInstanceState::Unobserved);
    assert(observe().state == FeatureInstanceState::NotInitialized);
    assert(observationLockWait == 0 && liveLeases == 0);
    int replacementMutex = 0;
    mutex.store(&replacementMutex);
    assert(observe().state == FeatureInstanceState::NotInitialized && observedMutex == &replacementMutex);
    mutex.store(nullptr);
    assert(observe().state == FeatureInstanceState::Unobserved);
    mutex.store(&existingMutex);
    const auto parent = parentConfig("provider");
    remember(manager, parent);
    assert(manager.setupIoExpander(parent));
    auto& provider = pcfFixture(parent.id);
    const auto writes = provider.transport.writes, reads = provider.transport.reads;
    for (int count = 0; count < 1000; ++count) {
        const auto snapshot = observe();
        assert(snapshot.state == FeatureInstanceState::Initialized && snapshot.initializedCount == 1);
    }
    assert(provider.transport.writes == writes && provider.transport.reads == reads);
    featureUnavailable = "IO_EXPANDER_DISABLED";
    assert(observe().state == FeatureInstanceState::Initialized); // intent is not release
    featureUnavailable = nullptr;
    ioNow += 10000; // stale input does not prove the initialized instance disappeared
    assert(!provider.driver.healthy(ioNow) && observe().initializedCount == 1);
    observationLockBusy = true;
    assert(observe().state == FeatureInstanceState::Busy && !observe().countKnown());
    observationLockBusy = false;
    assert(observationLockWait == 0);
    provider.transport.writeBusy = true;
    assert(!manager.teardownIoExpander(parent.id));
    assert(observe().state == FeatureInstanceState::CleanupPending && !observe().countKnown());
    provider.transport.writeBusy = false;
    manager.teardownIoExpander(parent.id);
    assert(observe().state == FeatureInstanceState::NotInitialized);
    // A retained provider whose first initialization failed is not initialized.
    assert(manager.setupIoExpander(parent));
    auto& retry = pcfFixture(parent.id);
    retry.driver.end(); retry.transport.writeOk = false;
    assert(!retry.driver.begin(0, 0xff, 20, ioNow));
    assert(observe().state == FeatureInstanceState::NotInitialized);
    retry.transport.writeOk = true; ioNow += 250;
    assert(retry.driver.poll(ioNow) && observe().initializedCount == 1);
    // Count actual providers, not channels/configuration entries.
    for (const char* id : {"second", "third", "fourth"}) {
        const auto config = parentConfig(id); remember(manager, config);
        assert(manager.setupIoExpander(config));
    }
    assert(observe().initializedCount == 4);
    assert(manager.shutdownIoExpanders());
    assert(observe().state == FeatureInstanceState::NotInitialized && liveLeases == 0);
}

// Deliberately NOT an MCP emulation: this records the actual PM contract calls
// for a 16-channel implementation. Register/reset behavior has separate tests.
struct WideObservation {
    uint16_t outputs = 0, inputs = 0, startup = 0, latch = 0, safe = 0;
    unsigned polls = 0, writes = 0, configurations = 0, stops = 0, destructions = 0;
};
struct WideProvider final : FastBee::DigitalIoProvider {
    WideObservation& trace;
    bool configBusy = false, stopBusy = false, rejectStop = false, isClosing = false;
    explicit WideProvider(WideObservation& t) : trace(t) {}
    ~WideProvider() override { ++trace.destructions; }
    uint16_t channelMask() const override { return 0xffff; }
    bool begin(uint16_t outputs, uint16_t startup, uint16_t, uint32_t now, uint16_t inputs) override {
        return configure(outputs, startup, now, inputs);
    }
    bool configure(uint16_t outputs, uint16_t startup, uint32_t, uint16_t inputs) override {
        if (configBusy || isClosing) return false;
        trace.latch = (trace.latch & trace.outputs & outputs) | (startup & outputs & ~trace.outputs);
        trace.outputs = outputs; trace.inputs = inputs; trace.startup = startup;
        ++trace.configurations; return true;
    }
    bool poll(uint32_t) override { ++trace.polls; return !isClosing; }
    bool writeChannel(uint8_t channel, bool high, uint32_t) override {
        if (isClosing || !supportsChannel(channel) || !(trace.outputs & (1U << channel))) return false;
        if (high) trace.latch |= uint16_t(1) << channel;
        else trace.latch &= ~(uint16_t(1) << channel);
        ++trace.writes; return true;
    }
    bool readChannel(uint8_t channel, bool output, uint32_t, bool& high) const override {
        if (isClosing || !supportsChannel(channel) ||
            !((output ? trace.outputs : trace.inputs) & (1U << channel))) return false;
        high = ((output ? trace.latch : trace.inputs) & (1U << channel)) != 0;
        return true;
    }
    bool healthy(uint32_t) const override { return !isClosing; }
    bool initialized() const override { return !isClosing; }
    uint32_t generation() const override { return 17; }
    uint32_t sampledAt() const override { return ioNow; }
    uint8_t failures() const override { return 0; }
    bool closing() const override { return isClosing; }
    FastBee::IoExpanderResult shutdown(uint16_t safe) override {
        ++trace.stops;
        if (rejectStop) return FastBee::IoExpanderResult::Error;
        if (!isClosing) { trace.safe = safe; isClosing = true; }
        return stopBusy ? FastBee::IoExpanderResult::Busy : FastBee::IoExpanderResult::Ok;
    }
};

static void wide_channels_survive_manager_configuration_read_write_and_blink() {
    clearRuntime(100000);
    PeripheralManager manager;
    const auto parent = wideParentConfig("provider");
    auto high = outputConfig("high", 15, GPIOState::STATE_LOW);
    auto low = outputConfig("low", 7, GPIOState::STATE_HIGH);
    auto input = outputConfig("input", 14, GPIOState::STATE_LOW);
    input.type = PeripheralType::GPIO_DIGITAL_INPUT;
    for (const auto& c : {parent, high, low, input}) remember(manager, c);
    WideObservation trace;
    ioProviders()[parent.id] = std::make_unique<WideProvider>(trace);
    assert(manager.syncIoExpander(parent.id));
    assert(trace.outputs == 0x8080 && trace.inputs == 0x4000 && trace.startup == 0x7fff);
    assert(manager.readPin(high.id) == GPIOState::STATE_LOW);
    assert(manager.readPin(low.id) == GPIOState::STATE_HIGH);
    assert(manager.readPin(input.id) == GPIOState::STATE_HIGH);
    assert(manager.writePin(high.id, GPIOState::STATE_HIGH) && trace.latch == 0x8080);
    assert(manager.writePin(low.id, GPIOState::STATE_LOW) && trace.latch == 0x8000);
    fixtureBlink(manager, high.id, 200);
    ioNow += 200;
    manager.pollIoExpanders();
    assert(trace.latch == 0 && trace.polls == 1 && ioBlinks().at(high.id).last == ioNow);
    ioBlinks().at(high.id).context.uartOrigin = FastBee::UartEventOrigin::captured(7,19);
    manager.uartCurrent = false;
    const unsigned writesBeforeRetirement = trace.writes;
    ioNow += 20; manager.pollIoExpanders();
    assert(ioBlinks().empty() && trace.writes == writesBeforeRetirement && trace.latch == 0);
    assert(manager.runtimeStates.at(high.id).lastError == "CONTROL_IO_SOURCE_OR_SESSION_STALE");
    manager.uartCurrent = true;
    ioNow += 20; manager.pollIoExpanders();
    assert(ioBlinks().empty() && trace.writes == writesBeforeRetirement);
    assert(manager.teardownIoExpander(parent.id));
    assert(trace.safe == 0x7fff && trace.destructions == 1 && ioProviders().empty());
}

static void high_channel_retirement_blocks_reuse_and_direct_io_without_aliasing() {
    clearRuntime(110000);
    PeripheralManager manager;
    const auto parent = wideParentConfig("provider");
    const auto high = outputConfig("old", 15, GPIOState::STATE_LOW);
    const auto low = outputConfig("sibling", 7, GPIOState::STATE_HIGH);
    for (const auto& c : {parent, high, low}) remember(manager, c);
    WideObservation trace;
    auto object = std::make_unique<WideProvider>(trace);
    auto& provider = *object;
    ioProviders()[parent.id] = std::move(object);
    assert(manager.syncIoExpander(parent.id));
    provider.configBusy = true;
    assert(manager.teardownHardware(high));
    assert(ioPendingSyncs().at(parent.id) == 0x8000);
    assert(ioChannelPending(high) && !ioChannelPending(low));
    const auto writes = trace.writes;
    assert(!manager.writePin(high.id, GPIOState::STATE_HIGH) && trace.writes == writes);
    assert(manager.readPin(high.id) == GPIOState::STATE_UNDEFINED);
    assert(manager.writePin(low.id, GPIOState::STATE_LOW));
    manager.peripherals.erase(high.id); manager.runtimeStates.erase(high.id);
    const auto replacement = outputConfig("new", 15, GPIOState::STATE_HIGH);
    String error;
    assert(!manager.validateIoDependencies(replacement, replacement.id, error));
    assert(error.startsWith("IO_CONFIG_PENDING"));
    provider.configBusy = false;
    manager.pollIoExpanders();
    assert(ioPendingSyncs().empty() && trace.outputs == 0x80);
    assert(manager.validateIoDependencies(replacement, replacement.id, error));
    remember(manager, replacement);
    assert(manager.setupHardware(replacement) && trace.latch == 0x8000);
    assert(manager.teardownIoExpander(parent.id) && trace.destructions == 1);
}

static void wide_shutdown_retains_pending_ownership_until_terminal_release() {
    clearRuntime(120000);
    PeripheralManager manager;
    const auto parent = wideParentConfig("provider");
    const auto output = outputConfig("output", 15, GPIOState::STATE_LOW);
    remember(manager, parent); remember(manager, output);
    WideObservation trace;
    auto object = std::make_unique<WideProvider>(trace);
    auto& provider = *object;
    ioProviders()[parent.id] = std::move(object);
    assert(manager.syncIoExpander(parent.id));
    ioPendingSyncs()[parent.id] = 0x8000;
    provider.rejectStop = true;
    assert(!manager.teardownIoExpander(parent.id));
    assert(trace.destructions == 0 && !provider.closing() && ioPendingSyncs().at(parent.id) == 0x8000);
    provider.rejectStop = false; provider.stopBusy = true;
    assert(!manager.teardownIoExpander(parent.id));
    assert(trace.safe == 0x7fff && provider.closing() && trace.destructions == 0);
    assert(ioPendingSyncs().at(parent.id) == 0x8000);
    manager.peripherals[output.id].params.gpio.initialState = GPIOState::STATE_HIGH;
    provider.stopBusy = false;
    assert(manager.teardownIoExpander(parent.id));
    assert(trace.safe == 0x7fff && trace.destructions == 1 && ioPendingSyncs().empty());
}

static void narrow_provider_rejects_wide_child_without_reconfiguring_siblings() {
    clearRuntime(130000);
    PeripheralManager manager;
    const auto parent = parentConfig("provider");
    const auto low = outputConfig("low", 0, GPIOState::STATE_HIGH);
    remember(manager, parent); remember(manager, low);
    assert(manager.setupIoExpander(parent));
    auto& provider = pcfFixture(parent.id);
    const auto writes = provider.transport.writes;
    for (uint8_t channel : {8, 15, 16, 255}) {
        const auto invalid = outputConfig("invalid", channel, GPIOState::STATE_LOW);
        String error;
        assert(!manager.validateIoDependencies(invalid, invalid.id, error));
        assert(error.startsWith("IO_CHANNEL_INVALID"));
        remember(manager, invalid);
        assert(!manager.syncIoExpander(parent.id));
        assert(!manager.writePin(invalid.id, GPIOState::STATE_LOW));
        assert(manager.readPin(invalid.id) == GPIOState::STATE_UNDEFINED);
        assert(provider.transport.writes == writes && provider.transport.physical == 0xff);
        manager.peripherals.erase(invalid.id); manager.runtimeStates.erase(invalid.id);
    }
    assert(manager.teardownIoExpander(parent.id));
}

static void setup_rejects_foreign_retained_native_pin_before_provider_acquisition() {
    clearRuntime(140000);
    PeripheralManager manager;
    auto parent = parentConfig("provider");
    parent.pinCount = 2; parent.pins[0] = 21; parent.pins[1] = 22;
    remember(manager, parent);
    retainedNeoPixelPin = 21;
    assert(!manager.setupHardware(parent));
    assert(manager.runtimeStates[parent.id].lastError == "NEOPIXEL_RETAINED_PIN_RESTART_REQUIRED");
    assert(ioProviders().empty() && liveLeases == 0);
    retainedNeoPixelPin = 255;
    assert(manager.setupHardware(parent));
    assert(ioProviders().size() == 1 && liveLeases == 1);
    assert(manager.teardownIoExpander(parent.id));
}

int main() {
    setup_rejects_foreign_retained_native_pin_before_provider_acquisition();
    pca_shutdown_and_service_are_part_of_io_lifecycle();
    wide_channels_survive_manager_configuration_read_write_and_blink();
    high_channel_retirement_blocks_reuse_and_direct_io_without_aliasing();
    wide_shutdown_retains_pending_ownership_until_terminal_release();
    narrow_provider_rejects_wide_child_without_reconfiguring_siblings();
    passive_instance_observation();
    pending_retirement_blocks_only_reused_channel();
    maintenance_includes_missing_provider_setup_in_budget(5000);
    blink_retries_busy_and_stale_but_stops_on_real_errors();
    invalid_blink_ownership_is_removed_without_waiting_for_deadline();
    parent_shutdown_restores_enabled_outputs_and_releases_other_bits();
    parent_shutdown_busy_retries_and_errors_remain_unconfirmed();
    feature_disable_and_actual_rebooter_use_bounded_shutdown();
    maintenance_includes_missing_provider_setup_in_budget(UINT32_MAX - 5);
}
