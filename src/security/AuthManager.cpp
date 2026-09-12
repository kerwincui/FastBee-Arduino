#include "./security/AuthManager.h"
#include "systems/LoggerSystem.h"
#include "core/FeatureFlags.h"
#include <esp_random.h>

#if FASTBEE_ENABLE_AUTH

// 登录异步化 RAII 持锁器：递归互斥量，允许 login/verifySession 等方法嵌套持锁
namespace {
struct SessionsLock {
    SemaphoreHandle_t m;
    explicit SessionsLock(SemaphoreHandle_t mtx) : m(mtx) {
        if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    }
    ~SessionsLock() {
        if (m) xSemaphoreGiveRecursive(m);
    }
};
}  // namespace

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

    if (!_sessionsMutex) {
        _sessionsMutex = xSemaphoreCreateRecursiveMutex();
    }

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
    SessionsLock lk(_sessionsMutex);
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
    // 审计 P1-2：上限兜底，满时驱逐锁定时间最早的一条（按时间戳找最小）
    if (lockedAccounts.find(username) == lockedAccounts.end() &&
        lockedAccounts.size() >= MAX_LOCKED_ACCOUNTS) {
        auto oldest = lockedAccounts.begin();
        for (auto e = lockedAccounts.begin(); e != lockedAccounts.end(); ++e) {
            if (e->second < oldest->second) oldest = e;
        }
        lockedAccounts.erase(oldest);
    }
    lockedAccounts[username] = millis();
    logAuthEvent("account_locked", username, "Too many failed login attempts", false);
}

void AuthManager::unlockAccount(const String& username) {
    SessionsLock lk(_sessionsMutex);
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
    // 登录异步化：本方法在工作任务执行（见 AuthRouteHandler），
    // 耗时的 PBKDF2 验证（validateUser）在锁外进行，仅短小的会话表
    // 操作持锁，避免阻塞 lwIP 线程的会话校验
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
    
    // 锁外： UserManager 内部自行持锁， PBKDF2 计算不占任何会话锁
    if (!userManager || !userManager->validateUser(username, password)) {
        result.errorMessage = "Invalid username or password";
        
        if (userManager) {
            SessionsLock lk(_sessionsMutex);
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
    
    UserSession session;
    session.sessionId = generateSessionId(username);
    session.username = username;
    session.loginTime = millis();
    session.lastAccessTime = session.loginTime;
    session.expireTime = session.loginTime + securityConfig.sessionTimeout;
    session.ipAddress = ipAddress;
    session.userAgent = userAgent;
    session.isActive = true;

    {
        SessionsLock lk(_sessionsMutex);
        cleanupExpiredSessions();
        if (securityConfig.allowMultipleSessions) {
            trimActiveSessions(username);
        } else {
            forceLogout(username);
        }

        activeSessions[session.sessionId] = session;

        if (securityConfig.enableSessionPersistence) {
            saveSessionsToStorage();
        }
    }
    
    result.success = true;
    result.sessionId = session.sessionId;
    
    logAuthEvent("login_success", username, "User logged in from " + ipAddress, true);
    return result;
}

AuthResult AuthManager::verifySession(const String& sessionId, bool updateAccessTime) {
    SessionsLock lk(_sessionsMutex);
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
        const String invalidUsername = session.username;
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
        
        logAuthEvent("session_invalid", invalidUsername, result.errorMessage, false);
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
    SessionsLock lk(_sessionsMutex);
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
    SessionsLock lk(_sessionsMutex);
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
    SessionsLock lk(_sessionsMutex);
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
    SessionsLock lk(_sessionsMutex);
    auto it = activeSessions.find(sessionId);
    if (it != activeSessions.end()) {
        return it->second;
    }
    return UserSession();
}

std::vector<UserSession> AuthManager::getActiveSessions() {
    SessionsLock lk(_sessionsMutex);
    std::vector<UserSession> sessions;
    for (const auto& pair : activeSessions) {
        sessions.push_back(pair.second);
    }
    return sessions;
}

std::vector<UserSession> AuthManager::getUserSessions(const String& username) {
    SessionsLock lk(_sessionsMutex);
    std::vector<UserSession> userSessions;
    for (const auto& pair : activeSessions) {
        if (pair.second.username == username) {
            userSessions.push_back(pair.second);
        }
    }
    return userSessions;
}

bool AuthManager::updateSessionAccessTime(const String& sessionId) {
    SessionsLock lk(_sessionsMutex);
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
    SessionsLock lk(_sessionsMutex);
    return activeSessions.size();
}

// ============ 在线用户管理 ============

std::vector<String> AuthManager::getOnlineUsers() {
    SessionsLock lk(_sessionsMutex);
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
    SessionsLock lk(_sessionsMutex);
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
    SessionsLock lk(_sessionsMutex);
    cleanupExpiredSessions();
    lastSessionCleanup = millis();
}

// ============ 持久化方法 ============

// WP-13: NVS 写入节流优化 (减少 Flash 寿命损耗)
bool AuthManager::saveSessionsToStorage() {
    SessionsLock lk(_sessionsMutex);
    if (!securityConfig.enableSessionPersistence) {
        return true;
    }
    
    // 节流检查：10 秒内多次变更合并一次写入
    uint32_t now = millis();
    if (now - lastNvsWriteTime_ < NVS_WRITE_INTERVAL_MS) {
        LOG_DEBUG("WP-13: NVS write throttle active");
        return true; // 跳过本次写入
    }
    
    FastBeeJsonDocLarge doc;
    JsonArray sessionsArray = doc["sessions"].to<JsonArray>();
    
    // WP-13: 限制会话数量
    size_t sessionCount = 0;
    for (const auto& pair : activeSessions) {
        if (sessionCount >= MAX_ACTIVE_SESSIONS_LIMIT) {
            LOG_WARNING("WP-13: Session limit reached, trimming oldest session");
            break;
        }
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
        
        sessionCount++;
    }
    
    String jsonStr;
    serializeJson(doc, jsonStr);
    
    // WP-13: 检查大小限制 (避免单次写入过大)
    if (jsonStr.length() > SESSION_PERSISTENCE_SIZE_THRESHOLD) {
        char buf[64];
        snprintf(buf, sizeof(buf), "WP-13: Session data too large (%u bytes), skipping write", 
                 (unsigned)jsonStr.length());
        LOG_WARNING(buf);
        return false;
    }
    
    // WP-13: Hash 比较避免重复写 Flash
    uint32_t currentHash = 0;
    for (size_t i = 0; i < jsonStr.length(); i++) {
        currentHash += jsonStr[i] * (i + 1);
    }
    
    if (currentHash == lastSessionHash_) {
        LOG_DEBUG("WP-13: Session data unchanged, skipping NVS write");
        return true; // 内容无变化，跳过写入
    }
    
    bool success = sessionPrefs.putString("sessions", jsonStr) > 0;
    if (success) {
        lastSessionHash_ = currentHash;
        lastNvsWriteTime_ = now;
        LOG_DEBUGF("WP-13: Sessions saved to storage with optimization (count=%d, size=%uB)", 
                  (int)sessionCount, (unsigned)jsonStr.length());
    }
    return success;
}

bool AuthManager::loadSessionsFromStorage() {
    SessionsLock lk(_sessionsMutex);
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
    SessionsLock lk(_sessionsMutex);
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
