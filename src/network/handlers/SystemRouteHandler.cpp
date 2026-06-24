#include "./network/handlers/SystemRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "systems/LoggerSystem.h"
#include "systems/ConfigStorage.h"
#include "systems/HealthMonitor.h"
#include "systems/SystemRebooter.h"
#include "core/FastBeeFramework.h"
#include "core/SystemConstants.h"
#include "utils/NetworkUtils.h"
#include "utils/TimeUtils.h"
#include "utils/StaticPoolAllocator.h"
#include "utils/PsramJsonDocument.h"
#include "core/FeatureFlags.h"
#include "./network/WebConfigManager.h"
#include "./network/handlers/SSERouteHandler.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <memory>
#include <ctype.h>

namespace {
// Keep inline imports small because form parsing already holds the body in RAM.
// Larger config files are transferred through /api/config/transfer/import-chunk.
constexpr size_t kMaxConfigTransferBytes = 128 * 1024;
constexpr size_t kMaxConfigInlineImportBytes = 24 * 1024;
// Chunk limit raised from 8 KB to 16 KB: the frontend sends chunks via
// application/x-www-form-urlencoded, so a 4 KB raw JSON chunk expands to
// ~8-10 KB after URL-encoding.  The handler already requires 24 KB free
// heap before writing, so 16 KB decoded chunk is safe.
constexpr size_t kMaxConfigTransferChunkBytes = 16 * 1024;
constexpr int kMaxConfigTransferChunks = 32;

bool isSafeConfigFileName(const String& name) {
    if (name.isEmpty() || name.length() > 48) return false;
    if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf("..") >= 0) return false;
    if (name[0] == '.' || !name.endsWith(".json")) return false;
    for (size_t i = 0; i < name.length(); ++i) {
        char c = name[i];
        if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') continue;
        return false;
    }
    return true;
}

bool isAllowedConfigTransferFileName(const String& name) {
    if (!isSafeConfigFileName(name)) return false;
    if (name == "roles.json") return false;
#if FASTBEE_SINGLE_ADMIN_MODE || !FASTBEE_ENABLE_USER_ADMIN
    if (name == "users.json") return false;
#endif
    return true;
}

bool isIgnoredConfigImportFileName(const String& name) {
    if (name == "roles.json") return true;
#if FASTBEE_SINGLE_ADMIN_MODE || !FASTBEE_ENABLE_USER_ADMIN
    if (name == "users.json") return true;
#endif
    return false;
}

String configPathForName(const String& name) {
    return String("/config/") + name;
}

String basenameOfFile(const char* path) {
    String name(path ? path : "");
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    return name;
}

bool shouldUseCompactSystemInfo(bool explicitBrief) {
    return explicitBrief || ESP.getFreeHeap() < 12288 || ESP.getMaxAllocHeap() < 6144;
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

void sendCompactSystemInfo(AsyncWebServerRequest* request, WebHandlerContext* ctx, bool degraded) {
    auto doc = FastBee::makeJsonDocument();
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    fillCompactSystemInfo(data, ctx, degraded);
    HandlerUtils::sendJsonStream(request, doc);
}
}

