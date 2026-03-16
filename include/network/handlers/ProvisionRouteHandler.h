#ifndef PROVISION_ROUTE_HANDLER_H
#define PROVISION_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief 配网与设备配置路由处理器
 * 
 * 处理 /setup, /api/wifi/*, /api/ble/* 和 AP/BLE 配网相关路由
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
