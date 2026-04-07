#include "core/PeriphExecManager.h"
#include "core/RuleScript.h"
#include "core/FastBeeFramework.h"
#include "protocols/ProtocolManager.h"
#include "protocols/MQTTClient.h"
#include "systems/LoggerSystem.h"
#include <WiFi.h>

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
    // 上限检查
    if (newRule.triggers.size() > MAX_TRIGGERS_PER_RULE) {
        LOGGER.warningf("[PeriphExec] Too many triggers: %d", (int)newRule.triggers.size());
        return false;
    }
    if (newRule.actions.size() > MAX_ACTIONS_PER_RULE) {
        LOGGER.warningf("[PeriphExec] Too many actions: %d", (int)newRule.actions.size());
        return false;
    }
    // 重置运行时字段
    for (auto& t : newRule.triggers) {
        t.lastTriggerTime = 0;
        t.triggerCount = 0;
    }
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
    // 上限检查
    if (rule.triggers.size() > MAX_TRIGGERS_PER_RULE) return false;
    if (rule.actions.size() > MAX_ACTIONS_PER_RULE) return false;

    // 保留 per-trigger 运行时字段：按索引匹配旧触发器
    const auto& oldTriggers = it->second.triggers;
    PeriphExecRule updated = rule;
    updated.id = id;
    for (size_t i = 0; i < updated.triggers.size(); i++) {
        if (i < oldTriggers.size()) {
            updated.triggers[i].lastTriggerTime = oldTriggers[i].lastTriggerTime;
            updated.triggers[i].triggerCount = oldTriggers[i].triggerCount;
        } else {
            updated.triggers[i].lastTriggerTime = 0;
            updated.triggers[i].triggerCount = 0;
        }
    }
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
    doc["version"] = 3;  // v3: triggers[]/actions[] 数组格式
    JsonArray arr = doc["rules"].to<JsonArray>();

    for (const auto& pair : rules) {
        const PeriphExecRule& r = pair.second;
        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = r.id;
        obj["name"] = r.name;
        obj["enabled"] = r.enabled;
        obj["execMode"] = r.execMode;

        // 触发器数组
        JsonArray trigArr = obj["triggers"].to<JsonArray>();
        for (const auto& t : r.triggers) {
            JsonObject tObj = trigArr.add<JsonObject>();
            tObj["triggerType"] = t.triggerType;
            tObj["triggerPeriphId"] = t.triggerPeriphId;
            tObj["operatorType"] = t.operatorType;
            tObj["compareValue"] = t.compareValue;
            tObj["timerMode"] = t.timerMode;
            tObj["intervalSec"] = t.intervalSec;
            tObj["timePoint"] = t.timePoint;
            tObj["eventId"] = t.eventId;
        }

        // 动作数组
        JsonArray actArr = obj["actions"].to<JsonArray>();
        for (const auto& a : r.actions) {
            JsonObject aObj = actArr.add<JsonObject>();
            aObj["targetPeriphId"] = a.targetPeriphId;
            aObj["actionType"] = a.actionType;
            aObj["actionValue"] = a.actionValue;
            aObj["useReceivedValue"] = a.useReceivedValue;
            aObj["syncDelayMs"] = a.syncDelayMs;
        }

        // 数据转换管道
        obj["protocolType"] = r.protocolType;
        obj["scriptContent"] = r.scriptContent;

        // 数据上报控制
        obj["reportAfterExec"] = r.reportAfterExec;
    }

    File file = LittleFS.open(PERIPH_EXEC_CONFIG_FILE, "w");
    if (!file) {
        LOGGER.error("[PeriphExec] Failed to open config file for writing");
        return false;
    }

    size_t written = serializeJson(doc, file);
    file.close();

    LOGGER.infof("[PeriphExec] Saved %d rules (%d bytes, v3)", (int)rules.size(), (int)written);
    return written > 0;
}

