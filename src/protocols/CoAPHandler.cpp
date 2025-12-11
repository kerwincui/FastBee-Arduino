/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:30:40
 */

#include "protocols/CoAPHandler.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

CoAPHandler::CoAPHandler() 
    : isInitialized(false), messageId(0) {
}

CoAPHandler::~CoAPHandler() {
    end();
}

bool CoAPHandler::loadConfigFromLittleFS(const char* configPath) {
    if (!LittleFS.begin()) {
        Serial.println("CoAP Handler: Failed to mount LittleFS");
        return false;
    }
    
    File configFile = LittleFS.open(configPath, "r");
    if (!configFile) {
        Serial.println("CoAP Handler: Failed to open config file: " + String(configPath));
        LittleFS.end();
        return false;
    }
    
    size_t size = configFile.size();
    if (size == 0) {
        Serial.println("CoAP Handler: Config file is empty");
        configFile.close();
        LittleFS.end();
        return false;
    }
    
    // 分配JSON文档内存
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, configFile);
    configFile.close();
    LittleFS.end();
    
    if (error) {
        Serial.println("CoAP Handler: Failed to parse config file: " + String(error.c_str()));
        return false;
    }
    
    // 从JSON中读取配置
    config.server = doc["server"] | "coap.me";
    config.port = doc["port"] | 5683;
    config.localPort = doc["localPort"] | 5683;
    
    // 读取可选的CoAP参数
    config.defaultMethod = doc["defaultMethod"] | "POST";
    config.timeout = doc["timeout"] | 5000;
    config.retransmitCount = doc["retransmitCount"] | 3;
    
    // 读取资源路径映射（如果有）
    if (doc.containsKey("resources")) {
        JsonObject resources = doc["resources"];
        for (JsonPair resource : resources) {
            config.resourceMap[String(resource.key().c_str())] = String(resource.value().as<const char*>());
        }
    }
    
    Serial.println("CoAP Handler: Configuration loaded from LittleFS:");
    Serial.println("  Server: " + config.server);
    Serial.println("  Port: " + String(config.port));
    Serial.println("  Local Port: " + String(config.localPort));
    Serial.println("  Default Method: " + config.defaultMethod);
    Serial.println("  Timeout: " + String(config.timeout));
    Serial.println("  Retransmit Count: " + String(config.retransmitCount));
    Serial.println("  Resources count: " + String(config.resourceMap.size()));
    
    return true;
}

bool CoAPHandler::begin(const CoAPConfig& config) {
    this->config = config;
    
    if (udp.begin(config.localPort)) {
        isInitialized = true;
        Serial.println("CoAP Handler: Initialized on port " + String(config.localPort));
        return true;
    }
    
    return false;
}

bool CoAPHandler::beginFromConfig(const char* configPath) {
    if (!loadConfigFromLittleFS(configPath)) {
        return false;
    }
    
    return begin(config);
}

void CoAPHandler::end() {
    if (isInitialized) {
        udp.stop();
        isInitialized = false;
        Serial.println("CoAP Handler: Stopped");
    }
}

bool CoAPHandler::send(const String& resource, const String& data) {
    if (!isInitialized) return false;
    
    // 根据配置的默认方法发送
    CoAPMethod method = CoAPMethod::POST;
    if (config.defaultMethod == "GET") method = CoAPMethod::GET;
    else if (config.defaultMethod == "PUT") method = CoAPMethod::PUT;
    else if (config.defaultMethod == "DELETE") method = CoAPMethod::DELETE;
    
    // 检查资源映射
    String actualResource = resource;
    if (config.resourceMap.find(resource) != config.resourceMap.end()) {
        actualResource = config.resourceMap[resource];
        Serial.println("CoAP Handler: Mapped resource '" + resource + "' to '" + actualResource + "'");
    }
    
    return sendCoAPMessage(CoAPType::CON, method, actualResource, data);
}

bool CoAPHandler::get(const String& resource, const String& query) {
    if (!isInitialized) return false;
    
    String fullResource = resource;
    if (!query.isEmpty()) {
        fullResource += "?" + query;
    }
    
    return sendCoAPMessage(CoAPType::CON, CoAPMethod::GET, fullResource, "");
}

bool CoAPHandler::put(const String& resource, const String& data) {
    if (!isInitialized) return false;
    
    return sendCoAPMessage(CoAPType::CON, CoAPMethod::PUT, resource, data);
}

