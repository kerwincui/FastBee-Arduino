#pragma once

#include "core/FeatureFlags.h"

#if FASTBEE_ENABLE_SCREEN_PANEL || FASTBEE_ENABLE_POINT_HISTORY

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

class WebHandlerContext;

class ScreenPanelRouteHandler {
public:
    explicit ScreenPanelRouteHandler(WebHandlerContext* ctx);
    void setupRoutes(AsyncWebServer* server);

private:
#if FASTBEE_ENABLE_SCREEN_PANEL
    void handleGetConfig(AsyncWebServerRequest* request);
    void handleSaveConfig(AsyncWebServerRequest* request, JsonVariant& json);
    void handleChoices(AsyncWebServerRequest* request);
    void handleState(AsyncWebServerRequest* request);
#endif
#if FASTBEE_ENABLE_POINT_HISTORY
    void handleHistory(AsyncWebServerRequest* request);
    void handleGetHistoryConfig(AsyncWebServerRequest* request);
    void handleSaveHistoryConfig(AsyncWebServerRequest* request, JsonVariant& json);
#endif

    WebHandlerContext* _ctx;
};

#endif  // FASTBEE_ENABLE_SCREEN_PANEL || FASTBEE_ENABLE_POINT_HISTORY
