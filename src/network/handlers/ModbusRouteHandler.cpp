#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_MODBUS

#include "./network/handlers/ModbusRouteHandler.h"
#include "core/MemoryBudget.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "./protocols/ProtocolManager.h"
#include "./protocols/ModbusHandler.h"
#include "core/PeriphExecManager.h"
#include "core/MotorControlService.h"
#include "utils/PsramJsonDocument.h"
#include <ArduinoJson.h>
#include <new>
#include <freertos/task.h>

static void addModbusReadDiagnostics(JsonDocument& doc, const ModbusReadDiagnostics& trace) {
    JsonObject out = doc["diagnostic"].to<JsonObject>();
    out["scope"] = "request:lastAttempt";
    out["reason"] = trace.reason;
    out["attempts"] = trace.attempts;
    out["txLength"] = trace.txLength;
    out["txWrittenLastAttempt"] = trace.txWritten;
    out["rxBufferedLastAttempt"] = trace.rxBuffered;
    out["totalRxBuffered"] = trace.totalRxBuffered;
    out["uartReady"] = trace.uartReady;
    out["uartBaudRate"] = trace.uartBaudRate;
    out["rxOverflow"] = trace.rxOverflow;
    out["crcValid"] = trace.crcValid;
    out["addressMatched"] = trace.addressMatched;
    out["txPrefixTruncated"] = trace.txLength > trace.txPrefixLength;
    out["rxPrefixTruncated"] = trace.rxOverflow || trace.rxBuffered > trace.rxPrefixLength;
    char hex[sizeof(trace.rxPrefix) * 3]{};
    size_t used = 0;
    for (uint8_t i = 0; i < trace.txPrefixLength; ++i)
        used += snprintf(hex + used, sizeof(hex) - used, i ? " %02X" : "%02X", trace.txPrefix[i]);
    out["txPrefix"] = hex;
    used = 0;
    hex[0] = '\0';
    for (uint8_t i = 0; i < trace.rxPrefixLength; ++i)
        used += snprintf(hex + used, sizeof(hex) - used, i ? " %02X" : "%02X", trace.rxPrefix[i]);
    out["rxPrefix"] = hex;
}

// ============================================================================
// Modbus 閫氱敤鎺у埗 API 杈呭姪鍑芥暟
// ============================================================================

// 灏?OneShotError 杞崲涓?HTTP 閿欒鍝嶅簲
static void sendOneShotError(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                              const OneShotResult& result,
                              const ModbusReadDiagnostics* diagnostics = nullptr) {
    JsonDocument doc;
    doc["success"] = false;
    if (diagnostics) addModbusReadDiagnostics(doc, *diagnostics);
    
    const char* errorCode = "UNKNOWN";
    const char* errorMsg = "Unknown error";
    int httpCode = 500;
    
    switch (result.error) {
        case ONESHOT_TIMEOUT:
            errorCode = "TIMEOUT";
            errorMsg = "Slave not responding (timeout)";
            httpCode = 504;
            break;
        case ONESHOT_CRC_ERROR:
            errorCode = "CRC_ERROR";
            errorMsg = "CRC validation failed";
            httpCode = 502;
            break;
        case ONESHOT_EXCEPTION: {
            errorCode = "MODBUS_EXCEPTION";
            httpCode = 422;
            char msgBuf[64];
            switch (result.exceptionCode) {
                case 0x01: snprintf(msgBuf, sizeof(msgBuf), "Illegal function (0x01)"); break;
                case 0x02: snprintf(msgBuf, sizeof(msgBuf), "Illegal data address (0x02)"); break;
                case 0x03: snprintf(msgBuf, sizeof(msgBuf), "Illegal data value (0x03)"); break;
                case 0x04: snprintf(msgBuf, sizeof(msgBuf), "Slave device failure (0x04)"); break;
                default:   snprintf(msgBuf, sizeof(msgBuf), "Exception code 0x%02X", result.exceptionCode); break;
            }
            doc["error"] = msgBuf;
            doc["errorCode"] = errorCode;
            doc["exceptionCode"] = result.exceptionCode;
            String out;
            serializeJson(doc, out);
            request->send(httpCode, "application/json", out);
            return;
        }
        case ONESHOT_NOT_INITIALIZED:
            errorCode = "NOT_INITIALIZED";
            errorMsg = "Modbus not initialized";
            httpCode = 503;
            break;
        case ONESHOT_BUSY:
            errorCode = "BUSY";
            errorMsg = "Modbus busy, try again";
            httpCode = 503;
            break;
        default:
            break;
    }
    
    doc["error"] = errorMsg;
    doc["errorCode"] = errorCode;
    String out;
    serializeJson(doc, out);
    request->send(httpCode, "application/json", out);
}

// 鑾峰彇 ModbusHandler 鎸囬拡骞舵牎楠?Master 妯″紡锛屽け璐ユ椂鍙戦€侀敊璇搷搴?
struct MotorSoftLimitDecision {
    bool enabled = false;
    bool allowed = true;
    uint16_t pulse = 0;
    int32_t nextPosition = 0;
    const char* reason = "";
};

static MotorSoftLimitDecision evaluateMotorSoftLimit(const ModbusSubDevice& dev,
                                                     const String& action,
                                                     int requestedValue) {
    MotorSoftLimitDecision d;
    d.nextPosition = dev.motorCurrentPosition;
    if (!dev.hasMotorSoftLimit() || (action != "forward" && action != "reverse")) {
        return d;
    }

    d.enabled = true;
    int32_t desiredPulse = requestedValue > 0 ? requestedValue : 0;
    if (desiredPulse <= 0 && dev.motorLastPulse > 0) desiredPulse = dev.motorLastPulse;
    if (desiredPulse <= 0 && dev.motorMoveStep > 0) desiredPulse = dev.motorMoveStep;
    if (desiredPulse <= 0) {
        d.allowed = false;
        d.reason = "motor_soft_limit_requires_pulse";
        return d;
    }

    int32_t room = (action == "forward")
        ? (dev.motorMaxPosition - dev.motorCurrentPosition)
        : (dev.motorCurrentPosition - dev.motorMinPosition);
    if (room <= 0) {
        d.allowed = false;
        d.reason = (action == "forward") ? "motor_at_right_limit" : "motor_at_left_limit";
        return d;
    }

    if (desiredPulse > room) desiredPulse = room;
    if (desiredPulse > 65535) desiredPulse = 65535;
    d.pulse = static_cast<uint16_t>(desiredPulse);
    d.nextPosition = (action == "forward")
        ? dev.motorCurrentPosition + desiredPulse
        : dev.motorCurrentPosition - desiredPulse;
    return d;
}

static ProtocolManager::ModbusHandlerLease getModbusMaster(
    WebHandlerContext* ctx, AsyncWebServerRequest* request) {
    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return {};
    }
    auto lease = pm->acquireModbusHandler(pdMS_TO_TICKS(2000));
    ModbusHandler* modbus = lease.get();
    if (!modbus) {
        ctx->sendError(request, 503, "Modbus not started or restarting");
        return {};
    }
    if (modbus->getMode() != MODBUS_MASTER) {
        ctx->sendError(request, 400, "Modbus is not in Master mode");
        return {};
    }
    return lease;
}

static bool rejectModbusHeavyRequest(AsyncWebServerRequest* request,
                                     const char* requestLabel,
                                     size_t minHeapThreshold = 8192,
                                     int retryAfterSeconds = 6) {
    if (HandlerUtils::rejectHeavyRequestOnPressure(
            request, requestLabel, MemoryGuardLevel::SEVERE, retryAfterSeconds)) {
        return true;
    }
    return HandlerUtils::checkLowMemory(request, minHeapThreshold);
}

static bool rejectModbusCriticalControl(AsyncWebServerRequest* request,
                                        const char* requestLabel,
                                        size_t minHeapThreshold = 6144,
                                        int retryAfterSeconds = 4) {
    if (HandlerUtils::rejectHeavyRequestOnPressure(
            request, requestLabel, MemoryGuardLevel::CRITICAL, retryAfterSeconds)) {
        return true;
    }
    return HandlerUtils::checkLowMemory(request, minHeapThreshold);
}

// 杈呭姪锛氬皢 Modbus TX/RX 璋冭瘯甯ф坊鍔犲埌 JSON 鍝嶅簲
static void addModbusDebug(JsonDocument& doc, ModbusHandler* modbus) {
    String tx, rx;
    if (!modbus->getDebugSnapshot(tx, rx)) return;
    JsonObject debug = doc["debug"].to<JsonObject>();
    debug["tx"] = tx;
    debug["rx"] = rx;
}

// 杈呭姪锛氶€氳繃 slaveAddress 鏌ユ壘瀛愯澶囩储寮曪紙闆舵嫹璐濓紝閬垮厤 3.4KB ModbusConfig 鏍堝垎閰嶏級
static int findDeviceIndexBySlaveAddr(ModbusHandler* modbus, uint8_t slaveAddr) {
    for (uint8_t i = 0; i < modbus->getSubDeviceCount(); i++) {
        if (modbus->getSubDevice(i).slaveAddress == slaveAddr) return (int)i;
    }
    return -1;
}

// 杈呭姪锛氭瀯寤哄苟鍙戝竷鍗曟潯 MQTT 涓婃姤
static void publishControlReport(WebHandlerContext* ctx, ModbusHandler* modbus,
                                  int deviceIndex, uint16_t channel, const String& value) {
    if (deviceIndex < 0) return;
    String sid = modbus->buildSensorId((uint8_t)deviceIndex, channel);
    if (sid.isEmpty()) return;
    MQTTClient* mqtt = ctx->protocolManager ? ctx->protocolManager->getMQTTClient() : nullptr;
    if (!mqtt || !mqtt->getIsConnected()) return;
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    JsonObject obj = arr.add<JsonObject>();
    obj["id"] = sid;
    obj["value"] = value;
    obj["remark"] = "";
    String payload;
    serializeJson(doc, payload);
    LOG_WARNINGF("[MqttReport] publishControlReport: id='%s' value='%s'", sid.c_str(), value.c_str());
    mqtt->publishReportData(payload);
}

