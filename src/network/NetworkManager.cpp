/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:29:56
 */

#include "network/NetworkManager.h"
#include "systems/LoggerSystem.h"
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <core/SystemConstants.h>
#include <core/ConfigDefines.h>

NetworkManager::NetworkManager(AsyncWebServer* webServerPtr) 
    : webServer(webServerPtr),
      lastReconnectAttempt(0),
      lastStatusUpdate(0),
      autoReconnectEnabled(true),
      isInitialized(false),
      dnsServerStarted(false),
      mdnsStarted(false),
      connecting(false),
      connectingStartTime(0) {
    
    // 设置默认配置
    wifiConfig = WiFiConfig();
    statusInfo = NetworkStatusInfo();
}

NetworkManager::~NetworkManager() {
    // 在析构函数中调用 disconnect
    disconnect(); 
    //  webServer 指针很可能由 WebConfigManager 创建和管理，这里只置空
    webServer = nullptr;
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
    }

    // 设置WiFi事件回调
    WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
        this->handleWiFiEvent(event);
    });

    // 根据配置启动网络
    bool success = false;
    switch (wifiConfig.mode) {
        case WiFiMode::STA_ONLY:
            success = connectToWiFi();
            break;
        case WiFiMode::AP_ONLY:
            success = startAPMode();
            break;
        case WiFiMode::AP_STA:
            success = startAPMode() && connectToWiFi();
            break;
    }

    if (success) {
        isInitialized = true;
        LOG_INFO("NetworkManager: Network manager initialized successfully");
    } else {
        LOG_WARNING("NetworkManager: Network manager initialization failed");
    }

    return success;
}

void NetworkManager::disconnect() {
    LOG_INFO("NetworkManager: Disconnecting all network connections...");
    
    // 停止所有网络服务
    stopDNSServer();
    stopMDNS();
    stopAPMode();
    disconnectWiFi();
    
    // 重置状态
    isInitialized = false;
    dnsServerStarted = false;
    mdnsStarted = false;
    autoReconnectEnabled = false;
    connecting = false;
    connectingStartTime = 0;
    
    // 重置状态信息
    statusInfo.status = NetworkStatus::DISCONNECTED;
    statusInfo.ipAddress = "";
    statusInfo.apIPAddress = "";
    statusInfo.apClientCount = 0;
    statusInfo.rssi = 0;
    statusInfo.ssid = "";
    statusInfo.internetAvailable = false;
    statusInfo.reconnectAttempts = 0;
    statusInfo.lastConnectionTime = 0;
    
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

    // 处理连接超时（当事件回调未及时触发时的备用检查）
    // 注意：此检查仅在事件回调没有正确处理超时的情况下生效
    if (connecting && (currentTime - connectingStartTime >= wifiConfig.connectTimeout)) {
        LOG_WARNING("NetworkManager: Connection timeout detected in update()");
        connecting = false;
        statusInfo.status = NetworkStatus::CONNECTION_FAILED;
        String message = "Connection timeout: " + wifiConfig.staSSID;
        LOG_WARNING("NetworkManager: " + message);
        triggerEvent(NetworkStatus::CONNECTION_FAILED, message);
        
        // 由于连接失败，增加重连尝试计数
        statusInfo.reconnectAttempts++;
    }

    // 自动重连逻辑（不依赖connecting标志，而是根据网络状态）
    if (autoReconnectEnabled && 
        (wifiConfig.mode == WiFiMode::STA_ONLY || wifiConfig.mode == WiFiMode::AP_STA) &&
        // 只在未连接状态且不在连接过程中尝试重连
        !connecting &&
        (statusInfo.status == NetworkStatus::DISCONNECTED || 
         statusInfo.status == NetworkStatus::CONNECTION_FAILED) &&
        currentTime - lastReconnectAttempt >= wifiConfig.reconnectInterval) {
        
        attemptReconnect();
    }
}

