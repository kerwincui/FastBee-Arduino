#include "./security/AuthManager.h"
#include "systems/LoggerSystem.h"
#include "core/FeatureFlags.h"
#include <esp_random.h>

#if FASTBEE_ENABLE_AUTH

AuthManager::AuthManager(IUserManager* userMgr, RoleManager* roleMgr, LoggerSystem* loggerPtr)
    : userManager(userMgr), roleManager(roleMgr), logger(loggerPtr), lastSessionCleanup(0) {
    sessionPrefs.begin("auth_sessions", false);  // 仅用于会话持久化
}

AuthManager::~AuthManager() {
    if (securityConfig.enableSessionPersistence) {
        saveSessionsToStorage();
    }
    sessionPrefs.end();
}

bool AuthManager::initialize() {
    LOG_INFO("AuthManager: Initializing...");
    
    if (!loadSecurityConfig()) {
        LOG_INFO("AuthManager: Using default security config");
    } else {
        LOG_INFO("AuthManager: Security config loaded");
    }

    LOG_DEBUG("AuthManager: Initializing permissions...");
    initializePermissions();
    LOG_DEBUGF("AuthManager: %u permissions registered", (unsigned)permissions.size());

    if (securityConfig.enableSessionPersistence) {
        LOG_DEBUG("AuthManager: Loading sessions from storage...");
        loadSessionsFromStorage();
    }

    char buf[80];
    snprintf(buf, sizeof(buf), "AuthManager: Initialized permissions=%u sessions=%u",
             (unsigned)permissions.size(), (unsigned)activeSessions.size());
    LOG_INFO(buf);
    return true;
}

void AuthManager::initializePermissions() {
    // 系统权限
    permissions["system.info"] = Permission("system.info", "查看系统信息", UserRole::VIEWER);
    permissions["system.restart"] = Permission("system.restart", "重启系统", UserRole::USER);  // 操作员可重启
    
    // 用户管理权限
    permissions["user.view"] = Permission("user.view", "查看用户", UserRole::VIEWER);
    permissions["user.create"] = Permission("user.create", "创建用户", UserRole::ADMIN);
    permissions["user.edit"] = Permission("user.edit", "编辑用户", UserRole::ADMIN);
    permissions["user.delete"] = Permission("user.delete", "删除用户", UserRole::ADMIN);
    
    // 角色管理权限
    permissions["role.view"] = Permission("role.view", "查看角色", UserRole::VIEWER);
    permissions["role.create"] = Permission("role.create", "创建角色", UserRole::ADMIN);
    permissions["role.edit"] = Permission("role.edit", "编辑角色", UserRole::ADMIN);
    permissions["role.delete"] = Permission("role.delete", "删除角色", UserRole::ADMIN);
    
    // 配置权限
    permissions["config.view"] = Permission("config.view", "查看配置", UserRole::VIEWER);
    permissions["config.edit"] = Permission("config.edit", "编辑配置", UserRole::USER);  // 操作员可编辑
    
    // 网络权限
    permissions["network.view"] = Permission("network.view", "查看网络状态", UserRole::VIEWER);
    permissions["network.edit"] = Permission("network.edit", "编辑网络配置", UserRole::USER);  // 操作员可编辑
    
    // 设备控制权限
    permissions["device.view"] = Permission("device.view", "查看设备状态", UserRole::VIEWER);
    permissions["device.control"] = Permission("device.control", "控制设备", UserRole::USER);  // 操作员可控制
    
    // OTA权限
    permissions["ota.update"] = Permission("ota.update", "OTA更新", UserRole::USER);  // 操作员可升级
    
    // 文件系统权限
    permissions["fs.view"] = Permission("fs.view", "查看文件系统", UserRole::VIEWER);
    permissions["fs.manage"] = Permission("fs.manage", "管理文件系统", UserRole::ADMIN);
    
    // 审计日志权限
    permissions["audit.view"] = Permission("audit.view", "查看审计日志", UserRole::VIEWER);
}

