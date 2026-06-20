#include "./network/handlers/DeviceRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "systems/LoggerSystem.h"
#include "utils/TimeUtils.h"
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
        time_t now = TimeUtils::getTimestamp();
            
        // 检查网络状态
        bool internetAvailable = false;
        if (ctx->networkManager) {
            FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
            NetworkStatusInfo netInfo = netMgr->getStatusInfo();
            internetAvailable = netInfo.internetAvailable;
        }
            
        // 时间有效：时间戳大于 2020-01-01 (1577836800)
        bool timeValid = (now > 1577836800);
        bool synced = timeValid && internetAvailable;
            
        doc["success"] = true;
        doc["data"]["datetime"] = TimeUtils::formatTime(now, TimeUtils::HUMAN_READABLE);
        doc["data"]["timestamp"] = (long)now;
        doc["data"]["synced"] = synced;
        doc["data"]["timeValid"] = timeValid;
        doc["data"]["internetAvailable"] = internetAvailable;
        doc["data"]["uptime"] = millis();
        doc["data"]["timezone"] = "CST-8";
        if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
            File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
            if (f) {
                JsonDocument cfg;
                if (deserializeJson(cfg, f) == DeserializationError::Ok) {
                    if (cfg["timezone"].is<String>()) doc["data"]["timezone"] = cfg["timezone"].as<String>();
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
            NetworkStatusInfo netInfo = netMgr->getStatusInfo();
            internetAvailable = netInfo.internetAvailable;
        }
            
        // 如果网络不可用，直接返回失败
        if (!internetAvailable) {
            JsonDocument doc;
            time_t now = TimeUtils::getTimestamp();
            doc["success"] = false;
            doc["error"] = "No internet connection";
            doc["data"]["datetime"] = TimeUtils::formatTime(now, TimeUtils::HUMAN_READABLE);
            doc["data"]["timestamp"] = (long)now;
            doc["data"]["synced"] = false;
            doc["data"]["internetAvailable"] = false;
            doc["data"]["uptime"] = millis();
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
        setenv("TZ", tz.c_str(), 1);
        tzset();
        bool synced = false;
        if (ntpServer1.startsWith("http://") || ntpServer1.startsWith("https://")) {
            long long ts = 0;
            synced = TimeUtils::syncNTPFromHTTPWithTimestamp(ntpServer1, ts, 5000);
            if (synced) tzset();
        } else {
            configTzTime(tz.c_str(), ntpServer1.c_str(), "time.nist.gov");
            synced = TimeUtils::syncNTP(5000);
        }
        JsonDocument doc;
        time_t now = TimeUtils::getTimestamp();
        doc["success"] = true;
        doc["data"]["datetime"] = TimeUtils::formatTime(now, TimeUtils::HUMAN_READABLE);
        doc["data"]["timestamp"] = (long)now;
        doc["data"]["synced"] = synced;
        doc["data"]["internetAvailable"] = internetAvailable;
        doc["data"]["uptime"] = millis();
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
    JsonDocument doc;
    // Read existing config first
    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) { deserializeJson(doc, f); f.close(); }
    }
    // Update fields from JSON body
    if (obj["deviceName"].is<String>()) doc["deviceName"] = obj["deviceName"].as<String>();
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
        doc["deviceId"] = devId;
    }
    if (obj["productNumber"].is<int>() || obj["productNumber"].is<String>()) doc["productNumber"] = obj["productNumber"].as<int>();
    if (obj["userId"].is<String>()) doc["userId"] = obj["userId"].as<String>();
    if (obj["description"].is<String>()) doc["description"] = obj["description"].as<String>();
    if (obj["ntpServer1"].is<String>()) doc["ntpServer1"] = obj["ntpServer1"].as<String>();
    if (obj["ntpServer2"].is<String>()) doc["ntpServer2"] = obj["ntpServer2"].as<String>();
    if (obj["timezone"].is<String>()) doc["timezone"] = obj["timezone"].as<String>();
    if (obj["enableNTP"].is<String>() || obj["enableNTP"].is<bool>()) {
        String v = obj["enableNTP"].as<String>();
        doc["enableNTP"] = (v == "1" || v == "true" || obj["enableNTP"].as<bool>());
    }
    if (obj["syncInterval"].is<String>() || obj["syncInterval"].is<int>()) doc["syncInterval"] = obj["syncInterval"].as<int>();
    if (obj["logLevel"].is<String>()) {
        String lv = obj["logLevel"].as<String>();
        // 即时应用日志级别
        LogLevel level = LOG_INFO;
        if      (lv == "DEBUG")   level = LOG_DEBUG;
        else if (lv == "INFO")    level = LOG_INFO;
        else if (lv == "WARNING") level = LOG_WARNING;
        else if (lv == "ERROR")   level = LOG_ERROR;
        LOGGER.setLogLevel(level);
        doc["logLevel"] = lv;
    }
    if (obj["cacheDuration"].is<int>() || obj["cacheDuration"].is<String>()) {
        int cd = obj["cacheDuration"].as<int>();
        doc["cacheDuration"] = cd;
        ctx->cacheDuration = (uint32_t)cd;
    }
    if (!doc["developerModeEnabled"].is<bool>()) {
        doc["developerModeEnabled"] = true;
    }

    // 安全策略字段：转发到 UserManager 持久化到 users.json
    bool hasSecurityFields = false;
    uint8_t  sMaxAttempts  = 5;
    uint32_t sLockoutTime  = 300000UL;
    uint8_t  sMinPwdLen    = 6;
    bool     sRequireStrong = false;
    // 先读取 users.json 当前值作为默认值
    if (LittleFS.exists("/config/users.json")) {
        File uf = LittleFS.open("/config/users.json", "r");
        if (uf) {
            JsonDocument usersDoc;
            if (deserializeJson(usersDoc, uf) == DeserializationError::Ok) {
                JsonObject sec = usersDoc["security"];
                if (!sec.isNull()) {
                    sMaxAttempts   = sec["maxLoginAttempts"] | 5;
                    sLockoutTime   = sec["loginLockoutTime"] | 300000UL;
                    sMinPwdLen     = sec["minPasswordLength"] | 6;
                    sRequireStrong = sec["requireStrongPasswords"] | false;
                }
            }
            uf.close();
        }
    }
    if (obj["maxLoginAttempts"].is<int>())  { sMaxAttempts  = (uint8_t)obj["maxLoginAttempts"].as<int>();  hasSecurityFields = true; }
    if (obj["loginLockoutTime"].is<int>() || obj["loginLockoutTime"].is<long>()) {
        sLockoutTime = (uint32_t)obj["loginLockoutTime"].as<long>(); hasSecurityFields = true;
    }
    if (obj["minPasswordLength"].is<int>()) { sMinPwdLen    = (uint8_t)obj["minPasswordLength"].as<int>(); hasSecurityFields = true; }
    if (obj["requireStrongPasswords"].is<bool>()) { sRequireStrong = obj["requireStrongPasswords"].as<bool>(); hasSecurityFields = true; }
    if (hasSecurityFields && ctx->userManager) {
        ctx->userManager->updatePasswordPolicy(sMaxAttempts, sLockoutTime, sMinPwdLen, sRequireStrong);
    }

    File f = LittleFS.open(DEVICE_CONFIG_FILE, "w");
    if (!f) { ctx->sendError(request, 500, "Failed to save device config"); return; }
    serializeJsonPretty(doc, f);
    f.close();
    LOGGER.info("Device configuration updated via web");
    
    // 构建响应，包含生成的 deviceId
    JsonDocument resp;
    resp["success"] = true;
    resp["message"] = "Device configuration saved";
    resp["data"]["deviceId"] = doc["deviceId"];
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
                    fileCfg["developerModeEnabled"] = true;
                }
                // 从 users.json 读取安全策略配置（4个UI可配置字段）
                if (LittleFS.exists("/config/users.json")) {
                    File uf = LittleFS.open("/config/users.json", "r");
                    if (uf) {
                        JsonDocument usersDoc;
                        if (deserializeJson(usersDoc, uf) == DeserializationError::Ok) {
                            JsonObject sec = usersDoc["security"];
                            if (!sec.isNull()) {
                                fileCfg["maxLoginAttempts"]       = sec["maxLoginAttempts"] | 5;
                                fileCfg["loginLockoutTime"]       = sec["loginLockoutTime"] | 300000UL;
                                fileCfg["minPasswordLength"]      = sec["minPasswordLength"] | 6;
                                fileCfg["requireStrongPasswords"] = sec["requireStrongPasswords"] | false;
                            }
                        }
                        uf.close();
                    }
                }
                JsonDocument doc;
                doc["success"] = true;
                doc["data"] = fileCfg;
                HandlerUtils::sendJsonStream(request, doc);
                return;
            }
        }
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["data"]["deviceName"] = "FastBee-ESP32";
    doc["data"]["deviceId"] = String((uint32_t)ESP.getEfuseMac(), HEX);
    doc["data"]["developerModeEnabled"] = true;
    doc["data"]["logLevel"] = "INFO";
    doc["data"]["syncInterval"] = 3600;
    doc["data"]["maxLoginAttempts"]       = 5;
    doc["data"]["loginLockoutTime"]       = 300000;
    doc["data"]["minPasswordLength"]      = 6;
    doc["data"]["requireStrongPasswords"] = false;
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

    if (!doc["deviceName"].is<String>()) doc["deviceName"] = "FastBee-ESP32";
    if (!doc["deviceId"].is<String>()) doc["deviceId"] = String((uint32_t)ESP.getEfuseMac(), HEX);
    doc["developerModeEnabled"] = enabled;

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
    resp["success"] = true;
    resp["message"] = enabled ? "Developer mode enabled" : "Developer mode disabled";
    resp["data"]["developerModeEnabled"] = enabled;
    HandlerUtils::sendJsonStream(request, resp);
}

