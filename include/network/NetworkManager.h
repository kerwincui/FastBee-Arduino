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
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <vector>
#include <functional>
#include <core/ConfigDefines.h>
#include <ArduinoJson.h>

// IP冲突检测模式
enum class IPConflictMode {
    NONE = 0,      // 不检测
    PING = 1,      // 通过ICMP ping检测
    ARP = 2,       // 通过ARP请求检测
    AUTO = 3       // 自动选择
};

// IP故障转移策略
enum class IPFailoverStrategy {
    SEQUENTIAL = 0,     // 按顺序尝试备用IP
    RANDOM = 1,         // 随机尝试备用IP
    SMART = 2           // 智能选择（基于网络延迟）
};

// 网络模式枚举
enum class NetworkMode {
    NETWORK_OFF = 0,
    NETWORK_STA,
    NETWORK_AP,
    NETWORK_AP_STA
};

// IP配置类型枚举
enum class IPConfigType {
    DHCP = 0,      // 动态IP
    STATIC = 1,    // 静态IP
    SMART = 2      // DHCP失败时回退到静态IP
};

// 网络状态枚举
enum class NetworkStatus {
    DISCONNECTED = 0,      // 断开连接
    CONNECTING = 1,        // 连接中
    CONNECTED = 2,         // 已连接
    AP_MODE = 3,           // AP模式
    CONNECTION_FAILED = 4, // 连接失败
    IP_CONFLICT = 5,       // IP冲突
    FAILOVER_IN_PROGRESS = 6 // 故障转移中
};

// WiFi配置结构体
struct WiFiConfig {
    // 基本配置
    NetworkMode mode = NetworkMode::NETWORK_STA;
    String deviceName = "FBE10000001";
    
    // AP配置
    String apSSID;
    String apPassword;
    uint8_t apChannel = 1;
    bool apHidden = false;
    uint8_t apMaxConnections = 4;
    
    // STA配置
    String staSSID = "CMCC-7mnN";
    String staPassword = "eb66bcm9";
    IPConfigType ipConfigType = IPConfigType::DHCP;
    
    // 静态IP配置
    String staticIP;
    String gateway;
    String subnet;
    String dns1;
    String dns2;
    
    // IP冲突检测和备用IP配置
    std::vector<String> backupIPs;                    // 备用IP列表
    IPConflictMode conflictDetection = IPConflictMode::ARP;
    IPFailoverStrategy failoverStrategy = IPFailoverStrategy::SMART;
    bool autoFailover = true;                         // 自动故障转移
    uint16_t conflictCheckInterval = 30000;           // 冲突检测间隔(ms)
    uint8_t maxFailoverAttempts = 3;                  // 最大故障转移尝试次数
    uint8_t conflictThreshold = 2;                    // 连续检测到冲突的次数
    bool fallbackToDHCP = true;                       // 所有备用IP都失败时回退到DHCP
    
    // 高级配置
    uint32_t connectTimeout = 10000;     // 连接超时时间（毫秒）
    uint32_t reconnectInterval = 5000;   // 重连间隔（毫秒）
    uint8_t maxReconnectAttempts = 5;    // 最大重连尝试次数
    
    // 域名配置
    String customDomain = MDNS_HOSTNAME;
    bool enableMDNS = true;
    bool enableDNS = false;

    bool enableOTA = true;
    bool enableWebServer = true;
    bool conflictMode = true;
};

// 网络状态信息结构体
struct NetworkStatusInfo {
    NetworkStatus status = NetworkStatus::DISCONNECTED;
    String ssid;
    String ipAddress;
    String statusText;
    String apIPAddress;
    String macAddress;
    int32_t rssi = 0;
    uint8_t apClientCount = 0;
    uint32_t reconnectAttempts = 0;
    uint32_t lastConnectionTime = 0;
    uint32_t uptime = 0;
    bool internetAvailable = false;
    
    String signalStrength;
    String currentGateway;          // 当前网关
    String currentSubnet;           // 当前子网掩码
    String dnsServer;               // 当前DNS服务器
    int failoverCount;              // IP故障转移次数
    String activeIPType;            // "DHCP" 或 "Static"
    String conflictDetected;        // IP冲突检测结果
};

