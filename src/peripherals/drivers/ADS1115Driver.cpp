#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_I2C_SENSORS
#include "peripherals/drivers/ADS1115Driver.h"
#include "core/DriverRegistry.h"
#include "core/SensorDriverMetadata.h"
#include "core/FeatureBudget.h"
#include "core/RuntimeFeatureManager.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace FastBee {
namespace {
const char* sessionError(ADS1115ConversionSession::Error error) {
    using E = ADS1115ConversionSession::Error;
    switch (error) {
    case E::Timeout: return "ADS1115_CONVERSION_TIMEOUT";
    case E::StartNotObserved: return "ADS1115_START_NOT_OBSERVED";
    case E::ConfigLost: return "ADS1115_CONFIG_LOST";
    case E::Cancelled: return "ADS1115_CANCELLED";
    case E::GenerationExhausted: return "ADS1115_GENERATION_EXHAUSTED";
    default: return "ADS1115_CONVERSION_FAILED";
    }
}
bool integerParameter(JsonVariantConst input) {
    return input.isNull() || (input.is<uint16_t>() && !input.is<bool>());
}
}
const char* ADS1115Driver::getChannelName(uint8_t channel) const {
    const auto* item = sensorDriverMetadata("ADS1115")->getChannel(channel, 0, _scanMask);
    return item ? item->name : "unknown";
}
const char* ADS1115Driver::getChannelUnit(uint8_t channel) const {
    const auto* item = sensorDriverMetadata("ADS1115")->getChannel(channel, 0, _scanMask);
    return item ? item->unit : "";
}
bool ADS1115Driver::init(uint8_t pin, const char* params) {
    (void)pin;
    _ready = false;
    _scanActive = false;
    _scanMask = 0;
    _session.cancel(_session.generation());
    if (!_bus.close()) return false;
    if (!RuntimeFeatureManager::instance().shouldEnable("i2cSensors") ||
        FeatureBudgetRegistry::instance().isDenied("i2cSensors"))
        return _bus.fail("ADS1115_FEATURE_UNAVAILABLE");
    JsonDocument doc;
    if (params && params[0] &&
        (deserializeJson(doc, params) != DeserializationError::Ok || !doc.is<JsonObject>()))
        return _bus.fail("SENSOR_PARAMS_INVALID");
    if (!integerParameter(doc["mux"]) || !integerParameter(doc["fullScaleMv"]) ||
        !integerParameter(doc["dataRate"]) || !integerParameter(doc["scanMask"])) return _bus.fail("ADS1115_CONFIG_INVALID");
    const uint16_t mux = doc["mux"] | uint16_t(4);
    const uint16_t scanMask = doc["scanMask"] | uint16_t(0);
    if (!ads1115ScanMaskValid(scanMask)) return _bus.fail("ADS1115_SCAN_MASK_INVALID");
    const uint16_t fullScale = doc["fullScaleMv"] | uint16_t(4096);
    const uint16_t rate = doc["dataRate"] | uint16_t(128);
    ADS1115ConversionConfig config;
    if (!makeADS1115ConversionConfig(mux, fullScale, rate, config))
        return _bus.fail("ADS1115_CONFIG_INVALID");
    for (const char* key : {"calibrationGain", "calibrationOffset"}) {
        if (!doc[key].isNull() && (!doc[key].is<double>() || doc[key].is<bool>()))
            return _bus.fail("ADS1115_CALIBRATION_INVALID");
    }
    const double gain = doc["calibrationGain"] | 1.0;
    const double offset = doc["calibrationOffset"] | 0.0;
    if (!std::isfinite(gain) || gain < 0.000001 || gain > 1000000 ||
        !std::isfinite(offset) || std::fabs(offset) > 1000000)
        return _bus.fail("ADS1115_CALIBRATION_INVALID");
    if (!_bus.open(this, getName(), params, doc)) return false;
    uint16_t status = 0;
    // Readable config is transport evidence, NOT proof of ADS1115 identity.
    if (!_bus.readRegister16(1, status)) { _bus.close(); return false; }
    _scanMask = static_cast<uint8_t>(scanMask);
    _mux = static_cast<uint8_t>(mux); _fullScaleMv = fullScale; _rate = rate;
    _gain = gain; _offset = offset;
    // Drain an old conversion plus ours. Nominal timing is only a timeout
    // budget; a verified busy-to-ready transition is still required.
    _timeoutMs = 2 * ((config.nominalPeriodUs + 999) / 1000) + 50;
    _ready = true;
    return true;
}
bool ADS1115Driver::read(SensorReading& reading) {
    if (_scanMask) {
        SensorReadStep step;
        do {
            step = readStep(reading);
            if (step == SensorReadStep::Pending) delay(1);
        } while (step == SensorReadStep::Continue || step == SensorReadStep::Pending);
        return step == SensorReadStep::Complete;
    }
    reading = SensorReading();
    for (auto& value : reading.values) value = NAN;
    if (!_ready) return _bus.fail("ADS1115_NOT_INITIALIZED");
    const uint64_t generation = _session.begin(_mux, _fullScaleMv, _rate,
        static_cast<uint32_t>(millis()), _timeoutMs);
    if (!generation) return _bus.fail(sessionError(_session.error()));
    using Operation = ADS1115ConversionSession::Operation;
    while (_session.pending()) {
        if (!_session.tick(generation, static_cast<uint32_t>(millis()))) break;
        const auto operation = _session.operation();
        uint16_t value = 0;
        bool ok = true;
        switch (operation) {
        case Operation::ReadStatus:
            ok = _bus.readRegister16(1, value);
            if (ok) _session.observeStatus(generation, value, static_cast<uint32_t>(millis()));
            if (ok && _session.pending() && _session.operation() == Operation::ReadStatus) delay(1);
            break;
        case Operation::WriteIdle: case Operation::WriteStart:
            ok = _bus.writeRegister16(1, _session.writeValue());
            if (ok) _session.acknowledgeWrite(generation, true, static_cast<uint32_t>(millis()));
            // Immediately read status on next iteration; no delay after START.
            break;
        case Operation::ReadSample:
            ok = _bus.readRegister16(0, value);
            if (ok) _session.observeSample(generation, value, static_cast<uint32_t>(millis()));
            break;
        default: ok = _bus.fail("ADS1115_INVALID_OPERATION"); break;
        }
        if (!ok) {
            _session.transportFailed(generation, static_cast<uint32_t>(millis()));
            return false; // Preserve the precise transport diagnostic.
        }
    }
    if (_session.state() != ADS1115ConversionSession::State::Complete)
        return _bus.fail(sessionError(_session.error()));
    const auto& sample = _session.sample();
    if (!sample.valid || sample.atCodeRail) return _bus.fail("ADS1115_CODE_RAIL");
    const double volts = sample.volts * _gain + _offset;
    if (!std::isfinite(volts) || std::fabs(volts) > std::numeric_limits<float>::max())
        return _bus.fail("ADS1115_CALIBRATION_RANGE");
    reading.values[0] = static_cast<float>(volts);
    reading.values[1] = static_cast<float>(sample.signedCode);
    reading.channelCount = 2;
    reading.timestamp = _session.sampleTimeMs();
    reading.success = true;
    return true;
}
uint32_t ADS1115Driver::cooperativeReadBudgetMs() const {
    return _scanMask ? ads1115ReadBudgetMs(_rate, _scanMask) : 0;
}
SensorReadStep ADS1115Driver::scanFailed(SensorReading& reading,
    SensorChannelError error, const char* detail) {
    if (_scanIndex < _scanReading.channelCount)
        _scanReading.channelErrors[_scanIndex] = error;
    _scanReading.success = false;
    _scanActive = false;
    _session.cancel(_session.generation());
    reading = _scanReading;
    if (detail) _bus.fail(detail);
    return SensorReadStep::Failed;
}
SensorReadStep ADS1115Driver::readStep(SensorReading& reading) {
    if (!_scanMask) return ISensorDriver::readStep(reading);
    if (!_scanActive) {
        _scanReading = SensorReading();
        for (auto& value : _scanReading.values) value = NAN;
        for (auto& error : _scanReading.channelErrors) error = SensorChannelError::NotAttempted;
        _scanReading.channelMetadata = true;
        _scanReading.channelCount = getChannelCount();
        _scanIndex = 0;
        _scanMux = 0;
        while (!(_scanMask & (1U << _scanMux))) ++_scanMux;
        _scanStarted = static_cast<uint32_t>(millis());
        _scanActive = true;
        if (!_ready)
            return scanFailed(reading, SensorChannelError::Cancelled, "ADS1115_NOT_INITIALIZED");
        if (!_session.begin(_scanMux, _fullScaleMv, _rate, _scanStarted, _timeoutMs))
            return scanFailed(reading, SensorChannelError::Configuration, sessionError(_session.error()));
    }
    const uint32_t now = static_cast<uint32_t>(millis());
    if (now - _scanStarted >= cooperativeReadBudgetMs())
        return scanFailed(reading, SensorChannelError::Timeout, "ADS1115_SCAN_TIMEOUT");
    const uint64_t generation = _session.generation();
    if (!_session.tick(generation, now)) {
        const auto error = _session.error();
        return scanFailed(reading, error == ADS1115ConversionSession::Error::Timeout
            ? SensorChannelError::Timeout : error == ADS1115ConversionSession::Error::StartNotObserved
            ? SensorChannelError::StartNotObserved : SensorChannelError::Configuration, sessionError(error));
    }
    using Operation = ADS1115ConversionSession::Operation;
    const auto operation = _session.operation();
    uint16_t value = 0;
    bool ok = false;
    switch (operation) {
    case Operation::ReadStatus:
        ok = _bus.readRegister16(1, value);
        if (ok) _session.observeStatus(generation, value, static_cast<uint32_t>(millis()));
        break;
    case Operation::WriteIdle: case Operation::WriteStart:
        ok = _bus.writeRegister16(1, _session.writeValue());
        if (ok) _session.acknowledgeWrite(generation, true, static_cast<uint32_t>(millis()));
        break;
    case Operation::ReadSample:
        ok = _bus.readRegister16(0, value);
        if (ok) _session.observeSample(generation, value, static_cast<uint32_t>(millis()));
        break;
    default: return scanFailed(reading, SensorChannelError::Configuration, "ADS1115_INVALID_OPERATION");
    }
    if (!ok) {
        _session.transportFailed(generation, static_cast<uint32_t>(millis()));
        return scanFailed(reading, SensorChannelError::Transport, nullptr);
    }
    if (static_cast<uint32_t>(millis()) - _scanStarted >= cooperativeReadBudgetMs())
        return scanFailed(reading, SensorChannelError::Timeout, "ADS1115_SCAN_TIMEOUT");
    if (!_session.pending() && _session.state() != ADS1115ConversionSession::State::Complete) {
        const auto error = _session.error();
        return scanFailed(reading, error == ADS1115ConversionSession::Error::Timeout
            ? SensorChannelError::Timeout : error == ADS1115ConversionSession::Error::StartNotObserved
            ? SensorChannelError::StartNotObserved : SensorChannelError::Configuration, sessionError(error));
    }
    if (_session.pending())
        return operation == Operation::ReadStatus && _session.operation() == Operation::ReadStatus
            ? SensorReadStep::Pending : SensorReadStep::Continue;
    const auto& sample = _session.sample();
    if (!sample.valid || sample.atCodeRail)
        return scanFailed(reading, SensorChannelError::Range, "ADS1115_CODE_RAIL");
    const double volts = sample.volts * _gain + _offset;
    if (!std::isfinite(volts) || std::fabs(volts) > std::numeric_limits<float>::max())
        return scanFailed(reading, SensorChannelError::Calibration, "ADS1115_CALIBRATION_RANGE");
    _scanReading.values[_scanIndex] = static_cast<float>(volts);
    _scanReading.channelTimestamps[_scanIndex] = _session.sampleTimeMs();
    _scanReading.channelErrors[_scanIndex] = SensorChannelError::None;
    if (_scanIndex == 0) _scanReading.timestamp = _session.sampleTimeMs();
    if (++_scanIndex == getChannelCount()) {
        _scanReading.success = true;
        _scanActive = false;
        reading = _scanReading;
        return SensorReadStep::Complete;
    }
    do { ++_scanMux; } while (!(_scanMask & (1U << _scanMux)));
    if (!_session.begin(_scanMux, _fullScaleMv, _rate, static_cast<uint32_t>(millis()), _timeoutMs))
        return scanFailed(reading, SensorChannelError::Configuration, sessionError(_session.error()));
    return SensorReadStep::Continue;
}
void ADS1115Driver::deinit() {
    _ready = false;
    _scanActive = false;
    _session.cancel(_session.generation());
    _bus.close();
}
unsigned long ADS1115Driver::getMinInterval() const { return std::max<uint32_t>(100, _scanMask ? cooperativeReadBudgetMs() : _timeoutMs); }
}
using FastBee::ADS1115Driver;
FASTBEE_REGISTER_SENSOR("ADS1115", ADS1115Driver);
#endif
