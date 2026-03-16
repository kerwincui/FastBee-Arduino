#ifndef WEB_HANDLER_CONTEXT_H
#define WEB_HANDLER_CONTEXT_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>

// 接口包含
#include "core/interfaces/IAuthManager.h"
#include "core/interfaces/IUserManager.h"

// 前向声明
class AuthManager;
class UserManager;
class RoleManager;
class NetworkManager;
class OTAManager;
class ProtocolManager;

// JSON 文档大小限制
#define JSON_DOC_SMALL  512
#define JSON_DOC_MEDIUM 2048
#define JSON_DOC_LARGE  4096

/**
 * @brief Web处理器共享上下文
 * 
 * 提供所有 Handler 共用的管理器指针、参数提取、认证检查、
 * 响应生成和文件服务等工具方法。各 Handler 通过组合持有
 * WebHandlerContext* 来使用这些功能，避免重复代码。
 */
class WebHandlerContext {
public:
    // ============ 管理器指针（Handler 可直接读取）============
    AsyncWebServer* server;
    IAuthManager*   authManager;
    IUserManager*   userManager;
    RoleManager*    roleManager;
    NetworkManager* networkManager;
    OTAManager*     otaManager;
    ProtocolManager* protocolManager;

    // ============ 共享状态 ============
    String webRootPath;
    bool   scheduleRestart;
    unsigned long scheduledRestartTime;

    WebHandlerContext(AsyncWebServer* srv, IAuthManager* authMgr, IUserManager* userMgr);
    ~WebHandlerContext() = default;

    // 禁止拷贝
    WebHandlerContext(const WebHandlerContext&) = delete;
    WebHandlerContext& operator=(const WebHandlerContext&) = delete;

    // ============ 参数处理辅助方法 ============
    String getParamValue(AsyncWebServerRequest* request, const String& paramName,
                         const String& defaultValue = "");
    bool   getParamBool(AsyncWebServerRequest* request, const String& paramName,
                        bool defaultValue = false);
    int    getParamInt(AsyncWebServerRequest* request, const String& paramName,
                       int defaultValue = 0);

    // ============ 认证辅助方法 ============
    bool       requiresAuth(AsyncWebServerRequest* request);
    AuthResult authenticateRequest(AsyncWebServerRequest* request);
    String     getClientIP(AsyncWebServerRequest* request);
    String     getUserAgent(AsyncWebServerRequest* request);
    bool       checkPermission(AsyncWebServerRequest* request, const String& permission);

    // ============ 响应辅助方法 ============
    void sendJsonResponse(AsyncWebServerRequest* request, int code,
                          const JsonDocument& doc);
    void sendSuccess(AsyncWebServerRequest* request, const JsonDocument& data);
    void sendSuccess(AsyncWebServerRequest* request, const String& message = "");
    void sendError(AsyncWebServerRequest* request, int code, const String& message);
    void sendUnauthorized(AsyncWebServerRequest* request);
    void sendForbidden(AsyncWebServerRequest* request);
    void sendNotFound(AsyncWebServerRequest* request);
    void sendBadRequest(AsyncWebServerRequest* request, const String& message);

    // ============ 内置页面 ============
    void sendBuiltinLoginPage(AsyncWebServerRequest* request);
    void sendBuiltinDashboard(AsyncWebServerRequest* request);
    void sendBuiltinUsersPage(AsyncWebServerRequest* request);
    void sendBuiltinSetupPage(AsyncWebServerRequest* request);

    // ============ 文件服务方法 ============
    bool   serveStaticFile(AsyncWebServerRequest* request, const String& path);
    void   serveGzippedFile(AsyncWebServerRequest* request, const String& path);
    String getContentType(const String& filename);
    String readFile(const String& path);
    bool   fileExists(const String& path);

    // ============ 工具方法 ============
    String formatUptime(unsigned long ms);
};

#endif // WEB_HANDLER_CONTEXT_H