String AuthManager::generateSessionId(const String& username) {
    // 使用 32 字节密码学安全随机数，Base64 编码后生成 44 字符高熵 token
    // 相比原来的 MD5(username+millis+random)，抗猜测/碰撞能力大幅提升
    uint8_t randomBytes[32];
    esp_fill_random(randomBytes, sizeof(randomBytes));
    
    // Base64 编码（标准字母表，输出 44 字符）
    static const char* b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String token;
    token.reserve(44);
    for (int i = 0; i < 30; i += 3) {
        uint32_t n = ((uint32_t)randomBytes[i] << 16) | ((uint32_t)randomBytes[i+1] << 8) | randomBytes[i+2];
        token += b64chars[(n >> 18) & 0x3F];
        token += b64chars[(n >> 12) & 0x3F];
        token += b64chars[(n >>  6) & 0x3F];
        token += b64chars[(n      ) & 0x3F];
    }
    // 处理最后 2 字节 (32 mod 3 = 2)
    uint32_t n = ((uint32_t)randomBytes[30] << 16) | ((uint32_t)randomBytes[31] << 8);
    token += b64chars[(n >> 18) & 0x3F];
    token += b64chars[(n >> 12) & 0x3F];
    token += b64chars[(n >>  6) & 0x3F];
    token += '=';
    
    return token;
}

