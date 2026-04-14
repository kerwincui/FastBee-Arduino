#include "./network/WebConfigManager.h"
#include "./network/WebHandlerContext.h"
#include "protocols/ProtocolManager.h"
#include "./network/handlers/StaticRouteHandler.h"
#include "./network/handlers/AuthRouteHandler.h"
#include "./network/handlers/UserRouteHandler.h"
#include "./network/handlers/RoleRouteHandler.h"
#include "./network/handlers/SystemRouteHandler.h"
#include "./network/handlers/ProvisionRouteHandler.h"
#include "./network/handlers/OTARouteHandler.h"
#include "./network/handlers/PeripheralRouteHandler.h"
#include "./network/handlers/PeriphExecRouteHandler.h"
#include "./network/handlers/RuleScriptRouteHandler.h"
#include "./network/handlers/ProtocolRouteHandler.h"
#include "./network/handlers/SSERouteHandler.h"
#include "systems/LoggerSystem.h"

// ============ 构造 / 析构 ============

WebConfigManager::WebConfigManager(AsyncWebServer* webServer,
                                   IAuthManager* authMgr, IUserManager* userMgr)
    : server(webServer), isRunning(false) {
    ctx = std::unique_ptr<WebHandlerContext>(new WebHandlerContext(webServer, authMgr, userMgr));
}

WebConfigManager::~WebConfigManager() {
    stop();
}

// ============ 生命周期 ============

bool WebConfigManager::initialize() {
    if (!server || !ctx) {
        LOG_ERROR("[WebConfig] Server or context is null");
        return false;
    }

    // 创建 12 个专职 Handler
    staticHandler      = std::unique_ptr<StaticRouteHandler>(new StaticRouteHandler(ctx.get()));
    authHandler        = std::unique_ptr<AuthRouteHandler>(new AuthRouteHandler(ctx.get()));
    userHandler        = std::unique_ptr<UserRouteHandler>(new UserRouteHandler(ctx.get()));
    roleHandler        = std::unique_ptr<RoleRouteHandler>(new RoleRouteHandler(ctx.get()));
    systemHandler      = std::unique_ptr<SystemRouteHandler>(new SystemRouteHandler(ctx.get()));
    provisionHandler   = std::unique_ptr<ProvisionRouteHandler>(new ProvisionRouteHandler(ctx.get()));
    otaHandler         = std::unique_ptr<OTARouteHandler>(new OTARouteHandler(ctx.get()));
    peripheralHandler  = std::unique_ptr<PeripheralRouteHandler>(new PeripheralRouteHandler(ctx.get()));
    periphExecHandler  = std::unique_ptr<PeriphExecRouteHandler>(new PeriphExecRouteHandler(ctx.get()));
    ruleScriptHandler  = std::unique_ptr<RuleScriptRouteHandler>(new RuleScriptRouteHandler(ctx.get()));
    protocolHandler    = std::unique_ptr<ProtocolRouteHandler>(new ProtocolRouteHandler(ctx.get()));
    sseRouteHandler    = std::unique_ptr<SSERouteHandler>(new SSERouteHandler(ctx.get()));

    // 全局 CORS 头 —— 自动注入到所有 HTTP 响应（含静态文件）
    // 解决 fastbee.local 与 IP 地址混用时的跨域问题
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");

    setupAllRoutes();

    LOG_INFO("[WebConfig] Initialized with 12 route handlers");
    return true;
}

bool WebConfigManager::start() {
    if (isRunning) return true;
    if (!server) return false;

    server->begin();
    isRunning = true;
    LOG_INFO("[WebConfig] Web server started");
    return true;
}

void WebConfigManager::stop() {
    if (!isRunning) return;
    if (server) server->end();
    isRunning = false;
    LOG_INFO("[WebConfig] Web server stopped");
}

bool WebConfigManager::isServerRunning() const { return isRunning; }

// ============ 管理器注入 ============

void WebConfigManager::setRoleManager(RoleManager* roleMgr) {
    if (ctx) ctx->roleManager = roleMgr;
}

void WebConfigManager::setNetworkManager(NetworkManager* netMgr) {
    if (ctx) ctx->networkManager = netMgr;
}

void WebConfigManager::setOTAManager(OTAManager* otaMgr) {
    if (ctx) ctx->otaManager = otaMgr;
}

void WebConfigManager::setProtocolManager(ProtocolManager* protoMgr) {
    if (ctx) {
        ctx->protocolManager = protoMgr;
        if (protoMgr && sseRouteHandler) {
            SSERouteHandler* ssePtr = sseRouteHandler.get();
            protoMgr->setSSECallback(
                [ssePtr](uint8_t address, const String& data) {
                    if (ssePtr && ssePtr->clientCount() > 0) {
                        ssePtr->broadcastModbusData(data);
                    }
                }
            );
        }
    }
}

AsyncWebServer* WebConfigManager::getWebServer() const { return server; }

// ============ 维护 ============

void WebConfigManager::performMaintenance() {
    if (ctx && ctx->scheduleRestart && millis() >= ctx->scheduledRestartTime) {
        LOG_INFO("[WebConfig] Executing scheduled restart");
        delay(100);
        ESP.restart();
    }
}

// ============ 路由注册（严格顺序）============

void WebConfigManager::setupAllRoutes() {
    // 注册顺序严格遵循 ESPAsyncWebServer 前缀匹配规则：
    // 子路径必须在父路径之前注册

    // CORS 预检
    server->on("/api/*", HTTP_OPTIONS, [this](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(204);
        response->addHeader("Access-Control-Allow-Origin", "*");
        response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
        response->addHeader("Access-Control-Max-Age", "86400");
        request->send(response);
    });

    // 1. 认证路由（/api/auth/*）
    authHandler->setupRoutes(server);

    // 2. 用户路由（/api/users/*）
    userHandler->setupRoutes(server);

    // 3. 角色路由（/api/roles/*, /api/permissions, /api/audit/*）
    roleHandler->setupRoutes(server);

    // 4. 系统路由（/api/system/*, /api/network/*, /api/logs/*, /api/files/*, /api/config, /api/device/*, /api/health）
    systemHandler->setupRoutes(server);

    // 5. 配网路由（/setup, /api/wifi/*）
    provisionHandler->setupRoutes(server);

    // 6. OTA路由（/api/ota/*）
    otaHandler->setupRoutes(server);

    // 7. 外设路由（/api/peripherals/*）
    peripheralHandler->setupRoutes(server);

    // 8. 外设执行路由（/api/periph-exec/*）
    periphExecHandler->setupRoutes(server);

    // 9. 规则脚本路由（/api/rule-script/*）
    ruleScriptHandler->setupRoutes(server);

    // 10. 协议路由（/api/protocol/*）
    protocolHandler->setupRoutes(server);

    // 11. SSE 事件推送路由（/api/events）
    sseRouteHandler->setupRoutes(server);
    ctx->sseHandler = sseRouteHandler.get();
    if (ctx->protocolManager) {
        SSERouteHandler* ssePtr = sseRouteHandler.get();
        ctx->protocolManager->setSSECallback(
            [ssePtr](uint8_t address, const String& data) {
                if (ssePtr && ssePtr->clientCount() > 0) {
                    ssePtr->broadcastModbusData(data);
                }
            }
        );
    }

    // 12. 静态文件与页面路由（/, /login, /dashboard, /users, 404 fallback）
    // 必须最后注册，因为 onNotFound 是全局 fallback
    staticHandler->setupRoutes(server);
}
