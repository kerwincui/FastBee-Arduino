#include "./security/UserManager.h"
#include "systems/LoggerSystem.h"
#include "core/FeatureFlags.h"
#include "utils/TimeUtils.h"
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>
#include <mbedtls/pkcs5.h>
#include <esp_random.h>

#if FASTBEE_ENABLE_AUTH

// PBKDF2-HMAC-SHA256 密码哈希参数（新格式）
// 格式：$fb2$<iterations>$<salt_hex>$<hash_hex>
static const char* const PASSWORD_HASH_PREFIX   = "$fb2$";
static const uint32_t  PASSWORD_PBKDF2_ITER     = 20000;
static const size_t    PASSWORD_SALT_LEN        = 16;  // 盐 16 字节
static const size_t    PASSWORD_PBKDF2_KEY_LEN  = 32;  // 派生 32 字节

// 字节转十六进制（小写）
static String bytesToHexStr(const uint8_t* data, size_t length) {
    String hex;
    hex.reserve(length * 2);
    for (size_t i = 0; i < length; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", data[i]);
        hex += buf;
    }
    return hex;
}

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

// 登录异步化 RAII 持锁器：递归互斥量，允许方法间嵌套调用
namespace {
struct UsersLock {
    SemaphoreHandle_t m;
    explicit UsersLock(SemaphoreHandle_t mtx) : m(mtx) {
        if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    }
    ~UsersLock() {
        if (m) xSemaphoreGiveRecursive(m);
    }
};
}  // namespace

bool UserManager::initialize() {
    if (!_usersMutex) {
        _usersMutex = xSemaphoreCreateRecursiveMutex();
    }
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
    admin.passwordHash = hashPasswordPbkdf2(DEFAULT_ADMIN_PASS);
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
    // 旧格式（兼容验证用）：加盐单轮 SHA256，新密码不再使用
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

String UserManager::hashPasswordPbkdf2(const String& password) {
    // 16 字节随机盐（沿用现有随机源 esp_fill_random）
    uint8_t saltBytes[PASSWORD_SALT_LEN];
    esp_fill_random(saltBytes, sizeof(saltBytes));
    String saltHex = bytesToHexStr(saltBytes, sizeof(saltBytes));

    // PBKDF2-HMAC-SHA256 派生 32 字节（ESP-IDF mbedtls 仅提供 _ext 变体）
    uint8_t derived[PASSWORD_PBKDF2_KEY_LEN];
    int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                            (const uint8_t*)password.c_str(), password.length(),
                                            saltBytes, sizeof(saltBytes),
                                            PASSWORD_PBKDF2_ITER,
                                            sizeof(derived), derived);

    if (ret != 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "UserManager: PBKDF2 failed: -0x%04X", -ret);
        LOG_ERROR(buf);
        return "";
    }

    return String(PASSWORD_HASH_PREFIX) + String(PASSWORD_PBKDF2_ITER) + "$" +
           saltHex + "$" + bytesToHexStr(derived, sizeof(derived));
}

bool UserManager::verifyPasswordPbkdf2(const String& password, const String& storedHash) {
    // 格式：$fb2$<iterations>$<salt_hex>$<hash_hex>
    int p1 = storedHash.indexOf('$', 5);                 // iterations 之后
    if (p1 < 0) return false;
    int p2 = storedHash.indexOf('$', p1 + 1);            // salt 之后
    if (p2 < 0) return false;

    long iterations = storedHash.substring(5, p1).toInt();
    if (iterations <= 0 || iterations > 1000000) return false;

    String saltHex = storedHash.substring(p1 + 1, p2);
    String hashHex = storedHash.substring(p2 + 1);
    if (saltHex.length() == 0 || saltHex.length() % 2 != 0 ||
        hashHex.length() == 0 || hashHex.length() % 2 != 0) {
        return false;
    }

    // hex 转字节
    size_t saltLen = saltHex.length() / 2;
    size_t keyLen = hashHex.length() / 2;
    if (saltLen > 64 || keyLen > 64) return false;
    uint8_t saltBytes[64];
    uint8_t storedKey[64];
    for (size_t i = 0; i < saltLen; i++) {
        saltBytes[i] = (uint8_t)strtol(saltHex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
    }
    for (size_t i = 0; i < keyLen; i++) {
        storedKey[i] = (uint8_t)strtol(hashHex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
    }

    uint8_t derived[64];
    int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                            (const uint8_t*)password.c_str(), password.length(),
                                            saltBytes, saltLen,
                                            (unsigned int)iterations,
                                            (uint32_t)keyLen, derived);
    if (ret != 0) return false;

    // 恒定时间比较，避免时序侧信道
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < keyLen; i++) {
        diff |= (uint8_t)(derived[i] ^ storedKey[i]);
    }
    return diff == 0;
}

