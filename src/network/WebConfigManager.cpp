#include "./network/WebConfigManager.h"
#include "./network/WebHandlerContext.h"
#include "systems/HealthMonitor.h"
#include "protocols/ProtocolManager.h"
#include <esp_heap_caps.h>
#include "./network/handlers/StaticRouteHandler.h"
#include "./network/handlers/AuthRouteHandler.h"
#include "./network/handlers/UserRouteHandler.h"
#include "./network/handlers/RoleRouteHandler.h"
#include "./network/handlers/SystemRouteHandler.h"
#include "./network/handlers/LogRouteHandler.h"
#include "./network/handlers/DeviceRouteHandler.h"
#include "./network/handlers/BatchRouteHandler.h"
#include "./network/handlers/ProvisionRouteHandler.h"
#include "./network/handlers/OTARouteHandler.h"
#include "./network/handlers/PeripheralRouteHandler.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "./network/handlers/PeriphExecRouteHandler.h"
#endif
#if FASTBEE_ENABLE_RULE_SCRIPT
#include "./network/handlers/RuleScriptRouteHandler.h"
#endif
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

    // 创建 15 个专职 Handler
    staticHandler      = std::unique_ptr<StaticRouteHandler>(new StaticRouteHandler(ctx.get()));
    authHandler        = std::unique_ptr<AuthRouteHandler>(new AuthRouteHandler(ctx.get()));
    userHandler        = std::unique_ptr<UserRouteHandler>(new UserRouteHandler(ctx.get()));
    roleHandler        = std::unique_ptr<RoleRouteHandler>(new RoleRouteHandler(ctx.get()));
    systemHandler      = std::unique_ptr<SystemRouteHandler>(new SystemRouteHandler(ctx.get()));
    logHandler         = std::unique_ptr<LogRouteHandler>(new LogRouteHandler(ctx.get()));
    deviceHandler      = std::unique_ptr<DeviceRouteHandler>(new DeviceRouteHandler(ctx.get()));
    batchHandler       = std::unique_ptr<BatchRouteHandler>(new BatchRouteHandler(ctx.get()));
    provisionHandler   = std::unique_ptr<ProvisionRouteHandler>(new ProvisionRouteHandler(ctx.get()));
    otaHandler         = std::unique_ptr<OTARouteHandler>(new OTARouteHandler(ctx.get()));
    peripheralHandler  = std::unique_ptr<PeripheralRouteHandler>(new PeripheralRouteHandler(ctx.get()));
#if FASTBEE_ENABLE_PERIPH_EXEC
    periphExecHandler  = std::unique_ptr<PeriphExecRouteHandler>(new PeriphExecRouteHandler(ctx.get()));
#endif
#if FASTBEE_ENABLE_RULE_SCRIPT
    ruleScriptHandler  = std::unique_ptr<RuleScriptRouteHandler>(new RuleScriptRouteHandler(ctx.get()));
