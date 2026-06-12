/**
 * @file test_web_api.cpp
 * @brief Web API Tests
 */

#include <unity.h>
#include <Arduino.h>
#include "mocks/MockAuth.h"
#include "mocks/MockWiFi.h"
#include "mocks/MockLittleFS.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

void test_web_api_group();

// Mock HTTP structures
struct MockHTTPRequest {
    String method;
    String path;
    std::map<String, String> headers;
    String body;
    String sessionId;
};

struct MockHTTPResponse {
    int statusCode;
    std::map<String, String> headers;
    String body;
    bool success;

    MockHTTPResponse() : statusCode(200), success(true) {}
};

// Mock Web API Handler
class MockWebAPIHandler {
public:
    MockWebAPIHandler() : _authMgr(nullptr), _userMgr(nullptr), _roleMgr(nullptr) {}

    void setAuthManager(MockAuthManager* authMgr) {
        _authMgr = authMgr;
    }

    void setUserManager(MockUserManager* userMgr) {
        _userMgr = userMgr;
    }

    void setRoleManager(MockRoleManager* roleMgr) {
        _roleMgr = roleMgr;
    }

    MockHTTPResponse handleLogin(const MockHTTPRequest& req) {
        MockHTTPResponse resp;

        if (req.body.indexOf("\"username\"") >= 0 &&
            req.body.indexOf("\"password\"") >= 0) {

            // 从 JSON body 中解析 username 和 password
            String username = extractJsonField(req.body, "username");
            String password = extractJsonField(req.body, "password");

            if (_authMgr) {
                String sessionId = _authMgr->authenticate(username, password);
                if (!sessionId.isEmpty()) {
                    resp.body = "{\"success\":true,\"sessionId\":\"" + sessionId + "\"}";
                    resp.headers["Set-Cookie"] = "session=" + sessionId;
                } else {
                    resp.statusCode = 401;
                    resp.body = "{\"success\":false,\"error\":\"Invalid credentials\"}";
                    resp.success = false;
                }
            }
        } else {
            resp.statusCode = 400;
            resp.body = "{\"success\":false,\"error\":\"Missing credentials\"}";
            resp.success = false;
        }

        return resp;
    }

    MockHTTPResponse handleNetworkStatus(MockWiFiClass* wifi) {
        MockHTTPResponse resp;

        if (!wifi) {
            resp.statusCode = 500;
            resp.body = "{\"success\":false,\"error\":\"WiFi not available\"}";
            resp.success = false;
            return resp;
        }

        MockWiFiClass::NetworkStatusInfo status = wifi->getStatusInfo();

        resp.body = "{";
        resp.body += "\"success\":true,";
        resp.body += "\"data\":{";
        resp.body += "\"status\":\"" + status.status + "\",";
        resp.body += "\"wifiConnected\":" + String(status.wifiConnected ? "true" : "false") + ",";
        resp.body += "\"internetAvailable\":" + String(status.internetAvailable ? "true" : "false") + ",";
        resp.body += "\"ssid\":\"" + status.ssid + "\",";
        resp.body += "\"ipAddress\":\"" + status.ipAddress + "\",";
        resp.body += "\"apIPAddress\":\"" + status.apIPAddress + "\"";
        resp.body += "}}";

        return resp;
    }

    MockHTTPResponse handleSystemInfo() {
        MockHTTPResponse resp;

        resp.body = "{";
        resp.body += "\"success\":true,";
        resp.body += "\"data\":{";
        resp.body += "\"device\":{\"chipModel\":\"ESP32\"},";
        resp.body += "\"memory\":{\"heapFree\":" + String(ESP.getFreeHeap()) + "}";
        resp.body += "}}";

        return resp;
    }

    bool validateSession(const MockHTTPRequest& req) {
        if (!_authMgr) return false;

        String sessionId = req.sessionId;
        if (sessionId.isEmpty()) {
            auto it = req.headers.find("Cookie");
            if (it != req.headers.end()) {
                int start = it->second.indexOf("session=");
                if (start >= 0) {
                    start += 8;
                    int end = it->second.indexOf(";", start);
                    if (end < 0) end = it->second.length();
                    sessionId = it->second.substring(start, end);
                }
            }
        }

        return _authMgr->validateSession(sessionId);
    }

