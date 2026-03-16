#include "./network/WebHandlerContext.h"
#include "./security/AuthManager.h"
#include "systems/LoggerSystem.h"

WebHandlerContext::WebHandlerContext(AsyncWebServer* srv, IAuthManager* authMgr, IUserManager* userMgr)
    : server(srv), authManager(authMgr), userManager(userMgr),
      roleManager(nullptr), networkManager(nullptr), otaManager(nullptr),
      protocolManager(nullptr), webRootPath("/www"),
      scheduleRestart(false), scheduledRestartTime(0) {
}

// ============ 参数处理辅助方法 ============

String WebHandlerContext::getParamValue(AsyncWebServerRequest* request, const String& paramName,
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

bool WebHandlerContext::getParamBool(AsyncWebServerRequest* request, const String& paramName,
                                     bool defaultValue) {
    String value = getParamValue(request, paramName, defaultValue ? "true" : "false");
    return value.equalsIgnoreCase("true") || value == "1";
}

int WebHandlerContext::getParamInt(AsyncWebServerRequest* request, const String& paramName,
                                   int defaultValue) {
    String value = getParamValue(request, paramName, String(defaultValue));
    return value.toInt();
}

// ============ 认证辅助方法 ============

bool WebHandlerContext::requiresAuth(AsyncWebServerRequest* request) {
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

    for (const char* publicPath : publicPaths) {
        if (path.startsWith(publicPath)) {
            return false;
        }
    }

    return true;
}

AuthResult WebHandlerContext::authenticateRequest(AsyncWebServerRequest* request) {
    if (!authManager) {
        return AuthResult();
    }

    String sessionId = AuthManager::extractSessionIdFromRequest(request);
    if (sessionId.isEmpty()) {
        return AuthResult();
    }

    return authManager->verifySession(sessionId, true);
}

String WebHandlerContext::getClientIP(AsyncWebServerRequest* request) {
    if (!request) return "";

    if (request->hasHeader("X-Forwarded-For")) {
        return request->header("X-Forwarded-For");
    }

    if (request->hasHeader("X-Real-IP")) {
        return request->header("X-Real-IP");
    }

    return request->client()->remoteIP().toString();
}

String WebHandlerContext::getUserAgent(AsyncWebServerRequest* request) {
    if (!request) return "";

    if (request->hasHeader("User-Agent")) {
        return request->header("User-Agent");
    }

    return "";
}

bool WebHandlerContext::checkPermission(AsyncWebServerRequest* request, const String& permission) {
    if (!authManager || !request) {
        return false;
    }

    AuthResult authResult = authenticateRequest(request);
    if (!authResult.success) {
        return false;
    }

    return authManager->checkSessionPermission(authResult.sessionId, permission);
}

// ============ 响应辅助方法 ============

void WebHandlerContext::sendJsonResponse(AsyncWebServerRequest* request, int code,
                                         const JsonDocument& doc) {
    if (!request) return;

    size_t docSize = measureJson(doc);
    if (docSize < 512) {
        char jsonBuffer[512];
        serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));

        AsyncWebServerResponse* response = request->beginResponse(code, "application/json", jsonBuffer);
        response->addHeader("Access-Control-Allow-Origin", "*");
        response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
        request->send(response);
    } else {
        String jsonStr;
        serializeJson(doc, jsonStr);

        AsyncWebServerResponse* response = request->beginResponse(code, "application/json", jsonStr);
        response->addHeader("Access-Control-Allow-Origin", "*");
        response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
        request->send(response);
    }
}

void WebHandlerContext::sendSuccess(AsyncWebServerRequest* request, const JsonDocument& data) {
    String dataStr;
    serializeJson(data, dataStr);

    String out;
    out.reserve(dataStr.length() + 64);
    out = "{\"success\":true,\"timestamp\":";
    out += String(millis());
    if (!data.isNull()) {
        out += ",\"data\":";
        out += dataStr;
    }
    out += "}";

    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", out);
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
}

