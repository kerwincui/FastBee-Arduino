#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RULE_SCRIPT

#include "core/RuleScriptManager.h"
#include "core/AsyncExecTypes.h"
#include "systems/LoggerSystem.h"

RuleScriptManager& RuleScriptManager::getInstance() {
    static RuleScriptManager instance;
    return instance;
}

bool RuleScriptManager::initialize() {
    _mutex = xSemaphoreCreateMutex();
    bool loaded = loadConfiguration();
    LOGGER.infof("[RuleScript] Initialized, loaded %d rules", (int)_rules.size());
    return loaded;
}

// ========== CRUD（带互斥量保护） ==========

bool RuleScriptManager::addRule(const RuleScript& rule) {
    MutexGuard lock(_mutex);
    if (!lock.isLocked()) return false;

    RuleScript newRule = rule;
    if (newRule.id.isEmpty()) {
        newRule.id = generateUniqueId();
    }
    if (_rules.count(newRule.id)) {
        LOGGER.warningf("[RuleScript] Rule ID already exists: %s", newRule.id.c_str());
        return false;
    }
    newRule.lastTriggerTime = 0;
    newRule.triggerCount = 0;
    _rules[newRule.id] = newRule;
    LOGGER.infof("[RuleScript] Added rule: %s (%s)", newRule.id.c_str(), newRule.name.c_str());
    return true;
}

bool RuleScriptManager::updateRule(const String& id, const RuleScript& rule) {
    MutexGuard lock(_mutex);
    if (!lock.isLocked()) return false;

    auto it = _rules.find(id);
    if (it == _rules.end()) {
        LOGGER.warningf("[RuleScript] Rule not found for update: %s", id.c_str());
        return false;
    }
    unsigned long lastTrigger = it->second.lastTriggerTime;
    uint32_t count = it->second.triggerCount;

    RuleScript updated = rule;
    updated.id = id;
    updated.lastTriggerTime = lastTrigger;
    updated.triggerCount = count;
    _rules[id] = updated;
    LOGGER.infof("[RuleScript] Updated rule: %s", id.c_str());
    return true;
}

bool RuleScriptManager::removeRule(const String& id) {
    MutexGuard lock(_mutex);
    if (!lock.isLocked()) return false;

    auto it = _rules.find(id);
    if (it == _rules.end()) return false;
    _rules.erase(it);
    LOGGER.infof("[RuleScript] Removed rule: %s", id.c_str());
    return true;
}

RuleScript* RuleScriptManager::getRule(const String& id) {
    auto it = _rules.find(id);
    return (it != _rules.end()) ? &it->second : nullptr;
}

std::vector<RuleScript> RuleScriptManager::getAllRules() const {
    std::vector<RuleScript> result;
    result.reserve(_rules.size());
    for (const auto& pair : _rules) {
        result.push_back(pair.second);
    }
    return result;
}

bool RuleScriptManager::enableRule(const String& id) {
    MutexGuard lock(_mutex);
    if (!lock.isLocked()) return false;
    auto it = _rules.find(id);
    if (it == _rules.end()) return false;
    it->second.enabled = true;
    return true;
}

bool RuleScriptManager::disableRule(const String& id) {
    MutexGuard lock(_mutex);
    if (!lock.isLocked()) return false;
    auto it = _rules.find(id);
    if (it == _rules.end()) return false;
    it->second.enabled = false;
    return true;
}

// ========== 持久化 ==========

bool RuleScriptManager::saveConfiguration() {
    MutexGuard lock(_mutex);

    JsonDocument doc;
    JsonArray arr = doc["rules"].to<JsonArray>();

    for (const auto& pair : _rules) {
        const RuleScript& r = pair.second;
        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = r.id;
        obj["name"] = r.name;
        obj["enabled"] = r.enabled;
        obj["triggerType"] = r.triggerType;
        obj["protocolType"] = r.protocolType;
        obj["scriptContent"] = r.scriptContent;
        obj["sourceTopic"] = r.sourceTopic;
        obj["targetTopic"] = r.targetTopic;
    }

    File file = LittleFS.open(RULE_SCRIPT_CONFIG_FILE, "w");
    if (!file) {
        LOGGER.error("[RuleScript] Failed to open config file for writing");
        return false;
    }

    size_t written = serializeJson(doc, file);
    file.close();

    LOGGER.infof("[RuleScript] Saved %d rules (%d bytes)", (int)_rules.size(), (int)written);
    return written > 0;
}

