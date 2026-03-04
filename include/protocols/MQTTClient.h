#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_random.h>
#include <core/SystemConstants.h>

// MQTT配置结构体
struct MQTTConfig {
    String server;
    uint16_t port;
    String clientId;
    String username;
    String password;
    String topicPrefix;
    uint16_t keepAlive;

    // 默认构造函数
    MQTTConfig() : port(1883), keepAlive(60) {}
};

class MQTTClient {
public:
    MQTTClient();
    ~MQTTClient();

    bool loadMqttConfig(const String& filename = FileSystem::MQTT_CONFIG_FILE); 
    bool begin();
    bool connect();
    void disconnect();
    bool publish(const String& topic, const String& message);
    bool subscribe(const String& topic);
    void handle();
    String getStatus() const;
    
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