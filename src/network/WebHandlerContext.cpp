#include "./network/WebHandlerContext.h"
#include "./security/AuthManager.h"
#include "systems/LoggerSystem.h"
#include "systems/HealthMonitor.h"
#include "core/FastBeeFramework.h"

WebHandlerContext::WebHandlerContext(AsyncWebServer* srv, IAuthManager* authMgr, IUserManager* userMgr)
    : server(srv), authManager(authMgr), userManager(userMgr),
      roleManager(nullptr), networkManager(nullptr), otaManager(nullptr),
      protocolManager(nullptr), sseHandler(nullptr), webRootPath("/www"),
      scheduleRestart(false), scheduledRestartTime(0), cacheDuration(86400) {
    loadCacheDuration();
}

// ============ 参数处理辅助方法 ============

String WebHandlerContext::getParamValue(AsyncWebServerRequest* request, const String& paramName,
                                        const String& defaultValue) {
    if (!request) return defaultValue;

    // 优先从 POST body（表单参数）取，再从 URL query string 取
    if (request->hasParam(paramName, true)) {
        const AsyncWebParameter* param = request->getParam(paramName, true);
        return param->value();
    }
    if (request->hasParam(paramName, false)) {
        const AsyncWebParameter* param = request->getParam(paramName, false);
        return param->value();
    }
    return defaultValue;
}

bool WebHandlerContext::getParamBool(AsyncWebServerRequest* request, const String& paramName,
                                     bool defaultValue) {
    String value = getParamValue(request, paramName, defaultValue ? "true" : "false");
    return value.equalsIgnoreCase("true") || value == "1";
}

int WebHandlerContext::getParamInt(AsyncWebServerRequest* request, const String& paramName,
                                   int defaultValue) {
    String value = getParamValue(request, paramName, String(defaultValue));
    return value.toInt();
}

// ============ 认证辅助方法 ============

bool WebHandlerContext::requiresAuth(AsyncWebServerRequest* request) {
    const char* publicPaths[] = {
        "/api/auth/login",
        "/api/health",
        "/api/events",  // SSE 推送通道，EventSource 不支持 Cookie，需跳过 session 认证
        "/login",
        "/css/",
        "/js/",
        "/images/",
        "/assets/"
    };

    String path = request->url();

    for (const char* publicPath : publicPaths) {
        if (path.startsWith(publicPath)) {
            return false;
        }
    }

    return true;
}

AuthResult WebHandlerContext::authenticateRequest(AsyncWebServerRequest* request) {
    if (!authManager) {
        return AuthResult();
    }

    String sessionId = AuthManager::extractSessionIdFromRequest(request);
    if (sessionId.isEmpty()) {
        return AuthResult();
    }

    return authManager->verifySession(sessionId, true);
}

String WebHandlerContext::getClientIP(AsyncWebServerRequest* request) {
    if (!request) return "";

    if (request->hasHeader("X-Forwarded-For")) {
        return request->header("X-Forwarded-For");
    }

    if (request->hasHeader("X-Real-IP")) {
        return request->header("X-Real-IP");
    }

    return request->client()->remoteIP().toString();
}

String WebHandlerContext::getUserAgent(AsyncWebServerRequest* request) {
    if (!request) return "";

    if (request->hasHeader("User-Agent")) {
        return request->header("User-Agent");
    }

    return "";
}

bool WebHandlerContext::checkPermission(AsyncWebServerRequest* request, const String& permission) {
    if (!authManager || !request) {
        return false;
    }

    AuthResult authResult = authenticateRequest(request);
    if (!authResult.success) {
        return false;
    }

    // 直接用已认证的 username 检查权限，避免 checkSessionPermission 中重复调用 verifySession
    return authManager->checkPermission(authResult.username, permission);
}

// ============ 内存保护辅助 ============

static bool canAllocateResponse(size_t requiredSize) {
    auto* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    if (!monitor) return true;
    SystemHealth health = monitor->getHealthStatus();
    // 预留 512 字节安全余量，防止序列化过程中的临时分配
    size_t safeSize = requiredSize + 512;
    if (health.largestFreeBlock < safeSize) {
        Serial.printf("[MemGuard] Rejecting response: need ~%u bytes, largest block=%lu\n",
                      (unsigned)safeSize, (unsigned long)health.largestFreeBlock);
        return false;
    }
    return true;
}

static void sendServiceUnavailable(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(503, "text/plain",
                                                              "Service Unavailable - Low Memory");
    response->addHeader("Retry-After", "5");
    request->send(response);
}

// ============ 响应辅助方法 ============

