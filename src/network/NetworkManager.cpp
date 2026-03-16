/**
 * @file NetworkManager.cpp
 * @brief 网络管理器实现
 * @author kerwincui
 * @date 2025-12-02
 */

#include "network/NetworkManager.h"
#include "systems/LoggerSystem.h"
#include "utils/NetworkUtils.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

static const char* NETWORK_CONFIG_FILE = "/config/network.json";

NetworkManager::NetworkManager(AsyncWebServer* webServerPtr) 
    : webServer(webServerPtr),
      lastReconnectAttempt(0),
      lastStatusUpdate(0),
      lastConflictCheck(0),
      autoReconnectEnabled(true),
      isInitialized(false),
      connecting(false),
      pendingRestart(false),
      connectingStartTime(0),
      pendingRestartTime(0) {
    
    wifiConfig = WiFiConfig();
    statusInfo = NetworkStatusInfo();
    
    // 初始化子模块
    wifiManager.reset(new WiFiManager());
    ipManager.reset(new IPManager());
    dnsManager.reset(new DNSManager());
}

NetworkManager::~NetworkManager() {
    disconnect();
    if (preferences.isKey("initialized")) {
        preferences.end();
    }
}

void NetworkManager::setWiFiCredentials(const String& ssid, const String& password) {
    wifiConfig.staSSID = ssid;
    wifiConfig.staPassword = password;
    wifiConfig.mode = NetworkMode::NETWORK_STA;  // 强制使用 STA 模式
    
    char buf[128];
    snprintf(buf, sizeof(buf), "NetworkManager: WiFi credentials set - SSID='%s'", ssid.c_str());
    LOG_INFO(buf);
}

bool NetworkManager::initialize() {
    if (isInitialized) {
        return true;
    }

    // 初始化Preferences（独立 namespace，避免与 ConfigStorage 的 "fastbee" 冲突）
    if (!preferences.begin("net_cfg", false)) {
        LOG_ERROR("NetworkManager: Failed to initialize preferences");
        return false;
    }

    // 加载网络配置
    if (!loadNetworkConfig()) {
        LOG_WARNING("NetworkManager: Using default network configuration");
        // 生成默认备用IP
        ipManager->generateBackupIPs();
        saveNetworkConfig();
    }
    
    LOG_INFOF("NetworkManager: Config loaded - enableMDNS=%s, customDomain=%s", 
              wifiConfig.enableMDNS ? "true" : "false", 
              wifiConfig.customDomain.c_str());

    // 初始化子模块
    if (!wifiManager->initialize()) {
        LOG_ERROR("NetworkManager: Failed to initialize WiFi manager");
        return false;
    }
    
    if (!ipManager->initialize()) {
        LOG_ERROR("NetworkManager: Failed to initialize IP manager");
        return false;
    }
    
    if (!dnsManager->initialize()) {
        LOG_ERROR("NetworkManager: Failed to initialize DNS manager");
        return false;
    }

    // 设置WiFi事件回调
    WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
        wifiManager->handleWiFiEvent(event);
    });

    // 网络启动策略：
    // 1. NETWORK_STA (0): 仅STA模式，尝试连接WiFi，失败则回退AP模式
    // 2. NETWORK_AP (1): 仅AP模式，直接启动热点
    // 3. NETWORK_AP_STA (2): AP+STA双模式，同时启动热点和连接WiFi
    bool success = false;
    
    switch (wifiConfig.mode) {
        case NetworkMode::NETWORK_STA:
            // 仅STA模式
            LOG_INFO("NetworkManager: Starting in STA-only mode");
            if (!wifiConfig.staSSID.isEmpty()) {
                success = connectToWiFiBlocking();
                if (success) {
                    LOG_INFO("NetworkManager: STA connected successfully");
                    LOGGER.infof(">>> STA IP: %s <<<", WiFi.localIP().toString().c_str());
                } else {
                    LOG_WARNING("NetworkManager: STA connection failed, falling back to AP mode");
                    success = startAPMode();
                    if (success) {
                        LOGGER.infof(">>> AP IP: %s  Connect to [%s] pwd:[%s] <<<",
                            WiFi.softAPIP().toString().c_str(),
                            wifiConfig.apSSID.c_str(),
                            wifiConfig.apPassword.c_str());
                    }
                }
            } else {
                LOG_WARNING("NetworkManager: No staSSID configured, starting AP mode");
                success = startAPMode();
            }
            break;
            
        case NetworkMode::NETWORK_AP:
            // 仅AP模式
            LOG_INFO("NetworkManager: Starting in AP-only mode");
            success = startAPMode();
            if (success) {
                LOGGER.infof(">>> AP IP: %s  Connect to [%s] pwd:[%s] <<<",
                    WiFi.softAPIP().toString().c_str(),
                    wifiConfig.apSSID.c_str(),
                    wifiConfig.apPassword.c_str());
            }
            break;
            
        case NetworkMode::NETWORK_AP_STA:
            // AP+STA双模式：先启动AP，再尝试连接WiFi
            LOG_INFO("NetworkManager: Starting in AP+STA dual mode");
            
            // 启动AP热点（startAPMode会自动设置正确的WiFi模式）
            success = startAPMode();
            if (success) {
                LOGGER.infof(">>> AP IP: %s  Connect to [%s] pwd:[%s] <<<",
                    WiFi.softAPIP().toString().c_str(),
                    wifiConfig.apSSID.c_str(),
                    wifiConfig.apPassword.c_str());
            } else {
                LOG_ERROR("NetworkManager: Failed to start AP mode in AP+STA");
                break;
            }
            
            // 尝试连接WiFi（不阻塞，让它在后台连接）
            if (!wifiConfig.staSSID.isEmpty()) {
                LOG_INFO("NetworkManager: Attempting to connect to WiFi in background...");
                wifiManager->setNetworkConfig(wifiConfig);
                if (wifiManager->connectToWiFi()) {
                    // 等待一小段时间看能否快速连接
                    unsigned long startTime = millis();
                    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 5000) {
                        delay(100);
                    }
                    if (WiFi.status() == WL_CONNECTED) {
                        LOGGER.infof(">>> STA IP: %s <<<", WiFi.localIP().toString().c_str());
                    } else {
                        LOG_INFO("NetworkManager: WiFi connecting in background, AP is available");
                    }
                }
            } else {
                LOG_INFO("NetworkManager: No staSSID configured, AP mode only");
            }
            break;
            
        default:
            LOG_ERROR("NetworkManager: Unknown network mode, defaulting to AP");
            success = startAPMode();
            break;
    }

    isInitialized = success;
    if (success) {
        LOG_INFO("NetworkManager: Initialized successfully");
    } else {
        LOG_ERROR("NetworkManager: All network modes failed (check AP config)");
    }

    return success;
}

