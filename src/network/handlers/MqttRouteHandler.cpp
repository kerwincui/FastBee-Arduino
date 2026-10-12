#include "core/FeatureFlags.h"
#include "network/LiteralStringMatch.h"
#include "systems/LoggerSystem.h"
#if FASTBEE_ENABLE_MQTT

#include "./network/handlers/MqttRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "./protocols/ProtocolManager.h"
#include "./protocols/MQTTClient.h"
#include "protocols/MqttReportQueueStatus.h"
#include "protocols/MqttDiagnosticJson.h"
#include "network/FixedJsonField.h"
#include "./core/FastBeeFramework.h"
#include "./systems/ConfigStorage.h"
#include "./systems/HealthMonitor.h"
#include "./systems/SystemRebooter.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>

static const char* MQTT_AES_IV = "wumei-smart-open";
using FastBee::MqttDiagnostic;
using FastBee::writeMqttDiagnostic;
using FastBee::writeMqttDiagnosticData;
namespace MqttDiagFields = FastBee::MqttDiagnosticFields;
static const char* MQTT_TEST_BACKUP_PATH = "/config/mqtt-backup.json";

// 测试连接后需要恢复原配置的标志
static std::atomic<bool> s_mqttTestPendingRestore{false};
static unsigned long s_mqttTestStartTime = 0;
static std::atomic<bool> s_mqttTestWasConnected{false};
static std::atomic<bool> s_mqttTestJustRestored{false};
static std::atomic<bool> s_mqttTestRestoreBlocked{false};
static FastBee::MqttClientLifetime s_mqttTestGate;
static String s_mqttTestId; // Accessed only under s_mqttTestGate.

// ============================================================================
// 备份/恢复 MQTT 配置（测试连接不永久修改 protocol.json）
// ============================================================================
static bool backupMqttConfig(JsonVariantConst original, JsonVariantConst candidate) {
    // Never overwrite a previous recovery record, even if corrupt/legacy.
    if (LittleFS.exists(MQTT_TEST_BACKUP_PATH) || !original.is<JsonObjectConst>() ||
        !candidate.is<JsonObjectConst>()) return false;
    JsonDocument backupDoc;
    FastBee::setFixedJsonField(backupDoc, "version", 2);
    FastBee::setFixedJsonField(backupDoc, "mqtt", original);
    FastBee::setFixedJsonField(backupDoc, "testMqtt", candidate);
    if (backupDoc.overflowed()) return false;

    File f = LittleFS.open(MQTT_TEST_BACKUP_PATH, "w");
    if (!f) return false;
    const size_t expected = measureJson(backupDoc);
    const size_t written = serializeJson(backupDoc, f);
    f.flush();
    f.close();
    // Short writes and failed readback retain evidence and forbid config save.
    if (written != expected) return false;
    File check = LittleFS.open(MQTT_TEST_BACKUP_PATH, "r");
    if (!check) return false;
    JsonDocument verified;
    const bool valid = !deserializeJson(verified, check);
    check.close();
    return valid && verified.as<JsonVariantConst>() == backupDoc.as<JsonVariantConst>();
}

static bool restoreMqttConfigFromBackup(bool* recoveredEnabled = nullptr) {
    if (!LittleFS.exists(MQTT_TEST_BACKUP_PATH)) return false;

    JsonDocument backupDoc;
    {
        File f = LittleFS.open(MQTT_TEST_BACKUP_PATH, "r");
        if (!f) return false;
        DeserializationError err = deserializeJson(backupDoc, f);
        f.close();
        if (err) return false;
    }
    if ((backupDoc["version"] | 0) != 2 ||
        !backupDoc["mqtt"].is<JsonObject>() || !backupDoc["testMqtt"].is<JsonObject>())
        return false;

    // 加载当前 protocol.json
    JsonDocument protoDoc;
    if (!ConfigStorage::getInstance().loadProtocolConfig(protoDoc)) return false;

    const JsonVariantConst current = protoDoc["mqtt"].as<JsonVariantConst>();
    const JsonVariantConst original = backupDoc["mqtt"].as<JsonVariantConst>();
    if (recoveredEnabled) *recoveredEnabled = original["enabled"] | true;
    if (current == original) {
        // Save may have succeeded on a previous pass while cleanup failed.
        return LittleFS.remove(MQTT_TEST_BACKUP_PATH);
    }
    // Preserve conflicts; the final compare/write is serialized with other
    // ConfigStorage protocol writers (raw filesystem writes remain out of scope).
    if (current != backupDoc["testMqtt"].as<JsonVariantConst>()) return false;
    if (!ConfigStorage::getInstance().compareExchangeMqttConfig(
            backupDoc["testMqtt"].as<JsonVariantConst>(), original))
        return false;
    return LittleFS.remove(MQTT_TEST_BACKUP_PATH);
}

// ============================================================================
// 自动恢复检查（在主循环中周期调用）
// 当测试连接成功或超时后，自动恢复原始 MQTT 配置并重启客户端
// ============================================================================
static bool loadMqttStatusConfig(bool& enabled,
                                 String& server,
                                 int& port,
                                 String& clientId,
                                 bool* disabledByMemory = nullptr); // forward decl
static unsigned long s_lastRestoreCheck = 0;

void MqttRouteHandler::checkPendingTestRestore() {
    FastBee::MqttClientLifetime::Write testGuard(s_mqttTestGate);
    if (!testGuard) return;

    // 每 2 秒检查一次，避免频繁检查
    unsigned long now = millis();
    if (now - s_lastRestoreCheck < 2000UL) return;
    s_lastRestoreCheck = now;

    if (!s_mqttTestPendingRestore) {
        if (!LittleFS.exists(MQTT_TEST_BACKUP_PATH)) return;
        // Recovery survives restart/failed staging; never replace this record.
        s_mqttTestPendingRestore = true;
        s_mqttTestRestoreBlocked = true;
        s_mqttTestStartTime = now - 20001UL;
        s_mqttTestId = "";
    }

    unsigned long elapsed = now - s_mqttTestStartTime;

    // 检查 MQTT 连接状态
    bool connected = false;
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    if (fw && fw->getProtocolManager()) {
        auto mqtt = fw->getProtocolManager()->acquireMQTTClient();
        connected = mqtt && mqtt->isConnectionTestOnly() && !s_mqttTestId.isEmpty() &&
            mqtt->getConnectionTestId() == s_mqttTestId &&
            mqtt->getIsConnected() && !mqtt->isStopped();
    }

    // 连接成功 或 超过 20 秒 时恢复原配置
    if (connected || elapsed > 20000UL) {
        if (connected) s_mqttTestWasConnected = true;
        bool mqttEnabled = false;
        if (restoreMqttConfigFromBackup(&mqttEnabled)) {
            s_mqttTestPendingRestore = false;
            s_mqttTestRestoreBlocked = false;
            s_mqttTestJustRestored = true;
            if (fw && fw->getProtocolManager()) {
                fw->getProtocolManager()->stopMQTT();

                // 仅在 MQTT 启用时才重启客户端
                // 禁用时仅恢复配置并停止客户端，避免禁用状态下意外重连
                if (mqttEnabled) {
                    fw->getProtocolManager()->restartMQTTDeferred(true);
                    LOG_INFO("[MQTT Test] Auto-restore: config restored, main client restarting");
                } else {
                    LOG_INFO("[MQTT Test] Auto-restore: config restored, MQTT disabled - not restarting");
                }
            }
        } else {
            s_mqttTestRestoreBlocked = true;
        }
    }
}

