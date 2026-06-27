#include "./network/WebConfigManager.h"
#include "./network/WebHandlerContext.h"
#include "core/MemoryBudget.h"
#include "systems/HealthMonitor.h"
#include "systems/SystemRebooter.h"
#include "protocols/ProtocolManager.h"
#include <esp_heap_caps.h>
#include <cstdio>
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcpip.h"  // LOCK_TCPIP_CORE / UNLOCK_TCPIP_CORE
#include "./network/handlers/StaticRouteHandler.h"
#include "./network/handlers/AuthRouteHandler.h"
#if FASTBEE_ENABLE_USER_ADMIN
#include "./network/handlers/UserRouteHandler.h"
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
    // 设置反向指针，使 WebHandlerContext 能通知请求突增跟踪
    if (ctx) ctx->webConfigManager = this;
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

bool WebConfigManager::isPortListening(uint16_t port) const {
    bool found = false;
    LOCK_TCPIP_CORE();
    // tcp_listen_pcbs 是 union，需用 .listen_pcbs 访问 tcp_pcb_listen* 指针
    for (struct tcp_pcb_listen* pcb = tcp_listen_pcbs.listen_pcbs; pcb != nullptr; pcb = pcb->next) {
        if (pcb->local_port == port) {
            found = true;
            break;
        }
    }
    UNLOCK_TCPIP_CORE();
    return found;
}

bool WebConfigManager::start() {
    if (isRunning) return true;
    if (!server) return false;

    // 给 AsyncTCP 额外时间释放旧 socket（如果之前 end() 后立即调用 begin()）
    delay(50);

    server->begin();
    // 等待 AsyncTCP 任务处理 begin() 请求
    delay(100);

    // 验证服务器是否真的在监听端口 80
    // AsyncWebServer 没有公开的 isListening() 方法，
    // 但我们可以直接检查 lwIP 的 tcp_listen_pcbs 链表
    if (!isPortListening(80)) {
        LOG_ERROR("[WebConfig] Web server begin() succeeded but port 80 NOT listening (bind error?)");
        isRunning = false;
        return false;
    }

    isRunning = true;
    webPauseReason = WebServicePauseReason::None;
    webPauseUntilMs = 0;
    lastRequestSeenMs = millis();
    LOG_INFO("[WebConfig] Web server started");
    return true;
}

void WebConfigManager::stop() {
    if (!isRunning) return;
    if (server) server->end();
    isRunning = false;
    webPauseReason = WebServicePauseReason::None;
    webPauseUntilMs = 0;
    LOG_INFO("[WebConfig] Web server stopped");
}

bool WebConfigManager::isServerRunning() const { return isRunning; }

bool WebConfigManager::pauseForMqttsHandshake(unsigned long holdMs) {
    if (!server) return false;
    if (isForegroundRequestActive()) {
        LOG_INFO("[WebConfig] Keeping Web online for active foreground request; MQTTS deep pause skipped");
        return false;
    }

    const unsigned long now = millis();
    webPauseReason = WebServicePauseReason::MqttsHandshake;
    webPauseUntilMs = (holdMs == 0) ? 0 : (now + holdMs);

    size_t closedSse = 0;
    if (sseRouteHandler && sseRouteHandler->clientCount() > 0) {
        closedSse = sseRouteHandler->closeAllClients();
        if (closedSse > 0) {
            delay(50);
        }
    }

    if (isRunning) {
        server->end();
        isRunning = false;
        delay(50);
    }

    lastWebRecoveryMs = now;
    recordRecoveryEvent("manual_pause",
                        "mqtts_handshake",
                        now,
                        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                        0);
    LOG_WARNINGF("[WebConfig] Web server paused for MQTTS handshake (sseClosed=%u)",
                 (unsigned)closedSse);
    return true;
}

