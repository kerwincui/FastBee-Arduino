/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:31:14
 */

#include "protocols/MQTTClient.h"
#include <ArduinoJson.h>
#include <core/ConfigDefines.h>
#include <LittleFS.h>

MQTTClient::MQTTClient() 
    : isConnected(false), lastReconnectAttempt(0) {
}

MQTTClient::~MQTTClient() {
    disconnect();
}

bool MQTTClient::loadMqttConfig(const String& filename) {

    
    // 检查文件是否存在
    if (!!LittleFS.exists(filename)) {
        Serial.println("MQTT config file not found: " + filename);
        return false;
    }
    
    // 打开文件
    File file = LittleFS.open(filename, "r");
    if (!file) {
        Serial.println("Failed to open config file: " + filename);
        return false;
    }
    
    // 解析JSON
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        Serial.println("Failed to parse config file: " + String(error.c_str()));
        return false;
    }
    
    // 读取配置值
    config.server = doc["server"] | "";
    config.port = doc["port"] | 1883;
    // config.clientId = doc["clientId"] | ("ESP32-" + String(random(0xffff), HEX));
    config.username = doc["username"] | "";
    config.password = doc["password"] | "";
    config.topicPrefix = doc["topicPrefix"] | "";
    config.keepAlive = doc["keepAlive"] | 60;
    
    Serial.println("MQTT config loaded successfully:");
    Serial.println("  Server: " + config.server);
    Serial.println("  Port: " + String(config.port));
    Serial.println("  Client ID: " + config.clientId);
    Serial.println("  Username: " + config.username);
    Serial.println("  Topic Prefix: " + config.topicPrefix);
    Serial.println("  Keep Alive: " + String(config.keepAlive));
    
    return true;
}

bool MQTTClient::begin() {
    // 读取mqtt.json配置
    loadMqttConfig();
    
    mqttClient.setClient(wifiClient);
    mqttClient.setServer(config.server.c_str(), config.port);
    mqttClient.setCallback([this](char* topic, byte* payload, unsigned int length) {
        this->mqttCallback(topic, payload, length);
    });
    
    Serial.println("MQTT Client: Initialized");
    return true;
}

bool MQTTClient::connect() {
    Serial.println("MQTT Client: Connecting...");
    return reconnect();
}

void MQTTClient::disconnect() {
    mqttClient.disconnect();
    isConnected = false;
    Serial.println("MQTT Client: Disconnected");
}

bool MQTTClient::publish(const String& topic, const String& message) {
    if (!isConnected) {
        return false;
    }
    
    String fullTopic = config.topicPrefix + topic;
    bool result = mqttClient.publish(fullTopic.c_str(), message.c_str());
    
    if (result) {
        Serial.println("MQTT Client: Published to " + fullTopic);
    } else {
        Serial.println("MQTT Client: Failed to publish to " + fullTopic);
    }
    
    return result;
}

bool MQTTClient::subscribe(const String& topic) {
    if (!isConnected) {
        return false;
    }
    
    String fullTopic = config.topicPrefix + topic;
    bool result = mqttClient.subscribe(fullTopic.c_str());
    
    if (result) {
        Serial.println("MQTT Client: Subscribed to " + fullTopic);
    } else {
        Serial.println("MQTT Client: Failed to subscribe to " + fullTopic);
    }
    
    return result;
}

void MQTTClient::handle() {
    if (!mqttClient.connected()) {
        isConnected = false;
        unsigned long now = millis();
        if (now - lastReconnectAttempt > 5000) {
            lastReconnectAttempt = now;
            if (reconnect()) {
                lastReconnectAttempt = 0;
            }
        }
    } else {
        isConnected = true;
        mqttClient.loop();
    }
}

String MQTTClient::getStatus() const {
    if (isConnected) {
        return "Connected";
    } else {
        return "Disconnected";
    }
}

void MQTTClient::setMessageCallback(std::function<void(const String&, const String&)> callback) {
    this->messageCallback = callback;
}

void MQTTClient::mqttCallback(char* topic, byte* payload, unsigned int length) {
    String topicStr = String(topic);
    String message;
    
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    
    Serial.println("MQTT Client: Received message from " + topicStr + ": " + message);
    
    if (messageCallback) {
        messageCallback(topicStr, message);
    }
}

bool MQTTClient::reconnect() {
    Serial.println("MQTT Client: Attempting to reconnect...");
    
    if (mqttClient.connect(config.clientId.c_str(), config.username.c_str(), config.password.c_str())) {
        Serial.println("MQTT Client: Connected successfully");
        isConnected = true;
        return true;
    } else {
        Serial.println("MQTT Client: Connection failed, rc=" + String(mqttClient.state()));
        return false;
    }
}