bool NetworkManager::initializeFileSystem() {
    // 检查文件系统是否已初始化
    // 这里假设文件系统已在主程序初始化
    return true;
}

bool NetworkManager::loadNetworkConfig() {
    if (!preferences.isKey("initialized")) {
        LOG_INFO("NetworkManager: No saved network config found, using defaults");
        return false;
    }

    try {
        // 基本配置
        wifiConfig.mode = static_cast<WiFiMode>(preferences.getUInt("mode", static_cast<uint8_t>(WiFiMode::AP_STA)));
        wifiConfig.deviceName = preferences.getString("device_name", SystemInfo::DEFAULT_DEVICE_NAME);

        // AP配置
        wifiConfig.apSSID = preferences.getString("ap_ssid", Network::DEFAULT_AP_SSID);
        wifiConfig.apPassword = preferences.getString("ap_password", Network::DEFAULT_AP_PASSWORD);
        wifiConfig.apChannel = preferences.getUChar("ap_channel", Network::DEFAULT_AP_CHANNEL);
        wifiConfig.apHidden = preferences.getBool("ap_hidden", Network::DEFAULT_AP_HIDDEN);
        wifiConfig.apMaxConnections = preferences.getUChar("ap_max_conn", Network::DEFAULT_AP_MAX_CONNECTIONS);

        // STA配置
        wifiConfig.staSSID = preferences.getString("sta_ssid", "");
        wifiConfig.staPassword = preferences.getString("sta_password", "");
        wifiConfig.ipConfigType = static_cast<IPConfigType>(preferences.getUInt("ip_config", static_cast<uint8_t>(IPConfigType::DHCP)));

        // 静态IP配置
        wifiConfig.staticIP = preferences.getString("static_ip", "");
        wifiConfig.gateway = preferences.getString("gateway", "");
        wifiConfig.subnet = preferences.getString("subnet", "");
        wifiConfig.dns1 = preferences.getString("dns1", "");
        wifiConfig.dns2 = preferences.getString("dns2", "");

        // 高级配置
        wifiConfig.connectTimeout = preferences.getULong("connect_timeout", WIFI_CONNECT_TIMEOUT);
        wifiConfig.reconnectInterval = preferences.getULong("reconnect_interval", WIFI_RECONNECT_INTERVAL);
        wifiConfig.maxReconnectAttempts = preferences.getUChar("max_reconnect", MAX_WIFI_ATTEMPTS);

        // 域名配置
        wifiConfig.customDomain = preferences.getString("custom_domain", CUSTOM_DOMAIN);
        wifiConfig.enableMDNS = preferences.getBool("enable_mdns", true);
        wifiConfig.enableDNS = preferences.getBool("enable_dns", false);

        LOG_INFO("NetworkManager: Network configuration loaded successfully");
        return true;
    } catch (...) {
        LOG_ERROR("NetworkManager: Failed to load network configuration");
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

        // 高级配置
        preferences.putULong("connect_timeout", wifiConfig.connectTimeout);
        preferences.putULong("reconnect_interval", wifiConfig.reconnectInterval);
        preferences.putUChar("max_reconnect", wifiConfig.maxReconnectAttempts);

        // 域名配置
        preferences.putString("custom_domain", wifiConfig.customDomain);
        preferences.putBool("enable_mdns", wifiConfig.enableMDNS);
        preferences.putBool("enable_dns", wifiConfig.enableDNS);

        preferences.putBool("initialized", true);

        LOG_INFO("NetworkManager: Network configuration saved successfully");
        return true;
    } catch (...) {
        LOG_ERROR("NetworkManager: Failed to save network configuration");
        return false;
    }
}

