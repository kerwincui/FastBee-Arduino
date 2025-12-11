/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:30:27
 */

#include "network/WebConfigManager.h"
#include <esp_system.h> 
#include <systems/LoggerSystem.h>
#include <core/FastBeeFramework.h>
#include "systems/HealthMonitor.h"
#include <utils/FileUtils.h>

WebConfigManager::WebConfigManager(AsyncWebServer* webServerPtr) : server(webServerPtr){
    isRunning = false;
}

WebConfigManager::~WebConfigManager() {
    shutdown();  // 确保资源释放
}

bool WebConfigManager::initialize() {
    // 初始化 LittleFS
    if (!LittleFS.begin(true)) { // true 表示如果文件系统不存在则格式化
        Serial.println("LittleFS Mount Failed");
        return false;
    }

    // 设置路由
    setupRoutes();
    
    // 启动服务器
    server->begin();
    isRunning = true;
    LOG_INFO("[WebConfigManager] 异步Web服务器已在端口80启动");
    
    // 加载配置
    loadConfiguration();
    
    return true;
}

void WebConfigManager::shutdown() {
    if (!isRunning) return;

    Serial.println("WebConfigManager: Shutting down...");
    
    // 停止Web服务器
    if (isRunning) {
        server->end();
        delete server;
        server = nullptr;
        Serial.println("WebConfigManager: Web server stopped");
    }
    isRunning = false;
    
    // 关闭所有客户端连接
    // WebServer库通常会自动处理客户端连接的关闭
    
    // 关闭文件系统（谨慎操作，确保没有其他组件在使用）
    // LittleFS.end(); // 如果有其他组件在使用LittleFS，不要调用这个
    
    // 清理Preferences
    prefs.end();
    
    Serial.println("WebConfigManager: Shutdown completed");
}

// 过滤器：排除所有.gz文件和隐藏文件
bool WebConfigManager::requestFilter(AsyncWebServerRequest* request) {
    String url = request->url();  // 获取请求的URL路径
    
    // 检查路径是否应该被拒绝
    if (url.endsWith(".gz")) return false;
    if (url.endsWith(".map")) return false;
    
    // 只允许特定类型的文件请求
    if (url.endsWith(".css") || 
        url.endsWith(".js") || 
        url.endsWith(".html") || 
        url.endsWith(".htm") ||
        url.endsWith(".png") || 
        url.endsWith(".jpg") || 
        url.endsWith(".jpeg") ||
        url.endsWith(".gif") || 
        url.endsWith(".ico") || 
        url.endsWith(".svg") ||
        url.endsWith(".ttf") || 
        url.endsWith(".woff") || 
        url.endsWith(".woff2") ||
        url.endsWith(".eot") || 
        url.endsWith(".json") || 
        url.endsWith(".txt") ||
        url.endsWith(".xml") ||
        url.equals("/") ||  // 允许根路径
        url.startsWith("/api/")) {  // 允许API请求
        return true;
    }
    
    // 默认拒绝
    return false;
}