void WebHandlerContext::sendSuccess(AsyncWebServerRequest* request, const String& message) {
    char jsonBuffer[256];
    if (message.isEmpty()) {
        snprintf(jsonBuffer, sizeof(jsonBuffer), "{\"success\":true,\"timestamp\":%lu}", millis());
    } else {
        snprintf(jsonBuffer, sizeof(jsonBuffer), "{\"success\":true,\"message\":\"%s\",\"timestamp\":%lu}",
                 message.c_str(), millis());
    }

    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", jsonBuffer);
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
}

void WebHandlerContext::sendError(AsyncWebServerRequest* request, int code, const String& message) {
    char jsonBuffer[256];
    snprintf(jsonBuffer, sizeof(jsonBuffer),
             "{\"success\":false,\"error\":\"%s\",\"code\":%d,\"timestamp\":%lu}",
             message.c_str(), code, millis());

    AsyncWebServerResponse* response = request->beginResponse(code, "application/json", jsonBuffer);
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
}

void WebHandlerContext::sendUnauthorized(AsyncWebServerRequest* request) {
    sendError(request, 401, "Unauthorized");
}

void WebHandlerContext::sendForbidden(AsyncWebServerRequest* request) {
    sendError(request, 403, "Forbidden");
}

void WebHandlerContext::sendNotFound(AsyncWebServerRequest* request) {
    sendError(request, 404, "Not Found");
}

void WebHandlerContext::sendBadRequest(AsyncWebServerRequest* request, const String& message) {
    sendError(request, 400, message);
}

// ============ 内置页面 ============

void WebHandlerContext::sendBuiltinLoginPage(AsyncWebServerRequest* request) {
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
        .lang-switch { position: absolute; top: 15px; right: 15px; background: rgba(255,255,255,0.2); border: none; color: white; padding: 6px 12px; border-radius: 4px; cursor: pointer; font-size: 13px; }
        .lang-switch:hover { background: rgba(255,255,255,0.35); }
    </style>
</head>
<body>
    <button class="lang-switch" id="langBtn" onclick="toggleLang()">EN</button>
    <div class="login-box">
        <h1 id="title">FastBee IoT Platform</h1>
        <div id="message" class="message"></div>
        <form id="loginForm">
            <div class="form-group">
                <label for="username" id="lbl-user">Username</label>
                <input type="text" id="username" name="username" required placeholder="Enter username">
            </div>
            <div class="form-group">
                <label for="password" id="lbl-pwd">Password</label>
                <input type="password" id="password" name="password" required placeholder="Enter password">
            </div>
            <button type="submit" id="loginBtn">Login</button>
        </form>
        <div class="info" id="hint">Default: admin / admin123</div>
    </div>
    <script>
        var L={zh:{'title':'FastBee 物联网平台','lbl-user':'用户名','lbl-pwd':'密码','loginBtn':'登录','hint':'默认: admin / admin123','ph-user':'请输入用户名','ph-pwd':'请输入密码','ok':'登录成功! 正在跳转...','fail':'登录失败','err':'连接错误: '},en:{'title':'FastBee IoT Platform','lbl-user':'Username','lbl-pwd':'Password','loginBtn':'Login','hint':'Default: admin / admin123','ph-user':'Enter username','ph-pwd':'Enter password','ok':'Login successful! Redirecting...','fail':'Login failed','err':'Connection error: '}};
        var lang=localStorage.getItem('language')==='en'?'en':'zh';
        function applyLang(){var t=L[lang];document.getElementById('title').textContent=t['title'];document.getElementById('lbl-user').textContent=t['lbl-user'];document.getElementById('lbl-pwd').textContent=t['lbl-pwd'];document.getElementById('loginBtn').textContent=t['loginBtn'];document.getElementById('hint').textContent=t['hint'];document.getElementById('username').placeholder=t['ph-user'];document.getElementById('password').placeholder=t['ph-pwd'];document.getElementById('langBtn').textContent=lang==='zh'?'EN':'中文';}
        function toggleLang(){lang=lang==='zh'?'en':'zh';localStorage.setItem('language',lang==='zh'?'zh-CN':'en');applyLang();}
        applyLang();
        document.getElementById('loginForm').addEventListener('submit', async function(e) {
            e.preventDefault();
            var msg = document.getElementById('message');
            msg.className = 'message'; msg.style.display = 'none';
            var formData = new FormData(this);
            try {
                var resp = await fetch('/api/auth/login', { method: 'POST', body: formData });
                var data = await resp.json();
                if (data.success) {
                    msg.textContent = L[lang]['ok'];
                    msg.className = 'message success';
                    setTimeout(function(){ window.location.href = '/dashboard'; }, 1000);
                } else {
                    msg.textContent = data.message || L[lang]['fail'];
                    msg.className = 'message error';
                }
            } catch (err) {
                msg.textContent = L[lang]['err'] + err.message;
                msg.className = 'message error';
            }
        });
    </script>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", html);
}

