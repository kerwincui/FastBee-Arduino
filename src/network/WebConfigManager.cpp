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
#include "utils/JsonSerializationHelper.h"
#include "utils/JsonConverters.h"

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


void WebConfigManager::setupRoutes() {
    // 静态文件服务,setIsDir禁用目录列表，精确匹配路径
    server->serveStatic("/css/", LittleFS, "/www/css/")
        ->setIsDir(false)
        ->setCacheControl("max-age=3600");

    server->serveStatic("/js/", LittleFS, "/www/js/")
        ->setIsDir(false)
        ->setCacheControl("max-age=3600");

    server->serveStatic("/assets/", LittleFS, "/www/assets/")
        ->setIsDir(false)
        ->setCacheControl("max-age=86400");
    
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

    // 网络配置API路由
    server->on("/api/network/config", HTTP_GET, [this](AsyncWebServerRequest* request) {
        this->handleAPINetworkConfig(request);
    });
    server->on("/api/network/config", HTTP_POST, [this](AsyncWebServerRequest* request) {
        this->handleAPINetworkConfig(request);
    });

    server->on("/api/network/config/apply", HTTP_POST, [this](AsyncWebServerRequest* request) {
        this->handleAPINetworkApply(request);
    });

    server->on("/api/network/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        this->handleAPINetworkStatus(request);
    });

    server->on("/api/network/scan", HTTP_GET, [this](AsyncWebServerRequest* request) {
        this->handleAPINetworkScan(request);
    });

    server->on("/api/network/test-connection", HTTP_GET, [this](AsyncWebServerRequest* request) {
        this->handleAPINetworkTest(request);
    });

    server->on("/api/network/reset", HTTP_POST, [this](AsyncWebServerRequest* request) {
        this->handleAPINetworkReset(request);
    });

    server->on("/api/network/generate-backup-ips", HTTP_POST, [this](AsyncWebServerRequest* request) {
        this->handleAPIGenerateBackupIPs(request);
    });

    server->on("/api/network/switch-random-ip", HTTP_POST, [this](AsyncWebServerRequest* request) {
        this->handleAPISwitchRandomIP(request);
    });

    server->on("/api/network/check-conflict", HTTP_GET, [this](AsyncWebServerRequest* request) {
        this->handleAPIIPConflictCheck(request);
    });

    server->on("/api/network/failover", HTTP_POST, [this](AsyncWebServerRequest* request) {
        this->handleAPIFailover(request);
    });

    server->on("/api/network/diagnostic", HTTP_GET, [this](AsyncWebServerRequest* request) {
        this->handleAPINetworkDiagnostic(request);
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


/**
 * @brief 发送成功响应
 * @param request HTTP请求
 * @param jsonBody JSON响应体
 */
void WebConfigManager::sendSuccessResponse(AsyncWebServerRequest* request, const String& jsonBody) {
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", jsonBody);
    
    // 添加必要的响应头
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
    
    request->send(response);
}

/**
 * @brief 发送错误响应
 * @param request HTTP请求
 * @param errorCode 错误代码
 * @param errorMessage 错误消息
 */
void WebConfigManager::sendErrorResponse(AsyncWebServerRequest* request, int errorCode, const String& errorMessage) {
    DynamicJsonDocument doc(256);
    doc["error"] = errorMessage;
    doc["code"] = errorCode;
    doc["timestamp"] = millis();
    
    String jsonResponse;
    serializeJson(doc, jsonResponse);
    
    AsyncWebServerResponse* response = request->beginResponse(errorCode, "application/json", jsonResponse);
    
    // 添加必要的响应头
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
    
    request->send(response);
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

/**
 * @brief 处理网络配置获取/更新请求
 */
void WebConfigManager::handleAPINetworkConfig(AsyncWebServerRequest* request) {
    LOG_INFO("[API] 处理网络配置请求: " + String(request->method()) + " " + request->url());
    
    // 1. 认证检查
    if (!isAuthenticated(request)) {
        LOG_WARNING("[API] 未认证的网络配置请求");
        sendErrorResponse(request, 401, "未认证，请先登录");
        return;
    }

    // 2. 获取NetworkManager实例
    NetworkManager* networkManager = FastBeeFramework::getInstance()->getNetworkManager();
    if (!networkManager) {
        LOG_ERROR("[API] 网络管理器未初始化");
        sendErrorResponse(request, 500, "网络服务不可用");
        return;
    }

    // 3. 根据请求方法处理
    if (request->method() == HTTP_GET) {
        handleAPINetworkConfig_GET(request, networkManager);
    } else if (request->method() == HTTP_POST) {
        handleAPINetworkConfig_POST(request, networkManager);
    } else {
        sendErrorResponse(request, 405, "不支持的HTTP方法");
    }
}

/**
 * @brief 处理GET请求 - 获取网络配置和状态
 */
void WebConfigManager::handleAPINetworkConfig_GET(AsyncWebServerRequest* request, NetworkManager* networkManager) {
    LOG_DEBUG("[API] 获取网络配置");
    
    try {
        // 1. 获取配置和状态
        WiFiConfig config = networkManager->getConfig();
        NetworkStatusInfo status = networkManager->getStatusInfo();
        
        // 2. 创建JSON文档
        DynamicJsonDocument doc(4096);
        
        // 3. 使用辅助类序列化完整信息
        String jsonResponse = JsonSerializationHelper::fullNetworkInfoToJson(config, status, doc);
        
        LOG_DEBUG("[API] 生成的JSON大小: " + String(jsonResponse.length()) + " 字节");
        
        // 4. 发送成功响应
        sendSuccessResponse(request, jsonResponse);
        
        LOG_INFO("[API] 网络配置获取成功");
        
    } catch (const std::exception& e) {
        LOG_ERROR("[API] 获取网络配置异常: " + String(e.what()));
        sendErrorResponse(request, 500, "服务器内部错误: " + String(e.what()));
    } catch (...) {
        LOG_ERROR("[API] 获取网络配置时发生未知异常");
        sendErrorResponse(request, 500, "服务器内部错误");
    }
}

/**
 * @brief 处理POST请求 - 更新网络配置
 */
void WebConfigManager::handleAPINetworkConfig_POST(AsyncWebServerRequest* request, NetworkManager* networkManager) {
    LOG_DEBUG("[API] 更新网络配置");
    
    // 1. 检查Content-Type
    if (request->contentType() != "application/json") {
        LOG_WARNING("[API] 无效的Content-Type: " + String(request->contentType()));
        sendErrorResponse(request, 400, "Content-Type必须是application/json");
        return;
    }
    
    // 2. 获取请求体
    String body = request->arg("plain");
    if (body.length() == 0) {
        LOG_WARNING("[API] 请求体为空");
        sendErrorResponse(request, 400, "请求体不能为空");
        return;
    }
    
    LOG_DEBUG("[API] 收到配置JSON (长度: " + String(body.length()) + " 字节)");
    
    try {
        // 3. 解析JSON
        DynamicJsonDocument doc(2048);
        DeserializationError error = deserializeJson(doc, body);
        
        if (error) {
            LOG_ERROR("[API] JSON解析失败: " + String(error.c_str()));
            sendErrorResponse(request, 400, "JSON解析失败: " + String(error.c_str()));
            return;
        }
        
        // 4. 验证JSON结构
        if (!doc.is<JsonObject>()) {
            LOG_WARNING("[API] JSON不是对象格式");
            sendErrorResponse(request, 400, "JSON必须是对象格式");
            return;
        }
        
        JsonObject jsonObj = doc.as<JsonObject>();
        
        // 5. 获取当前配置作为基础
        WiFiConfig currentConfig = networkManager->getConfig();
        WiFiConfig newConfig = currentConfig; // 复制当前配置
        
        // 6. 使用辅助类解析配置
        if (!JsonSerializationHelper::wifiConfigFromJson(jsonObj, newConfig)) {
            LOG_ERROR("[API] 配置解析失败");
            sendErrorResponse(request, 400, "配置解析失败");
            return;
        }
        
        // 7. 检查配置是否有变化
        bool configChanged = false;
        bool restartRequired = false;
        
        // 检查关键配置变化
        if (newConfig.mode != currentConfig.mode) {
            configChanged = true;
            restartRequired = true;
            LOG_INFO("[API] 网络模式变更: " + String(static_cast<uint8_t>(currentConfig.mode)) + 
                    " -> " + String(static_cast<uint8_t>(newConfig.mode)));
        }
        
        if (newConfig.staSSID != currentConfig.staSSID) {
            configChanged = true;
            LOG_INFO("[API] WiFi SSID变更: " + currentConfig.staSSID + " -> " + newConfig.staSSID);
        }
        
        if (newConfig.ipConfigType != currentConfig.ipConfigType) {
            configChanged = true;
            LOG_INFO("[API] IP配置类型变更");
        }
        
        // 8. 如果配置有变化，则更新
        if (configChanged) {
            LOG_INFO("[API] 正在更新网络配置...");
            
            // 更新配置
            bool updateSuccess = networkManager->updateConfig(newConfig, true);
            
            if (updateSuccess) {
                LOG_INFO("[API] 网络配置更新成功");
                
                // 创建响应
                DynamicJsonDocument responseDoc(1024);
                JsonObject responseObj = responseDoc.to<JsonObject>();
                
                responseObj["success"] = true;
                responseObj["message"] = "网络配置更新成功";
                responseObj["requiresRestart"] = restartRequired;
                
                // 如果不需要重启，立即获取新状态
                if (!restartRequired) {
                    NetworkStatusInfo newStatus = networkManager->getStatusInfo();
                    JsonObject statusObj = responseObj.createNestedObject("newStatus");
                    JsonSerializationHelper::networkStatusToJson(newStatus, statusObj);
                } else {
                    responseObj["restartNote"] = "网络模式变更需要重启网络连接";
                }
                
                String responseJson;
                serializeJson(responseDoc, responseJson);
                sendSuccessResponse(request, responseJson);
                
            } else {
                LOG_ERROR("[API] 网络配置更新失败");
                sendErrorResponse(request, 500, "网络配置更新失败");
            }
        } else {
            LOG_DEBUG("[API] 配置无变化，跳过更新");
            
            // 配置无变化，返回当前状态
            DynamicJsonDocument responseDoc(512);
            JsonObject responseObj = responseDoc.to<JsonObject>();
            
            responseObj["success"] = true;
            responseObj["message"] = "配置无变化";
            responseObj["configChanged"] = false;
            
            String responseJson;
            serializeJson(responseDoc, responseJson);
            sendSuccessResponse(request, responseJson);
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("[API] 更新网络配置异常: " + String(e.what()));
        sendErrorResponse(request, 500, "服务器内部错误: " + String(e.what()));
    } catch (...) {
        LOG_ERROR("[API] 更新网络配置时发生未知异常");
        sendErrorResponse(request, 500, "服务器内部错误");
    }
}


/**
 * @brief 处理网络配置应用请求（重启网络）
 */
void WebConfigManager::handleAPINetworkApply(AsyncWebServerRequest* request) {
    LOG_INFO("[API] 处理网络配置应用请求");
    
    if (!isAuthenticated(request)) {
        sendErrorResponse(request, 401, "未认证");
        return;
    }

    NetworkManager* networkManager = FastBeeFramework::getInstance()->getNetworkManager();
    if (!networkManager) {
        sendErrorResponse(request, 500, "网络服务不可用");
        return;
    }

    try {
        // 重启网络
        LOG_INFO("[API] 正在重启网络...");
        bool restartSuccess = networkManager->restartNetwork();
        
        if (restartSuccess) {
            LOG_INFO("[API] 网络重启成功");
            
            // 等待网络稳定
            delay(2000);
            
            // 获取新的状态
            NetworkStatusInfo status = networkManager->getStatusInfo();
            
            DynamicJsonDocument doc(1024);
            JsonObject root = doc.to<JsonObject>();
            
            root["success"] = true;
            root["message"] = "网络配置已应用并重启";
            root["newIP"] = status.ipAddress;
            root["newStatus"] = static_cast<uint8_t>(status.status);
            
            String responseJson;
            serializeJson(doc, responseJson);
            sendSuccessResponse(request, responseJson);
        } else {
            LOG_ERROR("[API] 网络重启失败");
            sendErrorResponse(request, 500, "网络重启失败，请检查配置");
        }
    } catch (...) {
        LOG_ERROR("[API] 网络应用过程中发生异常");
        sendErrorResponse(request, 500, "应用配置时发生内部错误");
    }
}

/**
 * @brief 处理网络状态请求
 */
void WebConfigManager::handleAPINetworkStatus(AsyncWebServerRequest* request) {
    LOG_DEBUG("[API] 处理网络状态请求");
    
    if (!isAuthenticated(request)) {
        sendErrorResponse(request, 401, "未认证");
        return;
    }

    NetworkManager* networkManager = FastBeeFramework::getInstance()->getNetworkManager();
    if (!networkManager) {
        sendErrorResponse(request, 500, "网络服务不可用");
        return;
    }

    try {
        // 获取状态
        NetworkStatusInfo status = networkManager->getStatusInfo();
        
        DynamicJsonDocument doc(2048);
        JsonObject root = doc.to<JsonObject>();
        
        // 使用辅助类序列化状态
        JsonSerializationHelper::networkStatusToJson(status, root);
        
        // 添加额外信息
        root["timestamp"] = millis();
        root["uptime"] = millis();
        root["freeHeap"] = ESP.getFreeHeap();
        root["wifiMode"] = NetworkManager::getWiFiModeString();
        
        // 检查互联网连接
        bool internetAvailable = networkManager->checkInternetConnection();
        root["internetAvailable"] = internetAvailable;
        
        String responseJson;
        serializeJson(doc, responseJson);
        sendSuccessResponse(request, responseJson);
        
    } catch (...) {
        LOG_ERROR("[API] 获取网络状态时发生异常");
        sendErrorResponse(request, 500, "获取状态时发生内部错误");
    }
}

/**
 * @brief 处理WiFi网络扫描请求
 */
void WebConfigManager::handleAPINetworkScan(AsyncWebServerRequest* request) {
    LOG_INFO("[API] 处理WiFi扫描请求");
    
    if (!isAuthenticated(request)) {
        sendErrorResponse(request, 401, "未认证");
        return;
    }

    NetworkManager* networkManager = FastBeeFramework::getInstance()->getNetworkManager();
    if (!networkManager) {
        sendErrorResponse(request, 500, "网络服务不可用");
        return;
    }

    try {
        // 获取当前状态
        NetworkStatusInfo currentStatus = networkManager->getStatusInfo();
        
        LOG_INFO("[API] 开始扫描WiFi网络...");
        
        // 执行扫描
        String scanResult = networkManager->scanNetworks();
        
        LOG_INFO("[API] WiFi扫描完成");
        
        // 解析扫描结果
        DynamicJsonDocument doc(4096);
        DeserializationError error = deserializeJson(doc, scanResult);
        
        if (!error) {
            // 添加扫描元数据
            doc["scanTime"] = millis();
            doc["networksFound"] = doc.size();
            doc["currentNetwork"] = currentStatus.ssid;
            doc["currentRSSI"] = currentStatus.rssi;
            
            String responseJson;
            serializeJson(doc, responseJson);
            sendSuccessResponse(request, responseJson);
        } else {
            // 返回原始结果
            sendSuccessResponse(request, scanResult);
        }
    } catch (...) {
        LOG_ERROR("[API] WiFi扫描发生未知异常");
        sendErrorResponse(request, 500, "扫描过程中发生内部错误");
    }
}

/**
 * @brief 处理网络连接测试请求
 */
void WebConfigManager::handleAPINetworkTest(AsyncWebServerRequest* request) {
    LOG_INFO("[API] 处理网络测试请求");
    
    if (!isAuthenticated(request)) {
        sendErrorResponse(request, 401, "未认证");
        return;
    }

    NetworkManager* networkManager = FastBeeFramework::getInstance()->getNetworkManager();
    if (!networkManager) {
        sendErrorResponse(request, 500, "网络服务不可用");
        return;
    }

    try {
        // 获取当前状态
        NetworkStatusInfo status = networkManager->getStatusInfo();
        
        DynamicJsonDocument doc(1024);
        JsonObject root = doc.to<JsonObject>();
        
        root["testTime"] = millis();
        
        // 基本连接状态
        bool isConnected = (status.status == NetworkStatus::CONNECTED);
        root["wifiConnected"] = isConnected;
        root["wifiSSID"] = status.ssid;
        root["wifiRSSI"] = status.rssi;
        root["signalStrength"] = NetworkManager::rssiToPercentage(status.rssi);
        root["localIP"] = status.ipAddress;
        
        // 网络评估
        int score = 0;
        if (isConnected) score += 40;
        if (status.rssi > -70) score += 30;
        if (status.internetAvailable) score += 30;
        
        root["testScore"] = score;
        root["healthStatus"] = (score >= 80) ? "健康" : 
                              (score >= 60) ? "一般" : "差";
        
        // 建议
        JsonArray suggestions = root.createNestedArray("suggestions");
        if (!isConnected) {
            suggestions.add("WiFi未连接，请检查SSID和密码");
        }
        if (status.rssi < -80) {
            suggestions.add("信号强度较弱，请靠近路由器或检查天线");
        }
        if (!status.internetAvailable && isConnected) {
            suggestions.add("互联网连接不可用，请检查路由器外网连接");
        }
        
        String responseJson;
        serializeJson(doc, responseJson);
        sendSuccessResponse(request, responseJson);
        
        LOG_INFO("[API] 网络测试完成，得分: " + String(score));
        
    } catch (...) {
        LOG_ERROR("[API] 网络测试过程中发生异常");
        sendErrorResponse(request, 500, "网络测试失败");
    }
}

/**
 * @brief 处理网络配置重置请求
 */
void WebConfigManager::handleAPINetworkReset(AsyncWebServerRequest* request) {
    LOG_WARNING("[API] 处理网络配置重置请求");
    
    if (!isAuthenticated(request)) {
        sendErrorResponse(request, 401, "未认证");
        return;
    }

    NetworkManager* networkManager = FastBeeFramework::getInstance()->getNetworkManager();
    if (!networkManager) {
        sendErrorResponse(request, 500, "网络服务不可用");
        return;
    }

    try {
        // 需要确认参数
        bool confirmed = false;
        if (request->hasParam("confirm")) {
            String confirmValue = request->getParam("confirm")->value();
            confirmed = (confirmValue == "true" || confirmValue == "1" || confirmValue == "yes");
        }
        
        if (!confirmed) {
            sendErrorResponse(request, 400, "需要确认，请添加confirm=true参数");
            return;
        }
        
        LOG_INFO("[API] 正在重置网络配置...");
        bool resetSuccess = networkManager->resetToDefaults();
        
        if (resetSuccess) {
            LOG_INFO("[API] 网络配置重置成功");
            
            DynamicJsonDocument doc(256);
            doc["success"] = true;
            doc["message"] = "网络配置已重置为默认值";
            
            String responseJson;
            serializeJson(doc, responseJson);
            sendSuccessResponse(request, responseJson);
        } else {
            LOG_ERROR("[API] 网络配置重置失败");
            sendErrorResponse(request, 500, "重置网络配置失败");
        }
    } catch (...) {
        LOG_ERROR("[API] 重置网络配置时发生异常");
        sendErrorResponse(request, 500, "重置过程中发生内部错误");
    }
}

/**
 * @brief 处理生成备用IP请求
 */
void WebConfigManager::handleAPIGenerateBackupIPs(AsyncWebServerRequest* request) {
    LOG_INFO("[API] 处理生成备用IP请求");
    
    if (!isAuthenticated(request)) {
        request->send(401, "application/json", "{\"error\":\"未认证\"}");
        return;
    }

    if (request->method() != HTTP_POST) {
        request->send(405, "application/json", "{\"error\":\"只支持POST方法\"}");
        return;
    }

    String body = request->arg("plain");
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, body);
    
    if (error) {
        request->send(400, "application/json", "{\"error\":\"无效的JSON格式\"}");
        return;
    }
    
    try {
        // 验证必要参数
        if (!doc.containsKey("staticIP") || !doc.containsKey("subnet") || !doc.containsKey("gateway")) {
            request->send(400, "application/json", 
                "{\"error\":\"缺少必要参数: staticIP, subnet, gateway\"}");
            return;
        }
        
        String staticIP = doc["staticIP"].as<String>();
        String subnet = doc["subnet"].as<String>();
        String gateway = doc["gateway"].as<String>();
        
        // 验证IP格式
        if (!NetworkManager::isValidIP(staticIP) || 
            !NetworkManager::isValidSubnet(subnet) || 
            !NetworkManager::isValidIP(gateway)) {
            request->send(400, "application/json", 
                "{\"error\":\"IP地址或子网掩码格式无效\"}");
            return;
        }
        
        NetworkManager* networkManager = FastBeeFramework::getInstance()->getNetworkManager();
        if (!networkManager) {
            request->send(500, "application/json", "{\"error\":\"网络服务不可用\"}");
            return;
        }
        
        // 模拟生成备用IP（实际应该调用NetworkManager的方法）
        // 这里简化处理，实际应该根据网络配置计算
        std::vector<String> backupIPs;
        
        // 解析IP地址
        IPAddress ip, netmask, gw;
        ip.fromString(staticIP.c_str());
        netmask.fromString(subnet.c_str());
        gw.fromString(gateway.c_str());
        
        // 计算网络地址
        IPAddress network(ip[0] & netmask[0], 
                         ip[1] & netmask[1], 
                         ip[2] & netmask[2], 
                         ip[3] & netmask[3]);
        
        // 生成3个备用IP（避免.0、.1、.255和网关）
        int generated = 0;
        for (int i = 2; i < 254 && generated < 3; i++) {
            // 跳过网关和当前IP
            if (i == gw[3] || i == ip[3]) continue;
            
            IPAddress backup(network[0], network[1], network[2], i);
            backupIPs.push_back(backup.toString());
            generated++;
        }
        
        // 返回结果
        DynamicJsonDocument resultDoc(1024);
        resultDoc["success"] = true;
        resultDoc["message"] = "备用IP生成成功";
        resultDoc["count"] = generated;
        
        JsonArray ips = resultDoc.createNestedArray("backupIPs");
        for (const String& ip : backupIPs) {
            ips.add(ip);
        }
        
        // 更新NetworkManager的配置
        WiFiConfig config = networkManager->getConfig();
        config.backupIPs = backupIPs;
        networkManager->updateConfig(config, true);
        
        String response;
        serializeJson(resultDoc, response);
        request->send(200, "application/json", response);
        
        LOG_INFO("[API] 生成备用IP成功，数量: " + String(generated));
        
    } catch (...) {
        LOG_ERROR("[API] 生成备用IP时发生异常");
        request->send(500, "application/json", "{\"error\":\"生成备用IP失败\"}");
    }
}

/**
 * @brief 处理切换到随机IP请求
 */
void WebConfigManager::handleAPISwitchRandomIP(AsyncWebServerRequest* request) {
    LOG_INFO("[API] 处理切换到随机IP请求");
    
    if (!isAuthenticated(request)) {
        request->send(401, "application/json", "{\"error\":\"未认证\"}");
        return;
    }

    if (request->method() != HTTP_POST) {
        request->send(405, "application/json", "{\"error\":\"只支持POST方法\"}");
        return;
    }

    NetworkManager* networkManager = FastBeeFramework::getInstance()->getNetworkManager();
    if (!networkManager) {
        request->send(500, "application/json", "{\"error\":\"网络服务不可用\"}");
        return;
    }

    try {
        // 检查当前网络状态
        NetworkStatusInfo status = networkManager->getStatusInfo();
        if (status.status != NetworkStatus::CONNECTED) {
            request->send(400, "application/json", 
                "{\"error\":\"当前未连接到网络，无法切换IP\"}");
            return;
        }
        
        // 获取当前配置
        WiFiConfig config = networkManager->getConfig();
        if (config.ipConfigType != IPConfigType::STATIC) {
            request->send(400, "application/json", 
                "{\"error\":\"当前使用DHCP，无需切换IP\"}");
            return;
        }
        
        // 生成随机IP
        String randomIP = networkManager->getRandomIPInRange(config.staticIP, config.subnet);
        if (randomIP.isEmpty()) {
            request->send(500, "application/json", 
                "{\"error\":\"无法生成随机IP，请检查网络配置\"}");
            return;
        }
        
        LOG_INFO("[API] 生成随机IP: " + randomIP);
        
        // 切换到随机IP
        bool switchSuccess = networkManager->switchToRandomIP();
        
        if (switchSuccess) {
            // 等待网络重新连接
            delay(2000);
            
            // 获取新状态
            NetworkStatusInfo newStatus = networkManager->getStatusInfo();
            
            DynamicJsonDocument doc(512);
            doc["success"] = true;
            doc["message"] = "已切换到随机IP";
            doc["oldIP"] = status.ipAddress;
            doc["newIP"] = newStatus.ipAddress;
            doc["randomIP"] = randomIP;
            
            String response;
            serializeJson(doc, response);
            request->send(200, "application/json", response);
            
            LOG_INFO("[API] 成功切换到随机IP: " + newStatus.ipAddress);
        } else {
            request->send(500, "application/json", 
                "{\"error\":\"切换到随机IP失败\"}");
        }
    } catch (...) {
        LOG_ERROR("[API] 切换到随机IP时发生异常");
        request->send(500, "application/json", "{\"error\":\"切换IP过程中发生内部错误\"}");
    }
}

/**
 * @brief 处理IP冲突检测请求
 */
void WebConfigManager::handleAPIIPConflictCheck(AsyncWebServerRequest* request) {
    LOG_INFO("[API] 处理IP冲突检测请求");
    
    if (!isAuthenticated(request)) {
        request->send(401, "application/json", "{\"error\":\"未认证\"}");
        return;
    }

    NetworkManager* networkManager = FastBeeFramework::getInstance()->getNetworkManager();
    if (!networkManager) {
        request->send(500, "application/json", "{\"error\":\"网络服务不可用\"}");
        return;
    }

    try {
        bool hasConflict = networkManager->checkIPConflict();
        
        DynamicJsonDocument doc(512);
        doc["hasConflict"] = hasConflict;
        doc["detectionTime"] = millis();
        
        if (hasConflict) {
            doc["message"] = "检测到IP地址冲突";
            doc["recommendation"] = "建议切换到备用IP或使用DHCP";
            
            // 获取备用IP列表
            WiFiConfig config = networkManager->getConfig();
            if (!config.backupIPs.empty()) {
                JsonArray backups = doc.createNestedArray("availableBackups");
                for (const String& ip : config.backupIPs) {
                    backups.add(ip);
                }
            }
        } else {
            doc["message"] = "未检测到IP地址冲突";
        }
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
        
    } catch (...) {
        LOG_ERROR("[API] IP冲突检测时发生异常");
        request->send(500, "application/json", "{\"error\":\"冲突检测失败\"}");
    }
}

/**
 * @brief 处理故障转移请求
 */
void WebConfigManager::handleAPIFailover(AsyncWebServerRequest* request) {
    LOG_INFO("[API] 处理故障转移请求");
    
    if (!isAuthenticated(request)) {
        request->send(401, "application/json", "{\"error\":\"未认证\"}");
        return;
    }

    if (request->method() != HTTP_POST) {
        request->send(405, "application/json", "{\"error\":\"只支持POST方法\"}");
        return;
    }

    NetworkManager* networkManager = FastBeeFramework::getInstance()->getNetworkManager();
    if (!networkManager) {
        request->send(500, "application/json", "{\"error\":\"网络服务不可用\"}");
        return;
    }

    try {
        String body = request->arg("plain");
        DynamicJsonDocument doc(256);
        DeserializationError error = deserializeJson(doc, body);
        
        String targetIP;
        if (!error && doc.containsKey("targetIP")) {
            targetIP = doc["targetIP"].as<String>();
        }
        
        bool success = false;
        String message;
        
        if (!targetIP.isEmpty()) {
            // 切换到指定IP
            LOG_INFO("[API] 切换到指定IP: " + targetIP);
            
            // 这里需要实现切换到指定IP的逻辑
            // 由于NetworkManager可能没有直接的方法，这里简化处理
            success = networkManager->switchToBackupIP(); // 使用默认故障转移
            message = "正在尝试故障转移";
        } else {
            // 使用自动故障转移
            LOG_INFO("[API] 执行自动故障转移");
            success = networkManager->switchToBackupIP();
            message = "正在执行自动故障转移";
        }
        
        // 等待网络稳定
        delay(2000);
        
        // 获取新状态
        NetworkStatusInfo status = networkManager->getStatusInfo();
        
        DynamicJsonDocument resultDoc(512);
        resultDoc["success"] = success;
        resultDoc["message"] = message;
        resultDoc["newIP"] = status.ipAddress;
        resultDoc["newStatus"] = static_cast<uint8_t>(status.status);
        
        String response;
        serializeJson(resultDoc, response);
        request->send(200, "application/json", response);
        
    } catch (...) {
        LOG_ERROR("[API] 故障转移时发生异常");
        request->send(500, "application/json", "{\"error\":\"故障转移失败\"}");
    }
}

/**
 * @brief 处理网络诊断报告请求
 */
void WebConfigManager::handleAPINetworkDiagnostic(AsyncWebServerRequest* request) {
    LOG_INFO("[API] 处理网络诊断报告请求");
    
    if (!isAuthenticated(request)) {
        request->send(401, "application/json", "{\"error\":\"未认证\"}");
        return;
    }

    NetworkManager* networkManager = FastBeeFramework::getInstance()->getNetworkManager();
    if (!networkManager) {
        request->send(500, "application/json", "{\"error\":\"网络服务不可用\"}");
        return;
    }

    try {
        // 获取详细的状态信息
        NetworkStatusInfo status = networkManager->getStatusInfo();
        WiFiConfig config = networkManager->getConfig();
        
        DynamicJsonDocument doc(4096);
        
        // 基本信息
        doc["diagnosticTime"] = millis();
        doc["deviceID"] = NetworkManager::getChipID();
        doc["deviceName"] = config.deviceName;
        
        // 网络状态
        doc["networkStatus"] = static_cast<uint8_t>(status.status);
        doc["statusText"] = getNetworkStatusText(status.status);
        doc["ipAddress"] = status.ipAddress;
        doc["macAddress"] = status.macAddress;
        doc["ssid"] = status.ssid;
        doc["rssi"] = status.rssi;
        doc["signalStrength"] = networkManager->rssiToPercentage(status.rssi);
        doc["gateway"] = status.currentGateway;
        doc["subnet"] = status.currentSubnet;
        doc["dns"] = status.dnsServer;
        doc["internetAvailable"] = status.internetAvailable;
        
        // 配置信息
        doc["wifiMode"] = config.mode;
        doc["ipConfigType"] = config.ipConfigType;
        doc["staticIP"] = config.staticIP;
        doc["enableMDNS"] = config.enableMDNS;
        doc["customDomain"] = config.customDomain;
        
        // IP冲突信息
        doc["conflictDetection"] = config.conflictDetection;
        doc["autoFailover"] = config.autoFailover;
        doc["failoverCount"] = status.failoverCount;
        
        // 备用IP列表
        JsonArray backupIPs = doc.createNestedArray("backupIPs");
        for (const String& ip : config.backupIPs) {
            backupIPs.add(ip);
        }
        
        // 系统信息
        doc["freeHeap"] = ESP.getFreeHeap();
        doc["heapFragmentation"] = ESP.getHeapSize() - ESP.getFreeHeap();
        doc["uptime"] = millis();
        doc["resetReason"] = esp_reset_reason();
        
        // 网络事件日志
        JsonArray events = doc.createNestedArray("recentEvents");
        // 这里可以添加最近的事件记录
        
        // 诊断结果
        JsonArray issues = doc.createNestedArray("identifiedIssues");
        JsonArray recommendations = doc.createNestedArray("recommendations");
        
        // 分析问题
        if (status.status != NetworkStatus::CONNECTED) {
            issues.add("设备未连接到WiFi网络");
            recommendations.add("检查WiFi配置并重新连接");
        }
        
        if (status.rssi < -80) {
            issues.add("WiFi信号强度较弱 (" + String(status.rssi) + " dBm)");
            recommendations.add("将设备靠近路由器或检查天线连接");
        }
        
        if (config.ipConfigType == IPConfigType::STATIC && config.staticIP.isEmpty()) {
            issues.add("静态IP配置不完整");
            recommendations.add("配置完整的静态IP、网关和子网掩码");
        }
        
        if (config.backupIPs.empty() && config.ipConfigType == IPConfigType::STATIC) {
            issues.add("未配置备用IP地址");
            recommendations.add("生成备用IP列表以提高网络可靠性");
        }
        
        // 总体评估
        int healthScore = 100;
        if (issues.size() > 0) healthScore -= (issues.size() * 20);
        if (healthScore < 0) healthScore = 0;
        
        doc["healthScore"] = healthScore;
        doc["healthStatus"] = (healthScore >= 80) ? "健康" : 
                              (healthScore >= 60) ? "警告" : "故障";
        
        // 生成报告ID
        char reportID[20];
        snprintf(reportID, sizeof(reportID), "DIA%08X", (uint32_t)millis());
        doc["reportID"] = String(reportID);
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
        
        LOG_INFO("[API] 网络诊断报告生成完成，健康分数: " + String(healthScore));
        
    } catch (...) {
        LOG_ERROR("[API] 生成网络诊断报告时发生异常");
        request->send(500, "application/json", "{\"error\":\"诊断报告生成失败\"}");
    }
}

// ==================== 辅助函数 ====================

/**
 * @brief 获取网络状态文本描述
 */
String WebConfigManager::getNetworkStatusText(NetworkStatus status) {
    switch (status) {
        case NetworkStatus::DISCONNECTED: return "未连接";
        case NetworkStatus::CONNECTING: return "连接中";
        case NetworkStatus::CONNECTED: return "已连接";
        case NetworkStatus::AP_MODE: return "热点模式";
        case NetworkStatus::CONNECTION_FAILED: return "连接失败";
        case NetworkStatus::IP_CONFLICT: return "IP冲突";
        case NetworkStatus::FAILOVER_IN_PROGRESS: return "故障转移中";
        default: return "未知状态";
    }
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

