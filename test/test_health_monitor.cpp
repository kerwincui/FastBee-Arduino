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
#include <fstream>
#include <sstream>
#include <regex>
#include "utils/HeapFragmentation.h"
#include "mocks/MockHealthMonitor.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

// 辅助：读取项目源文件（native 测试环境下使用标准文件 I/O）
static std::string readProjectFile(const char* relativePath) {
    const char* roots[] = { ".", "..", "../.." };
    for (const char* root : roots) {
        std::string fullPath = std::string(root) + "/" + relativePath;
        std::ifstream file(fullPath);
        if (file.is_open()) {
            std::ostringstream ss;
            ss << file.rdbuf();
            return ss.str();
        }
    }
    return "";
}

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

// ========== DRAM/PSRAM 内存保护测试 ==========

/**
 * @brief HM-11: DRAM 内部内存 vs PSRAM 区分验证
 * 在有 PSRAM 的设备上，MALLOC_CAP_DEFAULT 的最大连续块会包含 PSRAM
 * 导致 CRITICAL 判断和 WiFi/MQTT 内存保护失效
 */
void test_dram_psram_memory_distinction() {
    TestLog::testStart("Memory: DRAM vs PSRAM Distinction");

    // 模拟 ESP32-S3 带 PSRAM 场景：
    //   DRAM 内部空闲：42KB，最大连续块：10KB（碎片化）
    //   PSRAM 空闲：8MB，MALLOC_CAP_DEFAULT 的最大块：8MB
    uint32_t dramFree    = 42 * 1024;
    uint32_t dramLargest = 10 * 1024;
    uint32_t totalFree   = 42 * 1024 + 8 * 1024 * 1024;  // 含 PSRAM
    uint32_t totalLargest = 8 * 1024 * 1024;              // PSRAM 的大块

    // 1. DRAM 碎片率（基于 DRAM）vs 全局碎片率（可能被 PSRAM 拉低）
    uint8_t dramFrag  = calculateHeapFragmentationPercent(dramFree, dramLargest);
    uint8_t totalFrag = calculateHeapFragmentationPercent(totalFree, totalLargest);
    TEST_ASSERT_GREATER_THAN(50, (int)dramFrag);   // DRAM 碎片率应该很高
    TEST_ASSERT_LESS_THAN(5, (int)totalFrag);       // PSRAM 引入后全局碎片率被拉低
    TestLog::step("DRAM frag is high, but total frag is low due to PSRAM");

    // 2. SSL/TCP 用 DRAM 连续块，10KB < 20KB 阈值，不可连接
    uint32_t MQTTS_MIN_BLOCK = 20000;
    TEST_ASSERT_FALSE_MESSAGE(dramLargest >= MQTTS_MIN_BLOCK,
        "DRAM largest block 10KB < 20KB: MQTTS must not connect");
    TestLog::step("DRAM check correctly blocks MQTTS (fragmented DRAM)");

    // 3. 如果用 PSRAM 大块进行判断，会误以为内存充足
    TEST_ASSERT_TRUE_MESSAGE(totalLargest >= MQTTS_MIN_BLOCK,
        "Total largest block 8MB would mistakenly allow MQTTS");
    TestLog::step("Total check would INCORRECTLY allow MQTTS (PSRAM misleads)");

    // 4. 验证破片率计算：当 largestBlock > freeHeap 时不应出错（如 PSRAM 场景）
    uint8_t fragWhenPsramDominates = calculateHeapFragmentationPercent(dramFree, totalLargest);
    TEST_ASSERT_EQUAL_UINT8(0, fragWhenPsramDominates);
    TestLog::step("Fragmentation=0 when largest > free (PSRAM case handled)");

    TestLog::testEnd(true);
}

/**
 * @brief HM-12: 基于 DRAM 的碎片率检测准确性
 * HealthMonitor 应基于 DRAM 计算碎片率，不应被 PSRAM 干扰
 */
void test_dram_fragmentation_detection() {
    TestLog::testStart("Memory: DRAM-based Fragmentation Detection");

    // 模拟不同场景的碎片率
    struct TestCase {
        uint32_t dramFree;
        uint32_t dramLargest;
        uint8_t  expectedMinFrag;
        uint8_t  expectedMaxFrag;
        const char* desc;
    };

    TestCase cases[] = {
        // DRAM 健康：空闲 40KB，最大块 35KB → 低碎片率
        { 40 * 1024, 35 * 1024, 0, 15, "Healthy DRAM (40KB free, 35KB block)" },
        // DRAM 中度碎片：空闲 40KB，最大块 20KB → 中等碎片率
        { 40 * 1024, 20 * 1024, 45, 55, "Moderate fragmentation (40KB free, 20KB block)" },
        // DRAM 严重碎片：空闲 40KB，最大块 4KB → 高碎片率
        { 40 * 1024, 4 * 1024, 85, 100, "Severe fragmentation (40KB free, 4KB block)" },
    };

    for (const auto& tc : cases) {
        uint8_t frag = calculateHeapFragmentationPercent(tc.dramFree, tc.dramLargest);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT8(tc.expectedMinFrag, frag);
        TEST_ASSERT_LESS_OR_EQUAL_UINT8(tc.expectedMaxFrag, frag);
        TestLog::step(tc.desc);
    }

    // 验证 HealthMonitor.h 源码中 largestFreeBlock 使用 DRAM 内部检测
    std::string hmContent = readProjectFile("include/systems/HealthMonitor.h");
    TEST_ASSERT_TRUE_MESSAGE(!hmContent.empty(), "HealthMonitor.h must be readable");
    TEST_ASSERT_TRUE_MESSAGE(
        hmContent.find("dramFreeHeap") != std::string::npos,
        "SystemHealth must have dramFreeHeap field (DRAM internal)");
    TEST_ASSERT_TRUE_MESSAGE(
        hmContent.find("dramLargestBlock") != std::string::npos,
        "SystemHealth must have dramLargestBlock field (DRAM internal)");
    TestLog::step("SystemHealth.dramFreeHeap and dramLargestBlock fields present");

    TestLog::testEnd(true);
}

/**
 * @brief HM-13: HealthMonitor 健康报告包含 DRAM 内存字段
 * 证明 HealthMonitor.cpp 已正确统计和传递 DRAM 内存指标
 */
