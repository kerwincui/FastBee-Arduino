/**
 * @file NetworkManager.cpp
 * @brief 网络管理器实现
 * @author kerwincui
 * @date 2025-12-02
 */

#include "network/NetworkManager.h"
#include "systems/LoggerSystem.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <lwip/netif.h>
#include <lwip/etharp.h>

NetworkManager::NetworkManager(AsyncWebServer* webServerPtr) 
    : webServer(webServerPtr),
      lastReconnectAttempt(0),
      lastStatusUpdate(0),
      lastConflictCheck(0),
      lastFailoverAttempt(0),
      autoReconnectEnabled(true),
      isInitialized(false),
      dnsServerStarted(false),
      mdnsStarted(false),
      connecting(false),
      connectingStartTime(0),
      ipConflictDetected(false),
      currentFailoverAttempts(0),
      currentBackupIPIndex(0),
      conflictDetectionCount(0) {
    
    wifiConfig = WiFiConfig();
    statusInfo = NetworkStatusInfo();
}

NetworkManager::~NetworkManager() {
    disconnect();
    if (preferences.isKey("initialized")) {
        preferences.end();
    }
}

bool NetworkManager::initialize() {
    if (isInitialized) {
        return true;
    }

    // 初始化Preferences
    if (!preferences.begin(PREFERENCES_NETWORK, false)) {
        LOG_ERROR("NetworkManager: Failed to initialize preferences");
        return false;
    }

    // 加载网络配置
    if (!loadNetworkConfig()) {
        LOG_WARNING("NetworkManager: Using default network configuration");
        // 生成默认备用IP
        generateBackupIPs();
        saveNetworkConfig();
    }

    // 设置WiFi事件回调
    WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
        this->handleWiFiEvent(event);
    });

    // 根据配置启动网络
    bool success = false;
    switch (wifiConfig.mode) {
        case NetworkMode::NETWORK_STA:
            success = connectToWiFi();
            break;
        case NetworkMode::NETWORK_AP:
            success = startAPMode();
            break;
        case NetworkMode::NETWORK_AP_STA:
            success = startAPMode() && connectToWiFi();
            break;
    }

    if (success) {
        isInitialized = true;
        LOG_INFO("NetworkManager: Initialized successfully");
    } else {
        LOG_WARNING("NetworkManager: Initialization failed");
    }

    return success;
}

void NetworkManager::disconnect() {
    LOG_INFO("NetworkManager: Disconnecting all network connections...");
    
    stopDNSServer();
    stopMDNS();
    stopAPMode();
    disconnectWiFi();
    
    isInitialized = false;
    dnsServerStarted = false;
    mdnsStarted = false;
    autoReconnectEnabled = false;
    connecting = false;
    connectingStartTime = 0;
    currentFailoverAttempts = 0;
    ipConflictDetected = false;
    conflictDetectionCount = 0;
    
    // 重置状态信息
    statusInfo = NetworkStatusInfo();
    statusInfo.status = NetworkStatus::DISCONNECTED;
    
    LOG_INFO("NetworkManager: All network connections disconnected");
}

void NetworkManager::update() {
    unsigned long currentTime = millis();

    // 更新状态信息（每秒一次）
    if (currentTime - lastStatusUpdate >= 1000) {
        updateStatusInfo();
        lastStatusUpdate = currentTime;
    }

    // 处理DNS请求（如果DNS服务器已启动）
    if (dnsServerStarted) {
        dnsServer.processNextRequest();
    }

    // IP冲突检测（按配置间隔）
    if (currentTime - lastConflictCheck >= wifiConfig.conflictCheckInterval) {
        if (wifiConfig.conflictDetection != IPConflictMode::NONE && 
            statusInfo.status == NetworkStatus::CONNECTED &&
            wifiConfig.ipConfigType == IPConfigType::STATIC) {
            
            updateIPConflictStatus();
        }
        lastConflictCheck = currentTime;
    }

    // 处理连接超时
    if (connecting && (currentTime - connectingStartTime >= wifiConfig.connectTimeout)) {
        LOG_WARNING("NetworkManager: Connection timeout");
        connecting = false;
        
        if (wifiConfig.autoFailover) {
            LOG_INFO("NetworkManager: Attempting failover...");
            performFailover();
        } else {
            statusInfo.status = NetworkStatus::CONNECTION_FAILED;
            triggerEvent(NetworkStatus::CONNECTION_FAILED, "Connection timeout");
        }
    }

    // 自动重连逻辑
    if (autoReconnectEnabled && 
        (wifiConfig.mode == NetworkMode::NETWORK_STA || wifiConfig.mode == NetworkMode::NETWORK_AP_STA) &&
        !connecting &&
        (statusInfo.status == NetworkStatus::DISCONNECTED || 
         statusInfo.status == NetworkStatus::CONNECTION_FAILED) &&
        currentTime - lastReconnectAttempt >= wifiConfig.reconnectInterval) {
        
        attemptReconnect();
    }
    
    // 清理冲突缓存（每小时一次）
    if (currentTime - lastStatusUpdate >= 3600000) {
        cleanupConflictCache();
    }
}

