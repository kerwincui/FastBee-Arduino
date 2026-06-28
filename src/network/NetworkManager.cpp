/**
 * @file NetworkManager.cpp
 * @brief 网络管理器实现
 * @author kerwincui
 * @date 2025-12-02
 */

#include "network/NetworkManager.h"
#include "systems/LoggerSystem.h"
#include "systems/RestartDiagnostics.h"
#include "systems/SystemRebooter.h"
#include "utils/NetworkUtils.h"
#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecManager.h"
#endif
#if FASTBEE_ENABLE_ETHERNET
#include "network/EthernetAdapter.h"
#endif
#if FASTBEE_ENABLE_CELLULAR
#include "network/CellularAdapter.h"
#endif
#include <WiFi.h>
#include <WiFiUdp.h>
#if FASTBEE_ENABLE_MDNS
#include <ESPmDNS.h>
#endif
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <algorithm>
#include "core/FastBeeFramework.h"  // 网络恢复 → MQTT 立即重连通知
#include "protocols/ProtocolManager.h"
#include "protocols/MQTTClient.h"

static const char* NETWORK_CONFIG_FILE = "/config/network.json";

// ESP32 NVS key 长度不能超过 15 字符（含结尾 null 共 16 字节）
// 下方 static_assert 在编译期检查所有 NVS key，防止运行时出现 KEY_TOO_LONG 错误
namespace NvsKeys {
    static_assert(sizeof("mode")-1            <= 15, "NVS key too long");
    static_assert(sizeof("device_name")-1     <= 15, "NVS key too long");
    static_assert(sizeof("ap_ssid")-1         <= 15, "NVS key too long");
    static_assert(sizeof("ap_password")-1     <= 15, "NVS key too long");
    static_assert(sizeof("ap_channel")-1      <= 15, "NVS key too long");
    static_assert(sizeof("ap_hidden")-1       <= 15, "NVS key too long");
    static_assert(sizeof("ap_max_conn")-1     <= 15, "NVS key too long");
    static_assert(sizeof("sta_ssid")-1        <= 15, "NVS key too long");
    static_assert(sizeof("sta_password")-1    <= 15, "NVS key too long");
    static_assert(sizeof("ip_config")-1       <= 15, "NVS key too long");
    static_assert(sizeof("static_ip")-1       <= 15, "NVS key too long");
    static_assert(sizeof("gateway")-1         <= 15, "NVS key too long");
    static_assert(sizeof("subnet")-1          <= 15, "NVS key too long");
    static_assert(sizeof("dns1")-1            <= 15, "NVS key too long");
    static_assert(sizeof("dns2")-1            <= 15, "NVS key too long");
    static_assert(sizeof("conf_detect")-1     <= 15, "NVS key too long");
    static_assert(sizeof("fail_strategy")-1   <= 15, "NVS key too long");
    static_assert(sizeof("auto_failover")-1   <= 15, "NVS key too long");
    static_assert(sizeof("conf_chk_intv")-1   <= 15, "NVS key too long");
    static_assert(sizeof("max_fail_try")-1    <= 15, "NVS key too long");
    static_assert(sizeof("conf_thresh")-1     <= 15, "NVS key too long");
    static_assert(sizeof("fallbk_dhcp")-1     <= 15, "NVS key too long");
    static_assert(sizeof("backup_ip_count")-1 <= 15, "NVS key too long");
    static_assert(sizeof("connect_timeout")-1 <= 15, "NVS key too long");
    static_assert(sizeof("reconn_intv")-1     <= 15, "NVS key too long");
    static_assert(sizeof("max_reconnect")-1   <= 15, "NVS key too long");
    static_assert(sizeof("custom_domain")-1   <= 15, "NVS key too long");
    static_assert(sizeof("enable_mdns")-1     <= 15, "NVS key too long");
    static_assert(sizeof("initialized")-1     <= 15, "NVS key too long");
    static_assert(sizeof("network_type")-1    <= 15, "NVS key too long");
}

FBNetworkManager::FBNetworkManager(AsyncWebServer* webServerPtr) 
    : webServer(webServerPtr),
      lastReconnectAttempt(0),
      lastStatusUpdate(0),
      lastConflictCheck(0),
      autoReconnectEnabled(true),
      isInitialized(false),
      connecting(false),
      pendingRestart(false),
      connectingStartTime(0),
      pendingRestartTime(0),
      pendingMDNSRestart(false),
      pendingMDNSRestartTime(0)
#if FASTBEE_ENABLE_ETHERNET
      , ethReconnectPending(false)
      , ethReconnectTime(0)
      , ethReconnectAttempts(0)
#endif
#if FASTBEE_ENABLE_CELLULAR
      , cellReconnectPending(false)
      , cellReconnectTime(0)
      , cellReconnectAttempts(0)
#endif
{
    
    wifiConfig = WiFiConfig();
    statusInfo = NetworkStatusInfo();
    
    // 初始化子模块
    wifiManager.reset(new WiFiManager());
    ipManager.reset(new IPManager());
    dnsManager.reset(new DNSManager());
}

FBNetworkManager::~FBNetworkManager() {
    disconnect();
    if (preferences.isKey("initialized")) {
        preferences.end();
    }
}

void FBNetworkManager::setWiFiCredentials(const String& ssid, const String& password) {
    wifiConfig.staSSID = ssid;
    wifiConfig.staPassword = password;
    wifiConfig.mode = NetworkMode::NETWORK_STA;  // 强制使用 STA 模式
    
    char buf[128];
    snprintf(buf, sizeof(buf), "NetworkManager: WiFi credentials set - SSID='%s'", ssid.c_str());
    LOG_INFO(buf);
}