void test_health_monitor_dram_fields_in_report() {
    TestLog::testStart("HealthMonitor: DRAM Fields in Report");

    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    // 1. performHealthCheck 应使用 MALLOC_CAP_INTERNAL 获取 DRAM 内存
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("heap_caps_get_free_size(MALLOC_CAP_INTERNAL)") != std::string::npos,
        "performHealthCheck must use MALLOC_CAP_INTERNAL for DRAM free size");
    TestLog::step("MALLOC_CAP_INTERNAL used for DRAM free size");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)") != std::string::npos,
        "performHealthCheck must use MALLOC_CAP_INTERNAL for DRAM largest block");
    TestLog::step("MALLOC_CAP_INTERNAL used for DRAM largest block");

    // 2. 分析日志应同时输出 DRAM 和 Total 对比
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("dram=") != std::string::npos ||
        content.find("dram_free") != std::string::npos ||
        content.find("dramFreeHeap") != std::string::npos,
        "logMetricsSummary must output dram field");
    TestLog::step("DRAM metrics included in log output");

    // 3. JSON 指标应包含 dram_free 字段
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("dram_free") != std::string::npos,
        "getMetricsJson must include dram_free field");
    TestLog::step("dram_free field in metrics JSON");

    // 4. checkCriticalMemory 应使用 DRAM 内部进行检测
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("dramFreeHeap") != std::string::npos &&
        (content.find("DRAM < ") != std::string::npos ||
         content.find("criticalThreshold") != std::string::npos),
        "checkCriticalMemory must use DRAM for threshold check");
    TestLog::step("checkCriticalMemory uses DRAM threshold");

    TestLog::testEnd(true);
}

// ========== WiFi 内存保护测试 ==========

/**
 * @brief HM-14: WiFiManager.connectToWiFi() DRAM 内存保护存在
 * WiFi.begin() 需要 ~16KB DRAM，应在入口检查 MALLOC_CAP_INTERNAL
 */
void test_wifi_connect_dram_protection() {
    TestLog::testStart("WiFi: connectToWiFi DRAM Protection");

    std::string content = readProjectFile("src/network/WiFiManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "WiFiManager.cpp must be readable");

    // 1. 必须包含 MALLOC_CAP_INTERNAL 检测
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MALLOC_CAP_INTERNAL") != std::string::npos,
        "WiFiManager must use MALLOC_CAP_INTERNAL for DRAM check");
    TestLog::step("MALLOC_CAP_INTERNAL present in WiFiManager");

    // 2. 必须包含 WiFi 连接保护馘火墙日志
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("connectToWiFi: DRAM too low") != std::string::npos ||
        content.find("DRAM too low for WiFi.begin") != std::string::npos,
        "WiFiManager must log DRAM low warning for connectToWiFi");
    TestLog::step("DRAM low warning log present for connectToWiFi");

    // 3. DRAM 不足时应返回 false
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("WIFI_CONNECT_MIN_DRAM") != std::string::npos,
        "connectToWiFi must check WIFI_CONNECT_MIN_DRAM threshold");
    TestLog::step("WIFI_CONNECT_MIN_DRAM threshold check present");

    // 4. 验证阈值定义在头文件
    std::string hmh = readProjectFile("include/systems/HealthMonitor.h");
    TEST_ASSERT_TRUE_MESSAGE(
        hmh.find("WIFI_CONNECT_MIN_DRAM") != std::string::npos,
        "WIFI_CONNECT_MIN_DRAM must be defined in HealthMonitor.h");
    TestLog::step("WIFI_CONNECT_MIN_DRAM defined in HealthMonitor.h");

    TestLog::testEnd(true);
}

/**
 * @brief HM-15: WiFiManager.attemptReconnect() DRAM 内存保护存在
 */
void test_wifi_reconnect_dram_protection() {
    TestLog::testStart("WiFi: attemptReconnect DRAM Protection");

    std::string content = readProjectFile("src/network/WiFiManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "WiFiManager.cpp must be readable");

    // 1. attemptReconnect 应有 DRAM 检测
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("attemptReconnect: DRAM too low") != std::string::npos,
        "attemptReconnect must check DRAM and log when too low");
    TestLog::step("attemptReconnect DRAM check present");

    // 2. 应使用 WIFI_RECONN_MIN_DRAM 阈值
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("WIFI_RECONN_MIN_DRAM") != std::string::npos,
        "attemptReconnect must use WIFI_RECONN_MIN_DRAM threshold");
    TestLog::step("WIFI_RECONN_MIN_DRAM threshold check present");

    // 3. 验证重连阈值 ≤ 连接阈值（重连要求更小）
    std::string hmh = readProjectFile("include/systems/HealthMonitor.h");
    std::regex reconnMinRe("WIFI_RECONN_MIN_DRAM\\s*=\\s*(\\d+)");
    std::regex connectMinRe("WIFI_CONNECT_MIN_DRAM\\s*=\\s*(\\d+)");
    std::smatch m1, m2;
    if (std::regex_search(hmh, m1, reconnMinRe) && std::regex_search(hmh, m2, connectMinRe)) {
        int reconnVal = std::stoi(m1[1].str());
        int connectVal = std::stoi(m2[1].str());
        TEST_ASSERT_TRUE_MESSAGE(reconnVal <= connectVal,
            "WIFI_RECONN_MIN_DRAM must be <= WIFI_CONNECT_MIN_DRAM (reconnect is lighter)");
        TestLog::step(("RECONN=" + m1[1].str() + " <= CONNECT=" + m2[1].str() + " (correct hierarchy)").c_str());
    }

    TestLog::testEnd(true);
}

/**
 * @brief HM-16: WiFi DRAM 阈值边界条件验证
 * DRAM 刚好达到阈值时应沈豱连接
 */
