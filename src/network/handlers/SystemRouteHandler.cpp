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
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <memory>

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
                    respDoc["data"]["apIP"] = "192.168.4.1";  // 默认AP IP
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
        FastBeeFramework* fw = FastBeeFramework::getInstance();
        if (fw && fw->getHealthMonitor() && !isProtectedFile) {
            MemoryGuardLevel level = fw->getHealthMonitor()->getMemoryGuardLevel();
            if (level >= MemoryGuardLevel::CRITICAL) {
                file.close();
                HandlerUtils::sendJsonError(request, 503, "Device memory critical, file read unavailable");
                return;
            }
            if (level >= MemoryGuardLevel::SEVERE) {
                file.close();
                HandlerUtils::sendJsonError(request, 503, "Device memory low, large file read temporarily unavailable");
                return;
            }
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
        JsonDocument doc;
        size_t total = LittleFS.totalBytes();
        size_t used  = LittleFS.usedBytes();
        doc["totalBytes"] = total;
        doc["usedBytes"]  = used;
        doc["freeBytes"]  = total > used ? total - used : 0;
        doc["usedPercent"]= total > 0 ? (used * 100 / total) : 0;
        ctx->sendSuccess(request, doc);
    });

    // Factory reset
    server->on("/api/system/factory-reset", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "system.restart")) { ctx->sendUnauthorized(request); return; }
        LOGGER.info("Factory reset initiated by user");
        const char* configFiles[] = {
            "/config/device.json", "/config/network.json", "/config/protocol.json",
            "/config/users.json", "/config/system.json",
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
        if (LittleFS.exists("/logs/system.log")) {
            File logFile = LittleFS.open("/logs/system.log", "w");
            if (logFile) { logFile.close(); LOGGER.info("Cleared system log file"); }
        }
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

    if (ctx->userManager) {
        doc["data"]["users"]["total"] = ctx->userManager->getUserCount();
    }
    if (ctx->authManager) {
        doc["data"]["users"]["activeSessions"] = ctx->authManager->getActiveSessionCount();
        doc["data"]["users"]["online"] = ctx->authManager->getOnlineUserCount();
    }

    doc["success"] = true;
    HandlerUtils::sendJsonStream(request, doc);
}

void SystemRouteHandler::handleSystemStatus(AsyncWebServerRequest* request) {
    unsigned long now = millis();
    if (_statusCache.valid && (now - _statusCache.timestamp) < STATUS_CACHE_TTL) {
        request->send(200, "application/json", _statusCache.json);
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
    request->send(200, "application/json", _statusCache.json);
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

void SystemRouteHandler::handleGetFilesList(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

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
    request->send(200, "application/json", json);
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
    doc["data"]["taskManager"]  = (bool)FASTBEE_ENABLE_TASK_MANAGER;

    doc["success"] = true;
    HandlerUtils::sendJsonStream(request, doc);
}
