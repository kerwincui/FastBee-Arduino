#include "./network/handlers/ProtocolRouteHandler.h"
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "./protocols/ProtocolManager.h"
#include "./protocols/ModbusHandler.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>

static const char* PROTOCOL_CONFIG_PATH = "/config/protocol.json";
static const char* MQTT_AES_IV = "wumei-smart-open";

static String aesEncryptForMqttTest(const String& plainData, const String& key, const String& iv) {
    if (key.length() < 16 || iv.length() < 16) return "";

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
        return "";
    }

    ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, paddedLen, ivBuf, data, data);
    mbedtls_aes_free(&aes);
    if (ret != 0) {
        delete[] data;
        return "";
    }

    size_t b64Len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64Len, data, paddedLen);
    uint8_t* b64Buf = new uint8_t[b64Len + 1];
    ret = mbedtls_base64_encode(b64Buf, b64Len + 1, &b64Len, data, paddedLen);
    delete[] data;
    if (ret != 0) {
        delete[] b64Buf;
        return "";
    }

    String result = String((char*)b64Buf, b64Len);
    delete[] b64Buf;
    return result;
}

static String fetchNtpTimeForMqttTest(const String& ntpServer, unsigned long& outDeviceSendTime) {
    if (ntpServer.isEmpty()) return "";
    HTTPClient http;
    String url = ntpServer;
    
    // FastBee平台需要 deviceSendTime 参数
    outDeviceSendTime = millis();
    
    if (url.indexOf('?') >= 0) {
        if (!url.endsWith("?") && !url.endsWith("&")) url += "&";
    } else {
        url += "?";
    }
    url += "deviceSendTime=" + String(outDeviceSendTime);
    
    if (!http.begin(url)) return "";
    http.setTimeout(5000);
    int httpCode = http.GET();
    String payload = "";
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        payload = http.getString();
    }
    http.end();
    return payload;
}

static String jsonVariantToString(JsonVariantConst v) {
    if (v.isNull()) return "";
    if (v.is<const char*>()) return String(v.as<const char*>());
    if (v.is<String>()) return v.as<String>();
    if (v.is<long long>()) return String(v.as<long long>());
    if (v.is<unsigned long long>()) return String(v.as<unsigned long long>());
    if (v.is<long>()) return String(v.as<long>());
    if (v.is<unsigned long>()) return String(v.as<unsigned long>());
    if (v.is<int>()) return String(v.as<int>());
    if (v.is<unsigned int>()) return String(v.as<unsigned int>());
    if (v.is<float>()) return String(v.as<float>(), 6);
    if (v.is<double>()) return String(v.as<double>(), 6);
    return "";
}

static double jsonVariantToDouble(JsonVariantConst v) {
    if (v.isNull()) return 0.0;
    if (v.is<double>()) return v.as<double>();
    if (v.is<float>()) return static_cast<double>(v.as<float>());
    if (v.is<long long>()) return static_cast<double>(v.as<long long>());
    if (v.is<unsigned long long>()) return static_cast<double>(v.as<unsigned long long>());
    if (v.is<long>()) return static_cast<double>(v.as<long>());
    if (v.is<unsigned long>()) return static_cast<double>(v.as<unsigned long>());
    if (v.is<int>()) return static_cast<double>(v.as<int>());
    if (v.is<unsigned int>()) return static_cast<double>(v.as<unsigned int>());
    String s = jsonVariantToString(v);
    return s.isEmpty() ? 0.0 : s.toDouble();
}