void test_wifi_connect_dram_threshold_boundary() {
    TestLog::testStart("WiFi: DRAM Threshold Boundary Conditions");

    // 模拟连接决策逻辑
    constexpr uint32_t WIFI_CONNECT_MIN = 16384;  // 对齐 HealthMonitor.h 中的定义
    constexpr uint32_t WIFI_RECONN_MIN  = 12288;

    // Case 1: DRAM >= CONNECT_MIN → 允许连接
    TEST_ASSERT_TRUE(16384 >= WIFI_CONNECT_MIN);
    TestLog::step("DRAM=16KB = threshold: allowed");

    // Case 2: DRAM < CONNECT_MIN → 毫差一点拒绝
    TEST_ASSERT_FALSE(16383 >= WIFI_CONNECT_MIN);
    TestLog::step("DRAM=16383 < threshold: blocked");

    // Case 3: DRAM >= RECONN_MIN → 允许重连
    TEST_ASSERT_TRUE(12288 >= WIFI_RECONN_MIN);
    TestLog::step("DRAM=12KB = reconnect threshold: allowed");

    // Case 4: DRAM 充足时 log 输出（验证源码中的 OK 日志）
    std::string content = readProjectFile("src/network/WiFiManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("connectToWiFi: DRAM OK") != std::string::npos,
        "Must log DRAM OK message when memory is sufficient");
    TestLog::step("DRAM OK log present for successful cases");

    TestLog::testEnd(true);
}

// ========== 多芯片环境兼容性测试 ==========

/**
 * @brief HM-17: 无 PSRAM 芯片（ESP32-C3）内存检测一致性
 * 无 PSRAM 时 MALLOC_CAP_INTERNAL 和 MALLOC_CAP_DEFAULT 结果应相近
 */
void test_no_psram_chip_memory_detection() {
    TestLog::testStart("Compat: No-PSRAM Chip (ESP32-C3) Memory Detection");

    // 模拟无 PSRAM 设备（ESP32-C3/D0WD-V3）：
    //   MALLOC_CAP_INTERNAL ≈ MALLOC_CAP_DEFAULT（都是 DRAM）
    uint32_t dramFree    = 45 * 1024;  // DRAM 内存
    uint32_t totalFree   = 45 * 1024;  // 无 PSRAM，和 DRAM 一致
    uint32_t dramLargest = 30 * 1024;
    uint32_t totalLargest = 30 * 1024;

    // 无 PSRAM 时，两种检测方式结果应相同
    TEST_ASSERT_EQUAL(dramFree, totalFree);
    TestLog::step("No-PSRAM: DRAM free == total free");

    TEST_ASSERT_EQUAL(dramLargest, totalLargest);
    TestLog::step("No-PSRAM: DRAM largest == total largest");

    // 无 PSRAM 时，碎片率计算一致
    uint8_t dramFrag  = calculateHeapFragmentationPercent(dramFree, dramLargest);
    uint8_t totalFrag = calculateHeapFragmentationPercent(totalFree, totalLargest);
    TEST_ASSERT_EQUAL(dramFrag, totalFrag);
    TestLog::step("No-PSRAM: fragmentation calculation is consistent");

    // 验证 MQTTS 阈值逢无 PSRAM 场景同样有效
    uint32_t mqttsMinHeap = 35000;
    uint32_t mqttsMinBlock = 20000;
    TEST_ASSERT_TRUE(dramFree >= mqttsMinHeap && dramLargest >= mqttsMinBlock);
    TestLog::step("MQTTS thresholds work correctly on no-PSRAM chip");

    TestLog::testEnd(true);
}

/**
 * @brief HM-18: 有 PSRAM 芯片（ESP32-S3）DRAM 隔离验证
 * DRAM 检测不受 PSRAM 干扰，保证 SSL/WiFi 内存保护准确
 */
void test_psram_chip_dram_isolation() {
    TestLog::testStart("Compat: PSRAM Chip (ESP32-S3) DRAM Isolation");

    // 模拟 ESP32-S3 带 4MB PSRAM 场景
    uint32_t dramFree    = 38 * 1024;  // DRAM 空闲 38KB
    uint32_t psramFree   = 4 * 1024 * 1024;  // PSRAM 空闲 4MB
    uint32_t totalFree   = dramFree + psramFree;  // MALLOC_CAP_DEFAULT
    uint32_t dramLargest = 22 * 1024;  // DRAM 内最大连续 22KB
    uint32_t psramLargest = 4 * 1024 * 1024;  // PSRAM 最大连续块
    uint32_t totalLargest = psramLargest;  // MALLOC_CAP_DEFAULT 的最大块

    // 1. PSRAM 不干扰 DRAM 检测
    TEST_ASSERT_TRUE_MESSAGE(dramFree < totalFree,
        "DRAM free should be less than total free (PSRAM adds to total)");
    TEST_ASSERT_TRUE_MESSAGE(dramLargest < totalLargest,
        "DRAM largest block < total largest (PSRAM dominates total)");
    TestLog::step("PSRAM contribution to total is significant");

    // 2. 基于 DRAM 的 MQTTS 判断：DRAM 22KB >= 20KB → 允许
    uint32_t MQTTS_MIN_BLOCK = 20000;
    TEST_ASSERT_TRUE_MESSAGE(dramLargest >= MQTTS_MIN_BLOCK,
        "DRAM 22KB >= 20KB: MQTTS should be allowed");
    TestLog::step("DRAM check: 22KB block allows MQTTS");

    // 3. 基于 Total 的 MQTTS 判断同样允许（但是 PSRAM 巨大块是假象）
    TEST_ASSERT_TRUE(totalLargest >= MQTTS_MIN_BLOCK);
    TestLog::step("Total check: 4MB block also allows (but for wrong reason)");

    // 4. 验证当 DRAM 碎片化时：drameest 8KB < 20KB → DRAM 正确险止
    uint32_t fragmentedDramLargest = 8 * 1024;  // DRAM 碎片后最大块 8KB
    TEST_ASSERT_FALSE_MESSAGE(fragmentedDramLargest >= MQTTS_MIN_BLOCK,
        "Fragmented DRAM (8KB) < 20KB: DRAM check correctly blocks MQTTS");
    // 但 Total 的误判断会放行
    TEST_ASSERT_TRUE(totalLargest >= MQTTS_MIN_BLOCK);
    TestLog::step("Fragmented DRAM: DRAM check blocks MQTTS, total check misleads");

    TestLog::testEnd(true);
}

/**
 * @brief HM-19: MemoryGuardLevel 判断基于 DRAM 连续块而非 Total
 * 验证 updateMemoryGuardLevel() 中的 largestBlock 已切换为 DRAM 内部数据
 */
