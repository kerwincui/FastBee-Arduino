#ifndef FEATURE_ROUTE_HANDLER_H
#define FEATURE_ROUTE_HANDLER_H

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief 运行时功能配置路由处理器
 *
 * 处理 /api/features/* 的状态查询、配置保存、保存并重启、资源预估
 */
class FeatureRouteHandler {
public:
    explicit FeatureRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    enum class PersistResult : uint8_t {
        SAVED,
        NO_VALID_ENTRIES,
        FEATURE_SAVE_FAILED,
        MDNS_MIRROR_FAILED
    };

    WebHandlerContext* ctx;

    void handleStatus(AsyncWebServerRequest* request);
    void handleConfig(AsyncWebServerRequest* request);
    void handleApply(AsyncWebServerRequest* request);
    void handleEstimate(AsyncWebServerRequest* request);

    // POST JSON body 收集（ESPAsyncWebServer 不自动缓存原始 body）
    void accumulateBody(AsyncWebServerRequest* request, uint8_t* data,
                        size_t len, size_t index, size_t total);
    // 解析累积的 body，返回是否成功；失败时已向客户端发送错误响应
    bool parseBody(AsyncWebServerRequest* request, JsonDocument& doc);
    PersistResult persistFeatureUpdate(const JsonDocument& doc);

    static constexpr size_t kMaxBodyBytes = 4096;
};

#endif // FEATURE_ROUTE_HANDLER_H
