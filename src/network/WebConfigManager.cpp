#include "./network/WebConfigManager.h"
#include "./security/AuthManager.h"
#include "./security/UserManager.h"
#include "./network/NetworkManager.h"
#include "./network/OTAManager.h"
#include "./protocols/ProtocolManager.h"

WebConfigManager::WebConfigManager(AsyncWebServer* webServer, 
                                  AuthManager* authMgr, UserManager* userMgr)
    : server(webServer), authManager(authMgr), userManager(userMgr),
      networkManager(nullptr), otaManager(nullptr), protocolManager(nullptr),
      isRunning(false) {
    preferences.begin("web_config", false);
    webRootPath = "/www";
}

WebConfigManager::~WebConfigManager() {
    stop();
    preferences.end();
}

bool WebConfigManager::initialize() {
    if (!server) {
        Serial.println("[WebConfig] Error: Web server is null");
        return false;
    }
    
    if (!authManager || !userManager) {
        Serial.println("[WebConfig] Error: AuthManager or UserManager is null");
        return false;
    }
    
    // 加载配置
    loadConfiguration();
    
    // 初始化文件系统
    if (!LittleFS.begin()) {
        Serial.println("[WebConfig] Failed to mount LittleFS");
        return false;
    }
    
    // 设置路由
    setupStaticRoutes();
    setupAuthRoutes();
    setupUserRoutes();
    setupSystemRoutes();
    setupAPIRoutes();

    // 启动服务器
    start();
    
    Serial.println("[WebConfig] Routes configured");
    return true;
}

bool WebConfigManager::start() {
    if (!server) {
        return false;
    }
    
    server->begin();
    isRunning = true;
    Serial.println("[WebConfig] Web server started");
    return true;
}

void WebConfigManager::stop() {
    if (server && isRunning) {
        // AsyncWebServer没有stop方法，这里只是标记状态
        isRunning = false;
        Serial.println("[WebConfig] Web server stopped");
    }
}

bool WebConfigManager::isServerRunning() const {
    return isRunning;
}

void WebConfigManager::setNetworkManager(NetworkManager* netMgr) {
    networkManager = netMgr;
}

void WebConfigManager::setOTAManager(OTAManager* otaMgr) {
    otaManager = otaMgr;
}

void WebConfigManager::setProtocolManager(ProtocolManager* protoMgr) {
    protocolManager = protoMgr;
}

AsyncWebServer* WebConfigManager::getWebServer() const {
    return server;
}

void WebConfigManager::performMaintenance() {
    // 可以在这里执行定期清理任务
}

// ============ 私有方法实现 ============

void WebConfigManager::loadConfiguration() {
    // 加载Web配置
    // 这里可以加载端口、超时时间等配置
}

void WebConfigManager::setupStaticRoutes() {
    // 静态文件服务,setIsDir禁用目录列表，精确匹配路径,max-age=86400=一天
    server->serveStatic("/css/", LittleFS, "/www/css/").setIsDir(false).setCacheControl("no-cache");
    server->serveStatic("/js/", LittleFS, "/www/js/").setIsDir(false).setCacheControl("no-cache");
    server->serveStatic("/assets/", LittleFS, "/www/assets/").setIsDir(false).setCacheControl("max-age=86400");

    // 根路径重定向到登录页或仪表板
    server->on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleRoot(request);
    });
    
    // 页面服务
    server->on("/login", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleLoginPage(request);
    });
    
    server->on("/dashboard", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleDashboard(request);
    });
    
    server->on("/users", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleUsersPage(request);
    });
    
    server->on("/settings", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleSettingsPage(request);
    });
    
    server->on("/monitor", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleMonitorPage(request);
    });
    
    // 默认文件服务
    server->onNotFound([this](AsyncWebServerRequest* request) {
        if (!serveStaticFile(request, webRootPath + request->url())) {
            sendNotFound(request);
        }
    });
}

void WebConfigManager::setupAuthRoutes() {
    // 登录API - 使用表单参数
    server->on("/api/auth/login", HTTP_POST, 
              [this](AsyncWebServerRequest* request) {
        handleAPILogin(request);
    });
    
    // 登出API
    server->on("/api/auth/logout", HTTP_POST, 
              [this](AsyncWebServerRequest* request) {
        handleAPILogout(request);
    });
    
    // 验证会话API
    server->on("/api/auth/session", HTTP_GET, 
              [this](AsyncWebServerRequest* request) {
        handleAPIVerifySession(request);
    });
    
    // 修改密码API - 使用表单参数
    server->on("/api/auth/change-password", HTTP_POST, 
              [this](AsyncWebServerRequest* request) {
        handleAPIChangePassword(request);
    });
}

