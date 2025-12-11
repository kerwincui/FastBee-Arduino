/**
 * @description: FastBee物联网平台框架核心
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:28:57
 */

#include "core/FastBeeFramework.h"
#include "systems/LoggerSystem.h"
#include "network/NetworkManager.h"
#include "network/WebConfigManager.h"
#include "network/OTAManager.h"
#include "systems/TaskManager.h"
#include "systems/HealthMonitor.h"
#include "security/UserManager.h"
#include "security/AuthManager.h"
#include "protocols/ProtocolManager.h"
#include "systems/ConfigStorage.h"
#include "utils/TimeUtils.h"
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// 静态实例指针
FastBeeFramework* FastBeeFramework::instance = nullptr;

// 构造函数
FastBeeFramework::FastBeeFramework() 
    : systemInitialized(false),
      lastHealthCheck(0),
      server(nullptr),
      webConfig(nullptr),
      network(nullptr),
      ota(nullptr),
      taskManager(nullptr),
      healthMonitor(nullptr),
      userManager(nullptr),
      authManager(nullptr),
      protocolManager(nullptr) {
    
    // 设置单例实例
    if (instance == nullptr) {
        instance = this;
    }
}

// 析构函数
FastBeeFramework::~FastBeeFramework() {
    shutdown();
    
    // 清理子系统，按创建顺序反向清理
    if (protocolManager) {
        delete protocolManager;
        protocolManager = nullptr;
    }
    
    if (authManager) {
        delete authManager;
        authManager = nullptr;
    }
    
    if (userManager) {
        delete userManager;
        userManager = nullptr;
    }
    
    if (healthMonitor) {
        delete healthMonitor;
        healthMonitor = nullptr;
    }
    
    if (taskManager) {
        delete taskManager;
        taskManager = nullptr;
    }
    
    if (ota) {
        delete ota;
        ota = nullptr;
    }
    
    if (webConfig) {
        delete webConfig;
        webConfig = nullptr;
    }
    
    if (network) {
        delete network;
        network = nullptr;
    }
    
    // 最后删除server，因为其他管理器可能还在使用
    if (server) {
        delete server;
        server = nullptr;
    }
    
    // 清理单例实例
    if (instance == this) {
        instance = nullptr;
    }
}

// 获取单例实例
FastBeeFramework* FastBeeFramework::getInstance() {
    if (instance == nullptr) {
        instance = new FastBeeFramework();
    }
    return instance;
}

