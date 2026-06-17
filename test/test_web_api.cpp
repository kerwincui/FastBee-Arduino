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
    MockWebAPIHandler() : _authMgr(nullptr), _userMgr(nullptr) {}

    void setAuthManager(MockAuthManager* authMgr) {
        _authMgr = authMgr;
    }

    void setUserManager(MockUserManager* userMgr) {
        _userMgr = userMgr;
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
     * 模拟 requireAuth 行为（单管理员模式）：
     * 认证失败返回401，认证成功即授权（返回200）
     */
    int requireAuth(const MockHTTPRequest& req) {
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

        // 单管理员模式：认证即授权，始终返回200
        return 200;
    }

private:
    MockAuthManager* _authMgr;
    MockUserManager* _userMgr;

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

// Test single-admin auth model (authenticated = authorized)
void test_auth_control() {
    TestLog::testStart("Single Admin Auth Model");

    MockUserManager& userMgr = MockUserMgr;
    userMgr.initialize();

    MockAuthManager authMgr(&userMgr);
    authMgr.initialize();

    MockWebAPIHandler apiHandler;
    apiHandler.setAuthManager(&authMgr);

    // Authenticate as admin
    String sessionId = authMgr.authenticate("admin", "admin123");
    TEST_ASSERT_FALSE(sessionId.isEmpty());

    MockHTTPRequest req;
    req.sessionId = sessionId;

    // In single-admin mode, any authenticated user passes auth check
    TEST_ASSERT_EQUAL(200, apiHandler.requireAuth(req));
    TestLog::step("Authenticated user passes auth check");

    TEST_ASSERT_EQUAL(200, apiHandler.requireAuth(req));
    TestLog::step("Authenticated user passes second auth check");

    TEST_ASSERT_EQUAL(200, apiHandler.requireAuth(req));
    TestLog::step("Authenticated user passes third auth check");

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

    userMgr.createUser("user1", "pass1");
    userMgr.createUser("user2", "pass2");
    userMgr.createUser("user3", "pass3");

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

    TEST_ASSERT_EQUAL(3, authMgr.getOnlineUserCount());
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
    const char* DEFAULT_USERS = "{\"version\":\"2.0\",\"users\":[{\"username\":\"admin\"}]}";
    if (writeDefault("/config/users.json", DEFAULT_USERS)) resetCount++;

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

    // Verify: all 6 core files written successfully
    TEST_ASSERT_EQUAL(6, resetCount);
    TestLog::step("All 6 core config files reset successfully");

    // Verify: core config files exist with default content
    TEST_ASSERT_TRUE(LittleFS.exists("/config/device.json"));
    TEST_ASSERT_TRUE(LittleFS.exists("/config/network.json"));
    TEST_ASSERT_TRUE(LittleFS.exists("/config/users.json"));
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
    writeDefault("/config/protocol.json", "{\"version\":2}");
    writeDefault("/config/peripherals.json", "{\"peripherals\":[]}");
    writeDefault("/config/periph_exec.json", "{\"version\":3,\"rules\":[]}");

    // All config files should exist - web system can read them without errors
    TEST_ASSERT_TRUE(LittleFS.exists("/config/device.json"));
    TEST_ASSERT_TRUE(LittleFS.exists("/config/network.json"));
    TEST_ASSERT_TRUE(LittleFS.exists("/config/users.json"));
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

// Test factory reset requires authentication (single-admin mode)
void test_factory_reset_requires_auth() {
    TestLog::testStart("Factory Reset Requires Auth");

    MockUserManager& userMgr = MockUserMgr;
    userMgr.initialize();

    MockAuthManager authMgr(&userMgr);
    authMgr.initialize();

    MockWebAPIHandler apiHandler;
    apiHandler.setAuthManager(&authMgr);

    // Unauthenticated should be rejected
    MockHTTPRequest noAuthReq;
    noAuthReq.sessionId = "";
    int status = apiHandler.requireAuth(noAuthReq);
    TEST_ASSERT_EQUAL(401, status);
    TestLog::step("Unauthenticated -> 401");

    // Admin authenticated should pass (single-admin: auth = authorized)
    String adminSession = authMgr.authenticate("admin", "admin123");
    TEST_ASSERT_FALSE(adminSession.isEmpty());

    MockHTTPRequest adminReq;
    adminReq.sessionId = adminSession;
    status = apiHandler.requireAuth(adminReq);
    TEST_ASSERT_EQUAL(200, status);
    TestLog::step("Authenticated admin -> 200 (authorized)");

    TestLog::testEnd(true);
}

// Test requireAuth returns 401 for unauthenticated requests
void test_require_auth_returns_401_for_unauthenticated() {
    TestLog::testStart("RequireAuth 401 Unauthenticated");

    MockUserManager& userMgr = MockUserMgr;
    userMgr.initialize();

    MockAuthManager authMgr(&userMgr);
    authMgr.initialize();

    MockWebAPIHandler apiHandler;
    apiHandler.setAuthManager(&authMgr);

    // No session ID
    MockHTTPRequest noSessionReq;
    noSessionReq.sessionId = "";
    int status = apiHandler.requireAuth(noSessionReq);
    TEST_ASSERT_EQUAL(401, status);
    TestLog::step("No session -> 401 Unauthorized");

    // Invalid session ID
    MockHTTPRequest invalidSessionReq;
    invalidSessionReq.sessionId = "invalid_session";
    status = apiHandler.requireAuth(invalidSessionReq);
    TEST_ASSERT_EQUAL(401, status);
    TestLog::step("Invalid session -> 401 Unauthorized");

    TestLog::testEnd(true);
}

// Test single-admin mode: all authenticated users get full access (no 403)
void test_single_admin_no_403() {
    TestLog::testStart("Single Admin Mode No 403");

    MockUserManager& userMgr = MockUserMgr;
    userMgr.initialize();

    // Create a non-admin user
    userMgr.createUser("regular_user", "pass123");

    MockAuthManager authMgr(&userMgr);
    authMgr.initialize();

    MockWebAPIHandler apiHandler;
    apiHandler.setAuthManager(&authMgr);

    // Any authenticated user passes auth check in single-admin mode
    String userSession = authMgr.authenticate("regular_user", "pass123");
    TEST_ASSERT_FALSE(userSession.isEmpty());

    MockHTTPRequest req;
    req.sessionId = userSession;

    // All auth checks should return 200 (no role-based restrictions)
    TEST_ASSERT_EQUAL(200, apiHandler.requireAuth(req));
    TEST_ASSERT_EQUAL(200, apiHandler.requireAuth(req));
    TEST_ASSERT_EQUAL(200, apiHandler.requireAuth(req));
    TEST_ASSERT_EQUAL(200, apiHandler.requireAuth(req));
    TEST_ASSERT_EQUAL(200, apiHandler.requireAuth(req));
    TestLog::step("Authenticated user passes all auth checks (single-admin)");

    TestLog::testEnd(true);
}

// Test admin user passes all auth checks
void test_admin_passes_all_auth() {
    TestLog::testStart("Admin Passes All Auth");

    MockUserManager& userMgr = MockUserMgr;
    userMgr.initialize();

    MockAuthManager authMgr(&userMgr);
    authMgr.initialize();

    MockWebAPIHandler apiHandler;
    apiHandler.setAuthManager(&authMgr);

    String adminSession = authMgr.authenticate("admin", "admin123");
    TEST_ASSERT_FALSE(adminSession.isEmpty());

    MockHTTPRequest adminReq;
    adminReq.sessionId = adminSession;

    // Admin should pass all auth checks
    TEST_ASSERT_EQUAL(200, apiHandler.requireAuth(adminReq));
    TEST_ASSERT_EQUAL(200, apiHandler.requireAuth(adminReq));
    TEST_ASSERT_EQUAL(200, apiHandler.requireAuth(adminReq));
    TEST_ASSERT_EQUAL(200, apiHandler.requireAuth(adminReq));
    TEST_ASSERT_EQUAL(200, apiHandler.requireAuth(adminReq));
    TEST_ASSERT_EQUAL(200, apiHandler.requireAuth(adminReq));
    TEST_ASSERT_EQUAL(200, apiHandler.requireAuth(adminReq));
    TestLog::step("Admin passed all 7 auth checks");

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
    RUN_TEST(test_auth_control);
    RUN_TEST(test_api_error_handling);
    RUN_TEST(test_session_expiration);
    RUN_TEST(test_concurrent_sessions);
    RUN_TEST(test_factory_reset_writes_defaults);
    RUN_TEST(test_factory_reset_no_file_missing);
    RUN_TEST(test_factory_reset_requires_auth);

    // 单管理员模式认证测试（认证即授权，无角色区分）
    RUN_TEST(test_require_auth_returns_401_for_unauthenticated);
    RUN_TEST(test_single_admin_no_403);
    RUN_TEST(test_admin_passes_all_auth);

    TestLog::groupEnd();
}