#endif
    protocolHandler    = std::unique_ptr<ProtocolRouteHandler>(new ProtocolRouteHandler(ctx.get()));
    sseRouteHandler    = std::unique_ptr<SSERouteHandler>(new SSERouteHandler(ctx.get()));

    // 全局 CORS 头 —— 自动注入到所有 HTTP 响应（含静态文件）
    // 解决 fastbee.local 与 IP 地址混用时的跨域问题
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");

    // 全局安全头 —— 防止 MIME 嗅探和点击劫持
    DefaultHeaders::Instance().addHeader("X-Content-Type-Options", "nosniff");
    DefaultHeaders::Instance().addHeader("X-Frame-Options", "SAMEORIGIN");

    setupAllRoutes();

    LOG_INFO("[WebConfig] Initialized with 15 route handlers");
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
            // 注册 MQTT 状态 SSE 回调
            protoMgr->setMQTTStatusSSECallback(
                [ssePtr](const String& data) {
                    if (ssePtr && ssePtr->clientCount() > 0) {
                        ssePtr->broadcastMqttStatus(data);
                    }
                }
            );
            // 注册 Modbus 状态 SSE 回调
            protoMgr->setModbusStatusSSECallback(
                [ssePtr](const String& data) {
                    if (ssePtr && ssePtr->clientCount() > 0) {
                        ssePtr->broadcastModbusStatus(data);
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

    unsigned long now = millis();
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    uint8_t frag = (freeHeap > 0) ? static_cast<uint8_t>(100U - (largestBlock * 100U / freeHeap)) : 0;

    // ── 策略1: freeHeap < 15KB 持续 60s → 硬复位 ──
    static unsigned long criticalSince = 0;
    if (freeHeap < MEM_THRESHOLD_SEVERE) {
        if (criticalSince == 0) {
            criticalSince = now;
            LOG_ERRORF("[WebConfig] Memory CRITICAL: freeHeap=%lu largestBlock=%lu, restart in 60s if not recovered",
                       (unsigned long)freeHeap, (unsigned long)largestBlock);
        } else if (now - criticalSince >= 60000UL) {
            LOG_ERROR("[WebConfig] Memory CRITICAL for 60s, force restarting...");
            delay(100);
            ESP.restart();
        }
    } else {
        if (criticalSince != 0) {
            LOG_INFOF("[WebConfig] Memory recovered: freeHeap=%lu largestBlock=%lu",
                      (unsigned long)freeHeap, (unsigned long)largestBlock);
        }
        criticalSince = 0;
    }

    // ── 策略2: largestBlock < 6KB 持续 30s → 硬复位 ──
    // 即使 freeHeap 看起来还行，如果最大可分配块太小，Web 服务已不可用
    static unsigned long smallBlockSince = 0;
    if (largestBlock < 6144) {
        if (smallBlockSince == 0) {
            smallBlockSince = now;
            LOG_ERRORF("[WebConfig] largestBlock CRITICAL: %lu bytes, restart in 30s if not recovered",
                       (unsigned long)largestBlock);
        } else if (now - smallBlockSince >= 30000UL) {
            LOG_ERROR("[WebConfig] largestBlock CRITICAL for 30s, force restarting...");
            delay(100);
            ESP.restart();
        }
    } else {
        smallBlockSince = 0;
    }

    // ── 策略3: 碎片率 >80% 且 largestBlock < 8KB 持续 60s → 硬复位 ──
    static unsigned long fragSince = 0;
    if (frag > 80 && largestBlock < 8192) {
        if (fragSince == 0) {
            fragSince = now;
            LOG_ERRORF("[WebConfig] High fragmentation: frag=%d%% largestBlock=%lu, restart in 60s if not recovered",
                       (int)frag, (unsigned long)largestBlock);
        } else if (now - fragSince >= 60000UL) {
            LOG_ERROR("[WebConfig] High fragmentation for 60s, force restarting...");
            delay(100);
            ESP.restart();
        }
    } else {
        fragSince = 0;
    }

    // ── 策略4: 轻度碎片化时尝试 Web 服务软重启 ──
    // 比硬复位更轻量，可能释放 async_tcp 积压的缓冲区
    static unsigned long softRestartSince = 0;
    if (frag > 60 && largestBlock < 16384 && isRunning) {
        if (softRestartSince == 0) {
            softRestartSince = now;
        } else if (now - softRestartSince >= 120000UL) {
            LOG_WARNING("[WebConfig] Triggering soft web server restart due to fragmentation");
            server->end();
            delay(50);
            server->begin();
            softRestartSince = 0;
        }
    } else {
        softRestartSince = 0;
    }

    // SSE 维护：心跳、僵尸连接清理、客户端数上报
    if (sseRouteHandler) {
        sseRouteHandler->performMaintenance();
    }
}

// ============ 路由注册（严格顺序）============

void WebConfigManager::setupAllRoutes() {
    // 注册顺序严格遵循 ESPAsyncWebServer 前缀匹配规则：
    // 子路径必须在父路径之前注册

    // CORS 预检（CORS/安全头已由 DefaultHeaders 全局注入，此处仅补充 Max-Age）
    server->on("/api/*", HTTP_OPTIONS, [this](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(204);
        response->addHeader("Access-Control-Max-Age", "86400");
        request->send(response);
    });

    // 1. 认证路由（/api/auth/*）
    authHandler->setupRoutes(server);

    // 2. 用户路由（/api/users/*）
    userHandler->setupRoutes(server);

    // 3. 角色路由（/api/roles/*, /api/permissions, /api/audit/*）
    roleHandler->setupRoutes(server);

    // 4. 系统路由（/api/system/*, /api/network/*, /api/files/*, /api/config, /api/health）
    systemHandler->setupRoutes(server);

    // 4a. 日志路由（/api/logs/*, /api/system/logs/*）
    logHandler->setupRoutes(server);

    // 4b. 设备路由（/api/device/*）
    deviceHandler->setupRoutes(server);

    // 4c. 批量请求路由（/api/batch）
    batchHandler->setupRoutes(server);

    // 5. 配网路由（/setup, /api/wifi/*）
    provisionHandler->setupRoutes(server);

    // 6. OTA路由（/api/ota/*）
    otaHandler->setupRoutes(server);

    // 7. 外设路由（/api/peripherals/*）
    peripheralHandler->setupRoutes(server);

    // 8. 外设执行路由（/api/periph-exec/*）
#if FASTBEE_ENABLE_PERIPH_EXEC
    periphExecHandler->setupRoutes(server);
#endif

    // 9. 规则脚本路由（/api/rule-script/*）
#if FASTBEE_ENABLE_RULE_SCRIPT
    ruleScriptHandler->setupRoutes(server);
#endif

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
        // 注册 MQTT 状态 SSE 回调
        ctx->protocolManager->setMQTTStatusSSECallback(
            [ssePtr](const String& data) {
                if (ssePtr && ssePtr->clientCount() > 0) {
                    ssePtr->broadcastMqttStatus(data);
                }
            }
        );
        // 注册 Modbus 状态 SSE 回调
        ctx->protocolManager->setModbusStatusSSECallback(
            [ssePtr](const String& data) {
                if (ssePtr && ssePtr->clientCount() > 0) {
                    ssePtr->broadcastModbusStatus(data);
                }
            }
        );
    }

    // 12. 静态文件与页面路由（/, /login, /dashboard, /users, 404 fallback）
    // 必须最后注册，因为 onNotFound 是全局 fallback
    staticHandler->setupRoutes(server);
}
