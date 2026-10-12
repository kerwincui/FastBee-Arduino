#pragma once
#include "RuntimeResourceJson.h"
#include "core/NeoPixelResourceSnapshot.h"
#include "core/StepperResourceSnapshot.h"

namespace FastBee {

inline void writeNeoPixelResourceJson(JsonObject data, const NeoPixelResourceSnapshot& snapshot) {
    if (!writeResourceObservation(data, snapshot.status)) {
        setFixedJsonField(data, "managerInitialized", snapshot.managerInitialized);
        if (!snapshot.managerInitialized) setFixedJsonField(data, "reason", "PM_SYNCHRONIZATION_UNAVAILABLE");
        return;
    }
    setFixedJsonField(data, "scope", "project_neopixel_owner");
    setFixedJsonField(data, "complete", false);
    setFixedJsonField(data, "capacity", NeoPixelResourceSnapshot::CAPACITY);
    if (!snapshot.supported) {
        setFixedJsonField(data, "status", "unsupported"); setFixedJsonField(data, "reason", "NEOPIXEL_NOT_COMPILED");
        setFixedJsonField(data, "leaseCount", nullptr); setFixedJsonField(data, "leases", nullptr);
        return;
    }
    setFixedJsonField(data, "managerInitialized", snapshot.managerInitialized);
    setFixedJsonField(data, "objectAllocated", snapshot.objectAllocated);
    const bool retained = snapshot.objectAllocated || snapshot.projectPinRetained || snapshot.backendPinRetained;
    setFixedJsonField(data, "leaseCount", retained ? 1 : 0);
    setFixedJsonField(data, "rmtChannelCount", nullptr);
    setFixedJsonField(data, "rmtCapacityKnown", false);
    JsonArray leases = data["leases"].to<JsonArray>();
    if (!retained) return;
    JsonObject lease = leases.add<JsonObject>();
    writeResourceConsumerJson(lease, snapshot.consumer);
    if (snapshot.pin == 255) setFixedJsonField(lease, "pin", nullptr);
    else setFixedJsonField(lease, "pin", snapshot.pin);
    setFixedJsonField(lease, "configuredLedCount", snapshot.ledCount);
    setFixedJsonField(lease, "state", FeatureInstanceSnapshot::lifecycle(snapshot.lifecycle).name());
    setFixedJsonField(lease, "projectPinRetained", snapshot.projectPinRetained);
    setFixedJsonField(lease, "backendPinRetained", snapshot.backendPinRetained);
    setFixedJsonField(lease, "writeAttempted", snapshot.writeAttempted);
    setFixedJsonField(lease, "releaseAttempted", snapshot.releaseAttempted);
    if (snapshot.writeAttempted) setFixedJsonField(lease, "lastWriteResult", static_cast<int>(snapshot.lastWriteResult));
    else setFixedJsonField(lease, "lastWriteResult", nullptr);
    if (snapshot.releaseAttempted) setFixedJsonField(lease, "lastReleaseResult", static_cast<int>(snapshot.lastReleaseResult));
    else setFixedJsonField(lease, "lastReleaseResult", nullptr);
    setFixedJsonField(lease, "cleanupPending", snapshot.lifecycle == FeatureInstanceState::CleanupPending ||
        snapshot.lifecycle == FeatureInstanceState::Retiring);
}

inline void writeStepperResourceJson(JsonObject data, const StepperResourceSnapshot& snapshot) {
    if (!writeResourceObservation(data, snapshot.status)) return;
    setFixedJsonField(data, "scope", "captured_stepper_gpio_owners");
    setFixedJsonField(data, "complete", false);
    setFixedJsonField(data, "capacity", StepperResourceSnapshot::CAPACITY);
    const uint8_t count = snapshot.count > StepperResourceSnapshot::CAPACITY
        ? StepperResourceSnapshot::CAPACITY : snapshot.count;
    setFixedJsonField(data, "leaseCount", count);
    setFixedJsonField(data, "truncated", snapshot.truncated || count != snapshot.count);
    setFixedJsonField(data, "gpioIsSublease", true);
    JsonArray leases = data["leases"].to<JsonArray>();
    for (uint8_t i = 0; i < count; ++i) {
        const auto& source = snapshot.leases[i];
        JsonObject lease = leases.add<JsonObject>();
        writeResourceConsumerJson(lease, source.consumer);
        JsonArray pins = lease["pins"].to<JsonArray>();
        for (uint8_t pin : source.pins) pins.add(pin);
        setFixedJsonField(lease, "retainedMask", source.retainedMask);
        setFixedJsonField(lease, "gpioOwnedMask", source.gpioOwnedMask);
        setFixedJsonField(lease, "gpioReadyMask", source.gpioReadyMask);
        setFixedJsonField(lease, "gpioFaultMask", source.gpioFaultMask);
        setFixedJsonField(lease, "gpioCleanupPendingMask", source.gpioCleanupPendingMask);
        setFixedJsonField(lease, "running", source.running);
        setFixedJsonField(lease, "ready", source.ready);
        setFixedJsonField(lease, "fault", source.fault);
        setFixedJsonField(lease, "cleanupPending", source.cleanupPending);
        setFixedJsonField(lease, "state", source.cleanupPending ? "retiring" : source.fault ? "retained_after_fault"
            : source.running ? "running" : source.ready ? "ready" : "reserved");
    }
}

} // namespace FastBee