void NetworkManager::disconnect() {
    LOG_INFO("NetworkManager: Disconnecting all network connections...");
    
    // 停止DNS服务器和mDNS
    dnsManager->stopDNSServer();
    dnsManager->stopMDNS();
    
    // 停止AP模式和WiFi连接
    wifiManager->stopAPMode();
    wifiManager->disconnectWiFi();
    
    isInitialized = false;
    autoReconnectEnabled = false;
    connecting = false;
    connectingStartTime = 0;
    
    // 重置状态信息
    statusInfo = NetworkStatusInfo();
    statusInfo.status = NetworkStatus::DISCONNECTED;
    
    LOG_INFO("NetworkManager: All network connections disconnected");
}

void NetworkManager::update() {
    unsigned long currentTime = millis();
    static bool wasConnected = false;
    static unsigned long lastMdnsUpdate = 0;

    // 处理延迟重启（配置保存后延迟500ms执行，确保HTTP响应已返回）
    if (pendingRestart) {
        if (pendingRestartTime == 0) {
            pendingRestartTime = currentTime;
        } else if (currentTime - pendingRestartTime >= 500) {
            LOG_INFO("NetworkManager: Executing delayed network restart...");
            pendingRestart = false;
            pendingRestartTime = 0;
            restartNetwork();
            return; // 重启后返回，下次循环再更新状态
        }
    }

    // 检测 WiFi 连接状态变化
    bool isConnected = (WiFi.status() == WL_CONNECTED);
    
    // WiFi 连接成功（从断开变为连接）
    if (isConnected && !wasConnected) {
        LOG_INFO("NetworkManager: WiFi connected, IP: " + WiFi.localIP().toString());
        statusInfo.reconnectAttempts = 0;  // 重置重连计数器
        statusInfo.status = NetworkStatus::CONNECTED;
        
        // 重新启动 mDNS（使用配置中的 customDomain）
        LOG_INFOF("NetworkManager: Checking mDNS - enableMDNS=%s, customDomain=%s", 
                  wifiConfig.enableMDNS ? "true" : "false", 
                  wifiConfig.customDomain.c_str());
        if (wifiConfig.enableMDNS) {
            MDNS.end();
            String hostname = wifiConfig.customDomain.length() > 0 ? wifiConfig.customDomain : "fastbee";
            LOG_INFOF("NetworkManager: Starting mDNS with hostname: %s", hostname.c_str());
            if (MDNS.begin(hostname.c_str())) {
                MDNS.addService("http", "tcp", 80);
                LOGGER.infof("NetworkManager: mDNS started as %s.local", hostname.c_str());
            } else {
                LOG_ERROR("NetworkManager: Failed to start mDNS");
            }
        } else {
            LOG_INFO("NetworkManager: mDNS is disabled");
        }
    }
    
    // WiFi 断开（从连接变为断开）
    if (!isConnected && wasConnected) {
        LOG_WARNING("NetworkManager: WiFi disconnected");
        statusInfo.status = NetworkStatus::DISCONNECTED;
    }
    
    wasConnected = isConnected;

    // 更新状态信息（每秒一次）
    if (currentTime - lastStatusUpdate >= 1000) {
        wifiManager->updateStatusInfo();
        lastStatusUpdate = currentTime;
    }

    // 处理DNS请求（如果DNS服务器已启动）
    dnsManager->processDNSRequests();

    // IP冲突检测（按配置间隔）
    if (currentTime - lastConflictCheck >= wifiConfig.conflictCheckInterval) {
        if (statusInfo.status == NetworkStatus::CONNECTED &&
            wifiConfig.ipConfigType == IPConfigType::STATIC) {
            
            ipManager->updateIPConflictStatus();
        }
        lastConflictCheck = currentTime;
    }

    // 处理连接超时
    if (connecting && (currentTime - connectingStartTime >= wifiConfig.connectTimeout)) {
        LOG_WARNING("NetworkManager: Connection timeout");
        connecting = false;
        
        if (wifiConfig.autoFailover) {
            LOG_INFO("NetworkManager: Attempting failover...");
            ipManager->performFailover();
        } else {
            statusInfo.status = NetworkStatus::CONNECTION_FAILED;
            triggerEvent(NetworkStatus::CONNECTION_FAILED, "Connection timeout");
        }
    }

    // 自动重连逻辑
    if (autoReconnectEnabled && 
        (wifiConfig.mode == NetworkMode::NETWORK_STA || wifiConfig.mode == NetworkMode::NETWORK_AP_STA) &&
        !connecting &&
        !isConnected &&
        currentTime - lastReconnectAttempt >= wifiConfig.reconnectInterval) {
        
        attemptReconnect();
    }
    
    // mDNS 维护（每 30 秒）
    if (isConnected && currentTime - lastMdnsUpdate >= 30000) {
        // ESP32 的 mDNS 不需要手动 update，但可以定期检查
        lastMdnsUpdate = currentTime;
    }
    
    // 清理冲突缓存（每小时一次）
    static unsigned long lastCacheClean = 0;
    if (currentTime - lastCacheClean >= 3600000) {
        ipManager->cleanupConflictCache();
        lastCacheClean = currentTime;
    }
}

