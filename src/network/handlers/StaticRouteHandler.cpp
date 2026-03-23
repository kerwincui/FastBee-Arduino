#include "./network/handlers/StaticRouteHandler.h"
#include "./network/WebHandlerContext.h"
#include <LittleFS.h>

StaticRouteHandler::StaticRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void StaticRouteHandler::setupRoutes(AsyncWebServer* server) {
    // SPA 路由 - 都返回 index.html 让前端路由处理
    const char* spaRoutes[] = {
        "/login", "/dashboard", "/users",
        "/device", "/network", "/data", "/logs",
        "/roles", "/protocol", "/peripheral", "/gpio"
    };
    for (const char* route : spaRoutes) {
        server->on(route, HTTP_GET,
                  [this](AsyncWebServerRequest* request) { handleSPAPage(request); });
    }

    server->on("/", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleRootPage(request); });

    // 404 fallback
    server->onNotFound(
        [this](AsyncWebServerRequest* request) { handleNotFound(request); });
}

void StaticRouteHandler::handleRootPage(AsyncWebServerRequest* request) {
    // 尝试 index.html
    String path = ctx->webRootPath + "/index.html";
    if (ctx->serveStaticFile(request, path)) return;

    // 回退到内置 WiFi 配置页（index.html 不存在时说明文件系统未上传）
    ctx->sendBuiltinSetupPage(request);
}

void StaticRouteHandler::handleSPAPage(AsyncWebServerRequest* request) {
    // SPA 页面 - 返回 index.html 让前端路由处理
    String path = ctx->webRootPath + "/index.html";
    if (ctx->serveStaticFile(request, path)) return;

    // 如果 index.html 不存在，重定向到根目录（会显示内置配置页）
    request->redirect("/");
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
