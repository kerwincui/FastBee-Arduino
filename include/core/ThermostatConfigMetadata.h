#pragma once
#include <cmath>
#include <cstddef>
#include <cstring>

namespace FastBee {
// Literal NC01 configuration only. Cross-field, source-graph and physical
// output admission remain in their existing validators.
struct ThermostatNumberParameter {
    const char* key;
    const char* unit;
    double defaultValue;
    double minValue;
    double maxValue;
    bool integer;
    bool accepts(double value) const {
        return std::isfinite(value) && value >= minValue && value <= maxValue &&
            (!integer || std::floor(value) == value);
    }
};
enum ThermostatNumberIndex { ThermostatSetpoint, ThermostatHysteresis,
    ThermostatMinOn, ThermostatMinOff, ThermostatMaxAge };
inline constexpr ThermostatNumberParameter kThermostatNumbers[] = {
    {"setpoint", "°C", 25, -55, 125, false},
    {"hysteresis", "°C", 1, 0.1, 50, false},
    {"minOnSec", "s", 30, 1, 86400, true},
    {"minOffSec", "s", 30, 1, 86400, true},
    {"maxAgeSec", "s", 30, 1, 3600, true}
};
struct ThermostatPolicyParameter {
    const char* key;
    const char* defaultValue;
    const char* choices[2];
    size_t count;
    bool accepts(const char* value) const {
        if (!value) return false;
        for (size_t i = 0; i < count; ++i)
            if (std::strcmp(value, choices[i]) == 0) return true;
        return false;
    }
};
enum ThermostatPolicyIndex { ThermostatMode, ThermostatRecovery, ThermostatStartup, ThermostatFault };
inline constexpr ThermostatPolicyParameter kThermostatPolicies[] = {
    {"mode", "heat", {"heat", "cool"}, 2},
    {"recovery", "manual", {"manual", "fresh"}, 2},
    {"startup", "fresh", {"fresh", nullptr}, 1},
    {"fault", "off", {"off", nullptr}, 1}
};
static constexpr size_t kThermostatSourceMaxBytes = 96;
static constexpr size_t kThermostatConfigFieldCount =
    1 + sizeof(kThermostatNumbers) / sizeof(kThermostatNumbers[0]) +
    sizeof(kThermostatPolicies) / sizeof(kThermostatPolicies[0]);

inline bool thermostatConfigFieldKnown(const char* key) {
    if (!std::strcmp(key, "sourceRef")) return true;
    for (const auto& parameter : kThermostatNumbers)
        if (!std::strcmp(key, parameter.key)) return true;
    for (const auto& parameter : kThermostatPolicies)
        if (!std::strcmp(key, parameter.key)) return true;
    return false;
}
} // namespace FastBee
