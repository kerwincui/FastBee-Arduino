/**
 * @file EthernetAdapter.h
 * @brief 以太网适配器 (W5500 SPI)
 * @author kerwincui
 * @date 2026-06-02
 * 
 * 使用 ESP32 内置 ETH 驱动 + W5500 SPI 转网芯片
 * RJ45 10/100Mbps，连接至 SPI2 接口
 */

#ifndef ETHERNET_ADAPTER_H
#define ETHERNET_ADAPTER_H

#include "core/FeatureFlags.h"

#if FASTBEE_ENABLE_ETHERNET

#include <Arduino.h>
#include <SPI.h>
#include <ETH.h>
#include <WiFiClient.h>
#include "network/WiFiManager.h"

/**
 * @class EthernetAdapter
 * @brief W5500 SPI 以太网适配器
 * 
 * 通过 SPI2 接口连接 W5500 芯片，提供有线以太网连接。
 * 支持 DHCP 和静态 IP 配置。
 */
class EthernetAdapter {
public:
    EthernetAdapter();
    ~EthernetAdapter();

    /**
     * @brief 初始化以太网适配器
     * @param config 网络配置（含以太网引脚配置）
     * @return 初始化是否成功
     */
    bool begin(const WiFiConfig& config);

    /**
     * @brief 阻塞等待连接（带超时）
     * @param timeoutMs 超时毫秒数
     * @return 是否连接成功
     */
    bool waitForConnection(uint32_t timeoutMs = 10000);

    /**
     * @brief 检查是否已连接
     */
    bool isConnected() const;

    /**
     * @brief 获取本地 IP
     */
    IPAddress localIP() const;

    /**
     * @brief 获取 MAC 地址
     */
    String macAddress() const;

    /**
     * @brief 断开连接
     */
    void disconnect();

    /**
     * @brief 获取 Arduino Client 指针（用于 PubSubClient）
     */
    Client* getClient();

    /**
     * @brief 更新状态（在主循环中调用）
     */
    void update();

    /**
     * @brief 获取连接状态字符串
     */
    String getStatusString() const;

private:
    SPIClass* _spi = nullptr;
    WiFiClient _ethClient;  // ETH 使用 WiFiClient 兼容接口
    wifi_event_id_t _ethEventId = 0;  // WiFi 事件回调 ID（用于 removeEvent）
    bool _initialized = false;
    bool _connected = false;
    EthernetConfig _pinConfig;

    static void onEthEvent(arduino_event_id_t event, arduino_event_info_t info);
    static EthernetAdapter* _instance;  // 静态回调需要的实例指针
};

#endif // FASTBEE_ENABLE_ETHERNET
#endif // ETHERNET_ADAPTER_H
