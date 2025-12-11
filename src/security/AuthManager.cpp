/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:31:54
 */

#include "security/AuthManager.h"
#include "Security/UserManager.h"
#include "Systems/LoggerSystem.h"
#include "Security/CryptoUtils.h"
#include "Utils/TimeUtils.h"
#include <algorithm>

AuthManager::AuthManager(UserManager* userMgr, LoggerSystem* loggerPtr) 
    : userManager(userMgr), logger(loggerPtr) {
    
    // 初始化默认安全配置
    securityConfig = SecurityConfig();
    
    // 初始化权限系统
    initializePermissions();
}

AuthManager::~AuthManager() {
    // 清理资源
    activeSessions.clear();
    loginAttempts.clear();
    lockedAccounts.clear();
}

bool AuthManager::initialize() {
    if (logger) logger->info("Initializing AuthManager...", "AUTH");
    
    // 初始化Preferences
    if (!preferences.begin("auth_manager", false)) {
        if (logger) logger->error("Failed to initialize Preferences", "AUTH");
        return false;
    }
    
    // 加载安全配置
    if (!loadSecurityConfig()) {
        if (logger) logger->warning("Failed to load security config, using defaults", "AUTH");
    }
    
    // 清理过期的会话和锁定账户
    cleanupExpiredSessions();
    
    if (logger) logger->info("AuthManager initialized successfully", "AUTH");
    return true;
}

void AuthManager::initializePermissions() {
    permissions.clear();
    
    // 系统配置权限
    Permission sysConfig;
    sysConfig.name = "system_config";
    sysConfig.description = "系统配置管理";
    sysConfig.allowedRoles = {UserRole::ADMIN};
    permissions.push_back(sysConfig);
    
    // 网络配置权限
    Permission netConfig;
    netConfig.name = "network_config";
    netConfig.description = "网络配置管理";
    netConfig.allowedRoles = {UserRole::ADMIN, UserRole::USER};
    permissions.push_back(netConfig);
    
    // 用户管理权限
    Permission userManage;
    userManage.name = "user_manage";
    userManage.description = "用户管理";
    userManage.allowedRoles = {UserRole::ADMIN};
    permissions.push_back(userManage);
    
    // 设备监控权限
    Permission deviceMonitor;
    deviceMonitor.name = "device_monitor";
    deviceMonitor.description = "设备监控";
    deviceMonitor.allowedRoles = {UserRole::ADMIN, UserRole::USER, UserRole::VIEWER};
    permissions.push_back(deviceMonitor);
    
    // OTA升级权限
    Permission otaUpdate;
    otaUpdate.name = "ota_update";
    otaUpdate.description = "固件OTA升级";
    otaUpdate.allowedRoles = {UserRole::ADMIN};
    permissions.push_back(otaUpdate);
    
    // 协议配置权限
    Permission protocolConfig;
    protocolConfig.name = "protocol_config";
    protocolConfig.description = "协议配置";
    protocolConfig.allowedRoles = {UserRole::ADMIN, UserRole::USER};
    permissions.push_back(protocolConfig);
    
    // 数据查看权限
    Permission dataView;
    dataView.name = "data_view";
    dataView.description = "数据查看";
    dataView.allowedRoles = {UserRole::ADMIN, UserRole::USER, UserRole::VIEWER};
    permissions.push_back(dataView);
    
    // 数据修改权限
    Permission dataModify;
    dataModify.name = "data_modify";
    dataModify.description = "数据修改";
    dataModify.allowedRoles = {UserRole::ADMIN, UserRole::USER};
    permissions.push_back(dataModify);
}

bool AuthManager::validatePasswordStrength(const String& password) {
    if (password.length() < securityConfig.minPasswordLength) {
        return false;
    }
    
    if (password.length() > securityConfig.maxPasswordLength) {
        return false;
    }
    
    if (securityConfig.requireStrongPasswords) {
        // 检查密码复杂度（至少包含数字、字母）
        bool hasDigit = false;
        bool hasAlpha = false;
        
        for (size_t i = 0; i < password.length(); i++) {
            char c = password[i];
            if (isdigit(c)) hasDigit = true;
            if (isAlpha(c)) hasAlpha = true;
        }
        
        if (!hasDigit || !hasAlpha) {
            return false;
        }
    }
    
    return true;
}

