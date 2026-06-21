/**
 * @file WiFiManager.cpp
 * @brief WiFi 管理器实现
 * @author kerwincui
 * @date 2026-03-03
 */

#include "network/WiFiManager.h"
#include "systems/LoggerSystem.h"
#include "utils/NetworkUtils.h"
#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecManager.h"
#endif
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
    // 多 SSID 择优逻辑：如果配置了 networks 列表，扫描并选择最佳网络
    String targetSSID = wifiConfig.staSSID;
    String targetPassword = wifiConfig.staPassword;

    if (!wifiConfig.networks.empty()) {
        String bestSSID, bestPassword;
        if (selectBestNetwork(bestSSID, bestPassword)) {
            targetSSID = bestSSID;
            targetPassword = bestPassword;
            LOG_INFO("WiFiManager: Selected best network: " + targetSSID);
        } else {
            LOG_WARNING("WiFiManager: No configured network found in scan, using primary SSID");
        }
    }

    if (targetSSID.isEmpty()) {
        LOG_INFO("WiFiManager: No STA SSID configured");
        return false;
    }

    // 在纯 STA 模式下，断开已有连接再重新连接
    wl_status_t currentStatus = WiFi.status();
    if (currentStatus == WL_CONNECTED || currentStatus == WL_IDLE_STATUS) {
        LOG_DEBUG("WiFiManager: Disconnecting before new connection attempt");
        WiFi.disconnect(false);
        delay(50);  // 短暂延迟确保断开完成
    }

    // 确保 WiFi 模式正确
    WiFiMode_t currentMode = WiFi.getMode();

    if (wifiConfig.mode == NetworkMode::NETWORK_STA) {
        // STA 模式策略：
        // - 如果当前是 AP+STA 双模式，保持不变（AP 回退正在使用）
        // - 如果当前不包含 STA（如纯 AP 或 NULL），才切换到 STA
        if (!(currentMode & WIFI_STA)) {
            WiFi.mode(WIFI_STA);
            delay(500);  // AP→STA 模式切换后等待 WiFi 子系统重新就绪
        }
        // AP+STA 模式保持原样：WiFi.begin() 在 AP+STA 下同样有效
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
    WiFi.begin(targetSSID.c_str(), targetPassword.c_str());
    staInitialized = true;  // 标记STA已初始化

    Serial.printf("[WiFi] Connecting: ssid=%s ip_mode=%s timeout=%lums\n",
                  targetSSID.c_str(),
                  (wifiConfig.ipConfigType == IPConfigType::STATIC) ? "STATIC" : "DHCP",
                  (unsigned long)wifiConfig.connectTimeout);

    char buffer[100];
    snprintf(buffer, sizeof(buffer), "Connecting to WiFi: %s", targetSSID.c_str());
    LOG_INFO("WiFiManager: Attempting to connect to " + targetSSID);
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
    // 1. WiFi模式正确（纯AP模式必须是WIFI_MODE_AP）
    // 2. softAPIP有效
    // 3. AP热点确实在运行（可以通过检查连接数或SSID来验证）
    WiFiMode_t currentMode = WiFi.getMode();
    WiFiMode_t targetMode = WIFI_MODE_AP;

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

    LOG_INFO("WiFiManager: Setting WiFi mode to AP-only");
    if (!WiFi.mode(targetMode)) {
        LOG_ERROR("WiFiManager: Failed to set WiFi mode");
        return false;
    }

    // 配置 AP 网络参数（必须在 softAP 之前调用）
    // 使用配置的固定IP，如果冲突则自动切换备用网段
    IPAddress apIP;
    IPAddress apGateway;
    IPAddress apSubnet(255, 255, 255, 0);

    // 解析配置的AP IP
    if (!wifiConfig.apIP.isEmpty() && apIP.fromString(wifiConfig.apIP)) {
        apGateway = apIP;  // 网关即为AP自身
    } else {
        apIP = IPAddress(192, 168, 4, 1);
        apGateway = apIP;
    }

    // 检测是否与STA网段冲突（如果同时存在STA连接）
    if (WiFi.status() == WL_CONNECTED) {
        IPAddress staIP = WiFi.localIP();
        // 比较前三字节（同一子网即冲突）
        if ((staIP[0] == apIP[0]) && (staIP[1] == apIP[1]) && (staIP[2] == apIP[2])) {
            LOG_WARNING("WiFiManager: AP IP conflicts with STA subnet, switching to backup");
            apIP = IPAddress(192, 168, 44, 1);
            apGateway = apIP;
        }
    }

    // 备用IP列表（当主配置IP无法使用时尝试）
    IPAddress fallbackIPs[] = {
        apIP,                          // 配置的主IP
        IPAddress(192, 168, 44, 1),    // 备用网段1
        IPAddress(10, 10, 10, 1),      // 备用网段2
        IPAddress(172, 16, 0, 1)       // 备用网段3
    };

    bool apConfigured = false;
    for (int i = 0; i < 4; i++) {
        apIP = fallbackIPs[i];
        apGateway = apIP;
        if (WiFi.softAPConfig(apIP, apGateway, apSubnet)) {
            LOGGER.infof("WiFiManager: AP IP configured: %s", apIP.toString().c_str());
            apConfigured = true;
            break;
        }
        LOG_WARNING("WiFiManager: Failed to configure AP with IP: " + apIP.toString() + ", trying next...");
    }

    if (!apConfigured) {
        LOG_ERROR("WiFiManager: All AP IP configurations failed");
        // 最后尝试不配置IP，使用默认值
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
    Serial.printf("[WiFi] AP Started: ssid=%s pwd=%s ip=%s ch=%d hidden=%d max_conn=%d\n",
                  apSSID.c_str(),
                  wifiConfig.apPassword.c_str(),
                  statusInfo.apIPAddress.c_str(),
                  wifiConfig.apChannel,
                  wifiConfig.apHidden,
                  wifiConfig.apMaxConnections);

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
        // 返回具体加密类型，便于前端自动匹配安全类型下拉选项
        wifi_auth_mode_t authMode = WiFi.encryptionType(i);
        const char* enc;
        switch (authMode) {
            case WIFI_AUTH_OPEN:             enc = "open"; break;
            case WIFI_AUTH_WEP:              enc = "wep";  break;
            case WIFI_AUTH_WPA_PSK:          enc = "wpa";  break;
            case WIFI_AUTH_WPA2_PSK:         enc = "wpa2"; break;
            case WIFI_AUTH_WPA_WPA2_PSK:     enc = "wpa2"; break;  // WPA/WPA2混合，归类为wpa2
#if defined(WIFI_AUTH_WPA3_PSK)
            case WIFI_AUTH_WPA3_PSK:         enc = "wpa3"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK:    enc = "wpa3"; break;  // WPA2/WPA3混合，归类为wpa3
#endif
            default:                         enc = "wpa2"; break;  // 未知加密默认wpa2
        }
        network["encryption"] = enc;
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

            // WiFi已连接就标记为CONNECTED状态（不管是否有互联网）
            // internetAvailable 用于区分"WiFi已连接但无互联网"的场景
            if (statusInfo.status != NetworkStatus::CONNECTED) {
                statusInfo.status = NetworkStatus::CONNECTED;
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
                    {
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
            LOG_INFO("WiFiManager: WiFi STA connected (awaiting IP...)");
            Serial.printf("[WiFi] STA connected to: %s ch=%d\n",
                          WiFi.SSID().c_str(), WiFi.channel());
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
            Serial.printf("[WiFi] Connected! IP=%s GW=%s RSSI=%d DNS=%s\n",
                          WiFi.localIP().toString().c_str(),
                          WiFi.gatewayIP().toString().c_str(),
                          WiFi.RSSI(),
                          WiFi.dnsIP(0).toString().c_str());

            // 更新网络信息
            statusInfo.currentGateway = WiFi.gatewayIP().toString();
            statusInfo.currentSubnet = WiFi.subnetMask().toString();
            statusInfo.dnsServer = WiFi.dnsIP(0).toString();

            // 触发WiFi连接成功系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
            PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_WIFI_CONNECTED, statusInfo.ipAddress);
#endif

            char buffer[100];
            snprintf(buffer, sizeof(buffer), "WiFi connected: %s", statusInfo.ipAddress.c_str());
            triggerEvent(NetworkStatus::CONNECTED, buffer);
            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            // ESP32-C3 在 STA+AP 双模下可能会收到此事件，但实际 STA 仍然连接
            // 需要检查实际连接状态，避免误报
            if (WiFi.status() == WL_CONNECTED) {
                LOG_DEBUG("WiFiManager: STA disconnected event received but still connected (dual-mode behavior)");
                return;  // 忽略误报事件
            }

            statusInfo.status = NetworkStatus::DISCONNECTED;
            connecting = false;
            Serial.printf("[WiFi] Disconnected! reason=%d reconnects=%d\n",
                          (int)WiFi.status(), (int)statusInfo.reconnectAttempts);
            // 触发WiFi断开连接系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
            PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_WIFI_DISCONNECTED, "");
#endif
            // 模式切换时的断开是预期行为，不记录警告
            if (modeTransitioning) {
                LOG_DEBUG("WiFiManager: WiFi STA disconnected (mode transition)");
            } else {
                LOG_WARNING("WiFiManager: WiFi STA disconnected");
                // 非模式切换的断开，可能是连接失败
                if (connectingStartTime > 0) {
                    // 曾尝试连接但失败了
#if FASTBEE_ENABLE_PERIPH_EXEC
                    PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_WIFI_CONN_FAILED, "");
#endif
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

bool WiFiManager::selectBestNetwork(String& outSSID, String& outPassword) {
    if (wifiConfig.networks.empty()) {
        return false;
    }

    // 扫描可用网络
    LOG_INFO("WiFiManager: Scanning for best network...");
    int numFound = WiFi.scanNetworks(false, false, false, 300);
    if (numFound <= 0) {
        LOG_WARNING("WiFiManager: No networks found in scan");
        WiFi.scanDelete();
        // 扫描失败时，返回优先级最高的网络
        outSSID = wifiConfig.networks[0].ssid;
        outPassword = wifiConfig.networks[0].password;
        return true;
    }

    // 在扫描结果中匹配配置的网络，按 priority 分组后取 RSSI 最高者
    struct Candidate {
        String ssid;
        String password;
        uint8_t priority;
        int32_t rssi;
    };
    std::vector<Candidate> candidates;

    for (int i = 0; i < numFound; i++) {
        String scannedSSID = WiFi.SSID(i);
        int32_t scannedRSSI = WiFi.RSSI(i);

        for (const auto& net : wifiConfig.networks) {
            if (net.ssid == scannedSSID) {
                candidates.push_back({scannedSSID, net.password, net.priority, scannedRSSI});
                break;
            }
        }
    }
    WiFi.scanDelete();

    if (candidates.empty()) {
        LOG_WARNING("WiFiManager: None of configured networks found in scan");
        return false;
    }

    // 排序：先按 priority 升序，同 priority 按 RSSI 降序
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.rssi > b.rssi;
    });

    outSSID = candidates[0].ssid;
    outPassword = candidates[0].password;
    LOGGER.infof("WiFiManager: Best network: %s (priority=%d, RSSI=%d)",
                 outSSID.c_str(), candidates[0].priority, candidates[0].rssi);
    return true;
}

