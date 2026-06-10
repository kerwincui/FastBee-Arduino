/**
 * @file test_main.cpp
 * @brief FastBee测试框架主入口
 * @description Unity测试框架配置和主函数
 */

#include <unity.h>
#include <Arduino.h>
#include <LittleFS.h>

// 测试配置
#define TEST_VERBOSITY UNITY_VERBOSE

// 前置声明所有测试组
extern void test_network_config_group();
extern void test_mqtt_protocol_group();
extern void test_system_stability_group();
extern void test_web_api_group();
extern void test_e2e_scenarios_group();
extern void test_pagination_fixes_group();

// 测试夹具
void setUp() {
    // 每个测试前的设置
    Serial.println("\n[TEST] Setting up test environment...");
}

void tearDown() {
    // 每个测试后的清理
    Serial.println("[TEST] Tearing down test environment...");
    delay(100);  // 给系统一些时间稳定
}

// 测试主函数
void setup() {
    // 初始化串口
    Serial.begin(115200);
    while (!Serial) {
        ; // 等待串口连接
    }
    
    delay(2000);  // 等待串口监视器就绪
    
    Serial.println("\n========================================");
    Serial.println("  FastBee Automated Test Suite");
    Serial.println("========================================\n");
    
    // 初始化文件系统
    if (!LittleFS.begin()) {
        Serial.println("[WARNING] LittleFS initialization failed, attempting format...");
        if (!LittleFS.format()) {
            Serial.println("[ERROR] LittleFS format failed!");
        } else {
            LittleFS.begin();
        }
    }
    
    // 配置Unity
    UNITY_BEGIN();
    
    // 运行测试组
    Serial.println("\n[TEST] Running Network Configuration Tests...");
    test_network_config_group();
    
    Serial.println("\n[TEST] Running MQTT Protocol Tests...");
    test_mqtt_protocol_group();
    
    Serial.println("\n[TEST] Running System Stability Tests...");
    test_system_stability_group();
    
    Serial.println("\n[TEST] Running Web API Tests...");
    test_web_api_group();
    
    Serial.println("\n[TEST] Running End-to-End Scenario Tests...");
    test_e2e_scenarios_group();
    
    Serial.println("\n[TEST] Running Pagination Fixes & Smoke Tests...");
    test_pagination_fixes_group();
    
    // 结束测试并输出结果
    int result = UNITY_END();
    
    Serial.println("\n========================================");
    if (result == 0) {
        Serial.println("  All Tests PASSED!");
    } else {
        Serial.printf("  Tests FAILED: %d failures\n", result);
    }
    Serial.println("========================================\n");
    
#ifndef UNIT_TEST
    // 防止测试结束后重启
    while (true) {
        delay(1000);
    }
#endif
}

void loop() {
    // 测试在setup中完成，loop保持空
}

#ifdef UNIT_TEST
int main(int, char**) {
    setup();
    return 0;
}
#endif
