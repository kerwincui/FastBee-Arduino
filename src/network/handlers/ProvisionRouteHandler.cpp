#include "./network/handlers/ProvisionRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "./systems/LoggerSystem.h"
#include "./core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "./core/PeriphExecManager.h"
#endif
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>

// 统一使用 device.json 存储配网配置
static const char* DEVICE_CONFIG_FILE = "/config/device.json";
static const char* OLD_PROVISION_CONFIG_FILE = "/config/provision.json";
static const char* OLD_BLE_PROVISION_CONFIG_FILE = "/config/ble_provision.json";
static bool _apProvisionActive = false;
static bool _bleProvisionActive = false;
static unsigned long _bleProvisionStartTime = 0;

// 清理旧的独立配置文件（已迁移到 device.json）
static void cleanupOldConfigFiles() {
    if (LittleFS.exists(OLD_PROVISION_CONFIG_FILE)) {
        LittleFS.remove(OLD_PROVISION_CONFIG_FILE);
    }
    if (LittleFS.exists(OLD_BLE_PROVISION_CONFIG_FILE)) {
        LittleFS.remove(OLD_BLE_PROVISION_CONFIG_FILE);
    }
}

ProvisionRouteHandler::ProvisionRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
    // 清理旧的配置文件
    cleanupOldConfigFiles();
}

