#ifndef SYSTEM_ROUTE_HANDLER_H
#define SYSTEM_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>
#include <Arduino.h>

class WebHandlerContext;

/**
 * @brief 系统管理路由处理器
 * 
 * 处理 /api/system/*, /api/network/*, /api/files/*, /api/config,
 * /api/health, /api/filesystem 等系统核心路由
 */
class SystemRouteHandler {
public:
    explicit SystemRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    // 状态响应缓存
    struct ResponseCache {
        String json;
        unsigned long timestamp;
        bool valid;
        ResponseCache() : timestamp(0), valid(false) {}
    };
    ResponseCache _statusCache;
    static constexpr unsigned long STATUS_CACHE_TTL = 5000; // 5秒

    void handleSystemInfo(AsyncWebServerRequest* request);
    void handleSystemStatus(AsyncWebServerRequest* request);
    void handleSystemRestart(AsyncWebServerRequest* request);
    void handleNetworkConfig(AsyncWebServerRequest* request);
    void handleSaveNetworkConfig(AsyncWebServerRequest* request);
    void handleGetFilesList(AsyncWebServerRequest* request);
    void handleGetHealth(AsyncWebServerRequest* request);
    void handleSystemMetrics(AsyncWebServerRequest* request);
    void handleGetCapabilities(AsyncWebServerRequest* request);
};

#endif // SYSTEM_ROUTE_HANDLER_H