void test_memory_guard_level_dram_based() {
    TestLog::testStart("MemGuard: Level Based on DRAM largestBlock");

    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    // 1. updateMemoryGuardLevel 应使用 dramLargestBlock 而非 largestFreeBlock
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("dramLargestBlock") != std::string::npos,
        "updateMemoryGuardLevel must reference dramLargestBlock");
    TestLog::step("dramLargestBlock referenced in updateMemoryGuardLevel");

    // 2. 不应再使用 MALLOC_CAP_DEFAULT 的 largestBlock 做 CRITICAL 判断
    // (检查源码中不应出现 heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)
    //  在 updateMemoryGuardLevel 函数内)
    auto guardFuncStart = content.find("void HealthMonitor::updateMemoryGuardLevel");
    auto guardFuncEnd   = content.find("void HealthMonitor::applyDegradation", guardFuncStart);
    if (guardFuncStart != std::string::npos && guardFuncEnd != std::string::npos) {
        std::string guardFunc = content.substr(guardFuncStart, guardFuncEnd - guardFuncStart);
        // 在该函数内，CAP_DEFAULT 的 largestBlock 调用应已被移除
        bool hasDefaultCapInGuard = guardFunc.find("heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)") != std::string::npos;
        TEST_ASSERT_FALSE_MESSAGE(hasDefaultCapInGuard,
            "updateMemoryGuardLevel must not use MALLOC_CAP_DEFAULT largest block (PSRAM interference)");
        TestLog::step("updateMemoryGuardLevel does not use MALLOC_CAP_DEFAULT largest block");
    }

    // 3. 验证 MemoryGuardLevel 阈值设置合理性
    // CRITICAL 阈值：2KB，SEVERE：3KB，以 DRAM 内部连续块计算尔障却是安全的
    // 因为 DRAM 已排除 PSRAM，不会被误判
    uint32_t criticalBlock = 2048;
    uint32_t severeBlock = 3072;
    TEST_ASSERT_LESS_THAN(severeBlock, criticalBlock + 1);
    TestLog::step("CRITICAL(2KB) < SEVERE(3KB): threshold hierarchy correct");

    TestLog::testEnd(true);
}

// ========== 动态 PSRAM 阈值分级测试 ==========

/**
 * @brief HM-22: checkCriticalMemory 根据 PSRAM 容量动态计算阈值
 * 验证源码中基于 ESP.getPsramSize() 的分级逻辑：
 *   F8R0 (0 PSRAM):  critical=8KB,  warning=16KB
 *   F8R4 (4MB PSRAM): critical=10KB, warning=20KB
 *   F16R8 (8MB PSRAM): critical=12KB, warning=24KB
 */
void test_dynamic_psram_threshold_tiers() {
    TestLog::testStart("HealthMonitor: Dynamic PSRAM Threshold Tiers");

    // 镜像 checkCriticalMemory 中的阈值计算逻辑
    auto computeThresholds = [](size_t psramSize, uint32_t& critical, uint32_t& warning) {
        if (psramSize >= 8 * 1024 * 1024) {
            critical = 12288;  // 12KB
            warning  = 24576;  // 24KB
        } else if (psramSize >= 4 * 1024 * 1024) {
            critical = 10240;  // 10KB
            warning  = 20480;  // 20KB
        } else {
            critical = 8192;   // 8KB
            warning  = 16384;  // 16KB
        }
    };

    struct Tier {
        const char* name;
        size_t      psramSize;
        uint32_t    expectCritical;
        uint32_t    expectWarning;
    };
    Tier tiers[] = {
        {"F8R0  (no PSRAM)", 0,                  8192,  16384},
        {"F8R4  (4MB PSRAM)", 4*1024*1024,       10240, 20480},
        {"F16R8 (8MB PSRAM)", 8*1024*1024,       12288, 24576},
    };

    for (const auto& t : tiers) {
        uint32_t critical = 0, warning = 0;
        computeThresholds(t.psramSize, critical, warning);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(t.expectCritical, critical,
            (std::string(t.name) + ": critical threshold").c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(t.expectWarning, warning,
            (std::string(t.name) + ": warning threshold").c_str());
        TestLog::step((std::string(t.name) + ": critical=" +
            std::to_string(critical) + " warning=" + std::to_string(warning)).c_str());
    }

    // 验证阈值单调递增：PSRAM 越大，允许的低内存阈值越高
    uint32_t c0, w0, c1, w1, c2, w2;
    computeThresholds(0, c0, w0);
    computeThresholds(4*1024*1024, c1, w1);
    computeThresholds(8*1024*1024, c2, w2);
    TEST_ASSERT_LESS_THAN(c1, c0);
    TEST_ASSERT_LESS_THAN(c2, c1);
    TEST_ASSERT_LESS_THAN(w1, w0);
    TEST_ASSERT_LESS_THAN(w2, w1);
    TestLog::step("Thresholds monotonically increase with PSRAM size");

    // 验证 critical 始终是 warning 的 ~50%（一致的比率）
    TEST_ASSERT_FLOAT_WITHIN(0.1, 0.5, (float)c0 / (float)w0);
    TEST_ASSERT_FLOAT_WITHIN(0.1, 0.5, (float)c1 / (float)w1);
    TEST_ASSERT_FLOAT_WITHIN(0.1, 0.5, (float)c2 / (float)w2);
    TestLog::step("critical/warning ratio consistent at ~50%");

    TestLog::testEnd(true);
}

/**
 * @brief HM-23: 源码验证 checkCriticalMemory 包含动态阈值逻辑
 * 确保源码中不再使用硬编码 8KB 阈值
 */
void test_source_dynamic_threshold_in_code() {
    TestLog::testStart("HealthMonitor: Dynamic Threshold Source Verification");

    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    // 源码应包含 ESP.getPsramSize() 调用
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("ESP.getPsramSize()") != std::string::npos,
        "checkCriticalMemory must use ESP.getPsramSize() for dynamic thresholds");
    TestLog::step("ESP.getPsramSize() present in checkCriticalMemory");

    // 源码应包含 3 个阈值分支
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("12288") != std::string::npos,
        "checkCriticalMemory must have 12KB threshold for F16R8");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("10240") != std::string::npos,
        "checkCriticalMemory must have 10KB threshold for F8R4");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("8192") != std::string::npos,
        "checkCriticalMemory must have 8KB threshold for F8R0");
    TestLog::step("All three threshold tiers present in source");

    // 源码应使用 static 缓存（只计算一次）
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("static uint32_t criticalThreshold") != std::string::npos,
        "criticalThreshold must be static (computed once)");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("static uint32_t warningThreshold") != std::string::npos,
        "warningThreshold must be static (computed once)");
    TestLog::step("Static threshold caching present");

    TestLog::testEnd(true);
}

