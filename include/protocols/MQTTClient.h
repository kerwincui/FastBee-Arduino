#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_random.h>
#include <vector>
#include <core/SystemConstants.h>

// 发布主题配置结构体
struct MqttPublishTopic {
    String topic;
    uint8_t qos;
    bool retain;
    String content;
    
    MqttPublishTopic() : qos(0), retain(false) {}
};

// MQTT配置结构体
struct MQTTConfig {
    String server;
    uint16_t port;
    String clientId;
    String username;
    String password;
    String topicPrefix;
    String subscribeTopic;
    uint16_t keepAlive;
    // 新增字段
    bool directConnect;        // 是否直连（绕过协议管理器）
    bool autoReconnect;        // 自动重连
    uint32_t connectionTimeout;// 连接超时（毫秒）
    // 发布主题配置（支持多组）
    std::vector<MqttPublishTopic> publishTopics;

    // 默认构造函数
    MQTTConfig() : port(1883), keepAlive(60), 
                   directConnect(true), autoReconnect(true), 
                   connectionTimeout(30000) {}
};

class MQTTClient {
public:
    MQTTClient();
    ~MQTTClient();

    bool loadMqttConfig(const String& filename = FileSystem::PROTOCOL_CONFIG_FILE); 
    bool begin();
    bool connect();
    void disconnect();
    bool publish(const String& topic, const String& message);
    bool publishToTopic(size_t topicIndex, const String& message);
    bool subscribe(const String& topic);
    void handle();
    String getStatus() const;
    
    // 获取配置引用
    const MQTTConfig& getConfig() const { return config; }
    
    // 设置消息回调
    void setMessageCallback(std::function<void(const String&, const String&)> callback);

private:
    WiFiClient wifiClient;
    PubSubClient mqttClient;
    MQTTConfig config;
    bool isConnected;
    unsigned long lastReconnectAttempt;
    
    std::function<void(const String&, const String&)> messageCallback;
    
    void mqttCallback(char* topic, byte* payload, unsigned int length);
    bool reconnect();
};

#endif