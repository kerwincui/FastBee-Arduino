#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace FastBee {

enum class FeatureInstanceState : uint8_t {
    Unobserved, NotInitialized, Initialized, Busy, CleanupPending,
    Allocated, Running, Stopped, Retiring, Idle, OwnerAbsent
};

enum class FeatureInstanceScope : uint8_t {
    DriverInitialization, ServiceLifecycle, ExecutionActivity, StaticResource, OwnerLifetime, TransportAllocation
};
enum class FeatureInstanceReason : uint8_t {
    ObserverMissing, SynchronizationUnavailable, BackendUnavailable, InvalidSnapshot
};

struct FeatureInstanceSnapshot {
    FeatureInstanceState state = FeatureInstanceState::Unobserved;
    uint16_t initializedCount = 0;
    FeatureInstanceScope scope = FeatureInstanceScope::DriverInitialization;
    FeatureInstanceReason reason = FeatureInstanceReason::ObserverMissing;
    static FeatureInstanceSnapshot lifecycle(FeatureInstanceState state) {
        return {state, 0, FeatureInstanceScope::ServiceLifecycle};
    }
    static FeatureInstanceSnapshot unavailable(FeatureInstanceReason reason,
            FeatureInstanceScope scope = FeatureInstanceScope::DriverInitialization) {
        return {FeatureInstanceState::Unobserved, 0, scope, reason};
    }
    bool observed() const {
        return state == FeatureInstanceState::NotInitialized ||
               state == FeatureInstanceState::Initialized ||
               state == FeatureInstanceState::CleanupPending ||
               state == FeatureInstanceState::Allocated || state == FeatureInstanceState::Running ||
               state == FeatureInstanceState::Stopped || state == FeatureInstanceState::Retiring ||
               state == FeatureInstanceState::Idle || state == FeatureInstanceState::OwnerAbsent;
    }
    bool countKnown() const {
        return state == FeatureInstanceState::NotInitialized || state == FeatureInstanceState::Initialized;
    }
    const char* name() const {
        switch (state) {
            case FeatureInstanceState::NotInitialized: return "not_initialized";
            case FeatureInstanceState::Initialized: return "initialized";
            case FeatureInstanceState::Busy: return "busy";
            case FeatureInstanceState::CleanupPending: return "cleanup_pending";
            case FeatureInstanceState::Allocated: return "allocated";
            case FeatureInstanceState::Running: return "running";
            case FeatureInstanceState::Stopped: return "stopped";
            case FeatureInstanceState::Retiring: return "retiring";
            case FeatureInstanceState::Idle: return "idle";
            case FeatureInstanceState::OwnerAbsent: return "owner_absent";
            default: return "unobserved";
        }
    }
    const char* scopeName() const {
        switch (scope) {
            case FeatureInstanceScope::ServiceLifecycle: return "service_lifecycle";
            case FeatureInstanceScope::ExecutionActivity: return "execution_activity";
            case FeatureInstanceScope::StaticResource: return "static_resource";
            case FeatureInstanceScope::OwnerLifetime: return "owner_lifetime";
            case FeatureInstanceScope::TransportAllocation: return "transport_allocation";
            default: return "driver_initialization";
        }
    }
    const char* reasonName() const {
        if (observed()) return "";
        if (state == FeatureInstanceState::Busy) return "observation_busy";
        switch (reason) {
            case FeatureInstanceReason::ObserverMissing: return "observer_not_registered";
            case FeatureInstanceReason::SynchronizationUnavailable: return "synchronization_unavailable";
            case FeatureInstanceReason::BackendUnavailable: return "backend_unavailable";
            default: return "invalid_snapshot";
        }
    }
};

// Missing observation is distinct from a stopped instance. This classification
// explains the boundary; it never synthesizes a count or consults user intent.
inline const char* missingFeatureInstanceReason(const char* key, bool compiled) {
    if (!compiled) return "not_compiled";
    if (!key) return "invalid_snapshot";
    if (!std::strcmp(key, "lcdGb2312") || !std::strcmp(key, "deviceControl"))
        return "no_dedicated_instance";
    static constexpr const char* observedKeys[] = {"ds1302", "rfid", "ethernet", "ioExpander",
        "i2cSensors", "lcd", "lcd1602", "irRemote", "rfLearn", "ruleScript", "screenPanel",
        "ds18b20", "neopixel", "sevenSegment", "fileLogging", "userAdmin", "mdns", "commandScript",
        "webServer", "logViewer", "modbus", "ble", "cellular", "ota", "mqtts"};
    for (const auto* observedKey : observedKeys)
        if (!std::strcmp(key, observedKey)) return "observer_not_registered";
    return "owner_snapshot_unavailable";
}