    /**
     * 模拟 requirePermission 行为：认证失败返回401，权限拒绝返回403
     */
    int requirePermission(const MockHTTPRequest& req, const String& permission) {
        if (!_authMgr) return 500;

        // 提取 sessionId
        String sessionId = req.sessionId;
        if (sessionId.isEmpty()) {
            auto it = req.headers.find("Cookie");
            if (it != req.headers.end()) {
                int start = it->second.indexOf("session=");
                if (start >= 0) {
                    start += 8;
                    int end = it->second.indexOf(";", start);
                    if (end < 0) end = it->second.length();
                    sessionId = it->second.substring(start, end);
                }
            }
        }

        // 认证失败：返回 401
        if (!_authMgr->validateSession(sessionId)) {
            return 401;
        }

        // 获取用户名并检查权限
        String username = _authMgr->getSessionUser(sessionId);
        if (username.isEmpty()) {
            return 401;
        }

        // 权限拒绝：返回 403
        if (!_authMgr->userHasPermission(username, permission)) {
            return 403;
        }

        // 通过
        return 200;
    }

private:
    MockAuthManager* _authMgr;
    MockUserManager* _userMgr;
    MockRoleManager* _roleMgr;

    // 从简单 JSON 字符串中提取字段值
    static String extractJsonField(const String& json, const String& field) {
        String key = "\"" + field + "\"";
        int keyIdx = json.indexOf(key);
        if (keyIdx < 0) return "";
        int colonIdx = json.indexOf(':', keyIdx + key.length());
        if (colonIdx < 0) return "";
        int quoteStart = json.indexOf('"', colonIdx + 1);
        if (quoteStart < 0) return "";
        int quoteEnd = json.indexOf('"', quoteStart + 1);
        if (quoteEnd < 0) return "";
        return json.substring(quoteStart + 1, quoteEnd);
    }
};

// Test API login
void test_api_login() {
    TestLog::testStart("API Login");

    MockUserManager& userMgr = MockUserMgr;
    userMgr.initialize();

    MockAuthManager authMgr(&userMgr);
    authMgr.initialize();

    MockWebAPIHandler apiHandler;
    apiHandler.setAuthManager(&authMgr);

    MockHTTPRequest loginReq;
    loginReq.method = "POST";
    loginReq.path = "/api/auth/login";
    loginReq.body = "{\"username\":\"admin\",\"password\":\"admin123\"}";

    MockHTTPResponse loginResp = apiHandler.handleLogin(loginReq);

    TEST_ASSERT_EQUAL(200, loginResp.statusCode);
    TEST_ASSERT_TRUE(loginResp.body.indexOf("\"success\":true") >= 0);
    TEST_ASSERT_TRUE(loginResp.body.indexOf("sessionId") >= 0);
    TestLog::step("Login successful, session created");

    MockHTTPRequest badReq;
    badReq.method = "POST";
    badReq.path = "/api/auth/login";
    badReq.body = "{\"username\":\"admin\",\"password\":\"wrongpass\"}";

    MockHTTPResponse badResp = apiHandler.handleLogin(badReq);

    TEST_ASSERT_EQUAL(401, badResp.statusCode);
    TEST_ASSERT_TRUE(badResp.body.indexOf("\"success\":false") >= 0);
    TestLog::step("Invalid credentials rejected");

    TestLog::testEnd(true);
}

// Test session validation
void test_session_validation() {
    TestLog::testStart("Session Validation");

    MockUserManager& userMgr = MockUserMgr;
    userMgr.initialize();

    MockAuthManager authMgr(&userMgr);
    authMgr.initialize();

    MockWebAPIHandler apiHandler;
    apiHandler.setAuthManager(&authMgr);

    String sessionId = authMgr.authenticate("admin", "admin123");
    TEST_ASSERT_FALSE(sessionId.isEmpty());
    TestLog::step("Session created");

    MockHTTPRequest validReq;
    validReq.sessionId = sessionId;
    TEST_ASSERT_TRUE(apiHandler.validateSession(validReq));
    TestLog::step("Valid session accepted");

    MockHTTPRequest invalidReq;
    invalidReq.sessionId = "invalid_session_id";
    TEST_ASSERT_FALSE(apiHandler.validateSession(invalidReq));
    TestLog::step("Invalid session rejected");

    TestLog::testEnd(true);
}

