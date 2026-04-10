#include "./network/handlers/AuthRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./security/AuthManager.h"
#include "./security/UserManager.h"
#include "systems/LoggerSystem.h"
#include <ArduinoJson.h>

AuthRouteHandler::AuthRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void AuthRouteHandler::setupRoutes(AsyncWebServer* server) {
    server->on("/api/auth/login", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleLogin(request);
    });

    server->on("/api/auth/logout", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleLogout(request);
    });

    server->on("/api/auth/session", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleVerifySession(request);
    });

    server->on("/api/auth/change-password", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleChangePassword(request);
    });
}

void AuthRouteHandler::handleLogin(AsyncWebServerRequest* request) {
    if (request->method() != HTTP_POST) {
        ctx->sendError(request, 405, "Method not allowed");
        return;
    }

    String username = ctx->getParamValue(request, "username", "");
    String password = ctx->getParamValue(request, "password", "");

    LOG_INFOF("[LOGIN] username='%s' password_len=%d params=%d",
              username.c_str(), password.length(), request->params());

    if (username.isEmpty() || password.isEmpty()) {
        ctx->sendBadRequest(request, "Username and password are required");
        return;
    }

    String ipAddress = ctx->getClientIP(request);
    String userAgent = ctx->getUserAgent(request);

    if (!ctx->authManager) {
        ctx->sendError(request, 500, "Authentication service unavailable");
        return;
    }

    AuthResult result = ctx->authManager->login(username, password, ipAddress, userAgent);

    if (result.success) {
        JsonDocument responseDoc;
        responseDoc["success"] = true;
        responseDoc["sessionId"] = result.sessionId;
        responseDoc["username"] = result.username;

        String jsonStr;
        serializeJson(responseDoc, jsonStr);
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", jsonStr);

        String cookieName = "sessionId";
        String cookieValue = cookieName + "=" + result.sessionId + "; Path=/; Max-Age=" +
                           String(3600);

        response->addHeader("Set-Cookie", cookieValue);
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);
    } else {
        ctx->sendError(request, 401, result.errorMessage);
    }
}

void AuthRouteHandler::handleLogout(AsyncWebServerRequest* request) {
    // 登出只需要有效会话，不需要特定权限
    AuthResult authResult = ctx->authenticateRequest(request);
    if (!authResult.success) {
        ctx->sendUnauthorized(request);
        return;
    }

    String sessionId = AuthManager::extractSessionIdFromRequest(request);

    if (sessionId.isEmpty()) {
        ctx->sendError(request, 400, "No session found");
        return;
    }

    if (ctx->authManager && ctx->authManager->logout(sessionId)) {
        ctx->sendSuccess(request, "Logged out successfully");
    } else {
        ctx->sendError(request, 400, "Logout failed");
    }
}

void AuthRouteHandler::handleVerifySession(AsyncWebServerRequest* request) {
    AuthResult authResult = ctx->authenticateRequest(request);

    if (authResult.success) {
        JsonDocument responseDoc;
        responseDoc["username"] = authResult.username;
        responseDoc["sessionValid"] = true;
        responseDoc["timestamp"] = millis();

        if (ctx->userManager) {
            responseDoc["role"] = ctx->userManager->getUserRole(authResult.username);
        } else {
            responseDoc["role"] = "VIEWER";
        }
        responseDoc["canManageFs"] = ctx->authManager->checkSessionPermission(
            authResult.sessionId, "fs.manage");

        ctx->sendSuccess(request, responseDoc);
    } else {
        ctx->sendUnauthorized(request);
    }
}

void AuthRouteHandler::handleChangePassword(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "user.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String oldPassword = ctx->getParamValue(request, "oldPassword", "");
    String newPassword = ctx->getParamValue(request, "newPassword", "");

    if (oldPassword.isEmpty() || newPassword.isEmpty()) {
        ctx->sendBadRequest(request, "Old password and new password are required");
        return;
    }

    AuthResult authResult = ctx->authenticateRequest(request);
    if (!authResult.success) {
        ctx->sendUnauthorized(request);
        return;
    }

    if (!ctx->userManager) {
        ctx->sendError(request, 500, "User service unavailable");
        return;
    }

    if (ctx->userManager->changePassword(authResult.username, oldPassword, newPassword)) {
        if (ctx->authManager) {
            ctx->authManager->forceLogout(authResult.username);
        }
        ctx->sendSuccess(request, "Password changed successfully");
    } else {
        ctx->sendError(request, 400, "Password change failed");
    }
}
