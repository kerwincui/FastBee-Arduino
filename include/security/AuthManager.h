#ifndef AUTH_MANAGER_H
#define AUTH_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <map>
#include <vector>
#include <functional>
#include "core/interfaces/IAuthManager.h"
#include "core/interfaces/IUserManager.h"
#include "UserManager.h"
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
    unsigned long loginTime;
    unsigned long lastAccessTime;
    unsigned long expireTime;
    String ipAddress;
    String userAgent;
    bool isActive;

    UserSession() : loginTime(0),
                   lastAccessTime(0), expireTime(0), isActive(false) {}
};

/**
 * @brief 安全配置结构体（运行时缓存，实际配置存储在 UserManager 的 auth.json 中）
 */
struct SecurityConfig {
    uint32_t sessionTimeout = 3600000; // 1小时
    uint32_t sessionCleanupInterval = 60000; // 1分钟
    bool enableSessionPersistence = false;
    bool enableIpWhitelist = false;
    bool allowMultipleSessions = true;
    std::vector<String> ipWhitelist;

    // Cookie配置
    String cookieName = "sessionId";
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
 * 负责用户认证和会话管理（单管理员模式，无权限/角色区分）
 */
class AuthManager : public IAuthManager {
private:
    // 配置和状态
    SecurityConfig securityConfig;
    Preferences sessionPrefs;

    // 依赖组件
    IUserManager* userManager;
    LoggerSystem* logger;

    // 会话管理
    std::map<String, UserSession> activeSessions;
    std::map<String, unsigned long> lockedAccounts;
    unsigned long lastSessionCleanup;
    static constexpr size_t MAX_ACTIVE_SESSIONS = 6;
    static constexpr size_t MAX_SESSIONS_PER_USER = 3;

    // 事件回调
    AuthEventCallback authEventCallback;

    // 私有方法
    String generateSessionId(const String& username);
    void cleanupExpiredSessions();
    bool removeOldestSession(const String& username = "");
    void trimActiveSessions(const String& username = "");
    void lockAccount(const String& username);
    void logAuthEvent(const String& event, const String& username,
                     const String& details, bool success = true);
    bool loadSecurityConfig();
    bool saveSecurityConfig();
    bool checkIpWhitelist(const String& ipAddress);

public:
    AuthManager(IUserManager* userMgr = nullptr, LoggerSystem* loggerPtr = nullptr);
    ~AuthManager();

    bool initialize();

    // IAuthManager 接口实现
    void setUserManager(IUserManager* userManager) override;
    bool authenticate(const String& username, const String& password) override;
    String generateToken(const String& username) override;
    bool validateToken(const String& token) override;
    bool revokeToken(const String& token) override;
    void shutdown() override;
    AuthResult verifySession(const String& sessionId, bool updateLastActivity = true) override;
    AuthResult login(const String& username, const String& password, const String& ipAddress, const String& userAgent) override;
    bool logout(const String& sessionId) override;
    bool forceLogout(const String& username) override;
    bool isUserOnline(const String& username) override;
    std::vector<UserSession> getActiveSessions() override;
    size_t getActiveSessionCount() override;
    size_t getOnlineUserCount() override;
    SessionStatus validateSession(const UserSession& session) override;
    bool isAccountLocked(const String& username) override;
    void unlockAccount(const String& username) override;

    // ============ 认证管理 ============

    size_t forceLogoutAll();

    // ============ 会话管理 ============

    UserSession getSessionInfo(const String& sessionId);
    std::vector<UserSession> getUserSessions(const String& username);
    bool updateSessionAccessTime(const String& sessionId);

    // ============ 在线用户管理 ============

    std::vector<String> getOnlineUsers();

    // ============ 配置管理 ============

    SecurityConfig getSecurityConfig() const;
    bool updateSecurityConfig(const SecurityConfig& newConfig);

    // ============ 事件回调 ============

    void setAuthEventCallback(AuthEventCallback callback);

    // ============ 系统维护 ============

    void performMaintenance();

    // ============ 持久化 ============

    bool saveSessionsToStorage();
    bool loadSessionsFromStorage();

    // 静态工具方法
    static String sessionStatusToString(SessionStatus status);
    static String generateRandomToken(uint8_t length = 32);
    static String extractSessionIdFromRequest(AsyncWebServerRequest* request,
                                             const String& cookieName = "sessionId");
    static bool validatePasswordStrength(const String& password, String* errorMessage = nullptr);
    static String getPasswordStrengthRating(const String& password);
};

#endif
