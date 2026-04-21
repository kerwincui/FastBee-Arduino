/**
 *@description: 协议管理器实现
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:31:25
 */

#include "protocols/ProtocolManager.h"
#include "systems/LoggerSystem.h"
#include "core/PeriphExecManager.h"
#include "core/FeatureFlags.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <memory>

ProtocolManager::ProtocolManager() 
    : isInitialized(false) {
}

ProtocolManager::~ProtocolManager() {
    stopAll();
}

bool ProtocolManager::initialize() {
    if (isInitialized) {
        return true;
    }
    
    LOG_INFO("Protocol Manager: Initializing...");
    isInitialized = true;
    return true;
}

bool ProtocolManager::addProtocol(ProtocolType type, const String& name, bool enabled) {
    ProtocolConfig config;
    config.type    = type;
    config.name    = name;
    config.enabled = enabled;
    config.config  = nullptr;

    protocols.push_back(config);

    char buf[64];
    snprintf(buf, sizeof(buf), "Protocol Manager: Added [%s]", name.c_str());
    LOG_INFO(buf);
    return true;
}

bool ProtocolManager::setProtocolConfig(ProtocolType type, void* config) {
    for (auto& protocol : protocols) {
        if (protocol.type == type) {
            protocol.config = config;
            
            // 根据协议类型初始化
            switch (type) {
                case ProtocolType::MQTT:
                    return initMQTT(config);
                case ProtocolType::MODBUS:
                    return initModbus(config);
#if FASTBEE_ENABLE_TCP
                case ProtocolType::TCP:
                    return initTCP(config);
#endif
#if FASTBEE_ENABLE_HTTP
                case ProtocolType::HTTP:
                    return initHTTP(config);
#endif
#if FASTBEE_ENABLE_COAP
                case ProtocolType::COAP:
                    return initCoAP(config);
#endif
                default:
                    return false;
            }
        }
    }
    return false;
}

bool ProtocolManager::startAll() {
    LOG_INFO("Protocol Manager: Starting all protocols...");

    bool allStarted = true;
    for (const auto& protocol : protocols) {
        if (!protocol.enabled) continue;

        bool started = false;
        switch (protocol.type) {
            case ProtocolType::MQTT:   
                started = mqttClient && mqttClient->begin();
                if (started) PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_MQTT_ENABLED, "");
                break;
            case ProtocolType::MODBUS: 
                started = modbusHandler && modbusHandler->begin();
                if (started) PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_MODBUS_RTU_ENABLED, "");
                break;
#if FASTBEE_ENABLE_TCP
            case ProtocolType::TCP:    
                started = tcpHandler && tcpHandler->beginFromConfig();
                if (started) PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_TCP_ENABLED, "");
                break;
#endif
#if FASTBEE_ENABLE_HTTP
            case ProtocolType::HTTP:   
                started = httpClientWrapper && httpClientWrapper->beginFromConfig();
                if (started) PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_HTTP_ENABLED, "");
                break;
#endif
#if FASTBEE_ENABLE_COAP
            case ProtocolType::COAP:   
                started = coapHandler && coapHandler->beginFromConfig();
                if (started) PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_COAP_ENABLED, "");
                break;
#endif
        }

        if (!started) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Protocol Manager: Failed to start [%s]", protocol.name.c_str());
            LOG_WARNING(buf);
            allStarted = false;
        }
    }

    return allStarted;
}

void ProtocolManager::stopAll() {
    LOG_INFO("Protocol Manager: Stopping all protocols...");
    if (mqttClient)       mqttClient->disconnect();
    if (modbusHandler)    modbusHandler->end();
#if FASTBEE_ENABLE_TCP
    if (tcpHandler)       tcpHandler->disconnect();
#endif
#if FASTBEE_ENABLE_HTTP
    if (httpClientWrapper) httpClientWrapper->end();
#endif
#if FASTBEE_ENABLE_COAP
    if (coapHandler)      coapHandler->end();
#endif
}

void ProtocolManager::shutdown() {
    LOG_INFO("Protocol Manager: Shutting down...");

    stopAll();

    mqttClient.reset();
    modbusHandler.reset();
#if FASTBEE_ENABLE_TCP
    tcpHandler.reset();
#endif
#if FASTBEE_ENABLE_HTTP
    httpClientWrapper.reset();
#endif
#if FASTBEE_ENABLE_COAP
    coapHandler.reset();
#endif

    protocols.clear();
    messageCallback = nullptr;
    txCallback = nullptr;
    rxCallback = nullptr;
    sseCallback = nullptr;
    isInitialized   = false;

    LOG_INFO("Protocol Manager: Shutdown complete");
}

