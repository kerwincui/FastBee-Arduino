#ifndef WEB_CONFIG_MANAGER_H
#define WEB_CONFIG_MANAGER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>

// 前向声明
class AuthManager;
class UserManager;
class NetworkManager;
class OTAManager;
class ProtocolManager;
struct AuthResult;
struct User;
struct UserSession;
struct NetworkConfig;

/**
 * @brief Web配置管理器类
 * 负责提供Web配置界面和RESTful API接口
 */
/**
 * @brief Web配置管理器类
 * 
 * 负责管理Web服务器、路由配置、认证检查和API处理
 * 所有API使用URL参数或表单参数传递数据，不使用JSON body
 */
class WebConfigManager {
public:
    /**
     * @brief 构造函数
     * @param webServer 异步Web服务器指针
     * @param authMgr 认证管理器指针
     * @param userMgr 用户管理器指针
     */
    WebConfigManager(AsyncWebServer* webServer, 
                    AuthManager* authMgr, UserManager* userMgr);
    
    /**
     * @brief 析构函数
     */
    ~WebConfigManager();
    
    // 删除拷贝构造函数和赋值运算符
    WebConfigManager(const WebConfigManager&) = delete;
    WebConfigManager& operator=(const WebConfigManager&) = delete;
    
    /**
     * @brief 初始化Web配置管理器
     * @return true 初始化成功
     * @return false 初始化失败
     */
    bool initialize();
    
    /**
     * @brief 启动Web服务器
     * @return true 启动成功
     * @return false 启动失败
     */
    bool start();
    
    /**
     * @brief 停止Web服务器
     */
    void stop();
    
    /**
     * @brief 检查Web服务器是否正在运行
     * @return true 正在运行
     * @return false 未运行
     */
    bool isServerRunning() const;
    
    /**
     * @brief 设置网络管理器
     * @param netMgr 网络管理器指针
     */
    void setNetworkManager(NetworkManager* netMgr);
    
    /**
     * @brief 设置OTA管理器
     * @param otaMgr OTA管理器指针
     */
    void setOTAManager(OTAManager* otaMgr);
    
    /**
     * @brief 设置协议管理器
     * @param protoMgr 协议管理器指针
     */
    void setProtocolManager(ProtocolManager* protoMgr);
    
    /**
     * @brief 获取Web服务器实例
     * @return AsyncWebServer* Web服务器指针
     */
    AsyncWebServer* getWebServer() const;
    
    /**
     * @brief 执行维护任务
     */
    void performMaintenance();

private:
    AsyncWebServer* server;          ///< 异步Web服务器
    AuthManager* authManager;        ///< 认证管理器
    UserManager* userManager;        ///< 用户管理器
    NetworkManager* networkManager;  ///< 网络管理器
    OTAManager* otaManager;          ///< OTA管理器
    ProtocolManager* protocolManager; ///< 协议管理器
    
    Preferences preferences;         ///< 偏好设置
    bool isRunning;                  ///< 服务器运行状态
    String webRootPath;              ///< Web根目录路径
    
    // ============ 配置加载 ============
    void loadConfiguration();
    
    // ============ 路由设置 ============
    void setupStaticRoutes();
    void setupAuthRoutes();
    void setupUserRoutes();
    void setupSystemRoutes();
    void setupAPIRoutes();
    
    // ============ 参数处理辅助方法 ============
    String getParamValue(AsyncWebServerRequest* request, const String& paramName, 
                        const String& defaultValue = "");
    bool getParamBool(AsyncWebServerRequest* request, const String& paramName, 
                     bool defaultValue = false);
    int getParamInt(AsyncWebServerRequest* request, const String& paramName, 
                   int defaultValue = 0);
    
    // ============ 认证辅助方法 ============
    bool requiresAuth(AsyncWebServerRequest* request);
    AuthResult authenticateRequest(AsyncWebServerRequest* request);
    String getClientIP(AsyncWebServerRequest* request);
    String getUserAgent(AsyncWebServerRequest* request);
    bool checkPermission(AsyncWebServerRequest* request, const String& permission);
    
    // ============ 响应辅助方法 ============
    void sendJsonResponse(AsyncWebServerRequest* request, int code, 
                         const JsonDocument& doc);
    void sendSuccess(AsyncWebServerRequest* request, const JsonDocument& data);
    void sendSuccess(AsyncWebServerRequest* request, const String& message = "");
    void sendError(AsyncWebServerRequest* request, int code, const String& message);
    void sendUnauthorized(AsyncWebServerRequest* request);
    void sendForbidden(AsyncWebServerRequest* request);
    void sendNotFound(AsyncWebServerRequest* request);
    void sendBadRequest(AsyncWebServerRequest* request, const String& message);
    
    // ============ 文件服务方法 ============
    bool serveStaticFile(AsyncWebServerRequest* request, const String& path);
    String getContentType(const String& filename);
    String readFile(const String& path);
    bool fileExists(const String& path);
    
    // ============ 页面处理器 ============
    void handleRoot(AsyncWebServerRequest* request);
    void handleLoginPage(AsyncWebServerRequest* request);
    void handleDashboard(AsyncWebServerRequest* request);
    void handleUsersPage(AsyncWebServerRequest* request);
    void handleSettingsPage(AsyncWebServerRequest* request);
    void handleMonitorPage(AsyncWebServerRequest* request);
    
    // ============ 认证API处理器 ============
    void handleAPILogin(AsyncWebServerRequest* request);
    void handleAPILogout(AsyncWebServerRequest* request);
    void handleAPIVerifySession(AsyncWebServerRequest* request);
    void handleAPIChangePassword(AsyncWebServerRequest* request);
    
    // ============ 用户管理API处理器 ============
    void handleAPIGetUsers(AsyncWebServerRequest* request);
    void handleAPIGetUser(AsyncWebServerRequest* request);
    void handleAPIAddUser(AsyncWebServerRequest* request);
    void handleAPIUpdateUser(AsyncWebServerRequest* request);
    void handleAPIDeleteUser(AsyncWebServerRequest* request);
    void handleAPIGetOnlineUsers(AsyncWebServerRequest* request);
    
    // ============ 系统API处理器 ============
    void handleAPISystemInfo(AsyncWebServerRequest* request);
    void handleAPISystemStatus(AsyncWebServerRequest* request);
    void handleAPISystemRestart(AsyncWebServerRequest* request);
    void handleAPIFileSystemInfo(AsyncWebServerRequest* request);
    void handleAPIHealthCheck(AsyncWebServerRequest* request);
    
    // ============ 配置API处理器 ============
    void handleAPIGetConfig(AsyncWebServerRequest* request);
    void handleAPIUpdateConfig(AsyncWebServerRequest* request);
    void handleAPIGetNetworkConfig(AsyncWebServerRequest* request);
    void handleAPIUpdateNetworkConfig(AsyncWebServerRequest* request);
    
    // ============ OTA API处理器 ============
    void handleAPIOtaUpdate(AsyncWebServerRequest* request);
    void handleAPIOtaStatus(AsyncWebServerRequest* request);
    
    // ============ 额外工具方法 ============
    void handleResetPassword(AsyncWebServerRequest* request);
    void handleUnlockAccount(AsyncWebServerRequest* request);
    
    // ============ 请求体处理回调 ============
    // 用于处理OTA上传等需要处理请求体的场景
    void onUpload(AsyncWebServerRequest* request, String filename, 
                 size_t index, uint8_t* data, size_t len, bool final);
};

#endif