String AuthManager::generateSessionId(const String& username) {
    String rawToken = username + ":" + String(micros()) + ":" + generateRandomToken(16);
    return CryptoUtils::simpleHash(rawToken).substring(0, 32);
}

SessionStatus AuthManager::validateSession(const UserSession& session) {
    if (!session.isActive) {
        return SessionStatus::INVALID;
    }
    
    unsigned long currentTime = millis();
    unsigned long sessionAge = currentTime - session.lastAccessTime;
    
    if (sessionAge > securityConfig.sessionTimeout) {
        return SessionStatus::EXPIRED;
    }
    
    return SessionStatus::VALID;
}

void AuthManager::cleanupExpiredSessions() {
    unsigned long currentTime = millis();
    std::vector<String> sessionsToRemove;
    
    // 查找过期会话
    for (const auto& pair : activeSessions) {
        const UserSession& session = pair.second;
        unsigned long sessionAge = currentTime - session.lastAccessTime;
        
        if (sessionAge > securityConfig.sessionTimeout) {
            sessionsToRemove.push_back(pair.first);
        }
    }
    
    // 移除过期会话
    for (const String& sessionId : sessionsToRemove) {
        activeSessions.erase(sessionId);
        if (logger) {
            logger->info("Cleaned up expired session: " + sessionId, "AUTH");
        }
    }
    
    // 清理过期的账户锁定
    std::vector<String> accountsToUnlock;
    for (const auto& pair : lockedAccounts) {
        unsigned long lockTime = pair.second;
        if (currentTime - lockTime > securityConfig.loginLockoutTime) {
            accountsToUnlock.push_back(pair.first);
        }
    }
    
    for (const String& username : accountsToUnlock) {
        lockedAccounts.erase(username);
        loginAttempts[username] = 0;
        if (logger) {
            logger->info("Auto-unlocked account: " + username, "AUTH");
        }
    }
}

AuthResult AuthManager::authenticate(const String& username, const String& password, 
                                    const String& ipAddress, const String& userAgent) {
    AuthResult result;
    
    // 检查账户是否被锁定
    if (isAccountLocked(username)) {
        result.success = false;
        result.message = "Account is temporarily locked due to too many failed login attempts";
        result.status = SessionStatus::LOCKED;
        
        logAuthEvent(username, "LOGIN_FAILED_LOCKED", "Account locked", false);
        return result;
    }
    
    // 验证用户凭据
    if (!userManager || !userManager->authenticateUser(username, password)) {
        // 登录失败
        loginAttempts[username]++;
        
        if (loginAttempts[username] >= securityConfig.maxLoginAttempts) {
            lockAccount(username);
            result.message = "Too many failed login attempts. Account locked for " + 
                           String(securityConfig.loginLockoutTime / 60000) + " minutes";
            result.status = SessionStatus::LOCKED;
        } else {
            result.message = "Invalid username or password. Attempts remaining: " + 
                           String(securityConfig.maxLoginAttempts - loginAttempts[username]);
        }
        
        result.success = false;
        logAuthEvent(username, "LOGIN_FAILED", result.message, false);
        return result;
    }
    
    // 登录成功
    resetLoginAttempts(username);
    
    // 生成新会话
    UserSession newSession;
    newSession.sessionId = generateSessionId(username);
    newSession.username = username;
    newSession.role = getUserRole(username);
    newSession.loginTime = millis();
    newSession.lastAccessTime = newSession.loginTime;
    newSession.ipAddress = ipAddress;
    newSession.userAgent = userAgent;
    newSession.isActive = true;
    
    // 保存会话
    activeSessions[newSession.sessionId] = newSession;
    
    // 设置返回结果
    result.success = true;
    result.message = "Login successful";
    result.sessionId = newSession.sessionId;
    result.status = SessionStatus::VALID;
    result.userRole = newSession.role;
    
    logAuthEvent(username, "LOGIN_SUCCESS", "User logged in from " + ipAddress);
    
    // 触发事件回调
    if (authEventCallback) {
        authEventCallback(username, "login", "Successful login from " + ipAddress);
    }
    
    return result;
}

