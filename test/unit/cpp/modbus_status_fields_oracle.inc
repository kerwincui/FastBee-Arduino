void ModbusHandler::_notifyStatusChange() {
    if (!_statusChangeCallback || !isInitialized) return;

    // 计算状态 hash：基于统计计数判断是否有显著变化
    // 使用简单的 XOR hash，避免完整 JSON 序列化开销
    uint32_t newHash = _totalPollCount ^ _successPollCount ^ 
                       (_failedPollCount << 8) ^ (_timeoutPollCount << 16) ^
                       (isInitialized ? 0x80000000 : 0);

    // 只有 hash 变化时才推送（避免每次 poll 都推送）
    if (newHash == _lastStatusHash) return;
    _lastStatusHash = newHash;

    // 构建轻量级 JSON 状态字符串
    // 参考 ModbusRouteHandler::handleGetModbusStatus 的 JSON 结构
    JsonDocument doc;
    JsonObject data = doc.to<JsonObject>();

    data["mode"] = (config.mode == MODBUS_MASTER) ? "master" : "slave";
    data["status"] = getStatus();
    data["totalPolls"] = _totalPollCount;
    data["successPolls"] = _successPollCount;
    data["failedPolls"] = _failedPollCount;
    data["timeoutPolls"] = _timeoutPollCount;
    data["lastPollAgeSec"] = getLastPollAgeSec();
    data["taskCount"] = config.master.taskCount;

    String json;
    serializeJson(doc, json);

    _statusChangeCallback(json);
}

String ModbusHandler::getPollStatistics() const {
    JsonDocument doc;
    doc["totalPolls"] = _totalPollCount;
    doc["successPolls"] = _successPollCount;
    doc["failedPolls"] = _failedPollCount;
    doc["timeoutPolls"] = _timeoutPollCount;
    doc["backoffSkips"] = _slaveBackoffSkips;
    doc["backoffEvictions"] = _slaveBackoffEvictions;
    doc["lastPollMs"] = _lastPollTime > 0 ? (millis() - _lastPollTime) / 1000 : 0;

    const uint32_t now = millis();
    uint32_t slaveTimeouts = 0;
    uint32_t slaveCrcErrors = 0;
    uint32_t slaveExceptions = 0;
    uint8_t trackedSlaves = 0;
    JsonArray slaveHealth = doc["slaveHealth"].to<JsonArray>();
    for (uint8_t i = 0; i < SLAVE_BACKOFF_SLOT_COUNT; i++) {
        const SlaveBackoffState& state = _slaveBackoff[i];
        if (!state.occupied) continue;

        trackedSlaves++;
        slaveTimeouts += state.timeoutCount;
        slaveCrcErrors += state.crcErrorCount;
        slaveExceptions += state.exceptionCount;
        const bool backoffActive = state.cooldownUntil != 0 &&
                                   !_deadlineReached(now, state.cooldownUntil);

        JsonObject item = slaveHealth.add<JsonObject>();
        item["address"] = state.slaveAddress;
        item["consecutiveFailures"] = state.consecutiveFailures;
        item["backoffLevel"] = state.backoffLevel;
        item["backoffActive"] = backoffActive;
        item["remainingMs"] = backoffActive ? state.cooldownUntil - now : 0;
        item["halfOpen"] = state.halfOpen;
        item["lastError"] = state.lastError;
        item["timeouts"] = state.timeoutCount;
        item["crcErrors"] = state.crcErrorCount;
        item["exceptions"] = state.exceptionCount;
        item["successes"] = state.successCount;
        item["skipped"] = state.skippedCount;
        item["halfOpenProbes"] = state.halfOpenProbeCount;
    }
    doc["trackedSlaves"] = trackedSlaves;
    doc["slaveTimeouts"] = slaveTimeouts;
    doc["slaveCrcErrors"] = slaveCrcErrors;
    doc["slaveExceptions"] = slaveExceptions;
    
    String out;
    serializeJson(doc, out);
    return out;
}