SystemRouteHandler::SystemRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void SystemRouteHandler::setupRoutes(AsyncWebServer* server) {
    server->on("/api/system/info", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleSystemInfo(request); });

    server->on("/api/system/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleSystemStatus(request); });

    server->on("/api/system/restart", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleSystemRestart(request); });

    server->on("/api/network/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleNetworkConfig(request); });

    server->on("/api/network/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        // Network status handler - critical endpoint, bypass heavy memory guard
                if (!ctx->requireAuth(request)) return;
        auto doc = FastBee::makeJsonDocument();
        doc["success"] = true;
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
                case NetworkStatus::CONNECTED:        statusText = "connected";   break;
                case NetworkStatus::DISCONNECTED:     statusText = "disconnected"; break;
                case NetworkStatus::CONNECTING:       statusText = "connecting";  break;
                case NetworkStatus::AP_MODE:          statusText = "ap_mode";     break;
                case NetworkStatus::CONNECTION_FAILED:statusText = "failed";      break;
                default: break;
            }
            doc["data"]["status"] = statusText;
            doc["data"]["statusCode"] = static_cast<uint8_t>(info.status);
            doc["data"]["connected"] = (info.status == NetworkStatus::CONNECTED);
            doc["data"]["deviceNetworkType"] = static_cast<uint8_t>(cfg.networkType);
            doc["data"]["ipAddress"]     = info.ipAddress;
            doc["data"]["macAddress"]    = info.macAddress.isEmpty() ? WiFi.macAddress() : info.macAddress;
            doc["data"]["connectedTime"] = info.lastConnectionTime > 0 ? (millis() - info.lastConnectionTime) / 1000 : 0;
            doc["data"]["apIPAddress"]   = info.apIPAddress;
            doc["data"]["apClientCount"] = info.apClientCount;
            doc["data"]["apSSID"]        = cfg.apSSID;
            doc["data"]["internetAvailable"] = info.internetAvailable;

            // WiFi 鐗规湁瀛楁
            if (cfg.networkType == NetworkType::NET_WIFI) {
                doc["data"]["ssid"]          = info.ssid.isEmpty() ? cfg.staSSID : info.ssid;
                doc["data"]["rssi"]          = info.rssi;
                doc["data"]["signalStrength"]= NetworkUtils::rssiToPercentage(info.rssi);
                doc["data"]["gateway"]       = info.currentGateway;
                doc["data"]["subnet"]        = info.currentSubnet;
                doc["data"]["dnsServer"]     = info.dnsServer;
                doc["data"]["reconnectAttempts"] = info.reconnectAttempts;
            }

            // 4G 铚傜獫缃戠粶鐘舵€侊紙浠呭湪 4G 妯″紡鏃惰繑鍥烇級
            if (cfg.networkType == NetworkType::NET_4G) {
                doc["data"]["simStatus"]         = info.simStatus;
                doc["data"]["operator"]          = info.operatorName;
                doc["data"]["networkType"]       = info.cellularNetworkType;
                doc["data"]["apn"]               = info.apn;
                doc["data"]["imei"]              = info.imei;
                doc["data"]["iccId"]             = info.iccid;
                doc["data"]["signalQuality"]     = info.cellularSignalQuality;
                doc["data"]["rssi"]              = info.rssi;
                doc["data"]["signalStrength"]    = NetworkUtils::rssiToPercentage(info.rssi);
            }

            // 浠ュお缃戠姸鎬?
            if (cfg.networkType == NetworkType::NET_ETHERNET) {
                doc["data"]["gateway"]       = info.currentGateway;
            }

            const char* modeText = "unknown";
            switch (cfg.mode) {
                case NetworkMode::NETWORK_STA:    modeText = "STA"; break;
                case NetworkMode::NETWORK_AP:     modeText = "AP";  break;
                default: break;
            }
            doc["data"]["mode"]       = modeText;
            doc["data"]["modeCode"]   = static_cast<uint8_t>(cfg.mode);
            doc["data"]["enableMDNS"] = cfg.enableMDNS;
            doc["data"]["customDomain"] = cfg.customDomain;
            DNSManager* dns = netMgr->getDNSManager();
            if (dns) {
                doc["data"]["mdnsDomain"] = dns->getActualHostname();
            }
            unsigned long uptimeSec = millis() / 1000;
            char uptimeBuf[32];
            snprintf(uptimeBuf, sizeof(uptimeBuf), "%lud %02lu:%02lu:%02lu",
                uptimeSec / 86400, (uptimeSec % 86400) / 3600,
                (uptimeSec % 3600) / 60, uptimeSec % 60);
            doc["data"]["uptimeFormatted"] = uptimeBuf;
        } else {
            doc["success"] = false;
            doc["error"] = "Network service unavailable";
        }
        // 缃戠粶鐘舵€佹槸UI鍏抽敭绔偣锛屽搷搴斾綋杈冨皬锛?1KB锛夛紝鐩存帴鍙戦€佷笉妫€鏌ュ唴瀛樺畧鍗?
        HandlerUtils::sendJsonStream(request, doc);
    });

    // JSON body handler for network config update
    auto* networkJsonHandler = new AsyncCallbackJsonWebHandler("/api/network/config",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            if (!ctx->requireAuth(request)) return;
            if (!ctx->networkManager) {
                ctx->sendError(request, 500, "Network service unavailable");
                return;
            }
            FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
            WiFiConfig cfg = netMgr->getConfig();
            JsonObject obj = json.as<JsonObject>();
            if (obj["mode"].is<String>()) cfg.mode = static_cast<NetworkMode>(obj["mode"].as<String>().toInt());
            if (obj["staSSID"].is<String>()) cfg.staSSID = obj["staSSID"].as<String>();
            if (obj["staPassword"].is<String>()) { String pwd = obj["staPassword"].as<String>(); if (!pwd.isEmpty() && pwd != "********") cfg.staPassword = pwd; }
            if (obj["ipConfigType"].is<String>()) cfg.ipConfigType = static_cast<IPConfigType>(obj["ipConfigType"].as<String>().toInt());
            if (obj["staticIP"].is<String>()) cfg.staticIP = obj["staticIP"].as<String>();
            if (obj["gateway"].is<String>()) cfg.gateway = obj["gateway"].as<String>();
            if (obj["subnet"].is<String>()) cfg.subnet = obj["subnet"].as<String>();
            if (obj["apSSID"].is<String>()) cfg.apSSID = obj["apSSID"].as<String>();
            if (obj["apPassword"].is<String>()) { String pwd = obj["apPassword"].as<String>(); if (!pwd.isEmpty() && pwd != "********") cfg.apPassword = pwd; }
            if (obj["apIP"].is<String>()) cfg.apIP = obj["apIP"].as<String>();
            if (obj["apChannel"].is<String>() || obj["apChannel"].is<int>()) cfg.apChannel = obj["apChannel"].as<int>();
            if (obj["apHidden"].is<String>() || obj["apHidden"].is<bool>()) { String h = obj["apHidden"].as<String>(); cfg.apHidden = (h == "1" || h == "true" || obj["apHidden"].as<bool>()); }
            if (obj["apMaxConnections"].is<String>() || obj["apMaxConnections"].is<int>()) cfg.apMaxConnections = obj["apMaxConnections"].as<int>();
            if (obj["dns1"].is<String>()) cfg.dns1 = obj["dns1"].as<String>();
            if (obj["dns2"].is<String>()) cfg.dns2 = obj["dns2"].as<String>();
            if (obj["enableMDNS"].is<String>() || obj["enableMDNS"].is<bool>()) { String m = obj["enableMDNS"].as<String>(); cfg.enableMDNS = (m == "1" || m == "true" || obj["enableMDNS"].as<bool>()); }
            if (obj["customDomain"].is<String>()) cfg.customDomain = obj["customDomain"].as<String>();
            if (obj["connectTimeout"].is<String>() || obj["connectTimeout"].is<int>()) cfg.connectTimeout = obj["connectTimeout"].as<int>();
            if (obj["reconnectInterval"].is<String>() || obj["reconnectInterval"].is<int>()) cfg.reconnectInterval = obj["reconnectInterval"].as<int>();
            if (obj["maxReconnectAttempts"].is<String>() || obj["maxReconnectAttempts"].is<int>()) cfg.maxReconnectAttempts = obj["maxReconnectAttempts"].as<int>();
            if (obj["conflictDetection"].is<String>() || obj["conflictDetection"].is<int>()) cfg.conflictDetection = static_cast<IPConflictMode>(obj["conflictDetection"].as<int>());
            // 鑱旂綉鏂瑰紡
            if (obj["networkType"].is<String>() || obj["networkType"].is<int>()) {
                uint8_t rawNetworkType = obj["networkType"].as<String>().toInt();
                cfg.networkType = rawNetworkType <= static_cast<uint8_t>(NetworkType::NET_4G)
                    ? static_cast<NetworkType>(rawNetworkType)
                    : NetworkType::NET_WIFI;
            }
            // 浠ュお缃戦厤缃?
            if (obj["ethernet"].is<JsonObject>()) {
                JsonObject eth = obj["ethernet"];
                if (eth.containsKey("spiMosi")) cfg.ethernet.spiMosi = eth["spiMosi"].as<int8_t>();
                if (eth.containsKey("spiMiso")) cfg.ethernet.spiMiso = eth["spiMiso"].as<int8_t>();
                if (eth.containsKey("spiSck"))  cfg.ethernet.spiSck  = eth["spiSck"].as<int8_t>();
                if (eth.containsKey("csPin"))   cfg.ethernet.csPin   = eth["csPin"].as<int8_t>();
                if (eth.containsKey("rstPin"))  cfg.ethernet.rstPin  = eth["rstPin"].as<int8_t>();
                if (eth.containsKey("intPin"))  cfg.ethernet.intPin  = eth["intPin"].as<int8_t>();
            }
            // 4G 铚傜獫閰嶇疆
            if (obj["cellular"].is<JsonObject>()) {
                JsonObject cell = obj["cellular"];
                if (cell.containsKey("txPin"))    cfg.cellular.txPin    = cell["txPin"].as<int8_t>();
                if (cell.containsKey("rxPin"))    cfg.cellular.rxPin    = cell["rxPin"].as<int8_t>();
                if (cell.containsKey("pwrPin"))   cfg.cellular.pwrPin   = cell["pwrPin"].as<int8_t>();
                if (cell.containsKey("baudRate")) cfg.cellular.baudRate = cell["baudRate"].as<uint32_t>();
                if (cell.containsKey("apn"))      cfg.cellular.apn      = cell["apn"].as<String>();
            }
            if (netMgr->updateConfig(cfg, true)) {
                LOGGER.info("Network configuration updated via web");
                
                // 配置保存成功，取消运行时网络重启（改为设备重启）
                // updateConfig() 可能已设置 pendingRestart=true，这里清除它
                // 设备重启比运行时 destroy/rebuild 更可靠：干净堆状态，无内存碎片
                netMgr->clearPendingRestart();

                // 构建详细响应，包含网络模式和访问信息
                auto respDoc = FastBee::makeJsonDocument();
                respDoc["success"] = true;
                respDoc["message"] = "Network configuration saved, device will reboot";
                
                // 网络模式信息
                respDoc["data"]["networkType"] = static_cast<uint8_t>(cfg.networkType);
                respDoc["data"]["mode"] = static_cast<uint8_t>(cfg.mode);
                const char* modeText = "unknown";
                switch (cfg.mode) {
                    case NetworkMode::NETWORK_STA:    modeText = "STA"; break;
                    case NetworkMode::NETWORK_AP:     modeText = "AP";  break;
                    default: break;
                }
                respDoc["data"]["modeText"] = modeText;
                
                // AP信息（AP模式需要）
                if (cfg.mode == NetworkMode::NETWORK_AP) {
                    respDoc["data"]["apSSID"] = cfg.apSSID;
                    respDoc["data"]["apPassword"] = cfg.apPassword;
                    respDoc["data"]["apIP"] = cfg.apIP;
                }
                
                // mDNS信息（STA模式需要）
                if (cfg.mode == NetworkMode::NETWORK_STA && cfg.enableMDNS) {
                    String domain = cfg.customDomain.length() > 0 ? cfg.customDomain : "fastbee";
                    respDoc["data"]["mdnsDomain"] = domain + ".local";
                }
                
                // 提示设备将重启
                respDoc["data"]["restartRequired"] = true;

                HandlerUtils::sendJsonStream(request, respDoc);

                // HTTP 响应已发送，调度延迟重启（2秒后 ESP.restart）
                SystemRebooter::scheduleConfigReboot("Network config changed");
            } else {
                ctx->sendError(request, 500, "Failed to save network configuration");
            }
        });
    networkJsonHandler->setMethod(HTTP_POST | HTTP_PUT);
    server->addHandler(networkJsonHandler);

    // Legacy form fallback. Register after the JSON handler so application/json
    // POST requests are parsed instead of returning the fallback hint.
    server->on("/api/network/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleSaveNetworkConfig(request); });

#if FASTBEE_ENABLE_CONFIG_TRANSFER
    server->on("/api/config/transfer/list", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->requireAuth(request)) return;
        if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Config list", MemoryGuardLevel::SEVERE, 8)) return;
        if (HandlerUtils::checkLowMemory(request, 8192)) return;

        JsonDocument doc;
        doc["success"] = true;
        JsonArray files = doc["data"]["files"].to<JsonArray>();

        File root = LittleFS.open("/config");
        if (root && root.isDirectory()) {
            File file = root.openNextFile();
            uint8_t count = 0;
            while (file && count < 32) {
                if (!file.isDirectory()) {
                    String name = basenameOfFile(file.name());
                    if (isAllowedConfigTransferFileName(name) && file.size() <= kMaxConfigTransferBytes) {
                        JsonObject item = files.add<JsonObject>();
                        item["name"] = name;
                        item["size"] = file.size();
                        count++;
                    }
                }
                file = root.openNextFile();
            }
            root.close();
        }

        HandlerUtils::sendJsonStream(request, doc);
    });

    server->on("/api/config/transfer/export", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->requireAuth(request)) return;
        if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Config export", MemoryGuardLevel::SEVERE, 8)) return;
        if (!request->hasParam("name")) { ctx->sendError(request, 400, "Missing name parameter"); return; }
        String name = request->getParam("name")->value();
        if (!isAllowedConfigTransferFileName(name)) { ctx->sendError(request, 403, "Invalid config file name"); return; }

        String path = configPathForName(name);
        if (!LittleFS.exists(path)) { ctx->sendError(request, 404, "Config file not found"); return; }
        File file = LittleFS.open(path, "r");
        if (!file) { ctx->sendError(request, 500, "Failed to open config file"); return; }
        if (file.size() > kMaxConfigTransferBytes) {
            file.close();
            ctx->sendError(request, 413, "Config file too large");
            return;
        }

        auto state = std::make_shared<File>(file);
        AsyncWebServerResponse* resp = request->beginChunkedResponse(
            "application/json",
            [state](uint8_t* buffer, size_t maxLen, size_t /*index*/) -> size_t {
                if (!state || !(*state)) return 0;
                if (!state->available()) {
                    state->close();
                    return 0;
                }
                return state->read(buffer, maxLen);
            });
        if (!resp) {
            file.close();
            HandlerUtils::sendJsonError(request, 500, "Failed to create export response");
            return;
        }
        resp->addHeader("Content-Disposition", String("attachment; filename=\"") + name + "\"");
        resp->addHeader("Cache-Control", "no-store");
        resp->addHeader("Connection", "close");
        request->send(resp);
    });

    server->on("/api/config/transfer/import", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->requireAuth(request)) return;
        if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Config import", MemoryGuardLevel::SEVERE, 8)) return;
        if (!request->hasParam("name", true) || !request->hasParam("content", true)) {
            ctx->sendError(request, 400, "Missing name or content parameter");
            return;
        }
        String name = request->getParam("name", true)->value();
        const String& content = request->getParam("content", true)->value();
        if (!isSafeConfigFileName(name)) { ctx->sendError(request, 403, "Invalid config file name"); return; }
        if (isIgnoredConfigImportFileName(name)) {
            JsonDocument skipped;
            skipped["name"] = name;
            skipped["skipped"] = true;
            ctx->sendSuccess(request, skipped);
            return;
        }
        if (content.isEmpty() || content.length() > kMaxConfigInlineImportBytes) {
            ctx->sendError(request, 413, "Config content too large for inline import");
            return;
        }
        if (HandlerUtils::checkLowMemory(request, 24576)) return;

        if (!LittleFS.exists("/config") && !LittleFS.mkdir("/config")) {
            ctx->sendError(request, 500, "Failed to create config directory");
            return;
        }

        String path = configPathForName(name);
        File out = LittleFS.open(path, "w");
        if (!out) { ctx->sendError(request, 500, "Failed to open config file for writing"); return; }
        size_t written = out.print(content);
        out.close();
        if (written != content.length()) {
            ctx->sendError(request, 500, "Failed to write complete config file");
            return;
        }

        if (name == "device.json" || name == "protocol.json") {
            FastBeeFramework* fw = FastBeeFramework::getInstance();
            if (fw) fw->ensureDeviceIdentity();
        }

        JsonDocument doc;
        doc["name"] = name;
        doc["size"] = written;
        ctx->sendSuccess(request, doc);
    });

    server->on("/api/config/transfer/import-chunk", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->requireAuth(request)) return;
        if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Config chunk import", MemoryGuardLevel::SEVERE, 8)) return;
        if (!request->hasParam("name", true) || !request->hasParam("chunk", true)
            || !request->hasParam("index", true) || !request->hasParam("total", true)) {
            ctx->sendError(request, 400, "Missing chunk import parameter");
            return;
        }

        String name = request->getParam("name", true)->value();
        const String& chunk = request->getParam("chunk", true)->value();
        int index = request->getParam("index", true)->value().toInt();
        int total = request->getParam("total", true)->value().toInt();

        if (!isSafeConfigFileName(name)) { ctx->sendError(request, 403, "Invalid config file name"); return; }
        if (isIgnoredConfigImportFileName(name)) {
            JsonDocument skipped;
            skipped["name"] = name;
            skipped["skipped"] = true;
            ctx->sendSuccess(request, skipped);
            return;
        }
        if (total <= 0 || total > kMaxConfigTransferChunks || index < 0 || index >= total) {
            ctx->sendError(request, 400, "Invalid chunk index");
            return;
        }
        if (chunk.isEmpty() || chunk.length() > kMaxConfigTransferChunkBytes) {
            ctx->sendError(request, 413, "Config chunk too large or empty");
            return;
        }
        if (HandlerUtils::checkLowMemory(request, 24576)) return;

        if (!LittleFS.exists("/config") && !LittleFS.mkdir("/config")) {
            ctx->sendError(request, 500, "Failed to create config directory");
            return;
        }

        String path = configPathForName(name);
        if (index > 0 && !LittleFS.exists(path)) {
            ctx->sendError(request, 409, "Missing previous config chunk");
            return;
        }

        File out = LittleFS.open(path, index == 0 ? "w" : "a");
        if (!out) { ctx->sendError(request, 500, "Failed to open config file for chunk write"); return; }
        size_t written = out.print(chunk);
        size_t fileSize = out.size();
        out.close();

        if (written != chunk.length()) {
            ctx->sendError(request, 500, "Failed to write complete config chunk");
            return;
        }
        if (fileSize > kMaxConfigTransferBytes) {
            LittleFS.remove(path);
            ctx->sendError(request, 413, "Config file too large");
            return;
        }

        if (index == total - 1 && (name == "device.json" || name == "protocol.json")) {
            FastBeeFramework* fw = FastBeeFramework::getInstance();
            if (fw) fw->ensureDeviceIdentity();
        }

        JsonDocument doc;
        doc["name"] = name;
        doc["index"] = index;
        doc["total"] = total;
        doc["size"] = fileSize;
        ctx->sendSuccess(request, doc);
    });
