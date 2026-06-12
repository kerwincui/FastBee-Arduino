/**
 * @file test_health_monitor.cpp
 * @brief 健康监控 & 内存保护等级单元测试
 *
 * 覆盖范围：
 *  - 内存保护等级阈值边界（NORMAL/WARN/SEVERE/CRITICAL）
 *  - 等级单调递变（heap 下降时等级只能升高）
 *  - 降级恢复循环
 *  - 健康报告生成 & 内容验证
 *  - 文件系统空间监控
 *  - WiFi 连接状态追踪
 *  - 温度告警
 *  - 自定义健康检查项
 *  - 健康检查间隔配置
 */

#include <unity.h>
#include <Arduino.h>
#include "utils/HeapFragmentation.h"
#include "mocks/MockHealthMonitor.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

void test_health_monitor_group();

// ========== 内存保护等级阈值 ==========

// 对齐 HealthMonitor.h 的真实阈值常量
static constexpr uint32_t MEM_NORMAL  = 20480;  // 20KB
static constexpr uint32_t MEM_WARN    = 10240;  // 10KB
static constexpr uint32_t MEM_SEVERE  =  6144;  //  6KB

void test_memory_guard_level_boundaries() {
    TestLog::testStart("Memory Guard: Level Boundaries");

    auto& hm = MockHealthMon;
    hm.initialize();
    hm.clearWarningsAndErrors();

    // ---- NORMAL: heap >= 20KB ----
    hm.setFreeHeap(MEM_NORMAL);  // 刚好 20KB
    hm.update();
    SystemHealth h1 = hm.getHealthStatus();
    TEST_ASSERT_TRUE(h1.isHealthy);
    TEST_ASSERT_TRUE(h1.warnings.empty());
    TestLog::step("heap=20KB → NORMAL (healthy, no warnings)");

    hm.setFreeHeap(MEM_NORMAL + 10000);  // 30KB
    hm.update();
    h1 = hm.getHealthStatus();
    TEST_ASSERT_TRUE(h1.isHealthy);
    TestLog::step("heap=30KB → NORMAL");

    // ---- WARN: 10KB <= heap < 20KB ----
    hm.setFreeHeap(MEM_WARN);  // 刚好 10KB
    hm.update();
    h1 = hm.getHealthStatus();
    // Mock 的阈值是 20000 触发 warning、10000 触发 error
    // 10KB 应该触发 warning（freeHeap < 20000）
    TEST_ASSERT_FALSE(h1.warnings.empty());
    TestLog::step("heap=10KB → WARNING (low heap warning present)");

    hm.setFreeHeap(15000);  // 15KB - 在 WARN 区间
    hm.update();
    h1 = hm.getHealthStatus();
    TEST_ASSERT_FALSE(h1.warnings.empty());
    TEST_ASSERT_TRUE(h1.errors.empty());  // 但还不算 error
    TestLog::step("heap=15KB → WARN (warning, no error)");

    // ---- SEVERE/CRITICAL: heap < 10KB ----
    hm.setFreeHeap(MEM_SEVERE);  // 刚好 6KB
    hm.update();
    h1 = hm.getHealthStatus();
    TEST_ASSERT_FALSE(h1.isHealthy);
    TEST_ASSERT_FALSE(h1.errors.empty());
    TestLog::step("heap=6KB → CRITICAL (error, unhealthy)");

    hm.setFreeHeap(3000);  // 3KB - 极低
    hm.update();
    h1 = hm.getHealthStatus();
    TEST_ASSERT_FALSE(h1.isHealthy);
    TestLog::step("heap=3KB → CRITICAL");

    TestLog::testEnd(true);
}

// ========== 降级恢复循环 ==========

void test_heap_fragmentation_percent_bounds() {
    TestLog::testStart("Memory Guard: Fragmentation Percent Bounds");

    TEST_ASSERT_EQUAL_UINT8(0, calculateHeapFragmentationPercent(0, 0));
    TEST_ASSERT_EQUAL_UINT8(0, calculateHeapFragmentationPercent(16 * 1024, 16 * 1024));
    TEST_ASSERT_EQUAL_UINT8(0, calculateHeapFragmentationPercent(16 * 1024, 8 * 1024 * 1024));
    TEST_ASSERT_EQUAL_UINT8(50, calculateHeapFragmentationPercent(16 * 1024, 8 * 1024));
    TEST_ASSERT_EQUAL_UINT8(75, calculateHeapFragmentationPercent(16 * 1024, 4 * 1024));
    TEST_ASSERT_EQUAL_UINT8(100, calculateHeapFragmentationPercent(16 * 1024, 0));

    TestLog::step("Fragmentation is clamped to 0-100 even when PSRAM is the largest block");
    TestLog::testEnd(true);
}