SessionStatus AuthManager::validateSession(const UserSession& session) {
    SessionStatus status;
    if (!session.isActive) {
        status.valid = false;
        status.expired = false;
        status.locked = false;
        status.username = session.username;
        return status;
    }
    
    unsigned long currentTime = millis();
    
    // 检查会话是否过期
    if (currentTime > session.expireTime) {
        status.valid = false;
        status.expired = true;
        status.locked = false;
        status.username = session.username;
        return status;
    }
    
    // 检查账户是否被锁定
    if (isAccountLocked(session.username)) {
        status.valid = false;
        status.expired = false;
        status.locked = true;
        status.username = session.username;
        return status;
    }
    
    // 检查用户是否仍然存在和启用
    if (userManager) {
        User* user = userManager->getUser(session.username);
        if (!user || !user->enabled) {
            status.valid = false;
            status.expired = false;
            status.locked = false;
            status.username = session.username;
            return status;
        }
    }
    
    status.valid = true;
    status.expired = false;
    status.locked = false;
    status.username = session.username;
    return status;
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
                if (millis() - it->second < Security::LOGIN_LOCKOUT_TIME) {
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
        LOG_INFO(logMsg.c_str());
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
    // 从 UserManager 获取安全配置（配置统一存储在 users.json）
    if (userManager) {
        UserManager* um = static_cast<UserManager*>(userManager);
        securityConfig.sessionTimeout = um->getSessionTimeout();
        securityConfig.sessionCleanupInterval = um->getSessionCleanupInterval();
        securityConfig.enableSessionPersistence = um->isSessionPersistenceEnabled();
        securityConfig.cookieName = um->getCookieName();
        securityConfig.cookieMaxAge = um->getCookieMaxAge();
        securityConfig.cookieHttpOnly = um->isCookieHttpOnly();
        securityConfig.cookieSecure = um->isCookieSecure();
    }
    
    // IP白名单保持在内存中（运行时配置，不持久化）
    securityConfig.enableIpWhitelist = false;
    securityConfig.ipWhitelist.clear();
    
    return true;
}

bool AuthManager::saveSecurityConfig() {
    // 委托给 UserManager 保存（配置统一存储在 users.json）
    if (userManager) {
        UserManager* um = static_cast<UserManager*>(userManager);
        return um->updateSecurityConfig(
            securityConfig.sessionTimeout,
            securityConfig.sessionCleanupInterval,
            securityConfig.enableSessionPersistence,
            securityConfig.cookieName,
            securityConfig.cookieMaxAge,
            securityConfig.cookieHttpOnly,
            securityConfig.cookieSecure
        );
    }
    return false;
}

// ============ 认证管理方法 ============

AuthResult AuthManager::login(const String& username, const String& password,
                             const String& ipAddress, const String& userAgent) {
    AuthResult result;
    result.success = false;
    result.username = username;
    
    // 检查IP白名单
    if (!checkIpWhitelist(ipAddress)) {
        result.errorMessage = "IP address not allowed";
        logAuthEvent("login_failed_ip", username, "IP not in whitelist: " + ipAddress, false);
        return result;
    }
    
    // 检查账户是否被锁定
    if (isAccountLocked(username)) {
        result.errorMessage = "Account is locked";
        logAuthEvent("login_failed_locked", username, "Account locked", false);
        return result;
    }
    
    // 验证用户凭据
    if (!userManager || !userManager->validateUser(username, password)) {
        result.errorMessage = "Invalid username or password";
        
        // 记录登录失败，检查是否超过最大尝试次数
        if (userManager) {
            uint8_t attempts = userManager->getLoginAttempts(username);
            User* user = userManager->getUser(username);
            if (user) {
                if (attempts >= Security::MAX_LOGIN_ATTEMPTS) {
                    lockAccount(username);
                    result.errorMessage = "Too many failed attempts, account locked";
                }
            }
        }
        
        logAuthEvent("login_failed", username, "Invalid credentials", false);
        appendAuditEntry(username, "auth.login", username, "Login failed: invalid credentials", false, ipAddress);
        return result;
    }
    
    // 获取用户角色
    String roleStr = "VIEWER";
    if (userManager) {
        roleStr = userManager->getUserRole(username);
    }
    
    // 检查是否允许多会话
    // 嵌入式设备场景下，同一用户登录时清理旧会话，避免会话无限累积
    forceLogout(username);
    
    // 创建新会话
    UserSession session;
    session.sessionId = generateSessionId(username);
    session.username = username;
    session.role = UserManager::stringToRole(roleStr);
    session.loginTime = millis();
    session.lastAccessTime = session.loginTime;
    session.expireTime = session.loginTime + securityConfig.sessionTimeout;
    session.ipAddress = ipAddress;
    session.userAgent = userAgent;
    session.isActive = true;
    
    // 保存会话
    activeSessions[session.sessionId] = session;
    
    // 持久化会话到 NVS（保证重启后会话恢复）
    if (securityConfig.enableSessionPersistence) {
        saveSessionsToStorage();
    }
    
    // 准备返回结果
    result.success = true;
    result.sessionId = session.sessionId;
    
    logAuthEvent("login_success", username, "User logged in from " + ipAddress, true);
    appendAuditEntry(username, "auth.login", username, "Login from " + ipAddress, true, ipAddress);

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
        result.errorMessage = "Session not found";
        return result;
    }
    
    UserSession& session = it->second;
    result.username = session.username;
    
    // 验证会话状态
    SessionStatus status = validateSession(session);
    if (!status.valid) {
        if (status.expired) {
            result.errorMessage = "Session expired";
        } else if (status.locked) {
            result.errorMessage = "Account locked";
        } else {
            result.errorMessage = "Session invalid";
        }
        
        // 如果会话无效，删除它
        if (status.expired || !status.valid) {
            activeSessions.erase(it);
        }
        
        logAuthEvent("session_invalid", session.username, result.errorMessage, false);
        return result;
    }
    
    // 更新访问时间
    if (updateAccessTime) {
        session.lastAccessTime = currentTime;
        session.expireTime = currentTime + securityConfig.sessionTimeout;
    }
    
    // 返回验证结果
    result.success = true;
    result.sessionId = sessionId;
    
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

// ============ 角色管理集成 ============

void AuthManager::setRoleManager(RoleManager* roleMgr) {
    roleManager = roleMgr;
}

// ============ 审计日志 ============

void AuthManager::appendAuditEntry(const String& username, const String& action,
                                   const String& resource, const String& detail,
                                   bool success, const String& ip) {
    AuditEntry entry;
    entry.timestamp = millis();
    entry.username  = username;
    entry.action    = action;
    entry.resource  = resource;
    entry.detail    = detail;
    entry.success   = success;
    entry.ipAddress = ip;

    // 环形缓冲：超过上限时删除最旧条目
    if (auditLog.size() >= MAX_AUDIT_ENTRIES) {
        auditLog.erase(auditLog.begin());
    }
    auditLog.push_back(entry);
}

void AuthManager::recordAudit(const String& username, const String& action,
                              const String& resource, const String& detail,
                              bool success, const String& ip) {
    appendAuditEntry(username, action, resource, detail, success, ip);
}

std::vector<AuditEntry> AuthManager::getAuditLog() const {
    // 返回副本，新的在前
    std::vector<AuditEntry> result(auditLog.rbegin(), auditLog.rend());
    return result;
}

String AuthManager::getAuditLogJson(size_t limit) const {
    FastBeeJsonDocLarge doc;
    JsonArray arr = doc["logs"].to<JsonArray>();

    size_t count = 0;
    // 从最新到最旧
    for (auto it = auditLog.rbegin(); it != auditLog.rend(); ++it) {
        if (limit > 0 && count >= limit) break;
        JsonObject obj = arr.add<JsonObject>();
        obj["timestamp"] = it->timestamp;
        obj["username"]  = it->username;
        obj["action"]    = it->action;
        obj["resource"]  = it->resource;
        obj["detail"]    = it->detail;
        obj["success"]   = it->success;
        obj["ip"]        = it->ipAddress;
        count++;
    }

    String out;
    serializeJson(doc, out);
    return out;
}

void AuthManager::clearAuditLog() {
    auditLog.clear();
    LOG_INFO("AuthManager: Audit log cleared");
}

// ============ 权限管理方法 ============

bool AuthManager::checkPermission(const String& username, const String& permission,
                                 const String& method) {
    // 权限缓存查询（仅对无 method 约束的默认检查）
    if (method.isEmpty()) {
        unsigned long now = millis();
        for (uint8_t i = 0; i < PERM_CACHE_SIZE; i++) {
            if (_permCache[i].username == username &&
                _permCache[i].permission == permission &&
                (now - _permCache[i].timestamp) < PERM_CACHE_TTL) {
                return _permCache[i].result;
            }
        }
    }

    // 优先使用 RoleManager 进行多角色权限校验
    bool result = false;
    if (roleManager && userManager) {
        std::vector<String> userRoles;
        // 尝试通过 UserManager 获取多角色列表
        User* user = userManager->getUser(username);
        if (user) {
            userRoles = user->roles;
        }
        // 任一角色拥有权限即通过
        for (const String& roleId : userRoles) {
            if (roleManager->roleHasPermission(roleId, permission)) {
                result = true;
                break;
            }
        }
        // 若有角色列表但都未通过
        if (!result && !userRoles.empty()) {
            // 写入缓存（result 已为 false）
            if (method.isEmpty()) {
                _permCache[_permCacheIdx] = { username, permission, false, millis() };
                _permCacheIdx = (_permCacheIdx + 1) % PERM_CACHE_SIZE;
            }
            return false;
        }
        if (result) {
            if (method.isEmpty()) {
                _permCache[_permCacheIdx] = { username, permission, true, millis() };
                _permCacheIdx = (_permCacheIdx + 1) % PERM_CACHE_SIZE;
            }
            return true;
        }
    }

    // 降级：旧枚举比较（无 RoleManager 或无角色列表时兜底）
    auto permIt = permissions.find(permission);
    if (permIt == permissions.end()) {
        return false;
    }
    UserRole userRole = getUserRole(username);
    if (static_cast<int>(userRole) < static_cast<int>(permIt->second.minRole)) {
        return false;
    }

    // HTTP 方法检查
    if (!method.isEmpty() && !permIt->second.allowedMethods.empty()) {
        for (const String& m : permIt->second.allowedMethods) {
            if (method.equalsIgnoreCase(m)) return true;
        }
        return false;
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
        String roleStr = userManager->getUserRole(username);
        return UserManager::stringToRole(roleStr);
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
    
    FastBeeJsonDocLarge doc;
    JsonArray sessionsArray = doc["sessions"].to<JsonArray>();
    
    for (const auto& pair : activeSessions) {
        const UserSession& session = pair.second;
        JsonObject sessionObj = sessionsArray.add<JsonObject>();
        
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
    
    bool success = sessionPrefs.putString("sessions", jsonStr) > 0;
    if (success) {
        LOG_DEBUG("AuthManager: Sessions saved to storage");
    }
    return success;
}

bool AuthManager::loadSessionsFromStorage() {
    if (!securityConfig.enableSessionPersistence) {
        return true;
    }
    
    String jsonStr = sessionPrefs.getString("sessions", "");
    if (jsonStr.isEmpty()) {
        return true; // 没有会话数据是正常的
    }
    
    FastBeeJsonDocLarge doc;
    DeserializationError error = deserializeJson(doc, jsonStr);
    if (error) {
        char buf[72];
        snprintf(buf, sizeof(buf), "AuthManager: Failed to parse session data: %s", error.c_str());
        LOG_ERROR(buf);
        return false;
    }
    
    activeSessions.clear();
    
    JsonArray sessionsArray = doc["sessions"];
    unsigned long now = millis();
    
    // 第一遍：收集每个用户最新的会话（按 loginTime 最大值）
    std::map<String, UserSession> latestByUser;
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
        
        if (!session.isActive) continue;
        
        auto it = latestByUser.find(session.username);
        if (it == latestByUser.end() || session.loginTime > it->second.loginTime) {
            latestByUser[session.username] = session;
        }
    }
    
    // 第二遍：只保留每个用户最新的一个会话，并重置超时时间
    for (auto& pair : latestByUser) {
        UserSession& session = pair.second;
        // millis() 在重启后归零，存储的 expireTime 基于旧的 millis()，
        // 无法直接比较。重启后给会话一个新的超时窗口。
        session.expireTime = now + securityConfig.sessionTimeout;
        session.lastAccessTime = now;
        activeSessions[session.sessionId] = session;
    }
    
    char buf[64];
    snprintf(buf, sizeof(buf), "AuthManager: Loaded %u sessions from storage", (unsigned)activeSessions.size());
    LOG_INFO(buf);
    return true;
}

// ============ 静态工具方法 ============

String AuthManager::sessionStatusToString(SessionStatus status) {
    if (status.valid) {
        return "valid";
    } else if (status.expired) {
        return "expired";
    } else if (status.locked) {
        return "locked";
    } else {
        return "invalid";
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

bool AuthManager::validatePasswordStrength(const String& password, String* errorMessage) {
    // 检查密码长度
    if (password.length() < 8) {
        if (errorMessage) {
            *errorMessage = "Password must be at least 8 characters long";
        }
        return false;
    }
    
    // 检查是否包含数字
    bool hasDigit = false;
    // 检查是否包含大写字母
    bool hasUppercase = false;
    // 检查是否包含小写字母
    bool hasLowercase = false;
    // 检查是否包含特殊字符
    bool hasSpecial = false;
    
    for (unsigned int i = 0; i < password.length(); i++) {
        char c = password.charAt(i);
        if (isdigit(c)) {
            hasDigit = true;
        } else if (isupper(c)) {
            hasUppercase = true;
        } else if (islower(c)) {
            hasLowercase = true;
        } else if (!isalnum(c)) {
            hasSpecial = true;
        }
    }
    
    // 至少需要满足以下条件中的三个
    int conditionsMet = 0;
    if (hasDigit) conditionsMet++;
    if (hasUppercase) conditionsMet++;
    if (hasLowercase) conditionsMet++;
    if (hasSpecial) conditionsMet++;
    
    if (conditionsMet < 3) {
        if (errorMessage) {
            *errorMessage = "Password must contain at least 3 of the following: digits, uppercase letters, lowercase letters, special characters";
        }
        return false;
    }
    
    // 检查是否包含常见密码
    String commonPasswords[] = {
        "123456", "123456789", "12345", "1234", "111111", 
        "12345678", "abc123", "1234567", "password", "123123",
        "1234567890", "12345678910", "qwerty", "123456789a", "1234567891",
        "1234567892", "1234567893", "1234567894", "1234567895", "1234567896"
    };
    
    for (unsigned int i = 0; i < sizeof(commonPasswords) / sizeof(commonPasswords[0]); i++) {
        if (password.equals(commonPasswords[i])) {
            if (errorMessage) {
                *errorMessage = "Password is too common, please choose a different one";
            }
            return false;
        }
    }
    
    return true;
}

String AuthManager::getPasswordStrengthRating(const String& password) {
    int score = 0;
    
    // 长度评分
    if (password.length() >= 8) score += 20;
    if (password.length() >= 12) score += 10;
    if (password.length() >= 16) score += 10;
    
    // 复杂度评分
    bool hasDigit = false;
    bool hasUppercase = false;
    bool hasLowercase = false;
    bool hasSpecial = false;
    
    for (unsigned int i = 0; i < password.length(); i++) {
        char c = password.charAt(i);
        if (isdigit(c)) hasDigit = true;
        else if (isupper(c)) hasUppercase = true;
        else if (islower(c)) hasLowercase = true;
        else if (!isalnum(c)) hasSpecial = true;
    }
    
    if (hasDigit) score += 15;
    if (hasUppercase) score += 15;
    if (hasLowercase) score += 15;
    if (hasSpecial) score += 15;
    
    // 评分等级
    if (score < 40) return "Weak";
    else if (score < 70) return "Medium";
    else if (score < 90) return "Strong";
    else return "Very Strong";
}

void AuthManager::shutdown() {
    // 保存活跃会话到持久化存储
    if (securityConfig.enableSessionPersistence) {
        saveSessionsToStorage();
    }

    // 清空所有活跃会话
    activeSessions.clear();

    // 关闭会话 NVS
    sessionPrefs.end();

    LOG_INFO("AuthManager: Shutdown complete");
}

// IAuthManager 接口实现

void AuthManager::setUserManager(IUserManager* userManager) {
    this->userManager = userManager;
}

bool AuthManager::authenticate(const String& username, const String& password) {
    AuthResult result = login(username, password, "", "");
    return result.success;
}

String AuthManager::generateToken(const String& username) {
    AuthResult result = login(username, "", "", "");
    return result.success ? result.sessionId : "";
}

bool AuthManager::validateToken(const String& token) {
    AuthResult result = verifySession(token);
    return result.success;
}

bool AuthManager::revokeToken(const String& token) {
    return logout(token);
}

bool AuthManager::checkPermission(const String& username, const String& permission) {
    return checkPermission(username, permission, "");
}

bool AuthManager::checkSessionPermission(const String& sessionId, const String& permission) {
    return checkSessionPermission(sessionId, permission, "");
}

#endif // FASTBEE_ENABLE_AUTH