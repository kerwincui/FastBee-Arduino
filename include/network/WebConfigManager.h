#ifndef WEB_CONFIG_MANAGER_H
#define WEB_CONFIG_MANAGER_H

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <utils/StringUtils.h>
#include "NetworkManager.h"

class WebConfigManager {
private:
    AsyncWebServer* server;
    Preferences prefs;

    bool isRunning;            //服务器运行状态
    
    // 认证状态
    bool isAuthenticated(AsyncWebServerRequest *request);
    bool checkAuth(const String& username, const String& password);
    
    // Web路由处理
    void handleRoot(AsyncWebServerRequest *request);
    void handleLogin(AsyncWebServerRequest *request);
    void handleLogout(AsyncWebServerRequest *request);
    void handleSystemConfig(AsyncWebServerRequest *request);
    void handleNetworkConfig(AsyncWebServerRequest *request);
    void handleUserManagement(AsyncWebServerRequest *request);
    void handleDeviceMonitor(AsyncWebServerRequest *request);
    void handleOTAUpdate(AsyncWebServerRequest *request);
    void handleProtocolConfig(AsyncWebServerRequest *request);

    // 发送成功响应
    void sendSuccessResponse(AsyncWebServerRequest* request, const String& jsonBody);
    // 发送错误响应
    void sendErrorResponse(AsyncWebServerRequest* request, int errorCode, const String& errorMessage);
        
    // API端点
    void handleAPIStatus(AsyncWebServerRequest *request);
    void handleAPIConfig(AsyncWebServerRequest *request);
    void handleAPIUsers(AsyncWebServerRequest *request);
    void handleAPIDeviceInfo(AsyncWebServerRequest *request);
    void handleHealthCheck(AsyncWebServerRequest* request);
    void handleSystemInfo(AsyncWebServerRequest* request);
    void handleSystemRestart(AsyncWebServerRequest* request);
    void handleAPIFileSystemInfo(AsyncWebServerRequest* request);
    // 网络API处理器
    void handleAPINetworkConfig(AsyncWebServerRequest* request);
    void handleAPINetworkApply(AsyncWebServerRequest* request);
    void handleAPINetworkStatus(AsyncWebServerRequest* request);
    void handleAPINetworkScan(AsyncWebServerRequest* request);
    void handleAPINetworkTest(AsyncWebServerRequest* request);
    void handleAPINetworkReset(AsyncWebServerRequest* request);
    void handleAPIGenerateBackupIPs(AsyncWebServerRequest* request);
    void handleAPISwitchRandomIP(AsyncWebServerRequest* request);
    void handleAPIIPConflictCheck(AsyncWebServerRequest* request);
    void handleAPIFailover(AsyncWebServerRequest* request);
    void handleAPINetworkDiagnostic(AsyncWebServerRequest* request);
    void handleAPINetworkConfig_GET(AsyncWebServerRequest* request, NetworkManager* networkManager);
    void handleAPINetworkConfig_POST(AsyncWebServerRequest* request, NetworkManager* networkManager);
    
    // 辅助函数
    String getNetworkStatusText(NetworkStatus status);
    
    // 文件服务方法 
    bool serveFile(AsyncWebServerRequest *request,const String& path, const String& contentType = "text/html");
    String readHTMLFile(const String& filename);
    bool replacePlaceholders(String& content, const std::vector<std::pair<String, String>>& replacements);
    void setupRoutes();
    void loadConfiguration();

    // 静态文件过滤gzip文件（AsyncWebServer会默认查找）
    static bool requestFilter(AsyncWebServerRequest* request);
    
public:
    WebConfigManager(AsyncWebServer* webServerPtr);
    ~WebConfigManager();

     /**
     * @brief 初始化Web配置管理器
     * @return 初始化是否成功
     */
    bool initialize();

    /**
     * @brief 优雅关闭Web服务器和清理资源
     */
    void shutdown();

    // 配置管理
    bool saveSystemConfig(const JsonDocument& config);
    bool loadSystemConfig(JsonDocument& config);
    bool saveNetworkConfig(const JsonDocument& config);
    bool loadNetworkConfig(JsonDocument& config);
};

#endif