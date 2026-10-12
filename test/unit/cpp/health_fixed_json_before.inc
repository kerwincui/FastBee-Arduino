void HealthMonitor::fillDegradationJson(JsonObject dst) const {
    static const char* levelNames[] = {"NORMAL", "WARN", "SEVERE", "CRITICAL"};
    const uint8_t level = static_cast<uint8_t>(_currentLevel);
    dst["active"] = isMemoryDegraded();
    dst["runtimeOnly"] = true;
    dst["level"] = levelNames[level < 4 ? level : 0];
    dst["mqttsDowngraded"] = false;
    dst["mqttsPaused"] = _mqttsMemoryDowngrade;
    dst["mqttsProtection"] = "pause_without_plaintext_fallback";
    dst["mqttStopped"] = _mqttStoppedForMemory;
    dst["mqttDisabled"] = _mqttDisabledForMemory;
    dst["mqttRestorePending"] = _mqttRestorePendingForMemory;
    dst["modbusStopped"] = _modbusStoppedForMemory;
    dst["periphExecPaused"] = _periphExecPausedForMemory;
#if FASTBEE_ENABLE_PERIPH_EXEC
    dst["periphExecWorkerSuspended"] = arePeriphExecWorkersSuspendedForMemory();
    dst["periphExecWorkerSuspensionPending"] = isPeriphExecWorkerSuspensionPending();
#else
    dst["periphExecWorkerSuspended"] = false;
    dst["periphExecWorkerSuspensionPending"] = false;
#endif
    dst["optionalPeripheralsSuspended"] = _optionalPeripheralsSuspendedForMemory;
    dst["optionalPeripheralCount"] = _memorySuspendedPeripheralCount;
    JsonArray actions = dst["actions"].to<JsonArray>();
    if (_mqttsMemoryDowngrade) actions.add("MQTTS 已因内存压力暂停，禁止明文回退");
    if (_modbusStoppedForMemory) actions.add("Modbus 轮询已暂停");
    if (_periphExecPausedForMemory) actions.add("外设执行定时规则已暂停");
#if FASTBEE_ENABLE_PERIPH_EXEC
    if (isPeriphExecWorkerSuspensionPending()) {
        actions.add("外设执行 worker 正在退出，系统将继续重试");
    }
    if (arePeriphExecWorkersSuspendedForMemory() &&
        !isPeriphExecWorkerSuspensionPending()) {
        actions.add("外设执行 worker 已挂起，系统将继续恢复");
    }
#endif
    if (_optionalPeripheralsSuspendedForMemory) {
        actions.add("高占用可选外设已临时挂起");
    }
    if (_mqttStoppedForMemory) actions.add("MQTT 已停止");
    if (_mqttRestorePendingForMemory) actions.add("MQTT 恢复等待内存条件，系统将继续重试");
    if (_mqttRecoveryBlockedForStorage) actions.add("MQTT 准入资源不可用，需要重启后恢复");
    if (_mqttDisabledForMemory) actions.add("保留旧版 MQTT 禁用记录，需用户明确保存并启用");
    if (actions.size() == 0 && _currentLevel == MemoryGuardLevel::WARN) {
        actions.add("已降低日志和上报频率");
    }
}

