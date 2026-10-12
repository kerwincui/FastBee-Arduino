#include "./network/handlers/DeviceRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "network/DeviceConfigValidation.h"
#include "systems/ConfigStorage.h"
#include "systems/LoggerSystem.h"
#include "utils/TimeUtils.h"
#include "utils/SystemTimeZone.h"
#include "network/SystemTimeStatus.h"
#include "network/FixedJsonField.h"
#include "core/FeatureFlags.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>

static const char* DEVICE_CONFIG_FILE = "/config/device.json";

DeviceRouteHandler::DeviceRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
    // 预分配 JSON body handler，避免 setupRoutes() 期间集中堆分配
    _deviceJsonHandler = new AsyncCallbackJsonWebHandler("/api/device/config",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            handleSaveDeviceConfigJson(request, json);
        });
    _deviceJsonHandler->setMethod(HTTP_POST | HTTP_PUT);
}

DeviceRouteHandler::~DeviceRouteHandler() {
    // handler 由 AsyncWebServer 管理生命周期，不在此 delete
}

void DeviceRouteHandler::setupRoutes(AsyncWebServer* server) {
    // Device config
    server->on("/api/device/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetDeviceConfig(request); });

    server->on("/api/device/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleSaveDeviceConfig(request); });

    // JSON body handler（构造函数中已预分配）
    server->addHandler(_deviceJsonHandler);

    server->on("/api/device/developer-mode", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleSetDeveloperMode(request); });

    // Device time
    server->on("/api/device/time", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->requireAuth(request)) return;
        JsonDocument doc;
            
        // 检查网络状态
        bool internetAvailable = false;
        if (ctx->networkManager) {
            FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
            netMgr->updateStatusInfo();
            NetworkStatusInfo netInfo = netMgr->getStatusInfo();
            internetAvailable = netInfo.internetAvailable;
        }
            
        FastBee::setFixedJsonField(doc, "success", true);
        FastBee::appendSystemTimeStatus(doc["data"].to<JsonObject>());
        FastBee::setFixedJsonField(doc, "data", "internetAvailable", internetAvailable);
        FastBee::setFixedJsonField(doc, "data", "uptime", millis());
        FastBee::setFixedJsonField(doc, "data", "timezone", "CST-8");
        if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
            File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
            if (f) {
                JsonDocument cfg;
                if (deserializeJson(cfg, f) == DeserializationError::Ok) {
                    if (cfg["timezone"].is<String>()) FastBee::setFixedJsonField(doc, "data", "timezone", cfg["timezone"].as<String>());
                }
                f.close();
            }
        }
        HandlerUtils::sendJsonStream(request, doc);
    });

    server->on("/api/device/time/sync", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->requireAuth(request)) return;
            
        // 检查网络状态
        bool internetAvailable = false;
        if (ctx->networkManager) {
            FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
            // 刷新底层网络状态，避免刚联网时使用旧的 internetAvailable=false 阻止手动同步。
            netMgr->updateStatusInfo();
            NetworkStatusInfo netInfo = netMgr->getStatusInfo();
            internetAvailable = netInfo.internetAvailable;
        }
            
        // 如果网络不可用，直接返回失败
        if (!internetAvailable) {
            JsonDocument doc;
            FastBee::setFixedJsonField(doc, "success", false);
            FastBee::setFixedJsonField(doc, "error", "No internet connection");
            FastBee::appendSystemTimeStatus(doc["data"].to<JsonObject>());
            FastBee::setFixedJsonField(doc, "data", "syncRequestSucceeded", false);
            FastBee::setFixedJsonField(doc, "data", "internetAvailable", false);
            FastBee::setFixedJsonField(doc, "data", "uptime", millis());
            HandlerUtils::sendJsonStream(request, doc);
            return;
        }
            
        String ntpServer1 = "cn.pool.ntp.org";
        String tz = "CST-8";
        if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
            File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
            if (f) {
                JsonDocument cfg;
                if (deserializeJson(cfg, f) == DeserializationError::Ok) {
                    if (cfg["ntpServer1"].is<String>()) ntpServer1 = cfg["ntpServer1"].as<String>();
                    if (cfg["timezone"].is<String>()) tz = cfg["timezone"].as<String>();
                }
                f.close();
            }
        }
        bool synced = false;
        if (ntpServer1.startsWith("http://") || ntpServer1.startsWith("https://")) {
            long long ts = 0;
            synced = FastBee::SystemTimeZone::setPosix(tz.c_str()) &&
                TimeUtils::syncNTPFromHTTPWithTimestamp(ntpServer1, ts, 5000);
        } else {
            FastBee::SystemTimeQuality::installSntpObserver();
            const uint32_t beforeSync = FastBee::SystemTimeQuality::sequence();
            synced = FastBee::SystemTimeZone::startSntp(tz.c_str(), ntpServer1.c_str(), "time.nist.gov") &&
                TimeUtils::syncNTP(5000, beforeSync);
        }
        JsonDocument doc;
        FastBee::setFixedJsonField(doc, "success", true);
        FastBee::appendSystemTimeStatus(doc["data"].to<JsonObject>());
        FastBee::setFixedJsonField(doc, "data", "syncRequestSucceeded", synced);
        FastBee::setFixedJsonField(doc, "data", "internetAvailable", internetAvailable);
        FastBee::setFixedJsonField(doc, "data", "uptime", millis());
        if (synced) {
            LOGGER.info("NTP sync triggered via web");
        }
        HandlerUtils::sendJsonStream(request, doc);
    });

    // Device info
    server->on("/api/device/info", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetDeviceInfo(request); });
}

