#ifndef TCPHANDLER_H
#define TCPHANDLER_H

#include <WiFi.h>
#include <functional>
#include <vector>
#include <core/SystemConstants.h>

struct TCPConfig {
    bool isServer = false;
    String server = "";
    uint16_t port = 8080;
    uint16_t localPort = 8080;
};

class TCPHandler {
public:
    TCPHandler();
    ~TCPHandler();
    
    // 从LittleFS加载配置
    bool loadConfigFromLittleFS(const char* configPath = FileSystem::TCP_CONFIG_FILE);
    
    // 初始化方法
    bool begin(const TCPConfig& config);
    bool beginFromConfig(const char* configPath = FileSystem::TCP_CONFIG_FILE);
    
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
    WiFiServer* tcpServer;
    WiFiClient tcpClient;
    std::vector<WiFiClient> serverClients;
    bool isConnected;
    std::function<void(const String&)> messageCallback;
};

#endif