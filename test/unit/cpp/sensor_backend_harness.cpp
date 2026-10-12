#include "core/DriverRegistry.h"
#include "core/SensorDriverCache.h"
#include "core/FeatureInstanceRegistry.h"
#include <freertos/semphr.h>
#include "core/SensorConfigSupport.h"
#include "core/RuntimeFeatureManager.h"
#include "peripherals/drivers/INA226Driver.h"
#include "peripherals/drivers/SHT31Driver.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>
#include <atomic>
#include <esp_timer.h>

using namespace FastBee;
static unsigned reads = 0, inits = 0, deinits = 0;
static bool failRead = false, nanRead = false, failInit = false;
static std::atomic<bool> pauseRead{false}, readEntered{false}, resumeRead{false};
class CountedSensor final : public ISensorDriver {
public:
    const char* getName() const override { return "Counted"; }
    uint8_t getChannelCount() const override { return 2; }
    const char* getChannelName(uint8_t channel) const override { return channel ? "second" : "first"; }
    const char* getChannelUnit(uint8_t) const override { return "unit"; }
    bool init(uint8_t, const char*) override { ++inits; return !failInit; }
    bool read(SensorReading& value) override {
        ++reads;
        if (pauseRead) {
            readEntered = true;
            while (!resumeRead.load()) std::this_thread::yield();
        }
        if (failRead) return false;
        value.success = true; value.channelCount = 2; value.timestamp = millis();
        value.values[0] = nanRead ? NAN : float(reads);
        value.values[1] = float(reads * 10);
        return true;
    }
    void deinit() override { ++deinits; }
    unsigned long getMinInterval() const override { return 100; }
};
static ISensorDriver* createCounted() { return new CountedSensor; }

static void metadataTests() {
    const char* names[] = {"AHT20", "BH1750", "BMP280", "MPU6050", "SHT30", "SHT31", "INA226"};
    for (const char* name : names) {
        const auto* meta = sensorDriverMetadata(name);
        assert(meta && meta->supportsAddress(meta->defaultAddress));
        assert(meta->activeChannelCount() <= 4);
        assert(strcmp(meta->featureKey, "i2cSensors") == 0);
        assert(DriverRegistry::getInstance().getMetadata(name));
    }
    assert(!sensorDriverMetadata("BH1750")->supportsAddress(0x24));
    assert(!sensorDriverMetadata("SHT31")->supportsAddress(0x43));
    const auto* mpu = sensorDriverMetadata("mpu6050");
    assert(mpu->activeChannelCount(0) == 4 && mpu->activeChannelCount(1) == 4);
    assert(strcmp(mpu->getChannel(0, 1)->name, "gyroX") == 0);
    assert(strcmp(mpu->getChannel(3, 1)->name, "temperature") == 0);
    assert(DriverRegistry::getInstance().getMetadata("ina226"));
    assert(!DriverRegistry::getInstance().getMetadata("not-compiled"));
}

static void cacheTests() {
    sensorTestWrapBase = 0;
    assert(FeatureInstanceRegistry::instance().observe("i2cSensors").state == FeatureInstanceState::Unobserved);
    const auto coldMutexCount = sensorTestMutexCreates.load();
    auto& cache = SensorDriverCache::instance();
    const auto cold = FeatureInstanceRegistry::instance().observe("i2cSensors");
    assert(cold.state == FeatureInstanceState::NotInitialized && cold.initializedCount == 0);
    assert(sensorTestMutexCreates == coldMutexCount);
    auto& registry = DriverRegistry::getInstance();
    assert(registry.registerDriver("Counted", createCounted));
    // Exercise concurrent first use too, before the cache has any mutex.
    std::thread coldA([&] { float v; String u, e; assert(cache.readChannel("cold", "Counted", 1, "{}", "first", v, u, e)); });
    std::thread coldB([&] { float v; String u, e; assert(cache.readChannel("cold", "Counted", 1, "{}", "second", v, u, e)); });
    coldA.join(); coldB.join();
    assert(reads == 1 && inits == 1);
    cache.releaseAll();
    reads = inits = deinits = 0;
    float value = 0; String unit, error; uint32_t timestamp = 0;
    assert(cache.readChannel("s1", "Counted", 1, "{}", "first", value, unit, error, 1000, &timestamp));
    assert(value == 1 && timestamp == sensorTestNow);
    assert(cache.readChannel("s1", "Counted", 1, "{}", "second", value, unit, error, 1000));
    assert(value == 10 && reads == 1 && inits == 1);
    assert(!cache.readChannel("s1", "Counted", 1, "{}", "missing", value, unit, error, 1000));
    assert(std::isnan(value) && reads == 1);
    sensorTestNow += 1000;
    failRead = true;
    assert(!cache.readChannel("s1", "Counted", 1, "{}", "first", value, unit, error, 1000));
    assert(std::isnan(value) && reads == 2);
    assert(!cache.readChannel("s1", "Counted", 1, "{}", "second", value, unit, error, 1000));
    assert(reads == 2);
    SensorReading snapshot; bool stale = false;
    assert(cache.getSnapshot("s1", snapshot, stale, error) && stale && !snapshot.success);
    sensorTestNow += 1000; failRead = false;
    assert(cache.readChannel("s1", "Counted", 1, "{}", "first", value, unit, error, 1000));
    assert(value == 3 && inits == 2);
    assert(cache.readChannel("s1", "Counted", 1, "{\"addr\":65}", "first", value, unit, error, 1000));
    assert(inits == 3 && deinits == 1 && value == 4);
    SensorConfigEpoch::advance();
    const uint32_t version = SensorConfigEpoch::current();
    assert(cache.readChannel("s1", "Counted", 1, "{\"addr\":65}", "first", value, unit, error, 1000, nullptr, version));
    assert(inits == 4 && deinits == 2 && value == 5);
    sensorTestNow += 1000; nanRead = true;
    assert(!cache.readChannel("s1", "Counted", 1, "{\"addr\":65}", "first", value, unit, error, 1000, nullptr, version));
    assert(!cache.getSnapshot("unseen", snapshot, stale, error));
    nanRead = false;
    cache.releaseAll();
    const unsigned before = reads;
    sensorTestNow = 0xfffffff0U;
    assert(cache.readChannel("wrap", "Counted", 1, "{}", "first", value, unit, error, 100));
    sensorTestNow = 25;
    sensorTestWrapBase = UINT64_C(0x100000000);
    assert(cache.readChannel("wrap", "Counted", 1, "{}", "second", value, unit, error, 100));
    assert(reads == before + 1);
    sensorTestNow = 200;
    assert(cache.readChannel("wrap", "Counted", 1, "{}", "second", value, unit, error, 100));
    assert(reads == before + 2);
    for (int i = 0; i < 8; ++i) {
        assert(cache.readChannel(String("fill") + String(i), "Counted", 1, "{}", "first", value, unit, error));
        sensorTestNow += 2;
    }
    assert(cache.activeCount() == SensorDriverCache::MAX_ENTRIES);
    cache.releaseAll();
    assert(cache.activeCount() == 0);
    // Actual cache synchronization: multiple rule workers requesting different
    // channels of one sensor must share one physical sample.
    const unsigned concurrentBefore = reads;
    std::thread first([&] { float v; String u, e; assert(cache.readChannel("concurrent", "Counted", 1, "{}", "first", v, u, e, 1000)); });
    std::thread second([&] { float v; String u, e; assert(cache.readChannel("concurrent", "Counted", 1, "{}", "second", v, u, e, 1000)); });
    first.join(); second.join();
    assert(reads == concurrentBefore + 1);
    cache.releaseAll();
    failInit = true;
    const unsigned initBefore = inits;
    assert(!cache.readChannel("unplugged", "Counted", 1, "{}", "first", value, unit, error, 1000));
    assert(!cache.readChannel("unplugged", "Counted", 1, "{}", "second", value, unit, error, 1000));
    assert(inits == initBefore + 1 && std::isnan(value));
    sensorTestNow += 1000;
    failInit = false;
    assert(cache.readChannel("unplugged", "Counted", 1, "{}", "second", value, unit, error, 1000));
    assert(inits == initBefore + 2);
    cache.releaseAll();
    sensorTestWrapBase = 0;
}

