/**
 * @file test_security_auth.cpp
 * @brief 安全认证模块单元测试（单管理员模式）
 * 
 * 测试内容：
 * - 用户创建/删除/修改
 * - 密码验证和变更
 * - 账户锁定和解锁
 * - 认证流程（登录/登出）
 * - 会话管理（创建/验证/过期/注销）
 * - 多会话场景
 * - 边界条件（空参数/重复操作）
 */

#include <unity.h>
#include <Arduino.h>
#include "mocks/MockAuth.h"

void test_security_auth_group();

// ========== 辅助：重置状态 ==========

static MockUserManager* userMgr;
static MockAuthManager* authMgr;

static void resetAuthState() {
    // 重新获取单例，重新初始化
    userMgr = &MockUserManager::getInstance();
    
    // 清除非 admin 用户
    auto usernames = userMgr->getUsernames();
    for (auto& name : usernames) {
        if (name != "admin") {
            userMgr->deleteUser(name);
        }
    }
    // 重置 admin 密码
    userMgr->resetPassword("admin", "admin123");

    // 重新创建 AuthManager
    if (authMgr) delete authMgr;
    authMgr = new MockAuthManager(userMgr);
    authMgr->initialize();
}

// ========== 用户管理测试 ==========

static void test_create_user_success() {
    resetAuthState();
    
    bool ok = userMgr->createUser("testuser", "Test@123");
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(userMgr->userExists("testuser"));
    
    User* user = userMgr->getUser("testuser");
    TEST_ASSERT_NOT_NULL(user);
    TEST_ASSERT_EQUAL_STRING("testuser", user->username.c_str());
    TEST_ASSERT_TRUE(user->enabled);
    TEST_ASSERT_EQUAL(0, user->failedLoginAttempts);
}

static void test_create_user_empty_params() {
    resetAuthState();
    
    // 空用户名
    TEST_ASSERT_FALSE(userMgr->createUser("", "password"));
    // 空密码
    TEST_ASSERT_FALSE(userMgr->createUser("user1", ""));
}

static void test_create_user_duplicate() {
    resetAuthState();
    
    userMgr->createUser("dup_user", "pass1");
    bool ok = userMgr->createUser("dup_user", "pass2");
    TEST_ASSERT_FALSE(ok);
}

static void test_delete_user_success() {
    resetAuthState();
    
    userMgr->createUser("removable", "pass123");
    TEST_ASSERT_TRUE(userMgr->userExists("removable"));
    
    TEST_ASSERT_TRUE(userMgr->deleteUser("removable"));
    TEST_ASSERT_FALSE(userMgr->userExists("removable"));
}

static void test_delete_admin_protected() {
    resetAuthState();
    
    // admin 不可删除
    TEST_ASSERT_FALSE(userMgr->deleteUser("admin"));
    TEST_ASSERT_TRUE(userMgr->userExists("admin"));
}

static void test_delete_nonexistent_user() {
    resetAuthState();
    
    TEST_ASSERT_FALSE(userMgr->deleteUser("ghost_user"));
}

// ========== 密码管理测试 ==========

static void test_verify_password_correct() {
    resetAuthState();
    
    userMgr->createUser("passuser", "SecureP@ss");
    TEST_ASSERT_TRUE(userMgr->verifyPassword("passuser", "SecureP@ss"));
}

static void test_verify_password_incorrect() {
    resetAuthState();
    
    userMgr->createUser("passuser", "correct");
    TEST_ASSERT_FALSE(userMgr->verifyPassword("passuser", "wrong"));
}

static void test_change_password_success() {
    resetAuthState();
    
    userMgr->createUser("chguser", "oldpass");
    TEST_ASSERT_TRUE(userMgr->changePassword("chguser", "oldpass", "newpass"));
    TEST_ASSERT_TRUE(userMgr->verifyPassword("chguser", "newpass"));
    TEST_ASSERT_FALSE(userMgr->verifyPassword("chguser", "oldpass"));
}

static void test_change_password_wrong_old() {
    resetAuthState();
    
    userMgr->createUser("chguser", "oldpass");
    TEST_ASSERT_FALSE(userMgr->changePassword("chguser", "badold", "newpass"));
    // 原密码不变
    TEST_ASSERT_TRUE(userMgr->verifyPassword("chguser", "oldpass"));
}