bool PeriphExecManager::loadConfiguration() {
    if (!LittleFS.exists(PERIPH_EXEC_CONFIG_FILE)) {
        LOGGER.info("[PeriphExec] No config file found, starting empty");
        saveConfiguration();
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

    int configVersion = doc["version"] | 1;
    rules.clear();
    JsonArray arr = doc["rules"].as<JsonArray>();

    for (JsonObject obj : arr) {
        PeriphExecRule r;
        r.id = obj["id"].as<String>();
        r.name = obj["name"].as<String>();
        r.enabled = obj["enabled"] | true;
        r.execMode = obj["execMode"] | 0;

        // 数据转换管道字段
        r.protocolType = obj["protocolType"] | 0;
        r.scriptContent = obj["scriptContent"].as<String>();
        r.reportAfterExec = obj["reportAfterExec"] | true;

        if (configVersion >= 3) {
            // ===== v3 原生格式：读取 triggers[] 和 actions[] 数组 =====
            JsonArray trigArr = obj["triggers"].as<JsonArray>();
            for (JsonObject tObj : trigArr) {
                ExecTrigger t;
                t.triggerType = tObj["triggerType"] | 0;
                t.triggerPeriphId = tObj["triggerPeriphId"].as<String>();
                t.operatorType = tObj["operatorType"] | 0;
                t.compareValue = tObj["compareValue"].as<String>();
                t.timerMode = tObj["timerMode"] | 0;
                t.intervalSec = tObj["intervalSec"] | 60;
                t.timePoint = tObj["timePoint"].as<String>();
                t.eventId = tObj["eventId"].as<String>();
                t.lastTriggerTime = 0;
                t.triggerCount = 0;
                r.triggers.push_back(t);
            }

            JsonArray actArr = obj["actions"].as<JsonArray>();
            for (JsonObject aObj : actArr) {
                ExecAction a;
                a.targetPeriphId = aObj["targetPeriphId"].as<String>();
                a.actionType = aObj["actionType"] | 0;
                a.actionValue = aObj["actionValue"].as<String>();
                a.useReceivedValue = aObj["useReceivedValue"] | false;
                a.syncDelayMs = aObj["syncDelayMs"] | 0;
                r.actions.push_back(a);
            }
        } else {
            // ===== v1/v2 旧格式迁移：平铺字段 → 单元素 triggers[]/actions[] =====
            ExecTrigger t;
            t.triggerType = obj["triggerType"] | 0;
            // v1/v2 无 triggerPeriphId，用 targetPeriphId 作为数据源（旧行为兼容）
            t.triggerPeriphId = obj["targetPeriphId"].as<String>();
            t.operatorType = obj["operatorType"] | 0;
            t.compareValue = obj["compareValue"].as<String>();
            t.timerMode = obj["timerMode"] | 0;
            t.intervalSec = obj["intervalSec"] | 60;
            t.timePoint = obj["timePoint"].as<String>();
            t.lastTriggerTime = 0;
            t.triggerCount = 0;

            // 事件 ID 迁移（v1→v2 逻辑）
            if (configVersion >= 2) {
                t.eventId = obj["eventId"].as<String>();
            } else {
                uint8_t oldTriggerType = obj["triggerType"] | 0;
                String oldSystemEventId = obj["systemEventId"].as<String>();
                if (oldTriggerType == 5 || oldTriggerType == 6) {
                    t.triggerType = static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER);
                    if (!oldSystemEventId.isEmpty()) {
                        t.eventId = oldSystemEventId;
                        if (t.eventId.startsWith("sys_")) {
                            t.eventId = t.eventId.substring(4);
                        }
                    }
                } else if (oldTriggerType == 2) {
                    t.triggerType = static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER);
                    t.eventId = "data_receive";
                } else if (oldTriggerType == 3) {
                    t.triggerType = static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER);
                    t.eventId = "data_report";
                }
            }

            r.triggers.push_back(t);

            // 动作迁移
            ExecAction a;
            a.targetPeriphId = obj["targetPeriphId"].as<String>();
            a.actionType = obj["actionType"] | 0;
            a.actionValue = obj["actionValue"].as<String>();
            a.useReceivedValue = false;
            a.syncDelayMs = 0;

            // 向后兼容：旧版 inverted 字段迁移到新的 actionType
            bool oldInverted = obj["inverted"] | false;
            if (oldInverted) {
                if (a.actionType == static_cast<uint8_t>(ExecActionType::ACTION_HIGH)) {
                    a.actionType = static_cast<uint8_t>(ExecActionType::ACTION_HIGH_INVERTED);
                } else if (a.actionType == static_cast<uint8_t>(ExecActionType::ACTION_LOW)) {
                    a.actionType = static_cast<uint8_t>(ExecActionType::ACTION_LOW_INVERTED);
                }
            }

            r.actions.push_back(a);
        }

        if (!r.id.isEmpty()) {
            rules[r.id] = r;
        }
    }

    LOGGER.infof("[PeriphExec] Loaded %d rules (config v%d)", (int)rules.size(), configVersion);

    // v1/v2 自动升级为 v3
    if (configVersion < 3) {
        LOGGER.info("[PeriphExec] Migrating config to v3...");
        saveConfiguration();
    }

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

    // 阶段1: 持锁匹配，收集需要执行的规则副本及其匹配的接收值
    struct MatchedRule {
        PeriphExecRule rule;
        String receivedValue;
    };
    std::vector<MatchedRule> matchedRules;

    {
        MutexGuard lock(_rulesMutex);
        if (rules.empty()) return;

        for (JsonObject item : arr) {
            if (!item.containsKey("id") || !item.containsKey("value")) continue;

            String itemId = item["id"].as<String>();
            String itemValue = item["value"].as<String>();

            for (auto& pair : rules) {
                PeriphExecRule& rule = pair.second;
                if (!rule.enabled) continue;

                // 遍历所有触发器（OR 关系）
                bool triggerMatched = false;
                for (auto& trigger : rule.triggers) {
                    if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::PLATFORM_TRIGGER)) continue;

                    // 匹配数据源外设 ID（必须配置且匹配，空/null 不匹配任何数据）
                    if (trigger.triggerPeriphId.isEmpty() || trigger.triggerPeriphId == "null") continue;
                    if (trigger.triggerPeriphId != itemId) continue;

                    // 防重复触发：同一触发器最小间隔 1 秒
                    if (trigger.lastTriggerTime > 0 && (millis() - trigger.lastTriggerTime) < 1000) continue;

                    if (evaluateCondition(itemValue, trigger.operatorType, trigger.compareValue)) {
                        LOGGER.infof("[PeriphExec] Rule '%s' triggered by %s=%s (cmp=%s)",
                            rule.name.c_str(), itemId.c_str(), itemValue.c_str(), trigger.compareValue.c_str());

                        trigger.lastTriggerTime = millis();
                        trigger.triggerCount++;
                        triggerMatched = true;
                        break;  // 一个触发器匹配即可
                    }
                }

                if (triggerMatched) {
                    matchedRules.push_back({rule, itemValue});
                }
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁异步分发
    for (const auto& matched : matchedRules) {
        dispatchAsync(matched.rule, matched.receivedValue);
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

                // 遍历触发器匹配数据源
                for (auto& trigger : rule.triggers) {
                    if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::PLATFORM_TRIGGER)) continue;
                    if (trigger.triggerPeriphId.isEmpty() || trigger.triggerPeriphId == "null") continue;
                    if (trigger.triggerPeriphId != itemId) continue;

                    if (!evaluateCondition(itemValue, trigger.operatorType, trigger.compareValue)) continue;

                    matched = true;
                    trigger.lastTriggerTime = millis();
                    trigger.triggerCount++;
                    matchedItems.push_back({rule, itemId, itemValue});
                    break;  // 一个触发器匹配即可
                }
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

        auto results = executeAllActions(mi.rule, mi.itemValue);

        // 构建响应：每个动作的结果
        for (const auto& ar : results) {
            JsonObject reportItem = reportArr.add<JsonObject>();
            reportItem["id"] = ar.targetPeriphId.isEmpty() ? mi.itemId : ar.targetPeriphId;
            reportItem["value"] = ar.actualValue.isEmpty() ? mi.itemValue : ar.actualValue;
            reportItem["remark"] = ar.success ? "success" : "execute failed";
        }
        if (results.empty()) {
            JsonObject reportItem = reportArr.add<JsonObject>();
            reportItem["id"] = mi.itemId;
            reportItem["value"] = mi.itemValue;
            reportItem["remark"] = "no actions";
        }
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
            if (!rule.enabled) continue;

            // 遍历触发器，寻找定时触发类型
            for (auto& trigger : rule.triggers) {
                if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER)) continue;

                bool shouldTrigger = false;

                if (trigger.timerMode == 0) {
                    // 间隔模式
                    if (trigger.intervalSec == 0) continue;
                    unsigned long intervalMs = (unsigned long)trigger.intervalSec * 1000UL;
                    if (trigger.lastTriggerTime == 0 || (now - trigger.lastTriggerTime) >= intervalMs) {
                        shouldTrigger = true;
                    }
                } else if (trigger.timerMode == 1) {
                    // 每日时间点模式
                    struct tm timeinfo;
                    if (!getLocalTime(&timeinfo, 0) || timeinfo.tm_year < 100) continue;

                    if (trigger.timePoint.length() < 5) continue;
                    int colonIdx = trigger.timePoint.indexOf(':');
                    if (colonIdx < 0) continue;
                    int targetHour = trigger.timePoint.substring(0, colonIdx).toInt();
                    int targetMin = trigger.timePoint.substring(colonIdx + 1).toInt();

                    if (timeinfo.tm_hour == targetHour && timeinfo.tm_min == targetMin) {
                        if (trigger.lastTriggerTime > 0 && (now - trigger.lastTriggerTime) < 60000) continue;
                        shouldTrigger = true;
                    }
                }

                if (shouldTrigger) {
                    LOGGER.infof("[PeriphExec] Timer triggered: '%s'", rule.name.c_str());
                    trigger.lastTriggerTime = now;
                    trigger.triggerCount++;
                    triggeredRules.push_back(rule);
                    break;  // 一个触发器匹配即可触发该规则
                }
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁异步分发（定时触发无接收值）
    for (const auto& ruleCopy : triggeredRules) {
        dispatchAsync(ruleCopy, "");
    }
}