bool NetworkManager::loadNetworkConfig() {
    LOG_INFO("NetworkManager: Loading network configuration...");
    
    // 加载辅助函数：从文件读取配置
    auto loadFromFile = [this](const char* path) -> bool {
        LOG_DEBUGF("NetworkManager: Trying to load config from %s", path);
        File f = LittleFS.open(path, "r");
        if (!f) {
            LOG_DEBUGF("NetworkManager: File not found: %s", path);
            return false;
        }
        LOG_DEBUGF("NetworkManager: File opened: %s (size: %d bytes)", path, f.size());
        
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) {
            LOGGER.errorf("NetworkManager: JSON parse error (%s): %s", path, err.c_str());
            return false;
        }
        LOG_DEBUG("NetworkManager: JSON parsed successfully");
        if (doc.containsKey("mode"))                 wifiConfig.mode = static_cast<NetworkMode>(doc["mode"].as<uint8_t>());
        if (doc.containsKey("deviceName"))           wifiConfig.deviceName = doc["deviceName"].as<String>();
        if (doc.containsKey("apSSID"))               wifiConfig.apSSID = doc["apSSID"].as<String>();
        if (doc.containsKey("apPassword"))           wifiConfig.apPassword = doc["apPassword"].as<String>();
        if (doc.containsKey("apChannel"))            wifiConfig.apChannel = doc["apChannel"].as<uint8_t>();
        if (doc.containsKey("apHidden"))             wifiConfig.apHidden = doc["apHidden"].as<bool>();
        if (doc.containsKey("apMaxConnections"))     wifiConfig.apMaxConnections = doc["apMaxConnections"].as<uint8_t>();
        if (doc.containsKey("staSSID"))              wifiConfig.staSSID = doc["staSSID"].as<String>();
        if (doc.containsKey("staPassword"))          wifiConfig.staPassword = doc["staPassword"].as<String>();
        if (doc.containsKey("ipConfigType"))         wifiConfig.ipConfigType = static_cast<IPConfigType>(doc["ipConfigType"].as<uint8_t>());
        if (doc.containsKey("staticIP"))             wifiConfig.staticIP = doc["staticIP"].as<String>();
        if (doc.containsKey("gateway"))              wifiConfig.gateway = doc["gateway"].as<String>();
        if (doc.containsKey("subnet"))               wifiConfig.subnet = doc["subnet"].as<String>();
        if (doc.containsKey("dns1"))                 wifiConfig.dns1 = doc["dns1"].as<String>();
        if (doc.containsKey("dns2"))                 wifiConfig.dns2 = doc["dns2"].as<String>();
        if (doc.containsKey("connectTimeout"))       wifiConfig.connectTimeout = doc["connectTimeout"].as<uint32_t>();
        if (doc.containsKey("reconnectInterval"))    wifiConfig.reconnectInterval = doc["reconnectInterval"].as<uint32_t>();
        if (doc.containsKey("maxReconnectAttempts")) wifiConfig.maxReconnectAttempts = doc["maxReconnectAttempts"].as<uint8_t>();
        if (doc.containsKey("customDomain"))         wifiConfig.customDomain = doc["customDomain"].as<String>();
        if (doc.containsKey("enableMDNS"))           wifiConfig.enableMDNS = doc["enableMDNS"].as<bool>();
        if (doc.containsKey("enableDNS"))            wifiConfig.enableDNS = doc["enableDNS"].as<bool>();
        if (doc.containsKey("conflictDetection"))    wifiConfig.conflictDetection = static_cast<IPConflictMode>(doc["conflictDetection"].as<uint8_t>());
        if (doc.containsKey("failoverStrategy"))     wifiConfig.failoverStrategy = static_cast<IPFailoverStrategy>(doc["failoverStrategy"].as<uint8_t>());
        if (doc.containsKey("autoFailover"))         wifiConfig.autoFailover = doc["autoFailover"].as<bool>();
        if (doc.containsKey("conflictCheckInterval")) wifiConfig.conflictCheckInterval = doc["conflictCheckInterval"].as<uint16_t>();
        if (doc.containsKey("maxFailoverAttempts"))  wifiConfig.maxFailoverAttempts = doc["maxFailoverAttempts"].as<uint8_t>();
        if (doc.containsKey("conflictThreshold"))    wifiConfig.conflictThreshold = doc["conflictThreshold"].as<uint8_t>();
        if (doc.containsKey("fallbackToDHCP"))       wifiConfig.fallbackToDHCP = doc["fallbackToDHCP"].as<bool>();
        LOGGER.infof("NetworkManager: Config loaded from %s", path);
        return true;
    };

    // 从 /config/network.json 加载配置
    LOG_DEBUGF("NetworkManager: Checking if %s exists", NETWORK_CONFIG_FILE);
    if (LittleFS.exists(NETWORK_CONFIG_FILE)) {
        LOG_DEBUG("NetworkManager: network.json found, loading...");
        if (loadFromFile(NETWORK_CONFIG_FILE)) {
            LOG_INFO("NetworkManager: Config loaded from JSON file");
            return true;
        }
        LOG_WARNING("NetworkManager: Failed to load from JSON, trying NVS fallback");
    } else {
        LOG_DEBUG("NetworkManager: network.json not found");
    }

    // 回退：从 NVS 加载
    if (!preferences.isKey("initialized")) {
        LOG_INFO("NetworkManager: No saved network config found (NVS not initialized)");
        return false;
    }
    LOG_INFO("NetworkManager: Loading config from NVS fallback...");

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
        wifiConfig.staSSID = preferences.getString("sta_ssid", "");
        wifiConfig.staPassword = preferences.getString("sta_password", "");
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
        wifiConfig.customDomain = preferences.getString("custom_domain", Network::DEFAULT_MDNS_HOSTNAME);
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
    // 构建 JSON内容共用
    JsonDocument doc;
    doc["mode"] = static_cast<uint8_t>(wifiConfig.mode);
    doc["deviceName"] = wifiConfig.deviceName;
    doc["apSSID"] = wifiConfig.apSSID;
    doc["apPassword"] = wifiConfig.apPassword;
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
    doc["enableDNS"] = wifiConfig.enableDNS;
    doc["conflictDetection"] = static_cast<uint8_t>(wifiConfig.conflictDetection);
    doc["failoverStrategy"] = static_cast<uint8_t>(wifiConfig.failoverStrategy);
    doc["autoFailover"] = wifiConfig.autoFailover;
    doc["conflictCheckInterval"] = wifiConfig.conflictCheckInterval;
    doc["maxFailoverAttempts"] = wifiConfig.maxFailoverAttempts;
    doc["conflictThreshold"] = wifiConfig.conflictThreshold;
    doc["fallbackToDHCP"] = wifiConfig.fallbackToDHCP;

    // 辅助写文件函数
    auto writeToFile = [&doc](const char* path) -> bool {
        // 确保目录存在
        String p(path);
        int slash = p.lastIndexOf('/');
        if (slash > 0) {
            String dir = p.substring(0, slash);
            if (!LittleFS.exists(dir)) {
                LittleFS.mkdir(dir);
            }
        }
        File f = LittleFS.open(path, "w");
        if (!f) return false;
        serializeJson(doc, f);
        f.close();
        return true;
    };

    // ===== 写入 /config/network.json =====
    if (writeToFile(NETWORK_CONFIG_FILE)) {
        LOG_INFO("NetworkManager: Config saved to /config/network.json");
    } else {
        LOG_WARNING("NetworkManager: Failed to write /config/network.json");
    }

    // ===== 同步写入 NVS（备份） =====
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
    if (!wifiManager->startAPMode()) {
        return false;
    }

    // 启动DNS服务器
    if (wifiConfig.enableDNS) {
        dnsManager->startDNSServer(WiFi.softAPIP());
    }

    // 启动mDNS服务（使用customDomain作为hostname）
    if (wifiConfig.enableMDNS) {
        dnsManager->startMDNS(wifiConfig.customDomain);
    }

    statusInfo.status = NetworkStatus::AP_MODE;
    statusInfo.apIPAddress = WiFi.softAPIP().toString();

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

    // 重置故障转移状态
    ipManager->generateBackupIPs();

    // 配置网络
    if (wifiConfig.ipConfigType == IPConfigType::STATIC) {
        if (!wifiManager->configureStaticIP()) {
            LOG_WARNING("NetworkManager: Static IP configuration failed");
            return false;
        }
    } else if (wifiConfig.ipConfigType == IPConfigType::DHCP) {
        wifiManager->configureDHCP();
    }

    // 设置连接状态
    statusInfo.status = NetworkStatus::CONNECTING;
    connecting = true;
    connectingStartTime = millis();

    // 开始连接
    if (!wifiManager->connectToNetwork(wifiConfig.staSSID, wifiConfig.staPassword)) {
        return false;
    }
    
    char buffer[100];
    snprintf(buffer, sizeof(buffer), "Connecting to WiFi: %s", wifiConfig.staSSID.c_str());
    LOG_INFO("WiFiManager: Attempting to connect to " + wifiConfig.staSSID);
    triggerEvent(NetworkStatus::CONNECTING, buffer);
    
    return true;
}