void WebConfigManager::setupUserRoutes() {
    // 获取用户列表
    server->on("/api/users", HTTP_GET, 
              [this](AsyncWebServerRequest* request) {
        handleAPIGetUsers(request);
    });
    
    // 获取单个用户
    server->on("^\\/api\\/users\\/([^\\/]+)$", HTTP_GET, 
              [this](AsyncWebServerRequest* request) {
        handleAPIGetUser(request);
    });
    
    // 添加用户 - 使用表单参数
    server->on("/api/users", HTTP_POST, 
              [this](AsyncWebServerRequest* request) {
        handleAPIAddUser(request);
    });
    
    // 更新用户 - 使用表单参数
    server->on("^\\/api\\/users\\/([^\\/]+)$", HTTP_PUT, 
              [this](AsyncWebServerRequest* request) {
        handleAPIUpdateUser(request);
    });
    
    // 删除用户
    server->on("^\\/api\\/users\\/([^\\/]+)$", HTTP_DELETE, 
              [this](AsyncWebServerRequest* request) {
        handleAPIDeleteUser(request);
    });
    
    // 获取在线用户
    server->on("/api/users/online", HTTP_GET, 
              [this](AsyncWebServerRequest* request) {
        handleAPIGetOnlineUsers(request);
    });
}

void WebConfigManager::setupSystemRoutes() {
    // 系统信息
    server->on("/api/system/info", HTTP_GET, 
              [this](AsyncWebServerRequest* request) {
        handleAPISystemInfo(request);
    });
    
    // 系统状态
    server->on("/api/system/status", HTTP_GET, 
              [this](AsyncWebServerRequest* request) {
        handleAPISystemStatus(request);
    });
    
    // 系统重启
    server->on("/api/system/restart", HTTP_POST, 
              [this](AsyncWebServerRequest* request) {
        handleAPISystemRestart(request);
    });
    
    // 文件系统信息
    server->on("/api/system/fs-info", HTTP_GET, 
              [this](AsyncWebServerRequest* request) {
        handleAPIFileSystemInfo(request);
    });
    
    // 健康检查
    server->on("/api/health", HTTP_GET, 
              [this](AsyncWebServerRequest* request) {
        handleAPIHealthCheck(request);
    });
}

void WebConfigManager::setupAPIRoutes() {
    // 配置管理
    server->on("/api/config", HTTP_GET, 
              [this](AsyncWebServerRequest* request) {
        handleAPIGetConfig(request);
    });
    
    server->on("/api/config", HTTP_PUT, 
              [this](AsyncWebServerRequest* request) {
        handleAPIUpdateConfig(request);
    });
    
    // 网络配置
    server->on("/api/network/config", HTTP_GET, 
              [this](AsyncWebServerRequest* request) {
        handleAPIGetNetworkConfig(request);
    });
    
    server->on("/api/network/config", HTTP_PUT, 
              [this](AsyncWebServerRequest* request) {
        handleAPIUpdateNetworkConfig(request);
    });
    
    // OTA更新
    // server->on("/api/ota/update", HTTP_POST, 
    //           [this](AsyncWebServerRequest* request) {
    //     handleAPIOtaUpdate(request);
    // }, 
    // [this](AsyncWebServerRequest* request, const String& filename, 
    //        size_t index, uint8_t* data, size_t len, bool final) {
    //     if (otaManager) {
    //         otaManager->performUpdate(request, filename, index, data, len, final);
    //     }
    // });
    
    server->on("/api/ota/status", HTTP_GET, 
              [this](AsyncWebServerRequest* request) {
        handleAPIOtaStatus(request);
    });
}

// ============ 参数处理辅助方法 ============

String WebConfigManager::getParamValue(AsyncWebServerRequest* request, const String& paramName, 
                                      const String& defaultValue) {
    if (!request || !request->hasParam(paramName, true)) {
        return defaultValue;
    }
    
    // 修正：getParam返回const指针
    const AsyncWebParameter* param = request->getParam(paramName, true);
    return param->value();
}

bool WebConfigManager::getParamBool(AsyncWebServerRequest* request, const String& paramName, 
                                   bool defaultValue) {
    String value = getParamValue(request, paramName, defaultValue ? "true" : "false");
    return value.equalsIgnoreCase("true") || value == "1";
}

int WebConfigManager::getParamInt(AsyncWebServerRequest* request, const String& paramName, 
                                 int defaultValue) {
    String value = getParamValue(request, paramName, String(defaultValue));
    return value.toInt();
}

