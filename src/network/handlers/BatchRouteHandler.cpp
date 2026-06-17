#include "./network/handlers/BatchRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "core/FeatureFlags.h"
#include "core/SystemConstants.h"
#include "utils/NetworkUtils.h"
#include "utils/PsramJsonDocument.h"
#include "utils/TimeUtils.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>

namespace {
constexpr const char* DEVICE_CONFIG_FILE = "/config/device.json";

bool isBatchMemoryCriticallyLow() {
    if (FastBee::psramAvailableForJson(32768)) {
        return ESP.getFreeHeap() < 2048 || ESP.getMaxAllocHeap() < 1024;
    }
    return ESP.getFreeHeap() < 4096 || ESP.getMaxAllocHeap() < 1536;
}

bool shouldUseCompactSystemInfo() {
    return ESP.getFreeHeap() < 12288 || ESP.getMaxAllocHeap() < 6144;
}

void fillMemorySummary(JsonObject memory) {
    size_t heapSize = ESP.getHeapSize();
    size_t freeHeap = ESP.getFreeHeap();
    size_t heapUsed = heapSize > freeHeap ? heapSize - freeHeap : 0;

    memory["heapTotal"] = heapSize;
    memory["heapFree"] = freeHeap;
    memory["heapUsed"] = heapUsed;
    memory["heapMinFree"] = ESP.getMinFreeHeap();
    memory["heapMaxAlloc"] = ESP.getMaxAllocHeap();
    memory["heapUsagePercent"] = heapSize > 0 ? (int)((heapUsed * 100) / heapSize) : 0;

    size_t psramSize = ESP.getPsramSize();
    size_t psramFree = psramSize > 0 ? ESP.getFreePsram() : 0;
    size_t psramUsed = psramSize > psramFree ? psramSize - psramFree : 0;
    if (psramSize > 0) {
        memory["psramTotal"] = psramSize;
        memory["psramFree"] = psramFree;
        memory["psramUsed"] = psramUsed;
        memory["psramMinFree"] = ESP.getMinFreePsram();
        memory["psramUsagePercent"] = (int)((psramUsed * 100) / psramSize);
    }

    size_t total = heapSize + psramSize;
    size_t free = freeHeap + psramFree;
    size_t used = total > free ? total - free : 0;
    memory["total"] = total;
    memory["free"] = free;
    memory["used"] = used;
    memory["usagePercent"] = total > 0 ? (int)((used * 100) / total) : 0;
}

void fillCompactSystemInfo(JsonObject data, WebHandlerContext* ctx, bool degraded) {
    JsonObject device = data["device"].to<JsonObject>();
    device["id"] = String((uint32_t)ESP.getEfuseMac(), HEX);
    device["chipModel"] = ESP.getChipModel();
    device["chipRevision"] = ESP.getChipRevision();
    device["cpuFreqMHz"] = ESP.getCpuFreqMHz();
    device["sdkVersion"] = ESP.getSdkVersion();
    device["freeHeap"] = ESP.getFreeHeap();
    device["flashSize"] = ESP.getFlashChipSize();
    device["firmwareVersion"] = SystemInfo::VERSION;

    size_t sketchSize = ESP.getSketchSize();
    size_t freeSketchSpace = ESP.getFreeSketchSpace();
    JsonObject flash = data["flash"].to<JsonObject>();
    flash["total"] = ESP.getFlashChipSize();
    flash["sketchSize"] = sketchSize;
    flash["freeSketchSpace"] = freeSketchSpace;
    flash["used"] = sketchSize;
    flash["free"] = freeSketchSpace;
    flash["usagePercent"] = (int)((sketchSize * 100) / (sketchSize + freeSketchSpace));

    JsonObject memory = data["memory"].to<JsonObject>();
    fillMemorySummary(memory);

    size_t fsTotal = LittleFS.totalBytes();
    size_t fsUsed = LittleFS.usedBytes();
    JsonObject filesystem = data["filesystem"].to<JsonObject>();
    filesystem["total"] = fsTotal;
    filesystem["used"] = fsUsed;
    filesystem["free"] = fsTotal - fsUsed;
    filesystem["usagePercent"] = fsTotal > 0 ? (int)((fsUsed * 100) / fsTotal) : 0;

    unsigned long uptime = millis();
    JsonObject uptimeObj = data["uptime"].to<JsonObject>();
    uptimeObj["ms"] = uptime;
    uptimeObj["seconds"] = uptime / 1000;
    uptimeObj["formatted"] = ctx ? ctx->formatUptime(uptime) : String(uptime / 1000) + "s";

    if (ctx && ctx->networkManager) {
        NetworkStatusInfo info = ctx->networkManager->getStatusInfo();
        JsonObject network = data["network"].to<JsonObject>();
        network["connected"] = (info.status == NetworkStatus::CONNECTED);
        network["ipAddress"] = info.ipAddress;
        network["ssid"] = info.ssid;
        network["rssi"] = info.rssi;
        network["macAddress"] = WiFi.macAddress();
    }

    data["brief"] = true;
    data["degraded"] = degraded;
}
}