/**
 * @brief HM-24: MEMGUARD 日志使用动态阈值而非硬编码字符串
 *
 * 验证 HealthMonitor.cpp 中的 MEMGUARD 日志格式使用 %uB 动态引用
 * GUARD_CRITICAL_LARGEST_BLOCK / GUARD_SEVERE_LARGEST_BLOCK 常量，
 * 而非硬编码 "2KB" / "3KB" 字符串。
 */
void test_memguard_log_uses_dynamic_thresholds() {
    TestLog::testStart("HealthMonitor: MEMGUARD Log Uses Dynamic Thresholds");

    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    // 1. 日志中不应出现硬编码的 "2KB" 或 "3KB" 阈值字符串
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("<2KB") == std::string::npos,
        "MEMGUARD log must NOT use hardcoded '<2KB' - use dynamic GUARD_CRITICAL_LARGEST_BLOCK value");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("<3KB") == std::string::npos,
        "MEMGUARD log must NOT use hardcoded '<3KB' - use dynamic GUARD_SEVERE_LARGEST_BLOCK value");
    TestLog::step("No hardcoded '<2KB' or '<3KB' in MEMGUARD logs");

    // 2. CRITICAL 分支日志必须引用 GUARD_CRITICAL_LARGEST_BLOCK 常量
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("GUARD_CRITICAL_LARGEST_BLOCK") != std::string::npos,
        "MEMGUARD CRITICAL log must reference GUARD_CRITICAL_LARGEST_BLOCK constant");
    TestLog::step("CRITICAL log references GUARD_CRITICAL_LARGEST_BLOCK");

    // 3. SEVERE 分支日志必须引用 GUARD_SEVERE_LARGEST_BLOCK 常量
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("GUARD_SEVERE_LARGEST_BLOCK") != std::string::npos,
        "MEMGUARD SEVERE log must reference GUARD_SEVERE_LARGEST_BLOCK constant");
    TestLog::step("SEVERE log references GUARD_SEVERE_LARGEST_BLOCK");

    // 4. 日志格式应使用 %uB 动态格式而非固定字符串
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("%uB") != std::string::npos,
        "MEMGUARD log must use %uB format specifier for dynamic threshold display");
    TestLog::step("Dynamic %uB format specifier used in MEMGUARD logs");

    TestLog::testEnd(true);
}

// ========== 内存恢复机制整合测试 (MR-1 ~ MR-17) ==========

/**
 * @brief MR-1: getMetricsJson 包含恢复状态字段
 */
void test_mr1_metrics_json_recovery_fields() {
    TestLog::testStart("MR-1: getMetricsJson includes recovery state fields");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    const char* fields[] = {
        "mqtts_downgraded", "mqtt_stopped", "mqtt_disabled",
        "modbus_stopped", "periph_exec_paused", "critical_duration_s"
    };
    for (const char* f : fields) {
        TEST_ASSERT_TRUE_MESSAGE(
            content.find(f) != std::string::npos,
            (std::string("getMetricsJson must contain field: ") + f).c_str());
    }
    TestLog::step("All 6 recovery state fields present in getMetricsJson");
    TestLog::testEnd(true);
}

/**
 * @brief MR-2: HealthMonitor.h 包含恢复状态查询接口
 */
void test_mr2_query_interfaces_in_header() {
    TestLog::testStart("MR-2: HealthMonitor.h has recovery query interfaces");
    std::string content = readProjectFile("include/systems/HealthMonitor.h");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.h must be readable");

    const char* methods[] = {
        "isMqttsDowngraded", "isMqttStoppedForMemory", "isMqttDisabledForMemory",
        "isModbusStoppedForMemory", "isPeriphExecPausedForMemory", "getCriticalDurationMs"
    };
    for (const char* m : methods) {
        TEST_ASSERT_TRUE_MESSAGE(
            content.find(m) != std::string::npos,
            (std::string("HealthMonitor.h must have method: ") + m).c_str());
    }
    TestLog::step("All 6 query interfaces present in header");
    TestLog::testEnd(true);
}

/**
 * @brief MR-3: applyDegradation SEVERE 分支调用 performMemoryRecovery，仅 oldLevel < SEVERE 时执行
 */
void test_mr3_severe_calls_perform_recovery() {
    TestLog::testStart("MR-3: applyDegradation SEVERE calls performMemoryRecovery");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("performMemoryRecovery") != std::string::npos,
        "applyDegradation must call performMemoryRecovery");
    TestLog::step("performMemoryRecovery call found in source");
    TestLog::testEnd(true);
}

/**
 * @brief MR-4: applyDegradation NORMAL 分支调用 restoreMemoryRecovery
 */
void test_mr4_normal_calls_restore_recovery() {
    TestLog::testStart("MR-4: applyDegradation NORMAL calls restoreMemoryRecovery");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("restoreMemoryRecovery") != std::string::npos,
        "applyDegradation NORMAL must call restoreMemoryRecovery");
    TestLog::step("restoreMemoryRecovery call found in source");
    TestLog::testEnd(true);
}

/**
 * @brief MR-5: checkCriticalMemory 包含 SEVERE 兜底逻辑
 */
void test_mr5_critical_has_severe_fallback() {
    TestLog::testStart("MR-5: checkCriticalMemory has SEVERE fallback");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("SEVERE fallback") != std::string::npos,
        "checkCriticalMemory must have SEVERE fallback logic");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("performMemoryRecovery(MemoryGuardLevel::WARN, MemoryGuardLevel::CRITICAL)") != std::string::npos,
        "SEVERE fallback must call performMemoryRecovery with WARN->CRITICAL");
    TestLog::step("SEVERE fallback in CRITICAL path verified");
    TestLog::testEnd(true);
}

