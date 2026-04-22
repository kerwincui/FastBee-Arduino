#include "core/PeriphExecScheduler.h"
#include "core/PeriphExecManager.h"
#include "core/PeriphExecExecutor.h"
#include "core/FastBeeFramework.h"
#include "protocols/ProtocolManager.h"
#include "protocols/ModbusHandler.h"
#include "protocols/MQTTClient.h"
#include "systems/LoggerSystem.h"
#include "core/PeripheralManager.h"
#include <WiFi.h>

// 静态 JsonDocument 用于高频 JSON 解析，避免频繁堆内存分配
// 注意：这两个方法可能被不同任务调用（MQTT 回调任务和主循环任务），需要互斥锁保护
static JsonDocument g_mqttDoc;
static JsonDocument g_pollDoc;
static SemaphoreHandle_t g_mqttDocMutex = nullptr;
static SemaphoreHandle_t g_pollDocMutex = nullptr;

// 互斥锁初始化辅助函数
static void initDocMutexes() {
    if (g_mqttDocMutex == nullptr) {
        g_mqttDocMutex = xSemaphoreCreateMutex();
    }
    if (g_pollDocMutex == nullptr) {
        g_pollDocMutex = xSemaphoreCreateMutex();
    }
}

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

// ========== 定时触发 ==========

void PeriphExecScheduler::checkTimers() {
    unsigned long now = millis();

    // 每秒检查一次
    if (now - _lastTimerCheck < 1000) return;
    _lastTimerCheck = now;

    if (!_manager) return;

    // 预先获取 ModbusHandler 状态（避免在锁内调用）
    ModbusHandler* modbus = nullptr;
    bool modbusAvailable = false;
    auto* fw = FastBeeFramework::getInstance();
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

    // 初始化互斥锁（首次调用时）
    initDocMutexes();

    // 根据数据源类型选择静态资源和互斥锁（保持原有的并发安全性）
    JsonDocument* doc;
    SemaphoreHandle_t mutex;

    if (sourceType == DataSourceType::MQTT) {
        doc = &g_mqttDoc;
        mutex = g_mqttDocMutex;
    } else {
        doc = &g_pollDoc;
        mutex = g_pollDocMutex;
    }

    // 解析 JSON 数组: [{"id":"temperature","value":"27.43"}, ...]
    // 使用静态 JsonDocument 避免频繁堆内存分配
    JsonArray arr;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        doc->clear();  // 清除上次数据，复用内存
        DeserializationError err = deserializeJson(*doc, data);
        if (!err && doc->is<JsonArray>()) {
            arr = doc->as<JsonArray>();
        }
        xSemaphoreGive(mutex);
    } else {
        // 获取锁超时，使用临时 JsonDocument 作为降级方案
        JsonDocument tempDoc;
        DeserializationError err = deserializeJson(tempDoc, data);
        if (!err && tempDoc.is<JsonArray>()) {
            arr = tempDoc.as<JsonArray>();
        }
    }

    if (arr.isNull()) return;

    // 根据数据源类型调用对应的处理方法
    if (sourceType == DataSourceType::MQTT) {
        // 通过 manager 处理 MQTT 消息匹配
        _manager->processMqttMessageMatch(arr);
    } else {
        // 通过 manager 处理轮询数据匹配
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
    const char* categories[] = {"WiFi", "MQTT", "网络", "协议", "系统", "配网", "规则", "按键", "数据", "数据源", "外设执行"};
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
        "数据源条件触发事件",
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

MQTTClient* PeriphExecScheduler::getMqttClient() {
    auto* fw = FastBeeFramework::getInstance();
    if (fw) {
        auto* pm = fw->getProtocolManager();
        if (pm) return pm->getMQTTClient();
    }
    return nullptr;
}
