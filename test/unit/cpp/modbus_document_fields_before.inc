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
    FastBee::setFixedJsonField(data, "compact", compactStatus);
    if (modbus) modbus->getCoilDelayStatus(data["delayedWrites"].to<JsonObject>());
    JsonObject health = data["health"].to<JsonObject>();
    JsonArray warnings = health["warnings"].to<JsonArray>();

    if (!modbus) {
        // Modbus 鏈垵濮嬪寲鏃惰繑鍥炵┖鐘舵€佽€岄潪 404
        FastBee::setFixedJsonField(data, "mode", "master");
        FastBee::setFixedJsonField(data, "workMode", 1);
        FastBee::setFixedJsonField(data, "status", "stopped");
        FastBee::setFixedJsonField(data, "taskCount", 0);
        // 鍒濆鍖栫粺璁″瓧娈典负0
        FastBee::setFixedJsonField(data, "totalPolls", 0);
        FastBee::setFixedJsonField(data, "successPolls", 0);
        FastBee::setFixedJsonField(data, "failedPolls", 0);
        FastBee::setFixedJsonField(data, "timeoutPolls", 0);
        data["tasks"].to<JsonArray>();
        FastBee::setFixedJsonField(health, "riskLevel", "low");
        FastBee::setFixedJsonField(health, "enabledTaskCount", 0);
        FastBee::setFixedJsonField(health, "minPollInterval", 0);
        FastBee::setFixedJsonField(health, "maxPollInterval", 0);
        FastBee::setFixedJsonField(health, "responseTimeout", 0);
        FastBee::setFixedJsonField(health, "maxRetries", 0);
        FastBee::setFixedJsonField(health, "interPollDelay", 0);
        FastBee::setFixedJsonField(health, "lastPollAgeSec", 0);
        FastBee::setFixedJsonField(health, "successRate", 0.0);
        FastBee::setFixedJsonField(health, "failureRate", 0.0);
        FastBee::setFixedJsonField(health, "timeoutRate", 0.0);
    } else {
        FastBee::setFixedJsonField(data, "mode", (modbus->getMode() == MODBUS_MASTER) ? "master" : "slave");
        FastBee::setFixedJsonField(data, "workMode", modbus->getWorkMode());
        FastBee::setFixedJsonField(data, "status", modbus->getStatus());

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

            FastBee::setFixedJsonField(data, "taskCount", modbus->getPollTaskCount());

            // 娣诲姞杞缁熻鏁版嵁
            FastBee::setFixedJsonField(data, "totalPolls", totalPolls);
            FastBee::setFixedJsonField(data, "successPolls", successPolls);
            FastBee::setFixedJsonField(data, "failedPolls", failedPolls);
            FastBee::setFixedJsonField(data, "timeoutPolls", timeoutPolls);

            FastBee::setFixedJsonField(health, "riskLevel", buildModbusRiskLevel(modbus, enabledTaskCount, minPollInterval,
                                                       totalPolls, timeoutRate, failureRate, lastPollAgeSec));
            FastBee::setFixedJsonField(health, "enabledTaskCount", enabledTaskCount);
            FastBee::setFixedJsonField(health, "minPollInterval", minPollInterval);
            FastBee::setFixedJsonField(health, "maxPollInterval", maxPollInterval);
            FastBee::setFixedJsonField(health, "responseTimeout", modbus->getResponseTimeout());
            FastBee::setFixedJsonField(health, "maxRetries", modbus->getMaxRetries());
            FastBee::setFixedJsonField(health, "interPollDelay", modbus->getInterPollDelay());
            FastBee::setFixedJsonField(health, "lastPollAgeSec", lastPollAgeSec);
            FastBee::setFixedJsonField(health, "successRate", successRate);
            FastBee::setFixedJsonField(health, "failureRate", failureRate);
            FastBee::setFixedJsonField(health, "timeoutRate", timeoutRate);

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
                FastBee::setFixedJsonField(t, "slaveAddress", task.slaveAddress);
                FastBee::setFixedJsonField(t, "functionCode", task.functionCode);
                FastBee::setFixedJsonField(t, "startAddress", task.startAddress);
                FastBee::setFixedJsonField(t, "quantity", task.quantity);
                FastBee::setFixedJsonField(t, "pollInterval", task.pollInterval);
                FastBee::setFixedJsonField(t, "enabled", task.enabled);
                FastBee::setFixedJsonField(t, "name", String(task.name));
                FastBee::setFixedJsonField(t, "status", getTaskHealthStatus(task, cache));
                FastBee::setFixedJsonField(t, "cacheAgeSec", cacheAgeSec);
                FastBee::setFixedJsonField(t, "staleAfterSec", getTaskStaleThresholdSec(task));
                FastBee::setFixedJsonField(t, "lastError", cache ? cache->lastError : 0);

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
                        FastBee::setFixedJsonField(mo, "regOffset", m.regOffset);
                        FastBee::setFixedJsonField(mo, "dataType", m.dataType);
                        FastBee::setFixedJsonField(mo, "wordOrder", m.wordOrder);
                        FastBee::setFixedJsonField(mo, "scaleFactor", m.scaleFactor);
                        FastBee::setFixedJsonField(mo, "decimalPlaces", m.decimalPlaces);
                        FastBee::setFixedJsonField(mo, "sensorId", String(m.sensorId));
                        if (m.unit[0] != '\0') FastBee::setFixedJsonField(mo, "unit", String(m.unit));
                    }
                }

                // 娣诲姞缂撳瓨鏁版嵁锛堟渶鏂伴噰闆嗙殑鍊硷級
                if (cache && cache->valid) {
                    JsonObject cachedData = t["cachedData"].to<JsonObject>();
                    FastBee::setFixedJsonField(cachedData, "timestamp", cache->timestamp);
                    FastBee::setFixedJsonField(cachedData, "ageSec", cacheAgeSec);
                    FastBee::setFixedJsonField(cachedData, "lastError", cache->lastError);
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
            FastBee::setFixedJsonField(health, "riskLevel", "low");
            FastBee::setFixedJsonField(health, "enabledTaskCount", 0);
            FastBee::setFixedJsonField(health, "minPollInterval", 0);
            FastBee::setFixedJsonField(health, "maxPollInterval", 0);
            FastBee::setFixedJsonField(health, "responseTimeout", 0);
            FastBee::setFixedJsonField(health, "maxRetries", 0);
            FastBee::setFixedJsonField(health, "interPollDelay", 0);
            FastBee::setFixedJsonField(health, "lastPollAgeSec", 0);
            FastBee::setFixedJsonField(health, "successRate", 0.0);
            FastBee::setFixedJsonField(health, "failureRate", 0.0);
            FastBee::setFixedJsonField(health, "timeoutRate", 0.0);
        }
    }

    if (request->hasParam("hardware") && request->getParam("hardware")->value() == "1") {
        JsonObject hardware = data["uartHardware"].to<JsonObject>();
        if (modbus) modbus->appendUartHardwareSnapshot(hardware);
        else { FastBee::setFixedJsonField(hardware, "supported", false); FastBee::setFixedJsonField(hardware, "error", "MODBUS_NOT_RUNNING"); }
    }

    FastBee::setFixedJsonField(health, "warningCount", warnings.size());

    _statusCache.valid = false;
    HandlerUtils::sendJsonStream(request, doc);
}

