/**
 * @file NetworkManager.h
 * @brief 网络管理器类，负责WiFi连接、AP模式、DNS、mDNS、IP冲突检测等网络功能
 * @author kerwincui
 * @date 2025-12-02
 */

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <WiFi.h>
#include <Preferences.h>
#include <vector>
#include <functional>
#include <core/SystemConstants.h>
#include <ArduinoJson.h>
#include "core/interfaces/INetworkManager.h"
#include "core/interfaces/INetworkAdapter.h"
#include "core/FeatureFlags.h"
#include "network/WiFiManager.h"
#include "network/IPManager.h"
#include "network/DNSManager.h"
#if FASTBEE_ENABLE_ETHERNET
#include "network/EthernetAdapter.h"
#endif
#if FASTBEE_ENABLE_CELLULAR
#include "network/CellularAdapter.h"
#endif

// 事件回调类型
typedef std::function<void(NetworkStatus, const String&)> NetworkEventCallback;
// 网络切换前回调：在销毁网络适配器之前调用，用于停止依赖适配器 Client 的协议（如 MQTT）
typedef std::function<void()> PreNetworkSwitchCallback;

class AsyncWebServer;

/**
 * @class FBNetworkManager
 * @brief 网络管理器类，提供完整的网络管理功能
 */
class FBNetworkManager : public INetworkManager {
public:
    /**
     * @brief 构造函数
     * @param webServerPtr Web服务器指针
     */
    explicit FBNetworkManager(AsyncWebServer* webServerPtr = nullptr);
    
    /**
     * @brief 析构函数
     */
    ~FBNetworkManager();
    
    /**
     * @brief 初始化网络管理器
     * @return 初始化是否成功
     */
    bool initialize();
    
    /**
     * @brief 设置 WiFi凭证（用于测试或硬编码配置）
     * @param ssid WiFi名称
     * @param password WiFi密码
     */
    void setWiFiCredentials(const String& ssid, const String& password);
    
    /**
     * @brief 断开所有网络连接
     */
    void disconnect();
    
    /**
     * @brief 更新网络状态（需要在主循环中调用）
     */
    void update();
    
    /**
     * @brief 获取网络配置
     * @return WiFi配置
     */
    WiFiConfig getConfig() const;
    
    /**
     * @brief 更新网络配置
     * @param newConfig 新配置
     * @param saveToStorage 是否保存到存储
     * @return 更新是否成功
     */
    bool updateConfig(const WiFiConfig& newConfig, bool saveToStorage = true);
    
    /**
     * @brief 清除待处理的网络重启标志
     * 用于配置保存后改为设备重启时，取消运行时 restartNetwork()
     */
    void clearPendingRestart() { pendingRestart = false; }

    /**
     * @brief 获取网络状态信息
     * @return 网络状态信息
     */
    NetworkStatusInfo getStatusInfo() const;
    
    /**
     * @brief 扫描可用WiFi网络
     * @return JSON格式的网络列表
     */
    String scanNetworks();
    
    /**
     * @brief 连接到指定网络
     * @param ssid WiFi名称
     * @param password WiFi密码
     * @return 连接是否成功
     */
    bool connectToNetwork(const String& ssid, const String& password);
    
    /**
     * @brief 断开当前网络连接
     */
    void disconnectNetwork();
    
    /**
     * @brief 重启网络
     * @return 重启是否成功
     */
    bool restartNetwork();
    
    /**
     * @brief 检查互联网连接
     * @return 是否连接到互联网
     */
    bool checkInternetConnection();
    
    /**
     * @brief 设置连接成功回调
     * @param callback 回调函数
     */
    void setConnectionCallback(NetworkEventCallback callback);
    
    /**
     * @brief 设置断开连接回调
     * @param callback 回调函数
     */
    void setDisconnectionCallback(NetworkEventCallback callback);
    
