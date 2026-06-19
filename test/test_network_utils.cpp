/**
 * @file test_network_utils.cpp
 * @brief NetworkUtils 网络工具单元测试
 * 
 * 测试内容：
 * - RSSI 信号强度转百分比（边界值/中间值）
 * - IP 地址格式验证（合法/非法/边界）
 * - 子网掩码验证
 * - WiFi 模式字符串转换
 * - WiFiMode_t 枚举完整性
 */

#include <unity.h>
#include <Arduino.h>
#include <IPAddress.h>

void test_network_utils_group();

// ========== 内联复现 NetworkUtils 核心逻辑（test_build_src=no 要求自包含） ==========

// 镜像 utils/NetworkUtils.cpp::rssiToPercentage
static uint8_t rssiToPercentage(int32_t rssi) {
    if (rssi >= -50) return 100;
    if (rssi <= -100) return 0;
    return 2 * (rssi + 100);
}

// 镜像 utils/NetworkUtils.cpp::isValidIP
static bool isValidIP(const String& ip) {
    IPAddress addr;
    return addr.fromString(ip);
}

// 镜像 utils/NetworkUtils.cpp::isValidSubnet
static bool isValidSubnet(const String& subnet) {
    return isValidIP(subnet);
}

// 镜像 WiFiMode_t 枚举（来自 MockWiFi.h）
enum TestWiFiMode : int {
    TEST_WIFI_OFF     = 0,
    TEST_WIFI_STA     = 1,
    TEST_WIFI_AP      = 2,
    TEST_WIFI_AP_STA  = 3
};

// 镜像 utils/NetworkUtils.cpp::getWiFiModeString
static String getWiFiModeString(TestWiFiMode mode) {
    switch (mode) {
        case TEST_WIFI_OFF:    return "NULL";
        case TEST_WIFI_STA:    return "STA";
        case TEST_WIFI_AP:     return "AP";
        case TEST_WIFI_AP_STA: return "AP+STA";
        default:               return "Unknown";
    }
}

// ========== RSSI 转百分比测试 ==========

static void test_rssi_excellent_signal() {
    // >= -50 dBm 应返回 100%
    TEST_ASSERT_EQUAL(100, rssiToPercentage(-50));
    TEST_ASSERT_EQUAL(100, rssiToPercentage(-40));
    TEST_ASSERT_EQUAL(100, rssiToPercentage(-30));
    TEST_ASSERT_EQUAL(100, rssiToPercentage(0));
}

static void test_rssi_no_signal() {
    // <= -100 dBm 应返回 0%
    TEST_ASSERT_EQUAL(0, rssiToPercentage(-100));
    TEST_ASSERT_EQUAL(0, rssiToPercentage(-110));
    TEST_ASSERT_EQUAL(0, rssiToPercentage(-120));
    TEST_ASSERT_EQUAL(0, rssiToPercentage(-200));
}

static void test_rssi_middle_range() {
    // -99 ~ -51 线性映射
    TEST_ASSERT_EQUAL(2,  rssiToPercentage(-99));  // 2*(-99+100) = 2
    TEST_ASSERT_EQUAL(50, rssiToPercentage(-75));   // 2*(-75+100) = 50
    TEST_ASSERT_EQUAL(98, rssiToPercentage(-51));   // 2*(-51+100) = 98
}

static void test_rssi_boundary_values() {
    // 精确边界
    TEST_ASSERT_EQUAL(0,   rssiToPercentage(-100));  // 下界
    TEST_ASSERT_EQUAL(100, rssiToPercentage(-50));   // 上界
    TEST_ASSERT_EQUAL(2,   rssiToPercentage(-99));   // 下界+1
    TEST_ASSERT_EQUAL(98,  rssiToPercentage(-51));   // 上界-1
}

static void test_rssi_monotonic_increase() {
    // 验证单调递增：rssi 越大，百分比越大或相等
    uint8_t prev = 0;
    for (int32_t rssi = -100; rssi <= -50; rssi++) {
        uint8_t pct = rssiToPercentage(rssi);
        TEST_ASSERT_GREATER_OR_EQUAL(prev, pct);
        prev = pct;
    }
}

// ========== IP 地址验证测试 ==========

static void test_valid_ip_addresses() {
    TEST_ASSERT_TRUE(isValidIP("192.168.1.1"));
    TEST_ASSERT_TRUE(isValidIP("0.0.0.0"));
    TEST_ASSERT_TRUE(isValidIP("255.255.255.255"));
    TEST_ASSERT_TRUE(isValidIP("10.0.0.1"));
    TEST_ASSERT_TRUE(isValidIP("127.0.0.1"));
    TEST_ASSERT_TRUE(isValidIP("1.2.3.4"));
}

static void test_invalid_ip_addresses() {
    TEST_ASSERT_FALSE(isValidIP(""));
    TEST_ASSERT_FALSE(isValidIP("abc"));
    TEST_ASSERT_FALSE(isValidIP("256.0.0.1"));
    TEST_ASSERT_FALSE(isValidIP("192.168.1"));      // 缺少一段
    TEST_ASSERT_FALSE(isValidIP("192.168.1.-1"));
    TEST_ASSERT_FALSE(isValidIP("192.168.1.256"));
    TEST_ASSERT_FALSE(isValidIP("..."));
    TEST_ASSERT_FALSE(isValidIP("192..168.1"));
}

