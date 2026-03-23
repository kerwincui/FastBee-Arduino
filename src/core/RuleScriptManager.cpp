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
    doc["version"] = 1;
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
        LOGGER.info("[RuleScript] No config file found, populating defaults");
        populateDefaults();
        saveConfiguration();
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

// ========== 工具 ==========

String RuleScriptManager::generateUniqueId() {
    return "rs_" + String(millis());
}

// ========== 默认规则脚本示例 ==========

void RuleScriptManager::populateDefaults() {
    // 示例1: MQTT上报 — 数组格式转对象格式
    {
        RuleScript r;
        r.id = "rs_mqtt_report";
        r.name = "MQTT上报:数组转对象";
        r.enabled = false;
        r.triggerType = 1;  // DATA_REPORT
        r.protocolType = 0; // MQTT
        r.scriptContent = "{\"temperature\": ${temperature}, \"humidity\": ${humidity}}";
        _rules[r.id] = r;
    }

    // 示例2: MQTT接收 — 对象格式转数组格式
    {
        RuleScript r;
        r.id = "rs_mqtt_recv";
        r.name = "MQTT接收:对象转数组";
        r.enabled = false;
        r.triggerType = 0;  // DATA_RECEIVE
        r.protocolType = 0; // MQTT
        r.scriptContent = "[{\"id\":\"temperature\",\"value\":\"${temperature}\",\"remark\":\"\"},{\"id\":\"humidity\",\"value\":\"${humidity}\",\"remark\":\"\"}]";
        _rules[r.id] = r;
    }

    // 示例3: Modbus RTU接收转换
    {
        RuleScript r;
        r.id = "rs_rtu_recv";
        r.name = "ModbusRTU接收转换";
        r.enabled = false;
        r.triggerType = 0;  // DATA_RECEIVE
        r.protocolType = 1; // MODBUS_RTU
        r.scriptContent = "[{\"id\":\"temperature\",\"value\":\"${temperature}\",\"remark\":\"\"},{\"id\":\"humidity\",\"value\":\"${humidity}\",\"remark\":\"\"}]";
        _rules[r.id] = r;
    }

    // 示例4: Modbus TCP接收转换
    {
        RuleScript r;
        r.id = "rs_tcpmod_recv";
        r.name = "ModbusTCP接收转换";
        r.enabled = false;
        r.triggerType = 0;  // DATA_RECEIVE
        r.protocolType = 2; // MODBUS_TCP
        r.scriptContent = "[{\"id\":\"temperature\",\"value\":\"${temperature}\",\"remark\":\"\"},{\"id\":\"humidity\",\"value\":\"${humidity}\",\"remark\":\"\"}]";
        _rules[r.id] = r;
    }

    // 示例5: HTTP上报 — 自定义JSON格式
    {
        RuleScript r;
        r.id = "rs_http_report";
        r.name = "HTTP上报:自定义格式";
        r.enabled = false;
        r.triggerType = 1;  // DATA_REPORT
        r.protocolType = 3; // HTTP
        r.scriptContent = "{\"device\":\"esp32\",\"temp\":${temperature},\"humi\":${humidity}}";
        _rules[r.id] = r;
    }

    // 示例6: CoAP接收转换
    {
        RuleScript r;
        r.id = "rs_coap_recv";
        r.name = "CoAP接收转换";
        r.enabled = false;
        r.triggerType = 0;  // DATA_RECEIVE
        r.protocolType = 4; // COAP
        r.scriptContent = "[{\"id\":\"temperature\",\"value\":\"${temperature}\",\"remark\":\"\"},{\"id\":\"humidity\",\"value\":\"${humidity}\",\"remark\":\"\"}]";
        _rules[r.id] = r;
    }

    // 示例7: TCP上报 — 精简文本格式
    {
        RuleScript r;
        r.id = "rs_tcp_report";
        r.name = "TCP上报:精简格式";
        r.enabled = false;
        r.triggerType = 1;  // DATA_REPORT
        r.protocolType = 5; // TCP
        r.scriptContent = "T:${temperature},H:${humidity}";
        _rules[r.id] = r;
    }

    LOGGER.infof("[RuleScript] Populated %d default rules", 7);
}
