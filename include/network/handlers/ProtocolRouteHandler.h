#ifndef PROTOCOL_ROUTE_HANDLER_H
#define PROTOCOL_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>
#include <memory>

class WebHandlerContext;
class ModbusRouteHandler;
class MqttRouteHandler;

/**
 * @brief 协议配置路由处理器
 * 
 * 处理 /api/protocol/config 的 GET 和 POST 请求
 * 组合 ModbusRouteHandler 和 MqttRouteHandler 处理各自的 API
 */
class ProtocolRouteHandler {
public:
    explicit ProtocolRouteHandler(WebHandlerContext* ctx);
    ~ProtocolRouteHandler();

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;
    std::unique_ptr<ModbusRouteHandler> modbusHandler;
    std::unique_ptr<MqttRouteHandler> mqttHandler;

    void handleGetProtocolConfig(AsyncWebServerRequest* request);
    void handleSaveProtocolConfig(AsyncWebServerRequest* request);
};

#endif // PROTOCOL_ROUTE_HANDLER_H
