#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ResourceConsumerIdentity.h"

namespace FastBee {

// Bounded registry observations, not hardware probing or proof of free pins.
// Busy observations have no valid counts/records; callers must not interpret
// their reset storage as an empty registry. Slot numbers are not owner IDs.
enum class ResourceObservation : uint8_t { Observed, Busy };

struct I2CResourceSnapshot {
    static constexpr size_t CAPACITY = 16;
    ResourceObservation status = ResourceObservation::Observed;
    bool initialized = false;
    int8_t sda = -1, scl = -1;
    uint32_t frequency = 0;
    uint8_t count = 0;
    struct Lease {
        uint8_t slot = 0, address = 0;
        bool retiring = false;
        ResourceConsumerIdentity consumer{};
    } leases[CAPACITY]{};
};

struct SpiResourceSnapshot {
    static constexpr size_t CAPACITY = 16;
    ResourceObservation status = ResourceObservation::Observed;
    bool initialized = false;
    int8_t sck = -1, miso = -1, mosi = -1;
    uint8_t count = 0;
    struct Lease {
        uint8_t slot = 0;
        int8_t cs = -1, requestedMiso = -1;
        bool networkOwner = false;
        char name[41]{};
        ResourceConsumerIdentity consumer{};
    } leases[CAPACITY]{};
    bool networkReserved = false, networkDedicatedHost = false;
    int8_t networkPins[6]{-1, -1, -1, -1, -1, -1};
};

struct UartResourceSnapshot {
    static constexpr size_t CAPACITY = 3;
    uint8_t portCount = 0;
    uint32_t generations[CAPACITY]{};
    ResourceConsumerIdentity consumers[CAPACITY]{};
};

struct ControlOutputResourceSnapshot {
    static constexpr size_t CAPACITY = 4;
    ResourceObservation status = ResourceObservation::Observed;
    uint8_t count = 0;
    struct Lease {
        uint8_t slot = 0;
        int pin = -1;
        char id[41]{};
        bool fault = false, retiring = false, retirementAttempted = false;
        ResourceConsumerIdentity consumer{};
    } leases[CAPACITY]{};
};

struct EncoderResourceSnapshot {
    static constexpr size_t CAPACITY = 4;
    ResourceObservation status = ResourceObservation::Observed;
    uint8_t count = 0;
    struct Lease {
        uint8_t slot = 0, pinA = 255, pinB = 255;
        char id[41]{};
        bool active = false, interruptAttached = false, interruptCleanupPending = false;
        bool fault = false, cleanupPending = false;
    } leases[CAPACITY]{};
};

struct DhtRmtResourceSnapshot {
    ResourceObservation status = ResourceObservation::Observed;
    bool supported = false;
    int retainedPin = -1;
};

// PM PWM/servo/passive-buzzer and command-script attachment lifecycles. Other SDK
// users and implicit pinMode detachments are outside this software ledger.
struct LedcResourceSnapshot {
    static constexpr size_t CAPACITY = 16;
    ResourceObservation status = ResourceObservation::Observed;
    uint8_t count = 0;
    struct Lease {
        uint8_t slot = 0, pin = 255, channel = 255, requestedResolution = 0;
        uint32_t requestedFrequency = 0;
        char id[41]{};
        bool cleanupFailed = false;
        bool commandScript = false;
    } leases[CAPACITY]{};
};

struct BacklightResourceSnapshot {
    ResourceObservation status = ResourceObservation::Observed;
    bool available = false, retained = false, cleanupFailed = false;
    int8_t pin = -1, channel = -1;
    uint32_t sampledAt = 0;
};

} // namespace FastBee