// Test API network status
void test_api_network_status() {
    TestLog::testStart("API Network Status");

    MockWiFiClass wifi;
    MockWebAPIHandler apiHandler;

    wifi.mode(WIFI_AP);
    wifi.softAP("FastBee-Test", "12345678");
    wifi.setConnected(false);

    MockHTTPResponse resp = apiHandler.handleNetworkStatus(&wifi);

    TEST_ASSERT_EQUAL(200, resp.statusCode);
    TEST_ASSERT_TRUE(resp.body.indexOf("\"success\":true") >= 0);
    TEST_ASSERT_TRUE(resp.body.indexOf("\"wifiConnected\":false") >= 0);
    TEST_ASSERT_TRUE(resp.body.indexOf("\"internetAvailable\":false") >= 0);
    TestLog::step("AP mode status: wifiConnected=false, internetAvailable=false");

    TestLog::testEnd(true);
}

// Test API system info
void test_api_system_info() {
    TestLog::testStart("API System Info");

    MockWebAPIHandler apiHandler;
    MockHTTPResponse resp = apiHandler.handleSystemInfo();

    TEST_ASSERT_EQUAL(200, resp.statusCode);
    TEST_ASSERT_TRUE(resp.body.indexOf("\"success\":true") >= 0);
    TestLog::step("System info API returned successfully");

    TEST_ASSERT_TRUE(resp.body.indexOf("\"device\"") >= 0);
    TestLog::step("Device info present");

    TEST_ASSERT_TRUE(resp.body.indexOf("\"memory\"") >= 0);
    TestLog::step("Memory info present");

    TestLog::testEnd(true);
}

// Test unauthorized access
void test_unauthorized_access() {
    TestLog::testStart("Unauthorized Access Control");

    MockUserManager& userMgr = MockUserMgr;
    userMgr.initialize();

    MockAuthManager authMgr(&userMgr);
    authMgr.initialize();

    MockWebAPIHandler apiHandler;
    apiHandler.setAuthManager(&authMgr);

    MockHTTPRequest req;
    req.sessionId = "";

    bool authorized = apiHandler.validateSession(req);
    TEST_ASSERT_FALSE(authorized);
    TestLog::step("Access without session rejected");

    req.sessionId = "invalid_session";
    authorized = apiHandler.validateSession(req);
    TEST_ASSERT_FALSE(authorized);
    TestLog::step("Access with invalid session rejected");

    TestLog::testEnd(true);
}

// Test permission control
void test_permission_control() {
    TestLog::testStart("Permission Control");

    MockRoleManager& roleMgr = MockRoleMgr;
    roleMgr.initialize();

    MockUserManager& userMgr = MockUserMgr;
    userMgr.initialize();

    userMgr.createUser("operator", "pass123", {"operator"});

    MockAuthManager authMgr(&userMgr, &roleMgr);
    authMgr.initialize();

    String sessionId = authMgr.authenticate("operator", "pass123");
    TEST_ASSERT_FALSE(sessionId.isEmpty());

    TEST_ASSERT_TRUE(authMgr.sessionHasPermission(sessionId, "device.view"));
    TestLog::step("Operator has device.view permission");

    TEST_ASSERT_TRUE(authMgr.sessionHasPermission(sessionId, "device.control"));
    TestLog::step("Operator has device.control permission");

    TEST_ASSERT_FALSE(authMgr.sessionHasPermission(sessionId, "user.create"));
    TestLog::step("Operator does NOT have user.create permission");

    TestLog::testEnd(true);
}

// Test API error handling
void test_api_error_handling() {
    TestLog::testStart("API Error Handling");

    MockWebAPIHandler apiHandler;

    MockHTTPResponse resp = apiHandler.handleNetworkStatus(nullptr);

    TEST_ASSERT_EQUAL(500, resp.statusCode);
    TEST_ASSERT_TRUE(resp.body.indexOf("\"success\":false") >= 0);
    TestLog::step("Error response for missing WiFi");

    TestLog::testEnd(true);
}