// ============ 认证辅助方法 ============

bool WebConfigManager::requiresAuth(AsyncWebServerRequest* request) {
    // 定义不需要认证的路径
    const char* publicPaths[] = {
        "/api/auth/login",
        "/api/health",
        "/login",
        "/css/",
        "/js/",
        "/images/",
        "/assets/"
    };
    
    String path = request->url();
    
    // 检查是否为公开路径
    for (const char* publicPath : publicPaths) {
        if (path.startsWith(publicPath)) {
            return false;
        }
    }
    
    return true;
}

AuthResult WebConfigManager::authenticateRequest(AsyncWebServerRequest* request) {
    if (!authManager) {
        return AuthResult();
    }
    
    // 从请求中提取会话ID
    String sessionId = AuthManager::extractSessionIdFromRequest(request);
    if (sessionId.isEmpty()) {
        return AuthResult();
    }
    
    // 验证会话
    return authManager->verifySession(sessionId, true);
}

String WebConfigManager::getClientIP(AsyncWebServerRequest* request) {
    if (!request) return "";
    
    // 尝试获取真实IP（如果有代理）
    if (request->hasHeader("X-Forwarded-For")) {
        return request->header("X-Forwarded-For");
    }
    
    if (request->hasHeader("X-Real-IP")) {
        return request->header("X-Real-IP");
    }
    
    return request->client()->remoteIP().toString();
}

String WebConfigManager::getUserAgent(AsyncWebServerRequest* request) {
    if (!request) return "";
    
    if (request->hasHeader("User-Agent")) {
        return request->header("User-Agent");
    }
    
    return "";
}

bool WebConfigManager::checkPermission(AsyncWebServerRequest* request, const String& permission) {
    if (!authManager || !request) {
        return false;
    }
    
    // 验证请求
    AuthResult authResult = authenticateRequest(request);
    if (!authResult.success) {
        return false;
    }
    
    // 检查权限
    return authManager->checkSessionPermission(authResult.sessionId, permission, 
                                              request->methodToString());
}

// ============ 响应辅助方法 ============

void WebConfigManager::sendJsonResponse(AsyncWebServerRequest* request, int code, 
                                       const JsonDocument& doc) {
    if (!request) return;
    
    String jsonStr;
    serializeJson(doc, jsonStr);
    
    // 直接使用 request->send，它会自动处理响应对象
    AsyncWebServerResponse* response = request->beginResponse(code, "application/json", jsonStr);
    
    // 添加 CORS 头
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    
    request->send(response);
}

void WebConfigManager::sendSuccess(AsyncWebServerRequest* request, const JsonDocument& data) {
    StaticJsonDocument<256> doc;
    doc["success"] = true;
    doc["timestamp"] = millis();
    
    if (!data.isNull()) {
        doc["data"] = data;
    }
    
    sendJsonResponse(request, 200, doc);
}

void WebConfigManager::sendSuccess(AsyncWebServerRequest* request, const String& message) {
    StaticJsonDocument<256> doc;
    doc["success"] = true;
    doc["message"] = message;
    doc["timestamp"] = millis();
    
    sendJsonResponse(request, 200, doc);
}

void WebConfigManager::sendError(AsyncWebServerRequest* request, int code, const String& message) {
    StaticJsonDocument<256> doc;
    doc["success"] = false;
    doc["error"] = message;
    doc["code"] = code;
    doc["timestamp"] = millis();
    
    sendJsonResponse(request, code, doc);
}

void WebConfigManager::sendUnauthorized(AsyncWebServerRequest* request) {
    sendError(request, 401, "Unauthorized");
}

void WebConfigManager::sendForbidden(AsyncWebServerRequest* request) {
    sendError(request, 403, "Forbidden");
}

void WebConfigManager::sendNotFound(AsyncWebServerRequest* request) {
    sendError(request, 404, "Not Found");
}

void WebConfigManager::sendBadRequest(AsyncWebServerRequest* request, const String& message) {
    sendError(request, 400, message);
}

// ============ 文件服务方法 ============

bool WebConfigManager::serveStaticFile(AsyncWebServerRequest* request, const String& path) {
    if (!fileExists(path)) {
        return false;
    }
    
    String contentType = getContentType(path);
    request->send(LittleFS, path, contentType);
    return true;
}

