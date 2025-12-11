/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:32:12
 */

#include "security/UserManager.h"
#include "security/CryptoUtils.h"

UserManager::UserManager() {
    // 初始化默认用户
    User defaultUser;
    defaultUser.username = "admin";
    defaultUser.passwordHash = hashPassword("admin");
    defaultUser.role = "admin";
    defaultUser.enabled = true;
    
    users.push_back(defaultUser);
}

bool UserManager::initialize() {
    return loadUsersFromConfig();
}

String UserManager::hashPassword(const String& password) {
    // 在实际项目中应该使用安全的哈希算法
    // 这里使用简化实现
    return CryptoUtils::simpleHash(password);
}

bool UserManager::verifyPassword(const String& password, const String& hash) {
    return hashPassword(password) == hash;
}

bool UserManager::addUser(const String& username, const String& password, const String& role) {
    // 检查用户是否已存在
    for (auto& user : users) {
        if (user.username == username) {
            return false;
        }
    }
    
    User newUser;
    newUser.username = username;
    newUser.passwordHash = hashPassword(password);
    newUser.role = role;
    newUser.enabled = true;
    
    users.push_back(newUser);
    return saveUsersToConfig();
}

bool UserManager::deleteUser(const String& username) {
    if (username == "admin") {
        return false; // 不能删除admin用户
    }
    
    for (auto it = users.begin(); it != users.end(); ++it) {
        if (it->username == username) {
            users.erase(it);
            return saveUsersToConfig();
        }
    }
    return false;
}

bool UserManager::updateUser(const String& username, const String& newPassword, const String& newRole) {
    for (auto& user : users) {
        if (user.username == username) {
            if (!newPassword.isEmpty()) {
                user.passwordHash = hashPassword(newPassword);
            }
            if (!newRole.isEmpty()) {
                user.role = newRole;
            }
            return saveUsersToConfig();
        }
    }
    return false;
}

bool UserManager::authenticateUser(const String& username, const String& password) {
    for (auto& user : users) {
        if (user.username == username && user.enabled) {
            return verifyPassword(password, user.passwordHash);
        }
    }
    return false;
}

User* UserManager::getUser(const String& username) {
    for (auto& user : users) {
        if (user.username == username) {
            return &user;
        }
    }
    return nullptr;
}

std::vector<String> UserManager::getAllUsernames() {
    std::vector<String> usernames;
    for (auto& user : users) {
        usernames.push_back(user.username);
    }
    return usernames;
}

bool UserManager::hasPermission(const String& username, const String& permission) {
    User* user = getUser(username);
    if (!user) {
        return false;
    }
    
    // 简化权限检查
    if (user->role == "admin") {
        return true;
    } else if (user->role == "user") {
        return permission != "system_config"; // 用户不能修改系统配置
    } else if (user->role == "viewer") {
        return permission == "read_only"; // 查看者只有只读权限
    }
    
    return false;
}

bool UserManager::saveUsersToConfig() {
    DynamicJsonDocument doc(2048);
    JsonArray usersArray = doc.createNestedArray("users");
    
    for (auto& user : users) {
        JsonObject userObj = usersArray.createNestedObject();
        userObj["username"] = user.username;
        userObj["password_hash"] = user.passwordHash;
        userObj["role"] = user.role;
        userObj["enabled"] = user.enabled;
    }
    
    return storage->saveUserConfig(doc);
}

bool UserManager::loadUsersFromConfig() {
    DynamicJsonDocument doc(2048);
    if (!storage->loadUserConfig(doc)) {
        return false;
    }
    
    // users.clear();
    
    JsonArray usersArray = doc["users"];
    for (JsonObject userObj : usersArray) {
        User user;
        user.username = userObj["username"].as<String>();
        user.passwordHash = userObj["password_hash"].as<String>();
        user.role = userObj["role"].as<String>();
        user.enabled = userObj["enabled"].as<bool>();
        
        users.push_back(user);
    }
    
    return true;
}