void ModbusRouteHandler::handleModbusWrite(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ModbusHttp::fields(*ctx, request, {"slaveAddress", "regAddress", "value"})) return;
    uint32_t slave, address, value;
    if (!ModbusHttp::number(*ctx, request, "slaveAddress", 1, 1, 247, slave) ||
        !ModbusHttp::number(*ctx, request, "regAddress", 0, 0, 65535, address) ||
        !ModbusHttp::number(*ctx, request, "value", 0, 0, 65535, value)) return;
    auto lease = getModbusMaster(ctx, request);
    auto* modbus = lease.get();
    if (!modbus || rejectModbusCriticalControl(request, "Modbus register write", 6144)) return;
    FastBee::ModbusOutputFrame frame;
    if (!frame.single(slave, 6, address, value)) { ctx->sendBadRequest(request, "Invalid write frame"); return; }
    ModbusHttp::Output output(*ctx, request, *modbus);
    if (!output.begin()) return;
    const auto result = output.submit(frame);
    if (result.error != ONESHOT_SUCCESS) { output.fail(result); return; }
    JsonDocument doc; doc["success"] = true;
    doc["message"] = "Write command acknowledged";
    auto data = doc["data"].to<JsonObject>();
    data["register"] = address; data["value"] = value;
    output.append(data); addModbusDebug(doc, modbus);
    HandlerUtils::sendJsonStream(request, doc);
}

