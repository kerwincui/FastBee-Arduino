#include "network/handlers/ScreenPanelRouteHandler.h"

#if FASTBEE_ENABLE_SCREEN_PANEL || FASTBEE_ENABLE_POINT_HISTORY

#include <AsyncJson.h>
#if FASTBEE_ENABLE_POINT_HISTORY
#include <vector>
#endif

#if FASTBEE_ENABLE_POINT_HISTORY
#include "core/PointHistory.h"
#endif
#if FASTBEE_ENABLE_SCREEN_PANEL
#include "core/ScreenPanelManager.h"
#endif
#include "network/WebHandlerContext.h"
#include "network/handlers/HandlerUtils.h"

namespace {

// AsyncJson allocates and parses before invoking its callback. Gate that work
// while receiving headers/body, so a disabled developer session or a large
// unauthenticated request cannot consume the panel's configuration budget.
class PanelJsonHandler final : public AsyncWebHandler {
public:
    PanelJsonHandler(WebHandlerContext* ctx, const char* uri,
                     ArJsonRequestHandlerFunction callback, size_t limit)
        : ctx_(ctx), json_(uri, callback), limit_(limit) {
        json_.setMethod(HTTP_POST);
        json_.setMaxContentLength(limit);
    }

    bool canHandle(AsyncWebServerRequest* request) const override {
        return json_.canHandle(request);
    }

    void handleBody(AsyncWebServerRequest* request, uint8_t* data, size_t len,
                    size_t index, size_t total) override {
        if (request->getResponse()) return;
        if (index == 0) {
            if (!ctx_->requireAuth(request) ||
                !ctx_->requireDeveloperMode(request)) return;
            if (total > limit_) {
                ctx_->sendError(request, 413, "Configuration body is too large");
                return;
            }
            if (HandlerUtils::checkLowMemory(request, total * 2 + 8192)) return;
            if (ESP.getMaxAllocHeap() < total + 1) {
                ctx_->sendError(request, 503, "Configuration receive buffer is unavailable");
                return;
            }
        }
        json_.handleBody(request, data, len, index, total);
    }

    void handleRequest(AsyncWebServerRequest* request) override {
        if (request->getResponse() || !ctx_->requireAuth(request) ||
            !ctx_->requireDeveloperMode(request)) return;
        json_.handleRequest(request);
    }

private:
    WebHandlerContext* ctx_;
    AsyncCallbackJsonWebHandler json_;
    const size_t limit_;
};

#if FASTBEE_ENABLE_POINT_HISTORY
bool validSourceRef(const String& sourceRef) {
    if (sourceRef.isEmpty() || sourceRef.length() >
                                   FastBee::PointHistory::MAX_SOURCE_REF_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < sourceRef.length(); ++i) {
        const char c = sourceRef.charAt(i);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.' || c == ':')) {
            return false;
        }
    }
    return true;
}

bool historyConfigKeysValid(JsonObjectConst object) {
    for (JsonPairConst pair : object) {
        const String key = pair.key().c_str();
        if (key != "enabled" && key != "sourceAllowlist" && key != "expectedRevision") return false;
    }
    return true;
}
#endif

#if FASTBEE_ENABLE_SCREEN_PANEL
String lowerCopy(String value) {
    value.toLowerCase();
    return value;
}
#endif

}  // namespace

ScreenPanelRouteHandler::ScreenPanelRouteHandler(WebHandlerContext* ctx)
    : _ctx(ctx) {}

void ScreenPanelRouteHandler::setupRoutes(AsyncWebServer* server) {
#if FASTBEE_ENABLE_SCREEN_PANEL
    server->on("/api/screen-panel/config", HTTP_GET,
               [this](AsyncWebServerRequest* request) {
                   handleGetConfig(request);
               });
    server->on("/api/screen-panel/choices", HTTP_GET,
               [this](AsyncWebServerRequest* request) {
                   handleChoices(request);
               });
    server->on("/api/screen-panel/state", HTTP_GET,
               [this](AsyncWebServerRequest* request) {
                   handleState(request);
               });
#endif
#if FASTBEE_ENABLE_POINT_HISTORY
    // Plain URI matchers also accept child paths in ESPAsyncWebServer.
    // Keep series reads from intercepting /history/config.
    server->on(AsyncURIMatcher::exact("/api/periph-exec/history"), HTTP_GET,
               [this](AsyncWebServerRequest* request) {
                   handleHistory(request);
               });
    server->on(AsyncURIMatcher::exact("/api/periph-exec/history/config"), HTTP_GET,
               [this](AsyncWebServerRequest* request) {
                   handleGetHistoryConfig(request);
               });
#endif

#if FASTBEE_ENABLE_SCREEN_PANEL
    auto* configHandler = new PanelJsonHandler(
        _ctx, "/api/screen-panel/config",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            handleSaveConfig(request, json);
        }, ScreenPanelManager::MAX_CONFIG_BYTES + 1024);
    server->addHandler(configHandler);