bool NetworkManager::loadNetworkConfig() {
    if (!preferences.isKey("initialized")) {
        LOG_INFO("NetworkManager: No saved network config found");
        return false;
    }

    try {
        // 基本配置
        wifiConfig.mode = static_cast<NetworkMode>(preferences.getUInt("mode", 
            static_cast<uint8_t>(NetworkMode::NETWORK_STA)));
        wifiConfig.deviceName = preferences.getString("device_name", "FBE10000001");

        // AP配置
        wifiConfig.apSSID = preferences.getString("ap_ssid", "");
        wifiConfig.apPassword = preferences.getString("ap_password", "");
        wifiConfig.apChannel = preferences.getUChar("ap_channel", 1);
        wifiConfig.apHidden = preferences.getBool("ap_hidden", false);
        wifiConfig.apMaxConnections = preferences.getUChar("ap_max_conn", 4);

        // STA配置
        wifiConfig.staSSID = preferences.getString("sta_ssid", "CMCC-7mnN");
        wifiConfig.staPassword = preferences.getString("sta_password", "eb66bcm9");
        wifiConfig.ipConfigType = static_cast<IPConfigType>(
            preferences.getUInt("ip_config", static_cast<uint8_t>(IPConfigType::DHCP)));

        // 静态IP配置
        wifiConfig.staticIP = preferences.getString("static_ip", "");
        wifiConfig.gateway = preferences.getString("gateway", "");
        wifiConfig.subnet = preferences.getString("subnet", "");
        wifiConfig.dns1 = preferences.getString("dns1", "");
        wifiConfig.dns2 = preferences.getString("dns2", "");

        // IP冲突检测配置
        wifiConfig.conflictDetection = static_cast<IPConflictMode>(
            preferences.getUInt("conflict_detection", 
            static_cast<uint8_t>(IPConflictMode::ARP)));
        wifiConfig.failoverStrategy = static_cast<IPFailoverStrategy>(
            preferences.getUInt("failover_strategy", 
            static_cast<uint8_t>(IPFailoverStrategy::SMART)));
        wifiConfig.autoFailover = preferences.getBool("auto_failover", true);
        wifiConfig.conflictCheckInterval = preferences.getUShort(
            "conflict_check_interval", 30000);
        wifiConfig.maxFailoverAttempts = preferences.getUChar(
            "max_failover_attempts", 3);
        wifiConfig.conflictThreshold = preferences.getUChar(
            "conflict_threshold", 2);
        wifiConfig.fallbackToDHCP = preferences.getBool("fallback_to_dhcp", true);

        // 加载备用IP列表
        wifiConfig.backupIPs.clear();
        size_t backupCount = preferences.getUInt("backup_ip_count", 0);
        for (size_t i = 0; i < backupCount; i++) {
            char key[20];
            snprintf(key, sizeof(key), "backup_ip_%d", i);
            String ip = preferences.getString(key, "");
            if (ip.length() > 0 && isValidIP(ip)) {
                wifiConfig.backupIPs.push_back(ip);
            }
        }

        // 高级配置
        wifiConfig.connectTimeout = preferences.getULong("connect_timeout", 10000);
        wifiConfig.reconnectInterval = preferences.getULong("reconnect_interval", 5000);
        wifiConfig.maxReconnectAttempts = preferences.getUChar("max_reconnect", 5);

        // 域名配置
        wifiConfig.customDomain = preferences.getString("custom_domain", MDNS_HOSTNAME);
        wifiConfig.enableMDNS = preferences.getBool("enable_mdns", true);
        wifiConfig.enableDNS = preferences.getBool("enable_dns", false);

        LOG_INFO("NetworkManager: Configuration loaded successfully");
        return true;
    } catch (...) {
        LOG_ERROR("NetworkManager: Failed to load configuration");
        return false;
    }
}

bool NetworkManager::saveNetworkConfig() {
    try {
        // 基本配置
        preferences.putUInt("mode", static_cast<uint8_t>(wifiConfig.mode));
        preferences.putString("device_name", wifiConfig.deviceName);

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
        preferences.putUInt("conflict_detection", 
            static_cast<uint8_t>(wifiConfig.conflictDetection));
        preferences.putUInt("failover_strategy", 
            static_cast<uint8_t>(wifiConfig.failoverStrategy));
        preferences.putBool("auto_failover", wifiConfig.autoFailover);
        preferences.putUShort("conflict_check_interval", 
            wifiConfig.conflictCheckInterval);
        preferences.putUChar("max_failover_attempts", 
            wifiConfig.maxFailoverAttempts);
        preferences.putUChar("conflict_threshold", 
            wifiConfig.conflictThreshold);
        preferences.putBool("fallback_to_dhcp", wifiConfig.fallbackToDHCP);

        // 保存备用IP列表
        preferences.putUInt("backup_ip_count", wifiConfig.backupIPs.size());
        for (size_t i = 0; i < wifiConfig.backupIPs.size(); i++) {
            char key[20];
            snprintf(key, sizeof(key), "backup_ip_%d", i);
            preferences.putString(key, wifiConfig.backupIPs[i]);
        }

        // 高级配置
        preferences.putULong("connect_timeout", wifiConfig.connectTimeout);
        preferences.putULong("reconnect_interval", wifiConfig.reconnectInterval);
        preferences.putUChar("max_reconnect", wifiConfig.maxReconnectAttempts);

        // 域名配置
        preferences.putString("custom_domain", wifiConfig.customDomain);
        preferences.putBool("enable_mdns", wifiConfig.enableMDNS);
        preferences.putBool("enable_dns", wifiConfig.enableDNS);

        preferences.putBool("initialized", true);
        preferences.end();

        LOG_INFO("NetworkManager: Configuration saved successfully");
        return true;
    } catch (...) {
        LOG_ERROR("NetworkManager: Failed to save configuration");
        return false;
    }
}

