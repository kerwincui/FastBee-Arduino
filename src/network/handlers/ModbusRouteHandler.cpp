#include "./network/handlers/ModbusRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "./protocols/ProtocolManager.h"
#include "./protocols/ModbusHandler.h"
#include <ArduinoJson.h>

// ============================================================================
// Modbus 通用控制 API 辅助函数
// ============================================================================

// 将 OneShotError 转换为 HTTP 错误响应
static void sendOneShotError(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                              const OneShotResult& result) {
    JsonDocument doc;
    doc["success"] = false;
    
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

// 获取 ModbusHandler 指针并校验 Master 模式，失败时发送错误响应
static ModbusHandler* getModbusMaster(WebHandlerContext* ctx, AsyncWebServerRequest* request) {
    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return nullptr;
    }
    ModbusHandler* modbus = pm->getModbusHandler();
    if (!modbus) {
        ctx->sendError(request, 503, "Modbus not started");
        return nullptr;
    }
    if (modbus->getMode() != MODBUS_MASTER) {
        ctx->sendError(request, 400, "Modbus is not in Master mode");
        return nullptr;
    }
    return modbus;
}

// 辅助：将 Modbus TX/RX 调试帧添加到 JSON 响应
static void addModbusDebug(JsonDocument& doc, ModbusHandler* modbus) {
    JsonObject debug = doc["debug"].to<JsonObject>();
    debug["tx"] = modbus->getLastTxHex();
    debug["rx"] = modbus->getLastRxHex();
}

// 辅助：通过 slaveAddress 查找子设备索引
static int findDeviceIndexBySlaveAddr(ModbusHandler* modbus, uint8_t slaveAddr) {
    ModbusConfig cfg = modbus->getConfig();
    for (uint8_t i = 0; i < cfg.master.deviceCount; i++) {
        if (cfg.master.devices[i].slaveAddress == slaveAddr) return (int)i;
    }
    return -1;
}

// 辅助：构建并发布单条 MQTT 上报
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

// 辅助：构建并发布批量 MQTT 上报（多通道）
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

static uint8_t countEnabledPollTasks(const ModbusConfig& config) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < config.master.taskCount; i++) {
        if (config.master.tasks[i].enabled) count++;
    }
    return count;
}

static uint16_t getMinEnabledPollInterval(const ModbusConfig& config) {
    uint16_t minInterval = 0;
    for (uint8_t i = 0; i < config.master.taskCount; i++) {
        const PollTask& task = config.master.tasks[i];
        if (!task.enabled) continue;
        if (minInterval == 0 || task.pollInterval < minInterval) {
            minInterval = task.pollInterval;
        }
    }
    return minInterval;
}

static uint16_t getMaxEnabledPollInterval(const ModbusConfig& config) {
    uint16_t maxInterval = 0;
    for (uint8_t i = 0; i < config.master.taskCount; i++) {
        const PollTask& task = config.master.tasks[i];
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

static const char* buildModbusRiskLevel(const ModbusConfig& config,
                                        uint8_t enabledTaskCount,
                                        uint16_t minPollInterval,
                                        uint32_t totalPolls,
                                        float timeoutRate,
                                        float failureRate,
                                        uint32_t lastPollAgeSec) {
    if (enabledTaskCount >= 4 && minPollInterval > 0 && minPollInterval < 5) return "high";
    if (timeoutRate >= 20.0f && totalPolls >= 10) return "high";
    if (config.master.responseTimeout > 3000 && config.master.maxRetries > 2 && enabledTaskCount >= 3) return "high";

    if (enabledTaskCount >= 4) return "medium";
    if (minPollInterval > 0 && minPollInterval < 5) return "medium";
    if (config.master.interPollDelay < 100) return "medium";
    if (failureRate >= 10.0f && totalPolls >= 10) return "medium";
    if (enabledTaskCount > 0 && lastPollAgeSec > 0) {
        uint32_t staleThreshold = static_cast<uint32_t>(minPollInterval > 0 ? minPollInterval : 5) * 3UL;
        if (staleThreshold < 30UL) staleThreshold = 30UL;
        if (lastPollAgeSec > staleThreshold) return "medium";
    }
    return "low";
}

// ============================================================================
// 构造函数
// ============================================================================

ModbusRouteHandler::ModbusRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

// ============================================================================
// 路由注册
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

    // Modbus 通用控制 API（线圈、寄存器读写，设备参数）
    server->on("/api/modbus/coil/control", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusCoilControl(request);
    });

    server->on("/api/modbus/coil/batch", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusCoilBatch(request);
    });

    server->on("/api/modbus/coil/delay", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusCoilDelay(request);
    });

    server->on("/api/modbus/coil/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleModbusCoilStatus(request);
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

    // Modbus 通用寄存器读写 API
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
}

