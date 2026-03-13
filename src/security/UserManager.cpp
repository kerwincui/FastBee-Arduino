#include "./security/UserManager.h"
#include "security/RoleManager.h"
#include "systems/LoggerSystem.h"
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
    // JSON 文件存储，无需初始化 NVS
}

UserManager::~UserManager() {
    saveUsersToStorage();
}

bool UserManager::initialize() {
    if (!loadConfig()) {
        LOG_INFO("UserManager: Using default config");
    }

    if (!loadUsersFromStorage() || users.empty()) {
        LOG_INFO("UserManager: No user data found, creating default users");
        initializeDefaultAdmin();
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "UserManager: Initialized with %u users", (unsigned)users.size());
    LOG_INFO(buf);
    return true;
}

void UserManager::initializeDefaultAdmin() {
    // 创建默认管理员用户
    User admin;
    admin.username     = DEFAULT_ADMIN_USER;
    admin.salt         = generateSalt();
    admin.passwordHash = hashPassword(DEFAULT_ADMIN_PASS, admin.salt);
    admin.role         = UserRole::ADMIN;
    admin.roles        = { BuiltinRoles::ADMIN };  // 绑定内置 admin 角色
    admin.enabled      = true;
    admin.createTime   = millis();
    admin.lastModified = millis();
    admin.createBy     = "system";

    users[DEFAULT_ADMIN_USER] = admin;
    
    // 创建默认查看者用户
    User viewer;
    viewer.username     = "viewer";
    viewer.salt         = generateSalt();
    viewer.passwordHash = hashPassword(DEFAULT_ADMIN_PASS, viewer.salt);  // 使用相同的默认密码 admin123
    viewer.role         = UserRole::VIEWER;
    viewer.roles        = { BuiltinRoles::VIEWER };  // 绑定内置 viewer 角色
    viewer.enabled      = true;
    viewer.createTime   = millis();
    viewer.lastModified = millis();
    viewer.createBy     = "system";

    users["viewer"] = viewer;
    
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
    
    // 如果超过最大尝试次数，记录锁定时间（在AuthManager中处理）
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
    JsonDocument doc;
    doc["version"] = "2.0";
    
    // 保存用户数据
    JsonArray usersArray = doc["users"].to<JsonArray>();
    for (const auto& pair : users) {
        const User& user = pair.second;
        JsonObject userObj = usersArray.add<JsonObject>();

        userObj["username"]     = user.username;
        userObj["passwordHash"] = user.passwordHash;
        userObj["salt"]         = user.salt;
        userObj["role"]         = static_cast<int>(user.role);
        userObj["enabled"]      = user.enabled;
        userObj["createTime"]   = user.createTime;
        userObj["lastLogin"]    = user.lastLogin;
        userObj["lastModified"] = user.lastModified;
        userObj["email"]        = user.email;
        userObj["remark"]       = user.remark;
        userObj["createBy"]     = user.createBy;

        // 多角色列表
        JsonArray rolesArr = userObj["roles"].to<JsonArray>();
        for (const String& r : user.roles) {
            rolesArr.add(r);
        }
    }
    
    // 保存安全配置
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

    // 确保目录存在
    if (!LittleFS.exists("/config")) {
        if (!LittleFS.mkdir("/config")) {
            LOG_ERROR("UserManager: Failed to create /config directory");
            return false;
        }
    }
    
    // 写入文件
    File file = LittleFS.open(USERS_CONFIG_FILE, "w");
    if (!file) {
        LOG_ERROR("UserManager: Failed to open users file for writing");
        return false;
    }
    
    size_t written = serializeJson(doc, file);
    file.close();
    
    if (written > 0) {
        LOG_DEBUG("UserManager: Users saved to file");
        return true;
    } else {
        LOG_ERROR("UserManager: Failed to write users to file");
        return false;
    }
}

bool UserManager::loadUsersFromStorage() {
    if (!LittleFS.exists(USERS_CONFIG_FILE)) {
        LOG_INFO("UserManager: Users file not found");
        return false;
    }
    
    File file = LittleFS.open(USERS_CONFIG_FILE, "r");
    if (!file) {
        LOG_ERROR("UserManager: Failed to open users file for reading");
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        char buf[64];
        snprintf(buf, sizeof(buf), "UserManager: Failed to parse user data: %s", error.c_str());
        LOG_ERROR(buf);
        return false;
    }

    users.clear();

    // 加载用户数据
    JsonArray usersArray = doc["users"];
    for (JsonObject userObj : usersArray) {
        User user;
        user.username     = userObj["username"].as<String>();
        user.passwordHash = userObj["passwordHash"].as<String>();
        user.salt         = userObj["salt"].as<String>();
        user.role         = static_cast<UserRole>(userObj["role"].as<int>());
        user.enabled      = userObj["enabled"] | true;
        user.createTime   = userObj["createTime"] | 0UL;
        user.lastLogin    = userObj["lastLogin"]  | 0UL;
        user.lastModified = userObj["lastModified"] | 0UL;
        user.email        = userObj["email"].as<String>();
        user.remark       = userObj["remark"].as<String>();
        user.createBy     = userObj["createBy"].as<String>();

        // 多角色列表（向下兼容：无 roles 字段时从旧枚举生成）
        JsonArray rolesArr = userObj["roles"];
        if (!rolesArr.isNull() && rolesArr.size() > 0) {
            for (const auto& r : rolesArr) {
                String roleId = r.as<String>();
                // 迁移旧数据：将 'user' 转换为 'operator'
                if (roleId == "user") {
                    roleId = "operator";
                }
                user.roles.push_back(roleId);
            }
        } else {
            user.roles.push_back(roleToString(user.role));
        }

        users[user.username] = user;
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
        config.enableSessionPersistence= security["enableSessionPersistence"] | true;
        config.cookieName              = security["cookieName"] | "session";
        config.cookieMaxAge            = security["cookieMaxAge"] | 3600UL;
        config.cookieHttpOnly          = security["cookieHttpOnly"] | true;
        config.cookieSecure            = security["cookieSecure"] | false;
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "UserManager: Loaded %u users from file", (unsigned)users.size());
    LOG_INFO(buf);
    return true;
}

