#ifndef OTA_ROUTE_HANDLER_H
#define OTA_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>

class WebHandlerContext;
class OTAManager;

/**
 * @brief OTA固件升级路由处理器
 * 
 * 处理 /api/ota/* 的状态查询、触发升级、URL升级和文件上传
 */
class OTARouteHandler {
public:
    explicit OTARouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleOtaUpdate(AsyncWebServerRequest* request);
    void handleOtaStatus(AsyncWebServerRequest* request);
    void handleOtaUrl(AsyncWebServerRequest* request);
    void handleOtaUpload(AsyncWebServerRequest* request, const String& filename,
                         size_t index, uint8_t* data, size_t len, bool final);
};

#endif // OTA_ROUTE_HANDLER_H
