void normalizeProtocolConfig(JsonDocument& doc) {
    if (!doc["version"].is<int>()) {
        doc["version"] = PROTOCOL_CONFIG_VERSION;
    }
    if (!doc["modbusRtu"]["enabled"].is<bool>()) {
        doc["modbusRtu"]["enabled"] = true;
    }
    if (!doc["modbusRtu"]["mode"].is<const char*>()) {
        doc["modbusRtu"]["mode"] = "master";
    }
    const int transferType = doc["modbusRtu"]["transferType"] | 0;
    if (transferType != 0 && transferType != 1) {
        doc["modbusRtu"]["transferType"] = 0;
    }
    doc.as<JsonObject>().remove("modbusTcp");
    if (!doc["mqtt"]["enabled"].is<bool>()) {
        doc["mqtt"]["enabled"] = true;
    }
    ensureFastBeeOtaTopics(doc["mqtt"].as<JsonObject>());
    disableUnsupportedBinaryOtaTopics(doc["mqtt"].as<JsonObject>());
}

void ProtocolRouteHandler::handleSaveProtocolConfig(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

#if FASTBEE_ENABLE_REMOTE_RESOURCES
    struct RemoteConfigTransaction {
        bool locked = FastBee::RemoteResourceManager::instance()
                          .beginProtocolConfigTransaction();
        ~RemoteConfigTransaction() {
            if (locked) FastBee::RemoteResourceManager::instance()
                            .endProtocolConfigTransaction();
        }
        void release() {
            if (locked) FastBee::RemoteResourceManager::instance()
                            .endProtocolConfigTransaction();
            locked = false;
        }
    } remoteConfigTransaction;
    if (!remoteConfigTransaction.locked) {
        ctx->sendError(request, 503, "Protocol configuration is busy; retry shortly");
        return;
    }
#endif

    // Task 3.3: 完整 protocol.json 加载使用 PSRAM 分配器（有 PSRAM 时），降低 DRAM 峰值
    JsonDocument doc = FastBee::makeJsonDocument(8192);

    // 增量更新：先加载现有配置
    // fail-safe：文件存在但解析失败时拒绝保存。若继续，会把增量参数写进
    // 空 doc 后整体回写，导致其余协议节（mqtt/modbusRtu 等）丢失
    if (LittleFS.exists(PROTOCOL_CONFIG_PATH)) {
        File f = LittleFS.open(PROTOCOL_CONFIG_PATH, "r");
        if (!f) {
            ctx->sendError(request, 500, "Failed to open protocol config for update");
            return;
        }
        DeserializationError loadErr = deserializeJson(doc, f);
        f.close();
        if (loadErr || !doc.is<JsonObject>()) {
            LOG_ERRORF("Protocol config save aborted: existing config unreadable (%s)", loadErr.c_str());
            ctx->sendError(request, 500, "Existing protocol config unreadable, save aborted");
            return;
        }
    }
    #define GP(key, def) ctx->getParamValue(request, key, def)
    #define GPI(key, def) GP(key, def).toInt()

    bool updateModbusRtu = hasProtocolParam(request, "modbusRtu_enabled") ||
                           hasProtocolParam(request, "modbusRtu_peripheralId") ||
                           hasProtocolParam(request, "modbusRtu_mode") ||
                           hasProtocolParam(request, "modbusRtu_dePin") ||
                           hasProtocolParam(request, "modbusRtu_transferType") ||
                           hasProtocolParam(request, "modbusRtu_master_tasks") ||
                           hasProtocolParam(request, "modbusRtu_master_devices");
    auto previousModbusRtu = FastBee::makeJsonDocument();
    if (updateModbusRtu && !previousModbusRtu.set(doc["modbusRtu"])) {
        ctx->sendError(request, 503, "Insufficient memory to compare Modbus configuration");
        return;
    }
    bool updateMqtt = hasProtocolParam(request, "mqtt_enabled") ||
                      hasProtocolParam(request, "mqtt_scheme") ||
                      hasProtocolParam(request, "mqtt_server") ||
                      hasProtocolParam(request, "mqtt_port") ||
                      hasProtocolParam(request, "mqtt_clientId") ||
                      hasProtocolParam(request, "mqtt_username") ||
                      hasProtocolParam(request, "mqtt_password") ||
                      hasProtocolParam(request, "mqtt_keepAlive") ||
                      hasProtocolParam(request, "mqtt_autoReconnect") ||
                      hasProtocolParam(request, "mqtt_authType") ||
                      hasProtocolParam(request, "mqtt_mqttSecret") ||
                      hasProtocolParam(request, "mqtt_authCode") ||
                      hasProtocolParam(request, "mqtt_willTopic") ||
                      hasProtocolParam(request, "mqtt_willPayload") ||
                      hasProtocolParam(request, "mqtt_willQos") ||
                      hasProtocolParam(request, "mqtt_willRetain") ||
                      hasProtocolParam(request, "mqtt_longitude") ||
                      hasProtocolParam(request, "mqtt_latitude") ||
                      hasProtocolParam(request, "mqtt_iccid") ||
                      hasProtocolParam(request, "mqtt_cardPlatformId") ||
                      hasProtocolParam(request, "mqtt_summary") ||
                      hasProtocolParam(request, "mqtt_publishTopics") ||
                      hasProtocolParam(request, "mqtt_subscribeTopics");
    const bool updateOtaBase = hasProtocolParam(request, "mqtt_otaBaseUrl");
    // OTA-only saves must preserve every other protocol member, including
    // legacy topics and disabled transports. They do not restart MQTT/Modbus.
    if (updateMqtt || updateModbusRtu || !updateOtaBase) {
        normalizeProtocolConfig(doc);
        doc["version"] = PROTOCOL_CONFIG_VERSION;
    }
    if (updateOtaBase) {
        String otaBaseUrl = GP("mqtt_otaBaseUrl", "");
        otaBaseUrl.trim();
        String otaBaseUrlError;
        if (!validateOtaBaseUrl(otaBaseUrl, otaBaseUrlError)) {
            ctx->sendError(request, 400, otaBaseUrlError);
            return;
        }
        doc["mqtt"]["otaBaseUrl"] = otaBaseUrl;
    }
    bool clientIdGenerated = false;
    String clientId = doc["mqtt"]["clientId"] | "";

    if (updateModbusRtu && !ctx->requireDeveloperMode(request)) {
        return;
    }

    // Modbus RTU
    if (updateModbusRtu) {
    bool currentEnabled = doc["modbusRtu"]["enabled"] | true;
    String currentPeripheralId = doc["modbusRtu"]["peripheralId"] | "";
    String currentMode = doc["modbusRtu"]["mode"] | "master";
    int currentDePin = doc["modbusRtu"]["dePin"] | -1;
    int currentTransferType = doc["modbusRtu"]["transferType"] | 0;

    if (hasProtocolParam(request, "modbusRtu_transferType")) {
        String requestedTransferType = GP("modbusRtu_transferType", "");
        requestedTransferType.trim();
        if (requestedTransferType != "0" && requestedTransferType != "1") {
            ctx->sendError(request, 400,
                           "modbusRtu_transferType must be 0 (JSON) or 1 (RAW RTU)");
            return;
        }
        currentTransferType = requestedTransferType == "1" ? 1 : 0;
    }

    doc["modbusRtu"]["enabled"] = GP("modbusRtu_enabled", currentEnabled ? "true" : "false") == "true";
    doc["modbusRtu"]["peripheralId"] = GP("modbusRtu_peripheralId", currentPeripheralId);
    doc["modbusRtu"]["mode"] = GP("modbusRtu_mode", currentMode);
    doc["modbusRtu"]["dePin"] = GPI("modbusRtu_dePin", String(currentDePin));
    doc["modbusRtu"]["transferType"] = currentTransferType;
    // workMode 已移除：由轮询任务配置自动推导，不再保存

    // Modbus RTU Master 通信参数（responseTimeout/maxRetries/interPollDelay）已从构造函数硬编码，
    // 实际执行时由外设执行-轮询触发器覆盖，不写入 protocol.json。

    // Modbus RTU Master 轮询任务
    // 仅在前端实际发送了 tasks 数据时才清空并重建数组，避免从其他标签页保存时误删现有配置
    String masterTasksJson = GP("modbusRtu_master_tasks", "");
    if (!validateModbusArrayUpdate(ctx, request, doc["modbusRtu"]["master"]["tasks"].as<JsonArrayConst>(), masterTasksJson, true)) return;
    if (masterTasksJson.length() > 0) {
    JsonArray masterTasks = doc["modbusRtu"]["master"]["tasks"].to<JsonArray>();
    if (masterTasksJson.length() > 2) {
        JsonDocument tasksDoc;
        DeserializationError tasksErr = deserializeJson(tasksDoc, masterTasksJson.c_str(), masterTasksJson.length());
        if (!tasksErr && tasksDoc.is<JsonArray>()) {
            JsonArray arr = tasksDoc.as<JsonArray>();
            uint8_t taskCount = 0;
            for (JsonVariant v : arr) {
                if (taskCount >= Protocols::MODBUS_MAX_POLL_TASKS) break;
                int functionCode = v["functionCode"] | 3;
                if (functionCode != 1 && functionCode != 2 && functionCode != 3 && functionCode != 4) {
                    functionCode = 3;
                }
                JsonObject taskObj = masterTasks.add<JsonObject>();
                FastBee::setFixedJsonField(taskObj, "slaveAddress", clampProtocolValue<int>(v["slaveAddress"] | 1, 1, 247));
                FastBee::setFixedJsonField(taskObj, "functionCode", functionCode);
                FastBee::setFixedJsonField(taskObj, "startAddress", clampProtocolValue<int>(v["startAddress"] | 0, 0, 65535));
                FastBee::setFixedJsonField(taskObj, "quantity", clampProtocolValue<int>(v["quantity"] | 10, 1, Protocols::MODBUS_MAX_REGISTERS_PER_READ));
                FastBee::setFixedJsonField(taskObj, "pollInterval", clampProtocolValue<int>(v["pollInterval"] | 30,
                                                                  PROTOCOL_MIN_MODBUS_POLL_INTERVAL_SEC,
                                                                  PROTOCOL_MAX_MODBUS_POLL_INTERVAL_SEC));
                FastBee::setFixedJsonField(taskObj, "enabled", v["enabled"] | true);
                FastBee::setFixedJsonField(taskObj, "name", v["name"] | "");
                
                // 寄存器映射
                if (v.containsKey("mappings") && v["mappings"].is<JsonArray>()) {
                    JsonArray mappingsArr = taskObj["mappings"].to<JsonArray>();
                    for (JsonVariant mv : v["mappings"].as<JsonArray>()) {
                        JsonObject mo = mappingsArr.add<JsonObject>();
                        FastBee::setFixedJsonField(mo, "regOffset", mv["regOffset"] | 0);
                        FastBee::setFixedJsonField(mo, "dataType", mv["dataType"] | 0);
                        FastBee::setFixedJsonField(mo, "wordOrder", mv["wordOrder"] | 0);
                        FastBee::setFixedJsonField(mo, "scaleFactor", mv["scaleFactor"] | 1.0);
                        FastBee::setFixedJsonField(mo, "decimalPlaces", mv["decimalPlaces"] | 1);
                        FastBee::setFixedJsonField(mo, "sensorId", mv["sensorId"] | "");
                        FastBee::setFixedJsonField(mo, "unit", mv["unit"] | "");
                    }
                }
                taskCount++;
            }
        }
    }
    } // end: masterTasksJson.length() > 0

    // Modbus RTU Master 子设备
    // 仅在前端实际发送了 devices 数据时才清空并重建数组
    String masterDevicesJson = GP("modbusRtu_master_devices", "");
    if (!validateModbusArrayUpdate(ctx, request, doc["modbusRtu"]["master"]["devices"].as<JsonArrayConst>(), masterDevicesJson, false)) return;
    if (masterDevicesJson.length() > 0) {
    JsonArray masterDevices = doc["modbusRtu"]["master"]["devices"].to<JsonArray>();
    if (masterDevicesJson.length() > 2) {
        JsonDocument devicesDoc;
        DeserializationError devErr = deserializeJson(devicesDoc, masterDevicesJson.c_str(), masterDevicesJson.length());
        if (!devErr && devicesDoc.is<JsonArray>()) {
            JsonArray arr = devicesDoc.as<JsonArray>();
            for (JsonVariant dv : arr) {
                JsonObject devObj = masterDevices.add<JsonObject>();
                FastBee::setFixedJsonField(devObj, "name", dv["name"] | "Device");
                FastBee::setFixedJsonField(devObj, "sensorId", dv["sensorId"] | "");
                FastBee::setFixedJsonField(devObj, "deviceType", dv["deviceType"] | "relay");
                FastBee::setFixedJsonField(devObj, "slaveAddress", dv["slaveAddress"] | 1);
                FastBee::setFixedJsonField(devObj, "channelCount", dv["channelCount"] | 2);
                FastBee::setFixedJsonField(devObj, "coilBase", dv["coilBase"] | 0);
                FastBee::setFixedJsonField(devObj, "ncMode", dv["ncMode"] | false);
                FastBee::setFixedJsonField(devObj, "controlProtocol", dv["controlProtocol"] | 0);
                FastBee::setFixedJsonField(devObj, "batchRegister", dv["batchRegister"] | 0);
                FastBee::setFixedJsonField(devObj, "pwmRegBase", dv["pwmRegBase"] | 0);
                FastBee::setFixedJsonField(devObj, "pwmResolution", dv["pwmResolution"] | 8);
                FastBee::setFixedJsonField(devObj, "pidDecimals", dv["pidDecimals"] | 1);
                FastBee::setFixedJsonField(devObj, "enabled", dv["enabled"] | true);
                if (dv.containsKey("pidAddrs") && dv["pidAddrs"].is<JsonArray>()) {
                    JsonArray pa = devObj["pidAddrs"].to<JsonArray>();
                    for (JsonVariant pv : dv["pidAddrs"].as<JsonArray>())
                        pa.add(pv.as<int>());
                }
                // 电机参数
                FastBee::setFixedJsonField(devObj, "motorDecimals", dv["motorDecimals"] | 0);
                if (dv.containsKey("motorProtocol")) devObj["motorProtocol"].set(dv["motorProtocol"]);
                if (dv.containsKey("motorTravelDraft")) devObj["motorTravelDraft"].set(dv["motorTravelDraft"]);
                FastBee::setFixedJsonField(devObj, "motorMinPosition", dv["motorMinPosition"] | 0);
                FastBee::setFixedJsonField(devObj, "motorMaxPosition", dv["motorMaxPosition"] | 0);
                FastBee::setFixedJsonField(devObj, "motorCurrentPosition", dv["motorCurrentPosition"] | 0);
                FastBee::setFixedJsonField(devObj, "motorMoveStep", dv["motorMoveStep"] | 0);
                FastBee::setFixedJsonField(devObj, "motorLastPulse", dv["motorLastPulse"] | 0);
                if (dv.containsKey("motorRegs") && dv["motorRegs"].is<JsonArray>()) {
                    JsonArray mr = devObj["motorRegs"].to<JsonArray>();
                    for (JsonVariant mv : dv["motorRegs"].as<JsonArray>())
                        mr.add(mv.as<int>());
                }
            }
        }
    }
    } // end: masterDevicesJson.length() > 0
    } // end: updateModbusRtu

    // MQTT
    if (updateMqtt) {
    doc["mqtt"]["enabled"] = GP("mqtt_enabled", "true") == "true";
    String requestedScheme = GP("mqtt_scheme", "mqtt");
    doc["mqtt"]["scheme"] = requestedScheme;
    doc["mqtt"]["server"] = GP("mqtt_server", "iot.fastbee.cn");
    doc["mqtt"]["port"] = GPI("mqtt_port", "1883");

    // Any successful Web save is an explicit user decision.  Do not leave
    // memory-recovery markers behind, otherwise a later reboot could restore
    // an old MQTTS scheme or report MQTT as memory-disabled after the user has
    // re-enabled/edited the protocol.
    doc["mqtt"].remove("disabled_by_memory");
    doc["mqtt"].remove("disable_mqtt_by_memory");
    doc["mqtt"].remove("downgraded_by_memory");
    doc["mqtt"].remove("downgraded_original_scheme");
    doc["mqtt"].remove("downgraded_original_port");

    // 读取 clientId 和 authType，如果 clientId 为空则自动生成
    clientId = GP("mqtt_clientId", "");
    clientId.trim();
    int authType = GPI("mqtt_authType", "0");

    if (clientId.isEmpty()) {
        clientIdGenerated = true;  // 标记为自动生成
        // 1. 获取 deviceId、productNumber、userId：优先从 device.json 读取
        String deviceId = "";
        String productId = "";
        String userId = "1";

        // 尝试从 device.json 读取
        if (LittleFS.exists("/config/device.json")) {
            File devFile = LittleFS.open("/config/device.json", "r");
            if (devFile) {
                JsonDocument devDoc;
                if (!deserializeJson(devDoc, devFile)) {
                    deviceId = devDoc["deviceId"] | "";
                    int pn = devDoc["productNumber"] | 0;
                    if (pn > 0) productId = String(pn);
                    userId = devDoc["userId"] | "1";
                }
                devFile.close();
            }
        }

        // 如果 deviceId 仍为空，生成 FBE+MAC
        if (deviceId.isEmpty()) {
            String mac = WiFi.macAddress();
            mac.replace(":", "");
            deviceId = "FBE" + mac;
        }

        // 如果 productId 仍为空，使用默认值 "1"
        if (productId.isEmpty()) productId = "1";

        // 2. 根据认证类型构建 clientId
        String prefix = (authType == 1) ? "E" : "S";
        clientId = prefix + "&" + deviceId + "&" + productId + "&" + userId;
    }

    doc["mqtt"]["clientId"] = clientId;
    doc["mqtt"]["username"] = GP("mqtt_username", "");
    String submittedPassword = GP("mqtt_password", "");
    if (FastBee::ProtocolSecret::shouldPersist(
            submittedPassword, hasProtocolParam(request, "mqtt_password"))) {
        doc["mqtt"]["password"] = submittedPassword;
    }
    doc["mqtt"]["keepAlive"] = GPI("mqtt_keepAlive", "60");
    doc["mqtt"]["autoReconnect"] = GP("mqtt_autoReconnect", "true") == "true";
    // MQTT 认证配置
    doc["mqtt"]["authType"] = authType;
    String submittedMqttSecret = GP("mqtt_mqttSecret", "");
    if (FastBee::ProtocolSecret::shouldPersist(
            submittedMqttSecret, hasProtocolParam(request, "mqtt_mqttSecret"))) {
        doc["mqtt"]["mqttSecret"] = submittedMqttSecret;
    }
    String submittedAuthCode = GP("mqtt_authCode", "");
    if (FastBee::ProtocolSecret::shouldPersist(
            submittedAuthCode, hasProtocolParam(request, "mqtt_authCode"))) {
        doc["mqtt"]["authCode"] = submittedAuthCode;
    }
    // MQTT 遗嘱消息
    doc["mqtt"]["willTopic"] = GP("mqtt_willTopic", "");
    doc["mqtt"]["willPayload"] = GP("mqtt_willPayload", "");
    doc["mqtt"]["willQos"] = GPI("mqtt_willQos", "0");
    doc["mqtt"]["willRetain"] = GP("mqtt_willRetain", "false") == "true";

    // Card 高级配置
    doc["mqtt"]["longitude"] = atof(GP("mqtt_longitude", "0").c_str());
    doc["mqtt"]["latitude"]  = atof(GP("mqtt_latitude", "0").c_str());
    doc["mqtt"]["iccid"]     = GP("mqtt_iccid", "");
    doc["mqtt"]["cardPlatformId"] = GPI("mqtt_cardPlatformId", "0");
    doc["mqtt"]["summary"]   = GP("mqtt_summary", "");

    // 发布主题配置（仅在前端实际发送了主题数据时才清空重建，避免覆盖已有配置）
    String publishTopicsJson = GP("mqtt_publishTopics", "[]");
    if (hasProtocolParam(request, "mqtt_publishTopics") && publishTopicsJson.length() > 2) {
        JsonArray publishTopics = doc["mqtt"]["publishTopics"].to<JsonArray>();
        JsonDocument topicsDoc;
        DeserializationError err = deserializeJson(topicsDoc, publishTopicsJson.c_str(), publishTopicsJson.length());
        if (!err && topicsDoc.is<JsonArray>()) {
            JsonArray arr = topicsDoc.as<JsonArray>();
            for (JsonVariant v : arr) {
                const int topicType = v["topicType"] | 0;
                JsonObject topicObj = publishTopics.add<JsonObject>();
                FastBee::setFixedJsonField(topicObj, "topic", v["topic"] | "");
                FastBee::setFixedJsonField(topicObj, "qos", v["qos"] | 0);
                FastBee::setFixedJsonField(topicObj, "retain", v["retain"] | false);
                FastBee::setFixedJsonField(topicObj, "enabled", topicType == MQTT_TOPIC_TYPE_OTA_BINARY
                    ? false
                    : (v["enabled"] | true));
                FastBee::setFixedJsonField(topicObj, "autoPrefix", v["autoPrefix"] | true);
                FastBee::setFixedJsonField(topicObj, "topicType", topicType);
            }
        }
    }
    // 如果文件中完全没有 publishTopics 字段，初始化一个空数组
    if (doc["mqtt"]["publishTopics"].isNull()) {
        doc["mqtt"]["publishTopics"].to<JsonArray>();
    }

    // 订阅主题配置（同上：仅在前端发送了数据时才清空重建）
    String subscribeTopicsJson = GP("mqtt_subscribeTopics", "[]");
    if (hasProtocolParam(request, "mqtt_subscribeTopics") && subscribeTopicsJson.length() > 2) {
        JsonArray subscribeTopics = doc["mqtt"]["subscribeTopics"].to<JsonArray>();
        JsonDocument subTopicsDoc;
        DeserializationError err = deserializeJson(subTopicsDoc, subscribeTopicsJson.c_str(), subscribeTopicsJson.length());
        if (!err && subTopicsDoc.is<JsonArray>()) {
            JsonArray arr = subTopicsDoc.as<JsonArray>();
            for (JsonVariant v : arr) {
                const int topicType = v["topicType"] | 1;
                JsonObject topicObj = subscribeTopics.add<JsonObject>();
                FastBee::setFixedJsonField(topicObj, "topic", v["topic"] | "");
                FastBee::setFixedJsonField(topicObj, "qos", v["qos"] | 0);
                FastBee::setFixedJsonField(topicObj, "enabled", topicType == MQTT_TOPIC_TYPE_OTA_BINARY
                    ? false
                    : (v["enabled"] | true));
                FastBee::setFixedJsonField(topicObj, "autoPrefix", v["autoPrefix"] | true);
                FastBee::setFixedJsonField(topicObj, "topicType", topicType);
            }
        }
    }
    if (doc["mqtt"]["subscribeTopics"].isNull()) {
        doc["mqtt"]["subscribeTopics"].to<JsonArray>();
    }
    // Normalize the arrays after rebuilding them from the submitted form.
    // Otherwise an empty/unrelated type=5 row can suppress the FastBee pair
    // until the next GET/restart, while the just-saved runtime config is wrong.
    ensureFastBeeOtaTopics(doc["mqtt"].as<JsonObject>());
    disableUnsupportedBinaryOtaTopics(doc["mqtt"].as<JsonObject>());
    } // end: updateMqtt

    #undef GP
    #undef GPI

    // 让出 CPU 时间，避免文件写入阻塞 async_tcp 任务太久
    yield();

    if (!ConfigStorage::getInstance().saveProtocolConfig(doc)) {
        ctx->sendError(request, 500, "Failed to save protocol config");
        return;
    }
#if FASTBEE_ENABLE_REMOTE_RESOURCES
    // MQTT callbacks take the source mutex while holding the transport mutex.
    // Release the configuration transaction before any protocol lifecycle call.
    remoteConfigTransaction.release();
#endif

    // 让出 CPU 时间，避免文件 I/O + 后续协议重启累计阻塞太久
    yield();

    // 用户显式保存配置后，删除测试连接的 MQTT 备份文件
    // （测试连接的自动恢复机制不应覆盖用户显式保存的配置）
    if (updateMqtt && LittleFS.exists("/config/mqtt-backup.json")) {
        LittleFS.remove("/config/mqtt-backup.json");
    }

    // 保存成功后，根据MQTT启用状态处理
    // 策略：MQTT 配置变更优先热重建（loopTask 中异步执行），避免不必要的设备重启；
    // restartMQTTDeferred 内部按 scheme/PSRAM 实测内存门槛（含释放旧客户端后的
    // 最大连续块检查），门槛不过时由 ProtocolManager::handle() 回退调度设备重启，
    // 确保新配置必定生效且不会在碎片化堆上强行 TLS 握手导致长期不可用
    bool mqttHotRestart = false;
    bool mqttStoppedNow = false;
    bool mqttStopDeferred = false;
#if FASTBEE_ENABLE_MQTT
    if (updateMqtt) {
        bool mqttEnabledNow = doc["mqtt"]["enabled"].as<bool>();
        FastBeeFramework* fw = FastBeeFramework::getInstance();
        if (fw && fw->getHealthMonitor()) {
            fw->getHealthMonitor()->clearMqttMemoryOverrideState();
        }
        ProtocolManager* pm = ctx->protocolManager;
        if (!mqttEnabledNow) {
            // MQTT 已禁用：立即停止客户端即可，无需重启也无需重建
            if (pm) {
                mqttStoppedNow = pm->stopMQTT();
                mqttStopDeferred = !mqttStoppedNow;
            }
        } else if (pm) {
            // MQTT 启用：置标志由 loopTask 热重建（避免在 async_tcp 小栈里重建 TLS）
            pm->requestMqttRestartAsync();
            mqttHotRestart = true;
        } else {
            // 无 ProtocolManager（不应发生）：保底走设备重启确保配置生效
            SystemRebooter::scheduleConfigReboot("MQTT config changed");
        }
    }
#endif

    // Re-saving an identical document must not destroy a motor reference.
    // Compare with the saved pre-update section, including order-independent objects.
    bool modbusRestarted = false;
#if FASTBEE_ENABLE_MODBUS
    const bool modbusChanged = updateModbusRtu &&
        previousModbusRtu.as<JsonVariantConst>() != doc["modbusRtu"].as<JsonVariantConst>();
    if (modbusChanged && doc["modbusRtu"]["enabled"].as<bool>()) {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            modbusRestarted = pm->restartModbusDeferred();
        }
    } else if (modbusChanged) {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            pm->stopModbus();
        }
    }