void ModbusRouteHandler::handleModbusRegisterWrite(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ModbusHttp::fields(*ctx, request, {"slaveAddress", "registerAddress", "value"})) return;
    uint32_t slave, address, value;
    if (!ModbusHttp::number(*ctx, request, "slaveAddress", 1, 1, 247, slave) ||
        !ModbusHttp::number(*ctx, request, "registerAddress", 0, 0, 65535, address) ||
        !ModbusHttp::number(*ctx, request, "value", 0, 0, 65535, value)) return;
    auto lease = getModbusMaster(ctx, request);
    auto* modbus = lease.get();
    if (!modbus || rejectModbusCriticalControl(request, "Modbus register write", 6144)) return;
    FastBee::ModbusOutputFrame frame;
    if (!frame.single(slave, 6, address, value)) { ctx->sendBadRequest(request, "Invalid write frame"); return; }
    ModbusSubDevice reportDevice;
    FastBee::ModbusControlTarget reportTarget;
    const bool relayReport = value <= 1 && modbus->snapshotOrdinaryRelayEndpoint(
        slave, 6, address, reportDevice, reportTarget);
    ModbusHttp::Output output(*ctx, request, *modbus);
    if (!output.begin()) return;
    const auto result = output.submit(frame);
    if (result.error != ONESHOT_SUCCESS) { output.fail(result); return; }
    // Only an exact captured channel gets a relay report. A generic register
    // write must never impersonate channel zero or a later configuration.
    if (relayReport && reportTarget.instance == output.binding().instance &&
        reportTarget.configuration == output.binding().configuration &&
        address >= reportTarget.first && address < uint32_t(reportTarget.first) + reportTarget.count) {
        publishFrozenModbusReport(ctx, reportDevice, reportTarget.index,
            static_cast<uint16_t>(address - reportTarget.first),
            String(reportDevice.ncMode ? !value : value), "modbus_command_acknowledged");
    }
    JsonDocument doc; doc["success"] = true;
    auto data = doc["data"].to<JsonObject>();
    data["register"] = address; data["value"] = value;
    output.append(data); addModbusDebug(doc, modbus);
    HandlerUtils::sendJsonStream(request, doc);
}

void ModbusRouteHandler::handleModbusRegisterBatchWrite(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ModbusHttp::fields(*ctx, request, {"slaveAddress", "startAddress", "values"})) return;
    uint32_t slave, address;
    if (!ModbusHttp::number(*ctx, request, "slaveAddress", 1, 1, 247, slave) ||
        !ModbusHttp::number(*ctx, request, "startAddress", 0, 0, 65535, address)) return;
    if (rejectModbusHeavyRequest(request, "Modbus batch register write", 8192)) return;
    const String encoded = ctx->getParamValue(request, "values", "[]");
    if (encoded.length() > 1024 || encoded.length() != std::strlen(encoded.c_str())) {
        ctx->sendBadRequest(request, "Invalid values"); return;
    }
    JsonDocument values;
    FastBee::ModbusOutputFrame frame;
    if (!ModbusHttp::registerArray(encoded, values) ||
        !ModbusHttp::registers(frame, slave, address, values.as<JsonArrayConst>())) {
        ctx->sendBadRequest(request, "Expected 1-123 integer values (0-65535) within address range"); return;
    }
    auto lease = getModbusMaster(ctx, request);
    auto* modbus = lease.get();
    if (!modbus) return;
    ModbusHttp::Output output(*ctx, request, *modbus);
    if (!output.begin()) return;
    const auto result = output.submit(frame);
    if (result.error != ONESHOT_SUCCESS) { output.fail(result); return; }
    JsonDocument doc; doc["success"] = true;
    auto data = doc["data"].to<JsonObject>();
    data["startAddress"] = address; data["quantity"] = values.size();
    data["values"].set(values.as<JsonArrayConst>());
    output.append(data); addModbusDebug(doc, modbus);
    HandlerUtils::sendJsonStream(request, doc);
}

void ModbusRouteHandler::handleModbusDeviceAddress(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ModbusHttp::fields(*ctx, request, {"slaveAddress", "addressRegister", "newAddress"})) return;
    uint32_t slave, address;
    if (!ModbusHttp::number(*ctx, request, "slaveAddress", 1, 1, 247, slave) ||
        !ModbusHttp::number(*ctx, request, "addressRegister", 0, 0, 65535, address)) return;
    const String next = ctx->getParamValue(request, "newAddress", "");
    uint32_t newAddress = 0;
    if (!next.isEmpty() && !ModbusHttp::decimal(next, 1, 247, newAddress)) {
        ctx->sendBadRequest(request, "Invalid new address (1-247)"); return;
    }
    if (!next.isEmpty() && !ctx->requireDeveloperMode(request)) return;
    if (rejectModbusHeavyRequest(request, "Modbus device address", 8192)) return;
    auto lease = getModbusMaster(ctx, request);
    auto* modbus = lease.get();
    if (!modbus) return;
    JsonDocument doc; doc["success"] = true;
    auto data = doc["data"].to<JsonObject>();
    if (next.isEmpty()) {
        // Legacy discovery is read-only. The connected device responds with its
        // own slave address; no ordinary output permission is fabricated.
        const auto result = modbus->readRegistersOnce(0, 3, address, 1, true);
        if (result.error != ONESHOT_SUCCESS) { sendOneShotError(ctx, request, result); return; }
        data["currentAddress"] = result.data[0]; data["register"] = address;
    } else {
        uint8_t bytes[] = {0, 16, static_cast<uint8_t>(address >> 8),
            static_cast<uint8_t>(address), 0, 1, 2, 0, static_cast<uint8_t>(newAddress)};
        FastBee::ModbusOutputFrame frame;
        if (!frame.assign(bytes, sizeof(bytes), FastBee::ModbusFrameAccess::Maintenance)) {
            ctx->sendBadRequest(request, "Invalid address frame"); return;
        }
        ModbusHttp::Output output(*ctx, request, *modbus);
        if (!output.begin(true)) return;
        const auto result = output.submit(frame);
        if (result.error != ONESHOT_SUCCESS) { output.fail(result); return; }
        data["previousAddress"] = slave; data["newAddress"] = newAddress; data["register"] = address;
        data["scope"] = "busMaintenance"; data["configurationUpdated"] = false;
        output.append(data);
    }
    addModbusDebug(doc, modbus); HandlerUtils::sendJsonStream(request, doc);
}

