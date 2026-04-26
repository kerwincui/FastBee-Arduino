#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC

#include "core/PeriphExecScheduler.h"
#include "core/PeriphExecManager.h"
#include "core/PeriphExecExecutor.h"
#include "core/FastBeeFramework.h"
#include "protocols/ProtocolManager.h"
#if FASTBEE_ENABLE_MODBUS
#include "protocols/ModbusHandler.h"
#endif
#if FASTBEE_ENABLE_MQTT
#include "protocols/MQTTClient.h"
#endif
#include "systems/LoggerSystem.h"
#include "core/PeripheralManager.h"
#include <WiFi.h>

// 注意：不再使用静态 JsonDocument，因为 ArduinoJson v7 的 JsonDocument
// 内存池只增不减（clear() 不释放内存），长时间运行会导致堆碎片化。
// 改为每次使用局部 JsonDocument，用完即销毁释放内存。

// 协议连接状态标志位
#define PROTOCOL_MQTT_CONNECTED    (1 << 0)
#define PROTOCOL_MODBUS_CONNECTED  (1 << 1)
#define PROTOCOL_TCP_CONNECTED     (1 << 2)
#define PROTOCOL_HTTP_CONNECTED    (1 << 3)
#define PROTOCOL_COAP_CONNECTED    (1 << 4)

PeriphExecScheduler::PeriphExecScheduler() = default;
PeriphExecScheduler::~PeriphExecScheduler() = default;

void PeriphExecScheduler::initialize(PeriphExecManager* manager, PeriphExecExecutor* executor) {
    _manager = manager;
    _executor = executor;
}

// ========== 配置安全校验 ==========

void PeriphExecScheduler::validateLoadedConfig() {
    if (!_manager) return;

    auto& rulesMap = _manager->getRules();

    // 统计活跃定时/轮询任务数（enabled 且触发类型为 TIMER 或 POLL）
    uint8_t activeTimerTaskCount = 0;
    for (auto& kv : rulesMap) {
        if (!kv.second.enabled) continue;
        for (const auto& trigger : kv.second.triggers) {
            if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER) ||
                trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER)) {
                activeTimerTaskCount++;
                break;  // 每个规则只计数一次
            }
        }
    }

    // 校验活跃任务数上限
    if (activeTimerTaskCount > MAX_ACTIVE_TASKS) {
        LOGGER.warningf("[WARN] Active timer/poll tasks (%d) exceeds max (%d), "
                        "excess rules may cause resource pressure",
                        activeTimerTaskCount, MAX_ACTIVE_TASKS);
    } else if (activeTimerTaskCount > WARN_TASK_THRESHOLD) {
        LOGGER.warningf("[WARN] High number of active timer/poll tasks: %d (threshold=%d)",
                        activeTimerTaskCount, WARN_TASK_THRESHOLD);
    }

    // 遍历所有规则，校验并修正危险的轮询间隔
    bool configModified = false;
    for (auto& kv : rulesMap) {
        if (!kv.second.enabled) continue;

        for (auto& trigger : kv.second.triggers) {
            if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER) &&
                trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER)) {
                continue;
            }

            uint32_t intervalMs = static_cast<uint32_t>(trigger.intervalSec) * 1000UL;

            // 绝对下限：轮询间隔 < 5s 强制修正为 5s
            if (intervalMs < MIN_POLL_INTERVAL_MS) {
                LOGGER.warningf("[WARN] Poll interval too low (%lus) for rule '%s', "
                                "forced to %lus (absolute minimum)",
                                (unsigned long)trigger.intervalSec,
                                kv.second.name.c_str(),
                                (unsigned long)(MIN_POLL_INTERVAL_MS / 1000));
                trigger.intervalSec = MIN_POLL_INTERVAL_MS / 1000;
                configModified = true;
            }
            // 危险组合：活跃任务 > 8 且轮询间隔 < 10s
            else if (activeTimerTaskCount > WARN_TASK_THRESHOLD &&
                     intervalMs < 10000) {
                LOGGER.warningf("[WARN] Poll interval too aggressive (%lus with %d tasks) "
                                "for rule '%s', adjusted to 30s",
                                (unsigned long)trigger.intervalSec,
                                activeTimerTaskCount,
                                kv.second.name.c_str());
                trigger.intervalSec = SAFE_POLL_INTERVAL_MS / 1000;
                configModified = true;
            }
        }
    }

    if (configModified) {
        LOGGER.info("[PeriphExec] Config auto-corrected for safety, "
                    "changes apply to runtime only (file unchanged)");
    }

    LOGGER.infof("[PeriphExec] Config validation complete: %d active timer/poll tasks",
                 activeTimerTaskCount);
}

// ========== 定时触发 ==========

