#pragma once

#include "RuntimeResourceCapture.h"
#include "RuntimeResourceJson.h"
#include "RuntimeMotionResourceJson.h"
#include "core/CheckedAdc.h"
#include "core/ManagedPinIo.h"

namespace FastBee {

inline void writeAdcResourceJson(JsonObject data, const AdcResourceSnapshot& snapshot) {
    if (!writeResourceObservation(data, snapshot.observed ? ResourceObservation::Observed
                                                        : ResourceObservation::Busy)) return;
    setFixedJsonField(data, "scope", "checked_oneshot_pm_and_legacy_sensors");
    setFixedJsonField(data, "complete", false);
    setFixedJsonField(data, "managerInitialized", snapshot.initialized);
    setFixedJsonField(data, "capacity", AdcResourceSnapshot::CONSUMER_CAPACITY);
    setFixedJsonField(data, "leaseCount", snapshot.consumerCount);
    setFixedJsonField(data, "unitCount", snapshot.unitCount);
    setFixedJsonField(data, "channelCount", snapshot.channelCount);
    setFixedJsonField(data, "calibrationCount", snapshot.calibrationCount);
    setFixedJsonField(data, "calibrationCleanupPending", snapshot.calibrationCleanupPending);
    setFixedJsonField(data, "hardwareResolutionBits", 12);
    setFixedJsonField(data, "individualChannelReleaseSupported", false);
    JsonArray units = data["units"].to<JsonArray>();
    for (uint8_t i = 0; i < snapshot.unitCount && i < AdcResourceSnapshot::UNIT_CAPACITY; ++i) {
        const auto& source = snapshot.units[i];
        JsonObject unit = units.add<JsonObject>();
        setFixedJsonField(unit, "unit", source.unit);
        setFixedJsonField(unit, "retained", source.retained);
        setFixedJsonField(unit, "cleanupFailed", source.cleanupFailed);
        setFixedJsonField(unit, "lastError", source.lastError);
    }
    JsonArray channels = data["channels"].to<JsonArray>();
    for (uint8_t i = 0; i < snapshot.channelCount && i < AdcResourceSnapshot::CHANNEL_CAPACITY; ++i) {
        const auto& source = snapshot.channels[i];
        JsonObject channel = channels.add<JsonObject>();
        setFixedJsonField(channel, "unit", source.unit); setFixedJsonField(channel, "channel", source.channel);
        setFixedJsonField(channel, "pin", source.pin);
        setFixedJsonField(channel, "attenuation", source.attenuation);
        setFixedJsonField(channel, "configurationKnown", source.configurationKnown);
    }
    JsonArray leases = data["leases"].to<JsonArray>();
    for (uint8_t i = 0; i < snapshot.consumerCount && i < AdcResourceSnapshot::CONSUMER_CAPACITY; ++i) {
        const auto& source = snapshot.consumers[i];
        JsonObject lease = leases.add<JsonObject>();
        writeResourceConsumerJson(lease, source.identity);
        setFixedJsonField(lease, "pin", source.pin); setFixedJsonField(lease, "unit", source.unit); setFixedJsonField(lease, "channel", source.channel);
        setFixedJsonField(lease, "attenuation", source.attenuation); setFixedJsonField(lease, "resolutionBits", source.resolution);
        setFixedJsonField(lease, "transient", source.transient);
        setFixedJsonField(lease, "cleanupPending", source.retiring);
        setFixedJsonField(lease, "lastError", source.lastError);
        setFixedJsonField(lease, "fault", source.lastError != 0);
        setFixedJsonField(lease, "state", source.retiring ? "retiring" : source.lastError ? "retained_after_fault" : "reserved");
    }
}

inline const char* managedGpioModeName(ManagedGpioMode mode) {
    switch (mode) {
    case ManagedGpioMode::Input: return "input";
    case ManagedGpioMode::PullUp: return "input_pullup";
    case ManagedGpioMode::PullDown: return "input_pulldown";
    case ManagedGpioMode::Output: return "output";
    default: return "unknown";
    }
}

template <typename Lease>
void writeManagedPinLeaseJson(JsonObject lease, const Lease& source) {
    writeResourceConsumerJson(lease, source.consumer);
    if (source.commandScript) setFixedJsonField(lease, "ownerRole", "command_script");
    setFixedJsonField(lease, "pin", source.pin);
    setFixedJsonField(lease, "fault", source.fault);
    setFixedJsonField(lease, "cleanupPending", source.cleanupPending);
    setFixedJsonField(lease, "state", source.cleanupPending ? "retiring" : source.fault ? "retained_after_fault" : "reserved");
}

inline void writeGpioResourceJson(JsonObject data, const GpioResourceSnapshot& snapshot) {
    if (!writeResourceObservation(data, snapshot.status)) return;
    setFixedJsonField(data, "scope", "checked_pm_and_script_gpio");
    setFixedJsonField(data, "complete", false);
    setFixedJsonField(data, "capacity", GpioResourceSnapshot::CAPACITY);
    setFixedJsonField(data, "leaseCount", snapshot.count);
    setFixedJsonField(data, "controlOutputIsSublease", true);
    JsonArray leases = data["leases"].to<JsonArray>();
    for (uint8_t i = 0; i < snapshot.count && i < GpioResourceSnapshot::CAPACITY; ++i) {
        const auto& source = snapshot.leases[i];
        JsonObject lease = leases.add<JsonObject>();
        writeManagedPinLeaseJson(lease, source);
        setFixedJsonField(lease, "mode", managedGpioModeName(source.mode));
        setFixedJsonField(lease, "interruptAttached", source.interruptAttached);
        setFixedJsonField(lease, "interruptCleanupPending", source.interruptCleanupPending);
        if (source.interruptAttached) setFixedJsonField(lease, "interruptType", source.interruptType);
        else setFixedJsonField(lease, "interruptType", nullptr);
    }
}

inline void writeDacResourceJson(JsonObject data, const DacResourceSnapshot& snapshot) {
    if (!writeResourceObservation(data, snapshot.status)) return;
    setFixedJsonField(data, "scope", "checked_pm_and_script_dac");
    setFixedJsonField(data, "complete", false);
    if (!snapshot.supported) {
        setFixedJsonField(data, "status", "unsupported");
        setFixedJsonField(data, "reason", "NATIVE_DAC_NOT_SUPPORTED");
        setFixedJsonField(data, "leaseCount", nullptr); setFixedJsonField(data, "leases", nullptr);
        return;
    }
    setFixedJsonField(data, "capacity", DacResourceSnapshot::CAPACITY);
    setFixedJsonField(data, "leaseCount", snapshot.count);
    JsonArray leases = data["leases"].to<JsonArray>();
    for (uint8_t i = 0; i < snapshot.count && i < DacResourceSnapshot::CAPACITY; ++i)
        writeManagedPinLeaseJson(leases.add<JsonObject>(), snapshot.leases[i]);
}

template <typename Manager>
void writeManagedPinResources(JsonObject runtime, Manager& manager) {
    writeAllocatedResourceSnapshot<AdcResourceSnapshot>(runtime["adc"].to<JsonObject>(),
        snapshotAdcResources, writeAdcResourceJson);
    writeAllocatedResourceSnapshot<GpioResourceSnapshot>(runtime["gpio"].to<JsonObject>(),
        [&manager](GpioResourceSnapshot& snapshot) { manager.snapshotGpioResources(snapshot); }, writeGpioResourceJson);
    writeAllocatedResourceSnapshot<DacResourceSnapshot>(runtime["dac"].to<JsonObject>(),
        [&manager](DacResourceSnapshot& snapshot) { manager.snapshotDacResources(snapshot); }, writeDacResourceJson);
    writeAllocatedResourceSnapshot<NeoPixelResourceSnapshot>(runtime["neopixel"].to<JsonObject>(),
        [&manager](NeoPixelResourceSnapshot& snapshot) { manager.snapshotNeoPixelResources(snapshot); }, writeNeoPixelResourceJson);
    writeAllocatedResourceSnapshot<StepperResourceSnapshot>(runtime["stepper"].to<JsonObject>(),
        [&manager](StepperResourceSnapshot& snapshot) { manager.snapshotStepperResources(snapshot); }, writeStepperResourceJson);
}

} // namespace FastBee
