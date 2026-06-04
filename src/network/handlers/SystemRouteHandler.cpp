#include "./network/handlers/SystemRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "systems/LoggerSystem.h"
#include "systems/HealthMonitor.h"
#include "core/FastBeeFramework.h"
#include "core/SystemConstants.h"
#include "utils/NetworkUtils.h"
#include "utils/TimeUtils.h"
#include "utils/StaticPoolAllocator.h"
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
constexpr size_t kMaxConfigTransferChunkBytes = 8 * 1024;
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
#if !FASTBEE_ENABLE_ROLE_ADMIN
    if (name == "roles.json") return false;
#endif
#if FASTBEE_SINGLE_ADMIN_MODE || !FASTBEE_ENABLE_USER_ADMIN
    if (name == "users.json") return false;
#endif
    return true;
}

bool isIgnoredConfigImportFileName(const String& name) {
#if !FASTBEE_ENABLE_ROLE_ADMIN
    if (name == "roles.json") return true;
#endif
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
        // Network status handler - reuse network config with status focus
        if (!ctx->checkPermission(request, "network.view")) {
            ctx->sendUnauthorized(request);
            return;
        }
        JsonDocument doc;
        doc["success"] = true;
        if (ctx->networkManager) {
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
            doc["data"]["status"] = statusText;
            doc["data"]["statusCode"] = static_cast<uint8_t>(info.status);
            doc["data"]["ssid"]          = info.ssid.isEmpty() ? cfg.staSSID : info.ssid;
            doc["data"]["ipAddress"]     = info.ipAddress;
            doc["data"]["macAddress"]    = info.macAddress.isEmpty() ? WiFi.macAddress() : info.macAddress;
            doc["data"]["rssi"]          = info.rssi;
            doc["data"]["signalStrength"]= NetworkUtils::rssiToPercentage(info.rssi);
            doc["data"]["gateway"]       = info.currentGateway;
            doc["data"]["subnet"]        = info.currentSubnet;
            doc["data"]["dnsServer"]     = info.dnsServer;
            doc["data"]["dnsServer2"]    = WiFi.status() == WL_CONNECTED ? WiFi.dnsIP(1).toString() : String("");
            doc["data"]["connectedTime"] = info.lastConnectionTime > 0 ? (millis() - info.lastConnectionTime) / 1000 : 0;
            doc["data"]["apIPAddress"]   = info.apIPAddress;
            doc["data"]["apClientCount"] = info.apClientCount;
            doc["data"]["apSSID"]        = cfg.apSSID;
            doc["data"]["apChannel"]     = cfg.apChannel;
            doc["data"]["reconnectAttempts"] = info.reconnectAttempts;
            doc["data"]["uptime"]            = info.uptime;
            doc["data"]["internetAvailable"] = info.internetAvailable;
            doc["data"]["conflictDetected"]  = info.conflictDetected;
            doc["data"]["failoverCount"]     = info.failoverCount;
            doc["data"]["activeIPType"]      = info.activeIPType;
            doc["data"]["txCount"]           = info.txCount;
            doc["data"]["rxCount"]           = info.rxCount;

            // 4G 蜂窝网络状态
            doc["data"]["simStatus"]         = info.simStatus;
            doc["data"]["operator"]          = info.operatorName;
            doc["data"]["networkType"]       = info.cellularNetworkType;
            doc["data"]["apn"]               = info.apn;
            doc["data"]["imei"]              = info.imei;
            doc["data"]["iccId"]             = info.iccid;
            doc["data"]["signalQuality"]     = info.cellularSignalQuality;

            // LoRa 状态
            doc["data"]["loraMode"]          = info.loraMode;
            doc["data"]["loraAddress"]       = info.loraAddress;
            doc["data"]["loraFrequency"]     = info.loraFrequency;
            doc["data"]["loraAirRate"]       = info.loraAirRate;
            doc["data"]["loraChannel"]       = info.loraChannel;
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
            // 返回实际注册的 mDNS hostname（可能带 -2/-3 后缀）
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
        HandlerUtils::sendJsonStream(request, doc);
    });

    server->on("/api/network/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleSaveNetworkConfig(request); });

    // JSON body handler for network config update
    auto* networkJsonHandler = new AsyncCallbackJsonWebHandler("/api/network/config",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            if (!ctx->checkPermission(request, "network.edit")) {
                ctx->sendUnauthorized(request);
                return;
            }
            if (!ctx->networkManager) {
                ctx->sendError(request, 500, "Network service unavailable");
                return;
            }
            NetworkManager* netMgr = static_cast<NetworkManager*>(ctx->networkManager);
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
            // 联网方式
            if (obj["networkType"].is<String>() || obj["networkType"].is<int>()) cfg.networkType = static_cast<NetworkType>(obj["networkType"].as<String>().toInt());
            // 以太网配置
            if (obj["ethernet"].is<JsonObject>()) {
                JsonObject eth = obj["ethernet"];
                if (eth.containsKey("spiMosi")) cfg.ethernet.spiMosi = eth["spiMosi"].as<int8_t>();
                if (eth.containsKey("spiMiso")) cfg.ethernet.spiMiso = eth["spiMiso"].as<int8_t>();
                if (eth.containsKey("spiSck"))  cfg.ethernet.spiSck  = eth["spiSck"].as<int8_t>();
                if (eth.containsKey("csPin"))   cfg.ethernet.csPin   = eth["csPin"].as<int8_t>();
                if (eth.containsKey("rstPin"))  cfg.ethernet.rstPin  = eth["rstPin"].as<int8_t>();
                if (eth.containsKey("intPin"))  cfg.ethernet.intPin  = eth["intPin"].as<int8_t>();
            }
            // 4G 蜂窝配置
            if (obj["cellular"].is<JsonObject>()) {
                JsonObject cell = obj["cellular"];
                if (cell.containsKey("txPin"))    cfg.cellular.txPin    = cell["txPin"].as<int8_t>();
                if (cell.containsKey("rxPin"))    cfg.cellular.rxPin    = cell["rxPin"].as<int8_t>();
                if (cell.containsKey("pwrPin"))   cfg.cellular.pwrPin   = cell["pwrPin"].as<int8_t>();
                if (cell.containsKey("baudRate")) cfg.cellular.baudRate = cell["baudRate"].as<uint32_t>();
                if (cell.containsKey("apn"))      cfg.cellular.apn      = cell["apn"].as<String>();
            }
            // LoRa 配置
            if (obj["lora"].is<JsonObject>()) {
                JsonObject lora = obj["lora"];
                if (lora.containsKey("txPin"))    cfg.lora.txPin    = lora["txPin"].as<int8_t>();
                if (lora.containsKey("rxPin"))    cfg.lora.rxPin    = lora["rxPin"].as<int8_t>();
                if (lora.containsKey("m1Pin"))    cfg.lora.m1Pin    = lora["m1Pin"].as<int8_t>();
                if (lora.containsKey("baudRate")) cfg.lora.baudRate = lora["baudRate"].as<uint32_t>();
            }
            if (netMgr->updateConfig(cfg, true)) {
                LOGGER.info("Network configuration updated via web");
                
                // 构建详细响应，包含网络模式和访问信息
                JsonDocument respDoc;
                respDoc["success"] = true;
                respDoc["message"] = "Network configuration saved successfully";
                
                // 网络模式信息
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
                    respDoc["data"]["apIP"] = cfg.apIP;  // 配置的AP IP
                }
                
                // mDNS信息（STA模式需要）
                if (cfg.mode == NetworkMode::NETWORK_STA && cfg.enableMDNS) {
                    String domain = cfg.customDomain.length() > 0 ? cfg.customDomain : "fastbee";
                    respDoc["data"]["mdnsDomain"] = domain + ".local";
                }
                
                // 提示网络重启需要时间
                respDoc["data"]["restartRequired"] = true;
                
                HandlerUtils::sendJsonStream(request, respDoc);
            } else {
                ctx->sendError(request, 500, "Failed to save network configuration");
            }
        });
    networkJsonHandler->setMethod(HTTP_POST | HTTP_PUT);
    server->addHandler(networkJsonHandler);

