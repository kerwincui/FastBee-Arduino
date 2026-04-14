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

private:
    WebHandlerContext* ctx;
    AsyncEventSource _events;
    uint32_t _messageId;
};

#endif // SSE_ROUTE_HANDLER_H