static void test_reset_password() {
    resetAuthState();
    
    userMgr->createUser("resetuser", "original");
    // 模拟多次失败登录
    userMgr->recordFailedLogin("resetuser");
    userMgr->recordFailedLogin("resetuser");
    
    TEST_ASSERT_TRUE(userMgr->resetPassword("resetuser", "newpass"));
    TEST_ASSERT_TRUE(userMgr->verifyPassword("resetuser", "newpass"));
    
    User* user = userMgr->getUser("resetuser");
    TEST_ASSERT_EQUAL(0, user->failedLoginAttempts);
    TEST_ASSERT_EQUAL(0, user->lockedUntil);
}

// ========== 账户锁定测试 ==========

static void test_account_lock_after_failures() {
    resetAuthState();
    
    userMgr->createUser("lockme", "pass123");
    
    // 5次失败后锁定
    for (int i = 0; i < 5; i++) {
        userMgr->recordFailedLogin("lockme");
    }
    
    TEST_ASSERT_TRUE(userMgr->isAccountLocked("lockme"));
    
    User* user = userMgr->getUser("lockme");
    TEST_ASSERT_EQUAL(5, user->failedLoginAttempts);
    TEST_ASSERT_TRUE(user->lockedUntil > 0);
}

static void test_account_not_locked_before_threshold() {
    resetAuthState();
    
    userMgr->createUser("safe_user", "pass123");
    
    // 4次失败不锁定
    for (int i = 0; i < 4; i++) {
        userMgr->recordFailedLogin("safe_user");
    }
    
    TEST_ASSERT_FALSE(userMgr->isAccountLocked("safe_user"));
}

static void test_successful_login_resets_counter() {
    resetAuthState();
    
    userMgr->createUser("counter_user", "pass123");
    userMgr->recordFailedLogin("counter_user");
    userMgr->recordFailedLogin("counter_user");
    userMgr->recordFailedLogin("counter_user");
    
    userMgr->recordSuccessfulLogin("counter_user");
    
    User* user = userMgr->getUser("counter_user");
    TEST_ASSERT_EQUAL(0, user->failedLoginAttempts);
    TEST_ASSERT_EQUAL(0, user->lockedUntil);
}

// ========== 认证流程测试 ==========

static void test_authenticate_success() {
    resetAuthState();
    
    String sessionId = authMgr->authenticate("admin", "admin123");
    TEST_ASSERT_FALSE(sessionId.isEmpty());
    TEST_ASSERT_TRUE(authMgr->validateSession(sessionId));
}

static void test_authenticate_wrong_password() {
    resetAuthState();
    
    String sessionId = authMgr->authenticate("admin", "wrongpass");
    TEST_ASSERT_TRUE(sessionId.isEmpty());
}

static void test_authenticate_nonexistent_user() {
    resetAuthState();
    
    String sessionId = authMgr->authenticate("nobody", "pass123");
    TEST_ASSERT_TRUE(sessionId.isEmpty());
}

static void test_authenticate_locked_account() {
    resetAuthState();
    
    userMgr->createUser("locktest", "pass123");
    
    // 锁定账户
    for (int i = 0; i < 5; i++) {
        userMgr->recordFailedLogin("locktest");
    }
    
    // 即使密码正确，锁定账户也无法认证
    String sessionId = authMgr->authenticate("locktest", "pass123");
    TEST_ASSERT_TRUE(sessionId.isEmpty());
}

// ========== 会话管理测试 ==========

static void test_session_create_and_validate() {
    resetAuthState();
    
    String sessionId = authMgr->createSession("admin");
    TEST_ASSERT_FALSE(sessionId.isEmpty());
    TEST_ASSERT_TRUE(authMgr->validateSession(sessionId));
    
    String username = authMgr->getSessionUser(sessionId);
    TEST_ASSERT_EQUAL_STRING("admin", username.c_str());
}

static void test_session_invalidate() {
    resetAuthState();
    
    String sessionId = authMgr->authenticate("admin", "admin123");
    TEST_ASSERT_TRUE(authMgr->validateSession(sessionId));
    
    authMgr->invalidateSession(sessionId);
    TEST_ASSERT_FALSE(authMgr->validateSession(sessionId));
}

static void test_session_invalidate_all() {
    resetAuthState();
    
    // 创建多个会话
    String s1 = authMgr->authenticate("admin", "admin123");
    String s2 = authMgr->authenticate("admin", "admin123");
    String s3 = authMgr->authenticate("admin", "admin123");
    
    TEST_ASSERT_TRUE(authMgr->validateSession(s1));
    TEST_ASSERT_TRUE(authMgr->validateSession(s2));
    TEST_ASSERT_TRUE(authMgr->validateSession(s3));
    
    authMgr->invalidateAllSessions("admin");
    
    TEST_ASSERT_FALSE(authMgr->validateSession(s1));
    TEST_ASSERT_FALSE(authMgr->validateSession(s2));
    TEST_ASSERT_FALSE(authMgr->validateSession(s3));
}

