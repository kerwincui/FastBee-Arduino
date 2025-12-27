#ifndef AUTH_MANAGER_H
#define AUTH_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <map>
#include <vector>
#include <functional>
#include "UserManager.h"
#include <ESPAsyncWebServer.h>
#include <MD5Builder.h>

// 前向声明
class LoggerSystem;

/**
 * @brief 会话状态枚举
 */
enum class SessionStatus {
    VALID = 0,      // 会话有效
    EXPIRED = 1,    // 会话过期
    INVALID = 2,    // 会话无效
    LOCKED = 3      // 账户锁定
};

/**
 * @brief 用户会话结构体
 */
struct UserSession {
    String sessionId;
    String username;
    UserRole role;
    unsigned long loginTime;
    unsigned long lastAccessTime;
    unsigned long expireTime;
    String ipAddress;
    String userAgent;
    bool isActive;
    
    UserSession() : role(UserRole::VIEWER), loginTime(0), 
                   lastAccessTime(0), expireTime(0), isActive(false) {}
};

/**
 * @brief 权限定义结构体
 */
struct Permission {
    String name;
    String description;
    UserRole minRole;
    std::vector<String> allowedMethods; // HTTP方法
    
    Permission() : minRole(UserRole::VIEWER) {}
    Permission(const String& n, const String& d, UserRole r) 
        : name(n), description(d), minRole(r) {}
};

/**
 * @brief 认证结果结构体
 */
struct AuthResult {
    bool success;
    String message;
    String sessionId;
    SessionStatus status;
    UserRole userRole;
    String username;
    
    AuthResult() : success(false), status(SessionStatus::INVALID), 
                  userRole(UserRole::VIEWER) {}
};

/**
 * @brief 安全配置结构体
 */
struct SecurityConfig {
    uint32_t sessionTimeout = 3600000; // 1小时
    uint32_t sessionCleanupInterval = 60000; // 1分钟
    bool enableSessionPersistence = true;
    bool enableIpWhitelist = false;
    std::vector<String> ipWhitelist;
    
    // Cookie配置
    String cookieName = "session";
    uint32_t cookieMaxAge = 3600; // 1小时
    bool cookieHttpOnly = true;
    bool cookieSecure = false;
};

/**
 * @brief 认证事件回调函数类型
 */
typedef std::function<void(const String& event, const String& username, 
                          const String& details, bool success)> AuthEventCallback;

/**
 * @brief 认证管理器类
 * 负责用户认证、会话管理和权限验证
 */
class AuthManager {
private:
    // 配置和状态
    SecurityConfig securityConfig;
    Preferences preferences;
    
    // 依赖组件
    UserManager* userManager;
    LoggerSystem* logger;
    
    // 会话管理
    std::map<String, UserSession> activeSessions;
    std::map<String, unsigned long> lockedAccounts;
    unsigned long lastSessionCleanup;
    
    // 权限定义
    std::map<String, Permission> permissions;
    
    // 事件回调
    AuthEventCallback authEventCallback;
    
    // 私有方法
    void initializePermissions();
    String generateSessionId(const String& username);
    SessionStatus validateSession(const UserSession& session);
    void cleanupExpiredSessions();
    bool isAccountLocked(const String& username);
    void lockAccount(const String& username);
    void unlockAccount(const String& username);
    void logAuthEvent(const String& event, const String& username, 
                     const String& details, bool success = true);
    bool loadSecurityConfig();
    bool saveSecurityConfig();
    bool checkIpWhitelist(const String& ipAddress);
    
public:
    /**
     * @brief 构造函数
     * @param userMgr 用户管理器
     * @param loggerPtr 日志系统
     */
    AuthManager(UserManager* userMgr = nullptr, LoggerSystem* loggerPtr = nullptr);
    
    /**
     * @brief 析构函数
     */
    ~AuthManager();
    
    /**
     * @brief 初始化认证管理器
     * @return 是否成功
     */
    bool initialize();
    
    // ============ 认证管理 ============
    
    /**
     * @brief 用户登录
     * @param username 用户名
     * @param password 密码
     * @param ipAddress IP地址
     * @param userAgent 用户代理
     * @return 认证结果
     */
    AuthResult login(const String& username, const String& password,
                    const String& ipAddress = "", const String& userAgent = "");
    