    /**
     * @brief 设置IP冲突回调
     * @param callback 回调函数
     */
    void setIPConflictCallback(NetworkEventCallback callback);
    
    /**
     * @brief 设置网络切换前回调
     * @details 在 restartNetwork() 销毁网络适配器之前调用，
     *          用于停止依赖适配器 Client 指针的协议（如 MQTT），防止 use-after-free
     * @param callback 回调函数
     */
    void setPreNetworkSwitchCallback(PreNetworkSwitchCallback callback) { _preNetworkSwitchCb = callback; }

    /**
     * @brief 设置自动重连
     * @param enabled 是否启用自动重连
     */
    void setAutoReconnect(bool enabled);
    
    /**
     * @brief 获取配置的JSON字符串
     * @return JSON格式的配置
     */
    String getConfigJSON();
    
    /**
     * @brief 从JSON更新配置
     * @param jsonConfig JSON格式的配置
     * @return 更新是否成功
     */
    bool updateConfigFromJSON(const String& jsonConfig);
    
    /**
     * @brief 重置为默认配置
     * @return 重置是否成功
     */
    bool resetToDefaults();
    
    /**
     * @brief 获取网络统计信息
     * @return JSON格式的统计信息
     */
    String getStatistics();
    
    /**
     * @brief 递增发送计数
     */
    void incrementTxCount();
    
    /**
     * @brief 递增接收计数
     */
    void incrementRxCount();
    
    /**
     * @brief 检查IP冲突
     * @return 是否检测到IP冲突
     */
    bool checkIPConflict();
    
    /**
     * @brief 获取WiFi管理器
     * @return WiFi管理器指针
     */
    WiFiManager* getWiFiManager();
    
    /**
     * @brief 获取IP管理器
     * @return IP管理器指针
     */
    IPManager* getIPManager();
    
    /**
     * @brief 获取DNS管理器
     * @return DNS管理器指针
     */
    DNSManager* getDNSManager();

    /**
     * @brief 获取当前活动的 Arduino Client 指针
     * @details 根据当前 networkType 返回对应适配器的 Client
     * @return Client 指针，用于 PubSubClient 等协议层
     */
    Client* getActiveClient();

    /**
     * @brief 获取当前活动的网络适配器接口
     * @details 根据 networkType 返回对应的 INetworkAdapter*，WiFi 返回 nullptr
     * @return 活动适配器指针，WiFi 模式或无适配器时返回 nullptr
     */
    INetworkAdapter* getActiveAdapter();

    /**
     * @brief 获取当前联网方式类型
     */
    NetworkType getNetworkType() const { return wifiConfig.networkType; }

    /**
     * @brief 检查当前网络是否已连接（与 networkType 无关）
     */
    bool isNetworkConnected();

#if FASTBEE_ENABLE_ETHERNET
    EthernetAdapter* getEthernetAdapter() { return ethernetAdapter.get(); }
#endif
#if FASTBEE_ENABLE_CELLULAR
    CellularAdapter* getCellularAdapter() { return cellularAdapter.get(); }
#endif
    
    // 静态工具方法
    static bool isValidIP(const String& ip);
    static bool isValidSubnet(const String& subnet);
    static String getMACAddress();
    static String getChipID();
    static uint8_t rssiToPercentage(int32_t rssi);
    static String getWiFiModeString();
    
    // 内部方法
    bool startAPMode();
    void stopAPMode();
    /**
     * @brief 最后保障：强制以出厂默认配置启动 AP
     * 当所有联网方式（包括正常 AP 回退）都失败时调用，
     * 确保设备始终有一个入口访问和配置 Web 服务。
     * 若连最后保障 AP 也失败，则重启重试（连续3次失败则清除配置）。
     */
    bool ensureLastResortAP();
    bool connectToWiFi();
    /**
     * @brief 启动 STA 连接状态机（历史名称，现已非阻塞）
     * 连接结果和失败重试由 update() 统一处理。
     */
    // Historical name retained for the initialization hook. The implementation
    // starts the asynchronous boot STA scan and returns without waiting.
    bool connectToWiFiBlocking();
    void disconnectWiFi();
    bool configureStaticIP();
    bool configureDHCP();
    bool startMDNS();
    void stopMDNS();
    void updateStatusInfo();

private:
    // 网络组件
    AsyncWebServer* webServer;
    Preferences preferences;
    