bool ProtocolManager::sendData(ProtocolType type, const String& topic, const String& data) {
    switch (type) {
        case ProtocolType::MQTT:
            return mqttClient && mqttClient->publish(topic, data);
        case ProtocolType::MODBUS:
            if (!modbusHandler) return false;
            if (modbusHandler->getMode() == MODBUS_MASTER) {
                // Master模式: topic格式 "slaveAddr:regAddr", data为写入值
                int sepIdx = topic.indexOf(':');
                if (sepIdx > 0) {
                    uint8_t slaveAddr = (uint8_t)topic.substring(0, sepIdx).toInt();
                    uint16_t regAddr = (uint16_t)topic.substring(sepIdx + 1).toInt();
                    uint16_t value = (uint16_t)data.toInt();
                    return modbusHandler->masterWriteSingleRegister(slaveAddr, regAddr, value);
                }
                return false;
            }
            // Slave模式: topic为寄存器地址
            return modbusHandler->writeData(topic.toInt(), data);
        case ProtocolType::TCP:
#if FASTBEE_ENABLE_TCP
            return tcpHandler && tcpHandler->send(data);
#else
            return false;
#endif
        case ProtocolType::HTTP:
#if FASTBEE_ENABLE_HTTP
            return httpClientWrapper && httpClientWrapper->post(topic, data);
#else
            return false;
#endif
        case ProtocolType::COAP:
#if FASTBEE_ENABLE_COAP
            return coapHandler && coapHandler->send(topic, data);
#else
            return false;
#endif
        default:
            return false;
    }
}

void ProtocolManager::setMessageCallback(MessageCallback callback) {
    this->messageCallback = callback;
}

String ProtocolManager::getProtocolStatus(ProtocolType type) {
    switch (type) {
        case ProtocolType::MQTT:
            return mqttClient ? mqttClient->getStatus() : "Not initialized";
        case ProtocolType::MODBUS:
            return modbusHandler ? modbusHandler->getStatus() : "Not initialized";
        case ProtocolType::TCP:
#if FASTBEE_ENABLE_TCP
            return tcpHandler ? tcpHandler->getStatus() : "Not initialized";
#else
            return "Disabled";
#endif
        case ProtocolType::HTTP:
#if FASTBEE_ENABLE_HTTP
            return httpClientWrapper ? httpClientWrapper->getStatus() : "Not initialized";
#else
            return "Disabled";
#endif
        case ProtocolType::COAP:
#if FASTBEE_ENABLE_COAP
            return coapHandler ? coapHandler->getStatus() : "Not initialized";
#else
            return "Disabled";
#endif
        default:
            return "Unknown protocol";
    }
}

void ProtocolManager::handle() {
    if (mqttClient) mqttClient->handle();
    if (modbusHandler) modbusHandler->handle();
#if FASTBEE_ENABLE_TCP
    if (tcpHandler) tcpHandler->handle();
#endif
#if FASTBEE_ENABLE_HTTP
    if (httpClientWrapper) httpClientWrapper->handle();
#endif
#if FASTBEE_ENABLE_COAP
    if (coapHandler) coapHandler->handle();
#endif
}

void ProtocolManager::handleMessage(ProtocolType type, const String& topic, const String& message) {
    if (rxCallback) rxCallback();
    if (messageCallback) {
        messageCallback(type, topic, message);
    }
}

void ProtocolManager::setTxCallback(CounterCallback cb) { txCallback = cb; }
void ProtocolManager::setRxCallback(CounterCallback cb) { rxCallback = cb; }
void ProtocolManager::setSSECallback(SSEBroadcastCallback callback) { sseCallback = callback; }
void ProtocolManager::setMQTTStatusSSECallback(SimpleSSECallback callback) { mqttStatusSSECallback = callback; }
void ProtocolManager::setModbusStatusSSECallback(SimpleSSECallback callback) { modbusStatusSSECallback = callback; }

bool ProtocolManager::restartMQTT() {
    LOG_INFO("Protocol Manager: Restarting MQTT...");
    
    // 断开现有连接
    if (mqttClient) {
        mqttClient->disconnect();
        mqttClient.reset();
    }
    
    // 重新创建并初始化
    mqttClient = std::unique_ptr<MQTTClient>(new MQTTClient());
    mqttClient->setMessageCallback([this](const String& topic, const String& message, MqttTopicType tType) {
        handleMessage(ProtocolType::MQTT, topic, message);
    });
    
    // 设置状态变化回调（SSE 推送用）
    if (mqttStatusSSECallback) {
        mqttClient->setStatusChangeCallback([this](const String& data) {
            if (mqttStatusSSECallback) mqttStatusSSECallback(data);
        });
    }
    
    if (!mqttClient->begin()) {
        LOG_WARNING("Protocol Manager: MQTT restart begin() failed");
        return false;
    }
    
    // 尝试连接
    bool ok = mqttClient->connect();
    if (ok) {
        LOG_INFO("Protocol Manager: MQTT restarted and connected");
    } else {
        LOG_WARNING("Protocol Manager: MQTT restarted but connect failed (will auto-retry)");
    }
    return ok;
}