static void collectionFreshnessTests() {
    auto& cache = SensorDriverCache::instance();
    cache.releaseAll();
    sensorTestWrapBase = 0;
    float value; String unit, error; uint32_t timestamp;
    const unsigned before = reads;
    sensorTestNow = 10050; // previous dispatch at 10000, conversion completed 50 ms later
    uint32_t firstRound = 10000;
    assert(cache.readChannel("periodic", "Counted", 1, "{}", "first", value,
        unit, error, 10000, &timestamp, 0, &firstRound));
    assert(timestamp == 10050 && reads == before + 1);
    sensorTestNow = 20000; // next dispatch is due, old cache is still 50 ms from expiry
    uint32_t nextRound = sensorTestNow;
    assert(cache.readChannel("periodic", "Counted", 1, "{}", "first", value,
        unit, error, 10000, &timestamp, 0, &nextRound));
    assert(timestamp == 20000 && reads == before + 2);
    sensorTestNow += 10;
    assert(cache.readChannel("periodic", "Counted", 1, "{}", "second", value,
        unit, error, 10000, &timestamp, 0, &nextRound));
    assert(timestamp == 20000 && reads == before + 2); // same physical sample
    uint32_t overlappingRound = sensorTestNow;
    assert(cache.readChannel("periodic", "Counted", 1, "{}", "first", value,
        unit, error, 10000, &timestamp, 0, &overlappingRound));
    assert(timestamp == 20000 && reads == before + 2); // protect driver minimum interval
    sensorTestNow += 200;
    assert(cache.readChannel("periodic", "Counted", 1, "{}", "first", value,
        unit, error, 10000, &timestamp));
    assert(timestamp == 20000 && reads == before + 2); // ordinary reads preserve cache policy
    uint32_t newerRound = sensorTestNow;
    assert(cache.readChannel("periodic", "Counted", 1, "{}", "first", value,
        unit, error, 10000, &timestamp, 0, &newerRound));
    assert(timestamp == sensorTestNow && reads == before + 3);
    assert(cache.readChannel("periodic", "Counted", 1, "{}", "second", value,
        unit, error, 10000, &timestamp, 0, &nextRound));
    assert(timestamp == sensorTestNow && reads == before + 3); // delayed older round accepts newer sample
    SensorReading snapshot; bool stale;
    sensorTestNow += 10000;
    assert(cache.getSnapshot("periodic", snapshot, stale, error));
    assert(stale && !snapshot.success); // no relaxed expiry threshold

    cache.releaseAll();
    sensorTestNow = 0xfffff000U;
    firstRound = sensorTestNow;
    assert(cache.readChannel("wrap-collection", "Counted", 1, "{}", "first", value,
        unit, error, 10000, &timestamp, 0, &firstRound));
    sensorTestNow = 0; nextRound = 0;
    sensorTestWrapBase = UINT64_C(0x100000000);
    assert(cache.readChannel("wrap-collection", "Counted", 1, "{}", "first", value,
        unit, error, 10000, &timestamp, 0, &nextRound));
    assert(timestamp == 0 && reads == before + 5); // zero is a real collection time after wrap
    sensorTestNow = 20;
    assert(cache.readChannel("wrap-collection", "Counted", 1, "{}", "second", value,
        unit, error, 10000, &timestamp, 0, &nextRound));
    assert(timestamp == 0 && reads == before + 5);
    cache.releaseAll();

    sensorTestWrapBase = 0;
    sensorTestNow = 30000; nextRound = sensorTestNow;
    const unsigned concurrentBefore = reads;
    std::thread first([&] { float v; String u, e; uint32_t stamp;
        assert(cache.readChannel("round-concurrent", "Counted", 1, "{}", "first",
            v, u, e, 10000, &stamp, 0, &nextRound)); assert(stamp == nextRound); });
    std::thread second([&] { float v; String u, e; uint32_t stamp;
        assert(cache.readChannel("round-concurrent", "Counted", 1, "{}", "second",
            v, u, e, 10000, &stamp, 0, &nextRound)); assert(stamp == nextRound); });
    first.join(); second.join();
    assert(reads == concurrentBefore + 1);
    sensorTestNow += 10000; nextRound = sensorTestNow;
    failRead = true;
    assert(!cache.readChannel("round-concurrent", "Counted", 1, "{}", "first",
        value, unit, error, 10000, &timestamp, 0, &nextRound));
    assert(timestamp == 0 && std::isnan(value));
    sensorTestNow += 100; newerRound = sensorTestNow;
    assert(!cache.readChannel("round-concurrent", "Counted", 1, "{}", "second",
        value, unit, error, 10000, &timestamp, 0, &newerRound));
    assert(reads == concurrentBefore + 2); // collection does not bypass failed-read retry window
    failRead = false;
    cache.releaseAll();
}

