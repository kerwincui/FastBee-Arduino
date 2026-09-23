#include "core/SensorConfigSupport.h"
#include "core/DriverRegistry.h"
#if FASTBEE_ENABLE_I2C_SENSORS
#include "peripherals/drivers/INA226Driver.h"
#endif
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace FastBee {
namespace {
bool configNumber(JsonVariantConst input, double fallback, double minimum,
                  double maximum, bool integer, double& output) {
    if (input.isNull()) output = fallback;
    else if (input.is<bool>()) return false;
    else if (input.is<const char*>()) {
        const char* text = input.as<const char*>();
        if (!text || !text[0]) return false;
        char* end = nullptr;
        output = std::strtod(text, &end);
        if (end == text) return false;
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
        if (*end) return false;
    } else if (input.is<double>()) output = input.as<double>();
    else return false;
    return std::isfinite(output) && output >= minimum && output <= maximum &&
        (!integer || std::floor(output) == output);
}
bool configText(JsonVariantConst input, const char* fallback, char* output,
                size_t capacity) {
    const char* text = input.isNull() ? fallback :
        input.is<const char*>() ? input.as<const char*>() : nullptr;
    if (!text || std::strlen(text) >= capacity) return false;
    std::memcpy(output, text, std::strlen(text) + 1);
    return true;
}
} // namespace

bool parseSensorConfig(JsonObjectConst params, PeripheralConfig& config, String& error) {
    auto sensor = config.params.sensor;
    if (!memchr(sensor.driver, '\0', sizeof(sensor.driver)) ||
        !memchr(sensor.busPeripheralId, '\0', sizeof(sensor.busPeripheralId))) {
        error = "SENSOR_TEXT_INVALID";
        return false;
    }
    const auto driverInput = params["driver"].isNull() ? params["driverName"] : params["driver"];
    if (!configText(driverInput, config.params.sensor.driver, sensor.driver, sizeof(sensor.driver)) ||
        !configText(params["busPeripheralId"], config.params.sensor.busPeripheralId,
                    sensor.busPeripheralId, sizeof(sensor.busPeripheralId))) {
        error = "SENSOR_TEXT_INVALID: driver or bus reference is too long";
        return false;
    }
    String normalizedDriver(sensor.driver);
    normalizedDriver.trim();
    std::memcpy(sensor.driver, normalizedDriver.c_str(), normalizedDriver.length() + 1);
    // Parse and preserve bounded imported configuration even when its driver
    // is unavailable in this firmware; validation separately marks it invalid.
    const SensorDriverMetadata* meta = sensor.driver[0]
        ? DriverRegistry::getInstance().getMetadata(sensor.driver) : nullptr;
    if (!meta && sensor.driver[0]) meta = sensorDriverMetadata(sensor.driver);
    const bool changedDriver = !sensorNameEquals(sensor.driver, config.params.sensor.driver);
    if (meta) {
        if (std::strlen(meta->name) >= sizeof(sensor.driver)) {
            error = "SENSOR_TEXT_INVALID: driver name is too long"; return false;
        }
        std::memcpy(sensor.driver, meta->name, std::strlen(meta->name) + 1);
    }
    double number = 0;
    if (!configNumber(params["sensorType"], sensor.sensorType, 0, 255, true, number)) {
        error = "SENSOR_TYPE_INVALID"; return false;
    }
    sensor.sensorType = static_cast<uint8_t>(number);
    if (!configNumber(params["sampleInterval"], sensor.sampleInterval ? sensor.sampleInterval : 1000,
                      0, 86400000, true, number) || (number > 0 && number < 100)) {
        error = "SENSOR_INTERVAL_INVALID: sample interval must be 100ms to 24h"; return false;
    }
    sensor.sampleInterval = number == 0 ? 1000 : static_cast<uint32_t>(number);
    const auto addressInput = params["address"].isNull() ? params["i2cAddress"] : params["address"];
    const uint8_t addressDefault = meta && (changedDriver || !sensor.address)
        ? meta->defaultAddress : sensor.address;
    if (!configNumber(addressInput, addressDefault, 0, 127, true, number)) {
        error = "SENSOR_ADDRESS_INVALID"; return false;
    }
    sensor.address = static_cast<uint8_t>(number);
    JsonVariantConst mode = params["mode"];
    if (mode.is<const char*>() && sensorNameEquals(mode.as<const char*>(), "gyro")) sensor.mode = 1;
    else if (mode.is<const char*>() && sensorNameEquals(mode.as<const char*>(), "accel")) sensor.mode = 0;
    else {
        if (!configNumber(mode, changedDriver ? 0 : sensor.mode, 0, 1, true, number)) {
            error = "SENSOR_MODE_INVALID"; return false;
        }
        sensor.mode = static_cast<uint8_t>(number);
    }
    if (!configNumber(params["frequency"], sensor.frequency ? sensor.frequency : 100000,
                      100000, 400000, true, number) || (number != 100000 && number != 400000)) {
        error = "SENSOR_FREQUENCY_INVALID: use 100000 or 400000 Hz"; return false;
    }
    sensor.frequency = static_cast<uint32_t>(number);
    if (!configNumber(params["shuntResistance"], sensor.shuntResistance, 0, 1000, false, number)) {
        error = "INA226_SHUNT_INVALID"; return false;
    }
    sensor.shuntResistance = static_cast<float>(number);
    if (!configNumber(params["maxCurrent"], sensor.maxCurrent, 0, 1000000, false, number)) {
        error = "INA226_CURRENT_INVALID"; return false;
    }
    sensor.maxCurrent = static_cast<float>(number);
    if (!configNumber(params["conversionTimeUs"], sensor.conversionTimeUs ? sensor.conversionTimeUs : 1100,
                      0, 65535, true, number)) {
        error = "INA226_CONVERSION_CONFIG_INVALID"; return false;
    }
    sensor.conversionTimeUs = static_cast<uint16_t>(number);
    if (!configNumber(params["averaging"], sensor.averaging ? sensor.averaging : 16,
                      0, 65535, true, number)) {
        error = "INA226_CONVERSION_CONFIG_INVALID"; return false;
    }
    sensor.averaging = static_cast<uint16_t>(number);
    config.params.sensor = sensor;
    error = "";
    return true;
}

void serializeSensorConfig(JsonObject params, const PeripheralConfig& config) {
    const auto& sensor = config.params.sensor;
    params["sensorType"] = sensor.sensorType;
    params["sampleInterval"] = sensor.sampleInterval ? sensor.sampleInterval : 1000;
    if (!sensor.driver[0]) return;
    params["driver"] = sensor.driver;
    params["address"] = sensor.address;
    params["mode"] = sensor.mode;
    params["frequency"] = sensor.frequency;
    params["busPeripheralId"] = sensor.busPeripheralId;
    if (sensorNameEquals(sensor.driver, "INA226")) {
        params["shuntResistance"] = sensor.shuntResistance;
        params["maxCurrent"] = sensor.maxCurrent;
        params["conversionTimeUs"] = sensor.conversionTimeUs;
        params["averaging"] = sensor.averaging;
    }
}

bool validateSensorConfig(const PeripheralConfig& config, String& error) {
    const auto& sensor = config.params.sensor;
    if (!memchr(sensor.driver, '\0', sizeof(sensor.driver)) ||
        !memchr(sensor.busPeripheralId, '\0', sizeof(sensor.busPeripheralId))) {
        error = "SENSOR_TEXT_INVALID"; return false;
    }
    if (sensor.sampleInterval && (sensor.sampleInterval < 100 || sensor.sampleInterval > 86400000UL)) {
        error = "SENSOR_INTERVAL_INVALID: sample interval must be 100ms to 24h"; return false;
    }
    if (!sensor.driver[0]) {
        if (sensor.busPeripheralId[0]) { error = "SENSOR_BUS_WITHOUT_DRIVER"; return false; }
        return true;
    }
    const auto* meta = DriverRegistry::getInstance().getMetadata(sensor.driver);
    if (!meta) { error = "SENSOR_DRIVER_UNAVAILABLE: driver is unknown or not compiled"; return false; }
    if (!meta->supportsAddress(sensor.address)) { error = "SENSOR_ADDRESS_INVALID"; return false; }
    if (sensor.frequency != 100000 && sensor.frequency != 400000) {
        error = "SENSOR_FREQUENCY_INVALID"; return false;
    }
    if (sensor.mode > 1 || (!sensorNameEquals(sensor.driver, "MPU6050") && sensor.mode != 0)) {
        error = "SENSOR_MODE_INVALID"; return false;
    }
    if (sensor.busPeripheralId[0]) {
        if (config.pinCount != 0) { error = "I2C_REFERENCE_INVALID: choose bus reference or two pins"; return false; }
    } else if (config.pinCount != 2 || config.pins[0] == config.pins[1]) {
        error = "I2C_PINS_INVALID: different SDA/SCL pins required"; return false;
    }
    if (sensorNameEquals(sensor.driver, "INA226")) {
#if FASTBEE_ENABLE_I2C_SENSORS
        INA226Calibration calibration;
        const char* detail = "";
        if (!calculateINA226Calibration(sensor.shuntResistance, sensor.maxCurrent,
                sensor.conversionTimeUs, sensor.averaging, calibration, detail)) {
            error = detail; return false;
        }
#else
        error = "SENSOR_DRIVER_UNAVAILABLE: INA226 is not compiled"; return false;
#endif
    }
    error = "";
    return true;
}

void buildSensorDriverParams(JsonDocument& params, const PeripheralConfig& config,
                             uint8_t sda, uint8_t scl, uint32_t frequency) {
    params.clear();
    const auto& sensor = config.params.sensor;
    params["addr"] = sensor.address;
    params["sda"] = sda;
    params["scl"] = scl;
    params["frequency"] = frequency;
    params["mode"] = sensor.mode;
    if (sensorNameEquals(sensor.driver, "INA226")) {
        params["shuntResistance"] = sensor.shuntResistance;
        params["maxCurrent"] = sensor.maxCurrent;
        params["conversionTimeUs"] = sensor.conversionTimeUs;
        params["averaging"] = sensor.averaging;
    }
}

bool resolveSensorChannel(const char* driver, uint8_t mode, const char* requested,
                          String& channel, String& error) {
    const auto* metadata = DriverRegistry::getInstance().getMetadata(driver);
    if (!metadata) { error = "SENSOR_DRIVER_UNAVAILABLE"; return false; }
    channel = requested ? requested : "";
    if (channel == "accel_x") channel = "accelX";
    else if (channel == "accel_y") channel = "accelY";
    else if (channel == "accel_z") channel = "accelZ";
    else if (channel == "gyro_x") channel = "gyroX";
    else if (channel == "gyro_y") channel = "gyroY";
    else if (channel == "gyro_z") channel = "gyroZ";
    for (uint8_t i = 0; i < metadata->activeChannelCount(mode); ++i) {
        const auto* descriptor = metadata->getChannel(i, mode);
        if (channel.isEmpty() || channel == descriptor->name) {
            channel = descriptor->name;
            error = "";
            return true;
        }
    }
    error = "SENSOR_CHANNEL_UNKNOWN: channel is unavailable in the configured mode";
    return false;
}

uint32_t sensorSampleIntervalMs(const PeripheralConfig& config) {
    const auto& sensor = config.params.sensor;
    uint32_t interval = sensor.sampleInterval ? sensor.sampleInterval : 1000;
    const auto* metadata = DriverRegistry::getInstance().getMetadata(sensor.driver);
    if (metadata) interval = std::max(interval, metadata->minIntervalMs);
    if (sensorNameEquals(sensor.driver, "INA226")) {
        interval = std::max(interval, static_cast<uint32_t>(
            (2UL * sensor.conversionTimeUs * sensor.averaging + 999UL) / 1000UL));
    }
    return interval;
}

} // namespace FastBee