#endif

#if FASTBEE_ENABLE_POINT_HISTORY
    auto* historyConfigHandler = new PanelJsonHandler(
        _ctx, "/api/periph-exec/history/config",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            handleSaveHistoryConfig(request, json);
        }, 4096);
    server->addHandler(historyConfigHandler);
#endif
}

#if FASTBEE_ENABLE_SCREEN_PANEL
void ScreenPanelRouteHandler::handleGetConfig(
    AsyncWebServerRequest* request) {
    if (!_ctx->requireAuth(request) ||
        HandlerUtils::checkLowMemory(request, 8192)) {
        return;
    }

    auto& manager = ScreenPanelManager::instance();
    if (!manager.initialize()) {
        _ctx->sendError(request, 503, "Screen panel manager is unavailable");
        return;
    }

    JsonDocument config;
    uint32_t revision = 0;
    bool persisted = false;
    if (!manager.copyConfig(config, revision, persisted)) {
        _ctx->sendError(request, 503, "Screen panel configuration is busy");
        return;
    }

    JsonDocument response;
    response["revision"] = revision;
    response["persisted"] = persisted;
    response["config"].set(config.as<JsonVariantConst>());
    manager.appendCapabilities(response["capabilities"].to<JsonObject>());
    manager.appendApplication(response["application"].to<JsonObject>());
    _ctx->sendSuccess(request, response);
}

void ScreenPanelRouteHandler::handleSaveConfig(
    AsyncWebServerRequest* request, JsonVariant& json) {
    if (!_ctx->requireAuth(request) ||
        !_ctx->requireDeveloperMode(request)) {
        return;
    }
    if (!json.is<JsonObjectConst>()) {
        _ctx->sendBadRequest(request, "Request body must be an object");
        return;
    }
    JsonObjectConst body = json.as<JsonObjectConst>();
    if (!body["expectedRevision"].is<uint32_t>() ||
        !body["config"].is<JsonObjectConst>()) {
        _ctx->sendBadRequest(
            request, "expectedRevision and config are required");
        return;
    }

    auto& manager = ScreenPanelManager::instance();
    String error;
    const auto result = manager.saveConfig(
        body["config"].as<JsonVariantConst>(),
        body["expectedRevision"].as<uint32_t>(), error);
    if (result == ScreenPanelManager::SaveResult::REVISION_CONFLICT) {
        JsonDocument response;
        response["success"] = false;
        response["error"] = error;
        response["code"] = 409;
        response["currentRevision"] = manager.revision();
        _ctx->sendJsonResponse(request, 409, response);
        return;
    }
    if (result != ScreenPanelManager::SaveResult::SAVED) {
        const int status = result == ScreenPanelManager::SaveResult::INVALID_CONFIG
                               ? 400
                           : result == ScreenPanelManager::SaveResult::BUSY
                               ? 503
                               : 500;
        _ctx->sendError(request, status, error);
        return;
    }

    JsonDocument savedConfig;
    uint32_t revision = 0;
    bool persisted = false;
    if (!manager.copyConfig(savedConfig, revision, persisted)) {
        _ctx->sendError(request, 503,
                        "Configuration saved but response state is busy");
        return;
    }
    JsonDocument response;
    response["revision"] = revision;
    response["persisted"] = persisted;
    response["config"].set(savedConfig.as<JsonVariantConst>());
    manager.appendApplication(response["application"].to<JsonObject>());
    _ctx->sendSuccess(request, response);
}

void ScreenPanelRouteHandler::handleChoices(
    AsyncWebServerRequest* request) {
    if (!_ctx->requireAuth(request) ||
        HandlerUtils::checkLowMemory(request, 6144)) {
        return;
    }

    int page = _ctx->getParamInt(request, "page", 1);
    int pageSize = _ctx->getParamInt(request, "pageSize", 25);
    page = page < 1 ? 1 : page;
    pageSize = pageSize < 1 ? 1 : (pageSize > 50 ? 50 : pageSize);
    const String kind = lowerCopy(
        _ctx->getParamValue(request, "kind", "all"));
    const String query = lowerCopy(_ctx->getParamValue(request, "q", ""));

    JsonDocument allDocument;
    JsonArray all = allDocument.to<JsonArray>();
    ScreenPanelManager::instance().appendChoices(all);

    JsonDocument response;
    JsonArray items = response["items"].to<JsonArray>();
    const size_t first = static_cast<size_t>(page - 1) * pageSize;
    size_t matched = 0;
    for (JsonObjectConst candidate : all) {
        const String candidateKind = lowerCopy(String(candidate["kind"] | ""));
        if (kind != "all" && kind != candidateKind) continue;
        if (!query.isEmpty()) {
            const String haystack = lowerCopy(
                String(candidate["label"] | "") + " " +
                String(candidate["sourceRef"] | "") + " " +
                String(candidate["controlRef"] | ""));
            if (haystack.indexOf(query) < 0) continue;
        }
        if (matched >= first && items.size() < static_cast<size_t>(pageSize)) {
            items.add(candidate);
        }
        ++matched;
    }
    response["page"] = page;
    response["pageSize"] = pageSize;
    response["total"] = matched;
    response["hasMore"] = first + items.size() < matched;
    JsonObject history = response["capabilities"]["history"].to<JsonObject>();
#if FASTBEE_ENABLE_POINT_HISTORY
    history["supported"] = true;
    history["maxSources"] = FastBee::PointHistory::MAX_SOURCES;
    history["maxSamplesPerSource"] =
        FastBee::PointHistory::MAX_SAMPLES_PER_SOURCE;
#else
    history["supported"] = false;
#endif
    _ctx->sendSuccess(request, response);
}

