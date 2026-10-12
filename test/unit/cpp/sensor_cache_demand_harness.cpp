#include <Arduino.h>
#include <atomic>
#include <memory>
#include <new>
#include "core/interfaces/ISensorDriver.h"
#include "core/SensorConfigEpoch.h"
#include "core/SensorSampleMetadata.h"
#define private public
#include "core/SensorDriverCache.h"
#undef private
#include "core/DriverRegistry.h"
#include "core/FeatureInstanceRegistry.h"
#include "core/RuntimeFeatureManager.h"
#include "core/I2CBusManager.h"
#include <cassert>
#include <iostream>

using namespace FastBee;
static bool failEntryAllocation = false;
static unsigned entryAllocations = 0, constructed = 0, destroyed = 0, deinits = 0, reads = 0, inits = 0;
static bool pendingDriver = false, failInit = false, failRead = false;
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    if (bytes == sizeof(SensorDriverCache::Entry)) {
        if (failEntryAllocation) return nullptr;
        ++entryAllocations;
    }
    try { return ::operator new(bytes); } catch (...) { return nullptr; }
}

class DemandSensor final : public ISensorDriver {
    const bool pending = pendingDriver;
public:
    DemandSensor() { ++constructed; }
    ~DemandSensor() override { ++destroyed; }
    const char* getName() const override { return "Demand"; }
    uint8_t getChannelCount() const override { return 2; }
    const char* getChannelName(uint8_t channel) const override { return channel ? "second" : "first"; }
    const char* getChannelUnit(uint8_t) const override { return "unit"; }
    unsigned long getMinInterval() const override { return 100; }
    uint32_t cooperativeReadBudgetMs() const override { return pending ? 600 : 0; }
    bool init(uint8_t, const char*) override {
        ++inits;
        String error;
        return !failInit && I2CBusManager::instance().acquire(this, 21, 22, 100000, error);
    }
    void deinit() override { ++deinits; I2CBusManager::instance().release(this); }
    bool read(SensorReading& reading) override {
        ++reads;
        if (failRead) return false;
        reading.success = true; reading.channelCount = 2; reading.timestamp = millis();
        reading.values[0] = 0; reading.values[1] = 7;
        return true;
    }
    SensorReadStep readStep(SensorReading&) override { return SensorReadStep::Pending; }
    const char* getLastError() const override { return failInit ? "TEST_INIT_FAILED" : "TEST_READ_FAILED"; }
};
static ISensorDriver* createDemand() { return new DemandSensor(); }
static SensorDriverMetadata metadata = [] {
    SensorDriverMetadata value{}; value.name = "Demand"; value.featureKey = "i2cSensors"; return value;
}();

static size_t allocated(const SensorDriverCache& cache) {
    size_t result = 0;
    for (const auto& slot : cache._entries) if (slot) ++result;
    return result;
}
static void replacementDuringYield() {
    auto& cache = SensorDriverCache::instance();
    cache.release("cooperative");
    pendingDriver = false;
    float value; String unit, error;
    assert(cache.readChannel("cooperative", "Demand", 1, "{}", "first", value, unit, error));
    assert(value == 0 && allocated(cache) == 1);
}