// 杈呭姪锛氭瀯寤哄苟鍙戝竷鎵归噺 MQTT 涓婃姤锛堝閫氶亾锛?
static void publishBatchControlReport(WebHandlerContext* ctx, ModbusHandler* modbus,
                                       int deviceIndex, uint16_t channelCount, const bool* states) {
    if (deviceIndex < 0) return;
    MQTTClient* mqtt = ctx->protocolManager ? ctx->protocolManager->getMQTTClient() : nullptr;
    if (!mqtt || !mqtt->getIsConnected()) return;
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    bool hasEntry = false;
    for (uint16_t i = 0; i < channelCount; i++) {
        String sid = modbus->buildSensorId((uint8_t)deviceIndex, i);
        if (sid.isEmpty()) continue;
        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = sid;
        obj["value"] = String(states[i] ? 1 : 0);
        obj["remark"] = "";
        hasEntry = true;
    }
    if (!hasEntry) return;
    String payload;
    serializeJson(doc, payload);
    mqtt->publishReportData(payload);
}

static uint8_t countEnabledPollTasks(ModbusHandler* modbus) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < modbus->getPollTaskCount(); i++) {
        if (modbus->getPollTask(i).enabled) count++;
    }
    return count;
}

static uint16_t getMinEnabledPollInterval(ModbusHandler* modbus) {
    uint16_t minInterval = 0;
    for (uint8_t i = 0; i < modbus->getPollTaskCount(); i++) {
        PollTask task = modbus->getPollTask(i);
        if (!task.enabled) continue;
        if (minInterval == 0 || task.pollInterval < minInterval) {
            minInterval = task.pollInterval;
        }
    }
    return minInterval;
}

static uint16_t getMaxEnabledPollInterval(ModbusHandler* modbus) {
    uint16_t maxInterval = 0;
    for (uint8_t i = 0; i < modbus->getPollTaskCount(); i++) {
        PollTask task = modbus->getPollTask(i);
        if (!task.enabled) continue;
        if (task.pollInterval > maxInterval) {
            maxInterval = task.pollInterval;
        }
    }
    return maxInterval;
}

static uint32_t getTaskCacheAgeSec(const ModbusHandler::PollTaskCache* cache) {
    if (!cache || cache->timestamp == 0) return 0;
    return (millis() - cache->timestamp) / 1000UL;
}

static uint32_t getTaskStaleThresholdSec(const PollTask& task) {
    uint32_t threshold = static_cast<uint32_t>(task.pollInterval) * 3UL;
    return threshold < 30UL ? 30UL : threshold;
}

static float calcRate(uint32_t numerator, uint32_t denominator) {
    if (denominator == 0) return 0.0f;
    return (static_cast<float>(numerator) * 100.0f) / static_cast<float>(denominator);
}

static const char* getTaskHealthStatus(const PollTask& task, const ModbusHandler::PollTaskCache* cache) {
    if (!task.enabled) return "disabled";
    if (!cache || cache->timestamp == 0) return "pending";

    uint32_t ageSec = getTaskCacheAgeSec(cache);
    if (!cache->valid) {
        return cache->lastError != 0 ? "error" : "pending";
    }
    return ageSec > getTaskStaleThresholdSec(task) ? "stale" : "ok";
}

static const char* buildModbusRiskLevel(ModbusHandler* modbus,
                                        uint8_t enabledTaskCount,
                                        uint16_t minPollInterval,
                                        uint32_t totalPolls,
                                        float timeoutRate,
                                        float failureRate,
                                        uint32_t lastPollAgeSec) {
    if (enabledTaskCount >= 4 && minPollInterval > 0 && minPollInterval < 5) return "high";
    if (timeoutRate >= 20.0f && totalPolls >= 10) return "high";
    if (modbus->getResponseTimeout() > 3000 && modbus->getMaxRetries() > 2 && enabledTaskCount >= 3) return "high";

    if (enabledTaskCount >= 4) return "medium";
    if (minPollInterval > 0 && minPollInterval < 5) return "medium";
    if (modbus->getInterPollDelay() < 100) return "medium";
    if (failureRate >= 10.0f && totalPolls >= 10) return "medium";
    if (enabledTaskCount > 0 && lastPollAgeSec > 0) {
        uint32_t staleThreshold = static_cast<uint32_t>(minPollInterval > 0 ? minPollInterval : 5) * 3UL;
        if (staleThreshold < 30UL) staleThreshold = 30UL;
        if (lastPollAgeSec > staleThreshold) return "medium";
    }
    return "low";
}

// ============================================================================
// 鏋勯€犲嚱鏁?
// ============================================================================

ModbusRouteHandler::ModbusRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

namespace {
bool validRelayChannel(int slave, int channel, int base) {
    return slave >= 1 && slave <= 247 && channel >= 0 &&
           channel < Protocols::MODBUS_MAX_WRITE_COILS &&
           base >= 0 && base <= 65535 && channel <= 65535 - base;
}
// Match the S3 execution-worker floor for the same transaction/logger chain.
// This is a transient internal-DRAM stack; validate RELAY_METRICS on hardware.
constexpr uint32_t RELAY_TASK_STACK_BYTES = 7168U;
constexpr uint32_t RELAY_TASK_ALLOCATOR_OVERHEAD_BYTES = 256U;
constexpr uint32_t RELAY_TASK_DRAM_RESERVE_BYTES = 12288U;
SemaphoreHandle_t relayRequestSlot() {
    static SemaphoreHandle_t slot = xSemaphoreCreateCounting(1, 1);
    return slot;
}
}

struct ModbusRouteHandler::RelayJob {
    ModbusRouteHandler* owner;
    RelayHandler handler;
    AsyncWebServerRequestPtr request;
    SemaphoreHandle_t slot;
};

void ModbusRouteHandler::relayTask(void* argument) {
    auto* job = static_cast<RelayJob*>(argument);
    // Create before pause, then hand off ownership with a notification, as in
    // AuthRouteHandler. Failed creation must never leave a paused request.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    {
        auto request = job->request.lock();
        if (request && request->client() && request->client()->connected()) {
            // Hold a strong reference throughout the handler; disconnect cannot
            // free its parameters while a transaction is in progress.
            (job->owner->*(job->handler))(request.get());
        }
    }
    xSemaphoreGive(job->slot);
    delete job;
    Serial.printf("[RELAY_METRICS] stack=%lu free=%lu bytes\n",
                  static_cast<unsigned long>(RELAY_TASK_STACK_BYTES),
                  static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
    vTaskDelete(nullptr);
}

void ModbusRouteHandler::dispatchRelayRequest(AsyncWebServerRequest* request,
                                             RelayHandler handler) {
    if (!ctx->requireAuth(request)) return;
    const auto slot = relayRequestSlot();
    if (!slot || xSemaphoreTake(slot, 0) != pdTRUE) {
        HandlerUtils::sendJsonError(request, 503, "Relay request already running; refresh state before retrying", 1);
        return;
    }
    const uint32_t requiredBlock = RELAY_TASK_STACK_BYTES + RELAY_TASK_ALLOCATOR_OVERHEAD_BYTES;
    if (FastBee::MemoryBudget::dram8Largest() < requiredBlock ||
        FastBee::MemoryBudget::dram8Free() < requiredBlock + RELAY_TASK_DRAM_RESERVE_BYTES) {
        xSemaphoreGive(slot);
        HandlerUtils::sendJsonError(request, 503, "Insufficient memory for relay request", 3);
        return;
    }
    auto* job = new (std::nothrow) RelayJob{this, handler, {}, slot};
    if (!job) {
        xSemaphoreGive(slot);
        HandlerUtils::sendJsonError(request, 503, "Unable to allocate relay request", 3);
        return;
    }
    TaskHandle_t task = nullptr;
    const BaseType_t created = xTaskCreate(relayTask, "fb_relay", RELAY_TASK_STACK_BYTES,
                                          job, 1, &task);
    if (created != pdPASS) {
        delete job;
        xSemaphoreGive(slot);
        HandlerUtils::sendJsonError(request, 503, "Unable to start relay request", 3);
        return;
    }
    job->request = request->pause();
    xTaskNotifyGive(task);
}

// ============================================================================
// 璺敱娉ㄥ唽
// ============================================================================

void ModbusRouteHandler::setupRoutes(AsyncWebServer* server) {
    // Modbus Master API
    server->on("/api/modbus/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleGetModbusStatus(request);
    });

    server->on("/api/modbus/write", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusWrite(request);
    });

    // Modbus 閫氱敤鎺у埗 API锛堢嚎鍦堛€佸瘎瀛樺櫒璇诲啓锛岃澶囧弬鏁帮級
    server->on("/api/modbus/coil/control", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        dispatchRelayRequest(request, &ModbusRouteHandler::handleModbusCoilControl);
    });

    server->on("/api/modbus/coil/batch", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        dispatchRelayRequest(request, &ModbusRouteHandler::handleModbusCoilBatch);
    });

    server->on("/api/modbus/coil/delay", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        dispatchRelayRequest(request, &ModbusRouteHandler::handleModbusCoilDelay);
    });

    server->on("/api/modbus/coil/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        dispatchRelayRequest(request, &ModbusRouteHandler::handleModbusCoilStatus);
    });

    server->on("/api/modbus/device/address", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusDeviceAddress(request);
    });

    server->on("/api/modbus/device/baudrate", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusDeviceBaudrate(request);
    });

    server->on("/api/modbus/device/inputs", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleModbusDiscreteInputs(request);
    });

    // Modbus 閫氱敤瀵勫瓨鍣ㄨ鍐?API
    server->on("/api/modbus/register/read", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleModbusRegisterRead(request);
    });

    server->on("/api/modbus/register/write", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusRegisterWrite(request);
    });

    server->on("/api/modbus/register/batch-write", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusRegisterBatchWrite(request);
    });

    // Modbus 鐢垫満鎺у埗 API
    server->on("/api/modbus/motor/control", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusMotorControl(request);
    });
    server->on("/api/modbus/motor/state", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleModbusMotorControl(request);
    });
}

// ============================================================================
// GET /api/modbus/status 鈥?鑾峰彇 Modbus 鐘舵€?
// ============================================================================

