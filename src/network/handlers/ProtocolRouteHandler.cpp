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

    // Modbus 通用控制 API（线圈、寄存器读写，设备参数）
    server->on("/api/modbus/coil/control", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusCoilControl(request);
    });

    server->on("/api/modbus/coil/batch", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusCoilBatch(request);
    });

    server->on("/api/modbus/coil/delay", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusCoilDelay(request);
    });

    server->on("/api/modbus/coil/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleModbusCoilStatus(request);
    });

    server->on("/api/modbus/device/address", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusDeviceAddress(request);
    });

    server->on("/api/modbus/device/baudrate", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusDeviceBaudrate(request);
    });

    server->on("/api/modbus/device/inputs", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleModbusDiscreteInputs(request);
    });

    // Modbus 通用寄存器读写 API
    server->on("/api/modbus/register/read", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleModbusRegisterRead(request);
    });

    server->on("/api/modbus/register/write", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusRegisterWrite(request);
    });

    server->on("/api/modbus/register/batch-write", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusRegisterBatchWrite(request);
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
    doc["modbusRtu"]["peripheralId"] = GP("modbusRtu_peripheralId", "");
    doc["modbusRtu"]["timeout"] = GPI("modbusRtu_timeout", "1000");
    doc["modbusRtu"]["mode"] = GP("modbusRtu_mode", "master");
    doc["modbusRtu"]["dePin"] = GPI("modbusRtu_dePin", "-1");
    doc["modbusRtu"]["transferType"] = GPI("modbusRtu_transferType", "0");
    doc["modbusRtu"]["workMode"] = GPI("modbusRtu_workMode", "1");

    // Modbus RTU Master 配置
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

                // 寄存器映射配置
                if (task.mappingCount > 0) {
                    JsonArray mappings = t["mappings"].to<JsonArray>();
                    for (uint8_t j = 0; j < task.mappingCount; j++) {
                        const RegisterMapping& m = task.mappings[j];
                        if (m.sensorId[0] == '\0') continue;
                        JsonObject mo = mappings.add<JsonObject>();
                        mo["regOffset"] = m.regOffset;
                        mo["dataType"] = m.dataType;
                        mo["scaleFactor"] = m.scaleFactor;
                        mo["decimalPlaces"] = m.decimalPlaces;
                        mo["sensorId"] = m.sensorId;
                    }
                }

                // 最新采集数据缓存
                TaskDataCache cache = modbus->getTaskDataCache(i);
                if (cache.valid) {
                    JsonObject cd = t["cachedData"].to<JsonObject>();
                    cd["timestamp"] = cache.timestamp;
                    JsonArray vals = cd["values"].to<JsonArray>();
                    for (uint16_t v = 0; v < cache.count; v++) {
                        vals.add(cache.values[v]);
                    }
                }
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

// ============================================================================
// Modbus 通用控制 API 辅助函数
// ============================================================================

// 将 OneShotError 转换为 HTTP 错误响应
static void sendOneShotError(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                              const OneShotResult& result) {
    JsonDocument doc;
    doc["success"] = false;
    
    const char* errorCode = "UNKNOWN";
    const char* errorMsg = "Unknown error";
    int httpCode = 500;
    
    switch (result.error) {
        case ONESHOT_TIMEOUT:
            errorCode = "TIMEOUT";
            errorMsg = "Slave not responding (timeout)";
            httpCode = 504;
            break;
        case ONESHOT_CRC_ERROR:
            errorCode = "CRC_ERROR";
            errorMsg = "CRC validation failed";
            httpCode = 502;
            break;
        case ONESHOT_EXCEPTION: {
            errorCode = "MODBUS_EXCEPTION";
            httpCode = 422;
            char msgBuf[64];
            switch (result.exceptionCode) {
                case 0x01: snprintf(msgBuf, sizeof(msgBuf), "Illegal function (0x01)"); break;
                case 0x02: snprintf(msgBuf, sizeof(msgBuf), "Illegal data address (0x02)"); break;
                case 0x03: snprintf(msgBuf, sizeof(msgBuf), "Illegal data value (0x03)"); break;
                case 0x04: snprintf(msgBuf, sizeof(msgBuf), "Slave device failure (0x04)"); break;
                default:   snprintf(msgBuf, sizeof(msgBuf), "Exception code 0x%02X", result.exceptionCode); break;
            }
            doc["error"] = msgBuf;
            doc["errorCode"] = errorCode;
            doc["exceptionCode"] = result.exceptionCode;
            String out;
            serializeJson(doc, out);
            request->send(httpCode, "application/json", out);
            return;
        }
        case ONESHOT_NOT_INITIALIZED:
            errorCode = "NOT_INITIALIZED";
            errorMsg = "Modbus not initialized";
            httpCode = 503;
            break;
        case ONESHOT_BUSY:
            errorCode = "BUSY";
            errorMsg = "Modbus busy, try again";
            httpCode = 503;
            break;
        default:
            break;
    }
    
    doc["error"] = errorMsg;
    doc["errorCode"] = errorCode;
    String out;
    serializeJson(doc, out);
    request->send(httpCode, "application/json", out);
}

// 获取 ModbusHandler 指针并校验 Master 模式，失败时发送错误响应
static ModbusHandler* getModbusMaster(WebHandlerContext* ctx, AsyncWebServerRequest* request) {
    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return nullptr;
    }
    ModbusHandler* modbus = pm->getModbusHandler();
    if (!modbus) {
        ctx->sendError(request, 503, "Modbus not started");
        return nullptr;
    }
    if (modbus->getMode() != MODBUS_MASTER) {
        ctx->sendError(request, 400, "Modbus is not in Master mode");
        return nullptr;
    }
    return modbus;
}