bool FBNetworkManager::initialize() {
    ets_printf("[NET] FBNetworkManager::initialize() called\n");
    if (isInitialized) {
        return true;
    }

    // 初始化Preferences（独立 namespace，避免与 ConfigStorage 的 "fastbee" 冲突）
    if (!preferences.begin("net_cfg", false)) {
        LOG_ERROR("NetworkManager: Failed to initialize preferences");
        return false;
    }

    // 加载网络配置
    ets_printf("[NET] Loading network config...\n");
    if (!loadNetworkConfig()) {
        ets_printf("[NET] Using default network configuration\n");
        // 生成默认备用IP
        ipManager->generateBackupIPs();
        saveNetworkConfig();
    }
    
    // 同步冲突检测和故障转移配置到 IPManager
    syncIPManagerConfig();

    LOG_INFOF("NetworkManager: Config loaded - enableMDNS=%s, customDomain=%s", 
              wifiConfig.enableMDNS ? "true" : "false", 
              wifiConfig.customDomain.c_str());

    // 智能模式修正：首次启动或配置不完整时自动调整
    // 如果 mode 是 NETWORK_STA 但没有配置 staSSID，改为 NETWORK_AP
    // 这样可以确保首次启动时用户能通过 AP 模式配网
    if (wifiConfig.networkType == NetworkType::NET_WIFI &&
        wifiConfig.staSSID.isEmpty() && wifiConfig.mode == NetworkMode::NETWORK_STA) {
        LOG_WARNING("NetworkManager: No staSSID configured, switching from STA to AP mode");
        wifiConfig.mode = NetworkMode::NETWORK_AP;
        saveNetworkConfig();
    }

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

    // ========== 非 WiFi 联网方式初始化 ==========
    ets_printf("[NET] networkType=%d (WIFI=0, ETH=1, 4G=2)\n", (int)wifiConfig.networkType);
#if FASTBEE_ENABLE_CELLULAR
    ets_printf("[NET] CELLULAR=1, checking 4G (type==%d?=%d)\n", (int)wifiConfig.networkType, (int)NetworkType::NET_4G);
#else
    ets_printf("[NET] CELLULAR=0 (4G not compiled)\n");
#endif
#if FASTBEE_ENABLE_ETHERNET
    if (wifiConfig.networkType == NetworkType::NET_ETHERNET) {
        LOG_INFO("NetworkManager: Initializing Ethernet (W5500)...");
        ethernetAdapter.reset(new EthernetAdapter());
        bool ethOk = ethernetAdapter->begin(wifiConfig);
        if (ethOk) {
            ethOk = ethernetAdapter->waitForConnection(10000);
        }
        if (ethOk) {
            LOG_INFO("NetworkManager: Ethernet connected, IP: " + ethernetAdapter->localIP().toString());
            statusInfo.status = NetworkStatus::CONNECTED;
            statusInfo.ipAddress = ethernetAdapter->localIP().toString();
            statusInfo.lastConnectionTime = millis();
            
            // ========== 混合模式：以太网 + WiFi AP本地配置 ==========
            // 启动WiFi AP热点，用于本地Web配置访问
            LOG_INFO("NetworkManager: Starting WiFi AP for local config access...");
            bool apStarted = startAPMode();
            if (apStarted) {
                IPAddress apIP = WiFi.softAPIP();
                statusInfo.apIPAddress = apIP.toString();
                // 混合模式下网络状态应保持CONNECTED（因为以太网是主网络）
                statusInfo.status = NetworkStatus::CONNECTED;
                LOGGER.infof(">>> Config AP: %s  IP: %s <<<", 
                    wifiConfig.apSSID.c_str(), apIP.toString().c_str());
                LOG_INFO("NetworkManager: Hybrid mode active (Ethernet + WiFi AP)");
            } else {
                LOG_WARNING("NetworkManager: Failed to start WiFi AP, Ethernet only mode");
            }
            
            // 启动 mDNS（必须在所有 netif 就绪后启动，确保绑定到正确的网络接口）
            if (wifiConfig.enableMDNS) {
                dnsManager->startMDNS(wifiConfig.customDomain);
            }
            
            ethReconnectAttempts = 0;
            ethReconnectPending = false;
            isInitialized = true;
            return true;
        } else {
            LOG_WARNING("NetworkManager: Ethernet failed, falling back to AP mode for reconfiguration");
            ethernetAdapter.reset();
            // 不切换到 WiFi STA，保持 AP 模式让用户通过热点重新配置
            wifiConfig.networkType = NetworkType::NET_WIFI;
            wifiConfig.mode = NetworkMode::NETWORK_AP;
        }
    }
#endif

#if FASTBEE_ENABLE_CELLULAR
    if (wifiConfig.networkType == NetworkType::NET_4G) {
        ets_printf("[NET-4G] Starting 4G Cellular init (type=%d)...\n", (int)wifiConfig.networkType);
        cellularAdapter.reset(new CellularAdapter());
        bool cellOk = cellularAdapter->begin(wifiConfig);
        ets_printf("[NET-4G] CellularAdapter::begin() returned: %s\n", cellOk ? "true" : "false");
        if (cellOk) {
            LOG_INFO("NetworkManager: 4G connected");
            statusInfo.status = NetworkStatus::CONNECTED;
            statusInfo.lastConnectionTime = millis();
            
            // 获取4G IP地址
            if (cellularAdapter) {
                IPAddress cellIP = cellularAdapter->localIP();
                if (cellIP != IPAddress(0, 0, 0, 0)) {
                    statusInfo.ipAddress = cellIP.toString();
                    LOGGER.infof("NetworkManager: 4G IP: %s", cellIP.toString().c_str());
                }
            }
            
            // ========== 混合模式：4G上网 + WiFi AP本地配置 ==========
            // 启动WiFi AP热点，用于本地Web配置访问
            LOG_INFO("NetworkManager: Starting WiFi AP for local config access...");
            bool apStarted = startAPMode();
            if (apStarted) {
                IPAddress apIP = WiFi.softAPIP();
                statusInfo.apIPAddress = apIP.toString();
                // 混合模式下网络状态应保持CONNECTED（因为4G是主网络）
                statusInfo.status = NetworkStatus::CONNECTED;
                LOGGER.infof(">>> Config AP: %s  IP: %s <<<", 
                    wifiConfig.apSSID.c_str(), apIP.toString().c_str());
                LOG_INFO("NetworkManager: Hybrid mode active (4G + WiFi AP)");
            } else {
                LOG_WARNING("NetworkManager: Failed to start WiFi AP, 4G only mode");
            }
            
            // 启动 mDNS（需在 AP 启动后，使 mDNS 绑定到 AP 接口）
            if (wifiConfig.enableMDNS) {
                dnsManager->startMDNS(wifiConfig.customDomain);
            }
            
            isInitialized = true;
            return true;
        } else {
            ets_printf("[NET-4G] 4G failed, falling back to AP mode\n");
            LOG_WARNING("NetworkManager: 4G failed, falling back to AP mode for reconfiguration");
            cellularAdapter.reset();
            // 不切换到 WiFi STA，保持 AP 模式让用户通过热点重新配置
            wifiConfig.networkType = NetworkType::NET_WIFI;
            wifiConfig.mode = NetworkMode::NETWORK_AP;
        }
    }
#endif

    // ========== WiFi 联网方式（默认 / 回退） ==========
    // 网络启动策略（仅支持 STA 和 AP 两种模式）：
    // 1. NETWORK_STA (0): 尝试连接WiFi，失败则自动切换到AP模式
    // 2. NETWORK_AP (1): 直接启动热点
    bool success = false;
    
    // 调试：打印实际加载的WiFi配置
    Serial.printf("[NET] Config: mode=%d staSSID=[%s] networkType=%d\n",
        (int)wifiConfig.mode, wifiConfig.staSSID.c_str(), (int)wifiConfig.networkType);
    
    switch (wifiConfig.mode) {
        case NetworkMode::NETWORK_STA:
            LOG_INFO("NetworkManager: Starting in STA mode");
            if (!wifiConfig.staSSID.isEmpty()) {
                success = connectToWiFiBlocking();
                if (success) {
                    LOG_INFO("NetworkManager: STA connected successfully");
                    LOGGER.infof(">>> STA IP: %s <<<", WiFi.localIP().toString().c_str());
                } else {
                    LOG_WARNING("NetworkManager: STA connection failed, falling back to AP mode");
                    Serial.println("[NET] STA failed, switching to AP mode...");
                    ets_printf("[NET] STA failed, switching to AP mode...\n");
                    wifiManager->setModeTransitioning(false);
                    // STA 失败后回退到纯 AP 模式，供用户通过 Web 重新配置
                    // 不使用 AP+STA 双模式，避免频繁模式切换导致 arduino_events 栈溢出
                    success = startAPMode();
                    if (success) {
                        Serial.printf("[NET] AP started: %s  IP: %s\n",
                            wifiConfig.apSSID.c_str(),
                            WiFi.softAPIP().toString().c_str());
                        ets_printf("[NET] AP started: %s  IP: %s\n",
                            wifiConfig.apSSID.c_str(),
                            WiFi.softAPIP().toString().c_str());
                        LOGGER.infof(">>> AP fallback: AP IP=%s  SSID=[%s] pwd=[%s] <<<",
                            WiFi.softAPIP().toString().c_str(),
                            wifiConfig.apSSID.c_str(),
                            wifiConfig.apPassword.c_str());
                        // 回退到 AP 模式后更新配置模式
                        wifiConfig.mode = NetworkMode::NETWORK_AP;
                    } else {
                        Serial.println("[NET] AP start FAILED!");
                    }
                }
            } else {
                LOG_WARNING("NetworkManager: No staSSID configured, starting AP mode");
                wifiManager->setModeTransitioning(false);
                wifiConfig.mode = NetworkMode::NETWORK_AP;
                success = startAPMode();
            }
            break;
            
        case NetworkMode::NETWORK_AP:
            LOG_INFO("NetworkManager: Starting in AP mode");
            success = startAPMode();
            if (success) {
                LOGGER.infof(">>> AP IP: %s  Connect to [%s] pwd:[%s] <<<",
                    WiFi.softAPIP().toString().c_str(),
                    wifiConfig.apSSID.c_str(),
                    wifiConfig.apPassword.c_str());
                
                // AP 模式下仅启动纯 AP 热点，不使用 AP+STA 双模式
                // 避免模式切换导致 arduino_events 任务栈溢出崩溃
            }
            break;
            
        default:
            LOG_ERROR("NetworkManager: Unknown network mode, defaulting to AP");
            wifiConfig.mode = NetworkMode::NETWORK_AP;
            success = startAPMode();
            break;
    }

    isInitialized = success;
    if (success) {
        LOG_INFO("NetworkManager: Initialized successfully");
        // 初始化成功，清除连续失败计数器
        preferences.putInt("init_fail_cnt", 0);
    } else {
        LOG_ERROR("NetworkManager: All network modes failed (check AP config)");
        // === 最后保障：强制以出厂默认配置启动 AP ===
        ensureLastResortAP();
    }

    return isInitialized;
}

