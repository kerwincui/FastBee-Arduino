#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include "core/SensorConfigEpoch.h"
#include "core/UartEventOrigin.h"
#include <atomic>
#include <cassert>
#include <cmath>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <memory>
#include <new>

#undef assert
#define assert(expression) do { if (!(expression)) { std::fprintf(stderr, "assertion failed: %s:%d: %s\n", __FILE__, __LINE__, #expression); std::exit(2); } } while (0)
#undef malloc
#undef free
static thread_local int failAllocationAfter = -1;
static thread_local bool failPreparationOnly = false;
static int allocationCase = -1;
__attribute__((noinline)) void* operator new(std::size_t size) {
    if (failAllocationAfter == 0) { failAllocationAfter = -1; throw std::bad_alloc(); }
    if (failAllocationAfter > 0) --failAllocationAfter;
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
__attribute__((noinline)) void operator delete(void* value) noexcept { std::free(value); }
__attribute__((noinline)) void operator delete(void* value, std::size_t) noexcept { std::free(value); }
static std::atomic<uint32_t> publicationNow{100};
static uint64_t publicationWrapBase = 0;
static int64_t esp_timer_get_time() { return static_cast<int64_t>(publicationWrapBase + publicationNow.load()) * 1000; }
static uint32_t publicationMillis() { return publicationNow.load(); }
#define millis publicationMillis

#define FASTBEE_ENABLE_PERIPH_EXEC 1
#define FASTBEE_ENABLE_POINT_HISTORY 1
using std::min;

struct TestSemaphore {
    std::timed_mutex cache;
    std::recursive_timed_mutex configuration;
};
using SemaphoreHandle_t = TestSemaphore*;
static thread_local unsigned configDepth = 0, cacheDepth = 0;
static thread_local bool pauseAfterCommit = false;
static std::atomic<bool> commitFinished{false}, continueClaim{false};
static BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout) {
    assert(timeout != portMAX_DELAY);
    if (!mutex || !mutex->cache.try_lock_for(std::chrono::milliseconds(timeout))) return pdFALSE;
    ++cacheDepth;
    return pdTRUE;
}
static BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
    assert(cacheDepth); --cacheDepth;
    mutex->cache.unlock();
    return pdTRUE;
}
static BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t timeout) {
    if (failPreparationOnly) { failAllocationAfter = -1; failPreparationOnly = false; }
    assert(!cacheDepth); // PM -> cache is the only permitted nesting order.
    assert(timeout != portMAX_DELAY);
    if (!mutex || !mutex->configuration.try_lock_for(std::chrono::milliseconds(timeout))) return pdFALSE;
    ++configDepth;
    return pdTRUE;
}
static BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t mutex) {
    assert(configDepth); --configDepth;
    mutex->configuration.unlock();
    if (!configDepth && pauseAfterCommit) {
        pauseAfterCommit = false;
        commitFinished = true;
        while (!continueClaim.load()) std::this_thread::yield();
    }
    return pdTRUE;
}
#include "publication_guards.inc"
#include "publication_types.inc"

static void assertOutsideLocks() { assert(!configDepth && !cacheDepth); }
static std::atomic<unsigned> sseCount{0}, ruleCount{0}, historyCount{0};
static std::atomic<bool> waitingForRules{false};
static std::mutex ruleMutex;
struct TestScheduler {
    FastBee::UartEventOrigin lastOrigin;
    void handlePollArray(const String& source, JsonArray& data, const FastBee::UartEventOrigin& origin = {}) {
        assert(source == "sensor_cache" && data.size() == 1);
        assert(data[0]["id"].is<const char*>() && data[0]["value"].is<const char*>());
        lastOrigin = origin;
        assertOutsideLocks();
        waitingForRules = true;
        std::lock_guard<std::mutex> ruleGuard(ruleMutex);
        ++ruleCount;
    }
};
namespace FastBee {
struct PointHistory {
    static PointHistory& instance() { static PointHistory value; return value; }
    void recordSample(const String&, double, const String&, const String&, uint32_t, uint32_t) {
        assertOutsideLocks(); ++historyCount;
    }
};
}
struct TestLogger {
    void warning(const char*) {}
    template<class... Args> void warningf(const char*, Args...) {}
} LOGGER;
constexpr unsigned long SENSOR_CACHE_LOCK_WARN_INTERVAL_MS = 5000;