bool ProtocolManager::restartMQTTDeferred() {
    LOG_INFO("Protocol Manager: Restarting MQTT (deferred connect)...");
    
    // 断开现有连接
    if (mqttClient) {
        mqttClient->disconnect();
        mqttClient.reset();
    }
    
    // 重新创建并初始化（仅加载配置，不阻塞连接）
    mqttClient = std::unique_ptr<MQTTClient>(new MQTTClient());
    mqttClient->setMessageCallback([this](const String& topic, const String& message, MqttTopicType tType) {
        handleMessage(ProtocolType::MQTT, topic, message);
    });
    
    // 设置状态变化回调（SSE 推送用）
    if (mqttStatusSSECallback) {
        mqttClient->setStatusChangeCallback([this](const String& data) {
            if (mqttStatusSSECallback) mqttStatusSSECallback(data);
        });
    }
    
    if (!mqttClient->begin()) {
        LOG_WARNING("Protocol Manager: MQTT deferred restart begin() failed");
        return false;
    }
    
    // 不调用 connect()，由 MQTTClient::handle() 的 auto-reconnect 在 loop 中异步连接
    LOG_INFO("Protocol Manager: MQTT config reloaded, will auto-connect in loop");
    return true;
}

void ProtocolManager::stopMQTT() {
    LOG_INFO("Protocol Manager: Stopping MQTT...");
    if (mqttClient) {
        mqttClient->stop();
    }
}

