#pragma once

#include <ArduinoJson.h>
#include <cstring>
#include <memory>
#include <new>
#include "core/PeripheralExecution.h"
#include "core/ResourceProfile.h"
#include "core/RuleCompletionGraph.h"

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

// Exact rule IDs plus bounded temporary emission strings for streaming validation.
// No hashes or truncation that could mistake a collision for an exact match.
struct RuleReferenceIndex {
    char ids[ResourceProfile::MAX_PERIPH_EXEC_RULES][ResourceProfile::MAX_RULE_ID_LEN + 1];
    size_t count = 0;
    size_t emittedBytes = 0;
    RuleCompletionGraph completion;
    // At most profile.rules * MAX_ACTIONS_PER_RULE exact IDs. Strings are
    // copied once during the first streaming pass and released with the index.
    // No hash collisions or fixed-buffer truncation can invent/drop an edge.
    std::unique_ptr<char[]> emitted[ResourceProfile::MAX_PERIPH_EXEC_RULES][MAX_ACTIONS_PER_RULE];

    const char* captureRuleEmissions(JsonObjectConst rule, char* (*allocate)(size_t)) {
        if (!count || count > RuleCompletionGraph::Capacity ||
            rule["actions"].size() > MAX_ACTIONS_PER_RULE) return "CONFIG_RULE_EVENT_GRAPH_LIMIT";
        size_t used = 0;
        const bool enabled = rule["enabled"].isNull() || rule["enabled"].as<bool>();
        for (JsonObjectConst action : rule["actions"].as<JsonArrayConst>()) {
            if (enabled && hasDynamicRuleEventTarget(action["actionType"].as<uint8_t>(),
                    action["targetPeriphId"].as<const char*>(), action["useReceivedValue"].as<bool>()))
                return "CONFIG_RULE_EVENT_DYNAMIC_TARGET";
            const char* event = fixedRuleActionEvent(action["actionType"].as<uint8_t>(),
                action["targetPeriphId"].as<const char*>(), action["actionValue"].as<const char*>(),
                action["useReceivedValue"].as<bool>(), ids[count - 1]);
            if (!event || !*event) continue;
            bool duplicate = false;
            for (size_t i = 0; i < used; ++i)
                if (std::strcmp(emitted[count - 1][i].get(), event) == 0) duplicate = true;
            if (duplicate) continue;
            const size_t length = std::strlen(event);
            // Bound aggregate streaming scratch independently of allocator
            // health; every actual copy uses the supplied guarded allocator.
            if (length >= kMaxBytes || length + 1 > kMaxBytes - emittedBytes)
                return "CONFIG_RULE_EVENT_GRAPH_LIMIT";
            std::unique_ptr<char[]> copy(allocate ? allocate(length + 1) : nullptr);
            if (!copy) return "CONFIG_MEMORY_PRESSURE";
            std::memcpy(copy.get(), event, length + 1);
            emitted[count - 1][used++] = std::move(copy);
            emittedBytes += length + 1;
        }
        return nullptr;
    }