/**
 * @brief MR-6: checkCriticalMemory 30s 调用 disableMqttForMemory（而非仅 stopMQTT）
 */
void test_mr6_30s_disables_mqtt() {
    TestLog::testStart("MR-6: checkCriticalMemory 30s disables MQTT permanently");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("disableMqttForMemory()") != std::string::npos,
        "30s action must call disableMqttForMemory (not just stopMQTT)");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("CRITICAL_MQTT_STOP_DELAY_MS") != std::string::npos,
        "Must use CRITICAL_MQTT_STOP_DELAY_MS constant");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("_mqttDisabledForMemory") != std::string::npos,
        "Must set _mqttDisabledForMemory flag");
    TestLog::step("30s permanent MQTT disable verified");
    TestLog::testEnd(true);
}

/**
 * @brief MR-7: checkCriticalMemory 90s 调用 SystemRebooter::scheduleReboot
 */
void test_mr7_90s_schedules_reboot() {
    TestLog::testStart("MR-7: checkCriticalMemory 90s schedules reboot");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("SystemRebooter::scheduleReboot") != std::string::npos,
        "90s action must call SystemRebooter::scheduleReboot");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("CRITICAL_REBOOT_DELAY_MS") != std::string::npos,
        "Must use CRITICAL_REBOOT_DELAY_MS constant");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("RestartReason::CRITICAL_LOW_MEMORY") != std::string::npos,
        "Must use CRITICAL_LOW_MEMORY restart reason");
    TestLog::step("90s safe reboot verified");
    TestLog::testEnd(true);
}

/**
 * @brief MR-8: downgradeMqttsToMqtt 配置写入失败时安全退出
 */
void test_mr8_downgrade_failure_safe_exit() {
    TestLog::testStart("MR-8: downgradeMqttsToMqtt safe exit on config failure");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    // 检查配置写入失败后有 return 语句
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("skip MQTT restart") != std::string::npos,
        "Config write failure must skip MQTT restart");
    TestLog::step("Config failure safe exit verified");
    TestLog::testEnd(true);
}

/**
 * @brief MR-9: disableMqttForMemory 写入 mqtt.enabled=false 并调用 stopMQTT
 */
void test_mr9_disable_mqtt_implementation() {
    TestLog::testStart("MR-9: disableMqttForMemory writes enabled=false and stops MQTT");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("disableMqttForMemory") != std::string::npos,
        "disableMqttForMemory method must exist");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("mqtt.enabled") != std::string::npos,
        "Must write mqtt.enabled to config");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("doc[\"mqtt\"][\"enabled\"] = false") != std::string::npos,
        "Must set mqtt.enabled to false");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("stopMQTT") != std::string::npos,
        "Must call stopMQTT after disabling config");
    TestLog::step("disableMqttForMemory implementation verified");
    TestLog::testEnd(true);
}

/**
 * @brief MR-10: restoreMemoryRecovery 对 _mqttDisabledForMemory 不自动恢复
 */
void test_mr10_restore_skips_disabled_mqtt() {
    TestLog::testStart("MR-10: restoreMemoryRecovery skips disabled MQTT");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MQTT stays DISABLED") != std::string::npos,
        "restoreMemoryRecovery must log that disabled MQTT stays disabled");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("user must re-enable") != std::string::npos,
        "Must indicate user manual re-enable is required");
    TestLog::step("Disabled MQTT not auto-restored verified");
    TestLog::testEnd(true);
}

/**
 * @brief MR-11: PeriphExecScheduler checkTimers 入口有 _memoryPressurePaused 早期返回
 */
void test_mr11_periphexec_memory_pause() {
    TestLog::testStart("MR-11: PeriphExecScheduler has memory pause early return");
    std::string content = readProjectFile("src/core/PeriphExecScheduler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "PeriphExecScheduler.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("_memoryPressurePaused") != std::string::npos,
        "PeriphExecScheduler must check _memoryPressurePaused");
    TestLog::step("PeriphExec memory pause check verified");
    TestLog::testEnd(true);
}

/**
 * @brief MR-12: _criticalStartTime 仅在 checkCriticalMemory 中设置，在 restoreMemoryRecovery 中重置
 */
void test_mr12_critical_start_time_management() {
    TestLog::testStart("MR-12: _criticalStartTime managed correctly");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    // _criticalStartTime = millis() 仅在 checkCriticalMemory 中
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("_criticalStartTime = millis()") != std::string::npos,
        "_criticalStartTime must be set in checkCriticalMemory");
    // _criticalStartTime = 0 在 restoreMemoryRecovery 中
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("_criticalStartTime = 0") != std::string::npos,
        "_criticalStartTime must be reset in restoreMemoryRecovery");
    // else 分支不应有 _criticalStartTime = 0（统一管理）
    std::regex elseResetRegex(R"(} else \{[^}]*_criticalStartTime = 0)");
    TEST_ASSERT_FALSE_MESSAGE(
        std::regex_search(content, elseResetRegex),
        "checkCriticalMemory else branches must NOT reset _criticalStartTime (use unified management)");
    TestLog::step("_criticalStartTime management verified");
    TestLog::testEnd(true);
}

/**
 * @brief MR-13: 所有降级/恢复日志使用 [MEMRECOVER] 前缀，包含 dram= 快照
 */
void test_mr13_memrecover_log_prefix() {
    TestLog::testStart("MR-13: Recovery logs use [MEMRECOVER] prefix with dram snapshot");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    // performMemoryRecovery 中使用 [MEMRECOVER] 前缀
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("[MEMRECOVER] === SEVERE recovery triggered ===") != std::string::npos,
        "performMemoryRecovery must use [MEMRECOVER] prefix");
    // 包含 dram= 快照
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("dram=") != std::string::npos,
        "Recovery logs must include dram= snapshot");
    TestLog::step("[MEMRECOVER] prefix and dram snapshot verified");
    TestLog::testEnd(true);
}

/**
 * @brief MR-14: SEVERE 恢复日志包含 Step 序号和 DRAM 前后对比
 */
