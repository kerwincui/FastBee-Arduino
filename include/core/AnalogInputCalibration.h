#pragma once
#include <stdint.h>

namespace FastBee {
// Convert the already acquired ADC sample using the chip's calibration data.
// Serializes its bounded calibration cache with the checked ADC lifecycle.
// Call under PeripheralManager's mutex. Does not acquire another ADC sample.
bool calibratedAdcVoltage(uint8_t pin, uint8_t attenuation, uint8_t resolution,
                          uint16_t raw, float& volts);
}