struct PeripheralConfig { bool enabled = true; uint8_t address = 0x40; };
class PeripheralManager {
public:
    static PeripheralManager& getInstance() { static PeripheralManager value; return value; }
    TestSemaphore mutex;
    SemaphoreHandle_t _mutex = &mutex;
    uint64_t _configurationReviewVersion = 1;
    std::map<String, PeripheralConfig> peripherals{{"sensor", PeripheralConfig()}};
    SemaphoreHandle_t getMutex() const { return _mutex; }
    bool getPeripheralSnapshot(const String&, PeripheralConfig&, uint32_t* = nullptr) const;
    bool commitSensorRead(uint32_t, const std::function<bool()>&) const;
    void invalidateSensorConfigurationLocked();
    bool uartCurrent = true;
    bool isUartEventOriginCurrent(const FastBee::UartEventOrigin& origin) const {
        assert(!cacheDepth);
        return origin.unbound() || (uartCurrent && origin.matches(7,19));
    }
};
class PeriphExecManager {
public:
    static PeriphExecManager& getInstance() { static PeriphExecManager value; return value; }
#include "publication_cache.inc"
    static constexpr size_t SENSOR_CACHE_MAX_ENTRIES = 8;
    static constexpr unsigned long SENSOR_CACHE_TTL_MS = 120000;
    TestSemaphore mutex;
    SemaphoreHandle_t _sensorCacheMutex = &mutex;
    std::map<String, SensorReadCache> _sensorReadCache;
    uint32_t _nextObservationIdentity = 0, _nextSensorPublication = 0;
    TestScheduler scheduler;
    TestScheduler* _scheduler = &scheduler;
    bool updateSensorReadCache(const String&, const String&, const String&, const String&,
        const String& = "", const String& = "", uint32_t = 0, uint32_t = 0,
        bool = false, uint32_t = 0, const String& = "", uint32_t = 0,
        const FastBee::UartEventOrigin& = {}, uint64_t = UINT64_MAX);
    bool upsertObservation(const ObservationUpdate&, uint32_t = 0);
    bool upsertSensorObservationBatch(const ObservationUpdate* const*, size_t, uint32_t);
    void publishObservation(const ObservationUpdate&);
    void invalidateSensorReadCache(const String&, const String&, uint32_t = 0);
    void invalidateAllSensorReadCache();
    std::map<String, SensorReadCache> getSensorReadCacheCopy() const;
    bool lookupSensorCache(const String&, SensorReadCache&) const;
    bool getObservation(const String&, ObservationSnapshot&) const;
    std::vector<ObservationSnapshot> getObservationSnapshots(size_t = SENSOR_CACHE_MAX_ENTRIES) const;
    std::vector<ObservationSnapshot> getObservationSnapshots(const std::vector<const char*>&) const;
    void evictStaleSensorCache();
    void notifySensorDataSSE(const String&, const String&, const String&, const String&, const ObservationUpdate* = nullptr) {
        assertOutsideLocks(); ++sseCount;
    }
};
#include "publication_methods.inc"