bool UserManager::saveConfig() {
    // 安全配置已集成到 saveUsersToStorage() 中统一保存
    return saveUsersToStorage();
}

bool UserManager::loadConfig() {
    // 安全配置已集成到 loadUsersFromStorage() 中统一加载
    // 此处仅返回 true，实际加载在 loadUsersFromStorage() 中完成
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

String UserManager::roleToString(UserRole role) {
    switch (role) {
        case UserRole::ADMIN: return "admin";
        case UserRole::USER: return "operator";  // 前端使用 operator 表示操作员
        case UserRole::VIEWER: return "viewer";
        default: return "viewer";
    }
}

UserRole UserManager::stringToRole(const String& roleStr) {
    if (roleStr == "admin") return UserRole::ADMIN;
    if (roleStr == "user" || roleStr == "operator") return UserRole::USER;  // 兼容 user 和 operator
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

// ============ 多角色管理方法 ============

bool UserManager::assignRole(const String& username, const String& roleId) {
    auto it = users.find(username);
    if (it == users.end()) return false;
    if (!it->second.hasRole(roleId)) {
        it->second.roles.push_back(roleId);
    }
    return saveUsersToStorage();
}

bool UserManager::removeRole(const String& username, const String& roleId) {
    auto it = users.find(username);
    if (it == users.end()) return false;
    auto& roleVec = it->second.roles;
    for (auto rit = roleVec.begin(); rit != roleVec.end(); ++rit) {
        if (*rit == roleId) {
            roleVec.erase(rit);
            return saveUsersToStorage();
        }
    }
    return false;
}

bool UserManager::setRoles(const String& username, const std::vector<String>& roleIds) {
    auto it = users.find(username);
    if (it == users.end()) return false;
    it->second.roles = roleIds;
    // 同步旧枚举字段（取第一个角色）
    if (!roleIds.empty()) {
        it->second.role = stringToRole(roleIds[0]);
    }
    return saveUsersToStorage();
}

std::vector<String> UserManager::getUserRoles(const String& username) const {
    auto it = users.find(username);
    return (it != users.end()) ? it->second.roles : std::vector<String>{};
}

bool UserManager::hasRole(const String& username, const String& roleId) const {
    auto it = users.find(username);
    return (it != users.end()) && it->second.hasRole(roleId);
}

bool UserManager::updateUserMeta(const String& username, const String& email, const String& remark) {
    auto it = users.find(username);
    if (it == users.end()) return false;
    if (!email.isEmpty())  it->second.email  = email;
    if (!remark.isEmpty()) it->second.remark = remark;
    it->second.lastModified = millis();
    return saveUsersToStorage();
}

// IUserManager 接口实现

bool UserManager::addUser(const String& username, const String& password, const String& role) {
    UserRole userRole = stringToRole(role);
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
    newUser.role         = userRole;
    newUser.roles        = { role };   // 同步多角色列表
    newUser.enabled      = true;
    newUser.createTime   = millis();
    newUser.lastModified = millis();

    users[username] = newUser;
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
    
    // 检查用户是否启用
    if (!user.enabled) {
        recordLoginFailure(username);
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

String UserManager::getUserRole(const String& username) {
    auto it = users.find(username);
    UserRole role = (it != users.end()) ? it->second.role : UserRole::VIEWER;
    return roleToString(role);
}

String UserManager::getAllUsers() {
    JsonDocument doc;
    JsonArray usersArray = doc["users"].to<JsonArray>();

    for (const auto& pair : users) {
        const User& user = pair.second;
        JsonObject userObj = usersArray.add<JsonObject>();
        userObj["username"]   = user.username;
        userObj["role"]       = roleToString(user.role);   // 兼容旧字段
        userObj["enabled"]    = user.enabled;
        userObj["createTime"] = user.createTime;
        userObj["lastLogin"]  = user.lastLogin;
        userObj["email"]      = user.email;
        userObj["remark"]     = user.remark;
        userObj["createBy"]   = user.createBy;

        JsonArray rolesArr = userObj["roles"].to<JsonArray>();
        for (const String& r : user.roles) {
            rolesArr.add(r);
        }
    }

    String jsonStr;
    serializeJson(doc, jsonStr);
    return jsonStr;
}