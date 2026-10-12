#pragma once
#include <stdint.h>
#include "AdcResourceSnapshot.h"

namespace FastBee {
enum class AdcStatus : uint8_t {
    Ok, Busy, NoMemory, Invalid, Full, NotOwned, CreateFailed,
    ConfigureFailed, ReadFailed, ReleaseFailed, CalibrationFailed
};
// PM and legacy SensorDriver locks are acquired before this adapter's lock.
// No adapter path calls back into either manager. All sampling is task-context.
AdcStatus acquireAdc(const ResourceConsumerIdentity& owner, uint8_t pin,
                     uint8_t attenuation, uint8_t resolution);
AdcStatus releaseAdc(const ResourceConsumerIdentity& owner);
AdcStatus readAdcBatch(const ResourceConsumerIdentity& owner, uint16_t* samples,
                       uint8_t count);
// A transient legacy sensor lease has role Sensor and its real pin, no invented
// peripheral ID. Cache lifetime is separate from hardware-handle lifetime.
AdcStatus readTransientAdcBatch(uint8_t pin, uint16_t* samples, uint8_t count);
bool checkedAdcVoltage(uint8_t pin, uint8_t attenuation, uint8_t resolution,
                       uint16_t raw, float& volts);
void snapshotAdcResources(AdcResourceSnapshot& snapshot);
// PM holds its outer lock across this zero-wait check and GPIO/DAC/LEDC work.
// Busy and retained configured channels are unavailable, including failed adds.
bool adcPinUnavailable(uint8_t pin);
const char* adcStatusName(AdcStatus status);
}
