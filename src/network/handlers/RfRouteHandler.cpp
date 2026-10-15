/**
 * @description: 433MHz RF 遥控学习路由处理器实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 */

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RF_LEARN

#include "network/handlers/RfRouteHandler.h"
#include "network/handlers/HandlerUtils.h"
#include "network/WebHandlerContext.h"
#include "peripherals/drivers/RFLearnDriver.h"
#include <ArduinoJson.h>

namespace {
const char* rfLearnStateName(RfLearnState state) {
    switch (state) { case RfLearnState::LEARNING: return "learning"; case RfLearnState::CAPTURED: return "captured";
        case RfLearnState::TIMEOUT: return "timeout"; default: return "idle"; }
}
}

RfRouteHandler::RfRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx)
{
}

void RfRouteHandler::setupRoutes(AsyncWebServer* server)
{
    server->on("/api/rf/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleStatus(request);
    });
    server->on("/api/rf/learn/start", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleLearnStart(request);
    });
    server->on("/api/rf/learn/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleLearnStatus(request);
    });
    server->on("/api/rf/learn/stop", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleLearnStop(request);
    });
    // 子路径先于父路径注册（ESPAsyncWebServer BackwardCompatible 匹配）
    server->on("/api/rf/keys/delete", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleDeleteKey(request);
    });
    server->on("/api/rf/keys/clear", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleClearKeys(request);
    });
    server->on("/api/rf/keys", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleListKeys(request);
    });
    server->on("/api/rf/keys", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleSaveKey(request);
    });
}

void RfRouteHandler::handleStatus(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    RFLearnDriver& rf = RFLearnDriver::getInstance();
    RfStatusSnapshot snapshot;
    if (!rf.getSnapshot(snapshot)) { ctx->sendError(request, 503, "RF状态暂不可用"); return; }

    JsonDocument doc;
    doc["initialized"] = snapshot.initialized; doc["stopping"] = snapshot.stopping;
    doc["learnState"] = rfLearnStateName(snapshot.learnState);
    doc["keyCount"] = snapshot.keyCount;
    doc["lastCode"] = snapshot.lastCode; doc["lastBits"] = snapshot.lastBits; doc["lastPulse"] = snapshot.lastPulse;
    ctx->sendSuccess(request, doc);
}

void RfRouteHandler::handleLearnStart(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    RFLearnDriver& rf = RFLearnDriver::getInstance();

    uint32_t timeoutMs = 10000;
    if (request->hasParam("timeoutMs", true)) {
        if (!HandlerUtils::parseUnsignedRangeParam(request->getParam("timeoutMs", true)->value(), 1000, 60000, timeoutMs)) {
            ctx->sendError(request, 400, "timeoutMs必须为1000~60000的整数"); return;
        }
    }
    if (!rf.startLearn(timeoutMs)) { ctx->sendError(request, 409, "RF接收器未就绪、忙或正在停止"); return; }
    RfStatusSnapshot snapshot;
    if (!rf.getSnapshot(snapshot)) { ctx->sendError(request, 503, "学习已请求，状态暂不可用，请查询状态"); return; }

    JsonDocument doc;
    doc["learnState"] = rfLearnStateName(snapshot.learnState); doc["remainingMs"] = snapshot.remainingMs;
    ctx->sendSuccess(request, doc);
}

void RfRouteHandler::handleLearnStatus(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    RFLearnDriver& rf = RFLearnDriver::getInstance();
    RfStatusSnapshot snapshot;
    if (!rf.getSnapshot(snapshot)) { ctx->sendError(request, 503, "RF状态暂不可用"); return; }

    JsonDocument doc;
    doc["learnState"] = rfLearnStateName(snapshot.learnState); doc["remainingMs"] = snapshot.remainingMs;
    doc["code"] = snapshot.learnedCode; doc["bits"] = snapshot.learnedBits; doc["pulse"] = snapshot.learnedPulse;
    ctx->sendSuccess(request, doc);
}

void RfRouteHandler::handleLearnStop(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    if (!RFLearnDriver::getInstance().stopLearn()) { ctx->sendError(request, 503, "取消学习未确认"); return; }
    ctx->sendSuccess(request, "已取消学习");
}

void RfRouteHandler::handleListKeys(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    RFLearnDriver& rf = RFLearnDriver::getInstance();
    String keys; size_t count = 0; RfStatusSnapshot snapshot;
    if (!rf.getSnapshot(snapshot) || (!snapshot.keysLoaded && !rf.loadKeyMap()) || !rf.getKeySnapshot(keys, count)) {
        ctx->sendError(request, 503, "RF按键库暂不可用"); return;
    }
    if (!HandlerUtils::sendRemoteKeyListEnvelope(request, keys, count)) {
        ctx->sendError(request, 503, "RF按键库响应内存不足"); return;
    }
}

void RfRouteHandler::handleSaveKey(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    if (!request->hasParam("name", true) || !request->hasParam("code", true)) {
        ctx->sendError(request, 400, "缺少参数 name 或 code");
        return;
    }
    String name = request->getParam("name", true)->value();
    String code = request->getParam("code", true)->value();
    uint32_t bits = 24, pulse = 350;
    if ((request->hasParam("bits", true) && !HandlerUtils::parseUnsignedRangeParam(request->getParam("bits", true)->value(), 12, 32, bits)) ||
        (request->hasParam("pulse", true) && !HandlerUtils::parseUnsignedRangeParam(request->getParam("pulse", true)->value(), 80, 2000, pulse))) {
        ctx->sendError(request, 400, "RF按键bits须为12~32，pulse须为80~2000的整数"); return;
    }
    name.trim();

    String err;
    if (!RFLearnDriver::getInstance().upsertKey(name, code, bits, pulse, err)) {
        ctx->sendError(request, HandlerUtils::remoteKeyErrorStatus(err, "RF", nullptr), err.length() ? err : String("保存失败"));
        return;
    }
    ctx->sendSuccess(request, "按键已保存");
}

void RfRouteHandler::handleDeleteKey(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    if (!request->hasParam("name", true)) {
        ctx->sendError(request, 400, "缺少参数 name");
        return;
    }
    String name = request->getParam("name", true)->value();
    name.trim();
    String error;
    if (!RFLearnDriver::getInstance().deleteKey(name, &error)) {
        ctx->sendError(request, HandlerUtils::remoteKeyErrorStatus(error, "RF", nullptr), error);
        return;
    }
    ctx->sendSuccess(request, "按键已删除");
}

void RfRouteHandler::handleClearKeys(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String error;
    if (!RFLearnDriver::getInstance().clearKeys(&error)) { ctx->sendError(request, HandlerUtils::remoteKeyErrorStatus(error, "RF", nullptr), error); return; }
    ctx->sendSuccess(request, "已清除全部按键");
}

#endif // FASTBEE_ENABLE_RF_LEARN