bool NetworkManager::connectToWiFiBlocking() {
    if (wifiConfig.staSSID.isEmpty()) {
        LOG_INFO("NetworkManager: No STA SSID configured, skipping STA connection");
        return false;
    }

    // 同步配置到 WiFiManager
    wifiManager->setNetworkConfig(wifiConfig);

    // 配置 IP 模式
    if (wifiConfig.ipConfigType == IPConfigType::STATIC) {
        if (!wifiManager->configureStaticIP()) {
            LOG_WARNING("NetworkManager: Static IP config failed, using DHCP");
            wifiManager->configureDHCP();
        }
    } else {
        wifiManager->configureDHCP();
    }

    // 设置 WiFi 模式
    WiFiMode_t currentMode = WiFi.getMode();
    if (!(currentMode & WIFI_STA)) {
        if (wifiConfig.mode == NetworkMode::NETWORK_AP_STA) {
            WiFi.mode(WIFI_MODE_APSTA);
        } else {
            WiFi.mode(WIFI_MODE_STA);
        }
    }

    // 发起连接
    WiFi.begin(wifiConfig.staSSID.c_str(), wifiConfig.staPassword.c_str());
    statusInfo.status = NetworkStatus::CONNECTING;

    char buf[80];
    snprintf(buf, sizeof(buf), "NetworkManager: Connecting to WiFi [%s]...", wifiConfig.staSSID.c_str());
    LOG_INFO(buf);
    // 调试：打印实际使用的连接凭据
    LOGGER.debugf("NetworkManager: staSSID=[%s] len=%d", wifiConfig.staSSID.c_str(), wifiConfig.staSSID.length());
    LOGGER.debugf("NetworkManager: staPassword=[%s] len=%d", wifiConfig.staPassword.c_str(), wifiConfig.staPassword.length());

    // 阻塞等待，每 500ms 打印一个点，超时后退出
    uint32_t deadline = millis() + wifiConfig.connectTimeout;
    uint32_t dotTime  = 0;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        if (millis() - dotTime >= 500) {
            dotTime = millis();
            Serial.print('.');
        }
        delay(50);
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        connecting = false;
        statusInfo.status = NetworkStatus::CONNECTED;
        statusInfo.ipAddress = WiFi.localIP().toString();
        statusInfo.ssid = WiFi.SSID();

        snprintf(buf, sizeof(buf), "NetworkManager: WiFi connected! IP: %s", statusInfo.ipAddress.c_str());
        LOG_INFO(buf);

        // STA 已获得 IP，立即启动 mDNS
        if (wifiConfig.enableMDNS) {
            dnsManager->startMDNS(wifiConfig.customDomain);
        }

        triggerEvent(NetworkStatus::CONNECTED, statusInfo.ipAddress);
        return true;
    }

    connecting = false;
    statusInfo.status = NetworkStatus::CONNECTION_FAILED;
    snprintf(buf, sizeof(buf), "NetworkManager: WiFi connect timeout (SSID: %s)", wifiConfig.staSSID.c_str());
    LOG_WARNING(buf);
    LOGGER.debugf("NetworkManager: WiFi status code = %d", (int)WiFi.status());
    return false;
}

