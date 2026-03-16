#include "./network/handlers/ProvisionRouteHandler.h"
#include "./network/WebHandlerContext.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>

static const char* PROVISION_CONFIG_FILE = "/config/provision.json";
static const char* BLE_PROVISION_CONFIG_FILE = "/config/ble_provision.json";
static bool _apProvisionActive = false;
static bool _bleProvisionActive = false;
static unsigned long _bleProvisionStartTime = 0;

ProvisionRouteHandler::ProvisionRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
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
        String output; serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    server->on("/api/provision/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "network.view")) { ctx->sendUnauthorized(request); return; }
        JsonDocument doc;
        doc["success"] = true;
        if (LittleFS.exists(PROVISION_CONFIG_FILE)) {
            File f = LittleFS.open(PROVISION_CONFIG_FILE, "r");
            if (f) {
                JsonDocument cfg;
                if (deserializeJson(cfg, f) == DeserializationError::Ok) {
                    doc["data"] = cfg.as<JsonObject>();
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
        String output; serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    server->on("/api/provision/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "network.edit")) { ctx->sendUnauthorized(request); return; }
        JsonDocument doc;
        if (request->hasParam("provisionSSID", true)) doc["provisionSSID"] = request->getParam("provisionSSID", true)->value();
        if (request->hasParam("provisionPassword", true)) doc["provisionPassword"] = request->getParam("provisionPassword", true)->value();
        if (request->hasParam("provisionTimeout", true)) doc["provisionTimeout"] = request->getParam("provisionTimeout", true)->value().toInt();
        if (request->hasParam("provisionUserId", true)) doc["provisionUserId"] = request->getParam("provisionUserId", true)->value();
        if (request->hasParam("provisionProductId", true)) doc["provisionProductId"] = request->getParam("provisionProductId", true)->value();
        if (request->hasParam("provisionAuthCode", true)) doc["provisionAuthCode"] = request->getParam("provisionAuthCode", true)->value();
        if (request->hasParam("provisionIP", true)) doc["provisionIP"] = request->getParam("provisionIP", true)->value();
        if (request->hasParam("provisionGateway", true)) doc["provisionGateway"] = request->getParam("provisionGateway", true)->value();
        if (request->hasParam("provisionSubnet", true)) doc["provisionSubnet"] = request->getParam("provisionSubnet", true)->value();
        File f = LittleFS.open(PROVISION_CONFIG_FILE, "w");
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
            File f = LittleFS.open(PROVISION_CONFIG_FILE, "w");
            if (!f) { ctx->sendError(request, 500, "Failed to save provision config"); return; }
            serializeJsonPretty(obj, f);
            f.close();
            ctx->sendSuccess(request, "Provision configuration saved");
        });
    provisionJsonHandler->setMethod(HTTP_POST | HTTP_PUT);
    server->addHandler(provisionJsonHandler);

    server->on("/api/provision/start", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "network.edit")) { ctx->sendUnauthorized(request); return; }
        _apProvisionActive = true;
        // Read config for AP SSID
        String apSSID = "FastBee_Setup";
        if (LittleFS.exists(PROVISION_CONFIG_FILE)) {
            File f = LittleFS.open(PROVISION_CONFIG_FILE, "r");
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
        String output; serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    server->on("/api/provision/stop", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "network.edit")) { ctx->sendUnauthorized(request); return; }
        _apProvisionActive = false;
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
            if (LittleFS.exists(BLE_PROVISION_CONFIG_FILE)) {
                File f = LittleFS.open(BLE_PROVISION_CONFIG_FILE, "r");
                if (f) {
                    JsonDocument cfg;
                    if (deserializeJson(cfg, f) == DeserializationError::Ok) {
                        timeout = cfg["bleTimeout"] | 300;
                        if (cfg["bleDeviceName"].is<String>()) doc["data"]["deviceName"] = cfg["bleDeviceName"].as<String>();
                    }
                    f.close();
                }
            }
            int remaining = timeout - (int)elapsed;
            if (remaining < 0) remaining = 0;
            doc["data"]["remainingTime"] = remaining;
        } else {
            doc["data"]["remainingTime"] = 0;
        }
        String output; serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    server->on("/api/ble/provision/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "network.view")) { ctx->sendUnauthorized(request); return; }
        JsonDocument doc;
        doc["success"] = true;
        if (LittleFS.exists(BLE_PROVISION_CONFIG_FILE)) {
            File f = LittleFS.open(BLE_PROVISION_CONFIG_FILE, "r");
            if (f) {
                JsonDocument cfg;
                if (deserializeJson(cfg, f) == DeserializationError::Ok) {
                    doc["data"] = cfg.as<JsonObject>();
                }
                f.close();
            }
        }
        if (doc["data"].isNull()) {
            doc["data"]["bleDeviceName"] = "FastBee_BLE";
            doc["data"]["bleTimeout"] = 300;
            doc["data"]["bleServiceUUID"] = "";
        }
        String output; serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    // JSON body handler for BLE provision config (PUT)
    auto* bleProvisionJsonHandler = new AsyncCallbackJsonWebHandler("/api/ble/provision/config",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            if (!ctx->checkPermission(request, "network.edit")) { ctx->sendUnauthorized(request); return; }
            JsonObject obj = json.as<JsonObject>();
            File f = LittleFS.open(BLE_PROVISION_CONFIG_FILE, "w");
            if (!f) { ctx->sendError(request, 500, "Failed to save BLE provision config"); return; }
            serializeJsonPretty(obj, f);
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
        JsonDocument doc;
        doc["success"] = true;
        doc["data"]["active"] = true;
        doc["data"]["deviceName"] = "FastBee_BLE";
        String output; serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    server->on("/api/ble/provision/stop", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "network.edit")) { ctx->sendUnauthorized(request); return; }
        _bleProvisionActive = false;
        _bleProvisionStartTime = 0;
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

    ctx->sendSuccess(request, "Connecting to WiFi...");

    delay(100);

    WiFi.begin(ssid.c_str(), password.c_str());
}
