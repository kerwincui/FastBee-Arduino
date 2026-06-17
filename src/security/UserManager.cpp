#include "./security/UserManager.h"
#include "systems/LoggerSystem.h"
#include "core/FeatureFlags.h"
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>
#include <esp_random.h>

#if FASTBEE_ENABLE_AUTH

// Base64编码函数
static String base64Encode(const uint8_t* data, size_t length) {
    const char* base64_chars = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    String ret;
    int i = 0, j = 0;
    uint8_t char_array_3[3], char_array_4[4];
    
    while (length--) {
        char_array_3[i++] = *(data++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            
            for(i = 0; i < 4; i++) ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }
    
    if (i) {
        for(j = i; j < 3; j++) char_array_3[j] = '\0';
        
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        
        for (j = 0; j < i + 1; j++) ret += base64_chars[char_array_4[j]];
        while(i++ < 3) ret += '=';
    }
    
    return ret;
}

UserManager::UserManager() {
}

UserManager::~UserManager() {
    saveUsersToStorage();
}

bool UserManager::initialize() {
    if (!loadConfig()) {
        LOG_INFO("UserManager: Using default config");
    }

    if (!loadUsersFromStorage() || users.empty()) {
        LOG_INFO("UserManager: No user data found, creating default admin");
        initializeDefaultAdmin();
    }

#if FASTBEE_SINGLE_ADMIN_MODE
    // 单管理员模式：确保仅存在 admin 用户
    auto adminIt = users.find(DEFAULT_ADMIN_USER);
    if (adminIt == users.end()) {
        users.clear();
        initializeDefaultAdmin();
    } else if (users.size() != 1 || !adminIt->second.enabled) {
        User admin = adminIt->second;
        admin.enabled = true;
        admin.lastModified = millis();
        users.clear();
        users[DEFAULT_ADMIN_USER] = admin;
        saveUsersToStorage();
        LOG_INFO("UserManager: Single-admin mode applied");
    }
#endif

    char buf[48];
    snprintf(buf, sizeof(buf), "UserManager: Initialized with %u users", (unsigned)users.size());
    LOG_INFO(buf);
    return true;
}

void UserManager::initializeDefaultAdmin() {
    User admin;
    admin.username     = DEFAULT_ADMIN_USER;
    admin.salt         = generateSalt();
    admin.passwordHash = hashPassword(DEFAULT_ADMIN_PASS, admin.salt);
    admin.enabled      = true;
    admin.createTime   = millis();
    admin.lastModified = millis();

    users[DEFAULT_ADMIN_USER] = admin;
    saveUsersToStorage();
}

String UserManager::generateSalt() {
    uint8_t randomBytes[16];
    esp_fill_random(randomBytes, sizeof(randomBytes));
    return base64Encode(randomBytes, sizeof(randomBytes));
}

String UserManager::hashPassword(const String& password, const String& salt) {
    String data = salt + password + salt;
    
    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const uint8_t*)data.c_str(), data.length());
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    
    return base64Encode(hash, sizeof(hash));
}

bool UserManager::verifyPassword(const String& password, const String& hash, const String& salt) {
    String calculatedHash = hashPassword(password, salt);
    return calculatedHash == hash;
}

bool UserManager::validatePasswordStrength(const String& password) {
    if (password.length() < config.minPasswordLength || 
        password.length() > config.maxPasswordLength) {
        return false;
    }
    
    if (config.requireStrongPasswords) {
        bool hasDigit = false, hasLower = false, hasUpper = false;
        
        for (char c : password) {
            if (isdigit(c)) hasDigit = true;
            if (islower(c)) hasLower = true;
            if (isupper(c)) hasUpper = true;
        }
        
        if (!hasDigit || !hasLower || !hasUpper) {
            return false;
        }
    }
    
    return true;
}

void UserManager::updateUserLastModified(const String& username) {
    auto it = users.find(username);
    if (it != users.end()) {
        it->second.lastModified = millis();
    }
}

