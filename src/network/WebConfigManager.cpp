#include "./network/WebConfigManager.h"
#include "./network/WebHandlerContext.h"
#include "systems/HealthMonitor.h"
#include "protocols/ProtocolManager.h"
#include <esp_heap_caps.h>
#include <cstdio>
#include "./network/handlers/StaticRouteHandler.h"
#include "./network/handlers/AuthRouteHandler.h"
#if FASTBEE_ENABLE_USER_ADMIN
#include "./network/handlers/UserRouteHandler.h"
#endif
#if FASTBEE_ENABLE_ROLE_ADMIN
#include "./network/handlers/RoleRouteHandler.h"
#endif
#if FASTBEE_ENABLE_LOG_VIEWER || FASTBEE_ENABLE_FILE_LOGGING
#include "./network/handlers/LogRouteHandler.h"
#endif
#include "./network/handlers/SystemRouteHandler.h"
#include "./network/handlers/DeviceRouteHandler.h"
#include "./network/handlers/BatchRouteHandler.h"
#include "./network/handlers/ProvisionRouteHandler.h"
#include "./network/handlers/OTARouteHandler.h"
#include "./network/handlers/PeripheralRouteHandler.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "./network/handlers/PeriphExecRouteHandler.h"
#include "core/PeriphExecManager.h"
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

    // 创建专职 Handler（条件编译裁剪未启用的模块，节省 Flash 和 RAM）
    staticHandler      = std::unique_ptr<StaticRouteHandler>(new StaticRouteHandler(ctx.get()));
    authHandler        = std::unique_ptr<AuthRouteHandler>(new AuthRouteHandler(ctx.get()));
#if FASTBEE_ENABLE_USER_ADMIN
    userHandler        = std::unique_ptr<UserRouteHandler>(new UserRouteHandler(ctx.get()));
#endif
#if FASTBEE_ENABLE_ROLE_ADMIN
    roleHandler        = std::unique_ptr<RoleRouteHandler>(new RoleRouteHandler(ctx.get()));
#endif
#if FASTBEE_ENABLE_LOG_VIEWER || FASTBEE_ENABLE_FILE_LOGGING
    logHandler         = std::unique_ptr<LogRouteHandler>(new LogRouteHandler(ctx.get()));
#endif
    systemHandler      = std::unique_ptr<SystemRouteHandler>(new SystemRouteHandler(ctx.get()));
    deviceHandler      = std::unique_ptr<DeviceRouteHandler>(new DeviceRouteHandler(ctx.get()));
    batchHandler       = std::unique_ptr<BatchRouteHandler>(new BatchRouteHandler(ctx.get()));
    provisionHandler   = std::unique_ptr<ProvisionRouteHandler>(new ProvisionRouteHandler(ctx.get()));
#if FASTBEE_ENABLE_OTA
    otaHandler         = std::unique_ptr<OTARouteHandler>(new OTARouteHandler(ctx.get()));
#endif
    peripheralHandler  = std::unique_ptr<PeripheralRouteHandler>(new PeripheralRouteHandler(ctx.get()));
#if FASTBEE_ENABLE_PERIPH_EXEC
    periphExecHandler  = std::unique_ptr<PeriphExecRouteHandler>(new PeriphExecRouteHandler(ctx.get()));
#endif
#if FASTBEE_ENABLE_RULE_SCRIPT
    ruleScriptHandler  = std::unique_ptr<RuleScriptRouteHandler>(new RuleScriptRouteHandler(ctx.get()));
#endif
#if FASTBEE_ENABLE_MQTT || FASTBEE_ENABLE_MODBUS || FASTBEE_ENABLE_TCP || FASTBEE_ENABLE_HTTP || FASTBEE_ENABLE_COAP
    protocolHandler    = std::unique_ptr<ProtocolRouteHandler>(new ProtocolRouteHandler(ctx.get()));
#endif
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

    LOG_INFO("[WebConfig] Initialized with route handlers");
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