    // 配置和状态
    WiFiConfig wifiConfig;
    NetworkStatusInfo statusInfo;
    
    // 子模块
    std::unique_ptr<WiFiManager> wifiManager;
    std::unique_ptr<IPManager> ipManager;
    std::unique_ptr<DNSManager> dnsManager;
#if FASTBEE_ENABLE_ETHERNET
    std::unique_ptr<EthernetAdapter> ethernetAdapter;
#endif
#if FASTBEE_ENABLE_CELLULAR
    std::unique_ptr<CellularAdapter> cellularAdapter;
#endif
    
    // 连接状态
    bool isInitialized;
    bool autoReconnectEnabled;
    bool connecting;              // 是否正在连接
    bool pendingRestart;          // 是否有待处理的网络重启
    unsigned long connectingStartTime; // 连接开始时间
    unsigned long pendingRestartTime;  // 延迟重启时间
    bool pendingMDNSRestart;      // 是否有待处理的 mDNS 重启
    unsigned long pendingMDNSRestartTime; // mDNS 延迟重启时间
    
    // 以太网自动重连
#if FASTBEE_ENABLE_ETHERNET
    bool ethReconnectPending;         // 以太网重连待执行
    unsigned long ethReconnectTime;   // 以太网重连计划时间
    int ethReconnectAttempts;         // 以太网重连尝试次数
    static constexpr unsigned long ETH_RECONNECT_INTERVAL_MS = 10000;  // 重连间隔
    static constexpr int ETH_MAX_RECONNECT_ATTEMPTS = 10;             // 最大重连次数
#endif

#if FASTBEE_ENABLE_CELLULAR
    bool cellReconnectPending;         // 4G 重连待执行
    unsigned long cellReconnectTime;   // 4G 重连计划时间
    int cellReconnectAttempts;         // 4G 重连尝试次数
    static constexpr unsigned long CELL_RECONNECT_INTERVAL_MS = 15000; // 4G 注册较慢，间隔略长
    static constexpr int CELL_MAX_RECONNECT_ATTEMPTS = 8;
    static constexpr int CELL_FULL_RESTART_EVERY = 3;                 // 每 3 次重建适配器，释放串口/Modem 状态
#endif
    
    // 时间相关
    unsigned long lastReconnectAttempt;
    unsigned long lastStatusUpdate;
    unsigned long lastConflictCheck;
    
    // 回调函数
    NetworkEventCallback connectionCallback;
    NetworkEventCallback disconnectionCallback;
    NetworkEventCallback ipConflictCallback;
    PreNetworkSwitchCallback _preNetworkSwitchCb;  // 网络切换前回调（停止 MQTT 等依赖适配器的协议）
    
    // 私有方法
    bool initializeFileSystem();
    bool loadNetworkConfig();
    bool saveNetworkConfig();
    void syncIPManagerConfig();
    void attemptReconnect();
    void triggerEvent(NetworkStatus status, const String& message);