AuthResult AuthManager::verifySession(const String& sessionId, bool updateAccessTime) {
    AuthResult result;
    
    // 查找会话
    auto it = activeSessions.find(sessionId);
    if (it == activeSessions.end()) {
        result.success = false;
        result.message = "Session not found";
        result.status = SessionStatus::INVALID;
        return result;
    }
    
    UserSession& session = it->second;
    
    // 验证会话状态
    SessionStatus status = validateSession(session);
    if (status != SessionStatus::VALID) {
        result.success = false;
        String statusStr = sessionStatusToString(status);
        statusStr.toLowerCase();
        result.message = "Session " + statusStr;
        result.status = status;
        
        // 移除无效会话
        if (status == SessionStatus::EXPIRED) {
            activeSessions.erase(sessionId);
            logAuthEvent(session.username, "SESSION_EXPIRED", "Session expired");
        }
        
        return result;
    }
    
    // 更新访问时间
    if (updateAccessTime) {
        session.lastAccessTime = millis();
    }
    
    // 返回成功结果
    result.success = true;
    result.message = "Session is valid";
    result.sessionId = sessionId;
    result.status = SessionStatus::VALID;
    result.userRole = session.role;
    
    return result;
}

bool AuthManager::logout(const String& sessionId) {
    auto it = activeSessions.find(sessionId);
    if (it != activeSessions.end()) {
        String username = it->second.username;
        activeSessions.erase(it);
        
        logAuthEvent(username, "LOGOUT", "User logged out");
        
        if (authEventCallback) {
            authEventCallback(username, "logout", "User logged out");
        }
        
        return true;
    }
    
    return false;
}

bool AuthManager::forceLogout(const String& username) {
    bool found = false;
    std::vector<String> sessionsToRemove;
    
    for (const auto& pair : activeSessions) {
        if (pair.second.username == username) {
            sessionsToRemove.push_back(pair.first);
            found = true;
        }
    }
    
    for (const String& sessionId : sessionsToRemove) {
        activeSessions.erase(sessionId);
    }
    
    if (found) {
        logAuthEvent(username, "FORCE_LOGOUT", "All sessions terminated by administrator");
    }
    
    return found;
}

bool AuthManager::checkPermission(const String& username, const String& permission) {
    UserRole userRole = getUserRole(username);
    
    // 查找权限定义
    for (const auto& perm : permissions) {
        if (perm.name == permission) {
            // 检查用户角色是否在允许的角色列表中
            for (UserRole allowedRole : perm.allowedRoles) {
                if (userRole == allowedRole) {
                    return true;
                }
            }
            break;
        }
    }
    
    return false;
}

bool AuthManager::checkSessionPermission(const String& sessionId, const String& permission) {
    AuthResult authResult = verifySession(sessionId, false);
    if (!authResult.success) {
        return false;
    }
    
    return checkPermission(authResult.sessionId, permission);
}

UserRole AuthManager::getUserRole(const String& username) {
    if (!userManager) {
        return UserRole::VIEWER;
    }
    
    // 这里需要UserManager提供获取用户角色的方法
    // 简化实现，实际应该从UserManager获取
    if (username == "admin") {
        return UserRole::ADMIN;
    } else if (username == "user") {
        return UserRole::USER;
    } else {
        return UserRole::VIEWER;
    }
}

UserSession AuthManager::getSessionInfo(const String& sessionId) {
    auto it = activeSessions.find(sessionId);
    if (it != activeSessions.end()) {
        return it->second;
    }
    
    return UserSession(); // 返回空的会话
}

std::vector<UserSession> AuthManager::getActiveSessions() {
    std::vector<UserSession> sessions;
    for (const auto& pair : activeSessions) {
        sessions.push_back(pair.second);
    }
    return sessions;
}

SecurityConfig AuthManager::getSecurityConfig() const {
    return securityConfig;
}

bool AuthManager::updateSecurityConfig(const SecurityConfig& newConfig) {
    securityConfig = newConfig;
    return saveSecurityConfig();
}

void AuthManager::setAuthEventCallback(AuthEventCallback callback) {
    authEventCallback = callback;
}

void AuthManager::resetLoginAttempts(const String& username) {
    loginAttempts[username] = 0;
    lockedAccounts.erase(username);
}

uint8_t AuthManager::getLoginAttempts(const String& username) {
    auto it = loginAttempts.find(username);
    if (it != loginAttempts.end()) {
        return it->second;
    }
    return 0;
}

size_t AuthManager::getActiveSessionCount() {
    return activeSessions.size();
}

std::vector<Permission> AuthManager::getPermissions() const {
    return permissions;
}

void AuthManager::addPermission(const Permission& permission) {
    permissions.push_back(permission);
}

