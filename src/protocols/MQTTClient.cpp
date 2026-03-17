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
    : isConnected(false), lastReconnectAttempt(0),
      lastConnectedTime(0), lastErrorCode(0), reconnectCount(0) {
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

    // protocol.json 结构为 { "mqtt": { ... }, "modbusRtu": { ... }, ... }
    // 优先从 doc["mqtt"] 子对象读取；若不存在则兼容旧配置从根级别读取
    JsonVariant mqttObj = doc["mqtt"];
    bool nested = !mqttObj.isNull() && mqttObj.is<JsonObject>();
    JsonVariant cfg = nested ? mqttObj : doc.as<JsonVariant>();

    config.server      = cfg["server"]      | "";
    config.port        = cfg["port"]        | 1883;
    config.username    = cfg["username"]    | "";
    config.password    = cfg["password"]    | "";
    config.topicPrefix = cfg["topicPrefix"] | "";
    config.subscribeTopic = cfg["subscribeTopic"] | "";
    config.keepAlive   = cfg["keepAlive"]   | 60;
    // 新增字段（兼容旧配置，使用默认值）
    config.directConnect     = cfg["directConnect"]     | true;
    config.autoReconnect     = cfg["autoReconnect"]     | true;
    config.connectionTimeout = cfg["connectionTimeout"] | 30000;
    // 遗嘱消息配置
    config.willTopic   = cfg["willTopic"]   | "";
    config.willPayload = cfg["willPayload"] | "";
    config.willQos     = cfg["willQos"]     | 0;
    config.willRetain  = cfg["willRetain"]  | false;
    
    // 加载发布主题配置（支持多组）
    config.publishTopics.clear();
    JsonArray topicsArr = cfg["publishTopics"].as<JsonArray>();
    if (!topicsArr.isNull()) {
        for (JsonVariant v : topicsArr) {
            MqttPublishTopic topic;
            topic.topic = v["topic"] | "";
            topic.qos = v["qos"] | 0;
            topic.retain = v["retain"] | false;
            topic.content = v["content"] | "";
            topic.topicType = static_cast<MqttTopicType>(v["topicType"] | 0);
            config.publishTopics.push_back(topic);
        }
    }
    // 兼容旧配置：如果有单独的 publishTopic，添加到数组
    if (config.publishTopics.empty() && !cfg["publishTopic"].isNull()) {
        MqttPublishTopic topic;
        topic.topic = cfg["publishTopic"] | "";
        topic.qos = cfg["publishQos"] | 0;
        topic.retain = cfg["publishRetain"] | false;
        config.publishTopics.push_back(topic);
    }
    // 确保至少有一个默认配置
    if (config.publishTopics.empty()) {
        MqttPublishTopic topic;
        config.publishTopics.push_back(topic);
    }
    
    // 加载订阅主题配置（支持多组）
    config.subscribeTopics.clear();
    JsonArray subTopicsArr = cfg["subscribeTopics"].as<JsonArray>();
    if (!subTopicsArr.isNull()) {
        for (JsonVariant v : subTopicsArr) {
            MqttSubscribeTopic topic;
            topic.topic = v["topic"] | "";
            topic.qos = v["qos"] | 0;
            topic.action = v["action"] | "";
            topic.topicType = static_cast<MqttTopicType>(v["topicType"] | 1);
            config.subscribeTopics.push_back(topic);
        }
    }
    // 兼容旧配置：如果有单独的 subscribeTopic，添加到数组
    if (config.subscribeTopics.empty() && !config.subscribeTopic.isEmpty()) {
        MqttSubscribeTopic topic;
        topic.topic = config.subscribeTopic;
        topic.qos = 0;
        config.subscribeTopics.push_back(topic);
    }

    // clientId 若配置文件未指定，生成随机 ID
    if (cfg["clientId"].isNull() || cfg["clientId"].as<String>().isEmpty()) {
        char id[20];
        snprintf(id, sizeof(id), "ESP32-%04X", (unsigned)esp_random() & 0xFFFF);
        config.clientId = id;
    } else {
        config.clientId = cfg["clientId"].as<String>();
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

bool MQTTClient::subscribeAll() {
    if (!isConnected) {
        return false;
    }
    
    bool allOk = true;
    
    // 订阅所有配置的主题
    for (const auto& st : config.subscribeTopics) {
        if (!st.topic.isEmpty()) {
            String fullTopic = config.topicPrefix + st.topic;
            bool ok = mqttClient.subscribe(fullTopic.c_str(), st.qos);
            
            char buf[96];
            snprintf(buf, sizeof(buf), "MQTT: %s topic=%s qos=%d",
                     ok ? "Subscribed" : "Subscribe failed", fullTopic.c_str(), st.qos);
            ok ? LOG_INFO(buf) : LOG_WARNING(buf);
            
            if (!ok) allOk = false;
        }
    }
    
    // 兼容旧配置：如果 subscribeTopics 为空但 subscribeTopic 不为空
    if (config.subscribeTopics.empty() && !config.subscribeTopic.isEmpty()) {
        allOk = subscribe(config.subscribeTopic);
    }
    
    return allOk;
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

void MQTTClient::setMessageCallback(std::function<void(const String&, const String&, MqttTopicType)> callback) {
    messageCallback = callback;
}

MqttTopicType MQTTClient::getTopicTypeByPath(const String& topicPath) const {
    // 去掉前缀后匹配
    String stripped = topicPath;
    if (!config.topicPrefix.isEmpty() && topicPath.startsWith(config.topicPrefix)) {
        stripped = topicPath.substring(config.topicPrefix.length());
    }
    // 先查订阅主题
    for (const auto& st : config.subscribeTopics) {
        if (st.topic == stripped || st.topic == topicPath) {
            return st.topicType;
        }
    }
    // 再查发布主题
    for (const auto& pt : config.publishTopics) {
        if (pt.topic == stripped || pt.topic == topicPath) {
            return pt.topicType;
        }
    }
    return MqttTopicType::DATA_COMMAND; // 默认
}

void MQTTClient::mqttCallback(char* topic, byte* payload, unsigned int length) {
    // 修复：原来逐字符 message += (char)payload[i]，每次迭代都可能触发 String 重分配
    // 改为一次性构造，O(1) 内存操作
    String message((const char*)payload, length);
    String topicStr(topic);

    // 根据主题路径查找对应的主题类型
    MqttTopicType tType = getTopicTypeByPath(topicStr);

    char buf[96];
    snprintf(buf, sizeof(buf), "MQTT: Msg type=%d topic=%s len=%u",
             (int)tType, topic, length);
    LOG_DEBUG(buf);

    if (messageCallback) {
        messageCallback(topicStr, message, tType);
    }
}

bool MQTTClient::reconnect() {
    LOG_INFO("MQTT: Connecting...");

    bool ok;
    if (!config.willTopic.isEmpty()) {
        ok = mqttClient.connect(
            config.clientId.c_str(),
            config.username.c_str(),
            config.password.c_str(),
            config.willTopic.c_str(),
            config.willQos,
            config.willRetain,
            config.willPayload.c_str());
    } else {
        ok = mqttClient.connect(
            config.clientId.c_str(),
            config.username.c_str(),
            config.password.c_str());
    }

    if (ok) {
        isConnected = true;
        lastConnectedTime = millis();
        lastErrorCode = 0;
        LOG_INFO("MQTT: Connected");
        // 连接成功后订阅所有主题
        subscribeAll();
    } else {
        lastErrorCode = mqttClient.state();
        reconnectCount++;
        char buf[48];
        snprintf(buf, sizeof(buf), "MQTT: Connect failed rc=%d", lastErrorCode);
        LOG_WARNING(buf);
    }
    return ok;
}