BatchRouteHandler::BatchRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
    _batchJsonHandler = new AsyncCallbackJsonWebHandler("/api/batch",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            handleBatchRequest(request, json);
        });
    _batchJsonHandler->setMaxContentLength(1024);
}

BatchRouteHandler::~BatchRouteHandler() {
    // Handler lifetime is owned by AsyncWebServer.
}

void BatchRouteHandler::setupRoutes(AsyncWebServer* server) {
    server->addHandler(_batchJsonHandler);
}

void BatchRouteHandler::handleBatchRequest(AsyncWebServerRequest* request, JsonVariant& json) {
    if (isBatchMemoryCriticallyLow()) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Low memory: heap=%lu maxAlloc=%lu",
                 (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
        HandlerUtils::sendJsonError(request, 503, msg, 5);
        return;
    }

    JsonObject body = json.as<JsonObject>();
    JsonArray requests = body["requests"].as<JsonArray>();
    if (requests.isNull() || requests.size() == 0) {
        HandlerUtils::sendJsonError(request, 400, "Missing or empty 'requests' array");
        return;
    }
    if (requests.size() > 5) {
        HandlerUtils::sendJsonError(request, 400, "Max 5 sub-requests per batch");
        return;
    }

    auto respDoc = FastBee::makeJsonDocument();
    respDoc["success"] = true;
    JsonArray results = respDoc["results"].to<JsonArray>();

    for (JsonVariant req : requests) {
        String url = req["url"].as<String>();
        JsonObject item = results.add<JsonObject>();
        if (url.isEmpty()) {
            item["success"] = false;
            item["error"] = "Missing url";
            continue;
        }
        if (!buildSubResponse(request, url, item)) {
            item["success"] = false;
            item["error"] = "Unsupported endpoint";
        }
    }

    size_t jsonSize = measureJson(respDoc);
    if (jsonSize > 8192) {
        HandlerUtils::sendJsonError(request, 413, "Batch response too large");
        return;
    }

    HandlerUtils::sendJsonStream(request, respDoc);
}