void ModbusRouteHandler::handleModbusDeviceBaudrate(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request) || !ctx->requireDeveloperMode(request)) return;
    if (!ModbusHttp::fields(*ctx, request,
        {"slaveAddress", "baudRate", "mode", "baudRateReg", "baudCode"})) return;
    uint32_t slave, baudRate, mode, reg;
    if (!ModbusHttp::number(*ctx, request, "slaveAddress", 1, 1, 247, slave) ||
        !ModbusHttp::number(*ctx, request, "baudRate", 9600, 1, 115200, baudRate) ||
        !ModbusHttp::number(*ctx, request, "mode", 0, 0, 1, mode) ||
        !ModbusHttp::number(*ctx, request, "baudRateReg", 0x33, 0, 65535, reg)) return;
    if (rejectModbusHeavyRequest(request, "Modbus baudrate change", 8192)) return;
    auto lease = getModbusMaster(ctx, request);
    auto* modbus = lease.get();
    if (!modbus) return;
    ModbusSubDevice saved;
    FastBee::ModbusControlTarget savedTarget;
    const int index = findDeviceIndexBySlaveAddr(modbus, slave);
    const bool captured = index >= 0 && modbus->snapshotControlDevice(index, saved, savedTarget) &&
        saved.slaveAddress == slave;
    // Preserve the saved register-mode default, using a copied configuration.
    if (captured && mode == 0 && saved.baudRateMode == 1) {
        mode = 1; reg = saved.baudRateReg ? saved.baudRateReg : 0x33;
    }
    uint32_t code = 0;
    const auto* overrideCode = request->getParam("baudCode", true);
    if (overrideCode) {
        if (!ModbusHttp::decimal(overrideCode->value(), 0, mode ? 65535 : 255, code)) {
            ctx->sendBadRequest(request, "Invalid baud code"); return;
        }
    } else {
        bool known = true;
        if (mode) {
            switch (baudRate) {
                case 4800: code=0; break; case 9600: code=1; break;
                case 14400: code=2; break; case 19200: code=3; break;
                case 38400: code=4; break; case 56000: code=5; break;
                case 57600: code=6; break; case 115200: code=7; break;
                default: known=false; break;
            }
        } else {
            switch (baudRate) {
                case 1200: code=0; break; case 2400: code=1; break;
                case 4800: code=2; break; case 9600: code=3; break;
                case 19200: code=4; break; case 115200: code=5; break;
                default: known=false; break;
            }
        }
        if (!known) { ctx->sendBadRequest(request, "Baud rate has no code for selected mode"); return; }
    }
    FastBee::ModbusOutputFrame frame;
    const bool valid = mode
        ? frame.single(slave, 6, reg, code, FastBee::ModbusFrameAccess::Maintenance)
        : frame.single(slave, 0xB0, 0, code << 8, FastBee::ModbusFrameAccess::Maintenance);
    if (!valid) { ctx->sendBadRequest(request, "Invalid baud frame"); return; }
    FastBee::ModbusControlTarget current;
    if (!modbus->snapshotOrdinaryFrame(frame, current) ||
        (captured && (current.instance != savedTarget.instance || current.configuration != savedTarget.configuration))) {
        ctx->sendError(request, 409, "CONTROL_FRAME_MAPPING_UNCONFIRMED"); return;
    }
    ModbusHttp::Output output(*ctx, request, *modbus);
    output.expect(current);
    if (!output.begin(true)) return;
    const auto result = output.submit(frame);
    if (result.error != ONESHOT_SUCCESS) { output.fail(result); return; }
    JsonDocument doc; doc["success"] = true;
    auto data = doc["data"].to<JsonObject>();
    data["baudRate"] = baudRate; data["baudCode"] = code;
    data["mode"] = mode ? "register" : "proprietary";
    if (mode) data["register"] = reg;
    data["scope"] = "busMaintenance"; data["configurationUpdated"] = false;
    output.append(data); addModbusDebug(doc, modbus);
    HandlerUtils::sendJsonStream(request, doc);
}

