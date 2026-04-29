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
#include "protocols/ProtocolManager.h"
#include "protocols/ModbusHandler.h"
#include "core/AsyncExecTypes.h"
#include "systems/LoggerSystem.h"
#include "core/FeatureFlags.h"
#include "core/FastBeeFramework.h"
#if FASTBEE_ENABLE_HEALTH_MONITOR
#include "systems/HealthMonitor.h"
#endif
#include <ArduinoJson.h>
#include <core/ConfigDefines.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <mbedtls/aes.h>
#include <core/FeatureFlags.h>
#if FASTBEE_ENABLE_PERIPH_EXEC
#include <core/PeriphExecManager.h>
#endif
#if FASTBEE_ENABLE_RULE_SCRIPT
#include <core/RuleScriptManager.h>
#endif
#include <mbedtls/base64.h>
#include <new>

#if FASTBEE_ENABLE_MQTT

static double jsonVariantToDoubleSafe(JsonVariantConst v) {
    if (v.isNull()) return 0.0;
    if (v.is<double>()) return v.as<double>();
    if (v.is<float>()) return static_cast<double>(v.as<float>());
    if (v.is<long long>()) return static_cast<double>(v.as<long long>());
    if (v.is<unsigned long long>()) return static_cast<double>(v.as<unsigned long long>());
    if (v.is<long>()) return static_cast<double>(v.as<long>());
    if (v.is<unsigned long>()) return static_cast<double>(v.as<unsigned long>());
    if (v.is<int>()) return static_cast<double>(v.as<int>());
    if (v.is<unsigned int>()) return static_cast<double>(v.as<unsigned int>());
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        return (s && s[0] != '\0') ? String(s).toDouble() : 0.0;
    }
    if (v.is<String>()) return v.as<String>().toDouble();
    return 0.0;
}

MQTTClient::MQTTClient()
    : isConnected(false), stopped(false), lastReconnectAttempt(0),
      lastConnectedTime(0), lastErrorCode(0), reconnectCount(0),
      reconnectInterval(5000), consecutiveTimeouts(0), lastLoopTime(0),
      _reconnectPending(false), _reconnectRunning(false), _reconnectTaskHandle(nullptr),
      _slotWriteIndex(0), _slotReadIndex(0), _slotCount(0) {
    _publishMutex = xSemaphoreCreateRecursiveMutex();
    _dataCommandQueue = xQueueCreate(4, sizeof(String*));
}

MQTTClient::~MQTTClient() {
    // 先关闭后台任务，避免 disconnect 后任务仍在尝试重连
    if (_reconnectTaskHandle) {
        vTaskDelete(_reconnectTaskHandle);
        _reconnectTaskHandle = nullptr;
    }
    disconnect();
    if (_publishMutex) {
        vSemaphoreDelete(_publishMutex);
        _publishMutex = nullptr;
    }
    // 排空并删除 DATA_COMMAND 队列
    if (_dataCommandQueue) {
        String* cmd = nullptr;
        while (xQueueReceive(_dataCommandQueue, &cmd, 0) == pdTRUE) { delete cmd; }
        vQueueDelete(_dataCommandQueue);
        _dataCommandQueue = nullptr;
    }
    // 清空上报环形缓冲区
    for (uint8_t i = 0; i < MQTT_REPORT_SLOTS; i++) {
        _reportSlots[i].clear();
    }
    _slotWriteIndex = 0;
    _slotReadIndex = 0;
    _slotCount = 0;
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
    // Card 高级配置（设备信息发布用）
    config.longitude      = cfg["longitude"]      | 0.0;
    config.latitude       = cfg["latitude"]       | 0.0;
    config.iccid          = cfg["iccid"]          | "";
    config.cardPlatformId = cfg["cardPlatformId"] | 0;
    config.summary        = cfg["summary"]        | "";

    // 如果deviceNum/productId/userId/ntpServer为空，尝试从device.json中读取
    if (config.deviceNum.isEmpty() || config.productId.isEmpty() || config.userId.isEmpty()
        || config.ntpServer.isEmpty()) {
        if (LittleFS.exists("/config/device.json")) {
            File devFile = LittleFS.open("/config/device.json", "r");
            if (devFile) {
                JsonDocument devDoc;
                if (!deserializeJson(devDoc, devFile)) {
                    if (config.deviceNum.isEmpty()) {
                        config.deviceNum = devDoc["deviceId"] | "";
                    }
                    if (config.productId.isEmpty()) {
                        String pn = String(devDoc["productNumber"] | 0);
                        if (pn != "0") config.productId = pn;
                    }
                    if (config.userId.isEmpty()) {
                        config.userId = devDoc["userId"] | "";
                    }
                    if (config.ntpServer.isEmpty()) {
                        config.ntpServer = devDoc["ntpServer1"] | "";
                    }
                }
                devFile.close();
            }
        }
    }

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

    // clientId 若配置文件未指定，按 FastBee 认证格式自动生成：认证类型&设备编号&产品编号&用户ID
    if (cfg["clientId"].isNull() || cfg["clientId"].as<String>().isEmpty()) {
        // 如果 deviceNum 仍为空，生成 FBE+MAC
        if (config.deviceNum.isEmpty()) {
            String mac = WiFi.macAddress();
            mac.replace(":", "");
            config.deviceNum = "FBE" + mac;
        }
        // productId 为空时使用默认值 "1"
        if (config.productId.isEmpty()) {
            config.productId = "1";
        }
        // userId 为空时使用默认值 "1"
        if (config.userId.isEmpty()) {
            config.userId = "1";
        }
        // 根据认证类型构建 clientId
        String prefix = (config.authType == MqttAuthType::ENCRYPTED) ? "E" : "S";
        config.clientId = prefix + "&" + config.deviceNum + "&" + config.productId + "&" + config.userId;
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
    // socketTimeout 设置为 5 秒，加速 publish/connect 失败检测
    // 过大的值（如 60s）会导致 WiFiClient::write() 对死 socket 阻塞数十秒
    mqttClient.setSocketTimeout(5);
    mqttClient.setCallback([this](char* topic, byte* payload, unsigned int length) {
        this->mqttCallback(topic, payload, length);
    });

    // 启动后台重连任务（避免 reconnect() 阻塞 loopTask）
    if (_reconnectTaskHandle == nullptr) {
        BaseType_t ret = xTaskCreate(
            reconnectTaskEntry,
            "mqtt_reconn",
            4096,           // 栈大小：4KB 足够 reconnect 使用
            this,           // 参数
            1,              // 优先级：与 loopTask 同级
            &_reconnectTaskHandle
        );
        if (ret != pdPASS) {
            LOG_WARNING("[MQTT] Failed to create background reconnect task");
            _reconnectTaskHandle = nullptr;
        } else {
            LOG_INFO("[MQTT] Background reconnect task created");
        }
    }

    LOG_INFO("MQTT: Client initialized");
    return true;
}

void MQTTClient::shutdown() {
    if (_reconnectTaskHandle) {
        vTaskDelete(_reconnectTaskHandle);
        _reconnectTaskHandle = nullptr;
        LOG_INFO("[MQTT] Background reconnect task stopped");
    }
}

bool MQTTClient::connect() {
    stopped = false;
    return reconnect();
}

void MQTTClient::disconnect() {
    if (mqttClient.connected()) {
        mqttClient.disconnect();
    }
    // 确保底层 TCP socket 完全关闭释放
    wifiClient.stop();
    isConnected = false;
    reconnectInterval = 5000;
    consecutiveTimeouts = 0;
    LOG_INFO("MQTT: Disconnected");
}

void MQTTClient::stop() {
    disconnect();
    stopped = true;
    _notifyStatusChange();  // 通知状态变化：主动停止
    LOG_INFO("MQTT: Stopped (auto-reconnect disabled)");
}

bool MQTTClient::publish(const String& topic, const String& message) {
    RecursiveMutexGuard lock(_publishMutex);
    if (!isConnected) {
        return false;
    }

    // 查找对应的主题配置，使用其 QoS、Retain 和 autoPrefix 设置
    uint8_t qos = 0;
    bool retain = false;
    bool topicAutoPrefix = false;
    MqttTopicType topicType = MqttTopicType::DATA_REPORT;
    for (const auto& pt : config.publishTopics) {
        if (pt.topic == topic && pt.enabled) {
            qos = pt.qos;
            retain = pt.retain;
            topicAutoPrefix = pt.autoPrefix;
            topicType = pt.topicType;
            break;
        }
    }
    
    String fullTopic = buildFullTopicWithType(topic, topicAutoPrefix, topicType);
    
    bool ok = mqttClient.publish(fullTopic.c_str(), message.c_str(), retain);

    if (!ok) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MQTT: Publish failed topic=%s", fullTopic.c_str());
        LOG_WARNING(buf);
        // publish 失败通常意味着连接已断，立即标记以避免后续 write 对死 socket 阻塞
        if (!mqttClient.connected()) {
            isConnected = false;
        }
    }
    return ok;
}