// ============ 用户管理方法 ============

bool UserManager::addUser(const String& username, const String& password) {
#if FASTBEE_SINGLE_ADMIN_MODE
    (void)username;
    (void)password;
    return false;
#else
    if (users.find(username) != users.end()) {
        return false;
    }

    if (!validatePasswordStrength(password)) {
        return false;
    }

    User newUser;
    newUser.username     = username;
    newUser.salt         = generateSalt();
    newUser.passwordHash = hashPassword(password, newUser.salt);
    newUser.enabled      = true;
    newUser.createTime   = millis();
    newUser.lastModified = millis();

    users[username] = newUser;
    return saveUsersToStorage();
#endif
}

bool UserManager::deleteUser(const String& username) {
#if FASTBEE_SINGLE_ADMIN_MODE
    (void)username;
    return false;
#else
    // 不能删除默认管理员
    if (username == DEFAULT_ADMIN_USER) {
        return false;
    }
    
    auto it = users.find(username);
    if (it == users.end()) {
        return false;
    }
    
    users.erase(it);
    loginAttempts.erase(username);
    return saveUsersToStorage();
#endif
}

bool UserManager::updateUser(const String& username, const String& newPassword,
                           bool enabled) {
    auto it = users.find(username);
    if (it == users.end()) {
        return false;
    }
    
    User& user = it->second;
    
    // 更新密码
    if (!newPassword.isEmpty()) {
        if (!validatePasswordStrength(newPassword)) {
            return false;
        }
        user.salt = generateSalt();
        user.passwordHash = hashPassword(newPassword, user.salt);
    }
    
#if !FASTBEE_SINGLE_ADMIN_MODE
    // 更新启用状态
    user.enabled = enabled;
#else
    (void)enabled;
    user.enabled = true;
#endif
    
    user.lastModified = millis();
    return saveUsersToStorage();
}

bool UserManager::changePassword(const String& username, const String& oldPassword,
                               const String& newPassword) {
    auto it = users.find(username);
    if (it == users.end()) {
        return false;
    }
    
    User& user = it->second;
    
    if (!verifyPassword(oldPassword, user.passwordHash, user.salt)) {
        return false;
    }
    
    if (!validatePasswordStrength(newPassword)) {
        return false;
    }
    
    user.salt = generateSalt();
    user.passwordHash = hashPassword(newPassword, user.salt);
    user.lastModified = millis();
    
    return saveUsersToStorage();
}

bool UserManager::resetPassword(const String& username, const String& newPassword) {
    auto it = users.find(username);
    if (it == users.end()) {
        return false;
    }
    
    if (!validatePasswordStrength(newPassword)) {
        return false;
    }
    
    User& user = it->second;
    user.salt = generateSalt();
    user.passwordHash = hashPassword(newPassword, user.salt);
    user.lastModified = millis();
    
    return saveUsersToStorage();
}

// ============ 用户查询方法 ============

User* UserManager::getUser(const String& username) {
    auto it = users.find(username);
    return (it != users.end()) ? &it->second : nullptr;
}

std::vector<String> UserManager::getAllUsernames() {
    std::vector<String> usernames;
    for (const auto& pair : users) {
        usernames.push_back(pair.first);
    }
    return usernames;
}

bool UserManager::userExists(const String& username) {
    return users.find(username) != users.end();
}

bool UserManager::isUserEnabled(const String& username) {
    auto it = users.find(username);
    return (it != users.end()) ? it->second.enabled : false;
}

// ============ 登录保护方法 ============

void UserManager::recordLoginFailure(const String& username) {
    if (!config.maxLoginAttempts) return;
    
    loginAttempts[username]++;
    
    if (loginAttempts[username] >= config.maxLoginAttempts) {
        char buf[72];
        snprintf(buf, sizeof(buf), "UserManager: Account %s locked (too many failures)", username.c_str());
        LOG_WARNING(buf);
    }
}

