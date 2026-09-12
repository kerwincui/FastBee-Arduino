/**
 * @file NetworkManager.cpp
 * @brief 网络管理器实现
 * @author kerwincui
 * @date 2025-12-02
 */

#include "network/NetworkManager.h"
#include "systems/LoggerSystem.h"
#include "systems/ConfigStorage.h"
#include "systems/RestartDiagnostics.h"
#include "systems/SystemRebooter.h"
#include "utils/NetworkUtils.h"
#include "core/FeatureFlags.h"
#include "core/RuntimeFeatureManager.h"  // 以太网/4G 运行时功能门控
#include <esp_heap_caps.h>  // heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
#include "systems/HealthMonitor.h"  // WIFI_CONNECT_MIN_DRAM
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
#include <lwip/ip4_addr.h>
#include <lwip/etharp.h>
#include <lwip/netif.h>
#include <lwip/priv/tcpip_priv.h>  // LOCK_TCPIP_CORE / UNLOCK_TCPIP_CORE
#include <ping/ping_sock.h>
#include <esp_ping.h>
#if FASTBEE_ENABLE_MDNS
#include <ESPmDNS.h>
#endif
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <algorithm>
#include "core/FastBeeFramework.h"  // 网络恢复 → MQTT 立即重连通知
#include "network/WebConfigManager.h"  // Web 前台门控（mDNS 同步查询避让）
#include "protocols/ProtocolManager.h"
#include "protocols/MQTTClient.h"

static const char* NETWORK_CONFIG_FILE = "/config/network.json";