static String buildEncryptedPasswordForMqttTest(const String& password,
                                                const String& authCode,
                                                const String& mqttSecret,
                                                const String& ntpServer,
                                                String* errMsg = nullptr) {
    if (mqttSecret.length() < 16) {
        if (errMsg) *errMsg = "mqttSecret too short (need 16+ chars)";
        return "";
    }
    if (ntpServer.isEmpty()) {
        if (errMsg) *errMsg = "NTP server is empty";
        return "";
    }
    unsigned long deviceSendTime = 0;
    String ntpJson = fetchNtpTimeForMqttTest(ntpServer, deviceSendTime);
    if (ntpJson.isEmpty()) {
        if (errMsg) *errMsg = "NTP request failed";
        return "";
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, ntpJson);
    if (err) {
        if (errMsg) *errMsg = String("NTP response parse failed: ") + err.c_str();
        return "";
    }

    double serverSendTime = jsonVariantToDouble(doc["serverSendTime"]);
    double serverRecvTime = jsonVariantToDouble(doc["serverRecvTime"]);
    
    // 兼容 FastBee 平台返回格式: {"data": {"serverTime": xxx}} 或 {"code":200,"data":{"serverTime":xxx}}
    if (serverSendTime <= 0.0 || serverRecvTime <= 0.0) {
        if (doc["data"].is<JsonObject>()) {
            double serverTime = jsonVariantToDouble(doc["data"]["serverTime"]);
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
        if (errMsg) {
            *errMsg = "NTP timestamps invalid: serverSendTime=" + String(serverSendTime) + 
                      ", serverRecvTime=" + String(serverRecvTime) + 
                      ", raw=" + ntpJson.substring(0, 100);
        }
        return "";
    }
    double deviceRecvTime = (double)millis();
    double now = (serverSendTime + serverRecvTime + deviceRecvTime - (double)deviceSendTime) / 2.0;
    double expireTime = now + 3600000.0;
    // 毫秒时间戳必须使用 64 位，32 位会溢出导致 AES 鉴权失败
    unsigned long long expireTimeMs = (unsigned long long)expireTime;
    char expireBuf[24];
    snprintf(expireBuf, sizeof(expireBuf), "%llu", expireTimeMs);

    String plainPassword = password + "&" + String(expireBuf);
    if (!authCode.isEmpty()) {
        plainPassword += "&" + authCode;
    }
    return aesEncryptForMqttTest(plainPassword, mqttSecret, MQTT_AES_IV);
}

ProtocolRouteHandler::ProtocolRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void ProtocolRouteHandler::setupRoutes(AsyncWebServer* server) {
    server->on("/api/protocol/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleGetProtocolConfig(request);
    });

    server->on("/api/protocol/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleSaveProtocolConfig(request);
    });

    // Modbus Master API
    server->on("/api/modbus/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleGetModbusStatus(request);
    });

    server->on("/api/modbus/write", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusWrite(request);
    });

    // MQTT Connection Test API
    server->on("/api/mqtt/test", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleTestMqttConnection(request);
    });

    // MQTT Status API
    server->on("/api/mqtt/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleGetMqttStatus(request);
    });

    // MQTT Reconnect API
    server->on("/api/mqtt/reconnect", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleMqttReconnect(request);
    });

    // MQTT Disconnect API
    server->on("/api/mqtt/disconnect", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleMqttDisconnect(request);
    });

    // MQTT NTP Time Sync API
    server->on("/api/mqtt/ntp-sync", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleMqttNtpSync(request);
    });
}

