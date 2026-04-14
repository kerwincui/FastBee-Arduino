#include "./network/WebHandlerContext.h"
#include "./security/AuthManager.h"
#include "systems/LoggerSystem.h"

WebHandlerContext::WebHandlerContext(AsyncWebServer* srv, IAuthManager* authMgr, IUserManager* userMgr)
    : server(srv), authManager(authMgr), userManager(userMgr),
      roleManager(nullptr), networkManager(nullptr), otaManager(nullptr),
      protocolManager(nullptr), sseHandler(nullptr), webRootPath("/www"),
      scheduleRestart(false), scheduledRestartTime(0), cacheDuration(86400) {
    loadCacheDuration();
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
        "/api/events",  // SSE 推送通道，EventSource 不支持 Cookie，需跳过 session 认证
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
    AsyncWebServerResponse* response;

    if (docSize < 512) {
        char jsonBuffer[512];
        serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
        response = request->beginResponse(code, "application/json", jsonBuffer);
    } else {
        String jsonStr;
        jsonStr.reserve(docSize + 1);
        serializeJson(doc, jsonStr);
        response = request->beginResponse(code, "application/json", jsonStr);
    }

    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    request->send(response);
}

void WebHandlerContext::sendSuccess(AsyncWebServerRequest* request, const JsonDocument& data) {
    // 注意：ArduinoJson v7 的 serializeJson(doc, String) 会先清空目标字符串，
    // 因此必须先序列化到独立字符串，再拼接到响应中。
    String dataStr;
    if (!data.isNull()) {
        serializeJson(data, dataStr);
    }

    String out;
    out.reserve(dataStr.length() + 48);
    out = "{\"success\":true,\"timestamp\":";
    out += String(millis());
    if (dataStr.length() > 0) {
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
    String ext = path.substring(path.lastIndexOf('.'));
    String contentType = getContentType(path);
    bool isCompressible = (ext == ".html" || ext == ".js" || ext == ".css");

    // 优先尝试 .gz 压缩版本（直接 open 代替 exists + open 双次IO）
    if (isCompressible) {
        String gzPath = path + ".gz";
        File gzFile = LittleFS.open(gzPath, "r");
        if (gzFile) {
            size_t fileSize = gzFile.size();
            gzFile.close();

            // ETag 基于文件大小（flash 上文件稳定后大小不变）
            String etag = "\"" + String(fileSize, HEX) + "\"";

            // 在创建 response 之前检查缓存命中（避免 beginResponse 打开文件后又丢弃）
            if (request->hasHeader("If-None-Match")) {
                if (request->header("If-None-Match") == etag) {
                    request->send(304);
                    return true;
                }
            }

            AsyncWebServerResponse *response = request->beginResponse(LittleFS, gzPath, contentType);
            response->addHeader("Content-Encoding", "gzip");
            response->addHeader("ETag", etag);

            // 添加 Link header 用于资源预加载（仅对 index.html）
            if (path == "/www/index.html" || path == "/index.html") {
                response->addHeader("Link", "</js/state.js>; rel=preload; as=script, </css/main.css>; rel=preload; as=style");
            }
            // Service Worker 文件特殊处理
            if (path == "/www/sw.js" || path == "/sw.js") {
                response->addHeader("Service-Worker-Allowed", "/");
                response->addHeader("Cache-Control", "no-cache");  // SW 文件不缓存，确保更新
            }
            // pages 目录：长缓存（内容几乎不变）
            if (path.startsWith("/www/pages/")) {
                response->addHeader("Cache-Control", "public, max-age=86400, must-revalidate");
            } else if (ext == ".html") {
                response->addHeader("Cache-Control", "public, max-age=3600, must-revalidate");
            } else {
                // JS/CSS 静态资源：1天缓存
                response->addHeader("Cache-Control", "public, max-age=86400, must-revalidate");
            }
            response->addHeader("Vary", "Accept-Encoding");

            request->send(response);
            return true;
        }
    }

    // 尝试原始文件（直接 open，一次 IO）
    File file = LittleFS.open(path, "r");
    if (file) {
        size_t fileSize = file.size();
        file.close();

        String etag = "\"" + String(fileSize, HEX) + "\"";

        // 在创建 response 之前检查缓存命中
        if (request->hasHeader("If-None-Match")) {
            if (request->header("If-None-Match") == etag) {
                request->send(304);
                return true;
            }
        }

        AsyncWebServerResponse *response = request->beginResponse(LittleFS, path, contentType);
        response->addHeader("ETag", etag);

        // 图片等不可压缩资源缓存7天
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" ||
            ext == ".ico" || ext == ".svg" || ext == ".woff" || ext == ".woff2") {
            response->addHeader("Cache-Control", "public, max-age=604800, immutable");
        } else {
            response->addHeader("Cache-Control", "no-cache");
        }

        request->send(response);
        return true;
    }

    return false;
}

void WebHandlerContext::serveGzippedFile(AsyncWebServerRequest* request, const String& path) {
    // 统一使用 serveStaticFile，已包含 gzip 和缓存逻辑
    if (serveStaticFile(request, path)) return;
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

void WebHandlerContext::loadCacheDuration() {
    cacheDuration = 86400; // 默认24小时
    File f = LittleFS.open("/config/device.json", "r");
    if (f) {
        JsonDocument doc;
        if (!deserializeJson(doc, f)) {
            if (doc["cacheDuration"].is<int>()) {
                cacheDuration = (uint32_t)doc["cacheDuration"].as<int>();
            }
        }
        f.close();
    }
}