bool ProtocolManager::restartModbus() {
    LOG_INFO("Protocol Manager: Restarting Modbus...");
    
    // 停止现有实例
    if (modbusHandler) {
        modbusHandler->end();
        modbusHandler.reset();
    }
    
    // 读取 protocol.json 配置
    if (!LittleFS.exists("/config/protocol.json")) {
        LOG_WARNING("Protocol Manager: No protocol config found for Modbus");
        return false;
    }
    
    File f = LittleFS.open("/config/protocol.json", "r");
    if (!f) {
        LOG_ERROR("Protocol Manager: Failed to open protocol config");
        return false;
    }
    
    FastBeeJsonDocLarge doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    
    if (err) {
        LOG_ERRORF("Protocol Manager: JSON parse error: %s", err.c_str());
        return false;
    }
    
    JsonVariant rtu = doc["modbusRtu"];
    if (!rtu.is<JsonObject>()) {
        LOG_WARNING("Protocol Manager: No modbusRtu config found");
        return false;
    }
    
    // 从嵌套的 modbusRtu 配置构建 ModbusConfig
    ModbusConfig modbusConfig;
    
    // 从外设管理器获取串口配置（引脚 + 波特率）
    String peripheralId = rtu["peripheralId"] | "";
    if (peripheralId.isEmpty()) {
        LOG_ERROR("Modbus RTU: No peripheralId configured, cannot start");
        return false;
    }

    PeripheralConfig* periph = PeripheralManager::getInstance().getPeripheral(peripheralId);
    if (!periph) {
        LOG_ERRORF("Modbus RTU: Peripheral '%s' not found", peripheralId.c_str());
        return false;
    }

    if (periph->type != PeripheralType::UART) {
        LOG_ERRORF("Modbus RTU: Peripheral '%s' is not UART type", peripheralId.c_str());
        return false;
    }

    // 引脚约定: pins[0]=RX, pins[1]=TX
    modbusConfig.rxPin = periph->pins[0];
    modbusConfig.txPin = periph->pins[1];
    modbusConfig.baudRate = periph->params.uart.baudRate > 0 ? periph->params.uart.baudRate : 9600;

    LOG_INFOF("Modbus RTU: Using peripheral '%s' - RX:%d, TX:%d, Baud:%d",
              peripheralId.c_str(), modbusConfig.rxPin, modbusConfig.txPin, modbusConfig.baudRate);

    // dePin 保留从 protocol.json 读取（RS485 方向控制是 Modbus 特有配置）
    int dePin = rtu["dePin"] | -1;
    modbusConfig.dePin = (dePin < 0) ? 255 : (uint8_t)dePin;
    modbusConfig.slaveAddress = rtu["slaveAddress"] | (uint8_t)1;
    modbusConfig.responseTimeout = rtu["timeout"] | (uint16_t)1000;
    modbusConfig.transferType = rtu["transferType"] | (uint8_t)0;
    modbusConfig.workMode = rtu["workMode"] | (uint8_t)1;  // 默认主动轮询模式
    
    String modeStr = rtu["mode"] | "slave";
    modbusConfig.mode = (modeStr == "master") ? MODBUS_MASTER : MODBUS_SLAVE;
    
    // 解析 Master 配置
    if (rtu.containsKey("master")) {
        JsonObject masterObj = rtu["master"];
        modbusConfig.master.responseTimeout = masterObj["responseTimeout"] | (uint16_t)1000;
        modbusConfig.master.maxRetries = masterObj["maxRetries"] | (uint8_t)2;
        modbusConfig.master.interPollDelay = masterObj["interPollDelay"] | (uint16_t)100;
        
        if (masterObj.containsKey("tasks")) {
            JsonArray tasksArr = masterObj["tasks"];
            modbusConfig.master.taskCount = 0;
            for (JsonVariant taskVar : tasksArr) {
                if (modbusConfig.master.taskCount >= Protocols::MODBUS_MAX_POLL_TASKS) break;
                JsonObject t = taskVar.as<JsonObject>();
                PollTask& task = modbusConfig.master.tasks[modbusConfig.master.taskCount];
                task.slaveAddress = t["slaveAddress"] | (uint8_t)1;
                task.functionCode = t["functionCode"] | (uint8_t)0x03;
                task.startAddress = t["startAddress"] | (uint16_t)0;
                task.quantity     = t["quantity"] | (uint16_t)10;
                task.pollInterval = t["pollInterval"] | (uint16_t)Protocols::MODBUS_DEFAULT_POLL_INTERVAL;
                task.enabled      = t["enabled"] | true;
                // 向后兼容：优先读取 name，回退到 label
                const char* taskName = t["name"] | (t["label"] | "");
                strlcpy(task.name, taskName, sizeof(task.name));
                
                // 解析寄存器映射
                task.mappingCount = 0;
                if (t.containsKey("mappings")) {
                    JsonArray mappingsArr = t["mappings"];
                    for (JsonVariant mv : mappingsArr) {
                        if (task.mappingCount >= Protocols::MODBUS_MAX_MAPPINGS_PER_TASK) break;
                        JsonObject m = mv.as<JsonObject>();
                        RegisterMapping& mapping = task.mappings[task.mappingCount];
                        mapping.regOffset = m["regOffset"] | (uint8_t)0;
                        mapping.dataType = m["dataType"] | (uint8_t)0;
                        mapping.scaleFactor = m["scaleFactor"] | 1.0f;
                        mapping.decimalPlaces = m["decimalPlaces"] | (uint8_t)1;
                        const char* sid = m["sensorId"] | "";
                        strncpy(mapping.sensorId, sid, sizeof(mapping.sensorId) - 1);
                        mapping.sensorId[sizeof(mapping.sensorId) - 1] = '\0';
                        const char* u = m["unit"] | "";
                        strncpy(mapping.unit, u, sizeof(mapping.unit) - 1);
                        mapping.unit[sizeof(mapping.unit) - 1] = '\0';
                        task.mappingCount++;
                    }
                }
                
                modbusConfig.master.taskCount++;
            }
        }
        
        // 解析子设备
        if (masterObj.containsKey("devices")) {
            JsonArray devicesArr = masterObj["devices"];
            modbusConfig.master.deviceCount = 0;
            for (JsonVariant dv : devicesArr) {
                if (modbusConfig.master.deviceCount >= Protocols::MODBUS_MAX_SUB_DEVICES) break;
                JsonObject d = dv.as<JsonObject>();
                ModbusSubDevice& dev = modbusConfig.master.devices[modbusConfig.master.deviceCount];
                const char* dName = d["name"] | "Device";
                strncpy(dev.name, dName, sizeof(dev.name) - 1);
                dev.name[sizeof(dev.name) - 1] = '\0';
                const char* dType = d["deviceType"] | "relay";
                strncpy(dev.deviceType, dType, sizeof(dev.deviceType) - 1);
                dev.deviceType[sizeof(dev.deviceType) - 1] = '\0';
                dev.slaveAddress    = d["slaveAddress"] | (uint8_t)1;
                dev.channelCount    = d["channelCount"] | (uint8_t)2;
                dev.coilBase        = d["coilBase"] | (uint16_t)0;
                dev.ncMode          = d["ncMode"] | false;
                dev.controlProtocol = d["controlProtocol"] | (uint8_t)0;
                dev.batchRegister   = d["batchRegister"] | (uint16_t)0;
                dev.pwmRegBase      = d["pwmRegBase"] | (uint16_t)0;
                dev.pwmResolution   = d["pwmResolution"] | (uint8_t)8;
                dev.pidDecimals     = d["pidDecimals"] | (uint8_t)1;
                dev.motorDecimals   = d["motorDecimals"] | (uint8_t)0;
                dev.enabled         = d["enabled"] | true;
                if (d.containsKey("pidAddrs") && d["pidAddrs"].is<JsonArray>()) {
                    JsonArray pa = d["pidAddrs"].as<JsonArray>();
                    for (int i = 0; i < 6 && i < (int)pa.size(); i++)
                        dev.pidAddrs[i] = pa[i] | (uint16_t)0;
                }
                if (d.containsKey("motorRegs") && d["motorRegs"].is<JsonArray>()) {
                    JsonArray mr = d["motorRegs"].as<JsonArray>();
                    for (int i = 0; i < 5 && i < (int)mr.size(); i++)
                        dev.motorRegs[i] = mr[i] | (uint16_t)0;
                }
                modbusConfig.master.deviceCount++;
            }
        }
    }

    ModbusHandler::sanitizeConfig(modbusConfig);
    
    // 创建新实例并设置回调
    modbusHandler = std::unique_ptr<ModbusHandler>(new ModbusHandler());
    
    modbusHandler->setDataCallback([this](uint8_t address, const String& data) {
        // 1. MQTT 上报
        if (mqttClient && mqttClient->getIsConnected()) {
            mqttClient->publishReportData(data);
        }
        // 2. SSE 实时推送
        if (sseCallback) {
            sseCallback(address, data);
        }
        // 3. JSON 格式数据注入 PeriphExec 进行条件匹配
        if (data.length() > 0 && data[0] == '[') {
            PeriphExecManager::getInstance().handleMqttMessage("modbus/data", data);
            // 4. 轮询触发：本地数据源条件评估（POLL_TRIGGER 规则）
            PeriphExecManager::getInstance().handlePollData("modbus", data);
        }
        // 5. 全局消息路由
        handleMessage(ProtocolType::MODBUS, String(address), data);
    });
    
    // 注册 Modbus 一次性读取回调到 PeriphExec
    PeriphExecManager::getInstance().setModbusReadCallback(
        [this](const String& params) -> String {
            return this->executeModbusRead(params);
        }
    );
    
    // 设置状态变化回调（SSE 推送用）
    if (modbusStatusSSECallback) {
        modbusHandler->setStatusChangeCallback([this](const String& data) {
            if (modbusStatusSSECallback) modbusStatusSSECallback(data);
        });
    }
    
    bool ok = modbusHandler->begin(modbusConfig);
    if (ok) {
        LOG_INFOF("Protocol Manager: Modbus restarted in %s mode, %d tasks, %d devices",
                  (modbusConfig.mode == MODBUS_MASTER) ? "Master" : "Slave",
                  modbusConfig.master.taskCount,
                  modbusConfig.master.deviceCount);
        
        // 注册 Modbus 子设备为标准外设并设置通信回调
        registerModbusSubDevices(modbusConfig);
    } else {
        LOG_WARNING("Protocol Manager: Modbus restart failed");
    }
    return ok;
}