bool CoAPHandler::post(const String& resource, const String& data) {
    if (!isInitialized) return false;
    
    return sendCoAPMessage(CoAPType::CON, CoAPMethod::POST, resource, data);
}

bool CoAPHandler::del(const String& resource) {
    if (!isInitialized) return false;
    
    return sendCoAPMessage(CoAPType::CON, CoAPMethod::DELETE, resource, "");
}

void CoAPHandler::handle() {
    if (!isInitialized) return;
    
    int packetSize = udp.parsePacket();
    if (packetSize) {
        processCoAPPacket();
    }
}

String CoAPHandler::getStatus() const {
    return isInitialized ? "Initialized" : "Not initialized";
}

void CoAPHandler::setMessageCallback(std::function<void(const String&, const String&)> callback) {
    this->messageCallback = callback;
}

const CoAPConfig& CoAPHandler::getConfig() const {
    return config;
}

bool CoAPHandler::sendCoAPMessage(CoAPType type, CoAPMethod method, const String& resource, const String& payload) {
    // 简化的CoAP报文构造
    uint8_t buffer[256];
    uint8_t index = 0;
    
    // CoAP首部
    buffer[index++] = (0x01 << 6) | ((uint8_t)type << 4) | 0; // 版本1，类型，令牌长度0
    buffer[index++] = (uint8_t)method; // 方法代码
    buffer[index++] = (generateMessageId() >> 8) & 0xFF; // 消息ID高字节
    buffer[index++] = generateMessageId() & 0xFF; // 消息ID低字节
    
    // 资源路径选项
    String path = resource;
    if (path.startsWith("/")) {
        path = path.substring(1); // 移除开头的斜杠
    }
    
    buffer[index++] = 0xB0 | path.length(); // 选项delta和长度
    for (size_t i = 0; i < path.length(); i++) {
        buffer[index++] = path[i];
    }
    
    // 负载标记
    if (payload.length() > 0) {
        buffer[index++] = 0xFF; // 负载标记
        for (size_t i = 0; i < payload.length(); i++) {
            if (index >= sizeof(buffer)) break; // 防止缓冲区溢出
            buffer[index++] = payload[i];
        }
    }
    
    // 发送数据
    if (!udp.beginPacket(config.server.c_str(), config.port)) {
        Serial.println("CoAP Handler: Failed to begin packet");
        return false;
    }
    
    udp.write(buffer, index);
    int result = udp.endPacket();
    
    if (result == 1) {
        Serial.println("CoAP Handler: Sent " + getMethodString(method) + " message to " + resource);
        return true;
    } else {
        Serial.println("CoAP Handler: Failed to send message, error: " + String(result));
        return false;
    }
}

void CoAPHandler::processCoAPPacket() {
    uint8_t buffer[256];
    int len = udp.read(buffer, sizeof(buffer));
    
    if (len > 0) {
        // 简化的CoAP报文解析
        String resource = "";
        String payload = "";
        uint8_t responseCode = 0;
        
        // 解析基本头部
        if (len >= 4) {
            uint8_t tokenLength = buffer[0] & 0x0F;
            responseCode = buffer[1]; // 响应代码
            
            uint8_t payloadStart = 4 + tokenLength;
            
            // 查找负载
            for (int i = payloadStart; i < len; i++) {
                if (buffer[i] == 0xFF && i + 1 < len) {
                    // 找到负载
                    for (int j = i + 1; j < len; j++) {
                        payload += (char)buffer[j];
                    }
                    break;
                }
            }
        }
        
        String responseInfo = "Response Code: " + String(responseCode);
        if (!payload.isEmpty()) {
            responseInfo += ", Payload: " + payload;
        }
        
        Serial.println("CoAP Handler: Received message - " + responseInfo);
        
        if (messageCallback) {
            messageCallback("coap", payload);
        }
    }
}

uint16_t CoAPHandler::generateMessageId() {
    messageId++;
    if (messageId == 0) messageId = 1; // 跳过0
    return messageId;
}

String CoAPHandler::getMethodString(CoAPMethod method) {
    switch (method) {
        case CoAPMethod::GET: return "GET";
        case CoAPMethod::POST: return "POST";
        case CoAPMethod::PUT: return "PUT";
        case CoAPMethod::DELETE: return "DELETE";
        default: return "UNKNOWN";
    }
}