// ========== 动作执行（可在主循环或异步任务中调用） ==========

bool PeriphExecManager::executeActionItem(const ExecAction& action, const String& effectiveValue) {
    // 系统功能 (actionType 6-11)
    if (action.actionType >= 6 && action.actionType <= 11) {
        return executeSystemAction(action);
    }
    // 命令脚本 (actionType 15)
    if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT)) {
        return executeScriptAction(action);
    }
    // 外设动作
    return executePeripheralAction(action, effectiveValue);
}

std::vector<ActionExecResult> PeriphExecManager::executeAllActions(
    const PeriphExecRule& rule, const String& receivedValue) {

    std::vector<ActionExecResult> results;
    results.reserve(rule.actions.size());

    for (const auto& action : rule.actions) {
        // 执行前延时
        if (action.syncDelayMs > 0) {
            delay(action.syncDelayMs > 10000 ? 10000 : action.syncDelayMs);
        }

        // 确定有效值：useReceivedValue 时用接收值，否则用 actionValue
        String effectiveValue = (action.useReceivedValue && !receivedValue.isEmpty())
                                ? receivedValue : action.actionValue;

        LOGGER.infof("[PeriphExec] Exec action: target=%s type=%d val=%s (useRecv=%d)",
            action.targetPeriphId.c_str(), action.actionType,
            effectiveValue.c_str(), action.useReceivedValue);

        bool ok = executeActionItem(action, effectiveValue);

        ActionExecResult ar;
        ar.targetPeriphId = action.targetPeriphId;
        ar.actualValue = effectiveValue;
        ar.success = ok;
        results.push_back(ar);
    }

    // 精准上报执行结果
    if (rule.reportAfterExec && !results.empty()) {
        reportActionResults(results);
    }

    return results;
}

bool PeriphExecManager::executePeripheralAction(const ExecAction& action, const String& effectiveValue) {
    PeripheralManager& pm = PeripheralManager::getInstance();

    if (action.targetPeriphId.isEmpty()) {
        LOGGER.warning("[PeriphExec] No target peripheral specified");
        return false;
    }

    if (!pm.hasPeripheral(action.targetPeriphId)) {
        LOGGER.warningf("[PeriphExec] Target peripheral not found: %s", action.targetPeriphId.c_str());
        return false;
    }

    ExecActionType actType = static_cast<ExecActionType>(action.actionType);

    switch (actType) {
        case ExecActionType::ACTION_HIGH: {
            LOGGER.infof("[PeriphExec] Execute HIGH on %s", action.targetPeriphId.c_str());
            pm.stopActionTicker(action.targetPeriphId);
            return pm.writePin(action.targetPeriphId, GPIOState::STATE_HIGH);
        }

        case ExecActionType::ACTION_LOW: {
            LOGGER.infof("[PeriphExec] Execute LOW on %s", action.targetPeriphId.c_str());
            pm.stopActionTicker(action.targetPeriphId);
            return pm.writePin(action.targetPeriphId, GPIOState::STATE_LOW);
        }

        case ExecActionType::ACTION_BLINK: {
            LOGGER.infof("[PeriphExec] Execute BLINK on %s", action.targetPeriphId.c_str());
            uint16_t interval = effectiveValue.isEmpty() ? 500 : effectiveValue.toInt();
            pm.startActionTicker(action.targetPeriphId, 1, interval);
            return true;
        }

        case ExecActionType::ACTION_BREATHE: {
            LOGGER.infof("[PeriphExec] Execute BREATHE on %s", action.targetPeriphId.c_str());
            uint16_t speed = effectiveValue.isEmpty() ? 2000 : effectiveValue.toInt();
            pm.startActionTicker(action.targetPeriphId, 2, speed);
            return true;
        }

        case ExecActionType::ACTION_SET_PWM: {
            uint32_t duty = effectiveValue.isEmpty() ? 0 : effectiveValue.toInt();
            LOGGER.infof("[PeriphExec] Execute SetPWM(%d) on %s", (int)duty, action.targetPeriphId.c_str());
            return pm.writePWM(action.targetPeriphId, duty);
        }

        case ExecActionType::ACTION_SET_DAC: {
            uint8_t dacVal = effectiveValue.isEmpty() ? 0 : effectiveValue.toInt();
            LOGGER.infof("[PeriphExec] Execute SetDAC(%d) on %s", (int)dacVal, action.targetPeriphId.c_str());
            PeripheralConfig* cfg = pm.getPeripheral(action.targetPeriphId);
            if (!cfg) return false;
            uint8_t pin = cfg->getPrimaryPin();
            dacWrite(pin, dacVal);
            return true;
        }

        case ExecActionType::ACTION_HIGH_INVERTED: {
            LOGGER.infof("[PeriphExec] Execute HIGH(inverted) on %s", action.targetPeriphId.c_str());
            pm.stopActionTicker(action.targetPeriphId);
            return pm.writePin(action.targetPeriphId, GPIOState::STATE_LOW);
        }

        case ExecActionType::ACTION_LOW_INVERTED: {
            LOGGER.infof("[PeriphExec] Execute LOW(inverted) on %s", action.targetPeriphId.c_str());
            pm.stopActionTicker(action.targetPeriphId);
            return pm.writePin(action.targetPeriphId, GPIOState::STATE_HIGH);
        }

        default:
            LOGGER.warningf("[PeriphExec] Unknown peripheral action: %d", action.actionType);
            return false;
    }
}

