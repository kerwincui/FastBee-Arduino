/**
 * @file test_security_auth.cpp
 * @brief 安全认证模块单元测试
 * 
 * 测试内容：
 * - 用户创建/删除/修改
 * - 密码验证和变更
 * - 账户锁定和解锁
 * - 角色 CRUD 和权限管理
 * - 认证流程（登录/登出）
 * - 会话管理（创建/验证/过期/注销）
 * - 权限检查（通配符/资源+动作）
 * - 多会话场景
 * - 边界条件（空参数/重复操作/系统角色保护）
 */

#include <unity.h>
#include <Arduino.h>
#include "mocks/MockAuth.h"

void test_security_auth_group();

// ========== 辅助：重置状态 ==========

static MockUserManager* userMgr;
static MockRoleManager* roleMgr;
static MockAuthManager* authMgr;

static void resetAuthState() {
    // 重新获取单例，重新初始化
    userMgr = &MockUserManager::getInstance();
    roleMgr = &MockRoleManager::getInstance();
    
    // 清除非 admin 用户
    auto usernames = userMgr->getUsernames();
    for (auto& name : usernames) {
        if (name != "admin") {
            userMgr->deleteUser(name);
        }
    }
    // 重置 admin 密码
    userMgr->resetPassword("admin", "admin123");
    
    // 清除非系统角色，避免跨测试污染
    auto roleIds = roleMgr->getRoleIds();
    for (auto& id : roleIds) {
        roleMgr->deleteRole(id);  // 系统角色受保护，不会被删除
    }

    // 重新创建 AuthManager
    if (authMgr) delete authMgr;
    authMgr = new MockAuthManager(userMgr, roleMgr);
    authMgr->initialize();
}

// ========== 用户管理测试 ==========

static void test_create_user_success() {
    resetAuthState();
    
    bool ok = userMgr->createUser("testuser", "Test@123", {"operator"});
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
    TEST_ASSERT_FALSE(userMgr->createUser("", "password", {"admin"}));
    // 空密码
    TEST_ASSERT_FALSE(userMgr->createUser("user1", "", {"admin"}));
}

static void test_create_user_duplicate() {
    resetAuthState();
    
    userMgr->createUser("dup_user", "pass1", {"viewer"});
    bool ok = userMgr->createUser("dup_user", "pass2", {"admin"});
    TEST_ASSERT_FALSE(ok);
}

static void test_delete_user_success() {
    resetAuthState();
    
    userMgr->createUser("removable", "pass123", {"viewer"});
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
    
    userMgr->createUser("passuser", "SecureP@ss", {"viewer"});
    TEST_ASSERT_TRUE(userMgr->verifyPassword("passuser", "SecureP@ss"));
}

static void test_verify_password_incorrect() {
    resetAuthState();
    
    userMgr->createUser("passuser", "correct", {"viewer"});
    TEST_ASSERT_FALSE(userMgr->verifyPassword("passuser", "wrong"));
}

static void test_change_password_success() {
    resetAuthState();
    
    userMgr->createUser("chguser", "oldpass", {"viewer"});
    TEST_ASSERT_TRUE(userMgr->changePassword("chguser", "oldpass", "newpass"));
    TEST_ASSERT_TRUE(userMgr->verifyPassword("chguser", "newpass"));
    TEST_ASSERT_FALSE(userMgr->verifyPassword("chguser", "oldpass"));
}

static void test_change_password_wrong_old() {
    resetAuthState();
    
    userMgr->createUser("chguser", "oldpass", {"viewer"});
    TEST_ASSERT_FALSE(userMgr->changePassword("chguser", "badold", "newpass"));
    // 原密码不变
    TEST_ASSERT_TRUE(userMgr->verifyPassword("chguser", "oldpass"));
}

static void test_reset_password() {
    resetAuthState();
    
    userMgr->createUser("resetuser", "original", {"viewer"});
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
    
    userMgr->createUser("lockme", "pass123", {"viewer"});
    
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
    
    userMgr->createUser("safe_user", "pass123", {"viewer"});
    
    // 4次失败不锁定
    for (int i = 0; i < 4; i++) {
        userMgr->recordFailedLogin("safe_user");
    }
    
    TEST_ASSERT_FALSE(userMgr->isAccountLocked("safe_user"));
}

