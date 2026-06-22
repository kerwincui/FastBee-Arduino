#include "core/FeatureFlags.h"
#include "systems/LoggerSystem.h"
#if FASTBEE_ENABLE_MQTT

#include "./network/handlers/MqttRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "./protocols/ProtocolManager.h"
#include "./protocols/MQTTClient.h"
#include "./core/FastBeeFramework.h"
#include "./systems/ConfigStorage.h"
#include "./systems/SystemRebooter.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>

static const char* MQTT_AES_IV = "wumei-smart-open";
static const char* MQTT_TEST_BACKUP_PATH = "/config/mqtt-backup.json";

// 测试连接后需要恢复原配置的标志
static bool s_mqttTestPendingRestore = false;
static unsigned long s_mqttTestStartTime = 0;
static bool s_mqttTestWasConnected = false;   // 记录测试连接是否成功
static bool s_mqttTestJustRestored = false;   // 标记刚刚执行了恢复

// ============================================================================
// 备份/恢复 MQTT 配置（测试连接不永久修改 protocol.json）
// ============================================================================
static bool backupMqttConfig() {
    JsonDocument doc;
    if (!ConfigStorage::getInstance().loadProtocolConfig(doc)) {
        LOG_WARNING("[MQTT Test] Failed to load protocol.json for backup");
        return false;
    }

    // 只保存 MQTT 部分到备份文件
    JsonDocument backupDoc;
    if (doc["mqtt"].is<JsonObject>()) {
        backupDoc["mqtt"] = doc["mqtt"];
    }

    File f = LittleFS.open(MQTT_TEST_BACKUP_PATH, "w");
    if (!f) return false;
    serializeJson(backupDoc, f);
    f.close();
    // DEBUG: log what was backed up
    {
        String bs = backupDoc["mqtt"]["server"].as<String>();
        char dbgBuf[100];
        snprintf(dbgBuf, sizeof(dbgBuf), "[MQTT Test DEBUG] Backup server: %s", bs.c_str());
        LOG_INFO(dbgBuf);
    }
    LOG_INFO("[MQTT Test] Original MQTT config backed up");
    return true;
}

static bool restoreMqttConfigFromBackup() {
    if (!LittleFS.exists(MQTT_TEST_BACKUP_PATH)) return false;

    JsonDocument backupDoc;
    {
        File f = LittleFS.open(MQTT_TEST_BACKUP_PATH, "r");
        if (!f) return false;
        DeserializationError err = deserializeJson(backupDoc, f);
        f.close();
        if (err) return false;
    }

    // 加载当前 protocol.json
    JsonDocument protoDoc;
    if (!ConfigStorage::getInstance().loadProtocolConfig(protoDoc)) return false;

    // 用备份覆盖 MQTT 部分
    if (backupDoc["mqtt"].is<JsonObject>()) {
        protoDoc["mqtt"] = backupDoc["mqtt"];
    }

    bool ok = ConfigStorage::getInstance().saveProtocolConfig(protoDoc);
    if (ok) {
        LittleFS.remove(MQTT_TEST_BACKUP_PATH);
        LOG_INFO("[MQTT Test] Original MQTT config restored from backup");
    }
    return ok;
}

// ============================================================================
// 自动恢复检查（在主循环中周期调用）
// 当测试连接成功或超时后，自动恢复原始 MQTT 配置并重启客户端
// ============================================================================
static unsigned long s_lastRestoreCheck = 0;

void MqttRouteHandler::checkPendingTestRestore() {
    if (!s_mqttTestPendingRestore) return;

    // 每 2 秒检查一次，避免频繁检查
    unsigned long now = millis();
    if (now - s_lastRestoreCheck < 2000UL) return;
    s_lastRestoreCheck = now;

    unsigned long elapsed = now - s_mqttTestStartTime;

    // 检查 MQTT 连接状态
    bool connected = false;
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    if (fw && fw->getProtocolManager()) {
        MQTTClient* mqtt = fw->getProtocolManager()->getMQTTClient();
        connected = mqtt && mqtt->getIsConnected() && !mqtt->isStopped();
    }

    // 连接成功 或 超过 20 秒 时恢复原配置
    if (connected || elapsed > 20000UL) {
        s_mqttTestWasConnected = connected;
        s_mqttTestPendingRestore = false;
        if (restoreMqttConfigFromBackup()) {
            s_mqttTestJustRestored = true;
            if (fw && fw->getProtocolManager()) {
                MQTTClient* mqtt = fw->getProtocolManager()->getMQTTClient();
                if (mqtt && !mqtt->isStopped()) {
                    mqtt->stop();
                }
                fw->getProtocolManager()->restartMQTTDeferred();
            }
            LOG_INFO("[MQTT Test] Auto-restore: config restored, main client restarting");
        }
    }
}