uint32_t PeriphExecScheduler::getDynamicCheckPeriod(MemoryGuardLevel level) {
    switch (level) {
        case MemoryGuardLevel::WARN:    return CHECK_PERIOD_WARN_MS;
        case MemoryGuardLevel::SEVERE:  return CHECK_PERIOD_SEVERE_MS;
        case MemoryGuardLevel::CRITICAL: return CHECK_PERIOD_SEVERE_MS;  // CRITICAL 由 checkTimers 单独处理
        default:                        return CHECK_PERIOD_NORMAL_MS;
    }
}

void PeriphExecScheduler::checkTimers() {
    unsigned long now = millis();

    if (!_manager) return;

    // ========== 动态降频：根据 MemGuard 级别调整检查周期 ==========
    MemoryGuardLevel currentLevel = MemoryGuardLevel::NORMAL;
    auto* fw = FastBeeFramework::getInstance();
    if (fw) {
        HealthMonitor* monitor = fw->getHealthMonitor();
        if (monitor) {
            currentLevel = monitor->getMemoryGuardLevel();
        }
    }

    // CRITICAL: 暂停非关键轮询，直接返回
    if (currentLevel == MemoryGuardLevel::CRITICAL) {
        // 仅在等级变化时输出日志，避免周期性刷屏
        if (_lastMemGuardLevel != MemoryGuardLevel::CRITICAL) {
            LOGGER.warning("[PeriphExec] MemGuard CRITICAL — non-critical polling suspended");
        }
        _lastMemGuardLevel = currentLevel;
        _currentCheckPeriodMs = CHECK_PERIOD_SEVERE_MS;
        return;
    }

    // 等级变化时更新检查周期并输出日志
    if (currentLevel != _lastMemGuardLevel) {
        _currentCheckPeriodMs = getDynamicCheckPeriod(currentLevel);
        const char* prevName = "NORMAL";
        const char* curName = "NORMAL";
        switch (_lastMemGuardLevel) {
            case MemoryGuardLevel::WARN:    prevName = "WARN"; break;
            case MemoryGuardLevel::SEVERE:  prevName = "SEVERE"; break;
            case MemoryGuardLevel::CRITICAL: prevName = "CRITICAL"; break;
            default: break;
        }
        switch (currentLevel) {
            case MemoryGuardLevel::WARN:    curName = "WARN"; break;
            case MemoryGuardLevel::SEVERE:  curName = "SEVERE"; break;
            default: break;
        }
        LOGGER.infof("[PeriphExec] MemGuard level changed: %s -> %s, check period=%lums",
                     prevName, curName, (unsigned long)_currentCheckPeriodMs);
        _lastMemGuardLevel = currentLevel;
    }

    // 按动态周期检查
    if (now - _lastTimerCheck < _currentCheckPeriodMs) return;
    _lastTimerCheck = now;

    // 预先获取 ModbusHandler 状态（避免在锁内调用）
    ModbusHandler* modbus = nullptr;
    bool modbusAvailable = false;
    if (fw) {
        auto* protMgr = fw->getProtocolManager();
        if (protMgr) {
            modbus = protMgr->getModbusHandler();
            modbusAvailable = (modbus != nullptr && modbus->getMode() == MODBUS_MASTER);
        }
    }

    // 通过 manager 获取规则列表并检查定时触发
    _manager->checkTimerTriggers(now, modbusAvailable);
}

// ========== 事件触发 ==========

void PeriphExecScheduler::triggerEvent(EventType eventType, const String& eventData) {
    if (!_manager) return;

    // 启动保护：PeriphExecManager 未初始化时（mutex 未创建），跳过事件处理
    if (!_manager->isInitialized()) return;

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

    // 通过 manager 分发事件匹配的规则
    _manager->dispatchEventMatchedRules(targetEventId, eventData);
}

void PeriphExecScheduler::triggerEventById(const String& eventId, const String& eventData) {
    const EventDef* def = findStaticEvent(eventId.c_str());
    if (def) {
        triggerEvent(def->type, eventData);
    } else {
        // 检查是否是外设执行规则事件
        if (eventId.startsWith("exec_")) {
            triggerPeriphExecEvent(eventId, eventData);
        } else if (eventId.startsWith("mc:")) {
            // Modbus 控制类子设备事件
            if (!_manager) return;
            if (!_manager->isInitialized()) return;
            LOGGER.infof("[PeriphExec] Modbus control event triggered: %s (data=%s)", eventId.c_str(), eventData.c_str());
            _manager->dispatchEventMatchedRules(eventId, eventData);
        } else {
            LOGGER.warningf("[PeriphExec] Unknown event ID: %s", eventId.c_str());
        }
    }
}