void WebConfigManager::setupRoutes() {
    // 设置请求过滤器
    auto configureHandler = [](AsyncStaticWebHandler* handler, const char* cacheControl = nullptr) {
        handler->setFilter(WebConfigManager::requestFilter);
        handler->setIsDir(false);
        if (cacheControl) {
            handler->setCacheControl(cacheControl);
        }
    };

    // 静态文件服务 - 直接从 LittleFS 提供文件
    configureHandler(&server->serveStatic("/css/", LittleFS, "/www/css/"));
    configureHandler(&server->serveStatic("/js/", LittleFS, "/www/js/"));
    configureHandler(&server->serveStatic("/assets/", LittleFS, "/www/assets/"));
    
    // 页面路由
    server->on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
        this->handleRoot(request);
    });
    server->on("/login", HTTP_GET, [this](AsyncWebServerRequest *request) {
        this->handleLogin(request);
    });
    server->on("/login", HTTP_POST, [this](AsyncWebServerRequest *request) {
        this->handleLogin(request);
    });
    server->on("/logout", HTTP_GET, [this](AsyncWebServerRequest *request) {
        this->handleLogout(request);
    });
    server->on("/system", HTTP_GET, [this](AsyncWebServerRequest *request) {
        this->handleSystemConfig(request);
    });
    server->on("/network", HTTP_GET, [this](AsyncWebServerRequest *request) {
        this->handleNetworkConfig(request);
    });
    server->on("/users", HTTP_GET, [this](AsyncWebServerRequest *request) {
        this->handleUserManagement(request);
    });
    server->on("/monitor", HTTP_GET, [this](AsyncWebServerRequest *request) {
        this->handleDeviceMonitor(request);
    });
    server->on("/ota", HTTP_GET, [this](AsyncWebServerRequest *request) {
        this->handleOTAUpdate(request);
    });
    server->on("/protocol", HTTP_GET, [this](AsyncWebServerRequest *request) {
        this->handleProtocolConfig(request);
    });
    
    // API路由
    server->on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
        this->handleAPIStatus(request);
    });
    server->on("/api/config", HTTP_GET, [this](AsyncWebServerRequest *request) {
        this->handleAPIConfig(request);
    });
    server->on("/api/config", HTTP_POST, [this](AsyncWebServerRequest *request) {
        this->handleAPIConfig(request);
    });
    server->on("api/users", HTTP_GET, [this](AsyncWebServerRequest *request) {
        this->handleAPIUsers(request);
    });
    server->on("/api/device", HTTP_GET, [this](AsyncWebServerRequest *request) {
        this->handleAPIDeviceInfo(request);
    });
    server->on("/api/health", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleHealthCheck(request);
    });
    server->on("/api/system/info", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleSystemInfo(request);
    });
    server->on("/api/system/restart", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleSystemRestart(request);
    });
    server->on("/api/filesystem", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleAPIFileSystemInfo(request);
    });

    // 404处理
    server->onNotFound([this](AsyncWebServerRequest *request) {
        String notFoundPage = readHTMLFile("/www/404.html");
        request->send(404, "text/html", notFoundPage);
    });
    
}

// 文件服务方法
bool WebConfigManager::serveFile(AsyncWebServerRequest *request, const String& path, const String& contentType) {
    if (LittleFS.exists(path)) {
        request->send(LittleFS, path, contentType);
        return true;
    }
    request->send(404, "text/plain", "文件未找到: " + path);
    return false;
}

String WebConfigManager::readHTMLFile(const String& filename) {
    String content = "";
    if (LittleFS.exists(filename)) {
        File file = LittleFS.open(filename, "r");
        if (file) {
            content = file.readString();
            file.close();
        }
    }
    return content;
}

bool WebConfigManager::replacePlaceholders(String& content, const std::vector<std::pair<String, String>>& replacements) {
    for (const auto& replacement : replacements) {
        content.replace(replacement.first, replacement.second);
    }
    return true;
}

// 认证相关方法
bool WebConfigManager::isAuthenticated(AsyncWebServerRequest *request) {
    if (request->hasHeader("Cookie")) {
        String cookie = request->header("Cookie");
        Serial.printf("[Auth] 收到Cookie: %s\n", cookie.c_str());
        return cookie.indexOf("fastbee_session=authenticated") != -1;
    }
    Serial.println("[Auth] 未找到Cookie头");
    return false;
}



bool WebConfigManager::checkAuth(const String& username, const String& password) {
    // 从Preferences加载用户数据进行验证
    prefs.begin("users", true);
    String storedUser = prefs.getString("admin_user", "admin");
    String storedPass = prefs.getString("admin_pass", "admin8");
    prefs.end();
    
    bool result = (username == storedUser && password == storedPass);
    Serial.printf("[Auth] 验证用户: %s, 结果: %s\n", 
                  username.c_str(), result ? "成功" : "失败");
    return result;
}

// Web页面处理函数
void WebConfigManager::handleRoot(AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
        request->redirect("/login");
        return;
    }
    // 已认证，返回主页
    serveFile(request, "/www/index.html");
}

void WebConfigManager::handleLogin(AsyncWebServerRequest *request) {
    if (request->method() == HTTP_POST) {
        String username = request->arg("username");
        String password = request->arg("password");
        
        if (checkAuth(username, password)) {
            // 关键修复：设置带有效期和路径的Cookie，解决会话保持问题
            AsyncWebServerResponse* response = request->beginResponse(200, "application/json", 
                StringUtils::buildJsonResponse(1, "登录成功"));
            
            // 设置Cookie，添加Max-Age使浏览器持久化保存（24小时）
            String cookie = "fastbee_session=authenticated; Path=/; Max-Age=86400; HttpOnly";
            response->addHeader("Set-Cookie", cookie);
            response->addHeader("Access-Control-Allow-Credentials", "true");
            
            request->send(response);
            Serial.println("[Login] 登录成功，Cookie已设置");
        } else {
            request->send(200, "application/json", 
                StringUtils::buildJsonResponse(0, "账号密码错误"));
        }
    } else {
        // GET请求 - 显示登录页面
        serveFile(request, "/www/index.html", "text/html");
    }
}