static void configEpochTests() {
    auto& cache = SensorDriverCache::instance();
    const uint32_t oldVersion = SensorConfigEpoch::current();
    SensorConfigEpoch::advance();
    float value = 0; String unit, error; uint32_t timestamp = 123;
    const unsigned before = inits;
    assert(!cache.readChannel("disabled", "Counted", 1, "{}", "first", value, unit,
                              error, 1000, &timestamp, oldVersion));
    assert(inits == before && cache.activeCount() == 0);
    assert(std::isnan(value) && timestamp == 0 && error == "SENSOR_CONFIG_CHANGED");

    const uint32_t capturedVersion = SensorConfigEpoch::current();
    pauseRead = true; readEntered = false; resumeRead = false;
    std::thread reader([&] {
        assert(!cache.readChannel("in-flight", "Counted", 1, "{}", "first", value, unit,
                                  error, 1000, &timestamp, capturedVersion));
    });
    while (!readEntered.load()) std::this_thread::yield();
    SensorConfigEpoch::advance();
    resumeRead = true;
    reader.join();
    pauseRead = false;
    assert(std::isnan(value) && timestamp == 0 && error == "SENSOR_CONFIG_CHANGED");
    assert(cache.activeCount() == 0);
    SensorReading snapshot; bool stale;
    assert(!cache.getSnapshot("in-flight", snapshot, stale, error));
    assert(cache.readChannel("in-flight", "Counted", 1, "{}", "first", value, unit,
                             error, 1000, &timestamp, SensorConfigEpoch::current()));
    SensorConfigEpoch::advance();
    assert(cache.getSnapshot("in-flight", snapshot, stale, error));
    assert(stale && !snapshot.success && error == "SENSOR_CONFIG_CHANGED");
    cache.releaseAll();
}

static void calibrationTests() {
    INA226Calibration cal; const char* error = "";
    assert(calculateINA226Calibration(0.002, 15, 1100, 16, cal, error));
    assert(cal.registerValue > 0 && cal.registerValue <= 32767);
    assert(std::abs(cal.currentLsb * cal.registerValue * 0.002 - 0.00512) < 1e-12);
    assert(cal.currentLsb * 32767 >= 15 && cal.conversionMs == 36);
    assert(!calculateINA226Calibration(0, 15, 1100, 16, cal, error));
    assert(!calculateINA226Calibration(0.1, 15, 1100, 16, cal, error));
    assert(!calculateINA226Calibration(NAN, 15, 1100, 16, cal, error));
    assert(!calculateINA226Calibration(0.002, INFINITY, 1100, 16, cal, error));
    assert(!calculateINA226Calibration(0.002, 15, 1101, 16, cal, error));
    assert(!calculateINA226Calibration(0.002, 15, 8244, 1024, cal, error));
    assert(calculateINA226Calibration(0.000001, 0.001, 140, 1, cal, error));
    assert(cal.registerValue == 32767); // tiny ranges respect reserved D15
}

static void inaTests() {
    const char* params = "{\"addr\":64,\"sda\":21,\"scl\":22,\"frequency\":100000,\"shuntResistance\":0.002,\"maxCurrent\":15,\"conversionTimeUs\":1100,\"averaging\":16}";
    Wire.reset(); sensorTestNow = 10000;
    INA226Driver driver;
    assert(driver.init(21, params));
    SensorReading reading;
    assert(driver.read(reading) && reading.success && reading.channelCount == 3);
    assert(reading.values[0] == 12.0f && reading.values[1] > 0);
    assert(std::fabs(reading.values[2] - reading.values[0] * reading.values[1]) < 0.0001);
    Wire.registers[4] = static_cast<uint16_t>(int16_t(-1000));
    assert(driver.read(reading) && reading.values[1] < 0 && reading.values[2] < 0);
    Wire.registers[4] = 0;
    assert(driver.read(reading) && reading.values[1] == 0 && reading.values[2] == 0);
    Wire.ack = false;
    assert(!driver.read(reading) && !reading.success);
    Wire.ack = true; Wire.shortRead = true;
    assert(!driver.read(reading) && !reading.success);
    Wire.shortRead = false; Wire.overflow = true;
    assert(!driver.read(reading) && strstr(driver.getLastError(), "OVERFLOW"));
    Wire.overflow = false; Wire.conversionReady = false;
    assert(!driver.read(reading) && strstr(driver.getLastError(), "TIMEOUT"));
    Wire.conversionReady = true; Wire.loseCalibration = true;
    assert(!driver.read(reading) && strstr(driver.getLastError(), "CALIBRATION_LOST"));
    Wire.loseCalibration = false; Wire.registers[1] = 0x7fff;
    assert(!driver.read(reading) && strstr(driver.getLastError(), "OUT_OF_RANGE"));
    driver.deinit();
    assert(!driver.init(21, "{\"addr\":64}"));
    assert(!driver.init(21, "{\"addr\":320,\"shuntResistance\":0.002,\"maxCurrent\":15}"));
    Wire.reset(); Wire.registers[0xff] = 0x2270;
    assert(!driver.init(21, params));
    assert(I2CBusManager::instance().clientCount() == 0);
    assert(!sensorTestWaitedWithBusLock);
    Wire.reset();
    float value; String unit, error; uint32_t timestamp;
    auto& cache = SensorDriverCache::instance();
    assert(cache.readChannel("ina", "INA226", 21, params, "voltage", value, unit, error, 1000, &timestamp));
    const uint32_t voltageTime = timestamp;
    assert(cache.readChannel("ina", "INA226", 21, params, "current", value, unit, error, 1000, &timestamp));
    assert(timestamp == voltageTime);
    assert(cache.readChannel("ina", "INA226", 21, params, "power", value, unit, error, 1000, &timestamp));
    assert(Wire.conversions == 1 && timestamp == voltageTime);
    cache.releaseAll();
}

