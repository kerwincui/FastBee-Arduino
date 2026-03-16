#ifndef STATIC_ROUTE_HANDLER_H
#define STATIC_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief 静态文件与页面路由处理器
 * 
 * 处理 /login, /dashboard, /users, / 以及静态资源 fallback
 */
class StaticRouteHandler {
public:
    explicit StaticRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleLoginPage(AsyncWebServerRequest* request);
    void handleDashboardPage(AsyncWebServerRequest* request);
    void handleUsersPage(AsyncWebServerRequest* request);
    void handleRootPage(AsyncWebServerRequest* request);
    void handleSPAPage(AsyncWebServerRequest* request);
    void handleNotFound(AsyncWebServerRequest* request);
};

#endif // STATIC_ROUTE_HANDLER_H