String WebConfigManager::getContentType(const String& filename) {
    if (filename.endsWith(".html")) return "text/html";
    if (filename.endsWith(".css")) return "text/css";
    if (filename.endsWith(".js")) return "application/javascript";
    if (filename.endsWith(".png")) return "image/png";
    if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) return "image/jpeg";
    if (filename.endsWith(".gif")) return "image/gif";
    if (filename.endsWith(".ico")) return "image/x-icon";
    if (filename.endsWith(".svg")) return "image/svg+xml";
    if (filename.endsWith(".json")) return "application/json";
    if (filename.endsWith(".xml")) return "application/xml";
    if (filename.endsWith(".pdf")) return "application/pdf";
    if (filename.endsWith(".zip")) return "application/zip";
    if (filename.endsWith(".gz")) return "application/x-gzip";
    if (filename.endsWith(".txt")) return "text/plain";
    return "application/octet-stream";
}

String WebConfigManager::readFile(const String& path) {
    if (!LittleFS.exists(path)) {
        return "";
    }
    
    File file = LittleFS.open(path, "r");
    if (!file) {
        return "";
    }
    
    String content = file.readString();
    file.close();
    return content;
}

bool WebConfigManager::fileExists(const String& path) {
    return LittleFS.exists(path);
}

// ============ 请求处理器实现 ============

void WebConfigManager::handleRoot(AsyncWebServerRequest* request) {
    // 检查用户是否已登录
    AuthResult authResult = authenticateRequest(request);
    if (authResult.success) {
        // 已登录，重定向到仪表板
        request->redirect("/dashboard");
    } else {
        // 未登录，重定向到登录页
        request->redirect("/login");
    }
}

void WebConfigManager::handleLoginPage(AsyncWebServerRequest* request) {
    serveStaticFile(request, webRootPath + "/index.html");
}

void WebConfigManager::handleDashboard(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }
    serveStaticFile(request, webRootPath + "/index.html");
}

void WebConfigManager::handleUsersPage(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.view")) {
        sendUnauthorized(request);
        return;
    }
    serveStaticFile(request, webRootPath + "/index.html");
}

void WebConfigManager::handleSettingsPage(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.view")) {
        sendUnauthorized(request);
        return;
    }
    serveStaticFile(request, webRootPath + "/index.html");
}

void WebConfigManager::handleMonitorPage(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }
    serveStaticFile(request, webRootPath + "/index.html");
}

// ============ 认证API处理器 ============

void WebConfigManager::handleAPILogin(AsyncWebServerRequest* request) {
    if (request->method() != HTTP_POST) {
        sendError(request, 405, "Method not allowed");
        return;
    }
    
    // 从参数获取用户名和密码
    String username = getParamValue(request, "username", "");
    String password = getParamValue(request, "password", "");
    
    if (username.isEmpty() || password.isEmpty()) {
        sendBadRequest(request, "Username and password are required");
        return;
    }
    
    // 获取客户端信息
    String ipAddress = getClientIP(request);
    String userAgent = getUserAgent(request);
    
    // 执行登录
    if (!authManager) {
        sendError(request, 500, "Authentication service unavailable");
        return;
    }
    
    AuthResult result = authManager->login(username, password, ipAddress, userAgent);
    
    if (result.success) {
        StaticJsonDocument<256> responseDoc;
        responseDoc["success"] = true;
        responseDoc["sessionId"] = result.sessionId;
        responseDoc["username"] = result.username;
        responseDoc["role"] = UserManager::roleToString(result.userRole);
        // responseDoc["expiresIn"] = authManager->getSessionTimeout() / 1000; // 修正：使用正确的方法

        String jsonStr;
        serializeJson(responseDoc, jsonStr);
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", jsonStr);
        
        String cookieName = "sessionId"; // 简化：直接使用固定名称
        String cookieValue = cookieName + "=" + result.sessionId + "; Path=/; Max-Age=" + 
                           String(3600); // 简化：1小时
        
        response->addHeader("Set-Cookie", cookieValue);
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);
    } else {
        sendError(request, 401, result.message);
    }
}

void WebConfigManager::handleAPILogout(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.view")) {
        sendUnauthorized(request);
        return;
    }
    
    // 从请求中获取会话ID
    String sessionId = AuthManager::extractSessionIdFromRequest(request);
    
    if (sessionId.isEmpty()) {
        sendError(request, 400, "No session found");
        return;
    }
    
    // 执行登出
    if (authManager && authManager->logout(sessionId)) {
        sendSuccess(request, "Logged out successfully");
    } else {
        sendError(request, 400, "Logout failed");
    }
}

