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
class ProtocolManager;
class SystemHealth;
struct NetworkStatusInfo;

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
    NetworkManager* getNetworkManager() const;
    WebConfigManager* getWebConfigManager() const;
    OTAManager* getOTAManager() const;
    TaskManager* getTaskManager() const;
    HealthMonitor* getHealthMonitor() const;
    UserManager* getUserManager() const;
    AuthManager* getAuthManager() const;
    ProtocolManager* getProtocolManager() const;
    
private:
    // 私有方法
    bool addSystemTasks();
    void checkForRestart();
    
    // 静态实例
    static FastBeeFramework* instance;
    
    // 系统状态
    bool systemInitialized;
    unsigned long lastHealthCheck;
    
    // HTTP服务器
    AsyncWebServer* server;
    
    // 子系统指针
    NetworkManager* network;
    WebConfigManager* webConfig;
    OTAManager* ota;
    TaskManager* taskManager;
    HealthMonitor* healthMonitor;
    UserManager* userManager;
    AuthManager* authManager;
    ProtocolManager* protocolManager;
};

#endif