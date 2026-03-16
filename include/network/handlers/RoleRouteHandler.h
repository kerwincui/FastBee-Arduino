#ifndef ROLE_ROUTE_HANDLER_H
#define ROLE_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief 角色管理路由处理器
 * 
 * 处理 /api/roles/*, /api/permissions, /api/audit/* 路由
 */
class RoleRouteHandler {
public:
    explicit RoleRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleGetRoles(AsyncWebServerRequest* request);
    void handleGetRole(AsyncWebServerRequest* request);
    void handleCreateRole(AsyncWebServerRequest* request);
    void handleUpdateRole(AsyncWebServerRequest* request);
    void handleDeleteRole(AsyncWebServerRequest* request);
    void handleGetRolePermissions(AsyncWebServerRequest* request);
    void handleSetRolePermissions(AsyncWebServerRequest* request);
    void handleUpdateRoleByPost(AsyncWebServerRequest* request);
    void handleDeleteRoleByPost(AsyncWebServerRequest* request);
    void handleSetRolePermissionsByPost(AsyncWebServerRequest* request);
    void handleGetPermissions(AsyncWebServerRequest* request);
    void handleGetAuditLog(AsyncWebServerRequest* request);
    void handleClearAuditLog(AsyncWebServerRequest* request);

    // 辅助：从逗号分隔字符串解析权限列表
    std::vector<String> parsePermissionList(const String& permsParam);
};

#endif // ROLE_ROUTE_HANDLER_H
