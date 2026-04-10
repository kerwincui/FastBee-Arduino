#pragma once
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Arduino.h>

namespace HandlerUtils {

// 发送 JSON 成功响应
inline void sendJsonSuccess(AsyncWebServerRequest* request, const String& data = "") {
    String json = "{\"success\":true";
    if (data.length() > 0) {
        json += ",\"data\":";
        json += data;
    }
    json += "}";
    request->send(200, "application/json", json);
}

// 发送带消息的成功响应
inline void sendJsonSuccessMsg(AsyncWebServerRequest* request, const char* message) {
    String json = "{\"success\":true,\"message\":\"";
    json += message;
    json += "\"}";
    request->send(200, "application/json", json);
}

// 发送 JSON 错误响应
inline void sendJsonError(AsyncWebServerRequest* request, int code, const char* message) {
    String json = "{\"success\":false,\"message\":\"";
    json += message;
    json += "\"}";
    request->send(code, "application/json", json);
}

// 内存检查 - 返回 true 表示内存不足（已发送 503 响应）
inline bool checkLowMemory(AsyncWebServerRequest* request, size_t threshold = 20480) {
    if (ESP.getFreeHeap() < threshold) {
        sendJsonError(request, 503, "Low memory");
        return true;
    }
    return false;
}

// 安全获取 query 参数
inline String getParam(AsyncWebServerRequest* request, const char* name, const String& defaultVal = "") {
    if (request->hasParam(name)) {
        return request->getParam(name)->value();
    }
    return defaultVal;
}

// 安全获取整数参数
inline int getParamInt(AsyncWebServerRequest* request, const char* name, int defaultVal = 0) {
    if (request->hasParam(name)) {
        return request->getParam(name)->value().toInt();
    }
    return defaultVal;
}

// 安全获取布尔参数
inline bool getParamBool(AsyncWebServerRequest* request, const char* name, bool defaultVal = false) {
    if (request->hasParam(name)) {
        String val = request->getParam(name)->value();
        return val == "true" || val == "1";
    }
    return defaultVal;
}

} // namespace HandlerUtils