    // AP 模式下定期探测 WiFi 恢复，成功则自动切回 STA 模式
    // 仅在 STA 失败自动回退到 AP 时启用（_apFallbackAuto=true），
    // 用户主动设置的 AP 模式不会触发探测。
    // Non-blocking AP recovery probe. Each update() call advances at most one
    // scan/driver/association state; no polling loop runs in network_update.
    void handleApModeWifiProbe();
    void resetApModeWifiProbe();
    enum class ApProbeState : uint8_t {
        IDLE,
        SCANNING,
        MDNS_SETTLE,
        DRIVER_NULL_SETTLE,
        STA_SETTLE,
        WAITING_STA,
        RESTORE_AP
    };
    struct ApProbeCandidate {
        uint8_t bssid[6];
        int32_t channel;
        int32_t rssi;
    };
    unsigned long _lastApProbeTime;   // 上次探测时间戳
    bool _apProbeInProgress;          // 当前是否正在探测
    bool _apFallbackAuto;             // true = STA 失败自动回退的 AP；false = 用户主动设置的 AP
    int  _apProbeFailCount;           // 连续探测失败次数（用于退避）
    ApProbeState _apProbeState;
    ApProbeCandidate _apProbeCandidates[2];
    uint8_t _apProbeCandidateCount;
    uint8_t _apProbeAttempt;
    unsigned long _apProbeStateSince;
    unsigned long _apProbeStartedAt;
    unsigned long _apProbeAttemptStartedAt;
    static constexpr unsigned long AP_PROBE_INTERVAL_MS     = 120000;  // 正常探测间隔：2 分钟
    static constexpr unsigned long AP_PROBE_BACKOFF_MS      = 300000;  // 退避探测间隔：5 分钟，兼顾认证限流和局域网恢复速度
    static constexpr int AP_PROBE_BACKOFF_THRESHOLD         = 3;       // 连续失败 3 次后进入退避
    // 异步扫描超时和 STA 关联总时限。扫描阶段 AP 继续提供服务；只有发现
    // 目标 SSID 后才短暂切换 STA，弱信号/认证限流也不会阻塞 network_update。
    static constexpr unsigned long AP_PROBE_SCAN_TIMEOUT_MS = 10000;
    // Allow association + DHCP grace on weak/mesh networks without blocking the
    // main loop; the probe itself remains an asynchronous state machine.
    static constexpr unsigned long AP_PROBE_TIMEOUT_MS      = 15000;
    static constexpr unsigned long AP_PROBE_MDNS_SETTLE_MS  = 250;
    static constexpr unsigned long AP_PROBE_DRIVER_SETTLE_MS = 250;
    static constexpr unsigned long AP_PROBE_STA_SETTLE_MS   = 500;
    static constexpr unsigned long AP_PROBE_AP_SETTLE_MS    = 200;
    // AP 有客户端连接时的探测顺延间隔：60s 后重试（不杀 AP，不计失败）
    static constexpr unsigned long AP_PROBE_DEFER_MS        = 60000;

    // ── 开机 STA 连接重试（所有 Web 启动模式共用）─────────────────────────
    // 背景：开机只发起一次 WiFi.begin()，单次 AUTH_EXPIRE（reason=2，
    //       同名 SSID 多 AP 环境常见）超时即回退 AP，此后探测恢复退避到 5min，
    //       设备可卡 AP 数小时（fastbee.local 反复不可达的直接根因）。
    //       超时后先完整重置驱动重试若干次，全部失败才回退 AP。
    uint8_t _bootConnectRetries;                       // 当前开机重试计数
    static constexpr uint8_t BOOT_STA_MAX_RETRIES = 3; // 回退 AP 前的重试次数（共最多 4 次连接尝试）
    // 开机重试候选扫描也必须异步执行。同步 scanNetworks() 会阻塞
    // network_update 数秒，期间 AsyncTCP/Web 请求无法及时处理。
    struct BootStaCandidate {
        uint8_t bssid[6];
        int32_t channel;
        int32_t rssi;
    };
    bool _bootStaRetryScanInProgress;
    enum class BootStaRetryScanState : uint8_t {
        IDLE,
        DRIVER_NULL_SETTLE,
        DRIVER_STA_SETTLE,
        SCANNING
    };
    BootStaRetryScanState _bootStaRetryScanState;
    unsigned long _bootStaRetryScanStartedAt;
    uint8_t _bootStaRetryCandidateCount;
    BootStaCandidate _bootStaRetryCandidates[3];
    static constexpr unsigned long BOOT_STA_SCAN_TIMEOUT_MS = 10000;
    void startBootStaRetryScan();
    void handleBootStaRetryScan();
    void resetBootStaRetryScan();
    void launchBootStaRetryConnection();

