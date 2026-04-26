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

    // Device time
    server->on("/api/device/time", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "system.view")) { ctx->sendUnauthorized(request); return; }
        JsonDocument doc;
        time_t now = TimeUtils::getTimestamp();
            
        // 检查网络状态
        bool internetAvailable = false;
        if (ctx->networkManager) {
            NetworkManager* netMgr = static_cast<NetworkManager*>(ctx->networkManager);
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
        if (!ctx->checkPermission(request, "config.edit")) { ctx->sendUnauthorized(request); return; }
            
        // 检查网络状态
        bool internetAvailable = false;
        if (ctx->networkManager) {
            NetworkManager* netMgr = static_cast<NetworkManager*>(ctx->networkManager);
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
        if (synced) { LOGGER.info("NTP sync triggered via web"); }
        HandlerUtils::sendJsonStream(request, doc);
    });

    // Device info
    server->on("/api/device/info", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetDeviceInfo(request); });
}

void DeviceRouteHandler::handleSaveDeviceConfigJson(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
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
            String mac = WiFi.macAddress();
            mac.replace(":", "");
            devId = "FBE" + mac;
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
    if (obj["cacheDuration"].is<int>() || obj["cacheDuration"].is<String>()) {
        int cd = obj["cacheDuration"].as<int>();
        doc["cacheDuration"] = cd;
        ctx->cacheDuration = (uint32_t)cd;
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
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            JsonDocument fileCfg;
            DeserializationError err = deserializeJson(fileCfg, f);
            f.close();
            if (!err) {
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
    HandlerUtils::sendJsonStream(request, doc);
}

void DeviceRouteHandler::handleSaveDeviceConfig(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

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
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

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
