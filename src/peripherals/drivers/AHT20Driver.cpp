#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_SENSOR_DRIVER

#include "core/DriverRegistry.h"
#include "core/interfaces/ISensorDriver.h"
#include "peripherals/drivers/I2CSensorBus.h"

#include <ArduinoJson.h>
#include <Wire.h>

namespace {

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
        deinit();
        JsonDocument doc;
        if (!_bus.open(this, getName(), params, doc)) return false;
        const uint8_t initialize[] = {0xBE, 0x08, 0x00};
        _ready = _bus.write(initialize, sizeof(initialize));
        delay(10);
        return _ready;
    }

    bool read(SensorReading& reading) override {
        reading = SensorReading();
        if (!_ready) return _bus.fail("AHT20_NOT_INITIALIZED");
        const uint8_t trigger[] = {0xAC, 0x33, 0x00};
        if (!_bus.write(trigger, sizeof(trigger))) return false;
        delay(80);
        uint8_t data[6];
        if (!_bus.read(data, sizeof(data))) return false;
        if (data[0] & 0x80) return _bus.fail("AHT20_CONVERSION_NOT_READY");
        if (!(data[0] & 0x08)) return _bus.fail("AHT20_NOT_CALIBRATED");
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

    void deinit() override { _ready = false; _bus.close(); }
    unsigned long getMinInterval() const override { return 1000; }
    const char* getLastError() const override { return _bus.error(); }

private:
    FastBee::I2CSensorBus _bus;
    bool _ready = false;
};

} // namespace

FASTBEE_REGISTER_SENSOR("AHT20", AHT20Driver);

#endif
