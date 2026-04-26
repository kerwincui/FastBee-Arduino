#include "./network/handlers/LogRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "systems/LoggerSystem.h"
#include "core/FastBeeFramework.h"
#include "systems/HealthMonitor.h"
#include "core/FeatureFlags.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

LogRouteHandler::LogRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void LogRouteHandler::setupRoutes(AsyncWebServer* server) {
    // Logs
    server->on("/api/logs/list", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetLogsList(request); });

    server->on("/api/logs", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetLogContent(request); });

    server->on("/api/logs", HTTP_DELETE,
              [this](AsyncWebServerRequest* request) { handleDeleteLog(request); });

    // /api/system/logs — 别名，与文档路由命名对齐
    server->on("/api/system/logs", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetLogContent(request); });
    server->on("/api/system/logs", HTTP_DELETE,
              [this](AsyncWebServerRequest* request) { handleDeleteLog(request); });

    server->on("/api/logs/clear", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleDeleteLog(request); });

    // Log level API
    server->on("/api/logs/level", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetLogLevel(request); });

    server->on("/api/logs/level", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleSetLogLevel(request); });

    server->on("/api/logs/info", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "system.view")) { ctx->sendUnauthorized(request); return; }
        static const char* LOG_FILE = "/logs/system.log";
        JsonDocument doc;
        doc["success"] = true;
        if (LittleFS.exists(LOG_FILE)) {
            File file = LittleFS.open(LOG_FILE, "r");
            if (file) { doc["data"]["size"] = file.size(); doc["data"]["exists"] = true; file.close(); }
            else { doc["data"]["size"] = 0; doc["data"]["exists"] = false; }
        } else { doc["data"]["size"] = 0; doc["data"]["exists"] = false; }
        doc["data"]["maxSize"] = LOGGER.getLogFileSizeLimit();
        doc["data"]["level"] = (int)LOGGER.getLogLevel();
        doc["data"]["fileLogging"] = LOGGER.isFileLoggingEnabled();
        HandlerUtils::sendJsonStream(request, doc);
    });
}

void LogRouteHandler::handleGetLogsList(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    JsonArray files = doc["data"].to<JsonArray>();

    File root = LittleFS.open("/logs");
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        HandlerUtils::sendJsonStream(request, doc);
        return;
    }

    File file = root.openNextFile();
    while (file) {
        const char* name = file.name();
        if (strstr(name, ".log") != nullptr) {
            JsonObject f = files.add<JsonObject>();
            f["name"] = name;
            f["size"] = file.size();
            f["current"] = (strcmp(name, "system.log") == 0);
        }
        file = root.openNextFile();
    }
    root.close();

    HandlerUtils::sendJsonStream(request, doc);
}

