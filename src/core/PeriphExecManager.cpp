#include "core/PeriphExecManager.h"
#include "core/FastBeeFramework.h"
#include "protocols/ProtocolManager.h"
#include "systems/LoggerSystem.h"

PeriphExecManager& PeriphExecManager::getInstance() {
    static PeriphExecManager instance;
    return instance;
}

bool PeriphExecManager::initialize() {
    // 创建 FreeRTOS 同步原语
    _rulesMutex = xSemaphoreCreateMutex();
    _resultsMutex = xSemaphoreCreateMutex();
    _taskSlotSemaphore = xSemaphoreCreateCounting(MAX_ASYNC_TASKS, MAX_ASYNC_TASKS);

    bool loaded = loadConfiguration();
    LOGGER.infof("[PeriphExec] Initialized, loaded %d rules (async: max %d tasks)",
                 (int)rules.size(), MAX_ASYNC_TASKS);
    return loaded;
}

// ========== CRUD（带互斥量保护） ==========

bool PeriphExecManager::addRule(const PeriphExecRule& rule) {
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) return false;

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
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) return false;

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
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) return false;

    auto it = rules.find(id);
    if (it == rules.end()) {
        return false;
    }
    rules.erase(it);
    LOGGER.infof("[PeriphExec] Removed rule: %s", id.c_str());
    return true;
}

PeriphExecRule* PeriphExecManager::getRule(const String& id) {
    // 注意：返回指针，调用者需确保在主循环线程中使用
    auto it = rules.find(id);
    return (it != rules.end()) ? &it->second : nullptr;
}

std::vector<PeriphExecRule> PeriphExecManager::getAllRules() const {
    // 返回深拷贝，线程安全
    std::vector<PeriphExecRule> result;
    result.reserve(rules.size());
    for (const auto& pair : rules) {
        result.push_back(pair.second);
    }
    return result;
}

bool PeriphExecManager::enableRule(const String& id) {
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) return false;
    auto it = rules.find(id);
    if (it == rules.end()) return false;
    it->second.enabled = true;
    return true;
}

bool PeriphExecManager::disableRule(const String& id) {
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) return false;
    auto it = rules.find(id);
    if (it == rules.end()) return false;
    it->second.enabled = false;
    return true;
}

// ========== 持久化 ==========

