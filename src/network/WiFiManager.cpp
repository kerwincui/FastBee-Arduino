/**
 * @file WiFiManager.cpp
 * @brief WiFi 管理器实现
 * @author kerwincui
 * @date 2026-03-03
 */

#include "network/WiFiManager.h"
#include "systems/LoggerSystem.h"
#include "utils/NetworkUtils.h"
#include <ArduinoJson.h>

WiFiManager::WiFiManager() {
    wifiConfig = WiFiConfig();
    statusInfo = NetworkStatusInfo();
}

WiFiManager::~WiFiManager() {
    disconnectWiFi();
    stopAPMode();
}

bool WiFiManager::initialize() {
    LOG_INFO("WiFiManager: Initializing...");
    
    // 设置 WiFi 事件回调
    WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
        this->handleWiFiEvent(event);
    });
    
    LOG_INFO("WiFiManager: Initialized successfully");
    return true;
}

bool WiFiManager::connectToWiFi() {
    if (wifiConfig.staSSID.isEmpty()) {
        LOG_INFO("WiFiManager: No STA SSID configured");
        return false;
    }

    // 在 AP+STA 模式下，不要断开已有连接，因为 AP 模式可能已经在运行
    // 只有在纯 STA 模式下才断开已有连接
    if (wifiConfig.mode == NetworkMode::NETWORK_STA) {
        wl_status_t currentStatus = WiFi.status();
        if (currentStatus == WL_CONNECTED || currentStatus == WL_IDLE_STATUS) {
            LOG_DEBUG("WiFiManager: Disconnecting before new connection attempt");
            WiFi.disconnect(false);
            delay(50);  // 短暂延迟确保断开完成
        }
    }

    // 确保 WiFi 模式正确
    WiFiMode_t currentMode = WiFi.getMode();
    if (wifiConfig.mode == NetworkMode::NETWORK_STA && !(currentMode & WIFI_STA)) {
        WiFi.mode(WIFI_STA);
    } else if (wifiConfig.mode == NetworkMode::NETWORK_AP_STA && currentMode != WIFI_MODE_APSTA) {
        if (!WiFi.mode(WIFI_MODE_APSTA)) {
            LOG_ERROR("WiFiManager: Failed to set WiFi mode to AP_STA");
            return false;
        }
    }

    // 配置网络
    if (wifiConfig.ipConfigType == IPConfigType::STATIC) {
        if (!configureStaticIP()) {
            LOG_WARNING("WiFiManager: Static IP configuration failed");
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
    
    char buffer[100];
    snprintf(buffer, sizeof(buffer), "Connecting to WiFi: %s", wifiConfig.staSSID.c_str());
    LOG_INFO("WiFiManager: Attempting to connect to " + wifiConfig.staSSID);
    triggerEvent(NetworkStatus::CONNECTING, buffer);
    
    return true;
}

void WiFiManager::disconnectWiFi() {
    WiFi.disconnect(true);
    connecting = false;
    statusInfo.status = NetworkStatus::DISCONNECTED;
    
    LOG_INFO("WiFiManager: WiFi disconnected");
    triggerEvent(NetworkStatus::DISCONNECTED, "WiFi disconnected");
}

bool WiFiManager::startAPMode() {
    // 检查AP是否已经真正启动（不仅仅是模式包含AP位，而是热点已广播）
    // 通过检查softAPIP是否有效来判断
    if ((WiFi.getMode() & WIFI_AP) && WiFi.softAPIP() != IPAddress(0, 0, 0, 0)) {
        LOG_INFO("WiFiManager: AP mode already active");
        return true;
    }
    
    // 根据配置的网络模式设置 WiFi 模式
    WiFiMode_t targetMode;
    if (wifiConfig.mode == NetworkMode::NETWORK_AP) {
        // 纯AP模式：只作为热点，不连接外部WiFi
        targetMode = WIFI_MODE_AP;
        LOG_INFO("WiFiManager: Setting WiFi mode to AP-only");
    } else {
        // AP+STA双模式或STA模式回退时：同时作为热点和STA客户端
        targetMode = WIFI_MODE_APSTA;
        LOG_INFO("WiFiManager: Setting WiFi mode to AP+STA");
    }
    
    if (!WiFi.mode(targetMode)) {
        LOG_ERROR("WiFiManager: Failed to set WiFi mode");
        return false;
    }
    
    // 配置 AP 网络参数（必须在 softAP 之前调用）
    IPAddress apIP(192, 168, 4, 1);
    IPAddress apGateway(192, 168, 4, 1);
    IPAddress apSubnet(255, 255, 255, 0);
    
    if (!WiFi.softAPConfig(apIP, apGateway, apSubnet)) {
        LOG_WARNING("WiFiManager: Failed to configure AP network");
        // 继续尝试启动 AP，可能使用默认配置
    }
    
    // 配置 AP 参数
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
        LOG_ERROR("WiFiManager: Failed to start AP mode");
        return false;
    }
    
    statusInfo.status = NetworkStatus::AP_MODE;
    statusInfo.apIPAddress = WiFi.softAPIP().toString();
    
    LOG_INFO("WiFiManager: AP mode started: " + apSSID);
    LOG_INFO("WiFiManager: AP IP: " + statusInfo.apIPAddress);
    
    triggerEvent(NetworkStatus::AP_MODE, "AP mode started successfully");
    return true;
}

void WiFiManager::stopAPMode() {
    if (WiFi.getMode() & WIFI_AP) {
        WiFi.softAPdisconnect(true);
        LOG_INFO("WiFiManager: AP mode stopped");
    }
}

bool WiFiManager::configureStaticIP() {
    if (wifiConfig.staticIP.isEmpty() || 
        wifiConfig.gateway.isEmpty() || 
        wifiConfig.subnet.isEmpty()) {
        LOG_WARNING("WiFiManager: Incomplete static IP configuration");
        return false;
    }
    
    if (!NetworkUtils::isValidIP(wifiConfig.staticIP) || 
        !NetworkUtils::isValidIP(wifiConfig.gateway) || 
        !NetworkUtils::isValidSubnet(wifiConfig.subnet)) {
        LOG_WARNING("WiFiManager: Invalid static IP configuration");
        return false;
    }
    
    IPAddress staticIP, gateway, subnet, dns1, dns2;
    
    if (!staticIP.fromString(wifiConfig.staticIP) ||
        !gateway.fromString(wifiConfig.gateway) ||
        !subnet.fromString(wifiConfig.subnet)) {
        LOG_WARNING("WiFiManager: Failed to parse static IP configuration");
        return false;
    }
    
    // 设置 DNS 服务器
    if (!wifiConfig.dns1.isEmpty() && dns1.fromString(wifiConfig.dns1)) {
        if (!wifiConfig.dns2.isEmpty() && dns2.fromString(wifiConfig.dns2)) {
            WiFi.config(staticIP, gateway, subnet, dns1, dns2);
        } else {
            WiFi.config(staticIP, gateway, subnet, dns1);
        }
    } else {
        WiFi.config(staticIP, gateway, subnet);
    }
    
    LOG_INFO("WiFiManager: Static IP configured: " + wifiConfig.staticIP);
    statusInfo.activeIPType = "Static";
    return true;
}

bool WiFiManager::configureDHCP() {
    WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
    LOG_INFO("WiFiManager: DHCP configured");
    statusInfo.activeIPType = "DHCP";
    return true;
}

String WiFiManager::scanNetworks() {
    // 使用静态JSON文档减少内存碎片
    static char buffer[2048];
    StaticJsonDocument<2048> doc;
    JsonArray networks = doc.to<JsonArray>();
    
    int numNetworks = WiFi.scanNetworks();
    for (int i = 0; i < numNetworks; i++) {
        JsonObject network = networks.createNestedObject();
        network["ssid"] = WiFi.SSID(i);
        network["rssi"] = WiFi.RSSI(i);
        network["strength"] = NetworkUtils::rssiToPercentage(WiFi.RSSI(i));
        network["channel"] = WiFi.channel(i);
        network["encryption"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "secured";
        network["bssid"] = WiFi.BSSIDstr(i);
    }
    
    // 序列化到静态缓冲区
    size_t len = serializeJson(doc, buffer, sizeof(buffer));
    buffer[len] = '\0';
    return String(buffer);
}

bool WiFiManager::connectToNetwork(const String& ssid, const String& password) {
    if (ssid.isEmpty()) {
        LOG_WARNING("WiFiManager: SSID cannot be empty");
        return false;
    }
    
    wifiConfig.staSSID = ssid;
    wifiConfig.staPassword = password;
    
    return connectToWiFi();
}

void WiFiManager::disconnectNetwork() {
    disconnectWiFi();
}

bool WiFiManager::restartNetwork() {
    disconnectWiFi();
    stopAPMode();
    delay(1000);
    
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
    
    return success;
}

bool WiFiManager::checkInternetConnection() {
    return WiFi.status() == WL_CONNECTED && 
           WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void WiFiManager::updateStatusInfo() {
    statusInfo.uptime = millis();
    statusInfo.macAddress = WiFi.macAddress();
    
    wifi_mode_t mode = WiFi.getMode();
    
    if (mode & WIFI_STA) {
        if (WiFi.status() == WL_CONNECTED) {
            statusInfo.ssid = WiFi.SSID();
            statusInfo.rssi = WiFi.RSSI();
            statusInfo.internetAvailable = checkInternetConnection();
            
            if (statusInfo.status != NetworkStatus::CONNECTED && !connecting) {
                statusInfo.status = NetworkStatus::CONNECTED;
            }
            // 始终更新网络信息（IP、网关、子网、DNS）
            statusInfo.ipAddress = WiFi.localIP().toString();
            statusInfo.currentGateway = WiFi.gatewayIP().toString();
            statusInfo.currentSubnet = WiFi.subnetMask().toString();
            // ESP32: dnsIP(0) 获取首选DNS，dnsIP(1) 获取备用DNS
            statusInfo.dnsServer = WiFi.dnsIP(0).toString();
        } else {
            // STA未连接，清除互联网状态
            statusInfo.internetAvailable = false;
            if (statusInfo.status == NetworkStatus::CONNECTED) {
                statusInfo.status = NetworkStatus::DISCONNECTED;
                statusInfo.ipAddress = "";
                statusInfo.currentGateway = "";
                statusInfo.currentSubnet = "";
                statusInfo.dnsServer = "";
                connecting = false;
            }
        }
    } else {
        // 纯AP模式（无STA），明确设置无互联网连接
        statusInfo.internetAvailable = false;
    }
    
    if (mode & WIFI_AP) {
        statusInfo.apClientCount = WiFi.softAPgetStationNum();
        statusInfo.apIPAddress = WiFi.softAPIP().toString();
    }
}

void WiFiManager::handleWiFiEvent(arduino_event_id_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            LOG_INFO("WiFiManager: WiFi STA connected");
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

            LOG_INFO("WiFiManager: Got IP: " + statusInfo.ipAddress);

            // 更新网络信息
            statusInfo.currentGateway = WiFi.gatewayIP().toString();
            statusInfo.currentSubnet = WiFi.subnetMask().toString();
            statusInfo.dnsServer = WiFi.dnsIP(0).toString();
            
            char buffer[100];
            snprintf(buffer, sizeof(buffer), "WiFi connected: %s", statusInfo.ipAddress.c_str());
            triggerEvent(NetworkStatus::CONNECTED, buffer);
            break;
            
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            statusInfo.status = NetworkStatus::DISCONNECTED;
            connecting = false;
            LOG_WARNING("WiFiManager: WiFi STA disconnected");
            triggerEvent(NetworkStatus::DISCONNECTED, "WiFi disconnected");
            break;
            
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            statusInfo.apClientCount = WiFi.softAPgetStationNum();
            LOG_INFO("WiFiManager: Client connected to AP");
            break;
            
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            statusInfo.apClientCount = WiFi.softAPgetStationNum();
            LOG_INFO("WiFiManager: Client disconnected from AP");
            break;
            
        default:
            break;
    }
}

void WiFiManager::triggerEvent(NetworkStatus status, const char* message) {
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

void WiFiManager::attemptReconnect() {
    if (statusInfo.reconnectAttempts >= wifiConfig.maxReconnectAttempts) {
        LOG_ERROR("WiFiManager: Max reconnect attempts reached");
        autoReconnectEnabled = false;
        return;
    }

    lastReconnectAttempt = millis();
    statusInfo.reconnectAttempts++;

    LOG_INFO("WiFiManager: Reconnection attempt " +
             String(statusInfo.reconnectAttempts) +
             "/" + String(wifiConfig.maxReconnectAttempts));

    // 在重连前断开当前连接，清除可能的错误状态（如 AUTH_EXPIRE）
    if (WiFi.status() != WL_DISCONNECTED) {
        LOG_DEBUG("WiFiManager: Disconnecting before reconnect attempt");
        WiFi.disconnect(false);  // 断开但不擦除配置
        delay(100);  // 短暂延迟确保断开完成
    }

    connectToWiFi();
}

void WiFiManager::setConnectionCallback(NetworkEventCallback callback) {
    connectionCallback = callback;
}

void WiFiManager::setDisconnectionCallback(NetworkEventCallback callback) {
    disconnectionCallback = callback;
}

void WiFiManager::setIPConflictCallback(NetworkEventCallback callback) {
    ipConflictCallback = callback;
}

void WiFiManager::setAutoReconnect(bool enabled) {
    autoReconnectEnabled = enabled;
    LOG_INFO("WiFiManager: Auto reconnect: " + 
             String(enabled ? "enabled" : "disabled"));
}

WiFiConfig WiFiManager::getConfig() const {
    return wifiConfig;
}

void WiFiManager::setNetworkConfig(const WiFiConfig& config) {
    wifiConfig = config;
    LOG_INFO("WiFiManager: Network config updated");
}

NetworkStatusInfo WiFiManager::getStatusInfo() const {
    return statusInfo;
}

String WiFiManager::getWiFiModeString() {
    WiFiMode_t mode = WiFi.getMode();
    switch (mode) {
        case WIFI_MODE_NULL: return "NULL";
        case WIFI_MODE_STA: return "STA";
        case WIFI_MODE_AP: return "AP";
        case WIFI_MODE_APSTA: return "AP+STA";
        default: return "Unknown";
    }
}

String WiFiManager::getMACAddress() {
    return WiFi.macAddress();
}

String WiFiManager::getChipID() {
    uint64_t chipid = ESP.getEfuseMac();
    char chipidStr[13];
    snprintf(chipidStr, sizeof(chipidStr), "%04X%08X", 
             (uint16_t)(chipid >> 32), (uint32_t)chipid);
    return String(chipidStr);
}