int main() {
    static_assert(SensorDriverCache::MAX_ENTRIES == 6, "Existing cache capacity must be retained");
    static_assert(sizeof(SensorDriverCache) < 6 * sizeof(SensorDriverCache::Entry), "Samples cannot be resident inline");
    auto& cache = SensorDriverCache::instance();
    assert(allocated(cache) == 0 && entryAllocations == 0);
    assert(FeatureInstanceRegistry::instance().observe("i2cSensors").state == FeatureInstanceState::NotInitialized);
    assert(DriverRegistry::getInstance().registerDriver("Demand", createDemand, &metadata));
    static const int oled = 0;
    String busError;
    assert(I2CBusManager::instance().acquire(&oled, 21, 22, 100000, busError));
    float value = 123; String unit, error; uint32_t timestamp = 99;
    failEntryAllocation = true;
    assert(!cache.readChannel("oom", "Demand", 1, "{}", "first", value, unit, error, 1000, &timestamp));
    assert(std::isnan(value) && timestamp == 0 && error == "SENSOR_DRIVER_MEMORY_UNAVAILABLE");
    assert(constructed == 0 && allocated(cache) == 0 && I2CBusManager::instance().clientCount() == 1);
    failEntryAllocation = false;
    assert(cache.readChannel("warm", "Demand", 1, "{}", "first", value, unit, error));
    assert(value == 0 && entryAllocations == 1);
    assert(cache.readChannel("warm", "Demand", 1, "{}", "second", value, unit, error));
    assert(value == 7 && entryAllocations == 1 && reads == 1);
    failEntryAllocation = true;
    assert(!cache.readChannel("second-oom", "Demand", 1, "{}", "first", value, unit, error, 1000, &timestamp));
    assert(error == "SENSOR_DRIVER_MEMORY_UNAVAILABLE" && std::isnan(value) && timestamp == 0);
    assert(cache.activeCount() == 1 && allocated(cache) == 1);
    assert(cache.readChannel("warm", "Demand", 1, "{}", "first", value, unit, error) && value == 0);
    assert(entryAllocations == 1 && reads == 1); // Admission failure preserves the existing sample.
    failEntryAllocation = false;
    cache.release("warm");
    assert(allocated(cache) == 0 && destroyed == 1 && deinits == 1);
    assert(I2CBusManager::instance().clientCount() == 1); // OLED retains its independent lease.

    for (unsigned round = 0; round < 16; ++round) {
        for (unsigned i = 0; i < SensorDriverCache::MAX_ENTRIES; ++i) {
            sensorTestNow += 1;
            assert(cache.readChannel(String("fill") + String(i), "Demand", 1, "{}", "first", value, unit, error));
        }
        const unsigned beforeEviction = entryAllocations;
        assert(allocated(cache) == 6 && cache.activeCount() == 6);
        assert(cache.readChannel("seventh", "Demand", 1, "{}", "first", value, unit, error));
        assert(entryAllocations == beforeEviction && allocated(cache) == 6 && cache.activeCount() == 6);
        cache.releaseAll(); cache.releaseAll();
        assert(allocated(cache) == 0 && constructed == destroyed && I2CBusManager::instance().clientCount() == 1);
    }
    assert(!cache.readChannel("unknown", "Unregistered", 1, "{}", "first", value, unit, error));
    assert(allocated(cache) == 0 && std::isnan(value));
    failInit = true;
    const unsigned initBefore = inits;
    assert(!cache.readChannel("failed", "Demand", 1, "{}", "first", value, unit, error, 1000));
    assert(!cache.readChannel("failed", "Demand", 1, "{}", "second", value, unit, error, 1000));
    assert(error == "TEST_INIT_FAILED" && inits == initBefore + 1 && allocated(cache) == 1);
    failInit = false; sensorTestNow += 1000;
    assert(cache.readChannel("failed", "Demand", 1, "{}", "first", value, unit, error, 1000));
    failRead = true; sensorTestNow += 1000;
    assert(!cache.readChannel("failed", "Demand", 1, "{}", "first", value, unit, error, 1000));
    const unsigned failedReads = reads;
    assert(!cache.readChannel("failed", "Demand", 1, "{}", "second", value, unit, error, 1000));
    assert(reads == failedReads && allocated(cache) == 1 && error == "TEST_READ_FAILED");
    SensorReading snapshot; bool stale;
    assert(cache.getSnapshot("failed", snapshot, stale, error) && stale && !snapshot.success);
    failRead = false;
    RuntimeFeatureManager::instance().enabled = false;
    assert(!cache.readChannel("failed", "Demand", 1, "{}", "first", value, unit, error));
    assert(error == "I2C_SENSORS_UNAVAILABLE" && allocated(cache) == 0);
    RuntimeFeatureManager::instance().enabled = true;

    pendingDriver = true;
    const uint64_t beforeToken = cache._nextReadToken;
    sensorCacheYieldHook = replacementDuringYield;
    assert(!cache.readChannel("cooperative", "Demand", 1, "{}", "first", value, unit, error));
    assert(error == "SENSOR_SCAN_CANCELLED" && std::isnan(value) && cache._nextReadToken > beforeToken);
    assert(cache.getSnapshot("cooperative", snapshot, stale, error) && !stale && snapshot.success && snapshot.values[0] == 0);
    const uint64_t completedToken = cache._nextReadToken;
    cache.releaseAll();
    assert(cache._nextReadToken == completedToken && allocated(cache) == 0 && constructed == destroyed);
    assert(I2CBusManager::instance().clientCount() == 1);
    std::cout << "sensor cache demand lifecycle checks passed; host entry bytes="
              << sizeof(SensorDriverCache::Entry) << "; owner bytes=" << sizeof(SensorDriverCache) << '\n';
}
