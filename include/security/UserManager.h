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
static const char* ADMIN_AUTH_CONFIG_FILE = "/config/auth.json";

/**
 * @brief 用户结构体
 */
struct User {
    String username;
    String passwordHash;
    String salt;
    String description;         ///< 用户描述（可选，最大64字符）
    bool enabled;
    unsigned long createTime;
    unsigned long lastLogin;
    unsigned long lastModified;

    User() : enabled(true), createTime(0), lastLogin(0), lastModified(0) {}
};

/**
 * @brief 用户统计信息
 */
struct UserStats {
    String username;
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
 * 单管理员模式：仅支持一个 admin 用户的密码认证
 */
class UserManager : public IUserManager {
private:
    // 用户数据
    std::map<String, User> users;
    
    // 登录尝试记录
    std::map<String, uint8_t> loginAttempts;
    
    // 配置（包含安全配置，统一存储到 auth.json）
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
        bool enableSessionPersistence = false;
        String cookieName = "sessionId";
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
    
    // ============ IUserManager 接口实现 ============
    
    bool addUser(const String& username, const String& password) override;
    bool deleteUser(const String& username) override;
    bool updatePassword(const String& username, const String& newPassword) override;
    bool validateUser(const String& username, const String& password) override;

    bool userExists(const String& username) override;
    String getAllUsers() override;
    User* getUser(const String& username) override;
    void resetLoginAttempts(const String& username) override;
    uint8_t getLoginAttempts(const String& username) override;
    bool isAccountLocked(const String& username) override;
    void unlockAccount(const String& username) override;
    bool changePassword(const String& username, const String& oldPassword, const String& newPassword) override;
    bool resetPassword(const String& username, const String& newPassword) override;
    bool updateUser(const String& username, const String& newPassword, bool enabled) override;
    size_t getUserCount() override;

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

    /**
     * @brief 设置用户描述
     * @param username 用户名
     * @param description 描述（最大64字符）
     * @return 是否成功
     */
    bool setUserDescription(const String& username, const String& description);
    
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
    
    bool saveUsersToStorage();
    bool loadUsersFromStorage();
    bool saveConfig();
    bool loadConfig();
    
    // ============ 安全配置访问（供 AuthManager 使用） ============
    
    uint32_t getSessionTimeout() const { return config.sessionTimeout; }
    uint32_t getSessionCleanupInterval() const { return config.sessionCleanupInterval; }
    bool isSessionPersistenceEnabled() const { return config.enableSessionPersistence; }
    const String& getCookieName() const { return config.cookieName; }
    uint32_t getCookieMaxAge() const { return config.cookieMaxAge; }
    bool isCookieHttpOnly() const { return config.cookieHttpOnly; }
    bool isCookieSecure() const { return config.cookieSecure; }
    uint8_t getMaxLoginAttempts() const { return config.maxLoginAttempts; }
    uint32_t getLoginLockoutTime() const { return config.loginLockoutTime; }
    bool allowsMultipleSessions() const { return config.allowMultipleSessions; }
    
    bool updateSecurityConfig(uint32_t sessionTimeout, uint32_t sessionCleanupInterval,
                              bool enableSessionPersistence, const String& cookieName,
                              uint32_t cookieMaxAge, bool cookieHttpOnly, bool cookieSecure);
    
    /**
     * @brief 更新密码策略配置（4个UI可配置字段）
     * @details 更新后自动持久化到 users.json
     */
    void updatePasswordPolicy(uint8_t maxAttempts, uint32_t lockoutTime,
                              uint8_t minPwdLen, bool requireStrong);

    // ============ 工具方法 ============

    static String generateRandomSalt();
    static bool validatePassword(const String& password, uint8_t minLength = 6, 
                                bool requireComplexity = false);
    

};

#endif
