#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/CheckedAdc.h"
#include "core/AnalogInputCalibration.h"
namespace {
bool isAnalogInput(PeripheralType type) {
    return type == PeripheralType::GPIO_ANALOG_INPUT || type == PeripheralType::ADC;
}
bool analogOwner(const PeripheralConfig& config, FastBee::ResourceConsumerIdentity& owner) {
    return owner.assign(FastBee::ResourceConsumerRole::Peripheral, config.id.c_str());
}
float convertAnalogRaw(const PeripheralConfig& config, uint16_t raw,
                       AnalogInputQuality* quality = nullptr, float* pinVoltage = nullptr) {
    if (pinVoltage) *pinVoltage = NAN;
    const auto& adc = config.params.adc;
    if (adc.calibratedVoltage && adc.conversionEnabled && adc.signalMode >= 1 && adc.signalMode <= 3) {
        float voltage = NAN;
        if (!FastBee::calibratedAdcVoltage(config.getPrimaryPin(), adc.attenuation, adc.resolution, raw, voltage)) {
            if (quality) *quality = AnalogInputQuality::READ_FAILED;
            return NAN;
        }
        if (pinVoltage) *pinVoltage = voltage;
        return convertAnalogInputRaw(config, raw, quality, voltage);
    }
    if (pinVoltage && adc.signalMode >= 1 && adc.signalMode <= 3) {
        const uint8_t bits = adc.resolution >= 9 && adc.resolution <= 12 ? adc.resolution : 12;
        *pinVoltage = raw * positiveAnalogConfigValue(adc.vRef, 3.3f) / ((1UL << bits) - 1);
    }
    return convertAnalogInputRaw(config, raw, quality);
}
uint16_t filterAnalogSamples(uint16_t* samples, uint8_t count, uint8_t mode) {
    if (mode == 1) {
        for (uint8_t i = 1; i < count; ++i) {
            const uint16_t key = samples[i];
            int j = i - 1;
            while (j >= 0 && samples[j] > key) { samples[j + 1] = samples[j]; --j; }
            samples[j + 1] = key;
        }
        return samples[count / 2];
    }
    uint32_t sum = 0;
    for (uint8_t i = 0; i < count; ++i) sum += samples[i];
    return static_cast<uint16_t>(sum / count);
}
}

bool PeripheralManager::setupAnalogInput(const PeripheralConfig& config) {
    FastBee::ResourceConsumerIdentity owner;
    const auto pin = config.getPrimaryPin();
    if (pin == 255 || isReservedPin(pin) || !managedPinAvailableForAdc(pin) || !analogOwner(config, owner)) return false;
    auto status = FastBee::acquireAdc(owner, pin, config.params.adc.attenuation, config.params.adc.resolution);
    if (status == FastBee::AdcStatus::Ok) {
        uint16_t initial = 0;
        status = FastBee::readAdcBatch(owner, &initial, 1);
        if (status != FastBee::AdcStatus::Ok) {
            const auto cleanup = FastBee::releaseAdc(owner);
            if (cleanup != FastBee::AdcStatus::Ok) status = cleanup;
        }
    }
    auto state = runtimeStates.find(config.id);
    if (state != runtimeStates.end()) state->second.lastError = status == FastBee::AdcStatus::Ok ? "" : FastBee::adcStatusName(status);
    return status == FastBee::AdcStatus::Ok;
}
bool PeripheralManager::teardownAnalogInput(const PeripheralConfig& config) {
    FastBee::ResourceConsumerIdentity owner;
    if (!analogOwner(config, owner)) return false;
    invalidateSensorConfigurationLocked();
    const auto status = FastBee::releaseAdc(owner);
    auto state = runtimeStates.find(config.id);
    if (state != runtimeStates.end()) state->second.lastError = status == FastBee::AdcStatus::Ok ? "" : FastBee::adcStatusName(status);
    return status == FastBee::AdcStatus::Ok;
}
bool PeripheralManager::readAnalogSample(const String& id, AnalogReadSample& sample) {
    return readAnalogSampleByCStr(id.c_str(), sample);
}
bool PeripheralManager::readAnalogSampleByCStr(const char* id, AnalogReadSample& sample) {
    sample = {};
    RecursiveMutexGuard lock(_mutex, pdMS_TO_TICKS(50));
    if (!lock.isLocked() || !id || !*id) return false;
    const auto* config = getPeripheralCStrLocked(id);
    if (!config || !config->enabled || !isAnalogInput(config->type)) return false;
    sample.pin = config->getPrimaryPin();
    sample.conversionEnabled = config->params.adc.conversionEnabled;
    sample.calibratedVoltage = config->params.adc.calibratedVoltage;
    sample.signalMode = config->params.adc.signalMode;
    sample.decimalPlaces = config->params.adc.decimalPlaces > 6 ? 6 : config->params.adc.decimalPlaces;
    FastBee::ResourceConsumerIdentity owner;
    if (sample.pin == 255 || isReservedPin(sample.pin) || !analogOwner(*config, owner)) return false;
    uint8_t count = config->params.adc.sampleCount;
    if (!count) count = 1;
    if (count > 16) count = 16;
    uint16_t samples[16]{};
    const auto status = FastBee::readAdcBatch(owner, samples, count);
    auto state = runtimeStates.find(config->id);
    if (state != runtimeStates.end()) state->second.lastError = status == FastBee::AdcStatus::Ok ? "" : FastBee::adcStatusName(status);
    if (status != FastBee::AdcStatus::Ok) return false;
    sample.raw = filterAnalogSamples(samples, count, config->params.adc.filterMode);
    sample.sampled = true;
    sample.value = convertAnalogRaw(*config, sample.raw, &sample.quality, &sample.pinVoltage);
    if (sample.quality == AnalogInputQuality::READ_FAILED && state != runtimeStates.end())
        state->second.lastError = FastBee::adcStatusName(FastBee::AdcStatus::CalibrationFailed);
    sample.calculatedCurrentMa = calculateAnalogInputCurrentMa(*config, sample.pinVoltage);
    return true; // A valid raw sample may still have out-of-range engineering quality.
}
uint16_t PeripheralManager::readAnalog(uint8_t pin) {
    const String id = getPinPeripheralId(pin);
    return id.isEmpty() ? 0 : readAnalog(id);
}
uint16_t PeripheralManager::readAnalog(const String& id) { return readAnalogByCStr(id.c_str()); }
uint16_t PeripheralManager::readAnalogByCStr(const char* id) {
    AnalogReadSample sample;
    return readAnalogSampleByCStr(id, sample) ? sample.raw : 0;
}
float PeripheralManager::readAnalogValue(const String& id) { return readAnalogValueByCStr(id.c_str()); }
float PeripheralManager::readAnalogValueByCStr(const char* id) {
    AnalogReadSample sample;
    return readAnalogSampleByCStr(id, sample) ? sample.value : NAN;
}
float PeripheralManager::convertAnalogRawValue(const String& id, uint16_t raw,
                                               AnalogInputQuality* quality, float* pinVoltage) {
    RecursiveMutexGuard lock(_mutex, pdMS_TO_TICKS(50));
    if (pinVoltage) *pinVoltage = NAN;
    if (!lock.isLocked()) { if (quality) *quality = AnalogInputQuality::READ_FAILED; return NAN; }
    const auto* config = getPeripheralCStrLocked(id.c_str());
    if (!config || !isAnalogInput(config->type)) {
        if (quality) *quality = AnalogInputQuality::INVALID_CONFIG;
        return NAN;
    }
    return convertAnalogRaw(*config, raw, quality, pinVoltage);
}
