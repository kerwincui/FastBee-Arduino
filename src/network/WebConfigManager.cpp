#include "./network/WebConfigManager.h"
#include "./security/AuthManager.h"
#include "./security/UserManager.h"
#include "./security/RoleManager.h"
#include "./network/NetworkManager.h"
#include "./network/OTAManager.h"
#include "./protocols/ProtocolManager.h"
#include "systems/LoggerSystem.h"
#include "core/GPIOManager.h"
#include "core/ConfigDefines.h"
#include "utils/NetworkUtils.h"
#include <time.h>
#include <esp_wifi.h>
#include <NimBLEDevice.h>
#include <Update.h>

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
    LOG_INFO("WebConfig: Initializing...");
    
    if (!server) {
        LOG_ERROR("WebConfig: Web server is null");
        return false;
    }

    if (!authManager || !userManager) {
        LOG_ERROR("WebConfig: AuthManager or UserManager is null");
        return false;
    }

    LOG_DEBUG("WebConfig: Loading configuration...");
    loadConfiguration();

    // LittleFS 由 ConfigStorage 统一挂载，此处仅检测挂载状态
    size_t total = LittleFS.totalBytes();
    if (total == 0) {
        LOG_WARNING("WebConfig: LittleFS not mounted, static files unavailable");
        // 不作为致命错误，API 仍可用
    } else {
        LOG_DEBUGF("WebConfig: LittleFS mounted (total=%lu bytes)", (unsigned long)total);
    }

    LOG_DEBUG("WebConfig: Setting up routes...");
    setupStaticRoutes();
    LOG_DEBUG("WebConfig: Static routes OK");
    
    setupAuthRoutes();
    LOG_DEBUG("WebConfig: Auth routes OK");
    
    setupUserRoutes();
    LOG_DEBUG("WebConfig: User routes OK");
    
    setupRoleRoutes();
    LOG_DEBUG("WebConfig: Role routes OK");
    
    setupSystemRoutes();
    LOG_DEBUG("WebConfig: System routes OK");
    
    setupAPIRoutes();
    LOG_DEBUG("WebConfig: API routes OK");

    LOG_DEBUG("WebConfig: Starting web server...");
    start();

    LOG_INFO("WebConfig: Routes configured and server started");
    return true;
}

bool WebConfigManager::start() {
    if (!server) {
        LOG_ERROR("WebConfig: Server is null!");
        return false;
    }
    LOG_DEBUG("WebConfig: Calling server->begin()...");
    server->begin();
    LOG_DEBUG("WebConfig: server->begin() completed");
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
    // 静态文件服务，禁用 gzip 优先查找（设备上没有 .gz 文件，避免崩溃）
    server->serveStatic("/css/", LittleFS, "/www/css/")
        .setIsDir(false).setTryGzipFirst(false).setCacheControl("no-cache");
    server->serveStatic("/js/", LittleFS, "/www/js/")
        .setIsDir(false).setTryGzipFirst(false).setCacheControl("no-cache");
    server->serveStatic("/assets/", LittleFS, "/www/assets/")
        .setIsDir(false).setTryGzipFirst(false).setCacheControl("max-age=86400");

    // favicon.ico 重定向到 assets 目录
    server->on("/favicon.ico", HTTP_GET, [this](AsyncWebServerRequest* request) {
        request->send(LittleFS, "/www/assets/favicon.ico", "image/x-icon");
    });

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
    
    // 恢复出厂设置
    server->on("/api/system/factory-reset", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIFactoryReset(request);
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
    
    // ============ 设备配置 API ============

    // 获取设备配置
    server->on("/api/device/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetDeviceConfig(request);
    });

    // 保存设备配置
    server->on("/api/device/config", HTTP_PUT,
              [this](AsyncWebServerRequest* request) {
        handleAPIUpdateDeviceConfig(request);
    });

    // 获取当前时间
    server->on("/api/device/time", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetDeviceTime(request);
    });
    
    // ============ AP配网 API ============
    
    // 获取配网状态
    server->on("/api/provision/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetProvisionStatus(request);
    });
    
    // 启动AP配网
    server->on("/api/provision/start", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIStartProvision(request);
    });
    
    // 停止AP配网
    server->on("/api/provision/stop", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIStopProvision(request);
    });
    
    // 获取配网配置
    server->on("/api/provision/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetProvisionConfig(request);
    });
    
    // 保存配网配置
    server->on("/api/provision/config", HTTP_PUT,
              [this](AsyncWebServerRequest* request) {
        handleAPISaveProvisionConfig(request);
    });
    
    // 配网回调接口（供手机APP调用）
    server->on("/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIProvisionCallback(request);
    });
    
    // 配网状态检测接口（供手机APP调用）
    server->on("/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        request->send(200, "text/plain;charset=utf-8", "AP配网已准备就绪");
    });
    
    // ============ 蓝牙配网 API ============
    
    // 获取蓝牙配网状态
    server->on("/api/ble/provision/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetBLEProvisionStatus(request);
    });
    
    // 启动蓝牙配网
    server->on("/api/ble/provision/start", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIStartBLEProvision(request);
    });
    
    // 停止蓝牙配网
    server->on("/api/ble/provision/stop", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIStopBLEProvision(request);
    });
    
    // 获取蓝牙配网配置
    server->on("/api/ble/provision/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetBLEProvisionConfig(request);
    });
    
    // 保存蓝牙配网配置
    server->on("/api/ble/provision/config", HTTP_PUT,
              [this](AsyncWebServerRequest* request) {
        handleAPISaveBLEProvisionConfig(request);
    });
    
    // ============ 文件管理 API ============
    
    // 获取文件列表
    server->on("/api/fs/list", HTTP_GET, 
              [this](AsyncWebServerRequest* request) {
        handleAPIGetFileList(request);
    });
    
    // 获取文件内容
    server->on("/api/fs/read", HTTP_GET, 
              [this](AsyncWebServerRequest* request) {
        handleAPIGetFileContent(request);
    });
    
    // 保存文件内容
    server->on("/api/fs/save", HTTP_POST, 
              [this](AsyncWebServerRequest* request) {
        handleAPISaveFileContent(request);
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
    
    // 网络实时状态
    server->on("/api/network/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetNetworkStatus(request);
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
    
    // OTA URL在线升级
    server->on("/api/ota/url", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIOtaUrl(request);
    });
    
    // OTA 固件上传
    server->on("/api/ota/upload", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            // 上传完成后的响应
            if (otaManager && otaManager->isOTAInProgress()) {
                sendSuccess(request, "Firmware uploading...");
            } else {
                sendSuccess(request, "Upload completed");
            }
        },
        [this](AsyncWebServerRequest* request, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
            handleAPIOtaUpload(request, filename, index, data, len, final);
        }
    );

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
    
    // ============ GPIO API ============
    
    // 获取GPIO配置列表
    server->on("/api/gpio/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetGPIOConfig(request);
    });
    
    // 配置GPIO
    server->on("/api/gpio/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIConfigureGPIO(request);
    });
    
    // 读取GPIO状态
    server->on("/api/gpio/read", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIReadGPIO(request);
    });
    
    // 写入GPIO状态
    server->on("/api/gpio/write", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIWriteGPIO(request);
    });
    
    // 删除GPIO配置
    server->on("/api/gpio/delete", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIDeleteGPIO(request);
    });
    
    // 保存GPIO配置
    server->on("/api/gpio/save", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPISaveGPIOConfig(request);
    });
    
    // ============ 外设接口 API ============
    // 注意：更具体的路由必须放在通用路由之前，否则会被通用路由捕获
    
    // 获取外设类型列表
    server->on("/api/peripherals/types", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetPeripheralTypes(request);
    });
    
    // 启用/禁用外设（必须在 /api/peripherals POST 之前注册）
    server->on("/api/peripherals/enable", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIEnablePeripheral(request);
    });
    
    server->on("/api/peripherals/disable", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIDisablePeripheral(request);
    });
    
    // 获取外设状态
    server->on("/api/peripherals/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetPeripheralStatus(request);
    });
    
    // 读取/写入外设数据
    server->on("/api/peripherals/read", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIReadPeripheral(request);
    });
    
    server->on("/api/peripherals/write", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIWritePeripheral(request);
    });
    
    // 获取/更新/删除单个外设（带斜杠的路径）
    server->on("/api/peripherals/", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetPeripheral(request);
    });
    
    server->on("/api/peripherals/", HTTP_PUT,
              [this](AsyncWebServerRequest* request) {
        handleAPIUpdatePeripheral(request);
    });
    
    server->on("/api/peripherals/", HTTP_DELETE,
              [this](AsyncWebServerRequest* request) {
        handleAPIDeletePeripheral(request);
    });
    
    // 获取所有外设（支持按类型过滤）- 通用GET路由
    server->on("/api/peripherals", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetPeripherals(request);
    });
    
    // 添加外设 - 通用POST路由（放在最后）
    server->on("/api/peripherals", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPIAddPeripheral(request);
    });
    
    // ============ 协议配置 API ============
    
    // 获取协议配置
    server->on("/api/protocol/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleAPIGetProtocolConfig(request);
    });
    
    // 保存协议配置
    server->on("/api/protocol/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleAPISaveProtocolConfig(request);
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
    // 第四个参数 false = 禁用 gzip 自动查找（设备上没有 .gz 文件）
    request->send(LittleFS, path, contentType, false);
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

        // 返回用户角色和文件管理权限，供前端 RBAC 控制
        if (userManager) {
            responseDoc["role"] = userManager->getUserRole(authResult.username);
        } else {
            responseDoc["role"] = "VIEWER";
        }
        responseDoc["canManageFs"] = authManager->checkSessionPermission(
            authResult.sessionId, "fs.manage");
        
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
    
    // ========== 设备基本信息 ==========
    doc["data"]["device"]["id"] = String((uint32_t)ESP.getEfuseMac(), HEX);
    doc["data"]["device"]["chipModel"] = ESP.getChipModel();
    doc["data"]["device"]["chipRevision"] = ESP.getChipRevision();
    doc["data"]["device"]["cpuFreqMHz"] = ESP.getCpuFreqMHz();
    doc["data"]["device"]["sdkVersion"] = ESP.getSdkVersion();
    doc["data"]["device"]["freeHeap"] = ESP.getFreeHeap();
    doc["data"]["device"]["flashSize"] = ESP.getFlashChipSize();
    doc["data"]["device"]["firmwareVersion"] = SystemInfo::VERSION;
    
    // ========== Flash 存储信息 ==========
    size_t flashSize = ESP.getFlashChipSize();
    size_t sketchSize = ESP.getSketchSize();
    size_t freeSketchSpace = ESP.getFreeSketchSpace();
    doc["data"]["flash"]["total"] = flashSize;
    doc["data"]["flash"]["speed"] = ESP.getFlashChipSpeed();
    doc["data"]["flash"]["sketchSize"] = sketchSize;
    doc["data"]["flash"]["freeSketchSpace"] = freeSketchSpace;
    doc["data"]["flash"]["used"] = sketchSize;
    doc["data"]["flash"]["free"] = freeSketchSpace;
    doc["data"]["flash"]["usagePercent"] = (int)((sketchSize * 100) / (sketchSize + freeSketchSpace));
    
    // ========== 内存信息 ==========
    size_t heapSize = ESP.getHeapSize();
    size_t freeHeap = ESP.getFreeHeap();
    size_t minFreeHeap = ESP.getMinFreeHeap();
    doc["data"]["memory"]["heapTotal"] = heapSize;
    doc["data"]["memory"]["heapFree"] = freeHeap;
    doc["data"]["memory"]["heapUsed"] = heapSize - freeHeap;
    doc["data"]["memory"]["heapMinFree"] = minFreeHeap;
    doc["data"]["memory"]["heapMaxAlloc"] = ESP.getMaxAllocHeap();
    doc["data"]["memory"]["heapUsagePercent"] = (int)(((heapSize - freeHeap) * 100) / heapSize);
    
    // PSRAM 信息（如果有）
    size_t psramSize = ESP.getPsramSize();
    if (psramSize > 0) {
        doc["data"]["memory"]["psramTotal"] = psramSize;
        doc["data"]["memory"]["psramFree"] = ESP.getFreePsram();
        doc["data"]["memory"]["psramMinFree"] = ESP.getMinFreePsram();
    }
    
    // ========== 文件系统信息 ==========
    size_t fsTotal = LittleFS.totalBytes();
    size_t fsUsed = LittleFS.usedBytes();
    doc["data"]["filesystem"]["total"] = fsTotal;
    doc["data"]["filesystem"]["used"] = fsUsed;
    doc["data"]["filesystem"]["free"] = fsTotal - fsUsed;
    doc["data"]["filesystem"]["usagePercent"] = fsTotal > 0 ? (int)((fsUsed * 100) / fsTotal) : 0;
    
    // ========== 运行时间 ==========
    unsigned long uptime = millis();
    doc["data"]["uptime"]["ms"] = uptime;
    doc["data"]["uptime"]["seconds"] = uptime / 1000;
    doc["data"]["uptime"]["formatted"] = formatUptime(uptime);
    
    // ========== 网络信息 ==========
    if (networkManager) {
        NetworkStatusInfo info = networkManager->getStatusInfo();
        doc["data"]["network"]["connected"] = (info.status == NetworkStatus::CONNECTED);
        doc["data"]["network"]["ipAddress"] = info.ipAddress;
        doc["data"]["network"]["ssid"] = info.ssid;
        doc["data"]["network"]["rssi"] = info.rssi;
        doc["data"]["network"]["macAddress"] = WiFi.macAddress();
    }
    
    // ========== 用户统计 ==========
    if (userManager) {
        doc["data"]["users"]["total"] = userManager->getUserCount();
    }
    
    if (authManager) {
        doc["data"]["users"]["activeSessions"] = authManager->getActiveSessionCount();
        doc["data"]["users"]["online"] = authManager->getOnlineUserCount();
    }
    
    doc["success"] = true;
    
    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
}