void ModbusRouteHandler::handleGetModbusStatus(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    bool explicitFull = request->hasParam("full") && request->getParam("full")->value() == "1";
    bool compactStatus = !explicitFull ||
                         ctx->getParamBool(request, "compact", false) ||
                         FastBee::MemoryBudget::dramFree() < 12288 ||
                         FastBee::MemoryBudget::dramLargest() < 6144;

    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return;
    }

    auto modbusLease = pm->acquireModbusHandler(pdMS_TO_TICKS(2000));
    ModbusHandler* modbus = modbusLease.get();

    // 鍐呭瓨妫€鏌ワ細Modbus 鐘舵€佸搷搴斿彲鑳藉寘鍚ぇ閲忎换鍔℃暟鎹?
    if (rejectModbusHeavyRequest(request, "Modbus status", compactStatus ? 4096 : 8192)) return;

    auto doc = FastBee::makeJsonDocument(32768);
    doc["success"] = true;

    JsonObject data = doc["data"].to<JsonObject>();
    data["compact"] = compactStatus;
    JsonObject health = data["health"].to<JsonObject>();
    JsonArray warnings = health["warnings"].to<JsonArray>();

    if (!modbus) {
        // Modbus 鏈垵濮嬪寲鏃惰繑鍥炵┖鐘舵€佽€岄潪 404
        data["mode"] = "master";
        data["workMode"] = 1;
        data["status"] = "stopped";
        data["taskCount"] = 0;
        // 鍒濆鍖栫粺璁″瓧娈典负0
        data["totalPolls"] = 0;
        data["successPolls"] = 0;
        data["failedPolls"] = 0;
        data["timeoutPolls"] = 0;
        data["tasks"].to<JsonArray>();
        health["riskLevel"] = "low";
        health["enabledTaskCount"] = 0;
        health["minPollInterval"] = 0;
        health["maxPollInterval"] = 0;
        health["responseTimeout"] = 0;
        health["maxRetries"] = 0;
        health["interPollDelay"] = 0;
        health["lastPollAgeSec"] = 0;
        health["successRate"] = 0.0;
        health["failureRate"] = 0.0;
        health["timeoutRate"] = 0.0;
    } else {
        data["mode"] = (modbus->getMode() == MODBUS_MASTER) ? "master" : "slave";
        data["workMode"] = modbus->getWorkMode();
        data["status"] = modbus->getStatus();

        if (modbus->getMode() == MODBUS_MASTER) {
            uint8_t enabledTaskCount = countEnabledPollTasks(modbus);
            uint16_t minPollInterval = getMinEnabledPollInterval(modbus);
            uint16_t maxPollInterval = getMaxEnabledPollInterval(modbus);
            uint32_t totalPolls = modbus->getTotalPollCount();
            uint32_t successPolls = modbus->getSuccessPollCount();
            uint32_t failedPolls = modbus->getFailedPollCount();
            uint32_t timeoutPolls = modbus->getTimeoutPollCount();
            uint32_t lastPollAgeSec = modbus->getLastPollAgeSec();
            float successRate = calcRate(successPolls, totalPolls);
            float failureRate = calcRate(failedPolls + timeoutPolls, totalPolls);
            float timeoutRate = calcRate(timeoutPolls, totalPolls);

            data["taskCount"] = modbus->getPollTaskCount();

            // 娣诲姞杞缁熻鏁版嵁
            data["totalPolls"] = totalPolls;
            data["successPolls"] = successPolls;
            data["failedPolls"] = failedPolls;
            data["timeoutPolls"] = timeoutPolls;

            health["riskLevel"] = buildModbusRiskLevel(modbus, enabledTaskCount, minPollInterval,
                                                       totalPolls, timeoutRate, failureRate, lastPollAgeSec);
            health["enabledTaskCount"] = enabledTaskCount;
            health["minPollInterval"] = minPollInterval;
            health["maxPollInterval"] = maxPollInterval;
            health["responseTimeout"] = modbus->getResponseTimeout();
            health["maxRetries"] = modbus->getMaxRetries();
            health["interPollDelay"] = modbus->getInterPollDelay();
            health["lastPollAgeSec"] = lastPollAgeSec;
            health["successRate"] = successRate;
            health["failureRate"] = failureRate;
            health["timeoutRate"] = timeoutRate;

            if (enabledTaskCount >= 4) {
                warnings.add("Many polling tasks are enabled; watch bus load and Web response time.");
            }
            if (minPollInterval > 0 && minPollInterval < 5) {
                warnings.add("A polling interval is below 5 seconds; this can amplify bus jitter and UI stalls.");
            }
            if (modbus->getResponseTimeout() > 3000) {
                warnings.add("Modbus response timeout is high; offline slaves can hold the serial bus longer.");
            }
            if (modbus->getMaxRetries() > 2) {
                warnings.add("Modbus retry count is high; failed devices can extend a poll round.");
            }
            if (modbus->getInterPollDelay() < 100) {
                warnings.add("Inter-poll delay is very small; continuous polling can compress Web response time.");
            }
            if (totalPolls >= 10 && timeoutRate >= 20.0f) {
                warnings.add("Recent timeout rate is high; check slave stability, baud rate, and polling frequency.");
            } else if (totalPolls >= 10 && failureRate >= 10.0f) {
                warnings.add("Recent poll failure rate is high; check register mapping and device online status.");
            }
            if (enabledTaskCount > 0 && lastPollAgeSec > 0) {
                uint32_t staleThreshold = static_cast<uint32_t>(minPollInterval > 0 ? minPollInterval : 5) * 3UL;
                if (staleThreshold < 30UL) staleThreshold = 30UL;
                if (lastPollAgeSec > staleThreshold) {
                    warnings.add("Polling results have not refreshed for a while; the bus or task may be stalled.");
                }
            }
            JsonArray tasks = data["tasks"].to<JsonArray>();
            uint8_t taskLimit = compactStatus ? 0 : modbus->getPollTaskCount();
            for (uint8_t i = 0; i < taskLimit; i++) {
                PollTask task = modbus->getPollTask(i);
                const ModbusHandler::PollTaskCache* cache = modbus->getTaskCache(i);
                uint32_t cacheAgeSec = getTaskCacheAgeSec(cache);
                JsonObject t = tasks.add<JsonObject>();
                t["slaveAddress"] = task.slaveAddress;
                t["functionCode"] = task.functionCode;
                t["startAddress"] = task.startAddress;
                t["quantity"] = task.quantity;
                t["pollInterval"] = task.pollInterval;
                t["enabled"] = task.enabled;
                t["name"] = String(task.name);
                t["status"] = getTaskHealthStatus(task, cache);
                t["cacheAgeSec"] = cacheAgeSec;
                t["staleAfterSec"] = getTaskStaleThresholdSec(task);
                t["lastError"] = cache ? cache->lastError : 0;

                // 瀵勫瓨鍣ㄦ槧灏勯厤缃?- 鎸?regOffset 鎺掑簭鍚庤緭鍑猴紝纭繚鏄剧ず椤哄簭姝ｇ‘
                if (task.mappingCount > 0) {
                    // 鍒涘缓绱㈠紩鏁扮粍鐢ㄤ簬鎺掑簭
                    uint8_t sortedIdx[Protocols::MODBUS_MAX_MAPPINGS_PER_TASK];
                    for (uint8_t j = 0; j < task.mappingCount; j++) {
                        sortedIdx[j] = j;
                    }
                    // 绠€鍗曞啋娉℃帓搴忔寜 regOffset 鍗囧簭鎺掑垪
                    for (uint8_t j = 0; j < task.mappingCount - 1; j++) {
                        for (uint8_t k = 0; k < task.mappingCount - j - 1; k++) {
                            if (task.mappings[sortedIdx[k]].regOffset > task.mappings[sortedIdx[k + 1]].regOffset) {
                                uint8_t tmp = sortedIdx[k];
                                sortedIdx[k] = sortedIdx[k + 1];
                                sortedIdx[k + 1] = tmp;
                            }
                        }
                    }
                    JsonArray mappings = t["mappings"].to<JsonArray>();
                    for (uint8_t j = 0; j < task.mappingCount; j++) {
                        const RegisterMapping& m = task.mappings[sortedIdx[j]];
                        if (m.sensorId[0] == '\0') continue;
                        JsonObject mo = mappings.add<JsonObject>();
                        mo["regOffset"] = m.regOffset;
                        mo["dataType"] = m.dataType;
                        mo["scaleFactor"] = m.scaleFactor;
                        mo["decimalPlaces"] = m.decimalPlaces;
                        mo["sensorId"] = String(m.sensorId);
                        if (m.unit[0] != '\0') mo["unit"] = String(m.unit);
                    }
                }

                // 娣诲姞缂撳瓨鏁版嵁锛堟渶鏂伴噰闆嗙殑鍊硷級
                if (cache && cache->valid) {
                    JsonObject cachedData = t["cachedData"].to<JsonObject>();
                    cachedData["timestamp"] = cache->timestamp;
                    cachedData["ageSec"] = cacheAgeSec;
                    cachedData["lastError"] = cache->lastError;
                    JsonArray values = cachedData["values"].to<JsonArray>();
                    // 闃插尽鎬ф鏌ワ細纭繚 count 涓嶈秴杩囨暟缁勮竟鐣?
                    uint8_t safeCount = cache->count;
                    if (safeCount > Protocols::MODBUS_ONESHOT_BUFFER_SIZE) {
                        safeCount = Protocols::MODBUS_ONESHOT_BUFFER_SIZE;
                    }
                    for (uint8_t j = 0; j < safeCount; j++) {
                        values.add(cache->values[j]);
                    }
                }
            }
        } else {
            health["riskLevel"] = "low";
            health["enabledTaskCount"] = 0;
            health["minPollInterval"] = 0;
            health["maxPollInterval"] = 0;
            health["responseTimeout"] = 0;
            health["maxRetries"] = 0;
            health["interPollDelay"] = 0;
            health["lastPollAgeSec"] = 0;
            health["successRate"] = 0.0;
            health["failureRate"] = 0.0;
            health["timeoutRate"] = 0.0;
        }
    }

    if (request->hasParam("hardware") && request->getParam("hardware")->value() == "1") {
        JsonObject hardware = data["uartHardware"].to<JsonObject>();
        if (modbus) modbus->appendUartHardwareSnapshot(hardware);
        else { hardware["supported"] = false; hardware["error"] = "MODBUS_NOT_RUNNING"; }
    }

    health["warningCount"] = warnings.size();

    _statusCache.valid = false;
    HandlerUtils::sendJsonStream(request, doc);
}

// ============================================================================
// POST /api/modbus/write 鈥?鍐欏崟涓繚鎸佸瘎瀛樺櫒
// ============================================================================

void ModbusRouteHandler::handleModbusWrite(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return;
    }

    auto modbusLease = pm->acquireModbusHandler(pdMS_TO_TICKS(2000));
    ModbusHandler* modbus = modbusLease.get();
    if (!modbus) {
        ctx->sendError(request, 503, "Modbus not started");
        return;
    }

    if (modbus->getMode() != MODBUS_MASTER) {
        ctx->sendError(request, 400, "Modbus is not in Master mode");
        return;
    }

    if (rejectModbusCriticalControl(request, "Modbus queued write", 6144)) return;

    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t regAddr = (uint16_t)ctx->getParamInt(request, "regAddress", 0);
    uint16_t value = (uint16_t)ctx->getParamInt(request, "value", 0);

    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }

    if (modbus->masterWriteSingleRegister(slaveAddr, regAddr, value)) {
        ctx->sendSuccess(request, "Write request queued");
    } else {
        ctx->sendError(request, 503, "Write queue full");
    }
}

