#include "core/AnalogInputCalibration.h"
#include "core/CheckedAdc.h"

namespace FastBee {
bool calibratedAdcVoltage(uint8_t pin, uint8_t attenuation, uint8_t resolution,
                          uint16_t raw, float& volts) {
    return checkedAdcVoltage(pin, attenuation, resolution, raw, volts);
}
}