static uint8_t sensorFrameCrc(const uint8_t* data, size_t length) {
    uint8_t value = 0xff;
    while (length--) {
        value ^= *data++;
        for (int bit = 0; bit < 8; ++bit) value = (value & 0x80) ? (value << 1) ^ 0x31 : value << 1;
    }
    return value;
}

static void exerciseShtDriver(ISensorDriver& sensor) {
    Wire.reset(TwoWire::Device::STREAM);
    SensorReading reading;
    reading.success = true; reading.channelCount = 2; reading.values[0] = 123;
    assert(!sensor.read(reading) && !reading.success && !reading.channelCount);
    assert(strcmp(sensor.getLastError(), "SHT3X_NOT_INITIALIZED") == 0);
    assert(sensor.getChannelCount() == 2 && sensor.getMinInterval() == 500);
    assert(strcmp(sensor.getChannelName(0), "temperature") == 0);
    assert(strcmp(sensor.getChannelName(1), "humidity") == 0);
    assert(strcmp(sensor.getChannelUnit(0), "\xC2\xB0" "C") == 0);
    assert(strcmp(sensor.getChannelUnit(1), "%") == 0);
    assert(strcmp(sensor.getChannelName(2), "unknown") == 0);
    assert(strcmp(sensor.getChannelUnit(2), "") == 0);
    assert(!sensor.init(21, "{\"addr\":67}"));
    assert(strcmp(sensor.getLastError(), "SENSOR_ADDRESS_INVALID") == 0);
    assert(I2CBusManager::instance().clientCount() == 0);
    assert(sensor.init(21, "{\"addr\":68,\"sda\":21,\"scl\":22}"));
    assert(I2CBusManager::instance().clientCount() == 1);
    Wire.stream = {0x66, 0x66, 0, 0x80, 0, 0};
    Wire.stream[2] = sensorFrameCrc(Wire.stream.data(), 2);
    Wire.stream[5] = sensorFrameCrc(Wire.stream.data() + 3, 2);
    const uint32_t before = millis();
    assert(sensor.read(reading) && reading.success && reading.channelCount == 2);
    assert(std::fabs(reading.values[0] - 25) < 0.01);
    assert(std::fabs(reading.values[1] - 50.00076) < 0.01);
    assert(reading.timestamp == millis() && millis() - before == 20);
    assert(Wire.output == std::vector<uint8_t>({0x24, 0x00}));
    // Both words have independent CRCs. A failure must clear a prior sample.
    for (size_t crcIndex : {2U, 5U}) {
        Wire.stream[crcIndex] ^= 1;
        assert(!sensor.read(reading) && !reading.success && !reading.channelCount);
        assert(strcmp(sensor.getLastError(), "SHT3X_CRC_FAILED") == 0);
        Wire.stream[crcIndex] ^= 1;
        assert(sensor.read(reading) && !sensor.getLastError()[0]);
    }
    Wire.shortRead = true;
    assert(!sensor.read(reading) && !reading.success && !reading.channelCount);
    assert(strcmp(sensor.getLastError(), "SENSOR_I2C_SHORT_READ") == 0);
    Wire.shortRead = false;
    Wire.ack = false;
    assert(!sensor.read(reading) && !reading.success);
    assert(strcmp(sensor.getLastError(), "SENSOR_I2C_WRITE_FAILED") == 0);
    Wire.ack = true;
    // Reinitializing to the other supported address replaces, not leaks, a lease.
    assert(sensor.init(21, "{\"addr\":\"0x45\"}"));
    assert(I2CBusManager::instance().clientCount() == 1);
    assert(sensor.read(reading) && !sensor.getLastError()[0]);
    sensor.deinit();
    assert(I2CBusManager::instance().clientCount() == 0);
    assert(!sensor.read(reading) && !reading.success && !reading.channelCount);
    assert(strcmp(sensor.getLastError(), "SHT3X_NOT_INITIALIZED") == 0);
    assert(!sensorTestWaitedWithBusLock);
}

