#ifndef WEB_CONFIG_MANAGER_H
#define WEB_CONFIG_MANAGER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>

// JSON 文档大小限制（优化内存使用）
#define JSON_DOC_SMALL  512    // 小型响应
#define JSON_DOC_MEDIUM 2048   // 中型响应
#define JSON_DOC_LARGE  4096   // 大型响应（列表数据）

// 接口包含
#include "core/interfaces/IAuthManager.h"
#include "core/interfaces/IUserManager.h"

// 前向声明
class AuthManager;
class UserManager;
class RoleManager;
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
                    IAuthManager* authMgr, IUserManager* userMgr);
    
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
     * @brief 设置角色管理器
     * @param roleMgr 角色管理器指针
     */
    void setRoleManager(RoleManager* roleMgr);

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
    AsyncWebServer* server;           ///< 异步Web服务器
    IAuthManager*   authManager;      ///< 认证管理器
    IUserManager*   userManager;      ///< 用户管理器
    RoleManager*    roleManager;      ///< 角色管理器
    NetworkManager* networkManager;   ///< 网络管理器
    OTAManager*     otaManager;       ///< OTA管理器
    ProtocolManager* protocolManager; ///< 协议管理器

    Preferences preferences;          ///< 偏好设置
    bool isRunning;                   ///< 服务器运行状态
    String webRootPath;               ///< Web根目录路径
    
    // ============ 配置加载 ============
    void loadConfiguration();
    
    // ============ 路由设置 ============
    void setupStaticRoutes();
    void setupAuthRoutes();
    void setupUserRoutes();
    void setupRoleRoutes();     ///< 角色管理 API 路由
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
    void sendBuiltinLoginPage(AsyncWebServerRequest* request);
    void sendBuiltinDashboard(AsyncWebServerRequest* request);
    void sendBuiltinUsersPage(AsyncWebServerRequest* request);
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
    void handleSetupPage(AsyncWebServerRequest* request);
    
    // ============ WiFi配置API ============
    void handleAPIWiFiScan(AsyncWebServerRequest* request);
    void handleAPIWiFiConnect(AsyncWebServerRequest* request);
    void sendBuiltinSetupPage(AsyncWebServerRequest* request);
    
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
    void handleAPIUpdateUserByPost(AsyncWebServerRequest* request);
    void handleAPIDeleteUser(AsyncWebServerRequest* request);
    void handleAPIDeleteUserByPost(AsyncWebServerRequest* request);
    void handleAPIGetOnlineUsers(AsyncWebServerRequest* request);
    void handleAPIAssignUserRole(AsyncWebServerRequest* request);   ///< 为用户赋予角色
    void handleAPIRevokeUserRole(AsyncWebServerRequest* request);   ///< 撤销用户角色
    void handleAPIUpdateUserMeta(AsyncWebServerRequest* request);   ///< 更新用户元数据

    // ============ 角色管理API处理器 ============
    void handleAPIGetRoles(AsyncWebServerRequest* request);
    void handleAPIGetRole(AsyncWebServerRequest* request);
    void handleAPICreateRole(AsyncWebServerRequest* request);
    void handleAPIUpdateRole(AsyncWebServerRequest* request);
    void handleAPIDeleteRole(AsyncWebServerRequest* request);
    void handleAPIGetRolePermissions(AsyncWebServerRequest* request);
    void handleAPISetRolePermissions(AsyncWebServerRequest* request);
    void handleAPIUpdateRoleByPost(AsyncWebServerRequest* request);      ///< 更新角色(POST)
    void handleAPIDeleteRoleByPost(AsyncWebServerRequest* request);      ///< 删除角色(POST)
    void handleAPISetRolePermissionsByPost(AsyncWebServerRequest* request); ///< 设置角色权限(POST)
    void handleAPIGetPermissions(AsyncWebServerRequest* request);   ///< 获取所有权限定义

    // ============ 审计日志API处理器 ============
    void handleAPIGetAuditLog(AsyncWebServerRequest* request);
    void handleAPIClearAuditLog(AsyncWebServerRequest* request);
    
    // ============ 系统API处理器 ============
    void handleAPISystemInfo(AsyncWebServerRequest* request);
    void handleAPISystemStatus(AsyncWebServerRequest* request);
    void handleAPISystemRestart(AsyncWebServerRequest* request);
    void handleAPIFactoryReset(AsyncWebServerRequest* request);

    // 辅助函数
    String formatUptime(unsigned long ms);
    void handleAPIFileSystemInfo(AsyncWebServerRequest* request);
    void handleAPIHealthCheck(AsyncWebServerRequest* request);
    
    // ============ 文件管理 API 处理器 ============
    void handleAPIGetFileList(AsyncWebServerRequest* request);   ///< 获取文件列表
    void handleAPIGetFileContent(AsyncWebServerRequest* request); ///< 获取文件内容
    void handleAPISaveFileContent(AsyncWebServerRequest* request); ///< 保存文件内容    
    // ============ 日志管理 API 处理器 ============
    void handleAPIGetLogs(AsyncWebServerRequest* request);       ///< 获取日志内容
    void handleAPIGetLogInfo(AsyncWebServerRequest* request);    ///< 获取日志信息
    void handleAPIClearLogs(AsyncWebServerRequest* request);     ///< 清空日志
    
    // ============ 配置API处理器 ============
    void handleAPIGetConfig(AsyncWebServerRequest* request);
    void handleAPIUpdateConfig(AsyncWebServerRequest* request);
    void handleAPIGetNetworkConfig(AsyncWebServerRequest* request);
    void handleAPIUpdateNetworkConfig(AsyncWebServerRequest* request, JsonVariant& json);
    void handleAPIGetNetworkStatus(AsyncWebServerRequest* request);  ///< 获取网络实时状态
    
    // ============ OTA API处理器 ============
    void handleAPIOtaUpdate(AsyncWebServerRequest* request);
    void handleAPIOtaStatus(AsyncWebServerRequest* request);
    void handleAPIOtaUrl(AsyncWebServerRequest* request);     ///< 通过URL在线升级
    void handleAPIOtaUpload(AsyncWebServerRequest* request, const String& filename, 
                           size_t index, uint8_t* data, size_t len, bool final);  ///< 本地文件上传
    
    // ============ GPIO API处理器 (兼容旧版) ============
    void handleAPIGetGPIOConfig(AsyncWebServerRequest* request);   ///< 获取GPIO配置
    void handleAPIConfigureGPIO(AsyncWebServerRequest* request);   ///< 配置GPIO
    void handleAPIReadGPIO(AsyncWebServerRequest* request);        ///< 读取GPIO状态
    void handleAPIWriteGPIO(AsyncWebServerRequest* request);       ///< 写入GPIO状态
    void handleAPIDeleteGPIO(AsyncWebServerRequest* request);      ///< 删除GPIO配置
    void handleAPISaveGPIOConfig(AsyncWebServerRequest* request);  ///< 保存GPIO配置
    
    // ============ 外设接口 API处理器 ============
    void handleAPIGetPeripherals(AsyncWebServerRequest* request);      ///< 获取所有外设
    void handleAPIGetPeripheral(AsyncWebServerRequest* request);       ///< 获取单个外设
    void handleAPIAddPeripheral(AsyncWebServerRequest* request);       ///< 添加外设
    void handleAPIUpdatePeripheral(AsyncWebServerRequest* request);    ///< 更新外设
    void handleAPIDeletePeripheral(AsyncWebServerRequest* request);    ///< 删除外设
    void handleAPIEnablePeripheral(AsyncWebServerRequest* request);    ///< 启用外设
    void handleAPIDisablePeripheral(AsyncWebServerRequest* request);   ///< 禁用外设
    void handleAPIGetPeripheralStatus(AsyncWebServerRequest* request); ///< 获取外设状态
    void handleAPIReadPeripheral(AsyncWebServerRequest* request);      ///< 读取外设数据
    void handleAPIWritePeripheral(AsyncWebServerRequest* request);     ///< 写入外设数据
    void handleAPIGetPeripheralTypes(AsyncWebServerRequest* request);  ///< 获取外设类型列表
    
    // ============ 协议配置 API ============
    void handleAPIGetProtocolConfig(AsyncWebServerRequest* request);   ///< 获取协议配置
    void handleAPISaveProtocolConfig(AsyncWebServerRequest* request);  ///< 保存协议配置
    
    // ============ 额外工具方法 ============
    void handleResetPassword(AsyncWebServerRequest* request);
    void handleUnlockAccount(AsyncWebServerRequest* request);
    
    // ============ 设备配置 API 处理器 ============
    void handleAPIGetDeviceConfig(AsyncWebServerRequest* request);   ///< 获取设备配置(NTP/时区等)
    void handleAPIUpdateDeviceConfig(AsyncWebServerRequest* request); ///< 保存设备配置
    void handleAPIGetDeviceTime(AsyncWebServerRequest* request);      ///< 获取当前时间
    void handleAPISyncDeviceTime(AsyncWebServerRequest* request);     ///< 触发NTP时间同步并返回时间
    
    // ============ AP配网 API 处理器 ============
    void handleAPIGetProvisionStatus(AsyncWebServerRequest* request);  ///< 获取配网状态
    void handleAPIStartProvision(AsyncWebServerRequest* request);      ///< 启动AP配网
    void handleAPIStopProvision(AsyncWebServerRequest* request);       ///< 停止AP配网
    void handleAPIGetProvisionConfig(AsyncWebServerRequest* request);  ///< 获取配网配置
    void handleAPISaveProvisionConfig(AsyncWebServerRequest* request); ///< 保存配网配置
    void handleAPIProvisionCallback(AsyncWebServerRequest* request);   ///< 配网回调(接收WiFi配置)
    
    // ============ 蓝牙配网 API 处理器 ============
    void handleAPIGetBLEProvisionConfig(AsyncWebServerRequest* request);  ///< 获取蓝牙配网配置
    void handleAPISaveBLEProvisionConfig(AsyncWebServerRequest* request); ///< 保存蓝牙配网配置
    void handleAPIGetBLEProvisionStatus(AsyncWebServerRequest* request);  ///< 获取蓝牙配网状态
    void handleAPIStartBLEProvision(AsyncWebServerRequest* request);      ///< 启动蓝牙配网
    void handleAPIStopBLEProvision(AsyncWebServerRequest* request);       ///< 停止蓝牙配网
    
    // ============ 请求体处理回调 ============
    // 用于处理OTA上传等需要处理请求体的场景
    void onUpload(AsyncWebServerRequest* request, String filename, 
                 size_t index, uint8_t* data, size_t len, bool final);
};

#endif