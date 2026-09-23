#ifndef FASTBEE_SENSOR_DRIVER_METADATA_H
#define FASTBEE_SENSOR_DRIVER_METADATA_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace FastBee {

inline bool sensorNameEquals(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return false;
    }
    return *a == *b;
}

struct SensorChannelMetadata {
    const char* name;
    const char* unit;
    // 0: available in every mode; 1: acceleration; 2: gyroscope.
    uint8_t mode = 0;
};

struct SensorParameterMetadata {
    const char* name;
    const char* type;  // number or integer
    const char* unit;
    double defaultValue;
    double minValue;
    double maxValue;
    bool required;
    const uint16_t* choices = nullptr;
    uint8_t choiceCount = 0;
};

struct SensorDriverMetadata {
    const char* name;
    uint8_t defaultAddress;
    uint8_t addressMin;
    uint8_t addressMax;
    const uint8_t* addresses;
    uint8_t addressCount;
    const SensorChannelMetadata* channels;
    uint8_t channelCount;
    const SensorParameterMetadata* params;
    uint8_t paramCount;
    uint32_t minIntervalMs;
    const char* featureKey;

    bool supportsAddress(uint8_t address) const {
        if (address < addressMin || address > addressMax) return false;
        if (!addressCount) return true;
        for (uint8_t i = 0; i < addressCount; ++i) {
            if (addresses[i] == address) return true;
        }
        return false;
    }

    // The persisted MPU6050 mode remains 0=accel, 1=gyro.
    const SensorChannelMetadata* getChannel(uint8_t index, uint8_t mode = 0) const {
        for (uint8_t i = 0; i < channelCount; ++i) {
            if (channels[i].mode && channels[i].mode != mode + 1) continue;
            if (index-- == 0) return &channels[i];
        }
        return nullptr;
    }

    uint8_t activeChannelCount(uint8_t mode = 0) const {
        uint8_t count = 0;
        while (getChannel(count, mode)) ++count;
        return count;
    }
};

// Immutable, bounded metadata. Availability is determined by DriverRegistry,
// not by this catalogue: metadata never instantiates or probes hardware.
inline const SensorDriverMetadata* sensorDriverMetadata(const char* name) {
    static constexpr uint8_t shtAddresses[] = {0x44, 0x45};
    static constexpr uint8_t bhAddresses[] = {0x23, 0x5c};
    static constexpr SensorChannelMetadata temperatureHumidity[] = {
        {"temperature", "\xC2\xB0" "C"}, {"humidity", "%"}};
    static constexpr SensorChannelMetadata illuminance[] = {{"illuminance", "lx"}};
    static constexpr SensorChannelMetadata bmpChannels[] = {
        {"temperature", "\xC2\xB0" "C"}, {"pressure", "hPa"}, {"altitude", "m"}};
    static constexpr SensorChannelMetadata mpuChannels[] = {
        {"accelX", "m/s\xC2\xB2", 1}, {"accelY", "m/s\xC2\xB2", 1},
        {"accelZ", "m/s\xC2\xB2", 1}, {"gyroX", "rad/s", 2},
        {"gyroY", "rad/s", 2}, {"gyroZ", "rad/s", 2},
        {"temperature", "\xC2\xB0" "C"}};
    static constexpr SensorChannelMetadata inaChannels[] = {
        {"voltage", "V"}, {"current", "A"}, {"power", "W"}};
    static constexpr uint16_t modes[] = {0, 1};
    static constexpr SensorParameterMetadata mpuParams[] = {
        {"mode", "integer", "", 0, 0, 1, false, modes, 2}};
    static constexpr uint16_t conversionTimes[] = {140, 204, 332, 588, 1100, 2116, 4156, 8244};
    static constexpr uint16_t averages[] = {1, 4, 16, 64, 128, 256, 512, 1024};
    static constexpr SensorParameterMetadata inaParams[] = {
        {"shuntResistance", "number", "ohm", 0, 0.000001, 1000, true},
        {"maxCurrent", "number", "A", 0, 0.000001, 1000000, true},
        {"conversionTimeUs", "integer", "us", 1100, 140, 8244, false, conversionTimes, 8},
        {"averaging", "integer", "", 16, 1, 1024, false, averages, 8}};
    static constexpr SensorDriverMetadata catalogue[] = {
        {"AHT20", 0x38, 0x38, 0x38, nullptr, 0, temperatureHumidity, 2, nullptr, 0, 1000, "i2cSensors"},
        {"BH1750", 0x23, 0x23, 0x5c, bhAddresses, 2, illuminance, 1, nullptr, 0, 500, "i2cSensors"},
        {"BMP280", 0x76, 0x76, 0x77, nullptr, 0, bmpChannels, 3, nullptr, 0, 500, "i2cSensors"},
        {"MPU6050", 0x68, 0x68, 0x69, nullptr, 0, mpuChannels, 7, mpuParams, 1, 100, "i2cSensors"},
        {"SHT30", 0x44, 0x44, 0x45, shtAddresses, 2, temperatureHumidity, 2, nullptr, 0, 500, "i2cSensors"},
        {"SHT31", 0x44, 0x44, 0x45, shtAddresses, 2, temperatureHumidity, 2, nullptr, 0, 500, "i2cSensors"},
        {"INA226", 0x40, 0x40, 0x4f, nullptr, 0, inaChannels, 3, inaParams, 4, 100, "i2cSensors"}
    };
    for (const auto& item : catalogue) {
        if (sensorNameEquals(item.name, name)) return &item;
    }
    return nullptr;
}

} // namespace FastBee
#endif
