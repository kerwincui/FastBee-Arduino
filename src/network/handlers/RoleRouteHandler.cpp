#include "./network/handlers/RoleRouteHandler.h"
#include "./network/WebHandlerContext.h"
#include "./security/AuthManager.h"
#include "./security/RoleManager.h"
#include "systems/LoggerSystem.h"
#include <ArduinoJson.h>
#include <vector>

RoleRouteHandler::RoleRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

std::vector<String> RoleRouteHandler::parsePermissionList(const String& permsParam) {
    std::vector<String> permList;
    int start = 0;
    while (start < (int)permsParam.length()) {
        int comma = permsParam.indexOf(',', start);
        if (comma == -1) {
            String item = permsParam.substring(start);
            item.trim();
            if (!item.isEmpty()) permList.push_back(item);
            break;
        }
        String item = permsParam.substring(start, comma);
        item.trim();
        if (!item.isEmpty()) permList.push_back(item);
        start = comma + 1;
    }
    return permList;
}

void RoleRouteHandler::setupRoutes(AsyncWebServer* server) {
    // POST 子路由（必须在 /api/roles 之前注册）
    server->on("/api/roles/update", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleUpdateRoleByPost(request); });

    server->on("/api/roles/delete", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleDeleteRoleByPost(request); });

    server->on("/api/roles/permissions", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleSetRolePermissionsByPost(request); });

    server->on("/api/permissions", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetPermissions(request); });

    // 基础路由（exact 精确匹配）
    server->on(AsyncURIMatcher::exact("/api/roles"), HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetRoles(request); });

    server->on(AsyncURIMatcher::exact("/api/roles"), HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleCreateRole(request); });

    // 正则路由
    server->on("^\\/api\\/roles\\/([^\\/]+)$", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetRole(request); });

    server->on("^\\/api\\/roles\\/([^\\/]+)$", HTTP_PUT,
              [this](AsyncWebServerRequest* request) { handleUpdateRole(request); });

    server->on("^\\/api\\/roles\\/([^\\/]+)$", HTTP_DELETE,
              [this](AsyncWebServerRequest* request) { handleDeleteRole(request); });

    server->on("^\\/api\\/roles\\/([^\\/]+)\\/permissions$", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetRolePermissions(request); });

    server->on("^\\/api\\/roles\\/([^\\/]+)\\/permissions$", HTTP_PUT,
              [this](AsyncWebServerRequest* request) { handleSetRolePermissions(request); });

    // 审计日志
    server->on("/api/audit/logs", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetAuditLog(request); });

    server->on("/api/audit/logs", HTTP_DELETE,
              [this](AsyncWebServerRequest* request) { handleClearAuditLog(request); });
}

void RoleRouteHandler::handleGetRoles(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "role.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    if (!ctx->roleManager) {
        ctx->sendError(request, 500, "Role manager unavailable");
        return;
    }

    JsonDocument doc;
    JsonArray rolesArray = doc["roles"].to<JsonArray>();
    std::vector<Role> roles = ctx->roleManager->getAllRoles();
    for (const Role& role : roles) {
        JsonObject roleObj = rolesArray.add<JsonObject>();
        roleObj["id"] = role.id;
        roleObj["name"] = role.name;
        roleObj["description"] = role.description;
        roleObj["isBuiltin"] = role.isBuiltin;

        JsonArray permsArray = roleObj["permissions"].to<JsonArray>();
        for (const String& perm : role.permissions) {
            permsArray.add(perm);
        }
    }

    JsonArray permsDefArray = doc["permissions"].to<JsonArray>();
    std::vector<PermissionDef> permDefs = ctx->roleManager->getAllPermissions();
    for (const PermissionDef& perm : permDefs) {
        JsonObject permObj = permsDefArray.add<JsonObject>();
        permObj["id"] = perm.id;
        permObj["name"] = perm.name;
        permObj["description"] = perm.description;
        permObj["group"] = perm.group;
    }

    ctx->sendSuccess(request, doc);
}

void RoleRouteHandler::handleGetRole(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "role.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String path = request->url();
    int p = path.lastIndexOf('/');
    String roleId = (p != -1) ? path.substring(p + 1) : "";

    if (!ctx->roleManager || !ctx->roleManager->roleExists(roleId)) {
        ctx->sendNotFound(request);
        return;
    }

    String json = ctx->roleManager->roleToJson(roleId);
    AsyncWebServerResponse* resp = request->beginResponse(200, "application/json", json);
    resp->addHeader("Access-Control-Allow-Origin", "*");
    request->send(resp);
}