void WebHandlerContext::sendJsonResponse(AsyncWebServerRequest* request, int code,
                                         const JsonDocument& doc) {
    if (!request) return;

    size_t docSize = measureJson(doc);

    // MemGuard: CRITICAL 时仅拒绝大响应（>1KB），小响应放行以确保基础 API（如 auth/session）可用
    auto* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    if (monitor && monitor->isMemoryCritical() && docSize > 1024) {
        sendServiceUnavailable(request);
        return;
    }
    // MemGuard: 检查最大可分配块是否足够
    if (!canAllocateResponse(docSize)) {
        sendServiceUnavailable(request);
        return;
    }
    AsyncWebServerResponse* response;

    if (docSize < 512) {
        char jsonBuffer[512];
        serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
        response = request->beginResponse(code, "application/json", jsonBuffer);
    } else {
        String jsonStr;
        jsonStr.reserve(docSize + 1);
        serializeJson(doc, jsonStr);
        response = request->beginResponse(code, "application/json", jsonStr);
    }

    // CORS 头已由 DefaultHeaders 全局注入，无需重复设置
    request->send(response);
}

void WebHandlerContext::sendSuccess(AsyncWebServerRequest* request, const JsonDocument& data) {
    // MemGuard: CRITICAL 时仅拒绝大响应（>1KB），小响应放行
    auto* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    size_t estimatedSize = measureJson(data);
    if (monitor && monitor->isMemoryCritical() && estimatedSize > 1024) {
        sendServiceUnavailable(request);
        return;
    }
    // MemGuard: 检查最大可分配块是否足够（含 wrapper 开销 ~48 字节 + 安全余量）
    if (!canAllocateResponse(estimatedSize + 64)) {
        sendServiceUnavailable(request);
        return;
    }

    // 注意：ArduinoJson v7 的 serializeJson(doc, String) 会先清空目标字符串，
    // 因此必须先序列化到独立字符串，再拼接到响应中。
    String dataStr;
    if (!data.isNull()) {
        serializeJson(data, dataStr);
    }

    String out;
    out.reserve(dataStr.length() + 48);
    out = "{\"success\":true,\"timestamp\":";
    out += String(millis());
    if (dataStr.length() > 0) {
        out += ",\"data\":";
        out += dataStr;
    }
    out += "}";

    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", out);
    request->send(response);
}

void WebHandlerContext::sendSuccess(AsyncWebServerRequest* request, const String& message) {
    char jsonBuffer[256];
    if (message.isEmpty()) {
        snprintf(jsonBuffer, sizeof(jsonBuffer), "{\"success\":true,\"timestamp\":%lu}", millis());
    } else {
        snprintf(jsonBuffer, sizeof(jsonBuffer), "{\"success\":true,\"message\":\"%s\",\"timestamp\":%lu}",
                 message.c_str(), millis());
    }

    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", jsonBuffer);
    request->send(response);
}

void WebHandlerContext::sendError(AsyncWebServerRequest* request, int code, const String& message) {
    char jsonBuffer[256];
    snprintf(jsonBuffer, sizeof(jsonBuffer),
             "{\"success\":false,\"error\":\"%s\",\"code\":%d,\"timestamp\":%lu}",
             message.c_str(), code, millis());

    AsyncWebServerResponse* response = request->beginResponse(code, "application/json", jsonBuffer);
    request->send(response);
}

void WebHandlerContext::sendUnauthorized(AsyncWebServerRequest* request) {
    sendError(request, 401, "Unauthorized");
}

void WebHandlerContext::sendForbidden(AsyncWebServerRequest* request) {
    sendError(request, 403, "Forbidden");
}

void WebHandlerContext::sendNotFound(AsyncWebServerRequest* request) {
    sendError(request, 404, "Not Found");
}

void WebHandlerContext::sendBadRequest(AsyncWebServerRequest* request, const String& message) {
    sendError(request, 400, message);
}

// ============ 内置页面 ============

void WebHandlerContext::sendBuiltinSetupPage(AsyncWebServerRequest* request) {
    // 优先从 LittleFS 提供（节省 ~4KB 固件 Flash）
    if (LittleFS.exists("/www/setup.html.gz")) {
        request->send(LittleFS, "/www/setup.html.gz", "text/html");
    } else if (LittleFS.exists("/www/setup.html")) {
        request->send(LittleFS, "/www/setup.html", "text/html");
    } else {
        request->send(200, "text/html", "<html><body><h1>Setup page not found in filesystem</h1><p>Please upload LittleFS image first.</p></body></html>");
    }
}

// ============ 文件服务方法 ============