void LogRouteHandler::handleGetLogContent(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    // 低内存保护：SEVERE/CRITICAL 级别降级响应
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    if (fw && fw->getHealthMonitor()) {
        MemoryGuardLevel level = fw->getHealthMonitor()->getMemoryGuardLevel();
        if (level >= MemoryGuardLevel::CRITICAL) {
            HandlerUtils::sendJsonError(request, 503, "Critical memory - log unavailable");
            return;
        }
        if (level >= MemoryGuardLevel::SEVERE) {
            request->send(200, "application/json",
                F("{\"success\":true,\"data\":{\"content\":\"[Severe memory - log temporarily unavailable]\",\"size\":0,\"lines\":0,\"truncated\":true}}"));
            return;
        }
    }

    // 早期内存检查 — 此接口峰值消耗较大，预留 30KB
    if (HandlerUtils::checkLowMemory(request, 30720)) return;

    // tail=N — 只返回最新 N 条日志（优先级高于 lines）
    // lines=N — 旧参数，保留兼容
    int maxLines;
    if (request->hasParam("tail")) {
        maxLines = request->getParam("tail")->value().toInt();
        if (maxLines <= 0) maxLines = 50;
    } else {
        String linesParam = ctx->getParamValue(request, "lines", "50");
        maxLines = linesParam.toInt();
        if (maxLines <= 0) maxLines = 50;
    }
    if (maxLines > 500) maxLines = 500;

    // limit=N — 限制响应体最大 N 字节（0 = 不额外限制，由堆内存动态控制）
    size_t userLimitBytes = 0;
    if (request->hasParam("limit")) {
        int lv = request->getParam("limit")->value().toInt();
        if (lv > 0) userLimitBytes = (size_t)lv;
    }

    String fileParam = ctx->getParamValue(request, "file", "system.log");
    if (fileParam.indexOf("/") != -1 || fileParam.indexOf("\\") != -1) {
        ctx->sendError(request, 400, "Invalid file name");
        return;
    }
    String logFile = "/logs/" + fileParam;

    if (!LittleFS.exists(logFile)) {
        JsonDocument doc;
        doc["success"] = true;
        doc["data"]["content"] = "Log file does not exist";
        doc["data"]["size"] = 0;
        doc["data"]["lines"] = 0;
        HandlerUtils::sendJsonStream(request, doc);
        return;
    }

    File file = LittleFS.open(logFile, "r");
    if (!file) {
        ctx->sendError(request, 500, "Failed to open log file");
        return;
    }

    size_t fileSize = file.size();

    // 根据可用堆内存动态调整读取量，防止内存不足
    size_t freeHeap = ESP.getFreeHeap();
    size_t maxBlock = ESP.getMaxAllocHeap();
    size_t maxSize;
    if (freeHeap < 40960 || maxBlock < 8192) {
        file.close();
        request->send(200, "application/json",
            F("{\"success\":true,\"data\":{\"content\":\"[Low memory - log temporarily unavailable]\",\"size\":0,\"lines\":0,\"truncated\":true}}"));
        return;
    } else if (freeHeap < 61440) {
        maxSize = 1536;
    } else if (freeHeap < 81920) {
        maxSize = 3 * 1024;
    } else {
        maxSize = 6 * 1024;
    }

    // 如果用户设置了 limit，取 heap 动态限制和用户限制的较小值
    if (userLimitBytes > 0 && userLimitBytes < maxSize) {
        maxSize = userLimitBytes;
    }

    size_t startPos = (fileSize > maxSize) ? (fileSize - maxSize) : 0;

    file.seek(startPos);
    String content = file.readString();
    file.close();

    int lineCount = 0;
    int cutPos = 0;
    for (int i = content.length() - 1; i >= 0; i--) {
        if (content[i] == '\n') {
            lineCount++;
            if (lineCount >= maxLines) {
                cutPos = i + 1;
                break;
            }
        }
    }
    int totalLines = lineCount;

    // 原地裁剪，避免创建额外 String 副本
    if (cutPos > 0) {
        content = content.substring(cutPos);
    }

    // 直接构建 JSON String 响应
    String json;
    json.reserve(content.length() + 128);
    json += F("{\"success\":true,\"data\":{\"content\":\"");

    // 内联 JSON 转义
    for (size_t i = 0; i < content.length(); i++) {
        char c = content[i];
        switch (c) {
            case '"':  json += F("\\\""); break;
            case '\\': json += F("\\\\"); break;
            case '\n': json += F("\\n"); break;
            case '\r': json += F("\\r"); break;
            case '\t': json += F("\\t"); break;
            default:
                if ((uint8_t)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (uint8_t)c);
                    json += buf;
                } else {
                    json += c;
                }
        }
    }
    content = "";  // 尽早释放原始内容

    // 构建 JSON 元数据尾部
    char metaBuf[160];
    int metaLen = snprintf(metaBuf, sizeof(metaBuf),
        "\",\"size\":%u,\"lines\":%d,\"truncated\":%s,\"tail\":%d",
        (unsigned)fileSize, totalLines, startPos > 0 ? "true" : "false", maxLines);
    if (userLimitBytes > 0 && metaLen > 0 && (size_t)metaLen < sizeof(metaBuf) - 32) {
        metaLen += snprintf(metaBuf + metaLen, sizeof(metaBuf) - metaLen,
            ",\"limit\":%u", (unsigned)userLimitBytes);
    }
    metaBuf[sizeof(metaBuf) - 1] = '\0';
    json += metaBuf;
    json += F("}}");

    // limit 截断
    if (userLimitBytes > 0 && json.length() > userLimitBytes + 64) {
        String truncatedJson;
        truncatedJson.reserve(userLimitBytes + 64);
        truncatedJson += F("{\"success\":true,\"data\":{\"content\":\"");
        int contentEndIdx = json.indexOf(F("\",\"size\""));
        if (contentEndIdx > 0) {
            String contentOnly = json.substring(24, contentEndIdx);
            size_t maxContentChars = userLimitBytes > 100 ? userLimitBytes - 100 : 0;
            if (maxContentChars > 0 && contentOnly.length() > maxContentChars) {
                contentOnly = contentOnly.substring(0, maxContentChars);
            }
            truncatedJson += contentOnly;
        }
        char truncMeta[128];
        snprintf(truncMeta, sizeof(truncMeta),
            "\",\"size\":%u,\"lines\":%d,\"truncated\":true,\"tail\":%d,\"limit\":%u}}",
            (unsigned)fileSize, totalLines, maxLines, (unsigned)userLimitBytes);
        truncatedJson += truncMeta;
        json = truncatedJson;
    }

    request->send(200, "application/json", json);
}

void LogRouteHandler::handleDeleteLog(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    if (LOGGER.deleteLogFile()) {
        LOG_INFO("Log file cleared by user");
        ctx->sendSuccess(request, "Log file cleared");
    } else {
        ctx->sendError(request, 500, "Failed to clear log file");
    }
}

void LogRouteHandler::handleGetLogLevel(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    int level = (int)LOGGER.getLogLevel();
    doc["data"]["level"] = level;
    const char* levelNames[] = {"DEBUG", "INFO", "WARNING", "ERROR", "VERBOSE"};
    if (level >= 0 && level <= 4) {
        doc["data"]["levelName"] = levelNames[level];
    } else {
        doc["data"]["levelName"] = "UNKNOWN";
    }
    doc["data"]["fileLogging"] = LOGGER.isFileLoggingEnabled();
    HandlerUtils::sendJsonStream(request, doc);
}

void LogRouteHandler::handleSetLogLevel(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String levelStr = ctx->getParamValue(request, "level", "");
    if (levelStr.isEmpty()) {
        ctx->sendError(request, 400, "Missing 'level' parameter (0=DEBUG, 1=INFO, 2=WARNING, 3=ERROR)");
        return;
    }

    int level = levelStr.toInt();
    if (level < 0 || level > 4) {
        ctx->sendError(request, 400, "Invalid level. Use 0=DEBUG, 1=INFO, 2=WARNING, 3=ERROR, 4=VERBOSE");
        return;
    }

    LOGGER.setLogLevel(static_cast<LogLevel>(level));

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "Log level updated";
    doc["data"]["level"] = level;
    const char* levelNames[] = {"DEBUG", "INFO", "WARNING", "ERROR", "VERBOSE"};
    doc["data"]["levelName"] = levelNames[level];
    HandlerUtils::sendJsonStream(request, doc);
}
