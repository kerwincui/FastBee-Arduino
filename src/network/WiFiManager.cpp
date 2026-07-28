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
#include <esp_heap_caps.h>  // heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
#include "systems/HealthMonitor.h"  // WIFI_CONNECT_MIN_DRAM / WIFI_RECONN_MIN_DRAM
#include "core/FastBeeFramework.h"  // WiFi GOT_IP → MQTT 立即重连通知
#include "protocols/ProtocolManager.h"
#include "protocols/MQTTClient.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecManager.h"
#endif
#include <ArduinoJson.h>
#include <LittleFS.h>

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
    // ── DRAM 内存保护：检查 DRAM 内部空闲（排除 PSRAM）──────────────────
    // WiFi.begin() 会引发 lwIP TCP/IP 栈 + 驱动初始化，需要约 12-16KB DRAM
    // PSRAM 不能用于 WiFi 内部缓冲区，必须检测 MALLOC_CAP_INTERNAL
    {
        uint32_t dramFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (dramFree < WIFI_CONNECT_MIN_DRAM) {
            Serial.printf("[WiFi] connectToWiFi: DRAM too low (dram=%lu < %lu), skipping\n",
                          (unsigned long)dramFree, (unsigned long)WIFI_CONNECT_MIN_DRAM);
            LOG_WARNINGF("[WiFi] DRAM too low for WiFi.begin (dram=%lu need=%lu)",
                         (unsigned long)dramFree, (unsigned long)WIFI_CONNECT_MIN_DRAM);
            return false;
        }
        Serial.printf("[WiFi] connectToWiFi: DRAM OK (dram=%lu)\n", (unsigned long)dramFree);
    }
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

    // 断开已有连接再重新连接
    // 必须在 WiFi.begin() 前确保 STA 不在连接中状态，否则 ESP-IDF 会报
    // "sta is connecting, cannot set config" (ESP_ERR_WIFI_STATE)
    {
        wl_status_t currentStatus = WiFi.status();
        if (currentStatus != WL_DISCONNECTED) {
            LOGGER.debugf("WiFiManager: Disconnecting before new connection attempt (status=%d)", (int)currentStatus);
            WiFi.disconnect(false);
            delay(100);  // 等待 STA 状态机退出 connecting 状态
        }
    }

    // 确保 WiFi 模式正确
    WiFiMode_t currentMode = WiFi.getMode();

    if (wifiConfig.mode == NetworkMode::NETWORK_STA) {
        // STA 模式：确保 WiFi 包含 STA 接口
        // 如果当前是纯 AP 模式（回退状态），切换到 STA
        if (!(currentMode & WIFI_STA)) {
            WiFi.mode(WIFI_STA);
            delay(500);  // AP→STA 模式切换后等待 WiFi 子系统重新就绪
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

    // STA 连接可靠性增强：全信道扫描 + 按信号强度择优 AP + 关闭省电
    // 应对同名 SSID 多 AP（CMCC/放大器/双频合一）择到弱 AP 后认证被拒
    // （reason=6 NOT_AUTHED）的问题，详见 NetworkManager::connectToWiFiBlocking。
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    WiFi.setSleep(false);

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

    // 从设备配置(device.json)读取设备名称，用于 AP SSID 自动生成
    String devName = _readDeviceName();

    if (currentMode == targetMode && WiFi.softAPIP() != IPAddress(0, 0, 0, 0)) {
        // 模式正确且IP有效，检查热点是否真正在广播
        // 通过检查是否能获取SSID来验证
        String currentSSID = WiFi.softAPSSID();
        String expectedSSID = wifiConfig.apSSID.isEmpty() ?
            (devName + "_" + getChipID().substring(0, 6)) : wifiConfig.apSSID;

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
        apSSID = devName;
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
    // 使用异步扫描 + 定时 yield，避免阻塞 Web 服务器
    int numNetworks = WiFi.scanNetworks(true);  // async=true
    if (numNetworks == WIFI_SCAN_RUNNING || numNetworks == WIFI_SCAN_FAILED) {
        unsigned long startMs = millis();
        while (millis() - startMs < 8000) {
            delay(10);
            numNetworks = WiFi.scanComplete();
            if (numNetworks >= 0) break;
        }
    } else {
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
        int ch = WiFi.channel(i);
        network["channel"] = ch;
        // 频段标识：ESP32 全系列仅支持 2.4GHz (channel 1-14)
        network["band"] = (ch > 14) ? "5GHz" : "2.4GHz";
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
    WiFi.scanDelete();  // 释放扫描结果内存

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
    // 注意：此函数运行在 arduino_events 任务栈上，必须保持轻量
    // 禁止使用 LOGGER 日志宏（内部有 256 字节栈缓冲区），统一用 Serial.printf
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.printf("[WiFi] STA connected: %s ch=%d\n",
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
            modeTransitioning = false;

            Serial.printf("[WiFi] Connected! IP=%s GW=%s RSSI=%d\n",
                          statusInfo.ipAddress.c_str(),
                          WiFi.gatewayIP().toString().c_str(),
                          WiFi.RSSI());

            // 更新网络信息
            statusInfo.currentGateway = WiFi.gatewayIP().toString();
            statusInfo.currentSubnet = WiFi.subnetMask().toString();
            statusInfo.dnsServer = WiFi.dnsIP(0).toString();

            // 重量级后续处理（PeriphExec 规则分发/MQTT 退避重置/上层回调）
            // 延迟到 loopTask 的 processPendingEvents() 执行，
            // 避免在 arduino_events 任务栈上执行导致栈溢出崩溃
            pendingGotIPEvent = true;
            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            // ESP32-C3 在 STA+AP 双模下可能会收到此事件，但实际 STA 仍然连接
            if (WiFi.status() == WL_CONNECTED) {
                return;  // 忽略误报事件
            }

            statusInfo.status = NetworkStatus::DISCONNECTED;
            connecting = false;
            Serial.printf("[WiFi] Disconnected! reason=%d reconnects=%d\n",
                          (int)WiFi.status(), (int)statusInfo.reconnectAttempts);
            // 重量级后续处理延迟到 loopTask 执行（同上）
            if (!modeTransitioning && connectingStartTime > 0) {
                pendingConnFailedEvent = true;
            }
            pendingDisconnectEvent = true;
            break;

        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            statusInfo.apClientCount = WiFi.softAPgetStationNum();
            Serial.printf("[WiFi] AP client connected (total=%d)\n", statusInfo.apClientCount);
            break;

        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            statusInfo.apClientCount = WiFi.softAPgetStationNum();
            Serial.printf("[WiFi] AP client disconnected (total=%d)\n", statusInfo.apClientCount);
            break;

        default:
            break;
    }
}

void WiFiManager::processPendingEvents() {
    // 在 loopTask 上执行 WiFi 事件的重量级后续处理，
    // 标志由 handleWiFiEvent（arduino_events 栈）置位
    if (pendingGotIPEvent) {
        pendingGotIPEvent = false;

        // 触发WiFi连接成功系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
        PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_WIFI_CONNECTED, statusInfo.ipAddress);
#endif

        // WiFi 重连成功后，通知 MQTT 客户端重置退避计数器
#if FASTBEE_ENABLE_MQTT
        {
            auto* fw = FastBeeFramework::getInstance();
            auto* pm = fw ? fw->getProtocolManager() : nullptr;
            MQTTClient* mqtt = pm ? pm->getMQTTClient() : nullptr;
            if (mqtt) {
                mqtt->resetErrorCounters();
            }
        }
#endif

        triggerEvent(NetworkStatus::CONNECTED, statusInfo.ipAddress.c_str());
    }

    if (pendingDisconnectEvent) {
        pendingDisconnectEvent = false;
#if FASTBEE_ENABLE_PERIPH_EXEC
        PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_WIFI_DISCONNECTED, "");
#endif
        triggerEvent(NetworkStatus::DISCONNECTED, "WiFi disconnected");
    }

    if (pendingConnFailedEvent) {
        pendingConnFailedEvent = false;
#if FASTBEE_ENABLE_PERIPH_EXEC
        PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_WIFI_CONN_FAILED, "");
#endif
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

    // ── DRAM 内存保护（轻量检查）────────────────────────────
    {
        uint32_t dramFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (dramFree < WIFI_RECONN_MIN_DRAM) {
            Serial.printf("[WiFi] attemptReconnect: DRAM too low (dram=%lu < %lu), deferring\n",
                          (unsigned long)dramFree, (unsigned long)WIFI_RECONN_MIN_DRAM);
            // 延迟重连而非失败，等内存回收后再试
            return;
        }
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

String WiFiManager::_readDeviceName() {
    // 从设备配置(device.json)读取设备名称，用于 AP SSID 自动生成
    // deviceName 已移至设备配置统一管理，不再存储在 network.json 中
    const char* devicePath = "/config/device.json";
    if (LittleFS.exists(devicePath)) {
        File f = LittleFS.open(devicePath, "r");
        if (f) {
            JsonDocument doc;
            if (!deserializeJson(doc, f)) {
                f.close();
                if (doc["deviceName"].is<String>()) {
                    String name = doc["deviceName"].as<String>();
                    if (name.length() > 0) return name;
                }
            } else {
                f.close();
            }
        }
    }
    return "FastBee";  // 默认值
}

String WiFiManager::getChipID() {
    uint64_t chipid = ESP.getEfuseMac();
    char chipidStr[13];
    snprintf(chipidStr, sizeof(chipidStr), "%04X%08X",
             (uint16_t)(chipid >> 32), (uint32_t)chipid);
    return String(chipidStr);
}

// ── WiFi 状态码可读映射 ──────────────────────────────────────────────
const char* WiFiManager::wifiStatusToString(wl_status_t status) {
    switch (status) {
        case WL_IDLE_STATUS:      return "IDLE";
        case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL (network not found)";
        case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
        case WL_CONNECTED:       return "CONNECTED";
        case WL_CONNECT_FAILED:  return "CONNECT_FAILED (auth/assoc rejected)";
        case WL_CONNECTION_LOST: return "CONNECTION_LOST";
        case WL_DISCONNECTED:    return "DISCONNECTED";
        default:                 return "UNKNOWN";
    }
}

// ── WiFi 频段诊断 ──────────────────────────────────────────────────────
BandDiagnosis WiFiManager::diagnoseBandMismatch(const String& ssid) {
    // 执行快速扫描，检查目标 SSID 的频段分布
    int numFound = WiFi.scanNetworks(false, false, false, 300);
    if (numFound <= 0) {
        WiFi.scanDelete();
        Serial.printf("[WiFi-Diag] Scan returned 0 networks\n");
        return BandDiagnosis::NOT_FOUND;
    }

    bool has24GHz = false;
    bool has5GHz = false;
    int matchCount = 0;

    for (int i = 0; i < numFound; i++) {
        if (WiFi.SSID(i) == ssid) {
            matchCount++;
            int ch = WiFi.channel(i);
            int32_t rssi = WiFi.RSSI(i);
            const char* band = (ch > 14) ? "5GHz" : "2.4GHz";

            if (ch > 14) {
                has5GHz = true;
            } else {
                has24GHz = true;
            }

            // 输出详细诊断信息
            Serial.printf("[WiFi-Diag] Found '%s' BSSID=%s ch=%d band=%s RSSI=%d enc=%d\n",
                          ssid.c_str(), WiFi.BSSIDstr(i).c_str(),
                          ch, band, (int)rssi, (int)WiFi.encryptionType(i));
        }
    }
    WiFi.scanDelete();

    if (matchCount == 0) {
        Serial.printf("[WiFi-Diag] SSID '%s' not found in %d scanned networks\n",
                      ssid.c_str(), numFound);
        LOG_WARNING("WiFiManager: Band diagnosis - target SSID not found in scan");
        return BandDiagnosis::NOT_FOUND;
    }

    if (has5GHz && !has24GHz) {
        Serial.printf("[WiFi-Diag] ERROR: '%s' only broadcasts on 5GHz (ch>14). "
                      "ESP32 only supports 2.4GHz (ch 1-14).\n", ssid.c_str());
        LOG_ERROR("WiFiManager: Network only supports 5GHz but device only supports 2.4GHz!");
        return BandDiagnosis::ONLY_5GHZ;
    }

    if (has24GHz && has5GHz) {
        Serial.printf("[WiFi-Diag] '%s' has both 2.4GHz and 5GHz bands (dual-band OK)\n",
                      ssid.c_str());
        return BandDiagnosis::MIXED_BAND;
    }

    // has24GHz && !has5GHz
    Serial.printf("[WiFi-Diag] '%s' is on 2.4GHz — band is compatible\n", ssid.c_str());
    return BandDiagnosis::BAND_OK;
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

