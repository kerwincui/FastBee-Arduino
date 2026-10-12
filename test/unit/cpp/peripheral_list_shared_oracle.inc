void PeripheralRouteHandler::handleGetPeripherals(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    PeripheralManager& pm = PeripheralManager::getInstance();

    // 如果传了 id 参数，只返回单条外设（避免编辑时加载全部导致内存不足）
    if (request->hasParam("id")) {
        String periphId = request->getParam("id")->value();
        PeripheralConfig config;
        if (!pm.getPeripheralSnapshot(periphId, config)) {
            request->send(404, "application/json", "{\"success\":false,\"message\":\"Peripheral not found\"}");
            return;
        }

        JsonDocument doc;
        doc["id"] = config.id;
        doc["name"] = config.name;
        doc["type"] = static_cast<int>(config.type);
        doc["typeName"] = getPeripheralTypeName(config.type);
        doc["category"] = getCategoryName(getPeripheralCategory(config.type));
        doc["enabled"] = config.enabled;
        addPeripheralDiagnosticCapability(doc.as<JsonObject>(), config);
        addPassiveBuzzerFlag(doc, config);

        if (config.type == PeripheralType::UART) {
            JsonObject params = doc["params"].to<JsonObject>();
            addUartParams(params, config);
        } else if (config.type == PeripheralType::ENCODER) {
            JsonObject params = doc["params"].to<JsonObject>();
            addEncoderParams(params, config);
        } else if (isAnalogInputPeripheral(config.type)) {
            JsonObject params = doc["params"].to<JsonObject>();
            // 列表只带常用字段；详细接口再返回校准参数，控制响应体大小。
            addAnalogInputParams(params, config, false);
        } else if (isDigitalInputPeripheral(config.type)) {
            JsonObject params = doc["params"].to<JsonObject>();
            addDigitalInputParams(params, config);
        } else if (config.type == PeripheralType::PWM_SERVO) {
            JsonObject params = doc["params"].to<JsonObject>();
            addServoParams(params, config);
        } else if (config.type == PeripheralType::SENSOR_GENERIC ||
                   config.type == PeripheralType::SDIO ||
                   config.type == PeripheralType::DS1302_RTC ||
                   config.type == PeripheralType::LCD1602 ||
                   config.type == PeripheralType::RFID_MFRC522) {
            JsonObject params = doc["params"].to<JsonObject>();
            addP1PeripheralParams(params, config);
        }
        addGenericPeripheralFields(doc.as<JsonObject>(), config);

        JsonArray pins = doc["pins"].to<JsonArray>();
        for (int i = 0; i < config.pinCount && i < 8; i++) {
            if (config.pins[i] != 255) {
                pins.add(config.pins[i]);
            }
        }

        PeripheralRuntimeState runtimeState;
        if (pm.getRuntimeStateSnapshot(periphId, runtimeState)) {
            doc["status"] = static_cast<int>(runtimeState.status);
            doc["memorySuspended"] = runtimeState.memorySuspended;
            if (config.type == PeripheralType::UART) {
                addUartRuntime(doc.as<JsonObject>(), runtimeState, config.enabled);
            }
            if (config.type == PeripheralType::ENCODER) {
                EncoderReading reading;
                if (pm.getEncoderReading(periphId, reading)) {
                    addEncoderReading(doc.as<JsonObject>(), reading);
                }
            }
        } else {
            doc["status"] = 0;
            doc["memorySuspended"] = false;
        }

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", "{\"success\":true,\"data\":" + response + "}");
        return;
    }

    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Peripheral list", MemoryGuardLevel::CRITICAL, 5)) {
        return;
    }

    String typeFilter = ctx->getParamValue(request, "type", "");
    String categoryFilter = ctx->getParamValue(request, "category", "");
    bool enabledOnly = ctx->getParamValue(request, "enabledOnly", "0") == "1";
    bool requestedCompact = ctx->getParamValue(request, "compact", "0") == "1";
    bool degraded = shouldForceCompactList();
    bool compact = requestedCompact || degraded;

    // 使用指针向量代替深拷贝，大幅减少内存占用（38个外设：指针~152B vs 拷贝~6KB）
    std::vector<const PeripheralConfig*> ptrs;
    ptrs.reserve(40);  // 预估外设数量

    if (!typeFilter.isEmpty()) {
        PeripheralType type = parsePeripheralType(typeFilter.c_str());
        pm.forEachPeripheral([&ptrs, type, enabledOnly](const PeripheralConfig& p) {
            if (p.type == type &&
                p.type != PeripheralType::MODBUS_DEVICE &&
                (!enabledOnly || p.enabled)) {
                ptrs.push_back(&p);
            }
        });
    } else if (!categoryFilter.isEmpty()) {
        PeripheralCategory category = PeripheralCategory::CATEGORY_GPIO;
        if (categoryFilter == "communication") category = PeripheralCategory::CATEGORY_COMMUNICATION;
        else if (categoryFilter == "gpio") category = PeripheralCategory::CATEGORY_GPIO;
        else if (categoryFilter == "analog") category = PeripheralCategory::CATEGORY_ANALOG_SIGNAL;
        else if (categoryFilter == "debug") category = PeripheralCategory::CATEGORY_DEBUG;
        else if (categoryFilter == "special") category = PeripheralCategory::CATEGORY_SPECIAL;
        pm.forEachPeripheral([&ptrs, category, enabledOnly](const PeripheralConfig& p) {
            if (getPeripheralCategory(p.type) == category &&
                p.type != PeripheralType::MODBUS_DEVICE &&
                (!enabledOnly || p.enabled)) {
                ptrs.push_back(&p);
            }
        });
    } else {
        pm.forEachPeripheral([&ptrs, enabledOnly](const PeripheralConfig& p) {
            if (p.type != PeripheralType::MODBUS_DEVICE &&
                (!enabledOnly || p.enabled)) {
                ptrs.push_back(&p);
            }
        });
    }

    // 排序：启用的排前面，然后按名称排序
    std::sort(ptrs.begin(), ptrs.end(), [](const PeripheralConfig* a, const PeripheralConfig* b) {
        if (a->enabled != b->enabled) return a->enabled > b->enabled;
        return strcmp(a->name.c_str(), b->name.c_str()) < 0;
    });

    int total = ptrs.size();

    // 解析分页参数
    int page = 1;
    int pageSize = 10;
    if (request->hasParam("page")) {
        page = request->getParam("page")->value().toInt();
        if (page < 1) page = 1;
    }
    if (request->hasParam("pageSize")) {
        pageSize = request->getParam("pageSize")->value().toInt();
        if (pageSize < 1) pageSize = 1;
        if (pageSize > (compact ? 50 : 20)) pageSize = compact ? 50 : 20;
    }

    // 分页步长始终与前端请求保持一致，避免数据间隙（items被跳过无法显示）。
    // 内存不足时由 compact 模式减少每项序列化字段来节省空间，而非减少条数。
    // 如果连单条序列化都无法完成，由下方 isListMemoryCriticallyLow() 返回 503。
    int effectivePageSize = pageSize;

    // Deletion/filter changes can remove the requested page. Clamp before
    // multiplying to also avoid overflow from an excessively large page.
    page = std::min(page, std::max(1, (total + effectivePageSize - 1) / effectivePageSize));

    // 计算分页范围（统一使用 effectivePageSize）
    int startIdx = (page - 1) * effectivePageSize;
    int endIdx = (startIdx < total) ? std::min(startIdx + effectivePageSize, total) : startIdx;

    // Chunked responses can operate with small contiguous blocks. Only reject
    // when even per-item scratch strings are likely to fail.
    if (isListMemoryCriticallyLow()) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Low memory: heap=%lu maxAlloc=%lu",
                 (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
        HandlerUtils::sendJsonError(request, 503, msg, 5);
        return;
    }
    
    // 两遍法：先序列化每个项计算精确大小，再用精确 reserve 构建响应
    int itemCount = endIdx - startIdx;
    
    // 第一遍：序列化所有项到临时 String，累计精确大小
    std::vector<String> serializedItems;
    serializedItems.reserve(itemCount);
    size_t totalItemsSize = 0;
    
    for (int i = startIdx; i < endIdx; i++) {
        const auto& config = *ptrs[i];
        JsonDocument itemDoc;
        itemDoc["id"] = config.id;
        itemDoc["name"] = config.name;
        itemDoc["type"] = static_cast<int>(config.type);
        itemDoc["typeName"] = getPeripheralTypeName(config.type);
        itemDoc["category"] = getCategoryName(getPeripheralCategory(config.type));
        itemDoc["enabled"] = config.enabled;
        addPeripheralDiagnosticCapability(itemDoc.as<JsonObject>(), config);
        addPassiveBuzzerFlag(itemDoc, config);

        if (!compact) {
            JsonArray pins = itemDoc["pins"].to<JsonArray>();
            for (int j = 0; j < config.pinCount && j < 8; j++) {
                if (config.pins[j] != 255) {
                    pins.add(config.pins[j]);
                }
            }

            if (config.type == PeripheralType::UART) {
                JsonObject params = itemDoc["params"].to<JsonObject>();
                addUartParams(params, config);
            } else if (config.type == PeripheralType::ENCODER) {
                JsonObject params = itemDoc["params"].to<JsonObject>();
                addEncoderParams(params, config);
            } else if (isAnalogInputPeripheral(config.type)) {
                JsonObject params = itemDoc["params"].to<JsonObject>();
                addAnalogInputParams(params, config, false);
            } else if (isDigitalInputPeripheral(config.type)) {
                JsonObject params = itemDoc["params"].to<JsonObject>();
                addDigitalInputParams(params, config);
            } else if (config.type == PeripheralType::PWM_SERVO) {
                JsonObject params = itemDoc["params"].to<JsonObject>();
                addServoParams(params, config);
            } else if (config.type == PeripheralType::SENSOR_GENERIC ||
                       config.type == PeripheralType::SDIO ||
                       config.type == PeripheralType::DS1302_RTC ||
                       config.type == PeripheralType::LCD1602 ||
                       config.type == PeripheralType::RFID_MFRC522) {
                JsonObject params = itemDoc["params"].to<JsonObject>();
                addP1PeripheralParams(params, config);
            }

            PeripheralRuntimeState runtimeState;
            if (pm.getRuntimeStateSnapshot(config.id, runtimeState)) {
                itemDoc["status"] = static_cast<int>(runtimeState.status);
                itemDoc["memorySuspended"] = runtimeState.memorySuspended;
                if (config.type == PeripheralType::UART) {
                    addUartRuntime(itemDoc.as<JsonObject>(), runtimeState, config.enabled);
                }
                if (config.type == PeripheralType::ENCODER) {
                    JsonObject encoder = itemDoc["reading"].to<JsonObject>();
                    FastBee::setFixedJsonField(encoder, "count", runtimeState.state.encoder.count);
                    FastBee::setFixedJsonField(encoder, "delta", runtimeState.state.encoder.delta);
                    FastBee::setFixedJsonField(encoder, "direction", runtimeState.state.encoder.direction);
                    FastBee::setFixedJsonField(encoder, "frequencyHz", runtimeState.state.encoder.frequencyHz);
                    FastBee::setFixedJsonField(encoder, "rpm", runtimeState.state.encoder.rpm);
                }
            } else {
                itemDoc["status"] = 0;
                itemDoc["memorySuspended"] = false;
            }
        }

        addGenericPeripheralFields(itemDoc.as<JsonObject>(), config);
        String itemStr;
        serializeJson(itemDoc, itemStr);
        totalItemsSize += itemStr.length();
        serializedItems.push_back(std::move(itemStr));
    }
    
     // ============ Chunked 流式响应（复用 HandlerUtils::sendJsonListChunked）============
    // 旧方案：先构建一个 totalJsonSize 大小的连续 String（~10-20KB），对碎片化堆极不友好。
    // 新方案：items 已在 vector<String> 中，按 index 按需复制到 TCP 发送缓冲（~256B），
    //         堆峰 = items 合计（~6KB），不再产生额外大 String。
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 4096) {
        LOG_ERRORF("[Periph] Low heap for chunked response: free=%u", (unsigned)freeHeap);
        HandlerUtils::sendJsonError(request, 503, "Low memory for response");
        return;
    }

    // 构建 header：{"success":true,"total":N,"page":P,"pageSize":S,"data":[
    const int profileMax = static_cast<int>(FastBee::ResourceProfile::MAX_PERIPHERALS);
    const int profileUsed = static_cast<int>(pm.getPeripheralCount());
    const int profileRemaining = std::max(0, profileMax - profileUsed);
    const bool runtimeDegraded = HandlerUtils::memoryDegradationActive();

    String header;
    header.reserve(420);
    header = F("{\"success\":true,\"total\":");
    header += String(total);
    header += F(",\"page\":");
    header += String(page);
    header += F(",\"pageSize\":");
    header += String(effectivePageSize);
    if (degraded || runtimeDegraded) {
        header += F(",\"degraded\":true");
    }
    HandlerUtils::appendMemoryDegradationStatus(header);
    header += F(",\"profile\":{\"name\":\"");
    header += FastBee::ResourceProfile::NAME;
    header += F("\",\"max\":");
    header += String(profileMax);
    header += F(",\"used\":");
    header += String(profileUsed);
    header += F(",\"remaining\":");
    header += String(profileRemaining);
    header += F("}");
    header += F(",\"data\":[");

    if (!HandlerUtils::sendJsonListChunked(request, header, std::move(serializedItems))) {
        LOG_ERRORF("[Periph] beginChunkedResponse failed, heap=%u", (unsigned)ESP.getFreeHeap());
        HandlerUtils::sendJsonError(request, 503, "Failed to create streaming response");
    }
}