bool WebHandlerContext::serveStaticFile(AsyncWebServerRequest* request, const String& path) {
    String ext = path.substring(path.lastIndexOf('.'));
    String contentType = getContentType(path);
    bool isCompressible = (ext == ".html" || ext == ".js" || ext == ".css");

    // 优先尝试 .gz 压缩版本（直接 open 代替 exists + open 双次IO）
    if (isCompressible) {
        String gzPath = path + ".gz";
        File gzFile = LittleFS.open(gzPath, "r");
        if (gzFile) {
            size_t fileSize = gzFile.size();
            gzFile.close();

            // ETag 基于文件大小（flash 上文件稳定后大小不变）
            String etag = "\"" + String(fileSize, HEX) + "\"";

            // 在创建 response 之前检查缓存命中（避免 beginResponse 打开文件后又丢弃）
            if (request->hasHeader("If-None-Match")) {
                if (request->header("If-None-Match") == etag) {
                    request->send(304);
                    return true;
                }
            }

            AsyncWebServerResponse *response = request->beginResponse(LittleFS, gzPath, contentType);
            response->addHeader("Content-Encoding", "gzip");
            response->addHeader("ETag", etag);

            // 添加 Link header 用于资源预加载（仅对 index.html）
            if (path == "/www/index.html" || path == "/index.html") {
                response->addHeader("Link", "</js/state.js>; rel=preload; as=script, </css/main.css>; rel=preload; as=style");
            }
            // Service Worker 文件特殊处理
            if (path == "/www/sw.js" || path == "/sw.js") {
                response->addHeader("Service-Worker-Allowed", "/");
                response->addHeader("Cache-Control", "no-cache");  // SW 文件不缓存，确保更新
            }
            // pages 目录：长缓存（内容几乎不变）
            if (path.startsWith("/www/pages/")) {
                response->addHeader("Cache-Control", "public, max-age=86400, must-revalidate");
            } else if (ext == ".html") {
                response->addHeader("Cache-Control", "public, max-age=3600, must-revalidate");
            } else {
                // JS/CSS 静态资源：1天缓存
                response->addHeader("Cache-Control", "public, max-age=86400, must-revalidate");
            }
            response->addHeader("Vary", "Accept-Encoding");

            request->send(response);
            return true;
        }
    }

    // 尝试原始文件（直接 open，一次 IO）
    File file = LittleFS.open(path, "r");
    if (file) {
        size_t fileSize = file.size();
        file.close();

        String etag = "\"" + String(fileSize, HEX) + "\"";

        // 在创建 response 之前检查缓存命中
        if (request->hasHeader("If-None-Match")) {
            if (request->header("If-None-Match") == etag) {
                request->send(304);
                return true;
            }
        }

        AsyncWebServerResponse *response = request->beginResponse(LittleFS, path, contentType);
        response->addHeader("ETag", etag);

        // 图片等不可压缩资源缓存7天
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" ||
            ext == ".ico" || ext == ".svg" || ext == ".woff" || ext == ".woff2") {
            response->addHeader("Cache-Control", "public, max-age=604800, immutable");
        } else {
            response->addHeader("Cache-Control", "no-cache");
        }

        request->send(response);
        return true;
    }

    return false;
}

void WebHandlerContext::serveGzippedFile(AsyncWebServerRequest* request, const String& path) {
    // 统一使用 serveStaticFile，已包含 gzip 和缓存逻辑
    if (serveStaticFile(request, path)) return;
    sendNotFound(request);
}

String WebHandlerContext::getContentType(const String& filename) {
    if (filename.endsWith(".html")) return "text/html";
    if (filename.endsWith(".css")) return "text/css";
    if (filename.endsWith(".js")) return "application/javascript";
    if (filename.endsWith(".png")) return "image/png";
    if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) return "image/jpeg";
    if (filename.endsWith(".gif")) return "image/gif";
    if (filename.endsWith(".ico")) return "image/x-icon";
    if (filename.endsWith(".svg")) return "image/svg+xml";
    if (filename.endsWith(".json")) return "application/json";
    if (filename.endsWith(".xml")) return "application/xml";
    if (filename.endsWith(".pdf")) return "application/pdf";
    if (filename.endsWith(".zip")) return "application/zip";
    if (filename.endsWith(".gz")) return "application/x-gzip";
    if (filename.endsWith(".txt")) return "text/plain";
    return "application/octet-stream";
}

String WebHandlerContext::readFile(const String& path) {
    if (!LittleFS.exists(path)) {
        return "";
    }

    File file = LittleFS.open(path, "r");
    if (!file) {
        return "";
    }

    String content = file.readString();
    file.close();
    return content;
}

bool WebHandlerContext::fileExists(const String& path) {
    return LittleFS.exists(path);
}

// ============ 工具方法 ============

String WebHandlerContext::formatUptime(unsigned long ms) {
    unsigned long seconds = ms / 1000;
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;
    unsigned long days = hours / 24;

    char buf[64];
    if (days > 0) {
        snprintf(buf, sizeof(buf), "%lu天 %02lu:%02lu:%02lu", days, hours % 24, minutes % 60, seconds % 60);
    } else {
        snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", hours, minutes % 60, seconds % 60);
    }
    return String(buf);
}

void WebHandlerContext::loadCacheDuration() {
    cacheDuration = 86400; // 默认24小时
    File f = LittleFS.open("/config/device.json", "r");
    if (f) {
        JsonDocument doc;
        if (!deserializeJson(doc, f)) {
            if (doc["cacheDuration"].is<int>()) {
                cacheDuration = (uint32_t)doc["cacheDuration"].as<int>();
            }
        }
        f.close();
    }
}