// 初始化框架
bool FastBeeFramework::initialize() {
    if (systemInitialized) {
        LOG_WARNING("Framework already initialized");
        return true;
    }
    Serial.println("Initializing FastBee IoT Platform...");
    
    // 步骤1: 初始化配置存储
    if (!ConfigStorage::initialize()) {
        Serial.println("Failed to initialize config storage!");
        return false;
    }
    Serial.println("Config storage system initialized");

    // 初始化文件系统
    FileUtils::initialize();

    // 列出文件系统内容（调试用）
    FileUtils::listAllFiles("/");
    
    // 步骤2: 初始化日志系统
    if (!LOGGER.initialize()) {
        Serial.println("Failed to initialize logger system!");
        return false;
    }
    LOGGER.setLogLevel(LOG_DEBUG);
    LOG_INFO("Logger system initialized");
    
    // 步骤3: 创建HTTP服务器
    server = new AsyncWebServer(80);
    if (!server) {
        LOG_ERROR("Failed to create HTTP server");
        return false;
    }
    LOG_INFO("HTTP server created");
    
    // 步骤4: 初始化网络管理器
    network = new NetworkManager(server);
    if (!network || !network->initialize()) {
        LOG_ERROR("Failed to initialize network manager");
        return false;
    }
    LOG_INFO("Network manager initialized");
    
    // 步骤5: 初始化Web配置管理器
    webConfig = new WebConfigManager(server);
    if (!webConfig || !webConfig->initialize()) {
        LOG_ERROR("Failed to initialize web config manager");
        return false;
    }
    LOG_INFO("Web config manager initialized");
    
    // 步骤6: 初始化认证管理器
    authManager = new AuthManager();
    if (!authManager || !authManager->initialize()) {
        LOG_ERROR("Failed to initialize auth manager!");
        return false;
    }
    LOG_INFO("Auth manager initialized");
    
    // 步骤7: 初始化OTA管理器
    ota = new OTAManager(server);
    if (!ota || !ota->initialize()) {
        LOG_ERROR("Failed to initialize OTA manager");
        return false;
    }
    LOG_INFO("OTA manager initialized");
    
    // 步骤8: 初始化任务管理器
    taskManager = new TaskManager();
    if (!taskManager || !taskManager->initialize()) {
        LOG_ERROR("Failed to initialize task manager");
        return false;
    }
    LOG_INFO("Task manager initialized");
    
    // 步骤9: 初始化健康监控器
    healthMonitor = new HealthMonitor();
    if (!healthMonitor || !healthMonitor->initialize()) {
        LOG_ERROR("Failed to initialize health monitor");
        return false;
    }
    LOG_INFO("Health monitor initialized");
    
    // 步骤10: 初始化用户管理器
    userManager = new UserManager();
    if (!userManager || !userManager->initialize()) {
        LOG_ERROR("Failed to initialize user manager");
        return false;
    }
    LOG_INFO("User manager initialized");
    
    // 步骤11: 初始化协议管理器
    protocolManager = new ProtocolManager();
    if (!protocolManager || !protocolManager->initialize()) {
        LOG_ERROR("Failed to initialize protocol manager");
        return false;
    }
    LOG_INFO("Protocol manager initialized");
    
    // 步骤12: 添加系统任务
    if (!addSystemTasks()) {
        LOG_WARNING("Failed to add some system tasks");
    }
    
    // 步骤13: 检查系统健康状态
    if (!healthMonitor->isSystemHealthy()) {
        LOG_WARNING("System health check initial warning: " + healthMonitor->getHealthReport());
    }
    
    systemInitialized = true;
    LOG_INFO("FastBee IoT Platform initialized successfully");
    LOG_INFO("Device ready for operation");
    
    return true;
}

// 添加系统任务
bool FastBeeFramework::addSystemTasks() {
    if (!taskManager) {
        LOG_ERROR("Task manager not initialized");
        return false;
    }
    
    // 健康检查任务（每30秒）
    if (!taskManager->addTask("health_check", [](void* param) {
        FastBeeFramework* framework = (FastBeeFramework*)param;
        if (framework && framework->healthMonitor) {
            framework->healthMonitor->update();
            
            // 定期报告健康状态
            static unsigned long lastReport = 0;
            unsigned long now = millis();
            if (now - lastReport > 300000) { // 每5分钟报告一次
                if (!framework->healthMonitor->isSystemHealthy()) {
                    LOG_WARNING("System health check: " + framework->healthMonitor->getHealthReport());
                } else {
                    LOG_DEBUG("System health check: OK");
                }
                lastReport = now;
            }
        }else{
            LOG_DEBUG("framework或者healthMonitor不存在");
        }
    }, this, 30000)) {
        LOG_ERROR("Failed to add health check task");
        return false;
    }
    
    // Web客户端处理任务（每100ms）
    if (!taskManager->addTask("web_client_handler", [](void* param) {
        // 空任务，由AsyncWebServer自动处理
        // 这里可以添加额外的Web客户端处理逻辑
    }, nullptr, 100)) {
        LOG_WARNING("Failed to add web client handler task");
        // 这个任务不是关键的，所以不返回错误
    }
    
    // 网络状态更新任务（每5秒）
    if (!taskManager->addTask("network_update", [](void* param) {
        FastBeeFramework* framework = (FastBeeFramework*)param;
        if (framework && framework->network) {
            framework->network->update();
        }
    }, this, 5000)) {
        LOG_WARNING("Failed to add network update task");
    }
    
    // 内存监控任务（每60秒）
    if (!taskManager->addTask("memory_monitor", [](void* param) {
        static uint32_t lastHeap = 0;
        uint32_t currentHeap = ESP.getFreeHeap();
        
        if (lastHeap > 0) {
            int32_t diff = currentHeap - lastHeap;
            if (abs(diff) > 1024) { // 变化超过1KB时记录
                LOG_DEBUG("Memory: " + String(currentHeap / 1024) + "KB free" + 
                         (diff > 0 ? " (+" : " (") + String(diff) + " bytes)");
            }
        }
        lastHeap = currentHeap;
    }, nullptr, 60000)) {
        LOG_WARNING("Failed to add memory monitor task");
    }
    
    LOG_INFO("System tasks added");
    return true;
}

