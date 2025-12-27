#include "./security/AuthManager.h"
#include <esp_random.h>

// 前向声明LoggerSystem的空实现（如果未提供）
#ifndef LOGGER_SYSTEM_H
class LoggerSystem {
public:
    void log(const String& msg) { Serial.println(msg); }
    void error(const String& msg) { Serial.println("ERROR: " + msg); }
    void info(const String& msg) { Serial.println("INFO: " + msg); }
};
#endif

AuthManager::AuthManager(UserManager* userMgr, LoggerSystem* loggerPtr)
    : userManager(userMgr), logger(loggerPtr), lastSessionCleanup(0) {
    preferences.begin("auth_manager", false);
}

AuthManager::~AuthManager() {
    if (securityConfig.enableSessionPersistence) {
        saveSessionsToStorage();
    }
    preferences.end();
}

bool AuthManager::initialize() {
    // 加载安全配置
    if (!loadSecurityConfig()) {
        Serial.println("[AuthManager] Using default security config");
    }
    
    // 初始化权限系统
    initializePermissions();
    
    // 加载持久化的会话
    if (securityConfig.enableSessionPersistence) {
        loadSessionsFromStorage();
    }
    
    Serial.printf("[AuthManager] Initialized with %d permissions, %d active sessions\n", 
                 permissions.size(), activeSessions.size());
    
    return true;
}

void AuthManager::initializePermissions() {
    // 系统权限
    permissions["system.info"] = Permission("system.info", "查看系统信息", UserRole::VIEWER);
    permissions["system.restart"] = Permission("system.restart", "重启系统", UserRole::ADMIN);
    
    // 用户管理权限
    permissions["user.view"] = Permission("user.view", "查看用户", UserRole::VIEWER);
    permissions["user.create"] = Permission("user.create", "创建用户", UserRole::ADMIN);
    permissions["user.edit"] = Permission("user.edit", "编辑用户", UserRole::ADMIN);
    permissions["user.delete"] = Permission("user.delete", "删除用户", UserRole::ADMIN);
    
    // 配置权限
    permissions["config.view"] = Permission("config.view", "查看配置", UserRole::VIEWER);
    permissions["config.edit"] = Permission("config.edit", "编辑配置", UserRole::ADMIN);
    
    // 网络权限
    permissions["network.view"] = Permission("network.view", "查看网络状态", UserRole::VIEWER);
    permissions["network.edit"] = Permission("network.edit", "编辑网络配置", UserRole::ADMIN);
    
    // OTA权限
    permissions["ota.update"] = Permission("ota.update", "OTA更新", UserRole::ADMIN);
    
    // 文件系统权限
    permissions["fs.view"] = Permission("fs.view", "查看文件系统", UserRole::VIEWER);
    permissions["fs.manage"] = Permission("fs.manage", "管理文件系统", UserRole::ADMIN);
}

String AuthManager::generateSessionId(const String& username) {
    // 组合多个随机源增加唯一性
    unsigned long timestamp = millis();
    uint32_t random1 = esp_random();
    uint32_t random2 = esp_random();
    
    // 创建唯一字符串
    String uniqueString = username + "_" + 
                         String(timestamp) + "_" + 
                         String(random1, HEX) + "_" + 
                         String(random2, HEX);
    
    // 计算 MD5 哈希
    MD5Builder md5;
    md5.begin();
    md5.add(uniqueString);
    md5.calculate();
    
    return md5.toString();
}

SessionStatus AuthManager::validateSession(const UserSession& session) {
    if (!session.isActive) {
        return SessionStatus::INVALID;
    }
    
    unsigned long currentTime = millis();
    
    // 检查会话是否过期
    if (currentTime > session.expireTime) {
        return SessionStatus::EXPIRED;
    }
    
    // 检查账户是否被锁定
    if (isAccountLocked(session.username)) {
        return SessionStatus::LOCKED;
    }
    
    // 检查用户是否仍然存在和启用
    if (userManager) {
        User* user = userManager->getUser(session.username);
        if (!user || !user->enabled) {
            return SessionStatus::INVALID;
        }
    }
    
    return SessionStatus::VALID;
}

void AuthManager::cleanupExpiredSessions() {
    unsigned long currentTime = millis();
    
    for (auto it = activeSessions.begin(); it != activeSessions.end();) {
        if (currentTime > it->second.expireTime || !it->second.isActive) {
            logAuthEvent("session_expired", it->second.username, 
                        "Session expired or invalidated", true);
            it = activeSessions.erase(it);
        } else {
            ++it;
        }
    }
}