// Test session expiration
void test_session_expiration() {
    TestLog::testStart("Session Expiration");

    MockUserManager& userMgr = MockUserMgr;
    userMgr.initialize();

    MockAuthManager authMgr(&userMgr);
    authMgr.initialize();

    authMgr.setSessionTimeout(1);

    String sessionId = authMgr.authenticate("admin", "admin123");
    TEST_ASSERT_FALSE(sessionId.isEmpty());
    TEST_ASSERT_TRUE(authMgr.validateSession(sessionId));
    TestLog::step("Session created and valid");

    delay(1500);

    TEST_ASSERT_FALSE(authMgr.validateSession(sessionId));
    TestLog::step("Session expired after timeout");

    TestLog::testEnd(true);
}

// Test concurrent sessions
void test_concurrent_sessions() {
    TestLog::testStart("Concurrent Sessions");

    MockUserManager& userMgr = MockUserMgr;
    userMgr.initialize();

    userMgr.createUser("user1", "pass1", {"operator"});
    userMgr.createUser("user2", "pass2", {"viewer"});
    userMgr.createUser("user3", "pass3", {"operator"});

    MockAuthManager authMgr(&userMgr);
    authMgr.initialize();

    String session1 = authMgr.authenticate("user1", "pass1");
    String session2 = authMgr.authenticate("user2", "pass2");
    String session3 = authMgr.authenticate("user3", "pass3");

    TEST_ASSERT_FALSE(session1.isEmpty());
    TEST_ASSERT_FALSE(session2.isEmpty());
    TEST_ASSERT_FALSE(session3.isEmpty());
    TestLog::step("Three sessions created");

    TEST_ASSERT_TRUE(authMgr.validateSession(session1));
    TEST_ASSERT_TRUE(authMgr.validateSession(session2));
    TEST_ASSERT_TRUE(authMgr.validateSession(session3));
    TestLog::step("All sessions valid");

    auto onlineUsers = authMgr.getOnlineUsers();
    TEST_ASSERT_EQUAL(3, onlineUsers.size());
    TestLog::step("Online user count: 3");

    TestLog::testEnd(true);
}

