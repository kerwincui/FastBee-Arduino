#ifndef WEB_CONFIG_MANAGER_H
#define WEB_CONFIG_MANAGER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <memory>

#include "core/FeatureFlags.h"
#include "core/ChipConfig.h"
#include "core/MemoryBudget.h"
#include "core/ResourceProfile.h"

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
#if FASTBEE_ENABLE_PERIPH_EXEC
class PeriphExecRouteHandler;
#endif
#if FASTBEE_ENABLE_RULE_SCRIPT
class RuleScriptRouteHandler;
#endif
class ProtocolRouteHandler;
class SSERouteHandler;

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
    bool pauseForMqttsHandshake(unsigned long holdMs = FastBee::MemoryBudget::MQTTS_WEB_PAUSE_HOLD_MS);
    bool resumeFromMqttsHandshake();
    bool isPausedForMqttsHandshake() const { return webPauseReason == WebServicePauseReason::MqttsHandshake; }

    void setNetworkManager(FBNetworkManager* netMgr);
    void setOTAManager(OTAManager* otaMgr);
    void setProtocolManager(ProtocolManager* protoMgr);

    AsyncWebServer* getWebServer() const;
    void performMaintenance();

    /// 请求突增跟踪（由 WebHandlerContext::noteWebRequestActivity 调用）
    void trackWebRequest();
    bool isRequestBurst() const;
    bool isForegroundRequestActive() const;

    /// 暴露 SSE 路由处理器（供 HealthMonitor 在内存严重不足时强制断开 SSE 客户端）
    SSERouteHandler* getSseRouteHandler() const { return sseRouteHandler.get(); }
    size_t getSseClientCount() const;
    unsigned long getLastSoftRestartAtMs() const { return lastSoftRestartAtMs; }
    const String& getLastSoftRestartReason() const { return lastSoftRestartReason; }
    uint32_t getLastSoftRestartFreeHeap() const { return lastSoftRestartFreeHeap; }
    uint32_t getLastSoftRestartLargestBlock() const { return lastSoftRestartLargestBlock; }
    uint8_t getLastSoftRestartFragmentation() const { return lastSoftRestartFrag; }
    uint32_t getSoftRestartCount() const { return softRestartCount; }
    unsigned long getSeverePressureSinceMs() const { return severePressureSinceMs; }
    size_t copyRecoveryEvents(WebRecoveryEvent* out, size_t maxEvents) const;

private:
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
#if FASTBEE_ENABLE_PERIPH_EXEC
    std::unique_ptr<PeriphExecRouteHandler> periphExecHandler;
#endif
#if FASTBEE_ENABLE_RULE_SCRIPT
    std::unique_ptr<RuleScriptRouteHandler> ruleScriptHandler;
#endif
#if FASTBEE_ENABLE_MQTT || FASTBEE_ENABLE_MODBUS || FASTBEE_ENABLE_TCP || FASTBEE_ENABLE_HTTP || FASTBEE_ENABLE_COAP
    std::unique_ptr<ProtocolRouteHandler>   protocolHandler;
#endif
    std::unique_ptr<SSERouteHandler>        sseRouteHandler;

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
    bool softRestartWebServer(const char* reason);
    bool isPortListening(uint16_t port = 80) const;

    // TCP PCB 耗尽阈值：统一引用 ResourceProfile（单一真相来源）
    // 必须保证 < MEMP_NUM_TCP_PCB(16)，编译期 static_assert 保护
    static constexpr uint16_t TCP_CONN_EXHAUSTION_THRESHOLD =
        FastBee::ResourceProfile::TCP_CONN_EXHAUSTION_THRESHOLD;
    static constexpr uint8_t  UNHEALTHY_COUNT_TRIGGER = 3;         // 连续不健康次数触发恢复
    static constexpr unsigned long RECOVERY_COOLDOWN_MS = 30000UL; // 恢复冷却期 30s
    static constexpr unsigned long CHECK_INTERVAL_MS = 10000UL;    // 检查间隔 10s

    uint8_t  tcpUnhealthyCount = 0;       // 连续检测到TCP不健康的次数
    unsigned long lastTcpCheckMs = 0;     // 上次TCP健康检查时间
    unsigned long lastWebRecoveryMs = 0;  // 上次Web服务恢复时间
    uint32_t webRecoveryCount = 0;        // Web服务软重启累计次数
    WebServicePauseReason webPauseReason = WebServicePauseReason::None;
    unsigned long webPauseUntilMs = 0;
    bool isWebRecoverySuppressed(unsigned long now) const;

    // ── 请求突增检测与主动恢复 ──

    static constexpr unsigned long BURST_WINDOW_MS = 5000UL;   // 突增检测窗口 5s
    static constexpr uint16_t BURST_THRESHOLD = 20;           // 5s 内超过 20 次请求视为突增
    static constexpr uint8_t  BURST_RECOVERY_TRIGGER = 2;     // 连续 2 次检测到突增+低内存触发恢复

    uint16_t requestCountInWindow = 0;           // 当前窗口内请求数
    unsigned long burstWindowStartMs = 0;        // 当前窗口起始时间
    uint8_t  burstWithPressureCount = 0;         // 连续检测到突增+内存压力的次数

    // ── Web 服务看门狗：检测 isRunning=true 但端口未监听的情况 ──
    unsigned long lastRequestSeenMs = 0;           // 上次收到 HTTP 请求的时间
    unsigned long lastListenCheckMs = 0;           // 上次端口监听检查时间
    uint8_t  listenCheckFailCount = 0;             // 连续检测到端口未监听的次数
    static constexpr unsigned long LISTEN_CHECK_INTERVAL_MS = 30000UL;  // 30s 检查一次
    static constexpr uint8_t LISTEN_CHECK_FAIL_TRIGGER = 3;  // 连续 3 次失败触发恢复
    static constexpr unsigned long NO_REQUEST_WATCHDOG_MS = 300000UL;   // 5 分钟无请求触发重启
};

#endif // WEB_CONFIG_MANAGER_H
