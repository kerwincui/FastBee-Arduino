#pragma once
#include <ArduinoJson.h>
#include "core/ThermostatConfigMetadata.h"

namespace FastBee {
inline void writeThermostatConfigJson(JsonObject out) {
    out["version"] = 1;
    out["additionalProperties"] = false;
    out["requiresStandaloneTimerSec"] = 1;
    JsonArray fields = out["fields"].to<JsonArray>();
    JsonObject source = fields.add<JsonObject>();
    source["key"] = "sourceRef"; source["type"] = "string";
    source["required"] = true; source["maxBytes"] = kThermostatSourceMaxBytes;
    source["reference"] = "saved_enabled_celsius_channel";
    for (const auto& parameter : kThermostatNumbers) {
        JsonObject field = fields.add<JsonObject>();
        field["key"] = parameter.key;
        field["type"] = parameter.integer ? "integer" : "number";
        field["unit"] = parameter.unit; field["required"] = true;
        field["default"] = parameter.defaultValue;
        field["min"] = parameter.minValue; field["max"] = parameter.maxValue;
    }
    for (const auto& parameter : kThermostatPolicies) {
        JsonObject field = fields.add<JsonObject>();
        field["key"] = parameter.key; field["type"] = "string";
        field["required"] = true; field["default"] = parameter.defaultValue;
        JsonArray choices = field["choices"].to<JsonArray>();
        for (size_t i = 0; i < parameter.count; ++i) choices.add(parameter.choices[i]);
    }
    // Readonly descriptions do not grant runtime or wiring admission.
    out["crossField"] = "setpoint_plus_minus_hysteresis_within_setpoint_range";
    out["runtimeAdmitted"] = false;
}
} // namespace FastBee
