#include "network/handlers/LogRouteHandler.h"
#include "core/MemoryBudget.h"
#include "core/RuntimeFeatureManager.h"
#include "network/handlers/HandlerUtils.h"
#include "network/WebHandlerContext.h"
#include "core/FeatureFlags.h"
#include "systems/LoggerSystem.h"
#include "core/FastBeeFramework.h"
#include "systems/HealthMonitor.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cstring>

namespace {
constexpr const char* LOG_DIR = "/logs";
constexpr const char* DEFAULT_LOG_FILE = "system.log";

bool hasLogPermission(WebHandlerContext* ctx, AsyncWebServerRequest* request) {
    return ctx && ctx->requiresAuth(request);
}

bool requireDeviceLogs(WebHandlerContext* ctx, AsyncWebServerRequest* request) {
    auto& features = FastBee::RuntimeFeatureManager::instance();
    if (features.shouldEnable("logViewer") || features.shouldEnable("fileLogging")) {
        return true;
    }
    ctx->sendError(request, 503, "Device logs disabled by runtime feature config");
    return false;
}

class ScopedLogFileAccess {
public:
    explicit ScopedLogFileAccess(uint32_t timeoutMs = 100)
        : _locked(LOGGER.lockLogFiles(timeoutMs)) {}

    ~ScopedLogFileAccess() {
        if (_locked) LOGGER.unlockLogFiles();
    }

    bool locked() const { return _locked; }

private:
    bool _locked;
};

bool requireLogFileAccess(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                          ScopedLogFileAccess& access) {
    if (access.locked()) return true;
    ctx->sendError(request, 503, "Log file is busy, retry shortly");
    return false;
}

void sendLowMemoryLogPlaceholder(AsyncWebServerRequest* request) {
    request->send(200, "application/json",
        F("{\"success\":true,\"data\":{\"content\":\"[Low memory - log temporarily unavailable]\","
          "\"size\":0,\"lines\":0,\"truncated\":true,\"degraded\":true}}"));
}

String sanitizeLogFileName(WebHandlerContext* ctx, AsyncWebServerRequest* request) {
    String name = ctx->getParamValue(request, "file", DEFAULT_LOG_FILE);
    if (name.length() <= 4 || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 ||
        !name.endsWith(".log")) {
        return String();
    }
    return name;
}
}

LogRouteHandler::LogRouteHandler(WebHandlerContext* ctx) : ctx(ctx) {}

void LogRouteHandler::setupRoutes(AsyncWebServer* server) {
#if FASTBEE_ENABLE_LOG_VIEWER || FASTBEE_ENABLE_FILE_LOGGING
    server->on("/api/logs/info", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleInfo(request);
    });
    server->on("/api/logs/list", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleList(request);
    });
    server->on("/api/logs", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleRead(request);
    });
    server->on("/api/logs/clear", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleClear(request);
    });
    server->on("/api/system/logs", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleRead(request);
    });
    server->on("/api/system/logs", HTTP_DELETE, [this](AsyncWebServerRequest* request) {
        handleClear(request);
    });
#else
    (void)server;
#endif
}

void LogRouteHandler::handleInfo(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!requireDeviceLogs(ctx, request)) return;

    auto doc = FastBee::makeJsonDocument();
    doc["success"] = true;
    doc["data"]["enabled"] = true;
    doc["data"]["fileLogging"] = LOGGER.isFileLoggingEnabled();
    doc["data"]["level"] = static_cast<int>(LOGGER.getLogLevel());
    doc["data"]["maxSize"] = LOGGER.getLogFileSizeLimit();

    {
        ScopedLogFileAccess access;
        if (!requireLogFileAccess(ctx, request, access)) return;

        String path = String(LOG_DIR) + "/" + DEFAULT_LOG_FILE;
        if (LittleFS.exists(path)) {
            File file = LittleFS.open(path, "r");
            doc["data"]["exists"] = (bool)file;
            doc["data"]["size"] = file ? file.size() : 0;
            if (file) file.close();
        } else {
            doc["data"]["exists"] = false;
            doc["data"]["size"] = 0;
        }
    }

    HandlerUtils::sendJsonStream(request, doc);
}

void LogRouteHandler::handleList(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!requireDeviceLogs(ctx, request)) return;

    auto doc = FastBee::makeJsonDocument();
    doc["success"] = true;
    JsonArray files = doc["data"].to<JsonArray>();

    {
        ScopedLogFileAccess access;
        if (!requireLogFileAccess(ctx, request, access)) return;

        File root = LittleFS.open(LOG_DIR);
        if (root && root.isDirectory()) {
            File file = root.openNextFile();
            while (file) {
                const char* name = file.name();
                if (name && strstr(name, ".log")) {
                    JsonObject item = files.add<JsonObject>();
                    item["name"] = name;
                    item["size"] = file.size();
                    item["current"] = strcmp(name, DEFAULT_LOG_FILE) == 0;
                }
                file.close();
                file = root.openNextFile();
            }
        }
        if (root) root.close();
    }

    HandlerUtils::sendJsonStream(request, doc);
}

