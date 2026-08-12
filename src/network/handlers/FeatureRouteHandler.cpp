/**
 * @description: 运行时功能配置路由处理器实现
 *
 * 端点:
 *   GET  /api/features/status    全量功能状态 JSON
 *   POST /api/features/config    保存配置（不重启）
 *   POST /api/features/apply     保存配置 + 3 秒后重启
 *   POST /api/features/estimate  预估启用后的 Flash/RAM 占用（不保存）
 */

#include "network/handlers/FeatureRouteHandler.h"

#include <ArduinoJson.h>
#include <memory>

#include "core/RuntimeFeatureManager.h"
#include "network/WebHandlerContext.h"
#include "network/handlers/HandlerUtils.h"
#include "systems/LoggerSystem.h"
#include "systems/SystemRebooter.h"

namespace {
// body 缓冲区挂在 request->_tempObject 上（ESPAsyncWebServer 约定的处理器私有槽位）
using BodyBuffer = std::unique_ptr<String>;

BodyBuffer* bodyOf(AsyncWebServerRequest* request) {
    return static_cast<BodyBuffer*>(request->_tempObject);
}

void releaseBody(AsyncWebServerRequest* request) {
    delete bodyOf(request);
    request->_tempObject = nullptr;
}
}  // namespace

FeatureRouteHandler::FeatureRouteHandler(WebHandlerContext* ctx) : ctx(ctx) {}

void FeatureRouteHandler::setupRoutes(AsyncWebServer* server) {
    server->on("/api/features/status", HTTP_GET,
               [this](AsyncWebServerRequest* request) { handleStatus(request); });

    server->on("/api/features/config", HTTP_POST,
               [this](AsyncWebServerRequest* request) { handleConfig(request); },
               nullptr,
               [this](AsyncWebServerRequest* request, uint8_t* data, size_t len,
                      size_t index, size_t total) {
                   accumulateBody(request, data, len, index, total);
               });

    server->on("/api/features/apply", HTTP_POST,
               [this](AsyncWebServerRequest* request) { handleApply(request); },
               nullptr,
               [this](AsyncWebServerRequest* request, uint8_t* data, size_t len,
                      size_t index, size_t total) {
                   accumulateBody(request, data, len, index, total);
               });

    server->on("/api/features/estimate", HTTP_POST,
               [this](AsyncWebServerRequest* request) { handleEstimate(request); },
               nullptr,
               [this](AsyncWebServerRequest* request, uint8_t* data, size_t len,
                      size_t index, size_t total) {
                   accumulateBody(request, data, len, index, total);
               });
}

void FeatureRouteHandler::accumulateBody(AsyncWebServerRequest* request, uint8_t* data,
                                         size_t len, size_t index, size_t total) {
    if (total > kMaxBodyBytes) {
        // 超限：丢弃并标记（finalize 阶段统一报错）
        return;
    }
    if (index == 0) {
        request->_tempObject = new BodyBuffer(std::make_unique<String>());
        (*bodyOf(request))->reserve(total);
    }
    BodyBuffer* body = bodyOf(request);
    if (body) {
        (*body)->concat(reinterpret_cast<const char*>(data), len);
    }
}

bool FeatureRouteHandler::parseBody(AsyncWebServerRequest* request, JsonDocument& doc) {
    BodyBuffer* body = bodyOf(request);
    if (!body || (*body)->isEmpty()) {
        releaseBody(request);
        ctx->sendBadRequest(request, "Missing JSON body");
        return false;
    }
    DeserializationError err = deserializeJson(doc, *(*body));
    // ArduinoJson 默认深拷贝字符串，解析完成后即可释放原始 body
    releaseBody(request);
    if (err) {
        ctx->sendBadRequest(request, String("Invalid JSON: ") + err.c_str());
        return false;
    }
    return true;
}

void FeatureRouteHandler::handleStatus(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    auto doc = FastBee::makeJsonDocument(4096);
    doc["success"] = true;
    FastBee::RuntimeFeatureManager::instance().getStatusJson(doc["data"].to<JsonObject>());
    HandlerUtils::sendJsonStream(request, doc);
}

void FeatureRouteHandler::handleConfig(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    JsonDocument doc;
    if (!parseBody(request, doc)) return;

    auto& mgr = FastBee::RuntimeFeatureManager::instance();
    if (!mgr.updateFromJson(doc)) {
        ctx->sendBadRequest(request, "No valid feature entries");
        return;
    }
    if (!mgr.saveCurrentConfig()) {
        ctx->sendError(request, 500, "Failed to save feature config");
        return;
    }
    ctx->sendSuccess(request, "Feature config saved (reboot required to apply)");
}

void FeatureRouteHandler::handleApply(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    JsonDocument doc;
    if (!parseBody(request, doc)) return;

    auto& mgr = FastBee::RuntimeFeatureManager::instance();
    if (!mgr.updateFromJson(doc)) {
        ctx->sendBadRequest(request, "No valid feature entries");
        return;
    }
    if (!mgr.saveCurrentConfig()) {
        ctx->sendError(request, 500, "Failed to save feature config");
        return;
    }

    auto respDoc = FastBee::makeJsonDocument();
    respDoc["success"] = true;
    respDoc["data"]["rebootIn"] = 3;
    HandlerUtils::sendJsonStream(request, respDoc);

    // HTTP 响应已发送，调度 3 秒后重启使配置生效
    SystemRebooter::scheduleReboot("Feature config applied", 3000,
                                   RestartReason::USER_COMMAND);
}

void FeatureRouteHandler::handleEstimate(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    JsonDocument doc;
    if (!parseBody(request, doc)) return;

    uint32_t flashKB = 0;
    uint32_t ramB = 0;
    FastBee::RuntimeFeatureManager::instance().estimateFromJson(doc, flashKB, ramB);

    auto respDoc = FastBee::makeJsonDocument();
    respDoc["success"] = true;
    respDoc["data"]["flashKB"] = flashKB;
    respDoc["data"]["ramB"] = ramB;
    HandlerUtils::sendJsonStream(request, respDoc);
}
