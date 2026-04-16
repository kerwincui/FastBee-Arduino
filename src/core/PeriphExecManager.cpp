#include "core/PeriphExecManager.h"
#include "core/PeriphExecExecutor.h"
#include "core/PeriphExecScheduler.h"
#include "core/RuleScript.h"
#include "core/FastBeeFramework.h"
#include "core/ChipConfig.h"
#include "protocols/ProtocolManager.h"
#include "protocols/MQTTClient.h"
#include "systems/LoggerSystem.h"
#include "core/PeripheralManager.h"

namespace {
constexpr uint32_t PERIPH_EXEC_MIN_TIMER_INTERVAL_SEC = 1;
constexpr uint32_t PERIPH_EXEC_MAX_TIMER_INTERVAL_SEC = 86400UL;
constexpr uint16_t PERIPH_EXEC_MIN_POLL_TIMEOUT_MS = 100;
constexpr uint16_t PERIPH_EXEC_MAX_POLL_TIMEOUT_MS = 5000;
constexpr uint16_t PERIPH_EXEC_HEAVY_POLL_TIMEOUT_MS = 3000;
constexpr uint8_t PERIPH_EXEC_MAX_POLL_RETRIES = 3;
constexpr uint8_t PERIPH_EXEC_HEAVY_POLL_RETRIES = 2;
constexpr uint16_t PERIPH_EXEC_MIN_POLL_INTER_DELAY_MS = 20;
constexpr uint16_t PERIPH_EXEC_MAX_POLL_INTER_DELAY_MS = 1000;
constexpr uint16_t PERIPH_EXEC_HEAVY_POLL_INTER_DELAY_MS = 100;
constexpr unsigned long PERIPH_EXEC_POLL_TRIGGER_MIN_INTERVAL_MS = 1000;
constexpr unsigned long PERIPH_EXEC_HEAVY_POLL_TRIGGER_MIN_INTERVAL_MS = 2000;
constexpr unsigned long PERIPH_EXEC_MODBUS_POLL_INGRESS_MIN_INTERVAL_MS = 1000;
constexpr unsigned long PERIPH_EXEC_POLL_THROTTLE_LOG_INTERVAL_MS = 5000;

// 判断字符串是否为有效数值（整数或浮点数，含负数）
static bool isNumericString(const String& s) {
    if (s.isEmpty()) return false;
    bool hasDecimal = false;
    size_t start = 0;
    if (s.charAt(0) == '-' || s.charAt(0) == '+') {
        if (s.length() == 1) return false;
        start = 1;
    }
    for (size_t i = start; i < s.length(); i++) {
        char c = s.charAt(i);
        if (c == '.') {
            if (hasDecimal) return false;
            hasDecimal = true;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}
}

PeriphExecManager& PeriphExecManager::getInstance() {
    static PeriphExecManager instance;
    return instance;
}

bool PeriphExecManager::initialize() {
    // 创建 FreeRTOS 同步原语
    _rulesMutex = xSemaphoreCreateMutex();
    _resultsMutex = xSemaphoreCreateMutex();
    _taskSlotSemaphore = xSemaphoreCreateCounting(MAX_ASYNC_TASKS, MAX_ASYNC_TASKS);
    _runningRulesMutex = xSemaphoreCreateMutex();
    _pollIngressMutex = xSemaphoreCreateMutex();

    // 创建子模块
    _executor.reset(new PeriphExecExecutor());
    _executor->initialize(this);

    _scheduler.reset(new PeriphExecScheduler());
    _scheduler->initialize(this, _executor.get());

    bool loaded = loadConfiguration();
    LOGGER.infof("[PeriphExec] Initialized, loaded %d rules (async: max %d tasks)",
                 (int)rules.size(), MAX_ASYNC_TASKS);
    return loaded;
}

// ========== CRUD（带互斥量保护） ==========

bool PeriphExecManager::ruleNeedsModbus(const PeriphExecRule& rule) const {
    PeripheralManager& pm = PeripheralManager::getInstance();
    for (const auto& action : rule.actions) {
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_POLL) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_COIL_WRITE) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_REG_WRITE)) {
            return true;
        }
        // 检查目标外设是否为 Modbus 类型
        if (!action.targetPeriphId.isEmpty()) {
            const PeripheralConfig* cfg = pm.getPeripheral(action.targetPeriphId);
            if (cfg && cfg->isModbusPeripheral()) return true;
        }
    }
    return false;
}