void WebConfigManager::handleAPIVerifySession(AsyncWebServerRequest* request) {
    AuthResult authResult = authenticateRequest(request);
    
    if (authResult.success) {
        StaticJsonDocument<256> responseDoc;
        responseDoc["username"] = authResult.username;
        responseDoc["role"] = UserManager::roleToString(authResult.userRole);
        responseDoc["sessionValid"] = true;
        responseDoc["timestamp"] = millis();
        
        sendSuccess(request, responseDoc);
    } else {
        sendUnauthorized(request);
    }
}

void WebConfigManager::handleAPIChangePassword(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    // 从参数获取旧密码和新密码
    String oldPassword = getParamValue(request, "oldPassword", "");
    String newPassword = getParamValue(request, "newPassword", "");
    
    if (oldPassword.isEmpty() || newPassword.isEmpty()) {
        sendBadRequest(request, "Old password and new password are required");
        return;
    }
    
    // 获取当前用户
    AuthResult authResult = authenticateRequest(request);
    if (!authResult.success) {
        sendUnauthorized(request);
        return;
    }
    
    // 执行密码修改
    if (!userManager) {
        sendError(request, 500, "User service unavailable");
        return;
    }
    
    if (userManager->changePassword(authResult.username, oldPassword, newPassword)) {
        // 密码修改成功，强制登出所有会话（安全措施）
        if (authManager) {
            authManager->forceLogout(authResult.username);
        }
        
        sendSuccess(request, "Password changed successfully");
    } else {
        sendError(request, 400, "Password change failed");
    }
}

// ============ 用户管理API处理器 ============

void WebConfigManager::handleAPIGetUsers(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.view")) {
        sendUnauthorized(request);
        return;
    }
    
    if (!userManager) {
        sendError(request, 500, "User service unavailable");
        return;
    }
    
    // 可选的分页和过滤参数
    int page = getParamInt(request, "page", 1);
    int limit = getParamInt(request, "limit", 50);
    String filter = getParamValue(request, "filter", "");
    
    StaticJsonDocument<2048> doc;
    JsonArray usersArray = doc.createNestedArray("users");
    
    std::vector<User> users = userManager->getAllUsers();
    int startIndex = (page - 1) * limit;
    int endIndex = min(startIndex + limit, (int)users.size());
    int count = 0;
    
    for (int i = startIndex; i < endIndex; i++) {
        const User& user = users[i];
        
        // 应用过滤 - 使用indexOf代替contains
        if (!filter.isEmpty()) {
            if (user.username.indexOf(filter) == -1) {
                continue;
            }
        }
        
        JsonObject userObj = usersArray.createNestedObject();
        userObj["username"] = user.username;
        userObj["role"] = UserManager::roleToString(user.role);
        userObj["enabled"] = user.enabled;
        userObj["createTime"] = user.createTime;
        userObj["lastLogin"] = user.lastLogin;
        userObj["lastModified"] = user.lastModified;
        
        // 检查是否在线
        bool isOnline = false;
        if (authManager) {
            isOnline = authManager->isUserOnline(user.username);
        }
        userObj["isOnline"] = isOnline;
        
        // 检查是否锁定
        bool isLocked = userManager->isAccountLocked(user.username);
        userObj["isLocked"] = isLocked;
        
        // 获取登录尝试次数
        uint8_t attempts = userManager->getLoginAttempts(user.username);
        userObj["loginAttempts"] = attempts;
        
        count++;
    }
    
    doc["page"] = page;
    doc["limit"] = limit;
    doc["total"] = users.size();
    doc["count"] = count;
    doc["timestamp"] = millis();
    
    sendSuccess(request, doc);
}

void WebConfigManager::handleAPIGetUser(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.view")) {
        sendUnauthorized(request);
        return;
    }
    
    // 从URL路径提取用户名
    String path = request->url();
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash == -1) {
        sendError(request, 400, "Invalid URL");
        return;
    }
    
    String username = path.substring(lastSlash + 1);
    
    if (!userManager) {
        sendError(request, 500, "User service unavailable");
        return;
    }
    
    User* user = userManager->getUser(username);
    if (!user) {
        sendError(request, 404, "User not found");
        return;
    }
    
    StaticJsonDocument<512> doc;
    doc["username"] = user->username;
    doc["role"] = UserManager::roleToString(user->role);
    doc["enabled"] = user->enabled;
    doc["createTime"] = user->createTime;
    doc["lastLogin"] = user->lastLogin;
    doc["lastModified"] = user->lastModified;
    
    // 检查是否在线
    bool isOnline = false;
    if (authManager) {
        isOnline = authManager->isUserOnline(username);
    }
    doc["isOnline"] = isOnline;
    
    // 检查是否锁定
    bool isLocked = userManager->isAccountLocked(username);
    doc["isLocked"] = isLocked;
    
    // 获取登录尝试次数
    uint8_t attempts = userManager->getLoginAttempts(username);
    doc["loginAttempts"] = attempts;
    
    sendSuccess(request, doc);
}

