/**
 * @description: LCD显示路由处理器
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-04-15
 */

#ifndef LCD_ROUTE_HANDLER_H
#define LCD_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief LCD显示路由处理器
 * 
 * 处理所有 /api/lcd/* 端点：
 * - /api/lcd/text - 显示文本
 * - /api/lcd/clear - 清屏
 * - /api/lcd/info - 显示系统信息
 * - /api/lcd/font - 设置字体
 * - /api/lcd/status - 获取显示屏状态
 */
class LcdRouteHandler
{
public:
    explicit LcdRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleDisplayText(AsyncWebServerRequest* request);
    void handleClear(AsyncWebServerRequest* request);
    void handleShowInfo(AsyncWebServerRequest* request);
    void handleSetFont(AsyncWebServerRequest* request);
    void handleGetStatus(AsyncWebServerRequest* request);
};

#endif // LCD_ROUTE_HANDLER_H