// ============================================================================
// 保存测试成功的 MQTT 参数到 protocol.json（确保 restartMQTTDeferred 加载最新配置）
// ============================================================================
static bool saveMqttTestConfig(const String& server, int port, const String& username,
                               const String& password, const String& authCode,
                               int authType, const String& mqttSecret, const String& scheme = "mqtt",
                               const String& clientId = "") {
    JsonDocument doc;
    if (!ConfigStorage::getInstance().loadProtocolConfig(doc)) {
        LOG_WARNING("[MQTT Test] Failed to load protocol.json for saving test config");
        return false;
    }

    JsonObject mqtt = doc["mqtt"].to<JsonObject>();
    // 仅更新测试表单中用户可能修改的字段
    mqtt["scheme"]   = scheme;
    mqtt["server"]   = server;
    mqtt["port"]     = port;
    mqtt["username"] = username;
    mqtt["password"] = password;
    mqtt["authType"] = authType;
    if (!clientId.isEmpty())   mqtt["clientId"]   = clientId;
    if (!authCode.isEmpty())   mqtt["authCode"]   = authCode;
    if (!mqttSecret.isEmpty()) mqtt["mqttSecret"] = mqttSecret;

    bool ok = ConfigStorage::getInstance().saveProtocolConfig(doc);
    if (ok) {
        LOG_INFO("[MQTT Test] Test config saved to protocol.json");
    } else {
        LOG_WARNING("[MQTT Test] Failed to save test config to protocol.json");
    }
    return ok;
}

// ============================================================================
// MQTT 测试辅助函数
// ============================================================================

