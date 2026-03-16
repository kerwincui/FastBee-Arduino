#include "./network/handlers/OTARouteHandler.h"
#include "./network/WebHandlerContext.h"
#include "./network/OTAManager.h"
#include "systems/LoggerSystem.h"
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
    if (!ctx->checkPermission(request, "ota.update")) {
        ctx->sendUnauthorized(request);
        return;
    }

    ctx->sendSuccess(request, "OTA update started");
}

void OTARouteHandler::handleOtaStatus(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

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
    if (!ctx->checkPermission(request, "ota.update")) {
        ctx->sendUnauthorized(request);
        return;
    }

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

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "Starting firmware download and upgrade";
    doc["url"] = url;

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);

    delay(100);
    ctx->otaManager->startOTA(url);
}

void OTARouteHandler::handleOtaUpload(AsyncWebServerRequest* request, const String& filename,
                                       size_t index, uint8_t* data, size_t len, bool final) {
    if (!ctx->checkPermission(request, "ota.update")) {
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

                String out;
                serializeJson(doc, out);
                request->send(200, "application/json", out);

                delay(3000);
                ESP.restart();
            } else {
                LOGGER.error("OTA: Firmware verification failed");
            }
        } else {
            LOGGER.errorf("OTA: Update end failed - %s", Update.errorString());
        }
    }
}
