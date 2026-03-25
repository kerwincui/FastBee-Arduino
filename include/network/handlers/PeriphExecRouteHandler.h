#ifndef PERIPH_EXEC_ROUTE_HANDLER_H
#define PERIPH_EXEC_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief 外设执行路由处理器
 * 
 * 处理 /api/periph-exec/* 的外设执行 CRUD、启用/禁用等
 */
class PeriphExecRouteHandler {
public:
    explicit PeriphExecRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleGetRules(AsyncWebServerRequest* request);
    void handleAddRule(AsyncWebServerRequest* request);
    void handleUpdateRule(AsyncWebServerRequest* request);
    void handleDeleteRule(AsyncWebServerRequest* request);
    void handleEnableRule(AsyncWebServerRequest* request);
    void handleDisableRule(AsyncWebServerRequest* request);
    void handleRunOnce(AsyncWebServerRequest* request);
};

#endif // PERIPH_EXEC_ROUTE_HANDLER_H
