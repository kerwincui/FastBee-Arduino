#include "./network/handlers/UserRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./security/AuthManager.h"
#include "./security/UserManager.h"
#include "systems/LoggerSystem.h"
#include <ArduinoJson.h>

UserRouteHandler::UserRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void UserRouteHandler::setupRoutes(AsyncWebServer* server) {
    server->on("/api/users/update", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleUpdateUser(request); });

    server->on("/api/users/online", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetOnlineUsers(request); });

    server->on("/api/users/reset-password", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleResetPassword(request); });

    server->on("/api/users/unlock-account", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->checkPermission(request, "user.edit")) {
            ctx->sendUnauthorized(request);
            return;
        }
        String username = ctx->getParamValue(request, "username", "");
        if (username.isEmpty()) {
            ctx->sendBadRequest(request, "Username is required");
            return;
        }
        if (ctx->userManager) {
            ctx->userManager->unlockAccount(username);
        }
        if (ctx->authManager) {
            ctx->authManager->unlockAccount(username);
        }
        ctx->sendSuccess(request, "Account unlocked successfully");
    });

    server->on(AsyncURIMatcher::exact("/api/users"), HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetUsers(request); });

    server->on(AsyncURIMatcher::exact("/api/users"), HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleAddUser(request); });

    // REST风格路由
    server->on("^\\/api\\/users\\/([^\\/]+)$", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        // 从URL路径提取用户名
        String path = request->url();
        int lastSlash = path.lastIndexOf('/');
        if (lastSlash == -1) {
            ctx->sendError(request, 400, "Invalid URL");
            return;
        }
        String username = path.substring(lastSlash + 1);
        if (!ctx->userManager) {
            ctx->sendError(request, 500, "User service unavailable");
            return;
        }
        User* user = ctx->userManager->getUser(username);
        if (!user) {
            ctx->sendError(request, 404, "User not found");
            return;
        }
        JsonDocument doc;
        doc["username"] = user->username;
        doc["role"] = UserManager::roleToString(user->role);
        doc["enabled"] = user->enabled;
        doc["createTime"] = user->createTime;
        doc["lastLogin"] = user->lastLogin;
        doc["lastModified"] = user->lastModified;
        bool isOnline = ctx->authManager ? ctx->authManager->isUserOnline(username) : false;
        doc["isOnline"] = isOnline;
        doc["isLocked"] = ctx->userManager->isAccountLocked(username);
        doc["loginAttempts"] = ctx->userManager->getLoginAttempts(username);
        ctx->sendSuccess(request, doc);
    });

    server->on("^\\/api\\/users\\/([^\\/]+)$", HTTP_DELETE,
              [this](AsyncWebServerRequest* request) { handleDeleteUser(request); });
}

void UserRouteHandler::handleGetUsers(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "user.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    if (!ctx->userManager) {
        ctx->sendError(request, 500, "User service unavailable");
        return;
    }

    int page = ctx->getParamInt(request, "page", 1);
    int limit = ctx->getParamInt(request, "limit", 50);
    String filter = ctx->getParamValue(request, "filter", "");

    if (page < 1) page = 1;
    if (limit < 1 || limit > 200) limit = 50;

    JsonDocument doc;
    JsonArray usersArray = doc["users"].to<JsonArray>();

    String usersJson = ctx->userManager->getAllUsers();
    JsonDocument usersDoc;
    DeserializationError error = deserializeJson(usersDoc, usersJson);
    if (error) {
        ctx->sendError(request, 500, "Failed to parse users data");
        return;
    }

    JsonArray allUsers = usersDoc["users"].as<JsonArray>();
    int totalCount = allUsers.size();
    int startIndex = (page - 1) * limit;
    int count = 0;

    for (int i = 0; i < totalCount; i++) {
        JsonObject userObj = allUsers[i];
        String username = userObj["username"].as<String>();

        if (!filter.isEmpty() && username.indexOf(filter) == -1) {
            continue;
        }

        if (i < startIndex) continue;
        if (count >= limit) break;

        JsonObject newUserObj = usersArray.add<JsonObject>();
        newUserObj["username"] = username;
        newUserObj["role"] = userObj["role"];
        newUserObj["enabled"] = userObj["enabled"];
        newUserObj["createTime"] = userObj["createTime"];
        newUserObj["lastLogin"] = userObj["lastLogin"];
        newUserObj["lastModified"] = userObj["lastModified"];

        bool isOnline = ctx->authManager ? ctx->authManager->isUserOnline(username) : false;
        newUserObj["isOnline"] = isOnline;
        newUserObj["isLocked"] = ctx->userManager->isAccountLocked(username);
        newUserObj["loginAttempts"] = ctx->userManager->getLoginAttempts(username);

        count++;
    }

    doc["page"] = page;
    doc["limit"] = limit;
    doc["total"] = totalCount;
    doc["count"] = count;
    doc["timestamp"] = millis();

    ctx->sendSuccess(request, doc);
}