// ============================================================================
// 暂存仅连接测试配置；先验证恢复记录，再比较/替换 MQTT 子节点。
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

    if (!doc["mqtt"].is<JsonObject>() || (doc["mqtt"]["connectionTestOnly"] | false)) return false;
    JsonDocument candidate;
    candidate.set(doc["mqtt"]);
    JsonObject mqtt = candidate.as<JsonObject>();
    // 仅更新测试表单中用户可能修改的字段
    FastBee::setFixedJsonField(mqtt, "scheme", scheme);
    FastBee::setFixedJsonField(mqtt, "server", server);
    FastBee::setFixedJsonField(mqtt, "port", port);
    FastBee::setFixedJsonField(mqtt, "username", username);
    FastBee::setFixedJsonField(mqtt, "password", password);
    FastBee::setFixedJsonField(mqtt, "authType", authType);
    if (!clientId.isEmpty())   FastBee::setFixedJsonField(mqtt, "clientId", clientId);
    if (!authCode.isEmpty())   FastBee::setFixedJsonField(mqtt, "authCode", authCode);
    if (!mqttSecret.isEmpty()) FastBee::setFixedJsonField(mqtt, "mqttSecret", mqttSecret);

    const uint32_t sequence = FastBee::nextReportInstanceId();
    if (!sequence) return false;
    s_mqttTestId = String(esp_random(), HEX) + "-" + String(sequence);
    FastBee::setFixedJsonField(mqtt, "connectionTestOnly", true);
    FastBee::setFixedJsonField(mqtt, "connectionTestId", s_mqttTestId);
    FastBee::setFixedJsonField(mqtt, "enabled", true); // Only this explicit temporary probe, restored afterward.
    FastBee::setFixedJsonField(mqtt, "autoReconnect", true); // Explicit probe must try even if normal auto-reconnect is off.
    if (candidate.overflowed() ||
        !backupMqttConfig(doc["mqtt"].as<JsonVariantConst>(), candidate.as<JsonVariantConst>()))
        return false;
    bool ok = ConfigStorage::getInstance().compareExchangeMqttConfig(
        doc["mqtt"].as<JsonVariantConst>(), candidate.as<JsonVariantConst>());
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
        if (scheme == "mqtts") {
            errorMessage = "MQTTS requires the main client's TLS transport";
            return nullptr;
        }
        return &fallbackClient;
    }

    if (!netMgr->isNetworkConnected()) {
        errorMessage = "Active network is not connected";
        return nullptr;
    }

    Client* activeClient = nullptr;
#if FASTBEE_ENABLE_CELLULAR
    if (scheme == "mqtts" && netMgr->getNetworkType() == NetworkType::NET_4G) {
        CellularAdapter* cell = netMgr->getCellularAdapter();
        activeClient = cell ? cell->getSecureClient() : nullptr;
        if (!activeClient) errorMessage = "Cellular TLS transport is unavailable";
        return activeClient;
    }
#endif
    if (scheme == "mqtts") {
        errorMessage = "MQTTS requires a supported TLS transport";
        return nullptr;
    }
    activeClient = netMgr->getActiveClient();
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

static bool loadMqttStatusConfig(bool& enabled,
                                 String& server,
                                 int& port,
                                 String& clientId,
                                 bool* disabledByMemory) {
    enabled = false;
    server = "";
    port = 0;
    clientId = "";
    if (disabledByMemory) *disabledByMemory = false;

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
    // HealthMonitor writes the canonical marker below when it disables MQTT
    // under memory pressure. Accept the older alias as well so devices that
    // reboot with an earlier protocol.json still expose the right reason.
    if (disabledByMemory) {
        *disabledByMemory = (mqtt["disabled_by_memory"] | false) ||
                            (mqtt["disable_mqtt_by_memory"] | false);
    }

    // A syntactically valid JSON object is not necessarily a usable MQTT
    // configuration.  When MQTT is enabled, reject an empty broker or an
    // invalid port here so a stale in-memory MQTTClient cannot make the status
    // endpoint report a misleading connected/OK state after a failed reload.
    // An explicitly disabled MQTT section remains "loaded" even when the
    // broker fields are empty; that is a valid user choice and should render
    // as "MQTT 已禁用" rather than as a configuration error.
    if (!enabled) return true;
    return !server.isEmpty() && port > 0 && port <= 65535;
}

static bool mqttHasValidRuntimeConfig(MQTTClient* mqtt) {
    if (!mqtt) return false;
    MQTTConfig cfg = mqtt->getConfig();
    return (cfg.server != nullptr && cfg.server[0] != '\0' && cfg.port > 0);
}

// Status-only code emitted when the persisted MQTT section is missing or
// invalid. Keeping it separate from PubSubClient's return codes prevents the
// API from reporting lastError=0/OK alongside a configuration failure reason.
static constexpr int MQTT_STATUS_CONFIG_ERROR = -12;
// Distinguishes an automatic low-memory disable from an ordinary user choice
// (enabled=false), so the Web UI can give a recovery action instead of saying
// only that MQTT is offline.
static constexpr int MQTT_STATUS_DISABLED_BY_MEMORY = -13;
// A severe-memory episode can stop MQTT temporarily before the 30s persistent
// disable threshold is reached. Keep that state distinct from a user stop so
// the Web UI explains that the client will be restored automatically.
static constexpr int MQTT_STATUS_STOPPED_BY_MEMORY = -14;

// Keep MQTT failure reporting allocation-free and stable for the Web UI.  The
// numeric PubSubClient code remains in `lastError` for compatibility; these
// fields let callers distinguish a broker rejection from a local network or
// TLS-memory failure without scraping serial logs.
static const char* mqttErrorName(int code) {
    switch (code) {
        case 0:  return "OK";
        case -1: return "MQTT_DISCONNECTED";
        case -2: return "MQTT_CONNECT_FAILED";
        case -3: return "MQTT_CONNECTION_LOST";
        case -4: return "MQTT_CONNECTION_TIMEOUT";
        case -5: return "MQTTS_FEATURE_DISABLED";
        case -15: return "MQTTS_TRANSPORT_UNAVAILABLE";
        case -11: return "MQTT_CLIENT_INIT_FAILED";
        case MQTT_STATUS_CONFIG_ERROR: return "MQTT_CONFIG_INVALID";
        case MQTT_STATUS_DISABLED_BY_MEMORY: return "MQTT_DISABLED_BY_MEMORY";
        case MQTT_STATUS_STOPPED_BY_MEMORY: return "MQTT_STOPPED_BY_MEMORY";
        case 1:  return "MQTT_BAD_PROTOCOL";
        case 2:  return "MQTT_BAD_CLIENT_ID";
        case 3:  return "MQTT_UNAVAILABLE";
        case 4:  return "MQTT_BAD_CREDENTIALS";
        case 5:  return "MQTT_UNAUTHORIZED";
        default: return "MQTT_UNKNOWN";
    }
}

