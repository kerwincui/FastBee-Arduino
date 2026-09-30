#pragma once
#include <stdint.h>

namespace FastBee {
// Convert the already acquired ADC sample using the chip's calibration data.
// Call under PeripheralManager's mutex. Does not acquire another ADC sample.
bool calibratedAdcVoltage(uint8_t pin, uint8_t attenuation, uint8_t resolution,
                          uint16_t raw, float& volts);
}
