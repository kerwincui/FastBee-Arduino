/**
 * @file INetworkAdapter.h
 * @brief 网络适配器统一接口
 * @details WiFi、以太网（W5500）、4G（EC801E）适配器均实现此接口，
 *          使 NetworkManager 能以多态方式统一调度 connect/disconnect/update。
 *          减少 NetworkManager 中的 #if 条件分支，简化网络切换逻辑。
 */

#ifndef I_NETWORK_ADAPTER_H
#define I_NETWORK_ADAPTER_H

#include <Arduino.h>
#include <IPAddress.h>
#include "network/WiFiManager.h"

class Client;

/**
 * @class INetworkAdapter
 * @brief 网络适配器抽象接口
 *
 * 所有联网方式的适配器（EthernetAdapter、CellularAdapter）必须实现此接口，
 * NetworkManager 通过 INetworkAdapter* 指针统一调度，消除 switch-case 分发。
 */
class INetworkAdapter {
public:
    virtual ~INetworkAdapter() = default;

    /**
     * @brief 初始化适配器
     * @param config 网络配置（含引脚/IP/DHCP 等参数）
     * @return 初始化是否成功
     */
    virtual bool begin(const WiFiConfig& config) = 0;

    /**
     * @brief 检查是否已连接网络
     */
    virtual bool isConnected() const = 0;

    /**
     * @brief 断开连接
     */
    virtual void disconnect() = 0;

    /**
     * @brief 获取 Arduino Client 指针（用于 PubSubClient 等协议层）
     * @return Client 指针，未连接时返回 nullptr
     */
    virtual Client* getClient() = 0;

    /**
     * @brief 在主循环中更新适配器状态
     */
    virtual void update() = 0;

    /**
     * @brief 获取本地 IP 地址
     */
    virtual IPAddress localIP() const = 0;

    /**
     * @brief 获取适配器状态字符串（诊断用）
     */
    virtual String getStatusString() const = 0;
};

#endif // I_NETWORK_ADAPTER_H