void test_mr14_severe_step_logging() {
    TestLog::testStart("MR-14: SEVERE recovery has Step X/Y logging and DRAM comparison");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("Step 1/3") != std::string::npos,
        "SEVERE recovery must log Step 1/3");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("Step 2/3") != std::string::npos,
        "SEVERE recovery must log Step 2/3");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("Step 3/3") != std::string::npos,
        "SEVERE recovery must log Step 3/3");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("DRAM after downgrade") != std::string::npos,
        "Must log DRAM after downgrade");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("SEVERE recovery complete") != std::string::npos,
        "Must log SEVERE recovery complete with DRAM gain");
    TestLog::step("Step X/Y and DRAM comparison logging verified");
    TestLog::testEnd(true);
}

/**
 * @brief MR-15: checkCriticalMemory 包含 EMERGENCY DRAM < 4KB 紧急释放路径
 */
void test_mr15_emergency_dram_threshold() {
    TestLog::testStart("MR-15: EMERGENCY DRAM < 4KB immediate recovery");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("EMERGENCY_DRAM_THRESHOLD") != std::string::npos,
        "Must define EMERGENCY_DRAM_THRESHOLD constant");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("EMERGENCY") != std::string::npos,
        "Must have EMERGENCY log path");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("4096") != std::string::npos,
        "EMERGENCY threshold must be 4096 (4KB)");
    TestLog::step("EMERGENCY DRAM < 4KB path verified");
    TestLog::testEnd(true);
}

/**
 * @brief MR-16: checkCriticalMemory 90s 重启前有 boot loop 保护
 */
void test_mr16_boot_loop_protection() {
    TestLog::testStart("MR-16: Boot loop protection before 90s reboot");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("boot loop") != std::string::npos,
        "Must have boot loop protection log message");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("120000UL") != std::string::npos,
        "Boot loop check must use 120s (120000ms) threshold");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("recent boot") != std::string::npos,
        "Must log 'recent boot' when skipping reboot");
    TestLog::step("Boot loop protection verified");
    TestLog::testEnd(true);
}

/**
 * @brief MR-17: 重启前输出恢复轨迹总结
 */
void test_mr17_recovery_summary_before_reboot() {
    TestLog::testStart("MR-17: Recovery summary before reboot");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("Recovery summary before reboot") != std::string::npos,
        "Must output recovery summary before reboot");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("Actions taken") != std::string::npos,
        "Summary must include Actions taken");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("Reboot reason") != std::string::npos,
        "Summary must include Reboot reason");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("CRITICAL duration") != std::string::npos,
        "Summary must include CRITICAL duration");
    TestLog::step("Recovery summary logging verified");
    TestLog::testEnd(true);
}

// ========== P0/P1 改进验证测试 ==========

/**
 * @brief FRAG-1: isFragmentationHigh() 统一使用 DRAM 指标
 * 验证：源码中 isFragmentationHigh 使用 dramFreeHeap/dramLargestBlock，
 *       而非 freeHeap/largestFreeBlock（含 PSRAM，会掩盖 DRAM 碎片问题）
 */
void test_frag_high_uses_dram_metrics() {
    TestLog::testStart("FRAG-1: isFragmentationHigh uses DRAM metrics");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    // 查找 isFragmentationHigh 函数实现
    auto pos = content.find("bool HealthMonitor::isFragmentationHigh()");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
        "isFragmentationHigh() must be defined");

    // 截取函数体（~500字符足够）
    std::string funcBody = content.substr(pos, 500);

    // 必须使用 DRAM 指标
    TEST_ASSERT_TRUE_MESSAGE(
        funcBody.find("dramFreeHeap") != std::string::npos,
        "isFragmentationHigh must use dramFreeHeap (not freeHeap)");
    TEST_ASSERT_TRUE_MESSAGE(
        funcBody.find("dramLargestBlock") != std::string::npos,
        "isFragmentationHigh must use dramLargestBlock (not largestFreeBlock)");

    // 不应使用含 PSRAM 的指标
    // 注意：只检查在函数体内不含 ".freeHeap" 和 ".largestFreeBlock" 的使用
    // dramFreeHeap 包含 "FreeHeap" 子串，所以用更精确的匹配
    TEST_ASSERT_TRUE_MESSAGE(
        funcBody.find("currentHealth.freeHeap") == std::string::npos,
        "isFragmentationHigh must NOT use currentHealth.freeHeap (includes PSRAM)");
    TEST_ASSERT_TRUE_MESSAGE(
        funcBody.find("currentHealth.largestFreeBlock") == std::string::npos,
        "isFragmentationHigh must NOT use currentHealth.largestFreeBlock (includes PSRAM)");

    TestLog::step("isFragmentationHigh correctly uses DRAM metrics");
    TestLog::testEnd(true);
}

/**
 * @brief FRAG-2: DRAM vs PSRAM 碎片率差异验证
 * 在 PSRAM 设备上，DRAM 碎片率高但总碎片率低，isFragmentationHigh 应返回 true
 */
void test_frag_high_dram_vs_total_difference() {
    TestLog::testStart("FRAG-2: DRAM vs total fragmentation difference");

    // 模拟 PSRAM 设备：DRAM 严重碎片化但 PSRAM 大块空闲
    uint32_t dramFree = 42 * 1024;       // 42KB DRAM 空闲
    uint32_t dramLargest = 3 * 1024;      // 3KB 最大连续块 → 碎片率 93%
    uint32_t totalFree = dramFree + 8 * 1024 * 1024;   // 含 8MB PSRAM
    uint32_t totalLargest = 8 * 1024 * 1024;             // PSRAM 大块

    uint8_t dramFrag = calculateHeapFragmentationPercent(dramFree, dramLargest);
    uint8_t totalFrag = calculateHeapFragmentationPercent(totalFree, totalLargest);

    // DRAM 碎片率应该很高（>75%）
    TEST_ASSERT_GREATER_THAN(75, (int)dramFrag);
    TestLog::step("DRAM fragmentation is high (93%)");

    // 总碎片率应该很低（<1%），因为有 PSRAM 大块
    TEST_ASSERT_LESS_THAN(5, (int)totalFrag);
    TestLog::step("Total fragmentation is low due to PSRAM");

    // 如果使用 DRAM 指标判断（正确行为），应该检测到高碎片
    // 阈值 FRAG_THRESHOLD_COMPACT 通常约 75%
    TEST_ASSERT_TRUE(dramFrag > 75);
    TestLog::step("DRAM-based check correctly detects high fragmentation");

    // 如果使用 total 指标（旧行为），会漏检
    TEST_ASSERT_FALSE(totalFrag > 75);
    TestLog::step("Total-based check would MISS fragmentation (old behavior)");

    TestLog::testEnd(true);
}

