#pragma once
#include "RuntimeResourceSnapshot.h"
#include "FeatureInstanceRegistry.h"
#include "peripherals/NeoPixelTransport.h"

namespace FastBee {
// One project-owned Adafruit object. LED count is the configured pixel buffer
// length, never an observed physical LED count or an allocated RMT channel count.
struct NeoPixelResourceSnapshot {
    static constexpr size_t CAPACITY = 1;
    ResourceObservation status = ResourceObservation::Observed;
    bool supported = false, managerInitialized = false, objectAllocated = false;
    ResourceConsumerIdentity consumer{};
    uint8_t pin = 255;
    uint16_t ledCount = 0;
    FeatureInstanceState lifecycle = FeatureInstanceState::Stopped;
    bool projectPinRetained = false, backendPinRetained = false;
    bool writeAttempted = false, releaseAttempted = false;
    FastBeeNeoPixelResult lastWriteResult = FASTBEE_NEO_OK;
    FastBeeNeoPixelResult lastReleaseResult = FASTBEE_NEO_OK;
};
} // namespace FastBee