// 事件回调类型
typedef std::function<void(NetworkStatus, const String&)> NetworkEventCallback;

class AsyncWebServer;

/**
 * @class NetworkManager
 * @brief 网络管理器类，提供完整的网络管理功能
 */
class NetworkManager {
public:
    /**
     * @brief 构造函数
     * @param webServerPtr Web服务器指针
     */
    explicit NetworkManager(AsyncWebServer* webServerPtr = nullptr);
    
    /**
     * @brief 析构函数
     */
    ~NetworkManager();
    
    /**
     * @brief 初始化网络管理器
     * @return 初始化是否成功
     */
    bool initialize();
    
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
     * @brief 检查IP冲突
     * @return 是否检测到IP冲突
     */
    bool checkIPConflict();
    
    /**
     * @brief 切换到备用IP
     * @return 切换是否成功
     */
    bool switchToBackupIP();
    
    /**
     * @brief 切换到随机IP
     * @return 切换是否成功
     */
    bool switchToRandomIP();
    
    /**
     * @brief 切换到DHCP
     * @return 切换是否成功
     */
    bool switchToDHCP();
    
    /**
     * @brief 生成备用IP列表
     */
    void generateBackupIPs();
    
    /**
     * @brief 获取范围内的随机IP
     * @param network 网络地址
     * @param mask 子网掩码
     * @return 随机IP地址
     */
    String getRandomIPInRange(const String& network, const String& mask);
    
    /**
     * @brief 测试IP可用性
     * @param ip IP地址
     * @return IP是否可用
     */
    bool testIPAvailability(const String& ip);
    
    // 静态工具方法
    static bool isValidIP(const String& ip);
    static bool isValidSubnet(const String& subnet);
    static String getMACAddress();
    static String getChipID();
    static uint8_t rssiToPercentage(int32_t rssi);
    static String getWiFiModeString();

private:
    // 网络组件
    AsyncWebServer* webServer;
    Preferences preferences;
    DNSServer dnsServer;
    
    // 配置和状态
    WiFiConfig wifiConfig;
    NetworkStatusInfo statusInfo;
    
    // 连接状态
    bool isInitialized;
    bool dnsServerStarted;
    bool mdnsStarted;
    bool autoReconnectEnabled;
    bool connecting;              // 是否正在连接
    unsigned long connectingStartTime; // 连接开始时间
    bool ipConflictDetected;      // IP冲突检测状态
    
    // 时间相关
    unsigned long lastReconnectAttempt;
    unsigned long lastStatusUpdate;
    unsigned long lastConflictCheck;
    unsigned long lastFailoverAttempt;
    
    // 故障转移相关
    int currentFailoverAttempts;
    int currentBackupIPIndex;
    int conflictDetectionCount;
    
    // 冲突检测缓存
    struct ConflictCache {
        String ip;
        bool conflicted;
        unsigned long timestamp;
    };
    std::vector<ConflictCache> conflictCache;
    
    // 回调函数
    NetworkEventCallback connectionCallback;
    NetworkEventCallback disconnectionCallback;
    NetworkEventCallback ipConflictCallback;
    
    // 私有方法
    bool initializeFileSystem();
    bool loadNetworkConfig();
    bool saveNetworkConfig();
    
    bool startAPMode();
    void stopAPMode();
    bool connectToWiFi();
    void disconnectWiFi();
    
    bool configureStaticIP();
    bool configureDHCP();
    bool startMDNS();
    void stopMDNS();
    bool startDNSServer();
    void stopDNSServer();
    
    void handleWiFiEvent(arduino_event_id_t event);
    void updateStatusInfo();
    void updateIPConflictStatus();
    void attemptReconnect();
    void triggerEvent(NetworkStatus status, const String& message);
    
    // IP冲突检测方法
    bool detectConflictByARP(const String& ip);
    bool detectConflictByPing(const String& ip);
    bool detectConflictByGateway(const String& ip);
    
    // 故障转移方法
    bool performFailover();
    String selectNextIP();
    void cleanupConflictCache();
};

#endif