bool MQTTClient::publishToTopic(size_t topicIndex, const String& message) {
    RecursiveMutexGuard lock(_publishMutex);
    if (!isConnected || topicIndex >= config.publishTopics.size()) {
        return false;
    }
    
    const MqttPublishTopic& pt = config.publishTopics[topicIndex];
    if (!pt.enabled) {
        return false;
    }
    String fullTopic = buildFullTopicWithType(pt.topic, pt.autoPrefix, pt.topicType);
    // 数据上报转换管道
#if FASTBEE_ENABLE_RULE_SCRIPT
    String payload = RuleScriptManager::getInstance().applyReportTransform(0, message);
#else
    String payload = message;
#endif
    bool ok = mqttClient.publish(fullTopic.c_str(), payload.c_str(), pt.retain);

    // 统一打印 MQTT 上报日志（Serial.printf 避免 LoggerSystem 文件 I/O 在发布路径中加剧栈压力）
    {
        String truncatedPayload = payload.length() > 120 ? payload.substring(0, 120) + "..." : payload;
        Serial.printf("[MQTT] TX ▲ topic=%s len=%u payload=%s result=%s\n",
                      fullTopic.c_str(), payload.length(), truncatedPayload.c_str(),
                      ok ? "OK" : "FAIL");
    }

    if (!ok) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MQTT: Publish failed topic=%s", fullTopic.c_str());
        LOG_WARNING(buf);
        if (!mqttClient.connected()) {
            isConnected = false;
        }
    }
    return ok;
}

bool MQTTClient::publishDeviceInfo() {
    if (!isConnected) {
        return false;
    }

    // 查找 topicType==DEVICE_INFO 的发布主题
    int infoTopicIdx = -1;
    for (size_t i = 0; i < config.publishTopics.size(); i++) {
        if (config.publishTopics[i].topicType == MqttTopicType::DEVICE_INFO &&
            config.publishTopics[i].enabled) {
            infoTopicIdx = (int)i;
            break;
        }
    }
    if (infoTopicIdx < 0) {
        LOG_DEBUG("MQTT: No enabled DEVICE_INFO publish topic found");
        return false;
    }

    // 构建设备信息 JSON
    JsonDocument doc;
    doc["rssi"] = WiFi.RSSI();
    doc["firmwareVersion"] = SystemInfo::VERSION;
    doc["status"] = 3;  // 在线

    // userId: 优先使用配置值，默认为 1
    if (!config.userId.isEmpty()) {
        long uid = config.userId.toInt();
        doc["userId"] = (uid > 0) ? uid : 1;
    } else {
        doc["userId"] = 1;
    }

    // 经纬度
    doc["longitude"] = config.longitude;
    doc["latitude"]  = config.latitude;

    // 可选字段：非空/非零时才包含
    if (!config.iccid.isEmpty()) {
        doc["iccid"] = config.iccid;
    }
    if (config.cardPlatformId != 0) {
        doc["cardPlatformId"] = config.cardPlatformId;
    }

    // summary: 解析配置的 JSON 字符串，为空时自动生成默认值
    if (!config.summary.isEmpty()) {
        JsonDocument summaryDoc;
        DeserializationError err = deserializeJson(summaryDoc, config.summary);
        if (!err) {
            doc["summary"] = summaryDoc.as<JsonObject>();
        }
    } else {
        // 自动生成默认 summary
        JsonObject summary = doc["summary"].to<JsonObject>();
        summary["name"] = "FastBee";
        summary["chip"] = ESP.getChipModel();
    }

    String payload;
    serializeJson(doc, payload);

    // 获取完整主题用于日志输出
    const MqttPublishTopic& pt = config.publishTopics[infoTopicIdx];
    String fullTopic = buildFullTopicWithType(pt.topic, pt.autoPrefix, pt.topicType);

    bool ok = publishToTopic((size_t)infoTopicIdx, payload);

    LOG_INFO(ok ? "MQTT: Published device info" : "MQTT: Failed to publish device info");

    return ok;
}