void LogRouteHandler::handleRead(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!requireDeviceLogs(ctx, request)) return;

    const bool psramBacked = HandlerUtils::psramCanServeJson(8192);
    auto* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    if (!psramBacked &&
        ((monitor && monitor->getMemoryGuardLevel() >= MemoryGuardLevel::SEVERE) ||
         FastBee::MemoryBudget::dramFree() < 24576 || FastBee::MemoryBudget::dramLargest() < 6144)) {
        sendLowMemoryLogPlaceholder(request);
        return;
    }

    if (!psramBacked && HandlerUtils::checkLowMemory(request, 24576)) return;

    String name = sanitizeLogFileName(ctx, request);
    if (name.isEmpty()) {
        ctx->sendBadRequest(request, "Invalid log file");
        return;
    }

    // WP-74: Adaptive read size based on available memory
    size_t maxSize = FastBee::MemoryBudget::dramFree() < 61440 ? 1536 : 4096;
    if (psramBacked && ESP.getMaxAllocHeap() < 4096) {
        maxSize = 1024;  // Extremely low memory scenario
    }
    
    // Enforce hard limit for user requests (getParam 可能返回 nullptr，直接解引用会导致 LoadProhibited 崩溃)
    int requestedLimit = 0;
    if (request->hasParam("limit", true)) {
        requestedLimit = request->getParam("limit", true)->value().toInt();
    }
    if (requestedLimit > 0) {
        maxSize = min(maxSize, static_cast<size_t>(requestedLimit));  // Respect smaller of enforced/requested limit
    }
    
    size_t fileSize = 0;
    size_t start = 0;
    String content;
    content.reserve(maxSize + 1);
    bool readBudgetExceeded = false;
    {
        ScopedLogFileAccess access;
        if (!requireLogFileAccess(ctx, request, access)) return;

        String path = String(LOG_DIR) + "/" + name;
        if (!LittleFS.exists(path)) {
            request->send(200, "application/json",
                F("{\"success\":true,\"data\":{\"content\":\"Log file does not exist\",\"size\":0,\"lines\":0,\"truncated\":false}}"));
            return;
        }

        File file = LittleFS.open(path, "r");
        if (!file) {
            ctx->sendError(request, 500, "Failed to open log file");
            return;
        }

        fileSize = file.size();
        start = fileSize > maxSize ? fileSize - maxSize : 0;
        file.seek(start);

        unsigned long readStartMs = millis();
        size_t remaining = maxSize;
        char buffer[129];
        while (remaining > 0 && file.available()) {
            size_t chunk = remaining < (sizeof(buffer) - 1) ? remaining : (sizeof(buffer) - 1);
            size_t bytesRead = file.read(reinterpret_cast<uint8_t*>(buffer), chunk);
            if (bytesRead == 0) break;
            buffer[bytesRead] = '\0';
            content.concat(buffer, bytesRead);
            remaining -= bytesRead;
            if (millis() - readStartMs > 120) {
                readBudgetExceeded = true;
                break;
            }
        }
        file.close();
    }

    int maxLines = ctx->getParamInt(request, "tail", ctx->getParamInt(request, "lines", 80));
    if (maxLines <= 0) maxLines = 80;  // Default: show last 80 lines
    if (maxLines > 300) maxLines = 300;  // Hard cap to prevent JSON explosion

    int seen = 0;
    int cut = -1;
    for (int i = content.length() - 1; i >= 0; --i) {
        if (content[i] == '\n' && ++seen >= maxLines) {
            cut = i + 1;
            break;
        }
    }
    if (cut > 0) content = content.substring(cut);

    auto doc = FastBee::makeJsonDocument(maxSize + 2048);
    doc["success"] = true;
    doc["data"]["content"] = content;
    doc["data"]["size"] = fileSize;
    doc["data"]["lines"] = seen;
    doc["data"]["truncated"] = start > 0 || cut > 0 || readBudgetExceeded;
    doc["data"]["degraded"] = readBudgetExceeded;
    HandlerUtils::sendJsonStream(request, doc);
}

void LogRouteHandler::handleClear(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!requireDeviceLogs(ctx, request)) return;

    String name = sanitizeLogFileName(ctx, request);
    if (name.isEmpty()) {
        ctx->sendBadRequest(request, "Invalid log file");
        return;
    }
    if (!LOGGER.clearLogFile(name.c_str())) {
        ctx->sendError(request, 500, "Failed to clear log file");
        return;
    }
    ctx->sendSuccess(request, "Log cleared");
}
