#include "core/SensorConfigSupport.h"
#include "core/DriverRegistry.h"
#include <ArduinoJson/Variant/JsonVariantVisitor.hpp>
#include "peripherals/drivers/ADS1115Contract.h"
#if FASTBEE_ENABLE_I2C_SENSORS
#include "peripherals/drivers/INA226Driver.h"
#endif
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace FastBee {
namespace {
// Public is<float>() also accepts double/integer variants. The actual stored
// kind is needed here: ArduinoJson 7.4 parses short decimal tokens (1e-6) into
// float, while explicit higher-precision out-of-range values must stay invalid.
// This one adapter is tied to the installed library; behavior tests exercise
// the real deserializer so a library upgrade cannot silently change admission.
struct StoredFloatVisitor : ArduinoJson::detail::JsonVariantVisitor<bool> {
    using ArduinoJson::detail::JsonVariantVisitor<bool>::visit;
    bool visit(float) { return true; }
};
bool jsonStoresFloat(JsonVariantConst input) {
    StoredFloatVisitor visitor;
    return ArduinoJson::detail::accept(input, visitor);
}
bool configNumber(JsonVariantConst input, double fallback, double minimum,
                  double maximum, bool integer, double& output,
                  const SensorParameterMetadata* floatEndpoint = nullptr) {
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
    } else if (input.is<double>()) {
        output = input.as<double>();
        if (floatEndpoint && jsonStoresFloat(input))
            output = floatEndpoint->canonicalFloatValue(input.as<float>());
    }
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

bool parseSensorMode(JsonVariantConst input, uint8_t fallback, uint8_t& output) {
    if (input.is<const char*>() && sensorNameEquals(input.as<const char*>(), "gyro")) output = 1;
    else if (input.is<const char*>() && sensorNameEquals(input.as<const char*>(), "accel")) output = 0;
    else {
        double number = 0;
        if (!configNumber(input, fallback, 0, 1, true, number)) return false;
        output = static_cast<uint8_t>(number);
    }
    return true;
}

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
    if (!parseSensorMode(params["mode"], changedDriver ? 0 : sensor.mode, sensor.mode)) {
        error = "SENSOR_MODE_INVALID"; return false;
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
    if (sensorNameEquals(sensor.driver, "ADS1115")) {
        const auto parameter = [&](const char* key, double previous, bool reset,
                                   const char* reason, bool storedFloat = false) {
            const auto* field = meta ? meta->getParameter(key) : nullptr;
            if (!field || !configNumber(params[key], reset ? field->defaultValue :
                    storedFloat ? field->canonicalFloatValue(static_cast<float>(previous)) : previous,
                    field->minValue, field->maxValue, !std::strcmp(field->type, "integer"), number,
                    storedFloat ? field : nullptr) ||
                !field->acceptsNumber(number)) {
                error = reason; return false;
            }
            return true;
        };
        if (!parameter("mux", sensor.mux, changedDriver, "ADS1115_CONFIG_INVALID")) return false;
        sensor.mux = static_cast<uint8_t>(number);
        if (!parameter("scanMask", sensor.scanMask, changedDriver, "ADS1115_SCAN_MASK_INVALID") ||
            !ads1115ScanMaskValid(static_cast<uint32_t>(number))) {
            error = "ADS1115_SCAN_MASK_INVALID"; return false;
        }
        sensor.scanMask = static_cast<uint8_t>(number);
        if (!parameter("fullScaleMv", sensor.fullScaleMv, changedDriver || !sensor.fullScaleMv,
                       "ADS1115_CONFIG_INVALID")) return false;
        sensor.fullScaleMv = static_cast<uint16_t>(number);
        if (!parameter("dataRate", sensor.dataRate, changedDriver || !sensor.dataRate,
                       "ADS1115_CONFIG_INVALID")) return false;
        sensor.dataRate = static_cast<uint16_t>(number);
        ADS1115ConversionConfig adsConfig;
        if (!makeADS1115ConversionConfig(sensor.mux, sensor.fullScaleMv, sensor.dataRate, adsConfig) ||
            !ads1115ScanMaskValid(sensor.scanMask)) {
            error = "ADS1115_CONFIG_INVALID"; return false;
        }
        if (!parameter("calibrationGain", sensor.calibrationGain, changedDriver || !sensor.calibrationGain,
                       "ADS1115_CALIBRATION_INVALID", true)) return false;
        sensor.calibrationGain = static_cast<float>(number);
        if (!parameter("calibrationOffset", sensor.calibrationOffset, changedDriver,
                       "ADS1115_CALIBRATION_INVALID", true)) return false;
        sensor.calibrationOffset = static_cast<float>(number);
    }
    config.params.sensor = sensor;
    error = "";
    return true;
}

void serializeSensorConfig(JsonObject params, const PeripheralConfig& config) {
    const auto& sensor = config.params.sensor;
    params["sensorType"] = sensor.sensorType;
    params["sampleInterval"] = sensor.sampleInterval ? sensor.sampleInterval : 1000;
    if (!sensor.driver[0]) return;
    params["driver"] = JsonString(sensor.driver, false);
    params["address"] = sensor.address;
    params["mode"] = sensor.mode;
    params["frequency"] = sensor.frequency;
    params["busPeripheralId"] = JsonString(sensor.busPeripheralId, false);
    if (sensorNameEquals(sensor.driver, "ADS1115")) {
        params["mux"] = sensor.mux;
        params["scanMask"] = sensor.scanMask;
        params["fullScaleMv"] = sensor.fullScaleMv;
        params["dataRate"] = sensor.dataRate;
        const auto* meta = sensorDriverMetadata(sensor.driver);
        const auto* gain = meta ? meta->getParameter("calibrationGain") : nullptr;
        const auto* offset = meta ? meta->getParameter("calibrationOffset") : nullptr;
        params["calibrationGain"] = gain ? gain->canonicalFloatValue(sensor.calibrationGain) : double(sensor.calibrationGain);
        params["calibrationOffset"] = offset ? offset->canonicalFloatValue(sensor.calibrationOffset) : double(sensor.calibrationOffset);
    }
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
    if (sensorNameEquals(sensor.driver, "ADS1115")) {
        ADS1115ConversionConfig adsConfig;
        if (!makeADS1115ConversionConfig(sensor.mux, sensor.fullScaleMv, sensor.dataRate, adsConfig) ||
            !ads1115ScanMaskValid(sensor.scanMask)) {
            error = "ADS1115_CONFIG_INVALID"; return false;
        }
        const auto* gain = meta->getParameter("calibrationGain");
        const auto* offset = meta->getParameter("calibrationOffset");
        if (!gain || !offset || !gain->acceptsNumber(gain->canonicalFloatValue(sensor.calibrationGain)) ||
            !offset->acceptsNumber(offset->canonicalFloatValue(sensor.calibrationOffset))) {
            error = "ADS1115_CALIBRATION_INVALID"; return false;
        }
        const uint32_t budget = std::max<uint32_t>(100, ads1115ReadBudgetMs(sensor.dataRate, sensor.scanMask));
        if (sensor.sampleInterval && sensor.sampleInterval < budget) {
            error = "ADS1115_INTERVAL_TOO_SHORT"; return false;
        }
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
    if (sensorNameEquals(sensor.driver, "ADS1115")) {
        params["mux"] = sensor.mux;
        params["scanMask"] = sensor.scanMask;
        params["fullScaleMv"] = sensor.fullScaleMv;
        params["dataRate"] = sensor.dataRate;
        params["calibrationGain"] = sensor.calibrationGain;
        params["calibrationOffset"] = sensor.calibrationOffset;
    }
}

bool resolveSensorChannel(const char* driver, uint8_t mode, const char* requested,
                          String& channel, String& error, uint8_t scanMask) {
    const auto* metadata = DriverRegistry::getInstance().getMetadata(driver);
    if (!metadata) { error = "SENSOR_DRIVER_UNAVAILABLE"; return false; }
    channel = requested ? requested : "";
    if (channel == "accel_x") channel = "accelX";
    else if (channel == "accel_y") channel = "accelY";
    else if (channel == "accel_z") channel = "accelZ";
    else if (channel == "gyro_x") channel = "gyroX";
    else if (channel == "gyro_y") channel = "gyroY";
    else if (channel == "gyro_z") channel = "gyroZ";
    for (uint8_t i = 0; i < metadata->activeChannelCount(mode, scanMask); ++i) {
        const auto* descriptor = metadata->getChannel(i, mode, scanMask);
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
    if (sensorNameEquals(sensor.driver, "ADS1115") && sensor.dataRate) {
        interval = std::max<uint32_t>(interval, ads1115ReadBudgetMs(sensor.dataRate, sensor.scanMask));
    }
    if (sensorNameEquals(sensor.driver, "INA226")) {
        interval = std::max(interval, static_cast<uint32_t>(
            (2UL * sensor.conversionTimeUs * sensor.averaging + 999UL) / 1000UL));
    }
    return interval;
}

} // namespace FastBee
