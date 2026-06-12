/**
 * @file MockAuth.h
 * @brief 安全认证模拟对象
 * 
 * 提供用户管理、角色管理、认证功能的模拟实现
 */

#ifndef MOCK_AUTH_H
#define MOCK_AUTH_H

#include <Arduino.h>
#include <vector>
#include <map>
#include <ctime>

// 角色结构
struct Role {
    String id;
    String name;
    std::vector<String> permissions;
    bool isSystem;  // 系统内置角色，不可删除
    
    Role() : isSystem(false) {}
};

// 用户结构
struct User {
    String username;
    String passwordHash;
    std::vector<String> roles;
    bool enabled;
    int failedLoginAttempts;
    time_t lockedUntil;
    time_t lastLogin;
    bool passwordExpired;
    
    User() : enabled(true), failedLoginAttempts(0), 
             lockedUntil(0), lastLogin(0), passwordExpired(false) {}
};

// 会话结构
struct Session {
    String sessionId;
    String username;
    time_t createdAt;
    time_t expiresAt;
    String ipAddress;
    
    Session() : createdAt(0), expiresAt(0) {}
    
        bool isExpired() {
        return time(nullptr) >= expiresAt;
    }
};

// 模拟角色管理器
class MockRoleManager {
public:
    static MockRoleManager& getInstance() {
        static MockRoleManager instance;
        return instance;
    }

    bool initialize() {
        // 创建内置角色
        createBuiltinRoles();
        return true;
    }

    // 角色CRUD
    bool createRole(const Role& role) {
        if (role.id.isEmpty()) return false;
        if (_roles.find(role.id) != _roles.end()) return false;
        
        _roles[role.id] = role;
        return true;
    }

    bool updateRole(const String& roleId, const Role& role) {
        auto it = _roles.find(roleId);
        if (it == _roles.end()) return false;
        
        // 系统角色不允许修改ID
        if (it->second.isSystem && roleId != role.id) return false;
        
        _roles[roleId] = role;
        return true;
    }

    bool deleteRole(const String& roleId) {
        auto it = _roles.find(roleId);
        if (it == _roles.end()) return false;
        
        // 系统角色不可删除
        if (it->second.isSystem) return false;
        
        _roles.erase(it);
        return true;
    }

    Role* getRole(const String& roleId) {
        auto it = _roles.find(roleId);
        if (it != _roles.end()) {
            return &(it->second);
        }
        return nullptr;
    }

    std::vector<String> getRoleIds() {
        std::vector<String> ids;
        for (auto& entry : _roles) {
            ids.push_back(entry.first);
        }
        return ids;
    }

    // 权限管理
    bool assignPermission(const String& roleId, const String& permission) {
        Role* role = getRole(roleId);
        if (!role) return false;
        
        // 检查是否已存在
        for (auto& perm : role->permissions) {
            if (perm == permission) return true;
        }
        
        role->permissions.push_back(permission);
        return true;
    }

    bool removePermission(const String& roleId, const String& permission) {
        Role* role = getRole(roleId);
        if (!role) return false;
        
        for (auto it = role->permissions.begin(); 
             it != role->permissions.end(); ++it) {
            if (*it == permission) {
                role->permissions.erase(it);
                return true;
            }
        }
        
        return false;
    }

    bool roleHasPermission(const String& roleId, const String& permission) {
        Role* role = getRole(roleId);
        if (!role) return false;
        
        // 检查具体权限
        for (auto& perm : role->permissions) {
            if (perm == permission) return true;
            // 支持通配符，如 "device.*"
            if (perm.endsWith(".*")) {
                String prefix = perm.substring(0, perm.length() - 1);
                if (permission.startsWith(prefix)) return true;
            }
        }
        
        return false;
    }

    // 获取所有权限（用于前端展示）
    std::vector<String> getAllPermissions() {
        return {
            "device.view", "device.control", "device.config",
            "user.view", "user.create", "user.update", "user.delete", "user.admin",
            "role.view", "role.create", "role.update", "role.delete", "role.admin",
            "network.view", "network.edit",
            "mqtt.view", "mqtt.config",
            "modbus.view", "modbus.config",
            "ota.update", "ota.view",
            "system.view", "system.restart",
            "config.view", "config.edit",
            "log.view", "log.clear",
            "peripheral.view", "peripheral.control", "peripheral.config",
            "fs.view", "fs.manage",
            "script.view", "script.execute", "script.edit",
            "rule.view", "rule.edit"
        };
    }

private:
    MockRoleManager() {
        createBuiltinRoles();
    }