bool PeriphExecManager::ruleHasPollCollectionAction(const PeriphExecRule& rule) const {
    for (const auto& action : rule.actions) {
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_POLL)) {
            return true;
        }
    }
    return false;
}

bool PeriphExecManager::shouldAvoidSyncFallback(const PeriphExecRule& rule) const {
    PeripheralManager& pm = PeripheralManager::getInstance();
    for (const auto& action : rule.actions) {
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_POLL) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_COIL_WRITE) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_REG_WRITE) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ)) {
            return true;
        }
        // 检查目标外设是否为 Modbus 类型（Modbus 操作需要异步执行避免阻塞）
        if (!action.targetPeriphId.isEmpty()) {
            const PeripheralConfig* cfg = pm.getPeripheral(action.targetPeriphId);
            if (cfg && cfg->isModbusPeripheral()) return true;
        }
    }
    return false;
}

void PeriphExecManager::sanitizeTriggerForSafety(ExecTrigger& trigger,
                                                 bool hasPollCollectionAction,
                                                 const String& ruleName) const {
    uint32_t originalIntervalSec = trigger.intervalSec;
    uint16_t originalTimeout = trigger.pollResponseTimeout;
    uint8_t originalRetries = trigger.pollMaxRetries;
    uint16_t originalInterDelay = trigger.pollInterPollDelay;

    if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER)) {
        if (trigger.intervalSec < PERIPH_EXEC_MIN_TIMER_INTERVAL_SEC) {
            trigger.intervalSec = PERIPH_EXEC_MIN_TIMER_INTERVAL_SEC;
        } else if (trigger.intervalSec > PERIPH_EXEC_MAX_TIMER_INTERVAL_SEC) {
            trigger.intervalSec = PERIPH_EXEC_MAX_TIMER_INTERVAL_SEC;
        }
    }

    if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER)) {
        if (trigger.pollResponseTimeout < PERIPH_EXEC_MIN_POLL_TIMEOUT_MS) {
            trigger.pollResponseTimeout = PERIPH_EXEC_MIN_POLL_TIMEOUT_MS;
        } else if (trigger.pollResponseTimeout > PERIPH_EXEC_MAX_POLL_TIMEOUT_MS) {
            trigger.pollResponseTimeout = PERIPH_EXEC_MAX_POLL_TIMEOUT_MS;
        }

        if (trigger.pollMaxRetries > PERIPH_EXEC_MAX_POLL_RETRIES) {
            trigger.pollMaxRetries = PERIPH_EXEC_MAX_POLL_RETRIES;
        }

        if (trigger.pollInterPollDelay < PERIPH_EXEC_MIN_POLL_INTER_DELAY_MS) {
            trigger.pollInterPollDelay = PERIPH_EXEC_MIN_POLL_INTER_DELAY_MS;
        } else if (trigger.pollInterPollDelay > PERIPH_EXEC_MAX_POLL_INTER_DELAY_MS) {
            trigger.pollInterPollDelay = PERIPH_EXEC_MAX_POLL_INTER_DELAY_MS;
        }

        if (hasPollCollectionAction) {
            if (trigger.pollResponseTimeout > PERIPH_EXEC_HEAVY_POLL_TIMEOUT_MS) {
                trigger.pollResponseTimeout = PERIPH_EXEC_HEAVY_POLL_TIMEOUT_MS;
            }
            if (trigger.pollMaxRetries > PERIPH_EXEC_HEAVY_POLL_RETRIES) {
                trigger.pollMaxRetries = PERIPH_EXEC_HEAVY_POLL_RETRIES;
            }
            if (trigger.pollInterPollDelay < PERIPH_EXEC_HEAVY_POLL_INTER_DELAY_MS) {
                trigger.pollInterPollDelay = PERIPH_EXEC_HEAVY_POLL_INTER_DELAY_MS;
            }
        }
    }

    if (originalIntervalSec != trigger.intervalSec ||
        originalTimeout != trigger.pollResponseTimeout ||
        originalRetries != trigger.pollMaxRetries ||
        originalInterDelay != trigger.pollInterPollDelay) {
        LOGGER.warningf("[PeriphExec] Sanitized trigger params for rule '%s' (type=%d, interval=%lu, timeout=%u, retries=%u, delay=%u)",
                        ruleName.c_str(),
                        trigger.triggerType,
                        static_cast<unsigned long>(trigger.intervalSec),
                        static_cast<unsigned int>(trigger.pollResponseTimeout),
                        static_cast<unsigned int>(trigger.pollMaxRetries),
                        static_cast<unsigned int>(trigger.pollInterPollDelay));
    }
}

