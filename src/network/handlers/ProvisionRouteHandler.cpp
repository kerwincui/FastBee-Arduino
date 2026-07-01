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

    JsonArray networks = doc["networks"].to<JsonArray>();
    for (int i = 0; i < n && i < 20; i++) {
        JsonObject net = networks.add<JsonObject>();
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
        net["channel"] = WiFi.channel(i);
        net["encrypted"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
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

    // 解析可选的扩展配网参数（仅当参数存在时才更新 device.json）
    String userId    = ctx->getParamValue(request, "userId", "");
    String deviceNum = ctx->getParamValue(request, "deviceNum", "");
    String extra     = ctx->getParamValue(request, "extra", "");

    bool hasExtParam = !userId.isEmpty() || !deviceNum.isEmpty() || !extra.isEmpty();
    if (hasExtParam) {
        LOG_INFOF("[Provision] Extended params: userId=%s deviceNum=%s extra=%s",
                  userId.c_str(), deviceNum.c_str(), extra.c_str());
        _updateDeviceConfig(userId, deviceNum, extra);
    }

    // 通过 NetworkManager 保存配置并连接
    if (ctx->networkManager) {
        FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
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

// ---------------------------------------------------------------------------
// 内部工具：将配网下发的扩展参数写入 device.json（仅更新提供的字段）
// ---------------------------------------------------------------------------
void ProvisionRouteHandler::_updateDeviceConfig(const String& userId,
                                                 const String& deviceNum,
                                                 const String& extra) {
    JsonDocument doc;

    // 读取现有 device.json
    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            deserializeJson(doc, f);
            f.close();
        }
    }

    bool changed = false;

    // userId → device.json.userId
    if (!userId.isEmpty()) {
        doc["userId"] = userId;
        changed = true;
        LOG_INFOF("[Provision] device.json: userId=%s", userId.c_str());
    }

    // deviceNum → device.json.deviceId
    if (!deviceNum.isEmpty()) {
        doc["deviceId"] = deviceNum;
        changed = true;
        LOG_INFOF("[Provision] device.json: deviceId=%s", deviceNum.c_str());
    }

    // extra → device.json.productNumber（仅当 extra 为有效正整数时保存）
    if (!extra.isEmpty()) {
        long pn = extra.toInt();
        if (pn > 0) {
            doc["productNumber"] = (int)pn;
            changed = true;
            LOG_INFOF("[Provision] device.json: productNumber=%d", (int)pn);
        } else {
            LOG_WARNINGF("[Provision] extra='%s' is not a valid positive integer, discarded", extra.c_str());
        }
    }

    if (changed) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "w");
        if (f) {
            serializeJsonPretty(doc, f);
            f.close();
            LOG_INFO("[Provision] device.json updated");
        } else {
            LOG_ERROR("[Provision] Failed to write device.json");
        }
    }
}