void ProtocolManager::stopModbus() {
    LOG_INFO("Protocol Manager: Stopping Modbus...");
    PeriphExecManager::getInstance().setModbusReadCallback(nullptr);
    
    // 注销 Modbus 外设和通信回调
    unregisterModbusSubDevices();
    
    if (modbusHandler) {
        modbusHandler->end();
    }
}

// ========== MQTT触发的Modbus一次性读取 ==========

String ProtocolManager::executeModbusRead(const String& paramsJson) {
    FastBeeJsonDoc resultDoc;
    JsonArray resultArr = resultDoc.to<JsonArray>();

    // 检查 Modbus 是否可用
    if (!modbusHandler) {
        JsonObject err = resultArr.add<JsonObject>();
        err["id"] = "modbus_read";
        err["value"] = "0";
        err["remark"] = "error:not_initialized";
        String out;
        serializeJson(resultDoc, out);
        return out;
    }

    if (modbusHandler->getMode() != MODBUS_MASTER) {
        JsonObject err = resultArr.add<JsonObject>();
        err["id"] = "modbus_read";
        err["value"] = "0";
        err["remark"] = "error:not_master_mode";
        String out;
        serializeJson(resultDoc, out);
        return out;
    }

    // 解析参数 JSON
    JsonDocument paramsDoc;
    DeserializationError parseErr = deserializeJson(paramsDoc, paramsJson);
    if (parseErr) {
        LOG_WARNINGF("[Modbus] OneShot: JSON parse error: %s", parseErr.c_str());
        JsonObject err = resultArr.add<JsonObject>();
        err["id"] = "modbus_read";
        err["value"] = "0";
        err["remark"] = "error:invalid_params";
        String out;
        serializeJson(resultDoc, out);
        return out;
    }

    // 获取 tasks 数组（支持三种格式：{tasks:[...]}, [...], {slaveAddress:...}）
    JsonDocument tasksDoc;
    JsonArray tasks;
    if (paramsDoc.is<JsonObject>() && paramsDoc["tasks"].is<JsonArray>()) {
        tasks = paramsDoc["tasks"].as<JsonArray>();
    } else if (paramsDoc.is<JsonArray>()) {
        tasks = paramsDoc.as<JsonArray>();
    } else if (paramsDoc.is<JsonObject>() && paramsDoc.containsKey("slaveAddress")) {
        // 单个任务对象，包装为数组
        JsonArray arr = tasksDoc.to<JsonArray>();
        arr.add(paramsDoc);
        tasks = arr;
    }

    if (tasks.isNull() || tasks.size() == 0) {
        JsonObject err = resultArr.add<JsonObject>();
        err["id"] = "modbus_read";
        err["value"] = "0";
        err["remark"] = "error:invalid_params";
        String out;
        serializeJson(resultDoc, out);
        return out;
    }

    LOG_INFOF("[Modbus] OneShot: executing %d task(s) from MQTT command", tasks.size());

    // 逐任务执行
    for (JsonVariant tv : tasks) {
        JsonObject t = tv.as<JsonObject>();
        uint8_t  slaveAddr = t["slaveAddress"] | (uint8_t)1;
        uint8_t  fc        = t["functionCode"] | (uint8_t)0x03;
        uint16_t startAddr = t["startAddress"] | (uint16_t)0;
        uint16_t qty       = t["quantity"] | (uint16_t)1;

        // 执行一次性读取
        OneShotResult readResult = modbusHandler->readRegistersOnce(slaveAddr, fc, startAddr, qty);

        if (readResult.error != ONESHOT_SUCCESS) {
            // 生成错误响应
            JsonObject err = resultArr.add<JsonObject>();
            err["id"] = "modbus_read";
            err["value"] = "0";
            switch (readResult.error) {
                case ONESHOT_TIMEOUT:
                    err["remark"] = "error:timeout";
                    break;
                case ONESHOT_CRC_ERROR:
                    err["remark"] = "error:crc_error";
                    break;
                case ONESHOT_EXCEPTION: {
                    char buf[24];
                    snprintf(buf, sizeof(buf), "error:exception:0x%02X", readResult.exceptionCode);
                    err["remark"] = buf;
                    break;
                }
                case ONESHOT_BUSY:
                    err["remark"] = "error:busy";
                    break;
                default:
                    err["remark"] = "error:unknown";
                    break;
            }
            continue;
        }

        // 读取成功，根据传输类型决定上报格式
        uint8_t transferType = modbusHandler->getConfig().transferType;
        
        if (transferType == 1) {
            // 透传模式(RAW HEX)：重构响应帧为十六进制字符串
            String hexStr = modbusHandler->formatRawHex(slaveAddr, fc, readResult.data, readResult.count);
            JsonObject item = resultArr.add<JsonObject>();
            item["id"] = "modbus_raw";
            item["value"] = hexStr;
            item["remark"] = "";
        } else if (t.containsKey("mappings") && t["mappings"].is<JsonArray>()) {
            // JSON模式 + 有映射：应用映射转换
            JsonArray mappings = t["mappings"].as<JsonArray>();
            for (JsonVariant mv : mappings) {
                JsonObject m = mv.as<JsonObject>();
                uint8_t regOffset    = m["regOffset"] | (uint8_t)0;
                uint8_t dataType     = m["dataType"] | (uint8_t)0;
                float   scaleFactor  = m["scaleFactor"] | 1.0f;
                uint8_t decimalPlaces = m["decimalPlaces"] | (uint8_t)1;
                const char* sensorId = m["sensorId"] | "unknown";

                if (regOffset >= readResult.count) continue;

                // 数据类型转换（与 reportPollData 逻辑一致）
                float rawValue = 0;
                switch (dataType) {
                    case 0: // uint16
                        rawValue = (float)readResult.data[regOffset];
                        break;
                    case 1: // int16
                        rawValue = (float)(int16_t)readResult.data[regOffset];
                        break;
                    case 2: // uint32 (高字在前)
                        if (regOffset + 1 < readResult.count) {
                            uint32_t u32 = ((uint32_t)readResult.data[regOffset] << 16) | readResult.data[regOffset + 1];
                            rawValue = (float)u32;
                        }
                        break;
                    case 3: // int32 (高字在前)
                        if (regOffset + 1 < readResult.count) {
                            uint32_t u32 = ((uint32_t)readResult.data[regOffset] << 16) | readResult.data[regOffset + 1];
                            rawValue = (float)(int32_t)u32;
                        }
                        break;
                    case 4: // float32 (IEEE 754)
                        if (regOffset + 1 < readResult.count) {
                            uint32_t u32 = ((uint32_t)readResult.data[regOffset] << 16) | readResult.data[regOffset + 1];
                            memcpy(&rawValue, &u32, sizeof(float));
                        }
                        break;
                    default:
                        rawValue = (float)readResult.data[regOffset];
                        break;
                }

                float scaledValue = rawValue * scaleFactor;
                char valBuf[16];
                dtostrf(scaledValue, 1, decimalPlaces, valBuf);

                JsonObject item = resultArr.add<JsonObject>();
                item["id"] = sensorId;
                item["value"] = valBuf;
                item["remark"] = "";
            }
        } else {
            // 无映射：返回原始寄存器值
            for (uint16_t i = 0; i < readResult.count; i++) {
                JsonObject item = resultArr.add<JsonObject>();
                char idBuf[24];
                snprintf(idBuf, sizeof(idBuf), "reg_%d", startAddr + i);
                item["id"] = idBuf;
                item["value"] = String(readResult.data[i]);
                item["remark"] = "";
            }
        }
    }

    String out;
    serializeJson(resultDoc, out);
    LOG_INFOF("[Modbus] OneShot: result: %s", out.c_str());
    return out;
}