bool AuthManager::isAccountLocked(const String& username) {
    auto it = lockedAccounts.find(username);
    if (it != lockedAccounts.end()) {
        if (userManager) {
            User* user = userManager->getUser(username);
            if (user) {
                // UserManager::UserConfig config;
                // 假设UserManager有获取配置的方法
                // 这里简化处理，实际应该从配置获取
                unsigned long lockoutTime = 300000; // 5分钟
                
                if (millis() - it->second < lockoutTime) {
                    return true;
                } else {
                    // 锁定时间已过，解锁账户
                    lockedAccounts.erase(it);
                    userManager->resetLoginAttempts(username);
                    return false;
                }
            }
        }
    }
    return false;
}

void AuthManager::lockAccount(const String& username) {
    lockedAccounts[username] = millis();
    logAuthEvent("account_locked", username, "Too many failed login attempts", false);
}

void AuthManager::unlockAccount(const String& username) {
    lockedAccounts.erase(username);
    if (userManager) {
        userManager->resetLoginAttempts(username);
    }
    logAuthEvent("account_unlocked", username, "Account unlocked", true);
}

void AuthManager::logAuthEvent(const String& event, const String& username, 
                              const String& details, bool success) {
    String logMsg = "AuthEvent: " + event + " - User: " + username + 
                   " - Details: " + details + " - Success: " + String(success);
    
    if (logger) {
        if (success) {
            logger->info(logMsg);
        } else {
            logger->error(logMsg);
        }
    } else {
        Serial.println(logMsg);
    }
    
    // 触发回调
    if (authEventCallback) {
        authEventCallback(event, username, details, success);
    }
}

bool AuthManager::checkIpWhitelist(const String& ipAddress) {
    if (!securityConfig.enableIpWhitelist || securityConfig.ipWhitelist.empty()) {
        return true;
    }
    
    for (const String& ip : securityConfig.ipWhitelist) {
        if (ipAddress == ip) {
            return true;
        }
    }
    
    return false;
}

bool AuthManager::loadSecurityConfig() {
    securityConfig.sessionTimeout = preferences.getULong("session_timeout", 3600000);
    securityConfig.sessionCleanupInterval = preferences.getULong("cleanup_interval", 60000);
    securityConfig.enableSessionPersistence = preferences.getBool("session_persistence", true);
    securityConfig.enableIpWhitelist = preferences.getBool("ip_whitelist", false);
    securityConfig.cookieName = preferences.getString("cookie_name", "session");
    securityConfig.cookieMaxAge = preferences.getULong("cookie_max_age", 3600);
    securityConfig.cookieHttpOnly = preferences.getBool("cookie_http_only", true);
    securityConfig.cookieSecure = preferences.getBool("cookie_secure", false);
    
    // 加载IP白名单
    String ipListStr = preferences.getString("ip_whitelist", "");
    if (!ipListStr.isEmpty()) {
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, ipListStr);
        if (!error) {
            JsonArray ipArray = doc.as<JsonArray>();
            for (String ip : ipArray) {
                securityConfig.ipWhitelist.push_back(ip);
            }
        }
    }
    
    return true;
}

bool AuthManager::saveSecurityConfig() {
    preferences.putULong("session_timeout", securityConfig.sessionTimeout);
    preferences.putULong("cleanup_interval", securityConfig.sessionCleanupInterval);
    preferences.putBool("session_persistence", securityConfig.enableSessionPersistence);
    preferences.putBool("ip_whitelist", securityConfig.enableIpWhitelist);
    preferences.putString("cookie_name", securityConfig.cookieName);
    preferences.putULong("cookie_max_age", securityConfig.cookieMaxAge);
    preferences.putBool("cookie_http_only", securityConfig.cookieHttpOnly);
    preferences.putBool("cookie_secure", securityConfig.cookieSecure);
    
    // 保存IP白名单
    StaticJsonDocument<512> doc;
    JsonArray ipArray = doc.to<JsonArray>();
    for (const String& ip : securityConfig.ipWhitelist) {
        ipArray.add(ip);
    }
    
    String ipListStr;
    serializeJson(doc, ipListStr);
    preferences.putString("ip_whitelist", ipListStr);
    
    return true;
}

// ============ 认证管理方法 ============

