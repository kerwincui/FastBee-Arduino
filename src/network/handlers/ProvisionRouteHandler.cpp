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

// 清理旧的独立配置文件（已迁移到 device.json）
static void cleanupOldConfigFiles() {
    const char* OLD_PROVISION_CONFIG_FILE = "/config/provision.json";
    const char* OLD_BLE_PROVISION_CONFIG_FILE = "/config/ble_provision.json";
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
        
        // 切换到 STA 模式连接 WiFi
        cfg.mode = NetworkMode::NETWORK_STA;
        
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