bool UserManager::verifyLegacyPassword(const String& password, const String& storedHash,
                                       const String& salt) {
    // 旧格式：加盐单轮 SHA256（base64）
    String calculatedHash = hashPassword(password, salt);
    return calculatedHash == storedHash;
}

bool UserManager::verifyPassword(const String& password, const String& storedHash, const String& salt) {
    if (storedHash.startsWith(PASSWORD_HASH_PREFIX)) {
        return verifyPasswordPbkdf2(password, storedHash);
    }
    return verifyLegacyPassword(password, storedHash, salt);
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
    UsersLock lk(_usersMutex);
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
    UsersLock lk(_usersMutex);
    if (users.find(username) != users.end()) {
        return false;
    }

    if (!validatePasswordStrength(password)) {
        return false;
    }

    User newUser;
    newUser.username     = username;
    newUser.salt         = generateSalt();
    newUser.passwordHash = hashPasswordPbkdf2(password);
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
    
    UsersLock lk(_usersMutex);
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
    UsersLock lk(_usersMutex);
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
        user.passwordHash = hashPasswordPbkdf2(newPassword);
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
    UsersLock lk(_usersMutex);
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
    user.passwordHash = hashPasswordPbkdf2(newPassword);
    user.lastModified = millis();
    
    return saveUsersToStorage();
}

bool UserManager::resetPassword(const String& username, const String& newPassword) {
    UsersLock lk(_usersMutex);
    auto it = users.find(username);
    if (it == users.end()) {
        return false;
    }
    
    if (!validatePasswordStrength(newPassword)) {
        return false;
    }
    
    User& user = it->second;
    user.salt = generateSalt();
    user.passwordHash = hashPasswordPbkdf2(newPassword);
    user.lastModified = millis();
    
    return saveUsersToStorage();
}

// ============ 用户查询方法 ============

User* UserManager::getUser(const String& username) {
    UsersLock lk(_usersMutex);
    auto it = users.find(username);
    return (it != users.end()) ? &it->second : nullptr;
}

std::vector<String> UserManager::getAllUsernames() {
    UsersLock lk(_usersMutex);
    std::vector<String> usernames;
    for (const auto& pair : users) {
        usernames.push_back(pair.first);
    }
    return usernames;
}

bool UserManager::userExists(const String& username) {
    UsersLock lk(_usersMutex);
    return users.find(username) != users.end();
}

bool UserManager::isUserEnabled(const String& username) {
    UsersLock lk(_usersMutex);
    auto it = users.find(username);
    return (it != users.end()) ? it->second.enabled : false;
}

// ============ 登录保护方法 ============

void UserManager::recordLoginFailure(const String& username) {
    UsersLock lk(_usersMutex);
    if (!config.maxLoginAttempts) return;

    // 审计 P1-2：仅对存在的用户计数。未知用户名的失败请求只记聚合计数，
    // 避免未认证攻击者用随机用户名无限撑大 loginAttempts（只增不减直至重启）
    if (users.find(username) == users.end()) {
        unknownUserFailures++;
        if (unknownUserFailures % 10 == 1) {
            char buf[96];
            snprintf(buf, sizeof(buf),
                     "UserManager: %lu failed logins with unknown username (not tracked)",
                     (unsigned long)unknownUserFailures);
            LOG_WARNING(buf);
        }
        return;
    }

    auto it = loginAttempts.find(username);
    if (it == loginAttempts.end()) {
        // 上限兜底：满时驱逐失败次数最少的一条（有界优先于精确）
        if (loginAttempts.size() >= MAX_LOGIN_TRACKED) {
            auto victim = loginAttempts.begin();
            for (auto e = loginAttempts.begin(); e != loginAttempts.end(); ++e) {
                if (e->second < victim->second) victim = e;
            }
            loginAttempts.erase(victim);
        }
        it = loginAttempts.emplace(username, 0).first;
    }
    it->second++;

    if (it->second >= config.maxLoginAttempts) {
        char buf[72];
        snprintf(buf, sizeof(buf), "UserManager: Account %s locked (too many failures)", username.c_str());
        LOG_WARNING(buf);
    }
}

void UserManager::resetLoginAttempts(const String& username) {
    UsersLock lk(_usersMutex);
    loginAttempts.erase(username);
}

uint8_t UserManager::getLoginAttempts(const String& username) {
    UsersLock lk(_usersMutex);
    auto it = loginAttempts.find(username);
    return (it != loginAttempts.end()) ? it->second : 0;
}