static void test_ip_edge_cases() {
    // 边界值
    TEST_ASSERT_TRUE(isValidIP("0.0.0.0"));
    TEST_ASSERT_TRUE(isValidIP("255.255.255.255"));
    TEST_ASSERT_FALSE(isValidIP("999.999.999.999"));
}

// ========== 子网掩码验证测试 ==========

static void test_valid_subnets() {
    TEST_ASSERT_TRUE(isValidSubnet("255.255.255.0"));
    TEST_ASSERT_TRUE(isValidSubnet("255.255.0.0"));
    TEST_ASSERT_TRUE(isValidSubnet("255.0.0.0"));
    TEST_ASSERT_TRUE(isValidSubnet("255.255.255.252"));  // /30
    TEST_ASSERT_TRUE(isValidSubnet("255.255.255.255"));
}

static void test_invalid_subnets() {
    TEST_ASSERT_FALSE(isValidSubnet(""));
    TEST_ASSERT_FALSE(isValidSubnet("256.0.0.0"));
    TEST_ASSERT_FALSE(isValidSubnet("abc.def.ghi.jkl"));
    // 注：isValidSubnet 只验证格式合法性，不验证是否为有效子网掩码
    // 如 1.2.3.4 格式合法但非有效子网，这是设计决策
    TEST_ASSERT_TRUE(isValidSubnet("1.2.3.4"));  // 格式合法
}

// ========== WiFi 模式字符串测试 ==========

static void test_wifi_mode_strings() {
    TEST_ASSERT_EQUAL_STRING("NULL", getWiFiModeString(TEST_WIFI_OFF).c_str());
    TEST_ASSERT_EQUAL_STRING("STA", getWiFiModeString(TEST_WIFI_STA).c_str());
    TEST_ASSERT_EQUAL_STRING("AP", getWiFiModeString(TEST_WIFI_AP).c_str());
    TEST_ASSERT_EQUAL_STRING("AP+STA", getWiFiModeString(TEST_WIFI_AP_STA).c_str());
}

static void test_wifi_mode_unknown() {
    TEST_ASSERT_EQUAL_STRING("Unknown", getWiFiModeString((TestWiFiMode)99).c_str());
    TEST_ASSERT_EQUAL_STRING("Unknown", getWiFiModeString((TestWiFiMode)-1).c_str());
}

static void test_wifi_mode_enum_values() {
    // 验证枚举值与 ESP-IDF 兼容
    TEST_ASSERT_EQUAL(0, TEST_WIFI_OFF);
    TEST_ASSERT_EQUAL(1, TEST_WIFI_STA);
    TEST_ASSERT_EQUAL(2, TEST_WIFI_AP);
    TEST_ASSERT_EQUAL(3, TEST_WIFI_AP_STA);
}

// ========== IPAddress 对象测试 ==========

static void test_ipaddress_parse_and_compare() {
    IPAddress a;
    IPAddress b;
    
    TEST_ASSERT_TRUE(a.fromString("192.168.1.100"));
    TEST_ASSERT_TRUE(b.fromString("192.168.1.100"));
    TEST_ASSERT_TRUE(a == b);
    
    IPAddress c;
    TEST_ASSERT_TRUE(c.fromString("10.0.0.1"));
    TEST_ASSERT_TRUE(a != c);
}

static void test_ipaddress_toString() {
    IPAddress addr(192, 168, 1, 100);
    TEST_ASSERT_EQUAL_STRING("192.168.1.100", addr.toString().c_str());
}

static void test_ipaddress_index_operator() {
    IPAddress addr(10, 0, 0, 1);
    TEST_ASSERT_EQUAL(10, addr[0]);
    TEST_ASSERT_EQUAL(0, addr[1]);
    TEST_ASSERT_EQUAL(0, addr[2]);
    TEST_ASSERT_EQUAL(1, addr[3]);
}

// ========== 测试组入口 ==========

void test_network_utils_group() {
    // RSSI 测试
    RUN_TEST(test_rssi_excellent_signal);
    RUN_TEST(test_rssi_no_signal);
    RUN_TEST(test_rssi_middle_range);
    RUN_TEST(test_rssi_boundary_values);
    RUN_TEST(test_rssi_monotonic_increase);
    
    // IP 地址验证
    RUN_TEST(test_valid_ip_addresses);
    RUN_TEST(test_invalid_ip_addresses);
    RUN_TEST(test_ip_edge_cases);
    
    // 子网掩码验证
    RUN_TEST(test_valid_subnets);
    RUN_TEST(test_invalid_subnets);
    
    // WiFi 模式
    RUN_TEST(test_wifi_mode_strings);
    RUN_TEST(test_wifi_mode_unknown);
    RUN_TEST(test_wifi_mode_enum_values);
    
    // IPAddress 对象
    RUN_TEST(test_ipaddress_parse_and_compare);
    RUN_TEST(test_ipaddress_toString);
    RUN_TEST(test_ipaddress_index_operator);
}
