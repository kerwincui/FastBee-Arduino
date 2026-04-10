#ifndef MQTT_ROUTE_HANDLER_H
#define MQTT_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief MQTT协议路由处理器
 * 
 * 处理所有 /api/mqtt/* 端点：
 * - /api/mqtt/test - 测试MQTT连接
 * - /api/mqtt/status - 获取MQTT状态
 * - /api/mqtt/reconnect - 重连MQTT
 * - /api/mqtt/disconnect - 断开MQTT连接
 * - /api/mqtt/ntp-sync - NTP时间同步
 */
class MqttRouteHandler {
public:
    explicit MqttRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleTestMqttConnection(AsyncWebServerRequest* request);
    void handleGetMqttStatus(AsyncWebServerRequest* request);
    void handleMqttReconnect(AsyncWebServerRequest* request);
    void handleMqttDisconnect(AsyncWebServerRequest* request);
    void handleMqttNtpSync(AsyncWebServerRequest* request);
};

#endif // MQTT_ROUTE_HANDLER_H
