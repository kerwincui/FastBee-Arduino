/**
 * @description: 红外遥控路由处理器实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 */

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_IR_REMOTE

#include "network/handlers/IrRouteHandler.h"
#include "network/WebHandlerContext.h"
#include "peripherals/drivers/IRRemoteDriver.h"
#include <ArduinoJson.h>

IrRouteHandler::IrRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx)
{
}

void IrRouteHandler::setupRoutes(AsyncWebServer* server)
{
    // 状态
    server->on("/api/ir/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleStatus(request);
    });

    // 学习模式（对码）
    server->on("/api/ir/learn/start", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleLearnStart(request);
    });
    server->on("/api/ir/learn/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleLearnStatus(request);
    });
    server->on("/api/ir/learn/stop", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleLearnStop(request);
    });

    // 按键映射（对码保存 / 清码）
    // 注意：ESPAsyncWebServer 对普通字符串 URI 采用 BackwardCompatible 匹配，
    // 即 "/api/ir/keys" 同时匹配 "/api/ir/keys/xxx"，且按注册顺序取第一个命中的处理器。
    // 因此 /delete、/clear 这类子路径必须先于父路径 "/api/ir/keys" 注册，否则会被其吞掉。
    server->on("/api/ir/keys/delete", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleDeleteKey(request);
    });
    server->on("/api/ir/keys/clear", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleClearKeys(request);
    });
    server->on("/api/ir/keys", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleListKeys(request);
    });
    server->on("/api/ir/keys", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleSaveKey(request);
    });
}

void IrRouteHandler::handleStatus(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    IRRemoteDriver& ir = IRRemoteDriver::getInstance();

    JsonDocument doc;
    doc["initialized"] = ir.isInitialized();
    doc["learnState"] = ir.getLearnStateName();
    doc["keyCount"] = (int)ir.getKeyCount();
    doc["lastCode"] = ir.getLastCode();
    ctx->sendSuccess(request, doc);
}

void IrRouteHandler::handleLearnStart(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    IRRemoteDriver& ir = IRRemoteDriver::getInstance();
    if (!ir.isInitialized()) {
        ctx->sendError(request, 400, "红外接收器未初始化，请先在外设管理中启用红外接收头");
        return;
    }

    uint32_t timeoutMs = 10000;
    if (request->hasParam("timeoutMs", true)) {
        long v = request->getParam("timeoutMs", true)->value().toInt();
        if (v > 0) timeoutMs = (uint32_t)v;
    }
    ir.startLearn(timeoutMs);

    JsonDocument doc;
    doc["learnState"] = ir.getLearnStateName();
    doc["remainingMs"] = ir.getLearnRemainingMs();
    ctx->sendSuccess(request, doc);
}

void IrRouteHandler::handleLearnStatus(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    IRRemoteDriver& ir = IRRemoteDriver::getInstance();

    JsonDocument doc;
    doc["learnState"] = ir.getLearnStateName();
    doc["remainingMs"] = ir.getLearnRemainingMs();
    doc["code"] = ir.getLearnedCode();
    doc["protocol"] = ir.getLearnedProtocol();
    // 若捕获的编码已存在于按键库，返回其名称（便于前端提示重复）
    ctx->sendSuccess(request, doc);
}

void IrRouteHandler::handleLearnStop(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    IRRemoteDriver::getInstance().stopLearn();
    ctx->sendSuccess(request, "已取消学习");
}

void IrRouteHandler::handleListKeys(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    IRRemoteDriver& ir = IRRemoteDriver::getInstance();
    // 按键映射体积小，直接拼装 envelope，避免二次解析
    String keys = ir.getKeysJson();
    String resp = "{\"success\":true,\"data\":{\"count\":" + String((int)ir.getKeyCount()) +
                  ",\"keys\":" + keys + "}}";
    request->send(200, "application/json", resp);
}

void IrRouteHandler::handleSaveKey(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    if (!request->hasParam("name", true) || !request->hasParam("code", true)) {
        ctx->sendError(request, 400, "缺少参数 name 或 code");
        return;
    }
    String name = request->getParam("name", true)->value();
    String code = request->getParam("code", true)->value();
    String protocol = request->hasParam("protocol", true)
                          ? request->getParam("protocol", true)->value()
                          : String("");
    name.trim();
    protocol.trim();

    String err;
    if (!IRRemoteDriver::getInstance().upsertKey(name, code, protocol, err)) {
        ctx->sendError(request, 400, err.length() ? err : String("保存失败"));
        return;
    }
    ctx->sendSuccess(request, "按键已保存");
}

void IrRouteHandler::handleDeleteKey(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    if (!request->hasParam("name", true)) {
        ctx->sendError(request, 400, "缺少参数 name");
        return;
    }
    String name = request->getParam("name", true)->value();
    name.trim();
    if (!IRRemoteDriver::getInstance().deleteKey(name)) {
        ctx->sendError(request, 404, "未找到该按键");
        return;
    }
    ctx->sendSuccess(request, "按键已删除");
}

void IrRouteHandler::handleClearKeys(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    IRRemoteDriver::getInstance().clearKeys();
    ctx->sendSuccess(request, "已清除全部按键");
}

#endif // FASTBEE_ENABLE_IR_REMOTE
