    // Device time
    server->on("/api/device/time", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->requireAuth(request)) return;
        JsonDocument doc;
            
        // 检查网络状态
        bool internetAvailable = false;
        if (ctx->networkManager) {
            FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
            netMgr->updateStatusInfo();
            NetworkStatusInfo netInfo = netMgr->getStatusInfo();
            internetAvailable = netInfo.internetAvailable;
        }
            
        doc["success"] = true;
        FastBee::appendSystemTimeStatus(doc["data"].as<JsonObject>());
        doc["data"]["internetAvailable"] = internetAvailable;
        doc["data"]["uptime"] = millis();
        doc["data"]["timezone"] = "CST-8";
        if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
            File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
            if (f) {
                JsonDocument cfg;
                if (deserializeJson(cfg, f) == DeserializationError::Ok) {
                    if (cfg["timezone"].is<String>()) doc["data"]["timezone"] = cfg["timezone"].as<String>();
                }
                f.close();
            }
        }
        HandlerUtils::sendJsonStream(request, doc);
    });

    server->on("/api/device/time/sync", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        if (!ctx->requireAuth(request)) return;
            
        // 检查网络状态
        bool internetAvailable = false;
        if (ctx->networkManager) {
            FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
            // 刷新底层网络状态，避免刚联网时使用旧的 internetAvailable=false 阻止手动同步。
            netMgr->updateStatusInfo();
            NetworkStatusInfo netInfo = netMgr->getStatusInfo();
            internetAvailable = netInfo.internetAvailable;
        }
            
        // 如果网络不可用，直接返回失败
        if (!internetAvailable) {
            JsonDocument doc;
            doc["success"] = false;
            doc["error"] = "No internet connection";
            FastBee::appendSystemTimeStatus(doc["data"].as<JsonObject>());
            doc["data"]["syncRequestSucceeded"] = false;
            doc["data"]["internetAvailable"] = false;
            doc["data"]["uptime"] = millis();
            HandlerUtils::sendJsonStream(request, doc);
            return;
        }
            
        String ntpServer1 = "cn.pool.ntp.org";
        String tz = "CST-8";
        if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
            File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
            if (f) {
                JsonDocument cfg;
                if (deserializeJson(cfg, f) == DeserializationError::Ok) {
                    if (cfg["ntpServer1"].is<String>()) ntpServer1 = cfg["ntpServer1"].as<String>();
                    if (cfg["timezone"].is<String>()) tz = cfg["timezone"].as<String>();
                }
                f.close();
            }
        }
        bool synced = false;
        if (ntpServer1.startsWith("http://") || ntpServer1.startsWith("https://")) {
            long long ts = 0;
            synced = FastBee::SystemTimeZone::setPosix(tz.c_str()) &&
                TimeUtils::syncNTPFromHTTPWithTimestamp(ntpServer1, ts, 5000);
        } else {
            FastBee::SystemTimeQuality::installSntpObserver();
            const uint32_t beforeSync = FastBee::SystemTimeQuality::sequence();
            synced = FastBee::SystemTimeZone::startSntp(tz.c_str(), ntpServer1.c_str(), "time.nist.gov") &&
                TimeUtils::syncNTP(5000, beforeSync);
        }
        JsonDocument doc;
        doc["success"] = true;
        FastBee::appendSystemTimeStatus(doc["data"].as<JsonObject>());
        doc["data"]["syncRequestSucceeded"] = synced;
        doc["data"]["internetAvailable"] = internetAvailable;
        doc["data"]["uptime"] = millis();
        if (synced) {
            LOGGER.info("NTP sync triggered via web");
        }
        HandlerUtils::sendJsonStream(request, doc);
    });