bool BatchRouteHandler::buildSubResponse(AsyncWebServerRequest* request, const String& url, JsonObject out) {
    if (url == "/api/system/info") {
        if (!ctx->requiresAuth(request)) {
            out["success"] = false;
            out["error"] = "Unauthorized";
            return true;
        }

        out["success"] = true;
        JsonObject data = out["data"].to<JsonObject>();

        fillCompactSystemInfo(data, ctx, shouldUseCompactSystemInfo());
        return true;
    }

    if (url == "/api/system/status") {
        if (!ctx->requiresAuth(request)) {
            out["success"] = false;
            out["error"] = "Unauthorized";
            return true;
        }

        out["success"] = true;
        JsonObject data = out["data"].to<JsonObject>();
        data["status"] = "running";
        data["timestamp"] = millis();
        data["freeHeap"] = ESP.getFreeHeap();
        data["uptime"] = millis();
        if (ctx->networkManager) {
            FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
            netMgr->updateStatusInfo();
            NetworkStatusInfo info = netMgr->getStatusInfo();
            WiFiConfig cfg = netMgr->getConfig();
            if (cfg.networkType == NetworkType::NET_WIFI && WiFi.status() == WL_CONNECTED) {
                info.status = NetworkStatus::CONNECTED;
                if (info.ipAddress.isEmpty() || info.ipAddress == "0.0.0.0") info.ipAddress = WiFi.localIP().toString();
                if (info.ssid.isEmpty()) info.ssid = WiFi.SSID();
                info.rssi = WiFi.RSSI();
                if (info.macAddress.isEmpty()) info.macAddress = WiFi.macAddress();
            }
            const char* statusText = "unknown";
            switch (info.status) {
                case NetworkStatus::CONNECTED:         statusText = "connected"; break;
                case NetworkStatus::DISCONNECTED:      statusText = "disconnected"; break;
                case NetworkStatus::CONNECTING:        statusText = "connecting"; break;
                case NetworkStatus::AP_MODE:           statusText = "ap_mode"; break;
                case NetworkStatus::CONNECTION_FAILED: statusText = "failed"; break;
                default: break;
            }
            data["networkStatus"] = statusText;
            data["networkStatusCode"] = static_cast<uint8_t>(info.status);
            data["networkConnected"] = (info.status == NetworkStatus::CONNECTED);
            data["connected"] = (info.status == NetworkStatus::CONNECTED);
            data["deviceNetworkType"] = static_cast<uint8_t>(cfg.networkType);
            data["ipAddress"] = info.ipAddress;
            data["ssid"] = info.ssid;
            data["rssi"] = info.rssi;
            data["macAddress"] = info.macAddress.isEmpty() ? WiFi.macAddress() : info.macAddress;
            data["internetAvailable"] = info.internetAvailable;
        }
        return true;
    }

    if (url == "/api/network/status") {
        if (!ctx->requiresAuth(request)) {
            out["success"] = false;
            out["error"] = "Unauthorized";
            return true;
        }
        if (!ctx->networkManager) {
            out["success"] = false;
            out["error"] = "Network service unavailable";
            return true;
        }

        out["success"] = true;
        JsonObject data = out["data"].to<JsonObject>();
        FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
        netMgr->updateStatusInfo();
        NetworkStatusInfo info = netMgr->getStatusInfo();
        WiFiConfig cfg = netMgr->getConfig();
        if (cfg.networkType == NetworkType::NET_WIFI && WiFi.status() == WL_CONNECTED) {
            info.status = NetworkStatus::CONNECTED;
            if (info.ipAddress.isEmpty() || info.ipAddress == "0.0.0.0") info.ipAddress = WiFi.localIP().toString();
            if (info.ssid.isEmpty()) info.ssid = WiFi.SSID();
            info.rssi = WiFi.RSSI();
            if (info.macAddress.isEmpty()) info.macAddress = WiFi.macAddress();
        }

        const char* statusText = "unknown";
        switch (info.status) {
            case NetworkStatus::CONNECTED:         statusText = "connected"; break;
            case NetworkStatus::DISCONNECTED:      statusText = "disconnected"; break;
            case NetworkStatus::CONNECTING:        statusText = "connecting"; break;
            case NetworkStatus::AP_MODE:           statusText = "ap_mode"; break;
            case NetworkStatus::CONNECTION_FAILED: statusText = "failed"; break;
            default: break;
        }
        data["status"] = statusText;
        data["statusCode"] = static_cast<uint8_t>(info.status);
        data["connected"] = (info.status == NetworkStatus::CONNECTED);
        data["deviceNetworkType"] = static_cast<uint8_t>(cfg.networkType);
        data["ssid"] = info.ssid.isEmpty() ? cfg.staSSID : info.ssid;
        data["ipAddress"] = info.ipAddress;
        data["macAddress"] = info.macAddress.isEmpty() ? WiFi.macAddress() : info.macAddress;
        data["rssi"] = info.rssi;
        data["signalStrength"] = NetworkUtils::rssiToPercentage(info.rssi);
        data["gateway"] = info.currentGateway;
        data["subnet"] = info.currentSubnet;
        data["dnsServer"] = info.dnsServer;
        data["dnsServer2"] = WiFi.status() == WL_CONNECTED ? WiFi.dnsIP(1).toString() : String("");
        data["connectedTime"] = info.lastConnectionTime > 0 ? (millis() - info.lastConnectionTime) / 1000 : 0;
        data["apIPAddress"] = info.apIPAddress;
        data["apClientCount"] = info.apClientCount;
        data["apSSID"] = cfg.apSSID;
        data["apChannel"] = cfg.apChannel;
        data["reconnectAttempts"] = info.reconnectAttempts;
        data["uptime"] = info.uptime;
        data["internetAvailable"] = info.internetAvailable;
        data["conflictDetected"] = info.conflictDetected;
        data["failoverCount"] = info.failoverCount;
        data["activeIPType"] = info.activeIPType;
        data["txCount"] = info.txCount;
        data["rxCount"] = info.rxCount;
        const char* modeText = "unknown";
        switch (cfg.mode) {
            case NetworkMode::NETWORK_STA: modeText = "STA"; break;
            case NetworkMode::NETWORK_AP:  modeText = "AP"; break;
            default: break;
        }
        data["mode"] = modeText;
        data["modeCode"] = static_cast<uint8_t>(cfg.mode);
        data["enableMDNS"] = cfg.enableMDNS;
        data["customDomain"] = cfg.customDomain;
        DNSManager* dns = netMgr->getDNSManager();
        if (dns) data["mdnsDomain"] = dns->getActualHostname();
        unsigned long uptimeSec = millis() / 1000;
        char uptimeBuf[32];
        snprintf(uptimeBuf, sizeof(uptimeBuf), "%lud %02lu:%02lu:%02lu",
            uptimeSec / 86400, (uptimeSec % 86400) / 3600,
            (uptimeSec % 3600) / 60, uptimeSec % 60);
        data["uptimeFormatted"] = uptimeBuf;
        return true;
    }

    if (url == "/api/network/config") {
        if (!ctx->requiresAuth(request)) {
            out["success"] = false;
            out["error"] = "Unauthorized";
            return true;
        }
        if (!ctx->networkManager) {
            out["success"] = false;
            out["error"] = "Network service unavailable";
            return true;
        }

        out["success"] = true;
        JsonObject data = out["data"].to<JsonObject>();
        FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
        WiFiConfig cfg = netMgr->getConfig();
        NetworkStatusInfo info = netMgr->getStatusInfo();

        data["device"]["macAddress"] = WiFi.macAddress();
        data["network"]["networkType"] = static_cast<uint8_t>(cfg.networkType);
        data["network"]["mode"] = static_cast<uint8_t>(cfg.mode);
        data["network"]["ipConfigType"] = static_cast<uint8_t>(cfg.ipConfigType);
        data["network"]["enableMDNS"] = cfg.enableMDNS;
        data["network"]["customDomain"] = cfg.customDomain;

        data["sta"]["ssid"] = cfg.staSSID;
        data["sta"]["password"] = cfg.staPassword.length() > 0 ? "********" : "";
        data["sta"]["hasPassword"] = cfg.staPassword.length() > 0;
        data["sta"]["staticIP"] = cfg.staticIP;
        data["sta"]["gateway"] = cfg.gateway;
        data["sta"]["subnet"] = cfg.subnet;
        data["sta"]["dns1"] = cfg.dns1;
        data["sta"]["dns2"] = cfg.dns2;

        data["ap"]["ssid"] = cfg.apSSID;
        data["ap"]["password"] = cfg.apPassword.length() > 0 ? "********" : "";
        data["ap"]["hasPassword"] = cfg.apPassword.length() > 0;
        data["ap"]["channel"] = cfg.apChannel;
        data["ap"]["hidden"] = cfg.apHidden;
        data["ap"]["maxConnections"] = cfg.apMaxConnections;

        data["advanced"]["connectTimeout"] = cfg.connectTimeout;
        data["advanced"]["reconnectInterval"] = cfg.reconnectInterval;
        data["advanced"]["maxReconnectAttempts"] = cfg.maxReconnectAttempts;
        data["advanced"]["conflictDetection"] = static_cast<uint8_t>(cfg.conflictDetection);
        data["advanced"]["failoverStrategy"] = static_cast<uint8_t>(cfg.failoverStrategy);
        data["advanced"]["autoFailover"] = cfg.autoFailover;
        data["advanced"]["conflictCheckInterval"] = cfg.conflictCheckInterval;
        data["advanced"]["maxFailoverAttempts"] = cfg.maxFailoverAttempts;
        data["advanced"]["conflictThreshold"] = cfg.conflictThreshold;
        data["advanced"]["fallbackToDHCP"] = cfg.fallbackToDHCP;

        data["status"]["connected"] = (info.status == NetworkStatus::CONNECTED);
        data["status"]["ipAddress"] = info.ipAddress;
        data["status"]["rssi"] = info.rssi;
        data["status"]["ssid"] = info.ssid;
        return true;
    }

    if (url == "/api/device/config") {
        if (!ctx->requiresAuth(request)) {
            out["success"] = false;
            out["error"] = "Unauthorized";
            return true;
        }

        if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
            File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
            if (f) {
                JsonDocument fileCfg;
                DeserializationError err = deserializeJson(fileCfg, f);
                f.close();
                if (!err) {
                    out["success"] = true;
                    out["data"] = fileCfg;
                    return true;
                }
            }
        }

        out["success"] = true;
        out["data"]["deviceName"] = "FastBee-ESP32";
        out["data"]["deviceId"] = String((uint32_t)ESP.getEfuseMac(), HEX);
        return true;
    }

    if (url == "/api/device/time") {
        if (!ctx->requiresAuth(request)) {
            out["success"] = false;
            out["error"] = "Unauthorized";
            return true;
        }

        out["success"] = true;
        JsonObject data = out["data"].to<JsonObject>();
        time_t now = TimeUtils::getTimestamp();
        bool internetAvailable = false;

        if (ctx->networkManager) {
            FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
            NetworkStatusInfo netInfo = netMgr->getStatusInfo();
            internetAvailable = netInfo.internetAvailable;
        }

        bool timeValid = (now > 1577836800);
        bool synced = timeValid && internetAvailable;

        data["datetime"] = TimeUtils::formatTime(now, TimeUtils::HUMAN_READABLE);
        data["timestamp"] = (long)now;
        data["synced"] = synced;
        data["timeValid"] = timeValid;
        data["internetAvailable"] = internetAvailable;
        data["uptime"] = millis();
        data["timezone"] = "CST-8";

        if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
            File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
            if (f) {
                JsonDocument cfg;
                if (deserializeJson(cfg, f) == DeserializationError::Ok) {
                    if (cfg["timezone"].is<String>()) data["timezone"] = cfg["timezone"].as<String>();
                }
                f.close();
            }
        }
        return true;
    }

    if (url == "/api/protocol/config") {
        out["success"] = false;
        out["error"] = "Protocol config is too large for batch";
        return true;
    }

    if (url == "/api/system/health") {
        if (!ctx->requiresAuth(request)) {
            out["success"] = false;
            out["error"] = "Unauthorized";
            return true;
        }

        out["success"] = true;
        JsonObject data = out["data"].to<JsonObject>();
        data["status"] = "ok";
        data["freeHeap"] = ESP.getFreeHeap();
        data["uptime"] = millis() / 1000;
        return true;
    }

    return false;
}