bool WebConfigManager::resumeFromMqttsHandshake() {
    if (webPauseReason != WebServicePauseReason::MqttsHandshake) {
        return isRunning;
    }

    webPauseReason = WebServicePauseReason::None;
    webPauseUntilMs = 0;

    // 确保 AsyncTCP 完全释放监听 socket（避免 bind error: -8）
    // server->end() 后需要等待底层 TCP 状态机清理完成
    const unsigned long delayMs = FastBee::MemoryBudget::MQTTS_WEB_RESUME_DELAY_MS;
    LOG_INFOF("[WebConfig] Waiting %lums for TCP port release after MQTTS pause",
              (unsigned long)delayMs);
    delay(delayMs);

    // 尝试启动 Web 服务器，带重试机制
    const int maxRetries = 3;
    for (int attempt = 1; attempt <= maxRetries; attempt++) {
        const bool started = start();
        lastWebRecoveryMs = millis();

        if (started) {
            recordRecoveryEvent("manual_resume",
                                "mqtts_handshake",
                                lastWebRecoveryMs,
                                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                                0);
            LOG_INFO("[WebConfig] Web server resumed successfully after MQTTS pause");
            return true;
        }

        // 启动失败：可能端口仍未释放，等待后重试
        if (attempt < maxRetries) {
            const unsigned long retryDelay = 500 * attempt; // 500ms, 1000ms, 1500ms
            LOG_WARNINGF("[WebConfig] Web resume attempt %d failed, retrying in %lums",
                         attempt, (unsigned long)retryDelay);
            delay(retryDelay);
        } else {
            LOG_ERROR("[WebConfig] Web server resume failed after MQTTS pause (all retries exhausted)");
            recordRecoveryEvent("resume_failed",
                                "mqtts_handshake",
                                lastWebRecoveryMs,
                                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                                0);
        }
    }

    return false;
}

bool WebConfigManager::isWebRecoverySuppressed(unsigned long now) const {
    if (webPauseReason == WebServicePauseReason::None) {
        return false;
    }
    if (webPauseUntilMs == 0) {
        return true;
    }
    return (int32_t)(webPauseUntilMs - now) > 0;
}

