#ifndef PERIPH_EXEC_ROUTE_HANDLER_H
#define PERIPH_EXEC_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

class WebHandlerContext;
struct PeriphExecRule;

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
    void handleGetStaticEvents(AsyncWebServerRequest* request);
    void handleGetDynamicEvents(AsyncWebServerRequest* request);
    void handleGetEventCategories(AsyncWebServerRequest* request);
    void handleGetTriggerTypes(AsyncWebServerRequest* request);
    void handleGetControls(AsyncWebServerRequest* request);

    // JSON body handlers（支持 triggers[]/actions[] 数组）
    void handleAddRuleJson(AsyncWebServerRequest* request, JsonVariant& json);
    void handleUpdateRuleJson(AsyncWebServerRequest* request, JsonVariant& json);

    // JSON → PeriphExecRule 解析辅助
    void parseRuleFromJson(JsonObject& obj, PeriphExecRule& rule);

    // 完整序列化单条规则（含 triggers/actions 数组）到 JSON 字符串
    String serializeRuleFull(const PeriphExecRule& rule);
};

#endif // PERIPH_EXEC_ROUTE_HANDLER_H