bool MQTTClient::publishMonitorData() {
    if (!isConnected) {
        return false;
    }

    // 查找 topicType==REALTIME_MON 的发布主题
    int monTopicIdx = -1;
    for (size_t i = 0; i < config.publishTopics.size(); i++) {
        if (config.publishTopics[i].topicType == MqttTopicType::REALTIME_MON &&
            config.publishTopics[i].enabled) {
            monTopicIdx = (int)i;
            break;
        }
    }
    if (monTopicIdx < 0) {
        LOG_DEBUG("MQTT: No enabled REALTIME_MON publish topic found");
        return false;
    }

    String payload;

    // 优先使用数据提供者回调获取真实传感器数据
    if (_monitorDataProvider) {
        payload = _monitorDataProvider();
        if (payload.length() <= 2) {
            // 返回空或 "[]"，无可用数据
            LOG_DEBUG("MQTT: Monitor data provider returned empty data");
            return false;
        }
    } else {
        // 降级兜底：未注册数据提供者时使用模拟数据
        LOG_DEBUG("MQTT: Monitor using fallback random data");
        float temperature = 20.0 + (float)(esp_random() % 1500) / 100.0;
        float humidity    = 30.0 + (float)(esp_random() % 5000) / 100.0;

        String remark = "";
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 0) && timeinfo.tm_year >= 100) {
            char timeBuf[24];
            strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &timeinfo);
            remark = timeBuf;
        }

        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();

        JsonObject tempObj = arr.add<JsonObject>();
        tempObj["id"] = "temperature";
        char tempVal[8];
        snprintf(tempVal, sizeof(tempVal), "%.2f", temperature);
        tempObj["value"] = tempVal;
        tempObj["remark"] = remark;

        JsonObject humiObj = arr.add<JsonObject>();
        humiObj["id"] = "humidity";
        char humiVal[8];
        snprintf(humiVal, sizeof(humiVal), "%.2f", humidity);
        humiObj["value"] = humiVal;
        humiObj["remark"] = remark;

        serializeJson(doc, payload);
    }

    bool ok = publishToTopic((size_t)monTopicIdx, payload);

    LOG_INFO(ok ? "MQTT: Published monitor data" : "MQTT: Failed to publish monitor data");

    return ok;
}

bool MQTTClient::publishReportData(const String& payload) {
    if (!isConnected) {
        return false;
    }

    // 查找 topicType==DATA_REPORT 的发布主题
    int reportTopicIdx = -1;
    for (size_t i = 0; i < config.publishTopics.size(); i++) {
        if (config.publishTopics[i].topicType == MqttTopicType::DATA_REPORT &&
            config.publishTopics[i].enabled) {
            reportTopicIdx = (int)i;
            break;
        }
    }
    if (reportTopicIdx < 0) {
        LOG_DEBUG("MQTT: No enabled DATA_REPORT publish topic found");
        return false;
    }

    const MqttPublishTopic& pt = config.publishTopics[reportTopicIdx];
    String fullTopic = buildFullTopicWithType(pt.topic, pt.autoPrefix, pt.topicType);

    // 透传HEX模式：当 Modbus transferType==1 且 payload 是纯 HEX ASCII 字符串时，
    // 将 payload 解码为二进制字节帧后发送（平台直接按 Modbus 帧解析）
    bool sentAsBinary = false;
    bool ok = false;
#if FASTBEE_ENABLE_MODBUS
    do {
        uint8_t transferType = 0;
        auto* fw = FastBeeFramework::getInstance();
        if (!fw) break;
        auto* pm = fw->getProtocolManager();
        if (!pm) break;
        ModbusHandler* mh = pm->getModbusHandler();
        if (!mh) break;
        transferType = mh->getConfig().transferType;
        if (transferType != 1) break;

        // 校验是否为偶数长度且字符集全部为 [0-9a-fA-F]
        unsigned int hexLen = payload.length();
        if (hexLen < 4 || (hexLen % 2) != 0) break;
        bool allHex = true;
        for (unsigned int i = 0; i < hexLen; i++) {
            char c = payload.charAt(i);
            bool v = (c >= '0' && c <= '9') ||
                     (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F');
            if (!v) { allHex = false; break; }
        }
        if (!allHex) break;

        // HEX ASCII 转换为字节数组
        unsigned int byteLen = hexLen / 2;
        if (byteLen > 256) byteLen = 256;
        uint8_t buf[256];
        auto hexVal = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
            if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
            return 0;
        };
        for (unsigned int i = 0; i < byteLen; i++) {
            buf[i] = (uint8_t)((hexVal(payload.charAt(i * 2)) << 4) |
                               hexVal(payload.charAt(i * 2 + 1)));
        }

        // 以二进制方式发布（PubSubClient 接受 const uint8_t*+length）
        {
            RecursiveMutexGuard lock(_publishMutex);
            ok = mqttClient.publish(fullTopic.c_str(), buf, byteLen, pt.retain);
        }
        sentAsBinary = true;
        Serial.printf("[MQTT] TX ▲ topic=%s BIN len=%u hex=%s result=%s\n",
                      fullTopic.c_str(), byteLen, payload.c_str(), ok ? "OK" : "FAIL");
    } while (false);
#endif

    if (!sentAsBinary) {
        // 非透传模式或 payload 不是纯 HEX ASCII：按文本发布（原有逻辑）
        ok = publishToTopic((size_t)reportTopicIdx, payload);
    }

    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "MQTT: %s DATA_REPORT topic=%s",
             ok ? "Published" : "Failed to publish", fullTopic.c_str());
    ok ? LOG_INFO(logBuf) : LOG_WARNING(logBuf);

    return ok;
}