#endif

#if FASTBEE_ENABLE_FILE_MANAGER
    // Files
    server->on(AsyncURIMatcher::exact("/api/files"), HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetFilesList(request); });

    server->on("/api/files/content", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->requireAuth(request)) return;
        if (!request->hasParam("path")) { ctx->sendError(request, 400, "Missing path parameter"); return; }
        String path = request->getParam("path")->value();
        if (!path.startsWith("/")) path = "/" + path;
        if (path.indexOf("..") >= 0) { ctx->sendError(request, 403, "Invalid path"); return; }
        if (!LittleFS.exists(path)) { ctx->sendError(request, 404, "File not found"); return; }
        File file = LittleFS.open(path, "r");
        if (!file) { ctx->sendError(request, 500, "Failed to open file"); return; }
        size_t fileSize = file.size();
        if (fileSize > 128 * 1024) { file.close(); ctx->sendError(request, 413, "File too large (max 128KB)"); return; }

        // 鈽?Raw 蹇€熻矾寰勶細raw=1 鏃剁洿鎺?chunked 娴佸紡鍥炰紶鏂囦欢鍘熷瀛楄妭锛堟棤 JSON 鍖呰銆佹棤杞箟锛?
        // 鐢ㄤ簬閰嶇疆鏂囦欢瀵煎嚭鍦烘櫙锛氬嵆浣胯澶囧浜?MemGuard CRITICAL 绾т篃鑳芥垚鍔熶笅杞斤紝
        // 鍐呭瓨寮€閿€浠厏256B TCP 鍙戦€佺紦鍐?+ File 鍙ユ焺锛岃劚鏁忕敱鍓嶇鍦ㄦ祻瑙堝櫒鏈湴瀹屾垚銆?
        bool rawDownload = request->hasParam("raw") && request->getParam("raw")->value() == "1";
        if (rawDownload) {
            auto state = std::make_shared<File>(file);
            AsyncWebServerResponse* resp = request->beginChunkedResponse(
                "application/octet-stream",
                [state](uint8_t* buffer, size_t maxLen, size_t /*index*/) -> size_t {
                    if (!state || !(*state)) return 0;
                    if (!state->available()) {
                        state->close();
                        return 0;
                    }
                    return state->read(buffer, maxLen);
                });
            if (!resp) {
                file.close();
                HandlerUtils::sendJsonError(request, 500, "Failed to create raw response");
                return;
            }
            String fileName = path;
            int lastSlash = fileName.lastIndexOf('/');
            if (lastSlash >= 0) fileName = fileName.substring(lastSlash + 1);
            resp->addHeader("Content-Disposition", String("attachment; filename=\"") + fileName + "\"");
            resp->addHeader("X-Content-Type-Options", "nosniff");
            resp->addHeader("Cache-Control", "no-store");
            resp->addHeader("Connection", "close");
            request->send(resp);
            return;
        }

        // 鏂囦欢鍒嗙骇锛堢敤浜?MemGuard 绛栫暐鍐崇瓥锛夛細
        // - isTinyFile (鈮?4KB)锛氭瀬灏忔枃浠讹紙device.json/network.json锛?
        // - isConfigFile (/config/ 涓?鈮?16KB)锛氬彈淇濇姢閰嶇疆鏂囦欢锛坧eripherals/protocol/periph_exec锛?
        // - 鍏跺畠锛氭櫘閫氭枃浠?
        const bool isTinyFile = (fileSize <= 4096);
        const bool isConfigFile = path.startsWith("/config/") && (fileSize <= 16 * 1024);
        const bool isProtectedFile = isTinyFile || isConfigFile;

        // MemGuard 绛栫暐锛坈hunked 娴佸紡鍝嶅簲锛屽唴瀛樺崰鐢ㄦ亽瀹?~256B 缂撳啿 + 灏戦噺 state锛夛細
        // - 鍙椾繚鎶ゆ枃浠讹紙config/tiny锛夛細鏃犺 MemGuard 绾у埆锛屽缁堟斁琛岋紙CRITICAL 绾т笅涔熻兘鏁戞彺瀵煎嚭锛?
        // - 闈炲彈淇濇姢鏂囦欢锛欳RITICAL 绾у叏鎷掞紱SEVERE 绾ф嫆缁濓紙閬垮厤澶ф枃浠舵嫋鍨凡婵掍复 OOM 鐨勭郴缁燂級
        if (!isProtectedFile && HandlerUtils::rejectHeavyRequestOnPressure(
                request,
                rawDownload ? "Raw file download" : "File content",
                MemoryGuardLevel::SEVERE,
                8)) {
            file.close();
            return;
        }

        // 鍫嗗唴瀛樻渶灏忛槇鍊硷紙chunked 娴佸紡浠呴渶鏋佸皯鍐呭瓨鍗冲彲鍙戦€侊級锛?
        // - 鍙椾繚鎶ゆ枃浠讹細free 鈮?6KB, maxAlloc 鈮?2KB锛堜粎闇€鐘舵€佺粨鏋?+ TCP 鍙戦€佺紦鍐诧級
        // - 闈炲彈淇濇姢澶ф枃浠讹細free 鈮?20KB, maxAlloc 鈮?8KB
        size_t freeHeap = ESP.getFreeHeap();
        size_t maxAlloc = ESP.getMaxAllocHeap();
        size_t minFreeHeap = isProtectedFile ? 6000  : 20000;
        size_t minMaxAlloc = isProtectedFile ? 2000  : 8000;
        if (freeHeap < minFreeHeap || maxAlloc < minMaxAlloc) {
            // Chunked streaming uses minimal internal heap (~300B state + TCP buffer).
            // If PSRAM is available and heap isn't critically low, safe to proceed.
            if (!FastBee::psramAvailableForJson(4096) || HandlerUtils::internalHeapCriticallyLow(freeHeap, maxAlloc)) {
                file.close();
                char msg[96];
                snprintf(msg, sizeof(msg), "Low memory: heap=%u maxAlloc=%u", (unsigned)freeHeap, (unsigned)maxAlloc);
                HandlerUtils::sendJsonError(request, 503, msg);
                return;
            }
        }

        // ============ Chunked 娴佸紡鍝嶅簲鐘舵€佹満 ============
        // 閬垮厤涓€娆℃€у垎閰嶅ぇ String锛岃 CRITICAL 绾т笅鐨勯厤缃枃浠朵篃鑳藉畬鏁村鍑恒€?
        // 闃舵锛?=header, 1=content锛堣鏂囦欢+杞箟锛? 2=trailer "\"}}", 3=done
        struct FileStreamState {
            File file;
            size_t fileSize = 0;
            size_t rawPos = 0;
            String header;
            size_t headerPos = 0;
            size_t trailerPos = 0;
            // pendingBuf 缂撳瓨锛氬綋 buffer 鍓╀綑绌洪棿涓嶈冻浠ュ啓鍏?2 瀛楄妭杞箟鏃讹紝鏆傚瓨鍒颁笅娆″洖璋?
            uint8_t pendingBuf[130] = {0};  // 64 raw bytes * 2 max escape = 128 worst case
            size_t pendingLen = 0;
            size_t pendingHead = 0;
            uint8_t phase = 0;
        };

        auto state = std::make_shared<FileStreamState>();
        state->file = file;  // 绉讳氦鏂囦欢鍙ユ焺鎵€鏈夋潈
        state->fileSize = fileSize;
        state->header = F("{\"success\":true,\"data\":{\"path\":\"");
        state->header += path;
        state->header += F("\",\"size\":");
        state->header += String((unsigned int)fileSize);
        state->header += F(",\"content\":\"");

        AsyncWebServerResponse* resp = request->beginChunkedResponse(
            "application/json",
            [state](uint8_t* buffer, size_t maxLen, size_t /*index*/) -> size_t {
                size_t written = 0;
                // ---- Phase 0: header ----
                if (state->phase == 0) {
                    size_t headerLen = state->header.length();
                    while (state->headerPos < headerLen && written < maxLen) {
                        size_t remain = headerLen - state->headerPos;
                        size_t toCopy = (remain < (maxLen - written)) ? remain : (maxLen - written);
                        memcpy(buffer + written, state->header.c_str() + state->headerPos, toCopy);
                        state->headerPos += toCopy;
                        written += toCopy;
                    }
                    if (state->headerPos >= headerLen) state->phase = 1;
                    if (written > 0) return written;
                }
                // ---- Phase 1: 鏂囦欢鍐呭杞箟 ----
                if (state->phase == 1) {
                    // 浼樺厛娑堣€椾笂娆℃湭鑳藉啓鍏ョ殑杞箟瀛楄妭
                    while (state->pendingLen > 0 && written < maxLen) {
                        buffer[written++] = state->pendingBuf[state->pendingHead++];
                        state->pendingLen--;
                    }
                    if (state->pendingLen == 0) state->pendingHead = 0;

                    uint8_t raw[64];
                    while (written < maxLen && state->rawPos < state->fileSize && state->file.available()) {
                        size_t canRead = std::min((size_t)sizeof(raw), state->fileSize - state->rawPos);
                        int n = state->file.read(raw, canRead);
                        if (n <= 0) break;
                        state->rawPos += n;
                        for (int i = 0; i < n; i++) {
                            char c = (char)raw[i];
                            char esc[2]; size_t escLen = 0;
                            switch (c) {
                                case '"':  esc[0]='\\'; esc[1]='"'; escLen=2; break;
                                case '\\': esc[0]='\\'; esc[1]='\\'; escLen=2; break;
                                case '\n': esc[0]='\\'; esc[1]='n'; escLen=2; break;
                                case '\r': esc[0]='\\'; esc[1]='r'; escLen=2; break;
                                case '\t': esc[0]='\\'; esc[1]='t'; escLen=2; break;
                                default:
                                    if ((uint8_t)c >= 0x20) { esc[0]=c; escLen=1; }
                                    break;
                            }
                            if (escLen == 0) continue;  // 鎺у埗瀛楃鐩存帴涓㈠純
                            if (written + escLen <= maxLen) {
                                for (size_t j = 0; j < escLen; j++) buffer[written++] = esc[j];
                            } else {
                                // buffer 瑁呬笉涓嬪畬鏁磋浆涔夊簭鍒楋紝鏆傚瓨鍒?pendingBuf
                                for (size_t j = 0; j < escLen; j++) {
                                    if (state->pendingLen < sizeof(state->pendingBuf)) {
                                        state->pendingBuf[state->pendingLen++] = esc[j];
                                    }
                                }
                            }
                        }
                        // buffer 鍐欐弧涓?pending 闈炵┖锛氶€€鍑猴紝涓嬫鍐嶆潵
                        if (written >= maxLen) break;
                    }
                    if (state->rawPos >= state->fileSize && state->pendingLen == 0) {
                        state->phase = 2;
                    }
                    if (written > 0) return written;
                }
                // ---- Phase 2: trailer "\"}}" ----
                if (state->phase == 2) {
                    static const char kTrailer[] = "\"}}";
                    const size_t trailerLen = 3;
                    while (state->trailerPos < trailerLen && written < maxLen) {
                        buffer[written++] = (uint8_t)kTrailer[state->trailerPos++];
                    }
                    if (state->trailerPos >= trailerLen) state->phase = 3;
                    if (written > 0) return written;
                }
                // ---- Phase 3: done ----
                if (state->file) state->file.close();
                return 0;
            }
        );
        resp->addHeader("Connection", "close");
        request->send(resp);
    });

    server->on("/api/files/save", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->requireAuth(request)) return;
        if (!request->hasParam("path", true)) { ctx->sendError(request, 400, "Missing path parameter"); return; }
        String path = request->getParam("path", true)->value();
        if (!path.startsWith("/")) path = "/" + path;
        if (path.indexOf("..") >= 0) { ctx->sendError(request, 403, "Invalid path"); return; }
        if (!(path.endsWith(".json") || path.endsWith(".txt") || path.endsWith(".log") ||
              path.endsWith(".html") || path.endsWith(".js") || path.endsWith(".css"))) {
            ctx->sendError(request, 403, "File type not allowed for editing"); return;
        }
        String content = "";
        if (request->hasParam("content", true)) content = request->getParam("content", true)->value();
        int lastSlash = path.lastIndexOf('/');
        if (lastSlash > 0) {
            String parentDir = path.substring(0, lastSlash);
            if (!LittleFS.exists(parentDir)) { ctx->sendError(request, 500, "Parent directory does not exist"); return; }
        }
        File file = LittleFS.open(path, "w");
        if (!file) { ctx->sendError(request, 500, "Failed to open file for writing"); return; }
        size_t written = file.print(content);
        file.close();
        if (written == content.length()) {
            LOGGER.infof("File saved via web: %s", path.c_str());

            // 瀵煎叆/淇濆瓨閰嶇疆鍚庡洖濉韩浠藉瓧娈碉細褰撳啓鍏?device.json 鎴?protocol.json 鏃讹紝
            // 澶嶇敤鍚姩闃舵鐨?ensureDeviceIdentity() 鑷姩濉厖绌虹殑 deviceId / mqtt.clientId锛?
            // 纭繚璺ㄨ澶囧鍏ヤ篃鑳芥纭敓鎴愪竴鑷寸殑璁惧鏍囪瘑銆?
            if (path == "/config/device.json" || path == "/config/protocol.json") {
                FastBeeFramework* fw = FastBeeFramework::getInstance();
                if (fw) {
                    fw->ensureDeviceIdentity();
                }
            }

            ctx->sendSuccess(request, "File saved successfully");
        } else { ctx->sendError(request, 500, "Failed to write complete file"); }
    });

    // Filesystem info
    server->on("/api/filesystem", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->requireAuth(request)) return;
        if (ESP.getFreeHeap() < 2048 || ESP.getMaxAllocHeap() < 1024) {
            HandlerUtils::sendJsonError(request, 503, "Critical memory - filesystem info unavailable", 5);
            return;
        }
        auto doc = FastBee::makeJsonDocument(2048);
        size_t total = LittleFS.totalBytes();
        size_t used  = LittleFS.usedBytes();
        doc["totalBytes"] = total;
        doc["usedBytes"]  = used;
        doc["freeBytes"]  = total > used ? total - used : 0;
        doc["usedPercent"]= total > 0 ? (used * 100 / total) : 0;
        ctx->sendSuccess(request, doc);
    });