static void shtPublicDriverTests() {
    SHT31Driver sht31;
    SHT30Driver sht30;
    assert(strcmp(sht31.getName(), "SHT31") == 0);
    assert(strcmp(sht30.getName(), "SHT30") == 0);
    exerciseShtDriver(sht31);
    exerciseShtDriver(sht30);
    for (const char* name : {"SHT30", "SHT31"}) {
        std::unique_ptr<ISensorDriver> sensor(DriverRegistry::getInstance().createDriver(name));
        assert(sensor && dynamic_cast<SHT31Driver*>(sensor.get()));
        assert((dynamic_cast<SHT30Driver*>(sensor.get()) != nullptr) == (strcmp(name, "SHT30") == 0));
        exerciseShtDriver(*sensor);
    }
    // Public instances also release their own lease at destruction while a
    // sibling on the same physical bus retains access.
    assert(sht31.init(21));
    {
        SHT30Driver sibling;
        assert(sibling.init(21, "{\"addr\":69}"));
        assert(I2CBusManager::instance().clientCount() == 2);
    }
    assert(I2CBusManager::instance().clientCount() == 1);
    SensorReading reading;
    assert(sht31.read(reading));
    sht31.deinit();
    assert(I2CBusManager::instance().clientCount() == 0);
}

static void legacyFrameTests() {
    auto& registry = DriverRegistry::getInstance();
    SensorReading reading;
    Wire.reset(TwoWire::Device::STREAM);
    std::unique_ptr<ISensorDriver> aht(registry.createDriver("AHT20"));
    assert(aht && aht->init(21, "{\"sda\":21,\"scl\":22}"));
    Wire.stream = {0x08, 0x80, 0, 0x08, 0, 0};
    assert(aht->read(reading) && reading.values[0] == 50 && reading.values[1] == 50);
    Wire.stream[0] = 0x88;
    assert(!aht->read(reading) && !reading.success);
    Wire.stream[0] = 0;
    assert(!aht->read(reading) && strstr(aht->getLastError(), "CALIBRATED"));
    aht->deinit();
    Wire.reset(TwoWire::Device::STREAM);
    Wire.stream = {0x08, 0x80, 0x00, 0x08, 0x00, 0x00};
    auto& cache = SensorDriverCache::instance();
    float temperature; String unit, error; uint32_t timestamp = 0;
    assert(cache.readChannel("aht-reset", "AHT20", 21, "{}", "temperature",
        temperature, unit, error, 1000, &timestamp));
    const unsigned calibrations = Wire.ahtCalibrations;
    Wire.stream[0] = 0; // The physical sensor lost its calibrated status.
    sensorTestNow += 1000;
    assert(!cache.readChannel("aht-reset", "AHT20", 21, "{}", "temperature",
        temperature, unit, error, 1000, &timestamp));
    assert(timestamp == 0 && std::isnan(temperature) && strstr(error.c_str(), "CALIBRATED"));
    assert(!cache.readChannel("aht-reset", "AHT20", 21, "{}", "humidity",
        temperature, unit, error, 1000, &timestamp));
    assert(Wire.ahtCalibrations == calibrations);
    sensorTestNow += 1000;
    assert(cache.readChannel("aht-reset", "AHT20", 21, "{}", "temperature",
        temperature, unit, error, 1000, &timestamp));
    assert(Wire.ahtCalibrations == calibrations + 1 && temperature == 50);
    cache.releaseAll();
    Wire.reset(TwoWire::Device::STREAM);
    std::unique_ptr<ISensorDriver> bh(registry.createDriver("BH1750"));
    assert(bh && !bh->init(21, "{\"addr\":36}"));
    assert(bh->init(21, "{\"addr\":92}"));
    Wire.stream = {0, 120};
    assert(bh->read(reading) && std::fabs(reading.values[0] - 100) < 0.01);
    Wire.ack = false;
    assert(!bh->read(reading) && !reading.success);
    bh->deinit();
    assert(!sensorTestWaitedWithBusLock);
    assert(I2CBusManager::instance().clientCount() == 0);
}

static void bmpTests() {
    Wire.reset(TwoWire::Device::BMP280);
    // Bosch's published compensation example: 25.08 C and 100653 Pa.
    const int coefficients[] = {27504, 26435, -1000, 36477, -10685, 3024,
                                2855, 140, -7, 15500, -14600, 6000};
    Wire.byteRegisters[0xd0] = 0x58;
    for (uint8_t i = 0; i < 12; ++i) {
        Wire.byteRegisters[0x88 + 2 * i] = coefficients[i] & 0xff;
        Wire.byteRegisters[0x89 + 2 * i] = (coefficients[i] >> 8) & 0xff;
    }
    for (const auto& value : {std::make_pair(0xf7, 415148U), std::make_pair(0xfa, 519888U)}) {
        Wire.byteRegisters[value.first] = value.second >> 12;
        Wire.byteRegisters[value.first + 1] = (value.second >> 4) & 0xff;
        Wire.byteRegisters[value.first + 2] = (value.second << 4) & 0xff;
    }
    std::unique_ptr<ISensorDriver> driver(DriverRegistry::getInstance().createDriver("BMP280"));
    assert(driver && driver->init(21, "{\"addr\":118}"));
    SensorReading reading;
    assert(driver->read(reading) && reading.channelCount == 3);
    assert(std::fabs(reading.values[0] - 25.08) < 0.01);
    assert(std::fabs(reading.values[1] - 1006.53) < 0.02);
    assert(std::fabs(reading.values[2] - 56.08) < 0.1);
    Wire.ack = false;
    assert(!driver->read(reading) && !reading.success);
    Wire.ack = true; Wire.shortRead = true;
    assert(!driver->read(reading) && !reading.success);
    Wire.shortRead = false; Wire.conversionReady = false;
    const uint32_t started = sensorTestNow;
    assert(!driver->read(reading) && strstr(driver->getLastError(), "TIMEOUT"));
    assert(static_cast<uint32_t>(sensorTestNow - started) <= 150);
    Wire.conversionReady = true;
    assert(driver->read(reading) && reading.success);
    driver->deinit();
    Wire.byteRegisters[0x8e] = Wire.byteRegisters[0x8f] = 0;
    assert(!driver->init(21, "{\"addr\":118}") && strstr(driver->getLastError(), "CALIBRATION"));
    assert(I2CBusManager::instance().clientCount() == 0);
    assert(!sensorTestWaitedWithBusLock);
}

