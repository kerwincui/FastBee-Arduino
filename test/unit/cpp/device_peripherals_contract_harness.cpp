#include "core/DriverRegistry.h"
#include "core/SensorConfigSupport.h"
#include <cassert>
#include <cstring>
#include <iostream>

using namespace FastBee;

// Configuration validation never creates a driver or accesses hardware.
static ISensorDriver* unusedFactory() { assert(false); return nullptr; }

static PeripheralConfig example(const char* driver, uint8_t address) {
    JsonDocument params;
    params["driver"] = driver;
    params["address"] = address;
    params["frequency"] = 100000;
    params["sampleInterval"] = 5000;
    PeripheralConfig config{};
    config.type = PeripheralType::SENSOR_GENERIC;
    config.pinCount = 2;
    config.pins[0] = 21;
    config.pins[1] = 22;
    String error;
    assert(parseSensorConfig(params.as<JsonObjectConst>(), config, error));
    assert(validateSensorConfig(config, error));
    return config;
}

int main() {
    struct Fixture { const char* driver; uint8_t address; const char* channel; const char* unit; };
    const Fixture fixtures[] = {
        {"AHT20", 0x38, "humidity", "%"},
        {"BH1750", 0x23, "illuminance", "lx"},
        {"SHT31", 0x44, "temperature", "\xC2\xB0" "C"},
        {"BMP280", 0x76, "pressure", "hPa"},
        {"MPU6050", 0x68, "accelX", "m/s\xC2\xB2"}
    };
    for (const auto& fixture : fixtures) {
        assert(DriverRegistry::getInstance().registerDriver(fixture.driver, unusedFactory));
        auto config = example(fixture.driver, fixture.address);
        String error, channel;
        assert(resolveSensorChannel(fixture.driver, 0, fixture.channel, channel, error));
        const auto* metadata = DriverRegistry::getInstance().getMetadata(fixture.driver);
        bool found = false;
        for (uint8_t i = 0; i < metadata->activeChannelCount(); ++i) {
            const auto* field = metadata->getChannel(i);
            if (!std::strcmp(field->name, fixture.channel)) {
                assert(!std::strcmp(field->unit, fixture.unit));
                found = true;
            }
        }
        assert(found);
        JsonDocument saved;
        serializeSensorConfig(saved.to<JsonObject>(), config);
        PeripheralConfig restored{};
        restored.type = PeripheralType::SENSOR_GENERIC;
        restored.pinCount = 2; restored.pins[0] = 21; restored.pins[1] = 22;
        assert(parseSensorConfig(saved.as<JsonObjectConst>(), restored, error));
        assert(validateSensorConfig(restored, error));
        assert(!std::strcmp(restored.params.sensor.driver, fixture.driver));
        assert(restored.params.sensor.address == fixture.address);
        assert(restored.params.sensor.sampleInterval == 5000);
        // Invalid input must not partly change a previously valid configuration.
        saved["sampleInterval"] = 99;
        saved["address"] = 1;
        assert(!parseSensorConfig(saved.as<JsonObjectConst>(), restored, error));
        assert(restored.params.sensor.address == fixture.address);
        assert(restored.params.sensor.sampleInterval == 5000);
    }
    String error, channel;
    auto bh = example("BH1750", 0x5c);
    bh.params.sensor.address = 0x24;
    assert(!validateSensorConfig(bh, error));
    assert(error == "SENSOR_ADDRESS_INVALID");
    auto mpu = example("MPU6050", 0x69);
    assert(!resolveSensorChannel("MPU6050", 0, "gyroX", channel, error));
    mpu.params.sensor.mode = 1;
    assert(validateSensorConfig(mpu, error));
    assert(resolveSensorChannel("MPU6050", 1, "gyro_x", channel, error) && channel == "gyroX");
    assert(!resolveSensorChannel("MPU6050", 1, "accelX", channel, error));
    const auto* mpuMetadata = sensorDriverMetadata("MPU6050");
    assert(mpuMetadata->activeChannelCount(0) == 4 && mpuMetadata->activeChannelCount(1) == 4);
    assert(!std::strcmp(mpuMetadata->getChannel(0, 1)->unit, "rad/s"));
    auto aht = example("AHT20", 0x38);
    aht.params.sensor.sampleInterval = 100;
    assert(sensorSampleIntervalMs(aht) == 1000);
    std::strcpy(aht.params.sensor.busPeripheralId, "i2c_main");
    assert(!validateSensorConfig(aht, error));
    aht.pinCount = 0;
    assert(validateSensorConfig(aht, error));
    aht.params.sensor.mode = 1;
    assert(!validateSensorConfig(aht, error));
    auto unavailable = example("BMP280", 0x77);
    JsonDocument imported;
    imported["driver"] = "Unavailable";
    assert(parseSensorConfig(imported.as<JsonObjectConst>(), unavailable, error));
    assert(!std::strcmp(unavailable.params.sensor.driver, "Unavailable"));
    assert(!validateSensorConfig(unavailable, error));
    assert(error.startsWith("SENSOR_DRIVER_UNAVAILABLE"));
    std::cout << "device peripheral contract checks passed\n";
}