    void createBuiltinRoles() {
        // Admin角色 - 拥有所有权限
        Role admin;
        admin.id = "admin";
        admin.name = "Administrator";
        admin.isSystem = true;
        admin.permissions = getAllPermissions();
        _roles["admin"] = admin;

        // Operator角色 - 操作权限
        Role operatorRole;
        operatorRole.id = "operator";
        operatorRole.name = "Operator";
        operatorRole.isSystem = true;
        operatorRole.permissions = {
            "device.view", "device.control",
            "peripheral.view", "peripheral.control",
            "script.view", "script.execute",
            "log.view",
            "config.view",
            "rule.view", "rule.edit"
        };
        _roles["operator"] = operatorRole;

        // Viewer角色 - 只读权限
        Role viewer;
        viewer.id = "viewer";
        viewer.name = "Viewer";
        viewer.isSystem = true;
        viewer.permissions = {
            "device.view",
            "peripheral.view",
            "log.view",
            "system.view"
        };
        _roles["viewer"] = viewer;
    }

    std::map<String, Role> _roles;
};

// 模拟用户管理器
class MockUserManager {
public:
    static MockUserManager& getInstance() {
        static MockUserManager instance;
        return instance;
    }

    bool initialize() {
        // 创建默认管理员用户
        if (!userExists("admin")) {
            createUser("admin", "admin123", {"admin"});
        }
        return true;
    }

    // 用户CRUD
    bool createUser(const String& username, const String& password, 
                    const std::vector<String>& roles) {
        if (username.isEmpty() || password.isEmpty()) return false;
        if (userExists(username)) return false;
        
        User user;
        user.username = username;
        user.passwordHash = hashPassword(password);
        user.roles = roles;
        user.enabled = true;
        
        _users[username] = user;
        return true;
    }

    bool deleteUser(const String& username) {
        // 不能删除admin用户
        if (username == "admin") return false;
        
        auto it = _users.find(username);
        if (it == _users.end()) return false;
        
        _users.erase(it);
        return true;
    }

    bool updateUser(const String& username, const User& user) {
        auto it = _users.find(username);
        if (it == _users.end()) return false;
        
        _users[username] = user;
        return true;
    }

    User* getUser(const String& username) {
        auto it = _users.find(username);
        if (it != _users.end()) {
            return &(it->second);
        }
        return nullptr;
    }

    bool userExists(const String& username) {
        return _users.find(username) != _users.end();
    }

    std::vector<String> getUsernames() {
        std::vector<String> names;
        for (auto& entry : _users) {
            names.push_back(entry.first);
        }
        return names;
    }

    // 密码管理
    bool resetPassword(const String& username, const String& newPassword) {
        User* user = getUser(username);
        if (!user) return false;
        
        user->passwordHash = hashPassword(newPassword);
        user->failedLoginAttempts = 0;
        user->lockedUntil = 0;
        user->passwordExpired = false;
        
        return true;
    }

    bool verifyPassword(const String& username, const String& password) {
        User* user = getUser(username);
        if (!user) return false;
        
        return user->passwordHash == hashPassword(password);
    }

    bool changePassword(const String& username, const String& oldPassword, 
                        const String& newPassword) {
        if (!verifyPassword(username, oldPassword)) return false;
        return resetPassword(username, newPassword);
    }

    // 账户锁定
    bool isAccountLocked(const String& username) {
        User* user = getUser(username);
        if (!user) return false;
        
        if (user->lockedUntil > 0 && time(nullptr) < user->lockedUntil) {
            return true;
        }
        
        // 锁定时间已过，解锁
        if (user->lockedUntil > 0) {
            user->lockedUntil = 0;
            user->failedLoginAttempts = 0;
        }
        
        return false;
    }

    void recordFailedLogin(const String& username) {
        User* user = getUser(username);
        if (!user) return;
        
        user->failedLoginAttempts++;
        
        // 5次失败锁定30分钟
        if (user->failedLoginAttempts >= 5) {
            user->lockedUntil = time(nullptr) + 30 * 60;
        }
    }

    void recordSuccessfulLogin(const String& username) {
        User* user = getUser(username);
        if (!user) return;
        
        user->failedLoginAttempts = 0;
        user->lockedUntil = 0;
        user->lastLogin = time(nullptr);
    }

    // 角色管理
    bool assignRole(const String& username, const String& roleId) {
        User* user = getUser(username);
        if (!user) return false;
        
        // 检查角色是否存在
        if (!MockRoleManager::getInstance().getRole(roleId)) return false;
        
        // 检查是否已分配
        for (auto& role : user->roles) {
            if (role == roleId) return true;
        }
        
        user->roles.push_back(roleId);
        return true;
    }

    bool removeRole(const String& username, const String& roleId) {
        User* user = getUser(username);
        if (!user) return false;
        
        // 不能移除admin用户的admin角色
        if (username == "admin" && roleId == "admin") return false;
        
        for (auto it = user->roles.begin(); it != user->roles.end(); ++it) {
            if (*it == roleId) {
                user->roles.erase(it);
                return true;
            }
        }
        
        return false;
    }

    std::vector<String> getUserRoles(const String& username) {
        User* user = getUser(username);
        if (!user) return {};
        return user->roles;
    }

private:
    MockUserManager() {}

