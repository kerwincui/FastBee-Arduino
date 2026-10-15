#pragma once
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <LittleFS.h>
#include <vector>
#include <memory>
#include <new>
#include <cstring>
#include "core/FastBeeFramework.h"
#include "systems/HealthMonitor.h"
#include "utils/PsramJsonDocument.h"
#include "network/JsonDocumentResponse.h"

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

// 将运行期内存降级动作以稳定的小对象暴露给 Web/API。该对象只包含
// 固定字段和少量静态字符串，不会修改用户配置，也不会触发额外服务初始化。
inline void addMemoryDegradationStatus(JsonObject parent) {
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    if (!monitor) return;
    JsonObject status = parent["memoryDegradation"].to<JsonObject>();
    monitor->fillDegradationJson(status);

    // Keep the compact fields available to callers that only inspect the
    // normal memory object (for example the request governor).  The detailed
    // memoryDegradation object remains the canonical diagnostic payload.
    // to<JsonObject>() clears an existing object, including the heap/PSRAM
    // metrics already populated by compact system-info and metrics routes.
    JsonObject memory = parent["memory"].as<JsonObject>();
    if (memory.isNull()) memory = parent["memory"].to<JsonObject>();
    memory["guardLevel"] = memoryGuardLevelName(monitor->getMemoryGuardLevel());
    memory["degraded"] = monitor->isMemoryDegraded();
    memory["periphExecPaused"] = monitor->isPeriphExecPausedForMemory();
    memory["periphExecWorkerSuspended"] =
        monitor->arePeriphExecWorkersSuspendedForMemory();
    memory["periphExecWorkerSuspensionPending"] =
        monitor->isPeriphExecWorkerSuspensionPending();
    memory["modbusStopped"] = monitor->isModbusStoppedForMemory();
    memory["mqttRestorePending"] = monitor->isMqttRestorePendingForMemory();
    memory["optionalPeripheralsSuspended"] =
        monitor->areOptionalPeripheralsSuspendedForMemory();
    memory["optionalPeripheralCount"] = monitor->getMemorySuspendedPeripheralCount();
    parent["degraded"] = monitor->isMemoryDegraded();
}

// ArduinoJson does not implicitly convert a root JsonDocument to JsonObject;
// keep the document call sites allocation-neutral while sharing the same
// payload implementation above.
inline void addMemoryDegradationStatus(JsonDocument& parent) {
    addMemoryDegradationStatus(parent.as<JsonObject>());
}

inline bool memoryDegradationActive() {
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    return monitor && monitor->isMemoryDegraded();
}

inline const char* currentMemoryGuardLevelName() {
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    return monitor ? memoryGuardLevelName(monitor->getMemoryGuardLevel()) : "NORMAL";
}

