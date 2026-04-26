#ifndef USER_ROUTE_HANDLER_H
#define USER_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief 用户管理路由处理器
 * 
 * 处理 /api/users/* 路由：用户CRUD、在线用户、密码重置
 */
class UserRouteHandler {
public:
    explicit UserRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleGetUsers(AsyncWebServerRequest* request);
    void handleAddUser(AsyncWebServerRequest* request);
    void handleUpdateUser(AsyncWebServerRequest* request);
    void handleDeleteUser(AsyncWebServerRequest* request);
    void handleGetOnlineUsers(AsyncWebServerRequest* request);
    void handleResetPassword(AsyncWebServerRequest* request);
};

#endif // USER_ROUTE_HANDLER_H