bool NetworkManager::startAPMode() {
    if (WiFi.getMode() & WIFI_AP) {
        return true;
    }

    // 设置WiFi模式
    WiFiMode_t currentMode = WiFi.getMode();
    if (currentMode == WIFI_MODE_NULL || currentMode == WIFI_MODE_STA) {
        if (!WiFi.mode(WIFI_MODE_APSTA)) {
            LOG_ERROR("NetworkManager: Failed to set WiFi mode to AP_STA");
            return false;
        }
    } else if (!(currentMode & WIFI_AP)) {
        if (!WiFi.mode(WIFI_MODE_AP)) {
            LOG_ERROR("NetworkManager: Failed to set WiFi mode to AP");
            return false;
        }
    }

    // 配置AP参数
    String apSSID;
    if (wifiConfig.apSSID.isEmpty()) {
        apSSID = wifiConfig.deviceName;
        apSSID += "_";
        apSSID += getChipID().substring(0, 6);
    } else {
        apSSID = wifiConfig.apSSID;
    }

    if (!WiFi.softAP(apSSID.c_str(), wifiConfig.apPassword.c_str(), 
                     wifiConfig.apChannel, wifiConfig.apHidden, 
                     wifiConfig.apMaxConnections)) {
        LOG_ERROR("NetworkManager: Failed to start AP mode");
        return false;
    }

    // 配置AP网络
    IPAddress apIP(192, 168, 4, 1);
    IPAddress apGateway(192, 168, 4, 1);
    IPAddress apSubnet(255, 255, 255, 0);
    
    if (!WiFi.softAPConfig(apIP, apGateway, apSubnet)) {
        LOG_WARNING("NetworkManager: Failed to configure AP network");
    }

    // 启动DNS服务器
    if (wifiConfig.enableDNS) {
        startDNSServer();
    }

    // 启动mDNS服务
    if (wifiConfig.enableMDNS) {
        startMDNS();
    }

    statusInfo.status = NetworkStatus::AP_MODE;
    statusInfo.apIPAddress = WiFi.softAPIP().toString();

    LOG_INFO("NetworkManager: AP mode started: " + apSSID);
    LOG_INFO("NetworkManager: AP IP: " + statusInfo.apIPAddress);

    triggerEvent(NetworkStatus::AP_MODE, "AP mode started successfully");
    return true;
}

void NetworkManager::stopAPMode() {
    if (WiFi.getMode() & WIFI_AP) {
        WiFi.softAPdisconnect(true);
        stopDNSServer();
        LOG_INFO("NetworkManager: AP mode stopped");
    }
}

bool NetworkManager::connectToWiFi() {
    if (wifiConfig.staSSID.isEmpty()) {
        LOG_INFO("NetworkManager: No STA SSID configured");
        return false;
    }

    // 确保WiFi模式正确
    WiFiMode_t currentMode = WiFi.getMode();
    if (wifiConfig.mode == NetworkMode::NETWORK_STA && !(currentMode & WIFI_STA)) {
        WiFi.mode(WIFI_STA);
    } else if (wifiConfig.mode == NetworkMode::NETWORK_AP_STA && currentMode != WIFI_MODE_APSTA) {
        if (!WiFi.mode(WIFI_MODE_APSTA)) {
            LOG_ERROR("NetworkManager: Failed to set WiFi mode to AP_STA");
            return false;
        }
    }

    // 重置故障转移状态
    currentFailoverAttempts = 0;
    currentBackupIPIndex = 0;
    ipConflictDetected = false;
    conflictDetectionCount = 0;

    // 配置网络
    if (wifiConfig.ipConfigType == IPConfigType::STATIC) {
        if (!configureStaticIP()) {
            LOG_WARNING("NetworkManager: Static IP configuration failed");
            return false;
        }
    } else if (wifiConfig.ipConfigType == IPConfigType::DHCP) {
        configureDHCP();
    }

    // 设置连接状态
    statusInfo.status = NetworkStatus::CONNECTING;
    connecting = true;
    connectingStartTime = millis();

    // 开始连接
    WiFi.begin(wifiConfig.staSSID.c_str(), wifiConfig.staPassword.c_str());
    
    LOG_INFO("NetworkManager: Attempting to connect to " + wifiConfig.staSSID);
    triggerEvent(NetworkStatus::CONNECTING, "Connecting to WiFi: " + wifiConfig.staSSID);
    
    return true;
}

void NetworkManager::disconnectWiFi() {
    WiFi.disconnect(true);
    connecting = false;
    statusInfo.status = NetworkStatus::DISCONNECTED;
    stopMDNS();
    
    LOG_INFO("NetworkManager: WiFi disconnected");
    triggerEvent(NetworkStatus::DISCONNECTED, "WiFi disconnected");
}

bool NetworkManager::configureStaticIP() {
    if (wifiConfig.staticIP.isEmpty() || 
        wifiConfig.gateway.isEmpty() || 
        wifiConfig.subnet.isEmpty()) {
        LOG_WARNING("NetworkManager: Incomplete static IP configuration");
        return false;
    }

    if (!isValidIP(wifiConfig.staticIP) || 
        !isValidIP(wifiConfig.gateway) || 
        !isValidSubnet(wifiConfig.subnet)) {
        LOG_WARNING("NetworkManager: Invalid static IP configuration");
        return false;
    }

    IPAddress staticIP, gateway, subnet, dns1, dns2;
    
    if (!staticIP.fromString(wifiConfig.staticIP) ||
        !gateway.fromString(wifiConfig.gateway) ||
        !subnet.fromString(wifiConfig.subnet)) {
        LOG_WARNING("NetworkManager: Failed to parse static IP configuration");
        return false;
    }

    // 设置DNS服务器
    if (!wifiConfig.dns1.isEmpty() && dns1.fromString(wifiConfig.dns1)) {
        if (!wifiConfig.dns2.isEmpty() && dns2.fromString(wifiConfig.dns2)) {
            WiFi.config(staticIP, gateway, subnet, dns1, dns2);
        } else {
            WiFi.config(staticIP, gateway, subnet, dns1);
        }
    } else {
        WiFi.config(staticIP, gateway, subnet);
    }

    LOG_INFO("NetworkManager: Static IP configured: " + wifiConfig.staticIP);
    statusInfo.activeIPType = "Static";
    return true;
}

