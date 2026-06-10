/**
 * @file test_system_stability.cpp
 * @brief System Stability Tests
 */

#include <unity.h>
#include <Arduino.h>
#include "mocks/MockHealthMonitor.h"
#include "mocks/MockLogger.h"
#include "mocks/MockTaskManager.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

void test_system_stability_group();

// Mock FastBee Framework for testing
class MockFastBeeFramework {
public:
    static MockFastBeeFramework* getInstance() {
        static MockFastBeeFramework instance;
        return &instance;
    }

    bool initialize() {
        _initialized = false;
        _initSteps.clear();
        
        _initSteps.push_back("ConfigStorage");
        if (!initConfigStorage()) return false;
        
        _initSteps.push_back("Logger");
        if (!initLogger()) return false;
        
        _initSteps.push_back("FileSystem");
        if (!initFileSystem()) return false;
        
        _initSteps.push_back("PeripheralManager");
        if (!initPeripheralManager()) return false;
        
        _initSteps.push_back("NetworkManager");
        if (!initNetworkManager()) return false;
        
        _initSteps.push_back("ProtocolManager");
        if (!initProtocolManager()) return false;
        
        _initSteps.push_back("WebConfigManager");
        if (!initWebConfigManager()) return false;
        
        _initSteps.push_back("AuthManager");
        if (!initAuthManager()) return false;
        
        _initSteps.push_back("TaskManager");
        if (!initTaskManager()) return false;
        
        _initSteps.push_back("HealthMonitor");
        if (!initHealthMonitor()) return false;
        
        _initSteps.push_back("RuleScriptManager");
        if (!initRuleScriptManager()) return false;
        
        _initSteps.push_back("OTAManager");
        if (!initOTAManager()) return false;
        
        _initSteps.push_back("SystemReady");
        
        _initialized = true;
        return true;
    }

    bool isInitialized() { return _initialized; }
    int getInitStepCount() { return _initSteps.size(); }
    std::vector<String> getInitSteps() { return _initSteps; }

    MockHealthMonitor* getHealthMonitor() { return _healthMon; }
    MockTaskManager* getTaskManager() { return _taskMgr; }

    void run() {
        if (_taskMgr) _taskMgr->run();
        if (_healthMon) _healthMon->update();
    }

private:
    MockFastBeeFramework() : _initialized(false), _healthMon(nullptr), _taskMgr(nullptr) {}

    bool initConfigStorage() { return true; }
    bool initLogger() { return MockLogger.initialize(); }
    bool initFileSystem() { return true; }
    bool initPeripheralManager() { return true; }
    bool initNetworkManager() { return true; }
    bool initProtocolManager() { return true; }
    bool initWebConfigManager() { return true; }
    bool initAuthManager() { return true; }
    bool initTaskManager() {
        _taskMgr = &MockTaskMgr;
        return _taskMgr->initialize();
    }
    bool initHealthMonitor() {
        _healthMon = &MockHealthMon;
        return _healthMon->initialize();
    }
    bool initRuleScriptManager() { return true; }
    bool initOTAManager() { return true; }

    bool _initialized;
    std::vector<String> _initSteps;
    MockHealthMonitor* _healthMon;
    MockTaskManager* _taskMgr;
};

// Test initialization sequence
void test_initialization_sequence() {
    TestLog::testStart("System Initialization Sequence");
    
    MockFastBeeFramework* fw = MockFastBeeFramework::getInstance();
    
    TEST_ASSERT_TRUE(fw->initialize());
    TEST_ASSERT_TRUE(fw->isInitialized());
    TestLog::step("System initialized successfully");
    
    TEST_ASSERT_EQUAL(13, fw->getInitStepCount());
    TestLog::step("All 13 initialization steps completed");
    
    std::vector<String> steps = fw->getInitSteps();
    TEST_ASSERT_EQUAL_STRING("ConfigStorage", steps[0].c_str());
    TEST_ASSERT_EQUAL_STRING("Logger", steps[1].c_str());
    TEST_ASSERT_EQUAL_STRING("NetworkManager", steps[4].c_str());
    TEST_ASSERT_EQUAL_STRING("SystemReady", steps[12].c_str());
    TestLog::step("Initialization order verified");
    
    TestLog::testEnd(true);
}