void PeriphExecManager::sanitizeRuleForSafety(PeriphExecRule& rule) const {
    bool hasPollCollectionAction = ruleHasPollCollectionAction(rule);
    for (auto& trigger : rule.triggers) {
        sanitizeTriggerForSafety(trigger, hasPollCollectionAction, rule.name);
    }
}

unsigned long PeriphExecManager::getPollTriggerCooldownMs(const PeriphExecRule& rule, const String& source) const {
    if ((source == "modbus" || source == "modbus_poll") && ruleHasPollCollectionAction(rule)) {
        return PERIPH_EXEC_HEAVY_POLL_TRIGGER_MIN_INTERVAL_MS;
    }
    return PERIPH_EXEC_POLL_TRIGGER_MIN_INTERVAL_MS;
}

bool PeriphExecManager::shouldThrottlePollIngress(const String& source, unsigned long now) {
    if (source != "modbus_poll" || _pollIngressMutex == nullptr) {
        return false;
    }

    MutexGuard lock(_pollIngressMutex);
    if (!lock.isLocked()) {
        return false;
    }

    unsigned long& lastAccepted = _pollSourceLastAccepted[source];
    if (lastAccepted > 0 && (now - lastAccepted) < PERIPH_EXEC_MODBUS_POLL_INGRESS_MIN_INTERVAL_MS) {
        unsigned long& lastLog = _pollSourceLastThrottleLog[source];
        if (lastLog == 0 || (now - lastLog) >= PERIPH_EXEC_POLL_THROTTLE_LOG_INTERVAL_MS) {
            LOGGER.warningf("[PeriphExec] Throttle poll ingress from %s (%lums since last accept)",
                            source.c_str(),
                            now - lastAccepted);
            lastLog = now;
        }
        return true;
    }

    lastAccepted = now;
    return false;
}

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
    sanitizeRuleForSafety(newRule);
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
    sanitizeRuleForSafety(updated);
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
            // 轮询触发通信参数
            tObj["pollResponseTimeout"] = t.pollResponseTimeout;
            tObj["pollMaxRetries"] = t.pollMaxRetries;
            tObj["pollInterPollDelay"] = t.pollInterPollDelay;
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
            aObj["execMode"] = a.execMode;
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
                // 轮询触发通信参数
                t.pollResponseTimeout = tObj["pollResponseTimeout"] | 1000;
                t.pollMaxRetries = tObj["pollMaxRetries"] | 2;
                t.pollInterPollDelay = tObj["pollInterPollDelay"] | 100;
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
                a.execMode = aObj["execMode"] | 0;
                r.actions.push_back(a);
            }
            // 向后兼容：旧配置中 execMode 在规则级别，迁移到每个动作
            if (r.execMode != 0) {
                bool anyActionHasMode = false;
                for (const auto& a : r.actions) {
                    if (a.execMode != 0) { anyActionHasMode = true; break; }
                }
                if (!anyActionHasMode) {
                    for (auto& a : r.actions) {
                        a.execMode = r.execMode;
                    }
                }
            }
            sanitizeRuleForSafety(r);
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
            t.lastTriggerTime = millis();  // 初始化为当前时间，避免启动时立即触发
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
            sanitizeRuleForSafety(r);
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

// ========== 委托给 Scheduler 的方法 ==========

void PeriphExecManager::handleMqttMessage(const String& topic, const String& message) {
    if (_scheduler) _scheduler->handleMqttMessage(topic, message);
}

void PeriphExecManager::handlePollData(const String& source, const String& data) {
    unsigned long now = millis();
    if (shouldThrottlePollIngress(source, now)) {
        return;
    }
    if (_scheduler) _scheduler->handlePollData(source, data);
}

String PeriphExecManager::handleDataCommand(const String& message) {
    if (_scheduler) return _scheduler->handleDataCommand(message);
    return "";
}