static const char* mqttErrorClass(int code, bool tlsMemoryFailure) {
    if (code == 0) return "none";
    if (code == -5) return "feature";
    if (code == -11) return "resource";
    if (code == -15) return "security";
    if (code == MQTT_STATUS_CONFIG_ERROR) return "config";
    if (code == MQTT_STATUS_DISABLED_BY_MEMORY) return "resource";
    if (code == MQTT_STATUS_STOPPED_BY_MEMORY) return "resource";
    if (tlsMemoryFailure) return "memory";
    if (code >= 1 && code <= 3) return "broker";
    if (code == 4 || code == 5) return "auth";
    if (code >= -4 && code <= -1) return "network";
    return "unknown";
}

static const char* mqttErrorMessage(int code, bool networkConnected, bool tlsMemoryFailure) {
    switch (code) {
        case 0:  return "";
        case -1: return "MQTT 客户端未连接";
        case -2: return networkConnected
                    ? "无法连接 MQTT Broker（DNS/TCP）"
                    : "网络未连接，无法访问 MQTT Broker";
        case -3: return "MQTT 连接已断开，正在重连";
        case -4: return tlsMemoryFailure
                    ? "TLS 内存不足，已进入慢速重试；建议切换 mqtt"
                    : "MQTT 连接超时，请检查 Broker 地址和端口";
        case -5: return "MQTTS 功能未启用，请在功能配置中启用";
        case -15: return "TLS 传输不可用，已拒绝明文回退；请检查安全传输配置和内存";
        case -11: return "MQTT 客户端启动失败，可能是内存不足或配置无效";
        case MQTT_STATUS_CONFIG_ERROR: return "MQTT 配置未加载或格式无效，请检查网络配置";
        case MQTT_STATUS_DISABLED_BY_MEMORY: return "设备内存紧张，MQTT 已自动禁用，请恢复内存后重新启用";
        case MQTT_STATUS_STOPPED_BY_MEMORY: return "设备内存紧张，MQTT 已暂时停止，内存恢复后将自动重连";
        case 1:  return "Broker 拒绝：协议版本不支持";
        case 2:  return "Broker 拒绝：客户端 ID 无效或重复";
        case 3:  return "Broker 当前不可用";
        case 4:  return "Broker 拒绝：用户名或密码错误";
        case 5:  return "Broker 拒绝：客户端未授权";
        default: return "MQTT 连接失败，请查看错误码";
    }
}

static void setMqttErrorFields(JsonObject data, MQTTClient* mqtt, bool networkConnected) {
    const int code = mqtt ? mqtt->getLastErrorCode() : 0;
    const bool tlsMemoryFailure = mqtt && mqtt->isLastMqttsTlsMemoryFailure();
    data["lastError"] = code;  // legacy field
    data["lastErrorCode"] = code;
    data["lastErrorName"] = mqttErrorName(code);
    data["lastErrorClass"] = mqttErrorClass(code, tlsMemoryFailure);
    data["lastErrorMessage"] = mqttErrorMessage(code, networkConnected, tlsMemoryFailure);
    // errorMessage is consumed by older Web bundles; keep it as an alias.
    data["errorMessage"] = mqttErrorMessage(code, networkConnected, tlsMemoryFailure);
    data["tlsMemoryFailure"] = tlsMemoryFailure;
}