// Chunked list handlers build their response header without a JsonDocument.
// Append the small, stable degradation payload directly so low-memory paths
// expose the same state without allocating a second document.
inline void appendMemoryDegradationStatus(String& out) {
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    if (!monitor) return;

    const bool active = monitor->isMemoryDegraded();
    const bool modbusStopped = monitor->isModbusStoppedForMemory();
    const bool mqttRestorePending = monitor->isMqttRestorePendingForMemory();
    const bool periphExecPaused = monitor->isPeriphExecPausedForMemory();
    const bool periphExecWorkerSuspended =
        monitor->arePeriphExecWorkersSuspendedForMemory();
    const bool periphExecWorkerSuspensionPending =
        monitor->isPeriphExecWorkerSuspensionPending();
    const bool optionalSuspended = monitor->areOptionalPeripheralsSuspendedForMemory();
    const uint16_t optionalCount = monitor->getMemorySuspendedPeripheralCount();
    const char* level = memoryGuardLevelName(monitor->getMemoryGuardLevel());

    out += F(",\"memory\":{\"guardLevel\":\"");
    out += level;
    out += F("\",\"degraded\":");
    out += active ? F("true") : F("false");
    out += F(",\"modbusStopped\":");
    out += modbusStopped ? F("true") : F("false");
    out += F(",\"mqttRestorePending\":");
    out += mqttRestorePending ? F("true") : F("false");
    out += F(",\"periphExecPaused\":");
    out += periphExecPaused ? F("true") : F("false");
    out += F(",\"periphExecWorkerSuspended\":");
    out += periphExecWorkerSuspended ? F("true") : F("false");
    out += F(",\"periphExecWorkerSuspensionPending\":");
    out += periphExecWorkerSuspensionPending ? F("true") : F("false");
    out += F(",\"optionalPeripheralsSuspended\":");
    out += optionalSuspended ? F("true") : F("false");
    out += F(",\"optionalPeripheralCount\":");
    out += String(optionalCount);
    out += F("},\"memoryDegradation\":{\"active\":");
    out += active ? F("true") : F("false");
    out += F(",\"runtimeOnly\":true,\"level\":\"");
    out += level;
    out += F("\",\"modbusStopped\":");
    out += modbusStopped ? F("true") : F("false");
    out += F(",\"mqttRestorePending\":");
    out += mqttRestorePending ? F("true") : F("false");
    out += F(",\"periphExecPaused\":");
    out += periphExecPaused ? F("true") : F("false");
    out += F(",\"periphExecWorkerSuspended\":");
    out += periphExecWorkerSuspended ? F("true") : F("false");
    out += F(",\"periphExecWorkerSuspensionPending\":");
    out += periphExecWorkerSuspensionPending ? F("true") : F("false");
    out += F(",\"optionalPeripheralsSuspended\":");
    out += optionalSuspended ? F("true") : F("false");
    out += F(",\"optionalPeripheralCount\":");
    out += String(optionalCount);
    out += F("}");
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

// Send JSON. Large fallback responses take ownership of doc; callers must not
// reuse it after this call. The response releases it on completion/disconnect.
inline void sendJsonStream(AsyncWebServerRequest* request, JsonDocument& doc) {
    const size_t responseSize = measureJson(doc);
    if (doc.overflowed()) {
        sendJsonError(request, 503, "JSON document allocation failed", 5);
        return;
    }
    if (responseSize >= 2048 || responseSize + 512 > ESP.getMaxAllocHeap()) {
        if (sendJsonFromPsramBuffer(request, doc, responseSize)) return;
        // Retain the existing heap/fragmentation protection; only remove the
        // unnecessary response-sized contiguous allocation requirement.
        if (checkLowMemory(request, 8192)) return;
        auto* response = new (std::nothrow) FastBee::JsonDocumentResponse(std::move(doc));
        if (!response) {
            sendJsonError(request, 503, "JSON response allocation failed", 5);
            return;
        }
        sendWithClose(request, response);
        return;
    }
    if (checkResponseMemory(request, responseSize)) return;
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

// IR/RF 遥控按键库共用：纯数字无符号整数参数解析（IR 的 timeoutMs、RF 的 bits/pulse 等）
inline bool parseUnsignedRangeParam(const String& text, uint32_t low, uint32_t high, uint32_t& out) {
    if (text.isEmpty() || text.length() > 5) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + static_cast<uint32_t>(text[i] - '0');
    }
    if (value < low || value > high) return false;
    out = value; return true;
}

// IR/RF 遥控按键库共用：按键库错误码到 HTTP 状态码的映射。
// prefix 为 "IR"/"RF"；unavailableError 为驱动不可用错误码（无则传 nullptr）。
inline int remoteKeyErrorStatus(const String& error, const char* prefix, const char* unavailableError) {
    const String notFound = String(prefix) + "_KEY_NOT_FOUND";
    const String keysPrefix = String(prefix) + "_KEYS_";
    const String keysLimit = String(prefix) + "_KEYS_LIMIT";
    if (error == notFound) return 404;
    if (error.startsWith(keysPrefix) && error != keysLimit) return 503;
    if (unavailableError && error == unavailableError) return 503;
    return 400;
}

// IR/RF 遥控按键库共用：按键列表 envelope 构造 + 长度自校验。
// 返回 false 表示响应长度与预期不符（疑似分配截断），调用方负责回 503。
inline bool sendRemoteKeyListEnvelope(AsyncWebServerRequest* request, const String& keys, size_t count) {
    String resp = "{\"success\":true,\"data\":{\"count\":" + String((int)count) +
                  ",\"keys\":" + keys + "}}";
    const size_t expected = sizeof("{\"success\":true,\"data\":{\"count\":") - 1 +
                            String((int)count).length() + sizeof(",\"keys\":") - 1 + keys.length() + 2;
    if (resp.length() != expected) return false;
    request->send(200, "application/json", resp);
    return true;
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
