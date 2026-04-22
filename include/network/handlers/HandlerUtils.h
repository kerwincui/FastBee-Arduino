#pragma once
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <LittleFS.h>

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
// 同时检查总空闲堆和最大连续块（检测碎片化）
inline bool checkLowMemory(AsyncWebServerRequest* request, size_t threshold = 20480) {
    if (ESP.getFreeHeap() < threshold || ESP.getMaxAllocHeap() < 8192) {
        sendJsonError(request, 503, "Low memory");
        return true;
    }
    return false;
}

// 将字符串以 JSON 转义格式写入流（含引号），避免中间 String/JsonDocument 拷贝
inline void writeJsonEscapedString(AsyncResponseStream* response, const String& str) {
    response->print('"');
    for (size_t i = 0; i < str.length(); i++) {
        char c = str[i];
        switch (c) {
            case '"':  response->print("\\\""); break;
            case '\\': response->print("\\\\"); break;
            case '\n': response->print("\\n"); break;
            case '\r': response->print("\\r"); break;
            case '\t': response->print("\\t"); break;
            default:
                if ((uint8_t)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (uint8_t)c);
                    response->print(buf);
                } else {
                    response->write((uint8_t)c);
                }
        }
    }
    response->print('"');
}

// 发送 JSON 文档（序列化到 String 后一次性发送，避免 AsyncResponseStream cbuf 扩容 OOM）
inline void sendJsonStream(AsyncWebServerRequest* request, JsonDocument& doc) {
    if (checkLowMemory(request)) return;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

// 发送带 success:true 包装的 JSON 文档
inline void sendJsonDocSuccess(AsyncWebServerRequest* request, JsonDocument& doc) {
    if (checkLowMemory(request)) return;
    String json;
    json.reserve(measureJson(doc) + 32);
    json += F("{\"success\":true,\"data\":");
    String inner;
    serializeJson(doc, inner);
    json += inner;
    json += '}';
    request->send(200, "application/json", json);
}

// 统一的 "read JSON file -> deserialize -> modify -> serialize -> write" 流程
// 返回 true 表示成功
inline bool updateJsonConfig(const char* path, std::function<void(JsonDocument&)> modifier) {
    JsonDocument doc;
    if (LittleFS.exists(path)) {
        File f = LittleFS.open(path, "r");
        if (f) {
            deserializeJson(doc, f);
            f.close();
        }
    }
    modifier(doc);
    File out = LittleFS.open(path, "w");
    if (!out) return false;
    serializeJson(doc, out);
    out.close();
    return true;
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