#endif // FASTBEE_ENABLE_FILE_MANAGER

    // Factory reset - 恢复默认配置而非删除文件，避免 Web 系统因文件缺失报错
    server->on("/api/system/factory-reset", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->requireAuth(request)) return;
        LOGGER.info("Factory reset initiated by user");

        // 写入默认配置内容的辅助 lambda
        auto writeDefault = [](const char* path, const char* content) -> bool {
            File f = LittleFS.open(path, FILE_WRITE);
            if (!f) return false;
            f.print(content);
            f.close();
            return true;
        };

        int resetCount = 0;

        // ── 核心配置文件：写入默认 JSON ──────────────────────────────────────
        // device.json - 设备基本信息（清除个性化配置，保留合理默认值）
        static const char DEFAULT_DEVICE[] PROGMEM =
            "{\"deviceId\":\"\",\"productNumber\":0,\"userId\":\"1\","
            "\"deviceName\":\"FastBee-Device\",\"location\":\"\",\"description\":\"FastBee-Arduino\","
            "\"enableNTP\":true,\"ntpServer1\":\"ntp.aliyun.com\",\"ntpServer2\":\"cn.pool.ntp.org\","
            "\"timezone\":\"CST-8\",\"syncInterval\":3600,\"logLevel\":\"INFO\","
            "\"developerModeEnabled\":true}";
        if (writeDefault("/config/device.json", DEFAULT_DEVICE)) { resetCount++; }

        // network.json - 网络配置（清除WiFi凭据，恢复AP模式以便用户重新配网）
        static const char DEFAULT_NETWORK[] PROGMEM =
            "{\"mode\":0,\"networkType\":0,"
            "\"apSSID\":\"fastbee-ap\",\"apPassword\":\"12345678\",\"apIP\":\"192.168.4.1\","
            "\"apChannel\":1,\"apHidden\":false,\"apMaxConnections\":4,"
            "\"staSSID\":\"\",\"staPassword\":\"\",\"networks\":[],"
            "\"ipConfigType\":0,\"staticIP\":\"\",\"gateway\":\"\",\"subnet\":\"\","
            "\"dns1\":\"8.8.8.8\",\"dns2\":\"8.8.4.4\",\"connectTimeout\":20000,"
            "\"reconnectInterval\":5000,\"maxReconnectAttempts\":5,"
            "\"customDomain\":\"fastbee\",\"enableMDNS\":true}";
        if (writeDefault("/config/network.json", DEFAULT_NETWORK)) { resetCount++; }

        // users.json - 恢复默认用户（admin/viewer）
        static const char DEFAULT_USERS[] PROGMEM =
            "{\"version\":\"2.0\",\"users\":["
            "{\"username\":\"admin\",\"passwordHash\":\"MMqroQgcopTMz5bU/x3brwufT38Ks6YAWGPRkmE5jDw=\","
            "\"salt\":\"RmFzdEJlZURlZmF1bHQhIQ==\",\"enabled\":true,"
            "\"createTime\":0,\"lastLogin\":0,\"lastModified\":0,"
            "\"description\":\"\"},"
            "{\"username\":\"viewer\",\"passwordHash\":\"8GzolfPWya4MDDWln2gLrhPhcitJHQGboXspAgr1CDU=\","
            "\"salt\":\"Vmlld2VyRGVmYXVsdCEhIQ==\",\"enabled\":true,"
            "\"createTime\":0,\"lastLogin\":0,\"lastModified\":0,"
            "\"description\":\"\"}],"
            "\"security\":{\"maxLoginAttempts\":5,\"loginLockoutTime\":300000,"
            "\"minPasswordLength\":6,\"requireStrongPasswords\":false}}";
        if (writeDefault("/config/users.json", DEFAULT_USERS)) { resetCount++; }

        // protocol.json - 所有协议禁用，用户需重新配置
        static const char DEFAULT_PROTOCOL[] PROGMEM =
            "{\"version\":2,"
            "\"modbusRtu\":{\"enabled\":false,\"peripheralId\":\"modbus_rtu\",\"mode\":\"master\",\"dePin\":14,\"transferType\":0,\"master\":{\"tasks\":[],\"devices\":[]}},"
            "\"modbusTcp\":{\"enabled\":false,\"server\":\"\",\"port\":502,\"slaveId\":1,\"timeout\":5000},"
            "\"mqtt\":{\"enabled\":false,\"server\":\"\",\"port\":1883,\"clientId\":\"\",\"username\":\"\",\"password\":\"\",\"mqttSecret\":\"\",\"keepAlive\":60,\"autoReconnect\":true,\"authType\":0,\"publishTopics\":[],\"subscribeTopics\":[]},"
            "\"http\":{\"enabled\":false,\"url\":\"\",\"port\":80,\"method\":\"POST\",\"timeout\":30,\"interval\":60},"
            "\"coap\":{\"enabled\":false,\"server\":\"\",\"port\":5683},"
            "\"tcp\":{\"enabled\":false,\"server\":\"\",\"port\":5000,\"timeout\":5000}}";
        if (writeDefault("/config/protocol.json", DEFAULT_PROTOCOL)) { resetCount++; }

        // peripherals.json - 清空外设配置（用户按需添加）
        static const char DEFAULT_PERIPHERALS[] PROGMEM = "{\"peripherals\":[]}";
        if (writeDefault("/config/peripherals.json", DEFAULT_PERIPHERALS)) { resetCount++; }

        // periph_exec.json - 清空执行规则
        static const char DEFAULT_PERIPH_EXEC[] PROGMEM = "{\"rules\":[]}";
        if (writeDefault("/config/periph_exec.json", DEFAULT_PERIPH_EXEC)) { resetCount++; }

        // ── 运行时可选配置文件：直接删除（各模块已处理文件不存在的情况）────
        const char* optionalFiles[] = {
            "/config/auth.json", "/config/system.json",
            "/config/http.json", "/config/mqtt.json", "/config/tcp.json",
            "/config/modbus.json", "/config/coap.json"
        };
        for (const char* optFile : optionalFiles) {
            if (LittleFS.exists(optFile)) {
                LittleFS.remove(optFile);
            }
        }