bool NetworkManager::startAPMode() {
    if (WiFi.getMode() & WIFI_AP) {
        // AP模式已经启动
        return true;
    }

    // 根据当前模式设置WiFi模式
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
        apSSID.reserve(32); // 预分配内存
        apSSID = SystemInfo::DEFAULT_DEVICE_NAME;
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

    // 启动DNS服务器（如果启用）
    if (wifiConfig.enableDNS) {
        startDNSServer();
    }

    // 启动mDNS服务（如果启用）
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
    if (wifiConfig.mode == WiFiMode::STA_ONLY && !(currentMode & WIFI_STA)) {
        WiFi.mode(WIFI_STA);
    } else if (wifiConfig.mode == WiFiMode::AP_STA && currentMode != WIFI_MODE_APSTA) {
        if (!WiFi.mode(WIFI_MODE_APSTA)) {
            LOG_ERROR("NetworkManager: Failed to set WiFi mode to AP_STA");
            return false;
        }
    }

    // 配置静态IP（如果需要）
    if (wifiConfig.ipConfigType == IPConfigType::STATIC) {
        if (!configureStaticIP()) {
            LOG_WARNING("NetworkManager: Static IP configuration failed, falling back to DHCP");
        }
    }

    // 设置连接状态
    statusInfo.status = NetworkStatus::CONNECTING;
    connecting = true;
    connectingStartTime = millis();

    // 触发连接事件
    triggerEvent(NetworkStatus::CONNECTING, "Connecting to WiFi: " + wifiConfig.staSSID);
    LOG_INFO("NetworkManager: Attempting to connect to " + wifiConfig.staSSID);

    // 开始连接（非阻塞方式）
    WiFi.begin(wifiConfig.staSSID.c_str(), wifiConfig.staPassword.c_str());
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
    if (wifiConfig.staticIP.isEmpty() || wifiConfig.gateway.isEmpty() || wifiConfig.subnet.isEmpty()) {
        LOG_WARNING("NetworkManager: Incomplete static IP configuration");
        return false;
    }

    if (!isValidIP(wifiConfig.staticIP) || !isValidIP(wifiConfig.gateway) || !isValidSubnet(wifiConfig.subnet)) {
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
    return true;
}

bool NetworkManager::startMDNS() {
    if (mdnsStarted) {
        return true;
    }

    // 增强的网络状态检查
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
        LOG_INFO("NetworkManager: Added HTTP service to mDNS");
    }
    mdnsStarted = true;
    LOG_INFO("NetworkManager: mDNS started: " + hostname + ".local");
    LOG_INFO("NetworkManager: IP Address: " + WiFi.localIP().toString());
    LOG_INFO("NetworkManager: AP IP Address: " + WiFi.softAPIP().toString());
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
            statusInfo.lastConnectionTime = millis();
            statusInfo.reconnectAttempts = 0;

            LOG_INFO("NetworkManager: Got IP: " + statusInfo.ipAddress);

            // 启动mDNS服务
            if (wifiConfig.enableMDNS && !mdnsStarted) {
                startMDNS();
            }
            
            triggerEvent(NetworkStatus::CONNECTED, "WiFi connected: " + statusInfo.ipAddress);
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
            
            // 如果事件回调没有正确设置状态，这里进行补充
            if (statusInfo.status != NetworkStatus::CONNECTED && !connecting) {
                statusInfo.status = NetworkStatus::CONNECTED;
                statusInfo.ipAddress = WiFi.localIP().toString();
            }
        } else if (statusInfo.status == NetworkStatus::CONNECTED) {
            // 如果WiFi已断开但状态未更新
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

void NetworkManager::attemptReconnect() {
    if (statusInfo.reconnectAttempts >= wifiConfig.maxReconnectAttempts) {
        LOG_ERROR("NetworkManager: Max reconnect attempts reached");
        autoReconnectEnabled = false;
        return;
    }

    lastReconnectAttempt = millis();
    statusInfo.reconnectAttempts++;
    
    LOG_INFO("NetworkManager: Reconnection attempt " + String(statusInfo.reconnectAttempts) + 
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
        default:
            break;
    }
    
    if (callback) {
        callback(status, message);
    }
}

NetworkStatusInfo NetworkManager::getStatusInfo() const {
    return statusInfo;
}

WiFiConfig NetworkManager::getConfig() const {
    return wifiConfig;
}

bool NetworkManager::updateConfig(const WiFiConfig& newConfig, bool saveToStorage) {
    bool restartRequired = false;
    
    // 检查是否需要重启网络
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
    // 简单的互联网连接检查
    return WiFi.status() == WL_CONNECTED && 
           WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void NetworkManager::setConnectionCallback(NetworkEventCallback callback) {
    connectionCallback = callback;
}

void NetworkManager::setDisconnectionCallback(NetworkEventCallback callback) {
    disconnectionCallback = callback;
}

void NetworkManager::setAutoReconnect(bool enabled) {
    autoReconnectEnabled = enabled;
    LOG_INFO("NetworkManager: Auto reconnect: " + String(enabled ? "enabled" : "disabled"));
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

bool NetworkManager::updateConfigFromJSON(const String& jsonConfig) {
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, jsonConfig);
    
    if (error) {
        LOG_ERROR("NetworkManager: Failed to parse JSON configuration");
        return false;
    }
    
    WiFiConfig newConfig = wifiConfig;
    
    // 更新配置
    if (doc.containsKey("mode")) newConfig.mode = static_cast<WiFiMode>(doc["mode"].as<uint8_t>());
    if (doc.containsKey("deviceName")) newConfig.deviceName = doc["deviceName"].as<String>();
    
    if (doc.containsKey("apSSID")) newConfig.apSSID = doc["apSSID"].as<String>();
    if (doc.containsKey("apPassword")) newConfig.apPassword = doc["apPassword"].as<String>();
    if (doc.containsKey("apChannel")) newConfig.apChannel = doc["apChannel"].as<uint8_t>();
    if (doc.containsKey("apHidden")) newConfig.apHidden = doc["apHidden"].as<bool>();
    if (doc.containsKey("apMaxConnections")) newConfig.apMaxConnections = doc["apMaxConnections"].as<uint8_t>();
    
    if (doc.containsKey("staSSID")) newConfig.staSSID = doc["staSSID"].as<String>();
    if (doc.containsKey("staPassword")) newConfig.staPassword = doc["staPassword"].as<String>();
    if (doc.containsKey("ipConfigType")) newConfig.ipConfigType = static_cast<IPConfigType>(doc["ipConfigType"].as<uint8_t>());
    
    if (doc.containsKey("staticIP")) newConfig.staticIP = doc["staticIP"].as<String>();
    if (doc.containsKey("gateway")) newConfig.gateway = doc["gateway"].as<String>();
    if (doc.containsKey("subnet")) newConfig.subnet = doc["subnet"].as<String>();
    if (doc.containsKey("dns1")) newConfig.dns1 = doc["dns1"].as<String>();
    if (doc.containsKey("dns2")) newConfig.dns2 = doc["dns2"].as<String>();
    
    if (doc.containsKey("connectTimeout")) newConfig.connectTimeout = doc["connectTimeout"].as<uint32_t>();
    if (doc.containsKey("reconnectInterval")) newConfig.reconnectInterval = doc["reconnectInterval"].as<uint32_t>();
    if (doc.containsKey("maxReconnectAttempts")) newConfig.maxReconnectAttempts = doc["maxReconnectAttempts"].as<uint8_t>();
    
    if (doc.containsKey("customDomain")) newConfig.customDomain = doc["customDomain"].as<String>();
    if (doc.containsKey("enableMDNS")) newConfig.enableMDNS = doc["enableMDNS"].as<bool>();
    if (doc.containsKey("enableDNS")) newConfig.enableDNS = doc["enableDNS"].as<bool>();
    
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
    
    String result;
    serializeJson(doc, result);
    return result;
}

// 静态工具方法
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