bool PeriphExecManager::executeSystemAction(const ExecAction& action) {
    ExecActionType actType = static_cast<ExecActionType>(action.actionType);

    switch (actType) {
        case ExecActionType::ACTION_SYS_RESTART:
            LOGGER.info("[PeriphExec] Executing system restart...");
            delay(500);
            ESP.restart();
            return true;

        case ExecActionType::ACTION_SYS_FACTORY_RESET: {
            LOGGER.info("[PeriphExec] Executing factory reset...");
            const char* configFiles[] = {
                "/config/device.json", "/config/network.json", "/config/protocol.json",
                "/config/users.json", "/config/system.json",
                "/config/http.json", "/config/mqtt.json", "/config/tcp.json",
                "/config/modbus.json", "/config/coap.json", PERIPH_EXEC_CONFIG_FILE,
                RULE_SCRIPT_CONFIG_FILE, "/config/peripherals.json", "/config/roles.json"
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
            LOGGER.warningf("[PeriphExec] Unknown system action: %d", action.actionType);
            return false;
    }
}

// ========== 脚本执行 ==========

bool PeriphExecManager::executeScriptAction(const ExecAction& action) {
    if (action.actionValue.isEmpty()) {
        LOGGER.warning("[PeriphExec] Script is empty");
        return false;
    }

    auto cmds = ScriptEngine::parse(action.actionValue);
    if (cmds.empty()) {
        LOGGER.warning("[PeriphExec] Script parse failed");
        return false;
    }

    String errMsg;
    if (!ScriptEngine::validate(cmds, errMsg)) {
        LOGGER.warningf("[PeriphExec] Script validation failed: %s", errMsg.c_str());
        return false;
    }

    LOGGER.infof("[PeriphExec] Executing script (%d commands)", cmds.size());

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

void PeriphExecManager::dispatchAsync(const PeriphExecRule& rule, const String& receivedValue) {
    // 检查是否有系统重启/恢复出厂动作 —— 必须同步执行
    for (const auto& a : rule.actions) {
        if (a.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SYS_RESTART) ||
            a.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SYS_FACTORY_RESET)) {
            LOGGER.infof("[PeriphExec] Sync execute (system action): '%s'", rule.name.c_str());
            executeAllActions(rule, receivedValue);
            return;
        }
    }

    // 用户指定同步执行
    if (rule.execMode == static_cast<uint8_t>(ExecMode::EXEC_SYNC)) {
        LOGGER.infof("[PeriphExec] Sync execute (user config): '%s'", rule.name.c_str());
        executeAllActions(rule, receivedValue);
        return;
    }

    // 异步模式：判断资源是否充足
    if (!shouldRunAsync()) {
        LOGGER.infof("[PeriphExec] Fallback sync (heap=%d, slots=%d): '%s'",
                     (int)ESP.getFreeHeap(),
                     (int)uxSemaphoreGetCount(_taskSlotSemaphore),
                     rule.name.c_str());
        executeAllActions(rule, receivedValue);
        return;
    }

    // 获取一个任务槽（非阻塞）
    if (xSemaphoreTake(_taskSlotSemaphore, 0) != pdTRUE) {
        LOGGER.warningf("[PeriphExec] No async slot, fallback sync: '%s'", rule.name.c_str());
        executeAllActions(rule, receivedValue);
        return;
    }

    // 分配异步上下文（堆内存，任务结束后释放）
    AsyncExecContext* ctx = new (std::nothrow) AsyncExecContext();
    if (!ctx) {
        LOGGER.error("[PeriphExec] Failed to allocate async context");
        xSemaphoreGive(_taskSlotSemaphore);
        executeAllActions(rule, receivedValue);
        return;
    }

    ctx->ruleCopy = rule;
    ctx->receivedValue = receivedValue;
    ctx->manager = this;
    ctx->mqtt = getMqttClient();
    ctx->taskSlot = _taskSlotSemaphore;

    // 确定栈大小：有脚本动作用 8KB，其他 4KB
    bool hasScript = false;
    for (const auto& a : rule.actions) {
        if (a.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT)) {
            hasScript = true;
            break;
        }
    }
    uint32_t stackSize = hasScript ? SCRIPT_TASK_STACK : SIMPLE_TASK_STACK;

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
        executeAllActions(rule, receivedValue);
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

    // 执行所有动作
    auto actionResults = ctx->manager->executeAllActions(ctx->ruleCopy, ctx->receivedValue);

    // 统计结果
    bool allOk = true;
    for (const auto& ar : actionResults) {
        if (!ar.success) { allOk = false; break; }
    }

    result.endTime = millis();
    result.status = allOk ? AsyncExecStatus::COMPLETED : AsyncExecStatus::FAILED;

    LOGGER.infof("[PeriphExec] Async task finished: '%s' %s (%lums)",
                 result.ruleName.c_str(),
                 allOk ? "OK" : "FAILED",
                 result.endTime - result.startTime);

    // 记录执行结果
    ctx->manager->recordResult(result);

    // 触发外设执行完成事件
    ctx->manager->triggerPeriphExecEvent(ctx->ruleCopy.id, allOk ? "success" : "failed");

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

// ========== 手动执行 ==========

bool PeriphExecManager::runOnce(const String& id) {
    PeriphExecRule* rule = getRule(id);
    if (!rule) {
        return false;
    }
    
    // 创建副本执行
    PeriphExecRule ruleCopy = *rule;
    auto results = executeAllActions(ruleCopy, "");
    for (const auto& ar : results) {
        if (ar.success) return true;
    }
    return false;
}

// ========== 触发事件 ==========

void PeriphExecManager::triggerEvent(EventType eventType, const String& eventData) {
    // 查找匹配的事件ID
    const char* targetEventId = nullptr;
    for (size_t i = 0; STATIC_EVENTS[i].id != nullptr; i++) {
        if (STATIC_EVENTS[i].type == eventType) {
            targetEventId = STATIC_EVENTS[i].id;
            break;
        }
    }
    
    if (!targetEventId) {
        LOGGER.warningf("[PeriphExec] Unknown event type: %d", (int)eventType);
        return;
    }
    
    LOGGER.infof("[PeriphExec] Event triggered: %s (data=%s)", targetEventId, eventData.c_str());
    
    // 阶段1: 持锁匹配，收集规则副本
    std::vector<PeriphExecRule> triggeredRules;
    
    {
        MutexGuard lock(_rulesMutex);
        
        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled) continue;

            // 遍历触发器匹配事件
            for (auto& trigger : rule.triggers) {
                if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER)) continue;
                if (trigger.eventId.isEmpty()) continue;
                if (trigger.eventId != targetEventId) continue;

                // 防重复触发：同一触发器最小间隔 1 秒
                unsigned long now = millis();
                if (trigger.lastTriggerTime > 0 && (now - trigger.lastTriggerTime) < 1000) continue;

                LOGGER.infof("[PeriphExec] Event rule matched: '%s' (event=%s)",
                    rule.name.c_str(), targetEventId);

                trigger.lastTriggerTime = now;
                trigger.triggerCount++;
                triggeredRules.push_back(rule);
                break;  // 一个触发器匹配即可
            }
        }
    }
    // 锁已释放
    
    // 阶段2: 无锁异步分发
    for (const auto& ruleCopy : triggeredRules) {
        dispatchAsync(ruleCopy, eventData);
    }
}

