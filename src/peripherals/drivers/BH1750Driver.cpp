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

class BH1750Driver : public ISensorDriver {
public:
    const char* getName() const override { return "BH1750"; }
    uint8_t getChannelCount() const override { return 1; }

    const char* getChannelName(uint8_t channel) const override {
        return channel == 0 ? "illuminance" : "unknown";
    }

    const char* getChannelUnit(uint8_t channel) const override {
        return channel == 0 ? "lx" : "";
    }

    bool init(uint8_t pin, const char* params) override {
        (void)pin;
        _addr = 0x23;
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
        if (!writeCommand(0x01)) return false;
        delay(10);
        writeCommand(0x07);
        _ready = writeCommand(0x10);
        delay(180);
        return _ready;
    }

    bool read(SensorReading& reading) override {
        if (!_ready) return false;
        if (!writeCommand(0x10)) return false;
        delay(180);
        if (Wire.requestFrom(_addr, static_cast<uint8_t>(2)) != 2) return false;
        uint16_t raw = (static_cast<uint16_t>(Wire.read()) << 8) | Wire.read();
        reading.values[0] = static_cast<float>(raw) / 1.2f;
        reading.channelCount = 1;
        reading.success = true;
        reading.timestamp = millis();
        return true;
    }

    void deinit() override { _ready = false; }
    unsigned long getMinInterval() const override { return 500; }

private:
    bool writeCommand(uint8_t cmd) {
        Wire.beginTransmission(_addr);
        Wire.write(cmd);
        return Wire.endTransmission() == 0;
    }

    uint8_t _addr = 0x23;
    bool _ready = false;
};

} // namespace

FASTBEE_REGISTER_SENSOR("BH1750", BH1750Driver);

#endif