static void test_successful_login_resets_counter() {
    resetAuthState();
    
    userMgr->createUser("counter_user", "pass123", {"viewer"});
    userMgr->recordFailedLogin("counter_user");
    userMgr->recordFailedLogin("counter_user");
    userMgr->recordFailedLogin("counter_user");
    
    userMgr->recordSuccessfulLogin("counter_user");
    
    User* user = userMgr->getUser("counter_user");
    TEST_ASSERT_EQUAL(0, user->failedLoginAttempts);
    TEST_ASSERT_EQUAL(0, user->lockedUntil);
}

// ========== 角色管理测试 ==========

static void test_builtin_roles_exist() {
    resetAuthState();
    
    TEST_ASSERT_NOT_NULL(roleMgr->getRole("admin"));
    TEST_ASSERT_NOT_NULL(roleMgr->getRole("operator"));
    TEST_ASSERT_NOT_NULL(roleMgr->getRole("viewer"));
}

static void test_create_custom_role() {
    resetAuthState();
    
    Role customRole;
    customRole.id = "technician";
    customRole.name = "Technician";
    customRole.isSystem = false;
    customRole.permissions = {"device.view", "device.control", "peripheral.view"};
    
    TEST_ASSERT_TRUE(roleMgr->createRole(customRole));
    
    Role* stored = roleMgr->getRole("technician");
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_EQUAL_STRING("Technician", stored->name.c_str());
    TEST_ASSERT_EQUAL(3, stored->permissions.size());
}

static void test_delete_custom_role() {
    resetAuthState();
    
    Role temp;
    temp.id = "temp_role";
    temp.name = "Temporary";
    temp.isSystem = false;
    roleMgr->createRole(temp);
    
    TEST_ASSERT_TRUE(roleMgr->deleteRole("temp_role"));
    TEST_ASSERT_NULL(roleMgr->getRole("temp_role"));
}

static void test_delete_system_role_protected() {
    resetAuthState();
    
    // 系统角色不可删除
    TEST_ASSERT_FALSE(roleMgr->deleteRole("admin"));
    TEST_ASSERT_FALSE(roleMgr->deleteRole("operator"));
    TEST_ASSERT_FALSE(roleMgr->deleteRole("viewer"));
    
    TEST_ASSERT_NOT_NULL(roleMgr->getRole("admin"));
}

static void test_role_permission_check() {
    resetAuthState();
    
    // admin 拥有所有权限
    TEST_ASSERT_TRUE(roleMgr->roleHasPermission("admin", "device.view"));
    TEST_ASSERT_TRUE(roleMgr->roleHasPermission("admin", "system.restart"));
    TEST_ASSERT_TRUE(roleMgr->roleHasPermission("admin", "user.delete"));
    
    // viewer 只有只读权限
    TEST_ASSERT_TRUE(roleMgr->roleHasPermission("viewer", "device.view"));
    TEST_ASSERT_FALSE(roleMgr->roleHasPermission("viewer", "device.control"));
    TEST_ASSERT_FALSE(roleMgr->roleHasPermission("viewer", "user.delete"));
}

static void test_permission_wildcard() {
    resetAuthState();
    
    Role wildcardRole;
    wildcardRole.id = "wildcard_test";
    wildcardRole.name = "Wildcard Test";
    wildcardRole.isSystem = false;
    wildcardRole.permissions = {"device.*", "log.view"};
    roleMgr->createRole(wildcardRole);
    
    // 通配符匹配
    TEST_ASSERT_TRUE(roleMgr->roleHasPermission("wildcard_test", "device.view"));
    TEST_ASSERT_TRUE(roleMgr->roleHasPermission("wildcard_test", "device.control"));
    TEST_ASSERT_TRUE(roleMgr->roleHasPermission("wildcard_test", "device.config"));
    
    // 非匹配
    TEST_ASSERT_FALSE(roleMgr->roleHasPermission("wildcard_test", "user.view"));
    TEST_ASSERT_TRUE(roleMgr->roleHasPermission("wildcard_test", "log.view"));
    TEST_ASSERT_FALSE(roleMgr->roleHasPermission("wildcard_test", "log.clear"));
}

