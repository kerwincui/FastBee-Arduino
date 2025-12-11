/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:31:37
 */

#include "protocols/TCPHandler.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

TCPHandler::TCPHandler() 
    : tcpServer(nullptr), isConnected(false) {
}

TCPHandler::~TCPHandler() {
    disconnect();
}

bool TCPHandler::loadConfigFromLittleFS(const char* configPath) {
    if (!LittleFS.begin()) {
        Serial.println("TCP Handler: Failed to mount LittleFS");
        return false;
    }
    
    File configFile = LittleFS.open(configPath, "r");
    if (!configFile) {
        Serial.println("TCP Handler: Failed to open config file: " + String(configPath));
        LittleFS.end();
        return false;
    }
    
    size_t size = configFile.size();
    if (size == 0) {
        Serial.println("TCP Handler: Config file is empty");
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
        Serial.println("TCP Handler: Failed to parse config file: " + String(error.c_str()));
        return false;
    }
    
    // 从JSON中读取配置
    config.isServer = doc["isServer"] | false;
    config.server = doc["server"] | "";
    config.port = doc["port"] | 8080;
    config.localPort = doc["localPort"] | 8080;
    
    Serial.println("TCP Handler: Configuration loaded from LittleFS:");
    Serial.println("  Mode: " + String(config.isServer ? "Server" : "Client"));
    Serial.println("  Server: " + config.server);
    Serial.println("  Port: " + String(config.port));
    Serial.println("  Local Port: " + String(config.localPort));
    
    return true;
}

bool TCPHandler::begin(const TCPConfig& config) {
    this->config = config;
    
    if (config.isServer) {
        tcpServer = new WiFiServer(config.localPort);
        tcpServer->begin();
        Serial.println("TCP Handler: Started as server on port " + String(config.localPort));
    }
    
    return true;
}

bool TCPHandler::beginFromConfig(const char* configPath) {
    if (!loadConfigFromLittleFS(configPath)) {
        return false;
    }
    
    return begin(config);
}

bool TCPHandler::connect() {
    if (config.isServer) {
        return true; // 服务器模式不需要连接
    }
    
    Serial.println("TCP Handler: Connecting to " + config.server + ":" + String(config.port));
    
    if (tcpClient.connect(config.server.c_str(), config.port)) {
        isConnected = true;
        Serial.println("TCP Handler: Connected successfully");
        return true;
    } else {
        Serial.println("TCP Handler: Connection failed");
        return false;
    }
}

void TCPHandler::disconnect() {
    if (tcpClient.connected()) {
        tcpClient.stop();
        isConnected = false;
        Serial.println("TCP Handler: Disconnected");
    }
    
    if (tcpServer) {
        tcpServer->stop();
        delete tcpServer;
        tcpServer = nullptr;
    }
}

bool TCPHandler::send(const String& data) {
    if (config.isServer) {
        // 服务器模式：向所有连接的客户端发送数据
        bool sentToAny = false;
        for (auto& client : serverClients) {
            if (client.connected()) {
                client.print(data);
                sentToAny = true;
            }
        }
        return sentToAny;
    } else {
        // 客户端模式
        if (tcpClient.connected()) {
            size_t sent = tcpClient.print(data);
            return sent == data.length();
        }
        return false;
    }
}

void TCPHandler::handle() {
    if (config.isServer) {
        handleServer();
    } else {
        handleClient();
    }
}

String TCPHandler::getStatus() const {
    if (config.isServer) {
        return "Server mode, clients: " + String(serverClients.size());
    } else {
        return isConnected ? "Connected" : "Disconnected";
    }
}

void TCPHandler::setMessageCallback(std::function<void(const String&)> callback) {
    this->messageCallback = callback;
}

void TCPHandler::handleServer() {
    if (!tcpServer) return;
    
    // 处理新客户端连接
    if (tcpServer->hasClient()) {
        WiFiClient newClient = tcpServer->available();
        serverClients.push_back(newClient);
        Serial.println("TCP Handler: New client connected");
    }
    
    // 处理客户端数据
    for (size_t i = 0; i < serverClients.size(); i++) {
        if (serverClients[i].connected()) {
            while (serverClients[i].available()) {
                String message = serverClients[i].readString();
                Serial.println("TCP Handler: Received from client: " + message);
                
                if (messageCallback) {
                    messageCallback(message);
                }
            }
        } else {
            // 移除断开的客户端
            serverClients.erase(serverClients.begin() + i);
            i--;
            Serial.println("TCP Handler: Client disconnected");
        }
    }
}

void TCPHandler::handleClient() {
    if (!isConnected) {
        // 添加重连延迟，避免过于频繁的重连尝试
        static unsigned long lastReconnectAttempt = 0;
        if (millis() - lastReconnectAttempt > 5000) { // 每5秒尝试重连一次
            connect();
            lastReconnectAttempt = millis();
        }
        return;
    }
    
    // 处理接收数据
    while (tcpClient.available()) {
        String message = tcpClient.readString();
        Serial.println("TCP Handler: Received: " + message);
        
        if (messageCallback) {
            messageCallback(message);
        }
    }
    
    // 检查连接状态
    if (!tcpClient.connected()) {
        isConnected = false;
        Serial.println("TCP Handler: Connection lost");
    }
}

const TCPConfig& TCPHandler::getConfig() const {
    return config;
}