void ProtocolRouteHandler::handleGetProtocolConfig(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    JsonDocument doc;

    if (LittleFS.exists(PROTOCOL_CONFIG_PATH)) {
        File f = LittleFS.open(PROTOCOL_CONFIG_PATH, "r");
        if (f) {
            JsonDocument fileCfg;
            DeserializationError err = deserializeJson(fileCfg, f);
            f.close();

            if (!err) {
                doc["success"] = true;
                doc["data"] = fileCfg;
                String out;
                serializeJson(doc, out);
                request->send(200, "application/json", out);
                return;
            }
        }
    }

    doc["success"] = true;
    doc["data"].to<JsonObject>();
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void ProtocolRouteHandler::handleSaveProtocolConfig(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    JsonDocument doc;

    #define GP(key, def) ctx->getParamValue(request, key, def)
    #define GPI(key, def) GP(key, def).toInt()

    // Modbus RTU
    doc["modbusRtu"]["enabled"] = GP("modbusRtu_enabled", "false") == "true";
    doc["modbusRtu"]["baudRate"] = GPI("modbusRtu_baudRate", "9600");
    doc["modbusRtu"]["timeout"] = GPI("modbusRtu_timeout", "1000");
    doc["modbusRtu"]["mode"] = GP("modbusRtu_mode", "master");
    doc["modbusRtu"]["dePin"] = GPI("modbusRtu_dePin", "-1");
    doc["modbusRtu"]["transferType"] = GPI("modbusRtu_transferType", "0");
    doc["modbusRtu"]["workMode"] = GPI("modbusRtu_workMode", "1");

    // Modbus RTU Master 配置
    doc["modbusRtu"]["master"]["defaultPollInterval"] = GPI("modbusRtu_master_defaultPollInterval", "30");
    doc["modbusRtu"]["master"]["responseTimeout"] = GPI("modbusRtu_master_responseTimeout", "1000");
    doc["modbusRtu"]["master"]["maxRetries"] = GPI("modbusRtu_master_maxRetries", "2");
    doc["modbusRtu"]["master"]["interPollDelay"] = GPI("modbusRtu_master_interPollDelay", "100");

    // Modbus RTU Master 轮询任务
    String masterTasksJson = GP("modbusRtu_master_tasks", "[]");
    JsonArray masterTasks = doc["modbusRtu"]["master"]["tasks"].to<JsonArray>();
    if (masterTasksJson.length() > 2) {
        JsonDocument tasksDoc;
        DeserializationError tasksErr = deserializeJson(tasksDoc, masterTasksJson);
        if (!tasksErr && tasksDoc.is<JsonArray>()) {
            JsonArray arr = tasksDoc.as<JsonArray>();
            for (JsonVariant v : arr) {
                JsonObject taskObj = masterTasks.add<JsonObject>();
                taskObj["slaveAddress"] = v["slaveAddress"] | 1;
                taskObj["functionCode"] = v["functionCode"] | 3;
                taskObj["startAddress"] = v["startAddress"] | 0;
                taskObj["quantity"] = v["quantity"] | 10;
                taskObj["pollInterval"] = v["pollInterval"] | 30;
                taskObj["enabled"] = v["enabled"] | true;
                taskObj["label"] = v["label"] | "";
                
                // 寄存器映射
                if (v.containsKey("mappings") && v["mappings"].is<JsonArray>()) {
                    JsonArray mappingsArr = taskObj["mappings"].to<JsonArray>();
                    for (JsonVariant mv : v["mappings"].as<JsonArray>()) {
                        JsonObject mo = mappingsArr.add<JsonObject>();
                        mo["regOffset"] = mv["regOffset"] | 0;
                        mo["dataType"] = mv["dataType"] | 0;
                        mo["scaleFactor"] = mv["scaleFactor"] | 1.0;
                        mo["decimalPlaces"] = mv["decimalPlaces"] | 1;
                        mo["sensorId"] = mv["sensorId"] | "";
                    }
                }
            }
        }
    }

    // Modbus TCP
    doc["modbusTcp"]["enabled"] = GP("modbusTcp_enabled", "false") == "true";
    doc["modbusTcp"]["server"] = GP("modbusTcp_server", "192.168.1.100");
    doc["modbusTcp"]["port"] = GPI("modbusTcp_port", "502");
    doc["modbusTcp"]["slaveId"] = GPI("modbusTcp_slaveId", "1");
    doc["modbusTcp"]["timeout"] = GPI("modbusTcp_timeout", "5000");

    // MQTT
    doc["mqtt"]["enabled"] = GP("mqtt_enabled", "true") == "true";
    doc["mqtt"]["server"] = GP("mqtt_server", "iot.fastbee.cn");
    doc["mqtt"]["port"] = GPI("mqtt_port", "1883");
    doc["mqtt"]["clientId"] = GP("mqtt_clientId", "");
    doc["mqtt"]["username"] = GP("mqtt_username", "");
    doc["mqtt"]["password"] = GP("mqtt_password", "");
    doc["mqtt"]["keepAlive"] = GPI("mqtt_keepAlive", "60");
    doc["mqtt"]["autoReconnect"] = GP("mqtt_autoReconnect", "true") == "true";
    doc["mqtt"]["connectionTimeout"] = GPI("mqtt_connectionTimeout", "30000");
    // MQTT 认证配置
    doc["mqtt"]["authType"] = GPI("mqtt_authType", "0");
    doc["mqtt"]["mqttSecret"] = GP("mqtt_mqttSecret", "");
    doc["mqtt"]["authCode"] = GP("mqtt_authCode", "");
    // MQTT 遗嘱消息
    doc["mqtt"]["willTopic"] = GP("mqtt_willTopic", "");
    doc["mqtt"]["willPayload"] = GP("mqtt_willPayload", "");
    doc["mqtt"]["willQos"] = GPI("mqtt_willQos", "0");
    doc["mqtt"]["willRetain"] = GP("mqtt_willRetain", "false") == "true";

    // Card 高级配置
    doc["mqtt"]["longitude"] = atof(GP("mqtt_longitude", "0").c_str());
    doc["mqtt"]["latitude"]  = atof(GP("mqtt_latitude", "0").c_str());
    doc["mqtt"]["iccid"]     = GP("mqtt_iccid", "");
    doc["mqtt"]["cardPlatformId"] = GPI("mqtt_cardPlatformId", "0");
    doc["mqtt"]["summary"]   = GP("mqtt_summary", "");

    // 发布主题配置
    String publishTopicsJson = GP("mqtt_publishTopics", "[]");
    JsonArray publishTopics = doc["mqtt"]["publishTopics"].to<JsonArray>();
    if (publishTopicsJson.length() > 2) {
        JsonDocument topicsDoc;
        DeserializationError err = deserializeJson(topicsDoc, publishTopicsJson);
        if (!err && topicsDoc.is<JsonArray>()) {
            JsonArray arr = topicsDoc.as<JsonArray>();
            for (JsonVariant v : arr) {
                JsonObject topicObj = publishTopics.add<JsonObject>();
                topicObj["topic"] = v["topic"] | "";
                topicObj["qos"] = v["qos"] | 0;
                topicObj["retain"] = v["retain"] | false;
                topicObj["enabled"] = v["enabled"] | true;
                topicObj["autoPrefix"] = v["autoPrefix"] | false;
                topicObj["topicType"] = v["topicType"] | 0;
            }
        }
    }
    if (publishTopics.size() == 0) {
        JsonObject defaultTopic = publishTopics.add<JsonObject>();
        defaultTopic["topic"] = "";
        defaultTopic["qos"] = 0;
        defaultTopic["retain"] = false;
        defaultTopic["enabled"] = true;
        defaultTopic["autoPrefix"] = false;
        defaultTopic["topicType"] = 0;
    }

    // 订阅主题配置
    String subscribeTopicsJson = GP("mqtt_subscribeTopics", "[]");
    JsonArray subscribeTopics = doc["mqtt"]["subscribeTopics"].to<JsonArray>();
    if (subscribeTopicsJson.length() > 2) {
        JsonDocument subTopicsDoc;
        DeserializationError err = deserializeJson(subTopicsDoc, subscribeTopicsJson);
        if (!err && subTopicsDoc.is<JsonArray>()) {
            JsonArray arr = subTopicsDoc.as<JsonArray>();
            for (JsonVariant v : arr) {
                JsonObject topicObj = subscribeTopics.add<JsonObject>();
                topicObj["topic"] = v["topic"] | "";
                topicObj["qos"] = v["qos"] | 0;
                topicObj["enabled"] = v["enabled"] | true;
                topicObj["autoPrefix"] = v["autoPrefix"] | false;
                topicObj["topicType"] = v["topicType"] | 1;
            }
        }
    }
    if (subscribeTopics.size() == 0) {
        JsonObject defaultTopic = subscribeTopics.add<JsonObject>();
        defaultTopic["topic"] = "";
        defaultTopic["qos"] = 0;
        defaultTopic["enabled"] = true;
        defaultTopic["autoPrefix"] = false;
        defaultTopic["topicType"] = 1;
    }

    // HTTP
    doc["http"]["enabled"] = GP("http_enabled", "false") == "true";
    doc["http"]["url"] = GP("http_url", "https://api.example.com");
    doc["http"]["port"] = GPI("http_port", "80");
    doc["http"]["method"] = GP("http_method", "POST");
    doc["http"]["timeout"] = GPI("http_timeout", "30");
    doc["http"]["interval"] = GPI("http_interval", "60");
    doc["http"]["retry"] = GPI("http_retry", "3");
    // HTTP 认证配置
    doc["http"]["authType"] = GP("http_authType", "none");
    doc["http"]["authUser"] = GP("http_authUser", "");
    doc["http"]["authToken"] = GP("http_authToken", "");
    doc["http"]["contentType"] = GP("http_contentType", "application/json");

    // CoAP
    doc["coap"]["enabled"] = GP("coap_enabled", "false") == "true";
    doc["coap"]["server"] = GP("coap_server", "coap://example.com");
    doc["coap"]["port"] = GPI("coap_port", "5683");
    doc["coap"]["method"] = GP("coap_method", "POST");
    doc["coap"]["path"] = GP("coap_path", "sensors/temperature");
    doc["coap"]["msgType"] = GP("coap_msgType", "CON");
    doc["coap"]["retransmit"] = GPI("coap_retransmit", "3");
    doc["coap"]["timeout"] = GPI("coap_timeout", "5000");

    // TCP
    doc["tcp"]["enabled"] = GP("tcp_enabled", "false") == "true";
    doc["tcp"]["server"] = GP("tcp_server", "192.168.1.200");
    doc["tcp"]["port"] = GPI("tcp_port", "5000");
    doc["tcp"]["timeout"] = GPI("tcp_timeout", "5000");
    doc["tcp"]["keepAlive"] = GPI("tcp_keepAlive", "60");
    doc["tcp"]["maxRetry"] = GPI("tcp_maxRetry", "5");
    doc["tcp"]["reconnectInterval"] = GPI("tcp_reconnectInterval", "10");
    doc["tcp"]["mode"] = GP("tcp_mode", "client");
    doc["tcp"]["heartbeatMsg"] = GP("tcp_heartbeatMsg", "\\n");
    doc["tcp"]["maxClients"] = GPI("tcp_maxClients", "5");
    doc["tcp"]["idleTimeout"] = GPI("tcp_idleTimeout", "120");
    doc["tcp"]["localPort"] = GPI("tcp_localPort", "8080");

    #undef GP
    #undef GPI

    File f = LittleFS.open(PROTOCOL_CONFIG_PATH, "w");
    if (!f) {
        ctx->sendError(request, 500, "Failed to save protocol config");
        return;
    }

    serializeJsonPretty(doc, f);
    f.close();

    // 保存成功后，根据MQTT启用状态处理连接
    bool mqttReconnected = false;
    bool mqttDisconnected = false;
    int mqttError = 0;
    if (doc["mqtt"]["enabled"].as<bool>()) {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            // 使用非阻塞重启：仅重载配置，由 loop 自动重连
            // 避免 PubSubClient::connect() 阻塞 Web 服务器
            mqttReconnected = pm->restartMQTTDeferred();
            if (!mqttReconnected) {
                mqttError = -1;  // begin() 失败
            }
        }
    } else {
        // MQTT未启用，断开现有连接
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            pm->stopMQTT();
            mqttDisconnected = true;
        }
    }

    // 保存成功后，根据Modbus启用状态处理
    bool modbusRestarted = false;
    if (doc["modbusRtu"]["enabled"].as<bool>()) {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            modbusRestarted = pm->restartModbus();
        }
    } else {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            pm->stopModbus();
        }
    }

    JsonDocument resp;
    resp["success"] = true;
    resp["message"] = "Protocol configuration saved";
    resp["data"]["mqttReconnected"] = mqttReconnected;
    resp["data"]["mqttDeferred"] = mqttReconnected;  // 标识为延迟连接模式
    resp["data"]["mqttDisconnected"] = mqttDisconnected;
    resp["data"]["modbusRestarted"] = modbusRestarted;
    if (!mqttReconnected && doc["mqtt"]["enabled"].as<bool>()) {
        resp["data"]["mqttError"] = mqttError;
    }
    String out;
    serializeJson(resp, out);
    request->send(200, "application/json", out);
}

