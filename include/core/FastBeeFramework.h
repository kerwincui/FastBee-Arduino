/**
 * @file FastBeeFramework.h
 * @brief FastBee物联网平台框架核心类
 * @author kerwincui
 * @date 2025-12-02
 */

#ifndef FAST_BEE_FRAMEWORK_H
#define FAST_BEE_FRAMEWORK_H

#include <Arduino.h>
#include <functional>
#include <memory>

// 前向声明（减少头文件依赖）
class AsyncWebServer;
class AsyncWebServerRequest;
class NetworkManager;
class WebConfigManager;
class OTAManager;
class TaskManager;
class HealthMonitor;
class UserManager;
class AuthManager;
class RoleManager;
class ProtocolManager;
class SystemHealth;
struct NetworkStatusInfo;

// 接口类包含
#include "core/interfaces/INetworkManager.h"
#include "core/interfaces/ILoggerSystem.h"
#include "core/interfaces/IConfigStorage.h"

/**
 * @class FastBeeFramework
 * @brief FastBee物联网平台框架核心类，负责协调所有子系统
 */
class FastBeeFramework {
public:
    /**
     * @brief 获取框架单例实例
     * @return 框架实例指针
     */
    static FastBeeFramework* getInstance();
    
    /**
     * @brief 构造函数
     */
    FastBeeFramework();
    
    /**
     * @brief 析构函数
     */
    ~FastBeeFramework();
    
    // 删除拷贝构造函数和赋值运算符
    FastBeeFramework(const FastBeeFramework&) = delete;
    FastBeeFramework& operator=(const FastBeeFramework&) = delete;
    
    /**
     * @brief 初始化框架和所有子系统
     * @return 初始化是否成功
     */
    bool initialize();
    
    /**
     * @brief 运行框架主循环（应在Arduino loop()中调用）
     */
    void run();
    
    /**
     * @brief 关闭框架和所有子系统
     */
    void shutdown();
    
    /**
     * @brief 检查系统是否已初始化
     * @return 是否已初始化
     */
    bool isInitialized() const;
    
    /**
     * @brief 获取系统运行时间
     * @return 运行时间（毫秒）
     */
    unsigned long getUptime() const;
    
    // 获取子系统指针（提供各个子模块的统一访问入口，其他模块使用从入口调用，示例如下：）
    // FastBeeFramework* myFramework = FastBeeFramework::getInstance();
    // if (!myFramework->getHealthMonitor()->isSystemHealthy()) {
    //     处理异常
    // }
    INetworkManager* getNetworkManager() const;
    WebConfigManager* getWebConfigManager() const;
    OTAManager* getOTAManager() const;
    TaskManager* getTaskManager() const;
    HealthMonitor* getHealthMonitor() const;
    UserManager* getUserManager() const;
    AuthManager* getAuthManager() const;
    RoleManager* getRoleManager() const;
    ProtocolManager* getProtocolManager() const;
    
private:
    // 私有方法 - 初始化阶段拆分
    bool initStorageAndFS();           // 阶段 1: 存储和文件系统
    bool initLogger();                 // 阶段 2: 日志系统
    bool initWebServer();              // 阶段 3: HTTP 服务器
    bool initNetwork();                // 阶段 4: 网络管理器
    bool initSecurity();               // 阶段 5: 用户和认证管理
    bool initWebConfig();              // 阶段 6: Web配置管理
    bool initOTA();                    // 阶段 7: OTA 管理
    bool initSystems();                // 阶段 8: 任务和健康监控
    bool initProtocols();              // 阶段 9: 协议管理

    // 其他私有方法
    bool addSystemTasks();             // 添加系统任务
    void checkForRestart();            // 检查重启条件

    // 系统状态
    bool systemInitialized;
    unsigned long lastHealthCheck;
    
    // HTTP服务器
    std::unique_ptr<AsyncWebServer> server;
    
    // 子系统指针
    std::unique_ptr<NetworkManager> network;
    std::unique_ptr<WebConfigManager> webConfig;
    std::unique_ptr<OTAManager> ota;
    std::unique_ptr<TaskManager> taskManager;
    std::unique_ptr<HealthMonitor> healthMonitor;
    std::unique_ptr<UserManager> userManager;
    std::unique_ptr<RoleManager> roleManager;
    std::unique_ptr<AuthManager> authManager;
    std::unique_ptr<ProtocolManager> protocolManager;
};

#endif