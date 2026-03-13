#ifndef USER_MANAGER_H
#define USER_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>
#include <map>
#include "core/interfaces/IUserManager.h"
#include "core/SystemConstants.h"

// 用户配置文件路径
static const char* USERS_CONFIG_FILE = "/config/users.json";

/**
 * @brief 用户角色枚举
 */
enum class UserRole {
    VIEWER = 0,     // 查看者 - 只读权限
    USER = 1,       // 用户 - 基本读写权限
    ADMIN = 2       // 管理员 - 所有权限
};

/**
 * @brief 用户结构体
 */
struct User {
    String username;
    String passwordHash;
    String salt;
    UserRole role;                      ///< 兼容旧接口，等同于 roles[0]
    std::vector<String> roles;          ///< 多角色列表（角色 ID，对应 RoleManager）
    bool enabled;
    unsigned long createTime;
    unsigned long lastLogin;
    unsigned long lastModified;

    // 扩展元数据
    String email;       ///< 邮箱（可选）
    String remark;      ///< 备注
    String createBy;    ///< 创建人（username）

    User() : role(UserRole::VIEWER), enabled(true),
             createTime(0), lastLogin(0), lastModified(0) {}

    /** 检查是否拥有指定角色 */
    bool hasRole(const String& roleId) const {
        for (const String& r : roles) {
            if (r == roleId) return true;
        }
        return false;
    }
};

/**
 * @brief 用户统计信息
 */
struct UserStats {
    String username;
    UserRole role;
    bool isOnline;
    bool isLocked;
    uint8_t loginAttempts;
    unsigned long loginTime;
    unsigned long lastAccess;
    unsigned long createTime;
};

/**
 * @brief 用户管理器类
 * 负责用户数据的增删改查和存储管理
 */
class UserManager : public IUserManager {
private:
    // 用户数据
    std::map<String, User> users;
    
    // 登录尝试记录
    std::map<String, uint8_t> loginAttempts;
    
    // 配置（包含安全配置，统一存储到 users.json）
    struct UserConfig {
        // 用户安全配置
        uint8_t maxLoginAttempts = 5;
        uint32_t loginLockoutTime = 300000; // 5分钟
        uint8_t minPasswordLength = 6;
        uint8_t maxPasswordLength = 32;
        bool requireStrongPasswords = false;
        bool allowMultipleSessions = true;
        
        // 会话安全配置（从 AuthManager 合并）
        uint32_t sessionTimeout = 3600000;        // 1小时
        uint32_t sessionCleanupInterval = 60000;  // 1分钟
        bool enableSessionPersistence = true;
        String cookieName = "session";
        uint32_t cookieMaxAge = 3600;             // 1小时
        bool cookieHttpOnly = true;
        bool cookieSecure = false;
    } config;
    
    // 默认用户（统一使用 SystemConstants 中定义的常量）
    static constexpr const char* DEFAULT_ADMIN_USER = Security::DEFAULT_ADMIN_USERNAME;
    static constexpr const char* DEFAULT_ADMIN_PASS = Security::DEFAULT_ADMIN_PASSWORD;
    
    // 私有方法
    void initializeDefaultAdmin();
    String hashPassword(const String& password, const String& salt);
    bool verifyPassword(const String& password, const String& hash, const String& salt);
    String generateSalt();
    bool validatePasswordStrength(const String& password);
    void updateUserLastModified(const String& username);
    
public:
    UserManager();
    ~UserManager();
    
    /**
     * @brief 初始化用户管理器
     * @return 是否成功
     */
    bool initialize();
    
    // ============ 用户管理 ============
    
    /**
     * @brief 添加用户
     * @param username 用户名
     * @param password 密码
     * @param role 角色
     * @return 是否成功
     */

    
    // IUserManager 接口实现
    bool addUser(const String& username, const String& password, const String& role) override;
    bool deleteUser(const String& username) override;
    bool updatePassword(const String& username, const String& newPassword) override;
    bool validateUser(const String& username, const String& password) override;
    String getUserRole(const String& username) override;
    bool userExists(const String& username) override;
    String getAllUsers() override;
    User* getUser(const String& username) override;
    void resetLoginAttempts(const String& username) override;
    uint8_t getLoginAttempts(const String& username) override;
    bool isAccountLocked(const String& username) override;
    void unlockAccount(const String& username) override;
    bool changePassword(const String& username, const String& oldPassword, const String& newPassword) override;
    bool resetPassword(const String& username, const String& newPassword) override;
    bool updateUser(const String& username, const String& newPassword, const String& newRole, bool enabled) override;
    size_t getUserCount() override;
    
    // ============ 多角色管理 ============

    /**
     * @brief 为用户追加角色（不重复）
     * @param username 用户名
     * @param roleId   角色 ID
     * @return 是否成功
     */
    bool assignRole(const String& username, const String& roleId);