// Test factory reset - writes default configs instead of deleting
void test_factory_reset_writes_defaults() {
    TestLog::testStart("Factory Reset Writes Defaults");

    // Setup: pre-populate config files with user-modified content
    g_mockFiles["/config/device.json"] = "{\"deviceId\":\"CUSTOM_ID\",\"deviceName\":\"MyDevice\"}";
    g_mockFiles["/config/network.json"] = "{\"staSSID\":\"home-wifi\",\"staPassword\":\"secret123\"}";
    g_mockFiles["/config/users.json"] = "{\"users\":[{\"username\":\"custom_admin\"}]}";
    g_mockFiles["/config/roles.json"] = "{\"roles\":[]}";
    g_mockFiles["/config/protocol.json"] = "{\"mqtt\":{\"enabled\":true,\"server\":\"my.mqtt.com\"}}";
    g_mockFiles["/config/peripherals.json"] = "{\"peripherals\":[{\"id\":\"led1\"}]}";
    g_mockFiles["/config/periph_exec.json"] = "{\"version\":3,\"rules\":[{\"id\":\"rule1\"}]}";
    g_mockFiles["/config/auth.json"] = "{\"sessions\":[]}";
    g_mockFiles["/config/mqtt.json"] = "{\"enabled\":true}";

    // Simulate factory reset: write defaults to core files
    auto writeDefault = [](const char* path, const char* content) -> bool {
        MockFile f(String(path), "w");
        if (!f) return false;
        f.print(content);
        f.close();
        return true;
    };

    int resetCount = 0;

    // Write default device.json
    const char* DEFAULT_DEVICE = "{\"deviceId\":\"FBE100900001\",\"productNumber\":1070,\"deviceName\":\"FastBee-Device\"}";
    if (writeDefault("/config/device.json", DEFAULT_DEVICE)) resetCount++;

    // Write default network.json (WiFi cleared)
    const char* DEFAULT_NETWORK = "{\"mode\":0,\"apSSID\":\"fastbee-ap\",\"apPassword\":\"12345678\",\"staSSID\":\"\",\"staPassword\":\"\",\"networks\":[]}";
    if (writeDefault("/config/network.json", DEFAULT_NETWORK)) resetCount++;

    // Write default users.json
    const char* DEFAULT_USERS = "{\"version\":\"2.0\",\"users\":[{\"username\":\"admin\"},{\"username\":\"viewer\"}]}";
    if (writeDefault("/config/users.json", DEFAULT_USERS)) resetCount++;

    // Write default roles.json
    const char* DEFAULT_ROLES = "{\"roles\":[{\"id\":\"admin\"},{\"id\":\"operator\"},{\"id\":\"viewer\"}]}";
    if (writeDefault("/config/roles.json", DEFAULT_ROLES)) resetCount++;

    // Write default protocol.json
    const char* DEFAULT_PROTOCOL = "{\"version\":2,\"mqtt\":{\"enabled\":false},\"modbusRtu\":{\"enabled\":false}}";
    if (writeDefault("/config/protocol.json", DEFAULT_PROTOCOL)) resetCount++;

    // Write default peripherals.json
    const char* DEFAULT_PERIPHERALS = "{\"peripherals\":[]}";
    if (writeDefault("/config/peripherals.json", DEFAULT_PERIPHERALS)) resetCount++;

    // Write default periph_exec.json
    const char* DEFAULT_PERIPH_EXEC = "{\"version\":3,\"rules\":[]}";
    if (writeDefault("/config/periph_exec.json", DEFAULT_PERIPH_EXEC)) resetCount++;

    // Delete optional files
    LittleFS.remove("/config/auth.json");
    LittleFS.remove("/config/mqtt.json");

    // Verify: all 7 core files written successfully
    TEST_ASSERT_EQUAL(7, resetCount);
    TestLog::step("All 7 core config files reset successfully");

    // Verify: core config files exist with default content
    TEST_ASSERT_TRUE(LittleFS.exists("/config/device.json"));
    TEST_ASSERT_TRUE(LittleFS.exists("/config/network.json"));
    TEST_ASSERT_TRUE(LittleFS.exists("/config/users.json"));
    TEST_ASSERT_TRUE(LittleFS.exists("/config/roles.json"));
    TEST_ASSERT_TRUE(LittleFS.exists("/config/protocol.json"));
    TEST_ASSERT_TRUE(LittleFS.exists("/config/peripherals.json"));
    TEST_ASSERT_TRUE(LittleFS.exists("/config/periph_exec.json"));
    TestLog::step("All core config files exist after reset");

    // Verify: optional files deleted
    TEST_ASSERT_FALSE(LittleFS.exists("/config/auth.json"));
    TEST_ASSERT_FALSE(LittleFS.exists("/config/mqtt.json"));
    TestLog::step("Optional config files removed");

    // Verify: device.json has default deviceId (not custom)
    String deviceContent = g_mockFiles["/config/device.json"];
    TEST_ASSERT_TRUE(deviceContent.indexOf("FBE100900001") >= 0);
    TEST_ASSERT_FALSE(deviceContent.indexOf("CUSTOM_ID") >= 0);
    TestLog::step("device.json restored to default deviceId");

    // Verify: network.json has empty WiFi credentials
    String networkContent = g_mockFiles["/config/network.json"];
    TEST_ASSERT_TRUE(networkContent.indexOf("\"staSSID\":\"\"") >= 0);
    TEST_ASSERT_TRUE(networkContent.indexOf("\"staPassword\":\"\"") >= 0);
    TEST_ASSERT_TRUE(networkContent.indexOf("fastbee-ap") >= 0);
    TEST_ASSERT_FALSE(networkContent.indexOf("home-wifi") >= 0);
    TestLog::step("network.json WiFi cleared, AP mode restored");

    // Verify: protocol.json has MQTT disabled
    String protocolContent = g_mockFiles["/config/protocol.json"];
    TEST_ASSERT_TRUE(protocolContent.indexOf("\"enabled\":false") >= 0);
    TEST_ASSERT_FALSE(protocolContent.indexOf("my.mqtt.com") >= 0);
    TestLog::step("protocol.json all protocols disabled");

    // Verify: peripherals.json is empty array
    String periphContent = g_mockFiles["/config/peripherals.json"];
    TEST_ASSERT_EQUAL_STRING("{\"peripherals\":[]}", periphContent.c_str());
    TestLog::step("peripherals.json cleared to empty");

    // Verify: periph_exec.json is empty rules
    String execContent = g_mockFiles["/config/periph_exec.json"];
    TEST_ASSERT_EQUAL_STRING("{\"version\":3,\"rules\":[]}", execContent.c_str());
    TestLog::step("periph_exec.json cleared to empty rules");

    // Cleanup
    g_mockFiles.clear();

    TestLog::testEnd(true);
}