void AuthManager::logAuthEvent(const String& username, const String& event, const String& details, bool success) {
    if (!logger) return;
    
    String logMessage = "Auth event - User: " + username + ", Event: " + event + ", Details: " + details;
    
    if (success) {
        logger->info(logMessage, "AUTH");
    } else {
        logger->warning(logMessage, "AUTH");
    }
}

bool AuthManager::isAccountLocked(const String& username) {
    auto it = lockedAccounts.find(username);
    if (it != lockedAccounts.end()) {
        unsigned long lockTime = it->second;
        if (millis() - lockTime < securityConfig.loginLockoutTime) {
            return true;
        } else {
            // 锁定时间已过，自动解锁
            lockedAccounts.erase(username);
            loginAttempts[username] = 0;
        }
    }
    return false;
}

void AuthManager::lockAccount(const String& username) {
    lockedAccounts[username] = millis();
    logAuthEvent(username, "ACCOUNT_LOCKED", 
                "Account locked due to too many failed login attempts", false);
}

void AuthManager::unlockAccount(const String& username) {
    lockedAccounts.erase(username);
    loginAttempts[username] = 0;
    logAuthEvent(username, "ACCOUNT_UNLOCKED", "Account unlocked by administrator");
}

bool AuthManager::loadSecurityConfig() {
    // 从Preferences加载安全配置
    securityConfig.maxLoginAttempts = preferences.getUChar("max_login_attempts", Security::MAX_LOGIN_ATTEMPTS);
    securityConfig.sessionTimeout = preferences.getULong("session_timeout", Security::SESSION_TIMEOUT);
    securityConfig.loginLockoutTime = preferences.getULong("login_lockout_time", Security::LOGIN_LOCKOUT_TIME);
    securityConfig.minPasswordLength = preferences.getUChar("min_password_len", Security::MIN_PASSWORD_LENGTH);
    securityConfig.maxPasswordLength = preferences.getUChar("max_password_len", Security::MAX_PASSWORD_LENGTH);
    securityConfig.enableBruteForceProtection = preferences.getBool("brute_force_protection", true);
    securityConfig.enableSessionManagement = preferences.getBool("session_management", true);
    securityConfig.requireStrongPasswords = preferences.getBool("strong_passwords", false);
    
    return true;
}

bool AuthManager::saveSecurityConfig() {
    // 保存安全配置到Preferences
    preferences.putUChar("max_login_attempts", securityConfig.maxLoginAttempts);
    preferences.putULong("session_timeout", securityConfig.sessionTimeout);
    preferences.putULong("login_lockout_time", securityConfig.loginLockoutTime);
    preferences.putUChar("min_password_len", securityConfig.minPasswordLength);
    preferences.putUChar("max_password_len", securityConfig.maxPasswordLength);
    preferences.putBool("brute_force_protection", securityConfig.enableBruteForceProtection);
    preferences.putBool("session_management", securityConfig.enableSessionManagement);
    preferences.putBool("strong_passwords", securityConfig.requireStrongPasswords);
    
    return preferences.isKey("max_login_attempts"); // 简单验证是否保存成功
}

// 静态工具方法实现
String AuthManager::roleToString(UserRole role) {
    switch (role) {
        case UserRole::ADMIN: return "admin";
        case UserRole::USER: return "user";
        case UserRole::VIEWER: return "viewer";
        default: return "unknown";
    }
}

UserRole AuthManager::stringToRole(const String& roleStr) {
    if (roleStr == "admin") return UserRole::ADMIN;
    if (roleStr == "user") return UserRole::USER;
    if (roleStr == "viewer") return UserRole::VIEWER;
    return UserRole::VIEWER; // 默认
}

String AuthManager::sessionStatusToString(SessionStatus status) {
    switch (status) {
        case SessionStatus::VALID: return "VALID";
        case SessionStatus::EXPIRED: return "EXPIRED";
        case SessionStatus::INVALID: return "INVALID";
        case SessionStatus::LOCKED: return "LOCKED";
        default: return "UNKNOWN";
    }
}

String AuthManager::generateRandomToken(uint8_t length) {
    String token = "";
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    
    for (int i = 0; i < length; i++) {
        token += chars[random(0, 62)];
    }
    
    return token;
}

String AuthManager::hashPassword(const String& password, const String& salt) {
    // 使用CryptoUtils进行密码哈希
    String data = password + salt;
    return CryptoUtils::simpleHash(data);
}