static void mpuTests() {
    Wire.reset(TwoWire::Device::MPU6050);
    Wire.byteRegisters[0x75] = 0x68;
    const int16_t samples[] = {4096, -4096, 0, 340, 655, -655, 0};
    for (uint8_t i = 0; i < 7; ++i) {
        Wire.byteRegisters[0x3b + 2 * i] = (uint16_t(samples[i]) >> 8) & 0xff;
        Wire.byteRegisters[0x3c + 2 * i] = uint16_t(samples[i]) & 0xff;
    }
    std::unique_ptr<ISensorDriver> driver(DriverRegistry::getInstance().createDriver("MPU6050"));
    assert(driver && driver->init(21, "{\"addr\":105,\"mode\":0}"));
    SensorReading reading;
    assert(driver->read(reading) && reading.channelCount == 4);
    assert(std::fabs(reading.values[0] - 9.80665) < 0.0001);
    assert(std::fabs(reading.values[1] + 9.80665) < 0.0001);
    assert(std::fabs(reading.values[3] - 37.53) < 0.001);
    Wire.ack = false;
    assert(!driver->read(reading) && !reading.success);
    Wire.ack = true; Wire.shortRead = true;
    assert(!driver->read(reading) && !reading.success);
    Wire.shortRead = false;
    // Module reset restores configuration before the next data burst.
    Wire.byteRegisters[0x6b] = 0x40; Wire.byteRegisters[0x1c] = 0;
    assert(driver->read(reading) && Wire.byteRegisters[0x1c] == 16);
    for (const char* params : {"{\"mode\":1}", "{\"mode\":\"gyro\"}"}) {
        assert(driver->init(21, params));
        assert(strcmp(driver->getChannelName(0), "gyroX") == 0);
        assert(driver->read(reading));
        assert(std::fabs(reading.values[0] - 0.174532925) < 0.00001);
    }
    driver->deinit();
    Wire.resetStuck = true;
    const uint32_t started = sensorTestNow;
    assert(!driver->init(21, "{}") && strstr(driver->getLastError(), "RESET_TIMEOUT"));
    assert(static_cast<uint32_t>(sensorTestNow - started) == 200);
    assert(I2CBusManager::instance().clientCount() == 0);
    assert(!sensorTestWaitedWithBusLock);
}

