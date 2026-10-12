#ifndef LOG_ROUTE_HANDLER_H
#define LOG_ROUTE_HANDLER_H

#include <Arduino.h>
#include <atomic>
#include <freertos/task.h>
#include <ESPAsyncWebServer.h>
#include "core/FeatureOwnerLifetime.h"

class WebHandlerContext;

class LogRouteHandler {
public:
    // RoutePsramScope permits even tiny ordinary allocations in PSRAM.
    // This handler owns cross-task atomics; its storage must stay internal.
    static void* operator new(size_t bytes) noexcept;
    static void operator delete(void* storage) noexcept;
    static void operator delete(void* storage, size_t bytes) noexcept;
    explicit LogRouteHandler(WebHandlerContext* ctx);
    ~LogRouteHandler();
    LogRouteHandler(const LogRouteHandler&) = delete;
    LogRouteHandler& operator=(const LogRouteHandler&) = delete;
    void setupRoutes(AsyncWebServer* server);
    void performMaintenance();

private:
    FastBee::FeatureOwnerLifetime featureOwner_{FastBee::ObservedFeatureOwner::LogViewer};
    WebHandlerContext* ctx;
    std::atomic<bool> fileReadActive_{false};
    std::atomic<bool> fileReadStopping_{false};
    std::atomic<TaskHandle_t> fileReadTask_{nullptr};
    std::atomic<bool> fileReadCompleted_{false};
    std::atomic<bool> fileReadCleanupBusy_{false};

    void handleInfo(AsyncWebServerRequest* request);
    void handleList(AsyncWebServerRequest* request);
    void handleRead(AsyncWebServerRequest* request);
    void handleClear(AsyncWebServerRequest* request);
};

#endif // LOG_ROUTE_HANDLER_H