    const char* recordCompletionRule(JsonObjectConst rule) {
        if (count > RuleCompletionGraph::Capacity) return "CONFIG_RULE_EVENT_GRAPH_LIMIT";
        if (rule["triggers"].size() > MAX_TRIGGERS_PER_RULE) return "CONFIG_RULE_EVENT_GRAPH_LIMIT";
        const char* id = rule["id"].as<const char*>();
        size_t target = 0;
        while (target < count && std::strcmp(ids[target], id ? id : "") != 0) ++target;
        if (target == count) return "CONFIG_RULE_EVENT_GRAPH_LIMIT";
        // Match decodeConfigRule's legacy default and accepted boolean/0/1 forms.
        completion.setEnabled(target, rule["enabled"].isNull() || rule["enabled"].as<bool>());
        for (JsonObjectConst trigger : rule["triggers"].as<JsonArrayConst>()) {
            if (trigger["triggerType"].as<uint8_t>() != static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER)) continue;
            const char* sourceId = trigger["eventId"].as<const char*>();
            if (!sourceId || !*sourceId) continue;
            for (size_t source = 0; source < count; ++source) {
                if (std::strcmp(ids[source], sourceId) == 0) completion.addEdge(source, target);
                for (size_t i = 0; i < MAX_ACTIONS_PER_RULE; ++i)
                    if (emitted[source][i] && std::strcmp(emitted[source][i].get(), sourceId) == 0)
                        completion.addEdge(source, target);
            }
        }
        return nullptr;
    }

    bool add(JsonVariantConst value) {
        if (count >= ResourceProfile::MAX_PERIPH_EXEC_RULES ||
            !textFits(value, ResourceProfile::MAX_RULE_ID_LEN, true)) return false;
        std::strcpy(ids[count++], value.as<const char*>());
        return true;
    }

    bool contains(JsonVariantConst value) const {
        if (!textFits(value, ResourceProfile::MAX_RULE_ID_LEN, true)) return false;
        for (size_t i = 0; i < count; ++i)
            if (std::strcmp(ids[i], value.as<const char*>()) == 0) return true;
        return false;
    }
};

inline const char* validateRuleReferences(JsonObjectConst rule, const RuleReferenceIndex& index) {
    for (JsonObjectConst action : rule["actions"].as<JsonArrayConst>()) {
        const uint8_t type = action["actionType"].as<uint8_t>();
        if ((type == static_cast<uint8_t>(ExecActionType::ACTION_ENABLE_EXEC_RULE) ||
             type == static_cast<uint8_t>(ExecActionType::ACTION_DISABLE_EXEC_RULE)) &&
            !index.contains(action["targetPeriphId"])) return "CONFIG_RULE_REFERENCE";
    }
    return nullptr;
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
        if (a.isNull() || !uintFits(a["actionType"], 40) || !uintFits(a["syncDelayMs"], UINT16_MAX) ||
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
        if (a.actionType > 40 || a.execMode > 1) return "CONFIG_ACTION_TYPE";
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
    // The document already owns every string. Borrow its values rather than
    // allocating the streaming index or duplicating event targets on the heap.
    RuleCompletionGraph graph(rules.size());
    size_t target = 0;
    for (JsonObjectConst rule : rules) {
        const bool enabled = rule["enabled"].isNull() || rule["enabled"].as<bool>();
        graph.setEnabled(target, enabled);
        if (enabled) for (JsonObjectConst action : rule["actions"].as<JsonArrayConst>())
            if (hasDynamicRuleEventTarget(action["actionType"].as<uint8_t>(),
                    action["targetPeriphId"].as<const char*>(), action["useReceivedValue"].as<bool>()))
                return "CONFIG_RULE_EVENT_DYNAMIC_TARGET";
        for (JsonObjectConst trigger : rule["triggers"].as<JsonArrayConst>()) {
            if (trigger["triggerType"].as<uint8_t>() != static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER)) continue;
            const char* eventId = trigger["eventId"].as<const char*>();
            if (!eventId || !*eventId) continue;
            size_t source = 0;
            for (JsonObjectConst emitter : rules) {
                const char* id = emitter["id"].as<const char*>();
                if (std::strcmp(id, eventId) == 0) graph.addEdge(source, target);
                for (JsonObjectConst action : emitter["actions"].as<JsonArrayConst>()) {
                    const char* emitted = fixedRuleActionEvent(action["actionType"].as<uint8_t>(),
                        action["targetPeriphId"].as<const char*>(), action["actionValue"].as<const char*>(),
                        action["useReceivedValue"].as<bool>(), id);
                    if (emitted && std::strcmp(emitted, eventId) == 0) graph.addEdge(source, target);
                }
                ++source;
            }
        }
        ++target;
    }
    return graph.error();
}

} // namespace PeriphExecConfig
} // namespace FastBee
