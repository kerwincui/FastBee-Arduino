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

    // ========== 共享网络/MQTT 常量（避免各测试文件硬编码重复值） ==========

    // Modbus RTU Master 硬编码默认值（与 MasterConfig 构造函数同步）
    constexpr uint16_t MODBUS_DEFAULT_RESPONSE_TIMEOUT_MS = 1000;
    constexpr uint8_t  MODBUS_DEFAULT_MAX_RETRIES = 2;
    constexpr uint16_t MODBUS_DEFAULT_INTER_POLL_DELAY_MS = 100;

    // 以太网自动重连常量（与 MockMultiNetworkManager 同步）
    constexpr unsigned long ETH_RECONNECT_INTERVAL_MS = 10000;
    constexpr int ETH_MAX_RECONNECT_ATTEMPTS = 10;

    // MQTT 后台重连延迟（与生产代码同步）
    constexpr uint32_t MQTT_BOOT_STABILIZATION_DELAY_MS = 3000;

    // C3 低资源环境可用堆估算
    constexpr uint32_t C3_AVAILABLE_HEAP = 140000;
    constexpr uint32_t C3_TOTAL_HEAP = 320000;

    // 内存泄漏容忍阈值
    constexpr int32_t MAX_NETWORK_SWITCH_LEAK_BYTES = 5000;

    // 芯片环境配置组（统一维护，避免多处重复）
    struct ChipProfile {
        const char* name;
        uint32_t totalHeap;
        uint32_t psramSize;
        uint8_t  tcpBudget;
    };

    static constexpr ChipProfile ALL_CHIP_PROFILES[] = {
        {"ESP32-F4R0",    320000, 0,              6},
        {"ESP32-S3-F8R0",  320000, 0,              8},
        {"ESP32-S3-F8R4",  320000, 4*1024*1024,    8},
        {"ESP32-S3-F16R8", 320000, 8*1024*1024,    8},
    };
    static constexpr int CHIP_PROFILE_COUNT = 4;
}

#endif // TEST_CONFIG_H