AuthResult AuthManager::login(const String& username, const String& password,
                             const String& ipAddress, const String& userAgent) {
    AuthResult result;
    result.username = username;
    
    // 检查IP白名单
    if (!checkIpWhitelist(ipAddress)) {
        result.message = "IP address not allowed";
        result.status = SessionStatus::INVALID;
        logAuthEvent("login_failed_ip", username, "IP not in whitelist: " + ipAddress, false);
        return result;
    }
    
    // 检查账户是否被锁定
    if (isAccountLocked(username)) {
        result.message = "Account is locked";
        result.status = SessionStatus::LOCKED;
        logAuthEvent("login_failed_locked", username, "Account locked", false);
        return result;
    }
    
    // 验证用户凭据
    if (!userManager || !userManager->authenticateUser(username, password)) {
        result.message = "Invalid username or password";
        result.status = SessionStatus::INVALID;
        
        // 记录登录失败
        if (userManager) {
            uint8_t attempts = userManager->getLoginAttempts(username);
            User* user = userManager->getUser(username);
            if (user) {
                // 检查是否达到最大尝试次数
                // 这里简化处理，实际应该从配置获取
                if (attempts >= 5) {
                    lockAccount(username);
                    result.message = "Too many failed attempts, account locked";
                    result.status = SessionStatus::LOCKED;
                }
            }
        }
        
        logAuthEvent("login_failed", username, "Invalid credentials", false);
        return result;
    }
    
    // 获取用户角色
    UserRole role = UserRole::VIEWER;
    if (userManager) {
        role = userManager->getUserRole(username);
    }
    
    // 检查是否允许多会话
    if (!securityConfig.enableSessionPersistence) {
        // 如果不允许多会话，登出该用户的其他会话
        forceLogout(username);
    }
    
    // 创建新会话
    UserSession session;
    session.sessionId = generateSessionId(username);
    session.username = username;
    session.role = role;
    session.loginTime = millis();
    session.lastAccessTime = session.loginTime;
    session.expireTime = session.loginTime + securityConfig.sessionTimeout;
    session.ipAddress = ipAddress;
    session.userAgent = userAgent;
    session.isActive = true;
    
    // 保存会话
    activeSessions[session.sessionId] = session;
    
    // 准备返回结果
    result.success = true;
    result.message = "Login successful";
    result.sessionId = session.sessionId;
    result.status = SessionStatus::VALID;
    result.userRole = role;
    
    logAuthEvent("login_success", username, "User logged in from " + ipAddress, true);
    
    return result;
}

AuthResult AuthManager::verifySession(const String& sessionId, bool updateAccessTime) {
    AuthResult result;
    
    // 定期清理过期会话
    unsigned long currentTime = millis();
    if (currentTime - lastSessionCleanup > securityConfig.sessionCleanupInterval) {
        cleanupExpiredSessions();
        lastSessionCleanup = currentTime;
    }
    
    // 查找会话
    auto it = activeSessions.find(sessionId);
    if (it == activeSessions.end()) {
        result.message = "Session not found";
        result.status = SessionStatus::INVALID;
        return result;
    }
    
    UserSession& session = it->second;
    result.username = session.username;
    
    // 验证会话状态
    SessionStatus status = validateSession(session);
    if (status != SessionStatus::VALID) {
        result.message = "Session " + sessionStatusToString(status);
        result.status = status;
        result.userRole = session.role;
        
        // 如果会话无效，删除它
        if (status == SessionStatus::EXPIRED || status == SessionStatus::INVALID) {
            activeSessions.erase(it);
        }
        
        logAuthEvent("session_invalid", session.username, result.message, false);
        return result;
    }
    
    // 更新访问时间
    if (updateAccessTime) {
        session.lastAccessTime = currentTime;
        session.expireTime = currentTime + securityConfig.sessionTimeout;
    }
    
    // 返回验证结果
    result.success = true;
    result.message = "Session valid";
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
        
        logAuthEvent("logout", username, "User logged out", true);
        
        // 持久化会话数据
        if (securityConfig.enableSessionPersistence) {
            saveSessionsToStorage();
        }
        
        return true;
    }
    return false;
}

bool AuthManager::forceLogout(const String& username) {
    bool found = false;
    
    for (auto it = activeSessions.begin(); it != activeSessions.end();) {
        if (it->second.username == username) {
            it = activeSessions.erase(it);
            found = true;
        } else {
            ++it;
        }
    }
    
    if (found) {
        logAuthEvent("force_logout", username, "User forced logout", true);
        
        if (securityConfig.enableSessionPersistence) {
            saveSessionsToStorage();
        }
    }
    
    return found;
}

