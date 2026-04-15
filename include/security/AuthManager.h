#ifndef AUTH_MANAGER_H
#define AUTH_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>  // 仅用于会话持久化
#include <map>
#include <vector>
#include <functional>
#include "core/interfaces/IAuthManager.h"
#include "core/interfaces/IUserManager.h"
#include "UserManager.h"
#include "RoleManager.h"
#include "core/SystemConstants.h"
#include <ESPAsyncWebServer.h>

// 前向声明
class LoggerSystem;

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
 * @brief 安全配置结构体（运行时缓存，实际配置存储在 UserManager 的 users.json 中）
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
 * @brief 审计日志条目
 */
struct AuditEntry {
    unsigned long timestamp;  ///< millis()
    String username;          ///< 操作人
    String action;            ///< 操作类型，如 "login"/"user.create"
    String resource;          ///< 操作对象，如用户名/角色ID
    String detail;            ///< 详情
    bool   success;           ///< 是否成功
    String ipAddress;         ///< 来源 IP

    AuditEntry() : timestamp(0), success(true) {}
};

/**
 * @brief 认证管理器类
 * 负责用户认证、会话管理和权限验证
 */
class AuthManager : public IAuthManager {
private:
    // 配置和状态（运行时缓存，从 UserManager 同步）
    SecurityConfig securityConfig;
    Preferences sessionPrefs;  // 仅用于会话数据持久化

    // 依赖组件
    IUserManager* userManager;
    RoleManager*  roleManager;     ///< 角色管理器（权限校验委托给此类）
    LoggerSystem* logger;

    // 会话管理
    std::map<String, UserSession> activeSessions;
    std::map<String, unsigned long> lockedAccounts;
    unsigned long lastSessionCleanup;

    // 权限定义（保留旧结构供向下兼容）
    std::map<String, Permission> permissions;

    // 权限缓存（减少高频 API 的权限重复检查开销）
    struct PermCacheEntry {
        String username;
        String permission;
        bool   result;
        unsigned long timestamp;
    };
    static constexpr size_t PERM_CACHE_SIZE = 4;
    static constexpr unsigned long PERM_CACHE_TTL = 5000; // 5秒
    PermCacheEntry _permCache[4];
    uint8_t _permCacheIdx = 0;

    // 审计日志（环形缓冲，最多保留 MAX_AUDIT_ENTRIES 条）
    static constexpr size_t MAX_AUDIT_ENTRIES = 20;
    std::vector<AuditEntry> auditLog;

    // 事件回调
    AuthEventCallback authEventCallback;

    // 私有方法
    void initializePermissions();
    String generateSessionId(const String& username);
    void cleanupExpiredSessions();
    void lockAccount(const String& username);
    void logAuthEvent(const String& event, const String& username,
                     const String& details, bool success = true);
    void appendAuditEntry(const String& username, const String& action,
                         const String& resource, const String& detail,
                         bool success, const String& ip = "");
    bool loadSecurityConfig();
    bool saveSecurityConfig();
    bool checkIpWhitelist(const String& ipAddress);
    
public:
    /**
     * @brief 构造函数
     * @param userMgr   用户管理器
     * @param roleMgr   角色管理器
     * @param loggerPtr 日志系统
     */
    AuthManager(IUserManager* userMgr = nullptr, RoleManager* roleMgr = nullptr,
                LoggerSystem* loggerPtr = nullptr);
    
    /**
     * @brief 析构函数
     */
    ~AuthManager();
    
    /**
     * @brief 初始化认证管理器
     * @return 是否成功
     */
    bool initialize();

    // IAuthManager 接口实现
    void setUserManager(IUserManager* userManager) override;
    bool authenticate(const String& username, const String& password) override;
    String generateToken(const String& username) override;
    bool validateToken(const String& token) override;
    bool revokeToken(const String& token) override;
    bool checkPermission(const String& username, const String& permission) override;
    void shutdown() override;
    AuthResult verifySession(const String& sessionId, bool updateLastActivity = true) override;
    AuthResult login(const String& username, const String& password, const String& ipAddress, const String& userAgent) override;
    bool logout(const String& sessionId) override;
    bool forceLogout(const String& username) override;
    bool checkSessionPermission(const String& sessionId, const String& permission) override;
    bool isUserOnline(const String& username) override;
    std::vector<UserSession> getActiveSessions() override;
    size_t getActiveSessionCount() override;
    size_t getOnlineUserCount() override;
    SessionStatus validateSession(const UserSession& session) override;
    bool isAccountLocked(const String& username) override;
    void unlockAccount(const String& username) override;

    // ============ 角色管理集成 ============

    /**
     * @brief 设置角色管理器（初始化后可替换）
     * @param roleMgr 角色管理器指针
     */
    void setRoleManager(RoleManager* roleMgr);

    // ============ 审计日志 ============

    /**
     * @brief 获取审计日志（最多 MAX_AUDIT_ENTRIES 条，新的在前）
     */
    std::vector<AuditEntry> getAuditLog() const;

    /**
     * @brief 获取审计日志 JSON 字符串
     * @param limit 最多返回条数（0=全部）
     */
    String getAuditLogJson(size_t limit = 0) const;

    /**
     * @brief 清空审计日志
     */
    void clearAuditLog();

    /**
     * @brief 记录操作审计（外部模块调用，如用户/角色变更）
     * @param username 操作人
     * @param action   操作类型，格式 "resource.action"
     * @param resource 操作对象
     * @param detail   详情
     * @param success  是否成功
     * @param ip       来源 IP
     */
    void recordAudit(const String& username, const String& action,
                    const String& resource, const String& detail,
                    bool success = true, const String& ip = "");

    // ============ 认证管理 ============
    
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
    
    // 静态工具方法
    
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
    
    /**
     * @brief 验证密码强度
     */
    static bool validatePasswordStrength(const String& password, String* errorMessage = nullptr);
    
    /**
     * @brief 生成密码强度评级
     */
    static String getPasswordStrengthRating(const String& password);
};

#endif