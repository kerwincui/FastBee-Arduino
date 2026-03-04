/**
 *@description: 协议管理器实现
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:31:25
 */

#include "protocols/ProtocolManager.h"
#include "systems/LoggerSystem.h"
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
    isInitialized   = false;

    LOG_INFO("Protocol Manager: Shutdown complete");
}

bool ProtocolManager::sendData(ProtocolType type, const String& topic, const String& data) {
    switch (type) {
        case ProtocolType::MQTT:
            return mqttClient && mqttClient->publish(topic, data);
        case ProtocolType::MODBUS:
            return modbusHandler && modbusHandler->writeData(topic.toInt(), data);
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
    if (messageCallback) {
        messageCallback(type, topic, message);
    }
}

// 具体协议初始化实现
bool ProtocolManager::initMQTT(void* config) {
    if (!config) return false;
    
    MQTTConfig* mqttConfig = static_cast<MQTTConfig*>(config);
    
    // 使用自定义的 make_unique 实现
    mqttClient = std::unique_ptr<MQTTClient>(new MQTTClient());
    
    // 设置回调
    mqttClient->setMessageCallback([this](const String& topic, const String& message) {
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