#if FASTBEE_ENABLE_FILE_LOGGING || FASTBEE_ENABLE_LOG_VIEWER
        if (LittleFS.exists("/logs/system.log")) {
            File logFile = LittleFS.open("/logs/system.log", "w");
            if (logFile) { logFile.close(); LOGGER.info("Cleared system log file"); }
        }
#endif

#if FASTBEE_ENABLE_STORAGE_CACHE
        // 清除配置缓存，确保重启前不会把旧缓存写回磁盘
        ConfigStorage::getInstance().clearCache();
#endif

        LOGGER.infof("Factory reset: %d config files restored to defaults", resetCount);

        JsonDocument doc;
        doc["success"] = true;
        doc["message"] = "Factory reset completed. Device will restart.";
        doc["restoredFiles"] = resetCount;
        doc["timestamp"] = millis();
        String jsonStr; serializeJson(doc, jsonStr);
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", jsonStr);
        response->addHeader("Connection", "close");
        request->onDisconnect([this]() {
            LOGGER.info("Factory reset response sent, scheduling restart via SystemRebooter");
            SystemRebooter::scheduleReboot("Factory reset completed", 2000,
                                           RestartReason::FACTORY_RESET);
        });
        request->send(response);
    });

    // Config
    server->on("/api/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->requireAuth(request)) return;
        JsonDocument doc;
        Preferences prefs;
        prefs.begin("webconfig", true);
        doc["webPort"] = prefs.getUInt("webPort", 80);
        doc["sessionTimeout"] = prefs.getUInt("sessionTimeout", 3600000);
        doc["maxLoginAttempts"] = prefs.getUInt("maxLoginAttempts", 5);
        doc["lockoutTime"] = prefs.getUInt("lockoutTime", 300000);
        prefs.end();
        ctx->sendSuccess(request, doc);
    });

    server->on("/api/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->requireAuth(request)) return;
        Preferences prefs;
        prefs.begin("webconfig", false);
        uint32_t webPort = ctx->getParamInt(request, "webPort", 0);
        uint32_t sessionTimeout = ctx->getParamInt(request, "sessionTimeout", 0);
        uint32_t maxLoginAttempts = ctx->getParamInt(request, "maxLoginAttempts", 0);
        uint32_t lockoutTime = ctx->getParamInt(request, "lockoutTime", 0);
        bool updated = false;
        if (webPort >= 1 && webPort <= 65535) { prefs.putUInt("webPort", webPort); updated = true; }
        if (sessionTimeout >= 60000 && sessionTimeout <= 86400000) { prefs.putUInt("sessionTimeout", sessionTimeout); updated = true; }
        if (maxLoginAttempts >= 1 && maxLoginAttempts <= 10) { prefs.putUInt("maxLoginAttempts", maxLoginAttempts); updated = true; }
        if (lockoutTime >= 60000 && lockoutTime <= 3600000) { prefs.putUInt("lockoutTime", lockoutTime); updated = true; }
        prefs.end();
        if (updated) ctx->sendSuccess(request, "Configuration updated successfully");
        else ctx->sendError(request, 400, "No valid configuration parameters provided");
    });

    // Health check
    server->on("/api/health", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetHealth(request); });
    server->on("/api/system/health", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetHealth(request); });

    // System metrics
    server->on("/api/system/metrics", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleSystemMetrics(request); });

    server->on("/api/system/web-runtime", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleWebRuntime(request); });

    // System capabilities (no auth required - public endpoint)
    server->on("/api/system/capabilities", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetCapabilities(request); });
}