bool UserManager::isAccountLocked(const String& username) {
    UsersLock lk(_usersMutex);
    if (!config.maxLoginAttempts) return false;
    
    auto it = loginAttempts.find(username);
    return (it != loginAttempts.end() && it->second >= config.maxLoginAttempts);
}

void UserManager::unlockAccount(const String& username) {
    UsersLock lk(_usersMutex);
    loginAttempts.erase(username);
}

// ============ 统计信息方法 ============

UserStats UserManager::getUserStats(const String& username) {
    UsersLock lk(_usersMutex);
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
    UsersLock lk(_usersMutex);
    std::vector<UserStats> statsList;
    
    for (const auto& pair : users) {
        UserStats stats = getUserStats(pair.first);
        statsList.push_back(stats);
    }
    
    return statsList;
}

size_t UserManager::getUserCount() {
    UsersLock lk(_usersMutex);
    return users.size();
}

void UserManager::updateLastLogin(const String& username) {
    UsersLock lk(_usersMutex);
    auto it = users.find(username);
    if (it != users.end()) {
        time_t now = TimeUtils::getTimestamp();
        // NTP 同步前时间戳可能很小，用 >2020-01-01 判断是否有效
        it->second.lastLogin = (now > 1577836800) ? (unsigned long)now : millis();
        updateUserLastModified(username);
        saveUsersToStorage();
    }
}

// ============ 持久化方法 ============

bool UserManager::saveUsersToStorage() {
    UsersLock lk(_usersMutex);
#if FASTBEE_SINGLE_ADMIN_MODE
    auto adminIt = users.find(DEFAULT_ADMIN_USER);
    if (adminIt == users.end()) {
        LOG_ERROR("UserManager: Cannot save single-admin auth without admin user");
        return false;
    }

    const User& admin = adminIt->second;
    JsonDocument doc;

    JsonObject adminObj = doc["admin"].to<JsonObject>();
    adminObj["username"]     = DEFAULT_ADMIN_USER;
    adminObj["passwordHash"] = admin.passwordHash;
    adminObj["salt"]         = admin.salt;
    adminObj["lastLogin"]    = admin.lastLogin;
    adminObj["lastModified"] = admin.lastModified;
#else
    JsonDocument doc;

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
    security["requireStrongPasswords"]  = config.requireStrongPasswords;

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
    UsersLock lk(_usersMutex);
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

void UserManager::updatePasswordPolicy(uint8_t maxAttempts, uint32_t lockoutTime,
                                       uint8_t minPwdLen, bool requireStrong) {
    config.maxLoginAttempts       = maxAttempts;
    config.loginLockoutTime       = lockoutTime;
    config.minPasswordLength      = minPwdLen;
    config.requireStrongPasswords = requireStrong;
    saveUsersToStorage();
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
    UsersLock lk(_usersMutex);
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
    // 登录异步化：验证在独立工作任务执行，与 lwIP 线程的用户表访问并发。
    // 分三段：锁内快照 → 锁外 PBKDF2（秒级，不得持锁）→ 锁内记账
    String storedHash;
    String storedSalt;
    bool enabled = false;
    bool exists = false;

    {
        UsersLock lk(_usersMutex);
        auto it = users.find(username);
        if (it != users.end()) {
            exists = true;
            enabled = it->second.enabled;
            storedHash = it->second.passwordHash;
            storedSalt = it->second.salt;
        }
    }

    if (!exists || !enabled) {
        recordLoginFailure(username);
        return false;
    }

    // 锁外执行耗时的密码验证（新格式 PBKDF2 / 旧格式单轮 SHA256）
    if (!verifyPassword(password, storedHash, storedSalt)) {
        recordLoginFailure(username);
        return false;
    }

    // 透明迁移：旧格式（单轮 SHA256）验证成功后重哈希为 $fb2$ PBKDF2 格式。
    // 仅登录成功路径触发一次，后续登录已是新格式不会重复计算。
    // 重哈希同样在锁外计算，失败不阻断登录，下次登录会再次尝试迁移。
    if (!storedHash.startsWith(PASSWORD_HASH_PREFIX)) {
        String upgraded = hashPasswordPbkdf2(password);
        if (!upgraded.isEmpty()) {
            UsersLock lk(_usersMutex);
            auto it = users.find(username);
            if (it != users.end()) {
                it->second.passwordHash = upgraded;
                it->second.salt = generateSalt();  // 新格式盐已内嵌，刷新遗留 salt 字段
                it->second.lastModified = millis();
                if (saveUsersToStorage()) {
                    LOG_INFO("UserManager: Password hash upgraded to PBKDF2 ($fb2$)");
                }
            }
        }
    }

    resetLoginAttempts(username);
    updateLastLogin(username);
    
    return true;
}


String UserManager::getAllUsers() {
    UsersLock lk(_usersMutex);
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