// ============================================================================
// GET /api/modbus/status — 获取 Modbus 状态
// ============================================================================

void ModbusRouteHandler::handleGetModbusStatus(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    // 缓存命中：直接返回上次序列化的 JSON
    unsigned long now = millis();
    if (_statusCache.valid && (now - _statusCache.timestamp) < STATUS_CACHE_TTL) {
        request->send(200, "application/json", _statusCache.json);
        return;
    }

    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return;
    }

    ModbusHandler* modbus = pm->getModbusHandler();

    // 内存检查：Modbus 状态响应可能包含大量任务数据
    if (HandlerUtils::checkLowMemory(request)) return;

    JsonDocument doc;
    doc["success"] = true;

    JsonObject data = doc["data"].to<JsonObject>();
    JsonObject health = data["health"].to<JsonObject>();
    JsonArray warnings = health["warnings"].to<JsonArray>();

    if (!modbus) {
        // Modbus 未初始化时返回空状态而非 404
        data["mode"] = "master";
        data["workMode"] = 1;
        data["status"] = "stopped";
        data["taskCount"] = 0;
        // 初始化统计字段为0
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
            ModbusConfig config = modbus->getConfig();
            uint8_t enabledTaskCount = countEnabledPollTasks(config);
            uint16_t minPollInterval = getMinEnabledPollInterval(config);
            uint16_t maxPollInterval = getMaxEnabledPollInterval(config);
            uint32_t totalPolls = modbus->getTotalPollCount();
            uint32_t successPolls = modbus->getSuccessPollCount();
            uint32_t failedPolls = modbus->getFailedPollCount();
            uint32_t timeoutPolls = modbus->getTimeoutPollCount();
            uint32_t lastPollAgeSec = modbus->getLastPollAgeSec();
            float successRate = calcRate(successPolls, totalPolls);
            float failureRate = calcRate(failedPolls + timeoutPolls, totalPolls);
            float timeoutRate = calcRate(timeoutPolls, totalPolls);

            data["taskCount"] = modbus->getPollTaskCount();

            // 添加轮询统计数据
            data["totalPolls"] = totalPolls;
            data["successPolls"] = successPolls;
            data["failedPolls"] = failedPolls;
            data["timeoutPolls"] = timeoutPolls;

            health["riskLevel"] = buildModbusRiskLevel(config, enabledTaskCount, minPollInterval,
                                                       totalPolls, timeoutRate, failureRate, lastPollAgeSec);
            health["enabledTaskCount"] = enabledTaskCount;
            health["minPollInterval"] = minPollInterval;
            health["maxPollInterval"] = maxPollInterval;
            health["responseTimeout"] = config.master.responseTimeout;
            health["maxRetries"] = config.master.maxRetries;
            health["interPollDelay"] = config.master.interPollDelay;
            health["lastPollAgeSec"] = lastPollAgeSec;
            health["successRate"] = successRate;
            health["failureRate"] = failureRate;
            health["timeoutRate"] = timeoutRate;

            if (enabledTaskCount >= 4) {
                warnings.add("启用中的轮询任务较多，建议关注总线负载和 Web 响应。");
            }
            if (minPollInterval > 0 && minPollInterval < 5) {
                warnings.add("存在小于 5 秒的轮询任务，容易放大总线抖动和页面卡顿。");
            }
            if (config.master.responseTimeout > 3000) {
                warnings.add("Modbus 主站响应超时偏大，异常从站会更长时间占用串口。");
            }
            if (config.master.maxRetries > 2) {
                warnings.add("Modbus 主站重试次数偏高，失败设备会拖长单轮轮询耗时。");
            }
            if (config.master.interPollDelay < 100) {
                warnings.add("轮询间隔保护过小，连续请求可能压缩 Web 服务响应时间。");
            }
            if (totalPolls >= 10 && timeoutRate >= 20.0f) {
                warnings.add("近期超时比例偏高，请检查从站稳定性、波特率和轮询频率。");
            } else if (totalPolls >= 10 && failureRate >= 10.0f) {
                warnings.add("近期轮询失败偏多，建议检查寄存器配置和设备在线状态。");
            }
            if (enabledTaskCount > 0 && lastPollAgeSec > 0) {
                uint32_t staleThreshold = static_cast<uint32_t>(minPollInterval > 0 ? minPollInterval : 5) * 3UL;
                if (staleThreshold < 30UL) staleThreshold = 30UL;
                if (lastPollAgeSec > staleThreshold) {
                    warnings.add("轮询结果长时间未刷新，可能存在总线阻塞或任务停滞。");
                }
            }

            JsonArray tasks = data["tasks"].to<JsonArray>();
            for (uint8_t i = 0; i < modbus->getPollTaskCount(); i++) {
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

                // 寄存器映射配置 - 按 regOffset 排序后输出，确保显示顺序正确
                if (task.mappingCount > 0) {
                    // 创建索引数组用于排序
                    uint8_t sortedIdx[Protocols::MODBUS_MAX_MAPPINGS_PER_TASK];
                    for (uint8_t j = 0; j < task.mappingCount; j++) {
                        sortedIdx[j] = j;
                    }
                    // 简单冒泡排序按 regOffset 升序排列
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

                // 添加缓存数据（最新采集的值）
                if (cache && cache->valid) {
                    JsonObject cachedData = t["cachedData"].to<JsonObject>();
                    cachedData["timestamp"] = cache->timestamp;
                    cachedData["ageSec"] = cacheAgeSec;
                    cachedData["lastError"] = cache->lastError;
                    JsonArray values = cachedData["values"].to<JsonArray>();
                    // 防御性检查：确保 count 不超过数组边界
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

    health["warningCount"] = warnings.size();

    serializeJson(doc, _statusCache.json);
    _statusCache.timestamp = millis();
    _statusCache.valid = true;
    request->send(200, "application/json", _statusCache.json);
}

// ============================================================================
// POST /api/modbus/write — 写单个保持寄存器
// ============================================================================

void ModbusRouteHandler::handleModbusWrite(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return;
    }

    ModbusHandler* modbus = pm->getModbusHandler();
    if (!modbus) {
        ctx->sendError(request, 503, "Modbus not started");
        return;
    }

    if (modbus->getMode() != MODBUS_MASTER) {
        ctx->sendError(request, 400, "Modbus is not in Master mode");
        return;
    }

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
// POST /api/modbus/coil/control — 单个线圈控制（on/off/toggle）
// mode=coil: FC05 写线圈  mode=register: FC06 写保持寄存器（0x0001/0x0000）
// ============================================================================

void ModbusRouteHandler::handleModbusCoilControl(AsyncWebServerRequest* request) {
    static uint32_t _coilCtrlSeq = 0;
    uint32_t reqSeq = ++_coilCtrlSeq;
    
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t channel = (uint16_t)ctx->getParamInt(request, "channel", 0);
    uint16_t coilBase = (uint16_t)ctx->getParamInt(request, "coilBase", 0);
    String action = ctx->getParamValue(request, "action", "toggle");
    String mode = ctx->getParamValue(request, "mode", "coil");
    
    LOG_WARNINGF("[CoilCtrl#%u] HTTP entry: slave=%d ch=%d action=%s mode=%s clientIP=%s",
                 reqSeq, slaveAddr, channel, action.c_str(), mode.c_str(),
                 request->client() ? request->client()->remoteIP().toString().c_str() : "?");
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    
    uint16_t coilAddr = coilBase + channel;
    OneShotResult result;
    bool newState = false;  // 用于记录操作后的状态
    
    if (mode == "register") {
        // 寄存器模式：FC06 写保持寄存器
        // 写入成功后状态直接确定，无需再读取确认
        if (action == "on") {
            result = modbus->writeRegisterOnce(slaveAddr, coilAddr, 1, true);  // isControl=true
            newState = true;
        } else if (action == "off") {
            result = modbus->writeRegisterOnce(slaveAddr, coilAddr, 0, true);  // isControl=true
            newState = false;
        } else if (action == "toggle") {
            // 先读当前值再反转
            OneShotResult readR = modbus->readRegistersOnce(slaveAddr, 0x03, coilAddr, 1, true);  // isControl=true
            if (readR.error != ONESHOT_SUCCESS) {
                sendOneShotError(ctx, request, readR);
                return;
            }
            uint16_t newVal = (readR.data[0] != 0) ? 0 : 1;
            result = modbus->writeRegisterOnce(slaveAddr, coilAddr, newVal, true);  // isControl=true
            newState = (newVal != 0);  // 写入值即为新状态
        } else {
            ctx->sendBadRequest(request, "Invalid action (on/off/toggle)");
            return;
        }
    } else {
        // 线圈模式：FC05 写线圈
        // 写入成功后直接推导状态，无需额外读取确认
        if (action == "on") {
            result = modbus->writeCoilOnce(slaveAddr, coilAddr, true, true);  // isControl=true
            if (result.error == ONESHOT_SUCCESS) newState = true;
        } else if (action == "off") {
            result = modbus->writeCoilOnce(slaveAddr, coilAddr, false, true);  // isControl=true
            if (result.error == ONESHOT_SUCCESS) newState = false;
        } else if (action == "toggle") {
            // toggle 无法从写入值推导结果，需要先读取当前状态
            OneShotResult readR = modbus->readRegistersOnce(slaveAddr, 0x01, coilBase, 8, true);  // isControl=true
            if (readR.error != ONESHOT_SUCCESS) {
                sendOneShotError(ctx, request, readR);
                return;
            }
            bool currentState = (readR.data[channel] != 0);
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
    
    // MQTT 上报控制结果（ncMode 设备报告用户意图值，非原始线圈电平）
    // 否则平台回写该值时 processDataCommandMatch 会再次应用 ncMode 反转，
    // 导致双重反转使设备恢复原状
    int devIdx = findDeviceIndexBySlaveAddr(modbus, slaveAddr);
    if (devIdx >= 0) {
        ModbusConfig cfg = modbus->getConfig();
        bool reportState = newState;
        if (devIdx < (int)cfg.master.deviceCount && cfg.master.devices[devIdx].ncMode) {
            reportState = !newState;
        }
        publishControlReport(ctx, modbus, devIdx, channel, String(reportState ? 1 : 0));
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
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// POST /api/modbus/coil/batch — 批量线圈控制（allOn/allOff/allToggle）
// mode=coil: FC05 写线圈  mode=register: FC06 写保持寄存器
// ============================================================================

void ModbusRouteHandler::handleModbusCoilBatch(AsyncWebServerRequest* request) {
    static uint32_t _coilBatchSeq = 0;
    uint32_t reqSeq = ++_coilBatchSeq;
    
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t channelCount = (uint16_t)ctx->getParamInt(request, "channelCount", 8);
    uint16_t coilBase = (uint16_t)ctx->getParamInt(request, "coilBase", 0);
    String action = ctx->getParamValue(request, "action", "allOff");
    String mode = ctx->getParamValue(request, "mode", "coil");
    
    LOG_WARNINGF("[CoilBatch#%u] HTTP entry: slave=%d count=%d action=%s mode=%s clientIP=%s",
                 reqSeq, slaveAddr, channelCount, action.c_str(), mode.c_str(),
                 request->client() ? request->client()->remoteIP().toString().c_str() : "?");
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    if (channelCount == 0 || channelCount > Protocols::MODBUS_MAX_WRITE_COILS) {
        ctx->sendBadRequest(request, "Invalid channel count (1-32)");
        return;
    }
    
    OneShotResult result;
    
    // 查找设备配置中的 batchRegister
    uint16_t batchReg = 0;
    {
        ModbusConfig cfg = modbus->getConfig();
        for (uint8_t i = 0; i < cfg.master.deviceCount; i++) {
            if (cfg.master.devices[i].slaveAddress == slaveAddr) {
                batchReg = cfg.master.devices[i].batchRegister;
                break;
            }
        }
    }

    if (mode == "register") {
        if (batchReg > 0) {
            // 位图寄存器模式：单次 FC06 写位图寄存器
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
        } else {
            // 逐通道 FC06 模式
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
        // 线圈模式：FC15 批量写
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
    
    // 操作后读取实际状态
    OneShotResult readResult;
    bool useBitmapRead = (mode == "register" && batchReg > 0);
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
    bool stateValues[32]; // 用于 MQTT 上报
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

    // MQTT 上报批量控制结果（ncMode 设备需要反转为用户意图值）
    int devIdx = findDeviceIndexBySlaveAddr(modbus, slaveAddr);
    if (devIdx >= 0) {
        ModbusConfig cfg = modbus->getConfig();
        bool isNcMode = (devIdx < (int)cfg.master.deviceCount && cfg.master.devices[devIdx].ncMode);
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

    addModbusDebug(doc, modbus);
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// POST /api/modbus/coil/delay — 线圈延时控制
// NO模式：硬件闪开（设备硬件定时）；NC模式：软件延时任务（反转逻辑）
// ============================================================================

void ModbusRouteHandler::handleModbusCoilDelay(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t channel = (uint16_t)ctx->getParamInt(request, "channel", 0);
    uint16_t delayBase = (uint16_t)ctx->getParamInt(request, "delayBase", 0x0200);
    uint16_t delayUnits = (uint16_t)ctx->getParamInt(request, "delayUnits", 50);
    bool ncMode = ctx->getParamBool(request, "ncMode", false);
    uint16_t coilBase = (uint16_t)ctx->getParamInt(request, "coilBase", 0);
    String mode = ctx->getParamValue(request, "mode", "coil");
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    if (delayUnits == 0 || delayUnits > 255) {
        ctx->sendBadRequest(request, "Invalid delay (1-255 x100ms, max 25.5s)");
        return;
    }
    
    unsigned long delayMs = (unsigned long)delayUnits * 100;
    uint16_t coilAddr = coilBase + channel;

    if (mode == "register") {
        // 寄存器模式：使用 FC 0x06 写寄存器
        if (ncMode) {
            // NC 常闭模式：先写 0（断开），延时后写 1（恢复）
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
            // NO 常开模式：先写 1（吸合），延时后写 0（断开）
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
        
        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    } else {
        // 线圈模式：使用 FC 0x05 写线圈
        // 步骤1：立即打开继电器（写线圈 ON）
        // NO模式: 线圈ON=设备通电; NC模式: 线圈ON=NC断开=设备断电（但我们要先打开，所以NC这里写OFF）
        // 修正：延时断开 = 先打开设备，延时后关闭
        // NO模式: 先写ON(设备通) → 延时后自动OFF(设备断)
        // NC模式: 先写OFF(NC闭合=设备通) → 延时后写ON(NC断开=设备断)
        bool initialValue = ncMode ? false : true;  // NO写ON, NC写OFF
        OneShotResult result = modbus->writeCoilOnce(slaveAddr, coilAddr, initialValue, true);  // isControl=true
        if (result.error != ONESHOT_SUCCESS) {
            sendOneShotError(ctx, request, result);
            return;
        }

        // 步骤2：启动延时机制，延时后自动关闭
        if (ncMode) {
            // NC 常闭模式：使用软件延时任务
            // 延时后写线圈ON (NC断开=设备断电)
            bool ok = modbus->addCoilDelayTask(slaveAddr, coilAddr, delayMs, true);
            if (!ok) {
                ctx->sendError(request, 500, "Delay task queue full");
                return;
            }
        } else {
            // NO 常开模式：使用硬件闪开指令
            // 闪开指令：FC 0x05 地址=(delayBase + channel) 值=延时单位在高字节
            // 硬件会自动：延时后关闭继电器
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
        
        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    }
}

// ============================================================================
// GET /api/modbus/coil/status — 读取线圈状态
// mode=coil: FC01 读线圈  mode=register: FC03 读保持寄存器（每通道一个寄存器）
// ============================================================================

void ModbusRouteHandler::handleModbusCoilStatus(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.view") &&
        !ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t channelCount = (uint16_t)ctx->getParamInt(request, "channelCount", 8);
    uint16_t coilBase = (uint16_t)ctx->getParamInt(request, "coilBase", 0);
    String mode = ctx->getParamValue(request, "mode", "coil");
    
    // 验证 mode 参数
    if (mode != "coil" && mode != "register") {
        ctx->sendBadRequest(request, "Invalid mode (must be 'coil' or 'register')");
        return;
    }
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    if (channelCount == 0 || channelCount > Protocols::MODBUS_MAX_WRITE_COILS) {
        ctx->sendBadRequest(request, "Invalid channel count (1-32)");
        return;
    }
    
    OneShotResult result;
    if (mode == "register") {
        // 寄存器模式：FC03 读保持寄存器，每通道一个寄存器，值 !=0 为 ON
        result = modbus->readRegistersOnce(slaveAddr, 0x03, coilBase, channelCount, true);  // isControl=true
    } else {
        // 线圈模式：FC01 读线圈，向上取整到 8 的倍数
        uint16_t readQty = ((channelCount + 7) / 8) * 8;
        if (readQty < 8) readQty = 8;
        result = modbus->readRegistersOnce(slaveAddr, 0x01, coilBase, readQty, true);  // isControl=true
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
    // 防御性检查：确保不超出 result.count 边界
    for (uint16_t i = 0; i < channelCount; i++) {
        if (i < result.count) {
            states.add(result.data[i] != 0);
        } else {
            states.add(false);  // 超出范围的通道默认为 off
        }
    }
    addModbusDebug(doc, modbus);
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// POST /api/modbus/device/address — 读取/设置从站地址（FC 0x03 / FC 0x06）
// ============================================================================

void ModbusRouteHandler::handleModbusDeviceAddress(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t addrRegister = (uint16_t)ctx->getParamInt(request, "addressRegister", 0x0000);
    String newAddrStr = ctx->getParamValue(request, "newAddress", "");
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    
    if (newAddrStr.isEmpty()) {
        // 读取当前地址：使用广播地址 0x00（文档确认：00 03 00 00 00 01 85 DB）
        // 设备会以实际地址响应（如 03 03 02 00 03 ...）
        // sendOneShotRequest 中 expectedSlaveAddr==0 时已跳过地址匹配
        OneShotResult result = modbus->readRegistersOnce(0x00, 0x03, addrRegister, 1, true);  // isControl=true
        if (result.error != ONESHOT_SUCCESS) {
            sendOneShotError(ctx, request, result);
            return;
        }
        data["currentAddress"] = result.data[0];
        data["register"] = addrRegister;
    } else {
        // 设置新地址：使用广播地址 0x00，FC 0x10 写多个寄存器
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
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// POST /api/modbus/device/baudrate — 设置波特率（FC 0x06）
// ============================================================================

void ModbusRouteHandler::handleModbusDeviceBaudrate(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint32_t baudRate = (uint32_t)ctx->getParamInt(request, "baudRate", 9600);
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    
    // 波特率映射（继电器板 FC 0xB0 标准编码）
    uint8_t baudCode;
    switch (baudRate) {
        case 1200:   baudCode = 0; break;
        case 2400:   baudCode = 1; break;
        case 4800:   baudCode = 2; break;
        case 9600:   baudCode = 3; break;
        case 19200:  baudCode = 4; break;
        case 115200: baudCode = 5; break;
        default:     baudCode = 3; break; // 默认 9600
    }
    
    // 允许用户通过 baudCode 参数直接指定编码值
    String baudCodeStr = ctx->getParamValue(request, "baudCode", "");
    if (!baudCodeStr.isEmpty()) {
        baudCode = (uint8_t)baudCodeStr.toInt();
    }
    
    // 发送专有 FC 0xB0 波特率设置帧：[slaveAddr, 0xB0, 0x00, 0x00, baudCode, 0x00]
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
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// GET /api/modbus/device/inputs — 读取离散输入（FC 0x02）
// ============================================================================

void ModbusRouteHandler::handleModbusDiscreteInputs(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.view") &&
        !ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t inputCount = (uint16_t)ctx->getParamInt(request, "inputCount", 4);
    uint16_t inputBase = (uint16_t)ctx->getParamInt(request, "inputBase", 0);
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    if (inputCount == 0 || inputCount > Protocols::MODBUS_MAX_WRITE_COILS) {
        ctx->sendBadRequest(request, "Invalid input count (1-32)");
        return;
    }
    
    // 同 FC 0x01，使用 qty 向上取整到 8 的倍数确保标准格式
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
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// GET /api/modbus/register/read — 读取保持/输入寄存器（FC 0x03 / FC 0x04）
// ============================================================================

void ModbusRouteHandler::handleModbusRegisterRead(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.view") &&
        !ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t startAddr = (uint16_t)ctx->getParamInt(request, "startAddress", 0);
    uint16_t quantity = (uint16_t)ctx->getParamInt(request, "quantity", 1);
    uint8_t fc = (uint8_t)ctx->getParamInt(request, "functionCode", 3);
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    if (quantity == 0 || quantity > Protocols::MODBUS_MAX_REGISTERS_PER_READ) {
        ctx->sendBadRequest(request, "Invalid quantity (1-125)");
        return;
    }
    if (fc != 0x03 && fc != 0x04) {
        ctx->sendBadRequest(request, "Invalid function code (3 or 4)");
        return;
    }
    
    OneShotResult result = modbus->readRegistersOnce(slaveAddr, fc, startAddr, quantity, true);  // isControl=true
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
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
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// POST /api/modbus/register/write — 写单个保持寄存器（FC 0x06）
// ============================================================================

void ModbusRouteHandler::handleModbusRegisterWrite(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t regAddr = (uint16_t)ctx->getParamInt(request, "registerAddress", 0);
    uint16_t value = (uint16_t)ctx->getParamInt(request, "value", 0);
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    
    OneShotResult result = modbus->writeRegisterOnce(slaveAddr, regAddr, value, true);  // isControl=true
    if (result.error != ONESHOT_SUCCESS) {
        sendOneShotError(ctx, request, result);
        return;
    }
    
    // MQTT 上报寄存器写入结果
    int devIdx = findDeviceIndexBySlaveAddr(modbus, slaveAddr);
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
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// POST /api/modbus/register/batch-write — 批量写保持寄存器（FC 0x10）
// ============================================================================

void ModbusRouteHandler::handleModbusRegisterBatchWrite(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }
    
    ModbusHandler* modbus = getModbusMaster(ctx, request);
    if (!modbus) return;
    
    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t startAddr = (uint16_t)ctx->getParamInt(request, "startAddress", 0);
    String valuesStr = ctx->getParamValue(request, "values", "[]");
    
    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }
    
    // 解析 JSON 数组
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
    
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}