size_t AuthManager::forceLogoutAll() {
    size_t count = activeSessions.size();
    activeSessions.clear();
    
    logAuthEvent("logout_all", "system", "All users logged out", true);
    
    if (securityConfig.enableSessionPersistence) {
        saveSessionsToStorage();
    }
    
    return count;
}

// ============ 会话管理方法 ============

UserSession AuthManager::getSessionInfo(const String& sessionId) {
    auto it = activeSessions.find(sessionId);
    if (it != activeSessions.end()) {
        return it->second;
    }
    return UserSession();
}

std::vector<UserSession> AuthManager::getActiveSessions() {
    std::vector<UserSession> sessions;
    
    for (const auto& pair : activeSessions) {
        sessions.push_back(pair.second);
    }
    
    return sessions;
}

std::vector<UserSession> AuthManager::getUserSessions(const String& username) {
    std::vector<UserSession> userSessions;
    
    for (const auto& pair : activeSessions) {
        if (pair.second.username == username) {
            userSessions.push_back(pair.second);
        }
    }
    
    return userSessions;
}

bool AuthManager::updateSessionAccessTime(const String& sessionId) {
    auto it = activeSessions.find(sessionId);
    if (it != activeSessions.end()) {
        unsigned long currentTime = millis();
        it->second.lastAccessTime = currentTime;
        it->second.expireTime = currentTime + securityConfig.sessionTimeout;
        return true;
    }
    return false;
}

size_t AuthManager::getActiveSessionCount() {
    return activeSessions.size();
}

// ============ 权限管理方法 ============

bool AuthManager::checkPermission(const String& username, const String& permission, 
                                 const String& method) {
    // 检查权限是否存在
    auto permIt = permissions.find(permission);
    if (permIt == permissions.end()) {
        return false;
    }
    
    // 获取用户角色
    UserRole userRole = getUserRole(username);
    
    // 检查角色权限
    if (static_cast<int>(userRole) < static_cast<int>(permIt->second.minRole)) {
        return false;
    }
    
    // 检查HTTP方法（如果指定）
    if (!method.isEmpty() && !permIt->second.allowedMethods.empty()) {
        bool methodAllowed = false;
        for (const String& allowedMethod : permIt->second.allowedMethods) {
            if (method.equalsIgnoreCase(allowedMethod)) {
                methodAllowed = true;
                break;
            }
        }
        if (!methodAllowed) {
            return false;
        }
    }
    
    return true;
}

bool AuthManager::checkSessionPermission(const String& sessionId, const String& permission,
                                        const String& method) {
    // 验证会话
    AuthResult authResult = verifySession(sessionId, false);
    if (!authResult.success) {
        return false;
    }
    
    // 检查权限
    return checkPermission(authResult.username, permission, method);
}

void AuthManager::addPermission(const Permission& permission) {
    permissions[permission.name] = permission;
}

void AuthManager::removePermission(const String& permissionName) {
    permissions.erase(permissionName);
}

std::vector<Permission> AuthManager::getAllPermissions() {
    std::vector<Permission> permList;
    
    for (const auto& pair : permissions) {
        permList.push_back(pair.second);
    }
    
    return permList;
}

UserRole AuthManager::getUserRole(const String& username) {
    if (userManager) {
        return userManager->getUserRole(username);
    }
    return UserRole::VIEWER;
}

// ============ 在线用户管理 ============

std::vector<String> AuthManager::getOnlineUsers() {
    std::vector<String> onlineUsers;
    
    for (const auto& pair : activeSessions) {
        // 去重
        bool found = false;
        for (const String& user : onlineUsers) {
            if (user == pair.second.username) {
                found = true;
                break;
            }
        }
        
        if (!found) {
            onlineUsers.push_back(pair.second.username);
        }
    }
    
    return onlineUsers;
}

bool AuthManager::isUserOnline(const String& username) {
    for (const auto& pair : activeSessions) {
        if (pair.second.username == username) {
            return true;
        }
    }
    return false;
}

size_t AuthManager::getOnlineUserCount() {
    return getOnlineUsers().size();
}

// ============ 配置管理 ============

SecurityConfig AuthManager::getSecurityConfig() const {
    return securityConfig;
}

bool AuthManager::updateSecurityConfig(const SecurityConfig& newConfig) {
    securityConfig = newConfig;
    return saveSecurityConfig();
}

// ============ 事件回调 ============

void AuthManager::setAuthEventCallback(AuthEventCallback callback) {
    authEventCallback = callback;
}

// ============ 系统维护 ============

void AuthManager::performMaintenance() {
    cleanupExpiredSessions();
    lastSessionCleanup = millis();
}

