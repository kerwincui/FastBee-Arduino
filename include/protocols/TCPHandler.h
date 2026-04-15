#ifndef TCPHANDLER_H
#define TCPHANDLER_H

#include "core/FeatureFlags.h"

#if FASTBEE_ENABLE_TCP

#include <WiFi.h>
#include <functional>
#include <vector>
#include <memory>
#include <core/SystemConstants.h>
#include "systems/LoggerSystem.h"

struct TCPConfig {
    bool isServer = false;
    String server = "";
    uint16_t port = 8080;
    uint16_t localPort = 8080;
    uint32_t keepAliveInterval = 30000;  // 心跳间隔 (ms)
    uint32_t idleTimeout = 120000;       // 空闲超时 (ms)
    uint8_t maxClients = 5;              // 最大客户端连接数
    String keepAliveMessage = "\n";      // 心跳数据
};

struct ClientInfo {
    WiFiClient client;
    unsigned long lastActivityTime;
    unsigned long lastKeepAliveTime;
};

class TCPHandler {
public:
    TCPHandler();
    ~TCPHandler();
    
    // 从LittleFS加载配置
    bool loadConfigFromLittleFS(const char* configPath = FileSystem::PROTOCOL_CONFIG_FILE);
    
    // 初始化方法
    bool begin(const TCPConfig& config);
    bool beginFromConfig(const char* configPath = FileSystem::PROTOCOL_CONFIG_FILE);
    
    // 连接管理
    bool connect();
    void disconnect();
    
    // 数据发送
    bool send(const String& data);
    
    // 处理循环
    void handle();
    
    // 状态获取
    String getStatus() const;
    const TCPConfig& getConfig() const;
    
    // 回调设置
    void setMessageCallback(std::function<void(const String&)> callback);

private:
    void handleServer();
    void handleClient();
    
    TCPConfig config;
    std::unique_ptr<WiFiServer> tcpServer;
    WiFiClient tcpClient;
    std::vector<ClientInfo> serverClients;
    bool isConnected;
    std::function<void(const String&)> messageCallback;
    
    // 客户端模式时间戳
    unsigned long clientLastActivity;
    unsigned long clientLastKeepAlive;
};

#else // !FASTBEE_ENABLE_TCP

#include <Arduino.h>

struct TCPConfig {
    bool isServer = false;
    String server = "";
    uint16_t port = 8080;
    uint16_t localPort = 8080;
};

class TCPHandler {
public:
    bool initialize() { return true; }
    void shutdown() {}
    void handle() {}
    bool isRunning() const { return false; }
    bool loadConfig() { return true; }
    const TCPConfig& getConfig() const { static TCPConfig c; return c; }
};

#endif // FASTBEE_ENABLE_TCP

#endif