void PeriphExecScheduler::triggerPeriphExecEvent(const String& ruleId, const String& eventData) {
    if (!_manager) return;
    if (!_manager->isInitialized()) return;

    LOGGER.infof("[PeriphExec] Periph exec event triggered: %s", ruleId.c_str());

    // 通过 manager 分发外设执行事件匹配的规则
    _manager->dispatchPeriphExecEvent(ruleId, eventData);
}

// ========== 消息/数据处理 ==========

void PeriphExecScheduler::handleDataEvent(const String& source, const String& data, DataSourceType sourceType) {
    if (!_manager) return;

    // 使用局部 JsonDocument 解析 JSON 数组: [{"id":"temperature","value":"27.43"}, ...]
    // 局部变量用完即销毁，避免静态 JsonDocument 内存池只增不减导致堆碎片化
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data);
    if (err || !doc.is<JsonArray>()) return;

    JsonArray arr = doc.as<JsonArray>();

    // 根据数据源类型调用对应的处理方法
    if (sourceType == DataSourceType::MQTT) {
        _manager->processMqttMessageMatch(arr);
    } else {
        _manager->processPollDataMatch(arr, source);
    }
}

void PeriphExecScheduler::handleMqttMessage(const String& topic, const String& message) {
    handleDataEvent(topic, message, DataSourceType::MQTT);
}

void PeriphExecScheduler::handlePollData(const String& source, const String& data) {
    handleDataEvent(source, data, DataSourceType::POLL);
}

String PeriphExecScheduler::handleDataCommand(const String& message) {
    if (!_manager || !_executor) return "";

    JsonDocument cmdDoc;
    DeserializationError err = deserializeJson(cmdDoc, message);
    if (err || !cmdDoc.is<JsonArray>()) {
        LOGGER.warning("[PeriphExec] Received command: INVALID JSON (parse failed)");
        return "";
    }

    JsonArray cmdArr = cmdDoc.as<JsonArray>();
    LOGGER.infof("[PeriphExec] Received command: DATA_COMMAND with %d items", cmdArr.size());

    // 预处理阶段：处理 modbus_read 指令（阻塞操作，必须在持锁之前完成）
    JsonDocument modbusReportDoc;
    JsonArray modbusReportArr = modbusReportDoc.to<JsonArray>();
    std::vector<int> processedIndices;

    {
        int idx = 0;
        for (JsonObject item : cmdArr) {
            String itemId = item["id"].as<String>();
            if (itemId == "modbus_read" || itemId == "modbus_raw_send") {
                String itemValue = item["value"].as<String>();
                LOGGER.infof("[PeriphExec] DataCommand: %s command detected", itemId.c_str());
                // Note: This is handled by the manager now
                processedIndices.push_back(idx);
            }
            idx++;
        }
    }

    // 通过 manager 处理数据命令匹配和执行
    return _manager->processDataCommandMatch(cmdArr, processedIndices);
}

// ========== 数据上报 ==========

