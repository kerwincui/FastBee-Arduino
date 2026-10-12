bool OracleNetworkSerializer::wifiConfigToJson(const WiFiConfig& config, JsonObject& jsonObj) {
    // 基本配置
    // deviceName 已移至设备配置(device.json)
    jsonObj["mode"] = static_cast<uint8_t>(config.mode);
    jsonObj["apSSID"] = config.apSSID;
    jsonObj["apPassword"] = config.apPassword;
    jsonObj["apChannel"] = config.apChannel;
    jsonObj["apHidden"] = config.apHidden;
    jsonObj["staSSID"] = config.staSSID;
    jsonObj["staPassword"] = config.staPassword;
    
    // IP配置
    jsonObj["ipConfigType"] = static_cast<uint8_t>(config.ipConfigType);
    jsonObj["staticIP"] = config.staticIP;
    jsonObj["gateway"] = config.gateway;
    jsonObj["subnet"] = config.subnet;
    jsonObj["dns1"] = config.dns1;
    jsonObj["dns2"] = config.dns2;
    
    // 高级配置
    jsonObj["enableMDNS"] = config.enableMDNS;
    jsonObj["customDomain"] = config.customDomain;
    
    // 备用IP列表
    JsonArray backupIPs = jsonObj.createNestedArray("backupIPs");
    for (const String& ip : config.backupIPs) {
        backupIPs.add(ip);
    }
    
    return true;
}

void OracleNetworkSerializer::networkStatusToJson(const NetworkStatusInfo& status, JsonObject& jsonObj) {
    jsonObj["status"] = static_cast<uint8_t>(status.status);
    jsonObj["statusText"] = networkStatusToString(status.status);
    jsonObj["ipAddress"] = status.ipAddress;
    jsonObj["macAddress"] = status.macAddress;
    jsonObj["ssid"] = status.ssid;
    jsonObj["rssi"] = status.rssi;
    jsonObj["gateway"] = status.currentGateway;
    jsonObj["subnet"] = status.currentSubnet;
    jsonObj["dnsServer"] = status.dnsServer;
    jsonObj["internetAvailable"] = status.internetAvailable;
    jsonObj["failoverCount"] = status.failoverCount;
    jsonObj["uptime"] = status.uptime;
    jsonObj["lastConnectionTime"] = status.lastConnectionTime;
}

String OracleNetworkSerializer::fullNetworkInfoToJson(const WiFiConfig& config, 
                                                      const NetworkStatusInfo& status,
                                                      JsonDocument& doc) {
    JsonObject root = doc.to<JsonObject>();

    // 配置部分
    JsonObject configObj = root["config"].to<JsonObject>();
    wifiConfigToJson(config, configObj);

    // 状态部分
    JsonObject statusObj = root["status"].to<JsonObject>();
    networkStatusToJson(status, statusObj);

    // 可读字符串表示
    root["modeText"]        = networkModeToString(config.mode);
    root["ipConfigTypeText"]= ipConfigTypeToString(config.ipConfigType);
    root["statusText"]      = networkStatusToString(status.status);

    // 系统信息
    root["timestamp"] = millis();
    root["freeHeap"]  = ESP.getFreeHeap();

    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

String OracleNetworkSerializer::networkModeToString(NetworkMode mode) {
    // 使用整数值进行比较，避免宏冲突
    uint8_t modeValue = static_cast<uint8_t>(mode);
    
    switch(modeValue) {
        case 0: return "站点模式";      // NETWORK_STA
        case 1: return "热点模式";      // NETWORK_AP
        default: return "未知";
    }
}

String OracleNetworkSerializer::ipConfigTypeToString(IPConfigType type) {
    switch(type) {
        case IPConfigType::DHCP: return "DHCP";
        case IPConfigType::STATIC: return "静态IP";
        default: return "未知";
    }
}

String OracleNetworkSerializer::networkStatusToString(NetworkStatus status) {
    switch(status) {
        case NetworkStatus::DISCONNECTED: return "未连接";
        case NetworkStatus::CONNECTING: return "连接中";
        case NetworkStatus::CONNECTED: return "已连接";
        case NetworkStatus::AP_MODE: return "热点模式";
        case NetworkStatus::CONNECTION_FAILED: return "连接失败";
        case NetworkStatus::IP_CONFLICT: return "IP冲突";
        case NetworkStatus::FAILOVER_IN_PROGRESS: return "故障转移中";
        default: return "未知";
    }
}
