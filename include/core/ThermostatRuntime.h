#pragma once
#include "core/ControlOutput.h"
#include "core/ThermostatConfig.h"

namespace FastBee {
struct ThermostatRuntime {
    String ruleId;
    String targetId;
    String configurationText;
    String sourceRevision;
    String sourceBinding;
    uint64_t ruleRevision = 0;
    uint64_t checkedAtMs = 0;
    uint32_t sourceConfigEpoch = 0;
    ThermostatConfig config;
    ThermostatController controller;
    ControlOutput output;
    bool admitted = false;
    bool outputConfirmed = false;
    bool retiring = false;
};
} // namespace FastBee
