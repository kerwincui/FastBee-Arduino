#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ResourceConsumerIdentity.h"

namespace FastBee {
// Project-owned IDF oneshot handles only. No hardware query or free-pin claim.
// observed=false means busy; reset storage must not be presented as empty.
struct AdcResourceSnapshot {
    static constexpr size_t UNIT_CAPACITY = 2;
    static constexpr size_t CHANNEL_CAPACITY = 20;
    static constexpr size_t CONSUMER_CAPACITY = 32;
    bool observed = false, initialized = false;
    uint8_t unitCount = 0, channelCount = 0, consumerCount = 0;
    uint8_t calibrationCount = 0, calibrationCleanupPending = 0;
    struct Unit {
        uint8_t unit = 0;
        bool retained = false, cleanupFailed = false;
        int32_t lastError = 0;
    } units[UNIT_CAPACITY]{};
    struct Channel {
        uint8_t unit = 0, channel = 0, attenuation = 0, pin = 255;
        bool configurationKnown = false;
    } channels[CHANNEL_CAPACITY]{};
    struct Consumer {
        ResourceConsumerIdentity identity{};
        uint8_t pin = 255, unit = 0, channel = 0, attenuation = 0, resolution = 12;
        bool transient = false, retiring = false;
        int32_t lastError = 0;
    } consumers[CONSUMER_CAPACITY]{};
};
}
