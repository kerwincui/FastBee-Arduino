#pragma once
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <LittleFS.h>
#include <vector>
#include <memory>
#include <cstring>
#include "core/FastBeeFramework.h"
#include "systems/HealthMonitor.h"
#include "utils/PsramJsonDocument.h"

namespace HandlerUtils {

inline void sendWithClose(AsyncWebServerRequest* request, AsyncWebServerResponse* response) {
    if (!request || !response) return;
    response->addHeader("Connection", "close");
    request->send(response);
}

// 发送 JSON 成功响应
inline void sendJsonSuccess(AsyncWebServerRequest* request, const String& data = "") {
    String json;
    json.reserve(20 + data.length());
    json += F("{\"success\":true");
    if (data.length() > 0) {
        json += F(",\"data\":");
        json += data;
    }
    json += '}';
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
    sendWithClose(request, response);
}

// 发送带消息的成功响应
inline void sendJsonSuccessMsg(AsyncWebServerRequest* request, const char* message) {
    String json = "{\"success\":true,\"message\":\"";
    json += message;
    json += "\"}";
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
    sendWithClose(request, response);
}

// 发送 JSON 错误响应
inline void sendJsonError(AsyncWebServerRequest* request, int code, const char* message, int retryAfterSeconds = 0) {
    String json = "{\"success\":false,\"message\":\"";
    json += message;
    json += "\",\"error\":\"";
    json += message;
    json += "\",\"code\":";
    json += String(code);
    if (retryAfterSeconds > 0) {
        json += ",\"retryAfter\":";
        json += String(retryAfterSeconds);
    }
    if (code == 503) {
        json += ",\"memory\":{\"heapFree\":";
        json += String((unsigned long)ESP.getFreeHeap());
        json += ",\"heapMaxAlloc\":";
        json += String((unsigned long)ESP.getMaxAllocHeap());
#if FASTBEE_USE_PSRAM
        json += ",\"psramTotal\":";
        json += String((unsigned long)ESP.getPsramSize());
        json += ",\"psramFree\":";
        json += String((unsigned long)ESP.getFreePsram());
#endif
        json += "}";
    }
    json += "}";
    AsyncWebServerResponse* response = request->beginResponse(code, "application/json", json);
    if (retryAfterSeconds > 0) {
        response->addHeader("Retry-After", String(retryAfterSeconds));
    }
    sendWithClose(request, response);
}

inline const char* memoryGuardLevelName(MemoryGuardLevel level) {
    switch (level) {
        case MemoryGuardLevel::WARN: return "WARN";
        case MemoryGuardLevel::SEVERE: return "SEVERE";
        case MemoryGuardLevel::CRITICAL: return "CRITICAL";
        default: return "NORMAL";
    }
}

inline bool internalHeapCriticallyLow(uint32_t freeHeap = ESP.getFreeHeap(),
                                      uint32_t maxAlloc = ESP.getMaxAllocHeap()) {
    return freeHeap < 2048 || maxAlloc < 1024;
}

inline bool psramCanServeJson(size_t estimatedBytes = 8192) {
    return FastBee::psramAvailableForJson(estimatedBytes + 4096) &&
           !internalHeapCriticallyLow();
}

inline bool rejectHeavyRequestOnPressure(
    AsyncWebServerRequest* request,
    const char* requestLabel = "Heavy request",
    MemoryGuardLevel rejectLevel = MemoryGuardLevel::SEVERE,
    int retryAfterSeconds = 5) {
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    if (!monitor) return false;

    MemoryGuardLevel currentLevel = monitor->getMemoryGuardLevel();
    if (static_cast<uint8_t>(currentLevel) < static_cast<uint8_t>(rejectLevel)) {
        return false;
    }
    if (psramCanServeJson(16384)) {
        return false;
    }

    const char* label = (requestLabel && requestLabel[0] != '\0')
        ? requestLabel
        : "Heavy request";
    char msg[128];
    snprintf(msg, sizeof(msg), "%s temporarily unavailable (%s memory pressure)",
             label, memoryGuardLevelName(currentLevel));
    sendJsonError(request, 503, msg, retryAfterSeconds);
    return true;
}

// 内存检查 - 返回 true 表示内存不足（已发送 503 响应）
// 同时检查总空闲堆和最大连续块（检测碎片化）
// 阈值选定（路由瘦身后调低）：启动 heap=179984，仅在真正资源耗尽边缘熔断
inline bool checkLowMemory(AsyncWebServerRequest* request, size_t threshold = 8192) {
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t maxAlloc = ESP.getMaxAllocHeap();
    uint32_t minMaxAlloc = (threshold >= 16384) ? 6144 : 4096;
    if (freeHeap < threshold || maxAlloc < minMaxAlloc) {
        if (FastBee::psramAvailableForJson(threshold + 4096) &&
            !internalHeapCriticallyLow(freeHeap, maxAlloc)) {
            return false;
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "Low memory: heap=%lu maxAlloc=%lu", (unsigned long)freeHeap, (unsigned long)maxAlloc);
        sendJsonError(request, 503, msg, 5);
        return true;
    }
    return false;
}

// 带响应大小预估的内存检查 - 返回 true 表示内存不足
inline bool checkResponseMemory(AsyncWebServerRequest* request, size_t responseSize) {
    uint32_t maxAlloc = ESP.getMaxAllocHeap();
    size_t safeSize = responseSize + 512;  // 预留 512 字节安全余量
    if (maxAlloc < safeSize) {
        if (FastBee::psramAvailableForJson(safeSize + 4096) &&
            !internalHeapCriticallyLow()) {
            return false;
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "Low memory: need ~%u bytes, maxAlloc=%lu",
                 (unsigned)safeSize, (unsigned long)maxAlloc);
        AsyncWebServerResponse* response = request->beginResponse(503, "text/plain", msg);
        response->addHeader("Retry-After", "5");
        sendWithClose(request, response);
        return true;
    }
    return false;
}

// 将字符串以 JSON 转义格式写入流（含引号），避免中间 String/JsonDocument 拷贝
inline bool sendJsonFromPsramBuffer(AsyncWebServerRequest* request, const JsonDocument& doc, size_t responseSize) {
    if (!FastBee::psramAvailableForJson(responseSize + 4096)) {
        return false;
    }

    char* raw = static_cast<char*>(
        heap_caps_malloc(responseSize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!raw) {
        return false;
    }

    size_t len = serializeJson(doc, raw, responseSize + 1);
    raw[len] = '\0';

    struct State {
        std::shared_ptr<char> data;
        size_t len = 0;
    };
    auto state = std::make_shared<State>();
    state->data = std::shared_ptr<char>(raw, [](char* ptr) {
        if (ptr) heap_caps_free(ptr);
    });
    state->len = len;

    AsyncWebServerResponse* response = request->beginResponse(
        "application/json",
        state->len,
        [state](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
            if (index >= state->len) return 0;
            size_t remain = state->len - index;
            size_t copy = remain < maxLen ? remain : maxLen;
            memcpy(buffer, state->data.get() + index, copy);
            return copy;
        });
    if (!response) {
        return false;
    }
    response->addHeader("Connection", "close");
    request->send(response);
    return true;
}

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
    const size_t responseSize = measureJson(doc);
    if (checkResponseMemory(request, responseSize)) return;
    if ((responseSize >= 2048 || responseSize + 512 > ESP.getMaxAllocHeap()) &&
        sendJsonFromPsramBuffer(request, doc, responseSize)) {
        return;
    }
    String json;
    json.reserve(responseSize + 1);
    serializeJson(doc, json);
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
    sendWithClose(request, response);
}

// 发送带 success:true 包装的 JSON 文档
inline void sendJsonDocSuccess(AsyncWebServerRequest* request, JsonDocument& doc) {
    const size_t innerSize = measureJson(doc);
    if (checkResponseMemory(request, innerSize + 32)) return;

    auto outDoc = FastBee::makeJsonDocument(innerSize + 4096);
    outDoc["success"] = true;
    outDoc["data"].set(doc);
    sendJsonStream(request, outDoc);
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

// 列表型 JSON 响应的 Chunked 流式输出（非 inline，避免在多个 handler 中重复展开、膨胀 Flash）
// 输入 items 会被 std::move 接管，调用者不需手动释放。
// 输出格式：{header} {item0} ',' {item1} ... {footer}
// 典型用法：header = `{"success":true,"total":N,"data":[`, footer = `]}`.
// 特点：
//   - 不产生连续大 String 堆块（逐项拷贝到 TCP 发送缓冲，粒度 ~256B）
//   - 项发完后立即释放该 String，堆占用单调递减
//   - 由 shared_ptr 接管状态，在 lambda 释放后自动销毁
bool sendJsonListChunked(
    AsyncWebServerRequest* request,
    const String& header,
    std::vector<String>&& items,
    const char* footer = "]}");

} // namespace HandlerUtils