void ScreenPanelRouteHandler::handleState(AsyncWebServerRequest* request) {
    if (!_ctx->requireAuth(request) ||
        HandlerUtils::checkLowMemory(request, 6144)) {
        return;
    }
    JsonDocument response;
    auto& manager = ScreenPanelManager::instance();
    manager.appendState(response.to<JsonObject>());
    manager.appendApplication(response["application"].to<JsonObject>());
    manager.appendCapabilities(response["capabilities"].to<JsonObject>());
    _ctx->sendSuccess(request, response);
}
#endif

#if FASTBEE_ENABLE_POINT_HISTORY
void ScreenPanelRouteHandler::handleHistory(
    AsyncWebServerRequest* request) {
    if (!_ctx->requireAuth(request)) return;
    const String sourceRef = _ctx->getParamValue(request, "sourceRef", "");
    if (!validSourceRef(sourceRef)) {
        _ctx->sendBadRequest(request, "A valid sourceRef is required");
        return;
    }
    auto& history = FastBee::PointHistory::instance();
    if (!history.initialize()) {
        _ctx->sendError(request, 503, "Point history is unavailable");
        return;
    }
    JsonDocument response;
    history.appendSeriesJson(sourceRef, response.to<JsonObject>());
    history.appendStatusJson(response["persistence"].to<JsonObject>());
    _ctx->sendSuccess(request, response);
}

void ScreenPanelRouteHandler::handleGetHistoryConfig(
    AsyncWebServerRequest* request) {
    if (!_ctx->requireAuth(request)) return;
    auto& history = FastBee::PointHistory::instance();
    if (!history.initialize()) {
        _ctx->sendError(request, 503, "Point history is unavailable");
        return;
    }
    JsonDocument response;
    history.appendConfigJson(response["config"].to<JsonObject>());
    history.appendStatusJson(response["status"].to<JsonObject>());
    _ctx->sendSuccess(request, response);
}

void ScreenPanelRouteHandler::handleSaveHistoryConfig(
    AsyncWebServerRequest* request, JsonVariant& json) {
    if (!_ctx->requireAuth(request) ||
        !_ctx->requireDeveloperMode(request)) {
        return;
    }
    if (!json.is<JsonObjectConst>()) {
        _ctx->sendBadRequest(request, "History configuration must be an object");
        return;
    }
    JsonObjectConst body = json.as<JsonObjectConst>();
    if (!historyConfigKeysValid(body) || !body["expectedRevision"].is<uint32_t>() || !body["enabled"].is<bool>() ||
        !body["sourceAllowlist"].is<JsonArrayConst>()) {
        _ctx->sendBadRequest(request, "Invalid history configuration");
        return;
    }
    JsonArrayConst rawSources = body["sourceAllowlist"].as<JsonArrayConst>();
    if (rawSources.size() > FastBee::PointHistory::MAX_SOURCES) {
        _ctx->sendBadRequest(request, "History source limit exceeded");
        return;
    }
    std::vector<String> sources;
    sources.reserve(rawSources.size());
    for (JsonVariantConst rawSource : rawSources) {
        if (!rawSource.is<const char*>()) {
            _ctx->sendBadRequest(request,
                                 "History sourceRef values must be strings");
            return;
        }
        sources.emplace_back(rawSource.as<const char*>());
    }

    auto& history = FastBee::PointHistory::instance();
    String error;
    const uint32_t expectedRevision = body["expectedRevision"].as<uint32_t>();
    if (!history.configure(body["enabled"].as<bool>(), sources, error, true, &expectedRevision)) {
        _ctx->sendError(request, error == "History configuration revision conflict" ? 409 : 400, error);
        return;
    }
    JsonDocument response;
    history.appendConfigJson(response["config"].to<JsonObject>());
    history.appendStatusJson(response["status"].to<JsonObject>());
    _ctx->sendSuccess(request, response);
}
#endif

#endif  // FASTBEE_ENABLE_SCREEN_PANEL || FASTBEE_ENABLE_POINT_HISTORY