// scheme 参数版本：支持 mqtt/mqtts 协议选择
static Client* selectMqttTestClient(WebHandlerContext* ctx, WiFiClient& fallbackClient,
                                     String& errorMessage, const String& scheme) {
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

    // NTP 时间同步无需加密，强制降级 HTTPS → HTTP 避免 SSL 内存分配失败
    if (url.startsWith("https://")) {
        url = "http://" + url.substring(8);
    }

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

    bool needsStart = !mqttHasValidRuntimeConfig(mqtt);

    // 新增：MQTT 客户端存在且配置有效，但被显式停止或未连接 → 也需要重启
    if (!needsStart && mqtt) {
        if (!mqtt->getIsConnected() && mqtt->isStopped()) {
            needsStart = true;
        }
    }

    if (!needsStart) return false;

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
    if (!ctx->requireAuth(request)) return;

    String server = ctx->getParamValue(request, "server", "");
    int port = ctx->getParamInt(request, "port", 1883);
    String scheme = ctx->getParamValue(request, "scheme", "mqtt");
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

    // ====== 快速路径：主客户端已连接或正在连接且配置匹配时，直接返回 ======
    // 避免创建重复测试连接导致：
    //   1. 华为云IoT等平台因相同clientId踢掉已有连接 (MQTT_BAD_CLIENT_ID)
    //   2. MQTTS TLS握手在ESP32-C6上耗时较长导致测试超时
    //   3. 额外TCP连接占用有限的socket资源
    //   4. 主客户端正在连接中（尚未connected）时重复clientId导致被broker拒绝
    {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            MQTTClient* existingMqtt = pm->getMQTTClient();
            if (existingMqtt && !existingMqtt->isStopped()) {
                MQTTConfig cfg = existingMqtt->getConfig();
                // 检查当前表单的 server:port 和 scheme 是否与已连接配置一致
                bool serverMatch = (cfg.server == server && cfg.port == port);
                bool schemeMatch = (cfg.scheme == scheme);
                if (serverMatch && schemeMatch) {
                    JsonDocument doc;
                    doc["success"] = true;
                    if (existingMqtt->getIsConnected()) {
                        // 已连接：直接返回成功
                        doc["data"]["connected"] = true;
                        doc["data"]["alreadyConnected"] = true;
                    } else {
                        // 正在连接/重连中：返回deferred，让前端等状态轮询确认
                        doc["data"]["connected"] = false;
                        doc["data"]["deferred"] = true;
                    }
                    doc["data"]["server"] = cfg.server;
                    doc["data"]["port"] = cfg.port;
                    doc["data"]["clientId"] = cfg.clientId;
                    doc["data"]["scheme"] = cfg.scheme;
                    HandlerUtils::sendJsonStream(request, doc);
                    return;
                }
            }
        }
    }

    // ====== 测试连接：保存配置 + 重启主客户端 ======
    // 不再创建独立的 PubSubClient 进行测试，原因：
    //   1. 独立测试客户端与主客户端使用不同的 WiFiClient/PubSubClient 实例，
    //      在部分 broker（如华为云IoT）上可能因 DNS 解析、TCP 路由等差异导致
    //      测试返回 MQTT_BAD_CLIENT_ID 而保存后主客户端却能正常连接
    //   2. 华为云IoT等平台在相同 clientId 并发连接时返回 CONNACK code 2，
    //      即使先停主客户端也可能因 broker session 未清理而失败
    //   3. 保存后通过主客户端重连（与"保存"按钮完全相同的路径），
    //      确保测试行为与保存行为一致，消除歧义
    // 前端通过 deferred + 状态轮询确认最终连接结果

    JsonDocument doc;
    doc["success"] = true;

    // AES 加密认证：验证 clientId 格式和密码生成（轻量级本地校验）
    if (authType == 1) {
        // 优先使用前端传递的clientId（如果已经是E开头格式）
        if (!clientId.isEmpty() && clientId.startsWith("E&")) {
            // 使用前端传递的E认证clientId
        } else if (!deviceNum.isEmpty() && !productId.isEmpty() && !userId.isEmpty()) {
            clientId = "E&" + deviceNum + "&" + productId + "&" + userId;
        }

        if (clientId.isEmpty() || !clientId.startsWith("E&")) {
            doc["data"]["connected"] = false;
            doc["data"]["error"] = -8;
            doc["data"]["errorMessage"] = "AES clientId requires E&deviceNum&productId&userId format";
            HandlerUtils::sendJsonStream(request, doc);
            return;
        }

        // 验证AES密码能否正常生成（不依赖网络）
        String aesErr;
        String encryptedPassword = buildEncryptedPasswordForMqttTest(password, authCode, mqttSecret, ntpServer, &aesErr);
        if (encryptedPassword.isEmpty()) {
            doc["data"]["connected"] = false;
            doc["data"]["error"] = -7;
            doc["data"]["errorMessage"] = aesErr.isEmpty() ? "AES password generation failed" : aesErr;
            HandlerUtils::sendJsonStream(request, doc);
            return;
        }

        doc["data"]["clientId"] = clientId;
        doc["data"]["authType"] = "AES";
        // AES 校验通过，走保存+重启流程
    } else {
        // 简单认证模式：构建 clientId
        if (clientId.isEmpty()) {
            if (!deviceNum.isEmpty() && !productId.isEmpty()) {
                clientId = "S&" + deviceNum + "&" + productId + "&" + userId;
            } else {
                char id[20];
                snprintf(id, sizeof(id), "TEST-%04X", (unsigned)esp_random() & 0xFFFF);
                clientId = id;
            }
        }
        doc["data"]["clientId"] = clientId;
        doc["data"]["authType"] = "Simple";
    }

    // 日志：记录测试连接参数
    {
        char logBuf[160];
        snprintf(logBuf, sizeof(logBuf),
                 "[MQTT Test] Save+Restart: server=%s:%d scheme=%s authType=%d clientId=%s",
                 server.c_str(), port, scheme.c_str(), authType, clientId.c_str());
        LOG_INFO(logBuf);
    }

    // 步骤1: 备份当前 MQTT 配置（测试完成后自动恢复，不永久修改 protocol.json）
    bool backed = backupMqttConfig();
    if (!backed) {
        LOG_WARNING("[MQTT Test] Backup failed, proceeding without backup");
    }

    // 步骤2: 保存测试表单配置到 protocol.json（临时，供主客户端读取）
    bool saved = saveMqttTestConfig(server, port, username, password, authCode, authType, mqttSecret, scheme, clientId);
    if (!saved) {
        doc["data"]["connected"] = false;
        doc["data"]["error"] = -10;
        doc["data"]["errorMessage"] = "Failed to save config to protocol.json";
        HandlerUtils::sendJsonStream(request, doc);
        return;
    }

    // DEBUG: 验证 protocol.json 已更新
    {
        File dbgF = LittleFS.open("/config/protocol.json", "r");
        if (dbgF) {
            JsonDocument dbgDoc;
            deserializeJson(dbgDoc, dbgF);
            dbgF.close();
            String dbgServer = dbgDoc["mqtt"]["server"].as<String>();
            char dbgBuf[120];
            snprintf(dbgBuf, sizeof(dbgBuf), "[MQTT Test DEBUG] protocol.json server after save: %s", dbgServer.c_str());
            LOG_INFO(dbgBuf);
        }
    }

    // 设置恢复标志：status handler 检测到测试结果后自动恢复原配置
    s_mqttTestPendingRestore = true;
    s_mqttTestStartTime = millis();

    // 步骤3: 停止主客户端（防止旧连接占用相同 clientId 导致 broker 拒绝）
    {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            MQTTClient* existingMqtt = pm->getMQTTClient();
            if (existingMqtt && !existingMqtt->isStopped()) {
                LOG_INFO("[MQTT Test] Stopping main client before deferred restart");
                existingMqtt->stop();
            }
        }
    }

    // 步骤4: 通过 restartMQTTDeferred 重建主客户端（与"保存"按钮完全相同的路径）
    // 主客户端在 loop 中使用新配置重连，前端通过状态轮询确认连接结果
    bool deferred = false;
    {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            deferred = pm->restartMQTTDeferred();
        }
    }

    doc["data"]["connected"] = false;
    doc["data"]["deferred"] = deferred;
    doc["data"]["server"] = server;
    doc["data"]["port"] = port;
    if (!deferred) {
        doc["data"]["error"] = -11;
        doc["data"]["errorMessage"] = "Deferred restart failed (heap or config)";
    }

    HandlerUtils::sendJsonStream(request, doc);
}

