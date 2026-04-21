#include "./network/handlers/SSERouteHandler.h"
#include "./network/WebHandlerContext.h"
#include "systems/LoggerSystem.h"

SSERouteHandler::SSERouteHandler(WebHandlerContext* ctx)
    : ctx(ctx), _events("/api/events"), _messageId(0) {
}

void SSERouteHandler::setupRoutes(AsyncWebServer* server) {
    if (!server) {
        return;
    }

    _events.onConnect([this](AsyncEventSourceClient* client) {
        // 限制最大客户端数，超过则断开新连接
        if (_events.count() > MAX_SSE_CLIENTS) {
            LOG_WARNING("SSE: Max clients exceeded, closing new connection");
            client->close();
            return;
        }
        LOG_INFOF("SSE client connected, id: %p (total: %u)", static_cast<void*>(client), _events.count());
    });

    _events.onDisconnect([](AsyncEventSourceClient* client) {
        (void)client;
        LOG_INFO("SSE client disconnected");
    });

    if (ctx && ctx->authManager) {
        _events.authorizeConnect([this](AsyncWebServerRequest* request) {
            if (!ctx || !request) {
                return false;
            }
            if (!ctx->requiresAuth(request)) {
                return true;
            }
            AuthResult authResult = ctx->authenticateRequest(request);
            return authResult.success;
        });
    }

    server->addHandler(&_events);
}

void SSERouteHandler::broadcastModbusData(const String& data) {
    if (_events.count() == 0) {
        return;
    }
    // 限制消息大小，避免大数据包阻塞 async_tcp
    if (data.length() > MAX_SSE_MESSAGE_SIZE) {
        LOG_WARNING("SSE: modbus-data message too large, truncated");
        String truncated = data.substring(0, MAX_SSE_MESSAGE_SIZE);
        _events.send(truncated.c_str(), "modbus-data", _messageId++);
        return;
    }
    _events.send(data.c_str(), "modbus-data", _messageId++);
}

void SSERouteHandler::broadcastMqttStatus(const String& data) {
    if (_events.count() == 0) {
        return;
    }
    _events.send(data.c_str(), "mqtt-status", _messageId++);
}

void SSERouteHandler::broadcastModbusStatus(const String& data) {
    if (_events.count() == 0) {
        return;
    }
    _events.send(data.c_str(), "modbus-status", _messageId++);
}

size_t SSERouteHandler::clientCount() const {
    return _events.count();
}
