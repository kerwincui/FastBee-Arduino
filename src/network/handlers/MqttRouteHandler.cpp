#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_MQTT

#include "./network/handlers/MqttRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "./protocols/ProtocolManager.h"
#include "./protocols/MQTTClient.h"
#include "./systems/ConfigStorage.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>

static const char* MQTT_AES_IV = "wumei-smart-open";

// ============================================================================
// MQTT 测试辅助函数
// ============================================================================

static Client* selectMqttTestClient(WebHandlerContext* ctx, WiFiClient& fallbackClient, String& errorMessage) {
    errorMessage = "";
    FBNetworkManager* netMgr = (ctx && ctx->networkManager)
                                 ? static_cast<FBNetworkManager*>(ctx->networkManager)
                                 : nullptr;
    if (!netMgr || netMgr->getNetworkType() == NetworkType::NET_WIFI) {
        return &fallbackClient;
    }

    if (!netMgr->isNetworkConnected()) {
        errorMessage = "Active network is not connected";
        return nullptr;
    }

    Client* activeClient = netMgr->getActiveClient();
    if (!activeClient) {
        errorMessage = "Active network client is unavailable";
        return nullptr;
    }
    return activeClient;
}

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

static bool loadMqttStatusConfig(bool& enabled, String& server, int& port, String& clientId) {
    enabled = false;
    server = "";
    port = 0;
    clientId = "";

    JsonDocument doc;
    if (!ConfigStorage::getInstance().loadProtocolSection("mqtt", doc)) {
        return false;
    }

    JsonVariant mqtt = doc["mqtt"];
    if (!mqtt.is<JsonObject>()) {
        return false;
    }

    enabled = mqtt["enabled"] | true;
    server = jsonVariantToString(mqtt["server"]);
    port = mqtt["port"] | 1883;
    clientId = jsonVariantToString(mqtt["clientId"]);
    return true;
}

static bool mqttHasValidRuntimeConfig(MQTTClient* mqtt) {
    if (!mqtt) return false;
    MQTTConfig cfg = mqtt->getConfig();
    return (cfg.server != nullptr && cfg.server[0] != '\0' && cfg.port > 0);
}

