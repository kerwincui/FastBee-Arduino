#ifndef STATIC_ROUTE_HANDLER_H
#define STATIC_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief 静态文件与页面路由处理器
 * 
 * 处理 SPA 路由（/login, /dashboard, /users 等）以及静态资源 fallback
 * 所有页面路由统一返回 index.html，由前端 SPA 处理页面切换
 */
class StaticRouteHandler {
public:
    explicit StaticRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleRootPage(AsyncWebServerRequest* request);
    void handleSPAPage(AsyncWebServerRequest* request);
    void handleNotFound(AsyncWebServerRequest* request);
};

#endif // STATIC_ROUTE_HANDLER_H
