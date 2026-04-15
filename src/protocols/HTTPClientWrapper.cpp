/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:30:53
 */

#include "protocols/HTTPClientWrapper.h"

#if FASTBEE_ENABLE_HTTP

#include <ArduinoJson.h>
#include <LittleFS.h>


HTTPClientWrapper::HTTPClientWrapper() 
    : isInitialized(false) {
}

HTTPClientWrapper::~HTTPClientWrapper() {
    end();
}

bool HTTPClientWrapper::begin(const HTTPConfig& config) {
    this->config = config;
    isInitialized = true;
    
    Serial.printf("[HTTPClient] Initialized with baseURL: %s, timeout: %dms\n", 
                  config.baseURL.c_str(), config.timeout);
    return true;
}

bool HTTPClientWrapper::beginFromConfig(const char* configPath) {
    if (!LittleFS.begin()) {
        Serial.println("[HTTPClient] Failed to mount LittleFS");
        return false;
    }

    File configFile = LittleFS.open(configPath, "r");
    if (!configFile) {
        Serial.printf("[HTTPClient] Failed to open config file: %s\n", configPath);
        return false;
    }

    size_t size = configFile.size();
    std::unique_ptr<char[]> buf(new char[size]);
    configFile.readBytes(buf.get(), size);
    configFile.close();

    // 使用静态JSON文档减少内存碎片
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, buf.get());
    if (error) {
        Serial.printf("[HTTPClient] JSON deserialization failed: %s\n", error.c_str());
        return false;
    }

    // 解析配置
    config.baseURL = doc["baseURL"] | "";
    config.timeout = doc["timeout"] | 10000;
    config.contentType = doc["contentType"] | "application/json";

    if (config.baseURL.isEmpty()) {
        Serial.println("[HTTPClient] baseURL is empty in config file");
        return false;
    }

    isInitialized = true;
    Serial.printf("[HTTPClient] Config loaded: baseURL=%s, timeout=%d, contentType=%s\n",
                  config.baseURL.c_str(), config.timeout, config.contentType.c_str());
    return true;
}

void HTTPClientWrapper::end() {
    if (isInitialized) {
        httpClient.end();
        isInitialized = false;
        Serial.println("[HTTPClient] Ended");
    }
}

String HTTPClientWrapper::buildURL(const String& endpoint) {
    String url = config.baseURL;
    if (!url.endsWith("/") && !endpoint.startsWith("/")) {
        url += "/";
    }
    url += endpoint;
    return url;
}

// 应用认证请求头
static void applyAuthHeaders(::HTTPClient& httpClient, const HTTPConfig& config) {
    applyAuthHeaders(httpClient, config);
    if (config.authType == "basic" && !config.authUser.isEmpty()) {
        httpClient.setAuthorization(config.authUser.c_str(), config.authToken.c_str());
    } else if (config.authType == "bearer" && !config.authToken.isEmpty()) {
        httpClient.addHeader("Authorization", "Bearer " + config.authToken);
    }
}

bool HTTPClientWrapper::get(const String& endpoint, String& response) {
    if (!isInitialized) {
        Serial.println("[HTTPClient] Not initialized");
        return false;
    }

    String url = buildURL(endpoint);
    Serial.printf("[HTTPClient] GET: %s\n", url.c_str());

    httpClient.begin(wifiClient, url);
    httpClient.setTimeout(config.timeout);
    applyAuthHeaders(httpClient, config);

    int httpCode = httpClient.GET();
    bool success = (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED);
    
    if (success) {
        response = httpClient.getString();
        Serial.printf("[HTTPClient] GET success, response length: %d\n", response.length());
        
        // 触发回调
        if (responseCallback) {
            responseCallback(endpoint, response);
        }
    } else {
        Serial.printf("[HTTPClient] GET failed, code: %d, error: %s\n", 
                      httpCode, httpClient.errorToString(httpCode).c_str());
        response = httpClient.getString(); // 可能包含错误信息
    }

    httpClient.end();
    return success;
}

