#ifndef NETWORK_UTILS_H
#define NETWORK_UTILS_H

#include <Arduino.h>
#include <IPAddress.h>
#include <WiFi.h>

/**
 * @brief 网络工具类
 * 
 * 提供各种网络相关的工具方法：
 * - RSSI信号强度转换
 * - IP地址验证
 * - 网络状态处理
 */
class NetworkUtils {
public:
    // 禁止实例化，所有方法都是静态的
    NetworkUtils() = delete;
    NetworkUtils(const NetworkUtils&) = delete;
    NetworkUtils& operator=(const NetworkUtils&) = delete;

    /**
     * @brief 将RSSI值转换为百分比
     * @param rssi RSSI值（-100到-50之间）
     * @return 信号强度百分比（0-100）
     */
    static uint8_t rssiToPercentage(int32_t rssi);

    /**
     * @brief 验证IP地址是否有效
     * @param ip IP地址字符串
     * @return 是否有效
     */
    static bool isValidIP(const String& ip);

    /**
     * @brief 验证子网掩码是否有效
     * @param subnet 子网掩码字符串
     * @return 是否有效
     */
    static bool isValidSubnet(const String& subnet);

    /**
     * @brief 获取WiFi模式的字符串表示
     * @param mode WiFi模式
     * @return 模式字符串
     */
    static String getWiFiModeString(WiFiMode_t mode);
};

#endif