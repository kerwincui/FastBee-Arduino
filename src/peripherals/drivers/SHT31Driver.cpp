#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_SENSOR_DRIVER

#include "core/DriverRegistry.h"
#include "core/interfaces/ISensorDriver.h"

#include <ArduinoJson.h>
#include <Wire.h>

namespace {

uint8_t readI2cAddress(JsonVariantConst value, uint8_t fallback) {
    if (value.isNull()) return fallback;
    if (value.is<const char*>()) return static_cast<uint8_t>(strtoul(value.as<const char*>(), nullptr, 0));
    return static_cast<uint8_t>(value.as<uint32_t>());
}

int readPin(JsonVariantConst value, int fallback) {
    if (value.isNull()) return fallback;
    return value.as<int>();
}

uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

class SHT31RuntimeDriver : public ISensorDriver {
public:
    const char* getName() const override { return "SHT31"; }
    uint8_t getChannelCount() const override { return 2; }

    const char* getChannelName(uint8_t channel) const override {
        switch (channel) {
            case 0: return "temperature";
            case 1: return "humidity";
            default: return "unknown";
        }
    }

    const char* getChannelUnit(uint8_t channel) const override {
        switch (channel) {
            case 0: return "\xC2\xB0" "C";
            case 1: return "%";
            default: return "";
        }
    }

    bool init(uint8_t pin, const char* params) override {
        (void)pin;
        _addr = 0x44;
        int sda = -1;
        int scl = -1;
        if (params && params[0]) {
            JsonDocument doc;
            if (deserializeJson(doc, params) == DeserializationError::Ok) {
                _addr = readI2cAddress(doc["addr"], _addr);
                sda = readPin(doc["sda"], -1);
                scl = readPin(doc["scl"], -1);
            }
        }
        if (sda >= 0 && scl >= 0) Wire.begin(sda, scl);
        else Wire.begin();
        Wire.beginTransmission(_addr);
        _ready = (Wire.endTransmission() == 0);
        return _ready;
    }

    bool read(SensorReading& reading) override {
        if (!_ready) return false;
        Wire.beginTransmission(_addr);
        Wire.write(0x24);
        Wire.write(0x00);
        if (Wire.endTransmission() != 0) return false;
        delay(20);
        if (Wire.requestFrom(_addr, static_cast<uint8_t>(6)) != 6) return false;
        uint8_t data[6];
        for (uint8_t i = 0; i < 6; ++i) data[i] = Wire.read();
        if (crc8(data, 2) != data[2] || crc8(data + 3, 2) != data[5]) return false;
        uint16_t rawTemp = (static_cast<uint16_t>(data[0]) << 8) | data[1];
        uint16_t rawHum = (static_cast<uint16_t>(data[3]) << 8) | data[4];
        reading.values[0] = -45.0f + 175.0f * static_cast<float>(rawTemp) / 65535.0f;
        reading.values[1] = 100.0f * static_cast<float>(rawHum) / 65535.0f;
        reading.channelCount = 2;
        reading.success = true;
        reading.timestamp = millis();
        return true;
    }

    void deinit() override { _ready = false; }
    unsigned long getMinInterval() const override { return 500; }

private:
    uint8_t _addr = 0x44;
    bool _ready = false;
};

} // namespace

FASTBEE_REGISTER_SENSOR("SHT31", SHT31RuntimeDriver);

#endif