// 格式化运行时间
String WebConfigManager::formatUptime(unsigned long ms) {
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

void WebConfigManager::handleAPIFactoryReset(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.restart")) {
        sendUnauthorized(request);
        return;
    }

    LOGGER.info("Factory reset initiated by user");

    // 定义需要删除的配置文件列表
    const char* configFiles[] = {
        "/config/device.json",
        "/config/network.json",
        "/config/protocol.json",
        "/config/gpio.json",
        "/config/users.json",
        "/config/system.json",
        "/config/http.json",
        "/config/mqtt.json",
        "/config/tcp.json",
        "/config/modbus.json",
        "/config/coap.json"
    };

    // 删除所有配置文件
    int deletedCount = 0;
    for (int i = 0; i < sizeof(configFiles) / sizeof(configFiles[0]); i++) {
        if (LittleFS.exists(configFiles[i])) {
            if (LittleFS.remove(configFiles[i])) {
                deletedCount++;
                LOGGER.infof("Deleted config file: %s", configFiles[i]);
            } else {
                LOGGER.warningf("Failed to delete config file: %s", configFiles[i]);
            }
        }
    }

    // 清空日志文件
    if (LittleFS.exists("/logs/system.log")) {
        File logFile = LittleFS.open("/logs/system.log", "w");
        if (logFile) {
            logFile.close();
            LOGGER.info("Cleared system log file");
        }
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "Factory reset completed. Device will restart.";
    doc["deletedFiles"] = deletedCount;
    doc["timestamp"] = millis();

    sendJsonResponse(request, 200, doc);

    // 延迟2秒后重启
    delay(2000);
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
        
        // ========== 设备信息 ==========
        // 设备名称从 device.json 读取，不在网络配置中处理
        doc["data"]["device"]["macAddress"] = WiFi.macAddress();
        
        // ========== 网络模式 ==========
        doc["data"]["network"]["mode"] = static_cast<uint8_t>(cfg.mode);
        doc["data"]["network"]["ipConfigType"] = static_cast<uint8_t>(cfg.ipConfigType);
        doc["data"]["network"]["enableMDNS"] = cfg.enableMDNS;
        
        // ========== STA 配置 ==========
        doc["data"]["sta"]["ssid"] = cfg.staSSID;
        doc["data"]["sta"]["password"] = cfg.staPassword.length() > 0 ? "********" : "";
        doc["data"]["sta"]["hasPassword"] = cfg.staPassword.length() > 0;
        doc["data"]["sta"]["staticIP"] = cfg.staticIP;
        doc["data"]["sta"]["gateway"] = cfg.gateway;
        doc["data"]["sta"]["subnet"] = cfg.subnet;
        doc["data"]["sta"]["dns1"] = cfg.dns1;
        doc["data"]["sta"]["dns2"] = cfg.dns2;
        
        // ========== AP 配置 ==========
        doc["data"]["ap"]["ssid"] = cfg.apSSID;
        doc["data"]["ap"]["password"] = cfg.apPassword.length() > 0 ? "********" : "";
        doc["data"]["ap"]["hasPassword"] = cfg.apPassword.length() > 0;
        doc["data"]["ap"]["channel"] = cfg.apChannel;
        doc["data"]["ap"]["hidden"] = cfg.apHidden;
        doc["data"]["ap"]["maxConnections"] = cfg.apMaxConnections;
        
        // 当前连接状态
        NetworkStatusInfo info = netMgr->getStatusInfo();
        doc["data"]["status"]["connected"] = (info.status == NetworkStatus::CONNECTED);
        doc["data"]["status"]["ipAddress"] = info.ipAddress;
        doc["data"]["status"]["rssi"] = info.rssi;
        doc["data"]["status"]["ssid"] = info.ssid;
    } else {
        doc["success"] = false;
        doc["error"] = "Network service unavailable";
        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
        return;
    }

    doc["success"] = true;
    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
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

    NetworkManager* netMgr = static_cast<NetworkManager*>(networkManager);
    WiFiConfig cfg = netMgr->getConfig();
    
    // 注意：设备名称在 /api/device/config 中处理，不在网络配置中处理
    
    // ========== 网络模式 ==========
    String modeStr = getParamValue(request, "mode", "");
    if (!modeStr.isEmpty()) {
        cfg.mode = static_cast<NetworkMode>(modeStr.toInt());
    }
    
    // ========== STA 配置 ==========
    String staSSID = getParamValue(request, "staSSID", "");
    if (!staSSID.isEmpty()) {
        cfg.staSSID = staSSID;
    }
    
    String staPassword = getParamValue(request, "staPassword", "");
    if (!staPassword.isEmpty() && staPassword != "********") {
        cfg.staPassword = staPassword;
    }
    
    String ipConfigType = getParamValue(request, "ipConfigType", "");
    if (!ipConfigType.isEmpty()) {
        cfg.ipConfigType = static_cast<IPConfigType>(ipConfigType.toInt());
    }
    
    String staticIP = getParamValue(request, "staticIP", "");
    if (!staticIP.isEmpty()) {
        cfg.staticIP = staticIP;
    }
    
    String gateway = getParamValue(request, "gateway", "");
    if (!gateway.isEmpty()) {
        cfg.gateway = gateway;
    }
    
    String subnet = getParamValue(request, "subnet", "");
    if (!subnet.isEmpty()) {
        cfg.subnet = subnet;
    }
    
    // ========== AP 配置 ==========
    String apSSID = getParamValue(request, "apSSID", "");
    if (!apSSID.isEmpty()) {
        cfg.apSSID = apSSID;
    }
    
    String apPassword = getParamValue(request, "apPassword", "");
    if (!apPassword.isEmpty() && apPassword != "********") {
        cfg.apPassword = apPassword;
    }
    
    String apChannel = getParamValue(request, "apChannel", "");
    if (!apChannel.isEmpty()) {
        cfg.apChannel = apChannel.toInt();
    }
    
    String apHidden = getParamValue(request, "apHidden", "");
    if (!apHidden.isEmpty()) cfg.apHidden = (apHidden == "1" || apHidden == "true");
    
    // 保存配置
    if (netMgr->updateConfig(cfg, true)) {
        LOGGER.info("Network configuration updated via web");
        sendSuccess(request, "Network configuration saved successfully");
    } else {
        sendError(request, 500, "Failed to save network configuration");
    }
}