bool NetworkManager::configureDHCP() {
    WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
    LOG_INFO("NetworkManager: DHCP configured");
    statusInfo.activeIPType = "DHCP";
    return true;
}

bool NetworkManager::startMDNS() {
    if (mdnsStarted) {
        return true;
    }

    // 检查网络状态
    if (!(WiFi.getMode() & WIFI_STA)) {
        LOG_WARNING("NetworkManager: Cannot start mDNS - not in STA mode");
        return false;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        LOG_WARNING("NetworkManager: Cannot start mDNS - WiFi not connected");
        return false;
    }
    
    IPAddress staIP = WiFi.localIP();
    if (staIP == INADDR_NONE || staIP == IPAddress(0,0,0,0)) {
        LOG_WARNING("NetworkManager: Cannot start mDNS - no valid IP address");
        return false;
    }

    String hostname = wifiConfig.customDomain.isEmpty() ? 
                     wifiConfig.deviceName : wifiConfig.customDomain;
    
    // 清理无效字符
    hostname.replace(" ", "-");
    hostname.toLowerCase();
    
    if (!MDNS.begin(hostname.c_str())) {
        LOG_ERROR("NetworkManager: Failed to start mDNS");
        return false;
    }
    
    MDNS.setInstanceName(hostname.c_str());
    
    // 添加服务
    if (webServer) {
        MDNS.addService("http", "tcp", 80);
        // FastBee设备提供一个独特的服务标识
        MDNS.addService("fastbee", "tcp", 80);
        MDNS.addService("ws", "tcp", 81); 
        LOG_INFO("NetworkManager: Added HTTP service to mDNS");
    }
    
    mdnsStarted = true;
    LOG_INFO("NetworkManager: mDNS started: " + hostname + ".local");
    return true;
}

void NetworkManager::stopMDNS() {
    if (mdnsStarted) {
        MDNS.end();
        mdnsStarted = false;
        LOG_INFO("NetworkManager: mDNS stopped");
    }
}

bool NetworkManager::startDNSServer() {
    if (dnsServerStarted) {
        return true;
    }

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    if (!dnsServer.start(53, "*", WiFi.softAPIP())) {
        LOG_ERROR("NetworkManager: Failed to start DNS server");
        return false;
    }

    dnsServerStarted = true;
    LOG_INFO("NetworkManager: DNS server started");
    return true;
}

void NetworkManager::stopDNSServer() {
    if (dnsServerStarted) {
        dnsServer.stop();
        dnsServerStarted = false;
        LOG_INFO("NetworkManager: DNS server stopped");
    }
}

void NetworkManager::handleWiFiEvent(arduino_event_id_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            LOG_INFO("NetworkManager: WiFi STA connected");
            statusInfo.status = NetworkStatus::CONNECTING;
            connecting = true;
            break;
            
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            statusInfo.ipAddress = WiFi.localIP().toString();
            statusInfo.status = NetworkStatus::CONNECTED;
            connecting = false;
            connectingStartTime = 0;
            statusInfo.lastConnectionTime = millis();
            statusInfo.reconnectAttempts = 0;
            currentFailoverAttempts = 0;

            LOG_INFO("NetworkManager: Got IP: " + statusInfo.ipAddress);

            // 更新网络信息
            statusInfo.currentGateway = WiFi.gatewayIP().toString();
            statusInfo.currentSubnet = WiFi.subnetMask().toString();
            statusInfo.dnsServer = WiFi.dnsIP().toString();

            // 启动mDNS服务
            if (wifiConfig.enableMDNS && !mdnsStarted) {
                startMDNS();
            }
            
            triggerEvent(NetworkStatus::CONNECTED, 
                "WiFi connected: " + statusInfo.ipAddress);
            break;
            
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            statusInfo.status = NetworkStatus::DISCONNECTED;
            connecting = false;
            LOG_WARNING("NetworkManager: WiFi STA disconnected");
            triggerEvent(NetworkStatus::DISCONNECTED, "WiFi disconnected");
            break;
            
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            statusInfo.apClientCount = WiFi.softAPgetStationNum();
            LOG_INFO("NetworkManager: Client connected to AP");
            break;
            
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            statusInfo.apClientCount = WiFi.softAPgetStationNum();
            LOG_INFO("NetworkManager: Client disconnected from AP");
            break;
            
        default:
            break;
    }
}

void NetworkManager::updateStatusInfo() {
    statusInfo.uptime = millis();
    statusInfo.macAddress = WiFi.macAddress();
    
    if (WiFi.getMode() & WIFI_STA) {
        if (WiFi.status() == WL_CONNECTED) {
            statusInfo.ssid = WiFi.SSID();
            statusInfo.rssi = WiFi.RSSI();
            statusInfo.internetAvailable = checkInternetConnection();
            
            if (statusInfo.status != NetworkStatus::CONNECTED && !connecting) {
                statusInfo.status = NetworkStatus::CONNECTED;
                statusInfo.ipAddress = WiFi.localIP().toString();
            }
        } else if (statusInfo.status == NetworkStatus::CONNECTED) {
            statusInfo.status = NetworkStatus::DISCONNECTED;
            statusInfo.ipAddress = "";
            connecting = false;
        }
    }
    
    if (WiFi.getMode() & WIFI_AP) {
        statusInfo.apClientCount = WiFi.softAPgetStationNum();
        statusInfo.apIPAddress = WiFi.softAPIP().toString();
    }
}

