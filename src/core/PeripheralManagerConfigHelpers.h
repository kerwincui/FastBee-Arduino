#pragma once

#include "core/FeatureFlags.h"
#include "core/PeripheralConfig.h"

namespace FastBee {
namespace PeripheralManagerConfigDetail {

constexpr uint16_t STEPPER_DEFAULT_STEPS_PER_REV = 2048;
constexpr uint16_t STEPPER_DEFAULT_RPM = 8;
constexpr uint16_t STEPPER_MIN_RPM = 1;
constexpr uint16_t STEPPER_MAX_RPM = 30;
constexpr uint16_t NEOPIXEL_DEFAULT_COUNT = 1;
constexpr uint16_t NEOPIXEL_MAX_COUNT = FASTBEE_NEOPIXEL_MAX_LEDS;
constexpr uint8_t NEOPIXEL_DEFAULT_BRIGHTNESS = 64;
constexpr uint16_t RF_DEFAULT_PULSE_WIDTH_US = 350;
constexpr uint16_t RF_MIN_PULSE_WIDTH_US = 100;
constexpr uint16_t RF_MAX_PULSE_WIDTH_US = 2000;
constexpr uint8_t RF_DEFAULT_REPEAT = 8;
constexpr uint8_t RF_MIN_REPEAT = 1;
constexpr uint8_t RF_MAX_REPEAT = 20;
constexpr uint8_t RF_DEFAULT_BIT_LENGTH = 24;
constexpr uint8_t RF_MIN_BIT_LENGTH = 1;
constexpr uint8_t RF_MAX_BIT_LENGTH = 32;
constexpr uint8_t RADAR_MODE_DIGITAL = 0;
constexpr uint16_t RADAR_DEFAULT_DEBOUNCE_MS = 50;
constexpr uint16_t RADAR_DEFAULT_HOLD_MS = 2000;

inline bool isAdcInputType(PeripheralType type) {
    return type == PeripheralType::ADC || type == PeripheralType::GPIO_ANALOG_INPUT;
}

inline uint8_t normalizeAdcSampleCount(int value, uint8_t fallback = 1) {
    if (value <= 0) value = fallback > 0 ? fallback : 1;
    if (value > 16) value = 16;
    return static_cast<uint8_t>(value);
}

inline uint8_t normalizeAdcFilterMode(int value) {
    return value == 1 ? 1 : 0;
}

inline void initializeAdcDefaults(PeripheralConfig& config) {
    config.params.adc.attenuation = 3;
    config.params.adc.resolution = 12;
    config.params.adc.sampleRate = 0;
    config.params.adc.sampleCount = 1;
    config.params.adc.filterMode = 0;
    config.params.adc.signalMode = static_cast<uint8_t>(AnalogSignalMode::RAW);
    config.params.adc.conversionEnabled = false;
    config.params.adc.clampEnabled = false;
    config.params.adc.calibratedVoltage = false;
    config.params.adc.decimalPlaces = 2;
    config.params.adc.inputMin = 0.0f;
    config.params.adc.inputMax = 0.0f;
    config.params.adc.engineeringMin = 0.0f;
    config.params.adc.engineeringMax = 100.0f;
    config.params.adc.engineeringOffset = 0.0f;
    config.params.adc.vRef = 0.0f;
    config.params.adc.voltageRatio = 0.0f;
    config.params.adc.shuntResistance = 0.0f;
    config.params.adc.inputGain = 0.0f;
    config.params.adc.inputOffset = 0.0f;
    config.params.adc.rawMin = 0;
    config.params.adc.rawMax = 0;
}

inline uint16_t clampStepperSpeed(uint16_t rpm) {
    if (rpm < STEPPER_MIN_RPM) return STEPPER_MIN_RPM;
    if (rpm > STEPPER_MAX_RPM) return STEPPER_MAX_RPM;
    return rpm;
}

inline uint16_t clampRfPulseWidth(uint16_t pulseWidth) {
    if (pulseWidth < RF_MIN_PULSE_WIDTH_US) return RF_DEFAULT_PULSE_WIDTH_US;
    if (pulseWidth > RF_MAX_PULSE_WIDTH_US) return RF_MAX_PULSE_WIDTH_US;
    return pulseWidth;
}

inline uint8_t clampRfRepeat(uint8_t repeat) {
    if (repeat < RF_MIN_REPEAT) return RF_DEFAULT_REPEAT;
    if (repeat > RF_MAX_REPEAT) return RF_MAX_REPEAT;
    return repeat;
}

inline uint8_t clampRfBitLength(uint8_t bitLength) {
    if (bitLength < RF_MIN_BIT_LENGTH) return RF_DEFAULT_BIT_LENGTH;
    if (bitLength > RF_MAX_BIT_LENGTH) return RF_MAX_BIT_LENGTH;
    return bitLength;
}

inline uint16_t clampNeoPixelCount(uint16_t count) {
    if (count == 0) return NEOPIXEL_DEFAULT_COUNT;
    if (count > NEOPIXEL_MAX_COUNT) return NEOPIXEL_MAX_COUNT;
    return count;
}

inline uint8_t clampNeoPixelBrightness(uint16_t brightness) {
    if (brightness > 255) return 255;
    return static_cast<uint8_t>(brightness);
}

}  // namespace PeripheralManagerConfigDetail
}  // namespace FastBee