void NetworkManager::disconnectWiFi() {
    wifiManager->disconnectWiFi();
    connecting = false;
    statusInfo.status = NetworkStatus::DISCONNECTED;
    dnsManager->stopMDNS();
    
    LOG_INFO("NetworkManager: WiFi disconnected");
    triggerEvent(NetworkStatus::DISCONNECTED, "WiFi disconnected");
}

bool NetworkManager::configureStaticIP() {
    // 配置 IP 管理器的静态 IP 设置
    ipManager->staticIP = wifiConfig.staticIP;
    ipManager->gateway = wifiConfig.gateway;
    ipManager->subnet = wifiConfig.subnet;
    
    return wifiManager->configureStaticIP();
}

bool NetworkManager::configureDHCP() {
    return wifiManager->configureDHCP();
}

bool NetworkManager::startMDNS() {
    return dnsManager->startMDNS(wifiConfig.customDomain);
}

void NetworkManager::stopMDNS() {
    dnsManager->stopMDNS();
}

bool NetworkManager::startDNSServer() {
    return dnsManager->startDNSServer(WiFi.softAPIP());
}

void NetworkManager::stopDNSServer() {
    dnsManager->stopDNSServer();
}

// handleWiFiEvent 方法已移至 WiFiManager 类

void NetworkManager::updateStatusInfo() {
    // 调用 WiFi 管理器的更新方法
    wifiManager->updateStatusInfo();
    
    // 同步状态信息
    statusInfo = wifiManager->getStatusInfo();
}

