#ifndef AUTH_MANAGER_H
#define AUTH_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>
#include "../core/SystemConstants.h"

// 前向声明
class UserManager;
class LoggerSystem;

/**
 * @brief 用户角色枚举
 */
enum class UserRole {
    VIEWER = 0,     // 查看者 - 只读权限
    USER = 1,       // 用户 - 基本读写权限
    ADMIN = 2       // 管理员 - 所有权限
};

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
 * @brief 用户会话信息结构体
 */
struct UserSession {
    String sessionId;
    String username;
    UserRole role;
    unsigned long loginTime;
    unsigned long lastAccessTime;
    String ipAddress;
    String userAgent;
    bool isActive;
    
    UserSession() : role(UserRole::VIEWER), loginTime(0), lastAccessTime(0), isActive(false) {}
};

/**
 * @brief 权限定义结构体
 */
struct Permission {
    String name;
    String description;
    std::vector<UserRole> allowedRoles;
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
    
    AuthResult() : success(false), status(SessionStatus::INVALID), userRole(UserRole::VIEWER) {}
};

/**
 * @brief 安全配置结构体
 */
struct SecurityConfig {
    uint8_t maxLoginAttempts = Security::MAX_LOGIN_ATTEMPTS;
    uint32_t sessionTimeout = Security::SESSION_TIMEOUT;
    uint32_t loginLockoutTime = Security::LOGIN_LOCKOUT_TIME;
    uint8_t minPasswordLength = Security::MIN_PASSWORD_LENGTH;
    uint8_t maxPasswordLength = Security::MAX_PASSWORD_LENGTH;
    bool enableBruteForceProtection = true;
    bool enableSessionManagement = true;
    bool requireStrongPasswords = false;
};

/**
 * @brief 认证事件回调函数类型
 */
typedef std::function<void(const String& username, const String& event, const String& details)> AuthEventCallback;

/**
 * @brief 认证授权管理器类
 * 
 * 负责用户认证、会话管理和权限验证：
 * - 用户登录认证
 * - 会话生命周期管理
 * - 基于角色的权限控制 (RBAC)
 * - 安全策略实施
 * - 登录失败保护和账户锁定
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
    std::map<String, uint8_t> loginAttempts;
    std::map<String, unsigned long> lockedAccounts;
    
    // 权限定义
    std::vector<Permission> permissions;
    
    // 事件回调
    AuthEventCallback authEventCallback;
    
    /**
     * @brief 初始化权限系统
     */
    void initializePermissions();
    
    /**
     * @brief 验证密码强度
     * @param password 密码
     * @return 是否满足强度要求
     */
    bool validatePasswordStrength(const String& password);
    
    /**
     * @brief 生成会话ID
     * @param username 用户名
     * @return 会话ID
     */
    String generateSessionId(const String& username);
    
    /**
     * @brief 验证会话是否有效
     * @param session 用户会话
     * @return 会话状态
     */
    SessionStatus validateSession(const UserSession& session);
    
    /**
     * @brief 清理过期会话
     */
    void cleanupExpiredSessions();
    
    /**
     * @brief 记录认证事件
     * @param username 用户名
     * @param event 事件类型
     * @param details 事件详情
     * @param success 是否成功
     */
    void logAuthEvent(const String& username, const String& event, const String& details, bool success = true);
    
    /**
     * @brief 检查账户是否被锁定
     * @param username 用户名
     * @return 是否被锁定
     */
    bool isAccountLocked(const String& username);
    
    /**
     * @brief 锁定账户
     * @param username 用户名
     */
    void lockAccount(const String& username);
    
    /**
     * @brief 解锁账户
     * @param username 用户名
     */
    void unlockAccount(const String& username);
    
    /**
     * @brief 加载安全配置
     * @return 加载是否成功
     */
    bool loadSecurityConfig();
    
    /**
     * @brief 保存安全配置
     * @return 保存是否成功
     */
    bool saveSecurityConfig();

