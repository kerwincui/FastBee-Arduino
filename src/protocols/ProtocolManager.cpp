/**
 *@description: 协议管理器实现
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:31:25
 */

#include "protocols/ProtocolManager.h"
#include "systems/LoggerSystem.h"
#include "core/PeriphExecManager.h"
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
                case ProtocolType::TCP:
                    return initTCP(config);
                case ProtocolType::HTTP:
                    return initHTTP(config);
                case ProtocolType::COAP:
                    return initCoAP(config);
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
            case ProtocolType::MQTT:   started = mqttClient        && mqttClient->begin();             break;
            case ProtocolType::MODBUS: started = modbusHandler     && modbusHandler->begin();          break;
            case ProtocolType::TCP:    started = tcpHandler         && tcpHandler->beginFromConfig();   break;
            case ProtocolType::HTTP:   started = httpClientWrapper  && httpClientWrapper->beginFromConfig(); break;
            case ProtocolType::COAP:   started = coapHandler        && coapHandler->beginFromConfig();  break;
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
    if (tcpHandler)       tcpHandler->disconnect();
    if (httpClientWrapper) httpClientWrapper->end();
    if (coapHandler)      coapHandler->end();
}

void ProtocolManager::shutdown() {
    LOG_INFO("Protocol Manager: Shutting down...");

    stopAll();

    mqttClient.reset();
    modbusHandler.reset();
    tcpHandler.reset();
    httpClientWrapper.reset();
    coapHandler.reset();

    protocols.clear();
    messageCallback = nullptr;
    txCallback = nullptr;
    rxCallback = nullptr;
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
            return tcpHandler && tcpHandler->send(data);
        case ProtocolType::HTTP:
            return httpClientWrapper && httpClientWrapper->post(topic, data);
        case ProtocolType::COAP:
            return coapHandler && coapHandler->send(topic, data);
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
            return tcpHandler ? tcpHandler->getStatus() : "Not initialized";
        case ProtocolType::HTTP:
            return httpClientWrapper ? httpClientWrapper->getStatus() : "Not initialized";
        case ProtocolType::COAP:
            return coapHandler ? coapHandler->getStatus() : "Not initialized";
        default:
            return "Unknown protocol";
    }
}

void ProtocolManager::handle() {
    if (mqttClient) mqttClient->handle();
    if (modbusHandler) modbusHandler->handle();
    if (tcpHandler) tcpHandler->handle();
    if (httpClientWrapper) httpClientWrapper->handle();
    if (coapHandler) coapHandler->handle();
}

void ProtocolManager::handleMessage(ProtocolType type, const String& topic, const String& message) {
    if (rxCallback) rxCallback();
    if (messageCallback) {
        messageCallback(type, topic, message);
    }
}

void ProtocolManager::setTxCallback(CounterCallback cb) { txCallback = cb; }
void ProtocolManager::setRxCallback(CounterCallback cb) { rxCallback = cb; }

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
    
    JsonDocument doc;
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
    modbusConfig.baudRate = rtu["baudRate"] | (uint32_t)9600;
    modbusConfig.txPin = 17;  // ESP32 Serial2 固定引脚
    modbusConfig.rxPin = 16;  // ESP32 Serial2 固定引脚
    int dePin = rtu["dePin"] | -1;
    modbusConfig.dePin = (dePin < 0) ? 255 : (uint8_t)dePin;
    modbusConfig.slaveAddress = rtu["slaveAddress"] | (uint8_t)1;
    modbusConfig.responseTimeout = rtu["timeout"] | (uint16_t)1000;
    modbusConfig.transferType = rtu["transferType"] | (uint8_t)0;
    
    String modeStr = rtu["mode"] | "slave";
    modbusConfig.mode = (modeStr == "master") ? MODBUS_MASTER : MODBUS_SLAVE;
    
    // 解析 Master 配置
    if (rtu.containsKey("master")) {
        JsonObject masterObj = rtu["master"];
        modbusConfig.master.defaultPollInterval = masterObj["defaultPollInterval"] | (uint16_t)30;
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
                task.pollInterval = t["pollInterval"] | modbusConfig.master.defaultPollInterval;
                task.enabled      = t["enabled"] | true;
                const char* lbl   = t["label"] | "";
                strncpy(task.label, lbl, sizeof(task.label) - 1);
                task.label[sizeof(task.label) - 1] = '\0';
                
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
                        task.mappingCount++;
                    }
                }
                
                modbusConfig.master.taskCount++;
            }
        }
    }
    
    // 创建新实例并设置回调
    modbusHandler = std::unique_ptr<ModbusHandler>(new ModbusHandler());
    
    modbusHandler->setDataCallback([this](uint8_t address, const String& data) {
        // 1. MQTT 上报
        if (mqttClient && mqttClient->getIsConnected()) {
            mqttClient->publishReportData(data);
        }
        // 2. JSON 格式数据注入 PeriphExec 进行条件匹配
        if (data.length() > 0 && data[0] == '[') {
            PeriphExecManager::getInstance().handleMqttMessage("modbus/data", data);
        }
        // 3. 全局消息路由
        handleMessage(ProtocolType::MODBUS, String(address), data);
    });
    
    bool ok = modbusHandler->begin(modbusConfig);
    if (ok) {
        LOG_INFOF("Protocol Manager: Modbus restarted in %s mode, %d tasks",
                  (modbusConfig.mode == MODBUS_MASTER) ? "Master" : "Slave",
                  modbusConfig.master.taskCount);
    } else {
        LOG_WARNING("Protocol Manager: Modbus restart failed");
    }
    return ok;
}

void ProtocolManager::stopModbus() {
    LOG_INFO("Protocol Manager: Stopping Modbus...");
    if (modbusHandler) {
        modbusHandler->end();
    }
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
        // 2. JSON 格式数据注入 PeriphExec 进行条件匹配
        if (data.length() > 0 && data[0] == '[') {
            PeriphExecManager::getInstance().handleMqttMessage("modbus/data", data);
        }
        // 3. 保持全局消息路由
        handleMessage(ProtocolType::MODBUS, String(address), data);
    });
    
    return modbusHandler->begin(*modbusConfig);
}

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