#ifndef LOG_ROUTE_HANDLER_H
#define LOG_ROUTE_HANDLER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

class WebHandlerContext;

class LogRouteHandler {
public:
    explicit LogRouteHandler(WebHandlerContext* ctx);
    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleInfo(AsyncWebServerRequest* request);
    void handleList(AsyncWebServerRequest* request);
    void handleRead(AsyncWebServerRequest* request);
    void handleClear(AsyncWebServerRequest* request);
};

#endif // LOG_ROUTE_HANDLER_H
