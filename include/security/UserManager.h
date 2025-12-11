#ifndef USER_MANAGER_H
#define USER_MANAGER_H

#include <ArduinoJson.h>
#include "../systems/ConfigStorage.h"
#include <vector>

struct User {
    String username;
    String passwordHash;
    String role; // "admin", "user", "viewer"
    bool enabled;
};

class UserManager {
private:
    std::vector<User> users;
    ConfigStorage* storage;
    
    String hashPassword(const String& password);
    bool verifyPassword(const String& password, const String& hash);
    
public:
    UserManager();
    bool initialize();
    
    // 用户管理
    bool addUser(const String& username, const String& password, const String& role);
    bool deleteUser(const String& username);
    bool updateUser(const String& username, const String& newPassword, const String& newRole);
    bool authenticateUser(const String& username, const String& password);
    
    // 用户查询
    User* getUser(const String& username);
    std::vector<String> getAllUsernames();
    
    // 权限检查
    bool hasPermission(const String& username, const String& permission);
    
    // 配置持久化
    bool saveUsersToConfig();
    bool loadUsersFromConfig();
};

#endif