void SystemRouteHandler::handleSystemInfo(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    if (request->hasParam("probe")) {
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"success\":true,\"data\":{\"memory\":{\"heapFree\":%lu,\"heapMaxAlloc\":%lu,"
                 "\"psramTotal\":%lu,\"psramFree\":%lu},\"brief\":true,\"probe\":true}}",
                 (unsigned long)ESP.getFreeHeap(),
                 (unsigned long)ESP.getMaxAllocHeap(),
                 (unsigned long)ESP.getPsramSize(),
                 (unsigned long)ESP.getFreePsram());
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        response->addHeader("Connection", "close");
        request->send(response);
        return;
    }

    FastBeeFramework* fw = FastBeeFramework::getInstance();
    bool forceBrief = false;
    if (fw && fw->getHealthMonitor()) {
        MemoryGuardLevel level = fw->getHealthMonitor()->getMemoryGuardLevel();
        if (level >= MemoryGuardLevel::CRITICAL) {
            if (!FastBee::psramAvailableForJson(32768) && ESP.getFreeHeap() < 4096) {
                HandlerUtils::sendJsonError(request, 503, "Critical memory - system info unavailable");
                return;
            }
            forceBrief = true;
        }
        if (level >= MemoryGuardLevel::SEVERE) {
            forceBrief = true;
        }
    }

    bool explicitFull = request->hasParam("full") &&
        request->getParam("full")->value() == "1";
    bool explicitBrief = request->hasParam("brief");
    bool brief = !explicitFull || forceBrief || shouldUseCompactSystemInfo(explicitBrief);
    if (brief) {
        sendCompactSystemInfo(request, ctx, forceBrief || (explicitFull && !explicitBrief));
        return;
    }

    if (HandlerUtils::checkLowMemory(request)) return;

    auto doc = FastBee::makeJsonDocument();

    doc["data"]["device"]["id"] = String((uint32_t)ESP.getEfuseMac(), HEX);
    doc["data"]["device"]["chipModel"] = ESP.getChipModel();
    doc["data"]["device"]["chipRevision"] = ESP.getChipRevision();
    doc["data"]["device"]["cpuFreqMHz"] = ESP.getCpuFreqMHz();
    doc["data"]["device"]["sdkVersion"] = ESP.getSdkVersion();
    doc["data"]["device"]["freeHeap"] = ESP.getFreeHeap();
    doc["data"]["device"]["flashSize"] = ESP.getFlashChipSize();
    doc["data"]["device"]["firmwareVersion"] = SystemInfo::VERSION;

    size_t flashSize = ESP.getFlashChipSize();
    size_t sketchSize = ESP.getSketchSize();
    size_t freeSketchSpace = ESP.getFreeSketchSpace();
    doc["data"]["flash"]["total"] = flashSize;
    doc["data"]["flash"]["speed"] = ESP.getFlashChipSpeed();
    doc["data"]["flash"]["sketchSize"] = sketchSize;
    doc["data"]["flash"]["freeSketchSpace"] = freeSketchSpace;
    doc["data"]["flash"]["used"] = sketchSize;
    doc["data"]["flash"]["free"] = freeSketchSpace;
    doc["data"]["flash"]["usagePercent"] = (int)((sketchSize * 100) / (sketchSize + freeSketchSpace));

    JsonObject memory = doc["data"]["memory"].to<JsonObject>();
    fillMemorySummary(memory);

    // 闈欐€佹睜鍒╃敤鐜囩洃鎺э紙T1+T2 纰庣墖娌荤悊鎴愭灉鍙娴嬬偣锛?
    // 涓変釜姹狅細
    //   - (256, 32): peripherals + runtimeStates 鑺傜偣姹?锛?KB DRAM锛?
    //   - (192, 32): rules 鑺傜偣姹?锛?KB DRAM锛?
    //   - (64, 64) : SmallNodePool 楂橀灏忓鍣ㄨ妭鐐规睜 锛?KB DRAM锛?
    JsonObject pools = memory["staticPools"].to<JsonObject>();
#define FB_FILL_POOL_STATS(NAME, B, N) do {                                          \
    auto& _p = FastBee::sharedSlabPool<(B), (N)>();                                  \
    JsonObject _o = pools[NAME].to<JsonObject>();                                    \
    _o["capacity"]      = (uint32_t)_p.capacity();                                   \
    _o["blockSize"]     = (uint32_t)_p.blockSize();                                  \
    _o["used"]          = (uint32_t)_p.usedCount();                                  \
    _o["peakUsed"]      = (uint32_t)_p.peakUsed();                                   \
    _o["overflowCount"] = (uint32_t)_p.overflowCount();                              \
    _o["totalBytes"]    = (uint32_t)_p.totalBytes();                                 \
} while (0)
    FB_FILL_POOL_STATS("periph256", 256, 32);
    FB_FILL_POOL_STATS("rule192",   192, 32);
    FB_FILL_POOL_STATS("small64",   FastBee::SMALL_NODE_BLOCK_SIZE,
                                    FastBee::SMALL_NODE_BLOCK_COUNT);
#undef FB_FILL_POOL_STATS

    size_t fsTotal = LittleFS.totalBytes();
    size_t fsUsed = LittleFS.usedBytes();
    doc["data"]["filesystem"]["total"] = fsTotal;
    doc["data"]["filesystem"]["used"] = fsUsed;
    doc["data"]["filesystem"]["free"] = fsTotal - fsUsed;
    doc["data"]["filesystem"]["usagePercent"] = fsTotal > 0 ? (int)((fsUsed * 100) / fsTotal) : 0;

    unsigned long uptime = millis();
    doc["data"]["uptime"]["ms"] = uptime;
    doc["data"]["uptime"]["seconds"] = uptime / 1000;
    doc["data"]["uptime"]["formatted"] = ctx->formatUptime(uptime);

    if (ctx->networkManager) {
        NetworkStatusInfo info = ctx->networkManager->getStatusInfo();
        doc["data"]["network"]["connected"] = (info.status == NetworkStatus::CONNECTED);
        doc["data"]["network"]["ipAddress"] = info.ipAddress;
        doc["data"]["network"]["ssid"] = info.ssid;
        doc["data"]["network"]["rssi"] = info.rssi;
        doc["data"]["network"]["macAddress"] = WiFi.macAddress();
    }

#if FASTBEE_ENABLE_USER_ADMIN
    if (ctx->userManager) {
        doc["data"]["users"]["total"] = ctx->userManager->getUserCount();
    }
    if (ctx->authManager) {
        doc["data"]["users"]["activeSessions"] = ctx->authManager->getActiveSessionCount();
        doc["data"]["users"]["online"] = ctx->authManager->getOnlineUserCount();
    }
#endif

    doc["success"] = true;
    HandlerUtils::sendJsonStream(request, doc);
}

void SystemRouteHandler::handleSystemStatus(AsyncWebServerRequest* request) {
    unsigned long now = millis();
    if (_statusCache.valid && (now - _statusCache.timestamp) < STATUS_CACHE_TTL) {
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", _statusCache.json);
        response->addHeader("Connection", "close");
        request->send(response);
        return;
    }

    auto doc = FastBee::makeJsonDocument();
    doc["status"]    = "running";
    doc["timestamp"] = millis();
    doc["freeHeap"]  = ESP.getFreeHeap();
    doc["uptime"]    = millis();

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
        doc["networkStatus"]    = statusText;
        doc["networkStatusCode"] = static_cast<uint8_t>(info.status);
        doc["networkConnected"] = (info.status == NetworkStatus::CONNECTED);
        doc["connected"]        = (info.status == NetworkStatus::CONNECTED);
        doc["deviceNetworkType"] = static_cast<uint8_t>(cfg.networkType);
        doc["ipAddress"]        = info.ipAddress;
        doc["ssid"]             = info.ssid;
        doc["rssi"]             = info.rssi;
        doc["macAddress"]       = info.macAddress.isEmpty() ? WiFi.macAddress() : info.macAddress;
        doc["internetAvailable"] = info.internetAvailable;
    }

    auto wrapper = FastBee::makeJsonDocument();
    wrapper["success"] = true;
    wrapper["data"] = doc;
    serializeJson(wrapper, _statusCache.json);
    _statusCache.timestamp = millis();
    _statusCache.valid = true;
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", _statusCache.json);
    response->addHeader("Connection", "close");
    request->send(response);
}

void SystemRouteHandler::handleSystemRestart(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    int delaySeconds = ctx->getParamInt(request, "delay", 3);
    delaySeconds = constrain(delaySeconds, 1, 30);

    LOG_INFOF("[System] Restart scheduled in %d seconds", delaySeconds);

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "System will restart in " + String(delaySeconds) + " seconds";
    doc["delay"] = delaySeconds;
    doc["timestamp"] = millis();

    String jsonStr;
    serializeJson(doc, jsonStr);

    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", jsonStr);
    response->addHeader("Connection", "close");

    static int savedDelay = 0;
    savedDelay = delaySeconds;

    request->onDisconnect([this]() {
        SystemRebooter::scheduleReboot("User requested system restart",
                                       savedDelay * 1000UL,
                                       RestartReason::USER_COMMAND);
        LOG_INFO("[System] Connection closed, restart scheduled via SystemRebooter");
    });

    request->send(response);
}

