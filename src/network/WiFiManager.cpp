/**
 * @file WiFiManager.cpp
 * @brief WiFi 管理器实现
 * @author kerwincui
 * @date 2026-03-03
 */

#include "network/WiFiManager.h"
#include "systems/LoggerSystem.h"
#include "utils/NetworkUtils.h"
#include "core/PeriphExecManager.h"
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

    // AP+STA模式下的重连：使用WiFi.reconnect()避免干扰AP
    // WiFi.begin()会重新初始化STA接口，在ESP32上会短暂重置WiFi射频，导致AP信号中断
    // WiFi.reconnect()只是调用底层esp_wifi_connect()而不重新初始化射频，不会干扰AP
    if (staInitialized && wifiConfig.mode == NetworkMode::NETWORK_AP_STA) {
        // 检查STA是否被禁用（如果之前达到最大重连次数被关闭了）
        WiFiMode_t currentMode = WiFi.getMode();
        if (currentMode == WIFI_MODE_AP) {
            // STA被禁用了，需要重新启用
            LOG_INFO("WiFiManager: STA was disabled, re-enabling...");
            WiFi.enableSTA(true);
            delay(100);
            staInitialized = false;  // 需要重新WiFi.begin()
            // 继续执行下面的WiFi.begin()逻辑
        } else {
            // 确保当前模式仍然是APSTA
            if (!(currentMode & WIFI_AP) || !(currentMode & WIFI_STA)) {
                WiFi.mode(WIFI_MODE_APSTA);
                delay(100);
            }
            
            statusInfo.status = NetworkStatus::CONNECTING;
            connecting = true;
            connectingStartTime = millis();
            
            WiFi.reconnect();  // 不重置射频，不影响AP
            LOG_INFO("WiFiManager: Reconnecting to " + wifiConfig.staSSID + " (AP-safe reconnect)");
            triggerEvent(NetworkStatus::CONNECTING, "Reconnecting to WiFi");
            return true;
        }
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
    // 关键修复：AP+STA 模式下，必须确保 WiFi 模式包含 AP，否则 AP 热点会消失
    WiFiMode_t currentMode = WiFi.getMode();
    
    if (wifiConfig.mode == NetworkMode::NETWORK_AP_STA) {
        // AP+STA 模式：确保当前模式同时包含 AP 和 STA
        // 使用位运算检查，而不是相等比较，这样可以处理各种边界情况
        if (!(currentMode & WIFI_AP) || !(currentMode & WIFI_STA)) {
            LOG_INFOF("WiFiManager: Setting WiFi mode to AP_STA (current: %d)", currentMode);
            if (!WiFi.mode(WIFI_MODE_APSTA)) {
                LOG_ERROR("WiFiManager: Failed to set WiFi mode to AP_STA");
                return false;
            }
        }
    } else if (wifiConfig.mode == NetworkMode::NETWORK_STA) {
        // 纯 STA 模式：只在当前模式不包含 STA 时才设置
        if (!(currentMode & WIFI_STA)) {
            WiFi.mode(WIFI_STA);
        }
    }
    // 纯 AP 模式不需要在这里处理，由 startAPMode() 负责

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
    staInitialized = true;  // 标记STA已初始化
    
    char buffer[100];
    snprintf(buffer, sizeof(buffer), "Connecting to WiFi: %s", wifiConfig.staSSID.c_str());
    LOG_INFO("WiFiManager: Attempting to connect to " + wifiConfig.staSSID);
    triggerEvent(NetworkStatus::CONNECTING, buffer);
    
    return true;
}

void WiFiManager::disconnectWiFi() {
    WiFi.disconnect(true);
    connecting = false;
    staInitialized = false;  // 重置STA初始化标志
    statusInfo.status = NetworkStatus::DISCONNECTED;
    
    LOG_INFO("WiFiManager: WiFi disconnected");
    triggerEvent(NetworkStatus::DISCONNECTED, "WiFi disconnected");
}