void WebConfigManager::handleAPIAddUser(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.create")) {
        sendUnauthorized(request);
        return;
    }
    
    // 从参数获取用户信息
    String username = getParamValue(request, "username", "");
    String password = getParamValue(request, "password", "");
    String roleStr = getParamValue(request, "role", "user");
    
    if (username.isEmpty() || password.isEmpty()) {
        sendBadRequest(request, "Username and password are required");
        return;
    }
    
    UserRole role = UserManager::stringToRole(roleStr);
    
    if (!userManager) {
        sendError(request, 500, "User service unavailable");
        return;
    }
    
    // 修正：使用正确的参数个数
    if (userManager->addUser(username, password, role)) {
        sendSuccess(request, "User added successfully");
    } else {
        sendError(request, 400, "Failed to add user");
    }
}

void WebConfigManager::handleAPIUpdateUser(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    // 从URL路径提取用户名
    String path = request->url();
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash == -1) {
        sendError(request, 400, "Invalid URL");
        return;
    }
    
    String username = path.substring(lastSlash + 1);
    
    // 从参数获取更新信息
    String newPassword = getParamValue(request, "password", "");
    String newRole = getParamValue(request, "role", "");
    bool enabled = getParamBool(request, "enabled", true);
    
    if (!userManager) {
        sendError(request, 500, "User service unavailable");
        return;
    }
    
    // 检查用户是否存在
    User* user = userManager->getUser(username);
    if (!user) {
        sendError(request, 404, "User not found");
        return;
    }
    
    // 执行更新 - 使用UserManager的updateUser方法
    bool success = true;
    
    // 更新用户信息（使用updateUser方法）
    success = success && userManager->updateUser(username, newPassword, newRole, enabled);
    
    if (success) {
        sendSuccess(request, "User updated successfully");
    } else {
        sendError(request, 400, "Failed to update user");
    }
}

void WebConfigManager::handleAPIDeleteUser(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.delete")) {
        sendUnauthorized(request);
        return;
    }
    
    // 从URL路径提取用户名
    String path = request->url();
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash == -1) {
        sendError(request, 400, "Invalid URL");
        return;
    }
    
    String username = path.substring(lastSlash + 1);
    
    // 获取当前用户
    AuthResult authResult = authenticateRequest(request);
    if (!authResult.success) {
        sendUnauthorized(request);
        return;
    }
    
    // 不能删除自己
    if (username == authResult.username) {
        sendError(request, 400, "Cannot delete your own account");
        return;
    }
    
    if (!userManager) {
        sendError(request, 500, "User service unavailable");
        return;
    }
    
    if (userManager->deleteUser(username)) {
        // 强制登出被删除的用户
        if (authManager) {
            authManager->forceLogout(username);
        }
        
        sendSuccess(request, "User deleted successfully");
    } else {
        sendError(request, 400, "Failed to delete user");
    }
}

void WebConfigManager::handleAPIGetOnlineUsers(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.view")) {
        sendUnauthorized(request);
        return;
    }
    
    if (!authManager) {
        sendError(request, 500, "Authentication service unavailable");
        return;
    }
    
    std::vector<UserSession> sessions = authManager->getActiveSessions();
    
    StaticJsonDocument<2048> doc;
    JsonArray onlineUsersArray = doc.createNestedArray("onlineUsers");
    
    for (const UserSession& session : sessions) {
        JsonObject userObj = onlineUsersArray.createNestedObject();
        userObj["username"] = session.username;
        userObj["role"] = UserManager::roleToString(session.role);
        userObj["loginTime"] = session.loginTime;
        userObj["lastAccessTime"] = session.lastAccessTime;
        userObj["ipAddress"] = session.ipAddress;
        userObj["userAgent"] = session.userAgent;
        userObj["sessionId"] = session.sessionId.substring(0, 8) + "..."; // 部分显示
    }
    
    doc["count"] = sessions.size();
    doc["timestamp"] = millis();
    
    sendSuccess(request, doc);
}

// ============ 系统API处理器 ============

