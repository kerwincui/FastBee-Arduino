/**
 * @description: LCD显示路由处理器实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-04-15
 */

#include "network/handlers/LcdRouteHandler.h"
#include "network/handlers/HandlerUtils.h"
#include "network/WebHandlerContext.h"
#include "peripherals/LCDManager.h"
#include "core/PeripheralManager.h"
#include <ArduinoJson.h>

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
    if (!request->hasParam("text", true))
    {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing text parameter\"}");
        return;
    }
    
    String text = request->getParam("text", true)->value();
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t line = 255;  // 使用行号时设置
    uint8_t align = 1;   // 1=LEFT, 2=CENTER, 3=RIGHT
    
    // 解析可选参数
    if (request->hasParam("x", true))
    {
        x = request->getParam("x", true)->value().toInt();
    }
    if (request->hasParam("y", true))
    {
        y = request->getParam("y", true)->value().toInt();
    }
    if (request->hasParam("line", true))
    {
        line = request->getParam("line", true)->value().toInt();
    }
    if (request->hasParam("align", true))
    {
        align = request->getParam("align", true)->value().toInt();
    }
    
    LCDManager& lcd = LCDManager::getInstance();
    
    if (!lcd.isInitialized())
    {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"LCD not initialized\"}");
        return;
    }
    
    bool success = false;
    
    // 使用行号模式
    if (line != 255)
    {
        success = lcd.printLine(text, line);
    }
    // 使用坐标模式
    else
    {
        TextAlign textAlign = static_cast<TextAlign>(align);
        success = lcd.print(text, x, y, textAlign);
    }
    
    if (success)
    {
        lcd.refresh();
    }
    
    JsonDocument doc;
    doc["success"] = success;
    if (!success)
    {
        doc["error"] = "Failed to display text";
    }
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void LcdRouteHandler::handleClear(AsyncWebServerRequest* request)
{
    LCDManager& lcd = LCDManager::getInstance();
    
    if (!lcd.isInitialized())
    {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"LCD not initialized\"}");
        return;
    }
    
    lcd.clear();
    lcd.refresh();
    
    request->send(200, "application/json", "{\"success\":true}");
}

void LcdRouteHandler::handleShowInfo(AsyncWebServerRequest* request)
{
    LCDManager& lcd = LCDManager::getInstance();
    
    if (!lcd.isInitialized())
    {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"LCD not initialized\"}");
        return;
    }
    
    bool success = lcd.showSystemInfo();
    
    JsonDocument doc;
    doc["success"] = success;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void LcdRouteHandler::handleSetFont(AsyncWebServerRequest* request)
{
    if (!request->hasParam("font", true))
    {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing font parameter\"}");
        return;
    }
    
    uint8_t fontIndex = request->getParam("font", true)->value().toInt();
    
    LCDManager& lcd = LCDManager::getInstance();
    
    if (!lcd.isInitialized())
    {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"LCD not initialized\"}");
        return;
    }
    
    bool success = lcd.setFont(fontIndex);
    
    JsonDocument doc;
    doc["success"] = success;
    if (!success)
    {
        doc["error"] = "Invalid font index (0-2)";
    }
    else
    {
        doc["message"] = "Font set successfully";
    }
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
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
