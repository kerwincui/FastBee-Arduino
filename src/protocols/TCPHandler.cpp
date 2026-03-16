/**
 * @description: TCP 客户端/服务端处理器 - 带心跳与连接管理
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:31:37
 *
 * 支持: 双模式(Server/Client)、心跳保活、空闲超时、最大连接数限制
 */

#include "protocols/TCPHandler.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

TCPHandler::TCPHandler() 
    : tcpServer(nullptr), isConnected(false),
      clientLastActivity(0), clientLastKeepAlive(0) {
}

TCPHandler::~TCPHandler() {
    disconnect();
}

bool TCPHandler::loadConfigFromLittleFS(const char* configPath) {
    if (!LittleFS.begin()) {
        LOG_ERROR("TCP: Failed to mount LittleFS");
        return false;
    }
    
    File configFile = LittleFS.open(configPath, "r");
    if (!configFile) {
        LOG_ERRORF("TCP: Failed to open config file: %s", configPath);
        LittleFS.end();
        return false;
    }
    
    size_t size = configFile.size();
    if (size == 0) {
        LOG_WARNING("TCP: Config file is empty");
        configFile.close();
        LittleFS.end();
        return false;
    }
    
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, configFile);
    configFile.close();
    LittleFS.end();
    
    if (error) {
        LOG_ERRORF("TCP: Failed to parse config: %s", error.c_str());
        return false;
    }
    
    config.isServer = doc["isServer"] | false;
    config.server = doc["server"] | "";
    config.port = doc["port"] | 8080;
    config.localPort = doc["localPort"] | 8080;
    config.keepAliveInterval = doc["keepAliveInterval"] | 30000;
    config.idleTimeout = doc["idleTimeout"] | 120000;
    config.maxClients = doc["maxClients"] | 5;
    if (doc.containsKey("keepAliveMessage")) {
        config.keepAliveMessage = doc["keepAliveMessage"].as<String>();
    }
    
    LOG_INFO("TCP: Configuration loaded from LittleFS");
    LOG_INFOF("  Mode: %s, Server: %s, Port: %d",
              config.isServer ? "Server" : "Client",
              config.server.c_str(), config.port);
    LOG_INFOF("  Keepalive: %lums, IdleTimeout: %lums, MaxClients: %d",
              (unsigned long)config.keepAliveInterval,
              (unsigned long)config.idleTimeout,
              config.maxClients);
    
    return true;
}

bool TCPHandler::begin(const TCPConfig& config) {
    this->config = config;
    
    if (config.isServer) {
        tcpServer = std::unique_ptr<WiFiServer>(new WiFiServer(config.localPort));
        tcpServer->begin();
        LOG_INFOF("TCP: Started as server on port %d (max clients: %d)",
                  config.localPort, config.maxClients);
    } else {
        clientLastActivity = millis();
        clientLastKeepAlive = millis();
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
        return true;
    }
    
    LOG_INFOF("TCP: Connecting to %s:%d", config.server.c_str(), config.port);
    
    if (tcpClient.connect(config.server.c_str(), config.port)) {
        isConnected = true;
        clientLastActivity = millis();
        clientLastKeepAlive = millis();
        LOG_INFO("TCP: Connected successfully");
        return true;
    } else {
        LOG_WARNING("TCP: Connection failed");
        return false;
    }
}

void TCPHandler::disconnect() {
    if (tcpClient.connected()) {
        tcpClient.stop();
        isConnected = false;
        LOG_INFO("TCP: Client disconnected");
    }
    
    // 断开所有服务端客户端连接
    for (auto& info : serverClients) {
        if (info.client.connected()) {
            info.client.stop();
        }
    }
    serverClients.clear();
    
    if (tcpServer) {
        tcpServer->stop();
        tcpServer.reset();
        LOG_INFO("TCP: Server stopped");
    }
}