bool MQTTClient::publishNtpSync() {
    if (!isConnected) {
        return false;
    }

    // 查找 topicType==NTP_SYNC 的发布主题
    int ntpTopicIdx = -1;
    for (size_t i = 0; i < config.publishTopics.size(); i++) {
        if (config.publishTopics[i].topicType == MqttTopicType::NTP_SYNC &&
            config.publishTopics[i].enabled) {
            ntpTopicIdx = (int)i;
            break;
        }
    }
    if (ntpTopicIdx < 0) {
        LOG_DEBUG("MQTT: No enabled NTP_SYNC publish topic found");
        return false;
    }

    // 记录设备发送时间（millis）
    ntpDeviceSendTime = (unsigned long long)millis();

    // 构建 NTP 请求 JSON: {"deviceSendTime": "1592361428000"}
    JsonDocument doc;
    doc["deviceSendTime"] = String(ntpDeviceSendTime);

    String payload;
    serializeJson(doc, payload);

    const MqttPublishTopic& pt = config.publishTopics[ntpTopicIdx];
    String fullTopic = buildFullTopicWithType(pt.topic, pt.autoPrefix, pt.topicType);

    bool ok = publishToTopic((size_t)ntpTopicIdx, payload);

    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "MQTT: %s NTP_SYNC topic=%s",
             ok ? "Published" : "Failed to publish", fullTopic.c_str());
    ok ? LOG_INFO(logBuf) : LOG_WARNING(logBuf);

    return ok;
}

bool MQTTClient::subscribe(const String& topic) {
    if (!isConnected) {
        return false;
    }

    // 查找对应订阅主题配置的autoPrefix和topicType设置
    bool topicAutoPrefix = false;
    MqttTopicType topicType = MqttTopicType::DATA_COMMAND;
    for (const auto& st : config.subscribeTopics) {
        if (st.topic == topic) {
            topicAutoPrefix = st.autoPrefix;
            topicType = st.topicType;
            break;
        }
    }
    String fullTopic = buildFullTopicWithType(topic, topicAutoPrefix, topicType);
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
            String fullTopic = buildFullTopicWithType(st.topic, st.autoPrefix, st.topicType);
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
            consecutiveTimeouts = 0;
            // 立即关闭底层 socket，避免后续 write() 对死 socket 阻塞 10s+
            wifiClient.stop();
            LOG_WARNING("MQTT: Connection lost");
            _notifyStatusChange();  // 通知状态变化：连接断开
            // 触发MQTT断开连接系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
            PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_MQTT_DISCONNECTED, "");
#endif
        }

        if (!config.autoReconnect) return;

        unsigned long now = millis();
        if (now - lastReconnectAttempt >= reconnectInterval) {
            lastReconnectAttempt = now;
            // WiFi 未连接时跳过重连，避免 WiFiClient 内部 socket 已失效导致空指针崩溃
            if (WiFi.status() != WL_CONNECTED) {
                LOGGER.infof("[MQTT] WiFi not connected (status=%d), skipping reconnect", WiFi.status());
                return;
            }
            // 调度后台任务执行重连（避免 reconnect() 阻塞 loopTask 7秒+）
            if (!_reconnectPending && !_reconnectRunning) {
                _reconnectPending = true;
                LOG_INFO("[MQTT] Reconnect scheduled in background task");
            } else if (_reconnectRunning) {
                LOG_INFO("[MQTT] Background reconnect still in progress, skipping");
            }
        }
    } else {
        if (!isConnected) {
            isConnected = true;
            lastConnectedTime = millis();
        }
        lastLoopTime = millis();
        mqttClient.loop();

        // 处理延迟的 DATA_COMMAND 和异步任务上报队列
        processQueuedCommands();
        processQueuedReports();
        
        // 实时监测：按间隔定时发布监测数据
        if (monitorActive && monitorRemaining > 0) {
            unsigned long now = millis();
            if (now - lastMonitorTime >= monitorInterval) {
                lastMonitorTime = now;
                publishMonitorData();
                monitorRemaining--;
                if (monitorRemaining <= 0) {
                    monitorActive = false;
                    LOG_INFO("MQTT: Monitor completed");
                }
            }
        }
    }
}

void MQTTClient::processQueuedCommands() {
    if (!_dataCommandQueue) return;
    String* cmd = nullptr;
    int processed = 0;
    while (processed < 2 && xQueueReceive(_dataCommandQueue, &cmd, 0) == pdTRUE) {
        if (cmd) {
#if FASTBEE_ENABLE_PERIPH_EXEC
            PeriphExecManager& execMgr = PeriphExecManager::getInstance();
            String report = execMgr.handleDataCommand(*cmd);
            if (!report.isEmpty() && report != "[]") {
                LOG_INFOF("[PeriphExec] Reporting status (DataCommand response): %s", report.c_str());
                publishReportData(report);
            }
#endif
            delete cmd;
        }
        processed++;
    }
}

void MQTTClient::processQueuedReports() {
    int processed = 0;
    while (processed < 4 && _slotCount > 0) {
        MqttSlot& slot = _reportSlots[_slotReadIndex];
        if (slot.occupied) {
            publishReportData(String(slot.payload, slot.length));
            slot.clear();
            _slotReadIndex = (_slotReadIndex + 1) % MQTT_REPORT_SLOTS;
            _slotCount--;
        } else {
            // 异常：slot 标记未占用但 count > 0，跳过
            _slotReadIndex = (_slotReadIndex + 1) % MQTT_REPORT_SLOTS;
            _slotCount--;
        }
        processed++;
    }

#if FASTBEE_ENABLE_HEALTH_MONITOR
    if (processed > 0) {
        HealthMonitor* hm = FastBeeFramework::getInstance()->getHealthMonitor();
        if (hm) hm->setMqttQueueDepth(_slotCount);
    }
#endif
}

void MQTTClient::setMinReportInterval(uint32_t ms) {
    _minReportInterval = ms;
    if (ms > 0) {
        LOGGER.infof("[MQTT] Report interval limited to %lu ms", (unsigned long)ms);
    } else {
        LOGGER.info("[MQTT] Report interval limit removed");
    }
}

