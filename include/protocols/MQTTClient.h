#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_random.h>
#include <vector>
#include <core/SystemConstants.h>

// 主题类型枚举
enum class MqttTopicType : uint8_t {
    DATA_REPORT   = 0,  // 数据上报
    DATA_COMMAND  = 1,  // 数据下发
    DEVICE_INFO   = 2,  // 设备信息
    REALTIME_MON  = 3,  // 实时监测
    DEVICE_EVENT  = 4,  // 设备事件
    OTA_UPGRADE   = 5,  // OTA升级
    OTA_BINARY    = 6   // OTA二进制
};

// MQTT认证类型枚举
enum class MqttAuthType : uint8_t {
    SIMPLE    = 0,  // 简单认证 (S) - 密码直接传输
    ENCRYPTED = 1   // 加密认证 (E) - AES-CBC-128加密密码
};

// 发布主题配置结构体
struct MqttPublishTopic {
    String topic;
    uint8_t qos;
    bool retain;
    String content;
    MqttTopicType topicType;
    
    MqttPublishTopic() : qos(0), retain(false), topicType(MqttTopicType::DATA_REPORT) {}
};

// 订阅主题配置结构体
struct MqttSubscribeTopic {
    String topic;       // 订阅主题
    uint8_t qos;        // QoS等级
    String action;      // 执行字段，定义接收到消息时的处理逻辑
    MqttTopicType topicType;
    
    MqttSubscribeTopic() : qos(0), topicType(MqttTopicType::DATA_COMMAND) {}
};

// MQTT配置结构体
struct MQTTConfig {
    String server;
    uint16_t port;
    String clientId;
    String username;
    String password;
    String topicPrefix;
    String subscribeTopic;      // 兼容旧配置的单一订阅主题
    uint16_t keepAlive;
    // 新增字段
    int accessMode;            // 接入模式: 0=MQTT直连, 1=MQTT透传
    bool autoReconnect;        // 自动重连
    uint32_t connectionTimeout;// 连接超时（毫秒）
    // 认证配置
    MqttAuthType authType;     // 认证类型: 简单认证(S) / 加密认证(E)
    String deviceNum;          // 设备编号
    String productId;          // 产品ID
    String userId;             // 用户ID
    String mqttSecret;         // 产品秘钥（AES加密密钥，16字节）
    String authCode;           // 设备授权码（可选）
    String ntpServer;          // NTP服务器地址
    // 遗嘱消息配置
    String willTopic;
    String willPayload;
    uint8_t willQos = 0;
    bool willRetain = false;
    // 发布主题配置（支持多组）
    std::vector<MqttPublishTopic> publishTopics;
    // 订阅主题配置（支持多组）
    std::vector<MqttSubscribeTopic> subscribeTopics;

    // 默认构造函数
    MQTTConfig() : port(1883), keepAlive(60), 
                   accessMode(0), autoReconnect(true), 
                   connectionTimeout(30000), authType(MqttAuthType::SIMPLE),
                   willQos(0), willRetain(false) {}
};

class MQTTClient {
public:
    MQTTClient();
    ~MQTTClient();

    bool loadMqttConfig(const String& filename = FileSystem::PROTOCOL_CONFIG_FILE); 
    bool begin();
    bool connect();
    void disconnect();
    void stop();           // 显式停止MQTT（断开并阻止自动重连）
    bool publish(const String& topic, const String& message);
    bool publishToTopic(size_t topicIndex, const String& message);
    bool subscribe(const String& topic);
    bool subscribeAll();  // 订阅所有配置的主题
    void handle();
    String getStatus() const;
    
    // 获取配置引用
    const MQTTConfig& getConfig() const { return config; }
    
    // 详细状态信息
    bool getIsConnected() const { return isConnected; }
    int  getLastErrorCode() const { return lastErrorCode; }
    uint32_t getReconnectCount() const { return reconnectCount; }
    unsigned long getLastConnectedTime() const { return lastConnectedTime; }
    
    // 设置消息回调（topic, message, topicType）
    void setMessageCallback(std::function<void(const String&, const String&, MqttTopicType)> callback);

    // 根据主题路径查找对应的主题类型
    MqttTopicType getTopicTypeByPath(const String& topicPath) const;

private:
    WiFiClient wifiClient;
    PubSubClient mqttClient;
    MQTTConfig config;
    bool isConnected;
    bool stopped;                     // 是否被显式停止（阻止自动重连）
    unsigned long lastReconnectAttempt;
    unsigned long lastConnectedTime;  // 上次连接成功时间
    int  lastErrorCode;               // 上次错误码
    uint32_t reconnectCount;          // 重连次数
    
    std::function<void(const String&, const String&, MqttTopicType)> messageCallback;
    
    void mqttCallback(char* topic, byte* payload, unsigned int length);
    bool reconnect();
    
    // FastBee认证相关方法
    String buildClientId();            // 构建认证clientId: 类型&设备编号&产品ID&用户ID
    String buildSimplePassword();      // 简单认证密码: password 或 password&authCode
    String buildEncryptedPassword();   // 加密认证密码: AES-CBC-128加密
    String getNtpTime();               // 通过HTTP获取NTP时间
    String aesEncrypt(const String& plainData, const String& key, const String& iv); // AES-CBC-128加密
};

#endif