#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <WiFi.h>
#include <functional>
#include <vector>

/**
 * @brief WiFi 模式枚举
 */
enum class NetworkMode {
    NETWORK_STA = 0,    // 仅 STA 模式
    NETWORK_AP = 1,     // 仅 AP 模式
    NETWORK_AP_STA = 2  // AP+STA 双模式
};

/**
 * @brief IP 配置类型
 */
enum class IPConfigType {
    DHCP = 0,   // DHCP 模式
    STATIC = 1  // 静态 IP 模式
};

/**
 * @brief 网络状态枚举
 */
enum class NetworkStatus {
    DISCONNECTED = 0,          // 断开连接
    CONNECTING = 1,            // 正在连接
    CONNECTED = 2,             // 已连接
    CONNECTION_FAILED = 3,      // 连接失败
    AP_MODE = 4,               // AP 模式
    IP_CONFLICT = 5,           // IP 冲突
    FAILOVER_IN_PROGRESS = 6   // 故障转移中
};

/**
 * @brief IP冲突检测模式
 */
enum class IPConflictMode {
    ARP = 0,      // ARP 检测
    ICMP = 1,     // ICMP 检测
    BOTH = 2      // 两者都使用
};

/**
 * @brief IP故障转移策略
 */
enum class IPFailoverStrategy {
    SMART = 0,     // 智能故障转移
    SEQUENTIAL = 1, // 顺序故障转移
    RANDOM = 2     // 随机故障转移
};

/**
 * @brief 网络配置结构体
 */
struct WiFiConfig {
    // 基本配置
    NetworkMode mode = NetworkMode::NETWORK_AP_STA;   // 默认 AP+STA：无 SSID 时仍能进入 AP 配网
    String deviceName = "FastBee";
    
    // AP 配置（首次启动或 STA 失败时的配网热点）
    String apSSID = "fastbee-ap";  // 默认 AP 热点名称，确保不为空
    String apPassword = "";            // 开放热点，方便首次配网
    uint8_t apChannel = 1;
    bool apHidden = false;
    uint8_t apMaxConnections = 4;
    
    // STA 配置
    String staSSID = "";
    String staPassword = "";
    IPConfigType ipConfigType = IPConfigType::DHCP;
    
    // 静态 IP 配置
    String staticIP = "";
    String gateway = "";
    String subnet = "";
    String dns1 = "";
    String dns2 = "";
    
    // IP冲突检测配置
    IPConflictMode conflictDetection = IPConflictMode::ARP;
    IPFailoverStrategy failoverStrategy = IPFailoverStrategy::SMART;
    bool autoFailover = true;
    uint16_t conflictCheckInterval = 30000;
    uint8_t maxFailoverAttempts = 3;
    uint8_t conflictThreshold = 2;
    bool fallbackToDHCP = true;
    std::vector<String> backupIPs;
    
    // 连接配置
    uint32_t connectTimeout = 10000;
    uint32_t reconnectInterval = 5000;
    uint8_t maxReconnectAttempts = 5;
    
    // 域名配置
    String customDomain = "fastbee";
    bool enableMDNS = true;
    bool enableDNS = true;    // AP 模式下需要 DNS 提供 captive portal
};

/**
 * @brief 网络状态信息结构体
 */
struct NetworkStatusInfo {
    NetworkStatus status = NetworkStatus::DISCONNECTED;
    String ssid = "";
    String ipAddress = "";
    String macAddress = "";
    int rssi = 0;
    unsigned long uptime = 0;
    bool internetAvailable = false;
    uint8_t apClientCount = 0;
    String apIPAddress = "";
    String activeIPType = "";
    uint16_t reconnectAttempts = 0;
    unsigned long lastConnectionTime = 0;
    String currentGateway = "";
    String currentSubnet = "";
    String dnsServer = "";
    uint8_t failoverCount = 0;
    bool conflictDetected = false;
    uint32_t txCount = 0;   // 协议消息发送计数
    uint32_t rxCount = 0;   // 协议消息接收计数
};

/**
 * @brief 网络事件回调函数类型
 */
typedef std::function<void(NetworkStatus, const String&)> NetworkEventCallback;

/**
 * @brief WiFi 管理器类
 * @details 负责 WiFi 连接管理、AP 模式管理等功能
 */
class WiFiManager {
public:
    WiFiManager();
    ~WiFiManager();
    
    /**
     * @brief 初始化 WiFi 管理器
     * @return 是否初始化成功
     */
    bool initialize();
    
    /**
     * @brief 连接到 WiFi 网络
     * @return 是否连接成功
     */
    bool connectToWiFi();
    
