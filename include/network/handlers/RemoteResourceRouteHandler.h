#pragma once
#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_REMOTE_RESOURCES
class WebHandlerContext;
class AsyncWebServer;
class AsyncWebServerRequest;
class RemoteResourceRouteHandler {
public:
    explicit RemoteResourceRouteHandler(WebHandlerContext* ctx):ctx_(ctx) {}
    void setupRoutes(AsyncWebServer* server);
private:
    WebHandlerContext* ctx_;
    void save(AsyncWebServerRequest* request);
    void weatherTestStatus(AsyncWebServerRequest* request);
    void requestWeatherTest(AsyncWebServerRequest* request);
};
#endif