void ModbusRouteHandler::handleModbusCoilControl(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ModbusHttp::fields(*ctx, request, {"slaveAddress", "channel", "coilBase", "action", "mode",
        "confirmOverride", "controlGeneration", "controlTargetId", "controlConfigEpoch"})) return;
    uint32_t slave, channel, base;
    if (!ModbusHttp::number(*ctx, request, "slaveAddress", 1, 1, 247, slave) ||
        !ModbusHttp::number(*ctx, request, "channel", 0, 0, Protocols::MODBUS_MAX_WRITE_COILS - 1, channel) ||
        !ModbusHttp::number(*ctx, request, "coilBase", 0, 0, 65535, base)) return;
    const String action = ctx->getParamValue(request, "action", "toggle");
    const String mode = ctx->getParamValue(request, "mode", "coil");
    if (!validRelayChannel(slave, channel, base) || (mode != "coil" && mode != "register") ||
        (action != "on" && action != "off" && action != "toggle")) {
        ctx->sendBadRequest(request, "Invalid relay endpoint, mode or action"); return;
    }
    auto modbusLease = getModbusMaster(ctx, request);
    ModbusHandler* modbus = modbusLease.get();
    if (!modbus) return;
    if (rejectModbusCriticalControl(request, "Modbus relay control", 6144)) return;
    const uint16_t address = static_cast<uint16_t>(base + channel);
    const uint8_t function = mode == "register" ? 6 : 5;
    FastBee::ModbusControlTarget target;
    ModbusSubDevice device;
    if (!modbus->snapshotOrdinaryRelayEndpoint(static_cast<uint8_t>(slave), function, address, device, target) ||
        function != target.function || address < target.first ||
        uint32_t(address) >= uint32_t(target.first) + target.count) {
        ctx->sendError(request, 409, "CONTROL_RELAY_MAPPING_UNCONFIRMED"); return;
    }
    char canonical[41]; target.key(canonical);
    FastBee::OrdinaryControlContext control;
    FastBee::OrdinaryControlResult result;
    const uint32_t epoch = FastBee::SensorConfigEpoch::current();
    if (!FastBee::beginHttpModbusRelayOutput(*ctx, request, String(canonical), epoch, control)) return;
    struct Terminal {
        const FastBee::OrdinaryControlContext& context;
        ~Terminal() { PeripheralManager::getInstance().finishOrdinaryControl(context); }
    } terminal{control};
    const bool toggle = action == "toggle";
    const uint16_t value = action == "on" ? (function == 5 ? 0xff00 : 1) : 0;
    const OneShotResult written = PeripheralManager::getInstance().submitOrdinaryModbusRelay(
        *modbus, target, function, address, value, control, result, 0, toggle);
    if (written.error != ONESHOT_SUCCESS) {
        JsonDocument denied;
        denied["success"] = false; denied["reason"] = result.reason; denied["configEpoch"] = epoch;
        FastBee::writeOrdinaryControlJson(denied["control"].to<JsonObject>(), result);
        ctx->sendJsonResponse(request, written.outputAttempted ? 502 : 409, denied); return;
    }
    // The exact echoed FC05/06 value confirms transport only. Use the frozen
    // mapping for reporting; neither current-index lookup nor ACK proves load state.
    const bool state = written.data[4] || written.data[5];
    const bool logical = device.ncMode ? !state : state;
    const uint16_t savedChannel = static_cast<uint16_t>(address - target.first);
    // The common captured-report helper uses the copied sensorId and count;
    // do not resolve a mutable device index again after the transaction.
    ActionExecResult binding;
    char peripheral[16]; snprintf(peripheral, sizeof(peripheral), "modbus:%u", target.index);
    FastBee::captureModbusReportIdentity(binding, peripheral, device.sensorId, device.channelCount, savedChannel);
    FastBee::completeCapturedReport(binding, String(logical ? 1 : 0), true, "modbus_command_acknowledged");
    auto mqtt = ctx->protocolManager ? ctx->protocolManager->acquireMQTTClient() : FastBee::MqttClientLease{};
    const String* reportId = FastBee::capturedReportId(binding);
    if (mqtt && mqtt->getIsConnected() && binding.reportIdentity == FastBee::ActionReportIdentity::Field && reportId) {
        JsonDocument report;
        JsonArray array = report.to<JsonArray>(); JsonObject item = array.add<JsonObject>();
        item["id"] = *reportId; item["value"] = String(logical ? 1 : 0);
        item["remark"] = "modbus_command_acknowledged"; item["physicalOutputVerified"] = false;
        String payload; serializeJson(report, payload); mqtt->publishReportData(payload);
    }
    char mcId[16], event[96];
    snprintf(mcId, sizeof(mcId), "mc:%u", target.index);
    snprintf(event, sizeof(event), "{\"d\":%u,\"c\":%u,\"a\":\"%s\"}",
        target.index, savedChannel, logical ? "on" : "off");
    PeriphExecManager::getInstance().triggerEventById(String(mcId), String(event));
    JsonDocument document;
    document["success"] = true;
    JsonObject data = document["data"].to<JsonObject>();
    data["channel"] = savedChannel; data["coilAddress"] = address; data["state"] = state;
    data["action"] = action; data["mode"] = mode; data["configEpoch"] = epoch;
    data["commandAcknowledged"] = true; data["physicalOutputVerified"] = false;
    FastBee::writeOrdinaryControlJson(data["control"].to<JsonObject>(), result);
    addModbusDebug(document, modbus);
    HandlerUtils::sendJsonStream(request, document);
}