void PeriphExecManager::triggerEventById(const String& eventId, const String& eventData) {
    const EventDef* def = findStaticEvent(eventId.c_str());
    if (def) {
        triggerEvent(def->type, eventData);
    } else {
        // 检查是否是外设执行规则事件
        if (eventId.startsWith("exec_")) {
            triggerPeriphExecEvent(eventId, eventData);
        } else {
            LOGGER.warningf("[PeriphExec] Unknown event ID: %s", eventId.c_str());
        }
    }
}

void PeriphExecManager::triggerPeriphExecEvent(const String& ruleId, const String& eventData) {
    LOGGER.infof("[PeriphExec] Periph exec event triggered: %s", ruleId.c_str());
    
    // 阶段1: 持锁匹配，收集规则副本
    std::vector<PeriphExecRule> triggeredRules;
    
    {
        MutexGuard lock(_rulesMutex);
        
        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled) continue;

            for (auto& trigger : rule.triggers) {
                if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER)) continue;
                if (trigger.eventId.isEmpty()) continue;
                if (trigger.eventId != ruleId) continue;

                unsigned long now = millis();
                if (trigger.lastTriggerTime > 0 && (now - trigger.lastTriggerTime) < 500) continue;

                LOGGER.infof("[PeriphExec] Periph exec event rule matched: '%s' (triggerRule=%s)",
                    rule.name.c_str(), ruleId.c_str());

                trigger.lastTriggerTime = now;
                trigger.triggerCount++;
                triggeredRules.push_back(rule);
                break;
            }
        }
    }
    // 锁已释放
    
    // 阶段2: 无锁异步分发
    for (const auto& ruleCopy : triggeredRules) {
        dispatchAsync(ruleCopy, eventData);
    }
}

String PeriphExecManager::getStaticEventsJson() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    
    for (size_t i = 0; STATIC_EVENTS[i].id != nullptr; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = STATIC_EVENTS[i].id;
        obj["name"] = STATIC_EVENTS[i].name;
        obj["category"] = STATIC_EVENTS[i].category;
        obj["type"] = static_cast<uint8_t>(STATIC_EVENTS[i].type);
    }
    
    String result;
    serializeJson(doc, result);
    return result;
}

String PeriphExecManager::getDynamicEventsJson() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    
    // 添加外设执行规则作为动态事件
    MutexGuard lock(_rulesMutex);
    
    for (const auto& pair : rules) {
        const PeriphExecRule& rule = pair.second;
        if (!rule.enabled) continue;
        
        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = rule.id;
        obj["name"] = rule.name;
        obj["category"] = "外设执行";
        obj["type"] = static_cast<uint8_t>(EventType::EVENT_PERIPH_EXEC_COMPLETED);
        obj["isDynamic"] = true;
    }
    
    String result;
    serializeJson(doc, result);
    return result;
}

// ========== 精准执行结果上报 ==========

void PeriphExecManager::reportActionResults(const std::vector<ActionExecResult>& results) {
    if (results.empty()) return;

    // 构建上报数据：[{"id":"led1","value":"1","remark":"success"}, ...]
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (const auto& ar : results) {
        if (ar.targetPeriphId.isEmpty()) continue;
        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = ar.targetPeriphId;
        obj["value"] = ar.actualValue;
        obj["remark"] = ar.success ? "success" : "failed";
    }

    if (arr.size() == 0) return;

    String reportData;
    serializeJson(doc, reportData);

    // 通过 MQTT 上报
    MQTTClient* mqtt = getMqttClient();
    if (mqtt && mqtt->getIsConnected()) {
        bool ok = mqtt->publishReportData(reportData);
        LOGGER.infof("[PeriphExec] Report action results via MQTT: %s (%s)",
                     reportData.c_str(), ok ? "ok" : "failed");
    } else {
        LOGGER.debug("[PeriphExec] MQTT not connected, skip action results report");
    }
}

// ========== 动作执行后数据上报 ==========

// 协议连接状态标志位
#define PROTOCOL_MQTT_CONNECTED    (1 << 0)
#define PROTOCOL_MODBUS_CONNECTED  (1 << 1)
#define PROTOCOL_TCP_CONNECTED     (1 << 2)
#define PROTOCOL_HTTP_CONNECTED    (1 << 3)
#define PROTOCOL_COAP_CONNECTED    (1 << 4)