static void configurationTests() {
    auto directConfig = [] {
        PeripheralConfig config;
        config.type = PeripheralType::SENSOR_GENERIC;
        initializeSensorDefaults(config);
        config.pinCount = 2; config.pins[0] = 21; config.pins[1] = 22;
        return config;
    };
    String error;
    for (const char* name : {"AHT20", "BH1750", "BMP280", "MPU6050", "SHT30", "SHT31"}) {
        PeripheralConfig original = directConfig();
        JsonDocument json;
        json["driver"] = name;
        assert(parseSensorConfig(json.as<JsonObjectConst>(), original, error));
        assert(validateSensorConfig(original, error));
        assert(original.params.sensor.address == sensorDriverMetadata(name)->defaultAddress);
        JsonDocument saved;
        serializeSensorConfig(saved.to<JsonObject>(), original);
        std::string bytes;
        serializeJson(saved, bytes);
        JsonDocument loaded;
        assert(deserializeJson(loaded, bytes) == DeserializationError::Ok);
        PeripheralConfig restored = directConfig();
        assert(parseSensorConfig(loaded.as<JsonObjectConst>(), restored, error));
        assert(validateSensorConfig(restored, error));
        assert(strcmp(restored.params.sensor.driver, name) == 0);
        assert(restored.params.sensor.address == original.params.sensor.address);
    }
    PeripheralConfig ina = directConfig();
    ina.pinCount = 0;
    JsonDocument request;
    assert(deserializeJson(request,
        "{\"driverName\":\"ina226\",\"busPeripheralId\":\"shared-i2c\",\"i2cAddress\":\"0x40\","
        "\"sampleInterval\":\"1000\",\"shuntResistance\":\"0.002\",\"maxCurrent\":\"15\","
        "\"conversionTimeUs\":\"1100\",\"averaging\":\"16\"}") == DeserializationError::Ok);
    assert(parseSensorConfig(request.as<JsonObjectConst>(), ina, error));
    assert(validateSensorConfig(ina, error));
    JsonDocument saved;
    serializeSensorConfig(saved.to<JsonObject>(), ina);
    assert(saved["busPeripheralId"] == "shared-i2c" && saved["driver"] == "INA226");
    assert(saved["conversionTimeUs"] == 1100 && saved["averaging"] == 16);
    PeripheralConfig restored = directConfig(); restored.pinCount = 0;
    assert(parseSensorConfig(saved.as<JsonObjectConst>(), restored, error));
    assert(validateSensorConfig(restored, error));
    assert(restored.params.sensor.shuntResistance == ina.params.sensor.shuntResistance);
    assert(restored.params.sensor.maxCurrent == ina.params.sensor.maxCurrent);
    JsonDocument hardwareParams;
    buildSensorDriverParams(hardwareParams, ina, 4, 5, 400000);
    assert(hardwareParams["sda"] == 4 && hardwareParams["scl"] == 5);
    assert(hardwareParams["frequency"] == 400000 && hardwareParams["addr"] == 64);
    assert(hardwareParams["shuntResistance"].as<float>() == ina.params.sensor.shuntResistance);
    assert(sensorSampleIntervalMs(ina) == 1000);
    String channel;
    assert(resolveSensorChannel("INA226", 0, "", channel, error) && channel == "voltage");
    assert(resolveSensorChannel("MPU6050", 1, "gyro_x", channel, error) && channel == "gyroX");
    assert(!resolveSensorChannel("MPU6050", 1, "accelX", channel, error));
    Wire.reset();
    std::string hardwareText; serializeJson(hardwareParams, hardwareText);
    float measured; String measuredUnit; uint32_t measuredAt = 0;
    assert(SensorDriverCache::instance().readChannel("configured", "INA226", 4,
        hardwareText.c_str(), "current", measured, measuredUnit, error,
        sensorSampleIntervalMs(ina), &measuredAt));
    assert(measuredAt == sensorTestNow && measuredUnit == "A");
    RuntimeFeatureManager::instance().enabled = false;
    assert(validateSensorConfig(ina, error)); // inactive configuration is editable
    assert(!SensorDriverCache::instance().readChannel("configured", "INA226", 4,
        hardwareText.c_str(), "voltage", measured, measuredUnit, error));
    assert(std::isnan(measured) && error == "I2C_SENSORS_UNAVAILABLE");
    assert(SensorDriverCache::instance().activeCount() == 0);
    RuntimeFeatureManager::instance().enabled = true;
    restored.pinCount = 2;
    assert(!validateSensorConfig(restored, error) && strstr(error.c_str(), "REFERENCE"));
    restored.pinCount = 0; restored.params.sensor.shuntResistance = 0;
    assert(!validateSensorConfig(restored, error) && strstr(error.c_str(), "CALIBRATION"));
    restored = ina; restored.params.sensor.averaging = 1024;
    assert(!validateSensorConfig(restored, error) && strstr(error.c_str(), "TOO_SLOW"));
    for (const char* invalid : {"{\"shuntResistance\":\"nan\"}",
                               "{\"maxCurrent\":true}", "{\"maxCurrent\":-1}",
                               "{\"address\":320}", "{\"address\":\"64junk\"}",
                               "{\"mode\":1.5}", "{\"sampleInterval\":86400001}"}) {
        JsonDocument json; assert(deserializeJson(json, invalid) == DeserializationError::Ok);
        restored = ina;
        assert(!parseSensorConfig(json.as<JsonObjectConst>(), restored, error));
        assert(restored.params.sensor.address == ina.params.sensor.address);
    }
    JsonDocument changed;
    changed["driver"] = "BH1750";
    PeripheralConfig sensor = directConfig();
    assert(parseSensorConfig(changed.as<JsonObjectConst>(), sensor, error));
    sensor.params.sensor.address = 0x24;
    assert(!validateSensorConfig(sensor, error));
    changed["driver"] = "MPU6050"; changed["mode"] = "gyro";
    assert(parseSensorConfig(changed.as<JsonObjectConst>(), sensor, error));
    assert(validateSensorConfig(sensor, error));
    assert(sensor.params.sensor.address == 0x68 && sensor.params.sensor.mode == 1);
    // Full production config parser and the shared thermostat mode parser
    // must retain the same numeric, legacy textual, and fallback semantics.
    for (const char* token : {"0", "\"0\"", "\"accel\"", "\"ACCEL\"",
                              "1", "\"1\"", "\"gyro\"", "\"GyRo\"", "\" 1 \""}) {
        JsonDocument value; assert(!deserializeJson(value, token));
        JsonDocument modeConfig;
        modeConfig["driver"] = "MPU6050"; modeConfig["mode"].set(value.as<JsonVariantConst>());
        auto parsed = directConfig();
        assert(parseSensorConfig(modeConfig.as<JsonObjectConst>(), parsed, error));
        uint8_t shared = 99;
        assert(parseSensorMode(value.as<JsonVariantConst>(), 0, shared));
        assert(shared == parsed.params.sensor.mode);
        const bool gyro = token[0] == '1' || std::strstr(token, "1") ||
            !std::strcmp(token, "\"gyro\"") || !std::strcmp(token, "\"GyRo\"");
        assert(shared == (gyro ? 1 : 0));
    }
    for (const char* token : {"true", "[]", "{}", "\"invalid\"", "2", "-1", "0.5", "\"1junk\""}) {
        JsonDocument value; assert(!deserializeJson(value, token));
        JsonDocument modeConfig;
        modeConfig["driver"] = "MPU6050"; modeConfig["mode"].set(value.as<JsonVariantConst>());
        auto parsed = sensor;
        assert(!parseSensorConfig(modeConfig.as<JsonObjectConst>(), parsed, error));
        assert(error == "SENSOR_MODE_INVALID" && parsed.params.sensor.mode == 1);
        uint8_t shared = 99;
        assert(!parseSensorMode(value.as<JsonVariantConst>(), 1, shared) && shared == 99);
    }
    for (bool explicitNull : {false, true}) {
        JsonDocument modeConfig;
        modeConfig["driver"] = "MPU6050";
        if (explicitNull) modeConfig["mode"] = nullptr;
        auto parsed = sensor;
        assert(parseSensorConfig(modeConfig.as<JsonObjectConst>(), parsed, error));
        assert(parsed.params.sensor.mode == 1); // same driver retains saved mode
        parsed = directConfig();
        assert(parseSensorConfig(modeConfig.as<JsonObjectConst>(), parsed, error));
        assert(parsed.params.sensor.mode == 0); // a new driver uses the default
    }
    changed["driver"] = "BMP280"; changed.remove("mode");
    assert(parseSensorConfig(changed.as<JsonObjectConst>(), sensor, error));
    assert(sensor.params.sensor.address == 0x76 && sensor.params.sensor.mode == 0);
    changed["driver"] = "UNAVAILABLE";
    assert(parseSensorConfig(changed.as<JsonObjectConst>(), sensor, error));
    assert(!validateSensorConfig(sensor, error));
    saved.clear(); serializeSensorConfig(saved.to<JsonObject>(), sensor);
    assert(saved["driver"] == "UNAVAILABLE"); // preserve unavailable imported driver
    assert(I2CBusManager::instance().clientCount() == 0);
}

