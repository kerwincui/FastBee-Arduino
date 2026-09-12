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
#include <vector>

#include "core/RuntimeFeatureManager.h"
#include "network/NetworkManager.h"
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

FeatureRouteHandler::PersistResult FeatureRouteHandler::persistFeatureUpdate(
    const JsonDocument& doc) {
    auto& mgr = FastBee::RuntimeFeatureManager::instance();
    const bool previousMDNS = mgr.isUserEnabled("mdns");
    FastBee::RuntimeFeatureManager::UserConfigSnapshot previousState;
    const auto updateResult = mgr.updateAndSave(doc, &previousState);
    if (updateResult == FastBee::RuntimeFeatureManager::ConfigUpdateResult::NO_VALID_ENTRIES) {
        return PersistResult::NO_VALID_ENTRIES;
    }
    if (updateResult == FastBee::RuntimeFeatureManager::ConfigUpdateResult::SAVE_FAILED) {
        return PersistResult::FEATURE_SAVE_FAILED;
    }

    const bool configuredMDNS = mgr.isUserEnabled("mdns");
    if (ctx && ctx->networkManager) {
        if (ctx->networkManager->getConfig().enableMDNS == configuredMDNS) {
            return PersistResult::SAVED;
        }
        WiFiConfig networkConfig = ctx->networkManager->getConfig();
        networkConfig.enableMDNS = configuredMDNS;
        if (ctx->networkManager->updateConfig(networkConfig, true)) {
            return PersistResult::SAVED;
        }
    } else if (configuredMDNS == previousMDNS) {
        // Non-mDNS feature updates do not require the compatibility mirror.
        return PersistResult::SAVED;
    }

    // The feature file was already committed.  Restore it when the network
    // compatibility mirror cannot be committed, then re-read disk if even the
    // rollback write fails so live state always reflects the surviving file.
    if (!mgr.restoreUserConfig(previousState) || !mgr.saveCurrentConfig()) {
        LOGGER.error("[FeatureCfg] Failed to roll back after mDNS mirror save failure");
        mgr.loadConfig();
    }
    return PersistResult::MDNS_MIRROR_FAILED;
}

void FeatureRouteHandler::handleStatus(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    auto& mgr = FastBee::RuntimeFeatureManager::instance();
    std::vector<String> items;
    items.reserve(mgr.featureCount());

    // Build one small feature object at a time. The old path retained the
    // complete ArduinoJson tree, a ~5.6 KB serialized String and the
    // AsyncBasicResponse copy concurrently, which exhausts fragmented DRAM on
    // no-PSRAM C3 targets and can yield a truncated HTTP 200 response.
    for (size_t i = 0; i < mgr.featureCount(); ++i) {
        auto itemDoc = FastBee::makeJsonDocument(1024);
        if (!mgr.getFeatureStatusJson(i, itemDoc.to<JsonObject>()) || itemDoc.overflowed()) {
            HandlerUtils::sendJsonError(request, 503,
                                        "Feature status temporarily unavailable (JSON allocation failed)", 5);
            return;
        }

        const size_t expectedSize = measureJson(itemDoc);
        String item;
        if (!item.reserve(expectedSize + 1)) {
            HandlerUtils::sendJsonError(request, 503,
                                        "Feature status temporarily unavailable (response allocation failed)", 5);
            return;
        }
        const size_t written = serializeJson(itemDoc, item);
        if (written != expectedSize || item.length() != expectedSize) {
            HandlerUtils::sendJsonError(request, 503,
                                        "Feature status temporarily unavailable (response serialization failed)", 5);
            return;
        }
        items.push_back(std::move(item));
    }

    auto summaryDoc = FastBee::makeJsonDocument(1024);
    mgr.getStatusSummaryJson(summaryDoc.to<JsonObject>());
    if (summaryDoc.overflowed()) {
        HandlerUtils::sendJsonError(request, 503,
                                    "Feature status temporarily unavailable (summary allocation failed)", 5);
        return;
    }

    const size_t summarySize = measureJson(summaryDoc);
    String summary;
    if (!summary.reserve(summarySize + 1) ||
        serializeJson(summaryDoc, summary) != summarySize ||
        summary.length() != summarySize) {
        HandlerUtils::sendJsonError(request, 503,
                                    "Feature status temporarily unavailable (summary serialization failed)", 5);
        return;
    }

    // JSON member order is not semantically significant. Putting the small
    // summary in the header lets the shared list streamer keep a static footer.
    String header;
    const size_t headerSize = summary.length() + 64;
    if (!header.reserve(headerSize)) {
        HandlerUtils::sendJsonError(request, 503,
                                    "Feature status temporarily unavailable (header allocation failed)", 5);
        return;
    }
    header = F("{\"success\":true,\"data\":{\"summary\":");
    header += summary;
    header += F(",\"features\":[");

    if (!HandlerUtils::sendJsonListChunked(request, header, std::move(items), "]}}")) {
        HandlerUtils::sendJsonError(request, 503,
                                    "Feature status temporarily unavailable (stream allocation failed)", 5);
    }
}

void FeatureRouteHandler::handleConfig(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    JsonDocument doc;
    if (!parseBody(request, doc)) return;

    const PersistResult result = persistFeatureUpdate(doc);
    if (result == PersistResult::NO_VALID_ENTRIES) {
        ctx->sendBadRequest(request, "No valid feature entries");
        return;
    }
    if (result == PersistResult::FEATURE_SAVE_FAILED) {
        ctx->sendError(request, 500, "Failed to save feature config");
        return;
    }
    if (result == PersistResult::MDNS_MIRROR_FAILED) {
        ctx->sendError(request, 500, "Failed to synchronize mDNS compatibility config");
        return;
    }
    ctx->sendSuccess(request, "Feature config saved (reboot required to apply)");
}

void FeatureRouteHandler::handleApply(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    JsonDocument doc;
    if (!parseBody(request, doc)) return;

    const PersistResult result = persistFeatureUpdate(doc);
    if (result == PersistResult::NO_VALID_ENTRIES) {
        ctx->sendBadRequest(request, "No valid feature entries");
        return;
    }
    if (result == PersistResult::FEATURE_SAVE_FAILED) {
        ctx->sendError(request, 500, "Failed to save feature config");
        return;
    }
    if (result == PersistResult::MDNS_MIRROR_FAILED) {
        ctx->sendError(request, 500, "Failed to synchronize mDNS compatibility config");
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