static bool tryAutoStartMqttForStatus(ProtocolManager* pm,
                                      FastBee::MqttClientLease& mqttLease,
                                      bool mqttAbsent,
                                      bool configLoaded,
                                      bool configEnabled,
                                      bool& attempted,
                                      bool& started) {
    attempted = false;
    started = false;
    if (!pm || !configLoaded || !configEnabled) return false;
    // A denied lease is not proof that the client is missing. Do not enqueue
    // another restart while an existing stop/rebuild is draining readers.
    if (!mqttLease && !mqttAbsent) return false;
    MQTTClient* mqtt = mqttLease.get();
    // Status reads cannot undo explicit/timeout cancellation, including while
    // the worker is still returning from a socket operation.
    if (mqtt && mqtt->isReconnectCancellationRequested()) return false;

    // A status read must never undo an active memory-protection decision.
    // During the short window before `disabled_by_memory` is persisted, the
    // client can look like an ordinary stopped instance; rebuilding it here
    // would immediately reclaim the memory that HealthMonitor just freed and
    // could repeat on every REST poll.  The persisted marker is checked by the
    // caller as well, while these live flags close that race.
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    HealthMonitor* health = fw ? fw->getHealthMonitor() : nullptr;
    if ((health && (health->isMqttStoppedForMemory() ||
                    health->isMqttDisabledForMemory())) ||
        (mqtt && (mqtt->isStoppedByMemory() || mqtt->isDisabledByMemory()))) {
        return false;
    }

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
    mqttLease = {}; // Release before entering the exclusive rebuild path.
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
    DeserializationError err = deserializeJson(doc, ntpJson.c_str(), ntpJson.length());
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
    if (!s_mqttTestGate.available() ||
        (ctx->protocolManager && !ctx->protocolManager->mqttLifetimeAvailable())) {
        ctx->sendError(request, 503, "MQTT admission storage unavailable; restart required");
        return;
    }
    FastBee::MqttClientLifetime::Write testGuard(s_mqttTestGate);
    if (!testGuard || s_mqttTestPendingRestore || LittleFS.exists(MQTT_TEST_BACKUP_PATH)) {
        ctx->sendError(request, 409, "MQTT test or configuration recovery is pending");
        return;
    }
    if (!ctx->protocolManager) {
        ctx->sendError(request, 503, "Protocol manager unavailable");
        return;
    }

    String server = ctx->getParamValue(request, "server", "");
    int port = ctx->getParamInt(request, "port", 1883);
    String scheme = ctx->getParamValue(request, "scheme", "mqtt");
    String clientId = ctx->getParamValue(request, "clientId", "");
    String username = ctx->getParamValue(request, "username", "");
    String password = ctx->getParamValue(request, "password", "");
    String authCode = ctx->getParamValue(request, "authCode", "");
    String mqttSecret = ctx->getParamValue(request, "mqttSecret", "");

    if (server.isEmpty() || port < 1 || port > 65535 || (scheme != "mqtt" && scheme != "mqtts")) {
        ctx->sendBadRequest(request, "Valid server, port and mqtt/mqtts scheme are required");
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
        if (FastBee::stringEqualsIgnoreCaseLiteral(authTypeStr, "ENCRYPTED")) {
            authType = 1;
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(authTypeStr, "SIMPLE")) {
            authType = 0;
        } else {
            if (authTypeStr != "0" && authTypeStr != "1") {
                ctx->sendBadRequest(request, "Unsupported MQTT authentication type");
                return;
            }
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
    // 也尝试从 protocol.json 读取（protocol.json 中的 deviceNum/userId 优先级更高）
    // 注意：productId 只从 device.json 获取，不从 protocol.json 覆盖
    if (LittleFS.exists("/config/protocol.json")) {
        File f = LittleFS.open("/config/protocol.json", "r");
        if (f) {
            JsonDocument protoDoc;
            if (!deserializeJson(protoDoc, f)) {
                JsonVariant mqtt = protoDoc["mqtt"];
                if (mqtt.is<JsonObject>()) {
                    String dn = jsonVariantToString(mqtt["deviceNum"]);
                    String ui = jsonVariantToString(mqtt["userId"]);
                    if (!dn.isEmpty()) deviceNum = dn;
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
    if (authType > 1) {
        ctx->sendBadRequest(request, "Unsupported MQTT authentication type");
        return;
    }

    // ====== 快速路径：主客户端已连接或正在连接且配置匹配时，直接返回 ======
    // 避免创建重复测试连接导致：
    //   1. 华为云IoT等平台因相同clientId踢掉已有连接 (MQTT_BAD_CLIENT_ID)
    //   2. MQTTS TLS握手在ESP32-C6上耗时较长导致测试超时
    //   3. 额外TCP连接占用有限的socket资源
    //   4. 主客户端正在连接中（尚未connected）时重复clientId导致被broker拒绝
    {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            bool absent = false;
            auto existingLease = pm->acquireMQTTClient(&absent);
            if (!existingLease && !absent) {
                JsonDocument busy;
                busy["success"] = false;
                FastBee::setFixedJsonField(busy, "data", "lifecycleBusy", true);
                FastBee::setFixedJsonField(busy, "data", "stateAvailable", false);
                FastBee::setFixedJsonField(busy, "data", "statusReason", "MQTT 实例切换中，本次测试未启动");
                HandlerUtils::sendJsonStream(request, busy);
                return;
            }
            MQTTClient* existingMqtt = existingLease.get();
            if (existingMqtt && !existingMqtt->isStopped() && !existingMqtt->isConnectionTestOnly()) {
                MQTTConfig cfg = existingMqtt->getConfig();
                // Full endpoint AND credential/identity match is required below.
                bool serverMatch = (cfg.server == server && cfg.port == port);
                bool schemeMatch = (cfg.scheme == scheme);
                // Never call a changed password/client identity "already connected".
                // Empty/generated IDs use the isolated probe path conservatively.
                const bool identityMatch = !clientId.isEmpty() && cfg.clientId == clientId &&
                    cfg.username == username && cfg.password == password &&
                    static_cast<int>(cfg.authType) == authType && cfg.authCode == authCode &&
                    cfg.mqttSecret == mqttSecret && cfg.deviceNum == deviceNum &&
                    cfg.productId == productId && cfg.userId == userId && cfg.ntpServer == ntpServer;
                if (serverMatch && schemeMatch && identityMatch) {
                    bool networkConnected = false;
                    if (ctx->networkManager) {
                        networkConnected = static_cast<FBNetworkManager*>(ctx->networkManager)->isNetworkConnected();
                    }
                    JsonDocument doc;
                    doc["success"] = true;
                    JsonObject data = doc["data"].to<JsonObject>();
                    const bool connected = existingMqtt->getIsConnected();
                    if (connected) {
                        // 已连接：直接返回成功
                        FastBee::setFixedJsonField(data, "connected", true);
                        FastBee::setFixedJsonField(data, "alreadyConnected", true);
                    } else {
                        // 正在连接/重连中：仍允许前端轮询，但把已有的
                        // 错误码和可读原因一并返回。此前这里只返回
                        // deferred=true，导致已知的认证、DNS 或内存失败
                        // 被 Web 页面显示成无原因的“连接中”。
                        FastBee::setFixedJsonField(data, "connected", false);
                        FastBee::setFixedJsonField(data, "deferred", true);
                        const int errorCode = existingMqtt->getLastErrorCode();
                        if (errorCode != 0) {
                            FastBee::setFixedJsonField(data, "failureKnown", true);
                            FastBee::setFixedJsonField(data, "statusReason", mqttErrorMessage(
                                errorCode,
                                networkConnected,
                                existingMqtt->isLastMqttsTlsMemoryFailure()));
                        } else if (!networkConnected) {
                            FastBee::setFixedJsonField(data, "statusReason", "网络未连接，无法连接 MQTT Broker");
                        } else {
                            FastBee::setFixedJsonField(data, "statusReason", "正在连接 MQTT Broker");
                        }
                    }
                    FastBee::setFixedJsonField(data, "server", cfg.server);
                    FastBee::setFixedJsonField(data, "port", cfg.port);
                    FastBee::setFixedJsonField(data, "clientId", cfg.clientId);
                    FastBee::setFixedJsonField(data, "scheme", cfg.scheme);
                    FastBee::setFixedJsonField(data, "networkConnected", networkConnected);
                    setMqttErrorFields(data, existingMqtt, networkConnected);
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
            FastBee::setFixedJsonField(doc, "data", "connected", false);
            writeMqttDiagnosticData(doc, MqttDiagnostic::AesClientIdInvalid,
                                MqttDiagFields::Error | MqttDiagFields::Reason);
            doc["success"] = false;
            HandlerUtils::sendJsonStream(request, doc);
            return;
        }

        // 验证AES密码能否正常生成（不依赖网络）
        String aesErr;
        String encryptedPassword = buildEncryptedPasswordForMqttTest(password, authCode, mqttSecret, ntpServer, &aesErr);
        if (encryptedPassword.isEmpty()) {
            FastBee::setFixedJsonField(doc, "data", "connected", false);
            FastBee::setFixedJsonField(doc, "data", "error", -7);
            FastBee::setFixedJsonField(doc, "data", "lastErrorCode", -7);
            FastBee::setFixedJsonField(doc, "data", "lastErrorName", "MQTT_AES_PASSWORD_FAILED");
            FastBee::setFixedJsonField(doc, "data", "lastErrorClass", "config");
            FastBee::setFixedJsonField(doc, "data", "errorMessage", aesErr.isEmpty() ? "AES password generation failed" : aesErr);
            FastBee::setFixedJsonField(doc, "data", "statusReason", doc["data"]["errorMessage"]);
            doc["success"] = false;
            HandlerUtils::sendJsonStream(request, doc);
            return;
        }

        FastBee::setFixedJsonField(doc, "data", "clientId", clientId);
        FastBee::setFixedJsonField(doc, "data", "authType", "AES");
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
        FastBee::setFixedJsonField(doc, "data", "clientId", clientId);
        FastBee::setFixedJsonField(doc, "data", "authType", "Simple");
    }

    // 日志：记录测试连接参数
    {
        char logBuf[160];
        snprintf(logBuf, sizeof(logBuf),
                 "[MQTT Test] Save+Restart: server=%s:%d scheme=%s authType=%d clientId=%s",
                 server.c_str(), port, scheme.c_str(), authType, clientId.c_str());
        LOG_INFO(logBuf);
    }

    // Write and read back a fresh recovery record BEFORE changing protocol.json.
    // A failed/pending backup prevents this operation and is retained.
    bool saved = saveMqttTestConfig(server, port, username, password, authCode, authType, mqttSecret, scheme, clientId);
    if (!saved) {
        FastBee::setFixedJsonField(doc, "data", "connected", false);
        writeMqttDiagnosticData(doc, MqttDiagnostic::ConfigSaveFailed,
                            MqttDiagFields::Error | MqttDiagFields::Reason);
        doc["success"] = false;
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
    s_mqttTestWasConnected = false;
    s_mqttTestJustRestored = false;
    s_mqttTestRestoreBlocked = false;

    // 步骤3: 停止主客户端（防止旧连接占用相同 clientId 导致 broker 拒绝）
    {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            pm->stopMQTT();
        }
    }

    // 步骤4: 通过 restartMQTTDeferred 重建主客户端（与"保存"按钮完全相同的路径）
    // 主客户端在 loop 中使用新配置重连，前端通过状态轮询确认连接结果
    bool deferred = false;
    FastBee::MqttRestartResult restartResult = FastBee::MqttRestartResult::NotStarted;
    {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            deferred = pm->restartMQTTDeferred(true, &restartResult);
        }
    }

    FastBee::setFixedJsonField(doc, "data", "connected", false);
    FastBee::setFixedJsonField(doc, "data", "connectionTestOnly", true);
    FastBee::setFixedJsonField(doc, "data", "testRestorePending", true);
    const bool pending = restartResult == FastBee::MqttRestartResult::Deferred;
    FastBee::setFixedJsonField(doc, "data", "deferred", deferred || pending);
    FastBee::setFixedJsonField(doc, "data", "lifecyclePending", pending);
    FastBee::setFixedJsonField(doc, "data", "server", server);
    FastBee::setFixedJsonField(doc, "data", "port", port);
    if (pending) {
        FastBee::setFixedJsonField(doc, "data", "statusReason", "MQTT 测试重建已排队，等待在途操作结束");
    } else if (!deferred) {
        writeMqttDiagnosticData(doc, MqttDiagnostic::ClientStartFailed,
                            MqttDiagFields::Error | MqttDiagFields::Reason);
        doc["success"] = false;
    } else {
        FastBee::setFixedJsonField(doc, "data", "statusReason", "正在仅连接测试，业务上报与订阅已隔离，结束后恢复配置");
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
    if (!pm->mqttLifetimeAvailable()) {
        ctx->sendError(request, 503, "MQTT admission storage unavailable; restart required");
        return;
    }

    // 检查网络状态
    bool internetAvailable = false;
    bool networkConnected = false;
    if (ctx->networkManager) {
        FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
        NetworkStatusInfo netInfo = netMgr->getStatusInfo();
        internetAvailable = netInfo.internetAvailable;
        networkConnected = netMgr->isNetworkConnected();
    }

    bool mqttConfigEnabled = false;
    String configuredServer;
    int configuredPort = 0;
    String configuredClientId;
    bool mqttDisabledByMemory = false;
    bool mqttStoppedByMemory = false;
    bool mqttConfigLoaded = loadMqttStatusConfig(
        mqttConfigEnabled,
        configuredServer,
        configuredPort,
        configuredClientId,
        &mqttDisabledByMemory
    );

    bool mqttAbsent = false;
    auto mqttLease = pm->acquireMQTTClient(&mqttAbsent);
    MQTTClient* mqtt = mqttLease.get();

    // The persisted marker is authoritative across reboots, but there is a
    // short window while HealthMonitor is writing protocol.json (or while the
    // client is being stopped) in which disk state still says enabled. Merge
    // the live flags so the UI does not offer a futile reconnect.
    FastBeeFramework* statusFw = FastBeeFramework::getInstance();
    HealthMonitor* statusHealth = statusFw ? statusFw->getHealthMonitor() : nullptr;
    if ((statusHealth && statusHealth->isMqttDisabledForMemory()) ||
        (mqtt && mqtt->isDisabledByMemory())) {
        mqttDisabledByMemory = true;
    }
    if ((statusHealth && statusHealth->isMqttStoppedForMemory()) ||
        (mqtt && mqtt->isStoppedByMemory())) {
        mqttStoppedByMemory = true;
    }

    // 防御性检查：MQTT 配置已禁用但客户端仍在运行（如保存配置后重启前的窗口期）
    // 立即停止客户端，确保不会在禁用状态下继续保持与 Broker 的连接
    if (mqttConfigLoaded && !mqttConfigEnabled && mqtt && !mqtt->isStopped()) {
        mqttLease = {};
        mqtt = nullptr;
        pm->stopMQTT();
        mqttLease = pm->acquireMQTTClient(&mqttAbsent);
        mqtt = mqttLease.get();
    }

    bool autoStartAttempted = false;
    bool autoStartStarted = false;
    tryAutoStartMqttForStatus(
        pm,
        mqttLease,
        mqttAbsent,
        mqttConfigLoaded,
        mqttConfigEnabled,
        autoStartAttempted,
        autoStartStarted
    );
    // Every attempt released the old lease, even on rebuild failure.
    // Reacquire before reading state; no pointer crosses a rebuild boundary.
    if (autoStartAttempted) {
        mqttLease = pm->acquireMQTTClient(&mqttAbsent);
        mqtt = mqttLease.get();
    }
    const bool lifecycleBusy = !mqttLease && !mqttAbsent;

    // 读取测试连接结果标志（由主循环 checkPendingTestRestore 设置）
    bool testWasConnected = s_mqttTestWasConnected;
    bool testJustRestored = s_mqttTestJustRestored.exchange(false);

    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    FastBee::setFixedJsonField(data, "enabled", mqttConfigLoaded ? mqttConfigEnabled : false);
    FastBee::setFixedJsonField(data, "connectionTestOnly", mqtt && mqtt->isConnectionTestOnly());
    FastBee::setFixedJsonField(data, "testRestorePending", s_mqttTestPendingRestore.load());
    FastBee::setFixedJsonField(data, "testRestoreBlocked", s_mqttTestRestoreBlocked.load());
    if (mqtt) {
        const auto queueSnapshot = mqtt->getReportQueueSnapshot();
        FastBee::addMqttReportQueueStatus(data, &queueSnapshot);
    } else {
        FastBee::addMqttReportQueueStatus(data, nullptr);
    }
    FastBee::setFixedJsonField(data, "autoStartAttempted", autoStartAttempted);
    FastBee::setFixedJsonField(data, "autoStartStarted", autoStartStarted);
    FastBee::setFixedJsonField(data, "stateAvailable", !lifecycleBusy);
    FastBee::setFixedJsonField(data, "lifecycleBusy", lifecycleBusy);
    if (mqtt) {
        FastBee::setFixedJsonField(data, "reconnectWorkerActive", mqtt->isReconnectWorkerActive());
        FastBee::setFixedJsonField(data, "reconnectCancellationRequested", mqtt->isReconnectCancellationRequested());
        FastBee::setFixedJsonField(data, "reconnectStopPending", mqtt->isReconnectCancellationRequested() && mqtt->isReconnectWorkerActive());
    }
    // Keep the status schema stable even before a valid MQTT configuration is
    // loaded. The UI and telemetry consumers must not branch on key presence.
    FastBee::setFixedJsonField(data, "autoReconnect", false);
    FastBee::setFixedJsonField(data, "reconnectCount", 0);
    FastBee::setFixedJsonField(data, "networkConnected", networkConnected);
    // Always expose this field so older Web bundles can safely branch on it.
    // It is set only from the persisted HealthMonitor marker, never merely
    // because the user selected enabled=false.
    FastBee::setFixedJsonField(data, "disabledByMemory", mqttDisabledByMemory);
    FastBee::setFixedJsonField(data, "stoppedByMemory", mqttStoppedByMemory);
    writeMqttDiagnostic(data, MqttDiagnostic::Ok, MqttDiagFields::Status);
    FastBee::setFixedJsonField(data, "tlsMemoryFailure", false);
    FastBee::setFixedJsonField(data, "statusReason", mqttConfigLoaded
        ? (mqttConfigEnabled ? "" : "MQTT 已禁用")
        : "MQTT 配置未加载");
    if (!mqttConfigLoaded) {
        writeMqttDiagnostic(data, MqttDiagnostic::ConfigInvalid, MqttDiagFields::Status);
    }

    // 测试连接结果：在restore前记录的连接状态
    if (s_mqttTestPendingRestore || testJustRestored) {
        FastBee::setFixedJsonField(data, "testConnected", testWasConnected);
        FastBee::setFixedJsonField(data, "testRestored", testJustRestored);
    }

    if (mqtt) {
        // 防御性检查：确保 MQTT 对象已完全初始化
        // 通过检查 server 和 port 是否有效来判断配置是否已加载
        MQTTConfig cfg = mqtt->getConfig();
        bool hasValidConfig = mqttHasValidRuntimeConfig(mqtt);

        // Both the persisted section and the in-memory client must be valid.
        // A previous client may still exist after a failed file reload; do not
        // expose it as initialized/connected in that case.
        if (hasValidConfig && mqttConfigLoaded) {
            FastBee::setFixedJsonField(data, "initialized", true);

            // MQTT连接状态：配置禁用时强制报告未连接，即使客户端对象仍存在
            // 防止 mqtt.enabled=false 时前端错误显示"已连接"
            bool mqttConnected = mqttConfigEnabled && mqtt->getIsConnected() && !mqtt->isStopped();
            FastBee::setFixedJsonField(data, "connected", mqttConnected);
            FastBee::setFixedJsonField(data, "connecting", mqttConfigEnabled && !mqttConnected && cfg.autoReconnect && !mqtt->isStopped());
            FastBee::setFixedJsonField(data, "stopped", mqtt->isStopped());
            FastBee::setFixedJsonField(data, "internetAvailable", internetAvailable);
            FastBee::setFixedJsonField(data, "networkConnected", networkConnected);
            FastBee::setFixedJsonField(data, "server", cfg.server);
            FastBee::setFixedJsonField(data, "port", cfg.port);
            FastBee::setFixedJsonField(data, "clientId", cfg.clientId);
            // A stale in-memory client can outlive a failed config reload.
            // Preserve the explicit configuration error instead of replacing
            // it with the old client's (often zero/OK) transport status.
            if (mqttConfigLoaded) {
                setMqttErrorFields(data, mqtt, networkConnected);
            }
            FastBee::setFixedJsonField(data, "reconnectCount", mqtt->getReconnectCount());
            FastBee::setFixedJsonField(data, "autoReconnect", cfg.autoReconnect);
            FastBee::setFixedJsonField(data, "scheme", cfg.scheme);
            FastBee::setFixedJsonField(data, "tlsAllocated", mqtt->isTlsTransportAllocated());
            FastBee::setFixedJsonField(data, "webPausedForMqtts", mqtt->isWebPausedForMqtts());
            const int lastError = mqtt->getLastErrorCode();
            const bool tlsMemoryFailure = mqtt->isLastMqttsTlsMemoryFailure();
            if (mqttConfigEnabled && !mqttConnected) {
                if (lastError != 0) {
                    FastBee::setFixedJsonField(data, "statusReason", mqttErrorMessage(lastError, networkConnected, tlsMemoryFailure));
                } else if (!networkConnected) {
                    FastBee::setFixedJsonField(data, "statusReason", "网络未连接");
                } else if (data["connecting"].as<bool>()) {
                    FastBee::setFixedJsonField(data, "statusReason", "正在连接 MQTT Broker");
                } else {
                    FastBee::setFixedJsonField(data, "statusReason", "MQTT 未连接");
                }
            }
            unsigned long connTime = mqtt->getLastConnectedTime();
            FastBee::setFixedJsonField(data, "lastConnectedMs", connTime > 0 ? (millis() - connTime) / 1000 : 0);
        } else {
            // MQTT 配置或客户端初始化无效。区分持久化配置错误和低内存/
            // 客户端重建失败，便于 Web 页面给出正确的处理建议。
            FastBee::setFixedJsonField(data, "initialized", false);
            FastBee::setFixedJsonField(data, "connected", false);
            FastBee::setFixedJsonField(data, "connecting", mqttConfigEnabled && autoStartAttempted);
            FastBee::setFixedJsonField(data, "internetAvailable", internetAvailable);
            FastBee::setFixedJsonField(data, "networkConnected", networkConnected);
            if (!configuredServer.isEmpty()) FastBee::setFixedJsonField(data, "server", configuredServer);
            if (configuredPort > 0) FastBee::setFixedJsonField(data, "port", configuredPort);
            if (!configuredClientId.isEmpty()) FastBee::setFixedJsonField(data, "clientId", configuredClientId);
            if (!mqttConfigLoaded) {
                FastBee::setFixedJsonField(data, "error", "MQTT config not loaded");
                writeMqttDiagnostic(data, MqttDiagnostic::ConfigInvalid,
                                    MqttDiagFields::Status | MqttDiagFields::Reason);
            } else if (!mqttConfigEnabled) {
                FastBee::setFixedJsonField(data, "error", "MQTT disabled");
                FastBee::setFixedJsonField(data, "lastErrorClass", "none");
                FastBee::setFixedJsonField(data, "statusReason", "MQTT 已禁用");
            } else {
                FastBee::setFixedJsonField(data, "error", "MQTT client not initialized");
                writeMqttDiagnostic(data, MqttDiagnostic::ClientUninitialized,
                                    MqttDiagFields::Status | MqttDiagFields::Reason);
            }
        }
    } else {
        FastBee::setFixedJsonField(data, "initialized", false);
        FastBee::setFixedJsonField(data, "connected", false);
        FastBee::setFixedJsonField(data, "connecting", mqttConfigEnabled && autoStartAttempted);
        FastBee::setFixedJsonField(data, "internetAvailable", internetAvailable);
        FastBee::setFixedJsonField(data, "networkConnected", networkConnected);
        if (!configuredServer.isEmpty()) FastBee::setFixedJsonField(data, "server", configuredServer);
        if (configuredPort > 0) FastBee::setFixedJsonField(data, "port", configuredPort);
        if (!configuredClientId.isEmpty()) FastBee::setFixedJsonField(data, "clientId", configuredClientId);
        if (!mqttConfigLoaded) {
            writeMqttDiagnostic(data, MqttDiagnostic::ConfigInvalid, MqttDiagFields::Status);
        } else if (mqttConfigEnabled) {
            writeMqttDiagnostic(data, MqttDiagnostic::ClientUninitialized, MqttDiagFields::Status);
        } else {
            FastBee::setFixedJsonField(data, "lastErrorClass", "none");
        }
        FastBee::setFixedJsonField(data, "statusReason", mqttConfigLoaded
            ? (mqttConfigEnabled ? "MQTT 客户端未初始化" : "MQTT 已禁用")
            : "MQTT 配置未加载");
    }

    // A deferred auto-start can fail before an MQTTClient object exists (most
    // commonly because the low-memory admission check rejected the rebuild).
    // Report that attempted failure explicitly; otherwise the branches above
    // look identical to a normal first boot and the UI says only “初始化中”.
    // Do this only for an actual attempt so rate-limited/non-enabled states do
    // not become false alarms.
    if (mqttConfigEnabled && autoStartAttempted && !autoStartStarted && !lifecycleBusy) {
        writeMqttDiagnostic(data, MqttDiagnostic::ClientStartFailed,
                            MqttDiagFields::Status | MqttDiagFields::Reason);
    }

    if (lifecycleBusy) {
        FastBee::setFixedJsonField(data, "initialized", false);
        FastBee::setFixedJsonField(data, "connected", false);
        FastBee::setFixedJsonField(data, "connecting", false);
        writeMqttDiagnostic(data, MqttDiagnostic::LifecycleBusy,
                            MqttDiagFields::Status | MqttDiagFields::Reason);
    }

    // HealthMonitor may have stopped the client and persisted enabled=false
    // before this request arrived. Override the generic "MQTT 已禁用"
    // diagnostics with a stable, actionable explanation while preserving the
    // disabled state itself.
    if (mqttDisabledByMemory) {
        FastBee::setFixedJsonField(data, "enabled", false);
        FastBee::setFixedJsonField(data, "connected", false);
        FastBee::setFixedJsonField(data, "connecting", false);
        FastBee::setFixedJsonField(data, "disabledByMemory", true);
        writeMqttDiagnostic(data, MqttDiagnostic::MemoryDisabled,
                            MqttDiagFields::Status | MqttDiagFields::Reason | MqttDiagFields::CopyMessage);
    } else if (mqttStoppedByMemory) {
        FastBee::setFixedJsonField(data, "connected", false);
        FastBee::setFixedJsonField(data, "connecting", false);
        FastBee::setFixedJsonField(data, "stoppedByMemory", true);
        writeMqttDiagnostic(data, MqttDiagnostic::MemoryStopped,
                            MqttDiagFields::Status | MqttDiagFields::Reason | MqttDiagFields::CopyMessage);
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
    if (!pm->mqttLifetimeAvailable()) {
        ctx->sendError(request, 503, "MQTT admission storage unavailable; restart required");
        return;
    }

    // 先读取持久化配置，再判断“已经连接”的快速路径。内存保护会把
    // mqtt.enabled 写为 false；如果先检查旧的 MQTTClient 指针，配置刚写入
    // 到客户端停止之间的窗口会错误地返回 connected=true，前端也就看不到
    // “内存自动禁用”的恢复建议。
    bool mqttAbsent = false;
    auto mqttLease = pm->acquireMQTTClient(&mqttAbsent);
    MQTTClient* mqtt = mqttLease.get();
    bool mqttEnabled = false;
    String dummyServer;
    int dummyPort = 0;
    String dummyClientId;
    bool mqttDisabledByMemory = false;
    bool mqttStoppedByMemory = false;
    const bool statusConfigLoaded = loadMqttStatusConfig(
        mqttEnabled, dummyServer, dummyPort, dummyClientId, &mqttDisabledByMemory);

    // Use the live memory-guard state as well as the persisted marker. This
    // closes the write/stop race and prevents a reconnect from rebuilding MQTT
    // while HealthMonitor is actively recovering the heap.
    FastBeeFramework* reconnectFw = FastBeeFramework::getInstance();
    HealthMonitor* reconnectHealth = reconnectFw ? reconnectFw->getHealthMonitor() : nullptr;
    if ((reconnectHealth && reconnectHealth->isMqttDisabledForMemory()) ||
        (mqtt && mqtt->isDisabledByMemory())) {
        mqttDisabledByMemory = true;
    }
    if (reconnectHealth && reconnectHealth->isMqttStoppedForMemory()) {
        mqttStoppedByMemory = true;
    }

    // 检查 MQTT 是否已启用：禁用时拒绝重连/重启
    // 前端自动重连逻辑可能在禁用状态下误调用此接口（如轮询时序竞态）
    // 后端必须兜底：禁用时不触发任何重连或设备重启
    {
        if (!statusConfigLoaded) {
            JsonDocument doc;
            doc["success"] = false;
            FastBee::setFixedJsonField(doc, "data", "connected", false);
            writeMqttDiagnosticData(doc, MqttDiagnostic::BrokerConfigInvalid,
                                MqttDiagFields::LastError | MqttDiagFields::Reason);
            HandlerUtils::sendJsonStream(request, doc);
            return;
        }
        if (mqttDisabledByMemory) {
            JsonDocument doc;
            doc["success"] = false;
            FastBee::setFixedJsonField(doc, "data", "connected", false);
            FastBee::setFixedJsonField(doc, "data", "reconnecting", false);
            FastBee::setFixedJsonField(doc, "data", "mqttDisabled", true);
            FastBee::setFixedJsonField(doc, "data", "disabledByMemory", true);
            writeMqttDiagnosticData(doc, MqttDiagnostic::MemoryDisabled,
                                MqttDiagFields::LastError | MqttDiagFields::Reason);
            HandlerUtils::sendJsonStream(request, doc);
            return;
        }
        if (mqttStoppedByMemory) {
            JsonDocument doc;
            doc["success"] = false;
            FastBee::setFixedJsonField(doc, "data", "connected", false);
            FastBee::setFixedJsonField(doc, "data", "reconnecting", false);
            FastBee::setFixedJsonField(doc, "data", "stoppedByMemory", true);
            writeMqttDiagnosticData(doc, MqttDiagnostic::MemoryStopped,
                                MqttDiagFields::LastError | MqttDiagFields::Reason);
            HandlerUtils::sendJsonStream(request, doc);
            return;
        }
        if (!mqttEnabled) {
            JsonDocument doc;
            doc["success"] = true;
            FastBee::setFixedJsonField(doc, "data", "connected", false);
            FastBee::setFixedJsonField(doc, "data", "mqttDisabled", true);
            FastBee::setFixedJsonField(doc, "data", "message", "MQTT is disabled, reconnect rejected");
            FastBee::setFixedJsonField(doc, "data", "statusReason", "MQTT 已禁用，请先在协议配置中启用");
            HandlerUtils::sendJsonStream(request, doc);
            return;
        }
    }

    if (!mqttLease && !mqttAbsent) {
        JsonDocument busy;
        busy["success"] = false;
        FastBee::setFixedJsonField(busy, "data", "stateAvailable", false);
        FastBee::setFixedJsonField(busy, "data", "lifecycleBusy", true);
        FastBee::setFixedJsonField(busy, "data", "statusReason", "MQTT 实例切换中，本次重连未重复启动");
        HandlerUtils::sendJsonStream(request, busy);
        return;
    }

    // 如果 MQTT 已连接，直接返回成功（无需重建）。必须放在配置保护之后，
    // 避免旧客户端对象掩盖用户禁用或内存保护状态。
    if (mqtt && mqtt->getIsConnected()) {
        JsonDocument doc;
        doc["success"] = true;
        FastBee::setFixedJsonField(doc, "data", "connected", true);
        FastBee::setFixedJsonField(doc, "data", "reconnecting", false);
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
            FastBee::setFixedJsonField(doc, "data", "connected", false);
            FastBee::setFixedJsonField(doc, "data", "autoReconnectDisabled", true);
            FastBee::setFixedJsonField(doc, "data", "statusReason", "自动重连已关闭，请在协议配置中开启");
            HandlerUtils::sendJsonStream(request, doc);
            return;
        }
    }

    mqttLease = {};
    mqtt = nullptr; // No runtime pointer may cross the following rebuild.

    // 读取 MQTT scheme 判断是否需要 TLS（MQTTS 重建会导致 DRAM 碎片化，需设备重启）
    bool isMqtts = false;
    {
        JsonDocument schemeDoc;
        if (ConfigStorage::getInstance().loadProtocolSection("mqtt", schemeDoc)) {
            String scheme = schemeDoc["mqtt"]["scheme"].as<String>();
            isMqtts = (scheme == "mqtts");
        }
    }

    if (isMqtts) {
        // MQTTS 场景：运行时 destroy/rebuild TLS 客户端会导致 DRAM 碎片化，
        // 后续 TLS 握手需要 ~42KB 连续内存，碎片化后无法分配。
        // 通过设备重启获得干净的堆状态，确保 MQTTS 握手成功。
        JsonDocument doc;
        doc["success"] = true;
        FastBee::setFixedJsonField(doc, "data", "connected", false);
        FastBee::setFixedJsonField(doc, "data", "rebooting", true);
        FastBee::setFixedJsonField(doc, "data", "message", "Device will reboot to reconnect MQTTS cleanly");
        FastBee::setFixedJsonField(doc, "data", "statusReason", "设备将重启以重新连接 MQTTS");

        HandlerUtils::sendJsonStream(request, doc);

        // HTTP 响应已发送，调度延迟重启
        SystemRebooter::scheduleConfigReboot("MQTT manual reconnect (MQTTS)");
        return;
    }

    // 非 TLS 场景：运行时销毁并重建 MQTT 客户端（约 6KB，碎片化风险低）
    // restartMQTTDeferred(true) 会 stop+reset 旧客户端，创建新客户端，
    // 由 MQTTClient::handle() 的 auto-reconnect 在 loop 中异步连接
    LOG_INFO("[MQTT Reconnect] Non-TLS: attempting runtime reconnect");
    FastBee::MqttRestartResult restartResult = FastBee::MqttRestartResult::NotStarted;
    bool deferredOk = pm->restartMQTTDeferred(true, &restartResult);
    const bool pending = restartResult == FastBee::MqttRestartResult::Deferred;

    JsonDocument doc;
    // The request itself is handled successfully, but the operation can fail
    // when the heap admission guard rejects rebuilding the client.  Returning
    // success=true in that case made generic API callers report a false
    // success even though the payload already said reconnecting=false.
    doc["success"] = deferredOk || pending;
    FastBee::setFixedJsonField(doc, "data", "connected", false);
    FastBee::setFixedJsonField(doc, "data", "reconnecting", deferredOk);
    FastBee::setFixedJsonField(doc, "data", "lifecyclePending", pending);
    FastBee::setFixedJsonField(doc, "data", "message", deferredOk
        ? "MQTT client rebuilt, reconnecting in background"
        : "MQTT rebuild did not complete; check configuration, transport and resources");
    if (pending) {
        FastBee::setFixedJsonField(doc, "data", "message", "MQTT rebuild queued; waiting for active operations");
        FastBee::setFixedJsonField(doc, "data", "statusReason", "MQTT 重建已排队，等待在途操作结束");
    } else if (!deferredOk) {
        FastBee::setFixedJsonField(doc, "data", "failureKnown", true);
        FastBee::setFixedJsonField(doc, "data", "retryable", true);
        writeMqttDiagnosticData(doc, MqttDiagnostic::RebuildNotCompleted,
                            MqttDiagFields::LastError | MqttDiagFields::Reason);
    } else {
        FastBee::setFixedJsonField(doc, "data", "statusReason", "MQTT 客户端已重建，正在后台连接");
    }

    HandlerUtils::sendJsonStream(request, doc);
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
    if (!pm->mqttLifetimeAvailable()) {
        ctx->sendError(request, 503, "MQTT admission storage unavailable; restart required");
        return;
    }

    bool absent = false;
    auto mqtt = pm->acquireMQTTClient(&absent);
    const bool previousStateKnown = mqtt || absent;
    bool wasConnected = mqtt && mqtt->getIsConnected();
    mqtt = {};
    const bool stopped = pm->stopMQTT();

    JsonDocument resp;
    resp["success"] = true;
    FastBee::setFixedJsonField(resp, "data", "disconnected", stopped);
    FastBee::setFixedJsonField(resp, "data", "lifecyclePending", !stopped);
    FastBee::setFixedJsonField(resp, "data", "previousStateKnown", previousStateKnown);
    if (previousStateKnown) FastBee::setFixedJsonField(resp, "data", "wasConnected", wasConnected);
    FastBee::setFixedJsonField(resp, "data", "statusReason", stopped ? "MQTT 已在本地停止" : "MQTT 停止已排队，等待在途操作结束");

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
    if (!pm->mqttLifetimeAvailable()) {
        ctx->sendError(request, 503, "MQTT admission storage unavailable; restart required");
        return;
    }

    bool absent = false;
    auto mqtt = pm->acquireMQTTClient(&absent);
    if (!mqtt || !mqtt->getIsConnected()) {
        JsonDocument resp;
        resp["success"] = false;
        FastBee::setFixedJsonField(resp, "data", "lifecycleBusy", !mqtt && !absent);
        resp["error"] = (!mqtt && !absent) ? "MQTT state unavailable during lifecycle change" : "MQTT not connected";
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
