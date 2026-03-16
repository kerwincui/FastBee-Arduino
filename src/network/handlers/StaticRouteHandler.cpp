#include "./network/handlers/StaticRouteHandler.h"
#include "./network/WebHandlerContext.h"
#include <LittleFS.h>

StaticRouteHandler::StaticRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void StaticRouteHandler::setupRoutes(AsyncWebServer* server) {
    server->on("/login", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleLoginPage(request); });

    server->on("/dashboard", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleDashboardPage(request); });

    server->on("/users", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleUsersPage(request); });

    // SPA 路由 - 都返回 index.html 让前端路由处理
    server->on("/device", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleSPAPage(request); });

    server->on("/network", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleSPAPage(request); });

    server->on("/data", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleSPAPage(request); });

    server->on("/logs", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleSPAPage(request); });

    server->on("/roles", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleSPAPage(request); });

    server->on("/protocol", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleSPAPage(request); });

    server->on("/peripheral", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleSPAPage(request); });

    server->on("/gpio", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleSPAPage(request); });

    server->on("/", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleRootPage(request); });

    // 404 fallback
    server->onNotFound(
        [this](AsyncWebServerRequest* request) { handleNotFound(request); });
}

void StaticRouteHandler::handleLoginPage(AsyncWebServerRequest* request) {
    // 尝试文件系统
    String path = ctx->webRootPath + "/login.html";
    if (ctx->serveStaticFile(request, path)) return;

    // 内置页面
    ctx->sendBuiltinLoginPage(request);
}

void StaticRouteHandler::handleDashboardPage(AsyncWebServerRequest* request) {
    String path = ctx->webRootPath + "/dashboard.html";
    if (ctx->serveStaticFile(request, path)) return;

    ctx->sendBuiltinDashboard(request);
}

void StaticRouteHandler::handleUsersPage(AsyncWebServerRequest* request) {
    String path = ctx->webRootPath + "/users.html";
    if (ctx->serveStaticFile(request, path)) return;

    ctx->sendBuiltinUsersPage(request);
}

void StaticRouteHandler::handleRootPage(AsyncWebServerRequest* request) {
    // 尝试 index.html
    String path = ctx->webRootPath + "/index.html";
    if (ctx->serveStaticFile(request, path)) return;

    // 回退到登录页
    request->redirect("/login");
}

void StaticRouteHandler::handleSPAPage(AsyncWebServerRequest* request) {
    // SPA 页面 - 返回 index.html 让前端路由处理
    String path = ctx->webRootPath + "/index.html";
    if (ctx->serveStaticFile(request, path)) return;

    // 如果 index.html 不存在，返回 404
    ctx->sendNotFound(request);
}

void StaticRouteHandler::handleNotFound(AsyncWebServerRequest* request) {
    // OPTIONS 预检请求
    if (request->method() == HTTP_OPTIONS) {
        AsyncWebServerResponse* response = request->beginResponse(204);
        response->addHeader("Access-Control-Allow-Origin", "*");
        response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
        response->addHeader("Access-Control-Max-Age", "86400");
        request->send(response);
        return;
    }

    // 尝试从文件系统服务静态资源
    String path = ctx->webRootPath + request->url();
    if (ctx->serveStaticFile(request, path)) return;

    ctx->sendNotFound(request);
}