// ============================================================================
// POST /api/modbus/coil/control 鈥?鍗曚釜绾垮湀鎺у埗锛坥n/off/toggle锛?
// mode=coil: FC05 鍐欑嚎鍦? mode=register: FC06 鍐欎繚鎸佸瘎瀛樺櫒锛?x0001/0x0000锛?
// ============================================================================

void ModbusRouteHandler::handleModbusCoilControl(AsyncWebServerRequest* request) {
    static uint32_t _coilCtrlSeq = 0;
    uint32_t reqSeq = ++_coilCtrlSeq;
    
    if (!ctx->requireAuth(request)) return;
    
    auto modbusLease = getModbusMaster(ctx, request);
    ModbusHandler* modbus = modbusLease.get();
    if (!modbus) return;
    
    const int slaveInput = ctx->getParamInt(request, "slaveAddress", 1);
    uint8_t slaveAddr = static_cast<uint8_t>(slaveInput);
    const int channelInput = ctx->getParamInt(request, "channel", 0);
    uint16_t channel = static_cast<uint16_t>(channelInput);
    const int baseInput = ctx->getParamInt(request, "coilBase", 0);
    uint16_t coilBase = static_cast<uint16_t>(baseInput);
    String action = ctx->getParamValue(request, "action", "toggle");
    String mode = ctx->getParamValue(request, "mode", "coil");

    LOG_WARNINGF("[CoilCtrl#%u] HTTP entry: slave=%d ch=%d action=%s mode=%s clientIP=%s",
                 reqSeq, slaveAddr, channel, action.c_str(), mode.c_str(),
                 request->client() ? request->client()->remoteIP().toString().c_str() : "?");

    if (rejectModbusCriticalControl(request, "Modbus relay control", 6144)) return;

    if (!validRelayChannel(slaveInput, channelInput, baseInput)) {
        ctx->sendBadRequest(request, "Invalid relay address, channel or base");
        return;
    }
    
    if (mode != "coil" && mode != "register") {
        ctx->sendBadRequest(request, "Invalid mode (coil/register)");
        return;
    }
    const int configuredIndex = findDeviceIndexBySlaveAddr(modbus, slaveAddr);
    if (configuredIndex >= 0 && channel >= modbus->getSubDevice(configuredIndex).channelCount) {
        ctx->sendBadRequest(request, "Channel exceeds configured device");
        return;
    }
    uint16_t coilAddr = coilBase + channel;
    OneShotResult result;
    bool newState = false;  // 鐢ㄤ簬璁板綍鎿嶄綔鍚庣殑鐘舵€?
    
    if (mode == "register") {
        // 瀵勫瓨鍣ㄦā寮忥細FC06 鍐欎繚鎸佸瘎瀛樺櫒
        // 鍐欏叆鎴愬姛鍚庣姸鎬佺洿鎺ョ‘瀹氾紝鏃犻渶鍐嶈鍙栫‘璁?
        if (action == "on") {
            result = modbus->writeRegisterOnce(slaveAddr, coilAddr, 1, true);  // isControl=true
            newState = true;
        } else if (action == "off") {
            result = modbus->writeRegisterOnce(slaveAddr, coilAddr, 0, true);  // isControl=true
            newState = false;
        } else if (action == "toggle") {
            // 鍏堣褰撳墠鍊煎啀鍙嶈浆
            OneShotResult readR = modbus->readRegistersOnce(slaveAddr, 0x03, coilAddr, 1, true);  // isControl=true
            if (readR.error != ONESHOT_SUCCESS || readR.count < 1) {
                if (readR.error == ONESHOT_SUCCESS) readR.error = ONESHOT_CRC_ERROR;
                sendOneShotError(ctx, request, readR);
                return;
            }
            uint16_t newVal = (readR.data[0] != 0) ? 0 : 1;
            result = modbus->writeRegisterOnce(slaveAddr, coilAddr, newVal, true);  // isControl=true
            newState = (newVal != 0);  // 鍐欏叆鍊煎嵆涓烘柊鐘舵€?
        } else {
            ctx->sendBadRequest(request, "Invalid action (on/off/toggle)");
            return;
        }
    } else {
        // 绾垮湀妯″紡锛欶C05 鍐欑嚎鍦?
        // 鍐欏叆鎴愬姛鍚庣洿鎺ユ帹瀵肩姸鎬侊紝鏃犻渶棰濆璇诲彇纭
        if (action == "on") {
            result = modbus->writeCoilOnce(slaveAddr, coilAddr, true, true);  // isControl=true
            if (result.error == ONESHOT_SUCCESS) newState = true;
        } else if (action == "off") {
            result = modbus->writeCoilOnce(slaveAddr, coilAddr, false, true);  // isControl=true
            if (result.error == ONESHOT_SUCCESS) newState = false;
        } else if (action == "toggle") {
            // toggle 鏃犳硶浠庡啓鍏ュ€兼帹瀵肩粨鏋滐紝闇€瑕佸厛璇诲彇褰撳墠鐘舵€?
            OneShotResult readR = modbus->readRegistersOnce(slaveAddr, 0x01, coilAddr, 1, true);  // isControl=true
            if (readR.error != ONESHOT_SUCCESS || readR.count < 1) {
                if (readR.error == ONESHOT_SUCCESS) readR.error = ONESHOT_CRC_ERROR;
                sendOneShotError(ctx, request, readR);
                return;
            }
            bool currentState = (readR.data[0] != 0);
            result = modbus->writeCoilOnce(slaveAddr, coilAddr, !currentState, true);  // isControl=true
            if (result.error == ONESHOT_SUCCESS) newState = !currentState;
        } else {
            ctx->sendBadRequest(request, "Invalid action (on/off/toggle)");
            return;
        }
    }
    
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
        return;
    }
    
    // MQTT 涓婃姤鎺у埗缁撴灉锛坣cMode 璁惧鎶ュ憡鐢ㄦ埛鎰忓浘鍊硷紝闈炲師濮嬬嚎鍦堢數骞筹級
    // 鍚﹀垯骞冲彴鍥炲啓璇ュ€兼椂 processDataCommandMatch 浼氬啀娆″簲鐢?ncMode 鍙嶈浆锛?
    // 瀵艰嚧鍙岄噸鍙嶈浆浣胯澶囨仮澶嶅師鐘?
    int devIdx = findDeviceIndexBySlaveAddr(modbus, slaveAddr);
    if (devIdx >= 0) {
        const ModbusSubDevice& dev = modbus->getSubDevice((uint8_t)devIdx);
        bool reportState = newState;
        if (dev.ncMode) {
            reportState = !newState;
        }
        publishControlReport(ctx, modbus, devIdx, channel, String(reportState ? 1 : 0));
    }
    
    // 瑙﹀彂 mc: 浜嬩欢锛堜緵浜嬩欢瑙﹀彂瑙勫垯鍖归厤锛?
    if (devIdx >= 0) {
        char mcIdBuf[16];
        snprintf(mcIdBuf, sizeof(mcIdBuf), "mc:%d", devIdx);
        const char* evAct = newState ? "on" : "off";
        char mcDataBuf[96];
        snprintf(mcDataBuf, sizeof(mcDataBuf), "{\"d\":%d,\"c\":%d,\"a\":\"%s\"}",
            devIdx, channel, evAct);
        PeriphExecManager::getInstance().triggerEventById(String(mcIdBuf), String(mcDataBuf));
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["channel"] = channel;
    data["coilAddress"] = coilAddr;
    data["state"] = newState;
    data["action"] = action;
    data["mode"] = mode;
    addModbusDebug(doc, modbus);
    
    HandlerUtils::sendJsonStream(request, doc);
}

// ============================================================================
// POST /api/modbus/coil/batch 鈥?鎵归噺绾垮湀鎺у埗锛坅llOn/allOff/allToggle锛?
// mode=coil: FC05 鍐欑嚎鍦? mode=register: FC06 鍐欎繚鎸佸瘎瀛樺櫒
// ============================================================================

