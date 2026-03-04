#include "./network/WebConfigManager.h"
#include "./security/AuthManager.h"
#include "./security/UserManager.h"
#include "./security/RoleManager.h"
#include "./network/NetworkManager.h"
#include "./network/OTAManager.h"
#include "./protocols/ProtocolManager.h"
#include "systems/LoggerSystem.h"

WebConfigManager::WebConfigManager(AsyncWebServer* webServer,
                                   IAuthManager* authMgr, IUserManager* userMgr)
    : server(webServer), authManager(authMgr), userManager(userMgr),
      roleManager(nullptr), networkManager(nullptr), otaManager(nullptr),
      protocolManager(nullptr), isRunning(false) {
    preferences.begin("web_config", false);
    webRootPath = "/www";
}

WebConfigManager::~WebConfigManager() {
    stop();
    preferences.end();
}

bool WebConfigManager::initialize() {
    if (!server) {
        LOG_ERROR("WebConfig: Web server is null");
        return false;
    }

    if (!authManager || !userManager) {
        LOG_ERROR("WebConfig: AuthManager or UserManager is null");
        return false;
    }

    loadConfiguration();

    // LittleFS 由 ConfigStorage 统一挂载，此处仅检测挂载状态
    if (LittleFS.totalBytes() == 0) {
        LOG_WARNING("WebConfig: LittleFS not mounted, static files unavailable");
        // 不作为致命错误，API 仍可用
    }

    setupStaticRoutes();
    setupAuthRoutes();
    setupUserRoutes();
    setupRoleRoutes();
    setupSystemRoutes();
    setupAPIRoutes();

    start();

    LOG_INFO("WebConfig: Routes configured and server started");
    return true;
}

bool WebConfigManager::start() {
    if (!server) return false;
    server->begin();
    isRunning = true;
    LOG_INFO("WebConfig: Web server started on port 80");
    return true;
}

void WebConfigManager::stop() {
    if (isRunning) {
        // AsyncWebServer 无 stop() 方法，仅标记状态
        isRunning = false;
        LOG_INFO("WebConfig: Web server stopped");
    }
}

bool WebConfigManager::isServerRunning() const { return isRunning; }

void WebConfigManager::setRoleManager(RoleManager* roleMgr) { roleManager = roleMgr; }
void WebConfigManager::setNetworkManager (NetworkManager*  netMgr)   { networkManager  = netMgr;   }
void WebConfigManager::setOTAManager     (OTAManager*      otaMgr)   { otaManager      = otaMgr;   }
void WebConfigManager::setProtocolManager(ProtocolManager* protoMgr) { protocolManager = protoMgr; }

AsyncWebServer* WebConfigManager::getWebServer() const { return server; }

void WebConfigManager::performMaintenance() {
    // 定期清理任务预留位置
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
    
    // WiFi配置页面（AP模式下无需登录）
    server->on("/setup", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleSetupPage(request);
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
    
    // 更新用户 - 使用 POST 方法（避免正则路由问题）
    server->on("/api/users/update", HTTP_POST, 
              [this](AsyncWebServerRequest* request) {
        handleAPIUpdateUserByPost(request);
    });
    
    // 删除用户 - 使用 POST 方法
    server->on("/api/users/delete", HTTP_POST, 
              [this](AsyncWebServerRequest* request) {
        handleAPIDeleteUserByPost(request);
    });
    
    // 更新用户 - RESTful 备用
    server->on("^\\/api\\/users\\/([^\\/]+)$", HTTP_PUT, 
              [this](AsyncWebServerRequest* request) {
        handleAPIUpdateUser(request);
    });
    
    // 删除用户 - RESTful 备用
    server->on("^\\/api\\/users\\/([^\\/]+)$", HTTP_DELETE, 
              [this](AsyncWebServerRequest* request) {
        handleAPIDeleteUser(request);
    });
    
    // 获取在线用户
    server->on("/api/users/online", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetOnlineUsers(request);
    });

    // 为用户赋予角色
    server->on("^\\/api\\/users\\/([^\\/]+)\\/roles$", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIAssignUserRole(request);
    });

    // 撤销用户角色
    server->on("^\\/api\\/users\\/([^\\/]+)\\/roles\\/([^\\/]+)$", HTTP_DELETE,
              [this](AsyncWebServerRequest* request) {
        handleAPIRevokeUserRole(request);
    });

    // 更新用户元数据
    server->on("^\\/api\\/users\\/([^\\/]+)\\/meta$", HTTP_PUT,
              [this](AsyncWebServerRequest* request) {
        handleAPIUpdateUserMeta(request);
    });
    
    // ============ 角色管理 API ============
    
    // 获取所有角色和权限定义
    server->on("/api/roles", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetRoles(request);
    });
}

void WebConfigManager::setupRoleRoutes() {
    // 获取所有角色
    server->on("/api/roles", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetRoles(request);
    });

    // 创建角色
    server->on("/api/roles", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPICreateRole(request);
    });

    // 获取单个角色
    server->on("^\\/api\\/roles\\/([^\\/]+)$", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetRole(request);
    });

    // 更新角色基本信息
    server->on("^\\/api\\/roles\\/([^\\/]+)$", HTTP_PUT,
              [this](AsyncWebServerRequest* request) {
        handleAPIUpdateRole(request);
    });

    // 删除角色
    server->on("^\\/api\\/roles\\/([^\\/]+)$", HTTP_DELETE,
              [this](AsyncWebServerRequest* request) {
        handleAPIDeleteRole(request);
    });

    // 获取角色权限
    server->on("^\\/api\\/roles\\/([^\\/]+)\\/permissions$", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetRolePermissions(request);
    });

    // 替换角色权限集合
    server->on("^\\/api\\/roles\\/([^\\/]+)\\/permissions$", HTTP_PUT,
              [this](AsyncWebServerRequest* request) {
        handleAPISetRolePermissions(request);
    });

    // 获取所有权限定义
    server->on("/api/permissions", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetPermissions(request);
    });
    
    // ============ 角色管理 POST 路由（避免正则路由问题）============
    
    // 更新角色 - POST 方法
    server->on("/api/roles/update", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIUpdateRoleByPost(request);
    });
    
    // 删除角色 - POST 方法
    server->on("/api/roles/delete", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIDeleteRoleByPost(request);
    });
    
    // 设置角色权限 - POST 方法
    server->on("/api/roles/permissions", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPISetRolePermissionsByPost(request);
    });

    // 审计日志
    server->on("/api/audit/logs", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetAuditLog(request);
    });

    server->on("/api/audit/logs", HTTP_DELETE,
              [this](AsyncWebServerRequest* request) {
        handleAPIClearAuditLog(request);
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
    
    // ============ 日志管理 API ============
    
    // 获取日志内容
    server->on("/api/logs", HTTP_GET, 
              [this](AsyncWebServerRequest* request) {
        handleAPIGetLogs(request);
    });
    
    // 获取日志信息
    server->on("/api/logs/info", HTTP_GET, 
              [this](AsyncWebServerRequest* request) {
        handleAPIGetLogInfo(request);
    });
    
    // 清空日志
    server->on("/api/logs/clear", HTTP_POST, 
              [this](AsyncWebServerRequest* request) {
        handleAPIClearLogs(request);
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
    
    // WiFi扫描（AP模式下无需登录）
    server->on("/api/wifi/scan", HTTP_GET, 
              [this](AsyncWebServerRequest* request) {
        handleAPIWiFiScan(request);
    });
    
    // WiFi连接（AP模式下无需登录）
    server->on("/api/wifi/connect", HTTP_POST, 
              [this](AsyncWebServerRequest* request) {
        handleAPIWiFiConnect(request);
    });
    
    // OTA 固件状态查询
    server->on("/api/ota/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIOtaStatus(request);
    });

    // OTA 固件上传（触发升级）
    server->on("/api/ota/update", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIOtaUpdate(request);
    });

    // 管理员工具：重置密码
    server->on("/api/users/reset-password", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleResetPassword(request);
    });

    // 管理员工具：解锁账户
    server->on("/api/users/unlock-account", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleUnlockAccount(request);
    });
}

// ============ 参数处理辅助方法 ============