void WebConfigManager::handleAPIGetNetworkStatus(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "network.view")) {
        sendUnauthorized(request);
        return;
    }

    JsonDocument doc;
    doc["success"] = true;

    if (networkManager) {
        NetworkManager* netMgr = static_cast<NetworkManager*>(networkManager);
        NetworkStatusInfo info = netMgr->getStatusInfo();
        WiFiConfig cfg = netMgr->getConfig();

        // 状态文本映射
        const char* statusText = "unknown";
        switch (info.status) {
            case NetworkStatus::CONNECTED:        statusText = "connected";   break;
            case NetworkStatus::DISCONNECTED:     statusText = "disconnected"; break;
            case NetworkStatus::CONNECTING:       statusText = "connecting";  break;
            case NetworkStatus::AP_MODE:          statusText = "ap_mode";     break;
            case NetworkStatus::CONNECTION_FAILED:statusText = "failed";      break;
            default: break;
        }
        doc["data"]["status"] = statusText;
        doc["data"]["statusCode"] = static_cast<uint8_t>(info.status);

        // STA 信息
        doc["data"]["ssid"]          = info.ssid.isEmpty() ? cfg.staSSID : info.ssid;
        doc["data"]["ipAddress"]     = info.ipAddress;
        doc["data"]["macAddress"]    = info.macAddress.isEmpty() ? WiFi.macAddress() : info.macAddress;
        doc["data"]["rssi"]          = info.rssi;
        doc["data"]["signalStrength"]= NetworkUtils::rssiToPercentage(info.rssi);
        doc["data"]["gateway"]       = info.currentGateway;
        doc["data"]["subnet"]        = info.currentSubnet;
        doc["data"]["dnsServer"]     = info.dnsServer;

        // AP 信息
        doc["data"]["apIPAddress"]   = info.apIPAddress;
        doc["data"]["apClientCount"] = info.apClientCount;
        doc["data"]["apSSID"]        = cfg.apSSID;

        // 连接统计
        doc["data"]["reconnectAttempts"] = info.reconnectAttempts;
        doc["data"]["uptime"]            = info.uptime;
        doc["data"]["internetAvailable"] = info.internetAvailable;
        doc["data"]["conflictDetected"]  = info.conflictDetected;
        doc["data"]["failoverCount"]     = info.failoverCount;
        doc["data"]["activeIPType"]      = info.activeIPType;

        // 模式描述
        const char* modeText = "unknown";
        switch (cfg.mode) {
            case NetworkMode::NETWORK_STA:    modeText = "STA";    break;
            case NetworkMode::NETWORK_AP:     modeText = "AP";     break;
            case NetworkMode::NETWORK_AP_STA: modeText = "AP+STA"; break;
            default: break;
        }
        doc["data"]["mode"]       = modeText;
        doc["data"]["modeCode"]   = static_cast<uint8_t>(cfg.mode);
        doc["data"]["enableMDNS"] = cfg.enableMDNS;
        doc["data"]["customDomain"] = cfg.customDomain;
    } else {
        doc["success"] = false;
        doc["error"] = "Network service unavailable";
    }

    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
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
    if (!checkPermission(request, "system.view")) {
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

void WebConfigManager::handleAPIOtaUrl(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "ota.update")) {
        sendUnauthorized(request);
        return;
    }
    
    String url = getParamValue(request, "url", "");
    
    if (url.isEmpty()) {
        sendError(request, 400, "缺少固件URL参数");
        return;
    }
    
    if (!url.startsWith("http://") && !url.startsWith("https://")) {
        sendError(request, 400, "无效的URL格式，必须以http://或https://开头");
        return;
    }
    
    if (!otaManager) {
        sendError(request, 500, "OTA管理器未初始化");
        return;
    }
    
    if (otaManager->isOTAInProgress()) {
        sendError(request, 400, "OTA升级正在进行中");
        return;
    }
    
    LOGGER.infof("OTA: Starting URL upgrade from %s", url.c_str());
    
    // 返回响应后异步执行升级
    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "开始从URL下载固件并升级";
    doc["url"] = url;
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
    
    // 延迟执行OTA升级（让响应先发送）
    delay(100);
    otaManager->startOTA(url);
}

void WebConfigManager::handleAPIOtaUpload(AsyncWebServerRequest* request, const String& filename, 
                                          size_t index, uint8_t* data, size_t len, bool final) {
    if (!checkPermission(request, "ota.update")) {
        return;
    }
    
    // 文件开始上传
    if (index == 0) {
        LOGGER.infof("OTA: Upload started - file: %s", filename.c_str());
        
        // 计算最大可用空间
        uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        
        if (!Update.begin(maxSketchSpace)) {
            LOGGER.errorf("OTA: Update begin failed - %s", Update.errorString());
            return;
        }
        
        // 设置进度回调
        Update.onProgress([](size_t progress, size_t total) {
            int percent = total > 0 ? (progress * 100 / total) : 0;
            if (percent % 10 == 0) {
                LOGGER.infof("OTA: Upload progress: %d%%", percent);
            }
        });
    }
    
    // 写入数据
    if (Update.write(data, len) != len) {
        LOGGER.errorf("OTA: Write failed - %s", Update.errorString());
        Update.end(false);
        return;
    }
    
    // 上传完成
    if (final) {
        LOGGER.infof("OTA: Upload completed - total size: %d bytes", index + len);
        
        if (Update.end(true)) {
            if (Update.isFinished()) {
                LOGGER.info("OTA: Firmware verification passed, restarting...");
                
                // 发送成功响应
                JsonDocument doc;
                doc["success"] = true;
                doc["message"] = "固件上传成功，设备将在3秒后重启";
                doc["size"] = index + len;
                doc["md5"] = Update.md5String();
                
                String out;
                serializeJson(doc, out);
                request->send(200, "application/json", out);
                
                delay(3000);
                ESP.restart();
            } else {
                LOGGER.error("OTA: Firmware verification failed");
            }
        } else {
            LOGGER.errorf("OTA: Update end failed - %s", Update.errorString());
        }
    }
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

    LOGGER.debugf("CreateRole: id=[%s] name=[%s] desc=[%s]", id.c_str(), name.c_str(), desc.c_str());

    if (id.isEmpty() || name.isEmpty()) {
        LOGGER.warning("CreateRole: id or name is empty");
        sendBadRequest(request, "id and name are required");
        return;
    }

    if (!roleManager->createRole(id, name, desc)) {
        LOGGER.errorf("CreateRole: Failed to create role [%s]", id.c_str());
        sendError(request, 400, "Failed to create role (id may already exist)");
        return;
    }

    AuthResult ar = authenticateRequest(request);
    if (authManager) {
        static_cast<AuthManager*>(authManager)->recordAudit(
            ar.username, "role.create", id, "Created role: " + name,
            true, getClientIP(request));
    }
    LOGGER.infof("CreateRole: Role [%s] created successfully", id.c_str());
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
    LOGGER.debugf("DeleteRole: roleId=[%s]", roleId.c_str());
    
    if (roleId.isEmpty()) {
        LOGGER.warning("DeleteRole: Role id is empty");
        sendBadRequest(request, "Role id is required");
        return;
    }

    if (!roleManager) {
        sendError(request, 500, "Role service unavailable");
        return;
    }
    
    // 仅 admin 角色不可删除
    if (roleId == "admin") {
        LOGGER.warning("DeleteRole: Cannot delete admin role");
        sendError(request, 400, "Cannot delete admin role");
        return;
    }
    
    // 检查角色是否存在
    if (!roleManager->roleExists(roleId)) {
        LOGGER.warningf("DeleteRole: Role [%s] not found", roleId.c_str());
        sendNotFound(request);
        return;
    }

    if (!roleManager->deleteRole(roleId)) {
        LOGGER.errorf("DeleteRole: Failed to delete role [%s]", roleId.c_str());
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
    LOGGER.debugf("SetRolePermissions: roleId=[%s]", roleId.c_str());
    
    if (roleId.isEmpty()) {
        LOGGER.warning("SetRolePermissions: Role id is empty");
        sendBadRequest(request, "Role id is required");
        return;
    }

    if (!roleManager || !roleManager->roleExists(roleId)) {
        LOGGER.warningf("SetRolePermissions: Role [%s] not found", roleId.c_str());
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
    LOGGER.debugf("SetRolePermissions: permissions=[%s]", permsParam.c_str());
    
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

    LOGGER.debugf("SetRolePermissions: Parsed %d permissions", permList.size());
    
    if (!roleManager->setRolePermissions(roleId, permList)) {
        LOGGER.errorf("SetRolePermissions: Failed to set permissions for role [%s]", roleId.c_str());
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
    LOGGER.infof("SetRolePermissions: Role [%s] permissions updated", roleId.c_str());
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
    JsonArray data = doc["data"].to<JsonArray>();
    
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n && i < 20; i++) {
        JsonObject net = data.add<JsonObject>();
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
        net["channel"] = WiFi.channel(i);
        net["encryption"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? 1 : 0;
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
        // 启动 mDNS（从配置获取 hostname）
        String hostname = netMgr->getConfig().customDomain;
        if (hostname.isEmpty()) hostname = "fastbee";
        MDNS.begin(hostname.c_str());
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

// ============ 文件管理 API 处理函数 ============

void WebConfigManager::handleAPIGetFileList(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }
    
    String path = "/";
    if (request->hasParam("path")) {
        path = request->getParam("path")->value();
    }
    
    // 安全检查：确保路径在 /data 目录下
    if (!path.startsWith("/")) {
        path = "/" + path;
    }
    
    JsonDocument doc;
    doc["success"] = true;
    doc["data"]["path"] = path;
    
    File root = LittleFS.open(path);
    if (!root || !root.isDirectory()) {
        doc["success"] = false;
        doc["error"] = "Invalid directory";
        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
        return;
    }
    
    JsonArray files = doc["data"]["files"].to<JsonArray>();
    JsonArray dirs = doc["data"]["dirs"].to<JsonArray>();
    
    File file = root.openNextFile();
    while (file) {
        JsonObject item;
        if (file.isDirectory()) {
            item = dirs.add<JsonObject>();
        } else {
            item = files.add<JsonObject>();
            item["size"] = file.size();
        }
        item["name"] = String(file.name());
        file = root.openNextFile();
    }
    
    root.close();
    
    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
}

void WebConfigManager::handleAPIGetFileContent(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }
    
    if (!request->hasParam("path")) {
        sendError(request, 400, "Missing path parameter");
        return;
    }
    
    String path = request->getParam("path")->value();
    
    // 安全检查
    if (!path.startsWith("/")) {
        path = "/" + path;
    }
    
    // 禁止访问敏感路径
    if (path.indexOf("..") >= 0) {
        sendError(request, 403, "Invalid path");
        return;
    }
    
    if (!LittleFS.exists(path)) {
        sendError(request, 404, "File not found");
        return;
    }
    
    File file = LittleFS.open(path, "r");
    if (!file) {
        sendError(request, 500, "Failed to open file");
        return;
    }
    
    size_t fileSize = file.size();
    // 限制文件大小为 128KB
    if (fileSize > 128 * 1024) {
        file.close();
        sendError(request, 413, "File too large (max 128KB)");
        return;
    }
    
    String content = file.readString();
    file.close();
    
    JsonDocument doc;
    doc["success"] = true;
    doc["data"]["path"] = path;
    doc["data"]["size"] = fileSize;
    doc["data"]["content"] = content;
    
    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
}

void WebConfigManager::handleAPISaveFileContent(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    if (!request->hasParam("path", true)) {
        sendError(request, 400, "Missing path parameter");
        return;
    }
    
    String path = request->getParam("path", true)->value();
    
    // 安全检查
    if (!path.startsWith("/")) {
        path = "/" + path;
    }
    
    // 禁止访问敏感路径
    if (path.indexOf("..") >= 0) {
        sendError(request, 403, "Invalid path");
        return;
    }
    
    // 只允许编辑特定类型的文件
    if (!(path.endsWith(".json") || path.endsWith(".txt") || path.endsWith(".log") || 
          path.endsWith(".html") || path.endsWith(".js") || path.endsWith(".css"))) {
        sendError(request, 403, "File type not allowed for editing");
        return;
    }
    
    String content = "";
    if (request->hasParam("content", true)) {
        content = request->getParam("content", true)->value();
    }
    
    // 确保父目录存在
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash > 0) {
        String parentDir = path.substring(0, lastSlash);
        if (!LittleFS.exists(parentDir)) {
            // 创建目录（递归创建需要手动实现）
            sendError(request, 500, "Parent directory does not exist");
            return;
        }
    }
    
    File file = LittleFS.open(path, "w");
    if (!file) {
        sendError(request, 500, "Failed to open file for writing");
        return;
    }
    
    size_t written = file.print(content);
    file.close();
    
    if (written == content.length()) {
        LOGGER.infof("File saved via web: %s", path.c_str());
        sendSuccess(request, "File saved successfully");
    } else {
        sendError(request, 500, "Failed to write complete file");
    }
}

// ============ GPIO API 处理函数 ============

void WebConfigManager::handleAPIGetGPIOConfig(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }
    
    JsonDocument doc;
    doc["success"] = true;
    
    GPIOManager& gpio = GPIOManager::getInstance();
    std::vector<String> pinNames = gpio.getConfiguredPins();
    
    JsonArray pins = doc["data"]["pins"].to<JsonArray>();
    for (const String& name : pinNames) {
        // 通过名称查找引脚号
        uint8_t pinNum = 255;
        for (uint8_t i = 0; i < 40; i++) {
            if (gpio.getPinName(i) == name) {
                pinNum = i;
                break;
            }
        }
        if (pinNum == 255) continue;
        
        JsonObject pinObj = pins.add<JsonObject>();
        pinObj["pin"] = pinNum;
        pinObj["name"] = name;
        pinObj["mode"] = static_cast<int>(gpio.getPinMode(pinNum));
        GPIOState state = gpio.readPin(pinNum);
        pinObj["state"] = (state == GPIOState::STATE_HIGH) ? 1 : 0;
    }
    
    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
}

void WebConfigManager::handleAPIConfigureGPIO(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    // 获取参数
    String pinStr = getParamValue(request, "pin", "");
    String name = getParamValue(request, "name", "");
    String modeStr = getParamValue(request, "mode", "");
    
    LOGGER.debugf("GPIO config request: pin=[%s] name=[%s] mode=[%s]", 
                  pinStr.c_str(), name.c_str(), modeStr.c_str());
    
    if (pinStr.isEmpty() || name.isEmpty() || modeStr.isEmpty()) {
        LOGGER.warning("GPIO config: Missing required parameters");
        sendError(request, 400, "Missing required parameters: pin, name, mode");
        return;
    }
    
    uint8_t pin = pinStr.toInt();
    GPIOMode mode = static_cast<GPIOMode>(modeStr.toInt());
    
    LOGGER.debugf("GPIO config: parsed pin=%d mode=%d", pin, (int)mode);
    
    GPIOConfig config;
    config.pin = pin;
    config.name = name;
    config.mode = mode;
    config.initialState = static_cast<GPIOState>(getParamValue(request, "defaultValue", "0").toInt());
    config.inverted = getParamValue(request, "invert", "0") == "1";
    
    GPIOManager& gpio = GPIOManager::getInstance();
    
    // 先检查引脚有效性，给出明确错误信息
    if (!gpio.isValidPin(pin)) {
        LOGGER.warningf("GPIO %d: invalid pin (6-11 reserved for Flash)", pin);
        sendError(request, 400, "Invalid GPIO pin (6-11 are reserved for internal Flash)");
        return;
    }
    
    if (gpio.configurePin(config)) {
        // 配置成功后自动保存到 LittleFS
        if (gpio.saveConfiguration()) {
            LOGGER.infof("GPIO %d configured and saved via web API", pin);
            sendSuccess(request, "GPIO configured and saved successfully");
        } else {
            LOGGER.warningf("GPIO %d configured but save failed", pin);
            sendSuccess(request, "GPIO configured but save to file failed");
        }
    } else {
        LOGGER.errorf("Failed to configure GPIO pin %d", pin);
        sendError(request, 500, "Failed to configure GPIO");
    }
}

void WebConfigManager::handleAPIReadGPIO(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }
    
    String pinStr = getParamValue(request, "pin", "");
    String name = getParamValue(request, "name", "");
    
    if (pinStr.isEmpty() && name.isEmpty()) {
        sendError(request, 400, "Missing parameter: pin or name");
        return;
    }
    
    GPIOManager& gpio = GPIOManager::getInstance();
    GPIOState state;
    
    if (!name.isEmpty()) {
        state = gpio.readPin(name);
    } else {
        state = gpio.readPin(pinStr.toInt());
    }
    
    JsonDocument doc;
    doc["success"] = true;
    doc["data"]["state"] = static_cast<int>(state);
    doc["data"]["stateName"] = (state == GPIOState::STATE_HIGH) ? "HIGH" : "LOW";
    
    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
}

