#ifndef I_AUTH_MANAGER_H
#define I_AUTH_MANAGER_H

#include <Arduino.h>
#include <vector>
#include "IUserManager.h"

// 前向声明
struct UserSession;

/**
 * @brief 认证结果结构体
 */
struct AuthResult {
    bool success;
    String username;
    String sessionId;
    String errorMessage;
    
    AuthResult() : success(false) {}
};

/**
 * @brief 会话状态结构体
 */
struct SessionStatus {
    bool valid;
    String username;
    bool expired;
    bool locked;
    
    SessionStatus() : valid(false), expired(false), locked(false) {}
};

/**
 * @brief 认证管理器接口
 * @details 定义认证管理的基本操作
 */
class IAuthManager {
public:
    virtual ~IAuthManager() = default;
    
    /**
     * @brief 初始化认证管理器
     * @return 是否初始化成功
     */
    virtual bool initialize() = 0;
    
    /**
     * @brief 设置用户管理器
     * @param userManager 用户管理器指针
     */
    virtual void setUserManager(IUserManager* userManager) = 0;
    
    /**
     * @brief 验证用户凭据
     * @param username 用户名
     * @param password 密码
     * @return 是否验证成功
     */
    virtual bool authenticate(const String& username, const String& password) = 0;
    
    /**
     * @brief 生成会话令牌
     * @param username 用户名
     * @return 会话令牌
     */
    virtual String generateToken(const String& username) = 0;
    
    /**
     * @brief 验证会话令牌
     * @param token 会话令牌
     * @return 是否验证成功
     */
    virtual bool validateToken(const String& token) = 0;
    
    /**
     * @brief 撤销会话令牌
     * @param token 会话令牌
     * @return 是否撤销成功
     */
    virtual bool revokeToken(const String& token) = 0;
    
    /**
     * @brief 检查用户权限
     * @param username 用户名
     * @param permission 权限
     * @return 是否有权限
     */
    virtual bool checkPermission(const String& username, const String& permission) = 0;
    
    /**
     * @brief 关闭认证管理器
     */
    virtual void shutdown() = 0;
    
    /**
     * @brief 验证会话
     * @param sessionId 会话ID
     * @param updateLastActivity 是否更新最后活动时间
     * @return 认证结果
     */
    virtual AuthResult verifySession(const String& sessionId, bool updateLastActivity = true) = 0;
    
    /**
     * @brief 登录
     * @param username 用户名
     * @param password 密码
     * @param ipAddress IP地址
     * @param userAgent 用户代理
     * @return 认证结果
     */
    virtual AuthResult login(const String& username, const String& password, const String& ipAddress, const String& userAgent) = 0;
    
    /**
     * @brief 登出
     * @param sessionId 会话ID
     * @return 是否成功
     */
    virtual bool logout(const String& sessionId) = 0;
    
    /**
     * @brief 强制登出
     * @param username 用户名
     * @return 是否成功
     */
    virtual bool forceLogout(const String& username) = 0;
    
    /**
     * @brief 检查会话权限
     * @param sessionId 会话ID
     * @param permission 权限
     * @return 是否有权限
     */
    virtual bool checkSessionPermission(const String& sessionId, const String& permission) = 0;
    
    /**
     * @brief 检查用户是否在线
     * @param username 用户名
     * @return 是否在线
     */
    virtual bool isUserOnline(const String& username) = 0;
    
    /**
     * @brief 获取活跃会话
     * @return 会话列表
     */
    virtual std::vector<UserSession> getActiveSessions() = 0;
    
    /**
     * @brief 获取活跃会话数量
     * @return 会话数量
     */
    virtual size_t getActiveSessionCount() = 0;
    
    /**
     * @brief 获取在线用户数量
     * @return 用户数量
     */
    virtual size_t getOnlineUserCount() = 0;
    
    /**
     * @brief 验证会话状态
     * @param session 会话
     * @return 会话状态
     */
    virtual SessionStatus validateSession(const UserSession& session) = 0;
    
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
};

#endif // I_AUTH_MANAGER_H