// ============ 管理器注入 ============



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
    // 统一重启机制：由 SystemRebooter::update() 执行待处理的重启
    SystemRebooter::update();

    // 驱动异步软重启状态机（非阻塞）
    driveSoftRestartStateMachine();

    if (sseRouteHandler) {
        sseRouteHandler->performMaintenance();
    }

    if (ctx) {
        ctx->maintainWebForegroundTaskBudget();
    }

    const unsigned long maintenanceNow = millis();
    const bool inStartupGrace = (maintenanceNow < 120000UL);
    static unsigned long safeSoftRestartSince = 0;

    // ── Web 服务意外停止后的自动重启 ──
    // 如果 isRunning 为 false 但 server 实例有效，尝试重新启动（带冷却期保护）
    if (!isRunning && server) {
        if (isWebRecoverySuppressed(maintenanceNow)) {
            LOG_DEBUG("[WebConfig] Web auto-restart suppressed by active pause");
            safeSoftRestartSince = 0;
            severePressureSinceMs = 0;
            return;
        }
        if (webPauseReason != WebServicePauseReason::None) {
            LOG_WARNING("[WebConfig] Web pause window expired, allowing auto-restart");
            webPauseReason = WebServicePauseReason::None;
            webPauseUntilMs = 0;
        }
        if (lastWebRecoveryMs == 0 || (maintenanceNow - lastWebRecoveryMs >= RECOVERY_COOLDOWN_MS)) {
            LOG_WARNING("[WebConfig] Web server is stopped, attempting auto-restart");
            const bool restarted = start();  // start() 现在会验证端口监听
            lastWebRecoveryMs = maintenanceNow;
            if (restarted) {
                webRecoveryCount++;
                recordRecoveryEvent("auto_restart",
                                    "server_was_stopped",
                                    maintenanceNow,
                                    ESP.getFreeHeap(),
                                    heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                                    0);
            } else {
                LOG_ERROR("[WebConfig] Web auto-restart FAILED (port 80 not listening)");
                recordRecoveryEvent("auto_restart_failed",
                                    "bind_error",
                                    maintenanceNow,
                                    ESP.getFreeHeap(),
                                    heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                                    0);
            }
        } else {
            LOG_DEBUG("[WebConfig] Web server stopped, waiting for recovery cooldown");
        }
        // 停止状态下不执行后续压力检测
        safeSoftRestartSince = 0;
        severePressureSinceMs = 0;
        return;
    }

    // TCP 连接池健康监测与 Web 服务自动恢复
    checkAndRecoverWebServer();

    // 定期清理 TIME_WAIT 连接（每 30s 一次）
    // 不等到软重启才清理，主动释放 TCP PCB 槽位给新请求
    {
        static unsigned long lastTwCleanupMs = 0;
        if (maintenanceNow - lastTwCleanupMs >= 30000UL) {
            lastTwCleanupMs = maintenanceNow;
            uint8_t aborted = 0;
            LOCK_TCPIP_CORE();
            struct tcp_pcb* pcb = tcp_tw_pcbs;
            while (pcb != nullptr) {
                struct tcp_pcb* next = pcb->next;
                tcp_abort(pcb);
                aborted++;
                pcb = next;
            }
            UNLOCK_TCPIP_CORE();
            if (aborted > 0) {
                LOG_INFOF("[WebConfig] Periodic TIME_WAIT cleanup: aborted %u connections", (unsigned)aborted);
            }
        }
    }

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

    // ── 端口监听看门狗：检测 isRunning=true 但端口 80 实际未监听的情况 ──
    // 这会在 server->begin() 报 bind error: -8 后触发
    if (maintenanceNow - lastListenCheckMs >= LISTEN_CHECK_INTERVAL_MS) {
        lastListenCheckMs = maintenanceNow;
        if (isRunning && !isPortListening(80)) {
            listenCheckFailCount++;
            LOG_WARNINGF("[WebConfig] Port 80 not listening but isRunning=true (fail=%u/%u)",
                         listenCheckFailCount, LISTEN_CHECK_FAIL_TRIGGER);
            if (listenCheckFailCount >= LISTEN_CHECK_FAIL_TRIGGER) {
                LOG_ERROR("[WebConfig] Port 80 watchdog triggered: server claims running but not listening");
                isRunning = false;
                listenCheckFailCount = 0;
                recordRecoveryEvent("listen_watchdog",
                                    "port_not_listening",
                                    maintenanceNow,
                                    ESP.getFreeHeap(),
                                    heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                                    0);
                // 尝试立即重启
                if (start()) {
                    webRecoveryCount++;
                    recordRecoveryEvent("watchdog_restart",
                                        "port_watchdog_recovered",
                                        maintenanceNow,
                                        ESP.getFreeHeap(),
                                        heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                                        0);
                    LOG_INFO("[WebConfig] Port watchdog restart succeeded");
                } else {
                    LOG_ERROR("[WebConfig] Port watchdog restart FAILED, scheduling device reboot");
                    scheduleDeviceRestartForWebRecovery("watchdog_restart_failed",
                                                       maintenanceNow,
                                                       ESP.getFreeHeap(),
                                                       heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                                                       0);
                }
                return;
            }
        } else {
            listenCheckFailCount = 0;
        }
    }

    // ── 无请求看门狗：isRunning=true 但长时间未收到任何 HTTP 请求 ──
    // 仅在以下组合条件下触发，避免设备正常运行但无人访问时产生不必要的重启：
    //   1. WiFi 已连接（排除断网场景下的误判）
    //   2. 端口 80 确实在监听（排除 bind 失败）
    //   3. 存在 TIME_WAIT 或异常 TCP 连接堆积（表明之前有连接但当前僵死）
    // 如果端口正常监听且无异常连接，说明 Web 服务健康，仅因无人访问而已，无需重启
    if (lastRequestSeenMs > 0 && isRunning) {
        const unsigned long noRequestDuration = maintenanceNow - lastRequestSeenMs;
        if (noRequestDuration >= NO_REQUEST_WATCHDOG_MS) {
            // 检查 WiFi 连接状态（未联网时无请求是正常的）
            const bool wifiConnected = (WiFi.status() == WL_CONNECTED);
            if (!wifiConnected) {
                // WiFi 未连接，无人访问是正常的，重置计时器跳过
                lastRequestSeenMs = millis();
                LOG_DEBUG("[WebConfig] No-request watchdog skipped: WiFi not connected");
            } else {
                // WiFi 已连接，进一步检查是否有僵死连接
                uint8_t timeWaitCount = 0;
                const uint16_t activeConns = countTcpConnections(&timeWaitCount);
                if (activeConns == 0 && timeWaitCount == 0) {
                    // 没有任何 TCP 连接，端口正常监听，仅因无人访问 → 不重启
                    lastRequestSeenMs = millis();
                    LOG_DEBUGF("[WebConfig] No-request watchdog skipped: port healthy, no stale connections (%lus idle)",
                               (unsigned long)(noRequestDuration / 1000));
                } else {
                    // 有僵死连接或异常堆积 → 触发软重启清理
                    LOG_WARNINGF("[WebConfig] No HTTP requests for %lus with %u active/%u TIME_WAIT connections, triggering soft restart",
                                 (unsigned long)(noRequestDuration / 1000),
                                 (unsigned)activeConns, (unsigned)timeWaitCount);
                    lastRequestSeenMs = millis();  // 重置避免连续触发
                    softRestartWebServer("no_request_watchdog");
                    return;
                }
            }
        }
    }

    const uint32_t maintenanceFreeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t maintenanceLargestBlock =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const uint8_t maintenanceFrag = calculateHeapFragmentationPercent(
        maintenanceFreeHeap,
        maintenanceLargestBlock);

    const bool severeWebPressure = FastBee::MemoryBudget::isSevereWebPressure(
        maintenanceFreeHeap,
        maintenanceLargestBlock,
        maintenanceFrag);

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
        } else {
            const unsigned long pressureDuration = maintenanceNow - safeSoftRestartSince;
            // 持续 30s 严重压力：先尝试轻量软重启 Web 服务（比设备重启更温和）
            if (pressureDuration >= 30000UL && pressureDuration < 35000UL) {
                LOG_WARNING("[WebConfig] Sustained pressure 30s, trying soft-restart before device restart");
                softRestartWebServer("sustained_pressure_soft_restart");
                // softRestart 后重置计时器，给予恢复观察期
                safeSoftRestartSince = maintenanceNow;
            } else if (pressureDuration >= 120000UL) {
                // 120s 仍未恢复：调度设备重启
                LOG_WARNING("[WebConfig] Scheduling device restart after sustained web pressure");
                scheduleDeviceRestartForWebRecovery("sustained_web_pressure",
                                                    maintenanceNow,
                                                    maintenanceFreeHeap,
                                                    maintenanceLargestBlock,
                                                    maintenanceFrag);
                safeSoftRestartSince = 0;
                severePressureSinceMs = 0;
            }
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

// ============ TCP 连接池健康监测与自动恢复 ============

uint16_t WebConfigManager::countTcpConnections(uint8_t* outTimeWait) const {
    uint16_t total = 0;
    uint8_t timeWait = 0;

    // 必须持有 TCPIP 核心锁才能安全遍历 lwIP PCB 链表
    // 否则 AsyncTCP 任务并发访问会触发 tcp_abandon 断言
    LOCK_TCPIP_CORE();

    // 遍历 lwIP 活跃 TCP PCB 链表
    for (struct tcp_pcb* pcb = tcp_active_pcbs; pcb != nullptr; pcb = pcb->next) {
        total++;
        if (pcb->state == TIME_WAIT) {
            timeWait++;
        }
    }
    // 遍历 TIME_WAIT 专用链表
    for (struct tcp_pcb* pcb = tcp_tw_pcbs; pcb != nullptr; pcb = pcb->next) {
        total++;
        timeWait++;
    }

    UNLOCK_TCPIP_CORE();

    if (outTimeWait) {
        *outTimeWait = timeWait;
    }
    return total;
}

void WebConfigManager::checkAndRecoverWebServer() {
    if (!isRunning) return;

    const unsigned long now = millis();

    // 启动保护期：前 120 秒不做 TCP 健康检查（系统初始化 TCP 连接多属正常）
    if (now < 120000UL) return;

    // 按间隔执行检查
    if (now - lastTcpCheckMs < CHECK_INTERVAL_MS) return;
    lastTcpCheckMs = now;

    // 冷却期内不检查
    if (lastWebRecoveryMs > 0 && (now - lastWebRecoveryMs < RECOVERY_COOLDOWN_MS)) {
        tcpUnhealthyCount = 0;
        burstWithPressureCount = 0;
        return;
    }

    uint8_t timeWaitCount = 0;
    uint16_t totalConn = countTcpConnections(&timeWaitCount);

    // 判断 TCP 连接池是否处于耗尽状态
    const bool tcpExhausted = (totalConn >= TCP_CONN_EXHAUSTION_THRESHOLD) ||
                               (timeWaitCount >= (TCP_CONN_EXHAUSTION_THRESHOLD - 2));

    // 检查请求突增 + 内存压力组合
    const bool burst = isRequestBurst();
    const uint32_t freeHeap = ESP.getFreeHeap();
    const uint32_t maxAlloc = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    const bool memPressure = (freeHeap < 16384U) || (maxAlloc < 6144U);

    if (burst && memPressure) {
        burstWithPressureCount++;
        LOG_WARNINGF("[WebConfig] Burst + memory pressure: heap=%lu maxAlloc=%u requests=%u/%us count=%u/%u",
                     (unsigned long)freeHeap, (unsigned)maxAlloc,
                     (unsigned)requestCountInWindow, (unsigned)(BURST_WINDOW_MS / 1000),
                     (unsigned)burstWithPressureCount, (unsigned)BURST_RECOVERY_TRIGGER);

        if (burstWithPressureCount >= BURST_RECOVERY_TRIGGER) {
            char reason[64];
            snprintf(reason, sizeof(reason), "burst_pressure:heap=%lu,req=%u",
                     (unsigned long)freeHeap, (unsigned)requestCountInWindow);
            softRestartWebServer(reason);
            burstWithPressureCount = 0;
        }
    } else {
        if (burstWithPressureCount > 0 && !burst) {
            LOG_INFOF("[WebConfig] Burst pressure cleared: heap=%lu requests=%u",
                      (unsigned long)freeHeap, (unsigned)requestCountInWindow);
        }
        burstWithPressureCount = 0;
    }

    if (tcpExhausted) {
        tcpUnhealthyCount++;
        LOG_WARNINGF("[WebConfig] TCP pool unhealthy: total=%u timeWait=%u count=%u/%u",
                     (unsigned)totalConn, (unsigned)timeWaitCount,
                     (unsigned)tcpUnhealthyCount, (unsigned)UNHEALTHY_COUNT_TRIGGER);

        if (tcpUnhealthyCount >= UNHEALTHY_COUNT_TRIGGER) {
            char reason[64];
            snprintf(reason, sizeof(reason), "tcp_exhausted:total=%u,tw=%u",
                     (unsigned)totalConn, (unsigned)timeWaitCount);
            softRestartWebServer(reason);
            tcpUnhealthyCount = 0;
        }
    } else {
        if (tcpUnhealthyCount > 0) {
            LOG_INFOF("[WebConfig] TCP pool recovered: total=%u timeWait=%u",
                      (unsigned)totalConn, (unsigned)timeWaitCount);
        }
        tcpUnhealthyCount = 0;
    }
}

bool WebConfigManager::softRestartWebServer(const char* reason) {
    if (!server) return false;

    // 如果已有软重启在进行，忽略重复请求
    if (_softRestartPhase != SoftRestartPhase::IDLE) return false;

    const unsigned long now = millis();
    const uint32_t freeHeap = ESP.getFreeHeap();
    const uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    const uint8_t frag = calculateHeapFragmentationPercent(freeHeap, largestBlock);

    LOG_WARNINGF("[WebConfig] Soft-restarting web server: reason=%s heap=%lu largest=%lu",
                 reason, (unsigned long)freeHeap, (unsigned long)largestBlock);

    // 记录恢复事件（在启动前记录，startMs 用于冷却期计算）
    lastWebRecoveryMs = now;
    webRecoveryCount++;
    recordRecoveryEvent("web_soft_restart", reason, now, freeHeap, largestBlock, frag);

    // 更新 soft restart 统计信息
    lastSoftRestartAtMs = now;
    lastSoftRestartReason = reason ? reason : "tcp_exhaustion";
    lastSoftRestartFreeHeap = freeHeap;
    lastSoftRestartLargestBlock = largestBlock;
    lastSoftRestartFrag = frag;
    softRestartCount++;

    // 步骤1：断开所有 SSE 客户端（释放 TCP 缓冲区）
    if (sseRouteHandler) {
        size_t closed = sseRouteHandler->closeAllClients();
        LOG_INFOF("[WebConfig] Closed %u SSE clients before restart", (unsigned)closed);
    }

    // 步骤2：停止 Web 服务器
    server->end();
    isRunning = false;

    // 保存 reason 供状态机使用
    strncpy(_softRestartReason, reason ? reason : "unknown", sizeof(_softRestartReason) - 1);
    _softRestartReason[sizeof(_softRestartReason) - 1] = '\0';

    // 步骤3：进入异步状态机，等待 TCP 栈处理 FIN/RST（原 delay(200)）
    _softRestartPhase = SoftRestartPhase::WAIT_TCP_CLOSE;
    _softRestartPhaseStartMs = now;

    // 实际重启由 driveSoftRestartStateMachine() 在 performMaintenance() 中异步完成
    LOG_WARNINGF("[WebConfig] Web server soft-restart scheduled (total=%lu, cooldown=%lus)",
                 (unsigned long)webRecoveryCount, (unsigned long)(RECOVERY_COOLDOWN_MS / 1000));

    return true;
}

// 异步软重启状态机：由 performMaintenance() 定期驱动
// 将原来的 delay(200) + delay(100) 共 300ms 阻塞拆分为非阻塞阶段
void WebConfigManager::driveSoftRestartStateMachine() {
    if (_softRestartPhase == SoftRestartPhase::IDLE) return;
    if (!server) { _softRestartPhase = SoftRestartPhase::IDLE; return; }

    const unsigned long now = millis();
    const unsigned long elapsed = now - _softRestartPhaseStartMs;

    switch (_softRestartPhase) {
        case SoftRestartPhase::WAIT_TCP_CLOSE:
            if (elapsed >= 200) {
                // 步骤4：强制清理 TIME_WAIT 连接（通过 tcp_abort）
                uint8_t aborted = 0;
                LOCK_TCPIP_CORE();
                struct tcp_pcb* pcb = tcp_tw_pcbs;
                while (pcb != nullptr) {
                    struct tcp_pcb* next = pcb->next;
                    tcp_abort(pcb);
                    aborted++;
                    pcb = next;
                }
                UNLOCK_TCPIP_CORE();
                if (aborted > 0) {
                    LOG_INFOF("[WebConfig] Aborted %u TIME_WAIT connections", (unsigned)aborted);
                }
                // 进入 lwIP 清理等待（原 delay(100)）
                _softRestartPhase = SoftRestartPhase::WAIT_LWIP_CLEANUP;
                _softRestartPhaseStartMs = millis();
            }
            break;

        case SoftRestartPhase::WAIT_LWIP_CLEANUP:
            if (elapsed >= 100) {
                // 步骤6：重新启动 Web 服务器
                server->begin();
                isRunning = true;
                _softRestartPhase = SoftRestartPhase::IDLE;
                LOG_INFO("[WebConfig] Web server soft-restart completed (async)");
            }
            break;

        default:
            _softRestartPhase = SoftRestartPhase::IDLE;
            break;
    }
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

    // 使用统一的 SystemRebooter 调度重启（自动记录 RestartDiagnostics）
    SystemRebooter::scheduleReboot(
        lastSoftRestartReason.c_str(),
        delayMs,
        RestartReason::WEB_RECOVERY);

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

// ============ 请求突增检测 ============

void WebConfigManager::trackWebRequest() {
    unsigned long now = millis();
    lastRequestSeenMs = now;  // 记录最后一次请求时间（看门狗用）
    // 滑动窗口重置
    if (burstWindowStartMs == 0 || (now - burstWindowStartMs) >= BURST_WINDOW_MS) {
        burstWindowStartMs = now;
        requestCountInWindow = 1;
        return;
    }
    requestCountInWindow++;
}

bool WebConfigManager::isRequestBurst() const {
    if (burstWindowStartMs == 0) return false;
    unsigned long now = millis();
    // 仅在当前窗口内有效
    if ((now - burstWindowStartMs) >= BURST_WINDOW_MS) return false;
    return requestCountInWindow >= BURST_THRESHOLD;
}

bool WebConfigManager::isForegroundRequestActive() const {
    if (!ctx) return false;
    if (ctx->webForegroundModeActive) return true;
    return (int32_t)(ctx->webForegroundUntilMs - millis()) > 0;
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
    // 注意：ProtocolManager 的 SSE 回调（Modbus/MQTT 状态推送）统一在
    // setProtocolManager() 中注册，避免此处和 setProtocolManager() 双重注册
    // 导致 SSE 客户端收到重复数据。

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
