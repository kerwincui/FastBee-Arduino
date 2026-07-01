#ifndef PROVISION_ROUTE_HANDLER_H
#define PROVISION_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief WiFi 引导配网路由处理器
 * 
 * 处理 /setup 引导页与 /api/wifi/* 配网相关路由；
 * 支持手机/移动端通过 AP 热点调用接口完成 WiFi 配置和设备参数下发。
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

    // 将配网下发的扩展参数（userId/deviceNum/extra）写入 device.json
    void _updateDeviceConfig(const String& userId,
                             const String& deviceNum,
                             const String& extra);
};

#endif // PROVISION_ROUTE_HANDLER_H
