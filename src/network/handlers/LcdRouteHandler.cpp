/**
 * @description: LCD显示路由处理器实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-04-15
 */

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_LCD

#include "network/handlers/LcdRouteHandler.h"
#include "network/handlers/HandlerUtils.h"
#include "network/WebHandlerContext.h"
#include "peripherals/LCDManager.h"
#include "core/PeripheralManager.h"
#include <ArduinoJson.h>
#include "network/HttpOutputControl.h"
#include "core/ActionValueContract.h"

namespace {
bool readLcdInteger(AsyncWebServerRequest* request, const char* name,
    long fallback, long minimum, long maximum, long& value) {
    value = fallback;
    if (!request->hasParam(name, true)) return true;
    const String text = request->getParam(name, true)->value();
    // strtol also accepts leading whitespace/signs; these form fields are
    // unsigned decimal coordinates, lines and font identifiers.
    for (size_t i = 0; i < text.length(); ++i)
        if (text[i] < '0' || text[i] > '9') return false;
    return FastBee::parseStrictActionLong(text.c_str(), text.length(), value) &&
        value >= minimum && value <= maximum;
}

void submitLcdHttp(WebHandlerContext* web, AsyncWebServerRequest* request,
    FastBee::LcdOutputCommand command) {
    using Command = FastBee::LcdOutputCommand;
    if (!web->requireAuth(request) || !web->requireDeveloperMode(request)) return;
    auto invalid = [&]() { web->sendError(request, 400, "LCD_PARAMETERS_INVALID"); };
    if (request->params() > 5) { invalid(); return; }
    for (size_t i = 0; i < request->params(); ++i) {
        const auto* field = request->getParam(i);
        if (!field || !field->isPost() || field->isFile()) { invalid(); return; }
        const String& name = field->name();
        const bool known = command == Command::TextAt
            ? name == "text" || name == "x" || name == "y" || name == "line" || name == "align"
            : command == Command::Font && name == "font";
        if (!known) { invalid(); return; }
        for (size_t j = 0; j < i; ++j)
            if (request->getParam(j)->name() == name) { invalid(); return; }
    }
    FastBee::LcdOutputRequest output;
    output.command = command;
    if (command == Command::TextAt) {
        if (!request->hasParam("text", true)) { invalid(); return; }
        output.text = request->getParam("text", true)->value();
        if (output.text.length() > 1024 || output.text.length() != strlen(output.text.c_str())) {
            invalid(); return;
        }
        long x = 0, y = 0, line = -1, align = 1;
        if (!readLcdInteger(request, "x", 0, 0, 255, x) ||
            !readLcdInteger(request, "y", 0, 0, 255, y) ||
            !readLcdInteger(request, "line", -1, 0, 255, line) ||
            !readLcdInteger(request, "align", 1, 1, 3, align)) { invalid(); return; }
        output.x = static_cast<uint8_t>(x); output.y = static_cast<uint8_t>(y);
        output.align = static_cast<uint8_t>(align);
        if (line >= 0 && line != 255) {
            output.command = Command::TextLine;
            output.value = static_cast<int16_t>(line);
        }
    } else if (command == Command::Font) {
        long font = 0;
        if (!request->hasParam("font", true) || !readLcdInteger(request, "font", 0, 0, 2, font)) {
            invalid(); return;
        }
        output.value = static_cast<int16_t>(font);
    }
    auto& pm = PeripheralManager::getInstance();
    String id; uint32_t epoch = 0;
    if (!pm.getActiveLcdTarget(id, epoch)) {
        web->sendError(request, 409, "LCD_TARGET_UNCONFIRMED"); return;
    }
    const auto auth = web->authenticateRequest(request);
    if (!auth.success || !auth.outputActor) {
        web->sendError(request, 401, "CONTROL_SESSION_UNCONFIRMED"); return;
    }
    FastBee::OrdinaryControlContext context;
    if (!pm.beginOrdinaryControl(FastBee::OrdinaryControl::Source::Http, auth.outputActor,
        FastBee::OrdinaryControl::DefaultManualHoldMs, false, 0, "", epoch, {}, context)) {
        web->sendError(request, 409, "CONTROL_CONTEXT_UNCONFIRMED"); return;
    }
    FastBee::OrdinaryControlResult result;
    const bool accepted = pm.submitOrdinaryLcd(id, output, &context, &result);
    pm.finishOrdinaryControl(context, false);
    JsonDocument doc;
    doc["success"] = accepted; doc["reason"] = result.reason;
    if (!accepted) doc["error"] = result.reason;
    JsonObject data = doc["data"].to<JsonObject>();
    data["id"] = id; data["configEpoch"] = epoch;
    JsonObject control = data["control"].to<JsonObject>();
    FastBee::writeOrdinaryControlJson(control, result);
    // This legacy endpoint does not expose an override review workflow.
    control["canConfirmOverride"] = false;
    if (doc.overflowed()) { web->sendError(request, 503, "CONTROL_RESPONSE_UNCONFIRMED"); return; }
    web->sendJsonResponse(request, accepted ? 200 : result.attempted ? 503 : 409, doc);
}
} // namespace

LcdRouteHandler::LcdRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx)
{
}

void LcdRouteHandler::setupRoutes(AsyncWebServer* server)
{
    // 显示文本
    server->on("/api/lcd/text", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleDisplayText(request);
    });
    
    // 清屏
    server->on("/api/lcd/clear", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleClear(request);
    });
    
    // 显示系统信息
    server->on("/api/lcd/info", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleShowInfo(request);
    });
    
    // 设置字体
    server->on("/api/lcd/font", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleSetFont(request);
    });
    
    // 获取状态
    server->on("/api/lcd/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleGetStatus(request);
    });
}

void LcdRouteHandler::handleDisplayText(AsyncWebServerRequest* request)
{
    submitLcdHttp(ctx, request, FastBee::LcdOutputCommand::TextAt);
}

void LcdRouteHandler::handleClear(AsyncWebServerRequest* request)
{
    submitLcdHttp(ctx, request, FastBee::LcdOutputCommand::Clear);
}

void LcdRouteHandler::handleShowInfo(AsyncWebServerRequest* request)
{
    submitLcdHttp(ctx, request, FastBee::LcdOutputCommand::SystemInfo);
}

void LcdRouteHandler::handleSetFont(AsyncWebServerRequest* request)
{
    submitLcdHttp(ctx, request, FastBee::LcdOutputCommand::Font);
}

void LcdRouteHandler::handleGetStatus(AsyncWebServerRequest* request)
{
    LCDManager& lcd = LCDManager::getInstance();
    
    JsonDocument doc;
    JsonObject data = doc["data"].to<JsonObject>();
    
    data["initialized"] = lcd.isInitialized();
    data["width"] = lcd.getWidth();
    data["height"] = lcd.getHeight();
    data["maxLines"] = lcd.getMaxLines();
    data["fontHeight"] = lcd.getFontHeight();
    
    doc["success"] = true;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

#endif // FASTBEE_ENABLE_LCD