bool WiFiManager::startAPMode() {
    // 检查AP是否已经正确启动且模式匹配
    // 注意：必须同时满足以下条件才能跳过重新启动：
    // 1. WiFi模式正确（纯AP模式必须是WIFI_MODE_AP，AP+STA模式必须是WIFI_MODE_APSTA）
    // 2. softAPIP有效
    // 3. AP热点确实在运行（可以通过检查连接数或SSID来验证）
    WiFiMode_t currentMode = WiFi.getMode();
    WiFiMode_t targetMode = (wifiConfig.mode == NetworkMode::NETWORK_AP) ? WIFI_MODE_AP : WIFI_MODE_APSTA;
    
    if (currentMode == targetMode && WiFi.softAPIP() != IPAddress(0, 0, 0, 0)) {
        // 模式正确且IP有效，检查热点是否真正在广播
        // 通过检查是否能获取SSID来验证
        String currentSSID = WiFi.softAPSSID();
        String expectedSSID = wifiConfig.apSSID.isEmpty() ? 
            (wifiConfig.deviceName + "_" + getChipID().substring(0, 6)) : wifiConfig.apSSID;
        
        if (currentSSID.length() > 0 && currentSSID == expectedSSID) {
            LOG_INFO("WiFiManager: AP mode already active with correct configuration");
            return true;
        }
        LOG_INFO("WiFiManager: AP running but SSID mismatch, restarting...");
    }
    
    // 记录当前STA状态
    bool wasStaConnected = (WiFi.status() == WL_CONNECTED);
    IPAddress staIP = WiFi.localIP();
    
    // targetMode 和 currentMode 已在前面声明，直接使用
    LOG_INFO("WiFiManager: Setting WiFi mode to " + 
             String(targetMode == WIFI_MODE_AP ? "AP-only" : "AP+STA"));
    
    // 如果要从 STA 切换到 AP+STA，需要小心处理
    // 先配置AP网络参数，再切换模式，可以减少中断时间
    if ((currentMode == WIFI_MODE_STA || currentMode == WIFI_MODE_APSTA) && targetMode == WIFI_MODE_APSTA) {
        // 已经是STA模式，要添加AP功能
        // 先配置AP参数，再启用AP
        
        // 配置 AP 网络参数
        IPAddress apIP(192, 168, 4, 1);
        IPAddress apGateway(192, 168, 4, 1);
        IPAddress apSubnet(255, 255, 255, 0);
        
        // 配置softAP（但不广播）
        if (!WiFi.softAPConfig(apIP, apGateway, apSubnet)) {
            LOG_WARNING("WiFiManager: Failed to configure AP network");
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
        
        // 设置模式为APSTA（如果还没设置）
        if (currentMode != WIFI_MODE_APSTA) {
            if (!WiFi.mode(WIFI_MODE_APSTA)) {
                LOG_ERROR("WiFiManager: Failed to set WiFi mode to APSTA");
                return false;
            }
            // 短暂延迟让模式切换生效
            delay(100);
        }
        
        // 启动AP
        if (!WiFi.softAP(apSSID.c_str(), wifiConfig.apPassword.c_str(), 
                         wifiConfig.apChannel, wifiConfig.apHidden, 
                         wifiConfig.apMaxConnections)) {
            LOG_ERROR("WiFiManager: Failed to start AP mode");
            return false;
        }
        
        statusInfo.status = NetworkStatus::AP_MODE;
        statusInfo.apIPAddress = WiFi.softAPIP().toString();
        
        LOG_INFO("WiFiManager: AP mode added to STA: " + apSSID);
        LOG_INFO("WiFiManager: AP IP: " + statusInfo.apIPAddress);
        
        triggerEvent(NetworkStatus::AP_MODE, "AP mode started successfully");
        return true;
    }
    
    // 其他情况：正常切换模式
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
        staInitialized = false;  // 重置STA初始化标志
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
    // DHCP 模式：IP 使用 0.0.0.0 启用 DHCP，但仍可显式指定 DNS 服务器
    // ESP32 WiFi.config(0,0,0,0, dns1, dns2) 会启用 DHCP 同时设置 DNS
    IPAddress dns1, dns2;
    bool hasDns1 = !wifiConfig.dns1.isEmpty() && dns1.fromString(wifiConfig.dns1);
    bool hasDns2 = hasDns1 && !wifiConfig.dns2.isEmpty() && dns2.fromString(wifiConfig.dns2);
    
    if (hasDns2) {
        WiFi.config(IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(0,0,0,0), dns1, dns2);
        LOGGER.infof("WiFiManager: DHCP configured with DNS %s, %s", wifiConfig.dns1.c_str(), wifiConfig.dns2.c_str());
    } else if (hasDns1) {
        WiFi.config(IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(0,0,0,0), dns1);
        LOGGER.infof("WiFiManager: DHCP configured with DNS %s", wifiConfig.dns1.c_str());
    } else {
        WiFi.config(IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(0,0,0,0));
        LOG_INFO("WiFiManager: DHCP configured (using DHCP-provided DNS)");
    }
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
    // 检查STA模式是否已启用
    wifi_mode_t mode = WiFi.getMode();
    if (!(mode & WIFI_STA)) {
        return false;  // 纯AP模式，无互联网
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }
    
    IPAddress localIP = WiFi.localIP();
    if (localIP == IPAddress(0, 0, 0, 0)) {
        return false;
    }
    
    // 仅在纯AP模式下才认为 192.168.4.x 无互联网
    // AP+STA模式下，WiFi.localIP() 返回的是STA IP，不需要此检查
    if (mode == WIFI_AP) {
        if (localIP[0] == 192 && localIP[1] == 168 && localIP[2] == 4) {
            return false;
        }
    }
    
    return true;
}

void WiFiManager::updateStatusInfo() {
    statusInfo.uptime = millis();
    statusInfo.macAddress = WiFi.macAddress();
    
    wifi_mode_t mode = WiFi.getMode();
    
    // 处理 STA 模式状态
    if (mode & WIFI_STA) {
        if (WiFi.status() == WL_CONNECTED) {
            statusInfo.ssid = WiFi.SSID();
            statusInfo.rssi = WiFi.RSSI();
            statusInfo.internetAvailable = checkInternetConnection();
            
            // 只有在真正有互联网连接时才标记为 CONNECTED
            // 否则标记为 AP_MODE（表示 STA 已连接但无互联网）
            if (statusInfo.internetAvailable) {
                if (statusInfo.status != NetworkStatus::CONNECTED && !connecting) {
                    statusInfo.status = NetworkStatus::CONNECTED;
                }
            } else {
                // STA 已连接但无互联网（可能是 AP+STA 模式）
                if (statusInfo.status == NetworkStatus::CONNECTED) {
                    // 保持 CONNECTED 状态但 internetAvailable 为 false
                    // 这样前端可以区分 "WiFi已连接但无互联网"
                }
            }
            // 始终更新网络信息（IP、网关、子网、DNS）
            statusInfo.ipAddress = WiFi.localIP().toString();
            statusInfo.currentGateway = WiFi.gatewayIP().toString();
            statusInfo.currentSubnet = WiFi.subnetMask().toString();
            // ESP32: dnsIP(0) 获取首选DNS，dnsIP(1) 获取备用DNS
            statusInfo.dnsServer = WiFi.dnsIP(0).toString();
        } else {
            // STA未连接，清除互联网状态和 STA 相关信息
            statusInfo.internetAvailable = false;
            statusInfo.ssid = "";
            statusInfo.rssi = 0;
            
            // 只有在当前状态是 CONNECTED 时才切换到断开状态
            // 如果是 AP_MODE 或其他状态则保持不变
            if (statusInfo.status == NetworkStatus::CONNECTED) {
                // 检查是否是 AP+STA 模式且 AP 正在运行
                if ((mode & WIFI_AP) && WiFi.softAPIP() != IPAddress(0, 0, 0, 0)) {
                    statusInfo.status = NetworkStatus::AP_MODE;
                    LOG_INFO("WiFiManager: STA disconnected, switching to AP_MODE status");
                } else {
                    statusInfo.status = NetworkStatus::DISCONNECTED;
                }
                statusInfo.ipAddress = "";
                statusInfo.currentGateway = "";
                statusInfo.currentSubnet = "";
                statusInfo.dnsServer = "";
            }
            connecting = false;
        }
    } else {
        // 纯AP模式（无STA），明确设置无互联网连接
        statusInfo.internetAvailable = false;
        statusInfo.ssid = "";
        statusInfo.rssi = 0;
        statusInfo.status = NetworkStatus::AP_MODE;
        statusInfo.ipAddress = "";
        statusInfo.currentGateway = "";
        statusInfo.currentSubnet = "";
        statusInfo.dnsServer = "";
    }
    
    // 更新 AP 模式信息
    if (mode & WIFI_AP) {
        statusInfo.apClientCount = WiFi.softAPgetStationNum();
        statusInfo.apIPAddress = WiFi.softAPIP().toString();
        
        // 如果 AP 正在运行但 STA 未连接，确保状态为 AP_MODE
        if (!(mode & WIFI_STA) || WiFi.status() != WL_CONNECTED) {
            if (statusInfo.status != NetworkStatus::AP_MODE && 
                statusInfo.status != NetworkStatus::CONNECTING) {
                statusInfo.status = NetworkStatus::AP_MODE;
            }
        }
    } else {
        statusInfo.apClientCount = 0;
        statusInfo.apIPAddress = "";
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
            modeTransitioning = false;  // 连接成功，清除模式切换标志

            LOG_INFO("WiFiManager: Got IP: " + statusInfo.ipAddress);

            // 更新网络信息
            statusInfo.currentGateway = WiFi.gatewayIP().toString();
            statusInfo.currentSubnet = WiFi.subnetMask().toString();
            statusInfo.dnsServer = WiFi.dnsIP(0).toString();
            
            // 触发WiFi连接成功系统事件
            PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_WIFI_CONNECTED, statusInfo.ipAddress);
            
            char buffer[100];
            snprintf(buffer, sizeof(buffer), "WiFi connected: %s", statusInfo.ipAddress.c_str());
            triggerEvent(NetworkStatus::CONNECTED, buffer);
            break;
            
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            statusInfo.status = NetworkStatus::DISCONNECTED;
            connecting = false;
            // 触发WiFi断开连接系统事件
            PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_WIFI_DISCONNECTED, "");
            // 模式切换时的断开是预期行为，不记录警告
            if (modeTransitioning) {
                LOG_DEBUG("WiFiManager: WiFi STA disconnected (mode transition)");
            } else {
                LOG_WARNING("WiFiManager: WiFi STA disconnected");
                // 非模式切换的断开，可能是连接失败
                if (connectingStartTime > 0) {
                    // 曾尝试连接但失败了
                    PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_WIFI_CONN_FAILED, "");
                }
            }
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

void WiFiManager::setModeTransitioning(bool transitioning) {
    modeTransitioning = transitioning;
    if (transitioning) {
        LOG_DEBUG("WiFiManager: Mode transition started");
    }
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

