#include <Arduino.h>
#include <ArduinoJson.h>
#include "core/PeripheralConfig.h"
#include "core/SensorConfigSupport.h"
#include "core/Pca9685ConfigSupport.h"
#include "core/H1ParameterContract.h"
#include "core/RuntimeFeatureManager.h"
#include "network/FixedJsonField.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <string>

// HTTP transport only; all parsing and serialization below is production code.
struct AsyncWebServerRequest {
    std::map<std::string, String> fields;
    bool hasParam(const char* name, bool = false) const { return fields.count(name) != 0; }
};
struct WebHandlerContext {
    String getParamValue(AsyncWebServerRequest* request, const char* name, const String& fallback) {
        auto value = request->fields.find(name);
        return value == request->fields.end() ? fallback : value->second;
    }
};
static size_t strlcpy(char* dest, const char* source, size_t size) {
    size_t length = std::strlen(source);
    if (size) { std::memcpy(dest, source, std::min(length, size - 1)); dest[std::min(length, size - 1)] = 0; }
    return length;
}
#include "route_under_test.inc"

static void parentRoundTrip() {
    WebHandlerContext ctx;
    String error;
    PeripheralConfig created;
    created.type = PeripheralType::IO_EXPANDER;
    AsyncWebServerRequest request{{{"driver", "pcf8574"}, {"busPeripheralId", "shared_i2c"},
        {"address", "0x23"}, {"sampleInterval", "100"}}};
    assert(applyIoPeripheralRequestParameters(&ctx, &request, created, true, error));
    assert(created.params.ioExpander.address == 0x23);
    JsonDocument detail;
    addGenericPeripheralFields(detail.to<JsonObject>(), created);
    assert(detail["params"]["driver"] == "PCF8574");
    assert(detail["params"]["busPeripheralId"] == "shared_i2c");
    assert(detail["params"]["sampleInterval"] == 100);
    assert(detail["featureActive"] == true);
    AsyncWebServerRequest update{{{"address", "39"}, {"sampleInterval", "1000"}}};
    PeripheralConfig preflight = created;
    assert(applyIoPeripheralRequestParameters(&ctx, &update, preflight, false, error));
    assert(applyIoPeripheralRequestParameters(&ctx, &update, created, false, error));
    assert(std::memcmp(&created.params, &preflight.params, sizeof(created.params)) == 0);
    assert(created.params.ioExpander.address == 39 && created.params.ioExpander.sampleInterval == 1000);
    assert(std::strcmp(created.params.ioExpander.busPeripheralId, "shared_i2c") == 0);
    AsyncWebServerRequest alias{{{"driverName", "PCF8574"}, {"i2cAddress", "0x20"}}};
    assert(applyIoPeripheralRequestParameters(&ctx, &alias, created, false, error));
    for (const auto& invalid : std::initializer_list<std::pair<const char*, const char*>>{
        {"driver", "MCP23017"}, {"address", "0x38"}, {"address", "32junk"},
        {"address", "32.5"}, {"address", "256"}, {"address", "-1"},
        {"sampleInterval", "19"}, {"sampleInterval", "1001"},
        {"sampleInterval", "42949672960"}, {"sampleInterval", ""},
        {"busPeripheralId", "12345678901234567890123456789012345678901"}}) {
        AsyncWebServerRequest bad{{{invalid.first, invalid.second}}};
        PeripheralConfig copy = created;
        assert(!applyIoPeripheralRequestParameters(&ctx, &bad, copy, false, error));
        assert(std::memcmp(&copy.params, &created.params, sizeof(copy.params)) == 0);
    }
    PeripheralConfig pins = created;
    pins.pinCount = 2; pins.pins[0] = 4; pins.pins[1] = 5;
    assert(!applyIoPeripheralRequestParameters(&ctx, &alias, pins, false, error));
}