bool TCPHandler::send(const String& data) {
    if (config.isServer) {
        bool sentToAny = false;
        for (auto& info : serverClients) {
            if (info.client.connected()) {
                info.client.print(data);
                info.lastKeepAliveTime = millis(); // 数据发送隐含心跳
                sentToAny = true;
            }
        }
        return sentToAny;
    } else {
        if (tcpClient.connected()) {
            size_t sent = tcpClient.print(data);
            if (sent > 0) {
                clientLastKeepAlive = millis();
            }
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
        char buf[80];
        snprintf(buf, sizeof(buf), "Server mode, clients: %d/%d, keepalive: %lus",
                 (int)serverClients.size(), config.maxClients,
                 (unsigned long)(config.keepAliveInterval / 1000));
        return String(buf);
    } else {
        if (isConnected) {
            unsigned long idleSec = (millis() - clientLastActivity) / 1000;
            char buf[80];
            snprintf(buf, sizeof(buf), "Connected, idle: %lus, keepalive: %lus",
                     idleSec, (unsigned long)(config.keepAliveInterval / 1000));
            return String(buf);
        }
        return "Disconnected";
    }
}

void TCPHandler::setMessageCallback(std::function<void(const String&)> callback) {
    this->messageCallback = callback;
}

void TCPHandler::handleServer() {
    if (!tcpServer) return;
    
    // 接受新连接（检查最大连接数）
    if (tcpServer->hasClient()) {
        if ((uint8_t)serverClients.size() >= config.maxClients) {
            WiFiClient rejected = tcpServer->available();
            rejected.stop();
            LOG_WARNING("TCP: Max clients reached, rejected connection");
        } else {
            ClientInfo info;
            info.client = tcpServer->available();
            info.lastActivityTime = millis();
            info.lastKeepAliveTime = millis();
            serverClients.push_back(info);
            LOG_INFOF("TCP: Client connected, total: %d", (int)serverClients.size());
        }
    }
    
    unsigned long now = millis();
    
    // 反向遍历，安全删除
    for (int i = (int)serverClients.size() - 1; i >= 0; i--) {
        ClientInfo& ci = serverClients[i];
        
        // 检查连接状态
        if (!ci.client.connected()) {
            serverClients.erase(serverClients.begin() + i);
            LOG_INFOF("TCP: Client disconnected, remaining: %d", (int)serverClients.size());
            continue;
        }
        
        // 读取数据
        if (ci.client.available()) {
            String message = ci.client.readString();
            ci.lastActivityTime = now;
            
            if (messageCallback) {
                messageCallback(message);
            }
        }
        
        // 空闲超时检查
        if (config.idleTimeout > 0 && (now - ci.lastActivityTime) > config.idleTimeout) {
            LOG_INFOF("TCP: Client %d idle timeout, disconnecting", i);
            ci.client.stop();
            serverClients.erase(serverClients.begin() + i);
            continue;
        }
        
        // 心跳发送
        if (config.keepAliveInterval > 0 && (now - ci.lastKeepAliveTime) > config.keepAliveInterval) {
            size_t sent = ci.client.print(config.keepAliveMessage);
            if (sent > 0) {
                ci.lastKeepAliveTime = now;
            } else {
                LOG_WARNINGF("TCP: Keepalive failed for client %d", i);
                ci.client.stop();
                serverClients.erase(serverClients.begin() + i);
                continue;
            }
        }
    }
}

void TCPHandler::handleClient() {
    unsigned long now = millis();
    
    if (!tcpClient.connected()) {
        if (isConnected) {
            isConnected = false;
            LOG_WARNING("TCP: Connection lost");
        }
        // 重连逻辑（5秒间隔）
        static unsigned long lastReconnectAttempt = 0;
        if (now - lastReconnectAttempt > 5000) {
            connect();
            lastReconnectAttempt = now;
        }
        return;
    }
    
    // 读取数据
    while (tcpClient.available()) {
        String message = tcpClient.readString();
        clientLastActivity = now;
        
        if (messageCallback) {
            messageCallback(message);
        }
    }
    
    // 心跳发送
    if (config.keepAliveInterval > 0 && (now - clientLastKeepAlive) > config.keepAliveInterval) {
        size_t sent = tcpClient.print(config.keepAliveMessage);
        if (sent > 0) {
            clientLastKeepAlive = now;
        } else {
            LOG_WARNING("TCP: Keepalive send failed, connection may be broken");
            isConnected = false;
            return;
        }
    }
    
    // 空闲超时（客户端模式触发重连）
    if (config.idleTimeout > 0 && (now - clientLastActivity) > config.idleTimeout) {
        LOG_WARNING("TCP: Idle timeout, reconnecting");
        tcpClient.stop();
        isConnected = false;
    }
}

const TCPConfig& TCPHandler::getConfig() const {
    return config;
}