static bool tryAutoStartMqttForStatus(ProtocolManager* pm,
                                      MQTTClient* mqtt,
                                      bool configLoaded,
                                      bool configEnabled,
                                      bool& attempted,
                                      bool& started) {
    attempted = false;
    started = false;
    if (!pm || !configLoaded || !configEnabled) return false;
    if (mqttHasValidRuntimeConfig(mqtt)) return false;

    static unsigned long lastAttemptMs = 0;
    unsigned long now = millis();
    if (lastAttemptMs != 0 && (now - lastAttemptMs) < 10000UL) {
        return false;
    }

    lastAttemptMs = now;
    attempted = true;
    started = pm->restartMQTTDeferred();
    return true;
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

// ============================================================================
// 构造函数
// ============================================================================

MqttRouteHandler::MqttRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

// ============================================================================
// 路由注册
// ============================================================================

void MqttRouteHandler::setupRoutes(AsyncWebServer* server) {
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

// ============================================================================
// POST /api/mqtt/test — 测试MQTT连接
// ============================================================================

void MqttRouteHandler::handleTestMqttConnection(AsyncWebServerRequest* request) {
    if (!ctx->requirePermission(request, "config.edit")) return;

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
            HandlerUtils::sendJsonStream(request, doc);
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
            String transportError;
            Client* testTransport = selectMqttTestClient(ctx, testWifi, transportError);
            if (!testTransport) {
                doc["data"]["connected"] = false;
                doc["data"]["error"] = -9;
                doc["data"]["errorMessage"] = transportError;
                HandlerUtils::sendJsonStream(request, doc);
                return;
            }

            PubSubClient testClient(*testTransport);
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
        HandlerUtils::sendJsonStream(request, doc);
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

    JsonDocument doc;
    doc["success"] = true;
    doc["data"]["clientId"] = clientId;  // 返回实际使用的clientId
    doc["data"]["authType"] = "Simple";  // 标识认证类型

    WiFiClient testWifi;
    String transportError;
    Client* testTransport = selectMqttTestClient(ctx, testWifi, transportError);
    if (!testTransport) {
        doc["data"]["connected"] = false;
        doc["data"]["error"] = -9;
        doc["data"]["errorMessage"] = transportError;
        HandlerUtils::sendJsonStream(request, doc);
        return;
    }

    PubSubClient testClient(*testTransport);
    testClient.setServer(server.c_str(), port);
    testClient.setBufferSize(512);

    bool connected = testClient.connect(
        clientId.c_str(),
        username.isEmpty() ? nullptr : username.c_str(),
        connPassword.isEmpty() ? nullptr : connPassword.c_str());

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

    HandlerUtils::sendJsonStream(request, doc);
}

// ============================================================================
// GET /api/mqtt/status — 获取MQTT状态
// ============================================================================

void MqttRouteHandler::handleGetMqttStatus(AsyncWebServerRequest* request) {
    // 兼容"可编辑但不可查看"的自定义角色：允许 config.view 或 config.edit 访问状态接口
    if (!ctx->requireAnyPermission(request, "config.view", "config.edit")) return;

    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return;
    }

    // 检查网络状态
    bool internetAvailable = false;
    if (ctx->networkManager) {
        FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
        NetworkStatusInfo netInfo = netMgr->getStatusInfo();
        internetAvailable = netInfo.internetAvailable;
    }

    bool mqttConfigEnabled = false;
    String configuredServer;
    int configuredPort = 0;
    String configuredClientId;
    bool mqttConfigLoaded = loadMqttStatusConfig(
        mqttConfigEnabled,
        configuredServer,
        configuredPort,
        configuredClientId
    );

    MQTTClient* mqtt = pm->getMQTTClient();
    bool autoStartAttempted = false;
    bool autoStartStarted = false;
    tryAutoStartMqttForStatus(
        pm,
        mqtt,
        mqttConfigLoaded,
        mqttConfigEnabled,
        autoStartAttempted,
        autoStartStarted
    );
    if (autoStartStarted) {
        mqtt = pm->getMQTTClient();
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["enabled"] = mqttConfigLoaded ? mqttConfigEnabled : false;
    data["autoStartAttempted"] = autoStartAttempted;
    data["autoStartStarted"] = autoStartStarted;

    if (mqtt) {
        // 防御性检查：确保 MQTT 对象已完全初始化
        // 通过检查 server 和 port 是否有效来判断配置是否已加载
        MQTTConfig cfg = mqtt->getConfig();
        bool hasValidConfig = mqttHasValidRuntimeConfig(mqtt);
        
        if (hasValidConfig) {
            data["initialized"] = true;
            
            // MQTT连接状态：返回实际连接状态，internetAvailable 作为参考信息
            bool mqttConnected = mqtt->getIsConnected() && !mqtt->isStopped();
            data["connected"] = mqttConnected;
            data["connecting"] = !mqttConnected && cfg.autoReconnect && !mqtt->isStopped();
            data["stopped"] = mqtt->isStopped();
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
            // MQTT 对象存在但配置未加载
            data["initialized"] = false;
            data["connected"] = false;
            data["connecting"] = mqttConfigEnabled && autoStartAttempted;
            data["internetAvailable"] = internetAvailable;
            if (!configuredServer.isEmpty()) data["server"] = configuredServer;
            if (configuredPort > 0) data["port"] = configuredPort;
            if (!configuredClientId.isEmpty()) data["clientId"] = configuredClientId;
            data["error"] = "MQTT config not loaded";
        }
    } else {
        data["initialized"] = false;
        data["connected"] = false;
        data["connecting"] = mqttConfigEnabled && autoStartAttempted;
        data["internetAvailable"] = internetAvailable;
        if (!configuredServer.isEmpty()) data["server"] = configuredServer;
        if (configuredPort > 0) data["port"] = configuredPort;
        if (!configuredClientId.isEmpty()) data["clientId"] = configuredClientId;
    }

    HandlerUtils::sendJsonStream(request, doc);
}

// ============================================================================
// POST /api/mqtt/reconnect — 重连MQTT
// ============================================================================

void MqttRouteHandler::handleMqttReconnect(AsyncWebServerRequest* request) {
    if (!ctx->requirePermission(request, "config.edit")) return;

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

    HandlerUtils::sendJsonStream(request, doc);
}

// ============================================================================
// POST /api/mqtt/disconnect — 断开MQTT连接
// ============================================================================

void MqttRouteHandler::handleMqttDisconnect(AsyncWebServerRequest* request) {
    if (!ctx->requirePermission(request, "config.edit")) return;

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

// ============================================================================
// POST /api/mqtt/ntp-sync — NTP时间同步
// ============================================================================

void MqttRouteHandler::handleMqttNtpSync(AsyncWebServerRequest* request) {
    if (!ctx->requirePermission(request, "config.edit")) return;

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

#endif // FASTBEE_ENABLE_MQTT
