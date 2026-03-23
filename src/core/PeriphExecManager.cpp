#include "core/PeriphExecManager.h"
#include "core/FastBeeFramework.h"
#include "protocols/ProtocolManager.h"
#include "systems/LoggerSystem.h"

PeriphExecManager& PeriphExecManager::getInstance() {
    static PeriphExecManager instance;
    return instance;
}

bool PeriphExecManager::initialize() {
    bool loaded = loadConfiguration();
    LOGGER.infof("[PeriphExec] Initialized, loaded %d rules", (int)rules.size());
    return loaded;
}

// ========== CRUD ==========

bool PeriphExecManager::addRule(const PeriphExecRule& rule) {
    PeriphExecRule newRule = rule;
    if (newRule.id.isEmpty()) {
        newRule.id = generateUniqueId();
    }
    if (rules.count(newRule.id)) {
        LOGGER.warningf("[PeriphExec] Rule ID already exists: %s", newRule.id.c_str());
        return false;
    }
    newRule.lastTriggerTime = 0;
    newRule.triggerCount = 0;
    rules[newRule.id] = newRule;
    LOGGER.infof("[PeriphExec] Added rule: %s (%s)", newRule.id.c_str(), newRule.name.c_str());
    return true;
}

bool PeriphExecManager::updateRule(const String& id, const PeriphExecRule& rule) {
    auto it = rules.find(id);
    if (it == rules.end()) {
        LOGGER.warningf("[PeriphExec] Rule not found for update: %s", id.c_str());
        return false;
    }
    // 保留运行时字段
    unsigned long lastTrigger = it->second.lastTriggerTime;
    uint32_t count = it->second.triggerCount;

    PeriphExecRule updated = rule;
    updated.id = id;
    updated.lastTriggerTime = lastTrigger;
    updated.triggerCount = count;
    rules[id] = updated;
    LOGGER.infof("[PeriphExec] Updated rule: %s", id.c_str());
    return true;
}

bool PeriphExecManager::removeRule(const String& id) {
    auto it = rules.find(id);
    if (it == rules.end()) {
        return false;
    }
    rules.erase(it);
    LOGGER.infof("[PeriphExec] Removed rule: %s", id.c_str());
    return true;
}

PeriphExecRule* PeriphExecManager::getRule(const String& id) {
    auto it = rules.find(id);
    return (it != rules.end()) ? &it->second : nullptr;
}

std::vector<PeriphExecRule> PeriphExecManager::getAllRules() const {
    std::vector<PeriphExecRule> result;
    result.reserve(rules.size());
    for (const auto& pair : rules) {
        result.push_back(pair.second);
    }
    return result;
}

bool PeriphExecManager::enableRule(const String& id) {
    auto it = rules.find(id);
    if (it == rules.end()) return false;
    it->second.enabled = true;
    return true;
}

bool PeriphExecManager::disableRule(const String& id) {
    auto it = rules.find(id);
    if (it == rules.end()) return false;
    it->second.enabled = false;
    return true;
}

// ========== 持久化 ==========

bool PeriphExecManager::saveConfiguration() {
    JsonDocument doc;
    doc["version"] = 1;
    JsonArray arr = doc["rules"].to<JsonArray>();

    for (const auto& pair : rules) {
        const PeriphExecRule& r = pair.second;
        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = r.id;
        obj["name"] = r.name;
        obj["enabled"] = r.enabled;
        obj["triggerType"] = r.triggerType;

        // 设备触发
        obj["operatorType"] = r.operatorType;
        obj["compareValue"] = r.compareValue;

        // 定时触发
        obj["timerMode"] = r.timerMode;
        obj["intervalSec"] = r.intervalSec;
        obj["timePoint"] = r.timePoint;

        // 动作
        obj["targetPeriphId"] = r.targetPeriphId;
        obj["actionType"] = r.actionType;
        obj["actionValue"] = r.actionValue;
    }

    File file = LittleFS.open(PERIPH_EXEC_CONFIG_FILE, "w");
    if (!file) {
        LOGGER.error("[PeriphExec] Failed to open config file for writing");
        return false;
    }

    size_t written = serializeJson(doc, file);
    file.close();

    LOGGER.infof("[PeriphExec] Saved %d rules (%d bytes)", (int)rules.size(), (int)written);
    return written > 0;
}