void ProvisionRouteHandler::setupRoutes(AsyncWebServer* server) {
    server->on("/setup", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleSetupPage(request); });

    server->on("/api/wifi/scan", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleWiFiScan(request); });

    server->on("/api/wifi/connect", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleWiFiConnect(request); });

    // ============ AP Provision Routes ============

    server->on("/api/provision/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "network.view")) { ctx->sendUnauthorized(request); return; }
        JsonDocument doc;
        doc["success"] = true;
        doc["data"]["active"] = _apProvisionActive;
        doc["data"]["apSSID"] = WiFi.softAPSSID();
        doc["data"]["clients"] = WiFi.softAPgetStationNum();
        HandlerUtils::sendJsonStream(request, doc);
    });

    server->on("/api/provision/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "network.view")) { ctx->sendUnauthorized(request); return; }
        JsonDocument doc;
        doc["success"] = true;
        if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
            File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
            if (f) {
                JsonDocument cfg;
                if (deserializeJson(cfg, f) == DeserializationError::Ok) {
                    // 从 device.json 读取 AP 配网配置
                    doc["data"]["provisionSSID"] = cfg["provisionSSID"] | "";
                    doc["data"]["provisionPassword"] = cfg["provisionPassword"] | "";
                    doc["data"]["provisionTimeout"] = cfg["provisionTimeout"] | 300;
                    doc["data"]["provisionIP"] = cfg["provisionIP"] | "192.168.4.1";
                    doc["data"]["provisionGateway"] = cfg["provisionGateway"] | "192.168.4.1";
                    doc["data"]["provisionSubnet"] = cfg["provisionSubnet"] | "255.255.255.0";
                    doc["data"]["provisionUserId"] = cfg["provisionUserId"] | "";
                    doc["data"]["provisionProductId"] = cfg["provisionProductId"] | "";
                    doc["data"]["provisionAuthCode"] = cfg["provisionAuthCode"] | "";
                }
                f.close();
            }
        }
        if (doc["data"].isNull()) {
            doc["data"]["provisionSSID"] = "";
            doc["data"]["provisionPassword"] = "";
            doc["data"]["provisionTimeout"] = 300;
            doc["data"]["provisionIP"] = "192.168.4.1";
            doc["data"]["provisionGateway"] = "192.168.4.1";
            doc["data"]["provisionSubnet"] = "255.255.255.0";
        }
        HandlerUtils::sendJsonStream(request, doc);
    });

    server->on("/api/provision/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "network.edit")) { ctx->sendUnauthorized(request); return; }
        // 读取现有 device.json
        JsonDocument doc;
        if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
            File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
            if (f) { deserializeJson(doc, f); f.close(); }
        }
        // 更新 AP 配网字段
        if (request->hasParam("provisionSSID", true)) doc["provisionSSID"] = request->getParam("provisionSSID", true)->value();
        if (request->hasParam("provisionPassword", true)) doc["provisionPassword"] = request->getParam("provisionPassword", true)->value();
        if (request->hasParam("provisionTimeout", true)) doc["provisionTimeout"] = request->getParam("provisionTimeout", true)->value().toInt();
        if (request->hasParam("provisionUserId", true)) doc["provisionUserId"] = request->getParam("provisionUserId", true)->value();
        if (request->hasParam("provisionProductId", true)) doc["provisionProductId"] = request->getParam("provisionProductId", true)->value();
        if (request->hasParam("provisionAuthCode", true)) doc["provisionAuthCode"] = request->getParam("provisionAuthCode", true)->value();
        if (request->hasParam("provisionIP", true)) doc["provisionIP"] = request->getParam("provisionIP", true)->value();
        if (request->hasParam("provisionGateway", true)) doc["provisionGateway"] = request->getParam("provisionGateway", true)->value();
        if (request->hasParam("provisionSubnet", true)) doc["provisionSubnet"] = request->getParam("provisionSubnet", true)->value();
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "w");
        if (!f) { ctx->sendError(request, 500, "Failed to save provision config"); return; }
        serializeJsonPretty(doc, f);
        f.close();
        ctx->sendSuccess(request, "Provision configuration saved");
    });

    // JSON body handler for provision config (PUT)
    auto* provisionJsonHandler = new AsyncCallbackJsonWebHandler("/api/provision/config",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            if (!ctx->checkPermission(request, "network.edit")) { ctx->sendUnauthorized(request); return; }
            JsonObject obj = json.as<JsonObject>();
            // 读取现有 device.json
            JsonDocument doc;
            if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
                File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
                if (f) { deserializeJson(doc, f); f.close(); }
            }
            // 更新 AP 配网字段
            if (obj["provisionSSID"].is<String>()) doc["provisionSSID"] = obj["provisionSSID"].as<String>();
            if (obj["provisionPassword"].is<String>()) doc["provisionPassword"] = obj["provisionPassword"].as<String>();
            if (obj["provisionTimeout"].is<int>()) doc["provisionTimeout"] = obj["provisionTimeout"].as<int>();
            if (obj["provisionUserId"].is<String>()) doc["provisionUserId"] = obj["provisionUserId"].as<String>();
            if (obj["provisionProductId"].is<String>()) doc["provisionProductId"] = obj["provisionProductId"].as<String>();
            if (obj["provisionAuthCode"].is<String>()) doc["provisionAuthCode"] = obj["provisionAuthCode"].as<String>();
            if (obj["provisionIP"].is<String>()) doc["provisionIP"] = obj["provisionIP"].as<String>();
            if (obj["provisionGateway"].is<String>()) doc["provisionGateway"] = obj["provisionGateway"].as<String>();
            if (obj["provisionSubnet"].is<String>()) doc["provisionSubnet"] = obj["provisionSubnet"].as<String>();
            File f = LittleFS.open(DEVICE_CONFIG_FILE, "w");
            if (!f) { ctx->sendError(request, 500, "Failed to save provision config"); return; }
            serializeJsonPretty(doc, f);
            f.close();
            ctx->sendSuccess(request, "Provision configuration saved");
        });
    provisionJsonHandler->setMethod(HTTP_POST | HTTP_PUT);
    server->addHandler(provisionJsonHandler);

    server->on("/api/provision/start", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "network.edit")) { ctx->sendUnauthorized(request); return; }
        _apProvisionActive = true;
        // 触发AP配网开始系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
        PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_AP_PROVISION_START, "");
#endif
        // 从 device.json 读取 AP SSID
        String apSSID = "FastBee_Setup";
        if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
            File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
            if (f) {
                JsonDocument cfg;
                if (deserializeJson(cfg, f) == DeserializationError::Ok) {
                    if (cfg["provisionSSID"].is<String>() && !cfg["provisionSSID"].as<String>().isEmpty()) {
                        apSSID = cfg["provisionSSID"].as<String>();
                    }
                }
                f.close();
            }
        }
        JsonDocument doc;
        doc["success"] = true;
        doc["data"]["active"] = true;
        doc["data"]["apSSID"] = apSSID;
        HandlerUtils::sendJsonStream(request, doc);
    });

    server->on("/api/provision/stop", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "network.edit")) { ctx->sendUnauthorized(request); return; }
        _apProvisionActive = false;
        // 触发AP配网完成系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
        PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_AP_PROVISION_DONE, "");
