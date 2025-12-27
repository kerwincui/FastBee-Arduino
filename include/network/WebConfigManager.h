#ifndef WEB_CONFIG_MANAGER_H
#define WEB_CONFIG_MANAGER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "./security/AuthManager.h"
#include "./security/UserManager.h"

// 前向声明
class NetworkManager;
class OTAManager;
class ProtocolManager;

/**
 * @brief Web配置管理器类
 * 负责提供Web配置界面和RESTful API接口
 */
class WebConfigManager {
private:
    // HTTP服务器
    AsyncWebServer* server;
    
    // 依赖组件
    AuthManager* authManager;
    UserManager* userManager;
    NetworkManager* networkManager;
    OTAManager* otaManager;
    ProtocolManager* protocolManager;
    
    // 配置
    Preferences preferences;
    bool isRunning;
    
    // 文件系统路径
    String webRootPath;
    
    // ============ 私有方法 ============
    
    // 初始化方法
    void setupStaticRoutes();
    void setupAPIRoutes();
    void setupAuthRoutes();
    void setupUserRoutes();
    void setupSystemRoutes();
    void loadConfiguration();
    
    // 认证辅助方法
    bool requiresAuth(AsyncWebServerRequest* request);
    AuthResult authenticateRequest(AsyncWebServerRequest* request);
    String getClientIP(AsyncWebServerRequest* request);
    String getUserAgent(AsyncWebServerRequest* request);
    bool checkPermission(AsyncWebServerRequest* request, const String& permission);
    
    // 响应辅助方法
    void sendJsonResponse(AsyncWebServerRequest* request, int code, 
                         const JsonDocument& doc);
    void sendSuccess(AsyncWebServerRequest* request, const JsonDocument& data = JsonDocument());
    void sendError(AsyncWebServerRequest* request, int code, const String& message);
    void sendUnauthorized(AsyncWebServerRequest* request);
    void sendForbidden(AsyncWebServerRequest* request);
    void sendNotFound(AsyncWebServerRequest* request);
    
    // 文件服务方法
    bool serveStaticFile(AsyncWebServerRequest* request, const String& path);
    String getContentType(const String& filename);
    String readFile(const String& path);
    bool fileExists(const String& path);
    
    // JSON解析
    bool parseJsonBody(AsyncWebServerRequest* request, JsonDocument& doc);
    
    // ============ 请求处理器 ============
    
    // 静态页面
    void handleRoot(AsyncWebServerRequest* request);
    void handleLoginPage(AsyncWebServerRequest* request);
    void handleDashboard(AsyncWebServerRequest* request);
    void handleUsersPage(AsyncWebServerRequest* request);
    void handleSettingsPage(AsyncWebServerRequest* request);
    void handleMonitorPage(AsyncWebServerRequest* request);
    
    // 认证API
    void handleAPILogin(AsyncWebServerRequest* request);
    void handleAPILogout(AsyncWebServerRequest* request);
    void handleAPIVerifySession(AsyncWebServerRequest* request);
    void handleAPIChangePassword(AsyncWebServerRequest* request);
    
    // 用户管理API
    void handleAPIGetUsers(AsyncWebServerRequest* request);
    void handleAPIGetUser(AsyncWebServerRequest* request);
    void handleAPIAddUser(AsyncWebServerRequest* request);
    void handleAPIUpdateUser(AsyncWebServerRequest* request);
    void handleAPIDeleteUser(AsyncWebServerRequest* request);
    void handleAPIGetOnlineUsers(AsyncWebServerRequest* request);
    
    // 系统API
    void handleAPISystemInfo(AsyncWebServerRequest* request);
    void handleAPISystemStatus(AsyncWebServerRequest* request);
    void handleAPISystemRestart(AsyncWebServerRequest* request);
    void handleAPIFileSystemInfo(AsyncWebServerRequest* request);
    void handleAPIHealthCheck(AsyncWebServerRequest* request);
    
    // 配置API
    void handleAPIGetConfig(AsyncWebServerRequest* request);
    void handleAPIUpdateConfig(AsyncWebServerRequest* request);
    void handleAPIGetNetworkConfig(AsyncWebServerRequest* request);
    void handleAPIUpdateNetworkConfig(AsyncWebServerRequest* request);
    
    // OTA API
    void handleAPIOtaUpdate(AsyncWebServerRequest* request);
    void handleAPIOtaStatus(AsyncWebServerRequest* request);
    
public:
    /**
     * @brief 构造函数
     * @param webServer HTTP服务器
     * @param authMgr 认证管理器
     * @param userMgr 用户管理器
     */
    WebConfigManager(AsyncWebServer* webServer, AuthManager* authMgr, UserManager* userMgr);
    
    /**
     * @brief 析构函数
     */
    ~WebConfigManager();
    
    /**
     * @brief 初始化Web配置管理器
     * @return 是否成功
     */
    bool initialize();
    
    /**
     * @brief 启动Web服务器
     * @return 是否成功
     */
    bool start();
    
    /**
     * @brief 停止Web服务器
     */
    void stop();
    
    /**
     * @brief 检查服务器是否运行
     * @return 是否运行
     */
    bool isServerRunning() const;
    
    /**
     * @brief 设置网络管理器
     * @param netMgr 网络管理器
     */
    void setNetworkManager(NetworkManager* netMgr);
    
    /**
     * @brief 设置OTA管理器
     * @param otaMgr OTA管理器
     */
    void setOTAManager(OTAManager* otaMgr);
    
    /**
     * @brief 设置协议管理器
     * @param protoMgr 协议管理器
     */
    void setProtocolManager(ProtocolManager* protoMgr);
    
    /**
     * @brief 获取Web服务器
     * @return Web服务器指针
     */
    AsyncWebServer* getWebServer() const;
    
    /**
     * @brief 执行维护任务
     */
    void performMaintenance();
};

#endif