bool PeriphExecManager::loadConfiguration() {
    if (!LittleFS.exists(PERIPH_EXEC_CONFIG_FILE)) {
        LOGGER.info("[PeriphExec] No config file found, starting empty");
        return true;
    }

    File file = LittleFS.open(PERIPH_EXEC_CONFIG_FILE, "r");
    if (!file) {
        LOGGER.error("[PeriphExec] Failed to open config file for reading");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        LOGGER.errorf("[PeriphExec] JSON parse error: %s", err.c_str());
        return false;
    }

    rules.clear();
    JsonArray arr = doc["rules"].as<JsonArray>();
    for (JsonObject obj : arr) {
        PeriphExecRule r;
        r.id = obj["id"].as<String>();
        r.name = obj["name"].as<String>();
        r.enabled = obj["enabled"] | true;
        r.triggerType = obj["triggerType"] | 0;

        r.operatorType = obj["operatorType"] | 0;
        r.compareValue = obj["compareValue"].as<String>();

        r.timerMode = obj["timerMode"] | 0;
        r.intervalSec = obj["intervalSec"] | 60;
        r.timePoint = obj["timePoint"].as<String>();

        r.targetPeriphId = obj["targetPeriphId"].as<String>();
        r.actionType = obj["actionType"] | 0;
        r.actionValue = obj["actionValue"].as<String>();

        // 向后兼容：旧版 inverted 字段迁移到新的 actionType
        bool oldInverted = obj["inverted"] | false;
        if (oldInverted) {
            if (r.actionType == static_cast<uint8_t>(ExecActionType::ACTION_HIGH)) {
                r.actionType = static_cast<uint8_t>(ExecActionType::ACTION_HIGH_INVERTED);
            } else if (r.actionType == static_cast<uint8_t>(ExecActionType::ACTION_LOW)) {
                r.actionType = static_cast<uint8_t>(ExecActionType::ACTION_LOW_INVERTED);
            }
        }
        r.inverted = false;  // 不再使用独立的 inverted 字段

        r.lastTriggerTime = 0;
        r.triggerCount = 0;

        if (!r.id.isEmpty()) {
            rules[r.id] = r;
        }
    }

    LOGGER.infof("[PeriphExec] Loaded %d rules", (int)rules.size());
    return true;
}

// ========== MQTT 匹配引擎 ==========

void PeriphExecManager::handleMqttMessage(const String& topic, const String& message) {
    if (rules.empty()) return;

    // 解析 JSON 数组: [{"id":"temperature","value":"27.43","remark":""}, ...]
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, message);
    if (err) return;  // 非 JSON 消息静默忽略

    JsonArray arr;
    if (doc.is<JsonArray>()) {
        arr = doc.as<JsonArray>();
    } else {
        return;  // 非数组格式忽略
    }

    for (JsonObject item : arr) {
        if (!item.containsKey("id") || !item.containsKey("value")) continue;

        String itemId = item["id"].as<String>();
        String itemValue = item["value"].as<String>();

        // 遍历所有启用的设备触发规则
        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled || rule.triggerType != 0) continue;

            // 防重复触发：同一规则最小间隔 1 秒
            if (rule.lastTriggerTime > 0 && (millis() - rule.lastTriggerTime) < 1000) continue;

            if (evaluateCondition(itemValue, rule.operatorType, rule.compareValue)) {
                LOGGER.infof("[PeriphExec] Rule '%s' matched: %s %s %s",
                    rule.name.c_str(), itemId.c_str(), itemValue.c_str(), rule.compareValue.c_str());
                executeAction(rule);
            }
        }
    }
}

// ========== 数据下发命令处理 ==========

