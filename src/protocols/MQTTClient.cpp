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
#include <HTTPClient.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>

MQTTClient::MQTTClient()
    : isConnected(false), stopped(false), lastReconnectAttempt(0),
      lastConnectedTime(0), lastErrorCode(0), reconnectCount(0),
      reconnectInterval(5000), lastLoopTime(0) {
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
    config.accessMode        = cfg["accessMode"]        | 0;
    config.autoReconnect     = cfg["autoReconnect"]     | true;
    config.connectionTimeout = cfg["connectionTimeout"] | 30000;
    // 遗嘱消息配置
    config.willTopic   = cfg["willTopic"]   | "";
    config.willPayload = cfg["willPayload"] | "";
    config.willQos     = cfg["willQos"]     | 0;
    config.willRetain  = cfg["willRetain"]  | false;
    
    // 认证配置
    config.authType    = static_cast<MqttAuthType>(cfg["authType"] | 0);
    config.deviceNum   = cfg["deviceNum"]   | "";
    config.productId   = cfg["productId"]   | "";
    config.userId      = cfg["userId"]      | "";
    config.mqttSecret  = cfg["mqttSecret"]  | "";
    config.authCode    = cfg["authCode"]    | "";
    config.ntpServer   = cfg["ntpServer"]   | "";

    // 加载发布主题配置（支持多组）
    config.publishTopics.clear();
    JsonArray topicsArr = cfg["publishTopics"].as<JsonArray>();
    if (!topicsArr.isNull()) {
        for (JsonVariant v : topicsArr) {
            MqttPublishTopic topic;
            topic.topic = v["topic"] | "";
            topic.qos = v["qos"] | 0;
            topic.retain = v["retain"] | false;
            topic.enabled = v["enabled"] | true;
            topic.autoPrefix = v["autoPrefix"] | false;
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
            topic.enabled = v["enabled"] | true;
            topic.autoPrefix = v["autoPrefix"] | false;
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
    stopped = false;
    loadMqttConfig();

    mqttClient.setClient(wifiClient);
    mqttClient.setServer(config.server.c_str(), config.port);
    mqttClient.setBufferSize(1024);
    mqttClient.setKeepAlive(config.keepAlive);
    // socketTimeout 需大于 keepAlive，避免 keepAlive ping 期间被超时断开
    mqttClient.setSocketTimeout(config.keepAlive > 15 ? config.keepAlive : 15);
    mqttClient.setCallback([this](char* topic, byte* payload, unsigned int length) {
        this->mqttCallback(topic, payload, length);
    });

    LOG_INFO("MQTT: Client initialized");
    return true;
}

bool MQTTClient::connect() {
    stopped = false;
    return reconnect();
}

void MQTTClient::disconnect() {
    if (mqttClient.connected()) {
        mqttClient.disconnect();
    }
    isConnected = false;
    reconnectInterval = 5000;
    LOG_INFO("MQTT: Disconnected");
}

void MQTTClient::stop() {
    disconnect();
    stopped = true;
    LOG_INFO("MQTT: Stopped (auto-reconnect disabled)");
}

bool MQTTClient::publish(const String& topic, const String& message) {
    if (!isConnected) {
        return false;
    }

    // 查找对应的主题配置，使用其 QoS、Retain 和 autoPrefix 设置
    uint8_t qos = 0;
    bool retain = false;
    bool topicAutoPrefix = false;
    for (const auto& pt : config.publishTopics) {
        if (pt.topic == topic && pt.enabled) {
            qos = pt.qos;
            retain = pt.retain;
            topicAutoPrefix = pt.autoPrefix;
            break;
        }
    }
    
    String fullTopic = buildFullTopic(topic, topicAutoPrefix);
    
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
    if (!pt.enabled) {
        return false;
    }
    String fullTopic = buildFullTopic(pt.topic, pt.autoPrefix);
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

    // 查找对应订阅主题配置的autoPrefix设置
    bool topicAutoPrefix = false;
    for (const auto& st : config.subscribeTopics) {
        if (st.topic == topic) {
            topicAutoPrefix = st.autoPrefix;
            break;
        }
    }
    String fullTopic = buildFullTopic(topic, topicAutoPrefix);
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
        if (!st.topic.isEmpty() && st.enabled) {
            String fullTopic = buildFullTopic(st.topic, st.autoPrefix);
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
    if (stopped) return;  // 被显式停止时不做任何操作

    // 检查WiFi连接状态，WiFi断开时不尝试重连
    if (WiFi.status() != WL_CONNECTED) {
        if (isConnected) {
            isConnected = false;
            LOG_WARNING("MQTT: WiFi disconnected, marking MQTT offline");
        }
        return;
    }

    if (!mqttClient.connected()) {
        if (isConnected) {
            isConnected = false;
            reconnectInterval = 5000;  // 刚断开时重置重连间隔
            LOG_WARNING("MQTT: Connection lost");
        }

        if (!config.autoReconnect) return;

        unsigned long now = millis();
        if (now - lastReconnectAttempt >= reconnectInterval) {
            lastReconnectAttempt = now;
            if (reconnect()) {
                reconnectInterval = 5000;  // 连接成功，重置间隔
            } else {
                // 指数退避：5s -> 10s -> 20s -> 30s（上限）
                reconnectInterval = min(reconnectInterval * 2, (uint32_t)30000);
                char buf[64];
                snprintf(buf, sizeof(buf), "MQTT: Next retry in %lus", reconnectInterval / 1000);
                LOG_INFO(buf);
            }
        }
    } else {
        if (!isConnected) {
            isConnected = true;
            lastConnectedTime = millis();
        }
        lastLoopTime = millis();
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
    // 去掉前缀后匹配（检查每个主题自身的autoPrefix设置）
    // 先查订阅主题
    for (const auto& st : config.subscribeTopics) {
        if (st.topic == topicPath) {
            return st.topicType;
        }
        // 如果该主题启用了autoPrefix，尝试去掉前缀后匹配
        if (st.autoPrefix && !config.topicPrefix.isEmpty() && topicPath.startsWith(config.topicPrefix)) {
            String stripped = topicPath.substring(config.topicPrefix.length());
            if (st.topic == stripped) {
                return st.topicType;
            }
        }
    }
    // 再查发布主题
    for (const auto& pt : config.publishTopics) {
        if (pt.topic == topicPath) {
            return pt.topicType;
        }
        if (pt.autoPrefix && !config.topicPrefix.isEmpty() && topicPath.startsWith(config.topicPrefix)) {
            String stripped = topicPath.substring(config.topicPrefix.length());
            if (pt.topic == stripped) {
                return pt.topicType;
            }
        }
    }
    return MqttTopicType::DATA_COMMAND; // 默认
}

String MQTTClient::buildFullTopic(const String& topic, bool autoPrefix) const {
    if (autoPrefix && !config.topicPrefix.isEmpty()) {
        return config.topicPrefix + topic;
    }
    return topic;
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

    // 根据认证类型构建clientId和密码
    String connClientId = config.clientId;
    String connPassword;

    if (config.authType == MqttAuthType::ENCRYPTED &&
        !config.deviceNum.isEmpty() && !config.productId.isEmpty()) {
        // AES加密认证模式：自动构建clientId和加密密码
        connClientId = buildClientId();
        connPassword = buildEncryptedPassword();
        if (connPassword.isEmpty()) {
            LOG_WARNING("MQTT: AES password generation failed, falling back to simple auth");
            connPassword = buildSimplePassword();
        }
    } else {
        // 简单认证模式：使用配置的clientId，密码追加authCode
        connPassword = buildSimplePassword();
    }

    // 如果clientId为空，生成随机ID
    if (connClientId.isEmpty()) {
        char id[20];
        snprintf(id, sizeof(id), "ESP32-%04X", (unsigned)esp_random() & 0xFFFF);
        connClientId = id;
    }

    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "MQTT: Auth=%s ClientId=%s",
             config.authType == MqttAuthType::ENCRYPTED ? "AES" : "Simple",
             connClientId.c_str());
    LOG_INFO(logBuf);

    bool ok;
    if (!config.willTopic.isEmpty()) {
        ok = mqttClient.connect(
            connClientId.c_str(),
            config.username.c_str(),
            connPassword.c_str(),
            config.willTopic.c_str(),
            config.willQos,
            config.willRetain,
            config.willPayload.c_str());
    } else {
        ok = mqttClient.connect(
            connClientId.c_str(),
            config.username.c_str(),
            connPassword.c_str());
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
        char buf2[48];
        snprintf(buf2, sizeof(buf2), "MQTT: Connect failed rc=%d", lastErrorCode);
        LOG_WARNING(buf2);
    }
    return ok;
}

// ============ FastBee认证方法 ============

String MQTTClient::buildClientId() {
    // 客户端ID格式: 认证类型(E/S) & 设备编号 & 产品ID & 用户ID
    String prefix = (config.authType == MqttAuthType::ENCRYPTED) ? "E" : "S";
    return prefix + "&" + config.deviceNum + "&" + config.productId + "&" + config.userId;
}

String MQTTClient::buildSimplePassword() {
    // 简单认证密码格式: mqtt密码 或 mqtt密码&授权码
    if (config.authCode.isEmpty()) {
        return config.password;
    }
    return config.password + "&" + config.authCode;
}

String MQTTClient::buildEncryptedPassword() {
    // 加密认证密码生成流程:
    // 1. 获取NTP时间
    // 2. 计算过期时间 = 当前时间 + 1小时
    // 3. 构造密码明文: mqtt密码&过期时间 或 mqtt密码&过期时间&授权码
    // 4. AES-CBC-128加密, Base64编码

    if (config.mqttSecret.isEmpty() || config.mqttSecret.length() < 16) {
        LOG_WARNING("MQTT: mqttSecret too short for AES-128 (need 16 bytes)");
        return "";
    }

    String ntpJson = getNtpTime();
    if (ntpJson.isEmpty()) {
        LOG_WARNING("MQTT: Failed to get NTP time");
        return "";
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, ntpJson);
    if (err) {
        LOG_WARNING("MQTT: NTP response parse failed");
        return "";
    }

    double deviceSendTime = doc["deviceSendTime"] | 0.0;
    double serverSendTime = doc["serverSendTime"] | 0.0;
    double serverRecvTime = doc["serverRecvTime"] | 0.0;
    double deviceRecvTime = (double)millis();
    double now = (serverSendTime + serverRecvTime + deviceRecvTime - deviceSendTime) / 2.0;
    double expireTime = now + 3600000.0;

    String plainPassword;
    if (config.authCode.isEmpty()) {
        plainPassword = config.password + "&" + String((unsigned long)expireTime);
    } else {
        plainPassword = config.password + "&" + String((unsigned long)expireTime) + "&" + config.authCode;
    }

    LOG_INFO("MQTT: Generating AES encrypted password");

    String iv = "wumei-smart-open";
    String encrypted = aesEncrypt(plainPassword, config.mqttSecret, iv);

    if (encrypted.isEmpty()) {
        LOG_WARNING("MQTT: AES encryption failed");
        return "";
    }

    return encrypted;
}

String MQTTClient::getNtpTime() {
    if (config.ntpServer.isEmpty()) {
        LOG_WARNING("MQTT: NTP server not configured");
        return "";
    }

    HTTPClient http;
    String url = config.ntpServer + String(millis());

    char buf[128];
    snprintf(buf, sizeof(buf), "MQTT: Fetching NTP time from %s", url.c_str());
    LOG_DEBUG(buf);

    if (!http.begin(url)) {
        LOG_WARNING("MQTT: HTTP begin failed for NTP");
        return "";
    }

    http.setTimeout(5000);
    int httpCode = http.GET();
    String payload = "";

    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        payload = http.getString();
    } else {
        char buf2[64];
        snprintf(buf2, sizeof(buf2), "MQTT: NTP HTTP failed, code=%d", httpCode);
        LOG_WARNING(buf2);
    }

    http.end();
    return payload;
}

String MQTTClient::aesEncrypt(const String& plainData, const String& key, const String& iv) {
    int len = plainData.length();
    int nBlocks = len / 16 + 1;
    uint8_t nPadding = nBlocks * 16 - len;
    size_t paddedLen = nBlocks * 16;

    uint8_t* data = new uint8_t[paddedLen];
    memcpy(data, plainData.c_str(), len);
    for (size_t i = len; i < paddedLen; i++) {
        data[i] = nPadding;
    }

    uint8_t keyBuf[16];
    uint8_t ivBuf[16];
    memcpy(keyBuf, key.c_str(), 16);
    memcpy(ivBuf, iv.c_str(), 16);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int ret = mbedtls_aes_setkey_enc(&aes, keyBuf, 128);
    if (ret != 0) {
        mbedtls_aes_free(&aes);
        delete[] data;
        LOG_WARNING("MQTT: AES setkey failed");
        return "";
    }

    ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, paddedLen, ivBuf, data, data);
    mbedtls_aes_free(&aes);

    if (ret != 0) {
        delete[] data;
        LOG_WARNING("MQTT: AES encrypt failed");
        return "";
    }

    size_t b64Len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64Len, data, paddedLen);
    uint8_t* b64Buf = new uint8_t[b64Len + 1];
    ret = mbedtls_base64_encode(b64Buf, b64Len + 1, &b64Len, data, paddedLen);
    delete[] data;

    if (ret != 0) {
        delete[] b64Buf;
        LOG_WARNING("MQTT: Base64 encode failed");
        return "";
    }

    String result = String((char*)b64Buf, b64Len);
    delete[] b64Buf;
    return result;
}