void WebConfigManager::handleLogout(AsyncWebServerRequest *request) {
    // 清除认证Cookie
    AsyncWebServerResponse* response = request->beginResponse(302);
    response->addHeader("Location", "/login");
    response->addHeader("Set-Cookie", 
        "fastbee_session=deleted; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
    request->send(response);
}

void WebConfigManager::handleSystemConfig(AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
        request->redirect("/login");
        return;
    }
    serveFile(request, "/www/index.html");
}

void WebConfigManager::handleNetworkConfig(AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
        request->redirect("/login");
        return;
    }
    serveFile(request, "/www/index");
}

void WebConfigManager::handleUserManagement(AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
        request->redirect("/login");
        return;
    }
    serveFile(request, "/www/index.html");
}

void WebConfigManager::handleDeviceMonitor(AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
        request->redirect("/login");
        return;
    }
    serveFile(request, "/www/index.html");
}

void WebConfigManager::handleOTAUpdate(AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
        request->redirect("/login");
        return;
    }
    serveFile(request, "/www/index.html");
}

void WebConfigManager::handleProtocolConfig(AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
        request->redirect("/login");
        return;
    }
    serveFile(request, "/www/index.html");
}

// API处理函数
void WebConfigManager::handleAPIStatus(AsyncWebServerRequest *request) {
     DynamicJsonDocument doc(512);
    doc["status"] = "running";
    doc["uptime"] = millis();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["chip_id"] = String(ESP.getEfuseMac(), HEX);
    doc["server_running"] = isRunning;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void WebConfigManager::handleAPIConfig(AsyncWebServerRequest *request) {
     if (!isAuthenticated(request)) {
        request->send(401, "application/json", "{\"error\":\"未认证\"}");
        return;
    }
    
    if (request->method() == HTTP_GET) {
        DynamicJsonDocument doc(1024);
        if (loadSystemConfig(doc)) {
            String response;
            serializeJson(doc, response);
            request->send(200, "application/json", response);
        } else {
            request->send(500, "application/json", "{\"error\":\"加载配置失败\"}");
        }
    } else if (request->method() == HTTP_POST) {
        String body = request->arg("plain");
        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, body);
        
        if (error) {
            request->send(400, "application/json", "{\"error\":\"无效JSON\"}");
            return;
        }
        
        if (saveSystemConfig(doc)) {
            request->send(200, "application/json", "{\"status\":\"success\"}");
        } else {
            request->send(500, "application/json", "{\"error\":\"保存配置失败\"}");
        }
    }
}