void ModbusRouteHandler::handleModbusCoilBatch(AsyncWebServerRequest* request) {
    static uint32_t _coilBatchSeq = 0;
    uint32_t reqSeq = ++_coilBatchSeq;
    
    if (!ctx->requireAuth(request)) return;
    
    auto modbusLease = getModbusMaster(ctx, request);
    ModbusHandler* modbus = modbusLease.get();
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t channelCount = (uint16_t)ctx->getParamInt(request, "channelCount", 8);
    uint16_t coilBase = (uint16_t)ctx->getParamInt(request, "coilBase", 0);
    String action = ctx->getParamValue(request, "action", "allOff");
    String mode = ctx->getParamValue(request, "mode", "coil");
    
    LOG_WARNINGF("[CoilBatch#%u] HTTP entry: slave=%d count=%d action=%s mode=%s clientIP=%s",
                 reqSeq, slaveAddr, channelCount, action.c_str(), mode.c_str(),
                 request->client() ? request->client()->remoteIP().toString().c_str() : "?");

    if (rejectModbusHeavyRequest(request, "Modbus batch control", 8192)) return;

    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    if (channelCount == 0 || channelCount > Protocols::MODBUS_MAX_WRITE_COILS) {
        ctx->sendBadRequest(request, "Invalid channel count (1-32)");
        return;
    }
    
    OneShotResult result;
    
    // 鏌ユ壘璁惧閰嶇疆涓殑 batchRegister 鍜?batchRegType锛堥浂鎷疯礉璁块棶锛?
    uint16_t batchReg = 0;
    uint8_t batchRegType = 0;  // 0=浣嶅浘, 1=鍛戒护
    {
        int devIdxBatch = findDeviceIndexBySlaveAddr(modbus, slaveAddr);
        if (devIdxBatch >= 0) {
            const ModbusSubDevice& batchDev = modbus->getSubDevice((uint8_t)devIdxBatch);
            batchReg = batchDev.batchRegister;
            batchRegType = batchDev.batchRegType;
        }
    }

    if (mode == "register") {
        if (batchReg > 0 && batchRegType == 0) {
            // 浣嶅浘瀵勫瓨鍣ㄦā寮忥細鍗曟 FC06 鍐欎綅鍥撅紙姣?bit 瀵瑰簲涓€涓€氶亾锛?
            uint16_t bitmask = (channelCount >= 16) ? 0xFFFF : (uint16_t)((1 << channelCount) - 1);
            if (action == "allOn") {
                result = modbus->writeRegisterOnce(slaveAddr, batchReg, bitmask, true);
            } else if (action == "allOff") {
                result = modbus->writeRegisterOnce(slaveAddr, batchReg, (uint16_t)0, true);
            } else if (action == "allToggle") {
                OneShotResult readR = modbus->readRegistersOnce(slaveAddr, 0x03, batchReg, 1, true);
                if (readR.error != ONESHOT_SUCCESS) {
                    sendOneShotError(ctx, request, readR);
                    return;
                }
                uint16_t current = readR.data[0];
                result = modbus->writeRegisterOnce(slaveAddr, batchReg, (uint16_t)(current ^ bitmask), true);
            } else {
                ctx->sendBadRequest(request, "Invalid action (allOn/allOff/allToggle)");
                return;
            }
            if (result.error != ONESHOT_SUCCESS) {
                sendOneShotError(ctx, request, result);
                return;
            }
        } else if (batchReg > 0 && batchRegType == 1 && action != "allToggle") {
            // 鍛戒护瀵勫瓨鍣ㄦā寮忥細鍐?1=鍏ㄥ紑, 0=鍏ㄥ叧锛堝涓洓绉戞妧 0x0034锛?
            // allToggle 涓嶆敮鎸侊紝闄嶇骇鍒伴€愰€氶亾妯″紡
            if (action == "allOn") {
                result = modbus->writeRegisterOnce(slaveAddr, batchReg, (uint16_t)1, true);
            } else if (action == "allOff") {
                result = modbus->writeRegisterOnce(slaveAddr, batchReg, (uint16_t)0, true);
            } else {
                ctx->sendBadRequest(request, "Invalid action (allOn/allOff/allToggle)");
                return;
            }
            if (result.error != ONESHOT_SUCCESS) {
                sendOneShotError(ctx, request, result);
                return;
            }
        } else {
            // 閫愰€氶亾 FC06 妯″紡
            if (action == "allOn" || action == "allOff") {
                uint16_t targetVal = (action == "allOn") ? 1 : 0;
                for (uint16_t i = 0; i < channelCount; i++) {
                    OneShotResult wr = modbus->writeRegisterOnce(slaveAddr, coilBase + i, targetVal, true);
                    if (wr.error != ONESHOT_SUCCESS) {
                        sendOneShotError(ctx, request, wr);
                        return;
                    }
                }
                result.error = ONESHOT_SUCCESS;
            } else if (action == "allToggle") {
                OneShotResult readR = modbus->readRegistersOnce(slaveAddr, 0x03, coilBase, channelCount, true);
                if (readR.error != ONESHOT_SUCCESS) {
                    sendOneShotError(ctx, request, readR);
                    return;
                }
                for (uint16_t i = 0; i < channelCount; i++) {
                    uint16_t newVal = (readR.data[i] != 0) ? 0 : 1;
                    OneShotResult wr = modbus->writeRegisterOnce(slaveAddr, coilBase + i, newVal, true);
                    if (wr.error != ONESHOT_SUCCESS) {
                        sendOneShotError(ctx, request, wr);
                        return;
                    }
                }
                result.error = ONESHOT_SUCCESS;
            } else {
                ctx->sendBadRequest(request, "Invalid action (allOn/allOff/allToggle)");
                return;
            }
        }
    } else {
        // 绾垮湀妯″紡锛欶C15 鎵归噺鍐?
        if (action == "allOn" || action == "allOff") {
            bool targetOn = (action == "allOn");
            bool values[Protocols::MODBUS_MAX_WRITE_COILS];
            for (uint16_t i = 0; i < channelCount; i++) values[i] = targetOn;
            result = modbus->writeMultipleCoilsOnce(slaveAddr, coilBase, channelCount, values, true);
            if (result.error != ONESHOT_SUCCESS) {
                sendOneShotError(ctx, request, result);
                return;
            }
        } else if (action == "allToggle") {
            result = modbus->writeCoilOnce(slaveAddr, coilBase, (uint16_t)0x5A00, true);  // isControl=true
        } else {
            ctx->sendBadRequest(request, "Invalid action (allOn/allOff/allToggle)");
            return;
        }
    }
    
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
        return;
    }
    
    // 鎿嶄綔鍚庤鍙栧疄闄呯姸鎬?
    OneShotResult readResult;
    bool useBitmapRead = (mode == "register" && batchReg > 0 && batchRegType == 0);
    if (useBitmapRead) {
        readResult = modbus->readRegistersOnce(slaveAddr, 0x03, batchReg, 1, true);
    } else if (mode == "register") {
        readResult = modbus->readRegistersOnce(slaveAddr, 0x03, coilBase, channelCount, true);
    } else {
        uint16_t readQty = ((channelCount + 7) / 8) * 8;
        if (readQty < 8) readQty = 8;
        readResult = modbus->readRegistersOnce(slaveAddr, 0x01, coilBase, readQty, true);
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["channelCount"] = channelCount;
    data["action"] = action;
    data["mode"] = mode;
    JsonArray states = data["states"].to<JsonArray>();
    bool stateValues[32]; // 鐢ㄤ簬 MQTT 涓婃姤
    if (readResult.error == ONESHOT_SUCCESS) {
        if (useBitmapRead) {
            uint16_t bitmap = readResult.data[0];
            for (uint16_t i = 0; i < channelCount; i++) {
                bool s = (bitmap >> i) & 1 ? true : false;
                states.add(s);
                if (i < 32) stateValues[i] = s;
            }
        } else {
            for (uint16_t i = 0; i < channelCount; i++) {
                bool s = readResult.data[i] != 0;
                states.add(s);
                if (i < 32) stateValues[i] = s;
            }
        }
    } else {
        for (uint16_t i = 0; i < channelCount; i++) {
            bool s = (action == "allOn");
            states.add(s);
            if (i < 32) stateValues[i] = s;
        }
    }

    // MQTT 涓婃姤鎵归噺鎺у埗缁撴灉锛坣cMode 璁惧闇€瑕佸弽杞负鐢ㄦ埛鎰忓浘鍊硷級
    int devIdx = findDeviceIndexBySlaveAddr(modbus, slaveAddr);
    if (devIdx >= 0) {
        bool isNcMode = modbus->getSubDevice((uint8_t)devIdx).ncMode;
        if (isNcMode) {
            bool reportValues[32];
            for (uint16_t i = 0; i < channelCount && i < 32; i++) {
                reportValues[i] = !stateValues[i];
            }
            publishBatchControlReport(ctx, modbus, devIdx, channelCount, reportValues);
        } else {
            publishBatchControlReport(ctx, modbus, devIdx, channelCount, stateValues);
        }
    }

    // 瑙﹀彂 mc: 浜嬩欢锛堜緵浜嬩欢瑙﹀彂瑙勫垯鍖归厤锛屾寜姣忎釜閫氶亾鐙珛瑙﹀彂锛?
    if (devIdx >= 0) {
        char mcIdBuf[16];
        snprintf(mcIdBuf, sizeof(mcIdBuf), "mc:%d", devIdx);
        for (uint16_t ch = 0; ch < channelCount; ch++) {
            bool chState = (ch < 32) ? stateValues[ch] : (action == "allOn");
            char mcDataBuf[96];
            snprintf(mcDataBuf, sizeof(mcDataBuf), "{\"d\":%d,\"c\":%d,\"a\":\"%s\"}",
                devIdx, ch, chState ? "on" : "off");
            PeriphExecManager::getInstance().triggerEventById(String(mcIdBuf), String(mcDataBuf));
        }
    }

    addModbusDebug(doc, modbus);
    
    HandlerUtils::sendJsonStream(request, doc);
}

// ============================================================================
// POST /api/modbus/coil/delay 鈥?绾垮湀寤舵椂鎺у埗
// NO妯″紡锛氱‖浠堕棯寮€锛堣澶囩‖浠跺畾鏃讹級锛汵C妯″紡锛氳蒋浠跺欢鏃朵换鍔★紙鍙嶈浆閫昏緫锛?
// ============================================================================

