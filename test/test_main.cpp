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
extern void test_network_mqtt_integration_group();
extern void test_mdns_domain_group();
extern void test_mqtt_protocol_group();
extern void test_system_stability_group();
extern void test_web_api_group();
extern void test_e2e_scenarios_group();
extern void test_pagination_fixes_group();
extern void test_periph_exec_group();
extern void test_command_bus_group();
extern void test_ota_manager_group();
extern void test_rule_script_group();
extern void test_batch_sse_group();
extern void test_security_auth_group();
extern void test_system_services_group();
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
extern void test_static_concurrency_group();
extern void test_network_utils_group();
extern void test_script_engine_group();
extern void test_restart_diagnostics_group();
extern void test_system_rebooter_group();
extern void test_modbus_handler_group();
extern void test_ds1302_group();
extern void test_lcd1602_group();
extern void test_cellular_adapter_group();
extern void test_memory_budget_group();
extern void test_modbus_mqtt_interaction_group();
extern void test_sensor_driver_group();
extern void test_seven_segment_group();
extern void test_gpio_validation_group();
extern void test_driver_registry_group();
extern void test_ethernet_adapter_group();
extern void test_provision_api_group();
extern void test_wifi_network_group();
extern void test_longrun_stability_group();
extern void test_stability_fixes_group();
extern void test_ir_remote_group();
extern void test_rf_remote_group();
extern void test_peripheral_manager_opt_group();
extern void test_profile_consistency_group();
extern void test_runtime_feature_manager_group();
extern void test_feature_budget_group();
extern void test_network_config_additional_group();  // Added: LittleFS & network.json config tests

// Phase A→D 新功能测试组：对应的 test_fragmentation_aware_scheduler.cpp /
// test_tls_memory_pool.cpp / test_feature_toggle.cpp / test_memory_profile_db.cpp
// 四个源文件在仓库中不存在（历史重构遗留的孤儿引用），会导致 native suite
// 链接失败（undefined reference），故移除其 extern 声明与下方调用。
// 待这些测试文件补齐后再恢复。

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

    Serial.println("\n[TEST] Running Additional Network Config Tests (LittleFS, power loss recovery)...");
    test_network_config_additional_group();  // NEW: Added LittleFS and WiFi transition tests

    Serial.println("\n[TEST] Running Multi-Network Mode Tests...");
    test_multi_network_mode_group();

    Serial.println("\n[TEST] Running Network-MQTT Integration Tests...");
    test_network_mqtt_integration_group();

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

    Serial.println("\n[TEST] Running Static Concurrency & Login Page Speed Tests...");
    test_static_concurrency_group();

    Serial.println("\n[TEST] Running NetworkUtils Tests...");
    test_network_utils_group();

    Serial.println("\n[TEST] Running ScriptEngine Tests...");
    test_script_engine_group();

    Serial.println("\n[TEST] Running RestartDiagnostics Tests...");
    test_restart_diagnostics_group();

    Serial.println("\n[TEST] Running SystemRebooter Tests...");
    test_system_rebooter_group();

    Serial.println("\n[TEST] Running Modbus Handler Tests...");
    test_modbus_handler_group();

    Serial.println("\n[TEST] Running DS1302 RTC Tests...");
    test_ds1302_group();

    Serial.println("\n[TEST] Running LCD1602 Tests...");
    test_lcd1602_group();

    Serial.println("\n[TEST] Running CellularAdapter EC801E Tests...");
    test_cellular_adapter_group();

    Serial.println("\n[TEST] Running MemoryBudget Tests...");
    test_memory_budget_group();

    Serial.println("\n[TEST] Running Profile Consistency Tests...");
    test_profile_consistency_group();

    Serial.println("\n[TEST] Running Modbus-MQTT Interaction Tests...");
    test_modbus_mqtt_interaction_group();

    Serial.println("\n[TEST] Running SensorDriver Tests...");
    test_sensor_driver_group();

    Serial.println("\n[TEST] Running SevenSegment (TM1637) Tests...");
    test_seven_segment_group();

    Serial.println("\n[TEST] Running GPIO Validation Tests...");
    test_gpio_validation_group();

    Serial.println("\n[TEST] Running DriverRegistry Tests...");
    test_driver_registry_group();

    Serial.println("\n[TEST] Running EthernetAdapter (W5500) Tests...");
    test_ethernet_adapter_group();

    Serial.println("\n[TEST] Running Provision API Tests...");
    test_provision_api_group();

    Serial.println("\n[TEST] Running WiFi Network Improvement Tests...");
    test_wifi_network_group();

    Serial.println("\n[TEST] Running Long-Run Stability Tests...");
    test_longrun_stability_group();

    Serial.println("\n[TEST] Running Stability Fixes Guard Tests...");
    test_stability_fixes_group();

    Serial.println("\n[TEST] Running IR Remote (learn/keymap/match) Tests...");
    test_ir_remote_group();

    Serial.println("\n[TEST] Running RF Remote (decode/learn/keymap/lock) Tests...");
    test_rf_remote_group();

    Serial.println("\n[TEST] Running PeripheralManager Optimization Tests...");
    test_peripheral_manager_opt_group();

    Serial.println("\n[TEST] Running RuntimeFeatureManager Tests...");
    test_runtime_feature_manager_group();

    Serial.println("\n[TEST] Running FeatureBudget Tests...");
    test_feature_budget_group();

    // Phase A→D 新功能测试：源文件缺失，暂不调用（见上方 extern 说明）

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