void NetworkManager::updateIPConflictStatus() {
    if (wifiConfig.conflictDetection == IPConflictMode::NONE) {
        return;
    }

    String currentIP = WiFi.localIP().toString();
    if (currentIP.isEmpty() || currentIP == "0.0.0.0") {
        return;
    }

    // 检查缓存中是否有这个IP的记录
    bool cachedConflict = false;
    for (auto& cache : conflictCache) {
        if (cache.ip == currentIP && 
            (millis() - cache.timestamp < 300000)) { // 5分钟缓存
            cachedConflict = cache.conflicted;
            break;
        }
    }

    bool conflict = false;
    String detectionMethod = "";
    
    if (!cachedConflict) {
        // 根据配置的检测模式进行检测
        switch (wifiConfig.conflictDetection) {
            case IPConflictMode::PING:
                conflict = detectConflictByPing(currentIP);
                detectionMethod = "Ping";
                break;
            case IPConflictMode::ARP:
                conflict = detectConflictByARP(currentIP);
                detectionMethod = "ARP";
                break;
            case IPConflictMode::AUTO:
                conflict = detectConflictByGateway(currentIP);
                detectionMethod = "Gateway";
                break;
            default:
                return;
        }

        // 缓存结果
        ConflictCache cache = {currentIP, conflict, millis()};
        conflictCache.push_back(cache);
    } else {
        conflict = cachedConflict;
        detectionMethod = "Cached";
    }

    if (conflict) {
        conflictDetectionCount++;
        statusInfo.conflictDetected = detectionMethod + ": Conflict detected";
        
        LOG_WARNING("NetworkManager: IP conflict detected (" + 
                   detectionMethod + ") on IP: " + currentIP);
        
        if (conflictDetectionCount >= wifiConfig.conflictThreshold) {
            ipConflictDetected = true;
            statusInfo.status = NetworkStatus::IP_CONFLICT;
            triggerEvent(NetworkStatus::IP_CONFLICT, 
                "IP conflict detected: " + currentIP);
            
            // 自动故障转移
            if (wifiConfig.autoFailover) {
                LOG_INFO("NetworkManager: Starting automatic failover...");
                performFailover();
            }
        }
    } else {
        conflictDetectionCount = 0;
        statusInfo.conflictDetected = "No conflict";
    }
}

bool NetworkManager::detectConflictByARP(const String& ip) {
    // 这是一个简化的ARP检测实现
    LOG_DEBUG("NetworkManager: ARP conflict detection for IP: " + ip);
    return false; // 暂时返回false，需要根据实际情况实现
}

bool NetworkManager::detectConflictByPing(const String& ip) {
    LOG_DEBUG("NetworkManager: Ping conflict detection for IP: " + ip);
    // 这里可以实现ICMP ping检测
    // 注意：ESP32的ping功能可能需要额外的库
    return false;
}

bool NetworkManager::detectConflictByGateway(const String& ip) {
    LOG_DEBUG("NetworkManager: Gateway conflict detection for IP: " + ip);
    
    // 尝试与网关通信，如果失败可能表示IP冲突
    String gateway = WiFi.gatewayIP().toString();
    if (gateway.isEmpty() || gateway == "0.0.0.0") {
        return false;
    }
    
    // 简化的网关可达性检测
    // 实际实现可能需要尝试连接网关的特定端口
    return false;
}

bool NetworkManager::performFailover() {
    if (currentFailoverAttempts >= wifiConfig.maxFailoverAttempts) {
        LOG_ERROR("NetworkManager: Max failover attempts reached");
        
        if (wifiConfig.fallbackToDHCP) {
            LOG_INFO("NetworkManager: Falling back to DHCP");
            return switchToDHCP();
        }
        
        return false;
    }

    statusInfo.status = NetworkStatus::FAILOVER_IN_PROGRESS;
    currentFailoverAttempts++;
    statusInfo.failoverCount++;

    LOG_INFO("NetworkManager: Failover attempt " + 
             String(currentFailoverAttempts) + "/" + 
             String(wifiConfig.maxFailoverAttempts));

    String nextIP = selectNextIP();
    if (nextIP.isEmpty()) {
        LOG_WARNING("NetworkManager: No more backup IPs available");
        
        if (wifiConfig.fallbackToDHCP) {
            return switchToDHCP();
        }
        
        return false;
    }

    LOG_INFO("NetworkManager: Switching to IP: " + nextIP);
    
    // 更新配置并重新连接
    wifiConfig.staticIP = nextIP;
    disconnectWiFi();
    delay(1000);
    
    return connectToWiFi();
}

String NetworkManager::selectNextIP() {
    if (wifiConfig.backupIPs.empty()) {
        return "";
    }

    String selectedIP;
    
    switch (wifiConfig.failoverStrategy) {
        case IPFailoverStrategy::SEQUENTIAL:
            selectedIP = wifiConfig.backupIPs[currentBackupIPIndex];
            currentBackupIPIndex = (currentBackupIPIndex + 1) % wifiConfig.backupIPs.size();
            break;
            
        case IPFailoverStrategy::RANDOM:
            {
                int index = random(0, wifiConfig.backupIPs.size());
                selectedIP = wifiConfig.backupIPs[index];
            }
            break;
            
        case IPFailoverStrategy::SMART:
            // 智能选择：测试每个备用IP的可用性
            for (const auto& ip : wifiConfig.backupIPs) {
                if (testIPAvailability(ip)) {
                    selectedIP = ip;
                    break;
                }
            }
            if (selectedIP.isEmpty() && !wifiConfig.backupIPs.empty()) {
                selectedIP = wifiConfig.backupIPs[0];
            }
            break;
    }

    return selectedIP;
}

bool NetworkManager::testIPAvailability(const String& ip) {
    LOG_DEBUG("NetworkManager: Testing IP availability: " + ip);
    // 这里可以实现IP可用性测试逻辑
    // 例如：发送ARP请求或尝试连接
    return true; // 暂时返回true
}

