#include "./network/handlers/BatchRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "core/SystemConstants.h"
#include "utils/NetworkUtils.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>

BatchRouteHandler::BatchRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
    // 预分配 JSON body handler，避免 setupRoutes() 期间集中堆分配
    _batchJsonHandler = new AsyncCallbackJsonWebHandler("/api/batch",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            handleBatchRequest(request, json);
        });
    _batchJsonHandler->setMaxContentLength(1024);
}

BatchRouteHandler::~BatchRouteHandler() {
    // handler 由 AsyncWebServer 管理生命周期，不在此 delete
}

void BatchRouteHandler::setupRoutes(AsyncWebServer* server) {
    // Batch API endpoint - combine multiple GET requests into one
    // JSON handler 已在构造函数中预分配
    server->addHandler(_batchJsonHandler);
}

// ── Batch API: 合并多个 GET 请求为一次响应 ──
void BatchRouteHandler::handleBatchRequest(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }
    if (HandlerUtils::checkLowMemory(request)) return;

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

    JsonDocument respDoc;
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
        if (!buildSubResponse(url, item)) {
            item["success"] = false;
            item["error"] = "Unsupported endpoint";
        }
    }

    // 检查响应大小不超过 8KB
    size_t jsonSize = measureJson(respDoc);
    if (jsonSize > 8192) {
        HandlerUtils::sendJsonError(request, 413, "Batch response too large");
        return;
    }

    HandlerUtils::sendJsonStream(request, respDoc);
}

bool BatchRouteHandler::buildSubResponse(const String& url, JsonObject out) {
    // /api/system/info — 完整系统信息
    if (url == "/api/system/info") {
        out["success"] = true;
        JsonObject data = out["data"].to<JsonObject>();

        data["device"]["id"] = String((uint32_t)ESP.getEfuseMac(), HEX);
        data["device"]["chipModel"] = ESP.getChipModel();
        data["device"]["chipRevision"] = ESP.getChipRevision();
        data["device"]["cpuFreqMHz"] = ESP.getCpuFreqMHz();
        data["device"]["sdkVersion"] = ESP.getSdkVersion();
        data["device"]["freeHeap"] = ESP.getFreeHeap();
        data["device"]["flashSize"] = ESP.getFlashChipSize();
        data["device"]["firmwareVersion"] = SystemInfo::VERSION;

        size_t sketchSize = ESP.getSketchSize();
        size_t freeSketchSpace = ESP.getFreeSketchSpace();
        data["flash"]["total"] = ESP.getFlashChipSize();
        data["flash"]["sketchSize"] = sketchSize;
        data["flash"]["freeSketchSpace"] = freeSketchSpace;
        data["flash"]["used"] = sketchSize;
        data["flash"]["free"] = freeSketchSpace;
        data["flash"]["usagePercent"] = (int)((sketchSize * 100) / (sketchSize + freeSketchSpace));

        size_t heapSize = ESP.getHeapSize();
        size_t freeHeap = ESP.getFreeHeap();
        data["memory"]["heapTotal"] = heapSize;
        data["memory"]["heapFree"] = freeHeap;
        data["memory"]["heapUsed"] = heapSize - freeHeap;
        data["memory"]["heapMinFree"] = ESP.getMinFreeHeap();
        data["memory"]["heapMaxAlloc"] = ESP.getMaxAllocHeap();
        data["memory"]["heapUsagePercent"] = (int)(((heapSize - freeHeap) * 100) / heapSize);

        size_t fsTotal = LittleFS.totalBytes();
        size_t fsUsed = LittleFS.usedBytes();
        data["filesystem"]["total"] = fsTotal;
        data["filesystem"]["used"] = fsUsed;
        data["filesystem"]["free"] = fsTotal - fsUsed;
        data["filesystem"]["usagePercent"] = fsTotal > 0 ? (int)((fsUsed * 100) / fsTotal) : 0;

        unsigned long uptime = millis();
        data["uptime"]["ms"] = uptime;
        data["uptime"]["seconds"] = uptime / 1000;
        data["uptime"]["formatted"] = ctx->formatUptime(uptime);

        if (ctx->networkManager) {
            NetworkStatusInfo info = ctx->networkManager->getStatusInfo();
            data["network"]["connected"] = (info.status == NetworkStatus::CONNECTED);
            data["network"]["ipAddress"] = info.ipAddress;
            data["network"]["ssid"] = info.ssid;
            data["network"]["rssi"] = info.rssi;
            data["network"]["macAddress"] = WiFi.macAddress();
        }

        if (ctx->userManager) data["users"]["total"] = ctx->userManager->getUserCount();
        if (ctx->authManager) {
            data["users"]["activeSessions"] = ctx->authManager->getActiveSessionCount();
            data["users"]["online"] = ctx->authManager->getOnlineUserCount();
        }
        return true;
    }

    // /api/system/status — 轻量状态
    if (url == "/api/system/status") {
        out["success"] = true;
        JsonObject data = out["data"].to<JsonObject>();
        data["status"] = "running";
        data["timestamp"] = millis();
        data["freeHeap"] = ESP.getFreeHeap();
        data["uptime"] = millis();
        if (ctx->networkManager) {
            NetworkStatusInfo info = ctx->networkManager->getStatusInfo();
            data["networkConnected"] = (info.status == NetworkStatus::CONNECTED);
            data["ipAddress"] = info.ipAddress;
            data["ssid"] = info.ssid;
            data["rssi"] = info.rssi;
        }
        return true;
    }

    // /api/network/status — 网络详情
    if (url == "/api/network/status") {
        out["success"] = true;
        if (!ctx->networkManager) {
            out["success"] = false;
            out["error"] = "Network service unavailable";
            return true;
        }
        JsonObject data = out["data"].to<JsonObject>();
        NetworkManager* netMgr = static_cast<NetworkManager*>(ctx->networkManager);
        netMgr->updateStatusInfo();
        NetworkStatusInfo info = netMgr->getStatusInfo();
        WiFiConfig cfg = netMgr->getConfig();

        const char* statusText = "unknown";
        switch (info.status) {
            case NetworkStatus::CONNECTED:        statusText = "connected";   break;
            case NetworkStatus::DISCONNECTED:     statusText = "disconnected"; break;
            case NetworkStatus::CONNECTING:       statusText = "connecting";  break;
            case NetworkStatus::AP_MODE:          statusText = "ap_mode";     break;
            case NetworkStatus::CONNECTION_FAILED:statusText = "failed";      break;
            default: break;
        }
        data["status"] = statusText;
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
        data["txCount"] = info.txCount;
        data["rxCount"] = info.rxCount;
        const char* modeText = "unknown";
        switch (cfg.mode) {
            case NetworkMode::NETWORK_STA:    modeText = "STA";    break;
            case NetworkMode::NETWORK_AP:     modeText = "AP";     break;
            case NetworkMode::NETWORK_AP_STA: modeText = "AP+STA"; break;
            default: break;
        }
        data["mode"] = modeText;
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

    // /api/system/health — 健康检查
    if (url == "/api/system/health") {
        out["success"] = true;
        JsonObject data = out["data"].to<JsonObject>();
        data["status"] = "ok";
        data["freeHeap"] = ESP.getFreeHeap();
        data["uptime"] = millis() / 1000;
        return true;
    }

    return false;
}
