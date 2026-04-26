#ifndef DEVICE_ROUTE_HANDLER_H
#define DEVICE_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <Arduino.h>

class WebHandlerContext;

/**
 * @brief 设备信息路由处理器
 * 
 * 处理 /api/device/* 的设备配置、时间同步、设备信息等端点
 */
class DeviceRouteHandler {
public:
    explicit DeviceRouteHandler(WebHandlerContext* ctx);
    ~DeviceRouteHandler();

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;
    AsyncCallbackJsonWebHandler* _deviceJsonHandler = nullptr;

    void handleGetDeviceConfig(AsyncWebServerRequest* request);
    void handleSaveDeviceConfig(AsyncWebServerRequest* request);
    void handleSaveDeviceConfigJson(AsyncWebServerRequest* request, JsonVariant& json);
    void handleGetDeviceInfo(AsyncWebServerRequest* request);
};

#endif // DEVICE_ROUTE_HANDLER_H