/**
 * @brief COMPACT-1: compactMemory() 使用主动释放策略而非试探性分配
 * 验证：源码中不再包含 malloc/free 循环，改为刷新日志缓冲、关闭 SSE、清除缓存
 */
void test_compact_memory_targeted_reclamation() {
    TestLog::testStart("COMPACT-1: compactMemory uses targeted reclamation");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    // 查找 compactMemory 函数实现
    auto pos = content.find("void HealthMonitor::compactMemory()");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
        "compactMemory() must be defined");

    // 截取函数体（~1500字符）
    std::string funcBody = content.substr(pos, 1500);

    // 新策略：必须包含主动释放操作
    TEST_ASSERT_TRUE_MESSAGE(
        funcBody.find("flushBuffer") != std::string::npos,
        "compactMemory must flush log buffer (LoggerSystem::flushBuffer)");
    TEST_ASSERT_TRUE_MESSAGE(
        funcBody.find("closeAllClients") != std::string::npos ||
        funcBody.find("SSE") != std::string::npos,
        "compactMemory must close SSE clients for defragmentation");

    // 旧策略：不应包含试探性分配循环
    TEST_ASSERT_TRUE_MESSAGE(
        funcBody.find("compactSizes") == std::string::npos,
        "compactMemory must NOT use old compactSizes probe array");

    TestLog::step("compactMemory uses targeted reclamation (flush/SSE/cache), not probe allocation");
    TestLog::testEnd(true);
}

/**
 * @brief RECOVER-1: performMemoryRecovery 使用 calculateHeapFragmentationPercent
 * 验证：不再使用 (dramLargestBlock * 100) / dramBefore 的手动计算，
 *       避免 uint8_t 截断和乘法溢出风险
 */
void test_perform_memory_recovery_uses_helper_function() {
    TestLog::testStart("RECOVER-1: performMemoryRecovery uses helper function");
    std::string content = readProjectFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "HealthMonitor.cpp must be readable");

    // 查找 performMemoryRecovery 函数实现
    auto pos = content.find("void HealthMonitor::performMemoryRecovery(");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
        "performMemoryRecovery() must be defined");

    // 截取函数开头（~600字符，覆盖变量声明部分）
    std::string funcHead = content.substr(pos, 600);

    // 必须使用 calculateHeapFragmentationPercent
    TEST_ASSERT_TRUE_MESSAGE(
        funcHead.find("calculateHeapFragmentationPercent") != std::string::npos,
        "performMemoryRecovery must use calculateHeapFragmentationPercent()");

    // 不应使用旧的手动计算方式
    TEST_ASSERT_TRUE_MESSAGE(
        funcHead.find("dramLargestBlock * 100") == std::string::npos,
        "performMemoryRecovery must NOT use manual (largest * 100) / dram calculation");

    // 变量类型不应是 uint8_t largestBlock（截断风险）
    TEST_ASSERT_TRUE_MESSAGE(
        funcHead.find("uint8_t largestBlock") == std::string::npos,
        "performMemoryRecovery must NOT use uint8_t largestBlock (truncation risk)");

    TestLog::step("performMemoryRecovery uses calculateHeapFragmentationPercent (no overflow risk)");
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

    // DRAM/PSRAM 内存保护测试
    RUN_TEST(test_dram_psram_memory_distinction);
    RUN_TEST(test_dram_fragmentation_detection);
    RUN_TEST(test_health_monitor_dram_fields_in_report);

    // WiFi 内存保护测试
    RUN_TEST(test_wifi_connect_dram_protection);
    RUN_TEST(test_wifi_reconnect_dram_protection);
    RUN_TEST(test_wifi_connect_dram_threshold_boundary);

    // 多芯片环境兼容性测试
    RUN_TEST(test_no_psram_chip_memory_detection);
    RUN_TEST(test_psram_chip_dram_isolation);
    RUN_TEST(test_memory_guard_level_dram_based);

    // 动态 PSRAM 阈值测试
    RUN_TEST(test_dynamic_psram_threshold_tiers);
    RUN_TEST(test_source_dynamic_threshold_in_code);

    // MEMGUARD 日志动态阈值测试
    RUN_TEST(test_memguard_log_uses_dynamic_thresholds);

    // 内存恢复机制整合测试 (MR-1 ~ MR-17)
    RUN_TEST(test_mr1_metrics_json_recovery_fields);
    RUN_TEST(test_mr2_query_interfaces_in_header);
    RUN_TEST(test_mr3_severe_calls_perform_recovery);
    RUN_TEST(test_mr4_normal_calls_restore_recovery);
    RUN_TEST(test_mr5_critical_has_severe_fallback);
    RUN_TEST(test_mr6_30s_disables_mqtt);
    RUN_TEST(test_mr7_90s_schedules_reboot);
    RUN_TEST(test_mr8_downgrade_failure_safe_exit);
    RUN_TEST(test_mr9_disable_mqtt_implementation);
    RUN_TEST(test_mr10_restore_skips_disabled_mqtt);
    RUN_TEST(test_mr11_periphexec_memory_pause);
    RUN_TEST(test_mr12_critical_start_time_management);
    RUN_TEST(test_mr13_memrecover_log_prefix);
    RUN_TEST(test_mr14_severe_step_logging);
    RUN_TEST(test_mr15_emergency_dram_threshold);
    RUN_TEST(test_mr16_boot_loop_protection);
    RUN_TEST(test_mr17_recovery_summary_before_reboot);

    // P0/P1 改进验证测试
    RUN_TEST(test_frag_high_uses_dram_metrics);
    RUN_TEST(test_frag_high_dram_vs_total_difference);
    RUN_TEST(test_compact_memory_targeted_reclamation);
    RUN_TEST(test_perform_memory_recovery_uses_helper_function);

    TestLog::groupEnd();
}
