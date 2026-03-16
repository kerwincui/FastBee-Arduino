#ifndef WEB_CONFIG_MANAGER_H
#define WEB_CONFIG_MANAGER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <memory>

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
class ProvisionRouteHandler;
class OTARouteHandler;
class PeripheralRouteHandler;
class ProtocolRouteHandler;

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

    // 9 个专职路由处理器
    std::unique_ptr<StaticRouteHandler>     staticHandler;
    std::unique_ptr<AuthRouteHandler>       authHandler;
    std::unique_ptr<UserRouteHandler>       userHandler;
    std::unique_ptr<RoleRouteHandler>       roleHandler;
    std::unique_ptr<SystemRouteHandler>     systemHandler;
    std::unique_ptr<ProvisionRouteHandler>  provisionHandler;
    std::unique_ptr<OTARouteHandler>        otaHandler;
    std::unique_ptr<PeripheralRouteHandler> peripheralHandler;
    std::unique_ptr<ProtocolRouteHandler>   protocolHandler;

    void setupAllRoutes();
};

#endif // WEB_CONFIG_MANAGER_H
