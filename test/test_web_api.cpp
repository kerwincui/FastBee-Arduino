/**
 * @file test_web_api.cpp
 * @brief Web API Tests
 */

#include <unity.h>
#include <Arduino.h>
#include "mocks/MockAuth.h"
#include "mocks/MockWiFi.h"
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
    MockWebAPIHandler() : _authMgr(nullptr) {}
    
    void setAuthManager(MockAuthManager* authMgr) {
        _authMgr = authMgr;
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
    
private:
    MockAuthManager* _authMgr;
    
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
    
    TestLog::groupEnd();
}
