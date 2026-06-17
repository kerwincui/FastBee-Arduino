#include "network/handlers/LogRouteHandler.h"
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

void sendLowMemoryLogPlaceholder(AsyncWebServerRequest* request) {
    request->send(200, "application/json",
        F("{\"success\":true,\"data\":{\"content\":\"[Low memory - log temporarily unavailable]\","
          "\"size\":0,\"lines\":0,\"truncated\":true,\"degraded\":true}}"));
}

String sanitizeLogFileName(WebHandlerContext* ctx, AsyncWebServerRequest* request) {
    String name = ctx->getParamValue(request, "file", DEFAULT_LOG_FILE);
    if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || !name.endsWith(".log")) {
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

    auto doc = FastBee::makeJsonDocument();
    doc["success"] = true;
    doc["data"]["enabled"] = true;
    doc["data"]["fileLogging"] = LOGGER.isFileLoggingEnabled();
    doc["data"]["level"] = static_cast<int>(LOGGER.getLogLevel());
    doc["data"]["maxSize"] = LOGGER.getLogFileSizeLimit();

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

    HandlerUtils::sendJsonStream(request, doc);
}

void LogRouteHandler::handleList(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    auto doc = FastBee::makeJsonDocument();
    doc["success"] = true;
    JsonArray files = doc["data"].to<JsonArray>();

    File root = LittleFS.open(LOG_DIR);
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        HandlerUtils::sendJsonStream(request, doc);
        return;
    }

    File file = root.openNextFile();
    while (file) {
        const char* name = file.name();
        if (name && strstr(name, ".log")) {
            JsonObject item = files.add<JsonObject>();
            item["name"] = name;
            item["size"] = file.size();
            item["current"] = strcmp(name, DEFAULT_LOG_FILE) == 0;
        }
        file = root.openNextFile();
    }
    root.close();

    HandlerUtils::sendJsonStream(request, doc);
}

void LogRouteHandler::handleRead(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    const bool psramBacked = HandlerUtils::psramCanServeJson(8192);
    auto* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    if (!psramBacked &&
        ((monitor && monitor->getMemoryGuardLevel() >= MemoryGuardLevel::SEVERE) ||
         ESP.getFreeHeap() < 24576 || ESP.getMaxAllocHeap() < 6144)) {
        sendLowMemoryLogPlaceholder(request);
        return;
    }

    if (!psramBacked && HandlerUtils::checkLowMemory(request, 24576)) return;

    String name = sanitizeLogFileName(ctx, request);
    if (name.isEmpty()) {
        ctx->sendBadRequest(request, "Invalid log file");
        return;
    }

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

    const size_t fileSize = file.size();
    size_t maxSize = ESP.getFreeHeap() < 61440 ? 1536 : 4096;
    if (psramBacked && ESP.getMaxAllocHeap() < 4096) {
        maxSize = 1024;
    }
    if (request->hasParam("limit")) {
        int requested = request->getParam("limit")->value().toInt();
        if (requested > 0 && static_cast<size_t>(requested) < maxSize) {
            maxSize = static_cast<size_t>(requested);
        }
    }

    const size_t start = fileSize > maxSize ? fileSize - maxSize : 0;
    file.seek(start);

    String content;
    content.reserve(maxSize + 1);
    bool readBudgetExceeded = false;
    unsigned long readStartMs = millis();
    size_t remaining = maxSize;
    char buffer[129];
    while (remaining > 0 && file.available()) {
        size_t chunk = remaining < (sizeof(buffer) - 1) ? remaining : (sizeof(buffer) - 1);
        size_t bytesRead = file.read(reinterpret_cast<uint8_t*>(buffer), chunk);
        if (bytesRead == 0) {
            break;
        }
        buffer[bytesRead] = '\0';
        content.concat(buffer, bytesRead);
        remaining -= bytesRead;
        if (millis() - readStartMs > 120) {
            readBudgetExceeded = true;
            break;
        }
    }
    file.close();

    int maxLines = ctx->getParamInt(request, "tail", ctx->getParamInt(request, "lines", 80));
    if (maxLines <= 0) maxLines = 80;
    if (maxLines > 300) maxLines = 300;

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

    String name = sanitizeLogFileName(ctx, request);
    if (name.isEmpty()) name = DEFAULT_LOG_FILE;
    String path = String(LOG_DIR) + "/" + name;
    if (LittleFS.exists(path) && !LittleFS.remove(path)) {
        ctx->sendError(request, 500, "Failed to clear log file");
        return;
    }
    ctx->sendSuccess(request, "Log cleared");
}