void WebConfigManager::handleAPIWriteGPIO(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    String pinStr = getParamValue(request, "pin", "");
    String name = getParamValue(request, "name", "");
    String stateStr = getParamValue(request, "state", "");
    
    if ((pinStr.isEmpty() && name.isEmpty()) || stateStr.isEmpty()) {
        sendError(request, 400, "Missing required parameters");
        return;
    }
    
    GPIOManager& gpio = GPIOManager::getInstance();
    bool success = false;
    
    // 支持 toggle 操作
    if (stateStr == "toggle") {
        if (!name.isEmpty()) {
            success = gpio.togglePin(name);
        } else {
            success = gpio.togglePin((uint8_t)pinStr.toInt());
        }
    } else {
        GPIOState state = static_cast<GPIOState>(stateStr.toInt());
        if (!name.isEmpty()) {
            success = gpio.writePin(name, state);
        } else {
            success = gpio.writePin((uint8_t)pinStr.toInt(), state);
        }
    }
    
    if (success) {
        sendSuccess(request, "GPIO state updated");
    } else {
        sendError(request, 500, "Failed to write GPIO");
    }
}

void WebConfigManager::handleAPIDeleteGPIO(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    String pinStr = getParamValue(request, "pin", "");
    if (pinStr.isEmpty()) {
        sendError(request, 400, "Missing required parameter: pin");
        return;
    }
    
    uint8_t pin = (uint8_t)pinStr.toInt();
    GPIOManager& gpio = GPIOManager::getInstance();
    
    if (!gpio.isPinConfigured(pin)) {
        sendError(request, 404, "GPIO pin not configured");
        return;
    }
    
    // 通过重配置为 UNCONFIGURED 实现删除
    // 先永久化除出该引脚
    if (gpio.removePin(pin)) {
        // 自动保存配置
        gpio.saveConfiguration();
        LOGGER.infof("GPIO %d deleted via web API", pin);
        sendSuccess(request, "GPIO deleted successfully");
    } else {
        sendError(request, 500, "Failed to delete GPIO");
    }
}

void WebConfigManager::handleAPISaveGPIOConfig(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    GPIOManager& gpio = GPIOManager::getInstance();
    if (gpio.saveConfiguration()) {
        LOGGER.info("GPIO configuration saved via web API");
        sendSuccess(request, "GPIO configuration saved");
    } else {
        sendError(request, 500, "Failed to save GPIO configuration");
    }
}

// ============ 协议配置 API 处理器 ============

static const char* PROTOCOL_CONFIG_PATH = "/config/protocol.json";

void WebConfigManager::handleAPIGetProtocolConfig(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.view")) {
        sendUnauthorized(request);
        return;
    }
    
    JsonDocument doc;
    
    // 尝试从 LittleFS 读取配置
    if (LittleFS.exists(PROTOCOL_CONFIG_PATH)) {
        File f = LittleFS.open(PROTOCOL_CONFIG_PATH, "r");
        if (f) {
            JsonDocument fileCfg;
            DeserializationError err = deserializeJson(fileCfg, f);
            f.close();
            
            if (!err) {
                doc["success"] = true;
                doc["data"] = fileCfg;
                String out;
                serializeJson(doc, out);
                request->send(200, "application/json", out);
                return;
            }
        }
    }
    
    // 文件不存在或解析失败，返回空配置
    doc["success"] = true;
    doc["data"].to<JsonObject>();
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebConfigManager::handleAPISaveProtocolConfig(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    JsonDocument doc;
    
    // 辅助宏
    #define GP(key, def) getParamValue(request, key, def)
    #define GPI(key, def) GP(key, def).toInt()
    
    // Modbus RTU
    doc["modbusRtu"]["enabled"] = GP("modbusRtu_enabled", "false") == "true";
    doc["modbusRtu"]["port"] = GP("modbusRtu_port", "/dev/ttyS0");
    doc["modbusRtu"]["baudRate"] = GPI("modbusRtu_baudRate", "19200");
    doc["modbusRtu"]["dataBits"] = GPI("modbusRtu_dataBits", "8");
    doc["modbusRtu"]["stopBits"] = GPI("modbusRtu_stopBits", "1");
    doc["modbusRtu"]["parity"] = GP("modbusRtu_parity", "none");
    doc["modbusRtu"]["timeout"] = GPI("modbusRtu_timeout", "1000");
    
    // Modbus TCP
    doc["modbusTcp"]["enabled"] = GP("modbusTcp_enabled", "false") == "true";
    doc["modbusTcp"]["server"] = GP("modbusTcp_server", "192.168.1.100");
    doc["modbusTcp"]["port"] = GPI("modbusTcp_port", "502");
    doc["modbusTcp"]["slaveId"] = GPI("modbusTcp_slaveId", "1");
    doc["modbusTcp"]["timeout"] = GPI("modbusTcp_timeout", "5000");
    
    // MQTT
    doc["mqtt"]["enabled"] = GP("mqtt_enabled", "true") == "true";
    doc["mqtt"]["server"] = GP("mqtt_server", "iot.fastbee.cn");
    doc["mqtt"]["port"] = GPI("mqtt_port", "1883");
    doc["mqtt"]["clientId"] = GP("mqtt_clientId", "");
    doc["mqtt"]["username"] = GP("mqtt_username", "");
    doc["mqtt"]["password"] = GP("mqtt_password", "");
    doc["mqtt"]["keepAlive"] = GPI("mqtt_keepAlive", "60");
    doc["mqtt"]["subscribeTopic"] = GP("mqtt_subscribeTopic", "");
    // 新增字段
    doc["mqtt"]["directConnect"] = GP("mqtt_directConnect", "true") == "true";
    doc["mqtt"]["autoReconnect"] = GP("mqtt_autoReconnect", "true") == "true";
    doc["mqtt"]["connectionTimeout"] = GPI("mqtt_connectionTimeout", "30000");
    
    // 发布主题配置（支持多组）- 从JSON字符串解析
    String publishTopicsJson = GP("mqtt_publishTopics", "[]");
    JsonArray publishTopics = doc["mqtt"]["publishTopics"].to<JsonArray>();
    if (publishTopicsJson.length() > 2) { // 不是空数组 "[]"
        // 尝试解析前端传来的JSON数组字符串
        JsonDocument topicsDoc;
        DeserializationError err = deserializeJson(topicsDoc, publishTopicsJson);
        if (!err && topicsDoc.is<JsonArray>()) {
            JsonArray arr = topicsDoc.as<JsonArray>();
            for (JsonVariant v : arr) {
                JsonObject topicObj = publishTopics.add<JsonObject>();
                topicObj["topic"] = v["topic"] | "";
                topicObj["qos"] = v["qos"] | 0;
                topicObj["retain"] = v["retain"] | false;
                topicObj["content"] = v["content"] | "";
            }
        }
    }
    // 如果没有配置，添加一个默认空配置
    if (publishTopics.size() == 0) {
        JsonObject defaultTopic = publishTopics.add<JsonObject>();
        defaultTopic["topic"] = "";
        defaultTopic["qos"] = 0;
        defaultTopic["retain"] = false;
        defaultTopic["content"] = "";
    }
    
    // HTTP
    doc["http"]["enabled"] = GP("http_enabled", "false") == "true";
    doc["http"]["url"] = GP("http_url", "https://api.example.com");
    doc["http"]["port"] = GPI("http_port", "80");
    doc["http"]["method"] = GP("http_method", "POST");
    doc["http"]["timeout"] = GPI("http_timeout", "30");
    doc["http"]["interval"] = GPI("http_interval", "60");
    doc["http"]["retry"] = GPI("http_retry", "3");
    
    // CoAP
    doc["coap"]["enabled"] = GP("coap_enabled", "false") == "true";
    doc["coap"]["server"] = GP("coap_server", "coap://example.com");
    doc["coap"]["port"] = GPI("coap_port", "5683");
    doc["coap"]["method"] = GP("coap_method", "POST");
    doc["coap"]["path"] = GP("coap_path", "sensors/temperature");
    
    // TCP
    doc["tcp"]["enabled"] = GP("tcp_enabled", "false") == "true";
    doc["tcp"]["server"] = GP("tcp_server", "192.168.1.200");
    doc["tcp"]["port"] = GPI("tcp_port", "5000");
    doc["tcp"]["timeout"] = GPI("tcp_timeout", "5000");
    doc["tcp"]["keepAlive"] = GPI("tcp_keepAlive", "60");
    doc["tcp"]["maxRetry"] = GPI("tcp_maxRetry", "5");
    doc["tcp"]["reconnectInterval"] = GPI("tcp_reconnectInterval", "10");
    
    #undef GP
    #undef GPI
    
    // 保存到文件
    File f = LittleFS.open(PROTOCOL_CONFIG_PATH, "w");
    if (!f) {
        sendError(request, 500, "Failed to save protocol config");
        return;
    }
    
    serializeJsonPretty(doc, f);
    f.close();
    
    sendSuccess(request, "Protocol configuration saved");
}