void RoleRouteHandler::handleCreateRole(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "role.create")) {
        ctx->sendUnauthorized(request);
        return;
    }

    if (!ctx->roleManager) {
        ctx->sendError(request, 500, "Role service unavailable");
        return;
    }

    String id   = ctx->getParamValue(request, "id", "");
    String name = ctx->getParamValue(request, "name", "");
    String desc = ctx->getParamValue(request, "description", "");

    if (id.isEmpty() || name.isEmpty()) {
        ctx->sendBadRequest(request, "id and name are required");
        return;
    }

    if (!ctx->roleManager->createRole(id, name, desc)) {
        ctx->sendError(request, 400, "Failed to create role (id may already exist)");
        return;
    }

    AuthResult ar = ctx->authenticateRequest(request);
    if (ctx->authManager) {
        static_cast<AuthManager*>(ctx->authManager)->recordAudit(
            ar.username, "role.create", id, "Created role: " + name,
            true, ctx->getClientIP(request));
    }
    ctx->sendSuccess(request, "Role created");
}

void RoleRouteHandler::handleUpdateRole(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "role.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String path = request->url();
    int p = path.lastIndexOf('/');
    String roleId = (p != -1) ? path.substring(p + 1) : "";

    if (!ctx->roleManager || !ctx->roleManager->roleExists(roleId)) {
        ctx->sendNotFound(request);
        return;
    }

    String name = ctx->getParamValue(request, "name", "");
    String desc = ctx->getParamValue(request, "description", "");

    if (!ctx->roleManager->updateRole(roleId, name, desc)) {
        ctx->sendError(request, 400, "Failed to update role");
        return;
    }

    AuthResult ar = ctx->authenticateRequest(request);
    if (ctx->authManager) {
        static_cast<AuthManager*>(ctx->authManager)->recordAudit(
            ar.username, "role.edit", roleId, "Updated role info",
            true, ctx->getClientIP(request));
    }
    ctx->sendSuccess(request, "Role updated");
}

void RoleRouteHandler::handleDeleteRole(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "role.delete")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String path = request->url();
    int p = path.lastIndexOf('/');
    String roleId = (p != -1) ? path.substring(p + 1) : "";

    if (!ctx->roleManager) {
        ctx->sendError(request, 500, "Role service unavailable");
        return;
    }

    if (!ctx->roleManager->deleteRole(roleId)) {
        ctx->sendError(request, 400, "Failed to delete role (builtin roles cannot be deleted)");
        return;
    }

    AuthResult ar = ctx->authenticateRequest(request);
    if (ctx->authManager) {
        static_cast<AuthManager*>(ctx->authManager)->recordAudit(
            ar.username, "role.delete", roleId, "Deleted role",
            true, ctx->getClientIP(request));
    }
    ctx->sendSuccess(request, "Role deleted");
}

void RoleRouteHandler::handleGetRolePermissions(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "role.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String path = request->url();
    String trimmed = path.substring(0, path.lastIndexOf('/'));
    int p = trimmed.lastIndexOf('/');
    String roleId = (p != -1) ? trimmed.substring(p + 1) : "";

    if (!ctx->roleManager || !ctx->roleManager->roleExists(roleId)) {
        ctx->sendNotFound(request);
        return;
    }

    JsonDocument doc;
    JsonArray arr = doc["permissions"].to<JsonArray>();
    for (const String& perm : ctx->roleManager->getRolePermissions(roleId)) {
        arr.add(perm);
    }
    ctx->sendSuccess(request, doc);
}

void RoleRouteHandler::handleSetRolePermissions(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "role.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String path = request->url();
    String trimmed = path.substring(0, path.lastIndexOf('/'));
    int p = trimmed.lastIndexOf('/');
    String roleId = (p != -1) ? trimmed.substring(p + 1) : "";

    if (!ctx->roleManager || !ctx->roleManager->roleExists(roleId)) {
        ctx->sendNotFound(request);
        return;
    }

    String permsParam = ctx->getParamValue(request, "permissions", "");
    std::vector<String> permList = parsePermissionList(permsParam);

    if (!ctx->roleManager->setRolePermissions(roleId, permList)) {
        ctx->sendError(request, 400, "Failed to set permissions");
        return;
    }

    AuthResult ar = ctx->authenticateRequest(request);
    if (ctx->authManager) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Set %u permissions", (unsigned)permList.size());
        static_cast<AuthManager*>(ctx->authManager)->recordAudit(
            ar.username, "role.set_permissions", roleId, buf,
            true, ctx->getClientIP(request));
    }
    ctx->sendSuccess(request, "Permissions updated");
}

void RoleRouteHandler::handleUpdateRoleByPost(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "role.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String roleId = ctx->getParamValue(request, "id", "");
    if (roleId.isEmpty()) {
        ctx->sendBadRequest(request, "Role id is required");
        return;
    }

    if (!ctx->roleManager || !ctx->roleManager->roleExists(roleId)) {
        ctx->sendNotFound(request);
        return;
    }

    if (roleId == "admin") {
        ctx->sendError(request, 400, "Cannot modify admin role");
        return;
    }

    String name = ctx->getParamValue(request, "name", "");
    String desc = ctx->getParamValue(request, "description", "");

    if (!ctx->roleManager->updateRole(roleId, name, desc)) {
        ctx->sendError(request, 400, "Failed to save role (storage error)");
        return;
    }

    AuthResult ar = ctx->authenticateRequest(request);
    if (ctx->authManager) {
        static_cast<AuthManager*>(ctx->authManager)->recordAudit(
            ar.username, "role.edit", roleId, "Updated role info",
            true, ctx->getClientIP(request));
    }
    ctx->sendSuccess(request, "Role updated");
}