String PeriphExecManager::collectPeripheralData() {
    PeripheralManager& pm = PeripheralManager::getInstance();
    std::vector<PeripheralConfig> allPeriphs = pm.getAllPeripherals();
    
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    
    for (const auto& config : allPeriphs) {
        // 只收集已启用的外设
        if (!config.enabled) continue;
        
        // 只收集 GPIO 类型的外设状态
        int typeVal = static_cast<int>(config.type);
        if (typeVal < 11 || typeVal > 26) continue;  // 跳过非GPIO/ADC/DAC类型
        
        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = config.id;
        obj["name"] = config.name;
        obj["type"] = typeVal;
        
        // 获取运行时状态
        auto* runtimeState = pm.getRuntimeState(config.id);
        if (runtimeState) {
            obj["status"] = static_cast<int>(runtimeState->status);
        } else {
            obj["status"] = 0;
        }
        
        // 读取当前值
        if (typeVal >= 11 && typeVal <= 14) {
            // 数字输入/输出类型
            GPIOState state = pm.readPin(config.id);
            if (state != GPIOState::STATE_UNDEFINED) {
                obj["value"] = (state == GPIOState::STATE_HIGH) ? "1" : "0";
            }
        } else if (typeVal == 15 || typeVal == 26) {
            // 模拟输入或 ADC
            uint16_t analog = pm.readAnalog(config.id);
            obj["value"] = String(analog);
        }
    }
    
    String result;
    serializeJson(doc, result);
    return result;
}

bool PeriphExecManager::checkNetworkAndProtocolStatus(uint8_t& connectedProtocols) {
    connectedProtocols = 0;
    
    // 检查 WiFi 连接状态
    if (WiFi.status() != WL_CONNECTED) {
        LOGGER.debug("[PeriphExec] WiFi not connected, skip report");
        return false;
    }
    
    bool hasConnectedProtocol = false;
    
    // 获取 ProtocolManager
    auto* fw = FastBeeFramework::getInstance();
    if (!fw) {
        return false;
    }
    auto* protocolMgr = fw->getProtocolManager();
    if (!protocolMgr) {
        return false;
    }
    
    // 检查 MQTT 连接状态
    MQTTClient* mqtt = protocolMgr->getMQTTClient();
    if (mqtt && mqtt->getIsConnected()) {
        connectedProtocols |= PROTOCOL_MQTT_CONNECTED;
        hasConnectedProtocol = true;
    }
    
    // 检查 TCP 连接状态（通过状态字符串判断）
    String tcpStatus = protocolMgr->getProtocolStatus(ProtocolType::TCP);
    if (tcpStatus.indexOf("connected") >= 0 || tcpStatus.indexOf("listening") >= 0) {
        connectedProtocols |= PROTOCOL_TCP_CONNECTED;
        hasConnectedProtocol = true;
    }
    
    // 检查 Modbus 状态
    ModbusHandler* modbus = protocolMgr->getModbusHandler();
    if (modbus) {
        String modbusStatus = modbus->getStatus();
        if (modbusStatus.indexOf("running") >= 0 || modbusStatus.indexOf("initialized") >= 0) {
            connectedProtocols |= PROTOCOL_MODBUS_CONNECTED;
            hasConnectedProtocol = true;
        }
    }
    
    // 检查 HTTP 状态（通过状态字符串判断）
    String httpStatus = protocolMgr->getProtocolStatus(ProtocolType::HTTP);
    if (httpStatus.indexOf("initialized") >= 0 || httpStatus.indexOf("ready") >= 0) {
        connectedProtocols |= PROTOCOL_HTTP_CONNECTED;
        hasConnectedProtocol = true;
    }
    
    // 检查 CoAP 状态
    String coapStatus = protocolMgr->getProtocolStatus(ProtocolType::COAP);
    if (coapStatus.indexOf("initialized") >= 0 || coapStatus.indexOf("ready") >= 0) {
        connectedProtocols |= PROTOCOL_COAP_CONNECTED;
        hasConnectedProtocol = true;
    }
    
    return hasConnectedProtocol;
}

bool PeriphExecManager::tryReportDeviceData() {
    uint8_t connectedProtocols = 0;
    
    // 检查网络和协议连接状态
    if (!checkNetworkAndProtocolStatus(connectedProtocols)) {
        LOGGER.debug("[PeriphExec] No connected protocol, skip data report");
        return true;  // 无连接不算失败，只是跳过
    }
    
    // 收集外设数据
    String periphData = collectPeripheralData();
    if (periphData.isEmpty() || periphData == "[]") {
        LOGGER.debug("[PeriphExec] No peripheral data to report");
        return true;  // 无数据不算失败
    }
    
    // 优先通过 MQTT 上报
    if (connectedProtocols & PROTOCOL_MQTT_CONNECTED) {
        auto* fw = FastBeeFramework::getInstance();
        if (fw) {
            auto* protocolMgr = fw->getProtocolManager();
            if (protocolMgr) {
                MQTTClient* mqtt = protocolMgr->getMQTTClient();
                if (mqtt && mqtt->getIsConnected()) {
                    bool ok = mqtt->publishReportData(periphData);
                    if (ok) {
                        LOGGER.infof("[PeriphExec] Data reported via MQTT, size=%d", periphData.length());
                        return true;
                    }
                }
            }
        }
    }
    
    // 备选：通过 TCP 上报
    if (connectedProtocols & PROTOCOL_TCP_CONNECTED) {
        auto* fw = FastBeeFramework::getInstance();
        if (fw) {
            auto* protocolMgr = fw->getProtocolManager();
            if (protocolMgr) {
                bool ok = protocolMgr->sendData(ProtocolType::TCP, "", periphData);
                if (ok) {
                    LOGGER.infof("[PeriphExec] Data reported via TCP, size=%d", periphData.length());
                    return true;
                }
            }
        }
    }
    
    // 备选：通过 HTTP POST 上报
    if (connectedProtocols & PROTOCOL_HTTP_CONNECTED) {
        auto* fw = FastBeeFramework::getInstance();
        if (fw) {
            auto* protocolMgr = fw->getProtocolManager();
            if (protocolMgr) {
                bool ok = protocolMgr->sendData(ProtocolType::HTTP, "", periphData);
                if (ok) {
                    LOGGER.infof("[PeriphExec] Data reported via HTTP, size=%d", periphData.length());
                    return true;
                }
            }
        }
    }
    
    // 备选：通过 CoAP 上报
    if (connectedProtocols & PROTOCOL_COAP_CONNECTED) {
        auto* fw = FastBeeFramework::getInstance();
        if (fw) {
            auto* protocolMgr = fw->getProtocolManager();
            if (protocolMgr) {
                bool ok = protocolMgr->sendData(ProtocolType::COAP, "", periphData);
                if (ok) {
                    LOGGER.infof("[PeriphExec] Data reported via CoAP, size=%d", periphData.length());
                    return true;
                }
            }
        }
    }
    
    LOGGER.warning("[PeriphExec] Data report failed, no available protocol");
    return false;
}

