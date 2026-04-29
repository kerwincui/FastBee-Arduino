#ifndef PROVISION_ROUTE_HANDLER_H
#define PROVISION_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief WiFi 引导配网路由处理器
 * 
 * 处理 /setup 引导页与 /api/wifi/* 配网相关路由；
 * 项目已移除独立的蓝牙配网、AP 配网向导流程，已统一采用 AP+STA 双模自动切换。
 */
class ProvisionRouteHandler {
public:
    explicit ProvisionRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleSetupPage(AsyncWebServerRequest* request);
    void handleWiFiScan(AsyncWebServerRequest* request);
    void handleWiFiConnect(AsyncWebServerRequest* request);
};

#endif // PROVISION_ROUTE_HANDLER_H