// ============ 设备配置 API 处理器 ============

static const char* DEVICE_CONFIG_FILE = "/config/device.json";

void WebConfigManager::handleAPIGetDeviceConfig(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }

    JsonDocument doc;

    // 尝试从 LittleFS 读取配置
    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            JsonDocument fileCfg;
            if (!deserializeJson(fileCfg, f)) {
                doc["success"] = true;
                doc["data"] = fileCfg;
                // 添加 MAC 地址用于前端生成设备编号
                doc["data"]["macAddress"] = WiFi.macAddress();
                // 如果没有设备编号，生成一个（FBE + 完整MAC地址）
                if (!fileCfg["deviceId"].is<String>() || fileCfg["deviceId"].as<String>().isEmpty()) {
                    String mac = WiFi.macAddress();
                    mac.replace(":", "");
                    mac.toUpperCase();
                    doc["data"]["deviceId"] = "FBE" + mac;
                }
                f.close();
                String out;
                serializeJson(doc, out);
                request->send(200, "application/json", out);
                return;
            }
            f.close();
        }
    }

    // 返回默认配置
    doc["success"] = true;
    doc["data"]["ntpServer1"] = "pool.ntp.org";
    doc["data"]["ntpServer2"] = "time.nist.gov";
    doc["data"]["timezone"] = "CST-8";
    doc["data"]["enableNTP"] = true;
    doc["data"]["deviceName"] = "FastBee-Device";
    doc["data"]["location"] = "";
    doc["data"]["description"] = "";
    doc["data"]["syncInterval"] = 3600;
    // 添加 MAC 地址和设备编号（FBE + 完整MAC地址）
    String mac = WiFi.macAddress();
    doc["data"]["macAddress"] = mac;
    mac.replace(":", "");
    mac.toUpperCase();
    doc["data"]["deviceId"] = "FBE" + mac;

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============ AP配网 API 处理器实现 ============

static bool provisionModeActive = false;
static String provisionApSSID = "";
static unsigned long provisionStartTime = 0;
static uint32_t provisionTimeout = 300000; // 默认5分钟

void WebConfigManager::handleAPIGetProvisionStatus(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["data"]["active"] = provisionModeActive;
    doc["data"]["apSSID"] = provisionApSSID.isEmpty() ? "--" : provisionApSSID;
    doc["data"]["apIP"] = "192.168.4.1";
    
    // 获取已连接的客户端数量
    wifi_sta_list_t stationList;
    esp_wifi_ap_get_sta_list(&stationList);
    doc["data"]["clients"] = stationList.num;
    
    // 计算剩余时间
    if (provisionModeActive && provisionStartTime > 0) {
        unsigned long elapsed = millis() - provisionStartTime;
        if (elapsed < provisionTimeout) {
            doc["data"]["remainingTime"] = (provisionTimeout - elapsed) / 1000;
        } else {
            doc["data"]["remainingTime"] = 0;
        }
    } else {
        doc["data"]["remainingTime"] = 0;
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebConfigManager::handleAPIStartProvision(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }

    // 加载配网配置（从 device.json）
    String apSSID = "fastbee-device-" + String(random(1000, 9999));
    String apPassword = "";
    uint32_t timeout = 300000;
    String apIP = "192.168.4.1";
    String apGateway = "192.168.4.1";
    String apSubnet = "255.255.255.0";

    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            JsonDocument cfg;
            if (!deserializeJson(cfg, f)) {
                if (cfg.containsKey("provisionSSID") && cfg["provisionSSID"].as<String>().length() > 0) {
                    apSSID = cfg["provisionSSID"].as<String>();
                }
                if (cfg.containsKey("provisionPassword")) {
                    apPassword = cfg["provisionPassword"].as<String>();
                }
                if (cfg.containsKey("provisionTimeout")) {
                    timeout = cfg["provisionTimeout"].as<uint32_t>() * 1000;
                }
                // 读取AP网络参数
                if (cfg.containsKey("provisionIP") && cfg["provisionIP"].as<String>().length() > 0) {
                    apIP = cfg["provisionIP"].as<String>();
                }
                if (cfg.containsKey("provisionGateway") && cfg["provisionGateway"].as<String>().length() > 0) {
                    apGateway = cfg["provisionGateway"].as<String>();
                }
                if (cfg.containsKey("provisionSubnet") && cfg["provisionSubnet"].as<String>().length() > 0) {
                    apSubnet = cfg["provisionSubnet"].as<String>();
                }
            }
            f.close();
        }
    }

    // 解析IP地址
    IPAddress localIP, gateway, subnet;
    localIP.fromString(apIP);
    gateway.fromString(apGateway);
    subnet.fromString(apSubnet);

    // 启动AP模式
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(localIP, gateway, subnet);
    
    bool success;
    if (apPassword.length() >= 8) {
        success = WiFi.softAP(apSSID.c_str(), apPassword.c_str());
    } else {
        success = WiFi.softAP(apSSID.c_str());
    }

    if (success) {
        provisionModeActive = true;
        provisionApSSID = apSSID;
        provisionStartTime = millis();
        provisionTimeout = timeout;
        
        LOGGER.infof("Provision: AP started - SSID=%s IP=%s", 
                     apSSID.c_str(), WiFi.softAPIP().toString().c_str());
        
        JsonDocument doc;
        doc["success"] = true;
        doc["message"] = "AP配网已启动";
        doc["data"]["apSSID"] = apSSID;
        doc["data"]["apIP"] = WiFi.softAPIP().toString();
        
        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    } else {
        sendError(request, 500, "启动AP配网失败");
    }
}

void WebConfigManager::handleAPIStopProvision(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }

    WiFi.softAPdisconnect(true);
    provisionModeActive = false;
    provisionApSSID = "";
    provisionStartTime = 0;
    
    LOGGER.info("Provision: AP stopped");
    sendSuccess(request, "AP配网已停止");
}

void WebConfigManager::handleAPIGetProvisionConfig(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.view")) {
        sendUnauthorized(request);
        return;
    }

    JsonDocument doc;
    doc["success"] = true;

    // 默认配置
    doc["data"]["provisionSSID"] = "";
    doc["data"]["provisionPassword"] = "";
    doc["data"]["provisionTimeout"] = 300;
    doc["data"]["provisionUserId"] = "";
    doc["data"]["provisionProductId"] = "";
    doc["data"]["provisionAuthCode"] = "";
    doc["data"]["provisionIP"] = "192.168.4.1";
    doc["data"]["provisionGateway"] = "192.168.4.1";
    doc["data"]["provisionSubnet"] = "255.255.255.0";

    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            JsonDocument cfg;
            if (!deserializeJson(cfg, f)) {
                if (cfg.containsKey("provisionSSID"))     doc["data"]["provisionSSID"] = cfg["provisionSSID"];
                if (cfg.containsKey("provisionPassword")) doc["data"]["provisionPassword"] = cfg["provisionPassword"];
                if (cfg.containsKey("provisionTimeout"))  doc["data"]["provisionTimeout"] = cfg["provisionTimeout"];
                if (cfg.containsKey("provisionUserId"))   doc["data"]["provisionUserId"] = cfg["provisionUserId"];
                if (cfg.containsKey("provisionProductId")) doc["data"]["provisionProductId"] = cfg["provisionProductId"];
                if (cfg.containsKey("provisionAuthCode")) doc["data"]["provisionAuthCode"] = cfg["provisionAuthCode"];
                if (cfg.containsKey("provisionIP"))       doc["data"]["provisionIP"] = cfg["provisionIP"];
                if (cfg.containsKey("provisionGateway"))  doc["data"]["provisionGateway"] = cfg["provisionGateway"];
                if (cfg.containsKey("provisionSubnet"))   doc["data"]["provisionSubnet"] = cfg["provisionSubnet"];
            }
            f.close();
        }
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebConfigManager::handleAPISaveProvisionConfig(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }

    // 先读取现有 device.json 配置
    JsonDocument cfg;
    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            deserializeJson(cfg, f);
            f.close();
        }
    }

    // 更新配网相关字段
    cfg["provisionSSID"]     = getParamValue(request, "provisionSSID", "");
    cfg["provisionPassword"] = getParamValue(request, "provisionPassword", "");
    cfg["provisionTimeout"]  = getParamValue(request, "provisionTimeout", "300").toInt();
    cfg["provisionUserId"]   = getParamValue(request, "provisionUserId", "");
    cfg["provisionProductId"] = getParamValue(request, "provisionProductId", "");
    cfg["provisionAuthCode"] = getParamValue(request, "provisionAuthCode", "");
    cfg["provisionIP"]       = getParamValue(request, "provisionIP", "192.168.4.1");
    cfg["provisionGateway"]  = getParamValue(request, "provisionGateway", "192.168.4.1");
    cfg["provisionSubnet"]   = getParamValue(request, "provisionSubnet", "255.255.255.0");

    File f = LittleFS.open(DEVICE_CONFIG_FILE, "w");
    if (!f) {
        sendError(request, 500, "无法写入设备配置文件");
        return;
    }
    serializeJson(cfg, f);
    f.close();

    LOGGER.info("Provision: Config saved to device.json");
    sendSuccess(request, "AP配网配置已保存");
}