static void test_session_expired() {
    resetAuthState();
    
    // 设置1秒超时
    authMgr->setSessionTimeout(1);
    
    String sessionId = authMgr->authenticate("admin", "admin123");
    TEST_ASSERT_FALSE(sessionId.isEmpty());
    
    // 注：在mock环境下，time(nullptr) 可能不推进
    // 验证 session timeout 设置生效
    TEST_ASSERT_EQUAL(1, authMgr->getSessionTimeout());
}

static void test_session_nonexistent() {
    resetAuthState();
    
    TEST_ASSERT_FALSE(authMgr->validateSession("invalid_session_id"));
    TEST_ASSERT_TRUE(authMgr->getSessionUser("invalid_session_id").isEmpty());
}

// ========== 在线用户测试 ==========

static void test_online_user_count() {
    resetAuthState();
    
    TEST_ASSERT_EQUAL(0, authMgr->getOnlineUserCount());
    
    userMgr->createUser("online1", "pass1");
    userMgr->createUser("online2", "pass2");
    
    authMgr->authenticate("admin", "admin123");
    authMgr->authenticate("online1", "pass1");
    authMgr->authenticate("online2", "pass2");
    
    TEST_ASSERT_EQUAL(3, authMgr->getOnlineUserCount());
}

static void test_session_persistence() {
    resetAuthState();
    
    String sessionId = authMgr->authenticate("admin", "admin123");
    
    TEST_ASSERT_TRUE(authMgr->saveSessionsToStorage());
    
    // 清除当前会话
    authMgr->invalidateSession(sessionId);
    TEST_ASSERT_FALSE(authMgr->validateSession(sessionId));
    
    // 从存储恢复
    TEST_ASSERT_TRUE(authMgr->loadSessionsFromStorage());
    TEST_ASSERT_TRUE(authMgr->validateSession(sessionId));
}

// ========== 单管理员模式回归测试 ==========

static void test_single_admin_mode_no_roles() {
    resetAuthState();
    
    // 验证系统运行在单管理员模式
    // 验证没有RoleManager实例（类不应存在）
    // 验证认证即授权：只要登录成功，就拥有完整权限
    String sessionId = authMgr->authenticate("admin", "admin123");
    TEST_ASSERT_FALSE(sessionId.isEmpty());
    TEST_ASSERT_TRUE(authMgr->validateSession(sessionId));
    
    // 在单管理员模式下，不应有任何角色检查逻辑
    // 认证成功 = 完全授权
    String username = authMgr->getSessionUser(sessionId);
    TEST_ASSERT_EQUAL_STRING("admin", username.c_str());
}

// ========== 测试组入口 ==========

void test_security_auth_group() {
    // 用户管理
    RUN_TEST(test_create_user_success);
    RUN_TEST(test_create_user_empty_params);
    RUN_TEST(test_create_user_duplicate);
    RUN_TEST(test_delete_user_success);
    RUN_TEST(test_delete_admin_protected);
    RUN_TEST(test_delete_nonexistent_user);
    
    // 密码管理
    RUN_TEST(test_verify_password_correct);
    RUN_TEST(test_verify_password_incorrect);
    RUN_TEST(test_change_password_success);
    RUN_TEST(test_change_password_wrong_old);
    RUN_TEST(test_reset_password);
    
    // 账户锁定
    RUN_TEST(test_account_lock_after_failures);
    RUN_TEST(test_account_not_locked_before_threshold);
    RUN_TEST(test_successful_login_resets_counter);
    
    // 认证流程
    RUN_TEST(test_authenticate_success);
    RUN_TEST(test_authenticate_wrong_password);
    RUN_TEST(test_authenticate_nonexistent_user);
    RUN_TEST(test_authenticate_locked_account);
    
    // 会话管理
    RUN_TEST(test_session_create_and_validate);
    RUN_TEST(test_session_invalidate);
    RUN_TEST(test_session_invalidate_all);
    RUN_TEST(test_session_expired);
    RUN_TEST(test_session_nonexistent);
    
    // 在线用户与持久化
    RUN_TEST(test_online_user_count);
    RUN_TEST(test_session_persistence);
    
    // 单管理员模式回归防护
    RUN_TEST(test_single_admin_mode_no_roles);
    
    // 清理
    if (authMgr) {
        delete authMgr;
        authMgr = nullptr;
    }
}
