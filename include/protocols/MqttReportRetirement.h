#pragma once
#include <cstdint>

namespace FastBee {
enum class MqttRetirementReason : uint8_t {
    Unspecified, ConfigurationDisabled, Rebuild, NetworkSwitch, Shutdown, InitializationFailed, Count
};
inline const char* mqttRetirementReasonName(MqttRetirementReason reason) {
    switch (reason) {
        case MqttRetirementReason::ConfigurationDisabled: return "configuration_disabled";
        case MqttRetirementReason::Rebuild: return "rebuild";
        case MqttRetirementReason::NetworkSwitch: return "network_switch";
        case MqttRetirementReason::Shutdown: return "shutdown";
        case MqttRetirementReason::InitializationFailed: return "initialization_failed";
        default: return "unspecified";
    }
}
// Boot-local loss accounting only. Caller serializes record/snapshot. No payload,
// credentials, object identity, heap allocation, replay or persistent writes.
class MqttReportRetirement {
public:
    static constexpr uint8_t ReasonCount = static_cast<uint8_t>(MqttRetirementReason::Count);
    struct Snapshot {
        uint32_t instances = 0, instancesWithLoss = 0, discardedReports = 0, unconfirmedInstances = 0;
        uint32_t byReason[ReasonCount] = {};
        MqttRetirementReason lastLossReason = MqttRetirementReason::Unspecified;
        uint8_t lastLossRingDepth = 0;
        bool lastLossPendingReport = false, saturated = false;
    };
    void record(MqttRetirementReason reason, uint32_t ringDepth, bool pending, bool consumerActive) {
        add(_snapshot.instances, 1);
        if (consumerActive || ringDepth > 8) {
            add(_snapshot.unconfirmedInstances, 1);
            return; // Never convert an unconfirmed in-flight frame into exact loss.
        }
        const uint32_t lost = ringDepth + (pending ? 1u : 0u);
        if (!lost) return; // Empty retirements never erase the last loss evidence.
        uint8_t index = static_cast<uint8_t>(reason);
        if (index >= ReasonCount) index = 0;
        add(_snapshot.instancesWithLoss, 1);
        add(_snapshot.discardedReports, lost);
        add(_snapshot.byReason[index], lost);
        _snapshot.lastLossReason = static_cast<MqttRetirementReason>(index);
        _snapshot.lastLossRingDepth = static_cast<uint8_t>(ringDepth);
        _snapshot.lastLossPendingReport = pending;
    }
    Snapshot snapshot() const { return _snapshot; }
private:
    void add(uint32_t& target, uint32_t amount) {
        if (UINT32_MAX - target < amount) { target = UINT32_MAX; _snapshot.saturated = true; }
        else target += amount;
    }
    Snapshot _snapshot;
};
} // namespace FastBee