void WebConfigManager::handleAPIProvisionCallback(AsyncWebServerRequest* request) {
    // 配网回调接口，供手机APP调用，不需要认证
    
    String ssid = getParamValue(request, "SSID", "");
    String password = getParamValue(request, "password", "");
    String userId = getParamValue(request, "userId", "");
    
    if (ssid.isEmpty() || password.isEmpty()) {
        request->send(400, "text/plain;charset=utf-8", "缺少必要参数：SSID和password");
        return;
    }

    LOGGER.infof("Provision: Received config - SSID=%s userId=%s", ssid.c_str(), userId.c_str());

    // 保存到网络配置
    if (networkManager) {
        // 回复成功（先回复，再连接）
        request->send(200, "text/plain;charset=utf-8", "配置已接收，设备正在连接WiFi...");
        
        // 延迟后尝试连接WiFi
        delay(500);
        
        // 停止配网模式
        provisionModeActive = false;
        provisionApSSID = "";
        
        // 使用connectToNetwork连接WiFi（该方法会自动保存配置）
        LOGGER.info("Provision: Attempting to connect to WiFi...");
        networkManager->connectToNetwork(ssid, password);
    } else {
        request->send(500, "text/plain;charset=utf-8", "网络管理器未初始化");
    }
}

void WebConfigManager::handleAPIUpdateDeviceConfig(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }

    // 先读取现有配置，避免丢失其他字段
    JsonDocument cfg;
    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            deserializeJson(cfg, f);
            f.close();
        }
    }

    // 只更新传入的字段
    String val;
    
    // 设备编号：如果传入且不为空则保存，否则保留现有值或生成默认值
    val = getParamValue(request, "deviceId", "");
    if (!val.isEmpty()) {
        // 用户可自定义任意格式，直接保存
        cfg["deviceId"] = val;
    }
    // 如果 deviceId 为空且配置中也没有，生成基于MAC的默认值
    if ((!cfg.containsKey("deviceId") || cfg["deviceId"].as<String>().isEmpty()) && 
        WiFi.macAddress().length() > 0) {
        String mac = WiFi.macAddress();
        mac.replace(":", "");
        mac.toUpperCase();
        cfg["deviceId"] = "FBE" + mac;
    }
    
    val = getParamValue(request, "ntpServer1", "");
    if (!val.isEmpty()) cfg["ntpServer1"] = val;
    val = getParamValue(request, "ntpServer2", "");
    if (!val.isEmpty()) cfg["ntpServer2"] = val;
    val = getParamValue(request, "timezone", "");
    if (!val.isEmpty()) cfg["timezone"] = val;
    val = getParamValue(request, "enableNTP", "");
    if (!val.isEmpty()) cfg["enableNTP"] = (val == "1" || val == "true");
    val = getParamValue(request, "deviceName", "");
    if (!val.isEmpty()) cfg["deviceName"] = val;
    val = getParamValue(request, "location", "");
    if (request->hasParam("location")) cfg["location"] = val;
    val = getParamValue(request, "description", "");
    if (request->hasParam("description")) cfg["description"] = val;
    val = getParamValue(request, "syncInterval", "");
    if (!val.isEmpty()) cfg["syncInterval"] = val.toInt();
    val = getParamValue(request, "productNumber", "");
    if (request->hasParam("productNumber")) cfg["productNumber"] = val.toInt();

    // 保存到 LittleFS
    File f = LittleFS.open(DEVICE_CONFIG_FILE, "w");
    if (!f) {
        sendError(request, 500, "Failed to open device config file");
        return;
    }
    serializeJsonPretty(cfg, f);
    f.close();

    // 如果启用 NTP，立即配置
    if (cfg["enableNTP"].as<bool>()) {
        const char* tz  = cfg["timezone"] | "CST-8";
        const char* s1  = cfg["ntpServer1"] | "pool.ntp.org";
        const char* s2  = cfg["ntpServer2"] | "time.nist.gov";
        configTzTime(tz, s1, s2);
        LOGGER.infof("Device: NTP configured - tz=%s srv1=%s srv2=%s", tz, s1, s2);
    }

    sendSuccess(request, "Device configuration saved");
}

void WebConfigManager::handleAPIGetDeviceTime(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }

    JsonDocument doc;
    doc["success"] = true;

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 100)) {
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
        doc["data"]["datetime"] = buf;
        char dateBuf[12];
        strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &timeinfo);
        doc["data"]["date"] = dateBuf;
        char timeBuf[10];
        strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &timeinfo);
        doc["data"]["time"] = timeBuf;
        doc["data"]["timestamp"] = (long long)mktime(&timeinfo);
        doc["data"]["synced"] = true;
    } else {
        doc["data"]["datetime"] = "--";
        doc["data"]["date"] = "--";
        doc["data"]["time"] = "--";
        doc["data"]["timestamp"] = (long long)(millis() / 1000);
        doc["data"]["synced"] = false;
    }
    doc["data"]["uptime"] = millis();

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============ 蓝牙配网 API 处理器实现 ============

static bool bleProvisionActive = false;
static NimBLEServer* pBLEServer = nullptr;
static NimBLECharacteristic* pTxCharacteristic = nullptr;
static String bleDeviceName = "FBDevice";
static unsigned long bleProvisionStartTime = 0;
static uint32_t bleProvisionTimeout = 300000;

// 蓝牙服务端回调
class BLEProvisionServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server) override {
        LOGGER.info("BLE: Device connected");
    }
    
    void onDisconnect(NimBLEServer* server) override {
        LOGGER.info("BLE: Device disconnected");
        if (bleProvisionActive) {
            server->startAdvertising();
        }
    }
};

// 蓝牙特征码回调 - 处理接收的WiFi配置数据
class BLEProvisionCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic) override {
        std::string rxValue = characteristic->getValue();
        
        if (rxValue.length() > 0) {
            LOGGER.infof("BLE: Received data: %s", rxValue.c_str());
            
            if (rxValue.find("ssid") != std::string::npos) {
                JsonDocument doc;
                DeserializationError error = deserializeJson(doc, rxValue);
                
                if (error) {
                    LOGGER.error("BLE: Invalid JSON format");
                    if (pTxCharacteristic) {
                        pTxCharacteristic->setValue("error");
                        pTxCharacteristic->notify();
                    }
                    return;
                }
                
                String ssid = doc["ssid"].as<String>();
                String password = doc["password"].as<String>();
                
                if (ssid.length() > 0) {
                    LOGGER.infof("BLE: Connecting to WiFi SSID=%s", ssid.c_str());
                    
                    WiFi.begin(ssid.c_str(), password.c_str());
                    
                    int count = 0;
                    while (WiFi.status() != WL_CONNECTED && count < 20) {
                        delay(500);
                        count++;
                    }
                    
                    bool success = (WiFi.status() == WL_CONNECTED);
                    
                    if (pTxCharacteristic) {
                        pTxCharacteristic->setValue(success ? "true" : "false");
                        pTxCharacteristic->notify();
                    }
                    
                    if (success) {
                        LOGGER.infof("BLE: WiFi connected, IP=%s", WiFi.localIP().toString().c_str());
                    } else {
                        LOGGER.error("BLE: WiFi connection failed");
                    }
                }
            }
        }
    }
};

void WebConfigManager::handleAPIGetBLEProvisionConfig(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.view")) {
        sendUnauthorized(request);
        return;
    }

    JsonDocument doc;
    doc["success"] = true;

    // 默认配置
    doc["data"]["bleEnabled"] = false;
    doc["data"]["bleName"] = "FBDevice";
    doc["data"]["bleTimeout"] = 300;
    doc["data"]["bleAutoStart"] = false;
    doc["data"]["bleServiceUUID"] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9F";
    doc["data"]["bleRxUUID"] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9F";
    doc["data"]["bleTxUUID"] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9F";

    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            JsonDocument cfg;
            if (!deserializeJson(cfg, f)) {
                if (cfg.containsKey("bleEnabled"))     doc["data"]["bleEnabled"] = cfg["bleEnabled"];
                if (cfg.containsKey("bleName"))        doc["data"]["bleName"] = cfg["bleName"];
                if (cfg.containsKey("bleTimeout"))     doc["data"]["bleTimeout"] = cfg["bleTimeout"];
                if (cfg.containsKey("bleAutoStart"))   doc["data"]["bleAutoStart"] = cfg["bleAutoStart"];
                if (cfg.containsKey("bleServiceUUID")) doc["data"]["bleServiceUUID"] = cfg["bleServiceUUID"];
                if (cfg.containsKey("bleRxUUID"))      doc["data"]["bleRxUUID"] = cfg["bleRxUUID"];
                if (cfg.containsKey("bleTxUUID"))      doc["data"]["bleTxUUID"] = cfg["bleTxUUID"];
            }
            f.close();
        }
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebConfigManager::handleAPISaveBLEProvisionConfig(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }

    // 先读取现有 device.json 配置
    JsonDocument cfg;
    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            deserializeJson(cfg, f);
            f.close();
        }
    }

    // 更新蓝牙配网相关字段
    cfg["bleEnabled"]     = getParamValue(request, "bleEnabled", "false") == "true";
    cfg["bleName"]        = getParamValue(request, "bleName", "FBDevice");
    cfg["bleTimeout"]     = getParamValue(request, "bleTimeout", "300").toInt();
    cfg["bleAutoStart"]   = getParamValue(request, "bleAutoStart", "false") == "true";
    cfg["bleServiceUUID"] = getParamValue(request, "bleServiceUUID", "6E400001-B5A3-F393-E0A9-E50E24DCCA9F");
    cfg["bleRxUUID"]      = getParamValue(request, "bleRxUUID", "6E400002-B5A3-F393-E0A9-E50E24DCCA9F");
    cfg["bleTxUUID"]      = getParamValue(request, "bleTxUUID", "6E400003-B5A3-F393-E0A9-E50E24DCCA9F");

    File f = LittleFS.open(DEVICE_CONFIG_FILE, "w");
    if (!f) {
        sendError(request, 500, "无法写入设备配置文件");
        return;
    }
    serializeJson(cfg, f);
    f.close();

    LOGGER.info("BLE: Config saved to device.json");
    sendSuccess(request, "蓝牙配网配置已保存");
}

