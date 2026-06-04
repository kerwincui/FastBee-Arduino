/**
 * @file CellularAdapter.h
 * @brief 4G 蜂窝模块适配器 (EC801E-CN)
 * @author kerwincui
 * @date 2026-06-02
 * 
 * 通过 UART AT 指令控制中移 EC801E-CN 模块，
 * 使用 TinyGSM 库实现 PPP/TCP 透传，提供标准 Arduino Client 接口。
 */

#ifndef CELLULAR_ADAPTER_H
#define CELLULAR_ADAPTER_H

#include "core/FeatureFlags.h"

#if FASTBEE_ENABLE_CELLULAR

#include <Arduino.h>
#include <HardwareSerial.h>
#include "network/WiFiManager.h"

// TinyGSM 配置 - 在 include 之前定义
// EC801E-CN 的 AT 指令集与 SIM7600 系列（LTE Cat-4）兼容
#define TINY_GSM_MODEM_SIM7600
#define TINY_GSM_RX_BUFFER 1024

#include <TinyGsmClient.h>

/**
 * @class CellularAdapter
 * @brief 4G 蜂窝网络适配器 (EC801E-CN)
 * 
 * 通过 AT 指令激活 PDP 上下文，使用 TinyGSM 提供的 Client 接口
 * 与 PubSubClient 对接实现 MQTT over 4G。
 */
class CellularAdapter {
public:
    CellularAdapter();
    ~CellularAdapter();

    /**
     * @brief 初始化 4G 模块
     * @param config 网络配置（含 cellular 引脚配置）
     * @return 初始化是否成功
     */
    bool begin(const WiFiConfig& config);

    /**
     * @brief 检查是否已连接网络
     */
    bool isConnected();

    /**
     * @brief 断开连接并关闭模块
     */
    void disconnect();

    /**
     * @brief 获取 Arduino Client 指针（用于 PubSubClient）
     */
    Client* getClient();

    /**
     * @brief 获取信号质量 (CSQ)
     * @return 信号质量字符串 "CSQ: xx,yy"
     */
    String getSignalQuality();

    /**
     * @brief 获取 SIM 卡 ICCID
     */
    String getICCID();

    /**
     * @brief 获取 IMEI
     */
    String getIMEI();

    /**
     * @brief 获取运营商信息
     */
    String getOperator();

    /**
     * @brief 获取网络连接类型 (4G/3G/2G)
     */
    String getNetworkType();

    /**
     * @brief 获取信号质量 CSQ 值 (0-31, 99=未知)
     */
    int getSignalQualityCSQ();

    /**
     * @brief 检查SIM卡是否就绪
     */
    bool isSimReady();

    /**
     * @brief 获取连接状态字符串
     */
    String getStatusString() const;

    /**
     * @brief 获取 4G 模块分配的 IP 地址
     */
    IPAddress localIP();

    /**
     * @brief 更新状态（在主循环中调用）
     */
    void update();

    /**
     * @brief 重连网络
     */
    bool reconnect();

private:
    HardwareSerial* _serial = nullptr;
    TinyGsm* _modem = nullptr;
    TinyGsmClient* _gsmClient = nullptr;
    
    CellularConfig _pinConfig;
    String _apn;
    bool _initialized = false;
    bool _connected = false;
    unsigned long _lastCheckTime = 0;

    /**
     * @brief 模块上电
     */
    void powerOn();

    /**
     * @brief 模块断电
     */
    void powerOff();

    /**
     * @brief 等待模块就绪
     */
    bool waitForReady(uint32_t timeoutMs = 10000);

    /**
     * @brief 激活 PDP 上下文
     */
    bool activateNetwork();
};

#endif // FASTBEE_ENABLE_CELLULAR
#endif // CELLULAR_ADAPTER_H