#if FASTBEE_ENABLE_CONFIG_TRANSFER
    server->on("/api/config/transfer/list", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "config.view")) { ctx->sendUnauthorized(request); return; }
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
        if (!ctx->checkPermission(request, "config.view")) { ctx->sendUnauthorized(request); return; }
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
        if (!ctx->checkPermission(request, "config.edit")) { ctx->sendUnauthorized(request); return; }
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
        if (!ctx->checkPermission(request, "config.edit")) { ctx->sendUnauthorized(request); return; }
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
        if (!ctx->checkPermission(request, "system.view")) { ctx->sendUnauthorized(request); return; }
        if (!request->hasParam("path")) { ctx->sendError(request, 400, "Missing path parameter"); return; }
        String path = request->getParam("path")->value();
        if (!path.startsWith("/")) path = "/" + path;
        if (path.indexOf("..") >= 0) { ctx->sendError(request, 403, "Invalid path"); return; }
        if (!LittleFS.exists(path)) { ctx->sendError(request, 404, "File not found"); return; }
        File file = LittleFS.open(path, "r");
        if (!file) { ctx->sendError(request, 500, "Failed to open file"); return; }
        size_t fileSize = file.size();
        if (fileSize > 128 * 1024) { file.close(); ctx->sendError(request, 413, "File too large (max 128KB)"); return; }

        // ★ Raw 快速路径：raw=1 时直接 chunked 流式回传文件原始字节（无 JSON 包装、无转义）
        // 用于配置文件导出场景：即使设备处于 MemGuard CRITICAL 级也能成功下载，
        // 内存开销仅~256B TCP 发送缓冲 + File 句柄，脱敏由前端在浏览器本地完成。
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

        // 文件分级（用于 MemGuard 策略决策）：
        // - isTinyFile (≤ 4KB)：极小文件（device.json/network.json）
        // - isConfigFile (/config/ 下 ≤ 16KB)：受保护配置文件（peripherals/protocol/periph_exec）
        // - 其它：普通文件
        const bool isTinyFile = (fileSize <= 4096);
        const bool isConfigFile = path.startsWith("/config/") && (fileSize <= 16 * 1024);
        const bool isProtectedFile = isTinyFile || isConfigFile;

        // MemGuard 策略（chunked 流式响应，内存占用恒定 ~256B 缓冲 + 少量 state）：
        // - 受保护文件（config/tiny）：无视 MemGuard 级别，始终放行（CRITICAL 级下也能救援导出）
        // - 非受保护文件：CRITICAL 级全拒；SEVERE 级拒绝（避免大文件拖垮已濒临 OOM 的系统）
        if (!isProtectedFile && HandlerUtils::rejectHeavyRequestOnPressure(
                request,
                rawDownload ? "Raw file download" : "File content",
                MemoryGuardLevel::SEVERE,
                8)) {
            file.close();
            return;
        }

        // 堆内存最小阈值（chunked 流式仅需极少内存即可发送）：
        // - 受保护文件：free ≥ 6KB, maxAlloc ≥ 2KB（仅需状态结构 + TCP 发送缓冲）
        // - 非受保护大文件：free ≥ 20KB, maxAlloc ≥ 8KB
        size_t freeHeap = ESP.getFreeHeap();
        size_t maxAlloc = ESP.getMaxAllocHeap();
        size_t minFreeHeap = isProtectedFile ? 6000  : 20000;
        size_t minMaxAlloc = isProtectedFile ? 2000  : 8000;
        if (freeHeap < minFreeHeap || maxAlloc < minMaxAlloc) {
            file.close();
            char msg[96];
            snprintf(msg, sizeof(msg), "Low memory: heap=%u maxAlloc=%u", (unsigned)freeHeap, (unsigned)maxAlloc);
            HandlerUtils::sendJsonError(request, 503, msg);
            return;
        }

        // ============ Chunked 流式响应状态机 ============
        // 避免一次性分配大 String，让 CRITICAL 级下的配置文件也能完整导出。
        // 阶段：0=header, 1=content（读文件+转义）, 2=trailer "\"}}", 3=done
        struct FileStreamState {
            File file;
            size_t fileSize = 0;
            size_t rawPos = 0;
            String header;
            size_t headerPos = 0;
            size_t trailerPos = 0;
            // pendingBuf 缓存：当 buffer 剩余空间不足以写入 2 字节转义时，暂存到下次回调
            uint8_t pendingBuf[4] = {0};
            size_t pendingLen = 0;
            size_t pendingHead = 0;
            uint8_t phase = 0;
        };

        auto state = std::make_shared<FileStreamState>();
        state->file = file;  // 移交文件句柄所有权
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
                // ---- Phase 1: 文件内容转义 ----
                if (state->phase == 1) {
                    // 优先消耗上次未能写入的转义字节
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
                            if (escLen == 0) continue;  // 控制字符直接丢弃
                            if (written + escLen <= maxLen) {
                                for (size_t j = 0; j < escLen; j++) buffer[written++] = esc[j];
                            } else {
                                // buffer 装不下完整转义序列，暂存到 pendingBuf
                                for (size_t j = 0; j < escLen; j++) {
                                    state->pendingBuf[state->pendingLen++] = esc[j];
                                }
                            }
                        }
                        // buffer 写满且 pending 非空：退出，下次再来
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
        if (!ctx->checkPermission(request, "config.edit")) { ctx->sendUnauthorized(request); return; }
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

            // 导入/保存配置后回填身份字段：当写入 device.json 或 protocol.json 时，
            // 复用启动阶段的 ensureDeviceIdentity() 自动填充空的 deviceId / mqtt.clientId，
            // 确保跨设备导入也能正确生成一致的设备标识。
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
        if (!ctx->checkPermission(request, "fs.view")) { ctx->sendUnauthorized(request); return; }
        if (HandlerUtils::checkLowMemory(request, 6144)) return;
        JsonDocument doc;
        size_t total = LittleFS.totalBytes();
        size_t used  = LittleFS.usedBytes();
        doc["totalBytes"] = total;
        doc["usedBytes"]  = used;
        doc["freeBytes"]  = total > used ? total - used : 0;
        doc["usedPercent"]= total > 0 ? (used * 100 / total) : 0;
        ctx->sendSuccess(request, doc);
    });