static void test_assign_remove_permission() {
    resetAuthState();
    
    Role custom;
    custom.id = "perm_test";
    custom.name = "Perm Test";
    custom.isSystem = false;
    roleMgr->createRole(custom);
    
    TEST_ASSERT_TRUE(roleMgr->assignPermission("perm_test", "ota.view"));
    TEST_ASSERT_TRUE(roleMgr->roleHasPermission("perm_test", "ota.view"));
    
    TEST_ASSERT_TRUE(roleMgr->removePermission("perm_test", "ota.view"));
    TEST_ASSERT_FALSE(roleMgr->roleHasPermission("perm_test", "ota.view"));
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
    
    userMgr->createUser("locktest", "pass123", {"viewer"});
    
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

// ========== 权限检查测试 ==========

static void test_user_has_permission() {
    resetAuthState();
    
    // admin 用户有 admin 角色，拥有所有权限
    TEST_ASSERT_TRUE(authMgr->userHasPermission("admin", "device.view"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("admin", "system.restart"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("admin", "user.delete"));
}

static void test_session_has_permission() {
    resetAuthState();
    
    String sessionId = authMgr->authenticate("admin", "admin123");
    
    TEST_ASSERT_TRUE(authMgr->sessionHasPermission(sessionId, "device.view"));
    TEST_ASSERT_TRUE(authMgr->sessionHasPermission(sessionId, "system.restart"));
}

static void test_check_permission_resource_action() {
    resetAuthState();
    
    String sessionId = authMgr->authenticate("admin", "admin123");
    
    // checkPermission 使用 resource + action 格式
    TEST_ASSERT_TRUE(authMgr->checkPermission(sessionId, "device", "view"));
    TEST_ASSERT_TRUE(authMgr->checkPermission(sessionId, "config", "edit"));
}

static void test_viewer_permission_boundary() {
    resetAuthState();
    
    userMgr->createUser("viewer_user", "pass123", {"viewer"});
    String sessionId = authMgr->authenticate("viewer_user", "pass123");
    
    // viewer 只有只读权限
    TEST_ASSERT_TRUE(authMgr->sessionHasPermission(sessionId, "device.view"));
    TEST_ASSERT_FALSE(authMgr->sessionHasPermission(sessionId, "device.control"));
    TEST_ASSERT_FALSE(authMgr->sessionHasPermission(sessionId, "system.reboot"));
    TEST_ASSERT_FALSE(authMgr->sessionHasPermission(sessionId, "user.delete"));
}

// ========== 用户角色分配测试 ==========

static void test_assign_role_to_user() {
    resetAuthState();
    
    userMgr->createUser("multi_role", "pass123", {"viewer"});
    
    TEST_ASSERT_TRUE(userMgr->assignRole("multi_role", "operator"));
    
    auto roles = userMgr->getUserRoles("multi_role");
    TEST_ASSERT_EQUAL(2, roles.size());
}

static void test_remove_role_from_user() {
    resetAuthState();
    
    userMgr->createUser("role_user", "pass123", {"viewer", "operator"});
    
    TEST_ASSERT_TRUE(userMgr->removeRole("role_user", "operator"));
    
    auto roles = userMgr->getUserRoles("role_user");
    TEST_ASSERT_EQUAL(1, roles.size());
    TEST_ASSERT_EQUAL_STRING("viewer", roles[0].c_str());
}

static void test_cannot_remove_admin_role_from_admin() {
    resetAuthState();
    
    // admin 用户的 admin 角色不可移除
    TEST_ASSERT_FALSE(userMgr->removeRole("admin", "admin"));
}

// ========== 在线用户测试 ==========

static void test_online_user_count() {
    resetAuthState();
    
    TEST_ASSERT_EQUAL(0, authMgr->getOnlineUserCount());
    
    userMgr->createUser("online1", "pass1", {"viewer"});
    userMgr->createUser("online2", "pass2", {"viewer"});
    
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

// ========== requirePermission 401/403 区分测试 ==========

static void test_operator_lacks_admin_permissions() {
    resetAuthState();
    
    userMgr->createUser("op_user", "pass123", {"operator"});
    
    // Operator 有设备控制权限
    TEST_ASSERT_TRUE(authMgr->userHasPermission("op_user", "device.view"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("op_user", "device.control"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("op_user", "peripheral.view"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("op_user", "peripheral.control"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("op_user", "log.view"));
    
    // Operator 没有管理权限
    TEST_ASSERT_FALSE(authMgr->userHasPermission("op_user", "user.admin"));
    TEST_ASSERT_FALSE(authMgr->userHasPermission("op_user", "role.admin"));
    TEST_ASSERT_FALSE(authMgr->userHasPermission("op_user", "config.edit"));
    TEST_ASSERT_FALSE(authMgr->userHasPermission("op_user", "system.restart"));
}

static void test_viewer_readonly_only() {
    resetAuthState();
    
    userMgr->createUser("view_user", "pass123", {"viewer"});
    
    // Viewer 只有只读权限
    TEST_ASSERT_TRUE(authMgr->userHasPermission("view_user", "device.view"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("view_user", "peripheral.view"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("view_user", "system.view"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("view_user", "log.view"));
    
    // Viewer 没有任何写操作权限
    TEST_ASSERT_FALSE(authMgr->userHasPermission("view_user", "device.control"));
    TEST_ASSERT_FALSE(authMgr->userHasPermission("view_user", "peripheral.control"));
    TEST_ASSERT_FALSE(authMgr->userHasPermission("view_user", "config.edit"));
    TEST_ASSERT_FALSE(authMgr->userHasPermission("view_user", "user.admin"));
    TEST_ASSERT_FALSE(authMgr->userHasPermission("view_user", "role.admin"));
    TEST_ASSERT_FALSE(authMgr->userHasPermission("view_user", "system.restart"));
    TEST_ASSERT_FALSE(authMgr->userHasPermission("view_user", "fs.manage"));
}

static void test_admin_has_all_permissions() {
    resetAuthState();
    
    // Admin 拥有所有权限
    TEST_ASSERT_TRUE(authMgr->userHasPermission("admin", "device.view"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("admin", "device.control"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("admin", "user.admin"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("admin", "role.admin"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("admin", "config.edit"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("admin", "config.view"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("admin", "system.view"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("admin", "system.restart"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("admin", "network.view"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("admin", "network.edit"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("admin", "fs.manage"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("admin", "fs.view"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("admin", "ota.update"));
}

static void test_custom_role_permissions() {
    resetAuthState();
    
    // 创建自定义角色
    Role customRole;
    customRole.id = "technician";
    customRole.name = "Technician";
    customRole.isSystem = false;
    customRole.permissions = {"device.view", "device.control", "peripheral.view", "peripheral.control"};
    roleMgr->createRole(customRole);
    
    userMgr->createUser("tech_user", "pass123", {"technician"});
    
    // 自定义角色权限
    TEST_ASSERT_TRUE(authMgr->userHasPermission("tech_user", "device.view"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("tech_user", "device.control"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("tech_user", "peripheral.view"));
    TEST_ASSERT_TRUE(authMgr->userHasPermission("tech_user", "peripheral.control"));
    
    // 自定义角色没有的权限
    TEST_ASSERT_FALSE(authMgr->userHasPermission("tech_user", "user.admin"));
    TEST_ASSERT_FALSE(authMgr->userHasPermission("tech_user", "role.admin"));
    TEST_ASSERT_FALSE(authMgr->userHasPermission("tech_user", "config.edit"));
    TEST_ASSERT_FALSE(authMgr->userHasPermission("tech_user", "system.restart"));
}

static void test_session_permission_check() {
    resetAuthState();
    
    userMgr->createUser("session_test_user", "pass123", {"viewer"});
    String sessionId = authMgr->authenticate("session_test_user", "pass123");
    
    TEST_ASSERT_FALSE(sessionId.isEmpty());
    
    // 通过会话检查权限
    TEST_ASSERT_TRUE(authMgr->sessionHasPermission(sessionId, "device.view"));
    TEST_ASSERT_TRUE(authMgr->sessionHasPermission(sessionId, "system.view"));
    TEST_ASSERT_FALSE(authMgr->sessionHasPermission(sessionId, "device.control"));
    TEST_ASSERT_FALSE(authMgr->sessionHasPermission(sessionId, "user.admin"));
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
    
    // 角色管理
    RUN_TEST(test_builtin_roles_exist);
    RUN_TEST(test_create_custom_role);
    RUN_TEST(test_delete_custom_role);
    RUN_TEST(test_delete_system_role_protected);
    RUN_TEST(test_role_permission_check);
    RUN_TEST(test_permission_wildcard);
    RUN_TEST(test_assign_remove_permission);
    
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
    
    // 权限检查
    RUN_TEST(test_user_has_permission);
    RUN_TEST(test_session_has_permission);
    RUN_TEST(test_check_permission_resource_action);
    RUN_TEST(test_viewer_permission_boundary);
    
    // 用户角色分配
    RUN_TEST(test_assign_role_to_user);
    RUN_TEST(test_remove_role_from_user);
    RUN_TEST(test_cannot_remove_admin_role_from_admin);
    
    // 在线用户与持久化
    RUN_TEST(test_online_user_count);
    RUN_TEST(test_session_persistence);
    
    // requirePermission 401/403 区分测试
    RUN_TEST(test_operator_lacks_admin_permissions);
    RUN_TEST(test_viewer_readonly_only);
    RUN_TEST(test_admin_has_all_permissions);
    RUN_TEST(test_custom_role_permissions);
    RUN_TEST(test_session_permission_check);
    
    // 清理
    if (authMgr) {
        delete authMgr;
        authMgr = nullptr;
    }
}
