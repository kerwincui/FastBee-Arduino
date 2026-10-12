#pragma once
#include <ArduinoJson.h>
#include "utils/CheckedText.h"
#include <map>

namespace FastBee {
constexpr size_t RuleScriptImportMaxBytes = 24 * 1024;
constexpr size_t RuleScriptImportMaxRules = 64;

template<class Text, class Rule>
bool parseRuleScriptFormChoices(const Text& enabled, const Text& trigger, const Text& protocol, Rule& output) {
    if (enabled != "true" && enabled != "false" && enabled != "0" && enabled != "1") return false;
    if ((trigger != "0" && trigger != "1") || (protocol != "0" && protocol != "1")) return false;
    output.enabled = enabled == "true" || enabled == "1";
    output.triggerType = trigger == "1" ? 1 : 0;
    output.protocolType = protocol == "1" ? 1 : 0;
    return true;
}

template<class Rule>
bool copyRuleScriptConfig(const Rule& source, Rule& output) {
    if (!copyCheckedText(output.id, source.id.c_str(), source.id.length(), 40) ||
        !copyCheckedText(output.name, source.name.c_str(), source.name.length(), 128) ||
        !copyCheckedText(output.scriptContent, source.scriptContent.c_str(), source.scriptContent.length(), 2048) ||
        !copyCheckedText(output.sourceTopic, source.sourceTopic.c_str(), source.sourceTopic.length(), 256) ||
        !copyCheckedText(output.targetTopic, source.targetTopic.c_str(), source.targetTopic.length(), 256)) return false;
    output.enabled = source.enabled;
    output.triggerType = source.triggerType;
    output.protocolType = source.protocolType;
    output.lastTriggerTime = source.lastTriggerTime;
    output.triggerCount = source.triggerCount;
    return true;
}

template<class Rule>
void appendRuleScriptConfig(JsonArray rules, const Rule& rule) {
    JsonObject obj = rules.add<JsonObject>();
    // Preserve length, including embedded NUL, so the common parser can reject
    // it. A c_str-only serialization could silently sanitize a corrupt input.
    obj["id"] = JsonString(rule.id.c_str(), rule.id.length());
    obj["name"] = JsonString(rule.name.c_str(), rule.name.length());
    obj["enabled"] = rule.enabled;
    obj["triggerType"] = rule.triggerType;
    obj["protocolType"] = rule.protocolType;
    obj["scriptContent"] = JsonString(rule.scriptContent.c_str(), rule.scriptContent.length());
    obj["sourceTopic"] = JsonString(rule.sourceTopic.c_str(), rule.sourceTopic.length());
    obj["targetTopic"] = JsonString(rule.targetTopic.c_str(), rule.targetTopic.length());
}

template<class Text>
bool ruleScriptConfigText(JsonVariantConst input, Text& output, size_t min, size_t max, bool optional = false) {
    if (optional && input.isNull()) { output = ""; return true; }
    if (!input.is<JsonString>()) return false;
    const auto value = input.as<JsonString>();
    return value.size() >= min && value.size() <= max &&
        copyCheckedText(output, value.c_str(), value.size(), max);
}

inline bool ruleScriptTopicPattern(const char* text, size_t size, bool publish) {
    for (size_t i = 0; i < size; ++i) {
        if (text[i] == '#' && (publish || i + 1 != size || (i && text[i - 1] != '/'))) return false;
        if (text[i] == '+' && (publish || (i && text[i - 1] != '/') || (i + 1 < size && text[i + 1] != '/'))) return false;
    }
    return true;
}

// Template uses the production RuleScript type in firmware and the same
// schema with instrumented strings in host fault-injection checks.
template<class Text, class Rule>
bool prepareRuleScriptMap(std::map<Text, Rule>&, size_t) { return true; }
template<class Map>
bool prepareRuleScriptMap(Map& map, size_t count) { return map.reserve(count); }

template<class Rule, class Text, class Map>
bool parseRuleScriptImport(JsonVariantConst root, Map& candidate, Text& error, bool legacyDefaults = false) {
    candidate.clear();
    const auto reject = [&](const char* message) { candidate.clear(); error = message; return false; };
    if (!root.is<JsonObjectConst>() || root.size() != 1 ||
        (!root["rules"].is<JsonArrayConst>() && !(legacyDefaults && root["rules"].isNull())))
        return reject("Expected an object containing only a rules array");
    for (JsonPairConst field : root.as<JsonObjectConst>()) {
        if (field.key().size() != 5 || std::memcmp(field.key().c_str(), "rules", 5))
            return reject("Invalid script configuration root field");
    }
    const auto rules = root["rules"].as<JsonArrayConst>();
    if (rules.size() > RuleScriptImportMaxRules) return reject("Too many script rules (maximum 64)");
    if (!prepareRuleScriptMap(candidate, rules.size())) return reject("Script node allocation failed; running rules retained");
    for (JsonVariantConst entry : rules) {
        if (!entry.is<JsonObjectConst>()) return reject("Script rule must be an object");
        for (JsonPairConst field : entry.as<JsonObjectConst>()) {
            const char* key = field.key().c_str();
            if (field.key().size() != std::strlen(key)) return reject("Invalid script field");
            if (std::strcmp(key, "id") && std::strcmp(key, "name") && std::strcmp(key, "enabled") &&
                std::strcmp(key, "triggerType") && std::strcmp(key, "protocolType") &&
                std::strcmp(key, "scriptContent") && std::strcmp(key, "sourceTopic") && std::strcmp(key, "targetTopic"))
                return reject("Unknown script rule field");
        }
        Rule rule;
        if (!ruleScriptConfigText(entry["id"], rule.id, 1, 40) ||
            !ruleScriptConfigText(entry["name"], rule.name, legacyDefaults ? 0 : 1, 128, legacyDefaults) ||
            !ruleScriptConfigText(entry["scriptContent"], rule.scriptContent, 0, 2048, legacyDefaults) ||
            !ruleScriptConfigText(entry["sourceTopic"], rule.sourceTopic, 0, 256, true) ||
            !ruleScriptConfigText(entry["targetTopic"], rule.targetTopic, 0, 256, true))
            return reject("Script text is invalid, too large, or could not be copied");
        if ((!entry["enabled"].is<bool>() && !(legacyDefaults && entry["enabled"].isNull())) ||
            (!entry["triggerType"].is<unsigned int>() && !(legacyDefaults && entry["triggerType"].isNull())) ||
            entry["triggerType"].as<unsigned int>() > 1 ||
            (!entry["protocolType"].is<unsigned int>() && !(legacyDefaults && entry["protocolType"].isNull())) ||
            entry["protocolType"].as<unsigned int>() > 1)
            return reject("Explicit enabled boolean and supported trigger/protocol integers are required");
        rule.enabled = entry["enabled"] | true;
        rule.triggerType = entry["triggerType"].as<unsigned int>();
        rule.protocolType = entry["protocolType"].as<unsigned int>();
        if (!legacyDefaults && rule.enabled && rule.scriptContent.isEmpty()) return reject("Enabled script must have content");
        if (!ruleScriptTopicPattern(rule.sourceTopic.c_str(), rule.sourceTopic.length(), false) ||
            !ruleScriptTopicPattern(rule.targetTopic.c_str(), rule.targetTopic.length(), true))
            return reject("Invalid source topic filter or publish target");
        Text key;
        if (!copyCheckedText(key, rule.id.c_str(), rule.id.length(), 40)) return reject("Script identity copy failed");
        rule.lastTriggerTime = 0;
        rule.triggerCount = 0;
        if (!candidate.emplace(std::move(key), std::move(rule)).second) return reject("Duplicate script rule identity");
    }
    return true;
}
} // namespace FastBee
