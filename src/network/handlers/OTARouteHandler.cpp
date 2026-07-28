#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_OTA

#include "./network/handlers/OTARouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./network/OTAManager.h"
#include "systems/LoggerSystem.h"
#include "systems/RestartDiagnostics.h"
#include "systems/SystemRebooter.h"
#include <ArduinoJson.h>
#include <Update.h>

OTARouteHandler::OTARouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void OTARouteHandler::setupRoutes(AsyncWebServer* server) {
    server->on("/api/ota/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleOtaStatus(request);
    });

    server->on("/api/ota/update", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleOtaUpdate(request);
    });

    server->on("/api/ota/url", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleOtaUrl(request);
    });

    server->on("/api/ota/upload", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            if (ctx->otaManager && ctx->otaManager->isOTAInProgress()) {
                ctx->sendSuccess(request, "Firmware uploading...");
            } else {
                ctx->sendSuccess(request, "Upload completed");
            }
        },
        [this](AsyncWebServerRequest* request, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
            handleOtaUpload(request, filename, index, data, len, final);
        }
    );
}

void OTARouteHandler::handleOtaUpdate(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    ctx->sendSuccess(request, "OTA update started");
}

void OTARouteHandler::handleOtaStatus(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    JsonDocument doc;

    if (ctx->otaManager) {
        doc["status"] = ctx->otaManager->getOTAStatus();
        doc["progress"] = ctx->otaManager->getProgress();
    } else {
        doc["status"] = "unavailable";
        doc["progress"] = 0;
    }

    ctx->sendSuccess(request, doc);
}

void OTARouteHandler::handleOtaUrl(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    String url = ctx->getParamValue(request, "url", "");

    if (url.isEmpty()) {
        ctx->sendError(request, 400, "Missing firmware URL");
        return;
    }

    if (!url.startsWith("http://") && !url.startsWith("https://")) {
        ctx->sendError(request, 400, "Invalid URL format");
        return;
    }

    if (!ctx->otaManager) {
        ctx->sendError(request, 500, "OTA manager not initialized");
        return;
    }

    if (ctx->otaManager->isOTAInProgress()) {
        ctx->sendError(request, 400, "OTA upgrade in progress");
        return;
    }

    LOGGER.infof("OTA: Starting URL upgrade from %s", url.c_str());

    // 下载+写 flash 在独立后台任务执行，handler 立即返回；
    // 在 async_tcp 上同步执行会导致升级期间全站 Web 不可用甚至自饿死
    if (!ctx->otaManager->startOTAAsync(url)) {
        ctx->sendError(request, 500, "Failed to start OTA download task");
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "Firmware download started, poll /api/ota/status for progress";
    doc["url"] = url;

    HandlerUtils::sendJsonStream(request, doc);
}

void OTARouteHandler::handleOtaUpload(AsyncWebServerRequest* request, const String& filename,
                                       size_t index, uint8_t* data, size_t len, bool final) {
    if (!ctx->requiresAuth(request)) {
        return;
    }

    if (index == 0) {
        LOGGER.infof("OTA: Upload started - file: %s", filename.c_str());

        uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;

        if (!Update.begin(maxSketchSpace)) {
            LOGGER.errorf("OTA: Update begin failed - %s", Update.errorString());
            return;
        }

        Update.onProgress([](size_t progress, size_t total) {
            int percent = total > 0 ? (progress * 100 / total) : 0;
            if (percent % 10 == 0) {
                LOGGER.infof("OTA: Upload progress: %d%%", percent);
            }
        });
    }

    if (Update.write(data, len) != len) {
        LOGGER.errorf("OTA: Write failed - %s", Update.errorString());
        Update.end(false);
        return;
    }

    if (final) {
        LOGGER.infof("OTA: Upload completed - total size: %d bytes", index + len);

        if (Update.end(true)) {
            if (Update.isFinished()) {
                LOGGER.info("OTA: Firmware verification passed, restarting...");

                JsonDocument doc;
                doc["success"] = true;
                doc["message"] = "Firmware uploaded, restarting in 3s";
                doc["size"] = index + len;
                doc["md5"] = Update.md5String();

                HandlerUtils::sendJsonStream(request, doc);

                // 延迟重启交给主循环 SystemRebooter，避免在 async_tcp 上同步睡眠 3 秒
                // 阻塞响应发送（客户端收不到成功响应会误判失败）
                RestartDiagnostics::savePreRestartState(
                    RestartReason::OTA_UPDATE,
                    "OTA firmware uploaded via Web");
                SystemRebooter::scheduleReboot("OTA firmware uploaded via Web", 3000,
                                               RestartReason::OTA_UPDATE);
            } else {
                LOGGER.error("OTA: Firmware verification failed");
            }
        } else {
            LOGGER.errorf("OTA: Update end failed - %s", Update.errorString());
        }
    }
}

#endif // FASTBEE_ENABLE_OTA