void PeriphExecManager::checkTimers() {
    if (_scheduler) _scheduler->checkTimers();
}

void PeriphExecManager::triggerEvent(EventType eventType, const String& eventData) {
    if (_scheduler) _scheduler->triggerEvent(eventType, eventData);
}

void PeriphExecManager::triggerEventById(const String& eventId, const String& eventData) {
    if (_scheduler) _scheduler->triggerEventById(eventId, eventData);
}

String PeriphExecManager::getStaticEventsJson() {
    return PeriphExecScheduler::getStaticEventsJson();
}

String PeriphExecManager::getDynamicEventsJson() {
    if (_scheduler) return _scheduler->getDynamicEventsJson();
    return "[]";
}

void PeriphExecManager::setModbusReadCallback(std::function<String(const String&)> callback) {
    _modbusReadCallback = callback;
    if (_executor) _executor->setModbusReadCallback(callback);
}

void PeriphExecManager::checkButtonEvents() {
    if (_scheduler) _scheduler->checkButtonEvents();
}

bool PeriphExecManager::tryReportDeviceData() {
    if (_scheduler) return _scheduler->tryReportDeviceData();
    return false;
}

String PeriphExecManager::getValidTriggerTypes() {
    return PeriphExecScheduler::getValidTriggerTypes();
}

String PeriphExecManager::getEventCategoriesJson() {
    return PeriphExecScheduler::getEventCategoriesJson();
}

