#ifndef WEB_CONFIG_MANAGER_H
#define WEB_CONFIG_MANAGER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <memory>

#include "core/FeatureFlags.h"

// 接口包含
#include "core/interfaces/IAuthManager.h"
#include "core/interfaces/IUserManager.h"

// 前向声明
class RoleManager;
class NetworkManager;
class OTAManager;
class ProtocolManager;
class WebHandlerContext;
class StaticRouteHandler;
class AuthRouteHandler;
#if FASTBEE_ENABLE_USER_ADMIN
class UserRouteHandler;
#endif
#if FASTBEE_ENABLE_ROLE_ADMIN
class RoleRouteHandler;
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

    void setRoleManager(RoleManager* roleMgr);
    void setNetworkManager(NetworkManager* netMgr);
    void setOTAManager(OTAManager* otaMgr);
    void setProtocolManager(ProtocolManager* protoMgr);

    AsyncWebServer* getWebServer() const;
    void performMaintenance();

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
#if FASTBEE_ENABLE_ROLE_ADMIN
    std::unique_ptr<RoleRouteHandler>       roleHandler;
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
};

#endif // WEB_CONFIG_MANAGER_H
