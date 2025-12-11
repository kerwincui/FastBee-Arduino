#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <core/ConfigDefines.h>

// HTTP配置结构体
struct HTTPConfig {
    String baseURL;
    uint16_t timeout;
    String contentType;
};

class HTTPClientWrapper {
public:
    HTTPClientWrapper();
    ~HTTPClientWrapper();

    bool begin(const HTTPConfig& config);
    // 从LittleFS加载配置
    bool beginFromConfig(const char* configPath = CONFIG_FILE_HTTP);

    void end();
    bool get(const String& endpoint, String& response);
    bool post(const String& endpoint, const String& data, String& response);
    bool put(const String& endpoint, const String& data, String& response);
    bool del(const String& endpoint, String& response);
    void handle();
    String getStatus() const;
    
    // 简化接口（用于ProtocolManager）
    bool post(const String& endpoint, const String& data);
    
    // 设置响应回调
    void setResponseCallback(std::function<void(const String&, const String&)> callback);

private:
    HTTPConfig config;
    WiFiClient wifiClient;
    ::HTTPClient httpClient;
    bool isInitialized;
    
    std::function<void(const String&, const String&)> responseCallback;
    
    String buildURL(const String& endpoint);
};

#endif