void ModbusRouteHandler::handleModbusCoilBatch(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ModbusHttp::fields(*ctx, request, {"slaveAddress", "channelCount", "coilBase", "action", "mode"})) return;
    uint32_t slave, count, base;
    if (!ModbusHttp::number(*ctx, request, "slaveAddress", 1, 1, 247, slave) ||
        !ModbusHttp::number(*ctx, request, "channelCount", 8, 1, 32, count) ||
        !ModbusHttp::number(*ctx, request, "coilBase", 0, 0, 65535, base)) return;
    const String action = ctx->getParamValue(request, "action", "allOff");
    const String mode = ctx->getParamValue(request, "mode", "coil");
    if (base + count > 65536U || (mode != "coil" && mode != "register") ||
        (action != "allOn" && action != "allOff" && action != "allToggle")) {
        ctx->sendBadRequest(request, "Invalid batch action, mode or range"); return;
    }
    if (rejectModbusHeavyRequest(request, "Modbus batch control", 8192)) return;
    auto lease = getModbusMaster(ctx, request);
    auto* modbus = lease.get();
    if (!modbus) return;
    ModbusSubDevice device;
    FastBee::ModbusControlTarget target;
    const uint8_t function = mode == "register" ? 6 : 5;
    if (!modbus->snapshotOrdinaryRelayEndpoint(slave, function, base, device, target) ||
        target.first != base || target.function != function || count > target.count ||
        ((target.batch || (function == 5 && action == "allToggle")) && count != target.count)) {
        ctx->sendError(request, 409, "CONTROL_RELAY_MAPPING_UNCONFIRMED"); return;
    }
    ModbusHttp::Output output(*ctx, request, *modbus); output.expect(target);
    if (!output.begin()) return;
    bool commandKnown[32]{}, commandState[32]{};
    auto record = [&](uint16_t channel, bool state) {
        commandKnown[channel] = true; commandState[channel] = state;
    };
    FastBee::ModbusOutputFrame frame;
    OneShotResult written;
    const bool bitmap = function == 6 && target.batch && target.batchType == 0;
    if (bitmap || (function == 6 && target.batch && target.batchType == 1 && action != "allToggle")) {
        const uint16_t mask = bitmap ? (count == 16 ? 0xffff : static_cast<uint16_t>((1U << count) - 1U)) : 1;
        frame.single(slave, 6, target.batch, action == "allOff" ? 0 : mask);
        if (action == "allToggle") frame.transform = FastBee::ModbusFrameTransform::RelayBatchToggle;
        written = output.submit(frame);
        if (written.error != ONESHOT_SUCCESS) { output.fail(written); return; }
        const uint16_t echoed = static_cast<uint16_t>((written.data[4] << 8) | written.data[5]);
        for (uint16_t i = 0; i < count; ++i) record(i, bitmap ? ((echoed >> i) & 1U) : echoed != 0);
    } else if (function == 6) {
        for (uint16_t i = 0; i < count; ++i) {
            written = output.relay(target, 6, base + i, action == "allOn" ? 1 : 0, action == "allToggle");
            if (written.error != ONESHOT_SUCCESS) { output.fail(written); return; }
            record(i, written.data[4] || written.data[5]);
        }
    } else if (action == "allToggle") {
        frame.single(slave, 5, base, 0x5a00, FastBee::ModbusFrameAccess::RelayVendor);
        written = output.submit(frame);
        if (written.error != ONESHOT_SUCCESS) { output.fail(written); return; }
        // Vendor toggle ACK confirms the command, never a final channel state.
    } else {
        uint8_t bytes[11]{};
        const uint8_t byteCount = static_cast<uint8_t>((count + 7) / 8);
        bytes[0] = slave; bytes[1] = 15; bytes[2] = base >> 8; bytes[3] = base;
        bytes[4] = 0; bytes[5] = count; bytes[6] = byteCount;
        for (uint16_t i = 0; i < count; ++i) {
            const bool state = action == "allOn";
            if (state) bytes[7 + i / 8] |= 1U << (i % 8);
            record(i, state);
        }
        if (!frame.assign(bytes, 7 + byteCount)) { ctx->sendBadRequest(request, "Invalid batch frame"); return; }
        written = output.submit(frame);
        if (written.error != ONESHOT_SUCCESS) { output.fail(written); return; }
    }
    // Readback is separate evidence. Missing/short readback stays unknown and
    // cannot synthesize allOff or trigger rules as though the load was measured.
    FastBee::ModbusControlTarget after;
    ModbusSubDevice afterDevice;
    OneShotResult observed;
    if (modbus->snapshotOrdinaryRelayEndpoint(slave, function, base, afterDevice, after) &&
        target.sameMapping(after)) {
        observed = modbus->readRegistersOnce(slave, bitmap || function == 6 ? 3 : 1,
            bitmap ? target.batch : base, bitmap ? 1 : count, true);
    }
    const bool readback = observed.error == ONESHOT_SUCCESS && observed.count >= (bitmap ? 1 : count);
    JsonDocument doc; doc["success"] = true;
    auto data = doc["data"].to<JsonObject>();
    data["channelCount"] = count; data["action"] = action; data["mode"] = mode;
    data["readbackAvailable"] = readback;
    auto states = data["states"].to<JsonArray>();
    for (uint16_t i = 0; i < count; ++i) {
        if (readback) states.add(bitmap ? ((observed.data[0] >> i) & 1U) != 0 : observed.data[i] != 0);
        else states.add(nullptr);
        if (!commandKnown[i]) continue;
        const bool logical = device.ncMode ? !commandState[i] : commandState[i];
        publishFrozenModbusReport(ctx, device, target.index, i, String(logical ? 1 : 0),
            "modbus_command_acknowledged");
        char eventId[16], event[96];
        snprintf(eventId, sizeof(eventId), "mc:%u", target.index);
        snprintf(event, sizeof(event), "{\"d\":%u,\"c\":%u,\"a\":\"%s\"}",
            target.index, i, logical ? "on" : "off");
        PeriphExecManager::getInstance().triggerEventById(String(eventId), String(event));
    }
    output.append(data); addModbusDebug(doc, modbus); HandlerUtils::sendJsonStream(request, doc);
}

