#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_I2C_SENSORS

#include "core/DriverRegistry.h"
#include "core/FeatureBudget.h"
#include "core/RuntimeFeatureManager.h"
#include "peripherals/drivers/I2CSensorBus.h"
#include <cmath>

// Bosch BMP280 BST-BMP280-DS001: short shared-bus transactions, one forced
// conversion and one six-byte burst for a coherent temperature/pressure pair.
// No library begin()/reset() may retain the bus lock while awaiting hardware.
class BMP280Driver final : public ISensorDriver {
public:
    const char* getName() const override { return "BMP280"; }
    uint8_t getChannelCount() const override { return 3; }
    const char* getChannelName(uint8_t channel) const override {
        static const char* const names[] = {"temperature", "pressure", "altitude"};
        return channel < 3 ? names[channel] : "unknown";
    }
    const char* getChannelUnit(uint8_t channel) const override {
        static const char* const units[] = {"\xC2\xB0" "C", "hPa", "m"};
        return channel < 3 ? units[channel] : "";
    }

    bool init(uint8_t pin, const char* params = nullptr) override {
        (void)pin;
        deinit();
        if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("i2cSensors") ||
            FastBee::FeatureBudgetRegistry::instance().isDenied("i2cSensors")) {
            return _bus.fail("BMP280_FEATURE_UNAVAILABLE");
        }
        JsonDocument doc;
        if (!_bus.open(this, getName(), params, doc)) return false;
        uint8_t id = 0;
        if (!_bus.readRegister(0xd0, &id, 1)) return failInit();
        if (id != 0x58) { _bus.fail("BMP280_DEVICE_ID_MISMATCH"); return failInit(); }
        if (!writeRegister(0xe0, 0xb6)) return failInit();
        delay(3);
        if (!waitReady(50)) return failInit();
        uint8_t calibration[24];
        if (!_bus.readRegister(0x88, calibration, sizeof(calibration))) return failInit();
        for (uint8_t i = 0; i < 12; ++i) {
            _calibration[i] = static_cast<uint16_t>(calibration[2 * i]) |
                (static_cast<uint16_t>(calibration[2 * i + 1]) << 8);
        }
        if (!_calibration[0] || !_calibration[3] ||
            _calibration[0] == 0xffff || _calibration[3] == 0xffff) {
            _bus.fail("BMP280_CALIBRATION_INVALID"); return failInit();
        }
        _ready = true;
        return true;
    }

    bool read(SensorReading& reading) override {
        reading = SensorReading();
        if (!_ready) return _bus.fail("BMP280_NOT_INITIALIZED");
        // Sleep permits config writes; then temperature x2, pressure x16 and
        // forced mode. Reapply after module-only power cycles as well.
        if (!writeRegister(0xf4, 0) || !writeRegister(0xf5, 0x10) ||
            !writeRegister(0xf4, 0x55)) return false;
        delay(45);
        if (!waitReady(60)) return false;
        uint8_t data[6];
        if (!_bus.readRegister(0xf7, data, sizeof(data))) return false;
        const uint32_t rawPressure = (uint32_t(data[0]) << 12) |
            (uint32_t(data[1]) << 4) | (data[2] >> 4);
        const uint32_t rawTemperature = (uint32_t(data[3]) << 12) |
            (uint32_t(data[4]) << 4) | (data[5] >> 4);
        if (rawPressure == 0x80000 || rawTemperature == 0x80000) {
            return _bus.fail("BMP280_MEASUREMENT_DISABLED");
        }
        // Floating-point compensation from section 8.1, using the factory
        // trimming values. T1/P1 are unsigned, all remaining terms signed.
        const double temperatureDelta = rawTemperature / 16384.0 - _calibration[0] / 1024.0;
        const double fineDelta = rawTemperature / 131072.0 - _calibration[0] / 8192.0;
        const double tFine = temperatureDelta * signedCalibration(1) +
            fineDelta * fineDelta * signedCalibration(2);
        const double temperature = tFine / 5120.0;
        double var1 = tFine / 2.0 - 64000.0;
        double var2 = var1 * var1 * signedCalibration(8) / 32768.0;
        var2 += var1 * signedCalibration(7) * 2.0;
        var2 = var2 / 4.0 + signedCalibration(6) * 65536.0;
        var1 = (signedCalibration(5) * var1 * var1 / 524288.0 +
                signedCalibration(4) * var1) / 524288.0;
        var1 = (1.0 + var1 / 32768.0) * _calibration[3];
        if (std::fabs(var1) < 0.000001) return _bus.fail("BMP280_CALIBRATION_INVALID");
        double pressure = (1048576.0 - rawPressure - var2 / 4096.0) * 6250.0 / var1;
        var1 = signedCalibration(11) * pressure * pressure / 2147483648.0;
        var2 = pressure * signedCalibration(10) / 32768.0;
        pressure += (var1 + var2 + signedCalibration(9)) / 16.0;
        pressure /= 100.0;
        if (!std::isfinite(temperature) || !std::isfinite(pressure) ||
            temperature < -40 || temperature > 85 || pressure < 300 || pressure > 1100) {
            return _bus.fail("BMP280_MEASUREMENT_OUT_OF_RANGE");
        }
        reading.values[0] = static_cast<float>(temperature);
        reading.values[1] = static_cast<float>(pressure);
        reading.values[2] = static_cast<float>(44330.0 * (1.0 - std::pow(pressure / 1013.25, 0.1903)));
        reading.channelCount = 3;
        reading.timestamp = millis();
        reading.success = true;
        return true;
    }

    void deinit() override { _ready = false; _bus.close(); }
    unsigned long getMinInterval() const override { return 500; }
    const char* getLastError() const override { return _bus.error(); }

private:
    bool writeRegister(uint8_t reg, uint8_t value) {
        const uint8_t bytes[] = {reg, value};
        return _bus.write(bytes, sizeof(bytes));
    }
    bool waitReady(uint32_t timeout) {
        const uint32_t started = static_cast<uint32_t>(millis());
        do {
            uint8_t status = 0;
            if (!_bus.readRegister(0xf3, &status, 1)) return false;
            if (!(status & 0x09)) return true;
            delay(1);
        } while (static_cast<uint32_t>(millis() - started) < timeout);
        return _bus.fail("BMP280_CONVERSION_TIMEOUT");
    }
    bool failInit() { deinit(); return false; }
    int16_t signedCalibration(uint8_t index) const { return static_cast<int16_t>(_calibration[index]); }
    FastBee::I2CSensorBus _bus;
    uint16_t _calibration[12] = {};
    bool _ready = false;
};

FASTBEE_REGISTER_SENSOR("BMP280", BMP280Driver);
#endif