void DeviceRouteHandler::handleSaveDeviceConfigJson(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!ctx->requireAuth(request)) return;
    JsonObject obj = json.as<JsonObject>();
    if (const char* error = FastBee::validateDeviceConfigPatch(obj)) {
        ctx->sendError(request, 400, error);
        return;
    }
    JsonDocument doc;
    // Read existing config first
    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        if (!ConfigStorage::getInstance().loadJSONConfig(DEVICE_CONFIG_FILE, doc) || !doc.is<JsonObject>()) {
            ctx->sendError(request, 500, "Existing device configuration is unreadable");
            return;
        }
    }
    // Update fields from JSON body
    if (obj["deviceName"].is<String>()) FastBee::setFixedJsonField(doc, "deviceName", obj["deviceName"].as<String>());
    if (obj["deviceId"].is<String>()) {
        String devId = obj["deviceId"].as<String>();
        devId.trim();
        if (devId.isEmpty()) {
            // 用户在 Web 页面主动清空 deviceId：按默认规则 FBE+MAC 重新生成并覆盖原值
            // （不再保留原文件中的旧 deviceId，前端清空即表示要求重置）
            String mac = WiFi.macAddress();
            mac.replace(":", "");
            if (mac.length() < 12) {
                // MAC 尚不可用时，兜底使用 efuseMac 派生的 12 位十六进制
                uint64_t chipId = ESP.getEfuseMac();
                char macBuf[13] = {0};
                snprintf(macBuf, sizeof(macBuf), "%04X%08X",
                         (uint16_t)(chipId >> 32), (uint32_t)chipId);
                mac = String(macBuf);
            }
            devId = "FBE" + mac;
            LOGGER.infof("[DeviceConfig] deviceId empty in request, regenerated by default rule: %s",
                         devId.c_str());
        }
        FastBee::setFixedJsonField(doc, "deviceId", devId);
    }
    if (obj["productNumber"].is<int>() || obj["productNumber"].is<String>()) FastBee::setFixedJsonField(doc, "productNumber", obj["productNumber"].as<int>());
    if (obj["userId"].is<String>()) FastBee::setFixedJsonField(doc, "userId", obj["userId"].as<String>());
    if (obj["description"].is<String>()) FastBee::setFixedJsonField(doc, "description", obj["description"].as<String>());
    if (obj["ntpServer1"].is<String>()) FastBee::setFixedJsonField(doc, "ntpServer1", obj["ntpServer1"].as<String>());
    if (obj["ntpServer2"].is<String>()) FastBee::setFixedJsonField(doc, "ntpServer2", obj["ntpServer2"].as<String>());
    if (obj["timezone"].is<String>()) FastBee::setFixedJsonField(doc, "timezone", obj["timezone"].as<String>());
    if (obj["enableNTP"].is<String>() || obj["enableNTP"].is<bool>()) {
        String v = obj["enableNTP"].as<String>();
        FastBee::setFixedJsonField(doc, "enableNTP", (v == "1" || v == "true"));
    }
    if (obj["syncInterval"].is<String>() || obj["syncInterval"].is<int>()) FastBee::setFixedJsonField(doc, "syncInterval", obj["syncInterval"].as<int>());
    if (obj["logLevel"].is<String>()) {
        String lv = obj["logLevel"].as<String>();
        FastBee::setFixedJsonField(doc, "logLevel", lv);
    }
    if (obj["cacheDuration"].is<int>() || obj["cacheDuration"].is<String>()) {
        int cd = obj["cacheDuration"].as<int>();
        FastBee::setFixedJsonField(doc, "cacheDuration", cd);
    }
    if (!doc["developerModeEnabled"].is<bool>()) {
        FastBee::setFixedJsonField(doc, "developerModeEnabled", false);
    }

    // 安全策略字段：转发到 UserManager 持久化到 users.json
    const bool hasSecurityFields = !obj["maxLoginAttempts"].isNull() || !obj["loginLockoutTime"].isNull() ||
        !obj["minPasswordLength"].isNull() || !obj["requireStrongPasswords"].isNull();
    uint8_t  sMaxAttempts  = 5;
    uint32_t sLockoutTime  = 300000UL;
    uint8_t  sMinPwdLen    = 6;
    bool     sRequireStrong = false;
    // 先读取 users.json 当前值作为默认值
    if (hasSecurityFields && LittleFS.exists("/config/users.json")) {
        JsonDocument usersDoc;
        if (!ConfigStorage::getInstance().loadJSONConfig("/config/users.json", usersDoc)) {
            ctx->sendError(request, 500, "Existing security policy is unreadable");
            return;
        }
        {
                JsonObject sec = usersDoc["security"];
                if (!sec.isNull()) {
                    sMaxAttempts   = sec["maxLoginAttempts"] | 5;
                    sLockoutTime   = sec["loginLockoutTime"] | 300000UL;
                    sMinPwdLen     = sec["minPasswordLength"] | 6;
                    sRequireStrong = sec["requireStrongPasswords"] | false;
                }
        }
    }
    if (!obj["maxLoginAttempts"].isNull()) sMaxAttempts = obj["maxLoginAttempts"].as<uint8_t>();
    if (!obj["loginLockoutTime"].isNull()) sLockoutTime = obj["loginLockoutTime"].as<uint32_t>();
    if (!obj["minPasswordLength"].isNull()) sMinPwdLen = obj["minPasswordLength"].as<uint8_t>();
    if (!obj["requireStrongPasswords"].isNull()) {
        const String value = obj["requireStrongPasswords"].as<String>();
        sRequireStrong = value == "true" || value == "1";
    }
    if (hasSecurityFields) {
        if (!ctx->userManager || !ctx->userManager->updatePasswordPolicy(sMaxAttempts, sLockoutTime, sMinPwdLen, sRequireStrong)) {
            ctx->sendError(request, 500, "Failed to save security policy");
            return;
        }
        ctx->sendSuccess(request, "Security policy saved");
        return;
    }

    if (!ConfigStorage::getInstance().saveJSONConfig(DEVICE_CONFIG_FILE, doc)) {
        ctx->sendError(request, 500, "Failed to save device config");
        return;
    }
    // Publish runtime side effects only after the verified atomic commit.
    if (!obj["cacheDuration"].isNull()) ctx->cacheDuration = doc["cacheDuration"].as<uint32_t>();
    if (!obj["logLevel"].isNull()) {
        const String lv = doc["logLevel"].as<String>();
        LOGGER.setLogLevel(lv == "DEBUG" ? LOG_DEBUG : lv == "WARNING" ? LOG_WARNING : lv == "ERROR" ? LOG_ERROR : LOG_INFO);
    }
    LOGGER.info("Device configuration updated via web");
    
    // 构建响应，包含生成的 deviceId
    JsonDocument resp;
    FastBee::setFixedJsonField(resp, "success", true);
    FastBee::setFixedJsonField(resp, "message", "Device configuration saved");
    FastBee::setFixedJsonField(resp, "data", "deviceId", doc["deviceId"]);
    HandlerUtils::sendJsonStream(request, resp);
}