void WebConfigManager::handleAPIGetBLEProvisionStatus(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.view")) {
        sendUnauthorized(request);
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["data"]["active"] = bleProvisionActive;
    doc["data"]["deviceName"] = bleDeviceName;
    
    if (bleProvisionActive) {
        unsigned long elapsed = millis() - bleProvisionStartTime;
        unsigned long remaining = (bleProvisionTimeout > elapsed) ? (bleProvisionTimeout - elapsed) / 1000 : 0;
        doc["data"]["remainingTime"] = remaining;
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebConfigManager::handleAPIStartBLEProvision(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }

    // 加载蓝牙配网配置（从 device.json）
    String deviceName = "FBDevice";
    uint32_t timeout = 300000;
    String serviceUUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9F";
    String rxUUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9F";
    String txUUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9F";

    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            JsonDocument cfg;
            if (!deserializeJson(cfg, f)) {
                if (cfg.containsKey("bleName") && cfg["bleName"].as<String>().length() > 0) {
                    deviceName = cfg["bleName"].as<String>();
                }
                if (cfg.containsKey("bleTimeout")) {
                    timeout = cfg["bleTimeout"].as<uint32_t>() * 1000;
                }
                if (cfg.containsKey("bleServiceUUID") && cfg["bleServiceUUID"].as<String>().length() > 0) {
                    serviceUUID = cfg["bleServiceUUID"].as<String>();
                }
                if (cfg.containsKey("bleRxUUID") && cfg["bleRxUUID"].as<String>().length() > 0) {
                    rxUUID = cfg["bleRxUUID"].as<String>();
                }
                if (cfg.containsKey("bleTxUUID") && cfg["bleTxUUID"].as<String>().length() > 0) {
                    txUUID = cfg["bleTxUUID"].as<String>();
                }
            }
            f.close();
        }
    }

    // 初始化 BLE
    NimBLEDevice::init(deviceName.c_str());
    pBLEServer = NimBLEDevice::createServer();
    pBLEServer->setCallbacks(new BLEProvisionServerCallbacks());

    // 创建 BLE 服务
    NimBLEService* pService = pBLEServer->createService(serviceUUID.c_str());

    // 创建 TX 特征码（发送/通知）
    pTxCharacteristic = pService->createCharacteristic(
        txUUID.c_str(),
        NIMBLE_PROPERTY::NOTIFY
    );

    // 创建 RX 特征码（接收）
    NimBLECharacteristic* pRxCharacteristic = pService->createCharacteristic(
        rxUUID.c_str(),
        NIMBLE_PROPERTY::WRITE
    );
    pRxCharacteristic->setCallbacks(new BLEProvisionCallbacks());

    // 启动服务和广播
    pService->start();
    pBLEServer->getAdvertising()->start();

    bleProvisionActive = true;
    bleDeviceName = deviceName;
    bleProvisionStartTime = millis();
    bleProvisionTimeout = timeout;

    LOGGER.infof("BLE: Provision started - Name=%s", deviceName.c_str());

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "蓝牙配网已启动";
    doc["data"]["deviceName"] = deviceName;

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebConfigManager::handleAPIStopBLEProvision(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }

    if (pBLEServer) {
        NimBLEDevice::deinit(true);
        pBLEServer = nullptr;
        pTxCharacteristic = nullptr;
    }

    bleProvisionActive = false;

    LOGGER.info("BLE: Provision stopped");
    sendSuccess(request, "蓝牙配网已停止");
}

// ============ 外设接口 API处理器 ============

#include "core/PeripheralManager.h"

