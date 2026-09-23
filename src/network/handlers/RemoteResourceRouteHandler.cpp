#include "network/handlers/RemoteResourceRouteHandler.h"
#if FASTBEE_ENABLE_REMOTE_RESOURCES
#include "network/WebHandlerContext.h"
#include "network/handlers/HandlerUtils.h"
#include "protocols/RemoteResourceManager.h"
#include "protocols/RemoteResourceModel.h"
#include <cstdlib>
#include <cstring>

namespace {
// Plain malloc storage is also released by AsyncWebServer on disconnect.
struct Body { size_t total; size_t used; char bytes[1]; };
}
void RemoteResourceRouteHandler::setupRoutes(AsyncWebServer* server) {
    auto retired=[this](AsyncWebServerRequest* r) {
        if (!ctx_->requireAuth(r)) return;
        ctx_->sendError(r,410,"Remote resources are retired; use device screen weather settings");
    };
    server->on("/api/remote-resources",HTTP_GET,retired);
    server->on("/api/remote-resources",HTTP_POST,retired);
    server->on("/api/remote-resources/weather-test",HTTP_GET,retired);
    server->on("/api/remote-resources/weather-test",HTTP_POST,retired);
    server->on("/api/screen-panel/weather",HTTP_GET,[this](AsyncWebServerRequest* r) {
        if (!ctx_->requireAuth(r) || HandlerUtils::checkLowMemory(r,12288)) return;
        JsonDocument response;
        FastBee::RemoteResourceManager::instance().getScreenWeather(response.to<JsonObject>());
        ctx_->sendSuccess(r,response);
    });
    server->on("/api/screen-panel/weather",HTTP_POST,[this](AsyncWebServerRequest* r) { save(r); },nullptr,
      [this](AsyncWebServerRequest* r,uint8_t* data,size_t len,size_t index,size_t total) {
        if (total==0 || total>FastBee::RemoteResourceModel::MAX_BODY || index+len>total) return;
        if (index==0) {
            if (!ctx_->requireAuth(r) || !ctx_->requireDeveloperMode(r)) return;
            auto* b=static_cast<Body*>(malloc(sizeof(Body)+total));
            if (!b) return;
            b->total=total; b->used=0; r->_tempObject=b;
        }
        auto* b=static_cast<Body*>(r->_tempObject);
        if (!b || b->used!=index || b->total!=total) return;
        memcpy(b->bytes+index,data,len); b->used+=len; b->bytes[b->used]='\0';
      });
    server->on("/api/screen-panel/weather/refresh",HTTP_GET,
      [this](AsyncWebServerRequest* r) { weatherTestStatus(r); });
    server->on("/api/screen-panel/weather/refresh",HTTP_POST,
      [this](AsyncWebServerRequest* r) { requestWeatherTest(r); });
}

void RemoteResourceRouteHandler::weatherTestStatus(AsyncWebServerRequest* r) {
    if (!ctx_->requireAuth(r) || !ctx_->requireDeveloperMode(r)) return;
    String id=ctx_->getParamValue(r,"id","");
    if (!FastBee::RemoteResourceModel::identifier(id.c_str())) {
        ctx_->sendBadRequest(r,"Invalid weather source ID"); return;
    }
    JsonDocument response;
    if (!FastBee::RemoteResourceManager::instance().getWeatherTestStatus(id,response.to<JsonObject>())) {
        ctx_->sendError(r,404,"Weather source unavailable"); return;
    }
    ctx_->sendSuccess(r,response);
}

void RemoteResourceRouteHandler::requestWeatherTest(AsyncWebServerRequest* r) {
    if (!ctx_->requireAuth(r) || !ctx_->requireDeveloperMode(r)) return;
    if (r->contentLength()>64) {
        ctx_->sendError(r,413,"Weather test request is too large"); return;
    }
    String id=ctx_->getParamValue(r,"id","");
    if (!FastBee::RemoteResourceModel::identifier(id.c_str())) {
        ctx_->sendBadRequest(r,"Invalid weather source ID"); return;
    }
    String error;
    const bool accepted=FastBee::RemoteResourceManager::instance().requestRead(id,error);
    JsonDocument response;
    JsonObject data=response.to<JsonObject>();
    if (!FastBee::RemoteResourceManager::instance().getWeatherTestStatus(id,data)) {
        ctx_->sendError(r,404,"Weather source unavailable"); return;
    }
    data["accepted"]=accepted;
    if (!accepted) {
        data["rateLimited"]=error=="Weather minimum refresh interval is 5 minutes";
        data["message"]=error;
    }
    ctx_->sendSuccess(r,response);
}
void RemoteResourceRouteHandler::save(AsyncWebServerRequest* r) {
    if (!ctx_->requireAuth(r) || !ctx_->requireDeveloperMode(r)) return;
    if (r->contentLength()>FastBee::RemoteResourceModel::MAX_BODY) { ctx_->sendError(r,413,"Remote resource configuration exceeds 8 KiB"); return; }
    auto* b=static_cast<Body*>(r->_tempObject);
    if (!b || b->used!=b->total) { ctx_->sendBadRequest(r,"Missing or incomplete JSON body"); return; }
    JsonDocument doc;
    auto e=deserializeJson(doc,b->bytes,b->used,DeserializationOption::NestingLimit(12));
    free(r->_tempObject); r->_tempObject=nullptr;
    if (e) { ctx_->sendBadRequest(r,"Invalid JSON"); return; }
    String error;
    if (!FastBee::RemoteResourceManager::instance().saveScreenWeather(doc.as<JsonVariantConst>(),error)) {
        ctx_->sendError(r,error.startsWith("Weather revision conflict") ? 409 : 400,error); return;
    }
    JsonDocument response;
    FastBee::RemoteResourceManager::instance().getScreenWeather(response.to<JsonObject>());
    ctx_->sendSuccess(r,response);
}
#endif