String PeriphExecManager::getValidActionTypes(const String& periphId) {
    // 此静态方法需要访问 PeripheralManager，保留在 Manager 中
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

// ========== 异步执行结果管理 ==========

void PeriphExecManager::recordResult(const AsyncExecResult& result) {
    MutexGuard lock(_resultsMutex);
    if (!lock.isLocked()) return;

    // 堆守卫：低堆时跳过记录，防止 push_back 内部 new 失败导致 abort()
    if (ESP.getFreeHeap() < 10000) return;

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

// ========== 手动执行 ==========

bool PeriphExecManager::runOnce(const String& id) {
    PeriphExecRule* rule = getRule(id);
    if (!rule) {
        LOGGER.warning("[PeriphExec] Rule not found for runOnce: " + id);
        return false;
    }

    // 创建副本并异步分发，避免阻塞 Web 请求线程
    PeriphExecRule ruleCopy = *rule;
    LOGGER.infof("[PeriphExec] runOnce async dispatch: '%s' (id=%s)",
                 ruleCopy.name.c_str(), id.c_str());
    dispatchAsync(ruleCopy, "");
    return true;  // 返回 true 表示已提交
}

// ========== 异步调度 ==========

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

    // 用户指定同步执行（per-action 或 rule-level 向后兼容）
    bool hasSyncAction = false;
    for (const auto& a : rule.actions) {
        if (a.execMode == static_cast<uint8_t>(ExecMode::EXEC_SYNC)) {
            hasSyncAction = true;
            break;
        }
    }
    if (hasSyncAction || rule.execMode == static_cast<uint8_t>(ExecMode::EXEC_SYNC)) {
        LOGGER.infof("[PeriphExec] Sync execute (user config): '%s'", rule.name.c_str());
        executeAllActions(rule, receivedValue);
        return;
    }

    // 检查任务是否已在运行中（防止重复分发）
    {
        MutexGuard runLock(_runningRulesMutex);
        if (_runningRuleIds.count(rule.id) > 0) {
            LOGGER.warningf("[PeriphExec] Skip dispatch: task '%s' already running", rule.name.c_str());
            return;
        }
    }

    // 异步模式：判断资源是否充足
    if (!shouldRunAsync()) {
        if (shouldAvoidSyncFallback(rule)) {
            LOGGER.warningf("[PeriphExec] Skip heavy rule sync fallback (heap=%d, slots=%d): '%s'",
                            (int)ESP.getFreeHeap(),
                            (int)uxSemaphoreGetCount(_taskSlotSemaphore),
                            rule.name.c_str());
            {
                MutexGuard runLock(_runningRulesMutex);
                _failureBackoff[rule.id] = millis() + 5000;
            }
            return;
        }
        LOGGER.infof("[PeriphExec] Fallback sync (heap=%d, slots=%d): '%s'",
                     (int)ESP.getFreeHeap(),
                     (int)uxSemaphoreGetCount(_taskSlotSemaphore),
                     rule.name.c_str());
        executeAllActions(rule, receivedValue);
        return;
    }

    // 获取一个任务槽（非阻塞）
    if (xSemaphoreTake(_taskSlotSemaphore, 0) != pdTRUE) {
        if (shouldAvoidSyncFallback(rule)) {
            LOGGER.warningf("[PeriphExec] Skip heavy rule without async slot: '%s'", rule.name.c_str());
            {
                MutexGuard runLock(_runningRulesMutex);
                _failureBackoff[rule.id] = millis() + 5000;
            }
            return;
        }
        LOGGER.warningf("[PeriphExec] No async slot, fallback sync: '%s'", rule.name.c_str());
        executeAllActions(rule, receivedValue);
        return;
    }

    // 从对象池获取异步上下文（避免频繁堆分配）
    AsyncExecContext* ctx = _contextPool.acquire();
    if (!ctx) {
        if (shouldAvoidSyncFallback(rule)) {
            LOGGER.warningf("[PeriphExec] Context pool exhausted, skip heavy rule: '%s'", rule.name.c_str());
            xSemaphoreGive(_taskSlotSemaphore);
            {
                MutexGuard runLock(_runningRulesMutex);
                _failureBackoff[rule.id] = millis() + 5000;
            }
            return;
        }
        LOGGER.warning("[PeriphExec] Context pool exhausted, fallback to sync execution");
        xSemaphoreGive(_taskSlotSemaphore);
        executeAllActions(rule, receivedValue);
        return;
    }

    ctx->ruleCopy = rule;
    ctx->receivedValue = receivedValue;
    ctx->manager = this;
    ctx->mqtt = nullptr;  // Will be set by executor if needed
    ctx->taskSlot = _taskSlotSemaphore;

    // 确定栈大小：有脚本/Modbus动作用大栈，其他用小栈
    bool needLargeStack = shouldAvoidSyncFallback(rule);
    uint32_t stackSize = needLargeStack ? SCRIPT_TASK_STACK : SIMPLE_TASK_STACK;

    // 创建 FreeRTOS 任务
    char taskName[24];
    snprintf(taskName, sizeof(taskName), "exec_%.16s", rule.id.c_str());

    // 在创建任务前先将规则ID加入运行中集合
    {
        MutexGuard runLock(_runningRulesMutex);
        _runningRuleIds.insert(rule.id);
    }

    BaseType_t created =
#if CHIP_DUAL_CORE
        xTaskCreatePinnedToCore(
            asyncExecTaskFunc,      // 任务函数
            taskName,               // 任务名称
            stackSize,              // 栈大小
            ctx,                    // 参数
            ASYNC_TASK_PRIORITY,    // 优先级 0（低于主循环）
            nullptr,                // 不需要任务句柄
            1                       // 运行在 Core 1（与主循环相同）
        );
#else
        xTaskCreate(
            asyncExecTaskFunc,      // 任务函数
            taskName,               // 任务名称
            stackSize,              // 栈大小
            ctx,                    // 参数
            ASYNC_TASK_PRIORITY,    // 优先级 0（低于主循环）
            nullptr                 // 不需要任务句柄
        );
#endif

    if (created != pdPASS) {
        LOGGER.errorf("[PeriphExec] Failed to create async task, insufficient memory: '%s'", rule.name.c_str());
        // 任务创建失败，从运行中集合移除
        {
            MutexGuard runLock(_runningRulesMutex);
            _runningRuleIds.erase(rule.id);
        }
        _contextPool.release(ctx);  // 归还对象池
        xSemaphoreGive(_taskSlotSemaphore);
        executeAllActions(rule, receivedValue);
        return;
    }

    LOG_DEBUGF("[PeriphExec] Async dispatched: '%s' (stack=%d, heap=%d)",
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

    LOG_DEBUGF("[PeriphExec] Async task started: '%s'", result.ruleName.c_str());

    // 堆守卫：任务开始时检查堆内存，过低时直接放弃执行防止 abort()
    bool allOk = false;
    if (ESP.getFreeHeap() < 20000) {
        LOGGER.warningf("[PeriphExec] Heap too low (%d), skip execution: '%s'",
                        (int)ESP.getFreeHeap(), result.ruleName.c_str());
        result.status = AsyncExecStatus::FAILED;
    } else {
        // 执行所有动作
        auto actionResults = ctx->manager->executeAllActions(ctx->ruleCopy, ctx->receivedValue);

        // 统计结果
        allOk = true;
        for (const auto& ar : actionResults) {
            if (!ar.success) { allOk = false; break; }
        }
        result.status = allOk ? AsyncExecStatus::COMPLETED : AsyncExecStatus::FAILED;
    }

    result.endTime = millis();

    LOGGER.infof("[PeriphExec] Async task finished: '%s' %s (%lums)",
                 result.ruleName.c_str(),
                 allOk ? "OK" : "FAILED",
                 result.endTime - result.startTime);

    // 记录执行结果
    ctx->manager->recordResult(result);

    // 从运行中集合移除规则ID
    {
        MutexGuard runLock(ctx->manager->_runningRulesMutex);
        ctx->manager->_runningRuleIds.erase(ctx->ruleCopy.id);
    }

    // 如果执行失败，设置退避时间（30秒后才能再次触发）
    // 堆守卫：低堆时跳过 map 插入，防止 abort()
    if (ESP.getFreeHeap() > 10000) {
        if (!allOk) {
            MutexGuard runLock(ctx->manager->_runningRulesMutex);
            ctx->manager->_failureBackoff[ctx->ruleCopy.id] = millis() + 30000;
            LOGGER.infof("[PeriphExec] Rule '%s' failed, backoff for 30s", result.ruleName.c_str());
        } else {
            // 成功时清除退避记录
            MutexGuard runLock(ctx->manager->_runningRulesMutex);
            ctx->manager->_failureBackoff.erase(ctx->ruleCopy.id);
        }
    }

    // 触发外设执行完成事件（低堆时跳过，避免内部 STL 分配导致 abort）
    if (ESP.getFreeHeap() > 10000) {
        ctx->manager->dispatchPeriphExecEvent(ctx->ruleCopy.id, allOk ? "success" : "failed");
    }

    // 释放任务槽
    xSemaphoreGive(ctx->taskSlot);

    // 栈高水位标记（用于验证栈缩减安全性）
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
    LOGGER.infof("[PExec] Task '%s' stack HWM: %u bytes free",
                 result.ruleName.c_str(), (unsigned int)(hwm * sizeof(StackType_t)));

    // 归还上下文到对象池
    ctx->manager->_contextPool.release(ctx);

    // 删除自身任务
    vTaskDelete(nullptr);
}

// 执行规则的所有动作（供异步任务调用）
std::vector<ActionExecResult> PeriphExecManager::executeAllActions(
    const PeriphExecRule& rule, const String& receivedValue) {
    if (_executor) {
        return _executor->executeAllActions(rule, receivedValue);
    }
    return {};
}

// ========== 内部调度接口实现 ==========

void PeriphExecManager::checkTimerTriggers(unsigned long now, bool modbusAvailable) {
    // 堆守卫：低堆时跳过定时触发，防止 STL 容器分配导致 abort()
    if (ESP.getFreeHeap() < 20000) {
        return;
    }

    std::vector<PeriphExecRule> triggeredRules;
    triggeredRules.reserve(4);  // 预分配避免 push_back 时重新分配

    {
        MutexGuard lock(_rulesMutex);

        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled) continue;

            // 检查失败退避：如果上次执行失败且未过退避期，跳过
            {
                MutexGuard runLock(_runningRulesMutex);
                auto it = _failureBackoff.find(rule.id);
                if (it != _failureBackoff.end() && now < it->second) {
                    continue;  // 仍在退避期内
                }
            }

            // 遍历触发器，寻找定时触发或轮询触发类型
            for (auto& trigger : rule.triggers) {
                uint8_t tt = trigger.triggerType;
                if (tt != static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER) &&
                    tt != static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER)) continue;

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
                    // 检查规则是否需要 Modbus，以及 Modbus 是否可用
                    bool needsModbus = ruleNeedsModbus(rule);

                    // 如果需要 Modbus 但不可用，跳过本次触发并记录警告
                    if (needsModbus && !modbusAvailable) {
                        LOGGER.warningf("[PeriphExec] Skip timer '%s': Modbus not available", rule.name.c_str());
                        continue;
                    }

                    // 检查任务是否已在运行中
                    bool alreadyRunning = false;
                    {
                        MutexGuard runLock(_runningRulesMutex);
                        alreadyRunning = (_runningRuleIds.count(rule.id) > 0);
                    }

                    if (alreadyRunning) {
                        LOGGER.warningf("[PeriphExec] Skip timer '%s': task already running", rule.name.c_str());
                        continue;
                    }

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

void PeriphExecManager::dispatchEventMatchedRules(const String& eventId, const String& eventData) {
    std::vector<PeriphExecRule> triggeredRules;

    {
        MutexGuard lock(_rulesMutex);

        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled) continue;

            for (auto& trigger : rule.triggers) {
                if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER)) continue;
                if (trigger.eventId.isEmpty()) continue;
                if (trigger.eventId != eventId) continue;

                // 防重复触发：同一触发器最小间隔 1 秒
                unsigned long now = millis();
                if (trigger.lastTriggerTime > 0 && (now - trigger.lastTriggerTime) < 1000) continue;

                LOGGER.infof("[PeriphExec] Event rule matched: '%s' (event=%s)",
                    rule.name.c_str(), eventId.c_str());

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

void PeriphExecManager::dispatchPeriphExecEvent(const String& ruleId, const String& eventData) {
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

void PeriphExecManager::dispatchButtonEventRules(const String& eventId, const String& periphId) {
    std::vector<PeriphExecRule> triggeredRules;

    {
        MutexGuard lock(_rulesMutex);

        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled) continue;

            for (auto& trigger : rule.triggers) {
                if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER)) continue;
                if (trigger.eventId != eventId) continue;

                unsigned long now = millis();
                if (trigger.lastTriggerTime > 0 && (now - trigger.lastTriggerTime) < 100) continue;

                LOGGER.infof("[PeriphExec] Button event rule matched: '%s' (event=%s, periph=%s)",
                    rule.name.c_str(), eventId.c_str(), periphId.c_str());

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

void PeriphExecManager::processMqttMessageMatch(JsonArray& arr) {
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
                    // 确定要匹配的数据源ID（支持平台触发和事件触发数据源）
                    String matchItemId;
                    bool isPlatformTrigger = (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::PLATFORM_TRIGGER));
                    if (isPlatformTrigger) {
                        matchItemId = trigger.triggerPeriphId;
                    } else if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER) &&
                               trigger.eventId.startsWith("ds:")) {
                        matchItemId = trigger.eventId.substring(3);
                    } else {
                        continue;
                    }

                    // 匹配数据源外设 ID（必须配置且匹配，空/null 不匹配任何数据）
                    if (matchItemId.isEmpty() || matchItemId == "null") continue;
                    if (matchItemId != itemId) continue;

                    if (trigger.lastTriggerTime > 0 &&
                        (millis() - trigger.lastTriggerTime) < PERIPH_EXEC_POLL_TRIGGER_MIN_INTERVAL_MS) continue;

                    // 条件评估
                    bool conditionMet = false;
                    if (isPlatformTrigger && trigger.operatorType == 1) {
                        // 平台触发"设置模式"：仅需 ID 匹配即可执行，无需条件评估
                        conditionMet = true;
                    } else if (isPlatformTrigger && trigger.operatorType == 0) {
                        // 平台触发"匹配模式"：字符串精确匹配
                        conditionMet = (itemValue == trigger.compareValue);
                    } else {
                        // 事件触发数据源：使用通用条件评估（支持全部 10 种操作符）
                        conditionMet = evaluateCondition(itemValue, trigger.operatorType, trigger.compareValue);
                    }

                    if (conditionMet) {
                        LOGGER.infof("[PeriphExec] Rule '%s' triggered by %s=%s (op=%d, cmp=%s)",
                            rule.name.c_str(), itemId.c_str(), itemValue.c_str(),
                            trigger.operatorType, trigger.compareValue.c_str());

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

void PeriphExecManager::processPollDataMatch(JsonArray& arr, const String& source) {
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
                bool hasPollCollectionAction = ruleHasPollCollectionAction(rule);

                // 遍历所有触发器（OR 关系）
                bool triggerMatched = false;
                for (auto& trigger : rule.triggers) {
                    // 确定要匹配的数据源ID（支持轮询触发和事件触发数据源）
                    String matchItemId;
                    if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER)) {
                        if (source == "modbus_poll" && hasPollCollectionAction) continue;
                        matchItemId = trigger.triggerPeriphId;
                    } else if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER) &&
                               trigger.eventId.startsWith("ds:")) {
                        matchItemId = trigger.eventId.substring(3);
                    } else {
                        continue;
                    }

                    // 匹配数据源外设 ID（必须配置且匹配，空/null 不匹配任何数据）
                    if (matchItemId.isEmpty() || matchItemId == "null") continue;
                    if (matchItemId != itemId) continue;

                    unsigned long minTriggerIntervalMs = getPollTriggerCooldownMs(rule, source);
                    if (trigger.lastTriggerTime > 0 &&
                        (millis() - trigger.lastTriggerTime) < minTriggerIntervalMs) continue;

                    // 条件评估 - 使用通用方法（自动检测数值/字符串类型）
                    bool conditionMet = evaluateCondition(itemValue, trigger.operatorType, trigger.compareValue);

                    if (conditionMet) {
                        LOGGER.infof("[PeriphExec] Poll rule '%s' triggered by %s: %s=%s (op=%d, cmp=%s)",
                            rule.name.c_str(), source.c_str(), itemId.c_str(), itemValue.c_str(),
                            trigger.operatorType, trigger.compareValue.c_str());

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

String PeriphExecManager::processDataCommandMatch(JsonArray& cmdArr, const std::vector<int>& processedIndices) {
    // 预处理阶段：处理 modbus_read 指令
    JsonDocument modbusReportDoc;
    JsonArray modbusReportArr = modbusReportDoc.to<JsonArray>();

    if (_modbusReadCallback) {
        int idx = 0;
        for (JsonObject item : cmdArr) {
            String itemId = item["id"].as<String>();
            if (itemId == "modbus_read") {
                String itemValue = item["value"].as<String>();
                LOGGER.infof("[PeriphExec] DataCommand: modbus_read command detected");
                String modbusResult = _modbusReadCallback(itemValue);
                JsonDocument tmpDoc;
                if (!deserializeJson(tmpDoc, modbusResult) && tmpDoc.is<JsonArray>()) {
                    for (JsonVariant v : tmpDoc.as<JsonArray>()) {
                        modbusReportArr.add(v);
                    }
                }
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

                    // 条件评估 - 区分匹配模式和设置模式
                    bool conditionMet = false;
                    if (trigger.operatorType == 1) {
                        // 设置模式：仅需 ID 匹配即可执行
                        conditionMet = true;
                    } else if (trigger.operatorType == 0) {
                        // 匹配模式：字符串精确匹配
                        conditionMet = (itemValue == trigger.compareValue);
                    } else {
                        // 其他操作符：使用通用条件评估（支持全部 10 种）
                        conditionMet = evaluateCondition(itemValue, trigger.operatorType, trigger.compareValue);
                    }

                    if (!conditionMet) continue;

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

String PeriphExecManager::getDynamicEventsJsonInternal() {
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

// ========== 通用条件评估 ==========

bool PeriphExecManager::evaluateCondition(const String& value, uint8_t op, const String& compareValue) {
    ExecOperator oper = static_cast<ExecOperator>(op);

    // 字符串操作符（不需要数值转换）
    if (oper == ExecOperator::CONTAIN) {
        return value.indexOf(compareValue) >= 0;
    }
    if (oper == ExecOperator::NOT_CONTAIN) {
        return value.indexOf(compareValue) < 0;
    }

    // EQ / NEQ：自动检测数值/字符串类型
    if (oper == ExecOperator::EQ || oper == ExecOperator::NEQ) {
        bool bothNumeric = isNumericString(value) && isNumericString(compareValue);
        if (bothNumeric) {
            float val = value.toFloat();
            float cmp = compareValue.toFloat();
            return (oper == ExecOperator::EQ) ? (val == cmp) : (val != cmp);
        }
        // 非数值：字符串精确比较
        return (oper == ExecOperator::EQ) ? (value == compareValue) : (value != compareValue);
    }

    // 数值操作符（GT/LT/GTE/LTE/BETWEEN/NOT_BETWEEN）
    float val = value.toFloat();
    float cmp = compareValue.toFloat();

    switch (oper) {
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

// ========== 工具 ==========

String PeriphExecManager::generateUniqueId() {
    return "exec_" + String(millis());
}