void UserManager::resetLoginAttempts(const String& username) {
    loginAttempts.erase(username);
}

uint8_t UserManager::getLoginAttempts(const String& username) {
    auto it = loginAttempts.find(username);
    return (it != loginAttempts.end()) ? it->second : 0;
}

bool UserManager::isAccountLocked(const String& username) {
    if (!config.maxLoginAttempts) return false;
    
    auto it = loginAttempts.find(username);
    return (it != loginAttempts.end() && it->second >= config.maxLoginAttempts);
}

void UserManager::unlockAccount(const String& username) {
    loginAttempts.erase(username);
}

// ============ 统计信息方法 ============

UserStats UserManager::getUserStats(const String& username) {
    UserStats stats;
    stats.username = username;
    
    auto it = users.find(username);
    if (it != users.end()) {
        stats.createTime = it->second.createTime;
    }
    
    stats.isLocked = isAccountLocked(username);
    stats.loginAttempts = getLoginAttempts(username);
    
    return stats;
}

std::vector<UserStats> UserManager::getAllUserStats() {
    std::vector<UserStats> statsList;
    
    for (const auto& pair : users) {
        UserStats stats = getUserStats(pair.first);
        statsList.push_back(stats);
    }
    
    return statsList;
}

size_t UserManager::getUserCount() {
    return users.size();
}

void UserManager::updateLastLogin(const String& username) {
    auto it = users.find(username);
    if (it != users.end()) {
        it->second.lastLogin = millis();
        updateUserLastModified(username);
    }
}

// ============ 持久化方法 ============

bool UserManager::saveUsersToStorage() {
#if FASTBEE_SINGLE_ADMIN_MODE
    auto adminIt = users.find(DEFAULT_ADMIN_USER);
    if (adminIt == users.end()) {
        LOG_ERROR("UserManager: Cannot save single-admin auth without admin user");
        return false;
    }

    const User& admin = adminIt->second;
    JsonDocument doc;
    doc["version"] = "1.0";

    JsonObject adminObj = doc["admin"].to<JsonObject>();
    adminObj["username"]     = DEFAULT_ADMIN_USER;
    adminObj["passwordHash"] = admin.passwordHash;
    adminObj["salt"]         = admin.salt;
    adminObj["lastLogin"]    = admin.lastLogin;
    adminObj["lastModified"] = admin.lastModified;
#else
    JsonDocument doc;
    doc["version"] = "1.0";

    JsonArray usersArr = doc["users"].to<JsonArray>();
    for (const auto& pair : users) {
        const User& user = pair.second;
        JsonObject userObj = usersArr.add<JsonObject>();
        userObj["username"]     = user.username;
        userObj["passwordHash"] = user.passwordHash;
        userObj["salt"]         = user.salt;
        userObj["enabled"]      = user.enabled;
        userObj["description"]  = user.description;
        userObj["createTime"]   = user.createTime;
        userObj["lastLogin"]    = user.lastLogin;
        userObj["lastModified"] = user.lastModified;
    }
#endif

    JsonObject security = doc["security"].to<JsonObject>();
    security["maxLoginAttempts"]        = config.maxLoginAttempts;
    security["loginLockoutTime"]        = config.loginLockoutTime;
    security["minPasswordLength"]       = config.minPasswordLength;
    security["maxPasswordLength"]       = config.maxPasswordLength;
    security["requireStrongPasswords"]  = config.requireStrongPasswords;
    security["allowMultipleSessions"]   = config.allowMultipleSessions;
    security["sessionTimeout"]          = config.sessionTimeout;
    security["sessionCleanupInterval"]  = config.sessionCleanupInterval;
    security["enableSessionPersistence"]= config.enableSessionPersistence;
    security["cookieName"]              = config.cookieName;
    security["cookieMaxAge"]            = config.cookieMaxAge;
    security["cookieHttpOnly"]          = config.cookieHttpOnly;
    security["cookieSecure"]            = config.cookieSecure;

    if (!LittleFS.exists("/config") && !LittleFS.mkdir("/config")) {
        LOG_ERROR("UserManager: Failed to create /config directory");
        return false;
    }

    const char* savePath = FASTBEE_SINGLE_ADMIN_MODE ? ADMIN_AUTH_CONFIG_FILE : USERS_CONFIG_FILE;
    File file = LittleFS.open(savePath, "w");
    if (!file) {
        LOG_ERROR("UserManager: Failed to open file for writing");
        return false;
    }

    size_t written = serializeJson(doc, file);
    file.close();
    if (written == 0) {
        LOG_ERROR("UserManager: Failed to write file");
        return false;
    }

    return true;
}