bool NetworkManager::checkIPConflict() {
    return ipConflictDetected;
}

bool NetworkManager::switchToBackupIP() {
    return performFailover();
}

bool NetworkManager::switchToRandomIP() {
    String randomIP = getRandomIPInRange(wifiConfig.staticIP, wifiConfig.subnet);
    if (randomIP.isEmpty()) {
        return false;
    }
    
    wifiConfig.staticIP = randomIP;
    disconnectWiFi();
    delay(1000);
    
    return connectToWiFi();
}

bool NetworkManager::switchToDHCP() {
    wifiConfig.ipConfigType = IPConfigType::DHCP;
    disconnectWiFi();
    delay(1000);
    
    return connectToWiFi();
}

void NetworkManager::generateBackupIPs() {
    wifiConfig.backupIPs.clear();
    
    if (wifiConfig.staticIP.isEmpty() || 
        wifiConfig.subnet.isEmpty() || 
        wifiConfig.gateway.isEmpty()) {
        return;
    }

    // 解析网络地址和掩码
    IPAddress ip, subnet, gateway;
    ip.fromString(wifiConfig.staticIP.c_str());
    subnet.fromString(wifiConfig.subnet.c_str());
    gateway.fromString(wifiConfig.gateway.c_str());

    // 计算网络地址
    IPAddress network(ip[0] & subnet[0], 
                     ip[1] & subnet[1], 
                     ip[2] & subnet[2], 
                     ip[3] & subnet[3]);

    // 生成3个备用IP（避免.0、.1、.255和网关）
    int hostCount = 0;
    for (int i = 2; i < 254 && hostCount < 3; i++) {
        // 跳过网关
        if (i == gateway[3]) {
            continue;
        }
        
        // 跳过当前IP
        if (i == ip[3]) {
            continue;
        }
        
        IPAddress backupIP(network[0], network[1], network[2], i);
        wifiConfig.backupIPs.push_back(backupIP.toString());
        hostCount++;
    }

    LOG_INFO("NetworkManager: Generated " + String(hostCount) + " backup IPs");
}

String NetworkManager::getRandomIPInRange(const String& network, const String& mask) {
    if (!isValidIP(network) || !isValidSubnet(mask)) {
        return "";
    }

    IPAddress net, subnet;
    net.fromString(network.c_str());
    subnet.fromString(mask.c_str());

    // 计算网络地址
    IPAddress networkAddr(net[0] & subnet[0], 
                         net[1] & subnet[1], 
                         net[2] & subnet[2], 
                         net[3] & subnet[3]);

    // 计算广播地址
    IPAddress broadcastAddr;
    for (int i = 0; i < 4; i++) {
        broadcastAddr[i] = networkAddr[i] | (~subnet[i] & 0xFF);
    }

    // 生成随机IP（避免.0、.1、.255）
    IPAddress randomIP;
    do {
        for (int i = 0; i < 4; i++) {
            if (subnet[i] == 255) {
                randomIP[i] = networkAddr[i];
            } else {
                uint8_t min = (i == 3) ? 2 : networkAddr[i];
                uint8_t max = (i == 3) ? 254 : broadcastAddr[i];
                randomIP[i] = random(min, max + 1);
            }
        }
    } while (randomIP == networkAddr || 
             randomIP == broadcastAddr || 
             randomIP[3] == 0 || 
             randomIP[3] == 1 || 
             randomIP[3] == 255);

    return randomIP.toString();
}

void NetworkManager::cleanupConflictCache() {
    unsigned long currentTime = millis();
    auto it = conflictCache.begin();
    while (it != conflictCache.end()) {
        if (currentTime - it->timestamp > 3600000) { // 1小时
            it = conflictCache.erase(it);
        } else {
            ++it;
        }
    }
}

void NetworkManager::attemptReconnect() {
    if (statusInfo.reconnectAttempts >= wifiConfig.maxReconnectAttempts) {
        LOG_ERROR("NetworkManager: Max reconnect attempts reached");
        autoReconnectEnabled = false;
        return;
    }

    lastReconnectAttempt = millis();
    statusInfo.reconnectAttempts++;
    
    LOG_INFO("NetworkManager: Reconnection attempt " + 
             String(statusInfo.reconnectAttempts) + 
             "/" + String(wifiConfig.maxReconnectAttempts));    
    connectToWiFi();
}

void NetworkManager::triggerEvent(NetworkStatus status, const String& message) {
    NetworkEventCallback callback = nullptr;
    
    switch (status) {
        case NetworkStatus::CONNECTED:
            callback = connectionCallback;
            break;
        case NetworkStatus::DISCONNECTED:
            callback = disconnectionCallback;
            break;
        case NetworkStatus::IP_CONFLICT:
            callback = ipConflictCallback;
            break;
        default:
            break;
    }
    
    if (callback) {
        callback(status, message);
    }
}

WiFiConfig NetworkManager::getConfig() const {
    return wifiConfig;
}

NetworkStatusInfo NetworkManager::getStatusInfo() const {
    return statusInfo;
}