public:
    /**
     * @brief 构造函数
     * @param userMgr 用户管理器指针
     * @param loggerPtr 日志系统指针
     */
    AuthManager(UserManager* userMgr = nullptr, LoggerSystem* loggerPtr = nullptr);
    
    /**
     * @brief 析构函数
     */
    ~AuthManager();
    
    /**
     * @brief 初始化认证管理器
     * @return 初始化是否成功
     */
    bool initialize();
    
    /**
     * @brief 用户登录认证
     * @param username 用户名
     * @param password 密码
     * @param ipAddress 客户端IP地址
     * @param userAgent 用户代理
     * @return 认证结果
     */
    AuthResult authenticate(const String& username, const String& password, 
                           const String& ipAddress = "", const String& userAgent = "");
    
    /**
     * @brief 验证会话令牌
     * @param sessionId 会话ID
     * @param updateAccessTime 是否更新访问时间
     * @return 认证结果
     */
    AuthResult verifySession(const String& sessionId, bool updateAccessTime = true);
    
    /**
     * @brief 用户登出
     * @param sessionId 会话ID
     * @return 登出是否成功
     */
    bool logout(const String& sessionId);
    
    /**
     * @brief 强制登出用户
     * @param username 用户名
     * @return 登出是否成功
     */
    bool forceLogout(const String& username);
    
    /**
     * @brief 检查用户权限
     * @param username 用户名
     * @param permission 权限名称
     * @return 是否有权限
     */
    bool checkPermission(const String& username, const String& permission);
    
    /**
     * @brief 检查会话权限
     * @param sessionId 会话ID
     * @param permission 权限名称
     * @return 是否有权限
     */
    bool checkSessionPermission(const String& sessionId, const String& permission);
    
    /**
     * @brief 获取用户角色
     * @param username 用户名
     * @return 用户角色
     */
    UserRole getUserRole(const String& username);
    
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
     * @brief 获取安全配置
     * @return 安全配置
     */
    SecurityConfig getSecurityConfig() const;
    
    /**
     * @brief 更新安全配置
     * @param newConfig 新的安全配置
     * @return 更新是否成功
     */
    bool updateSecurityConfig(const SecurityConfig& newConfig);
    
    /**
     * @brief 设置认证事件回调
     * @param callback 回调函数
     */
    void setAuthEventCallback(AuthEventCallback callback);
    
    /**
     * @brief 重置用户登录尝试次数
     * @param username 用户名
     */
    void resetLoginAttempts(const String& username);
    
    /**
     * @brief 获取用户登录尝试次数
     * @param username 用户名
     * @return 尝试次数
     */
    uint8_t getLoginAttempts(const String& username);
    
    /**
     * @brief 获取活跃会话数量
     * @return 会话数量
     */
    size_t getActiveSessionCount();
    
    /**
     * @brief 获取权限列表
     * @return 权限列表
     */
    std::vector<Permission> getPermissions() const;
    
    /**
     * @brief 添加自定义权限
     * @param permission 权限定义
     */
    void addPermission(const Permission& permission);
    
    // 静态工具方法
public:
    /**
     * @brief 角色枚举转字符串
     * @param role 用户角色
     * @return 角色字符串
     */
    static String roleToString(UserRole role);
    
    /**
     * @brief 字符串转角色枚举
     * @param roleStr 角色字符串
     * @return 用户角色
     */
    static UserRole stringToRole(const String& roleStr);
    
    /**
     * @brief 会话状态转字符串
     * @param status 会话状态
     * @return 状态字符串
     */
    static String sessionStatusToString(SessionStatus status);
    
    /**
     * @brief 生成随机令牌
     * @param length 令牌长度
     * @return 随机令牌
     */
    static String generateRandomToken(uint8_t length = 32);
    
    /**
     * @brief 计算密码哈希
     * @param password 密码
     * @param salt 盐值
     * @return 哈希值
     */
    static String hashPassword(const String& password, const String& salt = "");
};

#endif