void WebConfigManager::setNetworkManager(FBNetworkManager* netMgr) {
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
size_t WebConfigManager::getSseClientCount() const {
    return sseRouteHandler ? sseRouteHandler->clientCount() : 0;
}

size_t WebConfigManager::copyRecoveryEvents(WebRecoveryEvent* out, size_t maxEvents) const {
    if (!out || maxEvents == 0 || recoveryEventCount == 0) {
        return 0;
    }

    const size_t count = (recoveryEventCount < maxEvents) ? recoveryEventCount : maxEvents;
    const size_t start = (recoveryEventHead + MAX_RECOVERY_EVENTS - count) % MAX_RECOVERY_EVENTS;
    for (size_t i = 0; i < count; ++i) {
        out[i] = recoveryEvents[(start + i) % MAX_RECOVERY_EVENTS];
    }
    return count;
}

// ============ 维护 ============

void WebConfigManager::performMaintenance() {
    if (ctx && ctx->scheduleRestart && millis() >= ctx->scheduledRestartTime) {
        LOG_INFO("[WebConfig] Executing scheduled restart");
        delay(100);
        ESP.restart();
    }

    if (sseRouteHandler) {
        sseRouteHandler->performMaintenance();
    }

    if (ctx) {
        ctx->maintainWebForegroundTaskBudget();
    }

    const unsigned long maintenanceNow = millis();
    const bool inStartupGrace = (maintenanceNow < 120000UL);
    static unsigned long safeSoftRestartSince = 0;

    if (!isRunning) {
        safeSoftRestartSince = 0;
        severePressureSinceMs = 0;
        return;
    }

    if (inStartupGrace) {
        safeSoftRestartSince = 0;
        severePressureSinceMs = 0;
        return;
    }

    const uint32_t maintenanceFreeHeap = ESP.getFreeHeap();
    const uint32_t maintenanceLargestBlock =
        heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    const uint8_t maintenanceFrag = (maintenanceFreeHeap > 0)
        ? static_cast<uint8_t>(100U - (maintenanceLargestBlock * 100U / maintenanceFreeHeap))
        : 0;

    const bool severeWebPressure =
        (maintenanceFreeHeap < 16384U) ||
        (maintenanceLargestBlock < 6144U) ||
        (maintenanceFrag >= 65U && maintenanceLargestBlock < 8192U);

    if (severeWebPressure) {
        if (safeSoftRestartSince == 0) {
            safeSoftRestartSince = maintenanceNow;
            severePressureSinceMs = maintenanceNow;
            recordRecoveryEvent("pressure_start",
                                "severe_web_pressure",
                                maintenanceNow,
                                maintenanceFreeHeap,
                                maintenanceLargestBlock,
                                maintenanceFrag);
            LOG_WARNINGF("[WebConfig] Severe web memory pressure: heap=%lu largest=%lu frag=%u%%",
                         (unsigned long)maintenanceFreeHeap,
                         (unsigned long)maintenanceLargestBlock,
                         (unsigned int)maintenanceFrag);
        } else if (maintenanceNow - safeSoftRestartSince >= 120000UL) {
            LOG_WARNING("[WebConfig] Scheduling device restart after sustained web pressure");
            scheduleDeviceRestartForWebRecovery("sustained_web_pressure",
                                                maintenanceNow,
                                                maintenanceFreeHeap,
                                                maintenanceLargestBlock,
                                                maintenanceFrag);
            safeSoftRestartSince = 0;
            severePressureSinceMs = 0;
        }
    } else {
        if (severePressureSinceMs != 0) {
            recordRecoveryEvent("pressure_clear",
                                "recovered",
                                maintenanceNow,
                                maintenanceFreeHeap,
                                maintenanceLargestBlock,
                                maintenanceFrag);
        }
        safeSoftRestartSince = 0;
        severePressureSinceMs = 0;
    }

    return;
}

// ============ 路由注册（严格顺序）============

void WebConfigManager::recordRecoveryEvent(const char* type,
                                           const char* reason,
                                           unsigned long atMs,
                                           uint32_t freeHeap,
                                           uint32_t largestBlock,
                                           uint8_t fragmentation) {
    WebRecoveryEvent& event = recoveryEvents[recoveryEventHead];
    event.atMs = atMs;
    event.freeHeap = freeHeap;
    event.largestBlock = largestBlock;
    event.fragmentation = fragmentation;
    const size_t sseClients = getSseClientCount();
    event.sseClients = static_cast<uint8_t>(sseClients > 255U ? 255U : sseClients);
    copyText(event.type, sizeof(event.type), type);
    copyText(event.reason, sizeof(event.reason), reason);

    recoveryEventHead = (recoveryEventHead + 1) % MAX_RECOVERY_EVENTS;
    if (recoveryEventCount < MAX_RECOVERY_EVENTS) {
        recoveryEventCount++;
    }
}

void WebConfigManager::scheduleDeviceRestartForWebRecovery(const char* reason,
                                                           unsigned long atMs,
                                                           uint32_t freeHeap,
                                                           uint32_t largestBlock,
                                                           uint8_t fragmentation,
                                                           unsigned long delayMs) {
    lastSoftRestartAtMs = atMs;
    lastSoftRestartReason = reason ? reason : "web_recovery";
    lastSoftRestartFreeHeap = freeHeap;
    lastSoftRestartLargestBlock = largestBlock;
    lastSoftRestartFrag = fragmentation;
    softRestartCount++;

    recordRecoveryEvent("restart_scheduled",
                        lastSoftRestartReason.c_str(),
                        atMs,
                        freeHeap,
                        largestBlock,
                        fragmentation);

    if (!ctx) {
        LOG_ERRORF("[WebConfig] Cannot schedule web recovery restart: %s",
                   lastSoftRestartReason.c_str());
        return;
    }

    if (!ctx->scheduleRestart) {
        ctx->scheduleRestart = true;
        ctx->scheduledRestartTime = atMs + delayMs;
    }

    LOG_WARNINGF("[WebConfig] Device restart scheduled for web recovery: reason=%s delay=%lums heap=%lu largest=%lu frag=%u%%",
                 lastSoftRestartReason.c_str(),
                 (unsigned long)delayMs,
                 (unsigned long)freeHeap,
                 (unsigned long)largestBlock,
                 (unsigned int)fragmentation);
}

void WebConfigManager::copyText(char* dest, size_t destSize, const char* text) const {
    if (!dest || destSize == 0) {
        return;
    }
    snprintf(dest, destSize, "%s", text ? text : "");
}

void WebConfigManager::setupAllRoutes() {
    // 注册顺序严格遵循 ESPAsyncWebServer 前缀匹配规则：
    // 子路径必须在父路径之前注册

    // [RouteProbe] 逐 handler heap 采样，默认关闭。打开方式：在编译参数加 -DFASTBEE_ROUTE_PROBE=1
    // 用于定位路由注册的堆消耗大头（OOM 排查辅助）
#if FASTBEE_ROUTE_PROBE
    auto probe = [](const char* tag) {
        uint32_t fh = ESP.getFreeHeap();
        uint32_t mb = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        LOGGER.infof("[RouteProbe] %-12s heap=%u maxBlock=%u", tag, (unsigned)fh, (unsigned)mb);
    };
    #define ROUTE_PROBE(tag) probe(tag)
#else
    #define ROUTE_PROBE(tag) ((void)0)
#endif
    ROUTE_PROBE("Begin");

    // CORS 预检（CORS/安全头已由 DefaultHeaders 全局注入，此处仅补充 Max-Age）
    server->on("/api/*", HTTP_OPTIONS, [this](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(204);
        response->addHeader("Access-Control-Max-Age", "86400");
        response->addHeader("Connection", "close");
        request->send(response);
    });
    ROUTE_PROBE("CORS");

    // 1. 认证路由（/api/auth/*）
    authHandler->setupRoutes(server);                        ROUTE_PROBE("Auth");

    // 2. 用户路由（/api/users/*）
#if FASTBEE_ENABLE_USER_ADMIN
    userHandler->setupRoutes(server);                        ROUTE_PROBE("User");
#endif

    // 3. 角色路由（/api/roles/*, /api/permissions, /api/audit/*）
#if FASTBEE_ENABLE_ROLE_ADMIN
    roleHandler->setupRoutes(server);                        ROUTE_PROBE("Role");
#endif

    // 4. 系统路由（/api/system/*, /api/network/*, /api/files/*, /api/config, /api/health）
    systemHandler->setupRoutes(server);                      ROUTE_PROBE("System");

#if FASTBEE_ENABLE_LOG_VIEWER || FASTBEE_ENABLE_FILE_LOGGING
    logHandler->setupRoutes(server);                         ROUTE_PROBE("Log");
#endif

    // 4b. 设备路由（/api/device/*）
    deviceHandler->setupRoutes(server);                      ROUTE_PROBE("Device");

    // 4c. 批量请求路由（/api/batch）
    batchHandler->setupRoutes(server);                       ROUTE_PROBE("Batch");

    // 5. 配网路由（/setup, /api/wifi/*）
    provisionHandler->setupRoutes(server);                   ROUTE_PROBE("Provision");

    // 6. OTA路由（/api/ota/*）
#if FASTBEE_ENABLE_OTA
    otaHandler->setupRoutes(server);                         ROUTE_PROBE("OTA");
#endif

    // 7. 外设路由（/api/peripherals/*）
    peripheralHandler->setupRoutes(server);                  ROUTE_PROBE("Peripheral");

    // 8. 外设执行路由（/api/periph-exec/*）
#if FASTBEE_ENABLE_PERIPH_EXEC
    periphExecHandler->setupRoutes(server);                  ROUTE_PROBE("PeriphExec");
#endif

    // 9. 规则脚本路由（/api/rule-script/*）
#if FASTBEE_ENABLE_RULE_SCRIPT
    ruleScriptHandler->setupRoutes(server);                  ROUTE_PROBE("RuleScript");
#endif

    // 10. 协议路由（/api/protocol/*）
#if FASTBEE_ENABLE_MQTT || FASTBEE_ENABLE_MODBUS || FASTBEE_ENABLE_TCP || FASTBEE_ENABLE_HTTP || FASTBEE_ENABLE_COAP
    protocolHandler->setupRoutes(server);                    ROUTE_PROBE("Protocol");
#endif

    // 11. SSE 事件推送路由（/api/events）
    sseRouteHandler->setupRoutes(server);                    ROUTE_PROBE("SSE");
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

    // 注册传感器数据 SSE 回调（PeriphExec 传感器读取后推送）
#if FASTBEE_ENABLE_PERIPH_EXEC
    {
        SSERouteHandler* ssePtr = sseRouteHandler.get();
        PeriphExecManager::getInstance().setSensorSSECallback(
            [ssePtr](const String& data) {
                if (ssePtr && ssePtr->clientCount() > 0) {
                    ssePtr->broadcastSensorData(data);
                }
            }
        );
    }
#endif

    // 12. 静态文件与页面路由（/, /login, /dashboard, /users, 404 fallback）
    // 必须最后注册，因为 onNotFound 是全局 fallback
    staticHandler->setupRoutes(server);                      ROUTE_PROBE("Static");
    ROUTE_PROBE("End");
#undef ROUTE_PROBE
}