// 具体协议初始化实现
bool ProtocolManager::initMQTT(void* config) {
    if (!config) return false;
    
    MQTTConfig* mqttConfig = static_cast<MQTTConfig*>(config);
    
    // 使用自定义的 make_unique 实现
    mqttClient = std::unique_ptr<MQTTClient>(new MQTTClient());
    
    // 设置回调
    mqttClient->setMessageCallback([this](const String& topic, const String& message, MqttTopicType tType) {
        handleMessage(ProtocolType::MQTT, topic, message);
    });

    // 设置状态变化回调（SSE 推送用）
    if (mqttStatusSSECallback) {
        mqttClient->setStatusChangeCallback([this](const String& data) {
            if (mqttStatusSSECallback) mqttStatusSSECallback(data);
        });
    }

    return mqttClient->begin();
}

bool ProtocolManager::initModbus(void* config) {
    if (!config) return false;
    
    ModbusConfig* modbusConfig = static_cast<ModbusConfig*>(config);
    modbusHandler = std::unique_ptr<ModbusHandler>(new ModbusHandler());
    
    // 设置回调
    modbusHandler->setDataCallback([this](uint8_t address, const String& data) {
        // 1. MQTT 上报（JSON 和透传都上报）
        if (mqttClient && mqttClient->getIsConnected()) {
            mqttClient->publishReportData(data);
        }
        // 2. SSE 实时推送
        if (sseCallback) {
            sseCallback(address, data);
        }
        // 3. JSON 格式数据注入 PeriphExec 进行条件匹配
        if (data.length() > 0 && data[0] == '[') {
            PeriphExecManager::getInstance().handleMqttMessage("modbus/data", data);
            // 4. 轮询触发：本地数据源条件评估（POLL_TRIGGER 规则）
            PeriphExecManager::getInstance().handlePollData("modbus", data);
        }
        // 5. 保持全局消息路由
        handleMessage(ProtocolType::MODBUS, String(address), data);
    });

    // 设置状态变化回调（SSE 推送用）
    if (modbusStatusSSECallback) {
        modbusHandler->setStatusChangeCallback([this](const String& data) {
            if (modbusStatusSSECallback) modbusStatusSSECallback(data);
        });
    }

    bool initOk = modbusHandler->begin(*modbusConfig);
    if (initOk) {
        // 注册 Modbus 子设备为标准外设并设置通信回调
        registerModbusSubDevices(*modbusConfig);
    }
    return initOk;
}

