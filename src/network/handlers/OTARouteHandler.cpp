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

namespace {
// OTA completion is owned by OTAManager, but keep the route-level recovery
// hook available for integrations that finish an OTA from the HTTP layer.
// It records the same diagnostic snapshot before scheduling a non-blocking
// reboot, so no async_tcp handler invokes the reset primitive directly.
void scheduleOtaRouteRestart(const char* reason) {
    RestartDiagnostics::savePreRestartState(RestartReason::OTA_UPDATE, reason);
    SystemRebooter::scheduleReboot(reason, 3000UL, RestartReason::OTA_UPDATE);
}
}  // namespace

OTARouteHandler::OTARouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void OTARouteHandler::setupRoutes(AsyncWebServer* server) {
    server->on("/api/ota/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleOtaStatus(request);
    });

    server->on("/api/ota/begin", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleOtaBegin(request);
    });

    server->on("/api/ota/cancel", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleOtaCancel(request);
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
            // final 分块可以已排队成功/失败响应，但库要等 multipart
            // 解析结束才真正发送，此时 isSent() 仍为 false。仅在连
            // 排队响应也不存在时兜底，避免用 400 覆盖真实上传结果。
            if (!request->isSent() && !request->getResponse()) {
                ctx->sendError(request, 400, "OTA upload received no data");
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

void OTARouteHandler::handleOtaBegin(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    if (!ctx->otaManager) {
        ctx->sendError(request, 500, "OTA manager not initialized");
        return;
    }

    // 委托 OTAManager：内存门控/停滞回收/Update.begin 均在内部完成，
    // 失败时由 handleOTAStart 按错误类型返回 400/500/503
    ctx->otaManager->handleOTAStart(request);
}

void OTARouteHandler::handleOtaCancel(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    if (!ctx->otaManager) {
        ctx->sendError(request, 500, "OTA manager not initialized");
        return;
    }

    ctx->otaManager->handleOTACancel(request);
}

void OTARouteHandler::handleOtaStatus(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    JsonDocument doc;

    if (ctx->otaManager) {
        // 状态轮询时同步触发僵死上传会话回收（前端轮询即可自愈）
        ctx->otaManager->checkStalledUpload();
        doc["status"] = ctx->otaManager->getOTAStatus();
        doc["in_progress"] = ctx->otaManager->isOTAInProgress();
        doc["progress"] = ctx->otaManager->getProgress();
        doc["current_size"] = ctx->otaManager->getCurrentSize();
        doc["total_size"] = ctx->otaManager->getTotalSize();  // 0 = 大小未知
        doc["elapsed_ms"] = ctx->otaManager->getElapsedTime();
        String err = ctx->otaManager->getErrorMessage();
        if (!err.isEmpty()) {
            doc["error"] = err;
        }
    } else {
        doc["status"] = "unavailable";
        doc["in_progress"] = false;
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

    if (!url.startsWith("https://")) {
        ctx->sendError(request, 400, "OTA URL must use HTTPS");
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
    if (!ctx->requireAuth(request)) return;

    if (!ctx->otaManager) {
        LOGGER.error("OTA: OTA manager not initialized, upload rejected");
        if (index == 0) {
            ctx->sendError(request, 500, "OTA manager not initialized");
        }
        return;
    }

    // 委托 OTAManager：begin 由 /api/ota/begin 预先完成（含内存门控与大小预留），
    // 此处仅负责分块写入与 final 收尾，避免二次 Update.begin 导致首块丢失
    ctx->otaManager->handleOTAUpload(request, filename, index, data, len, final);
}

#endif // FASTBEE_ENABLE_OTA