bool PeriphExecScheduler::tryReportDeviceData() {
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

void PeriphExecScheduler::checkButtonEvents() {
    unsigned long now = millis();

    // 每 50ms 检测一次按键状态（与任务间隔一致）
    if (now - _lastButtonCheck < 50) return;
    _lastButtonCheck = now;

    PeripheralManager& pm = PeripheralManager::getInstance();

    pm.forEachPeripheral([this, &pm, now](const PeripheralConfig& config) {
        // 只处理支持按键事件的外设类型（上拉/下拉输入）
        if (!config.enabled) return;
        if (!supportsButtonEvent(config.type)) return;

        // 获取或创建按键状态
        if (_buttonStates.find(config.id) == _buttonStates.end()) {
            ButtonRuntimeState state;
            state.periphId = config.id;
            state.lastState = true;  // 上拉默认高电平
            state.currentState = true;
            _buttonStates[config.id] = state;
        }

        ButtonRuntimeState& btnState = _buttonStates[config.id];

        // 读取当前按键状态
        GPIOState gpioState = pm.readPin(config.id);
        if (gpioState == GPIOState::STATE_UNDEFINED) return;

        bool currentLevel = (gpioState == GPIOState::STATE_HIGH);
        btnState.currentState = currentLevel;

        // 检测状态变化（消抖处理）
        if (currentLevel != btnState.lastState) {
            // 检查消抖时间
            if (now - btnState.lastChangeTime >= _buttonConfig.debounceMs) {
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
                    if (pressDuration < _buttonConfig.longPress2sMs) {
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
                if (!btnState.longPress2sTriggered && pressDuration >= _buttonConfig.longPress2sMs) {
                    btnState.longPress2sTriggered = true;
                    triggerButtonEvent(config.id, EventType::EVENT_BUTTON_LONG_PRESS_2S);
                    LOGGER.infof("[PeriphExec] Button LONG_PRESS_2S: %s", config.id.c_str());
                }

                // 长按5秒
                if (!btnState.longPress5sTriggered && pressDuration >= _buttonConfig.longPress5sMs) {
                    btnState.longPress5sTriggered = true;
                    triggerButtonEvent(config.id, EventType::EVENT_BUTTON_LONG_PRESS_5S);
                    LOGGER.infof("[PeriphExec] Button LONG_PRESS_5S: %s", config.id.c_str());
                }

                // 长按10秒
                if (!btnState.longPress10sTriggered && pressDuration >= _buttonConfig.longPress10sMs) {
                    btnState.longPress10sTriggered = true;
                    triggerButtonEvent(config.id, EventType::EVENT_BUTTON_LONG_PRESS_10S);
                    LOGGER.infof("[PeriphExec] Button LONG_PRESS_10S: %s", config.id.c_str());
                }
            } else {
                // 按键释放状态，检查双击
                if (btnState.clickCount > 0) {
                    // 检查双击超时
                    if (now - btnState.lastClickTime >= _buttonConfig.clickIntervalMs) {
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
    });
}

void PeriphExecScheduler::triggerButtonEvent(const String& periphId, EventType eventType) {
    // 查找事件ID
    const char* targetEventId = nullptr;
    for (size_t i = 0; STATIC_EVENTS[i].id != nullptr; i++) {
        if (STATIC_EVENTS[i].type == eventType) {
            targetEventId = STATIC_EVENTS[i].id;
            break;
        }
    }

    if (!targetEventId) return;

    if (!_manager) return;

    // 构建事件数据包含外设ID
    String eventData = "{\"periphId\":\"" + periphId + "\"}";

    // 通过 manager 分发按键事件匹配的规则
    _manager->dispatchButtonEventRules(targetEventId, periphId);
}

// ========== 配置辅助方法 ==========

String PeriphExecScheduler::getStaticEventsJson() {
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

String PeriphExecScheduler::getDynamicEventsJson() {
    if (!_manager) return "[]";
    return _manager->getDynamicEventsJsonInternal();
}

String PeriphExecScheduler::getValidTriggerTypes() {
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

    // 轮询触发（本地数据源条件评估）
    addTrigger(static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER),
               "轮询触发", "本地数据源（如Modbus传感器）周期轮询条件评估触发");

    String result;
    serializeJson(doc, result);
    return result;
}

String PeriphExecScheduler::getEventCategoriesJson() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    // 定义事件分类
    const char* categories[] = {"WiFi", "MQTT", "网络", "协议", "系统", "配网", "规则", "按键", "数据", "Modbus子设备", "外设执行"};
    const char* descriptions[] = {
        "WiFi连接状态变化事件",
        "MQTT连接状态变化事件",
        "网络模式切换事件",
        "协议启用事件",
        "系统服务事件",
        "配网过程事件",
        "规则引擎事件",
        "按键输入事件",
        "协议数据收发事件",
        "Modbus采集与控制子设备触发事件",
        "外设执行规则事件"
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

// ========== 内部方法 ==========

bool PeriphExecScheduler::evaluateCondition(const String& value, uint8_t op, const String& compareValue) {
    // 委托给 PeriphExecManager 的统一实现（支持字符串/数值自动检测）
    return PeriphExecManager::evaluateCondition(value, op, compareValue);
}

String PeriphExecScheduler::collectPeripheralData() {
    PeripheralManager& pm = PeripheralManager::getInstance();

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    pm.forEachPeripheral([&](const PeripheralConfig& config) {
        // 只收集已启用的外设
        if (!config.enabled) return;

        // 只收集 GPIO 类型的外设状态
        int typeVal = static_cast<int>(config.type);
        if (typeVal < 11 || typeVal > 26) return;  // 跳过非GPIO/ADC/DAC类型

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
    });

    String result;
    serializeJson(doc, result);
    return result;
}

bool PeriphExecScheduler::checkNetworkAndProtocolStatus(uint8_t& connectedProtocols) {
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

    // 检查 Modbus 状态（使用 bool 方法替代字符串分配）
    ModbusHandler* modbus = protocolMgr->getModbusHandler();
    if (modbus && modbus->isRunning()) {
        connectedProtocols |= PROTOCOL_MODBUS_CONNECTED;
        hasConnectedProtocol = true;
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

MQTTClient* PeriphExecScheduler::getMqttClient() {
    auto* fw = FastBeeFramework::getInstance();
    if (fw) {
        auto* pm = fw->getProtocolManager();
        if (pm) return pm->getMQTTClient();
    }
    return nullptr;
}

#endif // FASTBEE_ENABLE_PERIPH_EXEC