bool RuleScriptManager::loadConfiguration() {
    if (!LittleFS.exists(RULE_SCRIPT_CONFIG_FILE)) {
        LOGGER.info("[RuleScript] No config file found, creating empty config");
        // 创建空配置文件，不预设任何默认规则
        JsonDocument doc;
        doc["rules"] = JsonArray();
        
        File file = LittleFS.open(RULE_SCRIPT_CONFIG_FILE, "w");
        if (!file) {
            LOGGER.error("[RuleScript] Failed to create config file");
            return false;
        }
        serializeJson(doc, file);
        file.close();
        LOGGER.info("[RuleScript] Created empty config file");
        return true;
    }

    File file = LittleFS.open(RULE_SCRIPT_CONFIG_FILE, "r");
    if (!file) {
        LOGGER.error("[RuleScript] Failed to open config file for reading");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        LOGGER.errorf("[RuleScript] JSON parse error: %s", err.c_str());
        return false;
    }

    _rules.clear();
    JsonArray arr = doc["rules"].as<JsonArray>();
    for (JsonObject obj : arr) {
        RuleScript r;
        r.id = obj["id"].as<String>();
        r.name = obj["name"].as<String>();
        r.enabled = obj["enabled"] | true;
        r.triggerType = obj["triggerType"] | 0;
        r.protocolType = obj["protocolType"] | 0;
        r.scriptContent = obj["scriptContent"].as<String>();
        r.sourceTopic = obj["sourceTopic"].as<String>();
        r.targetTopic = obj["targetTopic"].as<String>();
        r.lastTriggerTime = 0;
        r.triggerCount = 0;

        if (!r.id.isEmpty()) {
            _rules[r.id] = r;
        }
    }

    LOGGER.infof("[RuleScript] Loaded %d rules", (int)_rules.size());
    return true;
}

// ========== 模板引擎 ==========

String RuleScriptManager::applyTemplate(const String& templateStr, const String& jsonInput) {
    if (templateStr.isEmpty() || jsonInput.isEmpty()) return jsonInput;

    JsonDocument inDoc;
    DeserializationError err = deserializeJson(inDoc, jsonInput);
    if (err) return jsonInput;

    struct KV { String key; String value; };
    std::vector<KV> kvPairs;
    kvPairs.reserve(16);

    if (inDoc.is<JsonArray>()) {
        for (JsonObject item : inDoc.as<JsonArray>()) {
            if (!item.containsKey("id") || !item.containsKey("value")) continue;
            if (kvPairs.size() >= 32) break;
            kvPairs.push_back({item["id"].as<String>(), item["value"].as<String>()});
        }
    } else if (inDoc.is<JsonObject>()) {
        for (JsonPair p : inDoc.as<JsonObject>()) {
            if (kvPairs.size() >= 32) break;
            kvPairs.push_back({String(p.key().c_str()), p.value().as<String>()});
        }
    } else {
        return jsonInput;
    }

    if (kvPairs.empty()) return jsonInput;

    String result = templateStr;
    for (const auto& kv : kvPairs) {
        String placeholder = "${" + kv.key + "}";
        result.replace(placeholder, kv.value);
    }

    LOGGER.infof("[RuleScript] Template applied: %d vars, %d->%d bytes",
                 (int)kvPairs.size(), (int)jsonInput.length(), (int)result.length());
    return result;
}

// ========== 数据转换管道 ==========

String RuleScriptManager::applyReceiveTransform(uint8_t protocolType, const String& rawData) {
    String scriptCopy;
    {
        MutexGuard lock(_mutex);
        if (!lock.isLocked()) return rawData;

        for (auto& pair : _rules) {
            RuleScript& rule = pair.second;
            if (!rule.enabled || rule.triggerType != 0) continue;  // 0=DATA_RECEIVE
            if (rule.protocolType != protocolType) continue;
            if (rule.scriptContent.isEmpty()) continue;

            rule.lastTriggerTime = millis();
            rule.triggerCount++;
            scriptCopy = rule.scriptContent;
            break;
        }
    }

    if (scriptCopy.isEmpty()) return rawData;
    return applyTemplate(scriptCopy, rawData);
}