void ModbusRouteHandler::handleModbusCoilDelay(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    
    auto modbusLease = getModbusMaster(ctx, request);
    ModbusHandler* modbus = modbusLease.get();
    if (!modbus) return;
    
    const int slaveInput = ctx->getParamInt(request, "slaveAddress", 1);
    uint8_t slaveAddr = static_cast<uint8_t>(slaveInput);
    const int channelInput = ctx->getParamInt(request, "channel", 0);
    uint16_t channel = static_cast<uint16_t>(channelInput);
    uint16_t delayBase = (uint16_t)ctx->getParamInt(request, "delayBase", 0x0200);
    uint16_t delayUnits = (uint16_t)ctx->getParamInt(request, "delayUnits", 50);
    bool ncMode = ctx->getParamBool(request, "ncMode", false);
    const int baseInput = ctx->getParamInt(request, "coilBase", 0);
    uint16_t coilBase = static_cast<uint16_t>(baseInput);
    String mode = ctx->getParamValue(request, "mode", "coil");
    // delayMode: 0=FC05闂紑(榛樿), 1=杞欢寤舵椂, 2=纭欢瀵勫瓨鍣ㄥ欢鏃?
    uint8_t delayMode = (uint8_t)ctx->getParamInt(request, "delayMode", 0);
    
    // 浠庤澶囬厤缃腑鏌ユ壘榛樿 delayMode
    int devIdxForDelay = findDeviceIndexBySlaveAddr(modbus, slaveAddr);
    if (devIdxForDelay >= 0 && delayMode == 0) {
        const ModbusSubDevice& delayDev = modbus->getSubDevice((uint8_t)devIdxForDelay);
        if (delayDev.delayMode > 0) {
            delayMode = delayDev.delayMode;
        }
    }

    if (rejectModbusHeavyRequest(request, "Modbus delayed control", 8192)) return;

    if (!validRelayChannel(slaveInput, channelInput, baseInput)) {
        ctx->sendBadRequest(request, "Invalid relay address, channel or base");
        return;
    }
    if (delayUnits == 0 || delayUnits > 255) {
        ctx->sendBadRequest(request, "Invalid delay (1-255 x100ms, max 25.5s)");
        return;
    }
    
    unsigned long delayMs = (unsigned long)delayUnits * 100;
    uint16_t coilAddr = coilBase + channel;

    // === 纭欢瀵勫瓨鍣ㄥ欢鏃舵ā寮?(delayMode=2) ===
    // 鍚戦€氶亾瀵勫瓨鍣ㄥ啓鍏ュ€?> 1锛岃澶囪嚜鍔ㄥ欢鏃?(value-1)*0.01s 鍚庣炕杞?
    // 闂紑娴佺▼锛氬厛鍐?(寮€鍚?锛屽啀鍐欏欢鏃跺€?寤舵椂鍚庣炕杞?鍏抽棴)
    if (delayMode == 2) {
        // 纭欢瀵勫瓨鍣ㄥ欢鏃讹細浣跨敤FC06鍚戦€氶亾瀵勫瓨鍣ㄥ啓鍏ュ欢鏃跺€?
        // 璁惧琛屼负锛氬啓鍏ュ€?N>1 鏃讹紝寤舵椂 (N-1)*0.01 绉掑悗缈昏浆
        // 闂紑瀹炵幇锛氬厛寮€(l)锛屽啀鍐欏欢鏃跺€硷紝璁惧鍒版椂鍚庤嚜鍔ㄧ炕杞?鍏抽棴)
        uint16_t delayValue = (uint16_t)(delayUnits * 10 / 1) + 1;  // delayUnits*100ms 鈫?value=(N-1)*0.01s 鈫?N=delayUnits*10+1
        if (delayValue < 2) delayValue = 2;
        if (delayValue > 65535) delayValue = 65535;
        
        // 鍏堝紑鍚户鐢靛櫒
        OneShotResult onResult;
        if (ncMode) {
            onResult = modbus->writeRegisterOnce(slaveAddr, coilAddr, 0, true);  // NC: 鍐?=鏂紑=璁惧閫氱數
        } else {
            onResult = modbus->writeRegisterOnce(slaveAddr, coilAddr, 1, true);  // NO: 鍐?=寮€鍚?
        }
        if (onResult.error != ONESHOT_SUCCESS) {
            sendOneShotError(ctx, request, onResult);
            return;
        }
        
        // 鍐欏叆寤舵椂鍊硷紝璁惧鍦ㄥ欢鏃跺悗鑷姩缈昏浆锛堝叧闂級
        OneShotResult delayResult = modbus->writeRegisterOnce(slaveAddr, coilAddr, delayValue, true);
        if (delayResult.error != ONESHOT_SUCCESS) {
            sendOneShotError(ctx, request, delayResult);
            return;
        }
        
        // NC妯″紡棰濆锛氭坊鍔犺蒋浠跺欢鏃朵换鍔″湪寤舵椂鍚庢仮澶嶄负ON锛堝洜涓虹‖浠剁炕杞細浣縉C鏂數锛?
        if (ncMode) {
            modbus->addCoilDelayTask(slaveAddr, coilAddr, delayMs + 500, true, true);
        }
        
        JsonDocument doc;
        doc["success"] = true;
        JsonObject data = doc["data"].to<JsonObject>();
        data["channel"] = channel;
        data["delayUnits"] = delayUnits;
        data["delayMs"] = (uint32_t)delayMs;
        data["ncMode"] = ncMode;
        data["mode"] = "hardware";
        data["delayValue"] = delayValue;
        addModbusDebug(doc, modbus);
        
        HandlerUtils::sendJsonStream(request, doc);
        return;
    }

    if (mode == "register") {
        // 瀵勫瓨鍣ㄦā寮忥細浣跨敤 FC 0x06 鍐欏瘎瀛樺櫒
        if (ncMode) {
            // NC 甯搁棴妯″紡锛氬厛鍐?0锛堟柇寮€锛夛紝寤舵椂鍚庡啓 1锛堟仮澶嶏級
            OneShotResult result = modbus->writeRegisterOnce(slaveAddr, coilAddr, 0, true);  // isControl=true
            if (result.error != ONESHOT_SUCCESS) {
                sendOneShotError(ctx, request, result);
                return;
            }
            bool ok = modbus->addCoilDelayTask(slaveAddr, coilAddr, delayMs, true, true);
            if (!ok) {
                ctx->sendError(request, 500, "Delay task queue full");
                return;
            }
        } else {
            // NO 甯稿紑妯″紡锛氬厛鍐?1锛堝惛鍚堬級锛屽欢鏃跺悗鍐?0锛堟柇寮€锛?
            OneShotResult result = modbus->writeRegisterOnce(slaveAddr, coilAddr, 1, true);  // isControl=true
            if (result.error != ONESHOT_SUCCESS) {
                sendOneShotError(ctx, request, result);
                return;
            }
            bool ok = modbus->addCoilDelayTask(slaveAddr, coilAddr, delayMs, false, true);
            if (!ok) {
                ctx->sendError(request, 500, "Delay task queue full");
                return;
            }
        }
        
        JsonDocument doc;
        doc["success"] = true;
        JsonObject data = doc["data"].to<JsonObject>();
        data["channel"] = channel;
        data["delayUnits"] = delayUnits;
        data["delayMs"] = (uint32_t)delayMs;
        data["ncMode"] = ncMode;
        data["mode"] = "software";
        addModbusDebug(doc, modbus);
        
        HandlerUtils::sendJsonStream(request, doc);
    } else {
        // 绾垮湀妯″紡锛氫娇鐢?FC 0x05 鍐欑嚎鍦?
        // 姝ラ1锛氱珛鍗虫墦寮€缁х數鍣紙鍐欑嚎鍦?ON锛?
        // NO妯″紡: 绾垮湀ON=璁惧閫氱數; NC妯″紡: 绾垮湀ON=NC鏂紑=璁惧鏂數锛堜絾鎴戜滑瑕佸厛鎵撳紑锛屾墍浠C杩欓噷鍐橭FF锛?
        // 淇锛氬欢鏃舵柇寮€ = 鍏堟墦寮€璁惧锛屽欢鏃跺悗鍏抽棴
        // NO妯″紡: 鍏堝啓ON(璁惧閫? 鈫?寤舵椂鍚庤嚜鍔∣FF(璁惧鏂?
        // NC妯″紡: 鍏堝啓OFF(NC闂悎=璁惧閫? 鈫?寤舵椂鍚庡啓ON(NC鏂紑=璁惧鏂?
        bool initialValue = ncMode ? false : true;  // NO鍐橭N, NC鍐橭FF
        OneShotResult result = modbus->writeCoilOnce(slaveAddr, coilAddr, initialValue, true);  // isControl=true
        if (result.error != ONESHOT_SUCCESS) {
            sendOneShotError(ctx, request, result);
            return;
        }

        // 姝ラ2锛氬惎鍔ㄥ欢鏃舵満鍒讹紝寤舵椂鍚庤嚜鍔ㄥ叧闂?
        if (ncMode) {
            // NC 甯搁棴妯″紡锛氫娇鐢ㄨ蒋浠跺欢鏃朵换鍔?
            // 寤舵椂鍚庡啓绾垮湀ON (NC鏂紑=璁惧鏂數)
            bool ok = modbus->addCoilDelayTask(slaveAddr, coilAddr, delayMs, true);
            if (!ok) {
                ctx->sendError(request, 500, "Delay task queue full");
                return;
            }
        } else {
            // NO 甯稿紑妯″紡锛氫娇鐢ㄧ‖浠堕棯寮€鎸囦护
            // 闂紑鎸囦护锛欶C 0x05 鍦板潃=(delayBase + channel) 鍊?寤舵椂鍗曚綅鍦ㄩ珮瀛楄妭
            // 纭欢浼氳嚜鍔細寤舵椂鍚庡叧闂户鐢靛櫒
            uint16_t delayAddr = delayBase + channel;
            uint16_t rawValue = ((uint16_t)delayUnits) << 8;

            result = modbus->writeCoilOnce(slaveAddr, delayAddr, rawValue, true);  // isControl=true
            if (result.error != ONESHOT_SUCCESS) {
                sendOneShotError(ctx, request, result);
                return;
            }
        }
        
        JsonDocument doc;
        doc["success"] = true;
        JsonObject data = doc["data"].to<JsonObject>();
        data["channel"] = channel;
        data["delayUnits"] = delayUnits;
        data["delayMs"] = (uint32_t)delayMs;
        data["ncMode"] = ncMode;
        data["mode"] = ncMode ? "software" : "hardware";
        addModbusDebug(doc, modbus);
        
        HandlerUtils::sendJsonStream(request, doc);
    }
}

// ============================================================================
// GET /api/modbus/coil/status 鈥?璇诲彇绾垮湀鐘舵€?
// mode=coil: FC01 璇荤嚎鍦? mode=register: FC03 璇讳繚鎸佸瘎瀛樺櫒锛堟瘡閫氶亾涓€涓瘎瀛樺櫒锛?
// ============================================================================