void WebConfigManager::handleAPISystemInfo(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }
    
    StaticJsonDocument<512> doc;
    
    // 系统信息
    doc["deviceId"] = String((uint32_t)ESP.getEfuseMac(), HEX);
    doc["chipModel"] = ESP.getChipModel();
    doc["chipRevision"] = ESP.getChipRevision();
    doc["cpuFreqMHz"] = ESP.getCpuFreqMHz();
    doc["flashChipSize"] = ESP.getFlashChipSize();
    doc["flashChipSpeed"] = ESP.getFlashChipSpeed();
    doc["sketchSize"] = ESP.getSketchSize();
    doc["freeSketchSpace"] = ESP.getFreeSketchSpace();
    doc["heapSize"] = ESP.getHeapSize();
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["minFreeHeap"] = ESP.getMinFreeHeap();
    doc["maxAllocHeap"] = ESP.getMaxAllocHeap();
    doc["psramSize"] = ESP.getPsramSize();
    doc["freePsram"] = ESP.getFreePsram();
    doc["minFreePsram"] = ESP.getMinFreePsram();
    doc["maxAllocPsram"] = ESP.getMaxAllocPsram();
    
    // SDK版本
    doc["sdkVersion"] = ESP.getSdkVersion();
    
    // 运行时间
    doc["uptime"] = millis();
    
    // 用户统计
    if (userManager) {
        doc["userCount"] = userManager->getUserCount();
    }
    
    if (authManager) {
        doc["activeSessions"] = authManager->getActiveSessionCount();
        doc["onlineUsers"] = authManager->getOnlineUserCount();
    }
    
    sendSuccess(request, doc);
}

void WebConfigManager::handleAPISystemStatus(AsyncWebServerRequest* request) {
    StaticJsonDocument<256> doc;
    
    doc["status"] = "running";
    doc["timestamp"] = millis();
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["uptime"] = millis();
    
    if (networkManager) {
        // doc["networkConnected"] = networkManager->isConnected();
    }
    
    sendSuccess(request, doc);
}

void WebConfigManager::handleAPISystemRestart(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.restart")) {
        sendUnauthorized(request);
        return;
    }
    
    // 获取延迟重启参数（可选）
    int delaySeconds = getParamInt(request, "delay", 3);
    delaySeconds = constrain(delaySeconds, 1, 30); // 限制在1-30秒之间
    
    StaticJsonDocument<128> doc;
    doc["message"] = "System will restart in " + String(delaySeconds) + " seconds";
    doc["delay"] = delaySeconds;
    doc["timestamp"] = millis();
    
    sendJsonResponse(request, 200, doc);
    
    // 延迟重启
    delay(delaySeconds * 1000);
    ESP.restart();
}

void WebConfigManager::handleAPIFileSystemInfo(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "fs.view")) {
        sendUnauthorized(request);
        return;
    }
    
    StaticJsonDocument<256> doc;
    
    // 获取文件系统信息
    // FSInfo fsInfo;
    // if (LittleFS.info(fsInfo)) {
    //     doc["totalBytes"] = fsInfo.totalBytes;
    //     doc["usedBytes"] = fsInfo.usedBytes;
    //     doc["freeBytes"] = fsInfo.totalBytes - fsInfo.usedBytes;
    //     doc["blockSize"] = fsInfo.blockSize;
    //     doc["pageSize"] = fsInfo.pageSize;
    //     doc["maxOpenFiles"] = fsInfo.maxOpenFiles;
    //     doc["maxPathLength"] = fsInfo.maxPathLength;
    // } else {
    //     doc["error"] = "Failed to get filesystem info";
    // }
    
    sendSuccess(request, doc);
}

void WebConfigManager::handleAPIHealthCheck(AsyncWebServerRequest* request) {
    StaticJsonDocument<128> doc;
    
    doc["status"] = "healthy";
    doc["timestamp"] = millis();
    doc["freeHeap"] = ESP.getFreeHeap();
    
    sendSuccess(request, doc);
}

// ============ 配置API处理器 ============

void WebConfigManager::handleAPIGetConfig(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.view")) {
        sendUnauthorized(request);
        return;
    }
    
    StaticJsonDocument<512> doc;
    
    // 从Preferences读取配置
    doc["webPort"] = preferences.getUInt("webPort", 80);
    doc["sessionTimeout"] = preferences.getUInt("sessionTimeout", 3600000);
    doc["maxLoginAttempts"] = preferences.getUInt("maxLoginAttempts", 5);
    doc["lockoutTime"] = preferences.getUInt("lockoutTime", 300000);
    
    sendSuccess(request, doc);
}

