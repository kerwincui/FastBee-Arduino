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
const char* weatherSaveMessage(const String& error) {
    if (error.startsWith("Weather revision conflict")) return "天气配置已被修改，请刷新后重试";
    if (error=="Seniverse API key is required") return "请填写心知天气 API Key";
    if (error=="Choose an existing Seniverse source or create a new source") return "请选择保留的心知天气来源或新建来源";
    if (error=="Selected weather source is unavailable") return "所选天气来源不可用，请重新选择";
    if (error=="Weather rule references unavailable") return "暂时无法检查外设执行绑定，请稍后重试";
    if (error=="Weather migration would remove a referenced source") return "其他天气来源仍被外设执行规则引用，请先调整相关规则的天气绑定（包括停用的规则），再选择保留来源";
    if (error=="Invalid weather location or credential" || error=="Invalid credential") return "城市或 API Key 格式无效";
    if (error=="Weather interval is out of bounds") return "天气刷新周期仅支持 15、30、60 分钟";
    if (error=="Remote state is being committed; retry shortly") return "天气数据正在保存，请稍后重试";
    if (error.startsWith("Cannot save") || error.startsWith("Cannot load") || error=="Weather settings unavailable") return "天气配置暂时无法读写，请稍后重试";
    return "天气配置无效，请检查组件设置";
}
}
void RemoteResourceRouteHandler::setupRoutes(AsyncWebServer* server) {
    auto retired=[this](AsyncWebServerRequest* r) {
        if (!ctx_->requireAuth(r)) return;
        ctx_->sendError(r,410,"远程资源已停用，请在设备屏幕的心知天气组件中配置");
    };
    server->on(AsyncURIMatcher::exact("/api/remote-resources"),HTTP_GET,retired);
    server->on(AsyncURIMatcher::exact("/api/remote-resources"),HTTP_POST,retired);
    server->on(AsyncURIMatcher::exact("/api/remote-resources/weather-test"),HTTP_GET,retired);
    server->on(AsyncURIMatcher::exact("/api/remote-resources/weather-test"),HTTP_POST,retired);
    server->on(AsyncURIMatcher::exact("/api/screen-panel/weather"),HTTP_GET,[this](AsyncWebServerRequest* r) {
        if (!ctx_->requireAuth(r) || HandlerUtils::checkLowMemory(r,12288)) return;
        JsonDocument response;
        FastBee::RemoteResourceManager::instance().getScreenWeather(response.to<JsonObject>());
        ctx_->sendSuccess(r,response);
    });
    server->on(AsyncURIMatcher::exact("/api/screen-panel/weather"),HTTP_POST,[this](AsyncWebServerRequest* r) { save(r); },nullptr,
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
    server->on(AsyncURIMatcher::exact("/api/screen-panel/weather/refresh"),HTTP_GET,
      [this](AsyncWebServerRequest* r) { weatherTestStatus(r); });
    server->on(AsyncURIMatcher::exact("/api/screen-panel/weather/refresh"),HTTP_POST,
      [this](AsyncWebServerRequest* r) { requestWeatherTest(r); });
}

void RemoteResourceRouteHandler::weatherTestStatus(AsyncWebServerRequest* r) {
    if (!ctx_->requireAuth(r) || !ctx_->requireDeveloperMode(r)) return;
    String id=ctx_->getParamValue(r,"id","");
    if (!FastBee::RemoteResourceModel::identifier(id.c_str())) {
        ctx_->sendBadRequest(r,"天气来源标识无效"); return;
    }
    JsonDocument response;
    if (!FastBee::RemoteResourceManager::instance().getWeatherTestStatus(id,response.to<JsonObject>())) {
        ctx_->sendError(r,404,"天气未启用或来源不可用"); return;
    }
    ctx_->sendSuccess(r,response);
}

void RemoteResourceRouteHandler::requestWeatherTest(AsyncWebServerRequest* r) {
    if (!ctx_->requireAuth(r) || !ctx_->requireDeveloperMode(r)) return;
    if (r->contentLength()>64) {
        ctx_->sendError(r,413,"天气刷新请求过大"); return;
    }
    String id=ctx_->getParamValue(r,"id","");
    if (!FastBee::RemoteResourceModel::identifier(id.c_str())) {
        ctx_->sendBadRequest(r,"天气来源标识无效"); return;
    }
    String error;
    const bool accepted=FastBee::RemoteResourceManager::instance().requestRead(id,error);
    JsonDocument response;
    JsonObject data=response.to<JsonObject>();
    if (!FastBee::RemoteResourceManager::instance().getWeatherTestStatus(id,data)) {
        ctx_->sendError(r,404,"天气未启用或来源不可用"); return;
    }
    data["accepted"]=accepted;
    if (!accepted) {
        data["rateLimited"]=(data["manualRetryAfterMs"] | 0U)>0 || (data["pending"] | false);
        data["message"]=error;
    }
    ctx_->sendSuccess(r,response);
}
void RemoteResourceRouteHandler::save(AsyncWebServerRequest* r) {
    if (!ctx_->requireAuth(r) || !ctx_->requireDeveloperMode(r)) return;
    if (r->contentLength()>FastBee::RemoteResourceModel::MAX_BODY) { ctx_->sendError(r,413,"天气配置超过 8 KiB"); return; }
    auto* b=static_cast<Body*>(r->_tempObject);
    if (!b || b->used!=b->total) { ctx_->sendBadRequest(r,"缺少完整的配置数据"); return; }
    JsonDocument doc;
    auto e=deserializeJson(doc,b->bytes,b->used,DeserializationOption::NestingLimit(12));
    free(r->_tempObject); r->_tempObject=nullptr;
    if (e) { ctx_->sendBadRequest(r,"配置数据格式无效"); return; }
    String error;
    if (!FastBee::RemoteResourceManager::instance().saveScreenWeather(doc.as<JsonVariantConst>(),error)) {
        ctx_->sendError(r,error.startsWith("Weather revision conflict") ? 409 : 400,weatherSaveMessage(error)); return;
    }
    JsonDocument response;
    FastBee::RemoteResourceManager::instance().getScreenWeather(response.to<JsonObject>());
    ctx_->sendSuccess(r,response);
}
#endif