String WebConfigManager::getParamValue(AsyncWebServerRequest* request, const String& paramName, 
                                      const String& defaultValue) {
    if (!request) return defaultValue;
    
    // 优先从 POST body（表单参数）取，再从 URL query string 取
    if (request->hasParam(paramName, true)) {
        const AsyncWebParameter* param = request->getParam(paramName, true);
        return param->value();
    }
    if (request->hasParam(paramName, false)) {
        const AsyncWebParameter* param = request->getParam(paramName, false);
        return param->value();
    }
    return defaultValue;
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
    return authManager->checkSessionPermission(authResult.sessionId, permission);
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
    JsonDocument doc;
    doc["success"] = true;
    doc["timestamp"] = millis();
    
    if (!data.isNull()) {
        doc["data"] = data;
    }
    
    sendJsonResponse(request, 200, doc);
}

void WebConfigManager::sendSuccess(AsyncWebServerRequest* request, const String& message) {
    JsonDocument doc;
    doc["success"] = true;
    if (!message.isEmpty()) {
        doc["message"] = message;
    }
    doc["timestamp"] = millis();
    
    sendJsonResponse(request, 200, doc);
}

void WebConfigManager::sendError(AsyncWebServerRequest* request, int code, const String& message) {
    JsonDocument doc;
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

void WebConfigManager::sendBuiltinLoginPage(AsyncWebServerRequest* request) {
    // 内置的简单登录页面，当 LittleFS 文件不存在时使用
    const char* html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>FastBee Login</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { font-family: Arial, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; display: flex; align-items: center; justify-content: center; }
        .login-box { background: white; padding: 40px; border-radius: 10px; box-shadow: 0 15px 35px rgba(0,0,0,0.2); width: 100%; max-width: 400px; }
        h1 { text-align: center; color: #333; margin-bottom: 30px; font-size: 24px; }
        .form-group { margin-bottom: 20px; }
        label { display: block; margin-bottom: 8px; color: #555; font-weight: bold; }
        input[type="text"], input[type="password"] { width: 100%; padding: 12px; border: 2px solid #ddd; border-radius: 5px; font-size: 16px; transition: border-color 0.3s; }
        input:focus { outline: none; border-color: #667eea; }
        button { width: 100%; padding: 14px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; border: none; border-radius: 5px; font-size: 16px; cursor: pointer; transition: transform 0.2s, box-shadow 0.2s; }
        button:hover { transform: translateY(-2px); box-shadow: 0 5px 15px rgba(102, 126, 234, 0.4); }
        .message { padding: 10px; border-radius: 5px; margin-bottom: 20px; text-align: center; display: none; }
        .error { background: #ffe0e0; color: #c00; display: block; }
        .success { background: #e0ffe0; color: #0a0; display: block; }
        .info { text-align: center; margin-top: 20px; color: #888; font-size: 14px; }
    </style>
</head>
<body>
    <div class="login-box">
        <h1>FastBee IoT Platform</h1>
        <div id="message" class="message"></div>
        <form id="loginForm">
            <div class="form-group">
                <label for="username">Username</label>
                <input type="text" id="username" name="username" required placeholder="Enter username">
            </div>
            <div class="form-group">
                <label for="password">Password</label>
                <input type="password" id="password" name="password" required placeholder="Enter password">
            </div>
            <button type="submit">Login</button>
        </form>
        <div class="info">Default: admin / admin123</div>
    </div>
    <script>
        document.getElementById('loginForm').addEventListener('submit', async function(e) {
            e.preventDefault();
            const msg = document.getElementById('message');
            msg.className = 'message';
            msg.style.display = 'none';
            const formData = new FormData(this);
            try {
                const resp = await fetch('/api/auth/login', { method: 'POST', body: formData });
                const data = await resp.json();
                if (data.success) {
                    msg.textContent = 'Login successful! Redirecting...';
                    msg.className = 'message success';
                    setTimeout(() => window.location.href = '/dashboard', 1000);
                } else {
                    msg.textContent = data.message || 'Login failed';
                    msg.className = 'message error';
                }
            } catch (err) {
                msg.textContent = 'Connection error: ' + err.message;
                msg.className = 'message error';
            }
        });
    </script>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", html);
}

void WebConfigManager::sendBuiltinDashboard(AsyncWebServerRequest* request) {
    // 内置控制台页面
    const char* html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>FastBee Dashboard</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { font-family: Arial, sans-serif; background: #f5f6fa; min-height: 100vh; }
        .header { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; padding: 20px; display: flex; justify-content: space-between; align-items: center; }
        .header h1 { font-size: 24px; }
        .logout-btn { background: rgba(255,255,255,0.2); border: none; color: white; padding: 10px 20px; border-radius: 5px; cursor: pointer; }
        .logout-btn:hover { background: rgba(255,255,255,0.3); }
        .container { max-width: 1200px; margin: 0 auto; padding: 20px; }
        .cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 20px; margin-top: 20px; }
        .card { background: white; border-radius: 10px; padding: 25px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        .card h3 { color: #333; margin-bottom: 15px; display: flex; align-items: center; gap: 10px; }
        .card-icon { width: 40px; height: 40px; border-radius: 10px; display: flex; align-items: center; justify-content: center; font-size: 20px; }
        .icon-blue { background: #e3f2fd; color: #1976d2; }
        .icon-green { background: #e8f5e9; color: #388e3c; }
        .icon-orange { background: #fff3e0; color: #f57c00; }
        .icon-purple { background: #f3e5f5; color: #7b1fa2; }
        .stat { font-size: 32px; font-weight: bold; color: #333; }
        .stat-label { color: #888; font-size: 14px; margin-top: 5px; }
        .info-row { display: flex; justify-content: space-between; padding: 10px 0; border-bottom: 1px solid #eee; }
        .info-row:last-child { border-bottom: none; }
        .info-label { color: #888; }
        .info-value { color: #333; font-weight: 500; }
        .status-ok { color: #388e3c; }
        .status-warn { color: #f57c00; }
    </style>
</head>
<body>
    <div class="header">
        <h1>FastBee IoT Dashboard</h1>
        <button class="logout-btn" onclick="logout()">Logout</button>
    </div>
    <div class="container">
        <div class="cards">
            <div class="card">
                <h3><span class="card-icon icon-blue">&#128268;</span> System Status</h3>
                <div id="systemStatus">Loading...</div>
            </div>
            <div class="card">
                <h3><span class="card-icon icon-green">&#128225;</span> Network</h3>
                <div id="networkInfo">Loading...</div>
            </div>
            <div class="card">
                <h3><span class="card-icon icon-orange">&#128190;</span> Memory</h3>
                <div id="memoryInfo">Loading...</div>
            </div>
            <div class="card">
                <h3><span class="card-icon icon-purple">&#128279;</span> Protocols</h3>
                <div id="protocolInfo">Loading...</div>
            </div>
        </div>
    </div>
    <script>
        async function loadDashboard() {
            try {
                const resp = await fetch('/api/system/status');
                const data = await resp.json();
                if (data.success) {
                    const s = data.data || data;
                    document.getElementById('systemStatus').innerHTML = `
                        <div class="info-row"><span class="info-label">Uptime</span><span class="info-value">${formatUptime(s.uptime || 0)}</span></div>
                        <div class="info-row"><span class="info-label">CPU Freq</span><span class="info-value">${s.cpuFreq || 240} MHz</span></div>
                        <div class="info-row"><span class="info-label">Temperature</span><span class="info-value">${s.temperature || 'N/A'}</span></div>
                    `;
                    document.getElementById('networkInfo').innerHTML = `
                        <div class="info-row"><span class="info-label">Status</span><span class="info-value status-ok">${s.network?.status || 'Connected'}</span></div>
                        <div class="info-row"><span class="info-label">IP Address</span><span class="info-value">${s.network?.ip || 'N/A'}</span></div>
                        <div class="info-row"><span class="info-label">RSSI</span><span class="info-value">${s.network?.rssi || 'N/A'} dBm</span></div>
                    `;
                    document.getElementById('memoryInfo').innerHTML = `
                        <div class="info-row"><span class="info-label">Free Heap</span><span class="info-value">${formatBytes(s.freeHeap || 0)}</span></div>
                        <div class="info-row"><span class="info-label">Min Free</span><span class="info-value">${formatBytes(s.minFreeHeap || 0)}</span></div>
                        <div class="info-row"><span class="info-label">PSRAM</span><span class="info-value">${s.psramSize ? formatBytes(s.psramSize) : 'N/A'}</span></div>
                    `;
                    document.getElementById('protocolInfo').innerHTML = `
                        <div class="info-row"><span class="info-label">MQTT</span><span class="info-value">${s.protocols?.mqtt || 'Disabled'}</span></div>
                        <div class="info-row"><span class="info-label">HTTP</span><span class="info-value status-ok">Active</span></div>
                        <div class="info-row"><span class="info-label">Modbus</span><span class="info-value">${s.protocols?.modbus || 'Disabled'}</span></div>
                    `;
                }
            } catch (e) {
                console.error('Failed to load dashboard:', e);
            }
        }
        function formatUptime(seconds) {
            const d = Math.floor(seconds / 86400);
            const h = Math.floor((seconds % 86400) / 3600);
            const m = Math.floor((seconds % 3600) / 60);
            return d > 0 ? `${d}d ${h}h ${m}m` : h > 0 ? `${h}h ${m}m` : `${m}m`;
        }
        function formatBytes(bytes) {
            if (bytes < 1024) return bytes + ' B';
            if (bytes < 1048576) return (bytes / 1024).toFixed(1) + ' KB';
            return (bytes / 1048576).toFixed(1) + ' MB';
        }
        async function logout() {
            await fetch('/api/auth/logout', { method: 'POST' });
            window.location.href = '/login';
        }
        loadDashboard();
        setInterval(loadDashboard, 5000);
    </script>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", html);
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
    if (!serveStaticFile(request, webRootPath + "/index.html")) {
        // LittleFS 文件不存在，返回内置登录页面
        sendBuiltinLoginPage(request);
    }
}

void WebConfigManager::handleDashboard(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        // 未登录，重定向到登录页
        request->redirect("/login");
        return;
    }
    if (!serveStaticFile(request, webRootPath + "/index.html")) {
        sendBuiltinDashboard(request);
    }
}

void WebConfigManager::handleUsersPage(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.view")) {
        request->redirect("/login");
        return;
    }
    if (!serveStaticFile(request, webRootPath + "/index.html")) {
        sendBuiltinUsersPage(request);
    }
}

void WebConfigManager::handleSettingsPage(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.view")) {
        request->redirect("/login");
        return;
    }
    if (!serveStaticFile(request, webRootPath + "/index.html")) {
        sendBuiltinDashboard(request);
    }
}

void WebConfigManager::handleMonitorPage(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        request->redirect("/login");
        return;
    }
    if (!serveStaticFile(request, webRootPath + "/index.html")) {
        sendBuiltinDashboard(request);
    }
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
        JsonDocument responseDoc;
        responseDoc["success"] = true;
        responseDoc["sessionId"] = result.sessionId;
        responseDoc["username"] = result.username;
        // 简化：不返回角色，前端可以通过其他接口获取

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
        sendError(request, 401, result.errorMessage);
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
        JsonDocument responseDoc;
        responseDoc["username"] = authResult.username;
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
    
    // 可选的分页和过滤参数（同时支持 GET query 和 POST body）
    int page = getParamInt(request, "page", 1);
    int limit = getParamInt(request, "limit", 50);
    String filter = getParamValue(request, "filter", "");
    
    // 参数范围校验
    if (page < 1) page = 1;
    if (limit < 1 || limit > 200) limit = 50;
    
    JsonDocument doc;
    JsonArray usersArray = doc["users"].to<JsonArray>();
    
    // 获取用户列表JSON字符串
    String usersJson = userManager->getAllUsers();
    JsonDocument usersDoc;
    DeserializationError error = deserializeJson(usersDoc, usersJson);
    if (error) {
        sendError(request, 500, "Failed to parse users data");
        return;
    }
    
    // 注意：getAllUsers() 返回 {"users": [...]}
    JsonArray allUsers = usersDoc["users"].as<JsonArray>();
    int totalCount = allUsers.size();
    int startIndex = (page - 1) * limit;
    int count = 0;
    
    for (int i = 0; i < totalCount; i++) {
        JsonObject userObj = allUsers[i];
        String username = userObj["username"].as<String>();
        
        // 应用过滤
        if (!filter.isEmpty() && username.indexOf(filter) == -1) {
            continue;
        }
        
        // 分页裁剪
        if (i < startIndex) continue;
        if (count >= limit) break;
        
        JsonObject newUserObj = usersArray.add<JsonObject>();
        newUserObj["username"] = username;
        newUserObj["role"] = userObj["role"];
        newUserObj["enabled"] = userObj["enabled"];
        newUserObj["createTime"] = userObj["createTime"];
        newUserObj["lastLogin"] = userObj["lastLogin"];
        newUserObj["lastModified"] = userObj["lastModified"];
        
        // 检查是否在线
        bool isOnline = authManager ? authManager->isUserOnline(username) : false;
        newUserObj["isOnline"] = isOnline;
        
        // 检查是否锁定
        newUserObj["isLocked"] = userManager->isAccountLocked(username);
        newUserObj["loginAttempts"] = userManager->getLoginAttempts(username);
        
        count++;
    }
    
    doc["page"] = page;
    doc["limit"] = limit;
    doc["total"] = totalCount;
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
    
    JsonDocument doc;
    doc["username"] = user->username;
    doc["role"] = UserManager::roleToString(user->role);
    doc["enabled"] = user->enabled;
    doc["createTime"] = user->createTime;
    doc["lastLogin"] = user->lastLogin;
    doc["lastModified"] = user->lastModified;
    
    // 检查是否在线
    bool isOnline = authManager ? authManager->isUserOnline(username) : false;
    doc["isOnline"] = isOnline;
    
    // 检查是否锁定
    doc["isLocked"] = userManager->isAccountLocked(username);
    doc["loginAttempts"] = userManager->getLoginAttempts(username);
    
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
    String roleStr  = getParamValue(request, "role", "user");
    
    // 参数完整性校验
    if (username.isEmpty()) {
        sendBadRequest(request, "Username is required");
        return;
    }
    if (password.isEmpty()) {
        sendBadRequest(request, "Password is required");
        return;
    }
    // 用户名长度限制
    if (username.length() < 3 || username.length() > 32) {
        sendBadRequest(request, "Username must be 3-32 characters");
        return;
    }
    // 密码强度：最少6位
    if (password.length() < 6) {
        sendBadRequest(request, "Password must be at least 6 characters");
        return;
    }
    
    UserRole role = UserManager::stringToRole(roleStr);
    
    if (!userManager) {
        sendError(request, 500, "User service unavailable");
        return;
    }
    
    if (userManager->addUser(username, password, UserManager::roleToString(role))) {
        sendSuccess(request, "User added successfully");
    } else {
        sendError(request, 400, "Failed to add user (username may already exist)");
    }
}

void WebConfigManager::handleAPIUpdateUser(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    // 从POST参数或URL路径提取用户名
    String username = getParamValue(request, "username", "");
    if (username.isEmpty()) {
        // 从URL路径提取
        String path = request->url();
        int lastSlash = path.lastIndexOf('/');
        if (lastSlash != -1) {
            username = path.substring(lastSlash + 1);
        }
    }
    
    if (username.isEmpty()) {
        sendError(request, 400, "Username required");
        return;
    }
    
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
    
    // 执行更新
    bool success = userManager->updateUser(username, newPassword, newRole, enabled);
    
    if (success) {
        sendSuccess(request, "User updated successfully");
    } else {
        sendError(request, 400, "Failed to update user");
    }
}

void WebConfigManager::handleAPIUpdateUserByPost(AsyncWebServerRequest* request) {
    // 复用 handleAPIUpdateUser，它已经支持从POST参数获取username
    handleAPIUpdateUser(request);
}

void WebConfigManager::handleAPIDeleteUserByPost(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.delete")) {
        sendUnauthorized(request);
        return;
    }
    
    // 从POST参数获取用户名
    String username = getParamValue(request, "username", "");
    if (username.isEmpty()) {
        sendError(request, 400, "Username required");
        return;
    }
    
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
    
    // 不能删除admin
    if (username == "admin") {
        sendError(request, 400, "Cannot delete admin account");
        return;
    }
    
    if (!userManager) {
        sendError(request, 500, "User service unavailable");
        return;
    }
    
    if (userManager->deleteUser(username)) {
        sendSuccess(request, "User deleted successfully");
    } else {
        sendError(request, 400, "Failed to delete user");
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
    
    JsonDocument doc;
    JsonArray onlineUsersArray = doc["onlineUsers"].to<JsonArray>();
    
    for (const UserSession& session : sessions) {
        JsonObject userObj = onlineUsersArray.add<JsonObject>();
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

// ============ 角色管理 API 处理器 ============

void WebConfigManager::handleAPIGetRoles(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "role.view")) {
        sendUnauthorized(request);
        return;
    }
    
    if (!roleManager) {
        sendError(request, 500, "Role manager unavailable");
        return;
    }
    
    JsonDocument doc;
    
    // 获取所有角色
    JsonArray rolesArray = doc["roles"].to<JsonArray>();
    std::vector<Role> roles = roleManager->getAllRoles();
    for (const Role& role : roles) {
        JsonObject roleObj = rolesArray.add<JsonObject>();
        roleObj["id"] = role.id;
        roleObj["name"] = role.name;
        roleObj["description"] = role.description;
        roleObj["isBuiltin"] = role.isBuiltin;
        
        JsonArray permsArray = roleObj["permissions"].to<JsonArray>();
        for (const String& perm : role.permissions) {
            permsArray.add(perm);
        }
    }
    
    // 获取所有权限定义
    JsonArray permsDefArray = doc["permissions"].to<JsonArray>();
    std::vector<PermissionDef> permDefs = roleManager->getAllPermissions();
    for (const PermissionDef& perm : permDefs) {
        JsonObject permObj = permsDefArray.add<JsonObject>();
        permObj["id"] = perm.id;
        permObj["name"] = perm.name;
        permObj["description"] = perm.description;
        permObj["group"] = perm.group;
    }
    
    sendSuccess(request, doc);
}

// ============ 系统API处理器 ============

void WebConfigManager::handleAPISystemInfo(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }
    
    JsonDocument doc;
    
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
    JsonDocument doc;

    doc["status"]    = "running";
    doc["timestamp"] = millis();
    doc["freeHeap"]  = ESP.getFreeHeap();
    doc["uptime"]    = millis();

    if (networkManager) {
        NetworkStatusInfo info = networkManager->getStatusInfo();
        doc["networkConnected"] = (info.status == NetworkStatus::CONNECTED);
        doc["ipAddress"]        = info.ipAddress;
        doc["ssid"]             = info.ssid;
        doc["rssi"]             = info.rssi;
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
    
    JsonDocument doc;
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

    JsonDocument doc;

    // LittleFS 空间信息（ESP32 ArduinoFS API）
    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    doc["totalBytes"] = total;
    doc["usedBytes"]  = used;
    doc["freeBytes"]  = total > used ? total - used : 0;
    doc["usedPercent"]= total > 0 ? (used * 100 / total) : 0;

    sendSuccess(request, doc);
}

void WebConfigManager::handleAPIHealthCheck(AsyncWebServerRequest* request) {
    JsonDocument doc;
    
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
    
    JsonDocument doc;
    
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

    JsonDocument doc;

    if (networkManager) {
        // 通过 NetworkManager 的具体子类访问配置
        NetworkManager* netMgr = static_cast<NetworkManager*>(networkManager);
        WiFiConfig cfg = netMgr->getConfig();
        doc["mode"]         = static_cast<uint8_t>(cfg.mode);
        doc["staSSID"]      = cfg.staSSID;
        doc["apSSID"]       = cfg.apSSID;
        doc["ipConfigType"] = static_cast<uint8_t>(cfg.ipConfigType);
        doc["staticIP"]     = cfg.staticIP;
        doc["gateway"]      = cfg.gateway;
        doc["subnet"]       = cfg.subnet;
        doc["enableMDNS"]   = cfg.enableMDNS;
        doc["customDomain"] = cfg.customDomain;

        // 当前连接状态
        NetworkStatusInfo info = netMgr->getStatusInfo();
        doc["connected"]    = (info.status == NetworkStatus::CONNECTED);
        doc["ipAddress"]    = info.ipAddress;
        doc["macAddress"]   = info.macAddress;
        doc["rssi"]         = info.rssi;
    } else {
        doc["error"] = "Network service unavailable";
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

    // 读取请求参数，构造新配置
    String ssid     = getParamValue(request, "ssid", "");
    String password = getParamValue(request, "password", "");

    if (ssid.isEmpty()) {
        sendBadRequest(request, "SSID is required");
        return;
    }

    NetworkManager* netMgr = static_cast<NetworkManager*>(networkManager);
    if (netMgr->connectToNetwork(ssid, password)) {
        sendSuccess(request, "Network configuration updated. Reconnecting...");
    } else {
        sendError(request, 400, "Failed to update network configuration");
    }
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
    
    JsonDocument doc;
    
    if (otaManager) {
        doc["status"] = otaManager->getOTAStatus();
        doc["progress"] = otaManager->getProgress();
        // doc["lastError"] = otaManager->getError();
    } else {
        doc["status"] = "unavailable";
        doc["progress"] = 0;
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

// ============ 用户多角色 API 处理器 ============

void WebConfigManager::handleAPIAssignUserRole(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.edit")) {
        sendUnauthorized(request);
        return;
    }

    String path = request->url();  // /api/users/{username}/roles
    // 提取 username：路径第三段
    String username;
    int p1 = path.indexOf('/', 5);   // 跳过 /api/
    int p2 = path.indexOf('/', p1 + 1);
    if (p1 != -1 && p2 != -1) {
        username = path.substring(p1 + 1, p2);
    }

    String roleId = getParamValue(request, "roleId", "");
    if (username.isEmpty() || roleId.isEmpty()) {
        sendBadRequest(request, "username and roleId are required");
        return;
    }

    UserManager* um = static_cast<UserManager*>(userManager);
    if (!um->assignRole(username, roleId)) {
        sendError(request, 400, "Failed to assign role");
        return;
    }

    // 审计
    AuthResult ar = authenticateRequest(request);
    if (authManager) {
        static_cast<AuthManager*>(authManager)->recordAudit(
            ar.username, "user.assign_role", username,
            "Assigned role: " + roleId, true, getClientIP(request));
    }
    sendSuccess(request, "Role assigned");
}

void WebConfigManager::handleAPIRevokeUserRole(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.edit")) {
        sendUnauthorized(request);
        return;
    }

    String path = request->url();   // /api/users/{username}/roles/{roleId}
    // 提取 username 和 roleId
    String username, roleId;
    int p1 = path.indexOf('/', 5);
    int p2 = (p1 != -1) ? path.indexOf('/', p1 + 1) : -1;
    int p3 = (p2 != -1) ? path.indexOf('/', p2 + 1) : -1;
    int p4 = (p3 != -1) ? path.indexOf('/', p3 + 1) : -1;
    if (p1 != -1 && p2 != -1) username = path.substring(p1 + 1, p2);
    if (p3 != -1) {
        int end = (p4 != -1) ? p4 : path.length();
        roleId = path.substring(p3 + 1, end);
    }

    if (username.isEmpty() || roleId.isEmpty()) {
        sendBadRequest(request, "username and roleId required");
        return;
    }

    UserManager* um = static_cast<UserManager*>(userManager);
    if (!um->removeRole(username, roleId)) {
        sendError(request, 400, "Failed to revoke role");
        return;
    }

    AuthResult ar = authenticateRequest(request);
    if (authManager) {
        static_cast<AuthManager*>(authManager)->recordAudit(
            ar.username, "user.revoke_role", username,
            "Revoked role: " + roleId, true, getClientIP(request));
    }
    sendSuccess(request, "Role revoked");
}

void WebConfigManager::handleAPIUpdateUserMeta(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "user.edit")) {
        sendUnauthorized(request);
        return;
    }

    String path = request->url();
    int p1 = path.indexOf('/', 5);
    int p2 = (p1 != -1) ? path.indexOf('/', p1 + 1) : -1;
    String username = (p1 != -1 && p2 != -1) ? path.substring(p1 + 1, p2) : "";

    if (username.isEmpty()) {
        sendBadRequest(request, "username required");
        return;
    }

    String email  = getParamValue(request, "email", "");
    String remark = getParamValue(request, "remark", "");

    UserManager* um = static_cast<UserManager*>(userManager);
    if (!um->updateUserMeta(username, email, remark)) {
        sendError(request, 400, "Failed to update user meta");
        return;
    }
    sendSuccess(request, "User meta updated");
}

void WebConfigManager::handleAPIGetRole(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "role.view")) {
        sendUnauthorized(request);
        return;
    }

    String path = request->url();
    int p = path.lastIndexOf('/');
    String roleId = (p != -1) ? path.substring(p + 1) : "";

    if (!roleManager || !roleManager->roleExists(roleId)) {
        sendNotFound(request);
        return;
    }

    String json = roleManager->roleToJson(roleId);
    // 直接返回已序列化的 JSON
    AsyncWebServerResponse* resp = request->beginResponse(200, "application/json", json);
    resp->addHeader("Access-Control-Allow-Origin", "*");
    request->send(resp);
}

void WebConfigManager::handleAPICreateRole(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "role.create")) {
        sendUnauthorized(request);
        return;
    }

    if (!roleManager) {
        sendError(request, 500, "Role service unavailable");
        return;
    }

    String id   = getParamValue(request, "id", "");
    String name = getParamValue(request, "name", "");
    String desc = getParamValue(request, "description", "");

    if (id.isEmpty() || name.isEmpty()) {
        sendBadRequest(request, "id and name are required");
        return;
    }

    if (!roleManager->createRole(id, name, desc)) {
        sendError(request, 400, "Failed to create role (id may already exist)");
        return;
    }

    AuthResult ar = authenticateRequest(request);
    if (authManager) {
        static_cast<AuthManager*>(authManager)->recordAudit(
            ar.username, "role.create", id, "Created role: " + name,
            true, getClientIP(request));
    }
    sendSuccess(request, "Role created");
}

void WebConfigManager::handleAPIUpdateRole(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "role.edit")) {
        sendUnauthorized(request);
        return;
    }

    String path = request->url();
    int p = path.lastIndexOf('/');
    String roleId = (p != -1) ? path.substring(p + 1) : "";

    if (!roleManager || !roleManager->roleExists(roleId)) {
        sendNotFound(request);
        return;
    }

    String name = getParamValue(request, "name", "");
    String desc = getParamValue(request, "description", "");

    if (!roleManager->updateRole(roleId, name, desc)) {
        sendError(request, 400, "Failed to update role");
        return;
    }

    AuthResult ar = authenticateRequest(request);
    if (authManager) {
        static_cast<AuthManager*>(authManager)->recordAudit(
            ar.username, "role.edit", roleId, "Updated role info",
            true, getClientIP(request));
    }
    sendSuccess(request, "Role updated");
}

void WebConfigManager::handleAPIDeleteRole(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "role.delete")) {
        sendUnauthorized(request);
        return;
    }

    String path = request->url();
    int p = path.lastIndexOf('/');
    String roleId = (p != -1) ? path.substring(p + 1) : "";

    if (!roleManager) {
        sendError(request, 500, "Role service unavailable");
        return;
    }

    if (!roleManager->deleteRole(roleId)) {
        sendError(request, 400, "Failed to delete role (builtin roles cannot be deleted)");
        return;
    }

    AuthResult ar = authenticateRequest(request);
    if (authManager) {
        static_cast<AuthManager*>(authManager)->recordAudit(
            ar.username, "role.delete", roleId, "Deleted role",
            true, getClientIP(request));
    }
    sendSuccess(request, "Role deleted");
}

void WebConfigManager::handleAPIGetRolePermissions(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "role.view")) {
        sendUnauthorized(request);
        return;
    }

    // URL: /api/roles/{roleId}/permissions
    String path = request->url();
    // 去掉尾部的 /permissions
    String trimmed = path.substring(0, path.lastIndexOf('/'));
    int p = trimmed.lastIndexOf('/');
    String roleId = (p != -1) ? trimmed.substring(p + 1) : "";

    if (!roleManager || !roleManager->roleExists(roleId)) {
        sendNotFound(request);
        return;
    }

    JsonDocument doc;
    JsonArray arr = doc["permissions"].to<JsonArray>();
    for (const String& perm : roleManager->getRolePermissions(roleId)) {
        arr.add(perm);
    }
    sendSuccess(request, doc);
}

void WebConfigManager::handleAPISetRolePermissions(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "role.edit")) {
        sendUnauthorized(request);
        return;
    }

    String path = request->url();
    String trimmed = path.substring(0, path.lastIndexOf('/'));
    int p = trimmed.lastIndexOf('/');
    String roleId = (p != -1) ? trimmed.substring(p + 1) : "";

    if (!roleManager || !roleManager->roleExists(roleId)) {
        sendNotFound(request);
        return;
    }

    // 权限列表通过逗号分隔的 "permissions" 参数传递
    String permsParam = getParamValue(request, "permissions", "");
    std::vector<String> permList;
    int start = 0;
    while (start < (int)permsParam.length()) {
        int comma = permsParam.indexOf(',', start);
        if (comma == -1) {
            String item = permsParam.substring(start);
            item.trim();
            if (!item.isEmpty()) permList.push_back(item);
            break;
        }
        String item = permsParam.substring(start, comma);
        item.trim();
        if (!item.isEmpty()) permList.push_back(item);
        start = comma + 1;
    }

    if (!roleManager->setRolePermissions(roleId, permList)) {
        sendError(request, 400, "Failed to set permissions");
        return;
    }

    AuthResult ar = authenticateRequest(request);
    if (authManager) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Set %u permissions", (unsigned)permList.size());
        static_cast<AuthManager*>(authManager)->recordAudit(
            ar.username, "role.set_permissions", roleId, buf,
            true, getClientIP(request));
    }
    sendSuccess(request, "Permissions updated");
}

// ============ 角色管理 POST 方法处理器 ============

void WebConfigManager::handleAPIUpdateRoleByPost(AsyncWebServerRequest* request) {
    Serial.println("[WebConfig] handleAPIUpdateRoleByPost called");
    
    if (!checkPermission(request, "role.edit")) {
        Serial.println("[WebConfig] Permission denied for role.edit");
        sendUnauthorized(request);
        return;
    }

    String roleId = getParamValue(request, "id", "");
    Serial.println("[WebConfig] Update role id: " + roleId);
    
    if (roleId.isEmpty()) {
        sendBadRequest(request, "Role id is required");
        return;
    }

    if (!roleManager || !roleManager->roleExists(roleId)) {
        Serial.println("[WebConfig] Role not found: " + roleId);
        sendNotFound(request);
        return;
    }

    // 仅 admin 角色不可修改
    if (roleId == "admin") {
        sendError(request, 400, "Cannot modify admin role");
        return;
    }

    String name = getParamValue(request, "name", "");
    String desc = getParamValue(request, "description", "");
    Serial.println("[WebConfig] Update role name: " + name);

    if (!roleManager->updateRole(roleId, name, desc)) {
        Serial.println("[WebConfig] updateRole failed!");
        sendError(request, 400, "Failed to save role (storage error)");
        return;
    }

    Serial.println("[WebConfig] Role updated successfully");
    AuthResult ar = authenticateRequest(request);
    if (authManager) {
        static_cast<AuthManager*>(authManager)->recordAudit(
            ar.username, "role.edit", roleId, "Updated role info",
            true, getClientIP(request));
    }
    sendSuccess(request, "Role updated");
}

void WebConfigManager::handleAPIDeleteRoleByPost(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "role.delete")) {
        sendUnauthorized(request);
        return;
    }

    String roleId = getParamValue(request, "id", "");
    if (roleId.isEmpty()) {
        sendBadRequest(request, "Role id is required");
        return;
    }

    if (!roleManager) {
        sendError(request, 500, "Role service unavailable");
        return;
    }
    
    // 仅 admin 角色不可删除
    if (roleId == "admin") {
        sendError(request, 400, "Cannot delete admin role");
        return;
    }

    if (!roleManager->deleteRole(roleId)) {
        sendError(request, 400, "Failed to delete role");
        return;
    }

    AuthResult ar = authenticateRequest(request);
    if (authManager) {
        static_cast<AuthManager*>(authManager)->recordAudit(
            ar.username, "role.delete", roleId, "Deleted role",
            true, getClientIP(request));
    }
    sendSuccess(request, "Role deleted");
}

void WebConfigManager::handleAPISetRolePermissionsByPost(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "role.edit")) {
        sendUnauthorized(request);
        return;
    }

    String roleId = getParamValue(request, "id", "");
    if (roleId.isEmpty()) {
        sendBadRequest(request, "Role id is required");
        return;
    }

    if (!roleManager || !roleManager->roleExists(roleId)) {
        sendNotFound(request);
        return;
    }

    // 仅 admin 角色权限不可修改
    if (roleId == "admin") {
        sendError(request, 400, "Cannot modify admin role permissions");
        return;
    }

    // 权限列表通过逗号分隔的 "permissions" 参数传递
    String permsParam = getParamValue(request, "permissions", "");
    std::vector<String> permList;
    int start = 0;
    while (start < (int)permsParam.length()) {
        int comma = permsParam.indexOf(',', start);
        if (comma == -1) {
            String item = permsParam.substring(start);
            item.trim();
            if (!item.isEmpty()) permList.push_back(item);
            break;
        }
        String item = permsParam.substring(start, comma);
        item.trim();
        if (!item.isEmpty()) permList.push_back(item);
        start = comma + 1;
    }

    if (!roleManager->setRolePermissions(roleId, permList)) {
        sendError(request, 400, "Failed to set permissions");
        return;
    }

    AuthResult ar = authenticateRequest(request);
    if (authManager) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Set %u permissions", (unsigned)permList.size());
        static_cast<AuthManager*>(authManager)->recordAudit(
            ar.username, "role.set_permissions", roleId, buf,
            true, getClientIP(request));
    }
    sendSuccess(request, "Permissions updated");
}