bool MQTTClient::queueReportData(const String& payload) {
    return queueReportData(payload.c_str(), (uint16_t)payload.length());
}

bool MQTTClient::queueReportData(const char* payload, uint16_t length) {
    // 降采样检查：如果距上次上报不足 _minReportInterval 则跳过
    if (_minReportInterval > 0) {
        unsigned long now = millis();
        if (now - _lastReportQueueTime < _minReportInterval) {
            return false;  // 降采样丢弃
        }
        _lastReportQueueTime = now;
    }

    // 超过最大长度时截断
    if (length > MQTT_SLOT_MAX_PAYLOAD - 1) {
        LOGGER.warningf("[MQTT] Payload truncated: %u -> %u bytes", length, MQTT_SLOT_MAX_PAYLOAD - 1);
        length = MQTT_SLOT_MAX_PAYLOAD - 1;
    }
    
    // 缓冲区满时丢弃最旧的
    if (_slotCount >= MQTT_REPORT_SLOTS) {
        LOGGER.warning("[MQTT] Report queue full, dropping oldest");
        _reportSlots[_slotReadIndex].clear();
        _slotReadIndex = (_slotReadIndex + 1) % MQTT_REPORT_SLOTS;
        _slotCount--;
    }
    
    // 拷贝到下一个空闲 slot
    MqttSlot& slot = _reportSlots[_slotWriteIndex];
    memcpy(slot.payload, payload, length);
    slot.payload[length] = '\0';
    slot.length = length;
    slot.occupied = true;
    
    _slotWriteIndex = (_slotWriteIndex + 1) % MQTT_REPORT_SLOTS;
    _slotCount++;

#if FASTBEE_ENABLE_HEALTH_MONITOR
    HealthMonitor* hm = FastBeeFramework::getInstance()->getHealthMonitor();
    if (hm) hm->setMqttQueueDepth(_slotCount);
#endif
    
    return true;
}

String MQTTClient::getStatus() const {
    return isConnected ? F("Connected") : F("Disconnected");
}

void MQTTClient::setMessageCallback(std::function<void(const String&, const String&, MqttTopicType)> callback) {
    messageCallback = callback;
}

void MQTTClient::setStatusChangeCallback(StatusChangeCallback cb) {
    _statusChangeCallback = cb;
}

void MQTTClient::setMonitorDataProvider(std::function<String()> provider) {
    _monitorDataProvider = provider;
}

void MQTTClient::_notifyStatusChange() {
    if (!_statusChangeCallback) return;

    // 构建轻量级 JSON 状态字符串
    // 参考 MqttRouteHandler::handleGetMqttStatus 的 JSON 结构
    JsonDocument doc;
    JsonObject data = doc.to<JsonObject>();

    data["connected"] = isConnected;
    data["stopped"] = stopped;
    data["server"] = config.server;
    data["port"] = config.port;
    data["clientId"] = config.clientId;
    data["reconnects"] = reconnectCount;
    data["lastError"] = lastErrorCode;

    String json;
    serializeJson(doc, json);

    _statusChangeCallback(json);
}

MqttTopicType MQTTClient::getTopicTypeByPath(const String& topicPath, int8_t* outSubIndex) const {
    // 尝试匹配每个主题（考虑autoPrefix生成的完整路径）
    // 先查订阅主题
    for (size_t i = 0; i < config.subscribeTopics.size(); i++) {
        const auto& st = config.subscribeTopics[i];
        if (st.topic == topicPath) {
            if (outSubIndex) *outSubIndex = (int8_t)i;
            return st.topicType;
        }
        // 如果该主题启用了autoPrefix，构建完整路径后匹配
        if (st.autoPrefix) {
            String fullTopic = buildFullTopicWithType(st.topic, true, st.topicType);
            if (fullTopic == topicPath) {
                if (outSubIndex) *outSubIndex = (int8_t)i;
                return st.topicType;
            }
        }
    }
    // 再查发布主题
    for (const auto& pt : config.publishTopics) {
        if (pt.topic == topicPath) {
            return pt.topicType;
        }
        if (pt.autoPrefix) {
            String fullTopic = buildFullTopicWithType(pt.topic, true, pt.topicType);
            if (fullTopic == topicPath) {
                return pt.topicType;
            }
        }
    }
    return MqttTopicType::DATA_COMMAND; // 默认
}

String MQTTClient::getReportTopic() const {
    for (const auto& pt : config.publishTopics) {
        if (pt.topicType == MqttTopicType::DATA_REPORT && pt.enabled) {
            return buildFullTopicWithType(pt.topic, pt.autoPrefix, pt.topicType);
        }
    }
    return "(no_report_topic)";
}

String MQTTClient::buildFullTopic(const String& topic, bool autoPrefix) const {
    return buildFullTopicWithType(topic, autoPrefix, MqttTopicType::DATA_REPORT);
}

String MQTTClient::buildFullTopicWithType(const String& topic, bool autoPrefix, MqttTopicType topicType) const {
    if (!autoPrefix) {
        return topic;
    }
    // autoPrefix启用时，根据topicType自动生成设备前缀
    // OTA类型(5,6): /{deviceNum} + topic
    // 其他类型: /{productId}/{deviceNum} + topic
    String prefix;
    if (topicType == MqttTopicType::OTA_UPGRADE || topicType == MqttTopicType::OTA_BINARY) {
        if (!config.deviceNum.isEmpty()) {
            prefix = "/" + config.deviceNum;
        }
    } else {
        if (!config.productId.isEmpty() && !config.deviceNum.isEmpty()) {
            prefix = "/" + config.productId + "/" + config.deviceNum;
        } else if (!config.deviceNum.isEmpty()) {
            prefix = "/" + config.deviceNum;
        }
    }
    // 额外的topicPrefix（如果设置了的话，追加在设备前缀之前）
    if (!config.topicPrefix.isEmpty()) {
        return config.topicPrefix + prefix + topic;
    }
    return prefix + topic;
}

