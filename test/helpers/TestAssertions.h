/**
 * @file TestAssertions.h
 * @brief 自定义测试断言
 */

#ifndef TEST_ASSERTIONS_H
#define TEST_ASSERTIONS_H

#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>

// 字符串包含断言
#define TEST_ASSERT_STRING_CONTAINS(expected, actual) \
    TEST_ASSERT_TRUE_MESSAGE(strstr(actual, expected) != nullptr, \
        (String("Expected '") + actual + "' to contain '" + expected + "'").c_str())

// JSON字段存在断言
#define TEST_ASSERT_JSON_HAS_FIELD(json, field) \
    TEST_ASSERT_TRUE_MESSAGE(json.containsKey(field), \
        (String("JSON missing field: ") + field).c_str())

// JSON字段值断言
#define TEST_ASSERT_JSON_FIELD_EQUALS(json, field, expected) \
    TEST_ASSERT_EQUAL_MESSAGE(expected, json[field].as<decltype(expected)>(), \
        (String("JSON field '") + field + "' value mismatch").c_str())

// 近似相等断言（浮点数）
#define TEST_ASSERT_FLOAT_EQUALS(expected, actual, tolerance) \
    TEST_ASSERT_TRUE_MESSAGE(abs(actual - expected) <= tolerance, \
        (String("Expected ") + expected + " +/- " + tolerance + ", got " + actual).c_str())

// 范围断言
#define TEST_ASSERT_IN_RANGE(value, min, max) \
    TEST_ASSERT_TRUE_MESSAGE(value >= min && value <= max, \
        (String("Value ") + value + " not in range [" + min + ", " + max + "]").c_str())

// IP地址格式断言
#define TEST_ASSERT_VALID_IP(ip) \
    TEST_ASSERT_TRUE_MESSAGE(isValidIP(ip), \
        (String("Invalid IP address: ") + ip).c_str())

#define TEST_ASSERT_VALID_IP_FORMAT(ip) \
    TEST_ASSERT_TRUE_MESSAGE(isValidIP(String(ip)), \
        (String("Invalid IP address format: ") + String(ip)).c_str())

// 非空字符串断言
#define TEST_ASSERT_STR_NOT_EMPTY(str) \
    TEST_ASSERT_FALSE_MESSAGE(str.isEmpty(), "String should not be empty")

// 空字符串断言
#define TEST_ASSERT_STR_EMPTY(str) \
    TEST_ASSERT_TRUE_MESSAGE(str.isEmpty(), "String should be empty")

// 状态码断言
#define TEST_ASSERT_STATUS_OK(status) \
    TEST_ASSERT_EQUAL_MESSAGE(200, status, "Expected HTTP 200 OK")

#define TEST_ASSERT_STATUS_UNAUTHORIZED(status) \
    TEST_ASSERT_EQUAL_MESSAGE(401, status, "Expected HTTP 401 Unauthorized")

#define TEST_ASSERT_STATUS_NOT_FOUND(status) \
    TEST_ASSERT_EQUAL_MESSAGE(404, status, "Expected HTTP 404 Not Found")

// 网络状态断言（AP模式）
#define TEST_ASSERT_AP_MODE_STATUS(status) \
    do { \
        TEST_ASSERT_EQUAL(NetworkStatus::AP_MODE, status.status); \
        TEST_ASSERT_FALSE(status.wifiConnected); \
        TEST_ASSERT_FALSE(status.internetAvailable); \
    } while(0)

// AP模式已断开状态断言
#define TEST_ASSERT_AP_MODE_DISCONNECTED(status) \
    do { \
        TEST_ASSERT_EQUAL_STRING("AP_MODE", status.status.c_str()); \
        TEST_ASSERT_FALSE(status.wifiConnected); \
        TEST_ASSERT_FALSE(status.internetAvailable); \
        TEST_ASSERT_EQUAL(0, status.rssi); \
        TEST_ASSERT_TRUE(status.ssid.isEmpty()); \
    } while(0)

// 网络状态断言（已连接）
#define TEST_ASSERT_CONNECTED_STATUS(status) \
    do { \
        TEST_ASSERT_EQUAL(NetworkStatus::CONNECTED, status.status); \
        TEST_ASSERT_TRUE(status.wifiConnected); \
        TEST_ASSERT_FALSE(status.ipAddress.isEmpty()); \
    } while(0)

// 辅助函数
inline bool isValidIP(const String& ip) {
    int parts[4];
    int count = sscanf(ip.c_str(), "%d.%d.%d.%d", &parts[0], &parts[1], &parts[2], &parts[3]);
    if (count != 4) return false;
    for (int i = 0; i < 4; i++) {
        if (parts[i] < 0 || parts[i] > 255) return false;
    }
    return true;
}

#endif // TEST_ASSERTIONS_H