void WebConfigManager::handleAPIUpdateConfig(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    // 获取配置参数
    uint32_t webPort = getParamInt(request, "webPort", 0);
    uint32_t sessionTimeout = getParamInt(request, "sessionTimeout", 0);
    uint32_t maxLoginAttempts = getParamInt(request, "maxLoginAttempts", 0);
    uint32_t lockoutTime = getParamInt(request, "lockoutTime", 0);
    
    // 验证并保存配置
    bool updated = false;
    
    if (webPort >= 1 && webPort <= 65535) {
        preferences.putUInt("webPort", webPort);
        updated = true;
    }
    
    if (sessionTimeout >= 60000 && sessionTimeout <= 86400000) {
        preferences.putUInt("sessionTimeout", sessionTimeout);
        updated = true;
    }
    
    if (maxLoginAttempts >= 1 && maxLoginAttempts <= 10) {
        preferences.putUInt("maxLoginAttempts", maxLoginAttempts);
        updated = true;
    }
    
    if (lockoutTime >= 60000 && lockoutTime <= 3600000) {
        preferences.putUInt("lockoutTime", lockoutTime);
        updated = true;
    }
    
    if (updated) {
        sendSuccess(request, "Configuration updated successfully");
    } else {
        sendError(request, 400, "No valid configuration parameters provided");
    }
}

void WebConfigManager::handleAPIGetNetworkConfig(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "network.view")) {
        sendUnauthorized(request);
        return;
    }
    
    StaticJsonDocument<512> doc;
    
    if (networkManager) {
        // 修正：使用正确的NetworkManager方法
        // doc["ssid"] = networkManager->getSSID();
        // doc["ip"] = networkManager->getIP().toString();
        // doc["mac"] = networkManager->getMAC();
        // doc["rssi"] = networkManager->getRSSI();
        // doc["connected"] = networkManager->isConnected();
    }
    
    sendSuccess(request, doc);
}

void WebConfigManager::handleAPIUpdateNetworkConfig(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "network.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    if (!networkManager) {
        sendError(request, 500, "Network service unavailable");
        return;
    }
    
    // 简化：只更新WiFi配置
    String ssid = getParamValue(request, "ssid", "");
    String password = getParamValue(request, "password", "");
    
    if (ssid.isEmpty()) {
        sendBadRequest(request, "SSID is required");
        return;
    }
    
    // 这里应该调用NetworkManager的配置方法
    // 由于具体方法未知，我们暂时注释掉
    sendError(request, 501, "Not implemented");
    
    // if (networkManager->updateWiFiConfig(ssid, password)) {
    //     sendSuccess(request, "Network configuration updated. Restart required.");
    // } else {
    //     sendError(request, 400, "Failed to update network configuration");
    // }
}

// ============ OTA API处理器 ============

void WebConfigManager::handleAPIOtaUpdate(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "ota.update")) {
        sendUnauthorized(request);
        return;
    }
    
    // OTA更新处理在回调函数中完成
    sendSuccess(request, "OTA update started");
}

void WebConfigManager::handleAPIOtaStatus(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "ota.view")) {
        sendUnauthorized(request);
        return;
    }
    
    StaticJsonDocument<128> doc;
    
    if (otaManager) {
        doc["status"] = otaManager->getOTAStatus();
        doc["progress"] = otaManager->getProgress();
        // doc["lastError"] = otaManager->getError();
    } else {
        doc["status"] = "unavailable";
    }
    
    sendSuccess(request, doc);
}

// ============ 额外的工具方法 ============

void WebConfigManager::handleResetPassword(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.admin")) {
        sendUnauthorized(request);
        return;
    }
    
    // 从参数获取用户名和新密码
    String username = getParamValue(request, "username", "");
    String newPassword = getParamValue(request, "newPassword", "");
    
    if (username.isEmpty() || newPassword.isEmpty()) {
        sendBadRequest(request, "Username and new password are required");
        return;
    }
    
    if (!userManager) {
        sendError(request, 500, "User service unavailable");
        return;
    }
    
    // 管理员可以重置任何用户的密码 - 使用resetPassword方法
    if (userManager->resetPassword(username, newPassword)) {
        // 强制登出该用户的所有会话
        if (authManager) {
            authManager->forceLogout(username);
        }
        sendSuccess(request, "Password reset successfully");
    } else {
        sendError(request, 400, "Failed to reset password");
    }
}

void WebConfigManager::handleUnlockAccount(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.admin")) {
        sendUnauthorized(request);
        return;
    }
    
    // 从参数获取用户名
    String username = getParamValue(request, "username", "");
    
    if (username.isEmpty()) {
        sendBadRequest(request, "Username is required");
        return;
    }
    
    if (!userManager) {
        sendError(request, 500, "User service unavailable");
        return;
    }
    
    // 解锁账户
    userManager->unlockAccount(username);
    sendSuccess(request, "Account unlocked successfully");
}