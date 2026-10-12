// Frozen complete original methods before the 45-write refactor.

bool MQTTClient::publishDeviceInfo() {
    if (isConnectionTestOnly()) return false;
    if (!isConnected) {
        return false;
    }

    // 内存保护：该函数需要约 2KB 内部 DRAM 分配（JsonDocument + String payload）。
    if (!mqttHasInternalDram(30000, 4096, "publishDeviceInfo")) {
        return false;
    }

    // 查找 topicType==DEVICE_INFO 的发布主题
    int infoTopicIdx = findEnabledPublishTopicIndex(MqttTopicType::DEVICE_INFO);
    if (infoTopicIdx < 0) {
        LOG_DEBUG("MQTT: No enabled DEVICE_INFO publish topic found");
        return false;
    }

    // 构建设备信息 JSON
    JsonDocument doc;
    doc["rssi"] = WiFi.RSSI();
    doc["firmwareVersion"] = SystemInfo::VERSION;
    doc["status"] = 3;  // 在线

    // userId: 优先使用配置值，默认为 1
    if (!config.userId.isEmpty()) {
        long uid = config.userId.toInt();
        doc["userId"] = (uid > 0) ? uid : 1;
    } else {
        doc["userId"] = 1;
    }

    // 经纬度
    doc["longitude"] = config.longitude;
    doc["latitude"]  = config.latitude;

    // 可选字段：非空/非零时才包含
    if (!config.iccid.isEmpty()) {
        doc["iccid"] = config.iccid;
    }
    if (config.cardPlatformId != 0) {
        doc["cardPlatformId"] = config.cardPlatformId;
    }

    // summary: 解析配置的 JSON 字符串，为空时自动生成默认值
    if (!config.summary.isEmpty()) {
        JsonDocument summaryDoc;
        DeserializationError err = deserializeJson(summaryDoc, config.summary.c_str(), config.summary.length());
        if (!err) {
            doc["summary"] = summaryDoc.as<JsonObject>();
        }
    } else {
        // 自动生成默认 summary
        JsonObject summary = doc["summary"].to<JsonObject>();
        summary["name"] = "FastBee";
        summary["chip"] = ESP.getChipModel();
    }

    String payload;
    serializeJson(doc, payload);

    // 获取完整主题用于日志输出
    const MqttPublishTopic& pt = config.publishTopics[infoTopicIdx];
    String fullTopic = buildFullTopicWithType(pt.topic, pt.autoPrefix, pt.topicType);

    bool ok = publishToTopic((size_t)infoTopicIdx, payload);

    LOG_INFO(ok ? "MQTT: Published device info" : "MQTT: Failed to publish device info");

    return ok;
}

void MQTTClient::_notifyStatusChange() {
    if (!_statusChangeCallback) return;

    // 构建轻量级 JSON 状态字符串
    // 字段名必须与 MqttRouteHandler::handleGetMqttStatus 保持一致，
    // 前端 _updateMqttStatusUI 依赖 initialized/connected/reconnectCount 等字段
    JsonDocument doc;
    JsonObject data = doc.to<JsonObject>();

    data["initialized"] = true;  // SSE 推送时 MQTT 客户端已存在且配置已加载
    data["connectionTestOnly"] = isConnectionTestOnly();
    data["connected"] = isConnected.load();
    data["stopped"] = stopped.load();
    data["reconnectWorkerActive"] = _reconnectLifetime.active();
    data["reconnectStopPending"] = _reconnectLifetime.stopping() && _reconnectLifetime.active();
    data["reconnectCancellationRequested"] = _reconnectLifetime.stopping();
    data["server"] = config.server;
    data["port"] = config.port;
    data["clientId"] = config.clientId;
    data["reconnectCount"] = reconnectCount.load();
    data["lastError"] = lastErrorCode.load();
    data["lastErrorCode"] = lastErrorCode.load();
    // Keep asynchronous SSE diagnostics aligned with the REST status route.
    // The transport pointer only says which Client object was injected; it
    // does not say that Ethernet/4G is currently registered on the network.
    // Conversely, Ethernet MQTTS intentionally uses the internal secure
    // client, so checking WiFi.status() alone would report a healthy Ethernet
    // link as offline.  Ask the active network manager for the authoritative
    // connection state and retain the old fallback during early boot, before
    // the framework has published its manager.
    bool networkConnected = false;
    FastBeeFramework* statusFw = FastBeeFramework::getInstance();
    FBNetworkManager* statusNet = statusFw
        ? static_cast<FBNetworkManager*>(statusFw->getNetworkManager())
        : nullptr;
    if (statusNet) {
        networkConnected = statusNet->isNetworkConnected();
    } else {
        networkConnected = _externalClient || WiFi.status() == WL_CONNECTED;
    }
    data["lastErrorName"] = mqttStatusErrorName(lastErrorCode);
    data["lastErrorClass"] = mqttStatusErrorClass(
        lastErrorCode, _lastMqttsTlsMemoryFailure);
    const char* errorMessage = mqttStatusErrorMessage(
        lastErrorCode, networkConnected, _lastMqttsTlsMemoryFailure);
    data["lastErrorMessage"] = errorMessage;
    // Keep the legacy alias used by older Web bundles and make the reason
    // available to SSE-only pages without waiting for a REST status poll.
    data["errorMessage"] = errorMessage;
    data["statusReason"] = errorMessage;
    data["networkConnected"] = networkConnected;
    data["autoReconnect"] = config.autoReconnect;
    data["connecting"] = !stopped && !isConnected && config.autoReconnect;
    data["tlsMemoryFailure"] = _lastMqttsTlsMemoryFailure.load();
    data["scheme"] = config.scheme;
    data["tlsAllocated"] = isTlsTransportAllocated();
    data["webPausedForMqtts"] = _webServerPaused.load();
    data["disabledByMemory"] = _disabledByMemory.load();
    data["stoppedByMemory"] = _stoppedByMemory.load();
    if (_disabledByMemory) {
        // Match the REST contract even when an SSE consumer receives the
        // stopped event before its next /api/mqtt/status poll.
        FastBee::writeMqttDiagnostic(data, FastBee::MqttDiagnostic::MemoryDisabled,
            FastBee::MqttDiagnosticFields::Status | FastBee::MqttDiagnosticFields::Reason);
    } else if (_stoppedByMemory) {
        // Keep SSE diagnostics aligned with the REST status contract while a
        // temporary memory stop is waiting for the next recovery sample.
        data["connected"] = false;
        data["connecting"] = false;
        FastBee::writeMqttDiagnostic(data, FastBee::MqttDiagnostic::MemoryStopped,
            FastBee::MqttDiagnosticFields::Status | FastBee::MqttDiagnosticFields::Reason);
    }
    // 队列背压统计（消费者侧监控用）
    const auto queueSnapshot = getReportQueueSnapshot();
    data["queueDepth"] = queueSnapshot.depth;
    data["queueEnqueue"] = queueSnapshot.stats.enqueueTotal;
    data["queueDrops"] = queueSnapshot.stats.dropTotal;
    data["queuePublished"] = queueSnapshot.stats.publishTotal;
    FastBee::addMqttReportQueueStatus(data, &queueSnapshot);

    String json;
    serializeJson(doc, json);

    _statusChangeCallback(json);
}