void ModbusRouteHandler::handleModbusCoilStatus(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    auto modbusLease = getModbusMaster(ctx, request);
    ModbusHandler* modbus = modbusLease.get();
    if (!modbus) return;

    // Validate before narrowing; wrapping an HTTP parameter can address another slave.
    const int slaveInput = ctx->getParamInt(request, "slaveAddress", 1);
    const int countInput = ctx->getParamInt(request, "channelCount", 8);
    const int baseInput = ctx->getParamInt(request, "coilBase", 0);
    const String mode = ctx->getParamValue(request, "mode", "coil");
    if (mode != "coil" && mode != "register") {
        ctx->sendBadRequest(request, "Invalid mode (must be 'coil' or 'register')");
        return;
    }
    if (slaveInput < 1 || slaveInput > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    if (countInput < 1 || countInput > Protocols::MODBUS_MAX_WRITE_COILS) {
        ctx->sendBadRequest(request, "Invalid channel count (1-32)");
        return;
    }
    if (baseInput < 0 || baseInput > 65535 || countInput - 1 > 65535 - baseInput) {
        ctx->sendBadRequest(request, "Relay address range exceeds 0-65535");
        return;
    }
    if (rejectModbusHeavyRequest(request, "Modbus coil status", 8192)) return;

    const uint8_t slaveAddr = static_cast<uint8_t>(slaveInput);
    const uint16_t channelCount = static_cast<uint16_t>(countInput);
    const uint16_t coilBase = static_cast<uint16_t>(baseInput);
    // FC01 quantity counts coils, not response bytes. Padding a four-channel
    // device to eight coils can read nonexistent addresses and falsely mark it offline.
    OneShotResult result = modbus->readRegistersOnce(
        slaveAddr, mode == "register" ? 0x03 : 0x01, coilBase, channelCount, true);
    if (result.error == ONESHOT_SUCCESS && result.count != channelCount) {
        result.error = ONESHOT_CRC_ERROR;
    }
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["channelCount"] = channelCount;
    data["coilBase"] = coilBase;
    data["mode"] = mode;
    JsonArray states = data["states"].to<JsonArray>();
    for (uint16_t i = 0; i < channelCount; i++) states.add(result.data[i] != 0);
    addModbusDebug(doc, modbus);
    HandlerUtils::sendJsonStream(request, doc);
}

// ============================================================================
// POST /api/modbus/device/address 鈥?璇诲彇/璁剧疆浠庣珯鍦板潃锛團C 0x03 / FC 0x06锛?
// ============================================================================

void ModbusRouteHandler::handleModbusDeviceAddress(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    
    auto modbusLease = getModbusMaster(ctx, request);
    ModbusHandler* modbus = modbusLease.get();
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t addrRegister = (uint16_t)ctx->getParamInt(request, "addressRegister", 0x0000);
    String newAddrStr = ctx->getParamValue(request, "newAddress", "");

    if (rejectModbusHeavyRequest(request, "Modbus device address", 8192)) return;

    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    
    if (newAddrStr.isEmpty()) {
        // 璇诲彇褰撳墠鍦板潃锛氫娇鐢ㄥ箍鎾湴鍧€ 0x00锛堟枃妗ｇ‘璁わ細00 03 00 00 00 01 85 DB锛?
        // 璁惧浼氫互瀹為檯鍦板潃鍝嶅簲锛堝 03 03 02 00 03 ...锛?
        // sendOneShotRequest 涓?expectedSlaveAddr==0 鏃跺凡璺宠繃鍦板潃鍖归厤
        OneShotResult result = modbus->readRegistersOnce(0x00, 0x03, addrRegister, 1, true);  // isControl=true
        if (result.error != ONESHOT_SUCCESS) {
            sendOneShotError(ctx, request, result);
            return;
        }
        data["currentAddress"] = result.data[0];
        data["register"] = addrRegister;
    } else {
        // 璁剧疆鏂板湴鍧€锛氫娇鐢ㄥ箍鎾湴鍧€ 0x00锛孎C 0x10 鍐欏涓瘎瀛樺櫒
        uint16_t newAddr = (uint16_t)newAddrStr.toInt();
        if (newAddr < 1 || newAddr > 255) {
            ctx->sendBadRequest(request, "Invalid new address (1-255)");
            return;
        }
        uint16_t regValues[1] = { newAddr };
        OneShotResult result = modbus->writeMultipleRegistersOnce(0x00, addrRegister, 1, regValues, true);  // isControl=true
        if (result.error != ONESHOT_SUCCESS) {
            sendOneShotError(ctx, request, result);
            return;
        }
        data["previousAddress"] = slaveAddr;
        data["newAddress"] = newAddr;
        data["register"] = addrRegister;
    }
    addModbusDebug(doc, modbus);
    
    HandlerUtils::sendJsonStream(request, doc);
}

// ============================================================================
// POST /api/modbus/device/baudrate 鈥?璁剧疆娉㈢壒鐜囷紙FC 0x06锛?
// ============================================================================

void ModbusRouteHandler::handleModbusDeviceBaudrate(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    
    auto modbusLease = getModbusMaster(ctx, request);
    ModbusHandler* modbus = modbusLease.get();
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint32_t baudRate = (uint32_t)ctx->getParamInt(request, "baudRate", 9600);
    // mode: 0=FC0xB0涓撴湁鎸囦护(榛樿), 1=FC06鍐欎繚鎸佸瘎瀛樺櫒
    uint8_t mode = (uint8_t)ctx->getParamInt(request, "mode", 0);
    // baudRateReg: FC06妯″紡涓嬫尝鐗圭巼瀵勫瓨鍣ㄥ湴鍧€(榛樿0x0033)
    uint16_t baudRateReg = (uint16_t)ctx->getParamInt(request, "baudRateReg", 0x0033);

    if (rejectModbusHeavyRequest(request, "Modbus baudrate change", 8192)) return;
    
    // 浠庤澶囬厤缃腑鏌ユ壘榛樿鍊?
    int devIdx = findDeviceIndexBySlaveAddr(modbus, slaveAddr);
    if (devIdx >= 0 && mode == 0) {
        const ModbusSubDevice& baudDev = modbus->getSubDevice((uint8_t)devIdx);
        // 濡傛灉璁惧閰嶇疆浜?baudRateMode=1锛岃嚜鍔ㄥ垏鎹㈠埌瀵勫瓨鍣ㄦā寮?
        if (baudDev.baudRateMode == 1) {
            mode = 1;
            baudRateReg = baudDev.baudRateReg;
            if (baudRateReg == 0) baudRateReg = 0x0033;
        }
    }
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    
    if (mode == 1) {
        // FC06 瀵勫瓨鍣ㄦā寮忥細鏍囧噯 Modbus 鍐欎繚鎸佸瘎瀛樺櫒
        // 閫氱敤娉㈢壒鐜囩紪鐮佹槧灏勶紙鍏煎涓洓绉戞妧绛夊搧鐗岋級
        // 0:4800, 1:9600, 2:14400, 3:19200, 4:38400, 5:56000, 6:57600, 7:115200
        uint16_t baudCode;
        switch (baudRate) {
            case 4800:   baudCode = 0; break;
            case 9600:   baudCode = 1; break;
            case 14400:  baudCode = 2; break;
            case 19200:  baudCode = 3; break;
            case 38400:  baudCode = 4; break;
            case 56000:  baudCode = 5; break;
            case 57600:  baudCode = 6; break;
            case 115200: baudCode = 7; break;
            default:     baudCode = 1; break; // 榛樿 9600
        }
        
        // 鍏佽閫氳繃 baudCode 鍙傛暟鐩存帴鎸囧畾缂栫爜鍊?
        String baudCodeStr = ctx->getParamValue(request, "baudCode", "");
        if (!baudCodeStr.isEmpty()) {
            baudCode = (uint16_t)baudCodeStr.toInt();
        }
        
        // FC 0x06 鍐欎繚鎸佸瘎瀛樺櫒
        OneShotResult result = modbus->writeRegisterOnce(slaveAddr, baudRateReg, baudCode, true);  // isControl=true
        if (result.error != ONESHOT_SUCCESS) {
            sendOneShotError(ctx, request, result);
            return;
        }
        
        JsonDocument doc;
        doc["success"] = true;
        JsonObject data = doc["data"].to<JsonObject>();
        data["baudRate"] = baudRate;
        data["baudCode"] = baudCode;
        data["mode"] = "register";
        data["register"] = baudRateReg;
        addModbusDebug(doc, modbus);
        
        HandlerUtils::sendJsonStream(request, doc);
    } else {
        // FC 0xB0 涓撴湁鎸囦护妯″紡锛堥粯璁わ紝鍏煎鍥戒骇閫氱敤缁х數鍣ㄦ澘锛?
        uint8_t baudCode;
        switch (baudRate) {
            case 1200:   baudCode = 0; break;
            case 2400:   baudCode = 1; break;
            case 4800:   baudCode = 2; break;
            case 9600:   baudCode = 3; break;
            case 19200:  baudCode = 4; break;
            case 115200: baudCode = 5; break;
            default:     baudCode = 3; break; // 榛樿 9600
        }
        
        // 鍏佽鐢ㄦ埛閫氳繃 baudCode 鍙傛暟鐩存帴鎸囧畾缂栫爜鍊?
        String baudCodeStr = ctx->getParamValue(request, "baudCode", "");
        if (!baudCodeStr.isEmpty()) {
            baudCode = (uint8_t)baudCodeStr.toInt();
        }
        
        // 鍙戦€佷笓鏈?FC 0xB0 娉㈢壒鐜囪缃抚锛歔slaveAddr, 0xB0, 0x00, 0x00, baudCode, 0x00]
        uint8_t frame[6];
        frame[0] = slaveAddr;
        frame[1] = 0xB0;
        frame[2] = 0x00;
        frame[3] = 0x00;
        frame[4] = baudCode;
        frame[5] = 0x00;
        
        OneShotResult result = modbus->sendRawFrameOnce(slaveAddr, frame, 6, true);  // isControl=true
        if (result.error != ONESHOT_SUCCESS) {
            sendOneShotError(ctx, request, result);
            return;
        }
        
        JsonDocument doc;
        doc["success"] = true;
        JsonObject data = doc["data"].to<JsonObject>();
        data["baudRate"] = baudRate;
        data["baudCode"] = baudCode;
        data["mode"] = "proprietary";
        addModbusDebug(doc, modbus);
        
        HandlerUtils::sendJsonStream(request, doc);
    }
}

// ============================================================================
// GET /api/modbus/device/inputs 鈥?璇诲彇绂绘暎杈撳叆锛團C 0x02锛?
// ============================================================================

void ModbusRouteHandler::handleModbusDiscreteInputs(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    
    auto modbusLease = getModbusMaster(ctx, request);
    ModbusHandler* modbus = modbusLease.get();
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t inputCount = (uint16_t)ctx->getParamInt(request, "inputCount", 4);
    uint16_t inputBase = (uint16_t)ctx->getParamInt(request, "inputBase", 0);

    if (rejectModbusHeavyRequest(request, "Modbus discrete inputs", 8192)) return;

    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    if (inputCount == 0 || inputCount > Protocols::MODBUS_MAX_WRITE_COILS) {
        ctx->sendBadRequest(request, "Invalid input count (1-32)");
        return;
    }
    
    // 鍚?FC 0x01锛屼娇鐢?qty 鍚戜笂鍙栨暣鍒?8 鐨勫€嶆暟纭繚鏍囧噯鏍煎紡
    uint16_t readQty = ((inputCount + 7) / 8) * 8;
    if (readQty < 8) readQty = 8;
    OneShotResult result = modbus->readRegistersOnce(slaveAddr, 0x02, inputBase, readQty, true);  // isControl=true
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
        return;
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["inputCount"] = inputCount;
    data["inputBase"] = inputBase;
    JsonArray states = data["states"].to<JsonArray>();
    for (uint16_t i = 0; i < inputCount; i++) {
        states.add(result.data[i] != 0);
    }
    
    HandlerUtils::sendJsonStream(request, doc);
}

// ============================================================================
// GET /api/modbus/register/read 鈥?璇诲彇淇濇寔/杈撳叆瀵勫瓨鍣紙FC 0x03 / FC 0x04锛?
// ============================================================================

void ModbusRouteHandler::handleModbusRegisterRead(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    
    auto modbusLease = getModbusMaster(ctx, request);
    ModbusHandler* modbus = modbusLease.get();
    if (!modbus) return;
    
    const int slaveAddr = ctx->getParamInt(request, "slaveAddress", 1);
    const int startAddr = ctx->getParamInt(request, "startAddress", 0);
    const int quantity = ctx->getParamInt(request, "quantity", 1);
    const int fc = ctx->getParamInt(request, "functionCode", 3);

    if (rejectModbusHeavyRequest(request, "Modbus register read", 12288)) return;
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    if (quantity < 1 || quantity > Protocols::MODBUS_ONESHOT_BUFFER_SIZE) {
        ctx->sendBadRequest(request, "Invalid quantity (1-32)");
        return;
    }
    if (startAddr < 0 || startAddr > 65535 || quantity > 65536 - startAddr) {
        ctx->sendBadRequest(request, "Invalid register range (0-65535)");
        return;
    }
    if (fc != 0x03 && fc != 0x04) {
        ctx->sendBadRequest(request, "Invalid function code (3 or 4)");
        return;
    }
    
    ModbusReadDiagnostics diagnostics;
    OneShotResult result = modbus->readRegistersOnce(slaveAddr, fc, startAddr, quantity,
                                                    true, 0, 0xFF, nullptr, &diagnostics);
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result, &diagnostics);
        return;
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["count"] = quantity;
    data["startAddress"] = startAddr;
    JsonArray values = data["values"].to<JsonArray>();
    for (uint16_t i = 0; i < quantity; i++) {
        values.add(result.data[i]);
    }
    addModbusDebug(doc, modbus);
    addModbusReadDiagnostics(doc, diagnostics);
    
    HandlerUtils::sendJsonStream(request, doc);
}