void DeviceRouteHandler::handleGetDeviceConfig(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            JsonDocument fileCfg;
            DeserializationError err = deserializeJson(fileCfg, f);
            f.close();
            if (!err) {
                if (!fileCfg["developerModeEnabled"].is<bool>()) {
                    FastBee::setFixedJsonField(fileCfg, "developerModeEnabled", false);
                }
                // 从 users.json 读取安全策略配置（4个UI可配置字段）
                if (LittleFS.exists("/config/users.json")) {
                    File uf = LittleFS.open("/config/users.json", "r");
                    if (uf) {
                        JsonDocument usersDoc;
                        if (deserializeJson(usersDoc, uf) == DeserializationError::Ok) {
                            JsonObject sec = usersDoc["security"];
                            if (!sec.isNull()) {
                                FastBee::setFixedJsonField(fileCfg, "maxLoginAttempts", sec["maxLoginAttempts"] | 5);
                                FastBee::setFixedJsonField(fileCfg, "loginLockoutTime", sec["loginLockoutTime"] | 300000UL);
                                FastBee::setFixedJsonField(fileCfg, "minPasswordLength", sec["minPasswordLength"] | 6);
                                FastBee::setFixedJsonField(fileCfg, "requireStrongPasswords", sec["requireStrongPasswords"] | false);
                            }
                        }
                        uf.close();
                    }
                }
                JsonDocument doc;
                FastBee::setFixedJsonField(doc, "success", true);
                FastBee::setFixedJsonField(doc, "data", fileCfg);
                HandlerUtils::sendJsonStream(request, doc);
                return;
            }
        }
    }

    JsonDocument doc;
    FastBee::setFixedJsonField(doc, "success", true);
    FastBee::setFixedJsonField(doc, "data", "deviceName", "FastBee-ESP32");
    FastBee::setFixedJsonField(doc, "data", "deviceId", String((uint32_t)ESP.getEfuseMac(), HEX));
    FastBee::setFixedJsonField(doc, "data", "developerModeEnabled", false);
    FastBee::setFixedJsonField(doc, "data", "logLevel", "INFO");
    FastBee::setFixedJsonField(doc, "data", "enableNTP", true);
    FastBee::setFixedJsonField(doc, "data", "ntpServer1", "http://iot.fastbee.cn/prod-api/iot/tool/ntp");
    FastBee::setFixedJsonField(doc, "data", "ntpServer2", "cn.pool.ntp.org");
    FastBee::setFixedJsonField(doc, "data", "timezone", "CST-8");
    FastBee::setFixedJsonField(doc, "data", "syncInterval", 3600);
    FastBee::setFixedJsonField(doc, "data", "maxLoginAttempts", 5);
    FastBee::setFixedJsonField(doc, "data", "loginLockoutTime", 300000);
    FastBee::setFixedJsonField(doc, "data", "minPasswordLength", 6);
    FastBee::setFixedJsonField(doc, "data", "requireStrongPasswords", false);
    HandlerUtils::sendJsonStream(request, doc);
}

