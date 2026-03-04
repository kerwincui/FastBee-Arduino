/**
 * @file NetworkUtils.cpp
 * @brief 网络工具类实现
 * @author kerwincui
 * @date 2026-03-03
 */

#include "utils/NetworkUtils.h"

/**
 * @brief 将RSSI值转换为百分比
 * @param rssi RSSI值（-100到-50之间）
 * @return 信号强度百分比（0-100）
 */
uint8_t NetworkUtils::rssiToPercentage(int32_t rssi) {
    if (rssi >= -50) return 100;
    if (rssi <= -100) return 0;
    return 2 * (rssi + 100);
}

/**
 * @brief 验证IP地址是否有效
 * @param ip IP地址字符串
 * @return 是否有效
 */
bool NetworkUtils::isValidIP(const String& ip) {
    IPAddress addr;
    return addr.fromString(ip);
}

/**
 * @brief 验证子网掩码是否有效
 * @param subnet 子网掩码字符串
 * @return 是否有效
 */
bool NetworkUtils::isValidSubnet(const String& subnet) {
    return isValidIP(subnet);
}

/**
 * @brief 获取WiFi模式的字符串表示
 * @param mode WiFi模式
 * @return 模式字符串
 */
String NetworkUtils::getWiFiModeString(WiFiMode_t mode) {
    switch (mode) {
        case WIFI_MODE_NULL: return "NULL";
        case WIFI_MODE_STA: return "STA";
        case WIFI_MODE_AP: return "AP";
        case WIFI_MODE_APSTA: return "AP+STA";
        default: return "Unknown";
    }
}