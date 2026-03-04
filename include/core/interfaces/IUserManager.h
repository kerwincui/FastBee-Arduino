#ifndef I_USER_MANAGER_H
#define I_USER_MANAGER_H

#include <Arduino.h>

// 前向声明
struct User;

/**
 * @brief 用户管理器接口
 * @details 定义用户管理的基本操作
 */
class IUserManager {
public:
    virtual ~IUserManager() = default;
    
    /**
     * @brief 初始化用户管理器
     * @return 是否初始化成功
     */
    virtual bool initialize() = 0;
    
    /**
     * @brief 添加用户
     * @param username 用户名
     * @param password 密码
     * @param role 角色
     * @return 是否添加成功
     */
    virtual bool addUser(const String& username, const String& password, const String& role) = 0;
    
    /**
     * @brief 删除用户
     * @param username 用户名
     * @return 是否删除成功
     */
    virtual bool deleteUser(const String& username) = 0;
    
    /**
     * @brief 更新用户密码
     * @param username 用户名
     * @param newPassword 新密码
     * @return 是否更新成功
     */
    virtual bool updatePassword(const String& username, const String& newPassword) = 0;
    
    /**
     * @brief 验证用户
     * @param username 用户名
     * @param password 密码
     * @return 是否验证成功
     */
    virtual bool validateUser(const String& username, const String& password) = 0;
    
    /**
     * @brief 获取用户角色
     * @param username 用户名
     * @return 用户角色
     */
    virtual String getUserRole(const String& username) = 0;
    
    /**
     * @brief 检查用户是否存在
     * @param username 用户名
     * @return 是否存在
     */
    virtual bool userExists(const String& username) = 0;
    
    /**
     * @brief 获取所有用户
     * @return 用户列表的 JSON 字符串
     */
    virtual String getAllUsers() = 0;
    
    /**
     * @brief 获取用户
     * @param username 用户名
     * @return 用户指针（不存在返回nullptr）
     */
    virtual User* getUser(const String& username) = 0;
    
    /**
     * @brief 重置登录尝试
     * @param username 用户名
     */
    virtual void resetLoginAttempts(const String& username) = 0;
    
    /**
     * @brief 获取登录尝试次数
     * @param username 用户名
     * @return 尝试次数
     */
    virtual uint8_t getLoginAttempts(const String& username) = 0;
    
    /**
     * @brief 检查账户是否被锁定
     * @param username 用户名
     * @return 是否锁定
     */
    virtual bool isAccountLocked(const String& username) = 0;
    
    /**
     * @brief 解锁账户
     * @param username 用户名
     */
    virtual void unlockAccount(const String& username) = 0;
    
    /**
     * @brief 修改密码
     * @param username 用户名
     * @param oldPassword 旧密码
     * @param newPassword 新密码
     * @return 是否成功
     */
    virtual bool changePassword(const String& username, const String& oldPassword, const String& newPassword) = 0;
    
    /**
     * @brief 重置密码（管理员用）
     * @param username 用户名
     * @param newPassword 新密码
     * @return 是否成功
     */
    virtual bool resetPassword(const String& username, const String& newPassword) = 0;
    
    /**
     * @brief 更新用户
     * @param username 用户名
     * @param newPassword 新密码（空则不修改）
     * @param newRole 新角色（空则不修改）
     * @param enabled 是否启用
     * @return 是否成功
     */
    virtual bool updateUser(const String& username, const String& newPassword = "", const String& newRole = "", bool enabled = true) = 0;
    
    /**
     * @brief 获取用户数量
     * @return 用户数量
     */
    virtual size_t getUserCount() = 0;
};

#endif // I_USER_MANAGER_H