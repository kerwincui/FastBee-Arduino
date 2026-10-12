// Frozen pre-sharing complete resident NetworkManager methods; captured 2026-10-04.

bool FBNetworkManager::saveNetworkConfig() {
    // 构建 JSON内容共用
    JsonDocument doc;
    doc["version"] = 1;
    doc["mode"] = static_cast<uint8_t>(wifiConfig.mode);
    // deviceName 已移至设备配置(device.json)，不再保存到 network.json
    doc["apSSID"] = wifiConfig.apSSID;
    doc["apPassword"] = wifiConfig.apPassword;
    doc["apIP"] = wifiConfig.apIP;
    doc["apChannel"] = wifiConfig.apChannel;
    doc["apHidden"] = wifiConfig.apHidden;
    doc["apMaxConnections"] = wifiConfig.apMaxConnections;
    doc["staSSID"] = wifiConfig.staSSID;
    doc["staPassword"] = wifiConfig.staPassword;
    doc["ipConfigType"] = static_cast<uint8_t>(wifiConfig.ipConfigType);
    doc["staticIP"] = wifiConfig.staticIP;
    doc["gateway"] = wifiConfig.gateway;
    doc["subnet"] = wifiConfig.subnet;
    doc["dns1"] = wifiConfig.dns1;
    doc["dns2"] = wifiConfig.dns2;
    doc["connectTimeout"] = wifiConfig.connectTimeout;
    doc["reconnectInterval"] = wifiConfig.reconnectInterval;
    doc["maxReconnectAttempts"] = wifiConfig.maxReconnectAttempts;
    doc["customDomain"] = wifiConfig.customDomain;
    doc["enableMDNS"] = wifiConfig.enableMDNS;
    doc["conflictDetection"] = static_cast<uint8_t>(wifiConfig.conflictDetection);
    doc["failoverStrategy"] = static_cast<uint8_t>(wifiConfig.failoverStrategy);
    doc["autoFailover"] = wifiConfig.autoFailover;
    doc["conflictCheckInterval"] = wifiConfig.conflictCheckInterval;
    doc["maxFailoverAttempts"] = wifiConfig.maxFailoverAttempts;
    doc["conflictThreshold"] = wifiConfig.conflictThreshold;
    doc["fallbackToDHCP"] = wifiConfig.fallbackToDHCP;

    // 联网方式
    doc["networkType"] = static_cast<uint8_t>(wifiConfig.networkType);

    // 以太网配置
    JsonObject ethObj = doc["ethernet"].to<JsonObject>();
    ethObj["spiMosi"] = wifiConfig.ethernet.spiMosi;
    ethObj["spiMiso"] = wifiConfig.ethernet.spiMiso;
    ethObj["spiSck"]  = wifiConfig.ethernet.spiSck;
    ethObj["csPin"]   = wifiConfig.ethernet.csPin;
    ethObj["rstPin"]  = wifiConfig.ethernet.rstPin;
    ethObj["intPin"]  = wifiConfig.ethernet.intPin;

    // 4G 蜂窝配置
    JsonObject cellObj = doc["cellular"].to<JsonObject>();
    cellObj["txPin"]    = wifiConfig.cellular.txPin;
    cellObj["rxPin"]    = wifiConfig.cellular.rxPin;
    cellObj["pwrPin"]   = wifiConfig.cellular.pwrPin;
    cellObj["baudRate"] = wifiConfig.cellular.baudRate;
    cellObj["apn"]      = wifiConfig.cellular.apn;

    // ===== 写入 /config/network.json =====
    const bool jsonSaved = ConfigStorage::getInstance().saveJSONConfig(NETWORK_CONFIG_FILE, doc);
    if (jsonSaved) {
        LOG_INFO("NetworkManager: Config saved to /config/network.json");
    } else {
        LOG_WARNING("NetworkManager: Failed to write /config/network.json");
        // Keep the prior NVS generation aligned with the prior JSON file.  A
        // failed request must not become successful later through NVS fallback.
        return false;
    }

    // ===== 同步写入 NVS（备份） =====
    // 基本配置
    preferences.putUInt("mode", static_cast<uint8_t>(wifiConfig.mode));
    // deviceName 已移至设备配置(device.json)，不再保存到 NVS

        // AP配置
        preferences.putString("ap_ssid", wifiConfig.apSSID);
        preferences.putString("ap_password", wifiConfig.apPassword);
        preferences.putUChar("ap_channel", wifiConfig.apChannel);
        preferences.putBool("ap_hidden", wifiConfig.apHidden);
        preferences.putUChar("ap_max_conn", wifiConfig.apMaxConnections);

        // STA配置
        preferences.putString("sta_ssid", wifiConfig.staSSID);
        preferences.putString("sta_password", wifiConfig.staPassword);
        preferences.putUInt("ip_config", static_cast<uint8_t>(wifiConfig.ipConfigType));

        // 静态IP配置
        preferences.putString("static_ip", wifiConfig.staticIP);
        preferences.putString("gateway", wifiConfig.gateway);
        preferences.putString("subnet", wifiConfig.subnet);
        preferences.putString("dns1", wifiConfig.dns1);
        preferences.putString("dns2", wifiConfig.dns2);

        // IP冲突检测配置
        preferences.putUInt("conf_detect", 
            static_cast<uint8_t>(wifiConfig.conflictDetection));
        preferences.putUInt("fail_strategy", 
            static_cast<uint8_t>(wifiConfig.failoverStrategy));
        preferences.putBool("auto_failover", wifiConfig.autoFailover);
        preferences.putUShort("conf_chk_intv", 
            wifiConfig.conflictCheckInterval);
        preferences.putUChar("max_fail_try", 
            wifiConfig.maxFailoverAttempts);
        preferences.putUChar("conf_thresh", 
            wifiConfig.conflictThreshold);
        preferences.putBool("fallbk_dhcp", wifiConfig.fallbackToDHCP);

        // 保存备用IP列表
        preferences.putUInt("backup_ip_count", wifiConfig.backupIPs.size());
        for (size_t i = 0; i < wifiConfig.backupIPs.size(); i++) {
            char key[20];
            snprintf(key, sizeof(key), "backup_ip_%d", i);
            preferences.putString(key, wifiConfig.backupIPs[i]);
        }

        // 高级配置
        preferences.putULong("connect_timeout", wifiConfig.connectTimeout);
        preferences.putULong("reconn_intv", wifiConfig.reconnectInterval);
        preferences.putUChar("max_reconnect", wifiConfig.maxReconnectAttempts);

        // 域名配置
        preferences.putString("custom_domain", wifiConfig.customDomain);
        preferences.putBool("enable_mdns", wifiConfig.enableMDNS);

        preferences.putBool("initialized", true);
        // 注意：不得在此 end()——Preferences 仅在 initialize() 中 begin 一次，
        // 提前 end 会使同一启动周期内后续所有 NVS 写入（含 ensureLastResortAP
        // 的 init_fail_cnt 与二次保存的备份）静默失败，破坏 JSON→NVS→默认 回退链

    LOG_INFO("NetworkManager: Configuration saved successfully");
    return jsonSaved;
}