void SystemRouteHandler::handleNetworkConfig(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    auto doc = FastBee::makeJsonDocument(8192);

    if (ctx->networkManager) {
        FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
        WiFiConfig cfg = netMgr->getConfig();

        doc["data"]["device"]["macAddress"] = WiFi.macAddress();
        // deviceName 已移至设备配置，网络配置 GET 不再返回
        doc["data"]["network"]["mode"] = static_cast<uint8_t>(cfg.mode);
        doc["data"]["network"]["networkType"] = static_cast<uint8_t>(cfg.networkType);
        doc["data"]["network"]["ipConfigType"] = static_cast<uint8_t>(cfg.ipConfigType);
        doc["data"]["network"]["enableMDNS"] = cfg.enableMDNS;
        doc["data"]["network"]["customDomain"] = cfg.customDomain;
        doc["data"]["sta"]["ssid"] = cfg.staSSID;
        doc["data"]["sta"]["password"] = cfg.staPassword.length() > 0 ? "********" : "";
        doc["data"]["sta"]["hasPassword"] = cfg.staPassword.length() > 0;
        doc["data"]["sta"]["staticIP"] = cfg.staticIP;
        doc["data"]["sta"]["gateway"] = cfg.gateway;
        doc["data"]["sta"]["subnet"] = cfg.subnet;
        doc["data"]["sta"]["dns1"] = cfg.dns1;
        doc["data"]["sta"]["dns2"] = cfg.dns2;
        doc["data"]["ap"]["ssid"] = cfg.apSSID;
        doc["data"]["ap"]["password"] = cfg.apPassword.length() > 0 ? "********" : "";
        doc["data"]["ap"]["hasPassword"] = cfg.apPassword.length() > 0;
        doc["data"]["ap"]["ip"] = cfg.apIP;
        doc["data"]["ap"]["channel"] = cfg.apChannel;
        doc["data"]["ap"]["hidden"] = cfg.apHidden;
        doc["data"]["ap"]["maxConnections"] = cfg.apMaxConnections;
        doc["data"]["advanced"]["connectTimeout"] = cfg.connectTimeout;
        doc["data"]["advanced"]["reconnectInterval"] = cfg.reconnectInterval;
        doc["data"]["advanced"]["maxReconnectAttempts"] = cfg.maxReconnectAttempts;
        doc["data"]["advanced"]["conflictDetection"] = static_cast<uint8_t>(cfg.conflictDetection);
        doc["data"]["advanced"]["failoverStrategy"] = static_cast<uint8_t>(cfg.failoverStrategy);
        doc["data"]["advanced"]["autoFailover"] = cfg.autoFailover;
        doc["data"]["advanced"]["conflictCheckInterval"] = cfg.conflictCheckInterval;
        doc["data"]["advanced"]["maxFailoverAttempts"] = cfg.maxFailoverAttempts;
        doc["data"]["advanced"]["conflictThreshold"] = cfg.conflictThreshold;
        doc["data"]["advanced"]["fallbackToDHCP"] = cfg.fallbackToDHCP;

        // 浠ュお缃戦厤缃?
        doc["data"]["ethernet"]["spiMosi"] = cfg.ethernet.spiMosi;
        doc["data"]["ethernet"]["spiMiso"] = cfg.ethernet.spiMiso;
        doc["data"]["ethernet"]["spiSck"]  = cfg.ethernet.spiSck;
        doc["data"]["ethernet"]["csPin"]   = cfg.ethernet.csPin;
        doc["data"]["ethernet"]["rstPin"]  = cfg.ethernet.rstPin;
        doc["data"]["ethernet"]["intPin"]  = cfg.ethernet.intPin;

        // 4G 铚傜獫閰嶇疆
        doc["data"]["cellular"]["txPin"]    = cfg.cellular.txPin;
        doc["data"]["cellular"]["rxPin"]    = cfg.cellular.rxPin;
        doc["data"]["cellular"]["pwrPin"]   = cfg.cellular.pwrPin;
        doc["data"]["cellular"]["baudRate"] = cfg.cellular.baudRate;
        doc["data"]["cellular"]["apn"]      = cfg.cellular.apn;

        NetworkStatusInfo info = netMgr->getStatusInfo();
        doc["data"]["status"]["connected"] = (info.status == NetworkStatus::CONNECTED);
        doc["data"]["status"]["statusCode"] = static_cast<uint8_t>(info.status);
        doc["data"]["status"]["ipAddress"] = info.ipAddress;
        doc["data"]["status"]["rssi"] = info.rssi;
        doc["data"]["status"]["ssid"] = info.ssid;
    } else {
        doc["success"] = false;
        doc["error"] = "Network service unavailable";
        HandlerUtils::sendJsonStream(request, doc);
        return;
    }

    doc["success"] = true;
    HandlerUtils::sendJsonStream(request, doc);
}

void SystemRouteHandler::handleSaveNetworkConfig(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    ctx->sendSuccess(request, "Use JSON body for network config update");
}

#if FASTBEE_ENABLE_FILE_MANAGER
void SystemRouteHandler::handleGetFilesList(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    const bool psramBacked = FastBee::psramAvailableForJson(32768);
    if (!psramBacked) {
        if (HandlerUtils::rejectHeavyRequestOnPressure(request, "File list", MemoryGuardLevel::SEVERE, 8)) {
            return;
        }
        if (HandlerUtils::checkLowMemory(request, 4096)) return;
    } else if (ESP.getFreeHeap() < 2048 || ESP.getMaxAllocHeap() < 1024) {
        HandlerUtils::sendJsonError(request, 503, "Critical memory - file list unavailable", 5);
        return;
    }

    String path = "/";
    if (request->hasParam("path")) {
        path = request->getParam("path")->value();
    }
    if (!path.startsWith("/")) {
        path = "/" + path;
    }

    auto doc = FastBee::makeJsonDocument();
    doc["success"] = true;
    doc["data"]["path"] = path;

    File root = LittleFS.open(path);
    if (!root || !root.isDirectory()) {
        doc["success"] = false;
        doc["error"] = "Invalid directory";
        HandlerUtils::sendJsonStream(request, doc);
        return;
    }

    JsonArray files = doc["data"]["files"].to<JsonArray>();
    JsonArray dirs = doc["data"]["dirs"].to<JsonArray>();

    File file = root.openNextFile();
    while (file) {
        JsonObject item;
        if (file.isDirectory()) {
            item = dirs.add<JsonObject>();
        } else {
            item = files.add<JsonObject>();
            item["size"] = file.size();
        }
        item["name"] = String(file.name());
        file = root.openNextFile();
    }
    root.close();

    HandlerUtils::sendJsonStream(request, doc);
}
#endif // FASTBEE_ENABLE_FILE_MANAGER

void SystemRouteHandler::handleGetHealth(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["status"] = "healthy";
    doc["timestamp"] = millis();
    doc["freeHeap"] = ESP.getFreeHeap();
    JsonObject memory = doc["memory"].to<JsonObject>();
    fillMemorySummary(memory);
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    if (monitor) {
        MemoryGuardLevel level = monitor->getMemoryGuardLevel();
        memory["guardLevel"] = HandlerUtils::memoryGuardLevelName(level);
        memory["critical"] = monitor->isMemoryCritical();
    }
    ctx->sendSuccess(request, doc);
}

void SystemRouteHandler::handleSystemMetrics(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    FastBeeFramework* fw = FastBeeFramework::getInstance();
    if (!fw || !fw->getHealthMonitor()) {
        ctx->sendError(request, 503, "Health monitor unavailable");
        return;
    }

    String json = fw->getHealthMonitor()->getMetricsJson();
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
    response->addHeader("Connection", "close");
    request->send(response);
}

