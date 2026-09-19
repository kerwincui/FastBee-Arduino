#ifndef WEB_CONFIG_MANAGER_H
#define WEB_CONFIG_MANAGER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <atomic>
#include <memory>

#include "core/FeatureFlags.h"
#include "core/ChipConfig.h"
#include "core/MemoryBudget.h"
#include "core/ResourceProfile.h"
#include "network/WebMemoryGuard.h"
#include "network/IpRateLimiter.h"

// 接口包含
#include "core/interfaces/IAuthManager.h"
#include "core/interfaces/IUserManager.h"

// 前向声明
class FBNetworkManager;
class OTAManager;
class ProtocolManager;
class WebHandlerContext;
class StaticRouteHandler;
class AuthRouteHandler;
#if FASTBEE_ENABLE_USER_ADMIN
class UserRouteHandler;
#endif
class LogRouteHandler;
class SystemRouteHandler;
class DeviceRouteHandler;
class BatchRouteHandler;
class ProvisionRouteHandler;
class OTARouteHandler;
class PeripheralRouteHandler;
class IrRouteHandler;
class RfRouteHandler;
#if FASTBEE_ENABLE_PERIPH_EXEC
class PeriphExecRouteHandler;
#endif
#if FASTBEE_ENABLE_RULE_SCRIPT
class RuleScriptRouteHandler;
#endif
class ProtocolRouteHandler;
class FeatureRouteHandler;
#if FASTBEE_ENABLE_SCREEN_PANEL || FASTBEE_ENABLE_POINT_HISTORY
class ScreenPanelRouteHandler;
#endif
#if FASTBEE_ENABLE_REMOTE_RESOURCES
class RemoteResourceRouteHandler;
#endif
class SSERouteHandler;
class BLEProvisioner;

struct WebRecoveryEvent {
    unsigned long atMs = 0;
    uint32_t freeHeap = 0;
    uint32_t largestBlock = 0;
    uint8_t fragmentation = 0;
    uint8_t sseClients = 0;
    char type[24] = "";
    char reason[32] = "";
};

enum class WebServicePauseReason : uint8_t {
    None = 0,
    MqttsHandshake = 1
};

/**
 * @brief Web配置管理器 - 瘦协调器
 * 
 * 持有 WebHandlerContext 共享上下文和9个专职 Handler，
 * 负责创建、注册路由和执行维护任务。
 */
class WebConfigManager {
public:
    WebConfigManager(AsyncWebServer* webServer,
                     IAuthManager* authMgr, IUserManager* userMgr);
    ~WebConfigManager();

    WebConfigManager(const WebConfigManager&) = delete;
    WebConfigManager& operator=(const WebConfigManager&) = delete;

    bool initialize();
    bool start();
    void stop();
    bool isServerRunning() const;
    bool pauseForMqttsHandshake(unsigned long holdMs = FastBee::MemoryBudget::MQTTS_WEB_PAUSE_HOLD_MS,
                                bool force = false);
    bool resumeFromMqttsHandshake();
    bool isPausedForMqttsHandshake() const { return webPauseReason == WebServicePauseReason::MqttsHandshake; }

    void setNetworkManager(FBNetworkManager* netMgr);
    void setOTAManager(OTAManager* otaMgr);
    void setProtocolManager(ProtocolManager* protoMgr);
    void setBLEProvisioner(BLEProvisioner* bleProv);

    AsyncWebServer* getWebServer() const;
    void performMaintenance();

    /// 请求突增跟踪（由 WebHandlerContext::noteWebRequestActivity 调用）
    void trackWebRequest();
    bool isRequestBurst() const;
    bool isForegroundRequestActive() const;
    // Web 忙碌：前台窗口活跃或有静态响应在途。MQTT publish 门控用
    // （实测 TLS publish 与在途响应竞争 lwIP/WiFi TX 会把响应卡死）。
    bool isWebBusy() const;

    /// 暴露 SSE 路由处理器（供 HealthMonitor 在内存严重不足时强制断开 SSE 客户端）
    SSERouteHandler* getSseRouteHandler() const { return sseRouteHandler.get(); }
    size_t getSseClientCount() const;

