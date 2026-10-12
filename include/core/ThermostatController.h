#pragma once
#include <cmath>
#include <cstdint>
#include "core/ThermostatConfigMetadata.h"

namespace FastBee {
// Pure, monotonic-time controller. It decides commands; only confirmed pad
// writes may commit state. It is not a physical contactor/load feedback model.
struct ThermostatSettings {
    bool cooling = std::strcmp(kThermostatPolicies[ThermostatMode].defaultValue, "cool") == 0;
    bool automaticRecovery = std::strcmp(kThermostatPolicies[ThermostatRecovery].defaultValue, "fresh") == 0;
    double setpoint = kThermostatNumbers[ThermostatSetpoint].defaultValue;
    double hysteresis = kThermostatNumbers[ThermostatHysteresis].defaultValue;
    uint32_t minOnMs = static_cast<uint32_t>(kThermostatNumbers[ThermostatMinOn].defaultValue) * 1000U;
    uint32_t minOffMs = static_cast<uint32_t>(kThermostatNumbers[ThermostatMinOff].defaultValue) * 1000U;
    uint32_t maxAgeMs = static_cast<uint32_t>(kThermostatNumbers[ThermostatMaxAge].defaultValue) * 1000U;
};

enum class ThermostatPhase : uint8_t {
    WaitingFresh, Idle, MinimumOff, On, MinimumOn, SourceFault,
    ManualRecovery, SourceChanged, OutputUnknown, MemorySuspended
};

inline const char* thermostatPhaseName(ThermostatPhase phase) {
    switch (phase) {
        case ThermostatPhase::WaitingFresh: return "WAITING_FRESH_SAMPLE";
        case ThermostatPhase::Idle: return "OFF_CONFIRMED";
        case ThermostatPhase::MinimumOff: return "MINIMUM_OFF_TIME";
        case ThermostatPhase::On: return "ON_CONFIRMED";
        case ThermostatPhase::MinimumOn: return "MINIMUM_ON_TIME";
        case ThermostatPhase::SourceFault: return "SOURCE_INVALID_SAFE_OFF";
        case ThermostatPhase::ManualRecovery: return "FAULT_LATCHED_REENABLE_REQUIRED";
        case ThermostatPhase::SourceChanged: return "SOURCE_CHANGED_REENABLE_REQUIRED";
        case ThermostatPhase::OutputUnknown: return "OUTPUT_UNCONFIRMED";
        case ThermostatPhase::MemorySuspended: return "MEMORY_SUSPENDED_SAFE_OFF";
    }
    return "OUTPUT_UNCONFIRMED";
}

struct ThermostatSample {
    bool valid = false;
    double value = 0;
    uint64_t sampledAtMs = 0;
};

class ThermostatController {
public:
    void begin(uint64_t now) {
        on_ = false; fault_ = false; permanentFault_ = false; acceptedSample_ = false;
        freshAfter_ = changedAt_ = now;
        phase_ = ThermostatPhase::WaitingFresh;
    }
    bool on() const { return on_; }
    bool faulted() const { return fault_; }
    ThermostatPhase phase() const { return phase_; }
    // Fault handling bypasses minimum-on. Restoring data never bypasses
    // minimum-off, and an old still-valid cache entry cannot rearm a fault.
    void fault(uint64_t now, ThermostatPhase reason, bool permanent = false) {
        if (!fault_) freshAfter_ = now;
        fault_ = true;
        permanentFault_ = permanentFault_ || permanent;
        phase_ = reason;
    }
    bool decide(const ThermostatSettings& cfg, const ThermostatSample& sample,
                uint64_t now) {
        if (permanentFault_) return false;
        const bool valid = sample.valid && std::isfinite(sample.value) &&
            now >= sample.sampledAtMs && now - sample.sampledAtMs <= cfg.maxAgeMs;
        if (!valid) {
            if (!acceptedSample_ && !fault_) {
                phase_ = ThermostatPhase::WaitingFresh;
                return false;
            }
            fault(now, ThermostatPhase::SourceFault);
            return false;
        }
        if (fault_) {
            if (!cfg.automaticRecovery) {
                phase_ = ThermostatPhase::ManualRecovery;
                return false;
            }
            if (sample.sampledAtMs <= freshAfter_) {
                phase_ = ThermostatPhase::WaitingFresh;
                return false;
            }
            fault_ = false;
        }
        if (sample.sampledAtMs <= freshAfter_) {
            phase_ = ThermostatPhase::WaitingFresh;
            return false;
        }
        acceptedSample_ = true;
        const double start = cfg.cooling ? cfg.setpoint + cfg.hysteresis
                                         : cfg.setpoint - cfg.hysteresis;
        bool wanted = on_;
        if (on_) {
            if (cfg.cooling ? sample.value <= cfg.setpoint : sample.value >= cfg.setpoint)
                wanted = false;
        } else if (cfg.cooling ? sample.value >= start : sample.value <= start) wanted = true;
        const uint64_t elapsed = now >= changedAt_ ? now - changedAt_ : 0;
        if (wanted != on_ && elapsed < (on_ ? cfg.minOnMs : cfg.minOffMs)) {
            phase_ = on_ ? ThermostatPhase::MinimumOn : ThermostatPhase::MinimumOff;
            return on_;
        }
        phase_ = wanted ? ThermostatPhase::On : ThermostatPhase::Idle;
        return wanted;
    }
    void confirm(bool commandedOn, uint64_t now) {
        if (on_ != commandedOn) changedAt_ = now;
        on_ = commandedOn;
    }
    void outputFailure(uint64_t now) {
        fault(now, ThermostatPhase::OutputUnknown, true);
    }
private:
    bool on_ = false;
    bool fault_ = false;
    bool permanentFault_ = false;
    bool acceptedSample_ = false;
    uint64_t freshAfter_ = 0;
    uint64_t changedAt_ = 0;
    ThermostatPhase phase_ = ThermostatPhase::WaitingFresh;
};
} // namespace FastBee