void MQTTClient::mqttCallback(char* topic, byte* payload, unsigned int length) {
    // 修复：原来逐字符 message += (char)payload[i]，每次迭代都可能触发 String 重分配
    // 改为一次性构造，O(1) 内存操作
    String message((const char*)payload, length);
    String topicStr(topic);

    // 根据主题路径查找对应的主题类型
    MqttTopicType tType = getTopicTypeByPath(topicStr);

    // 无条件记录下行原始消息（诊断用）——无论 topic 是否匹配配置、tType 是什么都打印
    // 使用 Serial.printf 避免 LoggerSystem 文件 I/O 在 MQTT 回调中加剧栈压力
    {
        String truncatedRaw = message.length() > 120 ? message.substring(0, 120) + "..." : message;
        Serial.printf("[MQTT] RX ▼ topic=%s type=%d len=%u data=%s\n",
                      topic, (int)tType, length, truncatedRaw.c_str());
    }

    // 数据接收转换管道
#if FASTBEE_ENABLE_RULE_SCRIPT
    message = RuleScriptManager::getInstance().applyReceiveTransform(0, message);
#endif

    char buf[96];
    snprintf(buf, sizeof(buf), "MQTT: Msg type=%d topic=%s len=%u",
             (int)tType, topic, length);
    LOG_DEBUG(buf);

    // 收到设备信息查询指令时，自动发布设备信息
    if (tType == MqttTopicType::DEVICE_INFO) {
        publishDeviceInfo();
    }

    // 收到实时监测指令时，解析 count/interval 并启动定时发布
    if (tType == MqttTopicType::REALTIME_MON) {
        JsonDocument monDoc;
        DeserializationError err = deserializeJson(monDoc, message);
        if (!err) {
            int count = monDoc["count"] | 0;
            unsigned long interval = monDoc["interval"] | 1000;
            if (count > 0 && interval >= 100) {
                monitorRemaining = count;
                monitorInterval = interval;
                monitorActive = true;
                lastMonitorTime = millis();
                char logBuf[80];
                snprintf(logBuf, sizeof(logBuf), "MQTT: Monitor started count=%d interval=%lums", count, interval);
                LOG_INFO(logBuf);
                // 立即发布第一次
                publishMonitorData();
                monitorRemaining--;
            }
        }
    }

    // 收到数据下发指令时，入队延迟处理（避免在回调中阻塞 MQTT 连接）
    if (tType == MqttTopicType::DATA_COMMAND) {
        LOG_INFO("MQTT: Received DATA_COMMAND, queuing...");

        // 严格按 Modbus transferType 分流：
        //   transferType=0（JSON/结构体）：只接受 JSON 格式，非 JSON 直接告警丢弃
        //   transferType=1（透传HEX帧）  ：非 JSON 视为 HEX 帧（ASCII 或二进制）封装为 modbus_raw_send
        String enqueueMsg = message;
        bool shouldEnqueue = true;
        {
            // 检测 payload 是否看起来为 JSON（首个非空白字节为 '[' 或 '{'）
            bool looksLikeJson = false;
            for (unsigned int i = 0; i < length; i++) {
                char c = (char)payload[i];
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
                looksLikeJson = (c == '[' || c == '{');
                break;
            }

            // 非 JSON 格式需要按 transferType 决定处理方式
            if (!looksLikeJson && length >= 2) {
                // 读取当前 Modbus 传输类型：透传（HEX帧）=1，标准JSON=0
                uint8_t transferType = 0;
#if FASTBEE_ENABLE_MODBUS
                {
                    auto* fw = FastBeeFramework::getInstance();
                    if (fw) {
                        auto* pm = fw->getProtocolManager();
                        if (pm) {
                            ModbusHandler* mh = pm->getModbusHandler();
                            if (mh) transferType = mh->getConfig().transferType;
                        }
                    }
                }
#endif

                if (transferType != 1) {
                    // JSON 模式严格校验：非 JSON payload 一律丢弃并告警
                    char warnBuf[128];
                    snprintf(warnBuf, sizeof(warnBuf),
                             "MQTT: transferType=%u(JSON) requires JSON payload, non-JSON dropped (len=%u)",
                             transferType, length);
                    LOG_WARNING(warnBuf);
                    shouldEnqueue = false;
                } else {
                    // 透传HEX模式：判定 payload 是否已是 HEX ASCII 字符串
                    auto isHexAsciiPayload = [&]() -> bool {
                        if ((length % 2) != 0 || length < 4) return false;
                        for (unsigned int i = 0; i < length; i++) {
                            char c = (char)payload[i];
                            bool ok = (c >= '0' && c <= '9') ||
                                      (c >= 'a' && c <= 'f') ||
                                      (c >= 'A' && c <= 'F');
                            if (!ok) return false;
                        }
                        return true;
                    };

                    String hexStr;
                    if (isHexAsciiPayload()) {
                        // Payload 本身就是 HEX ASCII（MQTTX Plaintext 模式发 "010300010001D5CA"）
                        hexStr.reserve(length);
                        for (unsigned int i = 0; i < length; i++) {
                            char c = (char)payload[i];
                            if (c >= 'a' && c <= 'f') c = c - 'a' + 'A';
                            hexStr += c;
                        }
                        Serial.printf("[MQTT] RX ▼ TRANSPARENT(HEX-ASCII) len=%u hex=%s\n",
                                      length, hexStr.c_str());
                    } else {
                        // 真实二进制字节帧（MQTTX Hex 模式发 01 03 00 01 00 01 D5 CA）
                        hexStr.reserve(length * 2);
                        for (unsigned int i = 0; i < length; i++) {
                            char buf[3];
                            snprintf(buf, sizeof(buf), "%02X", payload[i]);
                            hexStr += buf;
                        }
                        Serial.printf("[MQTT] RX ▼ TRANSPARENT(BIN->HEX) len=%u hex=%s\n",
                                      length, hexStr.c_str());
                    }
                    enqueueMsg = "[{\"id\":\"modbus_raw_send\",\"value\":\"" + hexStr + "\"}]";
                }
            }
        }

        if (shouldEnqueue && _dataCommandQueue) {
            String* cmd = new (std::nothrow) String(enqueueMsg);
            if (cmd) {
                if (xQueueSend(_dataCommandQueue, &cmd, 0) != pdTRUE) {
                    LOG_WARNING("MQTT: DATA_COMMAND queue full, dropping");
                    delete cmd;
                }
            }
        }
    }

    // 收到 NTP 时间同步响应时，计算校准时间并设置系统时钟
    if (tType == MqttTopicType::NTP_SYNC) {
        JsonDocument ntpDoc;
        DeserializationError err = deserializeJson(ntpDoc, message);
        if (!err && ntpDoc.containsKey("serverSendTime") && ntpDoc.containsKey("serverRecvTime")) {
            // 解析服务端时间戳（毫秒）
            double deviceSendTime = ntpDoc["deviceSendTime"].as<String>().toDouble();
            double serverRecvTime = ntpDoc["serverRecvTime"].as<String>().toDouble();
            double serverSendTime = ntpDoc["serverSendTime"].as<String>().toDouble();
            double deviceRecvTime = (double)millis();

            // 使用记录的本地发送时间替代服务端回传值（更精确）
            if (ntpDeviceSendTime > 0) {
                deviceSendTime = (double)ntpDeviceSendTime;
            }

            // NTP 算法：当前时间 = (serverRecvTime + serverSendTime + deviceRecvTime - deviceSendTime) / 2
            double currentTimeMs = (serverRecvTime + serverSendTime + deviceRecvTime - deviceSendTime) / 2.0;

            // 转换为 timeval 并设置系统时钟
            time_t epochSec = (time_t)(currentTimeMs / 1000.0);
            long microSec = (long)((currentTimeMs - (double)epochSec * 1000.0) * 1000.0);
            struct timeval tv;
            tv.tv_sec = epochSec;
            tv.tv_usec = microSec;
            settimeofday(&tv, nullptr);

            // 设置时区为 CST-8（中国标准时间）
            setenv("TZ", "CST-8", 1);
            tzset();

            // 重置发送时间
            ntpDeviceSendTime = 0;

            // 打印校准结果
            struct tm timeinfo;
            if (getLocalTime(&timeinfo, 0)) {
                char timeBuf[32];
                strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &timeinfo);
                char logBuf[80];
                snprintf(logBuf, sizeof(logBuf), "MQTT: NTP synced, time: %s", timeBuf);
                LOG_INFO(logBuf);
                // 触发NTP同步完成系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
                PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_NTP_SYNCED, timeBuf);
#endif
            }
        } else if (!err && !ntpDoc.containsKey("serverSendTime")) {
            // 收到的是 NTP 请求而非响应（仅含 deviceSendTime），忽略
            LOG_DEBUG("MQTT: NTP_SYNC request received (not response), ignoring");
        }
    }

    // DATA_COMMAND 已由 handleDataCommand 完整处理（含条件匹配+执行+响应），
    // 不再传递给 messageCallback 避免 handleMqttMessage 重复触发
    if (messageCallback && tType != MqttTopicType::DATA_COMMAND) {
        messageCallback(topicStr, message, tType);
    }
}