#endif // FASTBEE_ENABLE_FILE_MANAGER

    // Factory reset
    server->on("/api/system/factory-reset", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "system.restart")) { ctx->sendUnauthorized(request); return; }
        LOGGER.info("Factory reset initiated by user");
        const char* configFiles[] = {
            "/config/device.json", "/config/network.json", "/config/protocol.json",
            "/config/users.json", "/config/auth.json", "/config/system.json",
            "/config/http.json", "/config/mqtt.json", "/config/tcp.json",
            "/config/modbus.json", "/config/coap.json",
            "/config/roles.json", "/config/peripherals.json", "/config/periph_exec.json"
        };
        int deletedCount = 0;
        for (int i = 0; i < (int)(sizeof(configFiles) / sizeof(configFiles[0])); i++) {
            if (LittleFS.exists(configFiles[i])) {
                if (LittleFS.remove(configFiles[i])) { deletedCount++; LOGGER.infof("Deleted config file: %s", configFiles[i]); }
            }
        }
#if FASTBEE_ENABLE_FILE_LOGGING || FASTBEE_ENABLE_LOG_VIEWER
        if (LittleFS.exists("/logs/system.log")) {
            File logFile = LittleFS.open("/logs/system.log", "w");
            if (logFile) { logFile.close(); LOGGER.info("Cleared system log file"); }
        }