// A configuration can outlive the firmware image that created it.  When a
// board is rebuilt without an optional adapter, normalize stale Ethernet/4G
// selections before the state machine starts so it can still use WiFi STA/AP.
static bool normalizeUnsupportedNetworkType(WiFiConfig& config) {
    bool changed = false;
#if !FASTBEE_ENABLE_ETHERNET
    if (config.networkType == NetworkType::NET_ETHERNET) {
        config.networkType = NetworkType::NET_WIFI;
        changed = true;
        LOG_WARNING("NetworkManager: Ethernet is not compiled; falling back to WiFi");
    }
#endif
#if !FASTBEE_ENABLE_CELLULAR
    if (config.networkType == NetworkType::NET_4G) {
        config.networkType = NetworkType::NET_WIFI;
        changed = true;
        LOG_WARNING("NetworkManager: cellular adapter is not compiled; falling back to WiFi");
    }
#endif
    return changed;
}

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
      , _lastApProbeTime(0)
      , _apProbeInProgress(false)
      , _apFallbackAuto(false)
      , _apProbeFailCount(0)
      , _apProbeState(ApProbeState::IDLE)
      , _apProbeCandidateCount(0)
      , _apProbeAttempt(0)
      , _apProbeStateSince(0)
      , _apProbeStartedAt(0)
      , _apProbeAttemptStartedAt(0)
      , _bootConnectRetries(0)
      , _bootStaRetryScanInProgress(false)
      , _bootStaRetryScanState(BootStaRetryScanState::IDLE)
      , _bootStaRetryScanStartedAt(0)
      , _bootStaRetryCandidateCount(0)
      , _staDisconnectedSince(0)
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

    // 重置 AP 回退探测状态（每次重新初始化都从干净状态开始）
    _apFallbackAuto = false;
    _apProbeFailCount = 0;
    resetApModeWifiProbe();
    _bootConnectRetries = 0;
    resetBootStaRetryScan();

    // 初始化Preferences（独立 namespace，避免与 ConfigStorage 的 "fastbee" 冲突）
    if (!preferences.begin("net_cfg", false)) {
        LOG_ERROR("NetworkManager: Failed to initialize preferences");
        return false;
    }

    // 加载网络配置
    ets_printf("[NET] Loading network config...\n");
    bool networkConfigNeedsSave = false;
    if (!loadNetworkConfig()) {
        ets_printf("[NET] Using default network configuration\n");
        // 生成默认备用IP
        ipManager->generateBackupIPs();
        networkConfigNeedsSave = true;
    } else if (normalizeUnsupportedNetworkType(wifiConfig)) {
        networkConfigNeedsSave = true;
    }

    // features.json 是 mDNS 启停的权威配置。旧版只在 network.json 保存
    // enableMDNS；仅当功能文件没有显式 mdns 键时迁移该旧值，之后
    // network.json 中的同名字段只作为兼容镜像。
    auto& runtimeFeatures = FastBee::RuntimeFeatureManager::instance();
    if (!runtimeFeatures.hasExplicitSetting("mdns")) {
        bool accepted = runtimeFeatures.setUserEnabled("mdns", wifiConfig.enableMDNS);
        if (!accepted && wifiConfig.enableMDNS) {
            // A configuration can move to a build without mDNS.  Persist the
            // only usable value so the sparse migration does not repeat forever.
            accepted = runtimeFeatures.setUserEnabled("mdns", false);
        }
        if (accepted) {
            if (!runtimeFeatures.saveCurrentConfig()) {
                LOG_WARNING("NetworkManager: failed to persist legacy mDNS feature migration");
            }
        }
    }
    const bool configuredMDNS = runtimeFeatures.isUserEnabled("mdns");
    if (wifiConfig.enableMDNS != configuredMDNS) {
        wifiConfig.enableMDNS = configuredMDNS;
        networkConfigNeedsSave = true;
    }
    if (networkConfigNeedsSave && !saveNetworkConfig()) {
        LOG_WARNING("NetworkManager: failed to persist normalized network configuration");
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

    // 智能模式修正（反向 / STA 优先）：mode 是 NETWORK_AP 但已配置有效 staSSID。
    // 设备原本能连 STA，一旦 network.json 因 Web「网络设置」保存、异常或历史回退
    // 被持久化成 AP，开机会直接进 AP 且「永不尝试 STA」——因为 AP→STA 探测恢复
    // 只在运行时 STA 失败自动回退（_apFallbackAuto=true）时才触发，持久化 AP 并不
    // 触发探测，于是设备永久卡在 192.168.4.1，表现为「WiFi 配置正确却只能 AP 访问、
    // 老是连不上」。既然存在有效 WiFi 凭据，则开机优先尝试 STA；STA 失败仍会走既有
    // 回退逻辑退回 AP 供用户重新配网，不会丢失可达性。
    if (wifiConfig.networkType == NetworkType::NET_WIFI &&
        !wifiConfig.staSSID.isEmpty() && wifiConfig.mode == NetworkMode::NETWORK_AP) {
        LOG_WARNING("NetworkManager: staSSID present but mode=AP; preferring STA (auto-correct AP->STA)");
        wifiConfig.mode = NetworkMode::NETWORK_STA;
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
    dnsManager->setMDNSEnabled(shouldRunMDNS());

    // WiFi 事件回调已在 WiFiManager::initialize() 中注册，此处不再重复注册
    // 重复注册会导致每个事件在 arduino_events 栈上执行两次，引发栈溢出崩溃

    // 同步配置到 WiFiManager（确保 AP 密码等参数从 network.json 传递到底层驱动）
    // 必须在所有网络路径（以太网+AP、4G+AP、WiFi STA/AP）之前执行
    wifiManager->setNetworkConfig(wifiConfig);

    // ========== 非 WiFi 联网方式初始化 ==========
    ets_printf("[NET] networkType=%d (WIFI=0, ETH=1, 4G=2)\n", (int)wifiConfig.networkType);
#if FASTBEE_ENABLE_CELLULAR
    ets_printf("[NET] CELLULAR=1, checking 4G (type==%d?=%d)\n", (int)wifiConfig.networkType, (int)NetworkType::NET_4G);
#else
    ets_printf("[NET] CELLULAR=0 (4G not compiled)\n");
#endif
#if FASTBEE_ENABLE_ETHERNET
    // 运行时功能门控：以太网功能被禁用（features.json）时跳过适配器初始化，回退 WiFi
    if (wifiConfig.networkType == NetworkType::NET_ETHERNET &&
        !FastBee::RuntimeFeatureManager::instance().shouldEnable("ethernet")) {
        LOG_WARNING("NetworkManager: Ethernet feature disabled, falling back to WiFi");
        wifiConfig.networkType = NetworkType::NET_WIFI;
    }
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
            if (shouldRunMDNS()) {
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
    // 运行时功能门控：4G 功能被禁用（features.json）时跳过适配器初始化，回退 WiFi
    if (wifiConfig.networkType == NetworkType::NET_4G &&
        !FastBee::RuntimeFeatureManager::instance().shouldEnable("cellular")) {
        LOG_WARNING("NetworkManager: 4G cellular feature disabled, falling back to WiFi");
        wifiConfig.networkType = NetworkType::NET_WIFI;
    }
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
            if (shouldRunMDNS()) {
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
                    // The historical function name is retained for compatibility;
                    // initial scanning and association are now always advanced by
                    // update(), regardless of the Web start mode.
                    LOG_INFO("NetworkManager: STA connection initiated (async candidate scan)");
                } else {
                    if (!wifiConfig.autoFailover) {
                        LOG_WARNING("NetworkManager: STA connection failed; AP fallback disabled, keeping STA retry mode");
                        wifiManager->setModeTransitioning(false);
                        wifiConfig.mode = NetworkMode::NETWORK_STA;
                        autoReconnectEnabled = true;
                        statusInfo.status = NetworkStatus::DISCONNECTED;
                        _staDisconnectedSince = millis();
                        lastReconnectAttempt = millis();
                        success = true;  // Web remains available while STA retries asynchronously.
                        break;
                    }
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
                        LOGGER.infof(">>> AP fallback: AP IP=%s  SSID=[%s] pwd=*** (len=%d) <<<",
                            WiFi.softAPIP().toString().c_str(),
                            wifiConfig.apSSID.c_str(),
                            wifiConfig.apPassword.length());
                        // 回退到 AP 模式后更新配置模式
                        wifiConfig.mode = NetworkMode::NETWORK_AP;
                        _apFallbackAuto = true;  // 标记为自动回退，启用 WiFi 探测恢复
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
                LOGGER.infof(">>> AP IP: %s  Connect to [%s] pwd:*** (len=%d) <<<",
                    WiFi.softAPIP().toString().c_str(),
                    wifiConfig.apSSID.c_str(),
                    wifiConfig.apPassword.length());
                
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
    resetApModeWifiProbe();
    resetBootStaRetryScan();
    
    // 重置状态信息
    statusInfo = NetworkStatusInfo();
    statusInfo.status = NetworkStatus::DISCONNECTED;
    
    LOG_INFO("NetworkManager: All network connections disconnected");
}

void FBNetworkManager::resetBootStaRetryScan() {
    if (_bootStaRetryScanInProgress &&
        _bootStaRetryScanState == BootStaRetryScanState::SCANNING) {
        WiFi.scanDelete();
    }
    _bootStaRetryScanInProgress = false;
    _bootStaRetryScanState = BootStaRetryScanState::IDLE;
    _bootStaRetryScanStartedAt = 0;
    _bootStaRetryCandidateCount = 0;
}

void FBNetworkManager::launchBootStaRetryConnection() {
    const bool usePinnedCandidate =
        _bootConnectRetries >= 2 && _bootStaRetryCandidateCount > 1;
    if (usePinnedCandidate) {
        const int selected =
            (_bootConnectRetries - 1) % _bootStaRetryCandidateCount;
        const BootStaCandidate& ap = _bootStaRetryCandidates[selected];
        Serial.printf("[NET] STA boot retry pinned BSSID %02X:%02X:%02X:%02X:%02X:%02X ch=%d rssi=%d (%d/%d)\n",
                      ap.bssid[0], ap.bssid[1], ap.bssid[2],
                      ap.bssid[3], ap.bssid[4], ap.bssid[5],
                      (int)ap.channel, (int)ap.rssi,
                      selected + 1, (int)_bootStaRetryCandidateCount);
        WiFi.begin(wifiConfig.staSSID.c_str(), wifiConfig.staPassword.c_str(),
                   ap.channel, ap.bssid);
    } else {
        Serial.printf("[NET] STA boot retry %d using automatic AP selection\n",
                      (int)_bootConnectRetries);
        WiFi.begin(wifiConfig.staSSID.c_str(), wifiConfig.staPassword.c_str());
    }
    connecting = true;
    connectingStartTime = millis();
    statusInfo.status = NetworkStatus::CONNECTING;
}

void FBNetworkManager::startBootStaRetryScan() {
    // Rebuild the driver as cooperative settle states. The old timeout path
    // used delay(250)+delay(500) here, which made one network_update call block
    // for ~750ms before the scan even started.
    _bootStaRetryCandidateCount = 0;
    _bootStaRetryScanStartedAt = millis();
    _bootStaRetryScanState = BootStaRetryScanState::DRIVER_NULL_SETTLE;
    _bootStaRetryScanInProgress = true;
    WiFi.mode(WIFI_MODE_NULL);
    Serial.println("[NET] Async boot retry driver reset started");
}

void FBNetworkManager::handleBootStaRetryScan() {
    if (!_bootStaRetryScanInProgress) return;

    const unsigned long now = millis();

    if (_bootStaRetryScanState == BootStaRetryScanState::DRIVER_NULL_SETTLE) {
        if (now - _bootStaRetryScanStartedAt < 250UL) return;
        if (!WiFi.mode(WIFI_MODE_STA)) {
            LOG_WARNING("NetworkManager: STA driver restart reported failure; begin will retry initialization");
        }
        _bootStaRetryScanState = BootStaRetryScanState::DRIVER_STA_SETTLE;
        _bootStaRetryScanStartedAt = now;
        return;
    }

    if (_bootStaRetryScanState == BootStaRetryScanState::DRIVER_STA_SETTLE) {
        if (now - _bootStaRetryScanStartedAt < 500UL) return;
        WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
        WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(false);

        const int16_t scanStart = WiFi.scanNetworks(true, true);
        if (scanStart == WIFI_SCAN_FAILED) {
            WiFi.scanDelete();
            Serial.println("[NET] Async boot retry scan unavailable; using automatic AP selection");
            _bootStaRetryScanInProgress = false;
            _bootStaRetryScanState = BootStaRetryScanState::IDLE;
            _bootStaRetryScanStartedAt = 0;
            launchBootStaRetryConnection();
            return;
        }
        _bootStaRetryScanState = BootStaRetryScanState::SCANNING;
        _bootStaRetryScanStartedAt = now;
        Serial.println("[NET] Async boot retry scan started");
        return;
    }

    if (_bootStaRetryScanState != BootStaRetryScanState::SCANNING) return;

    int16_t scanCount = WiFi.scanComplete();
    if (scanCount == WIFI_SCAN_RUNNING &&
        now - _bootStaRetryScanStartedAt < BOOT_STA_SCAN_TIMEOUT_MS) {
        return;
    }

    const bool scanTimedOut = scanCount == WIFI_SCAN_RUNNING;
    if (scanTimedOut) {
        WiFi.scanDelete();
        scanCount = WIFI_SCAN_FAILED;
        Serial.println("[NET] Async boot retry scan timed out; using automatic AP selection");
    }

    if (scanCount >= 0) {
        for (int16_t i = 0; i < scanCount; i++) {
            if (WiFi.SSID(i) != wifiConfig.staSSID) continue;
            const uint8_t* bssid = WiFi.BSSID(i);
            if (!bssid) continue;

            bool duplicate = false;
            for (uint8_t j = 0; j < _bootStaRetryCandidateCount; j++) {
                if (memcmp(_bootStaRetryCandidates[j].bssid, bssid, 6) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate || _bootStaRetryCandidateCount >= 3) continue;

            BootStaCandidate& candidate =
                _bootStaRetryCandidates[_bootStaRetryCandidateCount++];
            memcpy(candidate.bssid, bssid, 6);
            candidate.channel = WiFi.channel(i);
            candidate.rssi = WiFi.RSSI(i);
        }
    }
    WiFi.scanDelete();

    // Sort strongest first, with BSSID as a deterministic tie breaker so a
    // mesh scan cannot change the retry order from one cycle to the next.
    for (uint8_t i = 1; i < _bootStaRetryCandidateCount; i++) {
        BootStaCandidate current = _bootStaRetryCandidates[i];
        uint8_t insertAt = i;
        while (insertAt > 0) {
            const BootStaCandidate& previous =
                _bootStaRetryCandidates[insertAt - 1];
            const bool stronger = current.rssi > previous.rssi;
            const bool sameRssi = current.rssi == previous.rssi;
            const bool stableBefore =
                memcmp(previous.bssid, current.bssid, 6) > 0;
            if (!(stronger || (sameRssi && stableBefore))) break;
            _bootStaRetryCandidates[insertAt] = previous;
            --insertAt;
        }
        _bootStaRetryCandidates[insertAt] = current;
    }

    Serial.printf("[NET] Async boot retry scan complete: %d candidate BSSID%s\n",
                  (int)_bootStaRetryCandidateCount,
                  _bootStaRetryCandidateCount == 1 ? "" : "s");
    _bootStaRetryScanInProgress = false;
    _bootStaRetryScanState = BootStaRetryScanState::IDLE;
    _bootStaRetryScanStartedAt = 0;
    launchBootStaRetryConnection();
}

void FBNetworkManager::update() {
    unsigned long currentTime = millis();
    static bool wasConnected = false;
    static unsigned long lastMdnsUpdate = 0;

    // 先处理 WiFi 事件的延迟重量级工作（PeriphExec/MQTT/回调），
    // 事件回调本身在 arduino_events 栈上仅置位标志，避免栈溢出
    if (wifiManager) {
        wifiManager->processPendingEvents();
    }

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
            if (dnsManager && shouldRunMDNS()) {
                dnsManager->setMDNSEnabled(true);
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
                if (shouldRunMDNS()) {
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
                if (shouldRunMDNS()) {
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
                    // 接口已从以太网切换到 AP，必须 restartMDNS 重新绑定（startMDNS 会因 mdnsStarted 提前返回）
                    if (shouldRunMDNS()) {
                        dnsManager->restartMDNS(wifiConfig.customDomain);
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
                if (shouldRunMDNS()) {
                    dnsManager->restartMDNS(wifiConfig.customDomain);
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
                    // 接口已从 4G 切换到 AP，必须 restartMDNS 重新绑定（startMDNS 会因 mdnsStarted 提前返回）
                    if (shouldRunMDNS()) {
                        dnsManager->restartMDNS(wifiConfig.customDomain);
                    }
                }
            }
        }
#endif

        if (currentTime - lastStatusUpdate >= 1000) {
            updateStatusInfo();
            lastStatusUpdate = currentTime;
        }

        // mDNS 健康检查降频门控：MDNS.begin() 内含同步域名冲突检测（queryService），
        // 实测阻塞 ~3s，原来 periph_exec R12 每 30s 一次导致 periodic DANGEROUSLY LONG。
        // Web 前台活动时跳过；空闲时降至 5min 一次（同名收敛属慢过程，足够）
        static unsigned long lastMdnsHealthMs = 0;
        if (currentTime - lastMdnsUpdate >= 30000) {
            if (shouldRunMDNS()) {
                // Web 前台门控：避免阻塞 AsyncTCP 事件循环
                auto* fwMdns = FastBeeFramework::getInstance();
                auto* webGate = fwMdns ? fwMdns->getWebConfigManager() : nullptr;
                bool inForeground = webGate && webGate->isForegroundRequestActive();
                
                // 降频判定：仅每 5min 执行一次健康检查（首次除外）
                unsigned long nowMs = millis();
                bool shouldSkipRateLimit = (lastMdnsHealthMs != 0 && 
                                            nowMs - lastMdnsHealthMs < 300000UL);
                
                if (!inForeground && !shouldSkipRateLimit) {
                    dnsManager->checkMDNSHealth();
                    lastMdnsUpdate = currentTime;
                    lastMdnsHealthMs = nowMs;  // 标记已执行健康检查
                }
            } else {
                lastMdnsUpdate = currentTime;
            }
        }
        return;
    }

    // AP recovery is a cooperative state machine. While it owns the WiFi
    // interface, suppress the normal STA transition/watchdog path so transient
    // probe states cannot be mistaken for a real runtime disconnect.
    if (_apProbeInProgress) {
        handleApModeWifiProbe();
        if (_apProbeInProgress) {
            return;
        }
        currentTime = millis();
    }

    // Boot STA retry candidate scanning is also cooperative. Do not enter the
    // normal timeout/reconnect path until the async scan has either completed
    // or reached its bounded timeout and launched the next STA attempt.
    if (_bootStaRetryScanInProgress) {
        handleBootStaRetryScan();
        if (_bootStaRetryScanInProgress) {
            return;
        }
        currentTime = millis();
    }

    bool isConnected = (WiFi.status() == WL_CONNECTED);
    
    // WiFi 连接成功（从断开变为连接）
    if (isConnected && !wasConnected) {
        LOG_INFO("NetworkManager: WiFi connected, IP: " + WiFi.localIP().toString());
        connecting = false;  // 非阻塞连接成功，清除 connecting 标志
        statusInfo.reconnectAttempts = 0;  // 重置重连计数器
        statusInfo.status = NetworkStatus::CONNECTED;
        statusInfo.lastConnectionTime = millis();
        _staDisconnectedSince = 0;  // 重连成功，清零长时间失联看门狗计时
        resetGatewayWatchdog();     // 重连成功，清零数据面连通性看门狗
        _bootConnectRetries = 0;    // 连接成功，清零开机重试计数
        
        // 重新启动mDNS以更新IP绑定
        if (shouldRunMDNS()) {
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

            // 冲突确认后自动回退 DHCP 并重连（IPManager::performFailover 仅维护计数，
            // 真正的切换动作需要 NetworkManager 层持久化配置并重新发起连接）
            if (ipManager->checkIPConflict() && wifiConfig.autoFailover) {
                LOG_WARNING("NetworkManager: Static IP conflict confirmed, falling back to DHCP");
                Serial.println("[NET] IP conflict detected! Switching to DHCP and reconnecting...");
                statusInfo.conflictDetected = true;
                wifiConfig.ipConfigType = IPConfigType::DHCP;
                saveNetworkConfig();  // 持久化，避免重启后再次陷入同一冲突
                connectToWiFi();
            }
        }
        lastConflictCheck = currentTime;
    }

    // 网关 ICMP 使用会话内成功基线做自适应判断。完全不响应 echo 的路由器始终
    // 只告警；已经成功过、随后连续多轮无回包，才视为 WiFi 僵尸关联并重连 STA。
    if (!connecting &&
        isConnected &&
        wifiConfig.mode == NetworkMode::NETWORK_STA &&
        statusInfo.status == NetworkStatus::CONNECTED &&
        (currentTime - statusInfo.lastConnectionTime >= 30000) &&
        (currentTime - _lastGwProbeTime >= GW_PROBE_INTERVAL_MS)) {
        _lastGwProbeTime = currentTime;
        // probeGatewayReachable() 在启动异步会话或等待回调时返回 true，表示
        // “尚未判定失败”，并不代表已收到回包。只有已有结果或兜底超时时
        // 才能更新成功基线/失败计数，否则每次新建会话都会错误清零计数。
        const bool probeResultReady =
            (_gwProbeState == 2) ||
            (_gwProbeState == 1 &&
             (currentTime - _gwProbeStart) >= GW_PROBE_TIMEOUT_MS);
        const bool gatewayReachable = probeGatewayReachable();
        if (!probeResultReady) {
            // 异步探测已启动或仍在等待，下一轮消费结果。
        } else if (gatewayReachable) {
            _gwProbeEverSucceeded = true;
            _gwArpFailCount = 0;
        } else {
            if (_gwArpFailCount < 255) _gwArpFailCount++;
            // A session that never established an ICMP baseline can still be a
            // false CONNECTED state (DHCP/IP is present but ARP/TCP is dead).
            // Recover it with a STA-only reconnect after the same bounded number
            // of failures; never escalate this path to reboot or AP fallback.
            const uint8_t recoveryThreshold = _gwProbeEverSucceeded
                ? GW_PROBE_RECOVERY_THRESHOLD
                : GW_PROBE_UNKNOWN_RECOVERY_THRESHOLD;
            const bool recoverZombie =
                _gwArpFailCount >= recoveryThreshold;
            if (recoverZombie) {
                Serial.printf("[NET] Gateway data path lost (baseline=%s, %d probes), reconnecting STA without reboot or AP fallback\n",
                              _gwProbeEverSucceeded ? "confirmed" : "unknown",
                              (int)_gwArpFailCount);
                LOG_WARNINGF("NetworkManager: Gateway data path lost (baseline=%s, %d probes), reconnecting STA without reboot or AP fallback",
                             _gwProbeEverSucceeded ? "confirmed" : "unknown",
                             (int)_gwArpFailCount);
                _gwArpFailCount = 0;
                _gwProbeEverSucceeded = false;
                wasConnected = false;
                statusInfo.status = NetworkStatus::DISCONNECTED;
                statusInfo.reconnectAttempts = 0;
                lastReconnectAttempt = 0;
                _staDisconnectedSince = currentTime;
                WiFi.disconnect(false);
            } else if (_gwArpFailCount <= GW_PROBE_WARNING_THRESHOLD ||
                (_gwArpFailCount % 20) == 0) {
                Serial.printf("[NET] Gateway ping unanswered (%d, baseline=%s, pending STA-only recovery)\n",
                              (int)_gwArpFailCount,
                              _gwProbeEverSucceeded ? "confirmed" : "unknown");
                LOG_WARNINGF("NetworkManager: Gateway ping unanswered (%d, baseline=%s, pending STA-only recovery)",
                             (int)_gwArpFailCount,
                             _gwProbeEverSucceeded ? "confirmed" : "unknown");
            }
        }
    }

    // 处理连接超时。STA 已关联但 DHCP/网关尚未完成时，20 秒默认值在
    // 拥挤的多 AP/mesh 网络上会把“已连上、正在取地址”误判为失败。
    // 给开机的非阻塞连接至少 45 秒窗口；仍尊重用户配置的更长超时。
    const uint32_t bootConnectTimeout =
        wifiConfig.connectTimeout < 45000UL ? 45000UL : wifiConfig.connectTimeout;
    if (connecting && (currentTime - connectingStartTime >= bootConnectTimeout)) {
        Serial.printf("[WiFi] Connection timeout after %lums\n",
                      (unsigned long)bootConnectTimeout);
        LOG_WARNING("NetworkManager: Connection timeout");
        connecting = false;

#if FASTBEE_WEB_START_EARLY
        // 开机重试：单次超时不立即回退 AP。同名 SSID 多 AP 环境下首次关联常
        // AUTH_EXPIRE（reason=2），重置驱动后重新全信道扫描择优 AP 即可成功；
        // 旧逻辑一次失败即回退 AP，之后探测退避过长，设备可长期卡在 AP。
        if (_bootConnectRetries < BOOT_STA_MAX_RETRIES) {
            _bootConnectRetries++;
            Serial.printf("[NET] STA boot retry %d/%d: resetting WiFi driver and reconnecting...\n",
                          (int)_bootConnectRetries, (int)BOOT_STA_MAX_RETRIES);
            LOGGER.infof("NetworkManager: STA boot timeout, retry %d/%d",
                         (int)_bootConnectRetries, (int)BOOT_STA_MAX_RETRIES);
            // 完整重置 WiFi 驱动、稳定等待和候选扫描均由协作式状态机执行，
            // 当前 update 立即返回，避免阻塞 AsyncTCP/Web 请求。
            startBootStaRetryScan();
            return;
        } else {
            _bootConnectRetries = 0;  // 清零计数，供下轮连接周期重新计次
            if (!wifiConfig.autoFailover && !wifiConfig.staSSID.isEmpty()) {
                // 有效 STA 凭据但网络暂时不可达时，不要把设备切到 AP：这会
                // 让局域网地址消失并中断用户当前网络。保持 STA，下一轮由
                // 指数退避重连；首次无凭据配网仍走显式 AP 分支。
                LOG_WARNING("NetworkManager: STA retries exhausted; AP fallback disabled, continuing STA-only recovery");
                wifiManager->setModeTransitioning(false);
                wifiConfig.mode = NetworkMode::NETWORK_STA;
                autoReconnectEnabled = true;
                statusInfo.status = NetworkStatus::DISCONNECTED;
                _staDisconnectedSince = currentTime;
                lastReconnectAttempt = currentTime;
                WiFi.disconnect(false);
                return;
            }
            // 非阻塞模式：重试全部失败后回退到 AP 模式，供用户通过 Web 重新配置
            LOG_WARNING("NetworkManager: STA timeout (non-blocking), falling back to AP mode");
            Serial.println("[NET] STA timeout, switching to AP mode...");
            ets_printf("[NET] STA timeout, switching to AP mode...\n");
            wifiManager->setModeTransitioning(false);
            if (startAPMode()) {
                Serial.printf("[NET] AP started: %s  IP: %s\n",
                    wifiConfig.apSSID.c_str(),
                    WiFi.softAPIP().toString().c_str());
                ets_printf("[NET] AP started: %s  IP: %s\n",
                    wifiConfig.apSSID.c_str(),
                    WiFi.softAPIP().toString().c_str());
                LOGGER.infof(">>> AP fallback: AP IP=%s  SSID=[%s] pwd=*** (len=%d) <<<",
                    WiFi.softAPIP().toString().c_str(),
                    wifiConfig.apSSID.c_str(),
                    wifiConfig.apPassword.length());
                wifiConfig.mode = NetworkMode::NETWORK_AP;
                _apFallbackAuto = true;
                // Give the access point and upstream mesh a cooldown window
                // before probing again.  Starting a probe in this same update
                // cycle creates an authentication burst after four failed STA
                // attempts and can trigger router rate limiting (reason=2/15).
                _lastApProbeTime = currentTime;
                _apProbeFailCount = 0;
            } else {
                Serial.println("[NET] AP start FAILED!");
                statusInfo.status = NetworkStatus::CONNECTION_FAILED;
                triggerEvent(NetworkStatus::CONNECTION_FAILED, "Connection timeout");
            }
        }
#else
        // 连接超时处理（纯 AP 模式下不会有 AP+STA 分支）
        LOG_WARNING("NetworkManager: Connection timeout");
        if (wifiConfig.autoFailover) {
            LOG_INFO("NetworkManager: Attempting failover...");
            ipManager->performFailover();
        } else {
            statusInfo.status = NetworkStatus::CONNECTION_FAILED;
            triggerEvent(NetworkStatus::CONNECTION_FAILED, "Connection timeout");
        }
#endif
    }

    // STA 断开跟踪 + 自动重连 + 长时间失联看门狗（仅 STA 模式、未连接、非正在连接时）
    if (!connecting &&
        !isConnected &&
        wifiConfig.mode == NetworkMode::NETWORK_STA) {
        // 记录 STA 持续断开的起点（用于长时间失联后重启回落 AP）。
        // 与自动重连是否启用无关：即使重连已达上限停用，也要继续计时以触发看门狗。
        if (_staDisconnectedSince == 0) {
            _staDisconnectedSince = currentTime;
        }

        // 自动重连逻辑 —— 指数退避策略：base * 2^(attempts-1), 封顶 RECONNECT_MAX_INTERVAL_MS (60s)
        if (autoReconnectEnabled) {
            uint8_t expShift = min((uint8_t)(statusInfo.reconnectAttempts), (uint8_t)4);
            unsigned long backoffInterval = min(
                (unsigned long)(wifiConfig.reconnectInterval * (1UL << expShift)),
                WiFiConfig::RECONNECT_MAX_INTERVAL_MS
            );
            if (!wifiConfig.autoFailover) {
                backoffInterval = max(backoffInterval, STA_ONLY_RETRY_INTERVAL_MS);
            }
            if (currentTime - lastReconnectAttempt >= backoffInterval) {
                attemptReconnect();
            }
        }

        // 运行时看门狗：STA 已持续断开超过阈值（5 分钟）仍未重连成功。
        // 由于运行中掉线不会触发开机 AP 回退，设备会永远卡在无 IP 状态
        // （mDNS 失效、局域网不可达且无法自恢复）。此处调度一次干净重启，
        // 重启后开机流程 STA 再失败即回退 AP，恢复 192.168.4.1 可达。
        // 不受 autoReconnectEnabled 门控：即使自动重连已达上限停用，也照常触发，
        // 避免设备永久卡死在无 IP 的 STA 状态。
        if (_staDisconnectedSince != 0 &&
            wifiConfig.autoFailover &&
            (currentTime - _staDisconnectedSince) >= STA_LOST_REBOOT_MS &&
            !SystemRebooter::isScheduled()) {
            LOG_WARNING("NetworkManager: STA disconnected too long, scheduling reboot for AP fallback");
            SystemRebooter::scheduleReboot(
                "STA lost too long, reboot for AP fallback",
                1000UL,
                RestartReason::AP_FALLBACK);
        }
    }

    // AP 模式下定期探测 WiFi 是否恢复，成功则自动切回 STA 模式
    // 仅在 STA 失败自动回退到 AP 时才启用探测（_apFallbackAuto=true）
    // 用户主动设置的 AP 模式不触发探测，保持 AP 热点稳定运行
    if (wifiConfig.mode == NetworkMode::NETWORK_AP &&
        _apFallbackAuto &&
        !_apProbeInProgress &&
        !wifiConfig.staSSID.isEmpty()) {
        // 防震荡退避：连续探测失败超过阈值后，探测间隔从 2min 增加到 5min
        unsigned long probeInterval = (_apProbeFailCount >= AP_PROBE_BACKOFF_THRESHOLD)
            ? AP_PROBE_BACKOFF_MS : AP_PROBE_INTERVAL_MS;
        if (currentTime - _lastApProbeTime >= probeInterval) {
            // 客户端门控：探测需临时停掉 AP 切 STA，若有客户端已连上 AP
            // （用户正在配网/访问 Web），停 AP 会切断进行中的 TCP 连接——
            // 这是 "AP 模式 Web 加载慢/不完整" 反复出现的直接原因，且传输
            // 期间的模式切换还可能触发 Interrupt Watchdog 死锁复位。
            // 有客户端时不计探测失败，顺延 AP_PROBE_DEFER_MS 后重试。
            uint8_t apStations = WiFi.softAPgetStationNum();
            if (apStations > 0) {
                _lastApProbeTime = currentTime - probeInterval + AP_PROBE_DEFER_MS;
                LOGGER.infof("[WiFi-AP] Probe deferred: %d client(s) connected to AP",
                             (int)apStations);
            } else {
                _apProbeInProgress = true;
                _lastApProbeTime = currentTime;
                _apProbeState = ApProbeState::IDLE;
                handleApModeWifiProbe();
            }
        }
    }
    
    // mDNS 健康检查降频门控：MDNS.begin() 内含同步域名冲突检测（queryService）
    // 实测阻塞 ~3s，期间 network_update 高优先级任务独占 CPU，AsyncTCP 事件循环冻结，
    // Web 前台活动时跳过；空闲时降至 5min 一次（同名收敛属慢过程，足够）
    static unsigned long lastMdnsHealthMs = 0;
    if (currentTime - lastMdnsUpdate >= 30000) {
        if (shouldRunMDNS()) {
            // Web 前台门控：避免阻塞 AsyncTCP 事件循环
            auto* fwMdns = FastBeeFramework::getInstance();
            auto* webGate = fwMdns ? fwMdns->getWebConfigManager() : nullptr;
            bool inForeground = webGate && webGate->isForegroundRequestActive();
            
            // 降频判定：仅每 5min 执行一次健康检查（首次除外）
            unsigned long nowMs = millis();
            bool shouldSkipRateLimit = (lastMdnsHealthMs != 0 && 
                                        nowMs - lastMdnsHealthMs < 300000UL);
            
            if (!inForeground && !shouldSkipRateLimit) {
                dnsManager->checkMDNSHealth();
                lastMdnsUpdate = currentTime;
                lastMdnsHealthMs = nowMs;  // 标记已执行健康检查
            }
        } else {
            lastMdnsUpdate = currentTime;
        }
    }

    // WiFi 僵尸连接检测（每 60 秒）。这里只把本机 UDP 数据无法交给 lwIP
    // 作为故障依据，不依赖网关是否回复 ICMP/DNS。
    static unsigned long lastZombieCheck = 0;
    static uint8_t zombieFailCount = 0;

    if (!isConnected) {
        zombieFailCount = 0;
    }

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
            wasConnected = false;               // 标记断开，供状态跳变检测
            statusInfo.reconnectAttempts = 0;   // 以最短退避间隔重连
            lastReconnectAttempt = 0;           // 允许下一次 update() 立即发起重连
            WiFi.disconnect(false);             // 断开僵尸 STA 连接
            // 不在此阻塞等待/重连：交给 update() 的非阻塞指数退避重连分支（L755-769）
            // 处理，避免在 loopTask 上阻塞 200ms+
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
        JsonDocument doc;
        if (!ConfigStorage::getInstance().loadJSONConfig(path, doc)) {
            LOGGER.errorf("NetworkManager: Primary and backup config invalid (%s)", path);
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
    if (LittleFS.exists(NETWORK_CONFIG_FILE) ||
        LittleFS.exists(String(NETWORK_CONFIG_FILE) + ".bak")) {
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

bool FBNetworkManager::startAPMode() {
    // mDNS owns sockets bound to the current Wi-Fi netif.  A direct STA -> AP
    // mode switch while the mDNS service task is still unwinding can leave it
    // dereferencing the old netif (classic ESP32 reports LoadProhibited from
    // esp_netif_is_netif_up()).  Always tear mDNS down before changing the
    // driver mode, then start it again after the AP netif is ready.
    if (dnsManager && dnsManager->isMDNSStarted()) {
        dnsManager->stopMDNS();
        delay(200);
    }

    if (!wifiManager->startAPMode()) {
        return false;
    }

    // 启动mDNS服务（使用customDomain作为hostname）
    if (shouldRunMDNS()) {
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

void FBNetworkManager::resetGatewayWatchdog() {
    _gwArpFailCount = 0;
    _gwProbeEverSucceeded = false;
    _gwProbeResult = false;
    _gwProbeState = 0;
    _lastGwProbeTime = millis();  // 重新连接后从完整间隔开始计时
}

// esp_ping 回调：收到任一回包即证明数据面双向可达
void FBNetworkManager::gwPingOnSuccess(void* handle, void* args) {
    auto* self = static_cast<FBNetworkManager*>(args);
    if (self) self->_gwProbeResult = true;
}

void FBNetworkManager::gwPingOnEnd(void* handle, void* args) {
    auto* self = static_cast<FBNetworkManager*>(args);
    if (self) {
        self->_gwProbeState = 2;  // 结果待下轮消费
        self->_gwPingHandle = nullptr;
    }
    // 会话一次性使用，结束后释放（官方示例即在 on_ping_end 中删除）
    esp_ping_stop(static_cast<esp_ping_handle_t>(handle));
    esp_ping_delete_session(static_cast<esp_ping_handle_t>(handle));
}

// 探测网关数据面可达性（异步状态机，单轮开销 <1ms，不阻塞任务循环）。
// 历史教训：① 同步轮询 ARP 缓存最多 1.5s，网关瞬时拥塞时累计阻塞 ~3s 拖慢 Web；
// ② 纯 ARP 缓存判定在入站流量（mDNS/广播）维持缓存条目时误判可达，
//    数据面实际已死也永不触发自愈。现改为对网关发单次 ICMP echo 并要求真实回包。
bool FBNetworkManager::probeGatewayReachable() {
    // 上一轮探测进行中：未超时前不计失败
    if (_gwProbeState == 1) {
        if (millis() - _gwProbeStart < GW_PROBE_TIMEOUT_MS) {
            return true;
        }
        _gwProbeState = 0;  // 兑底超时（ping 会话自身会在 2s 内结束并进入 state 2）
        return false;
    }

    // 上一轮已有结果：消费并返回
    if (_gwProbeState == 2) {
        _gwProbeState = 0;
        return _gwProbeResult;
    }

    // 空闲：发起新的异步 ping
    IPAddress gw = WiFi.gatewayIP();
    if (gw == IPAddress(0, 0, 0, 0)) {
        return true;  // 网关未知（DHCP 未完成），不判定为故障
    }

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = 1;
    cfg.interval_ms = 0;
    cfg.timeout_ms = 2000;
    cfg.target_addr.type = ESP_IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = static_cast<uint32_t>(gw);

    esp_ping_callbacks_t cbs = {};
    cbs.on_ping_success = &FBNetworkManager::gwPingOnSuccess;
    cbs.on_ping_timeout = nullptr;
    cbs.on_ping_end = &FBNetworkManager::gwPingOnEnd;
    cbs.cb_args = this;

    esp_ping_handle_t handle = nullptr;
    if (esp_ping_new_session(&cfg, &cbs, &handle) != ESP_OK || !handle) {
        return true;  // 无法发起探测时 fail-open，避免误触发重连
    }
    _gwProbeResult = false;
    _gwPingHandle = handle;
    _gwProbeState = 1;
    _gwProbeStart = millis();
    esp_ping_start(handle);
    return true;  // 本轮只负责发起，结果由下一轮判定
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
    // Historical name retained for the private initialization hook. It now
    // only prepares the STA configuration and starts the asynchronous boot
    // candidate state machine; association failures are handled by update().
    if (wifiConfig.staSSID.isEmpty()) {
        LOG_INFO("NetworkManager: No STA SSID configured, skipping STA connection");
        return false;
    }

    // ── DRAM 内存保护：WiFi.begin() 需要约 12-16KB DRAM ──────────────────
    {
        uint32_t dramFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (dramFree < WIFI_CONNECT_MIN_DRAM) {
            Serial.printf("[NET] connectToWiFiBlocking: DRAM too low (dram=%lu < %lu), skipping\n",
                          (unsigned long)dramFree, (unsigned long)WIFI_CONNECT_MIN_DRAM);
            LOG_WARNINGF("[NET] DRAM too low for WiFi.begin (dram=%lu need=%lu)",
                         (unsigned long)dramFree, (unsigned long)WIFI_CONNECT_MIN_DRAM);
            return false;
        }
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

    // ── STA 连接可靠性增强 ──────────────────────────────────────────────
    // 家用路由器/CMCC 常出现「同名 SSID 多 AP」（信号放大器、双频合一）。
    // 候选扫描和驱动切换由 startBootStaRetryScan()/handleBootStaRetryScan()
    // 协作执行；这里仅准备连接策略，绝不在 initialize() 或 network_update
    // 中同步扫描、轮询 WiFi.status() 或等待退避。
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(false);

    // All compile-time Web start modes use the same asynchronous initial scan.
    // The legacy method name is kept because it is private and covered by
    // existing diagnostics/tests, but it no longer waits for association.
    _bootConnectRetries = 0;
    connecting = false;
    connectingStartTime = 0;
    statusInfo.status = NetworkStatus::CONNECTING;
    startBootStaRetryScan();
    triggerEvent(NetworkStatus::CONNECTING, "STA scanning candidates (async)");
    Serial.printf("[NET] STA candidate scan initiated (async), polling in update()\n");
    LOG_INFO("NetworkManager: STA candidate scan initiated (async)");
    return true;
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
    if (!dnsManager || !shouldRunMDNS()) return false;
    dnsManager->setMDNSEnabled(true);
    return dnsManager->startMDNS(wifiConfig.customDomain);
}

void FBNetworkManager::stopMDNS() {
    if (dnsManager) dnsManager->stopMDNS();
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

    // ========== WiFi 联网方式：同步 WiFiManager 的实时状态 ==========
    // FBNetworkManager owns retry/backoff/AP-fallback.  WiFiManager keeps a
    // separate transport snapshot whose reconnectAttempts is not advanced by
    // this state machine, so copying it wholesale would reset the retry budget
    // whenever a Web/API status request calls updateStatusInfo().
    const uint16_t ownedReconnectAttempts = statusInfo.reconnectAttempts;
    statusInfo = wifiManager->getStatusInfo();
    statusInfo.reconnectAttempts = ownedReconnectAttempts;
}

// updateIPConflictStatus 方法已移至 IPManager 类

// IP 冲突检测和故障转移方法已移至 IPManager 类

void FBNetworkManager::attemptReconnect() {
    // 重连次数限制：达到最大次数后回退到纯 AP 模式供用户配置
    if (statusInfo.reconnectAttempts >= wifiConfig.maxReconnectAttempts) {
        if (!wifiConfig.autoFailover && !wifiConfig.staSSID.isEmpty()) {
            LOG_WARNING("NetworkManager: STA reconnect budget exhausted; AP fallback disabled, continuing STA-only recovery");
            statusInfo.reconnectAttempts = 0;
            autoReconnectEnabled = true;
            statusInfo.status = NetworkStatus::DISCONNECTED;
            lastReconnectAttempt = millis();
            WiFi.disconnect(false);
            return;
        }
        LOG_WARNING("NetworkManager: Max reconnect attempts reached; switching to AP mode");
        
        // 断开当前 STA 连接
        wifiManager->disconnectWiFi();
        
        // 启动纯 AP 模式（不使用 AP+STA，避免模式切换导致栈溢出）
        if (startAPMode()) {
            // 仅在 AP 已成功启动后标记为自动回退，避免 startAPMode()
            // 失败时误启用 AP 探测恢复状态机。
            _apFallbackAuto = true;
            LOGGER.infof(">>> AP fallback: AP IP=%s  SSID=[%s] pwd=*** (len=%d) <<<",
                WiFi.softAPIP().toString().c_str(),
                wifiConfig.apSSID.c_str(),
                wifiConfig.apPassword.length());
            Serial.println("[WiFi] AP fallback active - STA reconnect disabled until user reconfigures");
            LOG_INFO("NetworkManager: AP fallback active, STA reconnect paused");
            
            // 切换到 AP 模式，停止自动重连
            wifiConfig.mode = NetworkMode::NETWORK_AP;
            autoReconnectEnabled = false;
            // Do not immediately switch AP->STA after the reconnect budget is
            // exhausted.  A short cooldown avoids a second authentication
            // burst while the router is still clearing the failed handshake.
            _lastApProbeTime = millis();
            _apProbeFailCount = 0;
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

void FBNetworkManager::resetApModeWifiProbe() {
    _apProbeInProgress = false;
    _apProbeState = ApProbeState::IDLE;
    _apProbeCandidateCount = 0;
    _apProbeAttempt = 0;
    _apProbeStateSince = 0;
    _apProbeStartedAt = 0;
    _apProbeAttemptStartedAt = 0;
}

void FBNetworkManager::handleApModeWifiProbe() {
    // Cooperative AP -> STA recovery state machine. Async scan and connection
    // waits always return to network_update; this function never polls or
    // sleeps while waiting for driver progress.
    const unsigned long now = millis();

    if (_apProbeState == ApProbeState::IDLE) {
        const uint32_t dramFree = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        if (dramFree < WIFI_CONNECT_MIN_DRAM) {
            Serial.printf("[WiFi-AP] Probe skipped: DRAM too low (free8=%lu < %lu)\n",
                          (unsigned long)dramFree, (unsigned long)WIFI_CONNECT_MIN_DRAM);
            resetApModeWifiProbe();
            return;
        }
        if (WiFi.softAPgetStationNum() > 0) {
            Serial.println("[WiFi-AP] Probe deferred: AP client connected");
            const unsigned long interval =
                (_apProbeFailCount >= AP_PROBE_BACKOFF_THRESHOLD)
                    ? AP_PROBE_BACKOFF_MS : AP_PROBE_INTERVAL_MS;
            _lastApProbeTime = now - interval + AP_PROBE_DEFER_MS;
            resetApModeWifiProbe();
            return;
        }

        _apProbeCandidateCount = 0;
        _apProbeAttempt = 0;
        _apProbeStateSince = now;
        const int16_t scanStart = WiFi.scanNetworks(true, true);
        if (scanStart == WIFI_SCAN_FAILED) {
            WiFi.scanDelete();
            WiFi.enableSTA(false);
            _apProbeFailCount++;
            Serial.printf("[WiFi-AP] Async pre-scan failed (#%d), keeping AP\n",
                          _apProbeFailCount);
            resetApModeWifiProbe();
            return;
        }
        _apProbeState = ApProbeState::SCANNING;
        Serial.printf("[WiFi-AP] Async pre-scan started for SSID '%s'\n",
                      wifiConfig.staSSID.c_str());
        return;
    }

    if (_apProbeState == ApProbeState::SCANNING) {
        int16_t scanCount = WiFi.scanComplete();
        if (scanCount == WIFI_SCAN_RUNNING &&
            (now - _apProbeStateSince) < AP_PROBE_SCAN_TIMEOUT_MS) {
            return;
        }
        if (scanCount < 0) {
            WiFi.scanDelete();
            WiFi.enableSTA(false);  // async scan temporarily enabled APSTA
            _apProbeFailCount++;
            Serial.printf("[WiFi-AP] Async pre-scan timed out/failed (#%d), keeping AP\n",
                          _apProbeFailCount);
            resetApModeWifiProbe();
            return;
        }

        bool ssidVisible = false;
        for (int16_t i = 0; i < scanCount; i++) {
            if (WiFi.SSID(i) != wifiConfig.staSSID) continue;
            ssidVisible = true;
            ApProbeCandidate current;
            memcpy(current.bssid, WiFi.BSSID(i), sizeof(current.bssid));
            current.channel = WiFi.channel(i);
            current.rssi = WiFi.RSSI(i);

            bool duplicate = false;
            for (uint8_t j = 0; j < _apProbeCandidateCount; j++) {
                if (memcmp(_apProbeCandidates[j].bssid,
                           current.bssid,
                           sizeof(current.bssid)) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            if (_apProbeCandidateCount < 2) {
                _apProbeCandidates[_apProbeCandidateCount++] = current;
            } else if (current.rssi > _apProbeCandidates[1].rssi) {
                _apProbeCandidates[1] = current;
            }
            if (_apProbeCandidateCount == 2 &&
                _apProbeCandidates[0].rssi < _apProbeCandidates[1].rssi) {
                ApProbeCandidate tmp = _apProbeCandidates[0];
                _apProbeCandidates[0] = _apProbeCandidates[1];
                _apProbeCandidates[1] = tmp;
            }
        }
        WiFi.scanDelete();
        WiFi.enableSTA(false);  // restore pure AP after async APSTA scan
        Serial.printf("[WiFi-AP] Pre-scan: %d networks, SSID '%s' %s (%u candidate%s)\n",
                      (int)scanCount,
                      wifiConfig.staSSID.c_str(),
                      ssidVisible ? "visible" : "NOT visible (fast-fail, keep AP)",
                      (unsigned)_apProbeCandidateCount,
                      _apProbeCandidateCount > 1 ? "s" : "");

        if (!ssidVisible) {
            _apProbeFailCount++;
            resetApModeWifiProbe();
            return;
        }
        if (WiFi.softAPgetStationNum() > 0) {
            const unsigned long interval =
                (_apProbeFailCount >= AP_PROBE_BACKOFF_THRESHOLD)
                    ? AP_PROBE_BACKOFF_MS : AP_PROBE_INTERVAL_MS;
            _lastApProbeTime = now - interval + AP_PROBE_DEFER_MS;
            Serial.println("[WiFi-AP] Probe deferred: AP client joined during scan");
            resetApModeWifiProbe();
            return;
        }

        LOGGER.infof("[WiFi-AP] Probing WiFi [%s] (async STA switch)...",
                     wifiConfig.staSSID.c_str());
        if (dnsManager && dnsManager->isMDNSStarted()) {
            dnsManager->stopMDNS();
        }
        _apProbeState = ApProbeState::MDNS_SETTLE;
        _apProbeStateSince = now;
        return;
    }

    if (_apProbeState == ApProbeState::MDNS_SETTLE) {
        if ((now - _apProbeStateSince) < AP_PROBE_MDNS_SETTLE_MS) return;
        // Recheck immediately before taking AP down; a client may have joined
        // while mDNS was settling.
        if (WiFi.softAPgetStationNum() > 0) {
            if (shouldRunMDNS() && dnsManager) {
                dnsManager->startMDNS(wifiConfig.customDomain);
            }
            const unsigned long interval =
                (_apProbeFailCount >= AP_PROBE_BACKOFF_THRESHOLD)
                    ? AP_PROBE_BACKOFF_MS : AP_PROBE_INTERVAL_MS;
            _lastApProbeTime = now - interval + AP_PROBE_DEFER_MS;
            Serial.println("[WiFi-AP] Probe deferred: AP client joined before switch");
            resetApModeWifiProbe();
            return;
        }
        wifiManager->stopAPMode();
        WiFi.mode(WIFI_MODE_NULL);
        _apProbeStartedAt = now;
        _apProbeState = ApProbeState::DRIVER_NULL_SETTLE;
        _apProbeStateSince = now;
        return;
    }

    if (_apProbeState == ApProbeState::DRIVER_NULL_SETTLE) {
        if ((now - _apProbeStateSince) < AP_PROBE_DRIVER_SETTLE_MS) return;
        WiFi.mode(WIFI_MODE_STA);
        WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
        WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(false);
        wifiManager->setNetworkConfig(wifiConfig);
        _apProbeState = ApProbeState::STA_SETTLE;
        _apProbeStateSince = now;
        return;
    }

    if (_apProbeState == ApProbeState::STA_SETTLE) {
        if ((now - _apProbeStateSince) < AP_PROBE_STA_SETTLE_MS) return;
        // Mesh/中继网络可能使用动态的本地管理 BSSID。第一次尝试必须让驱动
        // 自动选择，避免把扫描瞬间得到的弱/过期节点固定下来；自动选择失败
        // 后才轮换扫描到的候选，作为有界的兜底策略。
        const bool usePinnedCandidate =
            _apProbeAttempt > 0 && _apProbeCandidateCount > 0;
        if (usePinnedCandidate) {
            const uint8_t selected =
                static_cast<uint8_t>((_apProbeAttempt - 1) % _apProbeCandidateCount);
            const ApProbeCandidate& ap = _apProbeCandidates[selected];
            WiFi.begin(wifiConfig.staSSID.c_str(), wifiConfig.staPassword.c_str(),
                       ap.channel, ap.bssid);
            Serial.printf("[WiFi-AP] Probe try %u pinned to BSSID %02X:%02X:%02X:%02X:%02X:%02X ch%ld rssi=%ld\n",
                          (unsigned)(_apProbeAttempt + 1),
                          ap.bssid[0], ap.bssid[1], ap.bssid[2],
                          ap.bssid[3], ap.bssid[4], ap.bssid[5],
                          (long)ap.channel, (long)ap.rssi);
        } else {
            Serial.printf("[WiFi-AP] Probe try %u using automatic AP selection\n",
                          (unsigned)(_apProbeAttempt + 1));
            WiFi.begin(wifiConfig.staSSID.c_str(), wifiConfig.staPassword.c_str());
        }
        _apProbeAttemptStartedAt = now;
        _apProbeState = ApProbeState::WAITING_STA;
        return;
    }

    if (_apProbeState == ApProbeState::WAITING_STA) {
        if (WiFi.status() == WL_CONNECTED &&
            WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
            Serial.printf("[WiFi-AP] WiFi recovered! IP: %s, switching back to STA mode\n",
                          WiFi.localIP().toString().c_str());
            LOGGER.infof("[WiFi-AP] WiFi recovered! IP: %s",
                         WiFi.localIP().toString().c_str());
            wifiConfig.mode = NetworkMode::NETWORK_STA;
            autoReconnectEnabled = true;
            connecting = false;
            statusInfo.status = NetworkStatus::CONNECTED;
            statusInfo.ipAddress = WiFi.localIP().toString();
            statusInfo.ssid = WiFi.SSID();
            statusInfo.reconnectAttempts = 0;
            statusInfo.lastConnectionTime = now;
            _staDisconnectedSince = 0;
            _apFallbackAuto = false;
            _apProbeFailCount = 0;
            if (shouldRunMDNS() && dnsManager) {
                dnsManager->startMDNS(wifiConfig.customDomain);
            }
#if FASTBEE_ENABLE_MQTT
            {
                auto* fw = FastBeeFramework::getInstance();
                auto* pm = fw ? fw->getProtocolManager() : nullptr;
                MQTTClient* mqtt = pm ? pm->getMQTTClient() : nullptr;
                if (mqtt) {
                    mqtt->resetErrorCounters();
                    LOG_INFO("[WiFi-AP] MQTT reconnect triggered after WiFi recovery from AP mode");
                }
            }
#endif
            resetApModeWifiProbe();
            return;
        }

        const uint8_t maxAttempts = _apProbeCandidateCount > 1 ? 2 : 1;
        const unsigned long perAttemptMs = AP_PROBE_TIMEOUT_MS / maxAttempts;
        const bool totalExpired = (now - _apProbeStartedAt) >= AP_PROBE_TIMEOUT_MS;
        const bool attemptExpired =
            (now - _apProbeAttemptStartedAt) >= perAttemptMs;
        if (!totalExpired && !attemptExpired) return;

        if (!totalExpired && (_apProbeAttempt + 1) < maxAttempts) {
            _apProbeAttempt++;
            WiFi.mode(WIFI_MODE_NULL);
            _apProbeState = ApProbeState::DRIVER_NULL_SETTLE;
            _apProbeStateSince = now;
            return;
        }

        // Association failed. Tear down STA now, then restore AP on a later
        // state-machine tick so the driver gets a settle window without
        // blocking network_update.
        WiFi.disconnect(true, true);
        _apProbeState = ApProbeState::RESTORE_AP;
        _apProbeStateSince = now;
        return;
    }

    if (_apProbeState == ApProbeState::RESTORE_AP) {
        if ((now - _apProbeStateSince) < AP_PROBE_AP_SETTLE_MS) return;
        if (startAPMode()) {
            if (shouldRunMDNS() && dnsManager) {
                dnsManager->startMDNS(wifiConfig.customDomain);
            }
            LOGGER.infof("[WiFi-AP] Probe failed, AP restored: %s IP=%s",
                         wifiConfig.apSSID.c_str(),
                         WiFi.softAPIP().toString().c_str());
        } else {
            LOG_ERROR("[WiFi-AP] AP restart failed after probe, activating last-resort AP");
            ensureLastResortAP();
        }
        _apProbeFailCount++;
        if (_apProbeFailCount == AP_PROBE_BACKOFF_THRESHOLD) {
            LOGGER.infof("[WiFi-AP] %d consecutive probe failures, backing off to %lumin",
                         _apProbeFailCount,
                         (unsigned long)(AP_PROBE_BACKOFF_MS / 60000));
        }
        Serial.printf("[WiFi-AP] Probe failed (#%d), AP restored, next probe in %lumin\n",
                      _apProbeFailCount,
                      (unsigned long)((_apProbeFailCount >= AP_PROBE_BACKOFF_THRESHOLD
                          ? AP_PROBE_BACKOFF_MS : AP_PROBE_INTERVAL_MS) / 60000));
        resetApModeWifiProbe();
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

bool FBNetworkManager::shouldRunMDNS() const {
    return wifiConfig.enableMDNS &&
           FastBee::RuntimeFeatureManager::instance().shouldEnable("mdns");
}

NetworkStatusInfo FBNetworkManager::getStatusInfo() const {
    return statusInfo;
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
        if (shouldRunMDNS()) {
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
                
                if (shouldRunMDNS()) {
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
                if (shouldRunMDNS() && dnsManager) {
                    dnsManager->startMDNS(wifiConfig.customDomain);
                }
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
                
                if (shouldRunMDNS()) {
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
                if (shouldRunMDNS() && dnsManager) {
                    dnsManager->startMDNS(wifiConfig.customDomain);
                }
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
    WiFiConfig effectiveConfig = newConfig;
    normalizeUnsupportedNetworkType(effectiveConfig);
    auto& runtimeFeatures = FastBee::RuntimeFeatureManager::instance();
    const auto previousFeatureState = runtimeFeatures.captureUserConfig();
    const bool previousFeatureMDNS = runtimeFeatures.isUserEnabled("mdns");
    bool mdnsFeatureChanged = false;
    if (effectiveConfig.enableMDNS != previousFeatureMDNS) {
        if (!runtimeFeatures.setUserEnabled("mdns", effectiveConfig.enableMDNS,
                                            &mdnsFeatureChanged)) {
            LOG_WARNING("NetworkManager: mDNS cannot be enabled because it is not compiled into this firmware");
            effectiveConfig.enableMDNS = previousFeatureMDNS;
        }
    }

    const WiFiConfig previousConfig = wifiConfig;
    bool restartRequired = false;
    
    // 检查是否需要重启网络
    if (effectiveConfig.mode != wifiConfig.mode ||
        effectiveConfig.apSSID != wifiConfig.apSSID ||
        effectiveConfig.staSSID != wifiConfig.staSSID ||
        effectiveConfig.ipConfigType != wifiConfig.ipConfigType ||
        effectiveConfig.networkType != wifiConfig.networkType ||
        effectiveConfig.ethernet.spiMosi != wifiConfig.ethernet.spiMosi ||
        effectiveConfig.ethernet.spiMiso != wifiConfig.ethernet.spiMiso ||
        effectiveConfig.ethernet.spiSck != wifiConfig.ethernet.spiSck ||
        effectiveConfig.ethernet.csPin != wifiConfig.ethernet.csPin ||
        effectiveConfig.ethernet.rstPin != wifiConfig.ethernet.rstPin ||
        effectiveConfig.ethernet.intPin != wifiConfig.ethernet.intPin ||
        effectiveConfig.cellular.txPin != wifiConfig.cellular.txPin ||
        effectiveConfig.cellular.rxPin != wifiConfig.cellular.rxPin ||
        effectiveConfig.cellular.pwrPin != wifiConfig.cellular.pwrPin ||
        effectiveConfig.cellular.baudRate != wifiConfig.cellular.baudRate ||
        effectiveConfig.cellular.apn != wifiConfig.cellular.apn) {
        restartRequired = true;
    }
    
    // 检查 mDNS/域名配置变更（不需要重启网络，但需立即重启 mDNS 服务）
    bool mdnsConfigChanged = (effectiveConfig.customDomain != wifiConfig.customDomain ||
                              effectiveConfig.enableMDNS != wifiConfig.enableMDNS);
    
    wifiConfig = effectiveConfig;
    
    // 保存配置到存储
    bool saveSuccess = true;
    if (saveToStorage) {
        // 网络页面保留兼容的 enableMDNS 开关，但用户期望状态统一由
        // features.json 持久化。任一文件保存失败都恢复内存状态，并尽力
        // 把已经落盘的功能配置回滚，避免两个入口长期显示不同状态。
        if (mdnsFeatureChanged && !runtimeFeatures.saveCurrentConfig()) {
            runtimeFeatures.restoreUserConfig(previousFeatureState);
            wifiConfig = previousConfig;
            LOG_ERROR("NetworkManager: Failed to save mDNS feature configuration");
            return false;
        }
        saveSuccess = saveNetworkConfig();
        if (!saveSuccess) {
            wifiConfig = previousConfig;
            if (mdnsFeatureChanged) {
                runtimeFeatures.restoreUserConfig(previousFeatureState);
                if (!runtimeFeatures.saveCurrentConfig()) {
                    LOG_ERROR("NetworkManager: Failed to roll back mDNS feature configuration");
                    // Re-read the authoritative on-disk generation so runtime
                    // state cannot disagree with whatever survived the failure.
                    runtimeFeatures.loadConfig();
                }
            }
            LOG_ERROR("NetworkManager: Failed to save network configuration");
            return false;
        }
        LOG_INFO("NetworkManager: Configuration saved successfully");
    }
    
    // mDNS 配置变更：如果已有设备重启调度，跳过 mDNS 运行时重启（重启后 mDNS 会自动初始化）
    if (mdnsConfigChanged && !restartRequired && dnsManager) {
        dnsManager->setMDNSEnabled(shouldRunMDNS());
        if (SystemRebooter::isScheduled()) {
            LOG_INFO("NetworkManager: mDNS config changed but device reboot already scheduled, skipping mDNS runtime restart");
        } else if (shouldRunMDNS()) {
            LOG_INFOF("NetworkManager: mDNS config changed, scheduling delayed restart with hostname '%s'",
                      wifiConfig.customDomain.c_str());
            pendingMDNSRestart = true;
            pendingMDNSRestartTime = 0;  // 在 update() 中记录时间戳
        } else {
            LOG_INFO("NetworkManager: mDNS disabled by config change, stopping mDNS");
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
    clearPersistentFallback();
    
    LOG_INFO("NetworkManager: Network configuration reset to defaults");
    return restartNetwork();
}

bool FBNetworkManager::clearPersistentFallback() {
    if (!preferences.clear()) {
        LOG_ERROR("NetworkManager: Failed to clear NVS fallback configuration");
        return false;
    }
    return true;
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