static uint32_t capture() {
    uint32_t version = 0; PeripheralConfig config;
    assert(PeripheralManager::getInstance().getPeripheralSnapshot("sensor", config, &version));
    assert(version && config.enabled);
    return version;
}
static void changeConfiguration() {
    auto& pm = PeripheralManager::getInstance();
    RecursiveMutexGuard guard(pm.getMutex(), pdMS_TO_TICKS(50));
    assert(guard.isLocked());
    pm.invalidateSensorConfigurationLocked();
    ++pm.peripherals["sensor"].address;
}
static ObservationUpdate update() {
    ObservationUpdate value;
    value.sourceRef = "sensor_temperature"; value.periphId = "sensor";
    value.label = "temperature"; value.value = "23.5"; value.unit = "C";
    value.dataField = "temperature"; value.recordable = true;
    value.maxAgeMs = 10000;
    return value;
}
static void staleAdmissionAndFailure() {
    auto& manager = PeriphExecManager::getInstance();
    const auto oldVersion = capture();
    changeConfiguration();
    assert(!manager.upsertObservation(update(), oldVersion));
    assert(manager.getSensorReadCacheCopy().empty() && !sseCount);
    const auto current = capture();
    assert(manager.upsertObservation(update(), current));
    assert(sseCount == 1 && ruleCount == 1 && historyCount == 1);
    manager.invalidateSensorReadCache("sensor", "late error", oldVersion);
    PeriphExecManager::SensorReadCache sample;
    assert(manager.lookupSensorCache("sensor_temperature", sample));
    assert(sample.quality == ObservationQuality::GOOD);
    changeConfiguration();
    assert(!manager.lookupSensorCache("sensor_temperature", sample));
    ObservationSnapshot snapshot;
    assert(!manager.getObservation("sensor_temperature", snapshot));
    assert(manager.getObservationSnapshots().empty());
}
static void mutationBetweenCommitAndClaim() {
    auto& manager = PeriphExecManager::getInstance();
    const auto version = capture();
    const auto before = sseCount.load();
    commitFinished = false; continueClaim = false;
    std::thread worker([&] {
        pauseAfterCommit = true;
        assert(!manager.upsertObservation(update(), version));
    });
    while (!commitFinished.load()) std::this_thread::yield();
    changeConfiguration();
    continueClaim = true;
    worker.join();
    assert(sseCount == before && ruleCount == before && historyCount == before);
    assert(manager.getSensorReadCacheCopy().empty());
}
static void contendedCleanupStillHidesOldCache() {
    auto& manager = PeriphExecManager::getInstance();
    assert(manager.upsertObservation(update(), capture()));
    std::atomic<bool> held{false}, release{false};
    std::thread holder([&] {
        MutexGuard guard(manager._sensorCacheMutex, pdMS_TO_TICKS(10));
        assert(guard.isLocked()); held = true;
        while (!release.load()) std::this_thread::yield();
    });
    while (!held.load()) std::this_thread::yield();
    changeConfiguration(); // cache cleanup times out; epoch still advances.
    release = true; holder.join();
    assert(!manager._sensorReadCache.empty());
    assert(manager.getSensorReadCacheCopy().empty());
    assert(manager.getObservationSnapshots().empty());
    ObservationSnapshot snapshot;
    assert(!manager.getObservation("sensor_temperature", snapshot));
    manager.evictStaleSensorCache();
    assert(manager._sensorReadCache.empty());
}
static void callbacksNeverHoldConfigurationOrCache() {
    auto& manager = PeriphExecManager::getInstance();
    const auto version = capture();
    waitingForRules = false;
    ruleMutex.lock();
    std::thread worker([&] { assert(manager.upsertObservation(update(), version)); });
    while (!waitingForRules.load()) std::this_thread::yield();
    const auto start = std::chrono::steady_clock::now();
    changeConfiguration(); // would deadlock if publication held PM -> rules.
    assert(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250));
    ruleMutex.unlock();
    worker.join();
    assert(manager.getSensorReadCacheCopy().empty());
}
static void responseCopyAndUnboundCompatibility() {
    auto& pm = PeripheralManager::getInstance();
    auto& manager = PeriphExecManager::getInstance();
    const auto oldVersion = capture();
    changeConfiguration();
    bool copied = false;
    assert(!pm.commitSensorRead(oldVersion, [&] { copied = true; return true; }));
    assert(!copied);
    JsonDocument response;
    assert(pm.commitSensorRead(capture(), [&] {
        assert(configDepth && !cacheDepth);
        response["value"] = 23.5;
        copied = true; return true;
    }));
    assertOutsideLocks(); // A Web send uses this copied response outside guard.
    assert(copied && response["value"] == 23.5);
    auto independent = update(); independent.sourceRef = "network_value";
    assert(manager.upsertObservation(independent));
    changeConfiguration();
    PeriphExecManager::SensorReadCache result;
    assert(manager.lookupSensorCache("network_value", result));
    assert(result.recordable && result.sampleIdentity);
}
static std::vector<ObservationUpdate> pair(uint32_t timestamp, const char* value) {
    auto temperature = update();
    temperature.value = value; temperature.sampleTimestamp = timestamp;
    temperature.sampleIdentity = timestamp;
    auto humidity = temperature;
    humidity.sourceRef = "sensor_humidity"; humidity.label = "humidity";
    humidity.dataField = "humidity"; humidity.unit = "%RH";
    return {temperature, humidity};
}
static bool publishPair(std::vector<ObservationUpdate>& values, uint32_t epoch) {
    const ObservationUpdate* pointers[] = {&values[0], &values[1]};
    return PeriphExecManager::getInstance().upsertSensorObservationBatch(pointers, 2, epoch);
}
static const std::vector<const char*> pairSources{"sensor_temperature", "sensor_humidity"};
static void verifyPair(uint32_t timestamp, const char* value) {
    const auto observed = PeriphExecManager::getInstance().getObservationSnapshots(pairSources);
    assert(observed.size() == 2);
    assert(observed[0].sampleTimestamp == timestamp && observed[1].sampleTimestamp == timestamp);
    assert(observed[0].value == value && observed[1].value == value);
}
static void atomicPairPublicationAndSnapshot() {
    auto& manager = PeriphExecManager::getInstance();
    changeConfiguration();
    const auto epoch = capture();
    auto values = pair(100, "old");
    assert(publishPair(values, epoch));
    std::atomic<bool> done{false};
    std::thread reader([&] {
        while (!done.load()) {
            auto snapshot = manager.getObservationSnapshots(pairSources);
            if (snapshot.empty()) continue; // bounded lock timeout is unavailable, never a partial pair.
            assert(snapshot.size() == 2);
            assert(snapshot[0].sampleTimestamp == snapshot[1].sampleTimestamp);
            assert(snapshot[0].value == snapshot[1].value);
        }
    });
    for (uint32_t i = 101; i < 201; ++i) {
        publicationNow = i;
        values = pair(i, "new");
        assert(publishPair(values, epoch));
    }
    done = true; reader.join();
    verifyPair(200, "new");
    auto captured = manager.getObservationSnapshots(pairSources);
    publicationNow = 201; values = pair(201, "later");
    assert(publishPair(values, epoch));
    assert(captured[0].value == "new" && captured[1].value == "new");
    assert(captured[0].sampleTimestamp == 200 && captured[1].sampleTimestamp == 200);
}
static void batchFailuresLeavePreviousGroupAndTtlHonest() {
    auto& manager = PeriphExecManager::getInstance();
    changeConfiguration(); publicationNow = 100;
    const auto epoch = capture();
    auto old = pair(100, "old"); assert(publishPair(old, epoch));
    auto invalid = pair(100, "new"); invalid[1].sampleTimestamp = 99;
    assert(!publishPair(invalid, epoch)); verifyPair(100, "old");
    invalid = pair(100, "new"); invalid[1].periphId = "another";
    assert(!publishPair(invalid, epoch)); verifyPair(100, "old");
    invalid = pair(100, "new"); invalid[1].quality = ObservationQuality::ERROR;
    assert(!publishPair(invalid, epoch)); verifyPair(100, "old");
    invalid = pair(100, "new"); invalid[1].sourceRef = invalid[0].sourceRef;
    assert(!publishPair(invalid, epoch)); verifyPair(100, "old");
    invalid = pair(100, "new"); invalid[0].sampledAtMs = 100;
    invalid[1].sampledAtMs = UINT64_C(0x100000064);
    assert(!publishPair(invalid, epoch)); verifyPair(100, "old");
    publicationNow = 10100;
    auto observed = manager.getObservationSnapshots(pairSources);
    assert(observed.size() == 2 && !observed[0].stale && !observed[1].stale);
    publicationNow = 10101;
    observed = manager.getObservationSnapshots(pairSources);
    assert(observed.size() == 2 && observed[0].stale && observed[1].stale);
    assert(observed[0].sampleTimestamp == 100 && observed[1].sampleTimestamp == 100);
    assert(!publishPair(old, epoch)); // No timestamp refresh or enlarged TTL.
    publicationNow = 100;
    std::atomic<bool> held{false}, release{false};
    std::thread holder([&] {
        MutexGuard guard(manager._sensorCacheMutex, pdMS_TO_TICKS(10)); assert(guard.isLocked());
        held = true; while (!release.load()) std::this_thread::yield();
    });
    while (!held.load()) std::this_thread::yield();
    assert(!publishPair(old, epoch));
    release = true; holder.join(); verifyPair(100, "old");
    for (int i = 0; i < 6; ++i) {
        auto extra = update(); extra.sourceRef = String("extra") + String(i);
        assert(manager.upsertObservation(extra, epoch));
    }
    invalid = pair(100, "new"); invalid[1].sourceRef = "new_humidity";
    assert(!publishPair(invalid, epoch)); verifyPair(100, "old");
}
static void scanGroupKeepsAcquisitionTimesAndOwner() {
    auto& manager=PeriphExecManager::getInstance();
    changeConfiguration();publicationNow=110;
    const auto epoch=capture();
    auto values=pair(100,"scan");
    values[0].sampledAtMs=100;values[1].sampleTimestamp=105;values[1].sampledAtMs=105;
    values[0].sampleGroup=values[1].sampleGroup=73;
    assert(publishPair(values,epoch));
    const auto observed=manager.getObservationSnapshots(pairSources);
    assert(observed.size()==2 && observed[0].sampledAtMs==100 && observed[1].sampledAtMs==105);
    auto invalid=values;invalid[1].sampleGroup=0;assert(!publishPair(invalid,epoch));
    invalid=values;invalid[1].sampleGroup=74;assert(!publishPair(invalid,epoch));
    invalid=values;invalid[1].periphId="another";assert(!publishPair(invalid,epoch));
    invalid=values;invalid[1].quality=ObservationQuality::ERROR;assert(!publishPair(invalid,epoch));
    invalid=values;invalid[1].sampledAtMs=111;invalid[1].sampleTimestamp=111;assert(!publishPair(invalid,epoch));
    changeConfiguration();assert(!publishPair(values,epoch));
}
static void batchEpochAndAllocationFailure() {
    auto& manager = PeriphExecManager::getInstance();
    changeConfiguration(); publicationNow = 100;
    const auto epoch = capture(); auto values = pair(100, "sample");
    const auto callbacks = sseCount.load();
    commitFinished = false; continueClaim = false;
    std::thread worker([&] {
        pauseAfterCommit = true;
        assert(!publishPair(values, epoch));
    });
    while (!commitFinished.load()) std::this_thread::yield();
    changeConfiguration(); continueClaim = true; worker.join();
    assert(sseCount == callbacks && manager.getObservationSnapshots(pairSources).empty());
    assert(!publishPair(values, epoch));
    unsigned failures = 0;
    for (int allocation = 0; allocation < 16; ++allocation) {
        allocationCase = allocation;
        changeConfiguration(); const auto current = capture();
        failPreparationOnly = true;
        failAllocationAfter = allocation;
        const bool accepted = publishPair(values, current);
        failAllocationAfter = -1; failPreparationOnly = false;
        const auto snapshot = manager.getObservationSnapshots(pairSources);
        if (!accepted) { ++failures; assert(snapshot.empty()); }
        else { assert(snapshot.size() == 2); verifyPair(100, "sample"); }
    }
    assert(failures >= 2); // Both group storage and first-publication map nodes.
    auto old = pair(100, "old"); assert(publishPair(old, capture()));
    auto changed = pair(100, "a replacement value long enough to require String allocation");
    const auto current = capture();
    unsigned replacementFailures = 0;
    for (int allocation = 0; allocation < 8; ++allocation) {
        assert(publishPair(old, current));
        allocationCase = 100 + allocation;
        failPreparationOnly = true; failAllocationAfter = allocation;
        const bool accepted = publishPair(changed, current);
        failAllocationAfter = -1; failPreparationOnly = false;
        if (!accepted) { ++replacementFailures; verifyPair(100, "old"); }
        else verifyPair(100, changed[0].value.c_str());
    }
    assert(replacementFailures > 0);
    assert(publishPair(old, current));
    failAllocationAfter = 0;
    allocationCase = 200;
    assert(manager.getObservationSnapshots(pairSources).empty());
    failAllocationAfter = -1; verifyPair(100, "old");
}
static void sampleClockAndOrigin() {
    auto& manager=PeriphExecManager::getInstance();auto& pm=PeripheralManager::getInstance();
    changeConfiguration();publicationWrapBase=0;publicationNow=100;
    auto value=update();value.sampleTimestamp=0;value.sampledAtMs=0;
    assert(manager.upsertObservation(value,capture()));
    ObservationSnapshot snapshot;assert(manager.getObservation(value.sourceRef,snapshot));
    assert(snapshot.sampleTimestamp==0 && snapshot.sampledAtMs==0 && !snapshot.stale);
    publicationWrapBase=UINT64_C(0x100000000);
    assert(manager.getObservation(value.sourceRef,snapshot) && snapshot.stale);
    PeriphExecManager::SensorReadCache cache;assert(!manager.lookupSensorCache(value.sourceRef,cache));
    publicationWrapBase=0;value.sampledAtMs=100;
    value.uartOrigin=FastBee::UartEventOrigin::captured(7,19);pm.uartCurrent=false;
    const auto before=ruleCount.load();assert(!manager.upsertObservation(value,capture()) && ruleCount==before);
    pm.uartCurrent=true;assert(manager.upsertObservation(value,capture()));
    assert(manager.scheduler.lastOrigin.matches(7,19));
    pm.uartCurrent=false;manager.publishObservation(value);assert(ruleCount==before+1);
    pm.uartCurrent=true;
}
int main() {
    std::set_terminate([] { std::fprintf(stderr,"unexpected exception at allocation case %d, remaining %d\n",allocationCase,failAllocationAfter); std::exit(3); });
    staleAdmissionAndFailure(); mutationBetweenCommitAndClaim();
    contendedCleanupStillHidesOldCache(); callbacksNeverHoldConfigurationOrCache();
    responseCopyAndUnboundCompatibility();
    atomicPairPublicationAndSnapshot();
    batchFailuresLeavePreviousGroupAndTtlHonest();
    batchEpochAndAllocationFailure();
    scanGroupKeepsAcquisitionTimesAndOwner();
    sampleClockAndOrigin();
    assertOutsideLocks();
    std::printf("SensorReadCache host sizeof=%zu; group storage is bounded heap\n",sizeof(PeriphExecManager::SensorReadCache));
}