#endif
        JsonDocument doc;
        doc["success"] = true;
        doc["message"] = "Factory reset completed. Device will restart.";
        doc["deletedFiles"] = deletedCount;
        doc["timestamp"] = millis();
        String jsonStr; serializeJson(doc, jsonStr);
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", jsonStr);
        response->addHeader("Connection", "close");
        request->onDisconnect([this]() {
            LOGGER.info("Factory reset response sent, scheduling restart in 2 seconds");
            ctx->scheduleRestart = true;
            ctx->scheduledRestartTime = millis() + 2000;
        });
        request->send(response);
    });

    // Config
    server->on("/api/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "config.view")) { ctx->sendUnauthorized(request); return; }
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
        if (!ctx->checkPermission(request, "config.edit")) { ctx->sendUnauthorized(request); return; }
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
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    FastBeeFramework* fw = FastBeeFramework::getInstance();
    bool forceBrief = false;
    if (fw && fw->getHealthMonitor()) {
        MemoryGuardLevel level = fw->getHealthMonitor()->getMemoryGuardLevel();
        if (level >= MemoryGuardLevel::CRITICAL) {
            HandlerUtils::sendJsonError(request, 503, "Critical memory - system info unavailable");
            return;
        }
        if (level >= MemoryGuardLevel::SEVERE) {
            forceBrief = true;
        }
    }

    bool brief = forceBrief || request->hasParam("brief");
    if (HandlerUtils::checkLowMemory(request)) return;

    JsonDocument doc;

    if (brief) {
        doc["data"]["device"]["chipModel"] = ESP.getChipModel();
        doc["data"]["device"]["freeHeap"] = ESP.getFreeHeap();
        doc["data"]["device"]["firmwareVersion"] = SystemInfo::VERSION;

        size_t freeHeap = ESP.getFreeHeap();
        size_t heapSize = ESP.getHeapSize();
        doc["data"]["memory"]["heapFree"] = freeHeap;
        doc["data"]["memory"]["heapUsagePercent"] = (int)(((heapSize - freeHeap) * 100) / heapSize);

        unsigned long uptime = millis();
        doc["data"]["uptime"]["seconds"] = uptime / 1000;

        if (ctx->networkManager) {
            NetworkStatusInfo info = ctx->networkManager->getStatusInfo();
            doc["data"]["network"]["connected"] = (info.status == NetworkStatus::CONNECTED);
            doc["data"]["network"]["ipAddress"] = info.ipAddress;
        }

        doc["data"]["brief"] = true;
        doc["success"] = true;
        HandlerUtils::sendJsonStream(request, doc);
        return;
    }

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

    size_t heapSize = ESP.getHeapSize();
    size_t freeHeap = ESP.getFreeHeap();
    size_t minFreeHeap = ESP.getMinFreeHeap();
    doc["data"]["memory"]["heapTotal"] = heapSize;
    doc["data"]["memory"]["heapFree"] = freeHeap;
    doc["data"]["memory"]["heapUsed"] = heapSize - freeHeap;
    doc["data"]["memory"]["heapMinFree"] = minFreeHeap;
    doc["data"]["memory"]["heapMaxAlloc"] = ESP.getMaxAllocHeap();
    doc["data"]["memory"]["heapUsagePercent"] = (int)(((heapSize - freeHeap) * 100) / heapSize);

    // 静态池利用率监控（T1+T2 碎片治理成果可观测点）
    // 三个池：
    //   - (256, 32): peripherals + runtimeStates 节点池 （8KB DRAM）
    //   - (192, 32): rules 节点池 （6KB DRAM）
    //   - (64, 64) : SmallNodePool 高频小容器节点池 （4KB DRAM）
    JsonObject pools = doc["data"]["memory"]["staticPools"].to<JsonObject>();
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

    size_t psramSize = ESP.getPsramSize();
    if (psramSize > 0) {
        doc["data"]["memory"]["psramTotal"] = psramSize;
        doc["data"]["memory"]["psramFree"] = ESP.getFreePsram();
        doc["data"]["memory"]["psramMinFree"] = ESP.getMinFreePsram();
    }

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

    JsonDocument doc;
    doc["status"]    = "running";
    doc["timestamp"] = millis();
    doc["freeHeap"]  = ESP.getFreeHeap();
    doc["uptime"]    = millis();

    if (ctx->networkManager) {
        NetworkStatusInfo info = ctx->networkManager->getStatusInfo();
        doc["networkConnected"] = (info.status == NetworkStatus::CONNECTED);
        doc["ipAddress"]        = info.ipAddress;
        doc["ssid"]             = info.ssid;
        doc["rssi"]             = info.rssi;
    }

    JsonDocument wrapper;
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
    if (!ctx->checkPermission(request, "system.restart")) {
        ctx->sendUnauthorized(request);
        return;
    }

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
        ctx->scheduleRestart = true;
        ctx->scheduledRestartTime = millis() + (savedDelay * 1000);
        LOG_INFO("[System] Connection closed, restart scheduled");
    });

    request->send(response);
}