String PeriphExecManager::handleDataCommand(const String& message) {
    JsonDocument cmdDoc;
    DeserializationError err = deserializeJson(cmdDoc, message);
    if (err || !cmdDoc.is<JsonArray>()) {
        LOGGER.warning("[PeriphExec] DataCommand: invalid JSON array");
        return "";
    }

    JsonArray cmdArr = cmdDoc.as<JsonArray>();

    // 构建响应 JSON 数组
    JsonDocument reportDoc;
    JsonArray reportArr = reportDoc.to<JsonArray>();

    for (JsonObject item : cmdArr) {
        if (!item.containsKey("id") || !item.containsKey("value")) continue;

        String itemId = item["id"].as<String>();
        String itemValue = item["value"].as<String>();

        bool matched = false;

        // 遍历所有规则，匹配 targetPeriphId == id 且为设备触发类型
        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled) continue;
            if (rule.triggerType != 0) continue;  // 排除定时触发
            if (rule.targetPeriphId != itemId) continue;

            matched = true;
            LOGGER.infof("[PeriphExec] DataCommand matched rule '%s' for id='%s' value='%s'",
                rule.name.c_str(), itemId.c_str(), itemValue.c_str());
            bool ok = executeAction(rule);

            JsonObject reportItem = reportArr.add<JsonObject>();
            reportItem["id"] = itemId;
            reportItem["value"] = itemValue;
            reportItem["remark"] = ok ? "success" : "execute failed";
        }

        // 没有匹配到任何规则时也记录到响应中
        if (!matched) {
            LOGGER.infof("[PeriphExec] DataCommand no matching rule for id='%s'", itemId.c_str());
            JsonObject reportItem = reportArr.add<JsonObject>();
            reportItem["id"] = itemId;
            reportItem["value"] = itemValue;
            reportItem["remark"] = "no matching rule";
        }
    }

    String result;
    serializeJson(reportDoc, result);
    return result;
}

// ========== 条件评估 ==========

bool PeriphExecManager::evaluateCondition(const String& value, uint8_t op, const String& compareValue) {
    ExecOperator oper = static_cast<ExecOperator>(op);

    // 字符串操作符
    if (oper == ExecOperator::CONTAIN) {
        return value.indexOf(compareValue) >= 0;
    }
    if (oper == ExecOperator::NOT_CONTAIN) {
        return value.indexOf(compareValue) < 0;
    }

    // 数值操作符
    float val = value.toFloat();
    float cmp = compareValue.toFloat();

    switch (oper) {
        case ExecOperator::EQ:  return val == cmp;
        case ExecOperator::NEQ: return val != cmp;
        case ExecOperator::GT:  return val > cmp;
        case ExecOperator::LT:  return val < cmp;
        case ExecOperator::GTE: return val >= cmp;
        case ExecOperator::LTE: return val <= cmp;
        case ExecOperator::BETWEEN:
        case ExecOperator::NOT_BETWEEN: {
            int commaIdx = compareValue.indexOf(',');
            if (commaIdx < 0) return false;
            float minVal = compareValue.substring(0, commaIdx).toFloat();
            float maxVal = compareValue.substring(commaIdx + 1).toFloat();
            bool inRange = (val >= minVal && val <= maxVal);
            return (oper == ExecOperator::BETWEEN) ? inRange : !inRange;
        }
        default: return false;
    }
}

// ========== 定时器引擎 ==========

void PeriphExecManager::checkTimers() {
    unsigned long now = millis();

    // 每秒检查一次
    if (now - lastTimerCheck < 1000) return;
    lastTimerCheck = now;

    for (auto& pair : rules) {
        PeriphExecRule& rule = pair.second;
        if (!rule.enabled || rule.triggerType != 1) continue;

        if (rule.timerMode == 0) {
            // 间隔模式
            if (rule.intervalSec == 0) continue;
            unsigned long intervalMs = (unsigned long)rule.intervalSec * 1000UL;
            if (rule.lastTriggerTime == 0 || (now - rule.lastTriggerTime) >= intervalMs) {
                LOGGER.infof("[PeriphExec] Timer interval triggered: '%s'", rule.name.c_str());
                executeAction(rule);
            }
        } else if (rule.timerMode == 1) {
            // 每日时间点模式 — 需要 NTP 同步
            struct tm timeinfo;
            if (!getLocalTime(&timeinfo, 0) || timeinfo.tm_year < 100) continue;

            // 解析 HH:MM
            if (rule.timePoint.length() < 5) continue;
            int colonIdx = rule.timePoint.indexOf(':');
            if (colonIdx < 0) continue;
            int targetHour = rule.timePoint.substring(0, colonIdx).toInt();
            int targetMin = rule.timePoint.substring(colonIdx + 1).toInt();

            if (timeinfo.tm_hour == targetHour && timeinfo.tm_min == targetMin) {
                // 防重复触发：距上次触发超过 60 秒
                if (rule.lastTriggerTime > 0 && (now - rule.lastTriggerTime) < 60000) continue;
                LOGGER.infof("[PeriphExec] Timer daily triggered: '%s' at %02d:%02d",
                    rule.name.c_str(), targetHour, targetMin);
                executeAction(rule);
            }
        }
    }
}