    /// 内存护栏状态（供 /api/system/web-runtime 诊断；未启用时恒 false/0）
#if FASTBEE_WEB_MEM_GUARD
    bool isMemGuardActive() const { return memGuard ? memGuard->isActive() : false; }
    uint32_t getMemGuardRejectedCount() const {
        return memGuard ? memGuard->getRejectedCount() : 0;
    }
    /// 暴露护栏中间件（供 HealthMonitor 周期调用 maintain()，保证无请求时
    /// FORCE-RECOVER 兜底仍能触发）
    WebMemoryGuardMiddleware* getMemGuard() const { return memGuard.get(); }
#else
    bool isMemGuardActive() const { return false; }
    uint32_t getMemGuardRejectedCount() const { return 0; }
#endif
    unsigned long getLastSoftRestartAtMs() const { return lastSoftRestartAtMs; }
    const String& getLastSoftRestartReason() const { return lastSoftRestartReason; }
    uint32_t getLastSoftRestartFreeHeap() const { return lastSoftRestartFreeHeap; }
    uint32_t getLastSoftRestartLargestBlock() const { return lastSoftRestartLargestBlock; }
    uint8_t getLastSoftRestartFragmentation() const { return lastSoftRestartFrag; }
    uint32_t getSoftRestartCount() const { return softRestartCount; }
    unsigned long getSeverePressureSinceMs() const { return severePressureSinceMs; }
    size_t copyRecoveryEvents(WebRecoveryEvent* out, size_t maxEvents) const;

    /// 停摆取证诊断：TCP 连接总数 / TIME_WAIT 数 / 距上次 HTTP 请求的毫秒数
    /// 供框架 STATUS 心跳输出，用于判定停摆时是连接积压、请求停流还是其他原因
    void getTcpDiag(uint16_t& total, uint8_t& timeWait, unsigned long& lastReqAgeMs) const {
        timeWait = 0;
        total = countTcpConnections(&timeWait);
        lastReqAgeMs = getLastWebRequestAgeMs();
    }

private:
    unsigned long getLastWebRequestAgeMs() const;
    static constexpr size_t MAX_RECOVERY_EVENTS = 8;

    AsyncWebServer* server;
    bool isRunning;

    // 共享上下文（所有 Handler 通过指针引用）
    std::unique_ptr<WebHandlerContext> ctx;

    // 14 个专职路由处理器（条件编译裁剪未启用的模块）
    std::unique_ptr<StaticRouteHandler>     staticHandler;
    std::unique_ptr<AuthRouteHandler>       authHandler;
#if FASTBEE_ENABLE_USER_ADMIN
    std::unique_ptr<UserRouteHandler>       userHandler;
#endif
#if FASTBEE_ENABLE_LOG_VIEWER || FASTBEE_ENABLE_FILE_LOGGING
    std::unique_ptr<LogRouteHandler>        logHandler;
#endif
    std::unique_ptr<SystemRouteHandler>     systemHandler;
    std::unique_ptr<DeviceRouteHandler>     deviceHandler;
    std::unique_ptr<BatchRouteHandler>      batchHandler;
    std::unique_ptr<ProvisionRouteHandler>  provisionHandler;
#if FASTBEE_ENABLE_OTA
    std::unique_ptr<OTARouteHandler>        otaHandler;
#endif
    std::unique_ptr<PeripheralRouteHandler> peripheralHandler;
#if FASTBEE_ENABLE_IR_REMOTE
    std::unique_ptr<IrRouteHandler>         irHandler;
#endif
#if FASTBEE_ENABLE_RF_LEARN
    std::unique_ptr<RfRouteHandler>         rfHandler;
#endif
#if FASTBEE_ENABLE_PERIPH_EXEC
    std::unique_ptr<PeriphExecRouteHandler> periphExecHandler;
#endif
#if FASTBEE_ENABLE_RULE_SCRIPT
    std::unique_ptr<RuleScriptRouteHandler> ruleScriptHandler;
#endif
#if FASTBEE_ENABLE_MQTT || FASTBEE_ENABLE_MODBUS
    std::unique_ptr<ProtocolRouteHandler>   protocolHandler;
#endif
    std::unique_ptr<FeatureRouteHandler>    featureHandler;
#if FASTBEE_ENABLE_SCREEN_PANEL || FASTBEE_ENABLE_POINT_HISTORY
    std::unique_ptr<ScreenPanelRouteHandler> screenPanelHandler;
#endif
#if FASTBEE_ENABLE_REMOTE_RESOURCES
    std::unique_ptr<RemoteResourceRouteHandler> remoteResourceHandler;
#endif
    std::unique_ptr<SSERouteHandler>        sseRouteHandler;

#if FASTBEE_WEB_MEM_GUARD
    // 内存护栏中间件（classic ESP32 无 PSRAM）：低 DRAM 时拒绝 API 请求。
    // server 中间件链只持裸指针，必须保证本对象生命周期覆盖 server 使用期
    std::unique_ptr<WebMemoryGuardMiddleware> memGuard;
#endif

