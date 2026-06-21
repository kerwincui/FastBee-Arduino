/**
 * @file TestConfig.h
 * @brief 测试配置管理
 */

#ifndef TEST_CONFIG_H
#define TEST_CONFIG_H

#include <Arduino.h>

namespace TestConfig {
    // 网络测试配置
    constexpr const char* TEST_WIFI_SSID = "TestNetwork";
    constexpr const char* TEST_WIFI_PASSWORD = "TestPassword123";
    constexpr const char* TEST_MQTT_SERVER = "test.mqtt.server";
    constexpr int TEST_MQTT_PORT = 1883;
    constexpr int TEST_MQTTS_PORT = 8883;
    
    // 超时配置（毫秒）
    constexpr unsigned long NETWORK_TIMEOUT = 30000;
    constexpr unsigned long MQTT_TIMEOUT = 10000;
    constexpr unsigned long OTA_TIMEOUT = 300000;
    constexpr unsigned long AP_STARTUP_TIMEOUT = 5000;
    constexpr unsigned long WIFI_CONNECT_TIMEOUT = 30000;
    
    // 测试数据路径
    constexpr const char* TEST_DATA_DIR = "/test/data";
    constexpr const char* TEST_CONFIG_FILE = "/test/test_config.json";
    
    // 默认AP配置
    constexpr const char* DEFAULT_AP_IP = "192.168.4.1";
    constexpr const char* DEFAULT_AP_SSID = "fastbee-ap";
    
    // 测试阈值
    constexpr uint32_t MIN_FREE_HEAP = 10000;  // 10KB
    constexpr uint32_t MAX_HEAP_LEAK = 5000;   // 5KB
    
    // 测试IP地址
    constexpr const char* TEST_LOCAL_IP = "192.168.1.100";
    constexpr const char* TEST_GATEWAY_IP = "192.168.1.1";
    constexpr const char* TEST_SUBNET_MASK = "255.255.255.0";
    constexpr const char* TEST_DNS_SERVER = "8.8.8.8";
    
    // 测试客户端ID
    constexpr const char* TEST_CLIENT_ID = "TestClient001";
    constexpr const char* TEST_USERNAME = "testuser";
    constexpr const char* TEST_PASSWORD = "testpass";
}

#endif // TEST_CONFIG_H
