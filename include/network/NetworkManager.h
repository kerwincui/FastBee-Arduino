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
#include "network/WiFiManager.h"
#include "network/IPManager.h"
#include "network/DNSManager.h"

// 事件回调类型
typedef std::function<void(NetworkStatus, const String&)> NetworkEventCallback;

class AsyncWebServer;

/**
 * @class NetworkManager
 * @brief 网络管理器类，提供完整的网络管理功能
 */
class NetworkManager : public INetworkManager {
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
    bool connectToWiFi();
    /**
     * @brief 阻塞等待 WiFi 连接（带超时）
     * 连接成功后自动启动 mDNS；超时返回 false。
     * 用于 initialize() 中确保 Web 服务在网络就绪后启动。
     */
    bool connectToWiFiBlocking();
    void disconnectWiFi();
    bool configureStaticIP();
    bool configureDHCP();
    bool startMDNS();
    void stopMDNS();
    bool startDNSServer();
    void stopDNSServer();
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
    
    // 连接状态
    bool isInitialized;
    bool autoReconnectEnabled;
    bool connecting;              // 是否正在连接
    unsigned long connectingStartTime; // 连接开始时间
    
    // 时间相关
    unsigned long lastReconnectAttempt;
    unsigned long lastStatusUpdate;
    unsigned long lastConflictCheck;
    
    // 回调函数
    NetworkEventCallback connectionCallback;
    NetworkEventCallback disconnectionCallback;
    NetworkEventCallback ipConflictCallback;
    
    // 私有方法
    bool initializeFileSystem();
    bool loadNetworkConfig();
    bool saveNetworkConfig();
    void attemptReconnect();
    void triggerEvent(NetworkStatus status, const String& message);
};

#endif