// ============================================================================
// GET /api/mqtt/status — 获取MQTT状态
// ============================================================================

void MqttRouteHandler::handleGetMqttStatus(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

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

    // 读取测试连接结果标志（由主循环 checkPendingTestRestore 设置）
    bool testWasConnected = s_mqttTestWasConnected;
    bool testJustRestored = s_mqttTestJustRestored;
    if (testJustRestored) {
        s_mqttTestJustRestored = false;  // 清除标志，只报告一次
    }

    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["enabled"] = mqttConfigLoaded ? mqttConfigEnabled : false;
    data["autoStartAttempted"] = autoStartAttempted;
    data["autoStartStarted"] = autoStartStarted;

    // 测试连接结果：在restore前记录的连接状态
    if (testWasConnected || testJustRestored) {
        data["testConnected"] = testWasConnected;
        data["testRestored"] = testJustRestored;
    }

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
    if (!ctx->requireAuth(request)) return;

    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return;
    }

    // 如果 MQTT 已连接，直接返回成功（无需重建）
    MQTTClient* mqtt = pm->getMQTTClient();
    if (mqtt && mqtt->getIsConnected()) {
        JsonDocument doc;
        doc["success"] = true;
        doc["data"]["connected"] = true;
        doc["data"]["reconnecting"] = false;
        HandlerUtils::sendJsonStream(request, doc);
        return;
    }

    // 检查 autoReconnect 配置：用户关闭自动重连时，不应自动触发重连
    // 前端状态轮询可能在 autoReconnect=false 时仍误调用此接口，后端需徽底
    {
        MQTTConfig cfg = mqtt ? mqtt->getConfig() : MQTTConfig();
        if (mqtt && !cfg.autoReconnect) {
            JsonDocument doc;
            doc["success"] = true;
            doc["data"]["connected"] = false;
            doc["data"]["autoReconnectDisabled"] = true;
            HandlerUtils::sendJsonStream(request, doc);
            return;
        }
    }

    // 未连接时，使用 SystemRebooter 重启设备（避免运行时 destroy/rebuild MQTT 客户端导致 DRAM 碎片化）
    // 重启后 MQTT 会在 boot 流程中自动连接，确保干净的堆状态
    JsonDocument doc;
    doc["success"] = true;
    doc["data"]["connected"] = false;
    doc["data"]["rebooting"] = true;
    doc["data"]["message"] = "Device will reboot to reconnect MQTT cleanly";

    HandlerUtils::sendJsonStream(request, doc);

    // HTTP 响应已发送，调度延迟重启
    SystemRebooter::scheduleConfigReboot("MQTT manual reconnect");
}

// ============================================================================
// POST /api/mqtt/disconnect — 断开MQTT连接
// ============================================================================

void MqttRouteHandler::handleMqttDisconnect(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

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
    if (!ctx->requireAuth(request)) return;

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