// Test factory reset preserves file system integrity (no file-not-found errors)
void test_factory_reset_no_file_missing() {
    TestLog::testStart("Factory Reset No File Missing");

    // Start with completely empty filesystem
    g_mockFiles.clear();

    // Simulate factory reset on fresh/empty device
    auto writeDefault = [](const char* path, const char* content) -> bool {
        MockFile f(String(path), "w");
        if (!f) return false;
        f.print(content);
        f.close();
        return true;
    };

    // Write all defaults
    writeDefault("/config/device.json", "{\"deviceId\":\"FBE100900001\"}");
    writeDefault("/config/network.json", "{\"mode\":0}");
    writeDefault("/config/users.json", "{\"users\":[]}");
    writeDefault("/config/roles.json", "{\"roles\":[]}");
    writeDefault("/config/protocol.json", "{\"version\":2}");
    writeDefault("/config/peripherals.json", "{\"peripherals\":[]}");
    writeDefault("/config/periph_exec.json", "{\"version\":3,\"rules\":[]}");

    // All config files should exist - web system can read them without errors
    TEST_ASSERT_TRUE(LittleFS.exists("/config/device.json"));
    TEST_ASSERT_TRUE(LittleFS.exists("/config/network.json"));
    TEST_ASSERT_TRUE(LittleFS.exists("/config/users.json"));
    TEST_ASSERT_TRUE(LittleFS.exists("/config/roles.json"));
    TEST_ASSERT_TRUE(LittleFS.exists("/config/protocol.json"));
    TEST_ASSERT_TRUE(LittleFS.exists("/config/peripherals.json"));
    TEST_ASSERT_TRUE(LittleFS.exists("/config/periph_exec.json"));
    TestLog::step("All config files available for web system after reset");

    // Simulate web API reading config (should not fail)
    for (const auto& entry : g_mockFiles) {
        MockFile f(entry.first, "r");
        TEST_ASSERT_TRUE(static_cast<bool>(f));
        String content = f.readString();
        TEST_ASSERT_TRUE(content.length() > 0);
        // Verify content is valid JSON (starts with '{' and ends with '}')
        TEST_ASSERT_EQUAL('{', content[0]);
        TEST_ASSERT_EQUAL('}', content[content.length() - 1]);
    }
    TestLog::step("All config files readable with valid JSON content");

    // Cleanup
    g_mockFiles.clear();

    TestLog::testEnd(true);
}

// Test factory reset permission requirement
void test_factory_reset_requires_permission() {
    TestLog::testStart("Factory Reset Permission");

    MockRoleManager& roleMgr = MockRoleMgr;
    roleMgr.initialize();

    MockUserManager& userMgr = MockUserMgr;
    userMgr.initialize();

    // Create a viewer user (no system.restart permission)
    userMgr.createUser("readonly", "pass123", {"viewer"});

    MockAuthManager authMgr(&userMgr, &roleMgr);
    authMgr.initialize();

    // Admin should have system.restart permission (required for factory reset)
    String adminSession = authMgr.authenticate("admin", "admin123");
    TEST_ASSERT_FALSE(adminSession.isEmpty());
    TEST_ASSERT_TRUE(authMgr.sessionHasPermission(adminSession, "system.restart"));
    TestLog::step("Admin has system.restart permission");

    // Viewer should NOT have system.restart permission
    String viewerSession = authMgr.authenticate("readonly", "pass123");
    TEST_ASSERT_FALSE(viewerSession.isEmpty());
    TEST_ASSERT_FALSE(authMgr.sessionHasPermission(viewerSession, "system.restart"));
    TestLog::step("Viewer does NOT have system.restart permission");

    TestLog::testEnd(true);
}

