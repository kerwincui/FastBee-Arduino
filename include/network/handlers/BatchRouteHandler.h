#ifndef BATCH_ROUTE_HANDLER_H
#define BATCH_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <Arduino.h>

class WebHandlerContext;

/**
 * @brief 批量请求路由处理器
 * 
 * 处理 /api/batch 端点，将多个 GET 请求合并为一次响应
 */
class BatchRouteHandler {
public:
    explicit BatchRouteHandler(WebHandlerContext* ctx);
    ~BatchRouteHandler();

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;
    AsyncCallbackJsonWebHandler* _batchJsonHandler = nullptr;

    void handleBatchRequest(AsyncWebServerRequest* request, JsonVariant& json);
    bool buildSubResponse(const String& url, JsonObject out);
};

#endif // BATCH_ROUTE_HANDLER_H
