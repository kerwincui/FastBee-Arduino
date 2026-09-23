#pragma once

#include <ArduinoJson.h>
#include <cstring>
#include "core/PeripheralExecution.h"
#include "core/ResourceProfile.h"

namespace FastBee {
namespace PeriphExecConfig {

constexpr size_t kMaxBytes = 96 * 1024;

// No allocation, truncation or default conversion in these checks. Null/missing
// optional fields remain compatible with older files and receive loader defaults.
inline bool textFits(JsonVariantConst value, size_t maxBytes, bool required = false) {
    if (value.isNull()) return !required;
    if (!value.is<const char*>()) return false;
    const JsonString text = value.as<JsonString>();
    return (!required || text.size() != 0) && text.size() <= maxBytes &&
           std::strlen(text.c_str()) == text.size();
}

inline bool uintFits(JsonVariantConst value, uint32_t maxValue) {
    return value.isNull() || (value.is<uint32_t>() && value.as<uint32_t>() <= maxValue);
}

inline bool boolFits(JsonVariantConst value) {
    return value.isNull() || value.is<bool>() || uintFits(value, 1);
}

inline const char* validateRule(JsonObjectConst rule) {
    if (rule.isNull()) return "CONFIG_RULE_OBJECT";
    if (!textFits(rule["id"], ResourceProfile::MAX_RULE_ID_LEN, true)) return "CONFIG_RULE_ID";
    if (!textFits(rule["name"], ResourceProfile::MAX_RULE_NAME_LEN, true)) return "CONFIG_RULE_NAME";
    if (!textFits(rule["scriptContent"], ResourceProfile::MAX_SCRIPT_CONTENT_LEN)) return "CONFIG_RULE_SCRIPT";
    if (!boolFits(rule["enabled"]) || !boolFits(rule["reportAfterExec"]) ||
        !boolFits(rule["allowPanelControl"]) || !boolFits(rule["recordHistory"]) ||
        !uintFits(rule["execMode"], 1) || !uintFits(rule["protocolType"], 5)) return "CONFIG_RULE_TYPE";
    const JsonArrayConst triggers = rule["triggers"].as<JsonArrayConst>();
    const JsonArrayConst actions = rule["actions"].as<JsonArrayConst>();
    if (triggers.isNull() || triggers.size() == 0 || triggers.size() > MAX_TRIGGERS_PER_RULE)
        return "CONFIG_TRIGGER_COUNT";
    if (actions.isNull() || actions.size() == 0 || actions.size() > MAX_ACTIONS_PER_RULE)
        return "CONFIG_ACTION_COUNT";
    for (JsonVariantConst value : triggers) {
        const JsonObjectConst t = value.as<JsonObjectConst>();
        if (t.isNull() || !uintFits(t["triggerType"], 5) || !uintFits(t["operatorType"], 9) ||
            !uintFits(t["timerMode"], 1) || !uintFits(t["intervalSec"], UINT32_MAX) ||
            !uintFits(t["pollResponseTimeout"], UINT16_MAX) || !uintFits(t["pollMaxRetries"], UINT8_MAX) ||
            !uintFits(t["pollInterPollDelay"], UINT16_MAX)) return "CONFIG_TRIGGER_TYPE";
        for (const char* field : {"triggerPeriphId", "compareValue", "timePoint", "eventId"}) {
            if (!textFits(t[field], kMaxBytes)) return "CONFIG_TRIGGER_TEXT";
        }
    }
    for (JsonVariantConst value : actions) {
        const JsonObjectConst a = value.as<JsonObjectConst>();
        if (a.isNull() || !uintFits(a["actionType"], 34) || !uintFits(a["syncDelayMs"], UINT16_MAX) ||
            !uintFits(a["execMode"], 1) || !boolFits(a["useReceivedValue"])) return "CONFIG_ACTION_TYPE";
        if (!textFits(a["targetPeriphId"], kMaxBytes) ||
            !textFits(a["actionValue"], ResourceProfile::MAX_ACTION_VALUE_LEN)) return "CONFIG_ACTION_TEXT";
    }
    return nullptr;
}

// Used by ordinary CRUD/save as well as the on-disk validator. Runtime fields
// are deliberately ignored. All limits are the existing unified limits.
inline const char* validateRule(const PeriphExecRule& rule) {
    if (rule.id.isEmpty() || rule.id.length() > ResourceProfile::MAX_RULE_ID_LEN) return "CONFIG_RULE_ID";
    if (rule.name.isEmpty() || rule.name.length() > ResourceProfile::MAX_RULE_NAME_LEN) return "CONFIG_RULE_NAME";
    if (rule.scriptContent.length() > ResourceProfile::MAX_SCRIPT_CONTENT_LEN) return "CONFIG_RULE_SCRIPT";
    if (rule.triggers.empty() || rule.triggers.size() > MAX_TRIGGERS_PER_RULE) return "CONFIG_TRIGGER_COUNT";
    if (rule.actions.empty() || rule.actions.size() > MAX_ACTIONS_PER_RULE) return "CONFIG_ACTION_COUNT";
    if (rule.execMode > 1 || rule.protocolType > 5) return "CONFIG_RULE_TYPE";
    for (const auto& t : rule.triggers) {
        if (t.triggerType > 5 || t.operatorType > 9 || t.timerMode > 1) return "CONFIG_TRIGGER_TYPE";
    }
    for (const auto& a : rule.actions) {
        if (a.actionType > 34 || a.execMode > 1) return "CONFIG_ACTION_TYPE";
        if (a.actionValue.length() > ResourceProfile::MAX_ACTION_VALUE_LEN) return "CONFIG_ACTION_TEXT";
    }
    return nullptr;
}

inline const char* validateDocument(JsonVariantConst root) {
    if (!root.is<JsonObjectConst>() || !root["rules"].is<JsonArrayConst>()) return "CONFIG_RULES_ARRAY";
    const JsonArrayConst rules = root["rules"].as<JsonArrayConst>();
    if (rules.size() > ResourceProfile::MAX_PERIPH_EXEC_RULES) return "CONFIG_RULE_COUNT";
    size_t index = 0;
    for (JsonVariantConst value : rules) {
        const char* error = validateRule(value.as<JsonObjectConst>());
        if (error) return error;
        for (size_t previous = 0; previous < index; ++previous) {
            if (std::strcmp(value["id"].as<const char*>(), rules[previous]["id"].as<const char*>()) == 0)
                return "CONFIG_DUPLICATE_ID";
        }
        ++index;
    }
    if (!root["irKeys"].isNull() && !root["irKeys"].is<JsonArrayConst>()) return "CONFIG_IR_ARRAY";
    return nullptr;
}

} // namespace PeriphExecConfig
} // namespace FastBee