// ============ 持久化方法 ============

bool AuthManager::saveSessionsToStorage() {
    if (!securityConfig.enableSessionPersistence) {
        return true;
    }
    
    StaticJsonDocument<4096> doc;
    JsonArray sessionsArray = doc.createNestedArray("sessions");
    
    for (const auto& pair : activeSessions) {
        const UserSession& session = pair.second;
        JsonObject sessionObj = sessionsArray.createNestedObject();
        
        sessionObj["sessionId"] = session.sessionId;
        sessionObj["username"] = session.username;
        sessionObj["role"] = static_cast<int>(session.role);
        sessionObj["loginTime"] = session.loginTime;
        sessionObj["lastAccessTime"] = session.lastAccessTime;
        sessionObj["expireTime"] = session.expireTime;
        sessionObj["ipAddress"] = session.ipAddress;
        sessionObj["userAgent"] = session.userAgent;
        sessionObj["isActive"] = session.isActive;
    }
    
    String jsonStr;
    serializeJson(doc, jsonStr);
    
    bool success = preferences.putString("sessions", jsonStr) > 0;
    if (success) {
        Serial.println("[AuthManager] Sessions saved to storage");
    }
    
    return success;
}

bool AuthManager::loadSessionsFromStorage() {
    if (!securityConfig.enableSessionPersistence) {
        return true;
    }
    
    String jsonStr = preferences.getString("sessions", "");
    if (jsonStr.isEmpty()) {
        return true; // 没有会话数据是正常的
    }
    
    StaticJsonDocument<4096> doc;
    DeserializationError error = deserializeJson(doc, jsonStr);
    if (error) {
        Serial.printf("[AuthManager] Failed to parse session data: %s\n", error.c_str());
        return false;
    }
    
    activeSessions.clear();
    
    JsonArray sessionsArray = doc["sessions"];
    for (JsonObject sessionObj : sessionsArray) {
        UserSession session;
        session.sessionId = sessionObj["sessionId"].as<String>();
        session.username = sessionObj["username"].as<String>();
        session.role = static_cast<UserRole>(sessionObj["role"].as<int>());
        session.loginTime = sessionObj["loginTime"];
        session.lastAccessTime = sessionObj["lastAccessTime"];
        session.expireTime = sessionObj["expireTime"];
        session.ipAddress = sessionObj["ipAddress"].as<String>();
        session.userAgent = sessionObj["userAgent"].as<String>();
        session.isActive = sessionObj["isActive"];
        
        // 只加载未过期的会话
        if (session.isActive && millis() < session.expireTime) {
            activeSessions[session.sessionId] = session;
        }
    }
    
    Serial.printf("[AuthManager] Loaded %d sessions from storage\n", activeSessions.size());
    return true;
}

// ============ 静态工具方法 ============

String AuthManager::sessionStatusToString(SessionStatus status) {
    switch (status) {
        case SessionStatus::VALID: return "valid";
        case SessionStatus::EXPIRED: return "expired";
        case SessionStatus::INVALID: return "invalid";
        case SessionStatus::LOCKED: return "locked";
        default: return "unknown";
    }
}

String AuthManager::generateRandomToken(uint8_t length) {
    const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    String token;
    
    for (uint8_t i = 0; i < length; i++) {
        uint32_t randomValue = esp_random();
        token += charset[randomValue % (sizeof(charset) - 1)];
    }
    
    return token;
}

String AuthManager::extractSessionIdFromRequest(AsyncWebServerRequest* request, 
                                               const String& cookieName) {
    if (!request) return "";
    
    // 1. 从Cookie获取
    if (request->hasHeader("Cookie")) {
        String cookieHeader = request->header("Cookie");
        int start = cookieHeader.indexOf(cookieName + "=");
        if (start != -1) {
            start += cookieName.length() + 1;
            int end = cookieHeader.indexOf(';', start);
            if (end == -1) end = cookieHeader.length();
            return cookieHeader.substring(start, end);
        }
    }
    
    // 2. 从Authorization头获取
    if (request->hasHeader("Authorization")) {
        String authHeader = request->header("Authorization");
        if (authHeader.startsWith("Bearer ")) {
            return authHeader.substring(7);
        }
    }
    
    // 3. 从查询参数获取
    if (request->hasParam("session")) {
        return request->getParam("session")->value();
    }
    
    // 4. 从表单数据获取
    if (request->hasParam("session", true)) {
        return request->getParam("session", true)->value();
    }
    
    return "";
}