// updateIPConflictStatus 方法已移至 IPManager 类

// IP 冲突检测和故障转移方法已移至 IPManager 类

void NetworkManager::attemptReconnect() {
    // 重连次数限制：达到最大次数后等待更长时间再重试
    if (statusInfo.reconnectAttempts >= wifiConfig.maxReconnectAttempts) {
        // 不是完全放弃，而是重置计数器，等待更长时间后再试
        LOG_WARNING("NetworkManager: Max reconnect attempts reached, will retry in 60s");
        statusInfo.reconnectAttempts = 0;
        lastReconnectAttempt = millis() + 55000;  // 等待 60 秒
        return;
    }

    lastReconnectAttempt = millis();
    statusInfo.reconnectAttempts++;

    LOG_INFO("NetworkManager: Reconnection attempt " +
             String(statusInfo.reconnectAttempts) +
             "/" + String(wifiConfig.maxReconnectAttempts));

    // 同步配置到 WiFiManager 后再尝试重连
    wifiManager->setNetworkConfig(wifiConfig);
    wifiManager->connectToWiFi();
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

bool NetworkManager::connectToNetwork(const String& ssid, const String& password) {
    if (ssid.isEmpty()) {
        LOG_WARNING("NetworkManager: SSID cannot be empty");
        return false;
    }

    wifiConfig.staSSID = ssid;
    wifiConfig.staPassword = password;

    // 同步设置 WiFiManager 的配置
    wifiManager->setNetworkConfig(wifiConfig);

    saveNetworkConfig();
    return wifiManager->connectToWiFi();
}

void NetworkManager::disconnectNetwork() {
    wifiManager->disconnectWiFi();
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
    StaticJsonDocument<2048> doc;
    
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
    
    // 检查是否需要重启网络
    if (newConfig.mode != wifiConfig.mode ||
        newConfig.apSSID != wifiConfig.apSSID ||
        newConfig.staSSID != wifiConfig.staSSID ||
        newConfig.ipConfigType != wifiConfig.ipConfigType) {
        restartRequired = true;
    }
    
    wifiConfig = newConfig;
    
    // 保存配置到存储
    bool saveSuccess = true;
    if (saveToStorage) {
        saveSuccess = saveNetworkConfig();
        if (!saveSuccess) {
            LOG_ERROR("NetworkManager: Failed to save network configuration");
            return false;
        }
        LOG_INFO("NetworkManager: Configuration saved successfully");
    }
    
    // 如果需要重启网络，异步执行（不影响保存结果）
    if (restartRequired) {
        LOG_INFO("NetworkManager: Network restart required, restarting...");
        // 延迟重启，让HTTP响应先返回
        // 使用标志位在update()中处理重启
        pendingRestart = true;
    }
    
    // 返回保存结果（不等待网络重启完成）
    return saveSuccess;
}

bool NetworkManager::updateConfigFromJSON(const String& jsonConfig) {
    StaticJsonDocument<2048> doc;
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
    StaticJsonDocument<1024> doc;
    
    doc["status"] = static_cast<uint8_t>(statusInfo.status);
    doc["ssid"] = statusInfo.ssid;
    doc["ipAddress"] = statusInfo.ipAddress;
    doc["macAddress"] = statusInfo.macAddress;
    doc["rssi"] = statusInfo.rssi;
    doc["signalStrength"] = NetworkUtils::rssiToPercentage(statusInfo.rssi);
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

String NetworkManager::getWiFiModeString() {
    WiFiMode_t mode = WiFi.getMode();
    return NetworkUtils::getWiFiModeString(mode);
}

bool NetworkManager::checkIPConflict() {
    return ipManager->checkIPConflict();
}

WiFiManager* NetworkManager::getWiFiManager() {
    return wifiManager.get();
}

IPManager* NetworkManager::getIPManager() {
    return ipManager.get();
}

DNSManager* NetworkManager::getDNSManager() {
    return dnsManager.get();
}

void NetworkManager::incrementTxCount() { statusInfo.txCount++; }
void NetworkManager::incrementRxCount() { statusInfo.rxCount++; }