#pragma once
#include "RuntimeResourceSnapshot.h"

namespace FastBee {
// Captured ULN2003 phase-output ownership, not a position/angle measurement.
struct StepperResourceSnapshot {
    static constexpr size_t CAPACITY = 16;
    ResourceObservation status = ResourceObservation::Observed;
    uint8_t count = 0;
    bool truncated = false;
    struct Lease {
        ResourceConsumerIdentity consumer{};
        uint8_t pins[4]{255, 255, 255, 255};
        uint8_t retainedMask = 0, gpioOwnedMask = 0, gpioReadyMask = 0;
        uint8_t gpioFaultMask = 0, gpioCleanupPendingMask = 0;
        bool running = false, ready = false, fault = false, cleanupPending = false;
    } leases[CAPACITY]{};
};
}