void WebConfigManager::handleAPIGetPermissions(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "role.view")) {
        sendUnauthorized(request);
        return;
    }

    if (!roleManager) {
        sendError(request, 500, "Role service unavailable");
        return;
    }

    JsonDocument doc;
    // 按分组返回
    JsonObject groups = doc["groups"].to<JsonObject>();
    for (const auto& kv : roleManager->getPermissionsByGroup()) {
        JsonArray grpArr = groups[kv.first].to<JsonArray>();
        for (const PermissionDef& pd : kv.second) {
            JsonObject pObj = grpArr.add<JsonObject>();
            pObj["id"]          = pd.id;
            pObj["name"]        = pd.name;
            pObj["description"] = pd.description;
        }
    }
    doc["total"] = roleManager->getAllPermissions().size();
    sendSuccess(request, doc);
}

// ============ 审计日志 API 处理器 ============

void WebConfigManager::handleAPIGetAuditLog(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "audit.view")) {
        sendUnauthorized(request);
        return;
    }

    if (!authManager) {
        sendError(request, 500, "Auth service unavailable");
        return;
    }

    int limit = getParamInt(request, "limit", 50);
    if (limit <= 0 || limit > 100) limit = 50;

    String json = static_cast<AuthManager*>(authManager)->getAuditLogJson((size_t)limit);
    AsyncWebServerResponse* resp = request->beginResponse(200, "application/json", json);
    resp->addHeader("Access-Control-Allow-Origin", "*");
    request->send(resp);
}