void DeviceRouteHandler::handleSetDeveloperMode(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    String password = ctx->getParamValue(request, "password", "");
    if (!ctx->verifyCurrentUserPassword(request, password)) {
        ctx->sendError(request, 403, "Password verification failed");
        return;
    }

    bool enabled = ctx->getParamBool(request, "enabled", true);
    JsonDocument doc;
    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            deserializeJson(doc, f);
            f.close();
        }
    }

    if (!doc["deviceName"].is<String>()) FastBee::setFixedJsonField(doc, "deviceName", "FastBee-ESP32");
    if (!doc["deviceId"].is<String>()) FastBee::setFixedJsonField(doc, "deviceId", String((uint32_t)ESP.getEfuseMac(), HEX));
    FastBee::setFixedJsonField(doc, "developerModeEnabled", enabled);

    File f = LittleFS.open(DEVICE_CONFIG_FILE, "w");
    if (!f) {
        ctx->sendError(request, 500, "Failed to save device config");
        return;
    }
    serializeJsonPretty(doc, f);
    f.close();

    // 开发环境模式变更后失效缓存，下次请求重新读取
    ctx->devModeCacheValid = false;

    JsonDocument resp;
    FastBee::setFixedJsonField(resp, "success", true);
    FastBee::setFixedJsonField(resp, "message", enabled ? "Developer mode enabled" : "Developer mode disabled");
    FastBee::setFixedJsonField(resp, "data", "developerModeEnabled", enabled);
    HandlerUtils::sendJsonStream(request, resp);
}

