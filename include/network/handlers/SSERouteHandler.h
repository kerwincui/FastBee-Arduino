#ifndef SSE_ROUTE_HANDLER_H
#define SSE_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>
#include <AsyncEventSource.h>

class WebHandlerContext;

class SSERouteHandler {
public:
    explicit SSERouteHandler(WebHandlerContext* ctx);
    void setupRoutes(AsyncWebServer* server);

    void broadcastModbusData(const String& data);
    void broadcastMqttStatus(const String& data);
    void broadcastModbusStatus(const String& data);
    size_t clientCount() const;

    // 最大 SSE 客户端数，防止多客户端导致内存压力
    static constexpr size_t MAX_SSE_CLIENTS = 3;
    // 单次消息最大字节数，超过则截断
    static constexpr size_t MAX_SSE_MESSAGE_SIZE = 1024;

private:
    WebHandlerContext* ctx;
    AsyncEventSource _events;
    uint32_t _messageId;
};

#endif // SSE_ROUTE_HANDLER_H