void WebConfigManager::handleAPIGetPeripherals(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }
    
    // 获取类型过滤参数
    String typeFilter = getParamValue(request, "type", "");
    String categoryFilter = getParamValue(request, "category", "");
    
    PeripheralManager& pm = PeripheralManager::getInstance();
    std::vector<PeripheralConfig> peripherals;
    
    if (!typeFilter.isEmpty()) {
        PeripheralType type = parsePeripheralType(typeFilter.c_str());
        peripherals = pm.getPeripheralsByType(type);
    } else if (!categoryFilter.isEmpty()) {
        PeripheralCategory category = PeripheralCategory::CATEGORY_GPIO;
        if (categoryFilter == "communication") category = PeripheralCategory::CATEGORY_COMMUNICATION;
        else if (categoryFilter == "gpio") category = PeripheralCategory::CATEGORY_GPIO;
        else if (categoryFilter == "analog") category = PeripheralCategory::CATEGORY_ANALOG_SIGNAL;
        else if (categoryFilter == "debug") category = PeripheralCategory::CATEGORY_DEBUG;
        else if (categoryFilter == "special") category = PeripheralCategory::CATEGORY_SPECIAL;
        peripherals = pm.getPeripheralsByCategory(category);
    } else {
        peripherals = pm.getAllPeripherals();
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonArray data = doc["data"].to<JsonArray>();
    
    for (const auto& config : peripherals) {
        JsonObject obj = data.add<JsonObject>();
        obj["id"] = config.id;
        obj["name"] = config.name;
        obj["type"] = static_cast<int>(config.type);
        obj["typeName"] = getPeripheralTypeName(config.type);
        obj["category"] = getCategoryName(getPeripheralCategory(config.type));
        obj["enabled"] = config.enabled;
        
        // 引脚信息
        JsonArray pins = obj["pins"].to<JsonArray>();
        for (int i = 0; i < config.pinCount && i < 8; i++) {
            if (config.pins[i] != 255) {
                pins.add(config.pins[i]);
            }
        }
        
        // 运行时状态
        auto runtimeState = pm.getRuntimeState(config.id);
        if (runtimeState) {
            obj["status"] = static_cast<int>(runtimeState->status);
        } else {
            obj["status"] = 0;  // DISABLED
        }
    }
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebConfigManager::handleAPIGetPeripheralTypes(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }
    
    JsonDocument doc;
    doc["success"] = true;
    
    // 按类别组织类型
    JsonObject data = doc["data"].to<JsonObject>();
    
    // 通信接口
    JsonArray commTypes = data["communication"].to<JsonArray>();
    for (int i = 1; i <= 5; i++) {
        PeripheralType type = static_cast<PeripheralType>(i);
        JsonObject typeObj = commTypes.add<JsonObject>();
        typeObj["value"] = i;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
    }
    
    // GPIO接口
    JsonArray gpioTypes = data["gpio"].to<JsonArray>();
    for (int i = 11; i <= 21; i++) {
        PeripheralType type = static_cast<PeripheralType>(i);
        JsonObject typeObj = gpioTypes.add<JsonObject>();
        typeObj["value"] = i;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
    }
    
    // 模拟信号
    JsonArray analogTypes = data["analog"].to<JsonArray>();
    for (int i = 26; i <= 27; i++) {
        PeripheralType type = static_cast<PeripheralType>(i);
        JsonObject typeObj = analogTypes.add<JsonObject>();
        typeObj["value"] = i;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
    }
    
    // 调试接口
    JsonArray debugTypes = data["debug"].to<JsonArray>();
    for (int i = 31; i <= 32; i++) {
        PeripheralType type = static_cast<PeripheralType>(i);
        JsonObject typeObj = debugTypes.add<JsonObject>();
        typeObj["value"] = i;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
    }
    
    // 专用外设
    JsonArray specialTypes = data["special"].to<JsonArray>();
    for (int i = 36; i <= 45; i++) {
        PeripheralType type = static_cast<PeripheralType>(i);
        JsonObject typeObj = specialTypes.add<JsonObject>();
        typeObj["value"] = i;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
    }
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebConfigManager::handleAPIGetPeripheral(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }
    
    String id = getParamValue(request, "id", "");
    if (id.isEmpty()) {
        sendError(request, 400, "Missing id parameter");
        return;
    }
    
    PeripheralManager& pm = PeripheralManager::getInstance();
    auto config = pm.getPeripheral(id);
    
    if (!config) {
        sendError(request, 404, "Peripheral not found");
        return;
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    
    data["id"] = config->id;
    data["name"] = config->name;
    data["type"] = static_cast<int>(config->type);
    data["typeName"] = getPeripheralTypeName(config->type);
    data["enabled"] = config->enabled;
    
    // 引脚信息
    JsonArray pins = data["pins"].to<JsonArray>();
    for (int i = 0; i < config->pinCount && i < 8; i++) {
        if (config->pins[i] != 255) {
            pins.add(config->pins[i]);
        }
    }
    
    // 类型特定参数
    JsonObject params = data["params"].to<JsonObject>();
    if (config->type == PeripheralType::UART) {
        params["baudRate"] = config->params.uart.baudRate;
        params["dataBits"] = config->params.uart.dataBits;
        params["stopBits"] = config->params.uart.stopBits;
        params["parity"] = config->params.uart.parity;
    }
    else if (config->type == PeripheralType::I2C) {
        params["frequency"] = config->params.i2c.frequency;
        params["address"] = config->params.i2c.address;
        params["isMaster"] = config->params.i2c.isMaster;
    }
    else if (config->type == PeripheralType::SPI) {
        params["frequency"] = config->params.spi.frequency;
        params["mode"] = config->params.spi.mode;
        params["msbFirst"] = config->params.spi.msbFirst;
    }
    else if (config->isGPIOPeripheral()) {
        params["initialState"] = static_cast<int>(config->params.gpio.initialState);
        params["inverted"] = config->params.gpio.inverted;
        params["pwmChannel"] = config->params.gpio.pwmChannel;
        params["pwmFrequency"] = config->params.gpio.pwmFrequency;
        params["pwmResolution"] = config->params.gpio.pwmResolution;
        params["debounceMs"] = config->params.gpio.debounceMs;
    }
    else if (config->type == PeripheralType::ADC) {
        params["attenuation"] = config->params.adc.attenuation;
        params["resolution"] = config->params.adc.resolution;
    }
    else if (config->type == PeripheralType::DAC) {
        params["channel"] = config->params.dac.channel;
    }
    
    // 运行时状态
    auto runtimeState = pm.getRuntimeState(id);
    if (runtimeState) {
        JsonObject status = data["status"].to<JsonObject>();
        status["state"] = static_cast<int>(runtimeState->status);
        status["initTime"] = runtimeState->initTime;
        status["lastActivity"] = runtimeState->lastActivity;
        status["errorCount"] = runtimeState->errorCount;
    }
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebConfigManager::handleAPIAddPeripheral(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    PeripheralConfig config;
    config.id = getParamValue(request, "id", "");
    config.name = getParamValue(request, "name", "");
    config.type = static_cast<PeripheralType>(getParamInt(request, "type", 0));
    config.enabled = getParamBool(request, "enabled", true);
    
    // 解析引脚
    String pinsStr = getParamValue(request, "pins", "");
    config.pinCount = 0;
    if (!pinsStr.isEmpty()) {
        int start = 0;
        int end = pinsStr.indexOf(',');
        while (end != -1 && config.pinCount < 8) {
            config.pins[config.pinCount++] = pinsStr.substring(start, end).toInt();
            start = end + 1;
            end = pinsStr.indexOf(',', start);
        }
        if (config.pinCount < 8) {
            config.pins[config.pinCount++] = pinsStr.substring(start).toInt();
        }
    }
    
    // 类型特定参数
    if (config.type == PeripheralType::UART) {
        config.params.uart.baudRate = getParamInt(request, "baudRate", 115200);
        config.params.uart.dataBits = getParamInt(request, "dataBits", 8);
        config.params.uart.stopBits = getParamInt(request, "stopBits", 1);
        config.params.uart.parity = getParamInt(request, "parity", 0);
    }
    else if (config.type == PeripheralType::I2C) {
        config.params.i2c.frequency = getParamInt(request, "frequency", 100000);
        config.params.i2c.address = getParamInt(request, "address", 0);
        config.params.i2c.isMaster = getParamBool(request, "isMaster", true);
    }
    else if (config.type == PeripheralType::SPI) {
        config.params.spi.frequency = getParamInt(request, "frequency", 1000000);
        config.params.spi.mode = getParamInt(request, "mode", 0);
        config.params.spi.msbFirst = getParamBool(request, "msbFirst", true);
    }
    else if (config.isGPIOPeripheral()) {
        config.params.gpio.initialState = static_cast<GPIOState>(getParamInt(request, "initialState", 0));
        config.params.gpio.inverted = getParamBool(request, "inverted", false);
        config.params.gpio.pwmChannel = getParamInt(request, "pwmChannel", 0);
        config.params.gpio.pwmFrequency = getParamInt(request, "pwmFrequency", 1000);
        config.params.gpio.pwmResolution = getParamInt(request, "pwmResolution", 8);
        config.params.gpio.debounceMs = getParamInt(request, "debounceMs", 50);
    }
    else if (config.type == PeripheralType::ADC) {
        config.params.adc.attenuation = getParamInt(request, "attenuation", 0);
        config.params.adc.resolution = getParamInt(request, "resolution", 12);
    }
    else if (config.type == PeripheralType::DAC) {
        config.params.dac.channel = getParamInt(request, "channel", 1);
    }
    
    // 生成ID（如果未提供）
    if (config.id.isEmpty()) {
        config.id = "periph_" + String(millis());
    }
    
    PeripheralManager& pm = PeripheralManager::getInstance();
    if (pm.addPeripheral(config)) {
        pm.saveConfiguration();
        sendSuccess(request, "外设添加成功");
    } else {
        sendError(request, 400, "添加外设失败，请检查配置");
    }
}

void WebConfigManager::handleAPIUpdatePeripheral(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    String id = getParamValue(request, "id", "");
    if (id.isEmpty()) {
        sendError(request, 400, "Missing id parameter");
        return;
    }
    
    PeripheralManager& pm = PeripheralManager::getInstance();
    auto existing = pm.getPeripheral(id);
    if (!existing) {
        sendError(request, 404, "Peripheral not found");
        return;
    }
    
    PeripheralConfig config = *existing;
    
    // 更新基本字段
    String name = getParamValue(request, "name", "");
    if (!name.isEmpty()) config.name = name;
    
    config.enabled = getParamBool(request, "enabled", config.enabled);
    
    // 更新引脚（如果提供）
    String pinsStr = getParamValue(request, "pins", "");
    if (!pinsStr.isEmpty()) {
        config.pinCount = 0;
        int start = 0;
        int end = pinsStr.indexOf(',');
        while (end != -1 && config.pinCount < 8) {
            config.pins[config.pinCount++] = pinsStr.substring(start, end).toInt();
            start = end + 1;
            end = pinsStr.indexOf(',', start);
        }
        if (config.pinCount < 8) {
            config.pins[config.pinCount++] = pinsStr.substring(start).toInt();
        }
    }
    
    // 更新类型特定参数
    if (config.type == PeripheralType::UART) {
        if (request->hasParam("baudRate", true)) config.params.uart.baudRate = getParamInt(request, "baudRate", config.params.uart.baudRate);
        if (request->hasParam("dataBits", true)) config.params.uart.dataBits = getParamInt(request, "dataBits", config.params.uart.dataBits);
    }
    else if (config.isGPIOPeripheral()) {
        if (request->hasParam("inverted", true)) config.params.gpio.inverted = getParamBool(request, "inverted", config.params.gpio.inverted);
        if (request->hasParam("pwmFrequency", true)) config.params.gpio.pwmFrequency = getParamInt(request, "pwmFrequency", config.params.gpio.pwmFrequency);
    }
    
    if (pm.updatePeripheral(id, config)) {
        pm.saveConfiguration();
        sendSuccess(request, "外设更新成功");
    } else {
        sendError(request, 400, "更新外设失败");
    }
}

void WebConfigManager::handleAPIDeletePeripheral(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    String id = getParamValue(request, "id", "");
    if (id.isEmpty()) {
        sendError(request, 400, "Missing id parameter");
        return;
    }
    
    PeripheralManager& pm = PeripheralManager::getInstance();
    if (pm.removePeripheral(id)) {
        pm.saveConfiguration();
        sendSuccess(request, "外设删除成功");
    } else {
        sendError(request, 404, "外设不存在");
    }
}

void WebConfigManager::handleAPIEnablePeripheral(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    String id = getParamValue(request, "id", "");
    if (id.isEmpty()) {
        sendError(request, 400, "Missing id parameter");
        return;
    }
    
    PeripheralManager& pm = PeripheralManager::getInstance();
    if (pm.enablePeripheral(id)) {
        pm.saveConfiguration();
        sendSuccess(request, "外设已启用");
    } else {
        sendError(request, 400, "启用外设失败");
    }
}

void WebConfigManager::handleAPIDisablePeripheral(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    String id = getParamValue(request, "id", "");
    if (id.isEmpty()) {
        sendError(request, 400, "Missing id parameter");
        return;
    }
    
    PeripheralManager& pm = PeripheralManager::getInstance();
    if (pm.disablePeripheral(id)) {
        pm.saveConfiguration();
        sendSuccess(request, "外设已禁用");
    } else {
        sendError(request, 400, "禁用外设失败");
    }
}

void WebConfigManager::handleAPIGetPeripheralStatus(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }
    
    String id = getParamValue(request, "id", "");
    if (id.isEmpty()) {
        sendError(request, 400, "Missing id parameter");
        return;
    }
    
    PeripheralManager& pm = PeripheralManager::getInstance();
    auto runtimeState = pm.getRuntimeState(id);
    auto config = pm.getPeripheral(id);
    
    if (!config) {
        sendError(request, 404, "Peripheral not found");
        return;
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    
    data["id"] = id;
    data["enabled"] = config->enabled;
    data["status"] = runtimeState ? static_cast<int>(runtimeState->status) : 0;
    
    if (runtimeState) {
        data["initTime"] = runtimeState->initTime;
        data["lastActivity"] = runtimeState->lastActivity;
        data["errorCount"] = runtimeState->errorCount;
        if (!runtimeState->lastError.isEmpty()) {
            data["lastError"] = runtimeState->lastError;
        }
    }
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebConfigManager::handleAPIReadPeripheral(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "system.view")) {
        sendUnauthorized(request);
        return;
    }
    
    String id = getParamValue(request, "id", "");
    if (id.isEmpty()) {
        sendError(request, 400, "Missing id parameter");
        return;
    }
    
    PeripheralManager& pm = PeripheralManager::getInstance();
    auto config = pm.getPeripheral(id);
    
    if (!config) {
        sendError(request, 404, "Peripheral not found");
        return;
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    
    data["id"] = id;
    
    // 根据类型读取数据
    if (config->isGPIOPeripheral()) {
        GPIOState state = pm.readPin(id);
        data["state"] = static_cast<int>(state);
        data["stateName"] = (state == GPIOState::STATE_HIGH) ? "HIGH" : (state == GPIOState::STATE_LOW) ? "LOW" : "UNDEFINED";
        
        // 模拟输入返回数值
        if (config->type == PeripheralType::GPIO_ANALOG_INPUT || config->type == PeripheralType::ADC) {
            data["value"] = pm.readAnalog(id);
        }
    } else {
        // 其他类型外设的数据读取（待实现）
        data["value"] = nullptr;
    }
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebConfigManager::handleAPIWritePeripheral(AsyncWebServerRequest* request) {
    if (!checkPermission(request, "config.edit")) {
        sendUnauthorized(request);
        return;
    }
    
    String id = getParamValue(request, "id", "");
    if (id.isEmpty()) {
        sendError(request, 400, "Missing id parameter");
        return;
    }
    
    PeripheralManager& pm = PeripheralManager::getInstance();
    auto config = pm.getPeripheral(id);
    
    if (!config) {
        sendError(request, 404, "Peripheral not found");
        return;
    }
    
    bool success = false;
    
    if (config->isGPIOPeripheral()) {
        // GPIO写入
        if (request->hasParam("state", true)) {
            int stateValue = getParamInt(request, "state", 0);
            GPIOState state = (stateValue == 1) ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
            success = pm.writePin(id, state);
        }
        else if (request->hasParam("toggle", true)) {
            success = pm.togglePin(id);
        }
        else if (request->hasParam("pwm", true) && 
                 (config->type == PeripheralType::GPIO_PWM_OUTPUT || config->type == PeripheralType::PWM_SERVO)) {
            uint32_t dutyCycle = getParamInt(request, "pwm", 0);
            success = pm.writePWM(id, dutyCycle);
        }
    } else {
        // 其他类型外设的数据写入（待实现）
        success = false;
    }
    
    if (success) {
        sendSuccess(request, "数据写入成功");
    } else {
        sendError(request, 400, "数据写入失败");
    }
}

