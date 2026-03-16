#ifndef AUTH_ROUTE_HANDLER_H
#define AUTH_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief 认证路由处理器
 * 
 * 处理 /api/auth/* 的登录、登出、会话验证和密码修改
 */
class AuthRouteHandler {
public:
    explicit AuthRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleLogin(AsyncWebServerRequest* request);
    void handleLogout(AsyncWebServerRequest* request);
    void handleVerifySession(AsyncWebServerRequest* request);
    void handleChangePassword(AsyncWebServerRequest* request);
};

#endif // AUTH_ROUTE_HANDLER_H
