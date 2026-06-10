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
}

#endif // TEST_CONFIG_H