    /**
     * @brief 断开 WiFi 连接
     */
    void disconnectWiFi();
    
    /**
     * @brief 启动 AP 模式
     * @return 是否启动成功
     */
    bool startAPMode();
    
    /**
     * @brief 停止 AP 模式
     */
    void stopAPMode();
    
    /**
     * @brief 配置静态 IP
     * @return 是否配置成功
     */
    bool configureStaticIP();
    
    /**
     * @brief 配置 DHCP
     * @return 是否配置成功
     */
    bool configureDHCP();
    
    /**
     * @brief 扫描可用网络
     * @return 网络列表的 JSON 字符串
     */
    String scanNetworks();
    
    /**
     * @brief 连接到指定网络
     * @param ssid SSID
     * @param password 密码
     * @return 是否连接成功
     */
    bool connectToNetwork(const String& ssid, const String& password);
    
    /**
     * @brief 断开网络连接
     */
    void disconnectNetwork();
    
    /**
     * @brief 重启网络
     * @return 是否重启成功
     */
    bool restartNetwork();
    
    /**
     * @brief 检查互联网连接
     * @return 是否有互联网连接
     */
    bool checkInternetConnection();
    
    /**
     * @brief 更新网络状态信息
     */
    void updateStatusInfo();
    
    /**
     * @brief 设置连接回调
     * @param callback 回调函数
     */
    void setConnectionCallback(NetworkEventCallback callback);
    
    /**
     * @brief 设置断开连接回调
     * @param callback 回调函数
     */
    void setDisconnectionCallback(NetworkEventCallback callback);
    
    /**
     * @brief 设置 IP 冲突回调
     * @param callback 回调函数
     */
    void setIPConflictCallback(NetworkEventCallback callback);
    
    /**
     * @brief 设置自动重连
     * @param enabled 是否启用
     */
    void setAutoReconnect(bool enabled);
    
    /**
     * @brief 设置模式切换状态
     * @param transitioning 是否正在切换模式
     */
    void setModeTransitioning(bool transitioning);
    
    /**
     * @brief 重置 STA 初始化标志
     * @details 允许在重新启用 STA 时使用 WiFi.begin() 而不是 reconnect()
     */
    void resetStaInitialized() { staInitialized = false; }
    
    /**
     * @brief 获取配置
     * @return WiFi 配置
     */
    WiFiConfig getConfig() const;

    /**
     * @brief 设置网络配置
     * @param config 网络配置
     */
    void setNetworkConfig(const WiFiConfig& config);
    
    /**
     * @brief 获取状态信息
     * @return 网络状态信息
     */
    NetworkStatusInfo getStatusInfo() const;
    
    /**
     * @brief 获取 WiFi 模式字符串
     * @return WiFi 模式字符串
     */
    String getWiFiModeString();
    
    /**
     * @brief 获取 MAC 地址
     * @return MAC 地址
     */
    String getMACAddress();
    
    /**
     * @brief 获取芯片 ID
     * @return 芯片 ID
     */
    String getChipID();
    
    /**
     * @brief 将 RSSI 转换为百分比
     * @param rssi RSSI 值
     * @return 信号强度百分比
     */
    uint8_t rssiToPercentage(int32_t rssi);
    
    /**
     * @brief 检查 IP 是否有效
     * @param ip IP 地址字符串
     * @return 是否有效
     */
    bool isValidIP(const String& ip);
    
    /**
     * @brief 检查子网是否有效
     * @param subnet 子网掩码字符串
     * @return 是否有效
     */
    bool isValidSubnet(const String& subnet);
    
    /**
     * @brief 处理 WiFi 事件
     * @param event 事件 ID
     */
    void handleWiFiEvent(arduino_event_id_t event);
    
private:
    WiFiConfig wifiConfig;
    NetworkStatusInfo statusInfo;
    NetworkEventCallback connectionCallback = nullptr;
    NetworkEventCallback disconnectionCallback = nullptr;
    NetworkEventCallback ipConflictCallback = nullptr;
    
    bool connecting = false;
    unsigned long connectingStartTime = 0;
    unsigned long lastReconnectAttempt = 0;
    bool autoReconnectEnabled = true;
    bool modeTransitioning = false;  // 模式切换中标志，避免记录不必要的断开警告
    bool staInitialized = false;     // STA 已初始化标志（用于区分首次连接和重连）
    
    /**
     * @brief 触发网络事件
     * @param status 网络状态
     * @param message 事件消息
     */
    void triggerEvent(NetworkStatus status, const char* message);
    
    /**
     * @brief 尝试重连
     */
    void attemptReconnect();
};

#endif // WIFI_MANAGER_H