bool PeriphExecManager::saveConfiguration() {
    MutexGuard lock(_rulesMutex);

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
        obj["execMode"] = r.execMode;

        // 设备触发
        obj["operatorType"] = r.operatorType;
        obj["compareValue"] = r.compareValue;
        obj["sourcePeriphId"] = r.sourcePeriphId;

        // 定时触发
        obj["timerMode"] = r.timerMode;
        obj["intervalSec"] = r.intervalSec;
        obj["timePoint"] = r.timePoint;

        // MQTT 主题触发
        obj["sourceTopicIndex"] = r.sourceTopicIndex;
        obj["targetTopicIndex"] = r.targetTopicIndex;
        obj["transformType"] = r.transformType;

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
        r.execMode = obj["execMode"] | 0;  // 默认异步

        r.operatorType = obj["operatorType"] | 0;
        r.compareValue = obj["compareValue"].as<String>();
        r.sourcePeriphId = obj["sourcePeriphId"].as<String>();

        r.timerMode = obj["timerMode"] | 0;
        r.intervalSec = obj["intervalSec"] | 60;
        r.timePoint = obj["timePoint"].as<String>();

        // MQTT 主题触发字段（向后兼容：旧配置无此字段时使用默认值）
        r.sourceTopicIndex = obj["sourceTopicIndex"] | (int8_t)-1;
        r.targetTopicIndex = obj["targetTopicIndex"] | (int8_t)-1;
        r.transformType = obj["transformType"] | 0;

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

// ========== MQTT 匹配引擎（异步分发） ==========

void PeriphExecManager::handleMqttMessage(const String& topic, const String& message) {
    // 解析 JSON 数组: [{"id":"temperature","value":"27.43","remark":""}, ...]
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, message);
    if (err) return;

    JsonArray arr;
    if (doc.is<JsonArray>()) {
        arr = doc.as<JsonArray>();
    } else {
        return;
    }

    // 阶段1: 持锁匹配，收集需要执行的规则副本
    std::vector<PeriphExecRule> matchedRules;

    {
        MutexGuard lock(_rulesMutex);
        if (rules.empty()) return;

        for (JsonObject item : arr) {
            if (!item.containsKey("id") || !item.containsKey("value")) continue;

            String itemId = item["id"].as<String>();
            String itemValue = item["value"].as<String>();

            for (auto& pair : rules) {
                PeriphExecRule& rule = pair.second;
                if (!rule.enabled || rule.triggerType != 0) continue;

                // 匹配外设ID：规则的 targetPeriphId 必须与消息中的 itemId 一致
                if (!rule.targetPeriphId.isEmpty() && rule.targetPeriphId != itemId) continue;

                // 防重复触发：同一规则最小间隔 1 秒
                if (rule.lastTriggerTime > 0 && (millis() - rule.lastTriggerTime) < 1000) continue;

                if (evaluateCondition(itemValue, rule.operatorType, rule.compareValue)) {
                    LOGGER.infof("[PeriphExec] Rule '%s' matched: %s %s %s",
                        rule.name.c_str(), itemId.c_str(), itemValue.c_str(), rule.compareValue.c_str());

                    // 更新原始规则的运行时字段
                    rule.lastTriggerTime = millis();
                    rule.triggerCount++;

                    // 深拷贝规则用于异步执行
                    matchedRules.push_back(rule);
                }
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁异步分发
    for (const auto& ruleCopy : matchedRules) {
        dispatchAsync(ruleCopy);
    }
}

// ========== 数据下发命令处理（始终同步） ==========

String PeriphExecManager::handleDataCommand(const String& message) {
    JsonDocument cmdDoc;
    DeserializationError err = deserializeJson(cmdDoc, message);
    if (err || !cmdDoc.is<JsonArray>()) {
        LOGGER.warning("[PeriphExec] DataCommand: invalid JSON array");
        return "";
    }

    JsonArray cmdArr = cmdDoc.as<JsonArray>();

    // 预处理阶段：处理 modbus_read 指令（阻塞操作，必须在持锁之前完成）
    JsonDocument modbusReportDoc;
    JsonArray modbusReportArr = modbusReportDoc.to<JsonArray>();
    std::vector<int> processedIndices;

    {
        int idx = 0;
        for (JsonObject item : cmdArr) {
            String itemId = item["id"].as<String>();
            if (itemId == "modbus_read") {
                String itemValue = item["value"].as<String>();
                LOGGER.infof("[PeriphExec] DataCommand: modbus_read command detected");
                if (_modbusReadCallback) {
                    String modbusResult = _modbusReadCallback(itemValue);
                    JsonDocument tmpDoc;
                    if (!deserializeJson(tmpDoc, modbusResult) && tmpDoc.is<JsonArray>()) {
                        for (JsonVariant v : tmpDoc.as<JsonArray>()) {
                            modbusReportArr.add(v);
                        }
                    }
                } else {
                    JsonObject errItem = modbusReportArr.add<JsonObject>();
                    errItem["id"] = "modbus_read";
                    errItem["value"] = "0";
                    errItem["remark"] = "error:not_initialized";
                }
                processedIndices.push_back(idx);
            }
            idx++;
        }
    }

    // 阶段1: 持锁匹配，收集规则副本
    struct MatchedItem {
        PeriphExecRule rule;
        String itemId;
        String itemValue;
    };
    std::vector<MatchedItem> matchedItems;
    std::vector<String> unmatchedIds;    // 未匹配的 item id
    std::vector<String> unmatchedValues; // 未匹配的 item value

    {
        MutexGuard lock(_rulesMutex);

        int itemIdx = 0;
        for (JsonObject item : cmdArr) {
            // 跳过已在预处理阶段处理的 modbus_read 项
            bool skip = false;
            for (int pi : processedIndices) { if (pi == itemIdx) { skip = true; break; } }
            itemIdx++;
            if (skip) continue;

            if (!item.containsKey("id") || !item.containsKey("value")) continue;

            String itemId = item["id"].as<String>();
            String itemValue = item["value"].as<String>();
            bool matched = false;

            for (auto& pair : rules) {
                PeriphExecRule& rule = pair.second;
                if (!rule.enabled) continue;
                if (rule.triggerType != 0) continue;
                if (rule.targetPeriphId != itemId) continue;

                // 评估条件：值必须满足运算符和比较值
                if (!evaluateCondition(itemValue, rule.operatorType, rule.compareValue)) continue;

                matched = true;
                rule.lastTriggerTime = millis();
                rule.triggerCount++;
                matchedItems.push_back({rule, itemId, itemValue});
            }

            if (!matched) {
                unmatchedIds.push_back(itemId);
                unmatchedValues.push_back(itemValue);
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁同步执行并构建响应
    JsonDocument reportDoc;
    JsonArray reportArr = reportDoc.to<JsonArray>();

    // 先添加 Modbus 读取结果
    for (JsonVariant v : modbusReportArr) {
        reportArr.add(v);
    }

    for (auto& mi : matchedItems) {
        LOGGER.infof("[PeriphExec] DataCommand matched rule '%s' for id='%s' value='%s'",
            mi.rule.name.c_str(), mi.itemId.c_str(), mi.itemValue.c_str());
        bool ok = executeAction(mi.rule);

        JsonObject reportItem = reportArr.add<JsonObject>();
        reportItem["id"] = mi.itemId;
        reportItem["value"] = mi.itemValue;
        reportItem["remark"] = ok ? "success" : "execute failed";
    }

    for (size_t i = 0; i < unmatchedIds.size(); i++) {
        LOGGER.infof("[PeriphExec] DataCommand no matching rule for id='%s'", unmatchedIds[i].c_str());
        JsonObject reportItem = reportArr.add<JsonObject>();
        reportItem["id"] = unmatchedIds[i];
        reportItem["value"] = unmatchedValues[i];
        reportItem["remark"] = "no matching rule";
    }

    String result;
    serializeJson(reportDoc, result);
    return result;
}

// ========== Modbus 读取回调注册 ==========

void PeriphExecManager::setModbusReadCallback(std::function<String(const String&)> callback) {
    _modbusReadCallback = callback;
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

// ========== 定时器引擎（异步分发） ==========

void PeriphExecManager::checkTimers() {
    unsigned long now = millis();

    // 每秒检查一次
    if (now - lastTimerCheck < 1000) return;
    lastTimerCheck = now;

    // 阶段1: 持锁匹配，收集规则副本
    std::vector<PeriphExecRule> triggeredRules;

    {
        MutexGuard lock(_rulesMutex);

        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled || rule.triggerType != 1) continue;

            bool shouldTrigger = false;

            if (rule.timerMode == 0) {
                // 间隔模式
                if (rule.intervalSec == 0) continue;
                unsigned long intervalMs = (unsigned long)rule.intervalSec * 1000UL;
                if (rule.lastTriggerTime == 0 || (now - rule.lastTriggerTime) >= intervalMs) {
                    shouldTrigger = true;
                }
            } else if (rule.timerMode == 1) {
                // 每日时间点模式
                struct tm timeinfo;
                if (!getLocalTime(&timeinfo, 0) || timeinfo.tm_year < 100) continue;

                if (rule.timePoint.length() < 5) continue;
                int colonIdx = rule.timePoint.indexOf(':');
                if (colonIdx < 0) continue;
                int targetHour = rule.timePoint.substring(0, colonIdx).toInt();
                int targetMin = rule.timePoint.substring(colonIdx + 1).toInt();

                if (timeinfo.tm_hour == targetHour && timeinfo.tm_min == targetMin) {
                    if (rule.lastTriggerTime > 0 && (now - rule.lastTriggerTime) < 60000) continue;
                    shouldTrigger = true;
                }
            }

            if (shouldTrigger) {
                LOGGER.infof("[PeriphExec] Timer triggered: '%s'", rule.name.c_str());
                rule.lastTriggerTime = now;
                rule.triggerCount++;
                triggeredRules.push_back(rule);
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁异步分发
    for (const auto& ruleCopy : triggeredRules) {
        dispatchAsync(ruleCopy);
    }
}

// ========== 设备触发检测（轮询输入外设状态） ==========

void PeriphExecManager::checkDeviceTriggers() {
    unsigned long now = millis();

    // 每 200ms 检查一次（比定时器更频繁，保证按键响应灵敏）
    if (now - _lastDeviceCheck < 200) return;
    _lastDeviceCheck = now;

    PeripheralManager& pm = PeripheralManager::getInstance();

    // 阶段1: 持锁匹配，收集规则副本
    std::vector<PeriphExecRule> triggeredRules;

    {
        MutexGuard lock(_rulesMutex);

        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled || rule.triggerType != 2) continue;  // 2=设备触发

            // 必须指定触发源外设
            if (rule.sourcePeriphId.isEmpty()) continue;

            // 防重复触发：同一规则最小间隔 500ms
            if (rule.lastTriggerTime > 0 && (now - rule.lastTriggerTime) < 500) continue;

            // 读取触发源外设当前值
            String currentValue;
            PeripheralConfig* periph = pm.getPeripheral(rule.sourcePeriphId);
            if (!periph) continue;

            PeripheralType pType = periph->type;
            int typeVal = static_cast<int>(pType);

            if (typeVal >= 11 && typeVal <= 14) {
                // 数字输入类型 (GPIO_DIGITAL_INPUT, OUTPUT, PULLUP, PULLDOWN)
                GPIOState state = pm.readPin(rule.sourcePeriphId);
                if (state == GPIOState::STATE_UNDEFINED) continue;
                currentValue = (state == GPIOState::STATE_HIGH) ? "1" : "0";
            } else if (typeVal == 15 || typeVal == 26) {
                // 模拟输入 (GPIO_ANALOG_INPUT=15) 或 ADC(26)
                uint16_t analog = pm.readAnalog(rule.sourcePeriphId);
                currentValue = String(analog);
            } else if (typeVal == 21) {
                // GPIO_TOUCH
                GPIOState state = pm.readPin(rule.sourcePeriphId);
                if (state == GPIOState::STATE_UNDEFINED) continue;
                currentValue = (state == GPIOState::STATE_HIGH) ? "1" : "0";
            } else {
                // 不支持的外设类型
                continue;
            }

            // 评估条件
            if (evaluateCondition(currentValue, rule.operatorType, rule.compareValue)) {
                LOGGER.infof("[PeriphExec] Device triggered: '%s' (source=%s, value=%s)",
                    rule.name.c_str(), rule.sourcePeriphId.c_str(), currentValue.c_str());
                rule.lastTriggerTime = now;
                rule.triggerCount++;
                triggeredRules.push_back(rule);
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁异步分发
    for (const auto& ruleCopy : triggeredRules) {
        dispatchAsync(ruleCopy);
    }
}

// ========== 动作执行（可在主循环或异步任务中调用） ==========

bool PeriphExecManager::executeAction(PeriphExecRule& rule) {
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

    MQTTClient* mqtt = getMqttClient();
    return ScriptEngine::execute(cmds, mqtt);
}

// ========== 异步调度引擎 ==========

bool PeriphExecManager::shouldRunAsync() const {
    // 检查可用堆内存
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_ASYNC) {
        return false;
    }
    // 检查是否有空闲任务槽（非阻塞 peek）
    if (uxSemaphoreGetCount(_taskSlotSemaphore) == 0) {
        return false;
    }
    return true;
}

void PeriphExecManager::dispatchAsync(const PeriphExecRule& rule) {
    // 系统重启/恢复出厂 必须同步执行（不能在子任务中执行后任务就被杀了）
    if (rule.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SYS_RESTART) ||
        rule.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SYS_FACTORY_RESET)) {
        LOGGER.infof("[PeriphExec] Sync execute (system action): '%s'", rule.name.c_str());
        PeriphExecRule mutableCopy = rule;
        executeAction(mutableCopy);
        return;
    }

    // 用户指定同步执行
    if (rule.execMode == static_cast<uint8_t>(ExecMode::EXEC_SYNC)) {
        LOGGER.infof("[PeriphExec] Sync execute (user config): '%s'", rule.name.c_str());
        PeriphExecRule mutableCopy = rule;
        executeAction(mutableCopy);
        return;
    }

    // 异步模式：判断资源是否充足
    if (!shouldRunAsync()) {
        LOGGER.infof("[PeriphExec] Fallback sync (heap=%d, slots=%d): '%s'",
                     (int)ESP.getFreeHeap(),
                     (int)uxSemaphoreGetCount(_taskSlotSemaphore),
                     rule.name.c_str());
        PeriphExecRule mutableCopy = rule;
        executeAction(mutableCopy);
        return;
    }

    // 获取一个任务槽（非阻塞）
    if (xSemaphoreTake(_taskSlotSemaphore, 0) != pdTRUE) {
        LOGGER.warningf("[PeriphExec] No async slot, fallback sync: '%s'", rule.name.c_str());
        PeriphExecRule mutableCopy = rule;
        executeAction(mutableCopy);
        return;
    }

    // 分配异步上下文（堆内存，任务结束后释放）
    AsyncExecContext* ctx = new (std::nothrow) AsyncExecContext();
    if (!ctx) {
        LOGGER.error("[PeriphExec] Failed to allocate async context");
        xSemaphoreGive(_taskSlotSemaphore);
        PeriphExecRule mutableCopy = rule;
        executeAction(mutableCopy);
        return;
    }

    ctx->ruleCopy = rule;
    ctx->manager = this;
    ctx->mqtt = getMqttClient();
    ctx->taskSlot = _taskSlotSemaphore;

    // 确定栈大小：脚本类 8KB，其他 4KB
    uint32_t stackSize = (rule.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT))
                         ? SCRIPT_TASK_STACK : SIMPLE_TASK_STACK;

    // 创建 FreeRTOS 任务
    char taskName[24];
    snprintf(taskName, sizeof(taskName), "exec_%.16s", rule.id.c_str());

    BaseType_t created = xTaskCreatePinnedToCore(
        asyncExecTaskFunc,      // 任务函数
        taskName,               // 任务名称
        stackSize,              // 栈大小
        ctx,                    // 参数
        ASYNC_TASK_PRIORITY,    // 优先级 0（低于主循环）
        nullptr,                // 不需要任务句柄
        1                       // 运行在 Core 1（与主循环相同）
    );

    if (created != pdPASS) {
        LOGGER.errorf("[PeriphExec] xTaskCreate failed, fallback sync: '%s'", rule.name.c_str());
        delete ctx;
        xSemaphoreGive(_taskSlotSemaphore);
        PeriphExecRule mutableCopy = rule;
        executeAction(mutableCopy);
        return;
    }

    LOGGER.infof("[PeriphExec] Async dispatched: '%s' (stack=%d, heap=%d)",
                 rule.name.c_str(), (int)stackSize, (int)ESP.getFreeHeap());
}

// FreeRTOS 任务入口函数
void PeriphExecManager::asyncExecTaskFunc(void* pvParameters) {
    AsyncExecContext* ctx = static_cast<AsyncExecContext*>(pvParameters);
    if (!ctx) {
        vTaskDelete(nullptr);
        return;
    }

    AsyncExecResult result;
    result.ruleId = ctx->ruleCopy.id;
    result.ruleName = ctx->ruleCopy.name;
    result.startTime = millis();
    result.status = AsyncExecStatus::RUNNING;

    LOGGER.infof("[PeriphExec] Async task started: '%s'", result.ruleName.c_str());

    // 执行动作
    bool ok = false;
    PeriphExecRule& rule = ctx->ruleCopy;
    ExecActionType actionType = static_cast<ExecActionType>(rule.actionType);

    // 脚本执行
    if (rule.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT)) {
        if (!rule.actionValue.isEmpty()) {
            auto cmds = ScriptEngine::parse(rule.actionValue);
            if (!cmds.empty()) {
                String errMsg;
                if (ScriptEngine::validate(cmds, errMsg)) {
                    ok = ScriptEngine::execute(cmds, ctx->mqtt);
                } else {
                    LOGGER.warningf("[PeriphExec] Async script validation failed: %s", errMsg.c_str());
                }
            }
        }
    }
    // 系统动作（非重启类已在 dispatchAsync 过滤）
    else if (rule.actionType >= 6 && rule.actionType <= 11) {
        ok = ctx->manager->executeSystemAction(rule);
    }
    // 外设动作
    else {
        ok = ctx->manager->executePeripheralAction(rule);
    }

    result.endTime = millis();
    result.status = ok ? AsyncExecStatus::COMPLETED : AsyncExecStatus::FAILED;

    LOGGER.infof("[PeriphExec] Async task finished: '%s' %s (%lums)",
                 result.ruleName.c_str(),
                 ok ? "OK" : "FAILED",
                 result.endTime - result.startTime);

    // 记录执行结果
    ctx->manager->recordResult(result);

    // 释放任务槽
    xSemaphoreGive(ctx->taskSlot);

    // 释放上下文
    delete ctx;

    // 删除自身任务
    vTaskDelete(nullptr);
}

// ========== 异步执行结果管理 ==========

void PeriphExecManager::recordResult(const AsyncExecResult& result) {
    MutexGuard lock(_resultsMutex);
    if (!lock.isLocked()) return;

    executionResults.push_back(result);

    // 保留最近 MAX_ASYNC_TASKS * 2 条记录
    while (executionResults.size() > MAX_ASYNC_TASKS * 2) {
        executionResults.erase(executionResults.begin());
    }
}

std::vector<AsyncExecResult> PeriphExecManager::getRecentResults() {
    MutexGuard lock(_resultsMutex);
    return executionResults;
}

// ========== JSON 格式转换 ==========

String PeriphExecManager::convertFormat(const String& input, uint8_t transformType) {
    if (transformType == 0 || input.isEmpty()) return input;

    JsonDocument inDoc;
    DeserializationError err = deserializeJson(inDoc, input);
    if (err) return input;  // 解析失败，返回原样

    if (transformType == 1) {
        // Array → Object: [{"id":"temp","value":"27.43"}] → {"temp":27.43}
        if (!inDoc.is<JsonArray>()) return input;
        JsonDocument outDoc;
        for (JsonObject item : inDoc.as<JsonArray>()) {
            if (!item.containsKey("id") || !item.containsKey("value")) continue;
            String id = item["id"].as<String>();
            String value = item["value"].as<String>();
            // 尝试转为数字
            char* endPtr;
            double numVal = strtod(value.c_str(), &endPtr);
            if (*endPtr == '\0' && value.length() > 0) {
                outDoc[id] = numVal;
            } else {
                outDoc[id] = value;
            }
        }
        String result;
        serializeJson(outDoc, result);
        LOGGER.infof("[PeriphExec] Transform array→object: %d bytes → %d bytes",
                     input.length(), result.length());
        return result;
    }

    if (transformType == 2) {
        // Object → Array: {"temp":27.43} → [{"id":"temp","value":"27.43","remark":""}]
        if (!inDoc.is<JsonObject>()) return input;
        JsonDocument outDoc;
        JsonArray arr = outDoc.to<JsonArray>();
        for (JsonPair p : inDoc.as<JsonObject>()) {
            JsonObject item = arr.add<JsonObject>();
            item["id"] = p.key().c_str();
            item["value"] = p.value().as<String>();
            item["remark"] = "";
        }
        String result;
        serializeJson(outDoc, result);
        LOGGER.infof("[PeriphExec] Transform object→array: %d bytes → %d bytes",
                     input.length(), result.length());
        return result;
    }

    return input;  // 未知转换类型
}

// ========== 订阅主题触发处理 ==========

void PeriphExecManager::handleTopicTrigger(int8_t subTopicIndex, const String& message) {
    if (subTopicIndex < 0) return;

    // 阶段1: 持锁匹配，收集规则副本
    struct TopicMatch {
        PeriphExecRule rule;
        String convertedPayload;
    };
    std::vector<TopicMatch> matched;

    {
        MutexGuard lock(_rulesMutex);
        if (rules.empty()) return;

        unsigned long now = millis();
        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled || rule.triggerType != 3) continue;
            if (rule.sourceTopicIndex != subTopicIndex) continue;

            // 防重复触发：同一规则最小间隔 1 秒
            if (rule.lastTriggerTime > 0 && (now - rule.lastTriggerTime) < 1000) continue;

            rule.lastTriggerTime = now;
            rule.triggerCount++;

            // 格式转换
            String payload = convertFormat(message, rule.transformType);

            LOGGER.infof("[PeriphExec] Topic trigger '%s': sub[%d] transform=%d",
                         rule.name.c_str(), subTopicIndex, rule.transformType);

            matched.push_back({rule, payload});
        }
    }
    // 锁已释放

    // 阶段2: 无锁转发到目标发布主题
    if (matched.empty()) return;

    MQTTClient* mqtt = getMqttClient();
    if (!mqtt) return;

    for (auto& m : matched) {
        if (m.rule.targetTopicIndex >= 0) {
            bool ok = mqtt->publishToTopic((size_t)m.rule.targetTopicIndex, m.convertedPayload);
            LOGGER.infof("[PeriphExec] Topic forward '%s' → pub[%d]: %s",
                         m.rule.name.c_str(), m.rule.targetTopicIndex, ok ? "OK" : "FAIL");
        }
    }
}

// ========== 发布前格式转换拦截 ==========

String PeriphExecManager::applyOutputTransform(size_t pubTopicIndex, const String& payload) {
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) return payload;

    for (auto& pair : rules) {
        PeriphExecRule& rule = pair.second;
        if (!rule.enabled || rule.triggerType != 4) continue;
        if (rule.targetTopicIndex < 0 || (size_t)rule.targetTopicIndex != pubTopicIndex) continue;

        // 匹配成功：更新运行时字段
        rule.lastTriggerTime = millis();
        rule.triggerCount++;

        LOGGER.infof("[PeriphExec] Output transform '%s': pub[%d] type=%d",
                     rule.name.c_str(), (int)pubTopicIndex, rule.transformType);

        // 只取第一条匹配的规则（避免链式转换的复杂性）
        return convertFormat(payload, rule.transformType);
    }

    return payload;  // 无匹配规则
}

// ========== 工具 ==========

MQTTClient* PeriphExecManager::getMqttClient() {
    auto* fw = FastBeeFramework::getInstance();
    if (fw) {
        auto* pm = fw->getProtocolManager();
        if (pm) return pm->getMQTTClient();
    }
    return nullptr;
}

String PeriphExecManager::generateUniqueId() {
    return "exec_" + String(millis());
}