#if FASTBEE_ENABLE_TCP
bool ProtocolManager::initTCP(void* config) {
    if (!config) return false;
    
    TCPConfig* tcpConfig = static_cast<TCPConfig*>(config);
    tcpHandler = std::unique_ptr<TCPHandler>(new TCPHandler());
    
    // 设置回调
    tcpHandler->setMessageCallback([this](const String& message) {
        handleMessage(ProtocolType::TCP, "tcp", message);
    });
    
    return tcpHandler->begin(*tcpConfig);
}
#endif

#if FASTBEE_ENABLE_HTTP
bool ProtocolManager::initHTTP(void* config) {
    if (!config) return false;
    
    HTTPConfig* httpConfig = static_cast<HTTPConfig*>(config);
    httpClientWrapper = std::unique_ptr<HTTPClientWrapper>(new HTTPClientWrapper());
    
    // 设置回调
    httpClientWrapper->setResponseCallback([this](const String& endpoint, const String& response) {
        handleMessage(ProtocolType::HTTP, endpoint, response);
    });
    
    return httpClientWrapper->begin(*httpConfig);
}
#endif

#if FASTBEE_ENABLE_COAP
bool ProtocolManager::initCoAP(void* config) {
    if (!config) return false;
    
    CoAPConfig* coapConfig = static_cast<CoAPConfig*>(config);
    coapHandler = std::unique_ptr<CoAPHandler>(new CoAPHandler());
    
    // 设置回调
    coapHandler->setMessageCallback([this](const String& resource, const String& message) {
        handleMessage(ProtocolType::COAP, resource, message);
    });
    
    return coapHandler->begin(*coapConfig);
}
#endif

