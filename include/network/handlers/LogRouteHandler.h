#ifndef LOG_ROUTE_HANDLER_H
#define LOG_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>
#include <Arduino.h>

class WebHandlerContext;

/**
 * @brief 日志管理路由处理器
 * 
 * 处理 /api/logs/*, /api/system/logs/* 等日志相关路由
 */
class LogRouteHandler {
public:
    explicit LogRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleGetLogsList(AsyncWebServerRequest* request);
    void handleGetLogContent(AsyncWebServerRequest* request);
    void handleDeleteLog(AsyncWebServerRequest* request);
    void handleGetLogLevel(AsyncWebServerRequest* request);
    void handleSetLogLevel(AsyncWebServerRequest* request);
};

#endif // LOG_ROUTE_HANDLER_H