void FBNetworkManager::disconnect() {
    LOG_INFO("NetworkManager: Disconnecting all network connections...");
    
    // 停止mDNS
    dnsManager->stopMDNS();
    delay(200);  // 等待 mDNS 完全停止

#if FASTBEE_ENABLE_ETHERNET
    if (ethernetAdapter) {
        ethernetAdapter->disconnect();
        ethernetAdapter.reset();
    }
#endif
#if FASTBEE_ENABLE_CELLULAR
    if (cellularAdapter) {
        cellularAdapter->disconnect();
        cellularAdapter.reset();
    }
#endif
    
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

void FBNetworkManager::update() {
    unsigned long currentTime = millis();
    static bool wasConnected = false;
    static unsigned long lastMdnsUpdate = 0;

    // 处理延迟重启（配置保存后延迟1500ms执行，确保HTTP响应已返回）
    if (pendingRestart) {
        if (pendingRestartTime == 0) {
            pendingRestartTime = currentTime;
        } else if (currentTime - pendingRestartTime >= 1500) {
            LOG_INFO("NetworkManager: Executing delayed network restart...");
            pendingRestart = false;
            pendingRestartTime = 0;
            restartNetwork();
            return; // 重启后返回，下次循环再更新状态
        }
    }

    // 处理延迟 mDNS 重启（域名配置变更后延迟1500ms执行，确保HTTP响应已返回）
    if (pendingMDNSRestart) {
        if (pendingMDNSRestartTime == 0) {
            pendingMDNSRestartTime = currentTime;
        } else if (currentTime - pendingMDNSRestartTime >= 1500) {
            LOG_INFOF("NetworkManager: Executing delayed mDNS restart with hostname '%s'",
                      wifiConfig.customDomain.c_str());
            pendingMDNSRestart = false;
            pendingMDNSRestartTime = 0;
            if (dnsManager) {
                dnsManager->setCustomDomain(wifiConfig.customDomain);
                dnsManager->restartMDNS(wifiConfig.customDomain);
            }
        }
    }

    // 检测 WiFi 连接状态变化
    if (wifiConfig.networkType != NetworkType::NET_WIFI) {
#if FASTBEE_ENABLE_ETHERNET
        if (wifiConfig.networkType == NetworkType::NET_ETHERNET && ethernetAdapter) {
            ethernetAdapter->update();
        }
#endif
#if FASTBEE_ENABLE_CELLULAR
        if (wifiConfig.networkType == NetworkType::NET_4G && cellularAdapter) {
            cellularAdapter->update();
        }
#endif

        bool activeConnected = isNetworkConnected();
        if (activeConnected && !wasConnected) {
            LOG_INFO("NetworkManager: active network connected");
            statusInfo.reconnectAttempts = 0;
            statusInfo.status = NetworkStatus::CONNECTED;
            statusInfo.lastConnectionTime = millis();
            triggerEvent(NetworkStatus::CONNECTED, statusInfo.ipAddress);
#if FASTBEE_ENABLE_ETHERNET
            // 以太网重连成功后重置计数器
            if (wifiConfig.networkType == NetworkType::NET_ETHERNET) {
                ethReconnectAttempts = 0;
                ethReconnectPending = false;
                LOGGER.infof("NetworkManager: Ethernet reconnected, IP: %s",
                             ethernetAdapter ? ethernetAdapter->localIP().toString().c_str() : "?");
                // 重连成功后重启mDNS确保绑定正确接口
                if (wifiConfig.enableMDNS) {
                    dnsManager->stopMDNS();
                    dnsManager->startMDNS(wifiConfig.customDomain);
                }
                // 通知 MQTT 客户端网络已恢复，立即尝试重连
#if FASTBEE_ENABLE_MQTT
                {
                    auto* fw = FastBeeFramework::getInstance();
                    auto* pm = fw ? fw->getProtocolManager() : nullptr;
                    MQTTClient* mqtt = pm ? pm->getMQTTClient() : nullptr;
                    if (mqtt) {
                        mqtt->resetErrorCounters();
                        LOG_INFO("[NET] MQTT reconnect triggered after Ethernet reconnection");
                    }
                }
#endif
            }
#endif
#if FASTBEE_ENABLE_CELLULAR
            if (wifiConfig.networkType == NetworkType::NET_4G) {
                cellReconnectAttempts = 0;
                cellReconnectPending = false;
                LOGGER.infof("NetworkManager: 4G reconnected, IP: %s",
                             cellularAdapter ? cellularAdapter->localIP().toString().c_str() : "?");
                if (wifiConfig.enableMDNS) {
                    dnsManager->stopMDNS();
                    dnsManager->startMDNS(wifiConfig.customDomain);
                }
                // 通知 MQTT 客户端网络已恢复，立即尝试重连
#if FASTBEE_ENABLE_MQTT
                {
                    auto* fw = FastBeeFramework::getInstance();
                    auto* pm = fw ? fw->getProtocolManager() : nullptr;
                    MQTTClient* mqtt = pm ? pm->getMQTTClient() : nullptr;
                    if (mqtt) {
                        mqtt->resetErrorCounters();
                        LOG_INFO("[NET] MQTT reconnect triggered after 4G reconnection");
                    }
                }
#endif
            }
#endif
        } else if (!activeConnected && wasConnected) {
            LOG_WARNING("NetworkManager: active network disconnected");
            statusInfo.status = NetworkStatus::DISCONNECTED;
            triggerEvent(NetworkStatus::DISCONNECTED, "Network disconnected");
#if FASTBEE_ENABLE_ETHERNET
            // 以太网断连后调度自动重连
            if (wifiConfig.networkType == NetworkType::NET_ETHERNET && ethernetAdapter) {
                if (ethReconnectAttempts < ETH_MAX_RECONNECT_ATTEMPTS) {
                    ethReconnectPending = true;
                    ethReconnectTime = currentTime + ETH_RECONNECT_INTERVAL_MS;
                    LOGGER.infof("NetworkManager: Ethernet reconnect scheduled in %lus (attempt %d/%d)",
                                 (unsigned long)(ETH_RECONNECT_INTERVAL_MS / 1000),
                                 ethReconnectAttempts + 1, ETH_MAX_RECONNECT_ATTEMPTS);
                } else {
                    LOG_WARNING("NetworkManager: Ethernet max reconnect attempts reached, giving up auto-reconnect");
                }
            }
#endif
#if FASTBEE_ENABLE_CELLULAR
            if (wifiConfig.networkType == NetworkType::NET_4G && cellularAdapter) {
                if (cellReconnectAttempts < CELL_MAX_RECONNECT_ATTEMPTS) {
                    cellReconnectPending = true;
                    cellReconnectTime = currentTime + CELL_RECONNECT_INTERVAL_MS;
                    LOGGER.infof("NetworkManager: 4G reconnect scheduled in %lus (attempt %d/%d)",
                                 (unsigned long)(CELL_RECONNECT_INTERVAL_MS / 1000),
                                 cellReconnectAttempts + 1, CELL_MAX_RECONNECT_ATTEMPTS);
                } else {
                    LOG_WARNING("NetworkManager: 4G max reconnect attempts reached, keeping AP fallback only");
                    ensureLastResortAP();
                }
            }
#endif
        }
        wasConnected = activeConnected;

#if FASTBEE_ENABLE_ETHERNET
        // 处理以太网初始断连状态（启动时连接过但第一次update前就已断开）
        // 此时 wasConnected=false, activeConnected=false，状态转换检测无法触发
        if (!activeConnected && !ethReconnectPending &&
            wifiConfig.networkType == NetworkType::NET_ETHERNET && ethernetAdapter &&
            !ethernetAdapter->isConnected() &&
            ethReconnectAttempts < ETH_MAX_RECONNECT_ATTEMPTS) {
            // 首次检测到断连或重连后仍断连，调度重连
            if (ethReconnectAttempts == 0 || currentTime - lastStatusUpdate >= ETH_RECONNECT_INTERVAL_MS) {
                ethReconnectPending = true;
                ethReconnectTime = currentTime + ETH_RECONNECT_INTERVAL_MS;
                LOGGER.infof("NetworkManager: Ethernet disconnected, reconnect scheduled in %lus (attempt %d/%d)",
                             (unsigned long)(ETH_RECONNECT_INTERVAL_MS / 1000),
                             ethReconnectAttempts + 1, ETH_MAX_RECONNECT_ATTEMPTS);
            }
        }
#endif

#if FASTBEE_ENABLE_CELLULAR
        if (!activeConnected && !cellReconnectPending &&
            wifiConfig.networkType == NetworkType::NET_4G && cellularAdapter &&
            !cellularAdapter->isConnected() &&
            cellReconnectAttempts < CELL_MAX_RECONNECT_ATTEMPTS) {
            if (cellReconnectAttempts == 0 || currentTime - lastStatusUpdate >= CELL_RECONNECT_INTERVAL_MS) {
                cellReconnectPending = true;
                cellReconnectTime = currentTime + CELL_RECONNECT_INTERVAL_MS;
                LOGGER.infof("NetworkManager: 4G disconnected, reconnect scheduled in %lus (attempt %d/%d)",
                             (unsigned long)(CELL_RECONNECT_INTERVAL_MS / 1000),
                             cellReconnectAttempts + 1, CELL_MAX_RECONNECT_ATTEMPTS);
            }
        }
#endif

#if FASTBEE_ENABLE_ETHERNET
        // 以太网自动重连执行
        if (ethReconnectPending && wifiConfig.networkType == NetworkType::NET_ETHERNET &&
            currentTime >= ethReconnectTime) {
            ethReconnectPending = false;
            ethReconnectAttempts++;
            LOG_INFO("NetworkManager: Attempting Ethernet auto-reconnect...");
            // 使用 restartNetwork 重新初始化以太网适配器
            restartNetwork();
            // restartNetwork 是同步的，检查结果
            if (ethernetAdapter && ethernetAdapter->isConnected()) {
                LOG_INFO("NetworkManager: Ethernet auto-reconnect succeeded");
            } else {
                // 重连失败，调度下一次重试
                if (ethReconnectAttempts < ETH_MAX_RECONNECT_ATTEMPTS) {
                    ethReconnectPending = true;
                    ethReconnectTime = currentTime + ETH_RECONNECT_INTERVAL_MS;
                    LOGGER.infof("NetworkManager: Ethernet reconnect failed, next attempt in %lus",
                                 (unsigned long)(ETH_RECONNECT_INTERVAL_MS / 1000));
                } else {
                    LOG_WARNING("NetworkManager: Ethernet auto-reconnect exhausted, will not retry");
                    // 确保 AP 热点始终可用，作为用户回退配置的入口
                    ensureLastResortAP();
                    if (wifiConfig.enableMDNS) {
                        dnsManager->startMDNS(wifiConfig.customDomain);
                    }
                }
            }
        }
#endif

#if FASTBEE_ENABLE_CELLULAR
        if (cellReconnectPending && wifiConfig.networkType == NetworkType::NET_4G &&
            currentTime >= cellReconnectTime) {
            cellReconnectPending = false;
            cellReconnectAttempts++;
            LOG_INFO("NetworkManager: Attempting 4G auto-reconnect...");

            bool cellOk = false;
            if (cellularAdapter && (cellReconnectAttempts % CELL_FULL_RESTART_EVERY) != 0) {
                cellOk = cellularAdapter->reconnect();
            } else {
                LOG_INFO("NetworkManager: Recreating 4G adapter for full reconnect");
                restartNetwork();
                cellOk = (wifiConfig.networkType == NetworkType::NET_4G &&
                          cellularAdapter && cellularAdapter->isConnected());
            }

            if (cellOk) {
                LOG_INFO("NetworkManager: 4G auto-reconnect succeeded");
                cellReconnectAttempts = 0;
                cellReconnectPending = false;
                statusInfo.status = NetworkStatus::CONNECTED;
                statusInfo.lastConnectionTime = millis();
                if (cellularAdapter) {
                    IPAddress cellIP = cellularAdapter->localIP();
                    if (cellIP != IPAddress(0, 0, 0, 0)) {
                        statusInfo.ipAddress = cellIP.toString();
                    }
                }
                if (wifiConfig.enableMDNS) {
                    dnsManager->startMDNS(wifiConfig.customDomain);
                }
            } else if (wifiConfig.networkType == NetworkType::NET_4G) {
                if (cellReconnectAttempts < CELL_MAX_RECONNECT_ATTEMPTS) {
                    cellReconnectPending = true;
                    cellReconnectTime = currentTime + CELL_RECONNECT_INTERVAL_MS;
                    LOGGER.infof("NetworkManager: 4G reconnect failed, next attempt in %lus",
                                 (unsigned long)(CELL_RECONNECT_INTERVAL_MS / 1000));
                } else {
                    LOG_WARNING("NetworkManager: 4G auto-reconnect exhausted, keeping AP fallback available");
                    ensureLastResortAP();
                    if (wifiConfig.enableMDNS) {
                        dnsManager->startMDNS(wifiConfig.customDomain);
                    }
                }
            }
        }
#endif

        if (currentTime - lastStatusUpdate >= 1000) {
            updateStatusInfo();
            lastStatusUpdate = currentTime;
        }

        if (currentTime - lastMdnsUpdate >= 30000) {
            if (wifiConfig.enableMDNS) {
                dnsManager->checkMDNSHealth();
            }
            lastMdnsUpdate = currentTime;
        }
        return;
    }

    bool isConnected = (WiFi.status() == WL_CONNECTED);
    
    // WiFi 连接成功（从断开变为连接）
    if (isConnected && !wasConnected) {
        LOG_INFO("NetworkManager: WiFi connected, IP: " + WiFi.localIP().toString());
        statusInfo.reconnectAttempts = 0;  // 重置重连计数器
        statusInfo.status = NetworkStatus::CONNECTED;
        statusInfo.lastConnectionTime = millis();
        
        // 重新启动mDNS以更新IP绑定
        if (wifiConfig.enableMDNS) {
            LOG_INFOF("NetworkManager: WiFi connected, restarting mDNS for STA IP");
            // 使用dnsManager重启mDNS，让它检测并使用正确的IP
            dnsManager->restartMDNS(wifiConfig.customDomain);
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

    // 处理DNS请求（DNS服务器已移除）
    // dnsManager->processDNSRequests();  // removed

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
        Serial.printf("[WiFi] Connection timeout after %lums\n",
                      (unsigned long)wifiConfig.connectTimeout);
        LOG_WARNING("NetworkManager: Connection timeout");
        connecting = false;
        
        // 连接超时处理（纯 AP 模式下不会有 AP+STA 分支）
        LOG_WARNING("NetworkManager: Connection timeout");
        if (wifiConfig.autoFailover) {
            LOG_INFO("NetworkManager: Attempting failover...");
            ipManager->performFailover();
        } else {
            statusInfo.status = NetworkStatus::CONNECTION_FAILED;
            triggerEvent(NetworkStatus::CONNECTION_FAILED, "Connection timeout");
        }
    }

    // 自动重连逻辑（仅 STA 模式）
    if (autoReconnectEnabled && 
        !connecting &&
        !isConnected &&
        wifiConfig.mode == NetworkMode::NETWORK_STA &&
        currentTime - lastReconnectAttempt >= wifiConfig.reconnectInterval) {
        attemptReconnect();
    }
    
    // mDNS 健康检查（每 30 秒）
    if (currentTime - lastMdnsUpdate >= 30000) {
        if (wifiConfig.enableMDNS) {
            dnsManager->checkMDNSHealth();
        }
        lastMdnsUpdate = currentTime;
    }

    // WiFi 僵尸连接检测（每 60 秒）
    // 当 WiFi.status() 报告 WL_CONNECTED 但 lwIP 网络栈实际已中断时
    // （如 ARP 不可达、UDP 发送失败），强制重连 WiFi
    static unsigned long lastZombieCheck = 0;
    static uint8_t zombieFailCount = 0;

    if (isConnected && currentTime - lastZombieCheck >= 60000) {
        lastZombieCheck = currentTime;

        IPAddress gw = WiFi.gatewayIP();
        if (gw != IPAddress(0, 0, 0, 0)) {
            // 向网关发送一个 UDP 包测试网络栈是否正常
            // endPacket() 内部调用 lwip_sendto()，若网络栈异常会立即返回 0
            WiFiUDP testUdp;
            bool sendOk = false;
            if (testUdp.beginPacket(gw, 53) == 1) {
                testUdp.write((uint8_t)0);
                sendOk = (testUdp.endPacket() == 1);
            }
            testUdp.stop();

            if (sendOk) {
                if (zombieFailCount > 0) {
                    LOG_INFO("NetworkManager: Gateway reachable, zombie check cleared");
                }
                zombieFailCount = 0;
            } else {
                zombieFailCount++;
                char buf[80];
                snprintf(buf, sizeof(buf),
                         "NetworkManager: UDP to gateway failed (%d/3)", zombieFailCount);
                LOG_WARNING(buf);
            }
        } else {
            zombieFailCount++;
        }

        if (zombieFailCount >= 3) {
            LOG_WARNING("NetworkManager: WiFi zombie detected! Forcing reconnect...");
            zombieFailCount = 0;
            wasConnected = false;          // 让下次 update() 走"重新连接"分支
            WiFi.disconnect(false);        // 断开 STA
            delay(200);
            wifiManager->connectToWiFi();  // 重新连接 WiFi
        }
    }
    
    // 清理冲突缓存（每小时一次）
    static unsigned long lastCacheClean = 0;
    if (currentTime - lastCacheClean >= 3600000) {
        ipManager->cleanupConflictCache();
        lastCacheClean = currentTime;
    }
}

bool FBNetworkManager::loadNetworkConfig() {
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
        // deviceName 已移至设备配置(device.json)，不再从 network.json 加载
        if (doc.containsKey("apSSID"))               wifiConfig.apSSID = doc["apSSID"].as<String>();
        if (doc.containsKey("apPassword"))           wifiConfig.apPassword = doc["apPassword"].as<String>();
        if (doc.containsKey("apIP"))                 wifiConfig.apIP = doc["apIP"].as<String>();
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
        if (doc.containsKey("conflictDetection"))    wifiConfig.conflictDetection = static_cast<IPConflictMode>(doc["conflictDetection"].as<uint8_t>());
        if (doc.containsKey("failoverStrategy"))     wifiConfig.failoverStrategy = static_cast<IPFailoverStrategy>(doc["failoverStrategy"].as<uint8_t>());
        if (doc.containsKey("autoFailover"))         wifiConfig.autoFailover = doc["autoFailover"].as<bool>();
        if (doc.containsKey("conflictCheckInterval")) wifiConfig.conflictCheckInterval = doc["conflictCheckInterval"].as<uint16_t>();
        if (doc.containsKey("maxFailoverAttempts"))  wifiConfig.maxFailoverAttempts = doc["maxFailoverAttempts"].as<uint8_t>();
        if (doc.containsKey("conflictThreshold"))    wifiConfig.conflictThreshold = doc["conflictThreshold"].as<uint8_t>();
        if (doc.containsKey("fallbackToDHCP"))       wifiConfig.fallbackToDHCP = doc["fallbackToDHCP"].as<bool>();

        // 联网方式
        if (doc.containsKey("networkType")) {
            uint8_t rawNetworkType = doc["networkType"].as<uint8_t>();
            wifiConfig.networkType = rawNetworkType <= static_cast<uint8_t>(NetworkType::NET_4G)
                ? static_cast<NetworkType>(rawNetworkType)
                : NetworkType::NET_WIFI;
        }

        // 以太网配置
        if (doc.containsKey("ethernet") && doc["ethernet"].is<JsonObject>()) {
            JsonObject eth = doc["ethernet"];
            if (eth.containsKey("spiMosi")) wifiConfig.ethernet.spiMosi = eth["spiMosi"].as<int8_t>();
            if (eth.containsKey("spiMiso")) wifiConfig.ethernet.spiMiso = eth["spiMiso"].as<int8_t>();
            if (eth.containsKey("spiSck"))  wifiConfig.ethernet.spiSck  = eth["spiSck"].as<int8_t>();
            if (eth.containsKey("csPin"))   wifiConfig.ethernet.csPin   = eth["csPin"].as<int8_t>();
            if (eth.containsKey("rstPin"))  wifiConfig.ethernet.rstPin  = eth["rstPin"].as<int8_t>();
            if (eth.containsKey("intPin"))  wifiConfig.ethernet.intPin  = eth["intPin"].as<int8_t>();
        }

        // 4G 蜂窝配置
        if (doc.containsKey("cellular") && doc["cellular"].is<JsonObject>()) {
            JsonObject cell = doc["cellular"];
            if (cell.containsKey("txPin"))    wifiConfig.cellular.txPin    = cell["txPin"].as<int8_t>();
            if (cell.containsKey("rxPin"))    wifiConfig.cellular.rxPin    = cell["rxPin"].as<int8_t>();
            if (cell.containsKey("pwrPin"))   wifiConfig.cellular.pwrPin   = cell["pwrPin"].as<int8_t>();
            if (cell.containsKey("baudRate")) wifiConfig.cellular.baudRate = cell["baudRate"].as<uint32_t>();
            if (cell.containsKey("apn"))      wifiConfig.cellular.apn      = cell["apn"].as<String>();
        }

        // 多 SSID 列表解析（向下兼容：无 networks 字段时使用 staSSID/staPassword）
        if (doc.containsKey("networks") && doc["networks"].is<JsonArray>()) {
            wifiConfig.networks.clear();
            JsonArray arr = doc["networks"].as<JsonArray>();
            uint8_t count = 0;
            for (JsonObject netObj : arr) {
                if (count >= 3) break;  // 最多 3 个
                WiFiNetwork net;
                net.ssid = netObj["ssid"] | "";
                net.password = netObj["password"] | "";
                net.priority = netObj["priority"] | count;
                if (!net.ssid.isEmpty()) {
                    wifiConfig.networks.push_back(net);
                    count++;
                }
            }
            // 按 priority 升序排序
            std::sort(wifiConfig.networks.begin(), wifiConfig.networks.end(),
                [](const WiFiNetwork& a, const WiFiNetwork& b) { return a.priority < b.priority; });
            LOG_INFO("NetworkManager: Loaded " + String(wifiConfig.networks.size()) + " WiFi networks");
        }
        LOGGER.infof("NetworkManager: Config loaded from %s", path);
        ets_printf("[NET] Config loaded: networkType=%d mode=%d staSSID=[%s]\n",
            (int)wifiConfig.networkType, (int)wifiConfig.mode, wifiConfig.staSSID.c_str());
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

    // 基本配置
    wifiConfig.mode = static_cast<NetworkMode>(preferences.getUInt("mode", 
        static_cast<uint8_t>(NetworkMode::NETWORK_STA)));
    // deviceName 已移至设备配置(device.json)，不再从 NVS 加载

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
            preferences.getUInt("conf_detect", 
            static_cast<uint8_t>(IPConflictMode::ARP)));
        wifiConfig.failoverStrategy = static_cast<IPFailoverStrategy>(
            preferences.getUInt("fail_strategy", 
            static_cast<uint8_t>(IPFailoverStrategy::SMART)));
        wifiConfig.autoFailover = preferences.getBool("auto_failover", true);
        wifiConfig.conflictCheckInterval = preferences.getUShort(
            "conf_chk_intv", 30000);
        wifiConfig.maxFailoverAttempts = preferences.getUChar(
            "max_fail_try", 3);
        wifiConfig.conflictThreshold = preferences.getUChar(
            "conf_thresh", 2);
        wifiConfig.fallbackToDHCP = preferences.getBool("fallbk_dhcp", true);

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
        wifiConfig.reconnectInterval = preferences.getULong("reconn_intv", 5000);
        wifiConfig.maxReconnectAttempts = preferences.getUChar("max_reconnect", 5);

        // 域名配置
        wifiConfig.customDomain = preferences.getString("custom_domain", NetConst::DEFAULT_MDNS_HOSTNAME);
        wifiConfig.enableMDNS = preferences.getBool("enable_mdns", true);

        LOG_INFO("NetworkManager: Configuration loaded successfully");
    return true;
}

bool FBNetworkManager::saveNetworkConfig() {
    // 构建 JSON内容共用
    JsonDocument doc;
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
        preferences.end();

    LOG_INFO("NetworkManager: Configuration saved successfully");
    return true;
}

bool FBNetworkManager::startAPMode() {
    if (!wifiManager->startAPMode()) {
        return false;
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

void FBNetworkManager::stopAPMode() {
    if (WiFi.getMode() & WIFI_AP) {
        WiFi.softAPdisconnect(true);
        LOG_INFO("NetworkManager: AP mode stopped");
    }
}

bool FBNetworkManager::ensureLastResortAP() {
    // 如果 AP 已经在运行，直接返回成功
    if (WiFi.getMode() & WIFI_AP) {
        IPAddress apIP = WiFi.softAPIP();
        if (apIP != IPAddress(0, 0, 0, 0)) {
            return true;
        }
    }

    LOG_WARNING("NetworkManager: All network paths failed. Starting last-resort AP...");
    WiFi.mode(WIFI_AP);
    delay(100);
    // 生成唯一 SSID，避免多台设备同名热点在 WiFi 列表中只显示一个
    String lastResortSSID = "fastbee-" + String(wifiManager->getChipID().substring(0, 6));
    if (WiFi.softAP(lastResortSSID.c_str(), "12345678", 6, 0, 4)) {
        IPAddress apIP(192, 168, 4, 1);
        WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
        statusInfo.apIPAddress = apIP.toString();
        isInitialized = true;
        LOGGER.infof(">>> LAST-RESORT AP: %s  IP: %s <<<", lastResortSSID.c_str(), apIP.toString().c_str());
        LOG_INFO("NetworkManager: Last-resort AP started - connect to '" + lastResortSSID + "' (pwd: 12345678)");
        preferences.putInt("init_fail_cnt", 0);
        return true;
    }

    // AP 也失败，记录失败次数并重启重试
    LOG_ERROR("NetworkManager: CRITICAL - Last-resort AP failed, scheduling restart...");
    int failCount = preferences.getInt("init_fail_cnt", 0) + 1;
    preferences.putInt("init_fail_cnt", failCount);
    if (failCount >= 3) {
        LOG_ERROR("NetworkManager: 3 consecutive init failures, clearing network config to factory defaults...");
        preferences.clear();
        preferences.putInt("init_fail_cnt", 0);
    }
    ets_printf("[NET] CRITICAL: All network modes failed. Restarting in 5s...\n");
    RestartDiagnostics::savePreRestartState(
        RestartReason::AP_FALLBACK,
        "All network modes failed after 3 attempts");
    delay(5000);
    ESP.restart();
    return false;  // 永远不会执行到
}

bool FBNetworkManager::connectToWiFi() {
    if (wifiConfig.staSSID.isEmpty()) {
        LOG_INFO("NetworkManager: No STA SSID configured");
        return false;
    }

    // 重置故障转移状态
    wifiManager->setNetworkConfig(wifiConfig);
    WiFi.disconnect(false);
    delay(150);

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

bool FBNetworkManager::connectToWiFiBlocking() {
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

    // 设置 WiFi 模式为 STA
    WiFiMode_t currentMode = WiFi.getMode();
    if (!(currentMode & WIFI_STA)) {
        WiFi.mode(WIFI_MODE_STA);
        delay(500);  // AP→STA 模式切换后等待 WiFi 子系统重新就绪
    }

    // 发起连接（最多重试 2 次，应对首次扫描超时）
    const int MAX_WIFI_RETRIES = 2;
    bool wifiConnected = false;
    char buf[80];

    for (int attempt = 1; attempt <= MAX_WIFI_RETRIES; attempt++) {
        WiFi.disconnect(false);
        delay(200);
        WiFi.begin(wifiConfig.staSSID.c_str(), wifiConfig.staPassword.c_str());
        statusInfo.status = NetworkStatus::CONNECTING;

        snprintf(buf, sizeof(buf), "[NET] Connecting to WiFi [%s] (attempt %d/%d)...",
                 wifiConfig.staSSID.c_str(), attempt, MAX_WIFI_RETRIES);
        Serial.println(buf);
        ets_printf("%s\n", buf);
        LOG_INFO(buf);
        // 调试：打印实际使用的连接凭据
        if (attempt == 1) {
            LOGGER.debugf("NetworkManager: staSSID=[%s] len=%d", wifiConfig.staSSID.c_str(), wifiConfig.staSSID.length());
            LOGGER.debugf("NetworkManager: staPassword=[%s] len=%d", wifiConfig.staPassword.c_str(), wifiConfig.staPassword.length());
        }

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
            wifiConnected = true;
            break;
        }

        // 重试前短暂等待
        if (attempt < MAX_WIFI_RETRIES) {
            Serial.printf("[NET] WiFi attempt %d failed, retrying...\n", attempt);
            WiFi.disconnect(false);
            delay(1000);
        }
    }

    if (wifiConnected) {
        connecting = false;
        statusInfo.status = NetworkStatus::CONNECTED;
        statusInfo.ipAddress = WiFi.localIP().toString();
        statusInfo.ssid = WiFi.SSID();
        statusInfo.lastConnectionTime = millis();

        snprintf(buf, sizeof(buf), "[NET] WiFi connected! IP: %s", statusInfo.ipAddress.c_str());
        Serial.println(buf);
        ets_printf("%s\n", buf);
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
    snprintf(buf, sizeof(buf), "[NET] WiFi connect TIMEOUT (SSID: %s)", wifiConfig.staSSID.c_str());
    Serial.println(buf);
    ets_printf("%s\n", buf);
    LOG_WARNING(buf);
    LOGGER.debugf("NetworkManager: WiFi status code = %d", (int)WiFi.status());
    WiFi.disconnect(false);
    delay(100);
    return false;
}

void FBNetworkManager::disconnectWiFi() {
    wifiManager->disconnectWiFi();
    connecting = false;
    
    dnsManager->stopMDNS();
    statusInfo.status = NetworkStatus::DISCONNECTED;
    
    LOG_INFO("NetworkManager: WiFi disconnected");
    triggerEvent(NetworkStatus::DISCONNECTED, "WiFi disconnected");
}

bool FBNetworkManager::configureStaticIP() {
    // 配置 IP 管理器的静态 IP 设置
    ipManager->staticIP = wifiConfig.staticIP;
    ipManager->gateway = wifiConfig.gateway;
    ipManager->subnet = wifiConfig.subnet;
    
    // 同步冲突检测和故障转移配置
    syncIPManagerConfig();

    return wifiManager->configureStaticIP();
}

void FBNetworkManager::syncIPManagerConfig() {
    ipManager->autoFailover = wifiConfig.autoFailover;
    ipManager->conflictCheckInterval = wifiConfig.conflictCheckInterval;
    ipManager->maxFailoverAttempts = wifiConfig.maxFailoverAttempts;
    ipManager->conflictThreshold = wifiConfig.conflictThreshold;
    ipManager->fallbackToDHCP = wifiConfig.fallbackToDHCP;
}

bool FBNetworkManager::configureDHCP() {
    return wifiManager->configureDHCP();
}

bool FBNetworkManager::startMDNS() {
    return dnsManager->startMDNS(wifiConfig.customDomain);
}

void FBNetworkManager::stopMDNS() {
    dnsManager->stopMDNS();
}



// handleWiFiEvent 方法已移至 WiFiManager 类

void FBNetworkManager::updateStatusInfo() {
    // 更新 WiFi 底层状态（AP 客户端数、STA 信号等）
    wifiManager->updateStatusInfo();

    // 对于非 WiFi 联网方式，主状态由对应适配器决定，
    // 仅同步 AP 相关信息（客户端数、AP IP），不覆盖主连接状态
    if (wifiConfig.networkType != NetworkType::NET_WIFI) {
        // 保留 WiFiManager 更新到的 AP 字段
        NetworkStatusInfo wifiStatus = wifiManager->getStatusInfo();
        statusInfo.apClientCount = wifiStatus.apClientCount;
        statusInfo.apIPAddress = wifiStatus.apIPAddress;

#if FASTBEE_ENABLE_ETHERNET
        if (wifiConfig.networkType == NetworkType::NET_ETHERNET && ethernetAdapter) {
            bool connected = ethernetAdapter->isConnected();
            statusInfo.status = connected ? NetworkStatus::CONNECTED : NetworkStatus::DISCONNECTED;
            statusInfo.ipAddress = connected ? ethernetAdapter->localIP().toString() : "";
            statusInfo.macAddress = ethernetAdapter->macAddress();
            statusInfo.internetAvailable = connected;
        }
#endif

#if FASTBEE_ENABLE_CELLULAR
        if (wifiConfig.networkType == NetworkType::NET_4G && cellularAdapter) {
            cellularAdapter->forceUpdate();  // API请求时强制刷新PDP状态
            bool connected = cellularAdapter->isConnected();
            statusInfo.status = connected ? NetworkStatus::CONNECTED : NetworkStatus::DISCONNECTED;
            statusInfo.ipAddress = connected ? cellularAdapter->localIP().toString() : "";
            statusInfo.internetAvailable = connected;
            // 更新 4G 蜂窝网络状态
            statusInfo.simStatus = cellularAdapter->isSimReady() ? "ready" : "missing";
            statusInfo.operatorName = cellularAdapter->getOperator();
            statusInfo.cellularNetworkType = cellularAdapter->getNetworkType();
            statusInfo.apn = wifiConfig.cellular.apn;
            statusInfo.imei = cellularAdapter->getIMEI();
            statusInfo.iccid = cellularAdapter->getICCID();
            
            // 信号质量转换 (CSQ 0-31 -> RSSI dBm)
            int csq = cellularAdapter->getSignalQualityCSQ();
            if (csq >= 0 && csq <= 31) {
                statusInfo.cellularSignalQuality = csq;
                statusInfo.rssi = -113 + (csq * 2);  // CSQ to dBm conversion
            } else {
                statusInfo.cellularSignalQuality = 99;  // 未知
                statusInfo.rssi = 0;
            }
        }
#endif

        return;  // 非 WiFi 联网方式状态更新完成
    }

    // ========== WiFi 联网方式：同步 WiFiManager 的完整状态 ==========
    statusInfo = wifiManager->getStatusInfo();
}

// updateIPConflictStatus 方法已移至 IPManager 类

// IP 冲突检测和故障转移方法已移至 IPManager 类

void FBNetworkManager::attemptReconnect() {
    // 重连次数限制：达到最大次数后回退到纯 AP 模式供用户配置
    if (statusInfo.reconnectAttempts >= wifiConfig.maxReconnectAttempts) {
        LOG_WARNING("NetworkManager: Max reconnect attempts reached; switching to AP mode");
        
        // 断开当前 STA 连接
        wifiManager->disconnectWiFi();
        
        // 启动纯 AP 模式（不使用 AP+STA，避免模式切换导致栈溢出）
        if (startAPMode()) {
            LOGGER.infof(">>> AP fallback: AP IP=%s  SSID=[%s] pwd=[%s] <<<",
                WiFi.softAPIP().toString().c_str(),
                wifiConfig.apSSID.c_str(),
                wifiConfig.apPassword.c_str());
            Serial.println("[WiFi] AP fallback active - STA reconnect disabled until user reconfigures");
            LOG_INFO("NetworkManager: AP fallback active, STA reconnect paused");
            
            // 切换到 AP 模式，停止自动重连
            wifiConfig.mode = NetworkMode::NETWORK_AP;
            autoReconnectEnabled = false;
            statusInfo.status = NetworkStatus::AP_MODE;
        } else {
            LOG_ERROR("NetworkManager: Failed to start AP mode after STA failure");
            statusInfo.reconnectAttempts = 0;
            lastReconnectAttempt = millis() + 55000;  // 等待 60 秒
        }
        return;
    }

    lastReconnectAttempt = millis();
    statusInfo.reconnectAttempts++;

    char reconnBuf[120];
    snprintf(reconnBuf, sizeof(reconnBuf),
             "[WiFi] Reconnect attempt %d/%d (interval=%lums)",
             (int)statusInfo.reconnectAttempts,
             (int)wifiConfig.maxReconnectAttempts,
             (unsigned long)wifiConfig.reconnectInterval);
    Serial.println(reconnBuf);
    LOG_INFO("NetworkManager: Reconnection attempt " +
             String(statusInfo.reconnectAttempts) +
             "/" + String(wifiConfig.maxReconnectAttempts));

    // 同步配置到 WiFiManager 后再尝试重连
    wifiManager->setNetworkConfig(wifiConfig);
    if (!connectToWiFi()) {
        connecting = false;
        statusInfo.status = NetworkStatus::CONNECTION_FAILED;
    }
}

void FBNetworkManager::triggerEvent(NetworkStatus status, const String& message) {
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

WiFiConfig FBNetworkManager::getConfig() const {
    return wifiConfig;
}

NetworkStatusInfo FBNetworkManager::getStatusInfo() const {
    return statusInfo;
}

String FBNetworkManager::scanNetworks() {
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
    
    // 序列化到静态缓冲区
    size_t len = serializeJson(doc, buffer, sizeof(buffer));
    buffer[len] = '\0';
    return String(buffer);
}

bool FBNetworkManager::connectToNetwork(const String& ssid, const String& password) {
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

void FBNetworkManager::disconnectNetwork() {
    wifiManager->disconnectWiFi();
}

bool FBNetworkManager::restartNetwork() {
    char modeBuf[64];
    snprintf(modeBuf, sizeof(modeBuf), "NetworkManager: Restarting network (mode: %d)...", (int)wifiConfig.mode);
    LOG_INFO(modeBuf);
    
    // 设置模式切换标志，避免记录不必要的断开警告
    wifiManager->setModeTransitioning(true);
    
    // 保存当前模式以便比较
    NetworkMode newMode = wifiConfig.mode;
    bool keepAutoReconnect = autoReconnectEnabled;
    
    if (wifiConfig.networkType == NetworkType::NET_WIFI && newMode == NetworkMode::NETWORK_AP) {
        LOG_INFO("NetworkManager: Restarting in AP mode...");
        
        // 断开STA连接
        disconnect();
        autoReconnectEnabled = keepAutoReconnect;
        // 停止现有AP，再重新启动以确保配置正确
        delay(100);
        wifiManager->setNetworkConfig(wifiConfig);
        if (!wifiManager->startAPMode()) {
            LOG_ERROR("NetworkManager: Failed to start AP mode");
            wifiManager->setModeTransitioning(false);
            return false;
        }
        // 启动mDNS服务
        if (wifiConfig.enableMDNS) {
            dnsManager->startMDNS(wifiConfig.customDomain);
            LOG_INFO("NetworkManager: mDNS service restarted after network restart");
        }
        statusInfo.status = NetworkStatus::AP_MODE;
        statusInfo.internetAvailable = false;
        isInitialized = true;
        wifiManager->setModeTransitioning(false);
        LOG_INFO("NetworkManager: AP mode restarted successfully");
        // 触发网络模式切换为AP系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
        PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_NET_MODE_AP, "");
#endif
        return true;
    }
    
    // 对于非WiFi联网方式（4G/以太网），选择性断开，保持AP热点在线
    if (wifiConfig.networkType != NetworkType::NET_WIFI) {
        LOG_INFO("NetworkManager: Non-WiFi network restart, keeping AP as safety net...");
        
        // 网络切换前回调：停止依赖适配器 Client 指针的协议（如 MQTT），防止 use-after-free
        // 必须在销毁适配器之前调用，因为适配器的 Client 对象会被析构
        if (_preNetworkSwitchCb) {
            LOG_INFO("NetworkManager: Pre-network-switch callback invoked (stopping dependent protocols)");
            _preNetworkSwitchCb();
        }

        // 仅断开非AP接口（保持AP热点作为配置回退入口）
        dnsManager->stopMDNS();
        // 关键：mDNS 停止后需要短暂等待，确保其 UDP socket 和 FreeRTOS 任务完全清理
        // 否则销毁适配器时 mDNS 可能仍在访问已销毁的 netif → xQueueSemaphoreTake 崩溃
        delay(200);
#if FASTBEE_ENABLE_ETHERNET
        if (ethernetAdapter) {
            ethernetAdapter->disconnect();
            ethernetAdapter.reset();
        }
#endif
#if FASTBEE_ENABLE_CELLULAR
        if (cellularAdapter) {
            cellularAdapter->disconnect();
            cellularAdapter.reset();
        }
#endif
        // 断开WiFi STA但不关闭AP
        wifiManager->disconnectWiFi();

        autoReconnectEnabled = keepAutoReconnect;
        delay(100);  // 缩短延迟（500ms→100ms），减少网络切换期间的不可用时间

        // 销毁旧适配器后让 FreeRTOS idle 任务回收内存，减少堆碎片化
        uint32_t postDestroyHeap = ESP.getFreeHeap();
        uint32_t postDestroyMaxBlock = ESP.getMaxAllocHeap();
        LOG_INFOF("NetworkManager: Adapters destroyed, heap=%lu maxBlock=%lu",
                  (unsigned long)postDestroyHeap, (unsigned long)postDestroyMaxBlock);
        
        // 直接重新初始化网络适配器，不调用 initialize()（因为 isInitialized 仍为 true）
        // initialize() 会跳过已初始化状态，导致以太网/4G 适配器永远不会被重新创建
        bool ok = true;
#if FASTBEE_ENABLE_ETHERNET
        if (wifiConfig.networkType == NetworkType::NET_ETHERNET) {
            LOG_INFO("NetworkManager: Re-initializing Ethernet (W5500)...");
            ethernetAdapter.reset(new EthernetAdapter());
            bool ethOk = ethernetAdapter->begin(wifiConfig);
            if (ethOk) {
                ethOk = ethernetAdapter->waitForConnection(10000);
            }
            if (ethOk) {
                LOG_INFO("NetworkManager: Ethernet reconnected, IP: " + ethernetAdapter->localIP().toString());
                statusInfo.status = NetworkStatus::CONNECTED;
                statusInfo.ipAddress = ethernetAdapter->localIP().toString();
                statusInfo.lastConnectionTime = millis();
                ethReconnectAttempts = 0;  // 重置重连计数
                ethReconnectPending = false;
                
                // ========== 混合模式：以太网 + WiFi AP（与 initialize() 一致）==========
                // 启动WiFi AP热点，用于本地Web配置访问和mDNS服务
                if (!(WiFi.getMode() & WIFI_AP)) {
                    LOG_INFO("NetworkManager: Starting WiFi AP for hybrid mode (Ethernet + AP)...");
                    wifiManager->setNetworkConfig(wifiConfig);
                    if (wifiManager->startAPMode()) {
                        statusInfo.apIPAddress = WiFi.softAPIP().toString();
                        LOGGER.infof(">>> Config AP: %s  IP: %s <<<",
                            wifiConfig.apSSID.c_str(), WiFi.softAPIP().toString().c_str());
                        LOG_INFO("NetworkManager: Hybrid mode active (Ethernet + WiFi AP)");
                    } else {
                        LOG_WARNING("NetworkManager: Failed to start WiFi AP, Ethernet only mode");
                    }
                }
                // 确保状态保持 CONNECTED（以太网是主网络，不受 AP 状态影响）
                statusInfo.status = NetworkStatus::CONNECTED;
                
                if (wifiConfig.enableMDNS) {
                    dnsManager->startMDNS(wifiConfig.customDomain);
                }
            } else {
                LOG_WARNING("NetworkManager: Ethernet reconnect failed, falling back to AP");
                ethernetAdapter.reset();
                // 与 initialize() 一致：回退时更新 networkType 为 NET_WIFI
                // 否则 isNetworkConnected() 仍检查以太网适配器 → 始终返回 false
                // → MQTT 永远无法重启，Web UI 始终显示“未连接”
                wifiConfig.networkType = NetworkType::NET_WIFI;
                wifiConfig.mode = NetworkMode::NETWORK_AP;
                ensureLastResortAP();
                dnsManager->startMDNS(wifiConfig.customDomain);
                ok = false;
            }
        }
#endif
#if FASTBEE_ENABLE_CELLULAR
        if (wifiConfig.networkType == NetworkType::NET_4G) {
            LOG_INFO("NetworkManager: Re-initializing 4G Cellular...");
            cellularAdapter.reset(new CellularAdapter());
            bool cellOk = cellularAdapter->begin(wifiConfig);
            if (cellOk) {
                LOG_INFO("NetworkManager: 4G reconnected");
                statusInfo.status = NetworkStatus::CONNECTED;
                statusInfo.lastConnectionTime = millis();
                cellReconnectAttempts = 0;
                cellReconnectPending = false;
                if (cellularAdapter) {
                    IPAddress cellIP = cellularAdapter->localIP();
                    if (cellIP != IPAddress(0, 0, 0, 0)) {
                        statusInfo.ipAddress = cellIP.toString();
                    }
                }
                
                // ========== 混合模式：4G + WiFi AP（与 initialize() 一致）==========
                if (!(WiFi.getMode() & WIFI_AP)) {
                    LOG_INFO("NetworkManager: Starting WiFi AP for hybrid mode (4G + AP)...");
                    wifiManager->setNetworkConfig(wifiConfig);
                    if (wifiManager->startAPMode()) {
                        statusInfo.apIPAddress = WiFi.softAPIP().toString();
                        LOG_INFO("NetworkManager: Hybrid mode active (4G + WiFi AP)");
                    } else {
                        LOG_WARNING("NetworkManager: Failed to start WiFi AP, 4G only mode");
                    }
                }
                // 确保状态保持 CONNECTED（4G是主网络）
                statusInfo.status = NetworkStatus::CONNECTED;
                
                if (wifiConfig.enableMDNS) {
                    dnsManager->startMDNS(wifiConfig.customDomain);
                }
                // 通知 MQTT 客户端 4G 网络已恢复，立即尝试重连
#if FASTBEE_ENABLE_MQTT
                {
                    auto* fw2 = FastBeeFramework::getInstance();
                    auto* pm2 = fw2 ? fw2->getProtocolManager() : nullptr;
                    MQTTClient* mqtt = pm2 ? pm2->getMQTTClient() : nullptr;
                    if (mqtt) {
                        mqtt->resetErrorCounters();
                        LOG_INFO("[NET] MQTT reconnect triggered after 4G re-init");
                    }
                }
#endif
            } else {
                LOG_WARNING("NetworkManager: 4G reconnect failed, falling back to AP");
                cellularAdapter.reset();
                // 与 initialize() 一致：回退时更新 networkType 为 NET_WIFI
                wifiConfig.networkType = NetworkType::NET_WIFI;
                wifiConfig.mode = NetworkMode::NETWORK_AP;
                cellReconnectPending = false;
                ensureLastResortAP();
                dnsManager->startMDNS(wifiConfig.customDomain);
                ok = false;
            }
        }
#endif
        wifiManager->setModeTransitioning(false);
        return ok;
    }
    
    // 对于WiFi STA模式，需要完全重启
    LOG_INFO("NetworkManager: Full network restart for STA mode...");
    // 触发网络模式切换为STA系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
    PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_NET_MODE_STA, "");
#endif
    disconnect();
    autoReconnectEnabled = keepAutoReconnect;
    delay(500);
    bool ok = initialize();
    wifiManager->setModeTransitioning(false);
    return ok;
}

bool FBNetworkManager::checkInternetConnection() {
    return isNetworkConnected();
}

void FBNetworkManager::setConnectionCallback(NetworkEventCallback callback) {
    connectionCallback = callback;
}

void FBNetworkManager::setDisconnectionCallback(NetworkEventCallback callback) {
    disconnectionCallback = callback;
}

void FBNetworkManager::setIPConflictCallback(NetworkEventCallback callback) {
    ipConflictCallback = callback;
}

void FBNetworkManager::setAutoReconnect(bool enabled) {
    autoReconnectEnabled = enabled;
    LOG_INFO("NetworkManager: Auto reconnect: " + 
             String(enabled ? "enabled" : "disabled"));
}

String FBNetworkManager::getConfigJSON() {
    StaticJsonDocument<3072> doc;
    
    // 基本配置
    doc["mode"] = static_cast<uint8_t>(wifiConfig.mode);
    // deviceName 已移至设备配置，getConfigJSON 不再返回
    
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

    // 联网方式
    doc["networkType"] = static_cast<uint8_t>(wifiConfig.networkType);

    // 以太网配置
    JsonObject ethCfg = doc.createNestedObject("ethernet");
    ethCfg["spiMosi"] = wifiConfig.ethernet.spiMosi;
    ethCfg["spiMiso"] = wifiConfig.ethernet.spiMiso;
    ethCfg["spiSck"]  = wifiConfig.ethernet.spiSck;
    ethCfg["csPin"]   = wifiConfig.ethernet.csPin;
    ethCfg["rstPin"]  = wifiConfig.ethernet.rstPin;
    ethCfg["intPin"]  = wifiConfig.ethernet.intPin;

    // 4G 蜂窝配置
    JsonObject cellCfg = doc.createNestedObject("cellular");
    cellCfg["txPin"]    = wifiConfig.cellular.txPin;
    cellCfg["rxPin"]    = wifiConfig.cellular.rxPin;
    cellCfg["pwrPin"]   = wifiConfig.cellular.pwrPin;
    cellCfg["baudRate"] = wifiConfig.cellular.baudRate;
    cellCfg["apn"]      = wifiConfig.cellular.apn;

    String result;
    serializeJson(doc, result);
    return result;
}

bool FBNetworkManager::updateConfig(const WiFiConfig& newConfig, bool saveToStorage) {
    bool restartRequired = false;
    
    // 检查是否需要重启网络
    if (newConfig.mode != wifiConfig.mode ||
        newConfig.apSSID != wifiConfig.apSSID ||
        newConfig.staSSID != wifiConfig.staSSID ||
        newConfig.ipConfigType != wifiConfig.ipConfigType ||
        newConfig.networkType != wifiConfig.networkType ||
        newConfig.ethernet.spiMosi != wifiConfig.ethernet.spiMosi ||
        newConfig.ethernet.spiMiso != wifiConfig.ethernet.spiMiso ||
        newConfig.ethernet.spiSck != wifiConfig.ethernet.spiSck ||
        newConfig.ethernet.csPin != wifiConfig.ethernet.csPin ||
        newConfig.ethernet.rstPin != wifiConfig.ethernet.rstPin ||
        newConfig.ethernet.intPin != wifiConfig.ethernet.intPin ||
        newConfig.cellular.txPin != wifiConfig.cellular.txPin ||
        newConfig.cellular.rxPin != wifiConfig.cellular.rxPin ||
        newConfig.cellular.pwrPin != wifiConfig.cellular.pwrPin ||
        newConfig.cellular.baudRate != wifiConfig.cellular.baudRate ||
        newConfig.cellular.apn != wifiConfig.cellular.apn) {
        restartRequired = true;
    }
    
    // 检查 mDNS/域名配置变更（不需要重启网络，但需立即重启 mDNS 服务）
    bool mdnsConfigChanged = (newConfig.customDomain != wifiConfig.customDomain ||
                              newConfig.enableMDNS != wifiConfig.enableMDNS);
    
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
    
    // mDNS 配置变更：如果已有设备重启调度，跳过 mDNS 运行时重启（重启后 mDNS 会自动初始化）
    if (mdnsConfigChanged && !restartRequired && dnsManager) {
        if (SystemRebooter::isScheduled()) {
            LOG_INFO("NetworkManager: mDNS config changed but device reboot already scheduled, skipping mDNS runtime restart");
        } else if (wifiConfig.enableMDNS) {
            LOG_INFOF("NetworkManager: mDNS config changed, scheduling delayed restart with hostname '%s'",
                      wifiConfig.customDomain.c_str());
            pendingMDNSRestart = true;
            pendingMDNSRestartTime = 0;  // 在 update() 中记录时间戳
        } else {
            LOG_INFO("NetworkManager: mDNS disabled by config change, stopping mDNS");
            dnsManager->setMDNSEnabled(false);
        }
    }
    
    // 如果需要重启网络，仅在无设备级重启调度时设置运行时网络重启
    if (restartRequired) {
        if (SystemRebooter::isScheduled()) {
            LOG_INFO("NetworkManager: Network restart required but device reboot already scheduled");
        } else {
            LOG_INFO("NetworkManager: Network restart required, scheduling runtime restart...");
            pendingRestart = true;
        }
    }
    
    // 返回保存结果（不等待网络重启完成）
    return saveSuccess;
}

bool FBNetworkManager::updateConfigFromJSON(const String& jsonConfig) {
    StaticJsonDocument<3072> doc;
    DeserializationError error = deserializeJson(doc, jsonConfig);
    
    if (error) {
        LOG_ERROR("NetworkManager: Failed to parse JSON configuration");
        return false;
    }
    
    WiFiConfig newConfig = wifiConfig;
    
    // 更新配置
    if (doc.containsKey("mode")) 
        newConfig.mode = static_cast<NetworkMode>(doc["mode"].as<uint8_t>());
    // deviceName 已移至设备配置，updateConfigFromJSON 不再解析
        
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

    // 联网方式
    if (doc.containsKey("networkType")) {
        uint8_t rawNetworkType = doc["networkType"].as<uint8_t>();
        newConfig.networkType = rawNetworkType <= static_cast<uint8_t>(NetworkType::NET_4G)
            ? static_cast<NetworkType>(rawNetworkType)
            : NetworkType::NET_WIFI;
    }

    // 以太网配置
    if (doc.containsKey("ethernet") && doc["ethernet"].is<JsonObject>()) {
        JsonObject eth = doc["ethernet"];
        if (eth.containsKey("spiMosi")) newConfig.ethernet.spiMosi = eth["spiMosi"].as<int8_t>();
        if (eth.containsKey("spiMiso")) newConfig.ethernet.spiMiso = eth["spiMiso"].as<int8_t>();
        if (eth.containsKey("spiSck"))  newConfig.ethernet.spiSck  = eth["spiSck"].as<int8_t>();
        if (eth.containsKey("csPin"))   newConfig.ethernet.csPin   = eth["csPin"].as<int8_t>();
        if (eth.containsKey("rstPin"))  newConfig.ethernet.rstPin  = eth["rstPin"].as<int8_t>();
        if (eth.containsKey("intPin"))  newConfig.ethernet.intPin  = eth["intPin"].as<int8_t>();
    }

    // 4G 蜂窝配置
    if (doc.containsKey("cellular") && doc["cellular"].is<JsonObject>()) {
        JsonObject cell = doc["cellular"];
        if (cell.containsKey("txPin"))    newConfig.cellular.txPin    = cell["txPin"].as<int8_t>();
        if (cell.containsKey("rxPin"))    newConfig.cellular.rxPin    = cell["rxPin"].as<int8_t>();
        if (cell.containsKey("pwrPin"))   newConfig.cellular.pwrPin   = cell["pwrPin"].as<int8_t>();
        if (cell.containsKey("baudRate")) newConfig.cellular.baudRate = cell["baudRate"].as<uint32_t>();
        if (cell.containsKey("apn"))      newConfig.cellular.apn      = cell["apn"].as<String>();
    }

    return updateConfig(newConfig, true);
}

bool FBNetworkManager::resetToDefaults() {
    wifiConfig = WiFiConfig();
    preferences.clear();
    
    LOG_INFO("NetworkManager: Network configuration reset to defaults");
    return restartNetwork();
}

String FBNetworkManager::getStatistics() {
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

bool FBNetworkManager::isValidIP(const String& ip) {
    IPAddress addr;
    return addr.fromString(ip);
}

bool FBNetworkManager::isValidSubnet(const String& subnet) {
    return isValidIP(subnet);
}

String FBNetworkManager::getMACAddress() {
    return WiFi.macAddress();
}

String FBNetworkManager::getChipID() {
    uint64_t chipid = ESP.getEfuseMac();
    char chipidStr[13];
    snprintf(chipidStr, sizeof(chipidStr), "%04X%08X", 
             (uint16_t)(chipid >> 32), (uint32_t)chipid);
    return String(chipidStr);
}

String FBNetworkManager::getWiFiModeString() {
    WiFiMode_t mode = WiFi.getMode();
    return NetworkUtils::getWiFiModeString(mode);
}

bool FBNetworkManager::checkIPConflict() {
    return ipManager->checkIPConflict();
}

WiFiManager* FBNetworkManager::getWiFiManager() {
    return wifiManager.get();
}

IPManager* FBNetworkManager::getIPManager() {
    return ipManager.get();
}

DNSManager* FBNetworkManager::getDNSManager() {
    return dnsManager.get();
}

void FBNetworkManager::incrementTxCount() { statusInfo.txCount++; }
void FBNetworkManager::incrementRxCount() { statusInfo.rxCount++; }

// ============ 多网络类型支持 ============

INetworkAdapter* FBNetworkManager::getActiveAdapter() {
    switch (wifiConfig.networkType) {
#if FASTBEE_ENABLE_ETHERNET
        case NetworkType::NET_ETHERNET:
            return ethernetAdapter.get();
#endif
#if FASTBEE_ENABLE_CELLULAR
        case NetworkType::NET_4G:
            return cellularAdapter.get();
#endif
        default:
            return nullptr;  // WiFi 模式无适配器
    }
}

Client* FBNetworkManager::getActiveClient() {
    switch (wifiConfig.networkType) {
        case NetworkType::NET_WIFI:
            // WiFi 使用默认的 WiFiClient（由 MQTTClient 内部创建）
            return nullptr;  // 返回 nullptr 表示使用默认 WiFiClient

        default: {
            INetworkAdapter* adapter = getActiveAdapter();
            return (adapter && adapter->isConnected()) ? adapter->getClient() : nullptr;
        }
    }
}

bool FBNetworkManager::isNetworkConnected() {
    switch (wifiConfig.networkType) {
        case NetworkType::NET_WIFI:
            return WiFi.status() == WL_CONNECTED;

        default: {
            INetworkAdapter* adapter = getActiveAdapter();
            return adapter && adapter->isConnected();
        }
    }
}