// Test requirePermission returns 401 for unauthenticated requests
void test_require_permission_returns_401_for_unauthenticated() {
    TestLog::testStart("RequirePermission 401 Unauthenticated");

    MockRoleManager& roleMgr = MockRoleMgr;
    roleMgr.initialize();

    MockUserManager& userMgr = MockUserMgr;
    userMgr.initialize();

    MockAuthManager authMgr(&userMgr, &roleMgr);
    authMgr.initialize();

    MockWebAPIHandler apiHandler;
    apiHandler.setAuthManager(&authMgr);

    // No session ID
    MockHTTPRequest noSessionReq;
    noSessionReq.sessionId = "";
    int status = apiHandler.requirePermission(noSessionReq, "device.view");
    TEST_ASSERT_EQUAL(401, status);
    TestLog::step("No session -> 401 Unauthorized");

    // Invalid session ID
    MockHTTPRequest invalidSessionReq;
    invalidSessionReq.sessionId = "invalid_session";
    status = apiHandler.requirePermission(invalidSessionReq, "device.view");
    TEST_ASSERT_EQUAL(401, status);
    TestLog::step("Invalid session -> 401 Unauthorized");

    TestLog::testEnd(true);
}

// Test requirePermission returns 403 for authenticated but unauthorized requests
void test_require_permission_returns_403_for_forbidden() {
    TestLog::testStart("RequirePermission 403 Forbidden");

    MockRoleManager& roleMgr = MockRoleMgr;
    roleMgr.initialize();

    MockUserManager& userMgr = MockUserMgr;
    userMgr.initialize();

    // Create a viewer user with only read permissions
    userMgr.createUser("viewer_user", "pass123", {"viewer"});

    MockAuthManager authMgr(&userMgr, &roleMgr);
    authMgr.initialize();

    MockWebAPIHandler apiHandler;
    apiHandler.setAuthManager(&authMgr);

    // Viewer has device.view, should get 200
    String viewerSession = authMgr.authenticate("viewer_user", "pass123");
    TEST_ASSERT_FALSE(viewerSession.isEmpty());

    MockHTTPRequest viewerReq;
    viewerReq.sessionId = viewerSession;
    int status = apiHandler.requirePermission(viewerReq, "device.view");
    TEST_ASSERT_EQUAL(200, status);
    TestLog::step("Viewer with device.view -> 200 OK");

    // Viewer does NOT have device.control, should get 403
    status = apiHandler.requirePermission(viewerReq, "device.control");
    TEST_ASSERT_EQUAL(403, status);
    TestLog::step("Viewer without device.control -> 403 Forbidden");

    // Viewer does NOT have user.create, should get 403
    status = apiHandler.requirePermission(viewerReq, "user.create");
    TEST_ASSERT_EQUAL(403, status);
    TestLog::step("Viewer without user.create -> 403 Forbidden");

    // Viewer does NOT have system.reboot, should get 403
    status = apiHandler.requirePermission(viewerReq, "system.reboot");
    TEST_ASSERT_EQUAL(403, status);
    TestLog::step("Viewer without system.reboot -> 403 Forbidden");

    TestLog::testEnd(true);
}

