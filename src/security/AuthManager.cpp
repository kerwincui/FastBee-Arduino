#include "./security/AuthManager.h"
#include "systems/LoggerSystem.h"
#include "core/FeatureFlags.h"
#include <esp_random.h>

#if FASTBEE_ENABLE_AUTH

AuthManager::AuthManager(IUserManager* userMgr, LoggerSystem* loggerPtr)
    : userManager(userMgr), logger(loggerPtr), lastSessionCleanup(0) {
    sessionPrefs.begin("auth_sessions", false);
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

    if (securityConfig.enableSessionPersistence) {
        LOG_DEBUG("AuthManager: Loading sessions from storage...");
        loadSessionsFromStorage();
    }

    char buf[80];
    snprintf(buf, sizeof(buf), "AuthManager: Initialized sessions=%u",
             (unsigned)activeSessions.size());
    LOG_INFO(buf);
    return true;
}

String AuthManager::generateSessionId(const String& username) {
    (void)username;
    uint8_t randomBytes[32];
    esp_fill_random(randomBytes, sizeof(randomBytes));
    
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
    
    if (currentTime > session.expireTime) {
        status.valid = false;
        status.expired = true;
        status.locked = false;
        status.username = session.username;
        return status;
    }
    
    if (isAccountLocked(session.username)) {
        status.valid = false;
        status.expired = false;
        status.locked = true;
        status.username = session.username;
        return status;
    }
    
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

bool AuthManager::removeOldestSession(const String& username) {
    auto oldest = activeSessions.end();

    for (auto it = activeSessions.begin(); it != activeSessions.end(); ++it) {
        if (!username.isEmpty() && it->second.username != username) {
            continue;
        }
        if (oldest == activeSessions.end() ||
            it->second.lastAccessTime < oldest->second.lastAccessTime) {
            oldest = it;
        }
    }

    if (oldest == activeSessions.end()) {
        return false;
    }

    String prunedUser = oldest->second.username;
    activeSessions.erase(oldest);
    logAuthEvent("session_pruned", prunedUser, "Session limit reached", true);
    return true;
}

void AuthManager::trimActiveSessions(const String& username) {
    if (!securityConfig.allowMultipleSessions) {
        return;
    }

    size_t userLimit = MAX_SESSIONS_PER_USER;
    if (!username.isEmpty() && userLimit > 0) {
        userLimit -= 1;
    }

    while (!username.isEmpty()) {
        size_t userSessions = 0;
        for (const auto& pair : activeSessions) {
            if (pair.second.username == username) {
                userSessions++;
            }
        }
        if (userSessions <= userLimit || !removeOldestSession(username)) {
            break;
        }
    }

    size_t totalLimit = MAX_ACTIVE_SESSIONS;
    if (!username.isEmpty() && totalLimit > 0) {
        totalLimit -= 1;
    }
    while (activeSessions.size() > totalLimit) {
        if (!removeOldestSession("")) {
            break;
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
    if (userManager) {
        UserManager* um = static_cast<UserManager*>(userManager);
        securityConfig.sessionTimeout = um->getSessionTimeout();
        securityConfig.sessionCleanupInterval = um->getSessionCleanupInterval();
        securityConfig.enableSessionPersistence = um->isSessionPersistenceEnabled();
        securityConfig.cookieName = um->getCookieName();
        securityConfig.cookieMaxAge = um->getCookieMaxAge();
        securityConfig.cookieHttpOnly = um->isCookieHttpOnly();
        securityConfig.cookieSecure = um->isCookieSecure();
        securityConfig.allowMultipleSessions = um->allowsMultipleSessions();
    }
    
    securityConfig.enableIpWhitelist = false;
    securityConfig.ipWhitelist.clear();
    
    return true;
}

bool AuthManager::saveSecurityConfig() {
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
    
    if (!checkIpWhitelist(ipAddress)) {
        result.errorMessage = "IP address not allowed";
        logAuthEvent("login_failed_ip", username, "IP not in whitelist: " + ipAddress, false);
        return result;
    }
    
    if (isAccountLocked(username)) {
        result.errorMessage = "Account is locked";
        logAuthEvent("login_failed_locked", username, "Account locked", false);
        return result;
    }
    
    if (!userManager || !userManager->validateUser(username, password)) {
        result.errorMessage = "Invalid username or password";
        
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
        return result;
    }
    
    cleanupExpiredSessions();
    if (securityConfig.allowMultipleSessions) {
        trimActiveSessions(username);
    } else {
        forceLogout(username);
    }
    
    UserSession session;
    session.sessionId = generateSessionId(username);
    session.username = username;
    session.loginTime = millis();
    session.lastAccessTime = session.loginTime;
    session.expireTime = session.loginTime + securityConfig.sessionTimeout;
    session.ipAddress = ipAddress;
    session.userAgent = userAgent;
    session.isActive = true;
    
    activeSessions[session.sessionId] = session;
    
    if (securityConfig.enableSessionPersistence) {
        saveSessionsToStorage();
    }
    
    result.success = true;
    result.sessionId = session.sessionId;
    
    logAuthEvent("login_success", username, "User logged in from " + ipAddress, true);
    return result;
}

AuthResult AuthManager::verifySession(const String& sessionId, bool updateAccessTime) {
    AuthResult result;
    
    unsigned long currentTime = millis();
    if (currentTime - lastSessionCleanup > securityConfig.sessionCleanupInterval) {
        cleanupExpiredSessions();
        lastSessionCleanup = currentTime;
    }
    
    auto it = activeSessions.find(sessionId);
    if (it == activeSessions.end()) {
        result.errorMessage = "Session not found";
        return result;
    }
    
    UserSession& session = it->second;
    result.username = session.username;
    
    SessionStatus status = validateSession(session);
    if (!status.valid) {
        if (status.expired) {
            result.errorMessage = "Session expired";
        } else if (status.locked) {
            result.errorMessage = "Account locked";
        } else {
            result.errorMessage = "Session invalid";
        }
        
        if (status.expired || !status.valid) {
            activeSessions.erase(it);
        }
        
        logAuthEvent("session_invalid", session.username, result.errorMessage, false);
        return result;
    }
    
    if (updateAccessTime) {
        session.lastAccessTime = currentTime;
        session.expireTime = currentTime + securityConfig.sessionTimeout;
    }
    
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

// ============ 在线用户管理 ============

std::vector<String> AuthManager::getOnlineUsers() {
    std::vector<String> onlineUsers;
    
    for (const auto& pair : activeSessions) {
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
        return true;
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
    
    std::map<String, UserSession> latestByUser;
    for (JsonObject sessionObj : sessionsArray) {
        UserSession session;
        session.sessionId = sessionObj["sessionId"].as<String>();
        session.username = sessionObj["username"].as<String>();
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
    
    for (auto& pair : latestByUser) {
        UserSession& session = pair.second;
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

static String readSessionCookieValue(const String& cookieHeader, const String& cookieName) {
    int start = cookieHeader.indexOf(cookieName + "=");
    if (start == -1) {
        return "";
    }

    start += cookieName.length() + 1;
    int end = cookieHeader.indexOf(';', start);
    if (end == -1) {
        end = cookieHeader.length();
    }
    String value = cookieHeader.substring(start, end);
    value.trim();
    return value;
}

String AuthManager::extractSessionIdFromRequest(AsyncWebServerRequest* request, 
                                               const String& cookieName) {
    if (!request) return "";

    if (request->authType() == AsyncAuthType::AUTH_BEARER) {
        String token = request->authChallenge();
        token.trim();
        if (!token.isEmpty()) {
            return token;
        }
    }

    if (request->hasHeader("Authorization")) {
        String authHeader = request->header("Authorization");
        authHeader.trim();
        if (authHeader.startsWith("Bearer ")) {
            String token = authHeader.substring(7);
            token.trim();
            if (!token.isEmpty()) {
                return token;
            }
        }
    }

    for (const auto& header : request->getHeaders()) {
        if (!header.name().equalsIgnoreCase("Authorization")) continue;
        String authHeader = header.value();
        authHeader.trim();
        if (authHeader.startsWith("Bearer ")) {
            String token = authHeader.substring(7);
            token.trim();
            if (!token.isEmpty()) {
                return token;
            }
        }
    }
    
    if (request->hasHeader("Cookie")) {
        String cookieHeader = request->header("Cookie");
        String token = readSessionCookieValue(cookieHeader, cookieName);
        if (token.isEmpty() && cookieName != "sessionId") token = readSessionCookieValue(cookieHeader, "sessionId");
        if (token.isEmpty() && cookieName != "session") token = readSessionCookieValue(cookieHeader, "session");
        if (!token.isEmpty()) return token;
    }

    for (const auto& header : request->getHeaders()) {
        if (!header.name().equalsIgnoreCase("Cookie")) continue;
        String cookieHeader = header.value();
        String token = readSessionCookieValue(cookieHeader, cookieName);
        if (token.isEmpty() && cookieName != "sessionId") token = readSessionCookieValue(cookieHeader, "sessionId");
        if (token.isEmpty() && cookieName != "session") token = readSessionCookieValue(cookieHeader, "session");
        if (!token.isEmpty()) return token;
    }

    if (request->hasParam("session")) {
        return request->getParam("session")->value();
    }
    
    if (request->hasParam("session", true)) {
        return request->getParam("session", true)->value();
    }
    
    return "";
}

bool AuthManager::validatePasswordStrength(const String& password, String* errorMessage) {
    if (password.length() < 8) {
        if (errorMessage) {
            *errorMessage = "Password must be at least 8 characters long";
        }
        return false;
    }
    
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
    
    int conditionsMet = 0;
    if (hasDigit) conditionsMet++;
    if (hasUppercase) conditionsMet++;
    if (hasLowercase) conditionsMet++;
    if (hasSpecial) conditionsMet++;
    
    if (conditionsMet < 3) {
        if (errorMessage) {
            *errorMessage = "Password must contain at least 3 of: digits, uppercase, lowercase, special";
        }
        return false;
    }
    
    return true;
}

String AuthManager::getPasswordStrengthRating(const String& password) {
    int score = 0;
    
    if (password.length() >= 8) score += 20;
    if (password.length() >= 12) score += 10;
    if (password.length() >= 16) score += 10;
    
    bool hasDigit = false, hasUppercase = false, hasLowercase = false, hasSpecial = false;
    
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
    
    if (score < 40) return "Weak";
    else if (score < 70) return "Medium";
    else if (score < 90) return "Strong";
    else return "Very Strong";
}

void AuthManager::shutdown() {
    if (securityConfig.enableSessionPersistence) {
        saveSessionsToStorage();
    }
    activeSessions.clear();
    sessionPrefs.end();
    LOG_INFO("AuthManager: Shutdown complete");
}

// IAuthManager 接口实现

void AuthManager::setUserManager(IUserManager* um) {
    this->userManager = um;
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

#endif // FASTBEE_ENABLE_AUTH
