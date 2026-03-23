#ifndef RULE_SCRIPT_ROUTE_HANDLER_H
#define RULE_SCRIPT_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief 规则脚本路由处理器
 * 
 * 处理 /api/rule-script/* 的规则脚本 CRUD、启用/禁用等
 */
class RuleScriptRouteHandler {
public:
    explicit RuleScriptRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleGetRules(AsyncWebServerRequest* request);
    void handleAddRule(AsyncWebServerRequest* request);
    void handleUpdateRule(AsyncWebServerRequest* request);
    void handleDeleteRule(AsyncWebServerRequest* request);
    void handleEnableRule(AsyncWebServerRequest* request);
    void handleDisableRule(AsyncWebServerRequest* request);
};

#endif // RULE_SCRIPT_ROUTE_HANDLER_H