void ProtocolRouteHandler::handleGetModbusStatus(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return;
    }

    ModbusHandler* modbus = pm->getModbusHandler();

    JsonDocument doc;
    doc["success"] = true;

    JsonObject data = doc["data"].to<JsonObject>();

    if (!modbus) {
        // Modbus 未初始化时返回空状态而非 404
        data["mode"] = "master";
        data["workMode"] = 1;
        data["status"] = "stopped";
        data["totalPolls"] = 0;
        data["successPolls"] = 0;
        data["failedPolls"] = 0;
        data["timeoutPolls"] = 0;
        data["taskCount"] = 0;
        data["tasks"].to<JsonArray>();
    } else {
        data["mode"] = (modbus->getMode() == MODBUS_MASTER) ? "master" : "slave";
        data["workMode"] = modbus->getWorkMode();
        data["status"] = modbus->getStatus();

        if (modbus->getMode() == MODBUS_MASTER) {
            MasterStats stats = modbus->getMasterStats();
            data["totalPolls"] = stats.totalPolls;
            data["successPolls"] = stats.successPolls;
            data["failedPolls"] = stats.failedPolls;
            data["timeoutPolls"] = stats.timeoutPolls;
            data["taskCount"] = modbus->getPollTaskCount();

            JsonArray tasks = data["tasks"].to<JsonArray>();
            for (uint8_t i = 0; i < modbus->getPollTaskCount(); i++) {
                PollTask task = modbus->getPollTask(i);
                JsonObject t = tasks.add<JsonObject>();
                t["slaveAddress"] = task.slaveAddress;
                t["functionCode"] = task.functionCode;
                t["startAddress"] = task.startAddress;
                t["quantity"] = task.quantity;
                t["pollInterval"] = task.pollInterval;
                t["enabled"] = task.enabled;
                t["label"] = task.label;
            }
        }
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void ProtocolRouteHandler::handleModbusWrite(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return;
    }

    ModbusHandler* modbus = pm->getModbusHandler();
    if (!modbus) {
        ctx->sendError(request, 503, "Modbus not started");
        return;
    }

    if (modbus->getMode() != MODBUS_MASTER) {
        ctx->sendError(request, 400, "Modbus is not in Master mode");
        return;
    }

    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t regAddr = (uint16_t)ctx->getParamInt(request, "regAddress", 0);
    uint16_t value = (uint16_t)ctx->getParamInt(request, "value", 0);

    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }

    if (modbus->masterWriteSingleRegister(slaveAddr, regAddr, value)) {
        ctx->sendSuccess(request, "Write request queued");
    } else {
        ctx->sendError(request, 503, "Write queue full");
    }
}