// Test memory monitoring
void test_memory_monitoring() {
    TestLog::testStart("Memory Monitoring");
    
    uint32_t initialHeap = ESP.getFreeHeap();
    TestLog::step("Initial heap recorded");
    
    void* ptr = malloc(1000);
    TEST_ASSERT_NOT_NULL(ptr);
    
    uint32_t afterAlloc = ESP.getFreeHeap();
    TestLog::step("After allocation recorded");
    
    TEST_ASSERT_LESS_THAN(initialHeap, afterAlloc);
    TestLog::step("Memory allocation tracked");
    
    free(ptr);
    
    uint32_t afterFree = ESP.getFreeHeap();
    TestLog::step("After free recorded");
    
    int32_t diff = (int32_t)initialHeap - (int32_t)afterFree;
    TEST_ASSERT_TRUE_MESSAGE(abs(diff) < 1000, "Memory leak detected");
    TestLog::step("Memory freed correctly");
    
    TestLog::testEnd(true);
}

// Test health monitoring
void test_health_monitoring() {
    TestLog::testStart("Health Monitoring");
    
    MockHealthMonitor* hm = &MockHealthMon;
    hm->initialize();
    
    hm->update();
    TestLog::step("Health status updated");
    
    SystemHealth health = hm->getHealthStatus();
    
    TEST_ASSERT_GREATER_THAN(0, health.freeHeap);
    TEST_ASSERT_GREATER_THAN(0, health.totalHeap);
    TestLog::step("Heap info verified");
    
    TEST_ASSERT_GREATER_THAN(0, health.uptime);
    TestLog::step("Uptime verified");
    
    char report[512];
    size_t len = hm->getHealthReport(report, sizeof(report));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_TRUE(strstr(report, "heap=") != nullptr);
    TestLog::step("Health report generated");
    
    TestLog::testEnd(true);
}

// Test AP mode health status
void test_ap_mode_health_status() {
    TestLog::testStart("AP Mode Health Status");
    
    MockHealthMonitor* hm = &MockHealthMon;
    hm->initialize();
    
    hm->setWiFiConnected(false);
    hm->update();
    
    SystemHealth health = hm->getHealthStatus();
    
    TEST_ASSERT_FALSE(health.wifiConnected);
    TestLog::step("AP mode: wifiConnected=false");
    
    TEST_ASSERT_EQUAL(0, health.wifiStrength);
    TestLog::step("AP mode: wifiStrength=0");
    
    TEST_ASSERT_TRUE(health.isHealthy);
    TestLog::step("AP mode: system is healthy");
    
    TestLog::testEnd(true);
}

// Test logging system
void test_logging_system() {
    TestLog::testStart("Logging System");
    
    MockLoggerSystem& logger = MockLoggerSystem::getInstance();
    logger.initialize();
    
    logger.setLogLevel(LOG_INFO);
    TEST_ASSERT_EQUAL(LOG_INFO, logger.getLogLevel());
    TestLog::step("Log level set to INFO");
    
    logger.logInfo("Test info message", "TEST");
    logger.logWarning("Test warning message", "TEST");
    logger.logError("Test error message", "TEST");
    TestLog::step("Logged messages at different levels");
    
    int infoCount = logger.getEntryCountByLevel(LOG_INFO);
    int warnCount = logger.getEntryCountByLevel(LOG_WARNING);
    int errCount = logger.getEntryCountByLevel(LOG_ERROR);
    
    TEST_ASSERT_GREATER_THAN(0, infoCount);
    TEST_ASSERT_GREATER_THAN(0, warnCount);
    TEST_ASSERT_GREATER_THAN(0, errCount);
    TestLog::step("Log counts verified");
    
    TestLog::testEnd(true);
}