void WebHandlerContext::sendBuiltinDashboard(AsyncWebServerRequest* request) {
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

void WebHandlerContext::sendBuiltinUsersPage(AsyncWebServerRequest* request) {
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
            } catch (e) { showToast('Failed to load users', 'error'); }
        }
        function renderUsers() {
            const tbody = document.getElementById('userList');
            if (users.length === 0) { tbody.innerHTML = '<tr><td colspan="5" class="empty-state">No users found</td></tr>'; return; }
            tbody.innerHTML = users.map(u => `
                <tr>
                    <td>${u.username}</td>
                    <td>${u.role || 'user'}</td>
                    <td>
                        <span class="status-badge ${u.enabled ? 'status-active' : 'status-inactive'}">${u.enabled ? 'Active' : 'Inactive'}</span>
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
        function closeModal() { document.getElementById('userModal').classList.remove('active'); }
        document.getElementById('userForm').addEventListener('submit', async function(e) {
            e.preventDefault();
            const mode = document.getElementById('editMode').value;
            const formData = new FormData(this);
            formData.set('enabled', document.getElementById('enabled').value);
            try {
                let url = mode === 'add' ? '/api/users' : '/api/users/update';
                const resp = await fetch(url, { method: 'POST', body: formData });
                const data = await resp.json();
                if (data.success) { showToast(mode === 'add' ? 'User created' : 'User updated', 'success'); closeModal(); loadUsers(); }
                else { showToast(data.message || 'Operation failed', 'error'); }
            } catch (e) { showToast('Request failed', 'error'); }
        });
        async function deleteUser(username) {
            if (!confirm('Delete user "' + username + '"?')) return;
            try {
                const formData = new FormData();
                formData.append('username', username);
                const resp = await fetch('/api/users/delete', { method: 'POST', body: formData });
                const data = await resp.json();
                if (data.success) { showToast('User deleted', 'success'); loadUsers(); }
                else { showToast(data.message || 'Delete failed', 'error'); }
            } catch (e) { showToast('Request failed', 'error'); }
        }
        function showToast(message, type) {
            const toast = document.createElement('div');
            toast.className = 'toast toast-' + type;
            toast.textContent = message;
            document.body.appendChild(toast);
            setTimeout(() => toast.remove(), 3000);
        }
        async function logout() { await fetch('/api/auth/logout', { method: 'POST' }); window.location.href = '/login'; }
        loadUsers();
    </script>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", html);
}

void WebHandlerContext::sendBuiltinSetupPage(AsyncWebServerRequest* request) {
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
        .lang-switch { position: absolute; top: 15px; right: 15px; background: rgba(255,255,255,0.2); border: none; color: white; padding: 6px 12px; border-radius: 4px; cursor: pointer; font-size: 13px; }
        .lang-switch:hover { background: rgba(255,255,255,0.35); }
    </style>
</head>
<body>
    <button class="lang-switch" id="langBtn" onclick="toggleLang()">EN</button>
    <div class="setup-box">
        <h1 id="pageTitle">FastBee WiFi Setup</h1>
        <p class="subtitle" id="pageSubtitle">Configure your device's WiFi connection</p>
        <div class="current-status">
            <div class="status-row">
                <span class="status-label" id="lblStatus">Status:</span>
                <span id="connStatus" class="status-value disconnected">Checking...</span>
            </div>
            <div class="status-row">
                <span class="status-label" id="lblIp">IP Address:</span>
                <span id="ipAddr" class="status-value">-</span>
            </div>
        </div>
        <div id="message" class="message info" style="display:none;"></div>
        <button class="scan-btn" onclick="scanNetworks()" id="scanBtn">Scan WiFi Networks</button>
        <div id="networkList" class="network-list" style="display:none;"></div>
        <form id="wifiForm">
            <div class="form-group">
                <label id="lblSsid">WiFi Name (SSID)</label>
                <input type="text" id="ssid" name="ssid" required placeholder="Enter or select WiFi name">
            </div>
            <div class="form-group">
                <label id="lblPwd">Password</label>
                <input type="password" id="password" name="password" placeholder="Enter WiFi password">
            </div>
            <button type="submit" id="connectBtn">Connect</button>
        </form>
        <div style="text-align:center; margin-top:20px;">
            <a href="/login" style="color:#667eea;" id="loginLink">Go to Login Page</a>
        </div>
    </div>
    <script>
        var T={zh:{pageTitle:'FastBee WiFi',pageSubtitle:'WiFi',lblStatus:'',lblIp:'IP',lblSsid:'WiFi (SSID)',lblPwd:'',scanBtn:'WiFi',connectBtn:'',loginLink:'',phSsid:'WiFi',phPwd:'WiFi',scanning:'...',noNet:'WiFi',scanFail:': ',connecting:'WiFi...',waitMsg:'... 10',connOk:'! : http://',connYes:'',connNo:'',checking:'...'},en:{pageTitle:'FastBee WiFi Setup',pageSubtitle:'Configure your device\'s WiFi connection',lblStatus:'Status:',lblIp:'IP Address:',lblSsid:'WiFi Name (SSID)',lblPwd:'Password',scanBtn:'Scan WiFi Networks',connectBtn:'Connect',loginLink:'Go to Login Page',phSsid:'Enter or select WiFi name',phPwd:'Enter WiFi password',scanning:'Scanning...',noNet:'No networks found',scanFail:'Scan failed: ',connecting:'Connecting to WiFi...',waitMsg:'Connecting... Please wait 10 seconds then refresh.',connOk:'Connected! You can now access via: http://',connYes:'Connected',connNo:'Not Connected',checking:'Checking...'}};
        var lang=localStorage.getItem('language')==='en'?'en':'zh';
        function applyLang(){var t=T[lang];['pageTitle','pageSubtitle','lblStatus','lblIp','lblSsid','lblPwd','scanBtn','connectBtn','loginLink'].forEach(function(k){var e=document.getElementById(k);if(e)e.textContent=t[k];});document.getElementById('ssid').placeholder=t.phSsid;document.getElementById('password').placeholder=t.phPwd;document.getElementById('langBtn').textContent=lang==='zh'?'EN':'中文';}
        function toggleLang(){lang=lang==='zh'?'en':'zh';localStorage.setItem('language',lang==='zh'?'zh-CN':'en');applyLang();}
        applyLang();
        function showMessage(text, type) {
            var msg = document.getElementById('message');
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
            var btn = document.getElementById('scanBtn');
            var list = document.getElementById('networkList');
            btn.disabled = true;
            btn.textContent = T[lang].scanning;
            try {
                var resp = await fetch('/api/wifi/scan');
                var data = await resp.json();
                if (data.networks && data.networks.length > 0) {
                    list.innerHTML = data.networks.map(function(n){ return '<div class="network-item" onclick="selectNetwork(\'' + n.ssid + '\')"><span class="network-name">' + n.ssid + (n.encrypted ? '<span class="lock-icon">\uD83D\uDD12</span>' : '') + '</span><span class="network-signal ' + getSignalClass(n.rssi) + '">' + getSignalBars(n.rssi) + ' ' + n.rssi + 'dBm</span></div>'; }).join('');
                    list.style.display = 'block';
                } else { showMessage(T[lang].noNet, 'info'); }
            } catch (e) { showMessage(T[lang].scanFail + e.message, 'error'); }
            btn.disabled = false;
            btn.textContent = T[lang].scanBtn;
        }
        function selectNetwork(ssid) {
            document.getElementById('ssid').value = ssid;
            document.getElementById('password').focus();
        }
        document.getElementById('wifiForm').addEventListener('submit', async function(e) {
            e.preventDefault();
            var btn = document.getElementById('connectBtn');
            btn.disabled = true;
            btn.textContent = T[lang].connecting;
            showMessage(T[lang].connecting, 'info');
            var formData = new FormData(this);
            try {
                await fetch('/api/wifi/connect', { method: 'POST', body: formData });
                showMessage(T[lang].waitMsg, 'success');
                setTimeout(checkStatus, 10000);
            } catch (e) {
                showMessage(T[lang].waitMsg, 'info');
                setTimeout(checkStatus, 10000);
            }
            btn.disabled = false;
            btn.textContent = T[lang].connectBtn;
        });
        async function checkStatus() {
            try {
                var resp = await fetch('/api/network/config');
                var data = await resp.json();
                var status = document.getElementById('connStatus');
                var ip = document.getElementById('ipAddr');
                if (data.connected) {
                    status.textContent = T[lang].connYes;
                    status.className = 'status-value connected';
                    ip.textContent = data.ipAddress || '-';
                    showMessage(T[lang].connOk + data.ipAddress, 'success');
                } else {
                    status.textContent = T[lang].connNo;
                    status.className = 'status-value disconnected';
                    ip.textContent = '-';
                }
            } catch (e) { console.error('Status check failed:', e); }
        }
        document.getElementById('connStatus').textContent = T[lang].checking;
        checkStatus();
    </script>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", html);
}

// ============ 文件服务方法 ============

bool WebHandlerContext::serveStaticFile(AsyncWebServerRequest* request, const String& path) {
    // 检查文件是否存在（原始文件或 .gz 版本）
    String ext = path.substring(path.lastIndexOf('.'));
    if (ext == ".html" || ext == ".js" || ext == ".css") {
        String gzPath = path + ".gz";
        // 如果只有 .gz 文件存在，使用 request->send 让库自动处理
        if (LittleFS.exists(gzPath) && !LittleFS.exists(path)) {
            String contentType = getContentType(path);
            // 传入原始路径，库会自动查找 .gz 版本并添加 Content-Encoding
            request->send(LittleFS, path, contentType);
            return true;
        }
    }

    // 尝试原始文件
    if (LittleFS.exists(path)) {
        String contentType = getContentType(path);
        request->send(LittleFS, path, contentType);
        return true;
    }

    return false;
}

void WebHandlerContext::serveGzippedFile(AsyncWebServerRequest* request, const String& path) {
    String gzPath = path + ".gz";

    // 如果只有 .gz 文件存在，传入原始路径让库自动处理
    if (LittleFS.exists(gzPath) && !LittleFS.exists(path)) {
        String contentType = getContentType(path);
        // 传入原始路径，库会自动查找 .gz 版本并添加 Content-Encoding
        request->send(LittleFS, path, contentType);
        return;
    }

    // 回退：尝试原始文件
    if (LittleFS.exists(path)) {
        String contentType = getContentType(path);
        request->send(LittleFS, path, contentType);
        return;
    }

    sendNotFound(request);
}

String WebHandlerContext::getContentType(const String& filename) {
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

String WebHandlerContext::readFile(const String& path) {
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

bool WebHandlerContext::fileExists(const String& path) {
    return LittleFS.exists(path);
}

// ============ 工具方法 ============

String WebHandlerContext::formatUptime(unsigned long ms) {
    unsigned long seconds = ms / 1000;
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;
    unsigned long days = hours / 24;

    char buf[64];
    if (days > 0) {
        snprintf(buf, sizeof(buf), "%lu天 %02lu:%02lu:%02lu", days, hours % 24, minutes % 60, seconds % 60);
    } else {
        snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", hours, minutes % 60, seconds % 60);
    }
    return String(buf);
}
