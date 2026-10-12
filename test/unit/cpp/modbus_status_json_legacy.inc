// Frozen pre-refactor Modbus serializer methods; source SHA256

// FE6B53F37547BA0149EECFFFBD4E92B524514FF9E27E7BED043457A99DC2D267.

// Business method text preserved verbatim except function/class names below.

static void legacyAddModbusReadDiagnostics(JsonDocument& doc, const ModbusReadDiagnostics& trace) {
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

static void legacyAddModbusDebug(JsonDocument& doc, ModbusHandler* modbus) {
    String tx, rx;
    if (!modbus->getDebugSnapshot(tx, rx)) return;
    JsonObject debug = doc["debug"].to<JsonObject>();
    debug["tx"] = tx;
    debug["rx"] = rx;
}

void LegacyModbusRouteHandler::handleGetModbusStatus(AsyncWebServerRequest* request) {
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
    if (modbus) modbus->getCoilDelayStatus(data["delayedWrites"].to<JsonObject>());
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
                        mo["wordOrder"] = m.wordOrder;
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