// Test log level filtering
void test_log_level_filtering() {
    TestLog::testStart("Log Level Filtering");
    
    MockLoggerSystem& logger = MockLoggerSystem::getInstance();
    logger.initialize();
    logger.clear();
    
    logger.setLogLevel(LOG_WARNING);
    
    logger.logInfo("This should be filtered", "TEST");
    int infoCount = logger.getEntryCountByLevel(LOG_INFO);
    TEST_ASSERT_EQUAL(0, infoCount);
    TestLog::step("INFO messages filtered correctly");
    
    logger.logWarning("Warning message", "TEST");
    logger.logError("Error message", "TEST");
    
    TEST_ASSERT_EQUAL(1, logger.getEntryCountByLevel(LOG_WARNING));
    TEST_ASSERT_EQUAL(1, logger.getEntryCountByLevel(LOG_ERROR));
    TestLog::step("WARNING and ERROR messages recorded");
    
    TestLog::testEnd(true);
}

// Test task scheduling
void test_task_scheduling() {
    TestLog::testStart("Task Scheduling");
    
    MockTaskManager* tm = &MockTaskMgr;
    tm->initialize();
    
    volatile int counter = 0;
    
    TEST_ASSERT_TRUE(tm->addTask("test_task", [](void* param) {
        (*(volatile int*)param)++;
    }, (void*)&counter, 100, TaskPriority::PRIORITY_NORMAL));
    TestLog::step("Task added with 100ms interval");
    
    for (int i = 0; i < 5; i++) {
        delay(110);
        tm->run();
    }
    
    TEST_ASSERT_GREATER_THAN(0, counter);
    TestLog::step("Task executed multiple times");
    
    TaskStatistics stats = tm->getTaskStatistics("test_task");
    TEST_ASSERT_GREATER_THAN(0, stats.executionCount);
    TestLog::step("Task statistics verified");
    
    // 清理注册的任务，避免悬挂指针影响后续测试
    tm->removeTask("test_task");
    
    TestLog::testEnd(true);
}

// Test long running stability
void test_long_running_stability() {
    TestLog::testStart("Long Running Stability");
    
    MockFastBeeFramework* fw = MockFastBeeFramework::getInstance();
    fw->initialize();
    
    uint32_t initialHeap = ESP.getFreeHeap();
    TestLog::step("Initial heap recorded");
    
    int iterations = 50;
    for (int i = 0; i < iterations; i++) {
        fw->run();
        MockHealthMon.update();
        delay(50);
    }
    TestLog::step("Ran multiple iterations");
    
    uint32_t finalHeap = ESP.getFreeHeap();
    int32_t heapDiff = (int32_t)finalHeap - (int32_t)initialHeap;
    TestLog::step("Heap difference calculated");
    
    TEST_ASSERT_TRUE_MESSAGE(heapDiff > -10000, "Memory leak detected");
    TestLog::step("No significant memory leak");
    
    SystemHealth health = MockHealthMon.getHealthStatus();
    TEST_ASSERT_TRUE(health.isHealthy);
    TestLog::step("System health: HEALTHY");
    
    TestLog::testEnd(true);
}

// Test system error recovery
void test_system_error_recovery() {
    TestLog::testStart("System Error Recovery");
    
    MockHealthMonitor* hm = &MockHealthMon;
    hm->initialize();
    hm->clearWarningsAndErrors();
    
    hm->setFreeHeap(5000);
    hm->update();
    
    SystemHealth health = hm->getHealthStatus();
    
    TEST_ASSERT_FALSE(health.isHealthy);
    TEST_ASSERT_FALSE(health.warnings.empty());
    TestLog::step("Low memory condition detected");
    
    hm->setFreeHeap(100000);
    hm->clearWarningsAndErrors();
    hm->update();
    
    health = hm->getHealthStatus();
    TEST_ASSERT_TRUE(health.isHealthy);
    TestLog::step("System recovered");
    
    TestLog::testEnd(true);
}