String FBNetworkManager::scanNetworks() {
    // 使用异步扫描 + 定时 yield，避免阻塞 Web 服务器
    int numNetworks = WiFi.scanNetworks(true);  // async=true
    if (numNetworks == WIFI_SCAN_RUNNING || numNetworks == WIFI_SCAN_FAILED) {
        // 已有扫描在进行或失败，尝试同步等待
        unsigned long startMs = millis();
        while (millis() - startMs < 8000) {
            delay(10);
            numNetworks = WiFi.scanComplete();
            if (numNetworks >= 0) break;
        }
    } else {
        // 异步扫描已启动，等待完成（每 10ms yield，让出 CPU 给 Web 服务器）
        unsigned long startMs = millis();
        while (millis() - startMs < 8000) {
            delay(10);
            numNetworks = WiFi.scanComplete();
            if (numNetworks >= 0) break;
        }
    }

    if (numNetworks < 0) numNetworks = 0;

    // 使用静态JSON文档减少内存碎片
    static char buffer[2048];
    StaticJsonDocument<2048> doc;
    JsonArray networks = doc.to<JsonArray>();
    
    for (int i = 0; i < numNetworks; i++) {
        JsonObject network = networks.createNestedObject();
        network["ssid"] = WiFi.SSID(i);
        network["rssi"] = WiFi.RSSI(i);
        network["strength"] = NetworkUtils::rssiToPercentage(WiFi.RSSI(i));
        network["channel"] = WiFi.channel(i);
        // 返回具体加密类型，便于前端自动匹配安全类型下拉选项
        wifi_auth_mode_t authMode = WiFi.encryptionType(i);
        const char* enc;
        switch (authMode) {
            case WIFI_AUTH_OPEN:             enc = "open"; break;
            case WIFI_AUTH_WEP:              enc = "wep";  break;
            case WIFI_AUTH_WPA_PSK:          enc = "wpa";  break;
            case WIFI_AUTH_WPA2_PSK:         enc = "wpa2"; break;
            case WIFI_AUTH_WPA_WPA2_PSK:     enc = "wpa2"; break;
#if defined(WIFI_AUTH_WPA3_PSK)
            case WIFI_AUTH_WPA3_PSK:         enc = "wpa3"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK:    enc = "wpa3"; break;
#endif
            default:                         enc = "wpa2"; break;
        }
        network["encryption"] = enc;
        network["bssid"] = WiFi.BSSIDstr(i);
    }
    WiFi.scanDelete();  // 释放扫描结果内存
    
    // 序列化到静态缓冲区（审计 P2-6：预留 '\0' 位，截断时不越界写）
    size_t len = serializeJson(doc, buffer, sizeof(buffer) - 1);
    buffer[len] = '\0';
    return String(buffer);
}