inline FeatureInstanceScope missingFeatureInstanceScope(const char* key) {
    if (key && (!std::strcmp(key, "lcdGb2312") || !std::strcmp(key, "deviceControl")))
        return FeatureInstanceScope::StaticResource;
    if (key && !std::strcmp(key, "commandScript")) return FeatureInstanceScope::ExecutionActivity;
    if (key && !std::strcmp(key, "mqtts")) return FeatureInstanceScope::TransportAllocation;
    static constexpr const char* ownerKeys[] = {"webServer", "logViewer", "modbus", "ble", "cellular", "ota"};
    for (const auto* ownerKey : ownerKeys)
        if (key && !std::strcmp(key, ownerKey)) return FeatureInstanceScope::OwnerLifetime;
    return FeatureInstanceScope::DriverInitialization;
}

// Descriptors and their context must have static lifetime. Readers are passive,
// bounded, allocation-free snapshots: no hardware I/O, instance creation,
// admission changes, or calls back into feature status serialization.
struct FeatureInstanceObserver {
    const char* key;
    FeatureInstanceSnapshot (*read)(const void* context);
    const void* context;
};

class FeatureInstanceRegistry {
public:
    static constexpr size_t CAPACITY = 32;
    static constexpr size_t MAX_KEY_LENGTH = 40;
    static FeatureInstanceRegistry& instance() { static FeatureInstanceRegistry registry; return registry; }

    bool registerObserver(const FeatureInstanceObserver* observer) {
        if (!observer || !observer->read || !validKey(observer->key)) return false;
        for (auto& slot : observers_) {
            const auto* existing = slot.load(std::memory_order_acquire);
            if (!existing) {
                if (slot.compare_exchange_strong(existing, observer,
                        std::memory_order_acq_rel, std::memory_order_acquire)) return true;
            }
            if (existing == observer) return true;
            if (existing && std::strcmp(existing->key, observer->key) == 0) return false;
        }
        return false; // Fail closed: capacity exhaustion does not invent a state.
    }

    FeatureInstanceSnapshot observe(const char* key) const {
        if (!validKey(key)) return FeatureInstanceSnapshot::unavailable(FeatureInstanceReason::InvalidSnapshot);
        for (const auto& slot : observers_) {
            const auto* observer = slot.load(std::memory_order_acquire);
            if (!observer || std::strcmp(observer->key, key) != 0) continue;
            const auto value = observer->read(observer->context);
            const auto invalid = FeatureInstanceSnapshot::unavailable(FeatureInstanceReason::InvalidSnapshot);
            if (value.scope != FeatureInstanceScope::DriverInitialization &&
                value.scope != FeatureInstanceScope::ServiceLifecycle &&
                value.scope != FeatureInstanceScope::ExecutionActivity &&
                value.scope != FeatureInstanceScope::StaticResource &&
                value.scope != FeatureInstanceScope::OwnerLifetime &&
                value.scope != FeatureInstanceScope::TransportAllocation) return invalid;
            switch (value.state) {
                case FeatureInstanceState::Initialized:
                    return value.initializedCount && value.scope == FeatureInstanceScope::DriverInitialization ? value : invalid;
                case FeatureInstanceState::NotInitialized:
                    return value.initializedCount == 0 && value.scope == FeatureInstanceScope::DriverInitialization ? value : invalid;
                case FeatureInstanceState::Busy:
                case FeatureInstanceState::CleanupPending:
                    return value.scope != FeatureInstanceScope::StaticResource
                        ? FeatureInstanceSnapshot{value.state, 0, value.scope} : invalid;
                case FeatureInstanceState::Allocated:
                case FeatureInstanceState::Retiring:
                    return (value.scope == FeatureInstanceScope::ServiceLifecycle ||
                            value.scope == FeatureInstanceScope::OwnerLifetime ||
                            value.scope == FeatureInstanceScope::TransportAllocation) && !value.initializedCount ? value : invalid;
                case FeatureInstanceState::Stopped:
                    return (value.scope == FeatureInstanceScope::ServiceLifecycle ||
                            value.scope == FeatureInstanceScope::TransportAllocation) && !value.initializedCount ? value : invalid;
                case FeatureInstanceState::OwnerAbsent:
                    return value.scope == FeatureInstanceScope::OwnerLifetime && !value.initializedCount ? value : invalid;
                case FeatureInstanceState::Running:
                    return (value.scope == FeatureInstanceScope::ServiceLifecycle ||
                            value.scope == FeatureInstanceScope::ExecutionActivity) && !value.initializedCount ? value : invalid;
                case FeatureInstanceState::Idle:
                    return value.scope == FeatureInstanceScope::ExecutionActivity && !value.initializedCount ? value : invalid;
                case FeatureInstanceState::Unobserved:
                    return FeatureInstanceSnapshot::unavailable(value.reason, value.scope);
                default: return invalid;
            }
        }
        return {};
    }

private:
    static bool validKey(const char* key) {
        if (!key || !key[0]) return false;
        for (size_t i=0; i<=MAX_KEY_LENGTH; ++i) if (!key[i]) return true;
        return false;
    }
    std::atomic<const FeatureInstanceObserver*> observers_[CAPACITY]{};
};

} // namespace FastBee
