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
extern void test_multi_network_mode_group();
extern void test_mdns_domain_group();
extern void test_mqtt_protocol_group();
extern void test_system_stability_group();
extern void test_web_api_group();
extern void test_e2e_scenarios_group();
extern void test_pagination_fixes_group();
extern void test_periph_exec_group();
extern void test_command_bus_group();
extern void test_error_handler_group();
extern void test_ota_manager_group();
extern void test_rule_script_group();
extern void test_batch_sse_group();
extern void test_security_auth_group();
extern void test_system_services_group();
extern void test_protocol_handlers_group();
extern void test_wifi_ip_dns_group();
extern void test_performance_bench_group();
extern void test_string_utils_group();
extern void test_lcd_manager_group();
extern void test_periph_config_group();
extern void test_time_utils_group();
extern void test_file_utils_group();
extern void test_config_storage_group();
extern void test_task_manager_group();
extern void test_health_monitor_group();
extern void test_regression_guard_group();
extern void test_tcp_page_loading_group();
extern void test_network_utils_group();
extern void test_script_engine_group();
extern void test_restart_diagnostics_group();
extern void test_modbus_handler_group();

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

    Serial.println("\n[TEST] Running Multi-Network Mode Tests...");
    test_multi_network_mode_group();

    Serial.println("\n[TEST] Running mDNS Custom Domain Tests...");
    test_mdns_domain_group();

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

    Serial.println("\n[TEST] Running Peripheral Execution Engine Tests...");
    test_periph_exec_group();

    Serial.println("\n[TEST] Running CommandBus Tests...");
    test_command_bus_group();

    Serial.println("\n[TEST] Running ErrorHandler Tests...");
    test_error_handler_group();

    Serial.println("\n[TEST] Running OTA Manager Tests...");
    test_ota_manager_group();

    Serial.println("\n[TEST] Running RuleScript Tests...");
    test_rule_script_group();

    Serial.println("\n[TEST] Running Batch & SSE Tests...");
    test_batch_sse_group();

    Serial.println("\n[TEST] Running Security & Auth Tests...");
    test_security_auth_group();

    Serial.println("\n[TEST] Running System Services Tests...");
    test_system_services_group();

    Serial.println("\n[TEST] Running Protocol Handlers Tests...");
    test_protocol_handlers_group();

    Serial.println("\n[TEST] Running WiFi/IP/DNS Tests...");
    test_wifi_ip_dns_group();

    Serial.println("\n[TEST] Running Performance Benchmarks...");
    test_performance_bench_group();

    Serial.println("\n[TEST] Running StringUtils Tests...");
    test_string_utils_group();

    Serial.println("\n[TEST] Running LCD/OLED Manager Tests...");
    test_lcd_manager_group();

    Serial.println("\n[TEST] Running Peripheral Configuration Tests...");
    test_periph_config_group();

    Serial.println("\n[TEST] Running TimeUtils Tests...");
    test_time_utils_group();

    Serial.println("\n[TEST] Running FileUtils Tests...");
    test_file_utils_group();

    Serial.println("\n[TEST] Running ConfigStorage Tests...");
    test_config_storage_group();

    Serial.println("\n[TEST] Running TaskManager Tests...");
    test_task_manager_group();

    Serial.println("\n[TEST] Running HealthMonitor Tests...");
    test_health_monitor_group();

    Serial.println("\n[TEST] Running Regression Guard Tests...");
    test_regression_guard_group();

    Serial.println("\n[TEST] Running TCP & Page Loading Tests...");
    test_tcp_page_loading_group();

    Serial.println("\n[TEST] Running NetworkUtils Tests...");
    test_network_utils_group();

    Serial.println("\n[TEST] Running ScriptEngine Tests...");
    test_script_engine_group();

    Serial.println("\n[TEST] Running RestartDiagnostics Tests...");
    test_restart_diagnostics_group();

    Serial.println("\n[TEST] Running Modbus Handler Tests...");
    test_modbus_handler_group();

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