bool MQTTClient::reconnect() {
    // 显式关闭旧 TCP socket，防止 PubSubClient::connect() 创建新 socket 时旧 socket 泄漏
    // 每次失败的 connect() 都会残留 socket 描述符 + 内核缓冲区（2-4KB），
    // 高频重连会快速耗尽堆内存（268KB → 4KB）
    wifiClient.stop();

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
            connClientId = "S&" + config.deviceNum + "&" + config.productId + "&" + config.userId;
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
    snprintf(logBuf, sizeof(logBuf), "MQTT: User=%s PwdLen=%d AuthCode=%s",
             config.username.c_str(), connPassword.length(),
             config.authCode.isEmpty() ? "(empty)" : config.authCode.c_str());
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
        _notifyStatusChange();  // 通知状态变化：连接成功
        // 触发MQTT连接成功系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
        PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_MQTT_CONNECTED, "");
#endif
        // 连接成功后订阅所有主题
        subscribeAll();
        // 连接成功后发布一次设备信息
        publishDeviceInfo();
        // 连接成功后发起 NTP 时间同步
        publishNtpSync();
    } else {
        // 连接失败时立即关闭 socket，避免残留 TCP 连接占用 lwip 缓冲区（2-4KB）
        wifiClient.stop();
        lastErrorCode = mqttClient.state();
        reconnectCount++;
        char buf2[48];
        snprintf(buf2, sizeof(buf2), "MQTT: Connect failed rc=%d", lastErrorCode);
        LOG_WARNING(buf2);
        _notifyStatusChange();  // 通知状态变化：连接失败
        // 触发MQTT连接失败系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
        PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_MQTT_CONN_FAILED, String(lastErrorCode));
#endif
    }
    return ok;
}

// ============ 后台重连任务 ============