// ========== 动作执行 ==========

bool PeriphExecManager::executeAction(PeriphExecRule& rule) {
    rule.lastTriggerTime = millis();
    rule.triggerCount++;

    ExecActionType action = static_cast<ExecActionType>(rule.actionType);

    // 系统功能 (actionType 6-11)
    if (rule.actionType >= 6 && rule.actionType <= 11) {
        return executeSystemAction(rule);
    }

    // 命令脚本 (actionType 15)
    if (rule.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT)) {
        return executeScriptAction(rule);
    }

    // 外设动作
    return executePeripheralAction(rule);
}

bool PeriphExecManager::executePeripheralAction(const PeriphExecRule& rule) {
    PeripheralManager& pm = PeripheralManager::getInstance();

    if (rule.targetPeriphId.isEmpty()) {
        LOGGER.warning("[PeriphExec] No target peripheral specified");
        return false;
    }

    if (!pm.hasPeripheral(rule.targetPeriphId)) {
        LOGGER.warningf("[PeriphExec] Target peripheral not found: %s", rule.targetPeriphId.c_str());
        return false;
    }

    ExecActionType action = static_cast<ExecActionType>(rule.actionType);

    switch (action) {
        case ExecActionType::ACTION_HIGH: {
            LOGGER.infof("[PeriphExec] Execute HIGH on %s", rule.targetPeriphId.c_str());
            pm.stopActionTicker(rule.targetPeriphId);
            return pm.writePin(rule.targetPeriphId, GPIOState::STATE_HIGH);
        }

        case ExecActionType::ACTION_LOW: {
            LOGGER.infof("[PeriphExec] Execute LOW on %s", rule.targetPeriphId.c_str());
            pm.stopActionTicker(rule.targetPeriphId);
            return pm.writePin(rule.targetPeriphId, GPIOState::STATE_LOW);
        }

        case ExecActionType::ACTION_BLINK: {
            LOGGER.infof("[PeriphExec] Execute BLINK on %s", rule.targetPeriphId.c_str());
            uint16_t interval = rule.actionValue.isEmpty() ? 500 : rule.actionValue.toInt();
            pm.startActionTicker(rule.targetPeriphId, 1, interval);
            return true;
        }

        case ExecActionType::ACTION_BREATHE: {
            LOGGER.infof("[PeriphExec] Execute BREATHE on %s", rule.targetPeriphId.c_str());
            uint16_t speed = rule.actionValue.isEmpty() ? 2000 : rule.actionValue.toInt();
            pm.startActionTicker(rule.targetPeriphId, 2, speed);
            return true;
        }

        case ExecActionType::ACTION_SET_PWM: {
            uint32_t duty = rule.actionValue.isEmpty() ? 0 : rule.actionValue.toInt();
            LOGGER.infof("[PeriphExec] Execute SetPWM(%d) on %s", (int)duty, rule.targetPeriphId.c_str());
            return pm.writePWM(rule.targetPeriphId, duty);
        }

        case ExecActionType::ACTION_SET_DAC: {
            uint8_t dacVal = rule.actionValue.isEmpty() ? 0 : rule.actionValue.toInt();
            LOGGER.infof("[PeriphExec] Execute SetDAC(%d) on %s", (int)dacVal, rule.targetPeriphId.c_str());
            // DAC 通过 writePin 底层实现或直接 dacWrite
            PeripheralConfig* cfg = pm.getPeripheral(rule.targetPeriphId);
            if (!cfg) return false;
            uint8_t pin = cfg->getPrimaryPin();
            dacWrite(pin, dacVal);
            return true;
        }

        case ExecActionType::ACTION_HIGH_INVERTED: {
            LOGGER.infof("[PeriphExec] Execute HIGH(inverted) on %s", rule.targetPeriphId.c_str());
            pm.stopActionTicker(rule.targetPeriphId);
            return pm.writePin(rule.targetPeriphId, GPIOState::STATE_LOW);
        }

        case ExecActionType::ACTION_LOW_INVERTED: {
            LOGGER.infof("[PeriphExec] Execute LOW(inverted) on %s", rule.targetPeriphId.c_str());
            pm.stopActionTicker(rule.targetPeriphId);
            return pm.writePin(rule.targetPeriphId, GPIOState::STATE_HIGH);
        }

        default:
            LOGGER.warningf("[PeriphExec] Unknown peripheral action: %d", rule.actionType);
            return false;
    }
}