void HealthMonitor::fillMetricsJson(String& json) {
    JsonDocument doc;
    doc["heap"]["free"]          = currentHealth.freeHeap;
    doc["heap"]["min"]           = currentHealth.minFreeHeap;
    doc["heap"]["largest_block"] = currentHealth.largestFreeBlock;
    doc["heap"]["fragmentation"] = currentHealth.heapFragmentation;
    // DRAM 内部内存（排除 PSRAM，用于 WiFi/MQTT/SSL 保护决策）
    doc["heap"]["dram_free"]     = currentHealth.dramFreeHeap;
    doc["heap"]["dram_largest"]  = currentHealth.dramLargestBlock;
    doc["mqtt"]["queue_depth"]   = mqttQueueDepth;
    doc["sse"]["client_count"]   = sseClientCount;
    doc["poll"]["duration_ms"]   = pollDurationMs;
    doc["uptime"]                = currentHealth.uptime / 1000UL;
    doc["boot_time_ms"]           = currentHealth.bootTimeMs;

    // MemGuard 等级信息
    static const char* levelNames[] = { "NORMAL", "WARN", "SEVERE", "CRITICAL" };
    uint8_t lvl = static_cast<uint8_t>(_currentLevel);
    doc["memguard"]["level"]              = lvl;
    doc["memguard"]["level_name"]         = levelNames[lvl < 4 ? lvl : 0];
    doc["memguard"]["fragmentation_high"] = isFragmentationHigh();
    // 内存恢复状态（防砖机制进度）
    doc["memguard"]["mqtts_downgraded"]    = false;
    doc["memguard"]["mqtts_paused"]        = _mqttsMemoryDowngrade;
    doc["memguard"]["mqtts_protection"]    = "pause_without_plaintext_fallback";
    doc["memguard"]["mqtt_stopped"]        = _mqttStoppedForMemory;
    doc["memguard"]["mqtt_disabled"]       = _mqttDisabledForMemory;
    doc["memguard"]["mqtt_restore_pending"] = _mqttRestorePendingForMemory;
    doc["memguard"]["modbus_stopped"]      = _modbusStoppedForMemory;
    doc["memguard"]["periph_exec_paused"]  = _periphExecPausedForMemory;
#if FASTBEE_ENABLE_PERIPH_EXEC
    doc["memguard"]["periph_exec_worker_suspended"] =
        arePeriphExecWorkersSuspendedForMemory();
    doc["memguard"]["periph_exec_worker_suspension_pending"] =
        isPeriphExecWorkerSuspensionPending();
#else
    doc["memguard"]["periph_exec_worker_suspended"] = false;
    doc["memguard"]["periph_exec_worker_suspension_pending"] = false;
#endif
    doc["memguard"]["optional_peripherals_suspended"] =
        _optionalPeripheralsSuspendedForMemory;
    doc["memguard"]["optional_peripheral_count"] = _memorySuspendedPeripheralCount;
    doc["memguard"]["critical_duration_s"] = (unsigned long)(getCriticalDurationMs() / 1000);

    // DRAM 水位趋势（泄漏检测）
    doc["stability"]["dram_history_count"] = _dramHistoryCount;
    doc["stability"]["leak_suspect"] = (_dramHistoryCount >= 6) ? detectMemoryLeak() : false;
    doc["stability"]["uptime_hours"] = (unsigned long)(currentHealth.uptime / 3600000UL);

#if FASTBEE_ENABLE_PERIPH_EXEC
    // ISR 事件队列诊断（中断风暴可观测：队列满丢事件计数 + 当前积压）
    doc["isr"]["queue_overruns"] = (unsigned long)PeripheralManager::getInstance().getIsrQueueOverrunCount();
    doc["isr"]["queue_waiting"]  = (unsigned long)PeripheralManager::getInstance().getIsrQueueWaiting();
#endif

    // PSRAM 信息（用于前端判断 MQTTS 支持）
    doc["heap"]["psram_total"] = (unsigned long)(ESP.getPsramSize() / 1024);
    doc["heap"]["psram_free"]  = (unsigned long)(ESP.getFreePsram() / 1024);
    doc["heap"]["tls_supported"] = (psramFound() && ESP.getPsramSize() > 0);

    // 内存池统计（碎片预防监控）
    for (uint8_t i = 0; i < _trackedPoolCount; i++) {
        const PoolStatsEntry& p = _trackedPools[i];
        String key = String("pools.") + p.name;
        doc[key]["used"] = p.getUsed ? p.getUsed() : 0;
        doc[key]["capacity"] = p.capacity;
        doc[key]["exhaust"] = p.getExhaust ? (unsigned long)p.getExhaust() : 0UL;
    }

    // 复用外部 String：reserve 仅扩容不缩容，已足够大时是 no-op
    json.reserve(512);
    json = "";
    serializeJson(doc, json);
}