// ============================================================================
// POST /api/modbus/register/write 鈥?鍐欏崟涓繚鎸佸瘎瀛樺櫒锛團C 0x06锛?
// ============================================================================

void ModbusRouteHandler::handleModbusRegisterWrite(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    
    auto modbusLease = getModbusMaster(ctx, request);
    ModbusHandler* modbus = modbusLease.get();
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t regAddr = (uint16_t)ctx->getParamInt(request, "registerAddress", 0);
    uint16_t value = (uint16_t)ctx->getParamInt(request, "value", 0);

    if (rejectModbusCriticalControl(request, "Modbus register write", 6144)) return;
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    
    int devIdx = findDeviceIndexBySlaveAddr(modbus, slaveAddr);
    const ModbusSubDevice* motorDevPtr = nullptr;
    MotorSoftLimitDecision limitDecision;
    if (devIdx >= 0) {
        const ModbusSubDevice& dev = modbus->getSubDevice((uint8_t)devIdx);
        if (String(dev.deviceType) == "motor") {
            motorDevPtr = &dev;
            String motorAction;
            if (regAddr == dev.motorRegs[0]) motorAction = "forward";
            else if (regAddr == dev.motorRegs[1]) motorAction = "reverse";
            if (!motorAction.isEmpty()) {
                limitDecision = evaluateMotorSoftLimit(dev, motorAction, value);
                if (!limitDecision.allowed) {
                    JsonDocument doc;
                    doc["success"] = false;
                    doc["errorCode"] = "MOTOR_SOFT_LIMIT";
                    doc["error"] = "motor_soft_limit";
                    JsonObject data = doc["data"].to<JsonObject>();
                    data["currentPosition"] = dev.motorCurrentPosition;
                    data["minPosition"] = dev.motorMinPosition;
                    data["maxPosition"] = dev.motorMaxPosition;
                    HandlerUtils::sendJsonStream(request, doc);
                    return;
                }
                if (limitDecision.enabled && dev.motorRegs[4] != 0) {
                    OneShotResult pulseResult = modbus->writeRegisterOnce(slaveAddr, dev.motorRegs[4], limitDecision.pulse, true);
                    if (pulseResult.error != ONESHOT_SUCCESS) {
                        sendOneShotError(ctx, request, pulseResult);
                        return;
                    }
                    value = 1;
                }
            }
        }
    }

    OneShotResult result = modbus->writeRegisterOnce(slaveAddr, regAddr, value, true);  // isControl=true
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
        return;
    }
    
    // MQTT 涓婃姤瀵勫瓨鍣ㄥ啓鍏ョ粨鏋?
    if (devIdx >= 0 && motorDevPtr) {
        if (regAddr == motorDevPtr->motorRegs[4]) {
            modbus->updateMotorRuntime((uint8_t)devIdx, motorDevPtr->motorCurrentPosition, value);
        } else if (limitDecision.enabled) {
            modbus->updateMotorRuntime((uint8_t)devIdx, limitDecision.nextPosition, limitDecision.pulse);
        }
    }

    if (devIdx >= 0) {
        const ModbusSubDevice& dev = modbus->getSubDevice((uint8_t)devIdx);
        uint16_t channel = (regAddr >= dev.coilBase && regAddr < dev.coilBase + dev.channelCount)
                           ? (regAddr - dev.coilBase) : 0;
        publishControlReport(ctx, modbus, devIdx, channel, String(value));
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["register"] = regAddr;
    data["value"] = value;
    addModbusDebug(doc, modbus);
    
    HandlerUtils::sendJsonStream(request, doc);
}

// ============================================================================
// POST /api/modbus/register/batch-write 鈥?鎵归噺鍐欎繚鎸佸瘎瀛樺櫒锛團C 0x10锛?
// ============================================================================

void ModbusRouteHandler::handleModbusRegisterBatchWrite(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    
    auto modbusLease = getModbusMaster(ctx, request);
    ModbusHandler* modbus = modbusLease.get();
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t startAddr = (uint16_t)ctx->getParamInt(request, "startAddress", 0);
    String valuesStr = ctx->getParamValue(request, "values", "[]");

    if (rejectModbusHeavyRequest(request, "Modbus batch register write", 8192)) return;
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    
    // 瑙ｆ瀽 JSON 鏁扮粍
    JsonDocument valDoc;
    DeserializationError err = deserializeJson(valDoc, valuesStr);
    if (err || !valDoc.is<JsonArray>()) {
        ctx->sendBadRequest(request, "Invalid values (JSON array expected)");
        return;
    }
    
    JsonArray arr = valDoc.as<JsonArray>();
    uint16_t quantity = arr.size();
    if (quantity == 0 || quantity > Protocols::MODBUS_MAX_REGISTERS_PER_READ) {
        ctx->sendBadRequest(request, "Invalid quantity (1-125)");
        return;
    }
    
    uint16_t regValues[125];
    for (uint16_t i = 0; i < quantity; i++) {
        regValues[i] = (uint16_t)arr[i].as<int>();
    }
    
    OneShotResult result = modbus->writeMultipleRegistersOnce(slaveAddr, startAddr, quantity, regValues, true);  // isControl=true
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
        return;
    }
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["startAddress"] = startAddr;
    data["quantity"] = quantity;
    JsonArray respValues = data["values"].to<JsonArray>();
    for (uint16_t i = 0; i < quantity; i++) {
        respValues.add(regValues[i]);
    }
    addModbusDebug(doc, modbus);
    
    HandlerUtils::sendJsonStream(request, doc);
}

// ============================================================================
// POST /api/modbus/motor/control 鈥?鐢垫満鎺у埗(姝ｈ浆/鍙嶈浆/鍋滄/璁鹃€熷害/璁捐剦鍐?
// ============================================================================

void ModbusRouteHandler::handleModbusMotorControl(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    auto& motors = MotorControlService::instance();
    const String action = request->method() == HTTP_GET ? String("state") : ctx->getParamValue(request, "action", "");
    JsonDocument response;
    JsonObject data = response["data"].to<JsonObject>();
    if (action == "state") {
        const int slave = ctx->getParamInt(request, "slaveAddress", 0);
        String ref = ctx->getParamValue(request, "motorRef", "");
        if (ref.isEmpty() && slave >= 1 && slave <= 247) ref = motors.referenceForSlave(static_cast<uint8_t>(slave));
        if (ref.isEmpty() || !motors.validReference(ref)) {
            ctx->sendError(request, 404, "Motor unavailable or configuration changed"); return;
        }
        MotorControlState state;
        motors.state(ref, state);
        data["motorRef"] = ref; data["motionKnown"] = false;
        data["parametersKnown"] = state.known; data["pending"] = state.pending;
        data["precision"] = state.precision; data["updatedMs"] = state.updatedMs;
        if (state.known) { data["speed"] = state.speed; data["pulse"] = state.pulse; }
        data["lastAction"] = state.lastAction; data["commandKnown"] = state.commandKnown;
        data["commandSuccess"] = state.commandSuccess; data["commandUpdatedMs"] = state.commandUpdatedMs;
        data["lastAcknowledgedAction"] = state.lastAcknowledgedAction; data["acknowledgedMs"] = state.acknowledgedMs;
        data["error"] = state.error; data["readError"] = state.readError;
        data["softLimitEnabled"] = state.softLimitEnabled; data["positionFeedback"] = false;
        data["minPosition"] = state.minPosition; data["maxPosition"] = state.maxPosition;
        data["estimatedPosition"] = state.estimatedPosition;
    } else if (action == "operation") {
        const int id = ctx->getParamInt(request, "operationId", 0);
        bool busy = false;
        if (id <= 0 || !motors.operation(static_cast<uint32_t>(id), data, &busy)) {
            if (busy) { ctx->sendError(request, 503, "Motor result temporarily busy"); return; }
            ctx->sendError(request, 404, "Motor operation not found"); return;
        }
    } else {
        if (rejectModbusCriticalControl(request, "Modbus motor control", 6144)) return;
        const int slave = ctx->getParamInt(request, "slaveAddress", 0);
        String ref = ctx->getParamValue(request, "motorRef", "");
        if (ref.isEmpty() && slave >= 1 && slave <= 247) ref = motors.referenceForSlave(static_cast<uint8_t>(slave));
        const String requestId = ctx->getParamValue(request, "requestId", "");
        uint32_t operationId = 0; String error;
        if (!motors.submit(ref, action, ctx->getParamInt(request, "value", 0), requestId, operationId, error)) {
            ctx->sendBadRequest(request, error); return;
        }
        // Acceptance is durable even if the immediate result lock is busy.
        // Return the ID so the client polls it instead of resubmitting motion.
        if (!motors.operation(operationId, data)) {
            data["operationId"] = operationId; data["pending"] = true;
            data["action"] = action; data["motionKnown"] = false;
        }
        data["accepted"] = true;
    }
    response["success"] = true;
    HandlerUtils::sendJsonStream(request, response);
}

#endif // FASTBEE_ENABLE_MODBUS