bool UserManager::loadUsersFromStorage() {
    const bool hasUsersFile = LittleFS.exists(USERS_CONFIG_FILE);
    const bool hasAuthFile = LittleFS.exists(ADMIN_AUTH_CONFIG_FILE);

#if FASTBEE_SINGLE_ADMIN_MODE
    const char* loadPath = hasAuthFile ? ADMIN_AUTH_CONFIG_FILE : (hasUsersFile ? USERS_CONFIG_FILE : nullptr);
#else
    const char* loadPath = hasUsersFile ? USERS_CONFIG_FILE : (hasAuthFile ? ADMIN_AUTH_CONFIG_FILE : nullptr);
#endif

    if (!loadPath) {
        LOG_INFO("UserManager: No user data file found");
        return false;
    }

    File file = LittleFS.open(loadPath, "r");
    if (!file) {
        LOG_ERROR("UserManager: Failed to open user data file");
        return false;
    }

    FastBeeJsonDocLarge doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    if (error) {
        char buf[64];
        snprintf(buf, sizeof(buf), "UserManager: Failed to parse user data: %s", error.c_str());
        LOG_ERROR(buf);
        return false;
    }

    users.clear();

    // 尝试从 "users" 数组加载（多用户格式）
    JsonArray usersArray = doc["users"];
    if (!usersArray.isNull() && usersArray.size() > 0) {
        for (JsonObject userObj : usersArray) {
            User user;
            user.username     = userObj["username"].as<String>();
            user.passwordHash = userObj["passwordHash"].as<String>();
            user.salt         = userObj["salt"].as<String>();
            user.enabled      = userObj["enabled"] | true;
            user.description  = userObj["description"] | "";
            user.createTime   = userObj["createTime"] | 0UL;
            user.lastLogin    = userObj["lastLogin"] | 0UL;
            user.lastModified = userObj["lastModified"] | 0UL;
            if (!user.username.isEmpty() && !user.passwordHash.isEmpty()) {
                users[user.username] = user;
            }
        }
    } else {
        // 从 "admin" 对象加载（单管理员格式 / 迁移）
        JsonObject adminObj = doc["admin"];
        if (!adminObj.isNull()) {
            User admin;
            admin.username     = DEFAULT_ADMIN_USER;
            admin.passwordHash = adminObj["passwordHash"].as<String>();
            admin.salt         = adminObj["salt"].as<String>();
            admin.enabled      = true;
            admin.lastLogin    = adminObj["lastLogin"] | 0UL;
            admin.lastModified = adminObj["lastModified"] | millis();
            admin.createTime   = millis();
            if (!admin.passwordHash.isEmpty() && !admin.salt.isEmpty()) {
                users[DEFAULT_ADMIN_USER] = admin;
            }
        }
    }

    // 加载安全配置
    JsonObject security = doc["security"];
    if (!security.isNull()) {
        config.maxLoginAttempts        = security["maxLoginAttempts"] | 5;
        config.loginLockoutTime        = security["loginLockoutTime"] | 300000UL;
        config.minPasswordLength       = security["minPasswordLength"] | 6;
        config.maxPasswordLength       = security["maxPasswordLength"] | 32;
        config.requireStrongPasswords  = security["requireStrongPasswords"] | false;
        config.allowMultipleSessions   = security["allowMultipleSessions"] | true;
        config.sessionTimeout          = security["sessionTimeout"] | 3600000UL;
        config.sessionCleanupInterval  = security["sessionCleanupInterval"] | 60000UL;
        config.enableSessionPersistence= security["enableSessionPersistence"] | false;
        config.cookieName              = security["cookieName"] | "sessionId";
        config.cookieMaxAge            = security["cookieMaxAge"] | 3600UL;
        config.cookieHttpOnly          = security["cookieHttpOnly"] | true;
        config.cookieSecure            = security["cookieSecure"] | false;
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "UserManager: Loaded %u users", (unsigned)users.size());
    LOG_INFO(buf);
    return !users.empty();
}

