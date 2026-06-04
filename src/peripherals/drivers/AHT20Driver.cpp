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

class AHT20Driver : public ISensorDriver {
public:
    const char* getName() const override { return "AHT20"; }
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
        _addr = 0x38;
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
        if (Wire.endTransmission() != 0) return false;
        Wire.beginTransmission(_addr);
        Wire.write(0xBE);
        Wire.write(0x08);
        Wire.write(0x00);
        _ready = (Wire.endTransmission() == 0);
        delay(10);
        return _ready;
    }

    bool read(SensorReading& reading) override {
        if (!_ready) return false;
        Wire.beginTransmission(_addr);
        Wire.write(0xAC);
        Wire.write(0x33);
        Wire.write(0x00);
        if (Wire.endTransmission() != 0) return false;
        delay(80);
        if (Wire.requestFrom(_addr, static_cast<uint8_t>(6)) != 6) return false;
        uint8_t data[6];
        for (uint8_t i = 0; i < 6; ++i) data[i] = Wire.read();
        if (data[0] & 0x80) return false;
        uint32_t rawHum = (static_cast<uint32_t>(data[1]) << 12) |
                          (static_cast<uint32_t>(data[2]) << 4) |
                          (static_cast<uint32_t>(data[3]) >> 4);
        uint32_t rawTemp = ((static_cast<uint32_t>(data[3]) & 0x0F) << 16) |
                           (static_cast<uint32_t>(data[4]) << 8) |
                           data[5];
        reading.values[0] = (static_cast<float>(rawTemp) * 200.0f / 1048576.0f) - 50.0f;
        reading.values[1] = static_cast<float>(rawHum) * 100.0f / 1048576.0f;
        reading.channelCount = 2;
        reading.success = true;
        reading.timestamp = millis();
        return true;
    }

    void deinit() override { _ready = false; }
    unsigned long getMinInterval() const override { return 1000; }

private:
    uint8_t _addr = 0x38;
    bool _ready = false;
};

} // namespace

FASTBEE_REGISTER_SENSOR("AHT20", AHT20Driver);

#endif
