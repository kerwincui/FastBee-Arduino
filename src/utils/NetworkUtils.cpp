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
 * @brief 验证 SSID 是否满足 802.11 协议上限（0-32 字节任意值）
 * @param ssid SSID 字符串；空值合法（保留自动生成/未配置语义）
 * @return 是否有效
 */
bool NetworkUtils::isValidSSID(const String& ssid) {
    return ssid.length() <= 32;
}

/**
 * @brief 验证 mDNS 主机名是否为合法 RFC 1123 标签
 * @param hostname 主机名字符串；空值合法（调用方回退默认域名）
 * @return 是否有效
 */
bool NetworkUtils::isValidHostname(const String& hostname) {
    if (hostname.isEmpty()) return true;
    if (hostname.length() > 63) return false;
    if (hostname[0] == '-' || hostname[hostname.length() - 1] == '-') return false;
    for (size_t i = 0; i < hostname.length(); ++i) {
        const char c = hostname[i];
        if (isalnum(static_cast<unsigned char>(c)) || c == '-') continue;
        return false;
    }
    return true;
}

/**
 * @brief 验证可选 IP 字段（空值保留 DHCP/默认语义，非空必须是合法 IPv4）
 * @param ip IP 地址字符串
 * @return 是否有效
 */
bool NetworkUtils::isValidOptionalIP(const String& ip) {
    return ip.isEmpty() || isValidIP(ip);
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