// ========== 按键事件检测 ==========

void PeriphExecManager::checkButtonEvents() {
    unsigned long now = millis();
    
    // 每 20ms 检测一次按键状态（比设备触发更频繁）
    if (now - _lastButtonCheck < 20) return;
    _lastButtonCheck = now;
    
    PeripheralManager& pm = PeripheralManager::getInstance();
    std::vector<PeripheralConfig> allPeriphs = pm.getAllPeripherals();
    
    for (const auto& config : allPeriphs) {
        // 只处理支持按键事件的外设类型（上拉/下拉输入）
        if (!config.enabled) continue;
        if (!supportsButtonEvent(config.type)) continue;
        
        // 获取或创建按键状态
        if (buttonStates.find(config.id) == buttonStates.end()) {
            ButtonRuntimeState state;
            state.periphId = config.id;
            state.lastState = true;  // 上拉默认高电平
            state.currentState = true;
            buttonStates[config.id] = state;
        }
        
        ButtonRuntimeState& btnState = buttonStates[config.id];
        
        // 读取当前按键状态
        GPIOState gpioState = pm.readPin(config.id);
        if (gpioState == GPIOState::STATE_UNDEFINED) continue;
        
        bool currentLevel = (gpioState == GPIOState::STATE_HIGH);
        btnState.currentState = currentLevel;
        
        // 检测状态变化（消抖处理）
        if (currentLevel != btnState.lastState) {
            // 检查消抖时间
            if (now - btnState.lastChangeTime >= buttonConfig.debounceMs) {
                btnState.lastState = currentLevel;
                btnState.lastChangeTime = now;
                
                if (!currentLevel) {
                    // 按键按下（低电平）
                    btnState.pressStartTime = now;
                    
                    // 触发按键按下事件
                    triggerButtonEvent(config.id, EventType::EVENT_BUTTON_PRESS);
                    
                    LOGGER.debugf("[PeriphExec] Button PRESS: %s", config.id.c_str());
                } else {
                    // 按键释放（高电平）
                    unsigned long pressDuration = now - btnState.pressStartTime;
                    
                    // 触发按键释放事件
                    triggerButtonEvent(config.id, EventType::EVENT_BUTTON_RELEASE);
                    
                    // 判断是点击还是长按释放
                    if (pressDuration < buttonConfig.longPress2sMs) {
                        // 短按释放，计入点击计数
                        btnState.clickCount++;
                        btnState.lastClickTime = now;
                        
                        LOGGER.debugf("[PeriphExec] Button CLICK (%d): %s, duration=%lums", 
                            btnState.clickCount, config.id.c_str(), pressDuration);
                    }
                    
                    // 重置长按触发标记
                    btnState.longPress2sTriggered = false;
                    btnState.longPress5sTriggered = false;
                    btnState.longPress10sTriggered = false;
                }
            }
        } else {
            // 状态未变化，检查长按和双击
            if (!currentLevel) {
                // 按键持续按下，检查长按
                unsigned long pressDuration = now - btnState.pressStartTime;
                
                // 长按2秒
                if (!btnState.longPress2sTriggered && pressDuration >= buttonConfig.longPress2sMs) {
                    btnState.longPress2sTriggered = true;
                    triggerButtonEvent(config.id, EventType::EVENT_BUTTON_LONG_PRESS_2S);
                    LOGGER.infof("[PeriphExec] Button LONG_PRESS_2S: %s", config.id.c_str());
                }
                
                // 长按5秒
                if (!btnState.longPress5sTriggered && pressDuration >= buttonConfig.longPress5sMs) {
                    btnState.longPress5sTriggered = true;
                    triggerButtonEvent(config.id, EventType::EVENT_BUTTON_LONG_PRESS_5S);
                    LOGGER.infof("[PeriphExec] Button LONG_PRESS_5S: %s", config.id.c_str());
                }
                
                // 长按10秒
                if (!btnState.longPress10sTriggered && pressDuration >= buttonConfig.longPress10sMs) {
                    btnState.longPress10sTriggered = true;
                    triggerButtonEvent(config.id, EventType::EVENT_BUTTON_LONG_PRESS_10S);
                    LOGGER.infof("[PeriphExec] Button LONG_PRESS_10S: %s", config.id.c_str());
                }
            } else {
                // 按键释放状态，检查双击
                if (btnState.clickCount > 0) {
                    // 检查双击超时
                    if (now - btnState.lastClickTime >= buttonConfig.clickIntervalMs) {
                        // 双击超时，处理点击结果
                        if (btnState.clickCount == 2) {
                            // 双击
                            triggerButtonEvent(config.id, EventType::EVENT_BUTTON_DOUBLE_CLICK);
                            LOGGER.infof("[PeriphExec] Button DOUBLE_CLICK: %s", config.id.c_str());
                        } else if (btnState.clickCount == 1) {
                            // 单击
                            triggerButtonEvent(config.id, EventType::EVENT_BUTTON_CLICK);
                            LOGGER.infof("[PeriphExec] Button CLICK: %s", config.id.c_str());
                        }
                        // 重置点击计数
                        btnState.clickCount = 0;
                    }
                }
            }
        }
    }
}