#endif
        ctx->sendSuccess(request, "Provision stopped");
    });

    // ============ BLE Provision Routes ============

    server->on("/api/ble/provision/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "network.view")) { ctx->sendUnauthorized(request); return; }
        JsonDocument doc;
        doc["success"] = true;
        doc["data"]["active"] = _bleProvisionActive;
        doc["data"]["deviceName"] = "FastBee_BLE";
        if (_bleProvisionActive && _bleProvisionStartTime > 0) {
            unsigned long elapsed = (millis() - _bleProvisionStartTime) / 1000;
            int timeout = 300;
            String deviceName = "FastBee_BLE";
            // 从 device.json 读取蓝牙配网配置
            if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
                File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
                if (f) {
                    JsonDocument cfg;
                    if (deserializeJson(cfg, f) == DeserializationError::Ok) {
                        timeout = cfg["bleTimeout"] | 300;
                        if (cfg["bleName"].is<String>()) deviceName = cfg["bleName"].as<String>();
                    }
                    f.close();
                }
            }
            doc["data"]["deviceName"] = deviceName;
            int remaining = timeout - (int)elapsed;
            if (remaining < 0) remaining = 0;
            doc["data"]["remainingTime"] = remaining;
        } else {
            doc["data"]["remainingTime"] = 0;
        }
        HandlerUtils::sendJsonStream(request, doc);
    });

    server->on("/api/ble/provision/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "network.view")) { ctx->sendUnauthorized(request); return; }
        JsonDocument doc;
        doc["success"] = true;
        // 从 device.json 读取蓝牙配网配置
        if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
            File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
            if (f) {
                JsonDocument cfg;
                if (deserializeJson(cfg, f) == DeserializationError::Ok) {
                    doc["data"]["bleName"] = cfg["bleName"] | "FBDevice";
                    doc["data"]["bleTimeout"] = cfg["bleTimeout"] | 300;
                    doc["data"]["bleAutoStart"] = cfg["bleAutoStart"] | false;
                    doc["data"]["bleServiceUUID"] = cfg["bleServiceUUID"] | "6E400001-B5A3-F393-E0A9-E50E24DCCA9F";
                    doc["data"]["bleRxUUID"] = cfg["bleRxUUID"] | "6E400002-B5A3-F393-E0A9-E50E24DCCA9F";
                    doc["data"]["bleTxUUID"] = cfg["bleTxUUID"] | "6E400003-B5A3-F393-E0A9-E50E24DCCA9F";
                }
                f.close();
            }
        }
        if (doc["data"].isNull()) {
            doc["data"]["bleName"] = "FBDevice";
            doc["data"]["bleTimeout"] = 300;
            doc["data"]["bleAutoStart"] = false;
            doc["data"]["bleServiceUUID"] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9F";
            doc["data"]["bleRxUUID"] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9F";
            doc["data"]["bleTxUUID"] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9F";
        }
        HandlerUtils::sendJsonStream(request, doc);
    });

    // JSON body handler for BLE provision config (PUT)
    auto* bleProvisionJsonHandler = new AsyncCallbackJsonWebHandler("/api/ble/provision/config",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            if (!ctx->checkPermission(request, "network.edit")) { ctx->sendUnauthorized(request); return; }
            JsonObject obj = json.as<JsonObject>();
            // 读取现有 device.json
            JsonDocument doc;
            if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
                File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
                if (f) { deserializeJson(doc, f); f.close(); }
            }
            // 更新蓝牙配网字段
            if (obj["bleName"].is<String>()) doc["bleName"] = obj["bleName"].as<String>();
            if (obj["bleTimeout"].is<int>() || obj["bleTimeout"].is<String>()) doc["bleTimeout"] = obj["bleTimeout"].as<int>();
            if (obj["bleAutoStart"].is<bool>() || obj["bleAutoStart"].is<String>()) {
                String v = obj["bleAutoStart"].as<String>();
                doc["bleAutoStart"] = (v == "1" || v == "true" || obj["bleAutoStart"].as<bool>());
            }
            if (obj["bleServiceUUID"].is<String>()) doc["bleServiceUUID"] = obj["bleServiceUUID"].as<String>();
            if (obj["bleRxUUID"].is<String>()) doc["bleRxUUID"] = obj["bleRxUUID"].as<String>();
            if (obj["bleTxUUID"].is<String>()) doc["bleTxUUID"] = obj["bleTxUUID"].as<String>();
            File f = LittleFS.open(DEVICE_CONFIG_FILE, "w");
            if (!f) { ctx->sendError(request, 500, "Failed to save BLE provision config"); return; }
            serializeJsonPretty(doc, f);
            f.close();
            ctx->sendSuccess(request, "BLE provision configuration saved");
        });
    bleProvisionJsonHandler->setMethod(HTTP_POST | HTTP_PUT);
    server->addHandler(bleProvisionJsonHandler);

    server->on("/api/ble/provision/start", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "network.edit")) { ctx->sendUnauthorized(request); return; }
        _bleProvisionActive = true;
        _bleProvisionStartTime = millis();
        // 触发蓝牙配网开始系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
        PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_BLE_PROVISION_START, "");
