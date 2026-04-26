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
class UserRouteHandler;
class RoleRouteHandler;
class SystemRouteHandler;
class LogRouteHandler;
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

private:
    AsyncWebServer* server;
    bool isRunning;

    // 共享上下文（所有 Handler 通过指针引用）
    std::unique_ptr<WebHandlerContext> ctx;

    // 14 个专职路由处理器
    std::unique_ptr<StaticRouteHandler>     staticHandler;
    std::unique_ptr<AuthRouteHandler>       authHandler;
    std::unique_ptr<UserRouteHandler>       userHandler;
    std::unique_ptr<RoleRouteHandler>       roleHandler;
    std::unique_ptr<SystemRouteHandler>     systemHandler;
    std::unique_ptr<LogRouteHandler>        logHandler;
    std::unique_ptr<DeviceRouteHandler>     deviceHandler;
    std::unique_ptr<BatchRouteHandler>      batchHandler;
    std::unique_ptr<ProvisionRouteHandler>  provisionHandler;
    std::unique_ptr<OTARouteHandler>        otaHandler;
    std::unique_ptr<PeripheralRouteHandler> peripheralHandler;
#if FASTBEE_ENABLE_PERIPH_EXEC
    std::unique_ptr<PeriphExecRouteHandler> periphExecHandler;
#endif
#if FASTBEE_ENABLE_RULE_SCRIPT
    std::unique_ptr<RuleScriptRouteHandler> ruleScriptHandler;
#endif
    std::unique_ptr<ProtocolRouteHandler>   protocolHandler;
    std::unique_ptr<SSERouteHandler>        sseRouteHandler;

    void setupAllRoutes();
};

#endif // WEB_CONFIG_MANAGER_H
