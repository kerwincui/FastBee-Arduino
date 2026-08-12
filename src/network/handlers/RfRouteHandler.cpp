/**
 * @description: 433MHz RF 遥控学习路由处理器实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 */

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RF_LEARN

#include "network/handlers/RfRouteHandler.h"
#include "network/WebHandlerContext.h"
#include "peripherals/drivers/RFLearnDriver.h"
#include <ArduinoJson.h>

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

    JsonDocument doc;
    doc["initialized"] = rf.isInitialized();
    doc["learnState"] = rf.getLearnStateName();
    doc["keyCount"] = (int)rf.getKeyCount();
    doc["lastCode"] = rf.getLastCode();
    doc["lastBits"] = rf.getLastBitLength();
    doc["lastPulse"] = rf.getLastPulseWidth();
    ctx->sendSuccess(request, doc);
}

void RfRouteHandler::handleLearnStart(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    RFLearnDriver& rf = RFLearnDriver::getInstance();
    if (!rf.isInitialized()) {
        ctx->sendError(request, 400, "RF接收器未初始化，请先在外设管理中启用433MHz RF模块(RX模式)");
        return;
    }

    uint32_t timeoutMs = 10000;
    if (request->hasParam("timeoutMs", true)) {
        long v = request->getParam("timeoutMs", true)->value().toInt();
        if (v > 0) timeoutMs = (uint32_t)v;
    }
    rf.startLearn(timeoutMs);

    JsonDocument doc;
    doc["learnState"] = rf.getLearnStateName();
    doc["remainingMs"] = rf.getLearnRemainingMs();
    ctx->sendSuccess(request, doc);
}

void RfRouteHandler::handleLearnStatus(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    RFLearnDriver& rf = RFLearnDriver::getInstance();

    JsonDocument doc;
    doc["learnState"] = rf.getLearnStateName();
    doc["remainingMs"] = rf.getLearnRemainingMs();
    doc["code"] = rf.getLearnedCode();
    doc["bits"] = rf.getLearnedBitLength();
    doc["pulse"] = rf.getLearnedPulseWidth();
    ctx->sendSuccess(request, doc);
}

void RfRouteHandler::handleLearnStop(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    RFLearnDriver::getInstance().stopLearn();
    ctx->sendSuccess(request, "已取消学习");
}

void RfRouteHandler::handleListKeys(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    RFLearnDriver& rf = RFLearnDriver::getInstance();
    String keys = rf.getKeysJson();
    String resp = "{\"success\":true,\"data\":{\"count\":" + String((int)rf.getKeyCount()) +
                  ",\"keys\":" + keys + "}}";
    request->send(200, "application/json", resp);
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
    uint8_t bits = request->hasParam("bits", true)
                       ? (uint8_t)request->getParam("bits", true)->value().toInt()
                       : 24;
    uint16_t pulse = request->hasParam("pulse", true)
                         ? (uint16_t)request->getParam("pulse", true)->value().toInt()
                         : 350;
    name.trim();

    String err;
    if (!RFLearnDriver::getInstance().upsertKey(name, code, bits, pulse, err)) {
        ctx->sendError(request, 400, err.length() ? err : String("保存失败"));
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
    if (!RFLearnDriver::getInstance().deleteKey(name)) {
        ctx->sendError(request, 404, "未找到该按键");
        return;
    }
    ctx->sendSuccess(request, "按键已删除");
}

void RfRouteHandler::handleClearKeys(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    RFLearnDriver::getInstance().clearKeys();
    ctx->sendSuccess(request, "已清除全部按键");
}

#endif // FASTBEE_ENABLE_RF_LEARN
