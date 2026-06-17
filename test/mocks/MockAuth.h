/**
 * @file MockAuth.h
 * @brief 安全认证模拟对象（单管理员模式）
 * 
 * 提供用户管理、认证功能的模拟实现
 */

#ifndef MOCK_AUTH_H
#define MOCK_AUTH_H

#include <Arduino.h>
#include <vector>
#include <map>
#include <ctime>

// 用户结构
struct User {
    String username;
    String passwordHash;
    bool enabled;
    int failedLoginAttempts;
    time_t lockedUntil;
    time_t lastLogin;
    
    User() : enabled(true), failedLoginAttempts(0), 
             lockedUntil(0), lastLogin(0) {}
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

// 模拟用户管理器
class MockUserManager {
public:
    static MockUserManager& getInstance() {
        static MockUserManager instance;
        return instance;
    }

    bool initialize() {
        if (!userExists("admin")) {
            createUser("admin", "admin123");
        }
        return true;
    }

    // 用户CRUD
    bool createUser(const String& username, const String& password) {
        if (username.isEmpty() || password.isEmpty()) return false;
        if (userExists(username)) return false;
        
        User user;
        user.username = username;
        user.passwordHash = hashPassword(password);
        user.enabled = true;
        
        _users[username] = user;
        return true;
    }

    bool deleteUser(const String& username) {
        if (username == "admin") return false;
        
        auto it = _users.find(username);
        if (it == _users.end()) return false;
        
        _users.erase(it);
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

private:
    MockUserManager() {}

    String hashPassword(const String& password) {
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
    MockAuthManager(MockUserManager* userMgr = nullptr)
        : _userMgr(userMgr ? *userMgr : MockUserManager::getInstance()),
          _sessionTimeout(3600) {}

    bool initialize() {
        _sessions.clear();
        return true;
    }

    // 认证
    String authenticate(const String& username, const String& password) {
        if (!_userMgr.userExists(username)) {
            return "";
        }

        if (_userMgr.isAccountLocked(username)) {
            return "";
        }

        if (!_userMgr.verifyPassword(username, password)) {
            _userMgr.recordFailedLogin(username);
            return "";
        }

        _userMgr.recordSuccessfulLogin(username);
        return createSession(username);
    }

    // 会话管理
    String createSession(const String& username) {
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

    // 在线用户
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
        _persistedSessions = _sessions;
        return true;
    }

    bool loadSessionsFromStorage() {
        _sessions = _persistedSessions;
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
        String id = "sess_";
        for (int i = 0; i < 16; i++) {
            id += String(random(16), HEX);
        }
        return id;
    }

    MockUserManager& _userMgr;
    std::map<String, Session> _sessions;
    std::map<String, Session> _persistedSessions;
    int _sessionTimeout;
};

// 全局实例引用
#define MockUserMgr MockUserManager::getInstance()

#endif // MOCK_AUTH_H