void DeviceRouteHandler::handleSaveDeviceConfig(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    JsonDocument patch;
    const char* fields[] = { "deviceName", "deviceId", "productNumber", "userId", "description",
        "ntpServer1", "ntpServer2", "timezone", "enableNTP", "syncInterval", "logLevel", "cacheDuration",
        "maxLoginAttempts", "loginLockoutTime", "minPasswordLength", "requireStrongPasswords" };
    for (const char* field : fields) {
        if (request->hasParam(field, true) || request->hasParam(field)) patch[field] = ctx->getParamValue(request, field, "");
    }
    JsonVariant body = patch.as<JsonVariant>();
    handleSaveDeviceConfigJson(request, body);
}

void DeviceRouteHandler::handleGetDeviceInfo(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    JsonDocument doc;

    // ESP32 芯片信息
    FastBee::setFixedJsonField(doc, "data", "chip", "model", ESP.getChipModel());
    FastBee::setFixedJsonField(doc, "data", "chip", "revision", ESP.getChipRevision());
    FastBee::setFixedJsonField(doc, "data", "chip", "cpuFreqMHz", ESP.getCpuFreqMHz());
    FastBee::setFixedJsonField(doc, "data", "chip", "sdkVersion", ESP.getSdkVersion());
    FastBee::setFixedJsonField(doc, "data", "chip", "macAddress", WiFi.macAddress());

    // 从 device.json 读取设备配置
    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            JsonDocument cfg;
            if (deserializeJson(cfg, f) == DeserializationError::Ok) {
                if (cfg["deviceName"].is<String>()) FastBee::setFixedJsonField(doc, "data", "deviceName", cfg["deviceName"].as<String>());
                if (cfg["deviceId"].is<String>()) FastBee::setFixedJsonField(doc, "data", "deviceId", cfg["deviceId"].as<String>());
                if (cfg["description"].is<String>()) FastBee::setFixedJsonField(doc, "data", "description", cfg["description"].as<String>());
                if (cfg["productNumber"].is<int>()) FastBee::setFixedJsonField(doc, "data", "productNumber", cfg["productNumber"].as<int>());
                if (cfg["userId"].is<String>()) FastBee::setFixedJsonField(doc, "data", "userId", cfg["userId"].as<String>());
                if (cfg["location"].is<String>()) FastBee::setFixedJsonField(doc, "data", "location", cfg["location"].as<String>());
            }
            f.close();
        }
    }

    // 设置默认值
    if (!doc["data"]["deviceName"].is<String>()) {
        FastBee::setFixedJsonField(doc, "data", "deviceName", "FastBee-ESP32");
    }
    if (!doc["data"]["deviceId"].is<String>()) {
        FastBee::setFixedJsonField(doc, "data", "deviceId", String((uint32_t)ESP.getEfuseMac(), HEX));
    }

    FastBee::setFixedJsonField(doc, "success", true);
    HandlerUtils::sendJsonStream(request, doc);
}