    /**
     * @brief 从用户移除角色
     * @param username 用户名
     * @param roleId   角色 ID
     * @return 是否成功
     */
    bool removeRole(const String& username, const String& roleId);

    /**
     * @brief 替换用户的完整角色列表
     * @param username 用户名
     * @param roleIds  新角色列表
     * @return 是否成功
     */
    bool setRoles(const String& username, const std::vector<String>& roleIds);

    /**
     * @brief 获取用户角色列表
     * @param username 用户名
     * @return 角色 ID 列表
     */
    std::vector<String> getUserRoles(const String& username) const;

    /**
     * @brief 检查用户是否拥有指定角色
     * @param username 用户名
     * @param roleId   角色 ID
     * @return 是否拥有
     */
    bool hasRole(const String& username, const String& roleId) const;

    /**
     * @brief 更新用户元数据（email/remark）
     * @param username 用户名
     * @param email    新邮箱（空则不修改）
     * @param remark   新备注（空则不修改）
     * @return 是否成功
     */
    bool updateUserMeta(const String& username, const String& email, const String& remark);

    // ============ 用户查询 ============

    /**
     * @brief 获取所有用户名
     * @return 用户名列表
     */
    std::vector<String> getAllUsernames();

    /**
     * @brief 检查用户是否启用
     * @param username 用户名
     * @return 是否启用
     */
    bool isUserEnabled(const String& username);
    
    // ============ 登录保护 ============
    
    /**
     * @brief 记录登录失败
     * @param username 用户名
     */
    void recordLoginFailure(const String& username);
    
    // ============ 统计信息 ============
    
    /**
     * @brief 获取用户统计信息
     * @param username 用户名
     * @return 统计信息
     */
    UserStats getUserStats(const String& username);
    
    /**
     * @brief 获取所有用户统计信息
     * @return 统计信息列表
     */
    std::vector<UserStats> getAllUserStats();
    

    
    /**
     * @brief 更新用户最后登录时间
     * @param username 用户名
     */
    void updateLastLogin(const String& username);
    
    // ============ 持久化 ============
    
    /**
     * @brief 保存用户数据
     * @return 是否成功
     */
    bool saveUsersToStorage();
    
    /**
     * @brief 加载用户数据
     * @return 是否成功
     */
    bool loadUsersFromStorage();
    
    /**
     * @brief 保存配置
     * @return 是否成功
     */
    bool saveConfig();
    
    /**
     * @brief 加载配置
     * @return 是否成功
     */
    bool loadConfig();
    
    // ============ 安全配置访问（供 AuthManager 使用） ============
    
    /**
     * @brief 获取会话超时时间（毫秒）
     */
    uint32_t getSessionTimeout() const { return config.sessionTimeout; }
    
    /**
     * @brief 获取会话清理间隔（毫秒）
     */
    uint32_t getSessionCleanupInterval() const { return config.sessionCleanupInterval; }
    
    /**
     * @brief 是否启用会话持久化
     */
    bool isSessionPersistenceEnabled() const { return config.enableSessionPersistence; }
    
    /**
     * @brief 获取 Cookie 名称
     */
    const String& getCookieName() const { return config.cookieName; }
    
    /**
     * @brief 获取 Cookie 最大存活时间（秒）
     */
    uint32_t getCookieMaxAge() const { return config.cookieMaxAge; }
    
    /**
     * @brief 是否启用 HttpOnly Cookie
     */
    bool isCookieHttpOnly() const { return config.cookieHttpOnly; }
    
    /**
     * @brief 是否启用 Secure Cookie
     */
    bool isCookieSecure() const { return config.cookieSecure; }
    
    /**
     * @brief 获取最大登录尝试次数
     */
    uint8_t getMaxLoginAttempts() const { return config.maxLoginAttempts; }
    
    /**
     * @brief 获取登录锁定时间（毫秒）
     */
    uint32_t getLoginLockoutTime() const { return config.loginLockoutTime; }
    
    /**
     * @brief 更新安全配置
     */
    bool updateSecurityConfig(uint32_t sessionTimeout, uint32_t sessionCleanupInterval,
                              bool enableSessionPersistence, const String& cookieName,
                              uint32_t cookieMaxAge, bool cookieHttpOnly, bool cookieSecure);
    
    // ============ 工具方法 ============
    
    /**
     * @brief 角色转字符串
     * @param role 角色
     * @return 字符串
     */
    static String roleToString(UserRole role);
    
    /**
     * @brief 字符串转角色
     * @param roleStr 角色字符串
     * @return 角色
     */
    static UserRole stringToRole(const String& roleStr);
    
    /**
     * @brief 生成随机盐值
     * @return 盐值字符串
     */
    static String generateRandomSalt();
    
    /**
     * @brief 验证密码强度
     * @param password 密码
     * @param minLength 最小长度
     * @param requireComplexity 需要复杂度
     * @return 是否有效
     */
    static bool validatePassword(const String& password, uint8_t minLength = 6, 
                                bool requireComplexity = false);
};

#endif