#endif

    JsonDocument resp;
    resp["success"] = true;
    resp["message"] = "Protocol configuration saved";
    resp["data"]["restartRequired"] = false;
    resp["data"]["modbusRestarted"] = modbusRestarted;
    // MQTT 热重建/断开状态：前端依赖这些字段显示正确的通知
    if (mqttHotRestart) {
        resp["data"]["mqttHotRestart"] = true;
        // requestMqttRestartAsync() only queues work for loopTask.  Return a
        // stable pending/reason envelope so the UI can distinguish an
        // accepted asynchronous restart from an immediate connection failure.
        resp["data"]["mqttStatusPending"] = true;
        resp["data"]["mqttStatusReason"] = "MQTT 配置已应用，正在后台连接";
#if FASTBEE_ENABLE_MQTT
        if (ctx->protocolManager) {
            auto mqtt = ctx->protocolManager->acquireMQTTClient();
            if (mqtt && mqtt->getLastErrorCode() != 0) {
                String mqttErrorMessage;
                resp["data"]["mqttLastErrorCode"] = mqtt->getLastErrorCode();
                mqttErrorMessage = mqtt->getLastErrorCode() == -4
                    ? "连接 MQTT Broker 超时，请检查地址和端口"
                    : (mqtt->getLastErrorCode() == -2
                        ? "无法连接 MQTT Broker，请检查 DNS、网络和端口"
                        : "MQTT 连接失败，请查看状态详情");
                resp["data"]["mqttLastErrorMessage"] = mqttErrorMessage;
                resp["data"]["mqttStatusReason"] = mqttErrorMessage;
            }
        }
#endif
    }
    if (mqttStoppedNow) {
        resp["data"]["mqttDisconnected"] = true;
    }
    if (mqttStopDeferred) {
        resp["data"]["mqttStatusPending"] = true;
        resp["data"]["mqttStopPending"] = true;
        resp["data"]["mqttStatusReason"] = "MQTT 停止已排队，等待在途操作结束";
    }
    // 如果 clientId 是自动生成的，返回给前端
    if (clientIdGenerated) {
        resp["data"]["mqttClientId"] = clientId;
    }
    HandlerUtils::sendJsonStream(request, resp);
}
