#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_SENSOR_DRIVER

#include "core/DriverRegistry.h"
#include "core/interfaces/ISensorDriver.h"
#include "peripherals/drivers/I2CSensorBus.h"

#include <ArduinoJson.h>
#include <Wire.h>

namespace {

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
        deinit();
        JsonDocument doc;
        if (!_bus.open(this, getName(), params, doc)) return false;
        if (!writeCommand(0x01)) return false;
        delay(10);
        if (!writeCommand(0x07)) return false;
        _ready = writeCommand(0x10);
        delay(180);
        return _ready;
    }

    bool read(SensorReading& reading) override {
        reading = SensorReading();
        if (!_ready) return _bus.fail("BH1750_NOT_INITIALIZED");
        if (!writeCommand(0x10)) return false;
        delay(180);
        uint8_t data[2];
        if (!_bus.read(data, sizeof(data))) return false;
        uint16_t raw = (static_cast<uint16_t>(data[0]) << 8) | data[1];
        reading.values[0] = static_cast<float>(raw) / 1.2f;
        reading.channelCount = 1;
        reading.success = true;
        reading.timestamp = millis();
        return true;
    }

    void deinit() override { _ready = false; _bus.close(); }
    unsigned long getMinInterval() const override { return 500; }
    const char* getLastError() const override { return _bus.error(); }

private:
    bool writeCommand(uint8_t cmd) {
        return _bus.write(&cmd, 1);
    }

    FastBee::I2CSensorBus _bus;
    bool _ready = false;
};

} // namespace

FASTBEE_REGISTER_SENSOR("BH1750", BH1750Driver);

#endif