bool UserManager::saveConfig() {
    return saveUsersToStorage();
}

bool UserManager::loadConfig() {
    return true;
}

bool UserManager::updateSecurityConfig(uint32_t sessionTimeout, uint32_t sessionCleanupInterval,
                                       bool enableSessionPersistence, const String& cookieName,
                                       uint32_t cookieMaxAge, bool cookieHttpOnly, bool cookieSecure) {
    config.sessionTimeout = sessionTimeout;
    config.sessionCleanupInterval = sessionCleanupInterval;
    config.enableSessionPersistence = enableSessionPersistence;
    config.cookieName = cookieName;
    config.cookieMaxAge = cookieMaxAge;
    config.cookieHttpOnly = cookieHttpOnly;
    config.cookieSecure = cookieSecure;
    return saveUsersToStorage();
}

// ============ 静态工具方法 ============


String UserManager::generateRandomSalt() {
    uint8_t randomBytes[16];
    esp_fill_random(randomBytes, sizeof(randomBytes));
    return base64Encode(randomBytes, sizeof(randomBytes));
}

bool UserManager::validatePassword(const String& password, uint8_t minLength, 
                                 bool requireComplexity) {
    if (password.length() < minLength) {
        return false;
    }
    
    if (requireComplexity) {
        bool hasDigit = false, hasLower = false, hasUpper = false;
        
        for (char c : password) {
            if (isdigit(c)) hasDigit = true;
            if (islower(c)) hasLower = true;
            if (isupper(c)) hasUpper = true;
        }
        
        return hasDigit && hasLower && hasUpper;
    }
    
    return true;
}

bool UserManager::setUserDescription(const String& username, const String& description) {
    auto it = users.find(username);
    if (it == users.end()) return false;

    // 截断为最大64字符
    it->second.description = description.substring(0, 64);
    it->second.lastModified = millis();
    return saveUsersToStorage();
}

bool UserManager::updatePassword(const String& username, const String& newPassword) {
    return resetPassword(username, newPassword);
}

bool UserManager::validateUser(const String& username, const String& password) {
    auto it = users.find(username);
    if (it == users.end()) {
        recordLoginFailure(username);
        return false;
    }
    
    const User& user = it->second;
    
    if (!user.enabled) {
        recordLoginFailure(username);
        return false;
    }
    
    if (!verifyPassword(password, user.passwordHash, user.salt)) {
        recordLoginFailure(username);
        return false;
    }
    
    resetLoginAttempts(username);
    updateLastLogin(username);
    
    return true;
}


String UserManager::getAllUsers() {
    FastBeeJsonDocLarge doc;
    JsonArray usersArray = doc["users"].to<JsonArray>();

    for (const auto& pair : users) {
        const User& user = pair.second;
        JsonObject userObj = usersArray.add<JsonObject>();
        userObj["username"]    = user.username;
        userObj["enabled"]     = user.enabled;
        userObj["description"] = user.description;
        userObj["createTime"]  = user.createTime;
        userObj["lastLogin"]   = user.lastLogin;
    }

    String jsonStr;
    serializeJson(doc, jsonStr);
    return jsonStr;
}

#endif // FASTBEE_ENABLE_AUTH