void test_degradation_recovery_cycle() {
    TestLog::testStart("Health: Degradation → Recovery Cycle");

    auto& hm = MockHealthMon;
    hm.initialize();
    hm.clearWarningsAndErrors();

    // 阶段1：正常状态
    hm.setFreeHeap(100000);
    hm.update();
    SystemHealth h = hm.getHealthStatus();
    TEST_ASSERT_TRUE(h.isHealthy);
    TestLog::step("Phase 1: heap=100KB → HEALTHY");

    // 阶段2：堆下降到告警
    hm.setFreeHeap(8000);
    hm.update();
    h = hm.getHealthStatus();
    TEST_ASSERT_FALSE(h.isHealthy);
    TEST_ASSERT_FALSE(h.errors.empty());
    TestLog::step("Phase 2: heap=8KB → UNHEALTHY (critical)");

    // 阶段3：堆恢复
    hm.setFreeHeap(80000);
    hm.clearWarningsAndErrors();
    hm.update();
    h = hm.getHealthStatus();
    TEST_ASSERT_TRUE(h.isHealthy);
    TEST_ASSERT_TRUE(h.warnings.empty());
    TEST_ASSERT_TRUE(h.errors.empty());
    TestLog::step("Phase 3: heap=80KB → recovered to HEALTHY");

    // 阶段4：再次下降（验证可重复降级）
    hm.setFreeHeap(5000);
    hm.update();
    h = hm.getHealthStatus();
    TEST_ASSERT_FALSE(h.isHealthy);
    TestLog::step("Phase 4: heap=5KB → UNHEALTHY again");

    // 阶段5：再次恢复
    hm.setFreeHeap(120000);
    hm.clearWarningsAndErrors();
    hm.update();
    h = hm.getHealthStatus();
    TEST_ASSERT_TRUE(h.isHealthy);
    TestLog::step("Phase 5: heap=120KB → recovered again");

    TestLog::testEnd(true);
}

// ========== 健康报告内容 ==========

void test_health_report_content() {
    TestLog::testStart("Health: Report Content");

    auto& hm = MockHealthMon;
    hm.initialize();
    hm.setWiFiConnected(true);
    hm.setFreeHeap(50000);
    hm.update();

    char report[512];
    size_t len = hm.getHealthReport(report, sizeof(report));

    TEST_ASSERT_GREATER_THAN(0, (int)len);
    TestLog::step("Report generated, length > 0");

    // 验证关键字段
    TEST_ASSERT_TRUE(strstr(report, "heap=") != nullptr ||
                     strstr(report, "Heap:") != nullptr);
    TestLog::step("Report contains heap info");

    TEST_ASSERT_TRUE(strstr(report, "FS:") != nullptr ||
                     strstr(report, "File") != nullptr);
    TestLog::step("Report contains FS info");

    TEST_ASSERT_TRUE(strstr(report, "WiFi") != nullptr ||
                     strstr(report, "Connected") != nullptr);
    TestLog::step("Report contains WiFi info");

    TEST_ASSERT_TRUE(strstr(report, "Uptime") != nullptr);
    TestLog::step("Report contains uptime");

    TEST_ASSERT_TRUE(strstr(report, "HEALTHY") != nullptr ||
                     strstr(report, "UNHEALTHY") != nullptr);
    TestLog::step("Report contains health status");

    TestLog::testEnd(true);
}

// ========== 文件系统空间监控 ==========

void test_fs_space_monitoring() {
    TestLog::testStart("Health: FS Space Monitoring");

    auto& hm = MockHealthMon;
    hm.initialize();
    hm.clearWarningsAndErrors();

    // 正常空间
    hm.setFSSpace(500000, 1048576);  // 500KB used / 1MB total ≈ 48%
    hm.update();
    SystemHealth h = hm.getHealthStatus();
    TEST_ASSERT_TRUE(h.warnings.empty());
    TestLog::step("FS 48% used: no warning");

    // 高使用率（>80%）
    hm.setFSSpace(900000, 1048576);  // ≈ 86%
    hm.update();
    h = hm.getHealthStatus();
    TEST_ASSERT_FALSE(h.warnings.empty());
    TestLog::step("FS 86% used: WARNING");

    // 极高使用率（>95%）
    hm.setFSSpace(1020000, 1048576);  // ≈ 97%
    hm.update();
    h = hm.getHealthStatus();
    TEST_ASSERT_FALSE(h.isHealthy);
    TEST_ASSERT_FALSE(h.errors.empty());
    TestLog::step("FS 97% used: CRITICAL (unhealthy)");

    TestLog::testEnd(true);
}

// ========== WiFi 状态追踪 ==========

void test_wifi_status_tracking() {
    TestLog::testStart("Health: WiFi Status Tracking");

    auto& hm = MockHealthMon;
    hm.initialize();

    // 已连接
    hm.setWiFiConnected(true);
    hm.update();
    SystemHealth h = hm.getHealthStatus();
    TEST_ASSERT_TRUE(h.wifiConnected);
    TEST_ASSERT_EQUAL_STRING("TestWiFi", h.wifiSSID.c_str());
    TEST_ASSERT_FALSE(h.localIP.isEmpty());
    TestLog::step("WiFi connected: SSID and IP present");

    // 断开
    hm.setWiFiConnected(false);
    hm.update();
    h = hm.getHealthStatus();
    TEST_ASSERT_FALSE(h.wifiConnected);
    TEST_ASSERT_EQUAL_STRING("", h.wifiSSID.c_str());
    TEST_ASSERT_EQUAL_STRING("0.0.0.0", h.localIP.c_str());
    TestLog::step("WiFi disconnected: SSID empty, IP=0.0.0.0");

    TestLog::testEnd(true);
}