static void registryCapacityTests() {
    auto& registry = DriverRegistry::getInstance();
    // Exercise the real configured capacity instead of mirroring an old limit.
    assert(registry.getCapacity() == MAX_SENSOR_DRIVERS);
    assert(registry.getAvailableSlots() == MAX_SENSOR_DRIVERS - registry.getDriverCount());
    static char names[MAX_SENSOR_DRIVERS][24] = {};
    size_t added = 0;
    while (registry.getAvailableSlots() > 0) {
        snprintf(names[added], sizeof(names[added]), "capacity-test-%u", unsigned(added));
        assert(registry.registerDriver(names[added], createCounted));
        ++added;
    }
    assert(registry.getDriverCount() == MAX_SENSOR_DRIVERS);
    assert(registry.getAvailableSlots() == 0);
    const auto failures = registry.getRegistrationFailureCount();
    assert(!registry.registerDriver("extra7", createCounted));
    assert(registry.getRegistrationFailureCount() == failures + 1);
    assert(!registry.getDriverEntry(MAX_SENSOR_DRIVERS));
}

static void instanceObservationTests() {
    auto& cache = SensorDriverCache::instance();
    auto observe = [] { return FeatureInstanceRegistry::instance().observe("i2cSensors"); };
    static const SensorDriverMetadata observedMetadata = [] {
        auto metadata = *sensorDriverMetadata("SHT31");
        metadata.name = "ObservedCounted"; return metadata;
    }();
    assert(DriverRegistry::getInstance().registerDriver("ObservedCounted", createCounted, &observedMetadata));
    cache.releaseAll();
    float value; String unit, error;
    failRead = failInit = nanRead = false;
    assert(cache.readChannel("not-i2c", "Counted", 1, "{}", "first", value, unit, error));
    assert(observe().state == FeatureInstanceState::NotInitialized); // Do not count unrelated drivers.
    cache.releaseAll();
    assert(cache.readChannel("observed", "ObservedCounted", 1, "{}", "first", value, unit, error));
    const auto initialReads = reads, initialInits = inits, mutexes = sensorTestMutexCreates.load();
    for (unsigned i=0; i<1000; ++i) {
        const auto result = observe();
        assert(result.state == FeatureInstanceState::Initialized && result.initializedCount == 1);
    }
    assert(reads == initialReads && inits == initialInits && sensorTestMutexCreates == mutexes);
    RuntimeFeatureManager::instance().enabled = false;
    assert(observe().initializedCount == 1); // Changing admission does not pretend to free an instance.
    assert(!cache.readChannel("observed", "ObservedCounted", 1, "{}", "first", value, unit, error));
    assert(observe().state == FeatureInstanceState::NotInitialized);
    RuntimeFeatureManager::instance().enabled = true;
    failInit = true;
    assert(!cache.readChannel("init-fail", "ObservedCounted", 1, "{}", "first", value, unit, error));
    assert(observe().initializedCount == 0);
    cache.releaseAll(); failInit = false;
    assert(cache.readChannel("read-fail", "ObservedCounted", 1, "{}", "first", value, unit, error));
    sensorTestNow += 1000; failRead = true;
    assert(!cache.readChannel("read-fail", "ObservedCounted", 1, "{}", "first", value, unit, error));
    assert(observe().state == FeatureInstanceState::NotInitialized);
    failRead = false; cache.releaseAll();
    pauseRead = true; readEntered = false; resumeRead = false;
    std::thread reader([&] { float v; String u,e;
        assert(cache.readChannel("busy", "ObservedCounted", 1, "{}", "first", v,u,e));
    });
    while (!readEntered.load()) std::this_thread::yield();
    const auto busy = observe();
    assert(busy.state == FeatureInstanceState::Busy && !busy.countKnown());
    resumeRead = true; reader.join(); pauseRead = false;
    assert(observe().initializedCount == 1);
    cache.releaseAll();
    for (unsigned i=0; i<SensorDriverCache::MAX_ENTRIES+2; ++i) {
        assert(cache.readChannel(String("obs")+String(i), "ObservedCounted", 1, "{}", "first", value,unit,error));
        assert(observe().initializedCount == std::min<size_t>(i+1,SensorDriverCache::MAX_ENTRIES));
    }
    cache.releaseAll();
    assert(observe().state == FeatureInstanceState::NotInitialized && observe().initializedCount == 0);
}

static void monotonicSampleTests() {
    auto& cache=SensorDriverCache::instance();cache.releaseAll();
    float value;String unit,error;uint32_t timestamp;SensorSampleMetadata metadata;
    sensorTestNow=100;sensorTestWrapBase=0;
    assert(cache.readChannel("long-run","Counted",1,"{}","first",value,unit,error,1000,&timestamp,0,nullptr,&metadata));
    assert(metadata.sampledAtMs==100 && metadata.quality==SensorSampleQuality::Good);
    const auto before=reads;
    sensorTestNow=200;
    assert(cache.readChannel("long-run","Counted",1,"{}","first",value,unit,error,1000,&timestamp,0,nullptr,&metadata));
    assert(metadata.sampledAtMs==100 && metadata.capturedAtMs==200 && reads==before);
    sensorTestWrapBase=UINT64_C(0x100000000);sensorTestNow=200;
    SensorReading snapshot;bool stale=false;
    assert(cache.getSnapshot("long-run",snapshot,stale,error) && stale && !snapshot.success);
    assert(cache.readChannel("long-run","Counted",1,"{}","first",value,unit,error,1000,&timestamp,0,nullptr,&metadata));
    assert(reads==before+1 && metadata.sampledAtMs==sensorTestWrapBase+200);
    cache.releaseAll();sensorTestWrapBase=0;
}
int main() {
    metadataTests(); cacheTests(); collectionFreshnessTests(); configEpochTests(); calibrationTests(); inaTests();
    shtPublicDriverTests(); legacyFrameTests(); bmpTests(); mpuTests(); configurationTests(); instanceObservationTests(); registryCapacityTests();
    monotonicSampleTests();
    std::cout << "sensor backend behavior checks passed\n";
}