void WebConfigManager::handleAPIClearAuditLog(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.restart")) {  // 需要高级权限
        sendUnauthorized(request);
        return;
    }

    if (!authManager) {
        sendError(request, 500, "Auth service unavailable");
        return;
    }

    AuthResult ar = authenticateRequest(request);
    static_cast<AuthManager*>(authManager)->clearAuditLog();
    static_cast<AuthManager*>(authManager)->recordAudit(
        ar.username, "audit.clear", "audit_log", "Audit log cleared",
        true, getClientIP(request));
    sendSuccess(request, "Audit log cleared");
}

// ============ WiFi配置页面和API ============

void WebConfigManager::handleSetupPage(AsyncWebServerRequest* request) {
    // WiFi配置页面，AP模式下无需登录
    sendBuiltinSetupPage(request);
}

void WebConfigManager::handleAPIWiFiScan(AsyncWebServerRequest* request) {
    // WiFi扫描，AP模式下无需登录
    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();
    
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n && i < 20; i++) {
        JsonObject net = networks.add<JsonObject>();
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
        net["encrypted"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    WiFi.scanDelete();
    
    doc["success"] = true;
    doc["count"] = n;
    
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

void WebConfigManager::handleAPIWiFiConnect(AsyncWebServerRequest* request) {
    String ssid = getParamValue(request, "ssid", "");
    String password = getParamValue(request, "password", "");
    
    if (ssid.isEmpty()) {
        sendBadRequest(request, "SSID is required");
        return;
    }
    
    // 先返回响应，然后尝试连接
    sendSuccess(request, "Connecting to WiFi...");
    
    // 延迟连接，让响应先发送
    delay(100);
    
    // 尝试连接
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    // 等待连接
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        retries++;
    }
    
    if (WiFi.status() == WL_CONNECTED && networkManager) {
        // 保存配置
        NetworkManager* netMgr = static_cast<NetworkManager*>(networkManager);
        netMgr->setWiFiCredentials(ssid, password);
        // 启动 mDNS
        MDNS.begin("fastbee");
        MDNS.addService("http", "tcp", 80);
    }
}

void WebConfigManager::sendBuiltinSetupPage(AsyncWebServerRequest* request) {
    const char* html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>FastBee WiFi Setup</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { font-family: Arial, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; display: flex; align-items: center; justify-content: center; padding: 20px; }
        .setup-box { background: white; padding: 30px; border-radius: 10px; box-shadow: 0 15px 35px rgba(0,0,0,0.2); width: 100%; max-width: 450px; }
        h1 { text-align: center; color: #333; margin-bottom: 10px; font-size: 22px; }
        .subtitle { text-align: center; color: #888; margin-bottom: 25px; font-size: 14px; }
        .form-group { margin-bottom: 20px; }
        label { display: block; margin-bottom: 8px; color: #555; font-weight: bold; }
        select, input[type="text"], input[type="password"] { width: 100%; padding: 12px; border: 2px solid #ddd; border-radius: 5px; font-size: 16px; }
        select:focus, input:focus { outline: none; border-color: #667eea; }
        button { width: 100%; padding: 14px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; border: none; border-radius: 5px; font-size: 16px; cursor: pointer; margin-top: 10px; }
        button:hover { opacity: 0.9; }
        button:disabled { background: #ccc; cursor: not-allowed; }
        .scan-btn { background: #4CAF50; margin-bottom: 15px; }
        .message { padding: 12px; border-radius: 5px; margin-bottom: 20px; text-align: center; }
        .error { background: #ffe0e0; color: #c00; }
        .success { background: #e0ffe0; color: #080; }
        .info { background: #e3f2fd; color: #1565c0; }
        .network-list { max-height: 200px; overflow-y: auto; border: 1px solid #ddd; border-radius: 5px; margin-bottom: 15px; }
        .network-item { padding: 12px; border-bottom: 1px solid #eee; cursor: pointer; display: flex; justify-content: space-between; align-items: center; }
        .network-item:hover { background: #f5f5f5; }
        .network-item:last-child { border-bottom: none; }
        .network-name { font-weight: 500; }
        .network-signal { color: #888; font-size: 14px; }
        .signal-strong { color: #4CAF50; }
        .signal-medium { color: #FF9800; }
        .signal-weak { color: #f44336; }
        .lock-icon { margin-left: 8px; }
        .current-status { background: #f5f5f5; padding: 15px; border-radius: 5px; margin-bottom: 20px; }
        .status-row { display: flex; justify-content: space-between; margin-bottom: 8px; }
        .status-row:last-child { margin-bottom: 0; }
        .status-label { color: #888; }
        .status-value { font-weight: 500; }
        .connected { color: #4CAF50; }
        .disconnected { color: #f44336; }
    </style>
</head>
<body>
    <div class="setup-box">
        <h1>FastBee WiFi Setup</h1>
        <p class="subtitle">Configure your device's WiFi connection</p>
        
        <div class="current-status">
            <div class="status-row">
                <span class="status-label">Status:</span>
                <span id="connStatus" class="status-value disconnected">Checking...</span>
            </div>
            <div class="status-row">
                <span class="status-label">IP Address:</span>
                <span id="ipAddr" class="status-value">-</span>
            </div>
        </div>
        
        <div id="message" class="message info" style="display:none;"></div>
        
        <button class="scan-btn" onclick="scanNetworks()" id="scanBtn">Scan WiFi Networks</button>
        
        <div id="networkList" class="network-list" style="display:none;"></div>
        
        <form id="wifiForm">
            <div class="form-group">
                <label>WiFi Name (SSID)</label>
                <input type="text" id="ssid" name="ssid" required placeholder="Enter or select WiFi name">
            </div>
            <div class="form-group">
                <label>Password</label>
                <input type="password" id="password" name="password" placeholder="Enter WiFi password">
            </div>
            <button type="submit" id="connectBtn">Connect</button>
        </form>
        
        <div style="text-align:center; margin-top:20px;">
            <a href="/login" style="color:#667eea;">Go to Login Page</a>
        </div>
    </div>
    <script>
        function showMessage(text, type) {
            const msg = document.getElementById('message');
            msg.textContent = text;
            msg.className = 'message ' + type;
            msg.style.display = 'block';
        }
        
        function getSignalClass(rssi) {
            if (rssi >= -50) return 'signal-strong';
            if (rssi >= -70) return 'signal-medium';
            return 'signal-weak';
        }
        
        function getSignalBars(rssi) {
            if (rssi >= -50) return '\u2582\u2584\u2586\u2588';
            if (rssi >= -70) return '\u2582\u2584\u2586';
            if (rssi >= -80) return '\u2582\u2584';
            return '\u2582';
        }
        
        async function scanNetworks() {
            const btn = document.getElementById('scanBtn');
            const list = document.getElementById('networkList');
            btn.disabled = true;
            btn.textContent = 'Scanning...';
            
            try {
                const resp = await fetch('/api/wifi/scan');
                const data = await resp.json();
                
                if (data.networks && data.networks.length > 0) {
                    list.innerHTML = data.networks.map(n => `
                        <div class="network-item" onclick="selectNetwork('${n.ssid}')">
                            <span class="network-name">${n.ssid}${n.encrypted ? '<span class="lock-icon">\uD83D\uDD12</span>' : ''}</span>
                            <span class="network-signal ${getSignalClass(n.rssi)}">${getSignalBars(n.rssi)} ${n.rssi}dBm</span>
                        </div>
                    `).join('');
                    list.style.display = 'block';
                } else {
                    showMessage('No networks found', 'info');
                }
            } catch (e) {
                showMessage('Scan failed: ' + e.message, 'error');
            }
            
            btn.disabled = false;
            btn.textContent = 'Scan WiFi Networks';
        }
        
        function selectNetwork(ssid) {
            document.getElementById('ssid').value = ssid;
            document.getElementById('password').focus();
        }
        
        document.getElementById('wifiForm').addEventListener('submit', async function(e) {
            e.preventDefault();
            const btn = document.getElementById('connectBtn');
            btn.disabled = true;
            btn.textContent = 'Connecting...';
            showMessage('Connecting to WiFi...', 'info');
            
            const formData = new FormData(this);
            try {
                await fetch('/api/wifi/connect', { method: 'POST', body: formData });
                showMessage('Connecting... Please wait 10 seconds then refresh.', 'success');
                setTimeout(checkStatus, 10000);
            } catch (e) {
                showMessage('Connection request sent', 'info');
                setTimeout(checkStatus, 10000);
            }
            
            btn.disabled = false;
            btn.textContent = 'Connect';
        });
        
        async function checkStatus() {
            try {
                const resp = await fetch('/api/network/config');
                const data = await resp.json();
                const status = document.getElementById('connStatus');
                const ip = document.getElementById('ipAddr');
                
                if (data.connected) {
                    status.textContent = 'Connected';
                    status.className = 'status-value connected';
                    ip.textContent = data.ipAddress || '-';
                    showMessage('Connected! You can now access via: http://' + data.ipAddress, 'success');
                } else {
                    status.textContent = 'Not Connected';
                    status.className = 'status-value disconnected';
                    ip.textContent = '-';
                }
            } catch (e) {
                console.error('Status check failed:', e);
            }
        }
        
        checkStatus();
    </script>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", html);
}

void WebConfigManager::sendBuiltinUsersPage(AsyncWebServerRequest* request) {
    const char* html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>FastBee - User Management</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { font-family: Arial, sans-serif; background: #f5f6fa; min-height: 100vh; }
        .header { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; padding: 15px 20px; display: flex; justify-content: space-between; align-items: center; }
        .header h1 { font-size: 20px; }
        .nav-links a { color: white; text-decoration: none; margin-left: 20px; opacity: 0.9; }
        .nav-links a:hover { opacity: 1; }
        .container { max-width: 1200px; margin: 0 auto; padding: 20px; }
        .card { background: white; border-radius: 8px; padding: 20px; box-shadow: 0 2px 8px rgba(0,0,0,0.1); margin-bottom: 20px; }
        .card-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }
        .card-title { font-size: 18px; color: #333; }
        .btn { padding: 8px 16px; border: none; border-radius: 5px; cursor: pointer; font-size: 14px; }
        .btn-primary { background: #667eea; color: white; }
        .btn-danger { background: #e74c3c; color: white; }
        .btn-sm { padding: 5px 10px; font-size: 12px; }
        .btn:hover { opacity: 0.9; }
        table { width: 100%; border-collapse: collapse; }
        th, td { padding: 12px; text-align: left; border-bottom: 1px solid #eee; }
        th { background: #f8f9fa; color: #666; font-weight: 600; }
        .status-badge { padding: 4px 8px; border-radius: 4px; font-size: 12px; }
        .status-active { background: #d4edda; color: #155724; }
        .status-inactive { background: #f8d7da; color: #721c24; }
        .status-online { background: #cce5ff; color: #004085; }
        .modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.5); z-index: 1000; }
        .modal.active { display: flex; align-items: center; justify-content: center; }
        .modal-content { background: white; padding: 25px; border-radius: 10px; width: 100%; max-width: 450px; }
        .modal-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }
        .modal-title { font-size: 18px; }
        .close-btn { background: none; border: none; font-size: 24px; cursor: pointer; color: #999; }
        .form-group { margin-bottom: 15px; }
        .form-group label { display: block; margin-bottom: 5px; color: #555; font-weight: 500; }
        .form-group input, .form-group select { width: 100%; padding: 10px; border: 1px solid #ddd; border-radius: 5px; font-size: 14px; }
        .form-group input:focus, .form-group select:focus { outline: none; border-color: #667eea; }
        .actions { display: flex; gap: 8px; }
        .empty-state { text-align: center; padding: 40px; color: #888; }
        .toast { position: fixed; bottom: 20px; right: 20px; padding: 12px 20px; border-radius: 5px; color: white; z-index: 2000; }
        .toast-success { background: #28a745; }
        .toast-error { background: #dc3545; }
    </style>
</head>
<body>
    <div class="header">
        <h1>User Management</h1>
        <div class="nav-links">
            <a href="/dashboard">Dashboard</a>
            <a href="/setup">WiFi Setup</a>
            <a href="#" onclick="logout()">Logout</a>
        </div>
    </div>
    <div class="container">
        <div class="card">
            <div class="card-header">
                <span class="card-title">Users</span>
                <button class="btn btn-primary" onclick="showAddModal()">Add User</button>
            </div>
            <table>
                <thead>
                    <tr>
                        <th>Username</th>
                        <th>Role</th>
                        <th>Status</th>
                        <th>Last Login</th>
                        <th>Actions</th>
                    </tr>
                </thead>
                <tbody id="userList">
                    <tr><td colspan="5" class="empty-state">Loading...</td></tr>
                </tbody>
            </table>
        </div>
    </div>

    <!-- Add/Edit User Modal -->
    <div id="userModal" class="modal">
        <div class="modal-content">
            <div class="modal-header">
                <span class="modal-title" id="modalTitle">Add User</span>
                <button class="close-btn" onclick="closeModal()">&times;</button>
            </div>
            <form id="userForm">
                <input type="hidden" id="editMode" value="add">
                <div class="form-group">
                    <label>Username</label>
                    <input type="text" id="username" name="username" required>
                </div>
                <div class="form-group">
                    <label>Password</label>
                    <input type="password" id="password" name="password">
                </div>
                <div class="form-group">
                    <label>Role</label>
                    <select id="role" name="role">
                        <option value="user">User</option>
                        <option value="operator">Operator</option>
                        <option value="admin">Admin</option>
                    </select>
                </div>
                <div class="form-group">
                    <label>Status</label>
                    <select id="enabled" name="enabled">
                        <option value="true">Active</option>
                        <option value="false">Inactive</option>
                    </select>
                </div>
                <button type="submit" class="btn btn-primary" style="width:100%">Save</button>
            </form>
        </div>
    </div>

    <script>
        let users = [];
        
        async function loadUsers() {
            try {
                const resp = await fetch('/api/users');
                const data = await resp.json();
                if (data.success && data.data) {
                    users = data.data.users || [];
                    renderUsers();
                }
            } catch (e) {
                showToast('Failed to load users', 'error');
            }
        }
        
        function renderUsers() {
            const tbody = document.getElementById('userList');
            if (users.length === 0) {
                tbody.innerHTML = '<tr><td colspan="5" class="empty-state">No users found</td></tr>';
                return;
            }
            tbody.innerHTML = users.map(u => `
                <tr>
                    <td>${u.username}</td>
                    <td>${u.role || 'user'}</td>
                    <td>
                        <span class="status-badge ${u.enabled ? 'status-active' : 'status-inactive'}">
                            ${u.enabled ? 'Active' : 'Inactive'}
                        </span>
                        ${u.isOnline ? '<span class="status-badge status-online">Online</span>' : ''}
                    </td>
                    <td>${u.lastLogin ? new Date(u.lastLogin).toLocaleString() : 'Never'}</td>
                    <td class="actions">
                        <button class="btn btn-primary btn-sm" onclick="editUser('${u.username}')">Edit</button>
                        ${u.username !== 'admin' ? `<button class="btn btn-danger btn-sm" onclick="deleteUser('${u.username}')">Delete</button>` : ''}
                    </td>
                </tr>
            `).join('');
        }
        
        function showAddModal() {
            document.getElementById('modalTitle').textContent = 'Add User';
            document.getElementById('editMode').value = 'add';
            document.getElementById('username').value = '';
            document.getElementById('username').disabled = false;
            document.getElementById('password').value = '';
            document.getElementById('role').value = 'user';
            document.getElementById('enabled').value = 'true';
            document.getElementById('userModal').classList.add('active');
        }
        
        function editUser(username) {
            const user = users.find(u => u.username === username);
            if (!user) return;
            document.getElementById('modalTitle').textContent = 'Edit User';
            document.getElementById('editMode').value = 'edit';
            document.getElementById('username').value = user.username;
            document.getElementById('username').disabled = true;
            document.getElementById('password').value = '';
            document.getElementById('role').value = user.role || 'user';
            document.getElementById('enabled').value = user.enabled ? 'true' : 'false';
            document.getElementById('userModal').classList.add('active');
        }
        
        function closeModal() {
            document.getElementById('userModal').classList.remove('active');
        }
        
        document.getElementById('userForm').addEventListener('submit', async function(e) {
            e.preventDefault();
            const mode = document.getElementById('editMode').value;
            const formData = new FormData(this);
            formData.set('enabled', document.getElementById('enabled').value);
            
            try {
                let url, method;
                if (mode === 'add') {
                    url = '/api/users';
                    method = 'POST';
                } else {
                    url = '/api/users/update';
                    method = 'POST';
                }
                const resp = await fetch(url, { method, body: formData });
                const data = await resp.json();
                if (data.success) {
                    showToast(mode === 'add' ? 'User created' : 'User updated', 'success');
                    closeModal();
                    loadUsers();
                } else {
                    showToast(data.message || 'Operation failed', 'error');
                }
            } catch (e) {
                showToast('Request failed', 'error');
            }
        });
        
        async function deleteUser(username) {
            if (!confirm('Delete user "' + username + '"?')) return;
            try {
                const formData = new FormData();
                formData.append('username', username);
                const resp = await fetch('/api/users/delete', { method: 'POST', body: formData });
                const data = await resp.json();
                if (data.success) {
                    showToast('User deleted', 'success');
                    loadUsers();
                } else {
                    showToast(data.message || 'Delete failed', 'error');
                }
            } catch (e) {
                showToast('Request failed', 'error');
            }
        }
        
        function showToast(message, type) {
            const toast = document.createElement('div');
            toast.className = 'toast toast-' + type;
            toast.textContent = message;
            document.body.appendChild(toast);
            setTimeout(() => toast.remove(), 3000);
        }
        
        async function logout() {
            await fetch('/api/auth/logout', { method: 'POST' });
            window.location.href = '/login';
        }
        
        loadUsers();
    </script>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", html);
}
// ============ 日志管理 API 处理函数 ============

void WebConfigManager::handleAPIGetLogs(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }
    
    String linesParam = getParamValue(request, "lines", "100");
    int maxLines = linesParam.toInt();
    if (maxLines <= 0) maxLines = 100;
    if (maxLines > 1000) maxLines = 1000;
    
    static const char* LOG_FILE = "/logs/system.log";
    
    if (!LittleFS.exists(LOG_FILE)) {
        JsonDocument doc;
        doc["success"] = true;
        doc["data"]["content"] = "Log file does not exist";
        doc["data"]["size"] = 0;
        doc["data"]["lines"] = 0;
        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
        return;
    }
    
    File file = LittleFS.open(LOG_FILE, "r");
    if (!file) {
        sendError(request, 500, "Failed to open log file");
        return;
    }
    
    size_t fileSize = file.size();
    size_t maxSize = 32 * 1024;
    size_t startPos = (fileSize > maxSize) ? (fileSize - maxSize) : 0;
    
    file.seek(startPos);
    String content = file.readString();
    file.close();
    
    std::vector<String> lines;
    int start = 0;
    int pos;
    while ((pos = content.indexOf('\n', start)) != -1) {
        lines.push_back(content.substring(start, pos));
        start = pos + 1;
    }
    if (start < (int)content.length()) {
        lines.push_back(content.substring(start));
    }
    
    String result;
    int startLine = (int)lines.size() - maxLines;
    if (startLine < 0) startLine = 0;
    for (int i = startLine; i < (int)lines.size(); i++) {
        result += lines[i] + "\n";
    }
    
    JsonDocument doc;
    doc["success"] = true;
    doc["data"]["content"] = result;
    doc["data"]["size"] = fileSize;
    doc["data"]["lines"] = lines.size();
    doc["data"]["truncated"] = (startPos > 0);
    
    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
}

void WebConfigManager::handleAPIGetLogInfo(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }
    
    static const char* LOG_FILE = "/logs/system.log";
    
    JsonDocument doc;
    doc["success"] = true;
    
    if (LittleFS.exists(LOG_FILE)) {
        File file = LittleFS.open(LOG_FILE, "r");
        if (file) {
            doc["data"]["size"] = file.size();
            doc["data"]["exists"] = true;
            file.close();
        } else {
            doc["data"]["size"] = 0;
            doc["data"]["exists"] = false;
        }
    } else {
        doc["data"]["size"] = 0;
        doc["data"]["exists"] = false;
    }
    
    doc["data"]["maxSize"] = LOGGER.getLogFileSizeLimit();
    doc["data"]["level"] = (int)LOGGER.getLogLevel();
    doc["data"]["fileLogging"] = LOGGER.isFileLoggingEnabled();
    
    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
}

void WebConfigManager::handleAPIClearLogs(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    if (LOGGER.deleteLogFile()) {
        LOG_INFO("Log file cleared by user");
        sendSuccess(request, "Log file cleared");
    } else {
        sendError(request, 500, "Failed to clear log file");
    }
}