void ProtocolRouteHandler::handleTestMqttConnection(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String server = ctx->getParamValue(request, "server", "");
    int port = ctx->getParamInt(request, "port", 1883);
    String clientId = ctx->getParamValue(request, "clientId", "");
    String username = ctx->getParamValue(request, "username", "");
    String password = ctx->getParamValue(request, "password", "");
    String authCode = ctx->getParamValue(request, "authCode", "");
    String mqttSecret = ctx->getParamValue(request, "mqttSecret", "");

    if (server.isEmpty()) {
        ctx->sendBadRequest(request, "Server address is required");
        return;
    }

    // 从 device.json 读取设备基础信息
    String deviceNum, productId, userId, ntpServer;
    // 优先从请求参数读取 authType（反映前端当前选择），
    // 支持数值(0/1)与字符串(SIMPLE/ENCRYPTED)两种格式；
    // 如果请求未携带，再从 protocol.json 读取已保存的值
    int authType = -1;
    String authTypeStr = ctx->getParamValue(request, "authType", "");
    if (!authTypeStr.isEmpty()) {
        if (authTypeStr.equalsIgnoreCase("ENCRYPTED")) {
            authType = 1;
        } else if (authTypeStr.equalsIgnoreCase("SIMPLE")) {
            authType = 0;
        } else {
            authType = authTypeStr.toInt();
        }
    }
    if (LittleFS.exists("/config/device.json")) {
        File f = LittleFS.open("/config/device.json", "r");
        if (f) {
            JsonDocument devDoc;
            if (!deserializeJson(devDoc, f)) {
                deviceNum = jsonVariantToString(devDoc["deviceId"]);
                productId = jsonVariantToString(devDoc["productNumber"]);
                userId    = jsonVariantToString(devDoc["userId"]);
                ntpServer = jsonVariantToString(devDoc["ntpServer1"]);
            }
            f.close();
        }
    }
    // 也尝试从 protocol.json 读取（protocol.json 中的值优先级更高）
    if (LittleFS.exists("/config/protocol.json")) {
        File f = LittleFS.open("/config/protocol.json", "r");
        if (f) {
            JsonDocument protoDoc;
            if (!deserializeJson(protoDoc, f)) {
                JsonVariant mqtt = protoDoc["mqtt"];
                if (mqtt.is<JsonObject>()) {
                    String dn = jsonVariantToString(mqtt["deviceNum"]);
                    String pi = jsonVariantToString(mqtt["productId"]);
                    String ui = jsonVariantToString(mqtt["userId"]);
                    if (!dn.isEmpty()) deviceNum = dn;
                    if (!pi.isEmpty()) productId = pi;
                    if (!ui.isEmpty()) userId    = ui;
                    String ns = jsonVariantToString(mqtt["ntpServer"]);
                    String ms = jsonVariantToString(mqtt["mqttSecret"]);
                    if (!ns.isEmpty()) ntpServer = ns;
                    if (mqttSecret.isEmpty() && !ms.isEmpty()) mqttSecret = ms;
                    // 仅当请求参数未携带 authType 时使用配置文件的值
                    if (authType < 0) {
                        authType = mqtt["authType"] | 0;
                    }
                }
            }
            f.close();
        }
    }
    // 如果仍未确定 authType，默认为简单认证
    if (authType < 0) authType = 0;

    // AES 加密认证：使用当前表单参数直接构建加密密码并进行一次真实连接测试
    // 避免依赖"已保存配置"，防止出现"点击测试无响应/结果与当前输入不一致"
    if (authType == 1) {
        JsonDocument doc;
            
        // 优先使用前端传递的clientId（如果已经是E开头格式）
        // 否则根据deviceNum/productId/userId构建
        if (!clientId.isEmpty() && clientId.startsWith("E&")) {
            // 使用前端传递的E认证clientId
        } else if (!deviceNum.isEmpty() && !productId.isEmpty() && !userId.isEmpty()) {
            clientId = "E&" + deviceNum + "&" + productId + "&" + userId;
        }
            
        if (clientId.isEmpty() || !clientId.startsWith("E&")) {
            doc["success"] = true;
            doc["data"]["connected"] = false;
            doc["data"]["error"] = -8;
            doc["data"]["errorMessage"] = "AES clientId requires E&deviceNum&productId&userId format";
            String out;
            serializeJson(doc, out);
            request->send(200, "application/json", out);
            return;
        }

        String aesErr;
        String encryptedPassword = buildEncryptedPasswordForMqttTest(password, authCode, mqttSecret, ntpServer, &aesErr);
        doc["success"] = true;
        doc["data"]["clientId"] = clientId;  // 返回实际使用的clientId
        doc["data"]["authType"] = "AES";     // 标识认证类型
        if (encryptedPassword.isEmpty()) {
            doc["data"]["connected"] = false;
            doc["data"]["error"] = -7;
            doc["data"]["errorMessage"] = aesErr.isEmpty() ? "AES password generation failed" : aesErr;
        } else {
            WiFiClient testWifi;
            PubSubClient testClient(testWifi);
            testClient.setServer(server.c_str(), port);
            testClient.setBufferSize(512);

            bool connected = testClient.connect(
                clientId.c_str(),
                username.isEmpty() ? nullptr : username.c_str(),
                encryptedPassword.c_str());
            doc["data"]["connected"] = connected;
            if (!connected) {
                doc["data"]["error"] = testClient.state();
            }
            if (testClient.connected()) {
                testClient.disconnect();
            }
            
            // 测试成功后，触发实际MQTT客户端重新连接（使用已保存的配置）
            if (connected) {
                ProtocolManager* pm = ctx->protocolManager;
                if (pm) {
                    bool realConnected = pm->restartMQTT();
                    doc["data"]["realConnected"] = realConnected;
                    if (!realConnected) {
                        MQTTClient* mqtt = pm->getMQTTClient();
                        doc["data"]["realError"] = mqtt ? mqtt->getLastErrorCode() : -99;
                    }
                }
            }
        }
        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
        return;
    }

    // 简单认证模式：构建 FastBee 认证格式的 clientId: S&deviceNum&productId&userId
    if (!deviceNum.isEmpty() && !productId.isEmpty()) {
        clientId = "S&" + deviceNum + "&" + productId + "&" + userId;
    } else if (clientId.isEmpty()) {
        char id[20];
        snprintf(id, sizeof(id), "TEST-%04X", (unsigned)esp_random() & 0xFFFF);
        clientId = id;
    }

    // 构建简单认证密码: password 或 password&authCode
    String connPassword = password;
    if (!authCode.isEmpty()) {
        connPassword = password + "&" + authCode;
    }

    WiFiClient testWifi;
    PubSubClient testClient(testWifi);
    testClient.setServer(server.c_str(), port);
    testClient.setBufferSize(512);

    bool connected = testClient.connect(
        clientId.c_str(),
        username.isEmpty() ? nullptr : username.c_str(),
        connPassword.isEmpty() ? nullptr : connPassword.c_str());

    JsonDocument doc;
    doc["success"] = true;
    doc["data"]["clientId"] = clientId;  // 返回实际使用的clientId
    doc["data"]["authType"] = "Simple";  // 标识认证类型
    doc["data"]["connected"] = connected;
    if (!connected) {
        doc["data"]["error"] = testClient.state();
    }

    if (testClient.connected()) {
        testClient.disconnect();
    }

    // 测试成功后，触发实际MQTT客户端重新连接（使用已保存的配置）
    if (connected) {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            bool realConnected = pm->restartMQTT();
            doc["data"]["realConnected"] = realConnected;
            if (!realConnected) {
                MQTTClient* mqtt = pm->getMQTTClient();
                doc["data"]["realError"] = mqtt ? mqtt->getLastErrorCode() : -99;
            }
        }
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void ProtocolRouteHandler::handleGetMqttStatus(AsyncWebServerRequest* request) {
    // 兼容“可编辑但不可查看”的自定义角色：允许 config.view 或 config.edit 访问状态接口
    if (!ctx->checkPermission(request, "config.view") &&
        !ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return;
    }

    // 检查网络状态
    bool internetAvailable = false;
    if (ctx->networkManager) {
        NetworkManager* netMgr = static_cast<NetworkManager*>(ctx->networkManager);
        NetworkStatusInfo netInfo = netMgr->getStatusInfo();
        internetAvailable = netInfo.internetAvailable;
    }

    MQTTClient* mqtt = pm->getMQTTClient();
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();

    if (mqtt) {
        const MQTTConfig& cfg = mqtt->getConfig();
        data["initialized"] = true;
        
        // 如果MQTT被显式停止，返回断开状态
        // 如果网络不可用，MQTT 实际上无法通信，返回断开状态
        bool mqttConnected = mqtt->getIsConnected() && !mqtt->isStopped();
        if (!internetAvailable) {
            mqttConnected = false;
        }
        data["connected"] = mqttConnected;
        data["internetAvailable"] = internetAvailable;
        data["server"] = cfg.server;
        data["port"] = cfg.port;
        data["clientId"] = cfg.clientId;
        data["lastError"] = mqtt->getLastErrorCode();
        data["reconnectCount"] = mqtt->getReconnectCount();
        data["autoReconnect"] = cfg.autoReconnect;
        unsigned long connTime = mqtt->getLastConnectedTime();
        data["lastConnectedMs"] = connTime > 0 ? (millis() - connTime) / 1000 : 0;
    } else {
        data["initialized"] = false;
        data["connected"] = false;
        data["internetAvailable"] = internetAvailable;
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void ProtocolRouteHandler::handleMqttReconnect(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return;
    }

    bool ok = pm->restartMQTT();

    JsonDocument doc;
    doc["success"] = true;
    doc["data"]["connected"] = ok;
    if (!ok) {
        MQTTClient* mqtt = pm->getMQTTClient();
        doc["data"]["error"] = mqtt ? mqtt->getLastErrorCode() : -99;
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void ProtocolRouteHandler::handleMqttDisconnect(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return;
    }

    MQTTClient* mqtt = pm->getMQTTClient();
    bool wasConnected = mqtt && mqtt->getIsConnected();
    pm->stopMQTT();

    JsonDocument resp;
    resp["success"] = true;
    resp["data"]["disconnected"] = true;
    resp["data"]["wasConnected"] = wasConnected;

    String respOut;
    serializeJson(resp, respOut);
    request->send(200, "application/json", respOut);
}

void ProtocolRouteHandler::handleMqttNtpSync(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return;
    }

    MQTTClient* mqtt = pm->getMQTTClient();
    if (!mqtt || !mqtt->getIsConnected()) {
        JsonDocument resp;
        resp["success"] = false;
        resp["error"] = "MQTT not connected";
        String out;
        serializeJson(resp, out);
        request->send(200, "application/json", out);
        return;
    }

    bool ok = mqtt->publishNtpSync();

    JsonDocument resp;
    resp["success"] = ok;
    if (!ok) {
        resp["error"] = "No enabled NTP_SYNC publish topic found";
    }

    String out;
    serializeJson(resp, out);
    request->send(200, "application/json", out);
}
