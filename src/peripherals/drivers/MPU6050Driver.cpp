#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_I2C_SENSORS

#include "core/DriverRegistry.h"
#include "core/FeatureBudget.h"
#include "core/RuntimeFeatureManager.h"
#include "peripherals/drivers/I2CSensorBus.h"

// MPU-6000/MPU-6050 register map rev 4.2. Preserve the public four-channel
// accel/gyro modes while avoiding vendor begin()'s unbounded reset polling.
class MPU6050Driver final : public ISensorDriver {
public:
    const char* getName() const override { return "MPU6050"; }
    uint8_t getChannelCount() const override { return 4; }
    const char* getChannelName(uint8_t channel) const override {
        static const char* const accel[] = {"accelX", "accelY", "accelZ", "temperature"};
        static const char* const gyro[] = {"gyroX", "gyroY", "gyroZ", "temperature"};
        return channel < 4 ? (_gyroMode ? gyro[channel] : accel[channel]) : "unknown";
    }
    const char* getChannelUnit(uint8_t channel) const override {
        return channel > 3 ? "" : channel == 3 ? "\xC2\xB0" "C" :
            _gyroMode ? "rad/s" : "m/s\xC2\xB2";
    }

    bool init(uint8_t pin, const char* params = nullptr) override {
        (void)pin;
        deinit();
        if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("i2cSensors") ||
            FastBee::FeatureBudgetRegistry::instance().isDenied("i2cSensors")) {
            return _bus.fail("MPU6050_FEATURE_UNAVAILABLE");
        }
        JsonDocument doc;
        if (!_bus.open(this, getName(), params, doc)) return false;
        _gyroMode = doc["mode"].is<const char*>()
            ? strcmp(doc["mode"].as<const char*>(), "gyro") == 0
            : (doc["mode"] | 0) == 1;
        uint8_t id = 0;
        if (!_bus.readRegister(0x75, &id, 1)) return failInit();
        if (id != 0x68) { _bus.fail("MPU6050_DEVICE_ID_MISMATCH"); return failInit(); }
        if (!writeRegister(0x6b, 0x80)) return failInit();
        const uint32_t started = static_cast<uint32_t>(millis());
        uint8_t power = 0x80;
        do {
            delay(1);
            if (!_bus.readRegister(0x6b, &power, 1)) return failInit();
            if (!(power & 0x80)) break;
        } while (static_cast<uint32_t>(millis() - started) < 200);
        if (power & 0x80) { _bus.fail("MPU6050_RESET_TIMEOUT"); return failInit(); }
        delay(100);
        if (!writeRegister(0x68, 0x07)) return failInit();
        delay(100);
        if (!configure()) return failInit();
        _ready = true;
        return true;
    }

    bool read(SensorReading& reading) override {
        reading = SensorReading();
        if (!_ready) return _bus.fail("MPU6050_NOT_INITIALIZED");
        uint8_t power = 0, config[3] = {};
        if (!_bus.readRegister(0x6b, &power, 1) ||
            !_bus.readRegister(0x1a, config, sizeof(config))) return false;
        // A module-only reset must not silently change the acceleration/gyro
        // scale. Restore and allow stabilization before publishing a sample.
        if (power != 1 || config[0] != 4 || config[1] != 8 || config[2] != 16) {
            if (!configure()) return false;
        }
        uint8_t data[14];
        if (!_bus.readRegister(0x3b, data, sizeof(data))) return false;
        const uint8_t offset = _gyroMode ? 8 : 0;
        const float scale = _gyroMode ? (0.0174532925199433f / 65.5f) : (9.80665f / 4096.0f);
        for (uint8_t axis = 0; axis < 3; ++axis) {
            const uint8_t index = offset + axis * 2;
            const int16_t raw = static_cast<int16_t>((uint16_t(data[index]) << 8) | data[index + 1]);
            if (raw == INT16_MIN || raw == INT16_MAX) return _bus.fail("MPU6050_MEASUREMENT_OUT_OF_RANGE");
            reading.values[axis] = raw * scale;
        }
        const int16_t temperature = static_cast<int16_t>((uint16_t(data[6]) << 8) | data[7]);
        reading.values[3] = temperature / 340.0f + 36.53f;
        reading.channelCount = 4;
        reading.timestamp = millis();
        reading.success = true;
        return true;
    }

    void deinit() override { _ready = false; _bus.close(); }
    unsigned long getMinInterval() const override { return 100; }
    const char* getLastError() const override { return _bus.error(); }

private:
    bool writeRegister(uint8_t reg, uint8_t value) {
        const uint8_t bytes[] = {reg, value};
        return _bus.write(bytes, sizeof(bytes));
    }
    bool configure() {
        // 1 kHz sampling, 21 Hz low-pass, +/-500 deg/s and +/-8 g.
        if (!writeRegister(0x19, 0) || !writeRegister(0x1a, 4) ||
            !writeRegister(0x1b, 8) || !writeRegister(0x1c, 16) ||
            !writeRegister(0x6c, 0) || !writeRegister(0x6b, 1)) return false;
        delay(100);
        return true;
    }
    bool failInit() { deinit(); return false; }
    FastBee::I2CSensorBus _bus;
    bool _ready = false;
    bool _gyroMode = false;
};

FASTBEE_REGISTER_SENSOR("MPU6050", MPU6050Driver);
#endif