bool PeriphExecManager::executeSystemAction(const PeriphExecRule& rule) {
    ExecActionType action = static_cast<ExecActionType>(rule.actionType);

    switch (action) {
        case ExecActionType::ACTION_SYS_RESTART:
            LOGGER.info("[PeriphExec] Executing system restart...");
            delay(500);
            ESP.restart();
            return true;

        case ExecActionType::ACTION_SYS_FACTORY_RESET: {
            LOGGER.info("[PeriphExec] Executing factory reset...");
            const char* configFiles[] = {
                "/config/device.json", "/config/network.json", "/config/protocol.json",
                "/config/gpio.json", "/config/users.json", "/config/system.json",
                "/config/http.json", "/config/mqtt.json", "/config/tcp.json",
                "/config/modbus.json", "/config/coap.json", PERIPH_EXEC_CONFIG_FILE
            };
            for (int i = 0; i < (int)(sizeof(configFiles) / sizeof(configFiles[0])); i++) {
                if (LittleFS.exists(configFiles[i])) {
                    LittleFS.remove(configFiles[i]);
                }
            }
            delay(500);
            ESP.restart();
            return true;
        }

        case ExecActionType::ACTION_SYS_NTP_SYNC: {
            LOGGER.info("[PeriphExec] Executing NTP sync...");
            // 使用标准 NTP 配置
            configTzTime("CST-8", "cn.pool.ntp.org", "time.nist.gov");
            return true;
        }

        case ExecActionType::ACTION_SYS_OTA:
            LOGGER.info("[PeriphExec] OTA action - reserved for future implementation");
            return true;

        case ExecActionType::ACTION_SYS_AP_PROVISION:
            LOGGER.info("[PeriphExec] AP provision action - reserved for future implementation");
            return true;

        case ExecActionType::ACTION_SYS_BLE_PROVISION:
            LOGGER.info("[PeriphExec] BLE provision action - reserved for future implementation");
            return true;

        default:
            LOGGER.warningf("[PeriphExec] Unknown system action: %d", rule.actionType);
            return false;
    }
}

// ========== 脚本执行 ==========

bool PeriphExecManager::executeScriptAction(const PeriphExecRule& rule) {
    if (rule.actionValue.isEmpty()) {
        LOGGER.warning("[PeriphExec] Script is empty");
        return false;
    }

    auto cmds = ScriptEngine::parse(rule.actionValue);
    if (cmds.empty()) {
        LOGGER.warning("[PeriphExec] Script parse failed");
        return false;
    }

    String errMsg;
    if (!ScriptEngine::validate(cmds, errMsg)) {
        LOGGER.warningf("[PeriphExec] Script validation failed: %s", errMsg.c_str());
        return false;
    }

    LOGGER.infof("[PeriphExec] Executing script '%s' (%d commands)", rule.name.c_str(), cmds.size());

    // 获取 MQTTClient 指针供脚本 MQTT 命令使用
    MQTTClient* mqtt = nullptr;
    auto* fw = FastBeeFramework::getInstance();
    if (fw) {
        auto* pm = fw->getProtocolManager();
        if (pm) mqtt = pm->getMQTTClient();
    }

    return ScriptEngine::execute(cmds, mqtt);
}

// ========== 工具 ==========

String PeriphExecManager::generateUniqueId() {
    return "exec_" + String(millis());
}
