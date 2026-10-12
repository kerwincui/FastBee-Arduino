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

namespace {
const char* irLearnStateName(IrLearnState state) {
    switch (state) {
        case IrLearnState::LEARNING: return "learning";
        case IrLearnState::CAPTURED: return "captured";
        case IrLearnState::TIMEOUT: return "timeout";
        default: return "idle";
    }
}
int irKeyErrorStatus(const String& error) {
    if (error == "IR_KEY_NOT_FOUND") return 404;
    if (error.startsWith("IR_KEYS_") && error != "IR_KEYS_LIMIT") return 503;
    if (error == "IR_UNAVAILABLE") return 503;
    return 400;
}
bool parseIrLearnTimeout(const String& text, uint32_t& timeout) {
    if (text.isEmpty() || text.length() > 5) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + static_cast<uint32_t>(text[i] - '0');
    }
    if (value < 1000 || value > 60000) return false;
    timeout = value; return true;
}
}

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
    IrStatusSnapshot snapshot;
    if (!ir.getSnapshot(snapshot)) { ctx->sendError(request, 503, "IR 状态暂不可用，请稍后重试"); return; }

    JsonDocument doc;
    doc["initialized"] = snapshot.initialized;
    doc["stopping"] = snapshot.stopping;
    doc["learnState"] = irLearnStateName(snapshot.learnState);
    doc["keyCount"] = snapshot.keyCount;
    doc["lastCode"] = snapshot.lastCode;
    ctx->sendSuccess(request, doc);
}

void IrRouteHandler::handleLearnStart(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    IRRemoteDriver& ir = IRRemoteDriver::getInstance();

    uint32_t timeoutMs = 10000;
    if (request->hasParam("timeoutMs", true)) {
        if (!parseIrLearnTimeout(request->getParam("timeoutMs", true)->value(), timeoutMs)) {
            ctx->sendError(request, 400, "timeoutMs 必须为 1000~60000 的整数"); return;
        }
    }
    if (!ir.startLearn(timeoutMs)) {
        ctx->sendError(request, 409, "红外接收器未就绪、忙或正在停止，请稍后重试"); return;
    }
    IrStatusSnapshot snapshot;
    if (!ir.getSnapshot(snapshot)) { ctx->sendError(request, 503, "学习已请求，状态暂不可用，请查询状态"); return; }

    JsonDocument doc;
    doc["learnState"] = irLearnStateName(snapshot.learnState);
    doc["remainingMs"] = snapshot.remainingMs;
    ctx->sendSuccess(request, doc);
}

void IrRouteHandler::handleLearnStatus(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    IRRemoteDriver& ir = IRRemoteDriver::getInstance();
    IrStatusSnapshot snapshot;
    if (!ir.getSnapshot(snapshot)) { ctx->sendError(request, 503, "IR 状态暂不可用，请稍后重试"); return; }

    JsonDocument doc;
    doc["learnState"] = irLearnStateName(snapshot.learnState);
    doc["remainingMs"] = snapshot.remainingMs;
    doc["code"] = snapshot.learnedCode;
    doc["protocol"] = snapshot.learnedProtocol;
    // 若捕获的编码已存在于按键库，返回其名称（便于前端提示重复）
    ctx->sendSuccess(request, doc);
}

void IrRouteHandler::handleLearnStop(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    if (!IRRemoteDriver::getInstance().stopLearn()) {
        ctx->sendError(request, 503, "取消学习未确认，请稍后重试"); return;
    }
    ctx->sendSuccess(request, "已取消学习");
}

void IrRouteHandler::handleListKeys(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    IRRemoteDriver& ir = IRRemoteDriver::getInstance();
    // 按键映射体积小，直接拼装 envelope，避免二次解析
    String keys;
    size_t count = 0;
    IrStatusSnapshot snapshot;
    if (!ir.getSnapshot(snapshot) || (!snapshot.keysLoaded && !ir.loadKeyMap()) || !ir.getKeySnapshot(keys, count)) {
        ctx->sendError(request, 503, "按键库暂不可用，请稍后重试"); return;
    }
    String resp = "{\"success\":true,\"data\":{\"count\":" + String((int)count) +
                  ",\"keys\":" + keys + "}}";
    const size_t expected = sizeof("{\"success\":true,\"data\":{\"count\":") - 1 +
                            String((int)count).length() + sizeof(",\"keys\":") - 1 + keys.length() + 2;
    if (resp.length() != expected) { ctx->sendError(request, 503, "按键库响应内存不足"); return; }
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
        ctx->sendError(request, irKeyErrorStatus(err), err.length() ? err : String("保存失败"));
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
    String error;
    if (!IRRemoteDriver::getInstance().deleteKey(name, &error)) {
        ctx->sendError(request, irKeyErrorStatus(error), error);
        return;
    }
    ctx->sendSuccess(request, "按键已删除");
}

void IrRouteHandler::handleClearKeys(AsyncWebServerRequest* request)
{
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String error;
    if (!IRRemoteDriver::getInstance().clearKeys(&error)) {
        ctx->sendError(request, irKeyErrorStatus(error), error); return;
    }
    ctx->sendSuccess(request, "已清除全部按键");
}

#endif // FASTBEE_ENABLE_IR_REMOTE