void ModbusRouteHandler::handleModbusCoilDelay(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ModbusHttp::fields(*ctx, request, {"slaveAddress", "channel", "coilBase", "mode",
            "delayBase", "delayUnits", "ncMode", "delayMode"})) return;
    uint32_t slave, channel, base, delayBase, units, delayMode;
    if (!ModbusHttp::number(*ctx, request, "slaveAddress", 1, 1, 247, slave) ||
        !ModbusHttp::number(*ctx, request, "channel", 0, 0, 31, channel) ||
        !ModbusHttp::number(*ctx, request, "coilBase", 0, 0, 65535, base) ||
        !ModbusHttp::number(*ctx, request, "delayBase", 0x0200, 0, 65535, delayBase) ||
        !ModbusHttp::number(*ctx, request, "delayUnits", 50, 1, 255, units) ||
        !ModbusHttp::number(*ctx, request, "delayMode", 0, 0, 2, delayMode)) return;
    const String mode = ctx->getParamValue(request, "mode", "coil");
    if (base + channel > 65535U || delayBase + channel > 65535U ||
        (mode != "coil" && mode != "register")) {
        ctx->sendBadRequest(request, "Invalid delay endpoint or mode"); return;
    }
    if (rejectModbusHeavyRequest(request, "Modbus delayed control", 8192)) return;
    auto lease = getModbusMaster(ctx, request);
    auto* modbus = lease.get();
    if (!modbus) return;
    const uint8_t function = mode == "register" ? 6 : 5;
    const uint16_t address = static_cast<uint16_t>(base + channel);
    ModbusSubDevice device;
    FastBee::ModbusControlTarget target;
    if (!modbus->snapshotOrdinaryRelayEndpoint(slave, function, address, device, target) ||
        target.function != function || target.first != base || channel >= target.count) {
        ctx->sendError(request, 409, "CONTROL_RELAY_MAPPING_UNCONFIRMED"); return;
    }
    const bool nc = device.ncMode;
    if (request->hasParam("ncMode", true)) {
        const String text = request->getParam("ncMode", true)->value();
        if (text != "true" && text != "false" && text != "1" && text != "0") {
            ctx->sendBadRequest(request, "Invalid NC mode"); return;
        }
        if ((text == "true" || text == "1") != nc) {
            ctx->sendError(request, 409, "CONTROL_RELAY_CONFIGURATION_CHANGED"); return;
        }
    }
    // Zero retains the saved default. Explicit software mode must remain
    // software even for a normally-open coil with a hardware flash profile.
    if (!delayMode) delayMode = device.delayMode;
    const bool hardwareRegister = delayMode == 2;
    const bool hardwareCoil = function == 5 && delayMode == 0 && !nc;
    const bool hardware = hardwareRegister || hardwareCoil;
    if (delayMode > 2 || (hardwareRegister && (function != 6 || device.delayMode != 2)) ||
        (hardwareCoil && delayBase != 0x0200)) {
        ctx->sendError(request, 409, "CONTROL_DELAY_PROFILE_UNCONFIRMED"); return;
    }
    const uint32_t delayMs = units * 100U;
    const bool softwareFinal = !hardware || (hardwareRegister && nc);
    const uint32_t finalDelayMs = delayMs + (hardwareRegister && nc ? 500U : 0U);
    ModbusHttp::Output output(*ctx, request, *modbus); output.expect(target);
    if (!output.begin()) return;
    if (softwareFinal && output.context().request.holdMs <= finalDelayMs) {
        ctx->sendBadRequest(request, "CONTROL_DELAY_EXCEEDS_HOLD"); return;
    }
    uint64_t operationId = 0, hardwareOperationId = 0;
    ModbusDelayReceipt receipt;
    auto send = [&](bool success, const char* reason, const OneShotResult* fault = nullptr) {
        JsonDocument doc; doc["success"] = success;
        if (!success) doc["error"] = reason;
        auto data = doc["data"].to<JsonObject>();
        output.append(data);
        data["slaveAddress"] = slave; data["channel"] = channel; data["address"] = address;
        data["delayUnits"] = units; data["delayMs"] = delayMs; data["delayMode"] = delayMode;
        data["ncMode"] = nc; data["mode"] = hardware ? "hardware" : "software";
        data["initialCommandAcknowledged"] = data["acknowledgedCommandCount"].as<unsigned>() != 0;
        data["finalScheduled"] = operationId != 0;
        data["finalCommandAcknowledged"] = false;
        data["hardwareWindowTracked"] = hardwareOperationId != 0;
        if (!success) data["manualReviewRequired"] = data["outputAttempted"].as<bool>();
        if (fault) {
            data["errorCode"] = static_cast<unsigned>(fault->error);
            data["exceptionCode"] = fault->exceptionCode;
            data["resultUnknown"] = fault->outputAttempted && fault->error != ONESHOT_EXCEPTION;
        }
        if (operationId || hardwareOperationId) {
            char id[21]; snprintf(id, sizeof(id), "%llu", static_cast<unsigned long long>(operationId ? operationId : hardwareOperationId));
            data["operationId"] = id; data["handlerInstance"] = receipt.instance;
            data["configuration"] = receipt.configuration;
        }
        if (hardwareRegister) data["delayValue"] = units * 10U + 1U;
        addModbusDebug(doc, modbus);
        ctx->sendJsonResponse(request, success ? 200 : data["outputAttempted"].as<bool>() ? 502 : 409, doc);
    };
    FastBee::ModbusOutputFrame frame;
    frame.single(slave, function, address, nc ? 0 : function == 5 ? 0xff00 : 1);
    OneShotResult written = output.submit(frame);
    if (written.error != ONESHOT_SUCCESS || !written.delayReceipt.writeSequence) {
        send(false, "INITIAL_COMMAND_UNCONFIRMED", &written); return;
    }
    receipt = written.delayReceipt;
    if (hardware) {
        if (hardwareRegister) frame.single(slave, 6, address, units * 10U + 1U);
        else frame.single(slave, 5, delayBase + channel, units << 8, FastBee::ModbusFrameAccess::RelayVendor);
        frame.hardwareWindowMs = delayMs;
        written = output.submit(frame);
        // The PM core retains a possible hardware window for every attempted
        // trigger, including a lost ACK. No HTTP retry or late compensating write.
        if (written.error != ONESHOT_SUCCESS || !written.delayReceipt.writeSequence) {
            send(false, "HARDWARE_DELAY_UNCONFIRMED", &written); return;
        }
        receipt = written.delayReceipt;
        if (!modbus->retainOrdinaryHardwareDelay(slave, delayMs, receipt, output.context(), &hardwareOperationId)) {
            send(false, "HARDWARE_ACKNOWLEDGED_TRACKING_UNCONFIRMED"); return;
        }
    }
    if (softwareFinal && !modbus->addOrdinaryCoilDelayTask(slave, address, finalDelayMs, nc,
            function == 6, receipt, output.context(), &operationId)) {
        send(false, "INITIAL_ACKNOWLEDGED_FINAL_NOT_SCHEDULED"); return;
    }
    send(true, nullptr);
}