String NetworkManager::scanNetworks() {
    DynamicJsonDocument doc(2048);
    JsonArray networks = doc.to<JsonArray>();
    
    int numNetworks = WiFi.scanNetworks();
    for (int i = 0; i < numNetworks; i++) {
        JsonObject network = networks.createNestedObject();
        network["ssid"] = WiFi.SSID(i);
        network["rssi"] = WiFi.RSSI(i);
        network["strength"] = rssiToPercentage(WiFi.RSSI(i));
        network["channel"] = WiFi.channel(i);
        network["encryption"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "secured";
        network["bssid"] = WiFi.BSSIDstr(i);
    }
    
    String result;
    serializeJson(doc, result);
    return result;
}

bool NetworkManager::connectToNetwork(const String& ssid, const String& password) {
    if (ssid.isEmpty()) {
        LOG_WARNING("NetworkManager: SSID cannot be empty");
        return false;
    }
    
    wifiConfig.staSSID = ssid;
    wifiConfig.staPassword = password;
    
    saveNetworkConfig();
    return connectToWiFi();
}

void NetworkManager::disconnectNetwork() {
    disconnectWiFi();
}

bool NetworkManager::restartNetwork() {
    disconnect();
    delay(1000);
    return initialize();
}

bool NetworkManager::checkInternetConnection() {
    return WiFi.status() == WL_CONNECTED && 
           WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void NetworkManager::setConnectionCallback(NetworkEventCallback callback) {
    connectionCallback = callback;
}

void NetworkManager::setDisconnectionCallback(NetworkEventCallback callback) {
    disconnectionCallback = callback;
}

void NetworkManager::setIPConflictCallback(NetworkEventCallback callback) {
    ipConflictCallback = callback;
}

void NetworkManager::setAutoReconnect(bool enabled) {
    autoReconnectEnabled = enabled;
    LOG_INFO("NetworkManager: Auto reconnect: " + 
             String(enabled ? "enabled" : "disabled"));
}

String NetworkManager::getConfigJSON() {
    DynamicJsonDocument doc(2048);
    
    // 基本配置
    doc["mode"] = static_cast<uint8_t>(wifiConfig.mode);
    doc["deviceName"] = wifiConfig.deviceName;
    
    // AP配置
    doc["apSSID"] = wifiConfig.apSSID;
    doc["apPassword"] = wifiConfig.apPassword;
    doc["apChannel"] = wifiConfig.apChannel;
    doc["apHidden"] = wifiConfig.apHidden;
    doc["apMaxConnections"] = wifiConfig.apMaxConnections;
    
    // STA配置
    doc["staSSID"] = wifiConfig.staSSID;
    doc["staPassword"] = wifiConfig.staPassword;
    doc["ipConfigType"] = static_cast<uint8_t>(wifiConfig.ipConfigType);
    
    // 静态IP配置
    doc["staticIP"] = wifiConfig.staticIP;
    doc["gateway"] = wifiConfig.gateway;
    doc["subnet"] = wifiConfig.subnet;
    doc["dns1"] = wifiConfig.dns1;
    doc["dns2"] = wifiConfig.dns2;
    
    // IP冲突检测配置
    doc["conflictDetection"] = static_cast<uint8_t>(wifiConfig.conflictDetection);
    doc["failoverStrategy"] = static_cast<uint8_t>(wifiConfig.failoverStrategy);
    doc["autoFailover"] = wifiConfig.autoFailover;
    doc["conflictCheckInterval"] = wifiConfig.conflictCheckInterval;
    doc["maxFailoverAttempts"] = wifiConfig.maxFailoverAttempts;
    doc["conflictThreshold"] = wifiConfig.conflictThreshold;
    doc["fallbackToDHCP"] = wifiConfig.fallbackToDHCP;
    
    // 备用IP列表
    JsonArray backupIPs = doc.createNestedArray("backupIPs");
    for (const auto& ip : wifiConfig.backupIPs) {
        backupIPs.add(ip);
    }
    
    // 高级配置
    doc["connectTimeout"] = wifiConfig.connectTimeout;
    doc["reconnectInterval"] = wifiConfig.reconnectInterval;
    doc["maxReconnectAttempts"] = wifiConfig.maxReconnectAttempts;
    
    // 域名配置
    doc["customDomain"] = wifiConfig.customDomain;
    doc["enableMDNS"] = wifiConfig.enableMDNS;
    doc["enableDNS"] = wifiConfig.enableDNS;
    
    String result;
    serializeJson(doc, result);
    return result;
}

bool NetworkManager::updateConfig(const WiFiConfig& newConfig, bool saveToStorage) {
    bool restartRequired = false;
    
    if (newConfig.mode != wifiConfig.mode ||
        newConfig.apSSID != wifiConfig.apSSID ||
        newConfig.staSSID != wifiConfig.staSSID ||
        newConfig.ipConfigType != wifiConfig.ipConfigType) {
        restartRequired = true;
    }
    
    wifiConfig = newConfig;
    
    if (saveToStorage) {
        saveNetworkConfig();
    }
    
    if (restartRequired) {
        return restartNetwork();
    }
    
    return true;
}

bool NetworkManager::updateConfigFromJSON(const String& jsonConfig) {
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, jsonConfig);
    
    if (error) {
        LOG_ERROR("NetworkManager: Failed to parse JSON configuration");
        return false;
    }
    
    WiFiConfig newConfig = wifiConfig;
    
    // 更新配置
    if (doc.containsKey("mode")) 
        newConfig.mode = static_cast<NetworkMode>(doc["mode"].as<uint8_t>());
    if (doc.containsKey("deviceName")) 
        newConfig.deviceName = doc["deviceName"].as<String>();
    
    if (doc.containsKey("apSSID")) 
        newConfig.apSSID = doc["apSSID"].as<String>();
    if (doc.containsKey("apPassword")) 
        newConfig.apPassword = doc["apPassword"].as<String>();
    if (doc.containsKey("apChannel")) 
        newConfig.apChannel = doc["apChannel"].as<uint8_t>();
    if (doc.containsKey("apHidden")) 
        newConfig.apHidden = doc["apHidden"].as<bool>();
    if (doc.containsKey("apMaxConnections")) 
        newConfig.apMaxConnections = doc["apMaxConnections"].as<uint8_t>();
    
    if (doc.containsKey("staSSID")) 
        newConfig.staSSID = doc["staSSID"].as<String>();
    if (doc.containsKey("staPassword")) 
        newConfig.staPassword = doc["staPassword"].as<String>();
    if (doc.containsKey("ipConfigType")) 
        newConfig.ipConfigType = static_cast<IPConfigType>(doc["ipConfigType"].as<uint8_t>());
    
    if (doc.containsKey("staticIP")) 
        newConfig.staticIP = doc["staticIP"].as<String>();
    if (doc.containsKey("gateway")) 
        newConfig.gateway = doc["gateway"].as<String>();
    if (doc.containsKey("subnet")) 
        newConfig.subnet = doc["subnet"].as<String>();
    if (doc.containsKey("dns1")) 
        newConfig.dns1 = doc["dns1"].as<String>();
    if (doc.containsKey("dns2")) 
        newConfig.dns2 = doc["dns2"].as<String>();
    
    // IP冲突检测配置
    if (doc.containsKey("conflictDetection")) 
        newConfig.conflictDetection = static_cast<IPConflictMode>(
            doc["conflictDetection"].as<uint8_t>());
    if (doc.containsKey("failoverStrategy")) 
        newConfig.failoverStrategy = static_cast<IPFailoverStrategy>(
            doc["failoverStrategy"].as<uint8_t>());
    if (doc.containsKey("autoFailover")) 
        newConfig.autoFailover = doc["autoFailover"].as<bool>();
    if (doc.containsKey("conflictCheckInterval")) 
        newConfig.conflictCheckInterval = doc["conflictCheckInterval"].as<uint16_t>();
    if (doc.containsKey("maxFailoverAttempts")) 
        newConfig.maxFailoverAttempts = doc["maxFailoverAttempts"].as<uint8_t>();
    if (doc.containsKey("conflictThreshold")) 
        newConfig.conflictThreshold = doc["conflictThreshold"].as<uint8_t>();
    if (doc.containsKey("fallbackToDHCP")) 
        newConfig.fallbackToDHCP = doc["fallbackToDHCP"].as<bool>();
    
    // 备用IP列表
    if (doc.containsKey("backupIPs")) {
        newConfig.backupIPs.clear();
        JsonArray backupIPs = doc["backupIPs"];
        for (JsonVariant ip : backupIPs) {
            newConfig.backupIPs.push_back(ip.as<String>());
        }
    }
    
    if (doc.containsKey("connectTimeout")) 
        newConfig.connectTimeout = doc["connectTimeout"].as<uint32_t>();
    if (doc.containsKey("reconnectInterval")) 
        newConfig.reconnectInterval = doc["reconnectInterval"].as<uint32_t>();
    if (doc.containsKey("maxReconnectAttempts")) 
        newConfig.maxReconnectAttempts = doc["maxReconnectAttempts"].as<uint8_t>();
    
    if (doc.containsKey("customDomain")) 
        newConfig.customDomain = doc["customDomain"].as<String>();
    if (doc.containsKey("enableMDNS")) 
        newConfig.enableMDNS = doc["enableMDNS"].as<bool>();
    if (doc.containsKey("enableDNS")) 
        newConfig.enableDNS = doc["enableDNS"].as<bool>();
    
    return updateConfig(newConfig, true);
}

bool NetworkManager::resetToDefaults() {
    wifiConfig = WiFiConfig();
    preferences.clear();
    
    LOG_INFO("NetworkManager: Network configuration reset to defaults");
    return restartNetwork();
}

String NetworkManager::getStatistics() {
    DynamicJsonDocument doc(1024);
    
    doc["status"] = static_cast<uint8_t>(statusInfo.status);
    doc["ssid"] = statusInfo.ssid;
    doc["ipAddress"] = statusInfo.ipAddress;
    doc["macAddress"] = statusInfo.macAddress;
    doc["rssi"] = statusInfo.rssi;
    doc["signalStrength"] = rssiToPercentage(statusInfo.rssi);
    doc["reconnectAttempts"] = statusInfo.reconnectAttempts;
    doc["uptime"] = statusInfo.uptime;
    doc["internetAvailable"] = statusInfo.internetAvailable;
    doc["apClientCount"] = statusInfo.apClientCount;
    doc["apIPAddress"] = statusInfo.apIPAddress;
    doc["activeIPType"] = statusInfo.activeIPType;
    doc["failoverCount"] = statusInfo.failoverCount;
    doc["conflictDetected"] = statusInfo.conflictDetected;
    doc["currentGateway"] = statusInfo.currentGateway;
    doc["currentSubnet"] = statusInfo.currentSubnet;
    doc["dnsServer"] = statusInfo.dnsServer;
    
    String result;
    serializeJson(doc, result);
    return result;
}

bool NetworkManager::isValidIP(const String& ip) {
    IPAddress addr;
    return addr.fromString(ip);
}

bool NetworkManager::isValidSubnet(const String& subnet) {
    return isValidIP(subnet);
}

String NetworkManager::getMACAddress() {
    return WiFi.macAddress();
}

String NetworkManager::getChipID() {
    uint64_t chipid = ESP.getEfuseMac();
    char chipidStr[13];
    snprintf(chipidStr, sizeof(chipidStr), "%04X%08X", 
             (uint16_t)(chipid >> 32), (uint32_t)chipid);
    return String(chipidStr);
}

uint8_t NetworkManager::rssiToPercentage(int32_t rssi) {
    if (rssi >= -50) return 100;
    if (rssi <= -100) return 0;
    return 2 * (rssi + 100);
}

String NetworkManager::getWiFiModeString() {
    WiFiMode_t mode = WiFi.getMode();
    switch (mode) {
        case WIFI_MODE_NULL: return "NULL";
        case WIFI_MODE_STA: return "STA";
        case WIFI_MODE_AP: return "AP";
        case WIFI_MODE_APSTA: return "AP+STA";
        default: return "Unknown";
    }
}