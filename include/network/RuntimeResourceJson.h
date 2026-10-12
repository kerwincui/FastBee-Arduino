#pragma once
#include <ArduinoJson.h>
#include "network/FixedJsonField.h"
#include <initializer_list>
#include <cstring>
#include "core/RuntimeResourceSnapshot.h"

namespace FastBee {

inline const char* resourceConsumerRoleName(ResourceConsumerRole role) {
    switch (role) {
    case ResourceConsumerRole::Peripheral: return "peripheral";
    case ResourceConsumerRole::Sensor: return "sensor";
    case ResourceConsumerRole::Display: return "display";
    case ResourceConsumerRole::Touch: return "touch";
    case ResourceConsumerRole::SdCard: return "sd_card";
    case ResourceConsumerRole::Ethernet: return "ethernet";
    case ResourceConsumerRole::Modbus: return "modbus";
    case ResourceConsumerRole::Cellular: return "cellular";
    case ResourceConsumerRole::McpReset: return "mcp23017_reset";
    case ResourceConsumerRole::PcaGate: return "pca9685_oe";
    case ResourceConsumerRole::DhtCapture: return "dht_capture";
    default: return "unknown";
    }
}

inline void writeResourceConsumerJson(JsonObject lease, const ResourceConsumerIdentity& consumer) {
    setFixedJsonField(lease, "ownerKnown", consumer.id[0] != '\0');
    if (consumer.id[0]) setFixedJsonField(lease, "ownerId", JsonString(consumer.id, false));
    setFixedJsonField(lease, "ownerRole", resourceConsumerRoleName(consumer.role));
}

inline bool writeResourceObservation(JsonObject data, ResourceObservation status) {
    const bool observed = status == ResourceObservation::Observed;
    data.clear();
    setFixedJsonField(data, "status", observed ? "observed" : "busy");
    setFixedJsonField(data, "hardwareVerified", false);
    if (!observed) {
        setFixedJsonField(data, "reason", "REGISTRY_BUSY");
        setFixedJsonField(data, "leaseCount", nullptr);
        setFixedJsonField(data, "leases", nullptr);
    }
    return observed;
}

inline void writeRuntimeResourceHeader(JsonObject data, const char* activeProfileId) {
    data.clear();
    setFixedJsonField(data, "schemaVersion", 1);
    setFixedJsonField(data, "status", "partial");
    setFixedJsonField(data, "scope", "managed_leases");
    setFixedJsonField(data, "activeProfileId", activeProfileId);
    setFixedJsonField(data, "complete", false);
    setFixedJsonField(data, "atomicAcrossSources", false);
    setFixedJsonField(data, "hardwareProbed", false);
    setFixedJsonField(data, "freeCapacityKnown", false);
    setFixedJsonField(data, "coverageNote", "Unmanaged SDK users and other driver registries are not included");
    JsonArray missing = data["unavailable"].to<JsonArray>();
    for (const char* kind : {"LEDC", "RMT", "ADC", "SDMMC", "GPIO_OTHER"}) {
        JsonObject item = missing.add<JsonObject>();
        setFixedJsonField(item, "kind", kind);
        const bool sdmmc = std::strcmp(kind, "SDMMC") == 0;
        setFixedJsonField(item, "status", sdmmc ? "unsupported" : "unknown");
        setFixedJsonField(item, "reason", sdmmc ? "SDSPI_ONLY" : std::strcmp(kind, "RMT") == 0
            ? "PARTIAL_DHT_CAPTURE_AND_NEOPIXEL_OWNER_COVERAGE" : std::strcmp(kind, "LEDC") == 0
            ? "PARTIAL_PM_SCRIPT_AND_BACKLIGHT_COVERAGE" : std::strcmp(kind, "ADC") == 0
            ? "ONLY_CHECKED_ONESHOT_COVERED" : "ONLY_CHECKED_PM_SCRIPT_GPIO_DAC_COVERED");
    }
}

inline void writeI2CResourceJson(JsonObject data, const I2CResourceSnapshot& snapshot) {
    if (!writeResourceObservation(data, snapshot.status)) return;
    setFixedJsonField(data, "leaseCount", snapshot.count);
    setFixedJsonField(data, "managerInitialized", snapshot.initialized);
    setFixedJsonField(data, "sda", snapshot.sda);
    setFixedJsonField(data, "scl", snapshot.scl);
    setFixedJsonField(data, "frequency", snapshot.frequency);
    setFixedJsonField(data, "busState", snapshot.initialized ? "initialized_record" :
        snapshot.count ? "retained_uninitialized" : "no_managed_reservation");
    JsonArray leases = data["leases"].to<JsonArray>();
    for (uint8_t i = 0; i < snapshot.count && i < I2CResourceSnapshot::CAPACITY; ++i) {
        const auto& source = snapshot.leases[i];
        JsonObject lease = leases.add<JsonObject>();
        setFixedJsonField(lease, "slot", source.slot);
        writeResourceConsumerJson(lease, source.consumer);
        setFixedJsonField(lease, "addressReserved", source.address != 0);
        if (source.address) setFixedJsonField(lease, "address", source.address);
        setFixedJsonField(lease, "state", source.retiring ? "retiring" : "reserved");
        setFixedJsonField(lease, "cleanupPending", source.retiring);
    }
}

inline void writeSpiResourceJson(JsonObject data, const SpiResourceSnapshot& snapshot) {
    if (!writeResourceObservation(data, snapshot.status)) return;
    setFixedJsonField(data, "leaseCount", snapshot.count +
        (snapshot.networkReserved && snapshot.networkDedicatedHost ? 1 : 0));
    setFixedJsonField(data, "managerInitialized", snapshot.initialized);
    setFixedJsonField(data, "retainedWithoutClients", snapshot.initialized && snapshot.count == 0);
    setFixedJsonField(data, "sck", snapshot.sck); setFixedJsonField(data, "miso", snapshot.miso); setFixedJsonField(data, "mosi", snapshot.mosi);
    setFixedJsonField(data, "cleanupPending", nullptr);
    setFixedJsonField(data, "cleanupReason", "LEASE_LIFECYCLE_NOT_RECORDED");
    JsonArray leases = data["leases"].to<JsonArray>();
    for (uint8_t i = 0; i < snapshot.count && i < SpiResourceSnapshot::CAPACITY; ++i) {
        const auto& source = snapshot.leases[i];
        JsonObject lease = leases.add<JsonObject>();
        setFixedJsonField(lease, "slot", source.slot);
        writeResourceConsumerJson(lease, source.consumer);
        if (source.name[0]) {
            setFixedJsonField(lease, "ownerKnown", true);
            setFixedJsonField(lease, "ownerId", JsonString(source.name, false));
        }
        setFixedJsonField(lease, "cs", source.cs);
        setFixedJsonField(lease, "requestedMiso", source.requestedMiso);
        setFixedJsonField(lease, "networkOwner", source.networkOwner);
        setFixedJsonField(lease, "state", "reserved");
    }
    JsonObject network = data["network"].to<JsonObject>();
    setFixedJsonField(network, "reserved", snapshot.networkReserved);
    if (snapshot.networkReserved) {
        setFixedJsonField(network, "ownerKnown", false);
        setFixedJsonField(network, "ownerRole", "ethernet");
        setFixedJsonField(network, "dedicatedHost", snapshot.networkDedicatedHost);
        setFixedJsonField(network, "includedInClientLeases", !snapshot.networkDedicatedHost);
        JsonArray pins = network["pins"].to<JsonArray>();
        for (int8_t pin : snapshot.networkPins) pins.add(pin);
    }
}

inline void writeUartResourceJson(JsonObject data, const UartResourceSnapshot& snapshot) {
    writeResourceObservation(data, ResourceObservation::Observed);
    setFixedJsonField(data, "portCount", snapshot.portCount);
    setFixedJsonField(data, "atomicAcrossPorts", false);
    setFixedJsonField(data, "consoleExcluded", true);
    setFixedJsonField(data, "cleanupPending", nullptr);
    setFixedJsonField(data, "cleanupReason", "LEASE_LIFECYCLE_NOT_RECORDED");
    unsigned count = 0;
    JsonArray leases = data["leases"].to<JsonArray>();
    for (uint8_t port = 1; port < snapshot.portCount && port < UartResourceSnapshot::CAPACITY; ++port) {
        if (!snapshot.generations[port]) continue;
        ++count;
        JsonObject lease = leases.add<JsonObject>();
        setFixedJsonField(lease, "port", port);
        setFixedJsonField(lease, "generation", snapshot.generations[port]);
        writeResourceConsumerJson(lease, snapshot.consumers[port]);
        setFixedJsonField(lease, "state", "reserved");
    }
    setFixedJsonField(data, "leaseCount", count);
}

inline void writeControlOutputResourceJson(JsonObject data, const ControlOutputResourceSnapshot& snapshot) {
    if (!writeResourceObservation(data, snapshot.status)) return;
    setFixedJsonField(data, "leaseCount", snapshot.count);
    JsonArray leases = data["leases"].to<JsonArray>();
    for (uint8_t i = 0; i < snapshot.count && i < ControlOutputResourceSnapshot::CAPACITY; ++i) {
        const auto& source = snapshot.leases[i];
        JsonObject lease = leases.add<JsonObject>();
        setFixedJsonField(lease, "slot", source.slot);
        // Claimed native resource and actual acquiring consumer are distinct.
        writeResourceConsumerJson(lease, source.consumer);
        setFixedJsonField(lease, "resourceId", JsonString(source.id, false));
        setFixedJsonField(lease, "pin", source.pin);
        setFixedJsonField(lease, "fault", source.fault);
        setFixedJsonField(lease, "cleanupPending", source.retiring);
        setFixedJsonField(lease, "cleanupAttempted", source.retirementAttempted);
        setFixedJsonField(lease, "cleanupFailed", source.retiring && source.retirementAttempted && source.fault);
        setFixedJsonField(lease, "state", source.retiring ? "retiring" : source.fault ? "retained_after_fault" : "reserved");
    }
}

inline void writeEncoderResourceJson(JsonObject data, const EncoderResourceSnapshot& snapshot) {
    if (!writeResourceObservation(data, snapshot.status)) return;
    setFixedJsonField(data, "leaseCount", snapshot.count);
    setFixedJsonField(data, "scope", "gpio_isr_slots");
    setFixedJsonField(data, "pcntUsed", false);
    setFixedJsonField(data, "interruptEvidence", "checked_sdk_lifecycle");
    JsonArray leases = data["leases"].to<JsonArray>();
    for (uint8_t i = 0; i < snapshot.count && i < EncoderResourceSnapshot::CAPACITY; ++i) {
        const auto& source = snapshot.leases[i];
        JsonObject lease = leases.add<JsonObject>();
        setFixedJsonField(lease, "slot", source.slot);
        setFixedJsonField(lease, "ownerKnown", source.id[0] != '\0');
        if (source.id[0]) setFixedJsonField(lease, "ownerId", JsonString(source.id, false));
        setFixedJsonField(lease, "ownerRole", "encoder");
        setFixedJsonField(lease, "pinA", source.pinA); setFixedJsonField(lease, "pinB", source.pinB);
        setFixedJsonField(lease, "active", source.active);
        setFixedJsonField(lease, "interruptAttached", source.interruptAttached);
        setFixedJsonField(lease, "interruptCleanupPending", source.interruptCleanupPending);
        setFixedJsonField(lease, "fault", source.fault);
        setFixedJsonField(lease, "cleanupPending", source.cleanupPending);
        setFixedJsonField(lease, "state", source.cleanupPending ? "retiring" : source.fault ? "retained_after_fault"
            : source.active ? "active" : "reserved");
    }
}

inline void writeDhtRmtResourceJson(JsonObject data, const DhtRmtResourceSnapshot& snapshot) {
    if (!writeResourceObservation(data, snapshot.status)) return;
    setFixedJsonField(data, "scope", "dht_capture_only");
    setFixedJsonField(data, "complete", false);
    if (!snapshot.supported) {
        setFixedJsonField(data, "status", "unsupported"); setFixedJsonField(data, "reason", "DHT_RMT_NOT_COMPILED");
        setFixedJsonField(data, "leaseCount", nullptr); setFixedJsonField(data, "leases", nullptr);
        return;
    }
    setFixedJsonField(data, "leaseCount", snapshot.retainedPin >= 0 ? 1 : 0);
    JsonArray leases = data["leases"].to<JsonArray>();
    if (snapshot.retainedPin >= 0) {
        JsonObject lease = leases.add<JsonObject>();
        setFixedJsonField(lease, "ownerKnown", false); setFixedJsonField(lease, "ownerRole", "dht_capture");
        setFixedJsonField(lease, "pin", snapshot.retainedPin); setFixedJsonField(lease, "direction", "rx");
        setFixedJsonField(lease, "channel", nullptr);
        // Observer holds the same mutex as capture. A pin still retained
        // outside an in-flight capture is a failed teardown, never an idle RX.
        setFixedJsonField(lease, "state", "retained_after_fault");
        setFixedJsonField(lease, "cleanupPending", true); setFixedJsonField(lease, "cleanupFailed", true);
    }
}

inline void writeLedcResourceJson(JsonObject data, const LedcResourceSnapshot& snapshot) {
    if (!writeResourceObservation(data, snapshot.status)) return;
    setFixedJsonField(data, "scope", "pm_and_script_lifecycles_only");
    setFixedJsonField(data, "complete", false);
    setFixedJsonField(data, "leaseCount", snapshot.count);
    JsonArray leases = data["leases"].to<JsonArray>();
    for (uint8_t i = 0; i < snapshot.count && i < LedcResourceSnapshot::CAPACITY; ++i) {
        const auto& source = snapshot.leases[i];
        JsonObject lease = leases.add<JsonObject>();
        setFixedJsonField(lease, "slot", source.slot); setFixedJsonField(lease, "pin", source.pin);
        if (source.channel == 255) setFixedJsonField(lease, "channel", nullptr);
        else setFixedJsonField(lease, "channel", source.channel);
        setFixedJsonField(lease, "ownerKnown", source.id[0] != '\0');
        if (source.id[0]) setFixedJsonField(lease, "ownerId", JsonString(source.id, false));
        setFixedJsonField(lease, "ownerRole", source.commandScript ? "command_script" : "peripheral");
        setFixedJsonField(lease, "requestedFrequency", source.requestedFrequency);
        setFixedJsonField(lease, "requestedResolution", source.requestedResolution);
        setFixedJsonField(lease, "lastAttachSucceeded", true);
        setFixedJsonField(lease, "cleanupPending", source.cleanupFailed);
        setFixedJsonField(lease, "cleanupFailed", source.cleanupFailed);
        setFixedJsonField(lease, "state", source.cleanupFailed ? "retained_after_fault" : "reserved");
    }
}

inline void writeBacklightResourceJson(JsonObject data, const BacklightResourceSnapshot& snapshot, uint32_t now) {
    if (!writeResourceObservation(data, snapshot.status)) return;
    setFixedJsonField(data, "scope", "screen_task_published_backlight");
    if (!snapshot.available) {
        setFixedJsonField(data, "status", "unknown"); setFixedJsonField(data, "reason", "BACKLIGHT_NOT_OBSERVED");
        setFixedJsonField(data, "leaseCount", nullptr); setFixedJsonField(data, "leases", nullptr); return;
    }
    setFixedJsonField(data, "sampleAgeMs", static_cast<uint32_t>(now - snapshot.sampledAt));
    setFixedJsonField(data, "leaseCount", snapshot.retained ? 1 : 0);
    JsonArray leases = data["leases"].to<JsonArray>();
    if (!snapshot.retained) return;
    JsonObject lease = leases.add<JsonObject>();
    setFixedJsonField(lease, "ownerKnown", false); setFixedJsonField(lease, "ownerRole", "display_backlight");
    setFixedJsonField(lease, "pin", snapshot.pin); setFixedJsonField(lease, "channel", snapshot.channel);
    setFixedJsonField(lease, "lastAttachSucceeded", true);
    setFixedJsonField(lease, "cleanupFailed", snapshot.cleanupFailed); setFixedJsonField(lease, "cleanupPending", snapshot.cleanupFailed);
    setFixedJsonField(lease, "state", snapshot.cleanupFailed ? "retained_after_fault" : "reserved");
}

} // namespace FastBee