static void logicalOutputRoundTrip() {
    WebHandlerContext ctx;
    String error;
    PeripheralConfig output;
    output.type = PeripheralType::GPIO_DIGITAL_OUTPUT;
    AsyncWebServerRequest request{{{"ioSource", "expander"}, {"providerId", "pcf"},
        {"channel", "7"}, {"invertLogic", "true"}, {"initialState", "0"}}};
    assert(applyIoPeripheralRequestParameters(&ctx, &request, output, true, error));
    assert(output.isExpanderChannel() && output.params.gpio.channel == 7 && output.params.gpio.invertLogic);
    JsonDocument detail;
    addGenericPeripheralFields(detail.to<JsonObject>(), output);
    assert(detail["params"]["ioSource"] == 1 && detail["params"]["providerId"] == "pcf");
    assert(detail["params"]["channel"] == 7 && detail["params"]["invertLogic"] == true);
    // Reboot persistence exercises the actual save/load assignment blocks.
    JsonDocument stored;
    saveIoPersistence(stored.to<JsonObject>(), output);
    assert(stored["invertLogic"] == true);
    PeripheralConfig rebooted;
    rebooted.type = output.type;
    loadIoPersistence(stored.as<JsonObject>(), rebooted);
    assert(rebooted.params.gpio.invertLogic && rebooted.isExpanderChannel());
    auto startupHigh = [](const PeripheralConfig& value) {
        return digitalInputLogicalLevel(value.params.gpio.initialState == GPIOState::STATE_HIGH,
                                        value.params.gpio.invertLogic);
    };
    assert(startupHigh(output) && startupHigh(rebooted) == startupHigh(output));
    AsyncWebServerRequest numeric{{{"ioSource", "1"}, {"channel", "0"}}};
    assert(applyIoPeripheralRequestParameters(&ctx, &numeric, rebooted, false, error));
    // Request parsing admits the widest supported provider. The manager then
    // checks the selected parent's real 8/16-channel capability.
    for (const char* channel : {"8", "15"}) {
        AsyncWebServerRequest wide{{{"channel", channel}}};
        PeripheralConfig copy = rebooted;
        assert(applyIoPeripheralRequestParameters(&ctx, &wide, copy, false, error));
        assert(copy.params.gpio.channel == std::atoi(channel));
    }
    for (const auto& invalid : std::initializer_list<std::pair<const char*, const char*>>{
        {"ioSource", "2"}, {"channel", "16"}, {"channel", "256"}, {"channel", "1.2"},
        {"channel", ""}, {"providerId", ""}, {"invertLogic", "yes"}, {"initialState", "2"},
        {"inputMode", "1"}, {"providerId", "12345678901234567890123456789012345678901"}}) {
        AsyncWebServerRequest bad{{{invalid.first, invalid.second}}};
        PeripheralConfig copy = rebooted;
        assert(!applyIoPeripheralRequestParameters(&ctx, &bad, copy, false, error));
    }
    PeripheralConfig invalidType = rebooted;
    invalidType.type = PeripheralType::GPIO_PWM_OUTPUT;
    assert(!applyIoPeripheralRequestParameters(&ctx, &numeric, invalidType, false, error));
    AsyncWebServerRequest native{{{"ioSource", "gpio"}}};
    assert(applyIoPeripheralRequestParameters(&ctx, &native, rebooted, false, error));
    assert(!rebooted.isExpanderChannel() && !rebooted.params.gpio.providerId[0] && !rebooted.params.gpio.channel);
}

static void sensorRoundTrip() {
    WebHandlerContext ctx;
    String error;
    PeripheralConfig sensor;
    sensor.type = PeripheralType::SENSOR_GENERIC;
    AsyncWebServerRequest request{{{"driver", "INA226"}, {"busPeripheralId", "shared_i2c"},
        {"address", "64"}, {"sampleInterval", "1000"}, {"shuntResistance", "0.01"},
        {"maxCurrent", "5"}, {"conversionTimeUs", "1100"}, {"averaging", "64"}, {"channel", "power"}}};
    assert(applySensorRequest(&ctx, &request, sensor, true, error));
    JsonDocument detail;
    addGenericPeripheralFields(detail.to<JsonObject>(), sensor);
    assert(detail["params"]["driver"] == "INA226" && detail["params"]["averaging"] == 64);
    assert(detail["params"]["busPeripheralId"] == "shared_i2c");
    assert(detail["params"]["channel"].isNull());
    PeripheralConfig rebooted;
    rebooted.type = sensor.type;
    initializeSensorDefaults(rebooted);
    assert(FastBee::parseSensorConfig(detail["params"].as<JsonObjectConst>(), rebooted, error));
    assert(FastBee::validateSensorConfig(rebooted, error));
    assert(rebooted.params.sensor.averaging == 64 && rebooted.params.sensor.shuntResistance == sensor.params.sensor.shuntResistance);
    AsyncWebServerRequest invalid{{{"maxCurrent", "10"}}};
    assert(!applySensorRequest(&ctx, &invalid, rebooted, false, error));
    AsyncWebServerRequest mpu{{{"driver", "MPU6050"}, {"busPeripheralId", "shared_i2c"}, {"mode", "1"}}};
    assert(applySensorRequest(&ctx, &mpu, sensor, true, error));
    detail.clear();
    addGenericPeripheralFields(detail.to<JsonObject>(), sensor);
    assert(detail["params"]["mode"] == 1 && detail["params"]["channel"].isNull());
    FastBee::RuntimeFeatureManager::instance().enabled = false;
    addGenericPeripheralFields(detail.as<JsonObject>(), sensor);
    assert(detail["featureActive"] == false);
    FastBee::RuntimeFeatureManager::instance().enabled = true;
}

int main() {
    parentRoundTrip();
    logicalOutputRoundTrip();
    sensorRoundTrip();
    std::cout << "generic peripheral route behavior checks passed\n";
}
