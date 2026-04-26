#ifndef PROTOCOL_ROUTE_HANDLER_H
#define PROTOCOL_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>
#include <memory>
#include "core/FeatureFlags.h"

class WebHandlerContext;
#if FASTBEE_ENABLE_MODBUS
class ModbusRouteHandler;
#endif
#if FASTBEE_ENABLE_MQTT
class MqttRouteHandler;
#endif

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
#if FASTBEE_ENABLE_MODBUS
    std::unique_ptr<ModbusRouteHandler> modbusHandler;
#endif
#if FASTBEE_ENABLE_MQTT
    std::unique_ptr<MqttRouteHandler> mqttHandler;
#endif

    void handleGetProtocolConfig(AsyncWebServerRequest* request);
    void handleSaveProtocolConfig(AsyncWebServerRequest* request);
};

#endif // PROTOCOL_ROUTE_HANDLER_H