void RoleRouteHandler::handleDeleteRoleByPost(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "role.delete")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String roleId = ctx->getParamValue(request, "id", "");
    if (roleId.isEmpty()) {
        ctx->sendBadRequest(request, "Role id is required");
        return;
    }

    if (!ctx->roleManager) {
        ctx->sendError(request, 500, "Role service unavailable");
        return;
    }

    if (roleId == "admin") {
        ctx->sendError(request, 400, "Cannot delete admin role");
        return;
    }

    if (!ctx->roleManager->roleExists(roleId)) {
        ctx->sendNotFound(request);
        return;
    }

    if (!ctx->roleManager->deleteRole(roleId)) {
        ctx->sendError(request, 400, "Failed to delete role");
        return;
    }

    AuthResult ar = ctx->authenticateRequest(request);
    if (ctx->authManager) {
        static_cast<AuthManager*>(ctx->authManager)->recordAudit(
            ar.username, "role.delete", roleId, "Deleted role",
            true, ctx->getClientIP(request));
    }
    ctx->sendSuccess(request, "Role deleted");
}

void RoleRouteHandler::handleSetRolePermissionsByPost(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "role.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String roleId = ctx->getParamValue(request, "id", "");
    if (roleId.isEmpty()) {
        ctx->sendBadRequest(request, "Role id is required");
        return;
    }

    if (!ctx->roleManager || !ctx->roleManager->roleExists(roleId)) {
        ctx->sendNotFound(request);
        return;
    }

    if (roleId == "admin") {
        ctx->sendError(request, 400, "Cannot modify admin role permissions");
        return;
    }

    String permsParam = ctx->getParamValue(request, "permissions", "");
    std::vector<String> permList = parsePermissionList(permsParam);

    if (!ctx->roleManager->setRolePermissions(roleId, permList)) {
        ctx->sendError(request, 400, "Failed to set permissions");
        return;
    }

    AuthResult ar = ctx->authenticateRequest(request);
    if (ctx->authManager) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Set %u permissions", (unsigned)permList.size());
        static_cast<AuthManager*>(ctx->authManager)->recordAudit(
            ar.username, "role.set_permissions", roleId, buf,
            true, ctx->getClientIP(request));
    }
    ctx->sendSuccess(request, "Permissions updated");
}

void RoleRouteHandler::handleGetPermissions(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "role.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    if (!ctx->roleManager) {
        ctx->sendError(request, 500, "Role service unavailable");
        return;
    }

    JsonDocument doc;
    JsonObject groups = doc["groups"].to<JsonObject>();
    for (const auto& kv : ctx->roleManager->getPermissionsByGroup()) {
        JsonArray grpArr = groups[kv.first].to<JsonArray>();
        for (const PermissionDef& pd : kv.second) {
            JsonObject pObj = grpArr.add<JsonObject>();
            pObj["id"]          = pd.id;
            pObj["name"]        = pd.name;
            pObj["description"] = pd.description;
        }
    }
    doc["total"] = ctx->roleManager->getAllPermissions().size();
    ctx->sendSuccess(request, doc);
}

void RoleRouteHandler::handleGetAuditLog(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "audit.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    if (!ctx->authManager) {
        ctx->sendError(request, 500, "Auth service unavailable");
        return;
    }

    int limit = ctx->getParamInt(request, "limit", 50);
    if (limit <= 0 || limit > 100) limit = 50;

    String json = static_cast<AuthManager*>(ctx->authManager)->getAuditLogJson((size_t)limit);
    AsyncWebServerResponse* resp = request->beginResponse(200, "application/json", json);
    resp->addHeader("Access-Control-Allow-Origin", "*");
    request->send(resp);
}

void RoleRouteHandler::handleClearAuditLog(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "audit.clear")) {
        ctx->sendUnauthorized(request);
        return;
    }

    if (!ctx->authManager) {
        ctx->sendError(request, 500, "Auth service unavailable");
        return;
    }

    AuthResult ar = ctx->authenticateRequest(request);
    static_cast<AuthManager*>(ctx->authManager)->clearAuditLog();
    static_cast<AuthManager*>(ctx->authManager)->recordAudit(
        ar.username, "audit.clear", "audit_log", "Audit log cleared",
        true, ctx->getClientIP(request));
    ctx->sendSuccess(request, "Audit log cleared");
}