void SystemRouteHandler::handleNetworkConfig(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "network.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    JsonDocument doc;

    if (ctx->networkManager) {
        NetworkManager* netMgr = static_cast<NetworkManager*>(ctx->networkManager);
        WiFiConfig cfg = netMgr->getConfig();

        doc["data"]["device"]["macAddress"] = WiFi.macAddress();
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

        // 以太网配置
        doc["data"]["ethernet"]["spiMosi"] = cfg.ethernet.spiMosi;
        doc["data"]["ethernet"]["spiMiso"] = cfg.ethernet.spiMiso;
        doc["data"]["ethernet"]["spiSck"]  = cfg.ethernet.spiSck;
        doc["data"]["ethernet"]["csPin"]   = cfg.ethernet.csPin;
        doc["data"]["ethernet"]["rstPin"]  = cfg.ethernet.rstPin;
        doc["data"]["ethernet"]["intPin"]  = cfg.ethernet.intPin;

        // 4G 蜂窝配置
        doc["data"]["cellular"]["txPin"]    = cfg.cellular.txPin;
        doc["data"]["cellular"]["rxPin"]    = cfg.cellular.rxPin;
        doc["data"]["cellular"]["pwrPin"]   = cfg.cellular.pwrPin;
        doc["data"]["cellular"]["baudRate"] = cfg.cellular.baudRate;
        doc["data"]["cellular"]["apn"]      = cfg.cellular.apn;

        // LoRa 配置
        doc["data"]["lora"]["txPin"]    = cfg.lora.txPin;
        doc["data"]["lora"]["rxPin"]    = cfg.lora.rxPin;
        doc["data"]["lora"]["m1Pin"]    = cfg.lora.m1Pin;
        doc["data"]["lora"]["baudRate"] = cfg.lora.baudRate;

        NetworkStatusInfo info = netMgr->getStatusInfo();
        doc["data"]["status"]["connected"] = (info.status == NetworkStatus::CONNECTED);
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
    if (!ctx->checkPermission(request, "network.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    ctx->sendSuccess(request, "Use JSON body for network config update");
}

#if FASTBEE_ENABLE_FILE_MANAGER
void SystemRouteHandler::handleGetFilesList(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "File list", MemoryGuardLevel::SEVERE, 8)) {
        return;
    }
    if (HandlerUtils::checkLowMemory(request, 8192)) return;

    String path = "/";
    if (request->hasParam("path")) {
        path = request->getParam("path")->value();
    }
    if (!path.startsWith("/")) {
        path = "/" + path;
    }

    JsonDocument doc;
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
    ctx->sendSuccess(request, doc);
}

void SystemRouteHandler::handleSystemMetrics(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

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
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    JsonDocument doc;
    JsonObject data = doc["data"].to<JsonObject>();
    unsigned long now = millis();

    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t minFreeHeap = ESP.getMinFreeHeap();
    uint32_t maxAlloc = ESP.getMaxAllocHeap();
    uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    uint8_t frag = (freeHeap > 0)
        ? static_cast<uint8_t>(100U - (largestBlock * 100U / freeHeap))
        : 0;

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
        char healthBuf[192];
        hm->getHealthReport(healthBuf, sizeof(healthBuf));
        data["memory"]["healthReport"] = healthBuf;
    }

    if (webCfg) {
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
        NetworkManager* netMgr = static_cast<NetworkManager*>(ctx->networkManager);
        netMgr->updateStatusInfo();
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

    doc["data"]["ota"]           = (bool)FASTBEE_ENABLE_OTA;
    doc["data"]["auth"]         = (bool)FASTBEE_ENABLE_AUTH;
    doc["data"]["webServer"]    = (bool)FASTBEE_ENABLE_WEB_SERVER;
    doc["data"]["healthMonitor"]= (bool)FASTBEE_ENABLE_HEALTH_MONITOR;
    doc["data"]["logger"]       = (bool)FASTBEE_ENABLE_LOGGER;
    doc["data"]["logViewer"]    = (bool)FASTBEE_ENABLE_LOG_VIEWER;
    doc["data"]["fileLogging"]  = (bool)FASTBEE_ENABLE_FILE_LOGGING;
    doc["data"]["taskManager"]  = (bool)FASTBEE_ENABLE_TASK_MANAGER;

    doc["success"] = true;
    HandlerUtils::sendJsonStream(request, doc);
}