void DeviceRouteHandler::handleSaveDeviceConfig(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    JsonDocument doc;
    doc["deviceName"] = ctx->getParamValue(request, "deviceName", "FastBee-ESP32");
    
    String devId = ctx->getParamValue(request, "deviceId", "");
    devId.trim();
    if (devId.isEmpty()) {
        String mac = WiFi.macAddress();
        mac.replace(":", "");
        devId = "FBE" + mac;
    }
    doc["deviceId"] = devId;
    doc["developerModeEnabled"] = ctx->isDeveloperModeEnabled();

    File f = LittleFS.open(DEVICE_CONFIG_FILE, "w");
    if (!f) {
        ctx->sendError(request, 500, "Failed to save device config");
        return;
    }

    serializeJsonPretty(doc, f);
    f.close();

    ctx->sendSuccess(request, "Device configuration saved");
}

void DeviceRouteHandler::handleGetDeviceInfo(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    JsonDocument doc;

    // ESP32 芯片信息
    doc["data"]["chip"]["model"] = ESP.getChipModel();
    doc["data"]["chip"]["revision"] = ESP.getChipRevision();
    doc["data"]["chip"]["cpuFreqMHz"] = ESP.getCpuFreqMHz();
    doc["data"]["chip"]["sdkVersion"] = ESP.getSdkVersion();
    doc["data"]["chip"]["macAddress"] = WiFi.macAddress();

    // 从 device.json 读取设备配置
    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            JsonDocument cfg;
            if (deserializeJson(cfg, f) == DeserializationError::Ok) {
                if (cfg["deviceName"].is<String>()) doc["data"]["deviceName"] = cfg["deviceName"].as<String>();
                if (cfg["deviceId"].is<String>()) doc["data"]["deviceId"] = cfg["deviceId"].as<String>();
                if (cfg["description"].is<String>()) doc["data"]["description"] = cfg["description"].as<String>();
                if (cfg["productNumber"].is<int>()) doc["data"]["productNumber"] = cfg["productNumber"].as<int>();
                if (cfg["userId"].is<String>()) doc["data"]["userId"] = cfg["userId"].as<String>();
                if (cfg["location"].is<String>()) doc["data"]["location"] = cfg["location"].as<String>();
            }
            f.close();
        }
    }

    // 设置默认值
    if (!doc["data"]["deviceName"].is<String>()) {
        doc["data"]["deviceName"] = "FastBee-ESP32";
    }
    if (!doc["data"]["deviceId"].is<String>()) {
        doc["data"]["deviceId"] = String((uint32_t)ESP.getEfuseMac(), HEX);
    }

    doc["success"] = true;
    HandlerUtils::sendJsonStream(request, doc);
}