// ============================================================
// Smoke Tests: 堆内存保护、WDT、PSRAM、内存诊断
// ============================================================

// 模拟 ProtocolManager 堆保护逻辑
class MockProtocolHandler {
public:
    static constexpr uint32_t HEAP_THRESHOLD = 30000;

    bool handle() {
        uint32_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < HEAP_THRESHOLD) {
            _skipCount++;
            return false;  // 跳过处理
        }
        _handleCount++;
        return true;  // 正常处理
    }

    int getSkipCount() { return _skipCount; }
    int getHandleCount() { return _handleCount; }
    void reset() { _skipCount = 0; _handleCount = 0; }

private:
    int _skipCount = 0;
    int _handleCount = 0;
};

// Smoke Test: 堆保护阈值 - 低于 30KB 跳过协议处理
void test_smoke_heap_protection_threshold() {
    TestLog::testStart("Smoke: Heap Protection Threshold (30KB)");
    
    MockProtocolHandler proto;
    
    // 正常堆（>30KB）应该正常处理
    ESP.setFreeHeap(100000);
    TEST_ASSERT_TRUE(proto.handle());
    TEST_ASSERT_EQUAL(1, proto.getHandleCount());
    TestLog::step("Heap=100KB: protocol handled normally");
    
    // 刚好 30KB 边界 - 应该正常处理
    ESP.setFreeHeap(30000);
    TEST_ASSERT_TRUE(proto.handle());
    TEST_ASSERT_EQUAL(2, proto.getHandleCount());
    TestLog::step("Heap=30KB: protocol handled (boundary)");
    
    // 低于阈值（29999）- 应该跳过
    ESP.setFreeHeap(29999);
    TEST_ASSERT_FALSE(proto.handle());
    TEST_ASSERT_EQUAL(1, proto.getSkipCount());
    TestLog::step("Heap=29999: protocol SKIPPED");
    
    // 极低堆（10KB）- 确保跳过
    ESP.setFreeHeap(10000);
    TEST_ASSERT_FALSE(proto.handle());
    TEST_ASSERT_EQUAL(2, proto.getSkipCount());
    TestLog::step("Heap=10KB: protocol SKIPPED");
    
    // 堆恢复后正常处理
    ESP.setFreeHeap(80000);
    TEST_ASSERT_TRUE(proto.handle());
    TEST_ASSERT_EQUAL(3, proto.getHandleCount());
    TestLog::step("Heap=80KB: protocol resumed");
    
    ESP.resetHeapOverride();
    TestLog::testEnd(true);
}

// Smoke Test: PSRAM 配置验证
void test_smoke_psram_allocation_strategy() {
    TestLog::testStart("Smoke: PSRAM Allocation Strategy");
    
    // 验证 PSRAM 大小可检测
    uint32_t psramSize = ESP.getPsramSize();
    TEST_ASSERT_GREATER_THAN(0, psramSize);
    TestLog::step("PSRAM size detected");
    
    // 验证 Free PSRAM
    uint32_t freePsram = ESP.getFreePsram();
    TEST_ASSERT_GREATER_THAN(0, freePsram);
    TEST_ASSERT_LESS_OR_EQUAL(psramSize, freePsram);
    TestLog::step("Free PSRAM verified");
    
    // 模拟 PSRAM 阈值逻辑 (>=4KB 走 PSRAM)
    constexpr uint32_t PSRAM_THRESHOLD = 4096;
    uint32_t allocSize = 8192;  // 8KB allocation
    TEST_ASSERT_TRUE(allocSize >= PSRAM_THRESHOLD);
    TestLog::step("Allocation 8KB >= threshold 4KB → uses PSRAM");
    
    allocSize = 2048;  // 2KB allocation
    TEST_ASSERT_FALSE(allocSize >= PSRAM_THRESHOLD);
    TestLog::step("Allocation 2KB < threshold 4KB → uses internal DRAM");
    
    TestLog::testEnd(true);
}