// 运行框架主循环
void FastBeeFramework::run() {
    if (!systemInitialized) {
        LOG_ERROR("Framework not initialized, cannot run");
        return;
    }
    
    // 执行任务调度
    if (taskManager) {
        taskManager->run();
    }
    
    // 定期健康检查（除了任务调度中的检查外）
    unsigned long currentTime = millis();
    if (currentTime - lastHealthCheck > 60000) { // 每分钟额外检查一次
        if (healthMonitor) {
            SystemHealth health = healthMonitor->getHealthStatus();
            if (!healthMonitor->isSystemHealthy()) {
                LOG_WARNING("Periodic health check: " + healthMonitor->getHealthReport());
            }
        }
        lastHealthCheck = currentTime;
    }
    
    // 检查是否需要重启（例如在OTA更新后）
    checkForRestart();
}

// 检查是否需要重启
void FastBeeFramework::checkForRestart() {
    // 这里可以添加重启条件检查
    // 例如：内存过低、运行时间过长等
    static unsigned long lastRestartCheck = 0;
    unsigned long currentTime = millis();
    
    if (currentTime - lastRestartCheck > 3600000) { // 每小时检查一次
        uint32_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < 10240) { // 如果可用内存低于10KB
            LOG_ERROR("Low memory detected: " + String(freeHeap) + " bytes free, restarting...");
            delay(1000);
            ESP.restart();
        }
        lastRestartCheck = currentTime;
    }
}

// 关闭框架
void FastBeeFramework::shutdown() {
    if (!systemInitialized) {
        return;
    }
    
    LOG_INFO("Shutting down FastBee IoT Platform...");
    
    // 停止所有任务
    if (taskManager) {
        taskManager->stopAllTasks();
    }
    
    // 关闭各个子系统（按依赖顺序反向关闭）
    if (protocolManager) {
        protocolManager->shutdown();
    }
    
    if (userManager) {
        userManager->saveUsersToConfig();
    }
    
    if (authManager) {
        // AuthManager 没有 shutdown 方法，需要实现或忽略
        LOG_WARNING("AuthManager::shutdown() not implemented");
    }
    
    if (webConfig) {
        webConfig->shutdown();
    }
    
    if (ota) {
        // OTA管理器没有shutdown方法
    }
    
    if (network) {
        network->disconnect();
    }
    
    if (healthMonitor) {
        // 健康监控器没有shutdown方法
    }
    
    // 停止HTTP服务器
    if (server) {
        // AsyncWebServer没有stop方法，我们只需要确保不再处理请求
    }
    
    systemInitialized = false;
    LOG_INFO("FastBee IoT Platform shutdown complete");
}

// 获取子系统指针
NetworkManager* FastBeeFramework::getNetworkManager() const { return network; }
WebConfigManager* FastBeeFramework::getWebConfigManager() const { return webConfig; }
OTAManager* FastBeeFramework::getOTAManager() const { return ota; }
TaskManager* FastBeeFramework::getTaskManager() const { return taskManager; }
HealthMonitor* FastBeeFramework::getHealthMonitor() const { return healthMonitor; }
UserManager* FastBeeFramework::getUserManager() const { return userManager; }
AuthManager* FastBeeFramework::getAuthManager() const { return authManager; }
ProtocolManager* FastBeeFramework::getProtocolManager() const { return protocolManager; }

// 检查系统是否已初始化
bool FastBeeFramework::isInitialized() const {
    return systemInitialized;
}

// 获取系统运行时间
unsigned long FastBeeFramework::getUptime() const {
    return millis();
}