// ========== Modbus 子设备注册到 PeripheralManager ==========

void ProtocolManager::registerModbusSubDevices(const ModbusConfig& config) {
    PeripheralManager& pm = PeripheralManager::getInstance();
    
    // 先清理旧的 Modbus 外设（重启 Modbus 时需要先注销）
    unregisterModbusSubDevices();
    
    if (config.mode != MODBUS_MASTER || config.master.deviceCount == 0) {
        return;
    }
    
    // 设置 Modbus 通信回调（通过闭包捕获 modbusHandler）
    ModbusHandler* mb = modbusHandler.get();
    if (mb) {
        pm.setModbusCallbacks(
            // 线圈写入回调 (FC05) — 使用控制优先级通道，避免与轮询竞争
            [mb](uint8_t slaveAddr, uint16_t coilAddr, bool value) -> bool {
                OneShotResult result = mb->writeCoilOnce(slaveAddr, coilAddr, value, true);
                return result.error == ONESHOT_SUCCESS;
            },
            // 寄存器写入回调 (FC06) — 使用控制优先级通道
            [mb](uint8_t slaveAddr, uint16_t regAddr, uint16_t value) -> bool {
                OneShotResult result = mb->writeRegisterOnce(slaveAddr, regAddr, value, true);
                return result.error == ONESHOT_SUCCESS;
            }
        );
    }
    
    // 逐个注册子设备为标准外设
    for (uint8_t i = 0; i < config.master.deviceCount; i++) {
        const ModbusSubDevice& dev = config.master.devices[i];
        if (!dev.enabled) continue;
        
        PeripheralConfig pCfg;
        pCfg.id = "modbus:" + String(i);
        pCfg.name = String(dev.name);
        pCfg.type = PeripheralType::MODBUS_DEVICE;
        pCfg.enabled = true;
        pCfg.pinCount = 0;  // Modbus 外设不使用 GPIO 引脚
        
        // 设置 Modbus 特定参数
        pCfg.params.modbus.slaveAddress    = dev.slaveAddress;
        pCfg.params.modbus.channelCount    = dev.channelCount;
        pCfg.params.modbus.coilBase        = dev.coilBase;
        pCfg.params.modbus.ncMode          = dev.ncMode;
        pCfg.params.modbus.controlProtocol = dev.controlProtocol;
        pCfg.params.modbus.deviceIndex     = i;
        pCfg.params.modbus.batchRegister   = dev.batchRegister;
        pCfg.params.modbus.pwmRegBase      = dev.pwmRegBase;
        pCfg.params.modbus.pwmResolution   = dev.pwmResolution;
        strncpy(pCfg.params.modbus.sensorId, dev.sensorId, sizeof(pCfg.params.modbus.sensorId) - 1);
        pCfg.params.modbus.sensorId[sizeof(pCfg.params.modbus.sensorId) - 1] = '\0';
        
        // 确定设备类型
        String devType(dev.deviceType);
        if (devType == "relay")          pCfg.params.modbus.deviceType = 0;
        else if (devType == "pwm")      pCfg.params.modbus.deviceType = 1;
        else if (devType == "pid")      pCfg.params.modbus.deviceType = 2;
        else if (devType == "motor")    pCfg.params.modbus.deviceType = 3;
        else                            pCfg.params.modbus.deviceType = 0;
        
        // 电机参数
        memcpy(pCfg.params.modbus.motorRegs, dev.motorRegs, sizeof(dev.motorRegs));
        pCfg.params.modbus.motorDecimals = dev.motorDecimals;
        
        if (pm.addPeripheral(pCfg)) {
            LOG_INFOF("Protocol Manager: Registered Modbus sub-device '%s' as peripheral 'modbus:%d'",
                      dev.name, i);
        } else {
            LOG_WARNINGF("Protocol Manager: Failed to register Modbus sub-device '%s'", dev.name);
        }
    }
}

void ProtocolManager::unregisterModbusSubDevices() {
    PeripheralManager& pm = PeripheralManager::getInstance();
    
    // 清除通信回调
    pm.clearModbusCallbacks();
    
    // 移除所有 MODBUS_DEVICE 类型的外设
    auto allPeriphs = pm.getPeripheralsByType(PeripheralType::MODBUS_DEVICE);
    for (const auto& p : allPeriphs) {
        pm.removePeripheral(p.id);
        LOG_INFOF("Protocol Manager: Unregistered Modbus peripheral '%s'", p.id.c_str());
    }
}