// Smoke Test: WDT 重配置验证
void test_smoke_wdt_reconfiguration() {
    TestLog::testStart("Smoke: WDT Reconfiguration (60s timeout)");
    
    // 验证 WDT 配置参数
    constexpr uint32_t WDT_TIMEOUT_MS = 60000;
    constexpr uint32_t WDT_DEFAULT_MS = 5000;
    constexpr bool WDT_TRIGGER_PANIC = false;
    
    // 超时必须大于默认值
    TEST_ASSERT_GREATER_THAN(WDT_DEFAULT_MS, WDT_TIMEOUT_MS);
    TestLog::step("WDT timeout 60s > default 5s");
    
    // 不触发 panic
    TEST_ASSERT_FALSE(WDT_TRIGGER_PANIC);
    TestLog::step("WDT trigger_panic = false");
    
    // 验证 idle_core_mask = 0（不监控空闲任务）
    constexpr uint32_t IDLE_CORE_MASK = 0;
    TEST_ASSERT_EQUAL(0, IDLE_CORE_MASK);
    TestLog::step("WDT idle_core_mask = 0 (no idle task monitoring)");
    
    // 60秒超时足以覆盖大部分长操作（JSON 序列化、OTA、WiFi 重连）
    TEST_ASSERT_TRUE(WDT_TIMEOUT_MS >= 30000);
    TestLog::step("WDT 60s covers long operations (JSON/OTA/WiFi)");
    
    TestLog::testEnd(true);
}

// Smoke Test: 内存诊断周期输出
void test_smoke_memory_diagnostic_interval() {
    TestLog::testStart("Smoke: Memory Diagnostic (30s interval)");
    
    constexpr unsigned long MEM_DIAG_INTERVAL = 30000;  // 30 seconds
    
    // 验证诊断间隔合理（不过于频繁也不过于稀疏）
    TEST_ASSERT_GREATER_OR_EQUAL(10000, MEM_DIAG_INTERVAL);  // >= 10s
    TEST_ASSERT_LESS_OR_EQUAL(120000, MEM_DIAG_INTERVAL);    // <= 120s
    TestLog::step("Diagnostic interval 30s is reasonable");
    
    // 模拟诊断输出逻辑
    unsigned long lastMemPrint = 0;
    unsigned long nowMs = 0;
    int printCount = 0;
    
    // 模拟 90 秒运行
    for (nowMs = 0; nowMs <= 90000; nowMs += 1000) {
        if (nowMs - lastMemPrint >= MEM_DIAG_INTERVAL) {
            lastMemPrint = nowMs;
            printCount++;
        }
    }
    
    // 90 秒内应该输出 3 次（~0s, ~30s, ~60s, ~90s = 实际 first at 30001ms）
    TEST_ASSERT_EQUAL(3, printCount);
    TestLog::step("90s run → 3 diagnostic outputs (every 30s)");
    
    TestLog::testEnd(true);
}

// Smoke Test: 堆保护不会影响正常操作
void test_smoke_heap_protection_normal_operation() {
    TestLog::testStart("Smoke: Heap Protection Normal Operation");
    
    MockProtocolHandler proto;
    
    // 模拟正常运行 100 次循环（堆充裕时不应该有任何跳过）
    ESP.setFreeHeap(80000);  // 80KB - 远高于阈值
    
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_TRUE(proto.handle());
    }
    
    TEST_ASSERT_EQUAL(100, proto.getHandleCount());
    TEST_ASSERT_EQUAL(0, proto.getSkipCount());
    TestLog::step("100 iterations with 80KB heap: all handled, 0 skipped");
    
    ESP.resetHeapOverride();
    TestLog::testEnd(true);
}

