#pragma once
#include <ArduinoJson.h>
#include <cstring>
#include "core/PeripheralExecution.h"
#include "core/ThermostatController.h"

namespace FastBee {
struct ThermostatConfig : ThermostatSettings { String sourceRef; };

inline bool parseThermostatConfig(const String& text, ThermostatConfig& out, String& error) {
    auto fail = [&](const char* reason) { error = reason; return false; };
    if (text.isEmpty() || text.length() > 768 || text.length() != std::strlen(text.c_str()))
        return fail("THERMOSTAT_CONFIG_SIZE");
    JsonDocument doc;
    if (deserializeJson(doc, text.c_str()) || !doc.is<JsonObject>()) return fail("THERMOSTAT_CONFIG_JSON");
    const JsonObjectConst object = doc.as<JsonObjectConst>();
    for (JsonPairConst pair : object) {
        const char* key = pair.key().c_str();
        if (!thermostatConfigFieldKnown(key)) return fail("THERMOSTAT_CONFIG_FIELD");
    }
    if (object.size() != kThermostatConfigFieldCount || !object["sourceRef"].is<const char*>()) return fail("THERMOSTAT_CONFIG_FIELDS_REQUIRED");
    const JsonString source = object["sourceRef"].as<JsonString>();
    if (!source.size() || source.size() > kThermostatSourceMaxBytes || source.size() != std::strlen(source.c_str()))
        return fail("THERMOSTAT_SOURCE_INVALID");
    const char* mode = object["mode"] | "";
    const char* recovery = object["recovery"] | "";
    for (const auto& parameter : kThermostatPolicies)
        if (!parameter.accepts(object[parameter.key] | "")) return fail("THERMOSTAT_POLICY_INVALID");
    if (!object["setpoint"].is<double>() || !object["hysteresis"].is<double>())
        return fail("THERMOSTAT_TEMPERATURE_INVALID");
    const double setpoint = object["setpoint"].as<double>();
    const double hysteresis = object["hysteresis"].as<double>();
    const auto& temperature = kThermostatNumbers[ThermostatSetpoint];
    if (!temperature.accepts(setpoint) || !kThermostatNumbers[ThermostatHysteresis].accepts(hysteresis) ||
        setpoint - hysteresis < temperature.minValue || setpoint + hysteresis > temperature.maxValue)
        return fail("THERMOSTAT_TEMPERATURE_RANGE");
    for (const auto& parameter : kThermostatNumbers) {
        if (parameter.integer && (!object[parameter.key].is<uint32_t>() ||
            !parameter.accepts(object[parameter.key].as<uint32_t>())))
            return fail("THERMOSTAT_DURATION_RANGE");
    }
    ThermostatConfig candidate;
    candidate.sourceRef = source.c_str();
    if (candidate.sourceRef.length() != source.size()) return fail("THERMOSTAT_NO_MEMORY");
    candidate.cooling = std::strcmp(mode, "cool") == 0;
    candidate.automaticRecovery = std::strcmp(recovery, "fresh") == 0;
    candidate.setpoint = setpoint; candidate.hysteresis = hysteresis;
    candidate.minOnMs = object["minOnSec"].as<uint32_t>() * 1000U;
    candidate.minOffMs = object["minOffSec"].as<uint32_t>() * 1000U;
    candidate.maxAgeMs = object["maxAgeSec"].as<uint32_t>() * 1000U;
    out = candidate;
    if (out.sourceRef != candidate.sourceRef) return fail("THERMOSTAT_NO_MEMORY");
    error = "";
    return true;
}

inline bool hasThermostatAction(const PeriphExecRule& rule) {
    for (const auto& action : rule.actions)
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_THERMOSTAT)) return true;
    return false;
}

inline bool validateThermostatRule(const PeriphExecRule& rule, String& error) {
    if (!hasThermostatAction(rule)) return true;
    if (rule.actions.size() != 1 || rule.triggers.size() != 1 ||
        rule.triggers[0].triggerType != static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER) ||
        rule.triggers[0].timerMode != static_cast<uint8_t>(ExecTimerMode::INTERVAL) ||
        rule.triggers[0].intervalSec != 1 || rule.allowPanelControl || rule.recordHistory ||
        !rule.scriptContent.isEmpty()) {
        error = "THERMOSTAT_STANDALONE_TIMER_REQUIRED"; return false;
    }
    const auto& action = rule.actions[0];
    if (action.targetPeriphId.isEmpty() || action.useReceivedValue || action.syncDelayMs) {
        error = "THERMOSTAT_LITERAL_TARGET_REQUIRED"; return false;
    }
    ThermostatConfig cfg;
    return parseThermostatConfig(action.actionValue, cfg, error);
}
} // namespace FastBee