// 辅助：将 Modbus TX/RX 调试帧添加到 JSON 响应
static void addModbusDebug(JsonDocument& doc, ModbusHandler* modbus) {
    JsonObject debug = doc["debug"].to<JsonObject>();
    debug["tx"] = modbus->getLastTxHex();
    debug["rx"] = modbus->getLastRxHex();
}

// ============================================================================
// POST /api/modbus/coil/control — 单个线圈控制（on/off/toggle）
// mode=coil: FC05 写线圈  mode=register: FC06 写保持寄存器（0x0001/0x0000）
// ============================================================================
void ProtocolRouteHandler::handleModbusCoilControl(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t channel = (uint16_t)ctx->getParamInt(request, "channel", 0);
    uint16_t coilBase = (uint16_t)ctx->getParamInt(request, "coilBase", 0);
    String action = ctx->getParamValue(request, "action", "toggle");
    String mode = ctx->getParamValue(request, "mode", "coil");
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    
    uint16_t coilAddr = coilBase + channel;
    OneShotResult result;
    
    if (mode == "register") {
        // 寄存器模式：FC06 写保持寄存器
        if (action == "on") {
            result = modbus->writeRegisterOnce(slaveAddr, coilAddr, 1);
        } else if (action == "off") {
            result = modbus->writeRegisterOnce(slaveAddr, coilAddr, 0);
        } else if (action == "toggle") {
            // 先读当前值再反转
            OneShotResult readR = modbus->readRegistersOnce(slaveAddr, 0x03, coilAddr, 1);
            if (readR.error != ONESHOT_SUCCESS) {
                sendOneShotError(ctx, request, readR);
                return;
            }
            uint16_t newVal = (readR.data[0] != 0) ? 0 : 1;
            result = modbus->writeRegisterOnce(slaveAddr, coilAddr, newVal);
        } else {
            ctx->sendBadRequest(request, "Invalid action (on/off/toggle)");
            return;
        }
    } else {
        // 线圈模式：FC05 写线圈
        if (action == "on") {
            result = modbus->writeCoilOnce(slaveAddr, coilAddr, true);
        } else if (action == "off") {
            result = modbus->writeCoilOnce(slaveAddr, coilAddr, false);
        } else if (action == "toggle") {
            result = modbus->writeCoilOnce(slaveAddr, coilAddr, (uint16_t)0x5500);
        } else {
            ctx->sendBadRequest(request, "Invalid action (on/off/toggle)");
            return;
        }
    }
    
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
        return;
    }
    
    // 操作后读取实际状态
    bool newState = (action == "on");
    if (action == "toggle" || action == "off") {
        if (mode == "register") {
            OneShotResult readResult = modbus->readRegistersOnce(slaveAddr, 0x03, coilAddr, 1);
            if (readResult.error == ONESHOT_SUCCESS) {
                newState = (readResult.data[0] != 0);
            }
        } else {
            OneShotResult readResult = modbus->readRegistersOnce(slaveAddr, 0x01, coilBase, 8);
            if (readResult.error == ONESHOT_SUCCESS) {
                newState = (readResult.data[channel] != 0);
            }
        }
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["channel"] = channel;
    data["coilAddress"] = coilAddr;
    data["state"] = newState;
    data["action"] = action;
    data["mode"] = mode;
    addModbusDebug(doc, modbus);
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// POST /api/modbus/coil/batch — 批量线圈控制（allOn/allOff/allToggle）
// mode=coil: FC05 写线圈  mode=register: FC06 写保持寄存器
// ============================================================================
void ProtocolRouteHandler::handleModbusCoilBatch(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t channelCount = (uint16_t)ctx->getParamInt(request, "channelCount", 8);
    uint16_t coilBase = (uint16_t)ctx->getParamInt(request, "coilBase", 0);
    String action = ctx->getParamValue(request, "action", "allOff");
    String mode = ctx->getParamValue(request, "mode", "coil");
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    if (channelCount == 0 || channelCount > Protocols::MODBUS_MAX_WRITE_COILS) {
        ctx->sendBadRequest(request, "Invalid channel count (1-32)");
        return;
    }
    
    OneShotResult result;
    
    if (mode == "register") {
        // 寄存器模式：FC06 逐通道写
        if (action == "allOn" || action == "allOff") {
            uint16_t targetVal = (action == "allOn") ? 1 : 0;
            for (uint16_t i = 0; i < channelCount; i++) {
                OneShotResult wr = modbus->writeRegisterOnce(slaveAddr, coilBase + i, targetVal);
                if (wr.error != ONESHOT_SUCCESS) {
                    sendOneShotError(ctx, request, wr);
                    return;
                }
            }
            result.error = ONESHOT_SUCCESS;
        } else if (action == "allToggle") {
            // 先读取所有状态再逐个反转
            OneShotResult readR = modbus->readRegistersOnce(slaveAddr, 0x03, coilBase, channelCount);
            if (readR.error != ONESHOT_SUCCESS) {
                sendOneShotError(ctx, request, readR);
                return;
            }
            for (uint16_t i = 0; i < channelCount; i++) {
                uint16_t newVal = (readR.data[i] != 0) ? 0 : 1;
                OneShotResult wr = modbus->writeRegisterOnce(slaveAddr, coilBase + i, newVal);
                if (wr.error != ONESHOT_SUCCESS) {
                    sendOneShotError(ctx, request, wr);
                    return;
                }
            }
            result.error = ONESHOT_SUCCESS;
        } else {
            ctx->sendBadRequest(request, "Invalid action (allOn/allOff/allToggle)");
            return;
        }
    } else {
        // 线圈模式：FC05
        if (action == "allOn" || action == "allOff") {
            bool targetOn = (action == "allOn");
            for (uint16_t i = 0; i < channelCount; i++) {
                OneShotResult writeResult = modbus->writeCoilOnce(slaveAddr, coilBase + i, targetOn);
                if (writeResult.error != ONESHOT_SUCCESS) {
                    sendOneShotError(ctx, request, writeResult);
                    return;
                }
            }
            result.error = ONESHOT_SUCCESS;
        } else if (action == "allToggle") {
            result = modbus->writeCoilOnce(slaveAddr, coilBase, (uint16_t)0x5A00);
        } else {
            ctx->sendBadRequest(request, "Invalid action (allOn/allOff/allToggle)");
            return;
        }
    }
    
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
        return;
    }
    
    // 操作后读取实际状态
    OneShotResult readResult;
    if (mode == "register") {
        readResult = modbus->readRegistersOnce(slaveAddr, 0x03, coilBase, channelCount);
    } else {
        uint16_t readQty = ((channelCount + 7) / 8) * 8;
        if (readQty < 8) readQty = 8;
        readResult = modbus->readRegistersOnce(slaveAddr, 0x01, coilBase, readQty);
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["channelCount"] = channelCount;
    data["action"] = action;
    data["mode"] = mode;
    JsonArray states = data["states"].to<JsonArray>();
    if (readResult.error == ONESHOT_SUCCESS) {
        for (uint16_t i = 0; i < channelCount; i++) {
            states.add(readResult.data[i] != 0);
        }
    } else {
        for (uint16_t i = 0; i < channelCount; i++) {
            states.add(action == "allOn");
        }
    }
    addModbusDebug(doc, modbus);
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// POST /api/modbus/coil/delay — 线圈延时控制（闪开：开启后设备硬件自动关闭）
// ============================================================================
void ProtocolRouteHandler::handleModbusCoilDelay(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t channel = (uint16_t)ctx->getParamInt(request, "channel", 0);
    uint16_t delayBase = (uint16_t)ctx->getParamInt(request, "delayBase", 0x0200);
    uint16_t delayUnits = (uint16_t)ctx->getParamInt(request, "delayUnits", 50);
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    if (delayUnits == 0 || delayUnits > 255) {
        ctx->sendBadRequest(request, "Invalid delay (1-255 x100ms, max 25.5s)");
        return;
    }
    
    // 闪开指令：FC 0x05 地址=(delayBase + channel) 值=延时单位在高字节
    // 延时地址 = delayBase(0x0200) + 通道号(0-based)，与线圈地址映射一致
    // curl 实测: addr 0x0200→CH0, addr 0x0201→CH1
    uint16_t delayAddr = delayBase + channel;
    uint16_t rawValue = ((uint16_t)delayUnits) << 8;
    
    OneShotResult result = modbus->writeCoilOnce(slaveAddr, delayAddr, rawValue);
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
        return;
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["channel"] = channel;
    data["delayAddress"] = delayAddr;
    data["delayUnits"] = delayUnits;
    data["delayMs"] = (uint32_t)delayUnits * 100;
    addModbusDebug(doc, modbus);
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// GET /api/modbus/coil/status — 读取线圈状态
// mode=coil: FC01 读线圈  mode=register: FC03 读保持寄存器（每通道一个寄存器）
// ============================================================================
void ProtocolRouteHandler::handleModbusCoilStatus(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.view") &&
        !ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t channelCount = (uint16_t)ctx->getParamInt(request, "channelCount", 8);
    uint16_t coilBase = (uint16_t)ctx->getParamInt(request, "coilBase", 0);
    String mode = ctx->getParamValue(request, "mode", "coil");
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    if (channelCount == 0 || channelCount > Protocols::MODBUS_MAX_WRITE_COILS) {
        ctx->sendBadRequest(request, "Invalid channel count (1-32)");
        return;
    }
    
    OneShotResult result;
    if (mode == "register") {
        // 寄存器模式：FC03 读保持寄存器，每通道一个寄存器，值 !=0 为 ON
        result = modbus->readRegistersOnce(slaveAddr, 0x03, coilBase, channelCount);
    } else {
        // 线圈模式：FC01 读线圈，向上取整到 8 的倍数
        uint16_t readQty = ((channelCount + 7) / 8) * 8;
        if (readQty < 8) readQty = 8;
        result = modbus->readRegistersOnce(slaveAddr, 0x01, coilBase, readQty);
    }
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
        return;
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["channelCount"] = channelCount;
    data["coilBase"] = coilBase;
    data["mode"] = mode;
    JsonArray states = data["states"].to<JsonArray>();
    for (uint16_t i = 0; i < channelCount; i++) {
        states.add(result.data[i] != 0);
    }
    addModbusDebug(doc, modbus);
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// POST /api/modbus/device/address — 读取/设置从站地址（FC 0x03 / FC 0x06）
// ============================================================================
void ProtocolRouteHandler::handleModbusDeviceAddress(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t addrRegister = (uint16_t)ctx->getParamInt(request, "addressRegister", 0x0000);
    String newAddrStr = ctx->getParamValue(request, "newAddress", "");
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    
    if (newAddrStr.isEmpty()) {
        // 读取当前地址：使用广播地址 0x00（文档确认：00 03 00 00 00 01 85 DB）
        // 设备会以实际地址响应（如 03 03 02 00 03 ...）
        // sendOneShotRequest 中 expectedSlaveAddr==0 时已跳过地址匹配
        OneShotResult result = modbus->readRegistersOnce(0x00, 0x03, addrRegister, 1);
        if (result.error != ONESHOT_SUCCESS) {
            sendOneShotError(ctx, request, result);
            return;
        }
        data["currentAddress"] = result.data[0];
        data["register"] = addrRegister;
    } else {
        // 设置新地址：使用广播地址 0x00，FC 0x10 写多个寄存器
        uint16_t newAddr = (uint16_t)newAddrStr.toInt();
        if (newAddr < 1 || newAddr > 255) {
            ctx->sendBadRequest(request, "Invalid new address (1-255)");
            return;
        }
        uint16_t regValues[1] = { newAddr };
        OneShotResult result = modbus->writeMultipleRegistersOnce(0x00, addrRegister, 1, regValues);
        if (result.error != ONESHOT_SUCCESS) {
            sendOneShotError(ctx, request, result);
            return;
        }
        data["previousAddress"] = slaveAddr;
        data["newAddress"] = newAddr;
        data["register"] = addrRegister;
    }
    addModbusDebug(doc, modbus);
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// POST /api/modbus/device/baudrate — 设置波特率（FC 0x06）
// ============================================================================
void ProtocolRouteHandler::handleModbusDeviceBaudrate(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint32_t baudRate = (uint32_t)ctx->getParamInt(request, "baudRate", 9600);
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    
    // 波特率映射（继电器板 FC 0xB0 标准编码）
    uint8_t baudCode;
    switch (baudRate) {
        case 1200:   baudCode = 0; break;
        case 2400:   baudCode = 1; break;
        case 4800:   baudCode = 2; break;
        case 9600:   baudCode = 3; break;
        case 19200:  baudCode = 4; break;
        case 115200: baudCode = 5; break;
        default:     baudCode = 3; break; // 默认 9600
    }
    
    // 允许用户通过 baudCode 参数直接指定编码值
    String baudCodeStr = ctx->getParamValue(request, "baudCode", "");
    if (!baudCodeStr.isEmpty()) {
        baudCode = (uint8_t)baudCodeStr.toInt();
    }
    
    // 发送专有 FC 0xB0 波特率设置帧：[slaveAddr, 0xB0, 0x00, 0x00, baudCode, 0x00]
    uint8_t frame[6];
    frame[0] = slaveAddr;
    frame[1] = 0xB0;
    frame[2] = 0x00;
    frame[3] = 0x00;
    frame[4] = baudCode;
    frame[5] = 0x00;
    
    OneShotResult result = modbus->sendRawFrameOnce(slaveAddr, frame, 6);
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
        return;
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["baudRate"] = baudRate;
    data["baudCode"] = baudCode;
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// GET /api/modbus/device/inputs — 读取离散输入（FC 0x02）
// ============================================================================
void ProtocolRouteHandler::handleModbusDiscreteInputs(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.view") &&
        !ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t inputCount = (uint16_t)ctx->getParamInt(request, "inputCount", 4);
    uint16_t inputBase = (uint16_t)ctx->getParamInt(request, "inputBase", 0);
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    if (inputCount == 0 || inputCount > Protocols::MODBUS_MAX_WRITE_COILS) {
        ctx->sendBadRequest(request, "Invalid input count (1-32)");
        return;
    }
    
    // 同 FC 0x01，使用 qty 向上取整到 8 的倍数确保标准格式
    uint16_t readQty = ((inputCount + 7) / 8) * 8;
    if (readQty < 8) readQty = 8;
    OneShotResult result = modbus->readRegistersOnce(slaveAddr, 0x02, inputBase, readQty);
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
        return;
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["inputCount"] = inputCount;
    data["inputBase"] = inputBase;
    JsonArray states = data["states"].to<JsonArray>();
    for (uint16_t i = 0; i < inputCount; i++) {
        states.add(result.data[i] != 0);
    }
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// GET /api/modbus/register/read — 读取保持/输入寄存器（FC 0x03 / FC 0x04）
// ============================================================================
void ProtocolRouteHandler::handleModbusRegisterRead(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.view") &&
        !ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t startAddr = (uint16_t)ctx->getParamInt(request, "startAddress", 0);
    uint16_t quantity = (uint16_t)ctx->getParamInt(request, "quantity", 1);
    uint8_t fc = (uint8_t)ctx->getParamInt(request, "functionCode", 3);
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    if (quantity == 0 || quantity > Protocols::MODBUS_MAX_REGISTERS_PER_READ) {
        ctx->sendBadRequest(request, "Invalid quantity (1-125)");
        return;
    }
    if (fc != 0x03 && fc != 0x04) {
        ctx->sendBadRequest(request, "Invalid function code (3 or 4)");
        return;
    }
    
    OneShotResult result = modbus->readRegistersOnce(slaveAddr, fc, startAddr, quantity);
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
        return;
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["count"] = quantity;
    data["startAddress"] = startAddr;
    JsonArray values = data["values"].to<JsonArray>();
    for (uint16_t i = 0; i < quantity; i++) {
        values.add(result.data[i]);
    }
    addModbusDebug(doc, modbus);
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// POST /api/modbus/register/write — 写单个保持寄存器（FC 0x06）
// ============================================================================
void ProtocolRouteHandler::handleModbusRegisterWrite(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t regAddr = (uint16_t)ctx->getParamInt(request, "registerAddress", 0);
    uint16_t value = (uint16_t)ctx->getParamInt(request, "value", 0);
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    
    OneShotResult result = modbus->writeRegisterOnce(slaveAddr, regAddr, value);
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
        return;
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["register"] = regAddr;
    data["value"] = value;
    addModbusDebug(doc, modbus);
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// POST /api/modbus/register/batch-write — 批量写保持寄存器（FC 0x10）
// ============================================================================
void ProtocolRouteHandler::handleModbusRegisterBatchWrite(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t startAddr = (uint16_t)ctx->getParamInt(request, "startAddress", 0);
    String valuesStr = ctx->getParamValue(request, "values", "[]");
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    
    // 解析 JSON 数组
    JsonDocument valDoc;
    DeserializationError err = deserializeJson(valDoc, valuesStr);
    if (err || !valDoc.is<JsonArray>()) {
        ctx->sendBadRequest(request, "Invalid values (JSON array expected)");
        return;
    }
    
    JsonArray arr = valDoc.as<JsonArray>();
    uint16_t quantity = arr.size();
    if (quantity == 0 || quantity > Protocols::MODBUS_MAX_REGISTERS_PER_READ) {
        ctx->sendBadRequest(request, "Invalid quantity (1-125)");
        return;
    }
    
    uint16_t regValues[125];
    for (uint16_t i = 0; i < quantity; i++) {
        regValues[i] = (uint16_t)arr[i].as<int>();
    }
    
    OneShotResult result = modbus->writeMultipleRegistersOnce(slaveAddr, startAddr, quantity, regValues);
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
        return;
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["startAddress"] = startAddr;
    data["quantity"] = quantity;
    JsonArray respValues = data["values"].to<JsonArray>();
    for (uint16_t i = 0; i < quantity; i++) {
        respValues.add(regValues[i]);
    }
    addModbusDebug(doc, modbus);
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
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
