#include "./security/UserManager.h"
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>
#include <esp_random.h>

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
    preferences.begin("user_manager", false);
}

UserManager::~UserManager() {
    saveUsersToStorage();
    preferences.end();
}

bool UserManager::initialize() {
    // 加载配置
    if (!loadConfig()) {
        Serial.println("[UserManager] Using default config");
    }
    
    // 加载用户数据
    if (!loadUsersFromStorage()) {
        Serial.println("[UserManager] No user data found, creating default admin");
        initializeDefaultAdmin();
    }
    
    Serial.printf("[UserManager] Initialized with %d users\n", users.size());
    return true;
}

void UserManager::initializeDefaultAdmin() {
    User admin;
    admin.username = DEFAULT_ADMIN_USER;
    admin.salt = generateSalt();
    admin.passwordHash = hashPassword(DEFAULT_ADMIN_PASS, admin.salt);
    admin.role = UserRole::ADMIN;
    admin.enabled = true;
    admin.createTime = millis();
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
    // 使用SHA256哈希
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

bool UserManager::addUser(const String& username, const String& password, UserRole role) {
    // 检查用户名是否存在
    if (users.find(username) != users.end()) {
        return false;
    }
    
    // 验证密码强度
    if (!validatePasswordStrength(password)) {
        return false;
    }
    
    // 创建新用户
    User newUser;
    newUser.username = username;
    newUser.salt = generateSalt();
    newUser.passwordHash = hashPassword(password, newUser.salt);
    newUser.role = role;
    newUser.enabled = true;
    newUser.createTime = millis();
    newUser.lastModified = millis();
    
    users[username] = newUser;
    
    return saveUsersToStorage();
}

bool UserManager::deleteUser(const String& username) {
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
}

bool UserManager::updateUser(const String& username, const String& newPassword,
                           const String& newRole, bool enabled) {
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
    
    // 更新角色
    if (!newRole.isEmpty()) {
        user.role = stringToRole(newRole);
    }
    
    // 更新启用状态
    user.enabled = enabled;
    
    // 更新修改时间
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
    
    // 验证旧密码
    if (!verifyPassword(oldPassword, user.passwordHash, user.salt)) {
        return false;
    }
    
    // 验证新密码强度
    if (!validatePasswordStrength(newPassword)) {
        return false;
    }
    
    // 更新密码
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

bool UserManager::authenticateUser(const String& username, const String& password) {
    // 检查账户是否被锁定
    if (isAccountLocked(username)) {
        return false;
    }
    
    auto it = users.find(username);
    if (it == users.end()) {
        recordLoginFailure(username);
        return false;
    }
    
    User& user = it->second;
    
    // 检查用户是否启用
    if (!user.enabled) {
        return false;
    }
    
    // 验证密码
    if (!verifyPassword(password, user.passwordHash, user.salt)) {
        recordLoginFailure(username);
        return false;
    }
    
    // 验证成功，重置登录尝试
    resetLoginAttempts(username);
    updateLastLogin(username);
    
    return true;
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

std::vector<User> UserManager::getAllUsers() {
    std::vector<User> userList;
    for (const auto& pair : users) {
        userList.push_back(pair.second);
    }
    return userList;
}

bool UserManager::userExists(const String& username) {
    return users.find(username) != users.end();
}

bool UserManager::isUserEnabled(const String& username) {
    auto it = users.find(username);
    return (it != users.end()) ? it->second.enabled : false;
}

UserRole UserManager::getUserRole(const String& username) {
    auto it = users.find(username);
    return (it != users.end()) ? it->second.role : UserRole::VIEWER;
}

// ============ 登录保护方法 ============

void UserManager::recordLoginFailure(const String& username) {
    if (!config.maxLoginAttempts) return;
    
    loginAttempts[username]++;
    
    // 如果超过最大尝试次数，记录锁定时间（在AuthManager中处理）
    if (loginAttempts[username] >= config.maxLoginAttempts) {
        Serial.printf("[UserManager] Account %s locked due to too many failed attempts\n", 
                     username.c_str());
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
        stats.role = it->second.role;
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
    StaticJsonDocument<4096> doc;
    JsonArray usersArray = doc.createNestedArray("users");
    
    for (const auto& pair : users) {
        const User& user = pair.second;
        JsonObject userObj = usersArray.createNestedObject();
        
        userObj["username"] = user.username;
        userObj["passwordHash"] = user.passwordHash;
        userObj["salt"] = user.salt;
        userObj["role"] = static_cast<int>(user.role);
        userObj["enabled"] = user.enabled;
        userObj["createTime"] = user.createTime;
        userObj["lastLogin"] = user.lastLogin;
        userObj["lastModified"] = user.lastModified;
    }
    
    String jsonStr;
    serializeJson(doc, jsonStr);
    
    bool success = preferences.putString("users", jsonStr) > 0;
    if (success) {
        Serial.println("[UserManager] Users saved to storage");
    }
    
    return success;
}

bool UserManager::loadUsersFromStorage() {
    String jsonStr = preferences.getString("users", "");
    if (jsonStr.isEmpty()) {
        return false;
    }
    
    StaticJsonDocument<4096> doc;
    DeserializationError error = deserializeJson(doc, jsonStr);
    if (error) {
        Serial.printf("[UserManager] Failed to parse user data: %s\n", error.c_str());
        return false;
    }
    
    users.clear();
    
    JsonArray usersArray = doc["users"];
    for (JsonObject userObj : usersArray) {
        User user;
        user.username = userObj["username"].as<String>();
        user.passwordHash = userObj["passwordHash"].as<String>();
        user.salt = userObj["salt"].as<String>();
        user.role = static_cast<UserRole>(userObj["role"].as<int>());
        user.enabled = userObj["enabled"];
        user.createTime = userObj["createTime"];
        user.lastLogin = userObj["lastLogin"];
        user.lastModified = userObj["lastModified"];
        
        users[user.username] = user;
    }
    
    Serial.printf("[UserManager] Loaded %d users from storage\n", users.size());
    return true;
}

bool UserManager::saveConfig() {
    preferences.putUChar("max_login_attempts", config.maxLoginAttempts);
    preferences.putULong("login_lockout_time", config.loginLockoutTime);
    preferences.putUChar("min_password_length", config.minPasswordLength);
    preferences.putUChar("max_password_length", config.maxPasswordLength);
    preferences.putBool("require_strong_passwords", config.requireStrongPasswords);
    preferences.putBool("allow_multiple_sessions", config.allowMultipleSessions);
    
    Serial.println("[UserManager] Config saved");
    return true;
}

bool UserManager::loadConfig() {
    config.maxLoginAttempts = preferences.getUChar("max_login_attempts", 5);
    config.loginLockoutTime = preferences.getULong("login_lockout_time", 300000);
    config.minPasswordLength = preferences.getUChar("min_password_length", 6);
    config.maxPasswordLength = preferences.getUChar("max_password_length", 32);
    config.requireStrongPasswords = preferences.getBool("require_strong_passwords", false);
    config.allowMultipleSessions = preferences.getBool("allow_multiple_sessions", true);
    
    return true;
}

// ============ 静态工具方法 ============

String UserManager::roleToString(UserRole role) {
    switch (role) {
        case UserRole::ADMIN: return "admin";
        case UserRole::USER: return "user";
        case UserRole::VIEWER: return "viewer";
        default: return "unknown";
    }
}

UserRole UserManager::stringToRole(const String& roleStr) {
    if (roleStr == "admin") return UserRole::ADMIN;
    if (roleStr == "user") return UserRole::USER;
    return UserRole::VIEWER;
}

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