#ifndef PERIPHERAL_ROUTE_HANDLER_H
#define PERIPHERAL_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief 外设接口路由处理器
 * 
 * 处理 /api/peripherals/* 的外设 CRUD、启用/禁用、读写数据等
 */
class PeripheralRouteHandler {
public:
    explicit PeripheralRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleGetPeripherals(AsyncWebServerRequest* request);
    void handleGetPeripheralTypes(AsyncWebServerRequest* request);
    void handleGetPeripheral(AsyncWebServerRequest* request);
    void handleAddPeripheral(AsyncWebServerRequest* request);
    void handleUpdatePeripheral(AsyncWebServerRequest* request);
    void handleDeletePeripheral(AsyncWebServerRequest* request);
    void handleEnablePeripheral(AsyncWebServerRequest* request);
    void handleDisablePeripheral(AsyncWebServerRequest* request);
    void handleGetPeripheralStatus(AsyncWebServerRequest* request);
    void handleReadPeripheral(AsyncWebServerRequest* request);
    void handleWritePeripheral(AsyncWebServerRequest* request);
};

#endif // PERIPHERAL_ROUTE_HANDLER_H
