#ifndef PROTOCOL_MANAGER_H
#define PROTOCOL_MANAGER_H

#include <Arduino.h>
#include <memory>
#include <vector>
#include "MQTTClient.h"
#include "ModbusHandler.h"
#include "TCPHandler.h"
#include "CoAPHandler.h"
#include "HTTPClientWrapper.h"

// 协议类型枚举
enum class ProtocolType {
    MQTT,
    MODBUS,
    TCP,
    HTTP,
    COAP
};

// 协议配置结构体
struct ProtocolConfig {
    ProtocolType type;
    String name;
    bool enabled;
    void* config;  // 指向具体协议的配置
};

// 回调函数类型定义
typedef std::function<void(ProtocolType, const String&, const String&)> MessageCallback;

class ProtocolManager {
public:
    ProtocolManager();
    ~ProtocolManager();

    // 初始化协议管理器
    bool initialize();
    
    // 添加协议
    bool addProtocol(ProtocolType type, const String& name, bool enabled = true);
    
    // 设置协议配置
    bool setProtocolConfig(ProtocolType type, void* config);
    
    // 启动所有协议
    bool startAll();
    
    // 停止所有协议
    void stopAll();
    
    // 发送数据
    bool sendData(ProtocolType type, const String& topic, const String& data);
    
    // 注册消息回调
    void setMessageCallback(MessageCallback callback);
    
    // 获取协议状态
    String getProtocolStatus(ProtocolType type);
    
    // 处理循环（需要在 loop 中调用）
    void handle();

    /**
     * @brief 完全关闭协议管理器，释放所有资源
     */
    void shutdown();

private:
    // unique_ptr 智能指针，主要用于自动管理动态内存，避免内存泄漏并确保资源安全释放
    std::unique_ptr<MQTTClient> mqttClient;
    std::unique_ptr<ModbusHandler> modbusHandler;
    std::unique_ptr<TCPHandler> tcpHandler;
    std::unique_ptr<HTTPClientWrapper> httpClientWrapper;
    std::unique_ptr<CoAPHandler> coapHandler;
    
    std::vector<ProtocolConfig> protocols;
    MessageCallback messageCallback;
    
    bool isInitialized;
    
    // 初始化具体协议
    bool initMQTT(void* config);
    bool initModbus(void* config);
    bool initTCP(void* config);
    bool initHTTP(void* config);
    bool initCoAP(void* config);
    
    // 内部消息处理
    void handleMessage(ProtocolType type, const String& topic, const String& message);
};

#endif
