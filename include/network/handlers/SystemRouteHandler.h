#ifndef SYSTEM_ROUTE_HANDLER_H
#define SYSTEM_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>
#include <Arduino.h>
#include "core/FeatureFlags.h"

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
    void setupMigrationRoutes(AsyncWebServer* server);

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
    void handleNetworkStatus(AsyncWebServerRequest* request);
    void handleUpdateNetworkConfig(AsyncWebServerRequest* request, JsonVariant& json);
#if FASTBEE_ENABLE_CONFIG_TRANSFER
    void handleConfigTransferList(AsyncWebServerRequest* request);
    void handleConfigTransferExport(AsyncWebServerRequest* request);
    void handleConfigTransferImport(AsyncWebServerRequest* request);
    void handleConfigTransferImportBegin(AsyncWebServerRequest* request);
    void handleConfigTransferImportChunk(AsyncWebServerRequest* request);
#endif
#if FASTBEE_ENABLE_FILE_MANAGER
    void handleGetFilesList(AsyncWebServerRequest* request);
    void handleGetFileContent(AsyncWebServerRequest* request);
    void handleSaveFile(AsyncWebServerRequest* request);
    void handleGetFilesystem(AsyncWebServerRequest* request);
#endif
    void handleGetHealth(AsyncWebServerRequest* request);
    void handleSystemMetrics(AsyncWebServerRequest* request);
    void handleDiagBundle(AsyncWebServerRequest* request);
    void handleWebRuntime(AsyncWebServerRequest* request);
    void handleGetCapabilities(AsyncWebServerRequest* request);
    void handleFactoryReset(AsyncWebServerRequest* request);
    void handleGetWebConfig(AsyncWebServerRequest* request);
    void handleSaveWebConfig(AsyncWebServerRequest* request);
    // void handleGetMemoryProfile(AsyncWebServerRequest* request);  // Disabled
};

#endif // SYSTEM_ROUTE_HANDLER_H