    unsigned long lastSoftRestartAtMs = 0;
    String lastSoftRestartReason;
    uint32_t lastSoftRestartFreeHeap = 0;
    uint32_t lastSoftRestartLargestBlock = 0;
    uint8_t lastSoftRestartFrag = 0;
    uint32_t softRestartCount = 0;
    unsigned long severePressureSinceMs = 0;
    WebRecoveryEvent recoveryEvents[MAX_RECOVERY_EVENTS];
    size_t recoveryEventCount = 0;
    size_t recoveryEventHead = 0;

    // IP 维度令牌桶限流（固定 16 槽 + LRU，~320B，无堆分配）
    IpRateLimiter ipRateLimiter;

    // 限流分级判定 + 令牌消耗（分发链前端中间件调用）
    bool checkIpRateLimit(AsyncWebServerRequest* request, uint32_t& retryAfterSec);

    void setupAllRoutes();
    void recordRecoveryEvent(const char* type,
                             const char* reason,
                             unsigned long atMs,
                             uint32_t freeHeap,
                             uint32_t largestBlock,
                             uint8_t fragmentation);
    void scheduleDeviceRestartForWebRecovery(const char* reason,
                                             unsigned long atMs,
                                             uint32_t freeHeap,
                                             uint32_t largestBlock,
                                             uint8_t fragmentation,
                                             unsigned long delayMs = 3000UL);
    void copyText(char* dest, size_t destSize, const char* text) const;

    // ── TCP 连接池健康监测与 Web 服务自动恢复 ──
    void checkAndRecoverWebServer();
    uint16_t countTcpConnections(uint8_t* outTimeWait = nullptr) const;
    uint16_t countTcpConnectionsOnLocalPort(uint16_t port,
                                            uint8_t* outTimeWait = nullptr) const;
    uint8_t pruneTimeWaitPcbs(uint8_t maxToKill);
    // 强制回收占用指定本地端口的所有非监听 PCB（active + TIME_WAIT）
    // 用于：1) 软重启前释放端口，避免 tcp_bind 返回 ERR_USE(-8) 永久失败
    //       2) 清理对端已消失的僵死 ESTABLISHED 连接（占满 AsyncTCP 客户端槽位）
    uint8_t abortPcbsOnLocalPort(uint16_t port);
    bool softRestartWebServer(const char* reason);
    void driveSoftRestartStateMachine();  // 异步软重启状态机驱动
    bool isPortListening(uint16_t port = 80) const;