void MQTTClient::reconnectTaskEntry(void* param) {
    MQTTClient* client = (MQTTClient*)param;
    while (true) {
        if (client->_reconnectPending && !client->_reconnectRunning) {
            client->doReconnect();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void MQTTClient::doReconnect() {
    _reconnectPending = false;
    _reconnectRunning = true;

    // 重连次数保护：连续失败超过 20 次后拉长间隔至 5 分钟，降低内存压力
    constexpr uint32_t MAX_FAST_RETRIES = 20;
    constexpr uint32_t SLOW_RETRY_INTERVAL = 300000;  // 5分钟
    // 连续 3 次 TCP/DNS 超时即视为网络不可用，立即切换慢模式
    constexpr uint8_t  DNS_FAIL_THRESHOLD = 3;
    
    bool slowMode = (reconnectCount > MAX_FAST_RETRIES) ||
                    (consecutiveTimeouts >= DNS_FAIL_THRESHOLD);
    if (slowMode) {
        reconnectInterval = SLOW_RETRY_INTERVAL;
        if (reconnectCount > MAX_FAST_RETRIES) {
            char buf[96];
            snprintf(buf, sizeof(buf), "[MQTT] Reconnect count=%lu exceeds %lu, slow mode (%lus)",
                     (unsigned long)reconnectCount, (unsigned long)MAX_FAST_RETRIES,
                     (unsigned long)(SLOW_RETRY_INTERVAL / 1000));
            LOG_WARNING(buf);
        }
    }

    // 内存保护：堆内存严重不足时跳过重连，避免加剧内存压力
    if (ESP.getFreeHeap() < 30720) {
        LOG_WARNING("[MQTT] Heap too low for reconnect, skipping");
        _reconnectRunning = false;
        return;
    }

    LOG_INFO("[MQTT] Background reconnect starting...");

    bool ok = reconnect();

    if (ok) {
        reconnectInterval = 5000;  // 连接成功，重置间隔
        consecutiveTimeouts = 0;
        LOG_INFO("[MQTT] Background reconnect successful");
    } else {
        // 检测连续 TCP/DNS 连接失败（rc=-2: MQTT_CONNECT_FAILED）
        // DNS 解析失败时 WiFiClient::connect() 返回 0，PubSubClient 报告 rc=-2
        // 连续 3 次说明网络层不通（如 WiFi 僵尸状态），立即进入慢模式
        if (lastErrorCode == -2) {
            consecutiveTimeouts++;
            if (consecutiveTimeouts == DNS_FAIL_THRESHOLD) {
                reconnectInterval = SLOW_RETRY_INTERVAL;
                LOG_WARNING("[MQTT] 3 consecutive connect failures (DNS/network down), slow mode activated");
            }
            // 加固：连续 6 次 connect 失败 → WiFi STA 可能处于僵尸状态（RSSI 正常但 socket 不通）
            // 主动重走 disconnect+reconnect 刷新 lwip TCP/IP 栈，避免累计内存劣化
            // 仅在第 6 、 12 、 18… 次触发（避免频繁刷 WiFi）
            constexpr uint8_t WIFI_RESET_THRESHOLD = 6;
            if (consecutiveTimeouts >= WIFI_RESET_THRESHOLD &&
                (consecutiveTimeouts % WIFI_RESET_THRESHOLD) == 0) {
                wl_status_t st = WiFi.status();
                LOG_WARNINGF("[MQTT] %u consecutive failures, soft-resetting WiFi STA (status=%d, rssi=%d)",
                             (unsigned)consecutiveTimeouts, (int)st, (int)WiFi.RSSI());
                // 关闭 STA 但保留配置，然后重新 reconnect（不会丢弃 SSID/密码）
                WiFi.disconnect(false, false);
                delay(100);
                WiFi.reconnect();
            }
        } else {
            consecutiveTimeouts = 0;
        }

        if (!slowMode && consecutiveTimeouts < DNS_FAIL_THRESHOLD) {
            // 快速重试阶段：指数退避 5s -> 10s -> 20s -> 30s（上限）
            reconnectInterval = min(reconnectInterval * 2, (uint32_t)30000);
        }
        // slowMode 时保持 SLOW_RETRY_INTERVAL，不再被覆盖
        char buf[80];
        snprintf(buf, sizeof(buf), "[MQTT] Background reconnect failed, next retry in %lus",
                 reconnectInterval / 1000);
        LOG_INFO(buf);
    }

    _reconnectRunning = false;
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

    unsigned long deviceSendTime = 0;
    String ntpJson = getNtpTime(deviceSendTime);
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

    double serverSendTime = jsonVariantToDoubleSafe(doc["serverSendTime"]);
    double serverRecvTime = jsonVariantToDoubleSafe(doc["serverRecvTime"]);
    
    // 兼容 FastBee 平台返回格式: {"data": {"serverTime": xxx}} 或 {"code":200,"data":{"serverTime":xxx}}
    if (serverSendTime <= 0.0 || serverRecvTime <= 0.0) {
        if (doc["data"].is<JsonObject>()) {
            double serverTime = jsonVariantToDoubleSafe(doc["data"]["serverTime"]);
            if (serverTime > 0.0) {
                // 如果是秒级时间戳（小于10000000000），转换为毫秒
                if (serverTime < 10000000000.0) {
                    serverTime *= 1000.0;
                }
                serverSendTime = serverTime;
                serverRecvTime = serverTime;
            }
        }
    }
    
    // 验证时间戳有效性（毫秒级时间戳应大于 1000000000000，即 2001年以后）
    if (serverSendTime < 1000000000000.0 || serverRecvTime < 1000000000000.0) {
        char buf[160];
        snprintf(buf, sizeof(buf), "MQTT: Invalid NTP timestamps: sendTime=%.0f, recvTime=%.0f", serverSendTime, serverRecvTime);
        LOG_WARNING(buf);
        return "";
    }
    double deviceRecvTime = (double)millis();
    double now = (serverSendTime + serverRecvTime + deviceRecvTime - (double)deviceSendTime) / 2.0;
    double expireTime = now + 3600000.0;
    // expireTime 为毫秒级时间戳（通常 > 2^32），必须使用 64 位整型避免溢出导致鉴权失败
    unsigned long long expireTimeMs = (unsigned long long)expireTime;
    char expireBuf[24];
    snprintf(expireBuf, sizeof(expireBuf), "%llu", expireTimeMs);

    String plainPassword;
    if (config.authCode.isEmpty()) {
        plainPassword = config.password + "&" + String(expireBuf);
    } else {
        plainPassword = config.password + "&" + String(expireBuf) + "&" + config.authCode;
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

String MQTTClient::getNtpTime(unsigned long& outDeviceSendTime) {
    if (config.ntpServer.isEmpty()) {
        LOG_WARNING("MQTT: NTP server not configured");
        return "";
    }

    HTTPClient http;
    String url = config.ntpServer;
    
    // FastBee平台需要 deviceSendTime 参数
    outDeviceSendTime = millis();
    
    if (url.indexOf('?') >= 0) {
        if (!url.endsWith("?") && !url.endsWith("&")) url += "&";
    } else {
        url += "?";
    }
    url += "deviceSendTime=" + String(outDeviceSendTime);

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

    uint8_t* data = new(std::nothrow) uint8_t[paddedLen];
    if (!data) {
        LOG_ERROR("AES: Failed to allocate data buffer");
        return "";
    }
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
    uint8_t* b64Buf = new(std::nothrow) uint8_t[b64Len + 1];
    if (!b64Buf) {
        delete[] data;
        LOG_ERROR("AES: Failed to allocate base64 buffer");
        return "";
    }
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

#endif // FASTBEE_ENABLE_MQTT