// Test admin user passes all permission checks
void test_admin_passes_all_permissions() {
    TestLog::testStart("Admin Passes All Permissions");

    MockRoleManager& roleMgr = MockRoleMgr;
    roleMgr.initialize();

    MockUserManager& userMgr = MockUserMgr;
    userMgr.initialize();

    MockAuthManager authMgr(&userMgr, &roleMgr);
    authMgr.initialize();

    MockWebAPIHandler apiHandler;
    apiHandler.setAuthManager(&authMgr);

    String adminSession = authMgr.authenticate("admin", "admin123");
    TEST_ASSERT_FALSE(adminSession.isEmpty());

    MockHTTPRequest adminReq;
    adminReq.sessionId = adminSession;

    // Admin should pass all permission checks
    TEST_ASSERT_EQUAL(200, apiHandler.requirePermission(adminReq, "device.view"));
    TEST_ASSERT_EQUAL(200, apiHandler.requirePermission(adminReq, "device.control"));
    TEST_ASSERT_EQUAL(200, apiHandler.requirePermission(adminReq, "user.create"));
    TEST_ASSERT_EQUAL(200, apiHandler.requirePermission(adminReq, "user.delete"));
    TEST_ASSERT_EQUAL(200, apiHandler.requirePermission(adminReq, "role.create"));
    TEST_ASSERT_EQUAL(200, apiHandler.requirePermission(adminReq, "system.restart"));
    TEST_ASSERT_EQUAL(200, apiHandler.requirePermission(adminReq, "config.edit"));
    TEST_ASSERT_EQUAL(200, apiHandler.requirePermission(adminReq, "fs.manage"));
    TestLog::step("Admin passed all 8 permission checks");

    TestLog::testEnd(true);
}

// Test operator role permission boundaries
void test_operator_permission_boundary() {
    TestLog::testStart("Operator Permission Boundary");

    MockRoleManager& roleMgr = MockRoleMgr;
    roleMgr.initialize();

    MockUserManager& userMgr = MockUserMgr;
    userMgr.initialize();

    userMgr.createUser("operator_user", "pass123", {"operator"});

    MockAuthManager authMgr(&userMgr, &roleMgr);
    authMgr.initialize();

    MockWebAPIHandler apiHandler;
    apiHandler.setAuthManager(&authMgr);

    String operatorSession = authMgr.authenticate("operator_user", "pass123");
    TEST_ASSERT_FALSE(operatorSession.isEmpty());

    MockHTTPRequest operatorReq;
    operatorReq.sessionId = operatorSession;

    // Operator should have these permissions
    TEST_ASSERT_EQUAL(200, apiHandler.requirePermission(operatorReq, "device.view"));
    TEST_ASSERT_EQUAL(200, apiHandler.requirePermission(operatorReq, "device.control"));
    TEST_ASSERT_EQUAL(200, apiHandler.requirePermission(operatorReq, "peripheral.view"));
    TEST_ASSERT_EQUAL(200, apiHandler.requirePermission(operatorReq, "peripheral.control"));
    TEST_ASSERT_EQUAL(200, apiHandler.requirePermission(operatorReq, "log.view"));
    TestLog::step("Operator has expected permissions");

    // Operator should NOT have these permissions
    TEST_ASSERT_EQUAL(403, apiHandler.requirePermission(operatorReq, "user.create"));
    TEST_ASSERT_EQUAL(403, apiHandler.requirePermission(operatorReq, "user.delete"));
    TEST_ASSERT_EQUAL(403, apiHandler.requirePermission(operatorReq, "role.create"));
    TEST_ASSERT_EQUAL(403, apiHandler.requirePermission(operatorReq, "system.reboot"));
    TEST_ASSERT_EQUAL(403, apiHandler.requirePermission(operatorReq, "config.edit"));
    TestLog::step("Operator correctly denied admin-only permissions");

    TestLog::testEnd(true);
}

// Test group entry point
void test_web_api_group() {
    TestLog::groupStart("Web API Tests");

    RUN_TEST(test_api_login);
    RUN_TEST(test_session_validation);
    RUN_TEST(test_api_network_status);
    RUN_TEST(test_api_system_info);
    RUN_TEST(test_unauthorized_access);
    RUN_TEST(test_permission_control);
    RUN_TEST(test_api_error_handling);
    RUN_TEST(test_session_expiration);
    RUN_TEST(test_concurrent_sessions);
    RUN_TEST(test_factory_reset_writes_defaults);
    RUN_TEST(test_factory_reset_no_file_missing);
    RUN_TEST(test_factory_reset_requires_permission);

    // requirePermission 401/403 区分测试
    RUN_TEST(test_require_permission_returns_401_for_unauthenticated);
    RUN_TEST(test_require_permission_returns_403_for_forbidden);
    RUN_TEST(test_admin_passes_all_permissions);
    RUN_TEST(test_operator_permission_boundary);

    TestLog::groupEnd();
}