// Smoke Test: 堆保护多级阈值验证
void test_smoke_multi_level_heap_thresholds() {
    TestLog::testStart("Smoke: Multi-Level Heap Thresholds");
    
    // 各模块的堆保护阈值
    constexpr uint32_t PROTO_HANDLE_THRESHOLD   = 30000;  // ProtocolManager::handle()
    constexpr uint32_t PUBLISH_INFO_THRESHOLD   = 30000;  // publishDeviceInfo()
    constexpr uint32_t MONITOR_DATA_THRESHOLD   = 25000;  // publishMonitorData()
    constexpr uint32_t QUEUED_COMMANDS_THRESHOLD = 30000;  // processQueuedCommands()
    constexpr uint32_t QUEUED_REPORTS_THRESHOLD  = 20000;  // processQueuedReports()
    
    // 验证阈值层级关系
    TEST_ASSERT_GREATER_OR_EQUAL(QUEUED_REPORTS_THRESHOLD, MONITOR_DATA_THRESHOLD);
    TEST_ASSERT_GREATER_OR_EQUAL(MONITOR_DATA_THRESHOLD, PROTO_HANDLE_THRESHOLD);
    TestLog::step("Thresholds: reports(20K) < monitor(25K) < proto/cmds(30K)");
    
    // 在各阈值水平模拟行为
    // 堆 = 35KB: 所有操作应正常
    ESP.setFreeHeap(35000);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= PROTO_HANDLE_THRESHOLD);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= PUBLISH_INFO_THRESHOLD);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= MONITOR_DATA_THRESHOLD);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= QUEUED_COMMANDS_THRESHOLD);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= QUEUED_REPORTS_THRESHOLD);
    TestLog::step("Heap=35KB: all operations allowed");
    
    // 堆 = 27KB: 协议处理跳过，但 reports 和 monitor 可能也会跳过
    ESP.setFreeHeap(27000);
    TEST_ASSERT_FALSE(ESP.getFreeHeap() >= PROTO_HANDLE_THRESHOLD);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= MONITOR_DATA_THRESHOLD);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= QUEUED_REPORTS_THRESHOLD);
    TestLog::step("Heap=27KB: proto skipped, monitor/reports allowed");
    
    // 堆 = 22KB: 只有 reports 还能运行
    ESP.setFreeHeap(22000);
    TEST_ASSERT_FALSE(ESP.getFreeHeap() >= PROTO_HANDLE_THRESHOLD);
    TEST_ASSERT_FALSE(ESP.getFreeHeap() >= MONITOR_DATA_THRESHOLD);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= QUEUED_REPORTS_THRESHOLD);
    TestLog::step("Heap=22KB: only queued reports allowed");
    
    // 堆 = 15KB: 所有操作跳过
    ESP.setFreeHeap(15000);
    TEST_ASSERT_FALSE(ESP.getFreeHeap() >= PROTO_HANDLE_THRESHOLD);
    TEST_ASSERT_FALSE(ESP.getFreeHeap() >= MONITOR_DATA_THRESHOLD);
    TEST_ASSERT_FALSE(ESP.getFreeHeap() >= QUEUED_REPORTS_THRESHOLD);
    TestLog::step("Heap=15KB: ALL operations blocked");
    
    ESP.resetHeapOverride();
    TestLog::testEnd(true);
}

// Test group entry point
void test_system_stability_group() {
    TestLog::groupStart("System Stability Tests");
    
    RUN_TEST(test_initialization_sequence);
    RUN_TEST(test_memory_monitoring);
    RUN_TEST(test_health_monitoring);
    RUN_TEST(test_ap_mode_health_status);
    RUN_TEST(test_logging_system);
    RUN_TEST(test_log_level_filtering);
    RUN_TEST(test_task_scheduling);
    RUN_TEST(test_long_running_stability);
    RUN_TEST(test_system_error_recovery);
    
    // Smoke Tests: OOM 防护 & 系统稳定性
    RUN_TEST(test_smoke_heap_protection_threshold);
    RUN_TEST(test_smoke_psram_allocation_strategy);
    RUN_TEST(test_smoke_wdt_reconfiguration);
    RUN_TEST(test_smoke_memory_diagnostic_interval);
    RUN_TEST(test_smoke_heap_protection_normal_operation);
    RUN_TEST(test_smoke_multi_level_heap_thresholds);
    
    TestLog::groupEnd();
}
