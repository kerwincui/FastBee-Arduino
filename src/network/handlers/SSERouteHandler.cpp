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

    _events.onConnect([](AsyncEventSourceClient* client) {
        LOG_INFOF("SSE client connected, id: %p", static_cast<void*>(client));
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

    _events.send(data.c_str(), "modbus-data", _messageId++);
}

size_t SSERouteHandler::clientCount() const {
    return _events.count();
}