void WebConfigManager::handleAPIUsers(AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
        request->send(401, "application/json", "{\"error\":\"未认证\"}");
        return;
    }
    
    DynamicJsonDocument doc(512);
    JsonArray users = doc.to<JsonArray>();
    JsonObject admin = users.createNestedObject();
    admin["username"] = "admin";
    admin["role"] = "administrator";
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void WebConfigManager::handleAPIDeviceInfo(AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(512);
    doc["chip_id"] = String(ESP.getEfuseMac(), HEX);
    doc["flash_size"] = ESP.getFlashChipSize();
    doc["sdk_version"] = ESP.getSdkVersion();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["cpu_freq"] = ESP.getCpuFreqMHz();
    doc["reset_reason"] = esp_reset_reason();
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// 处理健康检查请求
void WebConfigManager::handleHealthCheck(AsyncWebServerRequest* request) {
    FastBeeFramework* framework = FastBeeFramework::getInstance();
    if (!framework || !framework->getHealthMonitor()) {
        request->send(500, "application/json", "{\"error\": \"Health monitor not available\"}");
        return;
    }
    
    DynamicJsonDocument doc(512);
    doc["status"] = framework-> getHealthMonitor() -> isSystemHealthy() ? "healthy" : "unhealthy";
    doc["uptime"] = millis();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["min_free_heap"] = ESP.getMinFreeHeap();
    doc["max_alloc_heap"] = ESP.getMaxAllocHeap();
    doc["health_report"] = framework->getHealthMonitor() -> getHealthReport();
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// 处理系统信息请求
void WebConfigManager::handleSystemInfo(AsyncWebServerRequest* request) {
    DynamicJsonDocument doc(1024);
    
    // 基础信息
    doc["device_name"] = "FastBee Device";
    doc["firmware_version"] = "1.0.0";
    doc["build_date"] = __DATE__ " " __TIME__;
    doc["sdk_version"] = ESP.getSdkVersion();
    
    uint64_t chipid = ESP.getEfuseMac();
    char chipidStr[13];
    snprintf(chipidStr, sizeof(chipidStr), "%04X%08X", 
             (uint16_t)(chipid >> 32), (uint32_t)chipid);
    doc["chip_id"] = String(chipidStr);
    
    doc["chip_revision"] = ESP.getChipRevision();
    doc["cpu_freq_mhz"] = ESP.getCpuFreqMHz();
    doc["flash_size"] = ESP.getFlashChipSize();
    doc["flash_speed"] = ESP.getFlashChipSpeed();
    
    // 内存信息
    doc["free_heap"] = ESP.getFreeHeap();
    doc["min_free_heap"] = ESP.getMinFreeHeap();
    doc["max_alloc_heap"] = ESP.getMaxAllocHeap();
    doc["heap_fragmentation"] = ESP.getHeapSize();
    doc["psram_size"] = ESP.getPsramSize();
    doc["free_psram"] = ESP.getFreePsram();
    
    esp_reset_reason_t reason = esp_reset_reason();
    const char* reasonStr;
    switch(reason) {
        case ESP_RST_POWERON: reasonStr = "Power On"; break;
        case ESP_RST_SW: reasonStr = "Software Reset"; break;
        case ESP_RST_PANIC: reasonStr = "Exception/Panic"; break;
        case ESP_RST_INT_WDT: reasonStr = "Interrupt Watchdog"; break;
        case ESP_RST_TASK_WDT: reasonStr = "Task Watchdog"; break;
        case ESP_RST_WDT: reasonStr = "Other Watchdog"; break;
        case ESP_RST_DEEPSLEEP: reasonStr = "Deep Sleep"; break;
        case ESP_RST_BROWNOUT: reasonStr = "Brownout"; break;
        case ESP_RST_SDIO: reasonStr = "SDIO"; break;
        default: reasonStr = "Unknown";
    }
    doc["last_restart_reason"] = reasonStr;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// 处理系统重启请求
void WebConfigManager::handleSystemRestart(AsyncWebServerRequest* request) {
    LOG_WARNING("System restart requested via API");
    
    // 发送响应
    request->send(200, "application/json", "{\"message\": \"System will restart in 3 seconds\"}");
    
    // 延迟重启，让客户端有时间收到响应
    delay(3000);
    
    LOG_INFO("Restarting system...");
    ESP.restart();
}

void WebConfigManager::handleAPIFileSystemInfo(AsyncWebServerRequest* request) {
    String jsonInfo = FileUtils::getFileSystemInfoJSON();
    request->send(200, "application/json", jsonInfo);
}

// 配置管理方法
bool WebConfigManager::saveSystemConfig(const JsonDocument& config) {
    prefs.begin("system", false);
    String configStr;
    serializeJson(config, configStr);
    bool result = prefs.putString("config", configStr) > 0;
    prefs.end();
    return result;
}

bool WebConfigManager::loadSystemConfig(JsonDocument& config) {
    prefs.begin("system", true);
    String configStr = prefs.getString("config", "{}");
    prefs.end();
    
    DeserializationError error = deserializeJson(config, configStr);
    return !error;
}

bool WebConfigManager::saveNetworkConfig(const JsonDocument& config) {
    prefs.begin("network", false);
    String configStr;
    serializeJson(config, configStr);
    bool result = prefs.putString("config", configStr) > 0;
    prefs.end();
    return result;
}

bool WebConfigManager::loadNetworkConfig(JsonDocument& config) {
    prefs.begin("network", true);
    String configStr = prefs.getString("config", "{}");
    prefs.end();
    
    DeserializationError error = deserializeJson(config, configStr);
    return !error;
}

void WebConfigManager::loadConfiguration() {
    // 检查是否有默认用户配置，如果没有则创建
    prefs.begin("users", true);
    if (!prefs.isKey("admin_user")) {
        prefs.end();
        prefs.begin("users", false);
        prefs.putString("admin_user", "admin");
        prefs.putString("admin_pass", "admin");
        Serial.println("[Config] 创建默认用户: admin/admin");
    }
    prefs.end();
}