String RuleScriptManager::applyReportTransform(uint8_t protocolType, const String& rawData) {
    String scriptCopy;
    {
        MutexGuard lock(_mutex);
        if (!lock.isLocked()) return rawData;

        for (auto& pair : _rules) {
            RuleScript& rule = pair.second;
            if (!rule.enabled || rule.triggerType != 1) continue;  // 1=DATA_REPORT
            if (rule.protocolType != protocolType) continue;
            if (rule.scriptContent.isEmpty()) continue;

            rule.lastTriggerTime = millis();
            rule.triggerCount++;
            scriptCopy = rule.scriptContent;
            break;
        }
    }

    if (scriptCopy.isEmpty()) return rawData;
    return applyTemplate(scriptCopy, rawData);
}

// ========== MQTT 主题匹配 ==========

bool RuleScriptManager::matchTopic(const String& pattern, const String& topic) {
    // 空模式匹配所有主题
    if (pattern.isEmpty()) return true;
    // 完全相等快速路径
    if (pattern == topic) return true;
    // 没有通配符，不相等则不匹配
    if (pattern.indexOf('+') < 0 && pattern.indexOf('#') < 0) return false;

    // 按 / 分割逐级比较
    int pLen = pattern.length();
    int tLen = topic.length();
    int pi = 0, ti = 0;

    while (pi < pLen && ti < tLen) {
        // 提取 pattern 当前级
        int pSlash = pattern.indexOf('/', pi);
        if (pSlash < 0) pSlash = pLen;
        String pPart = pattern.substring(pi, pSlash);

        if (pPart == "#") {
            // # 匹配剩余所有级
            return true;
        }

        // 提取 topic 当前级
        int tSlash = topic.indexOf('/', ti);
        if (tSlash < 0) tSlash = tLen;
        String tPart = topic.substring(ti, tSlash);

        if (pPart != "+" && pPart != tPart) {
            return false;
        }

        pi = pSlash + 1;
        ti = tSlash + 1;
    }

    // pattern 末尾是 # 时匹配
    if (pi < pLen && pattern.substring(pi) == "#") return true;

    // 两边都结束才匹配
    return (pi >= pLen && ti >= tLen);
}

// ========== 主题感知接收转换 ==========

String RuleScriptManager::applyReceiveTransform(uint8_t protocolType, const String& rawData,
                                                 const String& topic, String* outTargetTopic) {
    String scriptCopy;
    String targetTopicCopy;
    {
        MutexGuard lock(_mutex);
        if (!lock.isLocked()) return rawData;

        for (auto& pair : _rules) {
            RuleScript& rule = pair.second;
            if (!rule.enabled || rule.triggerType != 0) continue;  // 0=DATA_RECEIVE
            if (rule.protocolType != protocolType) continue;
            if (rule.scriptContent.isEmpty()) continue;

            // 主题过滤：sourceTopic 为空时匹配所有
            if (!rule.sourceTopic.isEmpty() && !matchTopic(rule.sourceTopic, topic)) {
                continue;
            }

            rule.lastTriggerTime = millis();
            rule.triggerCount++;
            scriptCopy = rule.scriptContent;
            targetTopicCopy = rule.targetTopic;
            break;
        }
    }

    if (scriptCopy.isEmpty()) return rawData;

    String result = applyTemplate(scriptCopy, rawData);

    // 写入目标主题（调用方根据此值决定是否重定向发布）
    if (outTargetTopic && !targetTopicCopy.isEmpty()) {
        *outTargetTopic = targetTopicCopy;
    }

    return result;
}

// ========== 工具 ==========

String RuleScriptManager::generateUniqueId() {
    return "rs_" + String(millis());
}
#endif // FASTBEE_ENABLE_RULE_SCRIPT
