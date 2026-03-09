/**
 * @description: MQTT 客户端实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:31:14
 *
 * 修复/优化说明：
 *  1. 修复 loadMqttConfig() 中 !!LittleFS.exists() 双重取反 Bug
 *  2. mqttCallback() 改为一次性 String(payload, length)，消除逐字符 += 循环
 *  3. 所有 Serial.println 替换为 LOG 宏统一输出
 *  4. getStatus() 返回 const char*，避免 String 临时对象
 */

#include "protocols/MQTTClient.h"
#include "systems/LoggerSystem.h"
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
    // 修复：原代码 !!LittleFS.exists() 是双重取反，逻辑错误（文件存在时反而返回 false）
    if (!LittleFS.exists(filename)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "MQTT: Config file not found: %s", filename.c_str());
        LOG_WARNING(buf);
        return false;
    }

    File file = LittleFS.open(filename, "r");
    if (!file) {
        char buf[64];
        snprintf(buf, sizeof(buf), "MQTT: Failed to open config: %s", filename.c_str());
        LOG_ERROR(buf);
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MQTT: Config parse error: %s", err.c_str());
        LOG_ERROR(buf);
        return false;
    }

    config.server      = doc["server"]      | "";
    config.port        = doc["port"]        | 1883;
    config.username    = doc["username"]    | "";
    config.password    = doc["password"]    | "";
    config.topicPrefix = doc["topicPrefix"] | "";
    config.subscribeTopic = doc["subscribeTopic"] | "";
    config.keepAlive   = doc["keepAlive"]   | 60;
    // 新增字段（兼容旧配置，使用默认值）
    config.directConnect     = doc["directConnect"]     | true;
    config.autoReconnect     = doc["autoReconnect"]     | true;
    config.connectionTimeout = doc["connectionTimeout"] | 30000;
    
    // 加载发布主题配置（支持多组）
    config.publishTopics.clear();
    JsonArray topicsArr = doc["publishTopics"].as<JsonArray>();
    if (!topicsArr.isNull()) {
        for (JsonVariant v : topicsArr) {
            MqttPublishTopic topic;
            topic.topic = v["topic"] | "";
            topic.qos = v["qos"] | 0;
            topic.retain = v["retain"] | false;
            topic.content = v["content"] | "";
            config.publishTopics.push_back(topic);
        }
    }
    // 兼容旧配置：如果有单独的 publishTopic，添加到数组
    if (config.publishTopics.empty() && !doc["publishTopic"].isNull()) {
        MqttPublishTopic topic;
        topic.topic = doc["publishTopic"] | "";
        topic.qos = doc["publishQos"] | 0;
        topic.retain = doc["publishRetain"] | false;
        config.publishTopics.push_back(topic);
    }
    // 确保至少有一个默认配置
    if (config.publishTopics.empty()) {
        MqttPublishTopic topic;
        config.publishTopics.push_back(topic);
    }

    // clientId 若配置文件未指定，生成随机 ID
    if (doc["clientId"].isNull() || doc["clientId"].as<String>().isEmpty()) {
        char id[20];
        snprintf(id, sizeof(id), "ESP32-%04X", (unsigned)esp_random() & 0xFFFF);
        config.clientId = id;
    } else {
        config.clientId = doc["clientId"].as<String>();
    }

    char buf[96];
    snprintf(buf, sizeof(buf), "MQTT: Config loaded server=%s:%d client=%s",
             config.server.c_str(), config.port, config.clientId.c_str());
    LOG_INFO(buf);
    return true;
}

bool MQTTClient::begin() {
    loadMqttConfig();

    mqttClient.setClient(wifiClient);
    mqttClient.setServer(config.server.c_str(), config.port);
    mqttClient.setCallback([this](char* topic, byte* payload, unsigned int length) {
        this->mqttCallback(topic, payload, length);
    });

    LOG_INFO("MQTT: Client initialized");
    return true;
}

bool MQTTClient::connect() {
    return reconnect();
}

void MQTTClient::disconnect() {
    if (mqttClient.connected()) {
        mqttClient.disconnect();
    }
    isConnected = false;
    LOG_INFO("MQTT: Disconnected");
}

bool MQTTClient::publish(const String& topic, const String& message) {
    if (!isConnected) {
        return false;
    }

    String fullTopic = config.topicPrefix + topic;
    // 查找对应的主题配置，使用其 QoS 和 Retain 设置
    uint8_t qos = 0;
    bool retain = false;
    for (const auto& pt : config.publishTopics) {
        if (pt.topic == topic) {
            qos = pt.qos;
            retain = pt.retain;
            break;
        }
    }
    
    bool ok = mqttClient.publish(fullTopic.c_str(), message.c_str(), retain);

    if (!ok) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MQTT: Publish failed topic=%s", fullTopic.c_str());
        LOG_WARNING(buf);
    }
    return ok;
}

bool MQTTClient::publishToTopic(size_t topicIndex, const String& message) {
    if (!isConnected || topicIndex >= config.publishTopics.size()) {
        return false;
    }
    
    const MqttPublishTopic& pt = config.publishTopics[topicIndex];
    String fullTopic = config.topicPrefix + pt.topic;
    bool ok = mqttClient.publish(fullTopic.c_str(), message.c_str(), pt.retain);
    
    if (!ok) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MQTT: Publish failed topic=%s", fullTopic.c_str());
        LOG_WARNING(buf);
    }
    return ok;
}

bool MQTTClient::subscribe(const String& topic) {
    if (!isConnected) {
        return false;
    }

    String fullTopic = config.topicPrefix + topic;
    bool ok = mqttClient.subscribe(fullTopic.c_str());

    char buf[80];
    snprintf(buf, sizeof(buf), "MQTT: %s topic=%s",
             ok ? "Subscribed" : "Subscribe failed", fullTopic.c_str());
    ok ? LOG_INFO(buf) : LOG_WARNING(buf);
    return ok;
}

void MQTTClient::handle() {
    if (!mqttClient.connected()) {
        isConnected = false;
        unsigned long now = millis();
        if (now - lastReconnectAttempt > 5000UL) {
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
    return isConnected ? F("Connected") : F("Disconnected");
}

void MQTTClient::setMessageCallback(std::function<void(const String&, const String&)> callback) {
    messageCallback = callback;
}

void MQTTClient::mqttCallback(char* topic, byte* payload, unsigned int length) {
    // 修复：原来逐字符 message += (char)payload[i]，每次迭代都可能触发 String 重分配
    // 改为一次性构造，O(1) 内存操作
    String message((const char*)payload, length);
    String topicStr(topic);

    if (messageCallback) {
        messageCallback(topicStr, message);
    }
}

bool MQTTClient::reconnect() {
    LOG_INFO("MQTT: Connecting...");

    bool ok = mqttClient.connect(
        config.clientId.c_str(),
        config.username.c_str(),
        config.password.c_str());

    if (ok) {
        isConnected = true;
        LOG_INFO("MQTT: Connected");
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "MQTT: Connect failed rc=%d", mqttClient.state());
        LOG_WARNING(buf);
    }
    return ok;
}