    // TCP PCB 耗尽阈值：统一引用 ResourceProfile（单一真相来源）
    // 必须保证 < MEMP_NUM_TCP_PCB(16)，编译期 static_assert 保护
    static constexpr uint16_t TCP_CONN_EXHAUSTION_THRESHOLD =
        FastBee::ResourceProfile::TCP_CONN_EXHAUSTION_THRESHOLD;
    static constexpr uint8_t  UNHEALTHY_COUNT_TRIGGER = 3;         // 连续不健康次数触发恢复
    static constexpr unsigned long RECOVERY_COOLDOWN_MS = 30000UL; // 恢复冷却期 30s
    static constexpr unsigned long CHECK_INTERVAL_MS = 5000UL;     // 检查间隔 5s（折中：2s 对 ESP32-F4R0 CPU 开销过大，10s 对 PCB 耗尽响应太慢）

    // TIME_WAIT 主动修剪：lwIP TIME_WAIT 默认持续 2×MSL(120s)，密集短连接很快占满 PCB 池
    // 超过阈值时主动回收（与 lwIP tcp_alloc 耗尽时杀 TIME_WAIT 同理），避免软重启造成服务中断
    static constexpr uint8_t TIME_WAIT_PRUNE_THRESHOLD = 6;  // TIME_WAIT 超过此值开始修剪（浏览器首屏 ~12 连接需留足 PCB 余量）
    static constexpr uint8_t TIME_WAIT_PRUNE_KEEP = 2;       // 修剪后保留的 TIME_WAIT 数量（1 太激进，在途 close 连接易被误杀）

    uint8_t  tcpUnhealthyCount = 0;       // 连续检测到TCP不健康的次数
    unsigned long lastTcpCheckMs = 0;     // 上次TCP健康检查时间
    unsigned long lastWebRecoveryMs = 0;  // 上次Web服务恢复时间
    uint32_t webRecoveryCount = 0;        // Web服务软重启累计次数
    WebServicePauseReason webPauseReason = WebServicePauseReason::None;
    unsigned long webPauseUntilMs = 0;
    bool isWebRecoverySuppressed(unsigned long now) const;

    // ── 异步软重启状态机（替代阻塞 delay）──
    enum class SoftRestartPhase : uint8_t {
        IDLE,                // 无进行中的软重启
        WAIT_TCP_CLOSE,      // 等待 TCP FIN/RST 处理 (200ms)
        WAIT_LWIP_CLEANUP,   // 等待 lwIP 完成 TIME_WAIT 清理 (100ms)
        WAIT_PORT_BIND       // 等待端口 80 绑定成功（最多重试 3 次）
    };
    SoftRestartPhase _softRestartPhase = SoftRestartPhase::IDLE;
    unsigned long _softRestartPhaseStartMs = 0;
    uint8_t _softRestartBindRetries = 0;
    char _softRestartReason[48] = {0};

    // ── 请求突增检测与主动恢复 ──

    static constexpr unsigned long BURST_WINDOW_MS = 5000UL;   // 突增检测窗口 5s
    static constexpr uint16_t BURST_THRESHOLD = 20;           // 5s 内超过 20 次请求视为突增
    static constexpr uint8_t  BURST_RECOVERY_TRIGGER = 2;     // 连续 2 次检测到突增+低内存触发恢复

    uint16_t requestCountInWindow = 0;           // 当前窗口内请求数
    unsigned long burstWindowStartMs = 0;        // 当前窗口起始时间
    uint8_t  burstWithPressureCount = 0;         // 连续检测到突增+内存压力的次数

    // ── Web 服务看门狗：检测 isRunning=true 但端口未监听的情况 ──
    std::atomic<uint32_t> lastRequestSeenMs{0};    // AsyncTCP 写入，维护任务读取
    unsigned long lastListenCheckMs = 0;           // 上次端口监听检查时间
    uint8_t  listenCheckFailCount = 0;             // 连续检测到端口未监听的次数
    static constexpr unsigned long LISTEN_CHECK_INTERVAL_MS = 30000UL;  // 30s 检查一次
    static constexpr uint8_t LISTEN_CHECK_FAIL_TRIGGER = 3;  // 连续 3 次失败触发恢复
    static constexpr unsigned long NO_REQUEST_WATCHDOG_MS = 120000UL;   // 2min 无请求且有残留连接时清理僵死 PCB
};

#endif // WEB_CONFIG_MANAGER_H