bool HTTPClientWrapper::post(const String& endpoint, const String& data, String& response) {
    if (!isInitialized) {
        Serial.println("[HTTPClient] Not initialized");
        return false;
    }

    String url = buildURL(endpoint);
    Serial.printf("[HTTPClient] POST: %s, data: %s\n", url.c_str(), data.c_str());

    httpClient.begin(wifiClient, url);
    httpClient.setTimeout(config.timeout);
    applyAuthHeaders(httpClient, config);

    int httpCode = httpClient.POST(data);
    bool success = (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED);
    
    if (success) {
        response = httpClient.getString();
        Serial.printf("[HTTPClient] POST success, response length: %d\n", response.length());
        
        // 触发回调
        if (responseCallback) {
            responseCallback(endpoint, response);
        }
    } else {
        Serial.printf("[HTTPClient] POST failed, code: %d, error: %s\n", 
                      httpCode, httpClient.errorToString(httpCode).c_str());
        response = httpClient.getString();
    }

    httpClient.end();
    return success;
}

bool HTTPClientWrapper::post(const String& endpoint, const String& data) {
    String dummyResponse;
    return post(endpoint, data, dummyResponse);
}

bool HTTPClientWrapper::put(const String& endpoint, const String& data, String& response) {
    if (!isInitialized) {
        Serial.println("[HTTPClient] Not initialized");
        return false;
    }

    String url = buildURL(endpoint);
    Serial.printf("[HTTPClient] PUT: %s, data: %s\n", url.c_str(), data.c_str());

    httpClient.begin(wifiClient, url);
    httpClient.setTimeout(config.timeout);
    applyAuthHeaders(httpClient, config);

    int httpCode = httpClient.PUT(data);
    bool success = (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED);
    
    if (success) {
        response = httpClient.getString();
        Serial.printf("[HTTPClient] PUT success, response length: %d\n", response.length());
        
        // 触发回调
        if (responseCallback) {
            responseCallback(endpoint, response);
        }
    } else {
        Serial.printf("[HTTPClient] PUT failed, code: %d, error: %s\n", 
                      httpCode, httpClient.errorToString(httpCode).c_str());
        response = httpClient.getString();
    }

    httpClient.end();
    return success;
}

bool HTTPClientWrapper::del(const String& endpoint, String& response) {
    if (!isInitialized) {
        Serial.println("[HTTPClient] Not initialized");
        return false;
    }

    String url = buildURL(endpoint);
    Serial.printf("[HTTPClient] DELETE: %s\n", url.c_str());

    httpClient.begin(wifiClient, url);
    httpClient.setTimeout(config.timeout);
    applyAuthHeaders(httpClient, config);

    int httpCode = httpClient.sendRequest("DELETE");
    bool success = (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_NO_CONTENT);
    
    if (success) {
        response = httpClient.getString();
        Serial.printf("[HTTPClient] DELETE success, response length: %d\n", response.length());
        
        // 触发回调
        if (responseCallback) {
            responseCallback(endpoint, response);
        }
    } else {
        Serial.printf("[HTTPClient] DELETE failed, code: %d, error: %s\n", 
                      httpCode, httpClient.errorToString(httpCode).c_str());
        response = httpClient.getString();
    }

    httpClient.end();
    return success;
}

void HTTPClientWrapper::handle() {
    // 当前实现为同步，如果需要异步处理可以在这里实现
    // 可以在这里处理重连、心跳等逻辑
}

String HTTPClientWrapper::getStatus() const {
    if (!isInitialized) {
        return "Not initialized";
    }
    
    String status = "Initialized - BaseURL: " + config.baseURL;
    status += ", Timeout: " + String(config.timeout);
    status += ", ContentType: " + config.contentType;
    return status;
}

void HTTPClientWrapper::setResponseCallback(std::function<void(const String&, const String&)> callback) {
    responseCallback = callback;
}

#endif // FASTBEE_ENABLE_HTTP