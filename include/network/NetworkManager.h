/**
 * @file NetworkManager.h
 * @brief 网络管理器类，负责WiFi连接、AP模式、DNS、mDNS等网络功能
 * @author kerwincui
 * @date 2025-12-02
 */

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <WiFi.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <vector>
#include <functional>
#include <core/ConfigDefines.h>

// 网络模式枚举
enum class WiFiMode {
    STA_ONLY = 0,  // 仅STA模式
    AP_ONLY = 1,   // 仅AP模式
    AP_STA = 2     // AP+STA模式
};

// IP配置类型枚举
enum class IPConfigType {
    DHCP = 0,      // 动态IP
    STATIC = 1     // 静态IP
};

// 网络状态枚举
enum class NetworkStatus {
    DISCONNECTED = 0,      // 断开连接
    CONNECTING = 1,        // 连接中
    CONNECTED = 2,         // 已连接
    AP_MODE = 3,           // AP模式
    CONNECTION_FAILED = 4  // 连接失败
};

// WiFi配置结构体
struct WiFiConfig {
    // 基本配置
    WiFiMode mode = WiFiMode::STA_ONLY;
    String deviceName = "FBE10000001";
    
    // AP配置
    String apSSID;
    String apPassword;
    uint8_t apChannel = 1;
    bool apHidden = false;
    uint8_t apMaxConnections = 4;
    
    // STA配置
    String staSSID = "CMCC-7mnN";;
    String staPassword = "eb66bcm9";
    IPConfigType ipConfigType = IPConfigType::DHCP;
    
    // 静态IP配置
    String staticIP;
    String gateway;
    String subnet;
    String dns1;
    String dns2;
    
    // 高级配置
    uint32_t connectTimeout = 10000;     // 连接超时时间（毫秒）
    uint32_t reconnectInterval = 5000;   // 重连间隔（毫秒）
    uint8_t maxReconnectAttempts = 5;    // 最大重连尝试次数
    
    // 域名配置
    String customDomain = MDNS_HOSTNAME;
    bool enableMDNS = true;
    bool enableDNS = false;
};

// 网络状态信息结构体
struct NetworkStatusInfo {
    NetworkStatus status = NetworkStatus::DISCONNECTED;
    String ssid;
    String ipAddress;
    String apIPAddress;
    String macAddress;
    int32_t rssi = 0;
    uint8_t apClientCount = 0;
    uint32_t reconnectAttempts = 0;
    uint32_t lastConnectionTime = 0;
    uint32_t uptime = 0;
    bool internetAvailable = false;
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
    
    // 时间相关
    unsigned long lastReconnectAttempt;
    unsigned long lastStatusUpdate;
    
    // 回调函数
    NetworkEventCallback connectionCallback;
    NetworkEventCallback disconnectionCallback;
    
    // 私有方法
    bool initializeFileSystem();
    bool loadNetworkConfig();
    bool saveNetworkConfig();
    
    bool startAPMode();
    void stopAPMode();
    bool connectToWiFi();
    void disconnectWiFi();
    
    bool configureStaticIP();
    bool startMDNS();
    void stopMDNS();
    bool startDNSServer();
    void stopDNSServer();
    
    void handleWiFiEvent(arduino_event_id_t event);
    void updateStatusInfo();
    void attemptReconnect();
    void triggerEvent(NetworkStatus status, const String& message);
};

#endif // NETWORK_MANAGER_H