void UserRouteHandler::handleAddUser(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "user.create")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String username = ctx->getParamValue(request, "username", "");
    String password = ctx->getParamValue(request, "password", "");
    String roleStr  = ctx->getParamValue(request, "role", "user");

    if (username.isEmpty()) {
        ctx->sendBadRequest(request, "Username is required");
        return;
    }
    if (password.isEmpty()) {
        ctx->sendBadRequest(request, "Password is required");
        return;
    }
    if (username.length() < 3 || username.length() > 32) {
        ctx->sendBadRequest(request, "Username must be 3-32 characters");
        return;
    }
    if (password.length() < 6) {
        ctx->sendBadRequest(request, "Password must be at least 6 characters");
        return;
    }

    UserRole role = UserManager::stringToRole(roleStr);

    if (!ctx->userManager) {
        ctx->sendError(request, 500, "User service unavailable");
        return;
    }

    if (ctx->userManager->addUser(username, password, UserManager::roleToString(role))) {
        ctx->sendSuccess(request, "User added successfully");
    } else {
        ctx->sendError(request, 400, "Failed to add user (username may already exist)");
    }
}

void UserRouteHandler::handleUpdateUser(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "user.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String username = ctx->getParamValue(request, "username", "");
    if (username.isEmpty()) {
        String path = request->url();
        int lastSlash = path.lastIndexOf('/');
        if (lastSlash != -1) {
            username = path.substring(lastSlash + 1);
        }
    }

    if (username.isEmpty()) {
        ctx->sendError(request, 400, "Username required");
        return;
    }

    String newPassword = ctx->getParamValue(request, "password", "");
    String newRole = ctx->getParamValue(request, "role", "");
    bool enabled = ctx->getParamBool(request, "enabled", true);

    if (!ctx->userManager) {
        ctx->sendError(request, 500, "User service unavailable");
        return;
    }

    User* user = ctx->userManager->getUser(username);
    if (!user) {
        ctx->sendError(request, 404, "User not found");
        return;
    }

    bool success = ctx->userManager->updateUser(username, newPassword, newRole, enabled);

    if (success) {
        ctx->sendSuccess(request, "User updated successfully");
    } else {
        ctx->sendError(request, 400, "Failed to update user");
    }
}

void UserRouteHandler::handleDeleteUser(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "user.delete")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String path = request->url();
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash == -1) {
        ctx->sendError(request, 400, "Invalid URL");
        return;
    }

    String username = path.substring(lastSlash + 1);

    AuthResult authResult = ctx->authenticateRequest(request);
    if (!authResult.success) {
        ctx->sendUnauthorized(request);
        return;
    }

    if (username == authResult.username) {
        ctx->sendError(request, 400, "Cannot delete your own account");
        return;
    }

    if (!ctx->userManager) {
        ctx->sendError(request, 500, "User service unavailable");
        return;
    }

    if (ctx->userManager->deleteUser(username)) {
        if (ctx->authManager) {
            ctx->authManager->forceLogout(username);
        }
        ctx->sendSuccess(request, "User deleted successfully");
    } else {
        ctx->sendError(request, 400, "Failed to delete user");
    }
}


void UserRouteHandler::handleGetOnlineUsers(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "user.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    if (!ctx->authManager) {
        ctx->sendError(request, 500, "Authentication service unavailable");
        return;
    }

    std::vector<UserSession> sessions = ctx->authManager->getActiveSessions();

    JsonDocument doc;
    JsonArray onlineUsersArray = doc["onlineUsers"].to<JsonArray>();

    for (const UserSession& session : sessions) {
        JsonObject userObj = onlineUsersArray.add<JsonObject>();
        userObj["username"] = session.username;
        userObj["role"] = UserManager::roleToString(session.role);
        userObj["loginTime"] = session.loginTime;
        userObj["lastAccessTime"] = session.lastAccessTime;
        userObj["ipAddress"] = session.ipAddress;
        userObj["userAgent"] = session.userAgent;
        userObj["sessionId"] = session.sessionId.substring(0, 8) + "...";
    }

    doc["count"] = sessions.size();
    doc["timestamp"] = millis();

    ctx->sendSuccess(request, doc);
}

void UserRouteHandler::handleResetPassword(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "user.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String username = ctx->getParamValue(request, "username", "");
    String newPassword = ctx->getParamValue(request, "newPassword", "");

    if (username.isEmpty() || newPassword.isEmpty()) {
        ctx->sendBadRequest(request, "Username and new password are required");
        return;
    }

    if (!ctx->userManager) {
        ctx->sendError(request, 500, "User service unavailable");
        return;
    }

    if (ctx->userManager->resetPassword(username, newPassword)) {
        if (ctx->authManager) {
            ctx->authManager->forceLogout(username);
        }
        ctx->sendSuccess(request, "Password reset successfully");
    } else {
        ctx->sendError(request, 400, "Failed to reset password");
    }
}