void SystemRouteHandler::handleWebRuntime(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    if (request->hasParam("probe")) {
        char json[512];
        unsigned long now = millis();
        snprintf(json, sizeof(json),
            "{\"success\":true,\"data\":{"
            "\"server\":{\"nowMs\":%lu,\"scheduleRestart\":%s,\"scheduledRestartInMs\":%lu},"
            "\"memory\":{\"freeHeap\":%lu,\"minFreeHeap\":%lu,\"maxAlloc\":%lu,\"largestBlock\":%lu},"
            "\"compact\":true,\"probe\":true"
            "}}",
            now,
            ctx->scheduleRestart ? "true" : "false",
            (ctx->scheduleRestart && ctx->scheduledRestartTime > now)
                ? (ctx->scheduledRestartTime - now)
                : 0,
            (unsigned long)ESP.getFreeHeap(),
            (unsigned long)ESP.getMinFreeHeap(),
            (unsigned long)ESP.getMaxAllocHeap(),
            (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        response->addHeader("Connection", "close");
        request->send(response);
        return;
    }

    auto doc = FastBee::makeJsonDocument(24576);
    JsonObject data = doc["data"].to<JsonObject>();
    unsigned long now = millis();

    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t minFreeHeap = ESP.getMinFreeHeap();
    uint32_t maxAlloc = ESP.getMaxAllocHeap();
    uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    bool explicitFull = request->hasParam("full") && request->getParam("full")->value() == "1";
    bool compactRuntime = !explicitFull || freeHeap < 12288 || maxAlloc < 6144;
    uint8_t frag = calculateHeapFragmentationPercent(freeHeap, largestBlock);

    data["server"]["nowMs"] = now;
    data["server"]["scheduleRestart"] = ctx->scheduleRestart;
    data["server"]["scheduledRestartAtMs"] = ctx->scheduledRestartTime;
    data["server"]["scheduledRestartInMs"] =
        (ctx->scheduleRestart && ctx->scheduledRestartTime > now)
            ? (ctx->scheduledRestartTime - now)
            : 0;

    data["memory"]["freeHeap"] = freeHeap;
    data["memory"]["minFreeHeap"] = minFreeHeap;
    data["memory"]["maxAlloc"] = maxAlloc;
    data["memory"]["largestBlock"] = largestBlock;
    data["memory"]["fragmentation"] = frag;

    FastBeeFramework* fw = FastBeeFramework::getInstance();
    WebConfigManager* webCfg = fw ? fw->getWebConfigManager() : nullptr;
    data["server"]["webRunning"] = webCfg ? webCfg->isServerRunning() : false;

    if (fw && fw->getHealthMonitor()) {
        HealthMonitor* hm = fw->getHealthMonitor();
        MemoryGuardLevel level = hm->getMemoryGuardLevel();
        const char* levelText = "NORMAL";
        switch (level) {
            case MemoryGuardLevel::WARN:     levelText = "WARN"; break;
            case MemoryGuardLevel::SEVERE:   levelText = "SEVERE"; break;
            case MemoryGuardLevel::CRITICAL: levelText = "CRITICAL"; break;
            default: break;
        }
        data["memory"]["guardLevel"] = levelText;
        if (!compactRuntime) {
            char healthBuf[192];
            hm->getHealthReport(healthBuf, sizeof(healthBuf));
            data["memory"]["healthReport"] = healthBuf;
        }
    }

    if (webCfg && !compactRuntime) {
        data["web"]["sseClients"] = webCfg->getSseClientCount();
        data["web"]["sseMaxClients"] = static_cast<uint32_t>(SSERouteHandler::MAX_SSE_CLIENTS);
        if (SSERouteHandler* sseHandler = webCfg->getSseRouteHandler()) {
            SSEStatsSnapshot sseStats = sseHandler->getStats();
            JsonObject sseStatsJson = data["web"]["sseStats"].to<JsonObject>();
            sseStatsJson["acceptedConnections"] = sseStats.acceptedConnections;
            sseStatsJson["rejectedLowMemory"] = sseStats.rejectedLowMemory;
            sseStatsJson["rejectedGuard"] = sseStats.rejectedGuard;
            sseStatsJson["rejectedCapacity"] = sseStats.rejectedCapacity;
            sseStatsJson["evictedOldestClients"] = sseStats.evictedOldestClients;
            sseStatsJson["forcedClosedClients"] = sseStats.forcedClosedClients;
            sseStatsJson["timedOutClients"] = sseStats.timedOutClients;
            sseStatsJson["disconnectedCleanups"] = sseStats.disconnectedCleanups;
            sseStatsJson["skippedBroadcastLowMemory"] = sseStats.skippedBroadcastLowMemory;
            sseStatsJson["skippedBroadcastGuard"] = sseStats.skippedBroadcastGuard;
            sseStatsJson["lastConnectAtMs"] = sseStats.lastConnectAtMs;
            sseStatsJson["lastRejectAtMs"] = sseStats.lastRejectAtMs;
            sseStatsJson["lastForcedCloseAtMs"] = sseStats.lastForcedCloseAtMs;
            sseStatsJson["lastCleanupAtMs"] = sseStats.lastCleanupAtMs;
            sseStatsJson["lastSkipBroadcastAtMs"] = sseStats.lastSkipBroadcastAtMs;
            sseStatsJson["lastRejectReason"] = sseStats.lastRejectReason;
            sseStatsJson["lastSkipReason"] = sseStats.lastSkipReason;
        }
        data["recovery"]["softRestartCount"] = webCfg->getSoftRestartCount();
        data["recovery"]["lastSoftRestartAtMs"] = webCfg->getLastSoftRestartAtMs();
        data["recovery"]["lastSoftRestartAgeMs"] =
            (webCfg->getLastSoftRestartAtMs() > 0 && now >= webCfg->getLastSoftRestartAtMs())
                ? (now - webCfg->getLastSoftRestartAtMs())
                : 0;
        data["recovery"]["lastSoftRestartReason"] = webCfg->getLastSoftRestartReason();
        data["recovery"]["lastSoftRestartFreeHeap"] = webCfg->getLastSoftRestartFreeHeap();
        data["recovery"]["lastSoftRestartLargestBlock"] = webCfg->getLastSoftRestartLargestBlock();
        data["recovery"]["lastSoftRestartFragmentation"] = webCfg->getLastSoftRestartFragmentation();
        data["recovery"]["severePressureSinceMs"] = webCfg->getSeverePressureSinceMs();
        data["recovery"]["severePressureDurationMs"] =
            (webCfg->getSeverePressureSinceMs() > 0 && now >= webCfg->getSeverePressureSinceMs())
                ? (now - webCfg->getSeverePressureSinceMs())
                : 0;
        WebRecoveryEvent recoveryEvents[8];
        const size_t recoveryCount = webCfg->copyRecoveryEvents(recoveryEvents, 8);
        JsonArray recoveryEventsJson = data["recovery"]["events"].to<JsonArray>();
        for (size_t i = 0; i < recoveryCount; ++i) {
            JsonObject eventJson = recoveryEventsJson.add<JsonObject>();
            eventJson["type"] = recoveryEvents[i].type;
            eventJson["reason"] = recoveryEvents[i].reason;
            eventJson["atMs"] = recoveryEvents[i].atMs;
            eventJson["ageMs"] =
                (recoveryEvents[i].atMs > 0 && now >= recoveryEvents[i].atMs)
                    ? (now - recoveryEvents[i].atMs)
                    : 0;
            eventJson["freeHeap"] = recoveryEvents[i].freeHeap;
            eventJson["largestBlock"] = recoveryEvents[i].largestBlock;
            eventJson["fragmentation"] = recoveryEvents[i].fragmentation;
            eventJson["sseClients"] = recoveryEvents[i].sseClients;
        }
    }

    if (ctx->networkManager) {
        FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
        if (!compactRuntime) {
            netMgr->updateStatusInfo();
        }
        NetworkStatusInfo info = netMgr->getStatusInfo();
        WiFiConfig cfg = netMgr->getConfig();

        const char* statusText = "unknown";
        switch (info.status) {
            case NetworkStatus::CONNECTED:         statusText = "connected"; break;
            case NetworkStatus::DISCONNECTED:      statusText = "disconnected"; break;
            case NetworkStatus::CONNECTING:        statusText = "connecting"; break;
            case NetworkStatus::AP_MODE:           statusText = "ap_mode"; break;
            case NetworkStatus::CONNECTION_FAILED: statusText = "failed"; break;
            default: break;
        }
        const char* modeText = "unknown";
        switch (cfg.mode) {
            case NetworkMode::NETWORK_STA: modeText = "STA"; break;
            case NetworkMode::NETWORK_AP:  modeText = "AP"; break;
            default: break;
        }

        data["network"]["status"] = statusText;
        data["network"]["mode"] = modeText;
        data["network"]["ipAddress"] = info.ipAddress;
        data["network"]["apIPAddress"] = info.apIPAddress;
        data["network"]["ssid"] = info.ssid.isEmpty() ? cfg.staSSID : info.ssid;
        data["network"]["internetAvailable"] = info.internetAvailable;
        data["network"]["customDomain"] = cfg.customDomain;
        auto* dns = netMgr->getDNSManager();
        if (dns) {
            data["network"]["mdnsDomain"] = dns->getActualHostname();
        }
    }

    data["compact"] = compactRuntime;
    doc["success"] = true;
    HandlerUtils::sendJsonStream(request, doc);
}

void SystemRouteHandler::handleGetCapabilities(AsyncWebServerRequest* request) {
    JsonDocument doc;

    doc["data"]["mqtt"]   = (bool)FASTBEE_ENABLE_MQTT;
    doc["data"]["modbus"] = (bool)FASTBEE_ENABLE_MODBUS;
    doc["data"]["tcp"]    = (bool)FASTBEE_ENABLE_TCP;
    doc["data"]["http"]   = (bool)FASTBEE_ENABLE_HTTP;
    doc["data"]["coap"]   = (bool)FASTBEE_ENABLE_COAP;

    doc["data"]["periphExec"]  = (bool)FASTBEE_ENABLE_PERIPH_EXEC;
    doc["data"]["ruleScript"]  = (bool)FASTBEE_ENABLE_RULE_SCRIPT;
    doc["data"]["lcd"]        = (bool)FASTBEE_ENABLE_LCD;
    doc["data"]["ledScreen"]  = (bool)FASTBEE_ENABLE_LED_SCREEN;

    doc["data"]["ethernet"]     = (bool)FASTBEE_ENABLE_ETHERNET;
    doc["data"]["cellular"]     = (bool)FASTBEE_ENABLE_CELLULAR;
    doc["data"]["ota"]           = (bool)FASTBEE_ENABLE_OTA;
    doc["data"]["auth"]         = (bool)FASTBEE_ENABLE_AUTH;
    doc["data"]["webServer"]    = (bool)FASTBEE_ENABLE_WEB_SERVER;
    doc["data"]["healthMonitor"]= (bool)FASTBEE_ENABLE_HEALTH_MONITOR;
    doc["data"]["logger"]       = (bool)FASTBEE_ENABLE_LOGGER;
    doc["data"]["logViewer"]    = (bool)FASTBEE_ENABLE_LOG_VIEWER;
    doc["data"]["fileLogging"]  = (bool)FASTBEE_ENABLE_FILE_LOGGING;
    doc["data"]["taskManager"]  = (bool)FASTBEE_ENABLE_TASK_MANAGER;
    doc["data"]["i18n"]          = (bool)FASTBEE_ENABLE_I18N;

    doc["success"] = true;
    HandlerUtils::sendJsonStream(request, doc);
}