// ========== 温度告警 ==========

void test_temperature_alerts() {
    TestLog::testStart("Health: Temperature Alerts");

    auto& hm = MockHealthMon;
    hm.initialize();
    hm.clearWarningsAndErrors();

    // 温度通过 update() 随机生成，需要直接测试 checkTemperature
    // 正常温度
    TEST_ASSERT_TRUE(hm.checkTemperature(80.0f));
    TestLog::step("checkTemperature(80): pass (if temp < 80)");

    // 验证温度在合理范围
    hm.update();
    SystemHealth h = hm.getHealthStatus();
    TEST_ASSERT_TRUE(h.temperature > 0.0f && h.temperature < 100.0f);
    TestLog::step("Temperature in valid range (0-100°C)");

    TestLog::testEnd(true);
}

// ========== 自定义健康检查项 ==========

void test_custom_health_checks() {
    TestLog::testStart("Health: Custom Health Checks");

    auto& hm = MockHealthMon;
    hm.initialize();

    // 添加自定义检查
    HealthCheckItem check1;
    check1.name = "MQTT Connection";
    check1.description = "MQTT broker connectivity";
    check1.passed = true;
    check1.message = "Connected to broker";
    hm.addHealthCheck(check1);

    HealthCheckItem check2;
    check2.name = "Sensor Read";
    check2.description = "Last sensor reading";
    check2.passed = false;
    check2.message = "Sensor timeout";
    hm.addHealthCheck(check2);

    std::vector<HealthCheckItem> checks = hm.getHealthChecks();
    TEST_ASSERT_EQUAL(2, checks.size());
    TestLog::step("2 custom checks added");

    // 执行 update 后，失败检查应产生 warning
    hm.update();
    std::vector<String> warnings = hm.getWarnings();
    bool foundSensorWarning = false;
    for (auto& w : warnings) {
        if (w.indexOf("Sensor") >= 0) {
            foundSensorWarning = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(foundSensorWarning);
    TestLog::step("Failed custom check generates warning");

    TestLog::testEnd(true);
}

// ========== 检查间隔配置 ==========

void test_check_interval_config() {
    TestLog::testStart("Health: Check Interval Config");

    auto& hm = MockHealthMon;
    hm.initialize();

    // 默认间隔
    TEST_ASSERT_EQUAL(30000, (int)hm.getCheckInterval());
    TestLog::step("Default interval: 30000ms (30s)");

    // 修改间隔
    hm.setCheckInterval(10000);
    TEST_ASSERT_EQUAL(10000, (int)hm.getCheckInterval());
    TestLog::step("Interval updated to 10000ms (10s)");

    // 验证间隔合理性
    TEST_ASSERT_TRUE(hm.getCheckInterval() >= 1000);   // 最小 1 秒
    TEST_ASSERT_TRUE(hm.getCheckInterval() <= 300000); // 最大 5 分钟
    TestLog::step("Interval within reasonable bounds [1s, 5min]");

    TestLog::testEnd(true);
}

// ========== 堆内存直接检查 ==========

void test_heap_check_method() {
    TestLog::testStart("Health: checkHeapMemory Method");

    auto& hm = MockHealthMon;
    hm.initialize();

    // 堆充足
    hm.setFreeHeap(50000);
    TEST_ASSERT_TRUE(hm.checkHeapMemory(10000));
    TestLog::step("heap=50KB, threshold=10KB → pass");

    // 堆不足
    hm.setFreeHeap(5000);
    TEST_ASSERT_FALSE(hm.checkHeapMemory(10000));
    TestLog::step("heap=5KB, threshold=10KB → fail");

    // 自定义阈值
    hm.setFreeHeap(25000);
    TEST_ASSERT_TRUE(hm.checkHeapMemory(20000));
    TEST_ASSERT_FALSE(hm.checkHeapMemory(30000));
    TestLog::step("Custom threshold check verified");

    TestLog::testEnd(true);
}

// ========== 测试组入口 ==========

void test_health_monitor_group() {
    TestLog::groupStart("HealthMonitor Tests");

    // 内存保护等级
    RUN_TEST(test_memory_guard_level_boundaries);
    RUN_TEST(test_heap_fragmentation_percent_bounds);
    RUN_TEST(test_degradation_recovery_cycle);

    // 健康报告
    RUN_TEST(test_health_report_content);

    // 子系统监控
    RUN_TEST(test_fs_space_monitoring);
    RUN_TEST(test_wifi_status_tracking);
    RUN_TEST(test_temperature_alerts);

    // 自定义检查
    RUN_TEST(test_custom_health_checks);

    // 配置
    RUN_TEST(test_check_interval_config);
    RUN_TEST(test_heap_check_method);

    TestLog::groupEnd();
}