void PeriphExecManager::triggerButtonEvent(const String& periphId, EventType eventType) {
    // 查找事件ID
    const char* targetEventId = nullptr;
    for (size_t i = 0; STATIC_EVENTS[i].id != nullptr; i++) {
        if (STATIC_EVENTS[i].type == eventType) {
            targetEventId = STATIC_EVENTS[i].id;
            break;
        }
    }
    
    if (!targetEventId) return;
    
    // 阶段1: 持锁匹配，收集规则副本
    std::vector<PeriphExecRule> triggeredRules;
    
    {
        MutexGuard lock(_rulesMutex);
        
        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled) continue;

            for (auto& trigger : rule.triggers) {
                if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER)) continue;
                if (trigger.eventId != targetEventId) continue;

                unsigned long now = millis();
                if (trigger.lastTriggerTime > 0 && (now - trigger.lastTriggerTime) < 100) continue;

                LOGGER.infof("[PeriphExec] Button event rule matched: '%s' (event=%s, periph=%s)",
                    rule.name.c_str(), targetEventId, periphId.c_str());

                trigger.lastTriggerTime = now;
                trigger.triggerCount++;
                triggeredRules.push_back(rule);
                break;
            }
        }
    }
    // 锁已释放
    
    // 阶段2: 无锁异步分发
    for (const auto& ruleCopy : triggeredRules) {
        dispatchAsync(ruleCopy, "");
    }
}

// ========== 配置辅助方法 ==========

String PeriphExecManager::getValidTriggerTypes() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    
    auto addTrigger = [&arr](uint8_t type, const char* name, const char* desc) {
        JsonObject obj = arr.add<JsonObject>();
        obj["type"] = type;
        obj["name"] = name;
        obj["description"] = desc;
    };
    
    // 平台触发 - MQTT指令下发
    addTrigger(static_cast<uint8_t>(ExecTriggerType::PLATFORM_TRIGGER), 
               "平台触发", "IoT平台通过MQTT下发指令时触发");
    
    // 定时触发
    addTrigger(static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER), 
               "定时触发", "按指定时间间隔或每日时间点触发");
    
    // 触发事件（合并了原系统事件、按键事件、设备触发、外设执行事件、数据事件）
    addTrigger(static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER),
               "事件触发", "WiFi/MQTT/按键/数据/外设执行等事件触发");
    
    String result;
    serializeJson(doc, result);
    return result;
}

String PeriphExecManager::getEventCategoriesJson() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    
    // 定义事件分类
    const char* categories[] = {"WiFi", "MQTT", "网络", "协议", "系统", "配网", "规则", "按键", "外设执行", "数据"};
    const char* descriptions[] = {
        "WiFi连接状态变化事件",
        "MQTT连接状态变化事件", 
        "网络模式切换事件",
        "协议启用事件",
        "系统服务事件",
        "配网过程事件",
        "规则引擎事件",
        "按键输入事件",
        "外设执行规则事件",
        "协议数据收发事件"
    };
    
    for (size_t i = 0; i < sizeof(categories) / sizeof(categories[0]); i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = categories[i];
        obj["description"] = descriptions[i];
    }
    
    String result;
    serializeJson(doc, result);
    return result;
}

String PeriphExecManager::getValidActionTypes(const String& periphId) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    
    PeripheralManager& pm = PeripheralManager::getInstance();
    PeripheralConfig* config = nullptr;
    
    if (!periphId.isEmpty()) {
        config = pm.getPeripheral(periphId);
    }
    
    auto addAction = [&arr](uint8_t type, const char* name, const char* category = "") {
        JsonObject obj = arr.add<JsonObject>();
        obj["type"] = type;
        obj["name"] = name;
        if (strlen(category) > 0) {
            obj["category"] = category;
        }
    };
    
    if (config) {
        PeripheralType pType = config->type;
        
        if (isInputType(pType)) {
            // 输入类型外设：不支持GPIO输出动作，只支持系统功能和脚本
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_RESTART), "系统重启", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_FACTORY_RESET), "恢复出厂设置", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_NTP_SYNC), "NTP时间同步", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "调用其他外设", "外设");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
        } else if (isOutputType(pType)) {
            // 输出类型外设：支持所有GPIO输出动作
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_HIGH), "设置高电平", "GPIO");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_LOW), "设置低电平", "GPIO");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_HIGH_INVERTED), "设置高电平(反转)", "GPIO");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_LOW_INVERTED), "设置低电平(反转)", "GPIO");
            
            // PWM输出
            if (pType == PeripheralType::GPIO_PWM_OUTPUT) {
                addAction(static_cast<uint8_t>(ExecActionType::ACTION_SET_PWM), "设置PWM占空比", "PWM");
                addAction(static_cast<uint8_t>(ExecActionType::ACTION_BLINK), "闪烁", "PWM");
                addAction(static_cast<uint8_t>(ExecActionType::ACTION_BREATHE), "呼吸灯", "PWM");
            }
            
            // DAC输出
            if (pType == PeripheralType::DAC || pType == PeripheralType::GPIO_ANALOG_OUTPUT) {
                addAction(static_cast<uint8_t>(ExecActionType::ACTION_SET_DAC), "设置DAC值", "DAC");
            }
            
            // 系统功能和脚本
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_RESTART), "系统重启", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_FACTORY_RESET), "恢复出厂设置", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_NTP_SYNC), "NTP时间同步", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "调用其他外设", "外设");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
        } else {
            // 其他类型外设：只支持系统功能和脚本
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_RESTART), "系统重启", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_FACTORY_RESET), "恢复出厂设置", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_NTP_SYNC), "NTP时间同步", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "调用其他外设", "外设");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
        }
    } else {
        // 未指定外设，返回所有动作类型
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_HIGH), "设置高电平", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_LOW), "设置低电平", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_BLINK), "闪烁", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_BREATHE), "呼吸灯", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SET_PWM), "设置PWM占空比", "PWM");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SET_DAC), "设置DAC值", "DAC");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_RESTART), "系统重启", "系统");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_FACTORY_RESET), "恢复出厂设置", "系统");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_NTP_SYNC), "NTP时间同步", "系统");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_OTA), "OTA升级", "系统");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_AP_PROVISION), "AP配网", "系统");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_BLE_PROVISION), "BLE配网", "系统");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "调用其他外设", "外设");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_HIGH_INVERTED), "设置高电平(反转)", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_LOW_INVERTED), "设置低电平(反转)", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
    }
    
    String result;
    serializeJson(doc, result);
    return result;
}
