#ifndef PROTOCOL_ROUTE_HANDLER_H
#define PROTOCOL_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief 协议配置路由处理器
 * 
 * 处理 /api/protocol/config 的 GET 和 POST 请求
 * 处理 /api/modbus/status 和 /api/modbus/write 的 Modbus Master 接口
 * 处理 /api/mqtt/test 的 MQTT 连接测试接口
 */
class ProtocolRouteHandler {
public:
    explicit ProtocolRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleGetProtocolConfig(AsyncWebServerRequest* request);
    void handleSaveProtocolConfig(AsyncWebServerRequest* request);
    void handleGetModbusStatus(AsyncWebServerRequest* request);
    void handleModbusWrite(AsyncWebServerRequest* request);
    void handleTestMqttConnection(AsyncWebServerRequest* request);
    void handleGetMqttStatus(AsyncWebServerRequest* request);
    void handleMqttReconnect(AsyncWebServerRequest* request);
};

#endif // PROTOCOL_ROUTE_HANDLER_H