#endif
        JsonDocument doc;
        doc["success"] = true;
        doc["data"]["active"] = true;
        doc["data"]["deviceName"] = "FastBee_BLE";
        HandlerUtils::sendJsonStream(request, doc);
    });

    server->on("/api/ble/provision/stop", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "network.edit")) { ctx->sendUnauthorized(request); return; }
        _bleProvisionActive = false;
        _bleProvisionStartTime = 0;
        // 触发蓝牙配网完成系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
        PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_BLE_PROVISION_DONE, "");
#endif
        ctx->sendSuccess(request, "BLE provision stopped");
    });
}

void ProvisionRouteHandler::handleSetupPage(AsyncWebServerRequest* request) {
    ctx->sendBuiltinSetupPage(request);
}

void ProvisionRouteHandler::handleWiFiScan(AsyncWebServerRequest* request) {
    JsonDocument doc;

    int n = WiFi.scanNetworks();

    if (n == WIFI_SCAN_FAILED) {
        doc["success"] = false;
        doc["error"] = "scan_failed";
        doc["message"] = "WiFi scan failed, please try again";
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
        return;
    }

    if (n == WIFI_SCAN_RUNNING) {
        doc["success"] = false;
        doc["error"] = "scan_busy";
        doc["message"] = "WiFi scan is already running, please wait";
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
        return;
    }

    JsonArray data = doc["data"].to<JsonArray>();
    for (int i = 0; i < n && i < 20; i++) {
        JsonObject net = data.add<JsonObject>();
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
        net["channel"] = WiFi.channel(i);
        net["encryption"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? 1 : 0;
    }
    WiFi.scanDelete();

    doc["success"] = true;
    doc["count"] = n;

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

void ProvisionRouteHandler::handleWiFiConnect(AsyncWebServerRequest* request) {
    String ssid = ctx->getParamValue(request, "ssid", "");
    String password = ctx->getParamValue(request, "password", "");

    if (ssid.isEmpty()) {
        ctx->sendBadRequest(request, "SSID is required");
        return;
    }

    LOG_INFOF("[Provision] WiFi connect request: SSID=%s", ssid.c_str());

    // 通过 NetworkManager 保存配置并连接
    if (ctx->networkManager) {
        NetworkManager* netMgr = static_cast<NetworkManager*>(ctx->networkManager);
        WiFiConfig cfg = netMgr->getConfig();
        
        // 更新 STA 配置
        cfg.staSSID = ssid;
        cfg.staPassword = password;
        
        // 如果之前是纯 AP 模式，切换到 AP+STA 或 STA 模式
        if (cfg.mode == NetworkMode::NETWORK_AP) {
            cfg.mode = NetworkMode::NETWORK_AP_STA;  // 保持 AP 可用以确保 Web 服务可用
            LOG_INFO("[Provision] Switching from AP to AP+STA mode for WiFi connection");
        }
        
        // 保存配置并触发网络重启
        if (netMgr->updateConfig(cfg, true)) {
            LOG_INFO("[Provision] WiFi configuration saved, network will restart");
            JsonDocument doc;
            doc["success"] = true;
            doc["message"] = "WiFi configuration saved, connecting...";
            doc["data"]["ssid"] = ssid;
            doc["data"]["mode"] = static_cast<uint8_t>(cfg.mode);
            String output;
            serializeJson(doc, output);
            request->send(200, "application/json", output);
            return;
        } else {
            LOG_ERROR("[Provision] Failed to save WiFi configuration");
            ctx->sendError(request, 500, "Failed to save WiFi configuration");
            return;
        }
    }

    // 回退：直接连接（不保存配置）
    ctx->sendSuccess(request, "Connecting to WiFi...");
    delay(100);
    WiFi.begin(ssid.c_str(), password.c_str());
}
