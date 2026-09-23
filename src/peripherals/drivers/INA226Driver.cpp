#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_I2C_SENSORS

#include "peripherals/drivers/INA226Driver.h"
#include "core/DriverRegistry.h"
#include "core/FeatureBudget.h"
#include "core/RuntimeFeatureManager.h"
#include <algorithm>
#include <cmath>

namespace FastBee {

bool calculateINA226Calibration(double shuntResistance, double maxCurrent,
                                uint16_t conversionTimeUs, uint16_t averaging,
                                INA226Calibration& calibration, const char*& error) {
    calibration = INA226Calibration();
    error = "";
    if (!std::isfinite(shuntResistance) || !std::isfinite(maxCurrent) ||
        shuntResistance < 0.000001 || shuntResistance > 1000 ||
        maxCurrent < 0.000001 || maxCurrent > 1000000) {
        error = "INA226_CALIBRATION_REQUIRED";
        return false;
    }
    if (shuntResistance * maxCurrent > 0.0819175) {
        error = "INA226_SHUNT_RANGE_EXCEEDED";
        return false;
    }
    constexpr uint16_t times[] = {140, 204, 332, 588, 1100, 2116, 4156, 8244};
    constexpr uint16_t averages[] = {1, 4, 16, 64, 128, 256, 512, 1024};
    int timeIndex = -1, averageIndex = -1;
    for (int i = 0; i < 8; ++i) {
        if (times[i] == conversionTimeUs) timeIndex = i;
        if (averages[i] == averaging) averageIndex = i;
    }
    if (timeIndex < 0 || averageIndex < 0) {
        error = "INA226_CONVERSION_CONFIG_INVALID";
        return false;
    }
    // Both shunt and bus conversions are required. Reject combinations that
    // would occupy a rule worker for seconds; bus locks are never held waiting.
    const uint32_t conversionMs =
        (2UL * conversionTimeUs * averaging + 999UL) / 1000UL;
    if (conversionMs > 500) {
        error = "INA226_CONVERSION_TOO_SLOW";
        return false;
    }
    // TI INA226 SBOS547C section 7.1.6: D15 is reserved. Choosing the LSB
    // from the final integer calibration avoids a truncation scale error.
    const double desiredLsb = maxCurrent / 32767.0;
    const double idealCalibration = 0.00512 / (desiredLsb * shuntResistance);
    const double boundedCalibration = std::min(32767.0, std::floor(idealCalibration));
    if (!std::isfinite(boundedCalibration) || boundedCalibration < 1) {
        error = "INA226_CALIBRATION_RANGE_INVALID";
        return false;
    }
    calibration.registerValue = static_cast<uint16_t>(boundedCalibration);
    calibration.currentLsb = 0.00512 / (boundedCalibration * shuntResistance);
    calibration.configValue = static_cast<uint16_t>(0x4000U |
        (averageIndex << 9) | (timeIndex << 6) | (timeIndex << 3));
    calibration.conversionMs = conversionMs;
    return true;
}

const char* INA226Driver::getChannelName(uint8_t channel) const {
    static const char* const names[] = {"voltage", "current", "power"};
    return channel < 3 ? names[channel] : "unknown";
}

const char* INA226Driver::getChannelUnit(uint8_t channel) const {
    static const char* const units[] = {"V", "A", "W"};
    return channel < 3 ? units[channel] : "";
}

bool INA226Driver::init(uint8_t pin, const char* params) {
    (void)pin;
    deinit();
    if (!RuntimeFeatureManager::instance().shouldEnable("i2cSensors") ||
        FeatureBudgetRegistry::instance().isDenied("i2cSensors")) {
        return _bus.fail("INA226_FEATURE_UNAVAILABLE");
    }
    JsonDocument doc;
    if (!_bus.open(this, getName(), params, doc)) return false;
    if (!doc["shuntResistance"].is<double>() || !doc["maxCurrent"].is<double>() ||
        (!doc["conversionTimeUs"].isNull() && !doc["conversionTimeUs"].is<uint16_t>()) ||
        (!doc["averaging"].isNull() && !doc["averaging"].is<uint16_t>())) {
        deinit();
        return _bus.fail("INA226_CALIBRATION_REQUIRED");
    }
    _maxCurrent = doc["maxCurrent"].as<double>();
    const char* error = "";
    if (!calculateINA226Calibration(doc["shuntResistance"].as<double>(), _maxCurrent,
            doc["conversionTimeUs"] | uint16_t(1100), doc["averaging"] | uint16_t(16),
            _calibration, error)) {
        deinit();
        return _bus.fail(error);
    }
    uint16_t manufacturer = 0, die = 0;
    if (!_bus.readRegister16(0xfe, manufacturer) || !_bus.readRegister16(0xff, die)) {
        deinit();
        return false;
    }
    if (manufacturer != 0x5449 || (die & 0xfff0) != 0x2260) {
        deinit();
        return _bus.fail("INA226_DEVICE_ID_MISMATCH");
    }
    if (!_bus.writeRegister16(0x00, _calibration.configValue) ||
        !_bus.writeRegister16(0x05, _calibration.registerValue)) {
        deinit();
        return false;
    }
    _ready = true;
    return true;
}

bool INA226Driver::read(SensorReading& reading) {
    reading = SensorReading();
    if (!_ready) return _bus.fail("INA226_NOT_INITIALIZED");
    // Reapply calibration before every trigger, including after a module-only
    // power cycle. Reading a reset calibration of zero as 0 A would be unsafe.
    if (!_bus.writeRegister16(0x05, _calibration.registerValue) ||
        !_bus.writeRegister16(0x00, _calibration.configValue | 0x0003)) return false;
    const uint32_t started = static_cast<uint32_t>(millis());
    const uint32_t timeoutMs = _calibration.conversionMs + 50;
    uint16_t status = 0;
    do {
        delay(1);
        if (!_bus.readRegister16(0x06, status)) return false;
        if (status & 0x0004) return _bus.fail("INA226_MATH_OVERFLOW");
        if (status & 0x0008) break;
    } while (static_cast<uint32_t>(static_cast<uint32_t>(millis()) - started) < timeoutMs);
    if (!(status & 0x0008)) return _bus.fail("INA226_CONVERSION_TIMEOUT");

    uint16_t calibration = 0, bus = 0, current = 0, shunt = 0;
    if (!_bus.readRegister16(0x05, calibration) ||
        !_bus.readRegister16(0x01, shunt) ||
        !_bus.readRegister16(0x02, bus) || !_bus.readRegister16(0x04, current)) return false;
    if (calibration != _calibration.registerValue) return _bus.fail("INA226_CALIBRATION_LOST");
    if (shunt == 0x7fff || shunt == 0x8000 || bus > 28800) return _bus.fail("INA226_MEASUREMENT_OUT_OF_RANGE");
    const double voltage = bus * 0.00125;
    const double amperes = static_cast<int16_t>(current) * _calibration.currentLsb;
    if (std::fabs(amperes) > _maxCurrent + _calibration.currentLsb) {
        return _bus.fail("INA226_CURRENT_OUT_OF_RANGE");
    }
    reading.values[0] = static_cast<float>(voltage);
    reading.values[1] = static_cast<float>(amperes);
    // Signed power from this pair of averaged readings, not the unsigned
    // hardware power register. This preserves direction for reverse current.
    reading.values[2] = static_cast<float>(voltage * amperes);
    reading.channelCount = 3;
    reading.timestamp = millis();
    reading.success = true;
    return true;
}

void INA226Driver::deinit() {
    _ready = false;
    _bus.close();
}

unsigned long INA226Driver::getMinInterval() const {
    return std::max<uint32_t>(100, _calibration.conversionMs);
}

} // namespace FastBee

using FastBee::INA226Driver;
FASTBEE_REGISTER_SENSOR("INA226", INA226Driver);

#endif
