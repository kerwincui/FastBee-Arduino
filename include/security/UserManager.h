#ifndef USER_MANAGER_H
#define USER_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <vector>
#include <map>

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
    UserRole role;
    bool enabled;
    unsigned long createTime;
    unsigned long lastLogin;
    unsigned long lastModified;
    
    User() : role(UserRole::VIEWER), enabled(true), 
             createTime(0), lastLogin(0), lastModified(0) {}
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
class UserManager {
private:
    // 存储
    Preferences preferences;
    
    // 用户数据
    std::map<String, User> users;
    
    // 登录尝试记录
    std::map<String, uint8_t> loginAttempts;
    
    // 配置
    struct UserConfig {
        uint8_t maxLoginAttempts = 5;
        uint32_t loginLockoutTime = 300000; // 5分钟
        uint8_t minPasswordLength = 6;
        uint8_t maxPasswordLength = 32;
        bool requireStrongPasswords = false;
        bool allowMultipleSessions = true;
    } config;
    
    // 默认用户
    static constexpr const char* DEFAULT_ADMIN_USER = "admin";
    static constexpr const char* DEFAULT_ADMIN_PASS = "admin123";
    
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
    bool addUser(const String& username, const String& password, UserRole role);
    
    /**
     * @brief 删除用户
     * @param username 用户名
     * @return 是否成功
     */
    bool deleteUser(const String& username);
    
    /**
     * @brief 更新用户
     * @param username 用户名
     * @param newPassword 新密码（空则不修改）
     * @param newRole 新角色（空则不修改）
     * @param enabled 是否启用
     * @return 是否成功
     */
    bool updateUser(const String& username, const String& newPassword = "", 
                   const String& newRole = "", bool enabled = true);
    
    /**
     * @brief 修改密码
     * @param username 用户名
     * @param oldPassword 旧密码
     * @param newPassword 新密码
     * @return 是否成功
     */
    bool changePassword(const String& username, const String& oldPassword, 
                       const String& newPassword);
    
    /**
     * @brief 重置密码（管理员用）
     * @param username 用户名
     * @param newPassword 新密码
     * @return 是否成功
     */
    bool resetPassword(const String& username, const String& newPassword);
    
    /**
     * @brief 验证用户凭据
     * @param username 用户名
     * @param password 密码
     * @return 是否验证通过
     */
    bool authenticateUser(const String& username, const String& password);
    
    // ============ 用户查询 ============
    
    /**
     * @brief 获取用户
     * @param username 用户名
     * @return 用户指针（不存在返回nullptr）
     */
    User* getUser(const String& username);
    
    /**
     * @brief 获取所有用户名
     * @return 用户名列表
     */
    std::vector<String> getAllUsernames();
    
    /**
     * @brief 获取所有用户
     * @return 用户列表
     */
    std::vector<User> getAllUsers();
    
    /**
     * @brief 检查用户是否存在
     * @param username 用户名
     * @return 是否存在
     */
    bool userExists(const String& username);
    
    /**
     * @brief 检查用户是否启用
     * @param username 用户名
     * @return 是否启用
     */
    bool isUserEnabled(const String& username);
    
    /**
     * @brief 获取用户角色
     * @param username 用户名
     * @return 用户角色
     */
    UserRole getUserRole(const String& username);
    
    // ============ 登录保护 ============
    
    /**
     * @brief 记录登录失败
     * @param username 用户名
     */
    void recordLoginFailure(const String& username);
    
    /**
     * @brief 重置登录尝试
     * @param username 用户名
     */
    void resetLoginAttempts(const String& username);
    
    /**
     * @brief 获取登录尝试次数
     * @param username 用户名
     * @return 尝试次数
     */
    uint8_t getLoginAttempts(const String& username);
    
    /**
     * @brief 检查账户是否被锁定
     * @param username 用户名
     * @return 是否锁定
     */
    bool isAccountLocked(const String& username);
    
    /**
     * @brief 解锁账户
     * @param username 用户名
     */
    void unlockAccount(const String& username);
    
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
     * @brief 获取用户数量
     * @return 用户数量
     */
    size_t getUserCount();
    
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