    // ── AP 回退滞留看门狗 ──────────────────────────────────────────────
    // 自动回退后探测可能因认证失败/弱信号持续不恢复（且退避到 5min 间隔），
    // 设备长期停在 192.168.4.1、局域网不可达。滞留超阈值且 AP 无客户端
    // （不打断用户配网）时调度一次干净重启，重启后开机流程走全信道扫描
    // + 多次重试的 STA 连接，成功率高于运行时探测。

    // STA 运行时长时间失联 → 调度重启回落 AP 的看门狗
    // 背景：AP 回退仅在开机流程（connectToWiFiBlocking 失败）触发；开机已连上、
    //       运行中才掉线的场景只会无限指数退避重连，永远不会回退 AP，导致设备
    //       无 STA IP、mDNS 失效、局域网彻底不可达且无法自恢复（需物理断电）。
    //       超过此阈值仍未重连成功则调度一次干净重启（重启后走开机 AP 回退路径，
    //       恢复 192.168.4.1 可达）。5 分钟可覆盖指数退避和弱信号短时波动，
    //       避免原 60 秒阈值导致路由器漫游期间频繁重启。
    unsigned long _staDisconnectedSince;  // STA 持续断开起始时间戳（0=已连接/未计时）
    static constexpr unsigned long STA_LOST_REBOOT_MS = 300000; // 5 分钟持续失联才重启回退 AP
    static constexpr unsigned long STA_ONLY_RETRY_INTERVAL_MS = 120000; // 禁止 AP 时避免认证风暴

    // ── 网关可达性观测与自适应恢复 ───────────────────────────────────
    // 从未响应过 ICMP 的路由器只做告警；若本次 STA 会话已确认网关可响应，随后
    // 连续多轮无回包，则判定为僵尸关联并仅重连 WiFi，不重启整机。
    bool probeGatewayReachable();
    void resetGatewayWatchdog();
    static void gwPingOnSuccess(void* handle, void* args);
    static void gwPingOnEnd(void* handle, void* args);
    unsigned long _lastGwProbeTime = 0;
    uint8_t _gwArpFailCount = 0;
    bool _gwProbeEverSucceeded = false;  // 当前 STA 会话是否建立过可达性基线
    uint8_t _gwProbeState = 0;            // 0=空闲 1=探测中 2=已有结果待消费
    volatile bool _gwProbeResult = false; // 上一轮 ping 是否收到回包
    unsigned long _gwProbeStart = 0;
    void* _gwPingHandle = nullptr;        // esp_ping_handle_t
    // ICMP 可能被家用路由器/mesh/AP 丢弃或限速，不能把短时无回包当成
    // 僵尸关联。降低探测频率并要求至少 5 个连续失败（约 5 分钟）后，
    // 才执行 STA-only 恢复，避免网页压力或弱信号瞬断被放大成断网。
    static constexpr unsigned long GW_PROBE_INTERVAL_MS = 60000;   // 探测间隔 60s
    static constexpr unsigned long GW_PROBE_TIMEOUT_MS = 8000;     // 单轮探测超时兜底（ping 自身 2s）
    static constexpr uint8_t GW_PROBE_WARNING_THRESHOLD = 3;       // 连续 3 次失败后降频输出观测告警
    static constexpr uint8_t GW_PROBE_RECOVERY_THRESHOLD = 5;      // 已有成功基线后连续失败 5 次，仅重连 STA
    static constexpr uint8_t GW_PROBE_UNKNOWN_RECOVERY_THRESHOLD = 5; // 无基线但持续无数据面也仅重连 STA
};

#endif