    /**
     * @brief 验证会话
     * @param sessionId 会话ID
     * @param updateAccessTime 是否更新访问时间
     * @return 认证结果
     */
    AuthResult verifySession(const String& sessionId, bool updateAccessTime = true);
    
    /**
     * @brief 用户登出
     * @param sessionId 会话ID
     * @return 是否成功
     */
    bool logout(const String& sessionId);
    
    /**
     * @brief 强制登出用户
     * @param username 用户名
     * @return 是否成功
     */
    bool forceLogout(const String& username);
    
    /**
     * @brief 强制登出所有会话
     * @return 登出的会话数量
     */
    size_t forceLogoutAll();
    
    // ============ 会话管理 ============
    
    /**
     * @brief 获取会话信息
     * @param sessionId 会话ID
     * @return 会话信息
     */
    UserSession getSessionInfo(const String& sessionId);
    
    /**
     * @brief 获取所有活跃会话
     * @return 会话列表
     */
    std::vector<UserSession> getActiveSessions();
    
    /**
     * @brief 获取用户的所有会话
     * @param username 用户名
     * @return 会话列表
     */
    std::vector<UserSession> getUserSessions(const String& username);
    
    /**
     * @brief 更新会话访问时间
     * @param sessionId 会话ID
     * @return 是否成功
     */
    bool updateSessionAccessTime(const String& sessionId);
    
    /**
     * @brief 获取活跃会话数量
     * @return 会话数量
     */
    size_t getActiveSessionCount();
    
    // ============ 权限管理 ============
    
    /**
     * @brief 检查用户权限
     * @param username 用户名
     * @param permission 权限名称
     * @param method HTTP方法（可选）
     * @return 是否有权限
     */
    bool checkPermission(const String& username, const String& permission, 
                        const String& method = "");
    
    /**
     * @brief 检查会话权限
     * @param sessionId 会话ID
     * @param permission 权限名称
     * @param method HTTP方法（可选）
     * @return 是否有权限
     */
    bool checkSessionPermission(const String& sessionId, const String& permission,
                               const String& method = "");
    
    /**
     * @brief 添加权限
     * @param permission 权限定义
     */
    void addPermission(const Permission& permission);
    
    /**
     * @brief 删除权限
     * @param permissionName 权限名称
     */
    void removePermission(const String& permissionName);
    
    /**
     * @brief 获取所有权限
     * @return 权限列表
     */
    std::vector<Permission> getAllPermissions();
    
    /**
     * @brief 获取用户角色
     * @param username 用户名
     * @return 用户角色
     */
    UserRole getUserRole(const String& username);
    
    // ============ 在线用户管理 ============
    
    /**
     * @brief 获取在线用户列表
     * @return 用户名列表
     */
    std::vector<String> getOnlineUsers();
    
    /**
     * @brief 检查用户是否在线
     * @param username 用户名
     * @return 是否在线
     */
    bool isUserOnline(const String& username);
    
    /**
     * @brief 获取在线用户数量
     * @return 用户数量
     */
    size_t getOnlineUserCount();
    
    // ============ 配置管理 ============
    
    /**
     * @brief 获取安全配置
     * @return 安全配置
     */
    SecurityConfig getSecurityConfig() const;
    
    /**
     * @brief 更新安全配置
     * @param newConfig 新配置
     * @return 是否成功
     */
    bool updateSecurityConfig(const SecurityConfig& newConfig);
    
    // ============ 事件回调 ============
    
    /**
     * @brief 设置认证事件回调
     * @param callback 回调函数
     */
    void setAuthEventCallback(AuthEventCallback callback);
    
    // ============ 系统维护 ============
    
    /**
     * @brief 执行维护任务
     */
    void performMaintenance();
    
    // ============ 持久化 ============
    
    /**
     * @brief 保存会话数据
     * @return 是否成功
     */
    bool saveSessionsToStorage();
    
    /**
     * @brief 加载会话数据
     * @return 是否成功
     */
    bool loadSessionsFromStorage();
    
    // ============ 静态工具方法 ============
    
    /**
     * @brief 会话状态转字符串
     */
    static String sessionStatusToString(SessionStatus status);
    
    /**
     * @brief 生成随机令牌
     */
    static String generateRandomToken(uint8_t length = 32);
    
    /**
     * @brief 从HTTP请求中提取会话ID
     */
    static String extractSessionIdFromRequest(AsyncWebServerRequest* request, 
                                             const String& cookieName = "session");
};

#endif