    String hashPassword(const String& password) {
        // 简化实现：使用简单的哈希
        // 实际应使用SHA256等安全哈希
        unsigned long hash = 5381;
        for (int i = 0; i < password.length(); i++) {
            hash = ((hash << 5) + hash) + password[i];
        }
        return String(hash, HEX);
    }

    std::map<String, User> _users;
};

// 模拟认证管理器
class MockAuthManager {
public:
    MockAuthManager(MockUserManager* userMgr = nullptr, 
                    MockRoleManager* roleMgr = nullptr)
        : _userMgr(userMgr ? *userMgr : MockUserManager::getInstance()),
          _roleMgr(roleMgr ? *roleMgr : MockRoleManager::getInstance()),
          _sessionTimeout(3600) {}  // 默认1小时超时

    bool initialize() {
        _sessions.clear();
        return true;
    }

    // 认证
    String authenticate(const String& username, const String& password) {
        // 检查用户是否存在
        if (!_userMgr.userExists(username)) {
            return "";
        }

        // 检查账户是否锁定
        if (_userMgr.isAccountLocked(username)) {
            return "";
        }

        // 验证密码
        if (!_userMgr.verifyPassword(username, password)) {
            _userMgr.recordFailedLogin(username);
            return "";
        }

        // 创建会话
        _userMgr.recordSuccessfulLogin(username);
        return createSession(username);
    }

    // 会话管理
    String createSession(const String& username) {
        // 生成会话ID
        String sessionId = generateSessionId();
        
        Session session;
        session.sessionId = sessionId;
        session.username = username;
        session.createdAt = time(nullptr);
        session.expiresAt = session.createdAt + _sessionTimeout;
        
        _sessions[sessionId] = session;
        return sessionId;
    }

    bool validateSession(const String& sessionId) {
        auto it = _sessions.find(sessionId);
        if (it == _sessions.end()) return false;
        
        if (it->second.isExpired()) {
            _sessions.erase(it);
            return false;
        }
        
        return true;
    }

    void invalidateSession(const String& sessionId) {
        _sessions.erase(sessionId);
    }

    void invalidateAllSessions(const String& username) {
        for (auto it = _sessions.begin(); it != _sessions.end();) {
            if (it->second.username == username) {
                it = _sessions.erase(it);
            } else {
                ++it;
            }
        }
    }

    Session* getSession(const String& sessionId) {
        auto it = _sessions.find(sessionId);
        if (it != _sessions.end() && !it->second.isExpired()) {
            return &(it->second);
        }
        return nullptr;
    }

    String getSessionUser(const String& sessionId) {
        Session* session = getSession(sessionId);
        if (session) return session->username;
        return "";
    }

    // 权限检查
    bool userHasPermission(const String& username, const String& permission) {
        std::vector<String> roles = _userMgr.getUserRoles(username);
        
        for (auto& roleId : roles) {
            if (_roleMgr.roleHasPermission(roleId, permission)) {
                return true;
            }
        }
        
        return false;
    }

    bool sessionHasPermission(const String& sessionId, const String& permission) {
        String username = getSessionUser(sessionId);
        if (username.isEmpty()) return false;
        return userHasPermission(username, permission);
    }

    // 检查资源访问权限
    bool checkPermission(const String& sessionId, const String& resource,
                         const String& action) {
        String permission = resource + "." + action;
        return sessionHasPermission(sessionId, permission);
    }

    // 在线用户
    std::vector<String> getOnlineUsers() {
        std::vector<String> users;
        
        for (auto& entry : _sessions) {
            if (!entry.second.isExpired()) {
                users.push_back(entry.second.username);
            }
        }
        
        return users;
    }

    int getOnlineUserCount() {
        int count = 0;
        for (auto& entry : _sessions) {
            if (!entry.second.isExpired()) {
                count++;
            }
        }
        return count;
    }

    // 会话持久化（模拟）
    bool saveSessionsToStorage() {
        // 模拟保存到存储
        _persistedSessions = _sessions;
        return true;
    }

    bool loadSessionsFromStorage() {
        // 模拟从存储加载
        _sessions = _persistedSessions;
        // 清理过期会话
        for (auto it = _sessions.begin(); it != _sessions.end();) {
            if (it->second.isExpired()) {
                it = _sessions.erase(it);
            } else {
                ++it;
            }
        }
        return true;
    }

    // 设置会话超时
    void setSessionTimeout(int seconds) {
        _sessionTimeout = seconds;
    }

    int getSessionTimeout() {
        return _sessionTimeout;
    }

private:
    String generateSessionId() {
        // 生成随机会话ID
        String id = "sess_";
        for (int i = 0; i < 16; i++) {
            id += String(random(16), HEX);
        }
        return id;
    }

    MockUserManager& _userMgr;
    MockRoleManager& _roleMgr;
    std::map<String, Session> _sessions;
    std::map<String, Session> _persistedSessions;
    int _sessionTimeout;
};

// 全局实例引用
#define MockRoleMgr MockRoleManager::getInstance()
#define MockUserMgr MockUserManager::getInstance()

#endif // MOCK_AUTH_H
