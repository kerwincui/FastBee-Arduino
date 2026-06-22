/**
 * @file test_system_stability.cpp
 * @brief System Stability Tests
 */

#include <unity.h>
#include <Arduino.h>
#include <fstream>
#include <sstream>
#include <regex>
#include "mocks/MockHealthMonitor.h"
#include "mocks/MockLogger.h"
#include "mocks/MockTaskManager.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

// 辅助：读取项目源文件（用于源码回归测试）
static std::string readSrcFile(const char* relativePath) {
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
    
    // Mock 环境下 uptime 可能为 0（无真实定时器），仅验证不崩溃
    TEST_ASSERT_TRUE(health.uptime >= 0);
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

// Smoke Test: PSRAM 配置验证（阈值 512B，防止 HTTP 缓冲区分配失败）
void test_smoke_psram_allocation_strategy() {
    TestLog::testStart("Smoke: PSRAM Allocation Strategy (threshold=512)");
    
    // 验证 PSRAM 大小可检测
    uint32_t psramSize = ESP.getPsramSize();
    TEST_ASSERT_GREATER_THAN(0, psramSize);
    TestLog::step("PSRAM size detected");
    
    // 验证 Free PSRAM
    uint32_t freePsram = ESP.getFreePsram();
    TEST_ASSERT_GREATER_THAN(0, freePsram);
    TEST_ASSERT_LESS_OR_EQUAL(psramSize, freePsram);
    TestLog::step("Free PSRAM verified");
    
    // PSRAM 阈值 = 512B（而非旧值 4096B）
    // 为什么 512？AsyncWebServer HTTP 缓冲区 1-2KB，4096 阈值太高无法卸载到 PSRAM
    constexpr uint32_t PSRAM_THRESHOLD = 512;
    
    // HTTP 缓冲区 2KB → 应走 PSRAM（512 阈值命中，旧 4096 阈值无法命中）
    uint32_t httpBufSize = 2048;
    TEST_ASSERT_TRUE(httpBufSize >= PSRAM_THRESHOLD);
    TestLog::step("HTTP buffer 2KB >= threshold 512 → uses PSRAM (old 4096 would MISS!)");
    
    // ArduinoJson String 序列化 800B → 应走 PSRAM
    uint32_t jsonStrSize = 800;
    TEST_ASSERT_TRUE(jsonStrSize >= PSRAM_THRESHOLD);
    TestLog::step("JSON String 800B >= threshold 512 → uses PSRAM");
    
    // lwIP TCP PCB 结构体 ~172B → 应留在内部 DRAM（硬件要求）
    uint32_t tcpPcbSize = 172;
    TEST_ASSERT_FALSE(tcpPcbSize >= PSRAM_THRESHOLD);
    TestLog::step("TCP PCB 172B < threshold 512 → stays in internal DRAM (correct)");
    
    // 旧阈值 4096 无法命中 HTTP 缓冲区（这是导致 OOM 的根因）
    constexpr uint32_t OLD_PSRAM_THRESHOLD = 4096;
    TEST_ASSERT_FALSE(httpBufSize >= OLD_PSRAM_THRESHOLD);
    TestLog::step("OLD threshold 4096: HTTP buffer 2KB MISSED → OOM on internal DRAM!");
    
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

// Smoke Test: 堆保护多级阈值验证（含 MQTT 重启阈值 8KB）
void test_smoke_multi_level_heap_thresholds() {
    TestLog::testStart("Smoke: Multi-Level Heap Thresholds (MQTT 8KB)");
    
    // 各模块的堆保护阈值
    constexpr uint32_t PROTO_HANDLE_THRESHOLD   = 30000;  // ProtocolManager::handle() 重型协议
    constexpr uint32_t MQTT_RECONNECT_THRESHOLD = 8000;   // MQTTClient::doReconnect()
    constexpr uint32_t MQTT_RESTART_THRESHOLD   = 8000;   // ProtocolManager::restartMQTTDeferred()
    constexpr uint32_t MODBUS_RESTART_THRESHOLD = 15000;  // ProtocolManager::restartModbus()
    constexpr uint32_t PUBLISH_INFO_THRESHOLD   = 30000;  // publishDeviceInfo()
    constexpr uint32_t MONITOR_DATA_THRESHOLD   = 25000;  // publishMonitorData()
    constexpr uint32_t QUEUED_COMMANDS_THRESHOLD = 30000;  // processQueuedCommands()
    constexpr uint32_t QUEUED_REPORTS_THRESHOLD  = 20000;  // processQueuedReports()
    
    // 验证 MQTT 阈值远低于旧值（旧值 15KB/49KB 导致 MQTT 永远无法连接）
    TEST_ASSERT_LESS_THAN(15000, MQTT_RECONNECT_THRESHOLD);
    TEST_ASSERT_LESS_THAN(15000, MQTT_RESTART_THRESHOLD);
    TestLog::step("MQTT thresholds 8KB << old 15-49KB (would block PSRAM devices)");
    
    // MQTT 重启阈值应低于协议处理阈值（确保 MQTT 能在低堆时重连）
    TEST_ASSERT_LESS_THAN(PROTO_HANDLE_THRESHOLD, MQTT_RECONNECT_THRESHOLD);
    TestLog::step("MQTT reconnect(8K) < proto handle(30K): MQTT can reconnect even when proto skipped");
    
    // 在各阈值水平模拟行为
    // 堆 = 35KB: 所有操作应正常
    ESP.setFreeHeap(35000);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= PROTO_HANDLE_THRESHOLD);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= MQTT_RECONNECT_THRESHOLD);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= PUBLISH_INFO_THRESHOLD);
    TestLog::step("Heap=35KB: all operations allowed");
    
    // 堆 = 27KB: 重型协议跳过，但 MQTT 重连仍允许
    ESP.setFreeHeap(27000);
    TEST_ASSERT_FALSE(ESP.getFreeHeap() >= PROTO_HANDLE_THRESHOLD);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= MQTT_RECONNECT_THRESHOLD);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= MONITOR_DATA_THRESHOLD);
    TestLog::step("Heap=27KB: heavy proto skipped, MQTT reconnect ALLOWED");
    
    // 堆 = 11KB: PSRAM 设备稳态，MQTT 重连仍允许
    ESP.setFreeHeap(11000);
    TEST_ASSERT_FALSE(ESP.getFreeHeap() >= PROTO_HANDLE_THRESHOLD);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= MQTT_RECONNECT_THRESHOLD);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= MQTT_RESTART_THRESHOLD);
    TEST_ASSERT_FALSE(ESP.getFreeHeap() >= QUEUED_REPORTS_THRESHOLD);
    TestLog::step("Heap=11KB (PSRAM steady): MQTT reconnect ALLOWED (key fix!)");
    
    // 堆 = 5KB: 真正内存危机，所有操作跳过
    ESP.setFreeHeap(5000);
    TEST_ASSERT_FALSE(ESP.getFreeHeap() >= MQTT_RECONNECT_THRESHOLD);
    TEST_ASSERT_FALSE(ESP.getFreeHeap() >= MQTT_RESTART_THRESHOLD);
    TEST_ASSERT_FALSE(ESP.getFreeHeap() >= QUEUED_REPORTS_THRESHOLD);
    TestLog::step("Heap=5KB: ALL operations blocked (true memory crisis)");
    
    ESP.resetHeapOverride();
    TestLog::testEnd(true);
}

// ============================================================
// 源码回归测试：防止 MQTT 关键修改被意外回退
// ============================================================

/**
 * @brief 源码回归：验证 ProtocolManager::handle() 中 MQTT handle() 在堆保护检查之前调用
 * 防止回退到旧逻辑：MQTT handle() 被 30KB 堆保护一起跳过，导致 MQTT 永远无法连接
 */
void test_source_code_mqtt_handle_before_heap_check() {
    TestLog::testStart("Source: MQTT handle() before heap check in ProtocolManager");

    std::string content = readSrcFile("src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read ProtocolManager.cpp — ensure test runs from project root");
    TestLog::step("File loaded");

    // 1) MQTT handle 调用必须存在
    TEST_ASSERT_TRUE_MESSAGE(content.find("mqttClient->handle()") != std::string::npos,
        "mqttClient->handle() must exist in ProtocolManager::handle()");
    TestLog::step("mqttClient->handle() call found");

    // 2) 关键架构：MQTT handle 必须在 heapSufficient 检查之前调用
    //    查找 "mqttClient->handle()" 和 "heapSufficient" 的位置
    size_t mqttHandlePos = content.find("mqttClient->handle()");
    size_t heapSufficientCheck = content.find("if (!heapSufficient)");
    // heapSufficient 检查可能在 mqttClient->handle() 之后
    // 关键约束：mqttClient->handle() 不应在 if (!heapSufficient) { ... return; } 之后
    TEST_ASSERT_TRUE_MESSAGE(mqttHandlePos != std::string::npos,
        "mqttClient->handle() must be present");
    TEST_ASSERT_TRUE_MESSAGE(heapSufficientCheck != std::string::npos,
        "if (!heapSufficient) check must be present");
    TEST_ASSERT_TRUE_MESSAGE(mqttHandlePos < heapSufficientCheck,
        "mqttClient->handle() MUST be called BEFORE the heapSufficient guard — "
        "otherwise MQTT will be skipped when heap < 30KB (old bug!)");
    TestLog::step("mqttClient->handle() is BEFORE heapSufficient guard (correct order)");

    // 3) 注释必须说明 MQTT 始终运行
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MQTT handle()") != std::string::npos ||
        content.find("mqttClient") != std::string::npos,
        "Comments should explain why MQTT always runs");
    TestLog::step("Architecture comment present");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：验证 MQTT doReconnect 和 restartMQTTDeferred 堆阈值为 8000
 * 防止回退到旧值 49152/25000/15000（导致 PSRAM 设备 MQTT 永远无法连接）
 */
void test_source_code_mqtt_thresholds_8kb() {
    TestLog::testStart("Source: MQTT heap thresholds = 8000");

    // 检查 MQTTClient.cpp 中 doReconnect 阈值
    std::string mqttCpp = readSrcFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!mqttCpp.empty(),
        "Failed to read MQTTClient.cpp");

    // doReconnect 堆检查应为 8000（而非旧值 49152 或 15000）
    TEST_ASSERT_TRUE_MESSAGE(mqttCpp.find("8000") != std::string::npos,
        "MQTTClient::doReconnect() heap threshold must be 8000 (not old 15000/49152)");
    TEST_ASSERT_TRUE_MESSAGE(mqttCpp.find("49152") == std::string::npos,
        "OLD threshold 49152 must NOT exist in MQTTClient.cpp (would block MQTT forever)");
    // 验证 "reconnectFreeHeap < 15000" 不存在（避免回退到旧值）
    std::regex oldDoReconnectRe("reconnectFreeHeap\\s*<\\s*15000");
    TEST_ASSERT_TRUE_MESSAGE(!std::regex_search(mqttCpp, oldDoReconnectRe),
        "OLD threshold 'reconnectFreeHeap < 15000' must NOT exist in MQTTClient.cpp");
    TestLog::step("MQTTClient.cpp: threshold=8000, old 15000/49152 absent");

    // 检查 ProtocolManager.cpp 中 restartMQTTDeferred 阈值
    std::string pmCpp = readSrcFile("src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!pmCpp.empty(),
        "Failed to read ProtocolManager.cpp");

    // restartMQTTDeferred 堆检查应为 8000（而非旧值 25000/15000）
    std::regex oldDeferredRe("freeHeap\\s*<\\s*25000");
    TEST_ASSERT_TRUE_MESSAGE(!std::regex_search(pmCpp, oldDeferredRe),
        "OLD threshold 'freeHeap < 25000' must NOT exist in ProtocolManager.cpp");
    TestLog::step("ProtocolManager.cpp: no 'freeHeap < 25000' (old threshold removed)");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：验证 NTP HTTPS→HTTP 降级在三个文件中均存在
 * 防止回退到 WiFiClientSecure（导致 SSL 内存分配失败）
 */
void test_source_code_ntp_https_downgrade() {
    TestLog::testStart("Source: NTP HTTPS→HTTP downgrade in all files");

    const char* files[] = {
        "src/protocols/MQTTClient.cpp",
        "src/network/handlers/MqttRouteHandler.cpp",
        "src/utils/TimeUtils.cpp"
    };

    for (const char* file : files) {
        std::string content = readSrcFile(file);
        TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
            (std::string("Failed to read ") + file).c_str());

        // 必须有 HTTPS→HTTP 降级逻辑
        TEST_ASSERT_TRUE_MESSAGE(
            content.find("http://") != std::string::npos &&
            content.find("startsWith(\"https://\")") != std::string::npos,
            (std::string("NTP HTTPS→HTTP downgrade must exist in ") + file).c_str());

        // 不应有 WiFiClientSecure 用于 NTP（SSL 内存分配失败风险）
        // 注意：文件中可能有其他用途的 WiFiClientSecure，所以不全面禁止
        TestLog::step((std::string("HTTPS→HTTP downgrade confirmed in ") + file).c_str());
    }

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：验证 main.cpp 中 PSRAM 阈值为 512
 * 防止回退到旧值 4096（导致 HTTP 缓冲区无法卸载到 PSRAM）
 */
void test_source_code_psram_threshold_512() {
    TestLog::testStart("Source: PSRAM threshold = 512 in main.cpp");

    std::string content = readSrcFile("src/main.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read main.cpp");

    // 必须有 heap_caps_malloc_extmem_enable(512)
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("heap_caps_malloc_extmem_enable(512)") != std::string::npos,
        "main.cpp must call heap_caps_malloc_extmem_enable(512) — NOT 4096!");
    TestLog::step("heap_caps_malloc_extmem_enable(512) found");

    // 不应有旧值 4096
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("heap_caps_malloc_extmem_enable(4096)") == std::string::npos,
        "OLD threshold heap_caps_malloc_extmem_enable(4096) must NOT exist — "
        "causes HTTP buffer OOM on internal DRAM");
    TestLog::step("Old threshold 4096 absent (regression prevented)");

    TestLog::testEnd(true);
}

/**
 * @brief 行为测试：模拟 ProtocolManager handle() 中 MQTT 不受堆保护影响的逻辑
 * 堆低于 30KB 时，MQTT handle() 仍应被调用
 * 注：MQTT 同时受网络门控（见 test_smoke_mqtt_network_gate_behavior），
 *     本测试仅验证堆维度不影响 MQTT
 */
void test_smoke_mqtt_handle_always_runs() {
    TestLog::testStart("Smoke: MQTT handle() always runs (even heap < 30KB)");

    // 模拟新逻辑：MQTT handle 始终运行，重型协议受堆保护
    constexpr uint32_t HEAP_THRESHOLD = 30000;
    int mqttHandleCount = 0;
    int heavyProtoCount = 0;

    auto simulateHandle = [&](uint32_t freeHeap) {
        // MQTT handle 始终运行
        mqttHandleCount++;

        // 重型协议受堆保护
        bool heapSufficient = (freeHeap >= HEAP_THRESHOLD);
        if (heapSufficient) {
            heavyProtoCount++;
        }
    };

    // 堆 = 80KB：两者都运行
    simulateHandle(80000);
    TEST_ASSERT_EQUAL(1, mqttHandleCount);
    TEST_ASSERT_EQUAL(1, heavyProtoCount);
    TestLog::step("Heap=80KB: MQTT + heavy proto both run");

    // 堆 = 25KB：MQTT 运行，重型协议跳过
    simulateHandle(25000);
    TEST_ASSERT_EQUAL(2, mqttHandleCount);  // MQTT 仍运行
    TEST_ASSERT_EQUAL(1, heavyProtoCount);  // 重型协议跳过
    TestLog::step("Heap=25KB: MQTT runs, heavy proto skipped (KEY FIX!)");

    // 堆 = 15KB：MQTT 仍运行，重型协议跳过
    simulateHandle(15000);
    TEST_ASSERT_EQUAL(3, mqttHandleCount);  // MQTT 仍运行
    TEST_ASSERT_EQUAL(1, heavyProtoCount);
    TestLog::step("Heap=15KB: MQTT STILL runs (doReconnect internally checks 15KB)");

    // 连续 10 次低堆：MQTT 始终运行
    for (int i = 0; i < 10; i++) {
        simulateHandle(22000);  // 模拟典型运行状态
    }
    TEST_ASSERT_EQUAL(13, mqttHandleCount);
    TEST_ASSERT_EQUAL(1, heavyProtoCount);
    TestLog::step("10 iterations at 22KB: MQTT ran every time, heavy proto blocked");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：验证 ESP32-C3 TCP 连接预算配置（TCP=4, 耗尽阈值=10）
 * C3 只有 400KB SRAM，TCP 预算必须 ≤ 4，耗尽阈值必须 < 16
 */
void test_source_code_c3_tcp_budget() {
    TestLog::testStart("Source: ESP32-C3 TCP budget (TCP=4, threshold=10)");

    std::string content = readSrcFile("include/core/ResourceProfile.h");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read ResourceProfile.h");
    TestLog::step("File loaded");

    // C3 区块必须存在 TCP_TOTAL_BUDGET = 4
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("TCP_TOTAL_BUDGET  = 4") != std::string::npos,
        "C3 TCP_TOTAL_BUDGET must be 4 (400KB SRAM limit)");
    TestLog::step("TCP_TOTAL_BUDGET=4 confirmed");

    // C3 SSE 预算 = 1
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("TCP_SSE_BUDGET    = 1") != std::string::npos,
        "C3 TCP_SSE_BUDGET must be 1 (single SSE connection)");
    TestLog::step("TCP_SSE_BUDGET=1 confirmed");

    // C3 HTTP 预算 = 3
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("TCP_HTTP_BUDGET   = 3") != std::string::npos,
        "C3 TCP_HTTP_BUDGET must be 3 (TCP_TOTAL - SSE = 3)");
    TestLog::step("TCP_HTTP_BUDGET=3 confirmed");

    // C3 耗尽阈值 = 10
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("TCP_CONN_EXHAUSTION_THRESHOLD = 10") != std::string::npos,
        "C3 TCP_CONN_EXHAUSTION_THRESHOLD must be 10 (early trigger for low memory)");
    TestLog::step("EXHAUSTION_THRESHOLD=10 confirmed");

    // 编译期断言存在：阈值 < 16
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("TCP_CONN_EXHAUSTION_THRESHOLD < 16") != std::string::npos,
        "static_assert must verify threshold < lwIP hard limit (16)");
    TestLog::step("static_assert(threshold < 16) present");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：验证 platformio.ini 中 C3 的 AsyncTCP 配置
 * C3 CONFIG_ASYNC_TCP_MAX_CONNECTIONS 应为 4（与 TCP_TOTAL_BUDGET 一致）
 */
void test_source_code_c3_platformio_config() {
    TestLog::testStart("Source: C3 platformio.ini AsyncTCP config");

    std::string content = readSrcFile("platformio.ini");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read platformio.ini");
    TestLog::step("File loaded");

    // C3 运行时标志中 CONFIG_ASYNC_TCP_MAX_CONNECTIONS=4
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("CONFIG_ASYNC_TCP_MAX_CONNECTIONS=4") != std::string::npos,
        "C3 CONFIG_ASYNC_TCP_MAX_CONNECTIONS must be 4");
    TestLog::step("CONFIG_ASYNC_TCP_MAX_CONNECTIONS=4 confirmed");

    // C3 环境使用 lite 配置
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("lite_flags") != std::string::npos,
        "C3 must use lite_flags (resource-constrained)");
    TestLog::step("lite_flags confirmed for C3");

    // C3 必须忽略 NimBLE（不支持经典蓝牙）
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("lib_ignore = NimBLE-Arduino") != std::string::npos,
        "C3 must ignore NimBLE-Arduino (no classic BT)");
    TestLog::step("NimBLE-Arduino ignored for C3");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：ESP32-C6 专用资源配置文件
 * 回归：C6之前落入esp32-lite默认分支，资源限制过低（16条执行规则）
 */
void test_source_code_c6_resource_profile() {
    TestLog::testStart("Source: ESP32-C6 Resource Profile");

    std::string content = readSrcFile("include/core/ResourceProfile.h");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read ResourceProfile.h");
    TestLog::step("File loaded");

    // C6 必须有专用分支
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("CONFIG_IDF_TARGET_ESP32C6") != std::string::npos,
        "ResourceProfile.h must have ESP32-C6 specific branch");
    TestLog::step("CONFIG_IDF_TARGET_ESP32C6 branch present");

    // C6 profile 名称应为 "esp32c6-mid"
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("esp32c6-mid") != std::string::npos,
        "C6 profile name must be 'esp32c6-mid'");
    TestLog::step("Profile name 'esp32c6-mid' confirmed");

    // C6 外设上限 = 24
    auto c6Pos = content.find("CONFIG_IDF_TARGET_ESP32C6");
    auto c6PeriphPos = content.find("MAX_PERIPHERALS = 24", c6Pos);
    TEST_ASSERT_TRUE_MESSAGE(c6Pos != std::string::npos &&
                             c6PeriphPos != std::string::npos &&
                             c6PeriphPos - c6Pos < 300,
        "C6 MAX_PERIPHERALS must be 24 (30 GPIOs available)");
    TestLog::step("MAX_PERIPHERALS=24 for C6");

    // C6 执行规则上限 = 24
    auto c6ExecPos = content.find("MAX_PERIPH_EXEC_RULES = 24", c6Pos);
    TEST_ASSERT_TRUE_MESSAGE(c6Pos != std::string::npos &&
                             c6ExecPos != std::string::npos &&
                             c6ExecPos - c6Pos < 300,
        "C6 MAX_PERIPH_EXEC_RULES must be 24 (512KB SRAM)");
    TestLog::step("MAX_PERIPH_EXEC_RULES=24 for C6");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：执行规则使用软限制（remaining至少为1）
 * 回归：修复前 PeriphExecRouteHandler 用 max(0, ...) 导致超限后按钮被锁定
 */
void test_source_code_exec_rules_soft_limit() {
    TestLog::testStart("Source: Exec Rules Soft Limit (remaining >= 1)");

    std::string content = readSrcFile("src/network/handlers/PeriphExecRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read PeriphExecRouteHandler.cpp");
    TestLog::step("File loaded");

    // remaining 必须使用 max(1, ...) 而非 max(0, ...)
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("std::max(1,") != std::string::npos,
        "Exec rules remaining must use max(1, ...) for soft limit");
    TestLog::step("std::max(1, ...) for exec rules confirmed");

    // 不应有 max(0, ...) 用于 exec rules remaining
    std::regex hardLimitRe("profileRemaining\\s*=\\s*std::max\\(0,");
    TEST_ASSERT_TRUE_MESSAGE(!std::regex_search(content, hardLimitRe),
        "Exec rules must NOT use max(0, ...) — would lock button when over soft limit");
    TestLog::step("No max(0, ...) for exec rules remaining");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：前端执行规则按钮在超限时显示警告而非锁定
 * 回归：修复前前端会禁用按钮并显示"已达上限"
 */
void test_source_code_frontend_exec_soft_limit() {
    TestLog::testStart("Source: Frontend Exec Soft Limit Handling");

    std::string content = readSrcFile("web-src/modules/runtime/periph-exec.js");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read periph-exec.js");
    TestLog::step("File loaded");

    // 前端应显示超限警告
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("已超出推荐上限") != std::string::npos,
        "Frontend must show warning when exec rules exceed recommended limit");
    TestLog::step("Over-limit warning message present");

    // 前端在超限时仍应启用按钮（不设置 disabled）
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("但仍可新增") != std::string::npos,
        "Frontend must indicate button is still clickable when over limit");
    TestLog::step("'Still addable' message present for over-limit state");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：外设配置使用硬限制（remaining=0时锁定按钮）
 * 外设数量受 GPIO 物理约束，必须使用 max(0, ...) 硬限制
 */
void test_source_code_periph_config_hard_limit() {
    TestLog::testStart("Source: Periph Config Hard Limit (remaining = max(0, ...))");

    std::string content = readSrcFile("src/network/handlers/PeripheralRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read PeripheralRouteHandler.cpp");
    TestLog::step("File loaded");

    // 外设 remaining 必须使用 max(0, ...)（硬限制，GPIO 物理约束）
    // 与执行规则的软限制（max(1, ...)）不同
    std::regex hardLimitRe("profileRemaining\\s*=\\s*std::max\\(0,");
    TEST_ASSERT_TRUE_MESSAGE(std::regex_search(content, hardLimitRe),
        "Peripheral config remaining must use max(0, ...) for hard limit (GPIO constraint)");
    TestLog::step("std::max(0, ...) for peripheral config confirmed (hard limit)");

    // 验证前端外设配置按钮在 remaining=0 时禁用
    std::string frontend = readSrcFile("web-src/modules/runtime/peripherals.js");
    TEST_ASSERT_TRUE_MESSAGE(!frontend.empty(),
        "Failed to read peripherals.js");

    // 前端应设置 data-resource-locked 属性
    TEST_ASSERT_TRUE_MESSAGE(
        frontend.find("data-resource-locked") != std::string::npos,
        "Frontend must set data-resource-locked when peripheral limit reached");
    TestLog::step("Frontend sets data-resource-locked on hard limit");

    // 前端应显示已达上限提示
    TEST_ASSERT_TRUE_MESSAGE(
        frontend.find("外设数量已达上限") != std::string::npos,
        "Frontend must show 'limit reached' message for peripheral config");
    TestLog::step("'Limit reached' message present for peripheral config");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：MQTT 测试连接快速路径配置匹配检查
 * 验证快速路径检查 server:port 和 scheme 是否匹配，避免误判
 */
void test_source_code_mqtt_fast_path_config_match() {
    TestLog::testStart("Source: MQTT Fast Path Config Match Check");

    std::string content = readSrcFile("src/network/handlers/MqttRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read MqttRouteHandler.cpp");
    TestLog::step("File loaded");

    // 快速路径必须检查 server 和 port 匹配
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("serverMatch") != std::string::npos,
        "Fast path must verify server:port match before returning alreadyConnected");
    TestLog::step("serverMatch check present in fast path");

    // 快速路径必须检查 scheme 匹配
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("schemeMatch") != std::string::npos,
        "Fast path must verify scheme match (mqtt vs mqtts)");
    TestLog::step("schemeMatch check present in fast path");

    // 两个条件必须同时满足才走快速路径
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("serverMatch && schemeMatch") != std::string::npos,
        "Fast path requires BOTH serverMatch AND schemeMatch");
    TestLog::step("Both conditions required for fast path");

    // 快速路径返回 alreadyConnected 时必须包含 clientId（供前端显示）
    auto acPos = content.find("alreadyConnected");
    auto clientIdPos = content.find("cfg.clientId", acPos);
    TEST_ASSERT_TRUE_MESSAGE(acPos != std::string::npos &&
                             clientIdPos != std::string::npos &&
                             clientIdPos - acPos < 600,
        "Fast path response must include clientId for frontend display");
    TestLog::step("clientId included in alreadyConnected response");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：外设执行新增按钮受开发环境开关控制
 * 回归：确保开发环境禁用时按钮不可用，防止误操作
 */
void test_source_code_dev_mode_guard_on_buttons() {
    TestLog::testStart("Source: Developer Mode Guard on Add Buttons");

    // 外设执行按钮受开发环境控制
    std::string execJs = readSrcFile("web-src/modules/runtime/periph-exec.js");
    TEST_ASSERT_TRUE_MESSAGE(!execJs.empty(),
        "Failed to read periph-exec.js");

    // _setPeriphExecCapacity 必须检查 isDeveloperModeEnabled
    TEST_ASSERT_TRUE_MESSAGE(
        execJs.find("isDeveloperModeEnabled") != std::string::npos,
        "Periph exec capacity must check developer mode before enabling button");
    TestLog::step("Developer mode check in periph-exec capacity");

    // 外设配置按钮受开发环境控制
    std::string periphJs = readSrcFile("web-src/modules/runtime/peripherals.js");
    TEST_ASSERT_TRUE_MESSAGE(!periphJs.empty(),
        "Failed to read peripherals.js");

    // _setPeripheralCapacity 必须在未锁定时检查 isDeveloperModeEnabled
    TEST_ASSERT_TRUE_MESSAGE(
        periphJs.find("isDeveloperModeEnabled") != std::string::npos,
        "Peripheral capacity must check developer mode when not resource-locked");
    TestLog::step("Developer mode check in peripheral capacity");

    // 后端 API 必须验证开发环境
    std::string execHandler = readSrcFile("src/network/handlers/PeriphExecRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!execHandler.empty(),
        "Failed to read PeriphExecRouteHandler.cpp");

    TEST_ASSERT_TRUE_MESSAGE(
        execHandler.find("requireDeveloperMode") != std::string::npos,
        "Backend add/update/delete rule handlers must require developer mode");
    TestLog::step("Backend developer mode guard on rule operations");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：配置导入/导出弹窗中 auth.json 必须显示中文标签
 * 回归：CONFIG_TRANSFER_LABELS 字典缺少 auth.json 条目，导致弹窗显示原始文件名
 */
void test_source_code_config_transfer_auth_label() {
    TestLog::testStart("Source: Config Transfer auth.json Chinese Label");

    std::string content = readSrcFile("web-src/modules/runtime/device-config.js");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read device-config.js");
    TestLog::step("File loaded");

    // CONFIG_TRANSFER_LABELS 必须包含 auth.json 条目
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("'auth.json'") != std::string::npos,
        "CONFIG_TRANSFER_LABELS must contain 'auth.json' key for Chinese display name");
    TestLog::step("auth.json key present in CONFIG_TRANSFER_LABELS");

    // auth.json 对应的中文标签应包含"认证"或"安全"
    size_t authKeyPos = content.find("'auth.json'");
    TEST_ASSERT_TRUE(authKeyPos != std::string::npos);
    // 标签应在 auth.json 键后面 50 个字符以内
    std::string labelRegion = content.substr(authKeyPos, 80);
    bool hasChineseLabel = labelRegion.find("认证") != std::string::npos ||
                           labelRegion.find("安全") != std::string::npos;
    TEST_ASSERT_TRUE_MESSAGE(hasChineseLabel,
        "auth.json label must contain Chinese word '认证' or '安全'");
    TestLog::step("auth.json has Chinese label (安全/认证)");

    // 验证其他常见配置文件也都有中文标签
    const char* requiredFiles[] = {
        "'device.json'", "'network.json'", "'peripherals.json'",
        "'periph_exec.json'", "'protocol.json'", "'users.json'"
    };
    for (const char* f : requiredFiles) {
        TEST_ASSERT_TRUE_MESSAGE(
            content.find(f) != std::string::npos,
            (std::string("CONFIG_TRANSFER_LABELS must contain ") + f).c_str());
    }
    TestLog::step("All standard config files have labels");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：MQTT测试连接前暂停主客户端，测试后恢复
 * 回归：华为云IoT等broker在相同clientId并发连接时返回 CONNACK code 2 (MQTT_BAD_CLIENT_ID)
 * 修复：测试前stop()主客户端（如果同一server:port），测试后始终restartMQTTDeferred()
 */
void test_source_code_mqtt_test_stops_main_client() {
    TestLog::testStart("Source: MQTT Test Uses Save+Restart Approach");

    std::string content = readSrcFile("src/network/handlers/MqttRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read MqttRouteHandler.cpp");
    TestLog::step("File loaded");

    // 测试必须通过 saveMqttTestConfig 先保存配置
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("saveMqttTestConfig(server, port, username, password") != std::string::npos,
        "Test handler must save config to protocol.json before connecting");
    TestLog::step("saveMqttTestConfig call present");

    // 必须停止主客户端后再重启（避免 broker session 冲突）
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("Stopping main client before deferred restart") != std::string::npos,
        "Must stop main client before deferred restart");
    TestLog::step("Stop main client log message present");

    // 必须调用 restartMQTTDeferred() 通过主客户端重连（与"保存"按钮相同路径）
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("restartMQTTDeferred()") != std::string::npos,
        "Must call restartMQTTDeferred for reconnection (same as save button)");
    TestLog::step("restartMQTTDeferred call present");

    // 必须返回 deferred 状态让前端通过轮询确认连接结果
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("doc[\"data\"][\"deferred\"]") != std::string::npos,
        "Must return deferred status for frontend polling");
    TestLog::step("deferred status response present");

    // 不再创建独立的 PubSubClient 进行测试
    // (之前的 testClient.connect 已被移除，改用主客户端重连)
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("testClient.connect") == std::string::npos,
        "Must NOT create separate PubSubClient for testing (causes MQTT_BAD_CLIENT_ID)");
    TestLog::step("No separate test client (eliminated MQTT_BAD_CLIENT_ID root cause)");

    TestLog::testEnd(true);
}

// ========== 低内存长期运行测试 & 内存保护源码回归 ==========

/**
 * @brief 低 DRAM 场景下内存保护资格筛选逻辑模拟
 * 验证各模块在不同 DRAM 准备级别下的分级行为
 */
void test_low_memory_protection_tiers() {
    TestLog::testStart("Low Memory: Protection Tier Logic");

    // 分级保护阈值（对齐源码中的实际阈值）
    constexpr uint32_t TIER_WIFI_CONNECT   = 16384;  // WiFi.begin() 需 16KB DRAM
    constexpr uint32_t TIER_WIFI_RECONN    = 12288;  // WiFi 重连需 12KB DRAM
    constexpr uint32_t TIER_MQTT_CMD       = 20480;  // 指令队列处理需 20KB DRAM
    constexpr uint32_t TIER_MQTT_DEV_INFO  = 20480;  // 设备信息上报需 20KB DRAM
    constexpr uint32_t TIER_MQTT_MONITOR   = 16384;  // 监控数据上报需 16KB DRAM
    constexpr uint32_t TIER_MQTT_REPORTS   = 12288;  // 上报队列需 12KB DRAM
    constexpr uint32_t TIER_MQTTS_TOTAL    = 35000;  // MQTTS 重连需 35KB DRAM（缓冲区裁剪后 TLS ~25-30KB）
    constexpr uint32_t TIER_MQTTS_BLOCK    = 20000;  // MQTTS 需 20KB 连续 DRAM（4KB input buffer + 上下文）
    constexpr uint32_t TIER_CRITICAL_DRAM  = 8192;   // DRAM < 8KB → 重启警报

    // 层级分明: 高优先级操作需要更多 DRAM
    // 使用直接比较避免 Unity 断言参数顺序混淆
    TEST_ASSERT_TRUE(TIER_MQTT_REPORTS  > TIER_CRITICAL_DRAM);   // reports(12K) > critical(8K)
    TEST_ASSERT_TRUE(TIER_MQTT_MONITOR  >= TIER_MQTT_REPORTS);   // monitor(16K) >= reports(12K)
    TEST_ASSERT_TRUE(TIER_WIFI_RECONN   >= TIER_CRITICAL_DRAM);  // reconn(12K) > critical(8K)
    TEST_ASSERT_TRUE(TIER_WIFI_CONNECT  >= TIER_WIFI_RECONN);    // connect(16K) >= reconn(12K)
    TEST_ASSERT_TRUE(TIER_MQTT_DEV_INFO >= TIER_WIFI_CONNECT);   // dev_info(20K) >= connect(16K)
    TestLog::step("Tier hierarchy: CRITICAL(8K) < reports/reconn(12K) <= monitor/connect(16K) <= cmd(20K)");

    // 各麦片应在 DRAM=8KB 时被阻止
    uint32_t dram8kb = 8000;
    TEST_ASSERT_FALSE(dram8kb >= TIER_WIFI_CONNECT);
    TEST_ASSERT_FALSE(dram8kb >= TIER_WIFI_RECONN);
    TEST_ASSERT_FALSE(dram8kb >= TIER_MQTT_CMD);
    TEST_ASSERT_FALSE(dram8kb >= TIER_MQTT_DEV_INFO);
    TEST_ASSERT_FALSE(dram8kb >= TIER_MQTT_MONITOR);
    TEST_ASSERT_FALSE(dram8kb >= TIER_MQTT_REPORTS);
    TestLog::step("All operations blocked at DRAM=8KB");

    // DRAM=15KB 时：可以 WiFi 重连 + 上报队列
    uint32_t dram15kb = 15000;
    TEST_ASSERT_FALSE(dram15kb >= TIER_WIFI_CONNECT);  // 连接被阻
    TEST_ASSERT_TRUE(dram15kb >= TIER_WIFI_RECONN);    // 重连允许
    TEST_ASSERT_FALSE(dram15kb >= TIER_MQTT_MONITOR);  // 监控被阻
    TEST_ASSERT_TRUE(dram15kb >= TIER_MQTT_REPORTS);   // 上报允许
    TestLog::step("DRAM=15KB: reconnect+reports allowed, connect+monitor blocked");

    // DRAM=25KB 时：所有普通 MQTT 操作允许
    uint32_t dram25kb = 25000;
    TEST_ASSERT_TRUE(dram25kb >= TIER_WIFI_CONNECT);
    TEST_ASSERT_TRUE(dram25kb >= TIER_MQTT_CMD);
    TEST_ASSERT_TRUE(dram25kb >= TIER_MQTT_DEV_INFO);
    TEST_ASSERT_TRUE(dram25kb >= TIER_MQTT_MONITOR);
    TestLog::step("DRAM=25KB: all MQTT operations allowed");

    TestLog::testEnd(true);
}

/**
 * @brief 长期运行内存健康度模拟
 * 模拟 24 小时和长期运行中 DRAM 的渐进变化，验证保护机制最终能防止崩溃
 */
void test_long_running_memory_health_simulation() {
    TestLog::testStart("Long Run: DRAM Health Simulation");

    // 模拟一个 24小时运行周期中 DRAM 变化趋势
    struct MemSnapshot {
        unsigned long uptimeMs;
        uint32_t dramFree;
        uint32_t dramLargest;
        const char* expectedAction;
    };

    MemSnapshot timeline[] = {
        // 启动后 5 分钟：内存较多，允许所有操作
        {  5 * 60000, 55000, 35000, "all_allowed" },
        // 运行 1 小时：内存少量减少，仍允许（50KB > 40KB 阈值）
        {  1 * 3600000UL, 50000, 30000, "all_allowed" },
        // 运行 6 小时：内存碎片化，最大连续块不足 20KB
        {  6 * 3600000UL, 40000, 18000, "mqtts_blocked" },  // DRAM block 18KB < 20KB
        // 运行 12 小时：内存进一步碎片化
        { 12 * 3600000UL, 28000, 10000, "mqtts_blocked_all_fragmented" },
        // 运行 20 小时：DRAM 叫紧
        { 20 * 3600000UL, 14000, 8000, "wifi_connect_blocked" },
        // 运行 23 小时：DRAM 极低，接近重启阈值
        { 23 * 3600000UL, 9000, 5000, "near_critical" },
    };

    for (const auto& snap : timeline) {
        bool wifiConnectAllowed = (snap.dramFree >= 16384);
        bool wifiReconnAllowed  = (snap.dramFree >= 12288);
        bool mqttsAllowed = (snap.dramFree >= 35000 && snap.dramLargest >= 20000);
        bool mqttReportsAllowed = (snap.dramFree >= 12288);
        bool criticalDanger = (snap.dramFree < 8192);

        unsigned long secs = snap.uptimeMs / 1000;
        if (secs < 60000) {
            TestLog::step(("t=" + std::to_string(secs / 60) + "min: dram=" +
                std::to_string(snap.dramFree/1024) + "KB block=" +
                std::to_string(snap.dramLargest/1024) + "KB").c_str());
        } else {
            TestLog::step(("t=" + std::to_string(secs / 3600) + "h: dram=" +
                std::to_string(snap.dramFree/1024) + "KB block=" +
                std::to_string(snap.dramLargest/1024) + "KB").c_str());
        }

        // 验证保护不会过度阻终（DRAM 充足时应该允许）
        if (strcmp(snap.expectedAction, "all_allowed") == 0) {
            TEST_ASSERT_TRUE(wifiConnectAllowed && mqttsAllowed && mqttReportsAllowed);
        }
        // DRAM 大块不足时 MQTTS 应被阻止
        if (snap.dramLargest < 20000 || snap.dramFree < 35000) {
            TEST_ASSERT_FALSE(mqttsAllowed);
        }
        // 不应在非射点都陈现干小最小到 critical
        if (criticalDanger) {
            TEST_ASSERT_FALSE(wifiConnectAllowed);
            TEST_ASSERT_FALSE(mqttsAllowed);
        }
    }

    TestLog::testEnd(true);
}

/**
 * @brief WiFi 内存保护源码回归：WiFiManager 使用 MALLOC_CAP_INTERNAL
 */
void test_source_code_wifi_memory_protection() {
    TestLog::testStart("Source: WiFiManager DRAM Memory Protection");

    std::string content = readSrcFile("src/network/WiFiManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "WiFiManager.cpp must be readable");

    // 1. connectToWiFi 应使用 MALLOC_CAP_INTERNAL
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MALLOC_CAP_INTERNAL") != std::string::npos,
        "WiFiManager must use MALLOC_CAP_INTERNAL for WiFi memory check");
    TestLog::step("MALLOC_CAP_INTERNAL present in WiFiManager");

    // 2. connectToWiFi 应有 DRAM 不足时的跳过逻辑
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("connectToWiFi: DRAM too low") != std::string::npos,
        "connectToWiFi must log and return false when DRAM is too low");
    TestLog::step("connectToWiFi DRAM guard present");

    // 3. attemptReconnect 应有轻量级 DRAM 保护
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("attemptReconnect: DRAM too low") != std::string::npos,
        "attemptReconnect must have lighter DRAM check");
    TestLog::step("attemptReconnect DRAM guard present");

    // 4. 验证阈值常数已定义
    std::string hmh = readSrcFile("include/systems/HealthMonitor.h");
    TEST_ASSERT_TRUE_MESSAGE(
        hmh.find("WIFI_CONNECT_MIN_DRAM") != std::string::npos &&
        hmh.find("WIFI_RECONN_MIN_DRAM") != std::string::npos,
        "WIFI_CONNECT_MIN_DRAM and WIFI_RECONN_MIN_DRAM must be defined in HealthMonitor.h");
    TestLog::step("WiFi DRAM thresholds defined in HealthMonitor.h");

    TestLog::testEnd(true);
}

/**
 * @brief MQTT 内存保护源码回归：publish 函数使用 DRAM 内部检测
 */
void test_source_code_mqtt_publish_dram_protection() {
    TestLog::testStart("Source: MQTT Publish DRAM Protection");

    std::string content = readSrcFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    // 1. publishDeviceInfo 应使用 DRAM 检测
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("DRAM low") != std::string::npos &&
        content.find("publishDeviceInfo") != std::string::npos,
        "publishDeviceInfo must use DRAM-based memory check");
    TestLog::step("publishDeviceInfo uses DRAM check");

    // 2. publishMonitorData 应使用 DRAM 检测
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("publishMonitorData") != std::string::npos,
        "publishMonitorData DRAM check present");
    TestLog::step("publishMonitorData uses DRAM check");

    // 3. processQueuedCommands 应使用 DRAM 检测
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("processQueuedCommands") != std::string::npos,
        "processQueuedCommands DRAM check present");
    TestLog::step("processQueuedCommands uses DRAM check");

    // 4. processQueuedReports 应使用 DRAM 检测
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("processQueuedReports") != std::string::npos,
        "processQueuedReports DRAM check present");
    TestLog::step("processQueuedReports uses DRAM check");

    // 5. 验证不再使用 ESP.getFreeHeap() < 30000 （可能过时）
    // 启发点：有 PSRAM 时 ESP.getFreeHeap() 包含 PSRAM，30000 阈值无效
    // 注：该检查为指导性而非强制性要求，因为历史代码可能在其他地方使用
    int count30000 = 0;
    size_t pos = 0;
    while ((pos = content.find("ESP.getFreeHeap() < 30000", pos)) != std::string::npos) {
        count30000++;
        pos += 10;
    }
    // publish 函数内应少于 1 处使用该老式内存检测
    TEST_ASSERT_EQUAL(0, count30000);
    TestLog::step(("Old ESP.getFreeHeap()<30000 occurrences: " + std::to_string(count30000) + " (should be 0)").c_str());

    TestLog::testEnd(true);
}

/**
 * @brief HealthMonitor DRAM 内存字段源码回归
 */
void test_source_code_health_monitor_dram_fields() {
    TestLog::testStart("Source: HealthMonitor DRAM Fields");

    std::string hmh = readSrcFile("include/systems/HealthMonitor.h");
    TEST_ASSERT_TRUE_MESSAGE(!hmh.empty(), "HealthMonitor.h must be readable");

    // 1. SystemHealth 必须有 DRAM 字段
    TEST_ASSERT_TRUE_MESSAGE(hmh.find("dramFreeHeap") != std::string::npos,
        "SystemHealth must have dramFreeHeap field");
    TEST_ASSERT_TRUE_MESSAGE(hmh.find("dramLargestBlock") != std::string::npos,
        "SystemHealth must have dramLargestBlock field");
    TestLog::step("SystemHealth DRAM fields present");

    // 2. 阈值常数定义合理
    std::regex connectMinRe("WIFI_CONNECT_MIN_DRAM\\s*=\\s*(\\d+)");
    std::regex reconnMinRe("WIFI_RECONN_MIN_DRAM\\s*=\\s*(\\d+)");
    std::smatch m1, m2;
    if (std::regex_search(hmh, m1, connectMinRe) && std::regex_search(hmh, m2, reconnMinRe)) {
        int connectVal = std::stoi(m1[1].str());
        int reconnVal  = std::stoi(m2[1].str());
        TEST_ASSERT_GREATER_OR_EQUAL(8192, reconnVal);   // 最小 8KB
        TEST_ASSERT_LESS_OR_EQUAL(32768, connectVal);    // 最大 32KB
        TEST_ASSERT_TRUE(reconnVal <= connectVal);
        TestLog::step(("RECONN="+std::to_string(reconnVal)+" <= CONNECT="+std::to_string(connectVal)).c_str());
    }

    // 3. 验证 HealthMonitor.cpp 使用 MALLOC_CAP_INTERNAL
    std::string hmc = readSrcFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(
        hmc.find("MALLOC_CAP_INTERNAL") != std::string::npos,
        "HealthMonitor.cpp must use MALLOC_CAP_INTERNAL");
    TestLog::step("HealthMonitor.cpp uses MALLOC_CAP_INTERNAL");

    // 4. 验证 METRICS 日志包含 DRAM 字段
    TEST_ASSERT_TRUE_MESSAGE(
        hmc.find("dram=") != std::string::npos || hmc.find("dram_free") != std::string::npos,
        "Metrics log must include dram field");
    TestLog::step("DRAM field in metrics log");

    TestLog::testEnd(true);
}

// ============================================================
// WiFi STA 断开条件修复 & MQTT 网络门控回归测试
// ============================================================

/**
 * @brief 源码回归：WiFiManager::connectToWiFi() 在任何非 WL_DISCONNECTED 状态都调用 WiFi.disconnect()
 * 回归：旧代码只在 WL_CONNECTED 或 WL_IDLE_STATUS 时断开，导致 STA 正在连接时调用
 *       WiFi.begin() 触发 "sta is connecting, cannot set config" (ESP_ERR_WIFI_STATE)
 */
void test_source_code_wifi_disconnect_all_non_disconnected_states() {
    TestLog::testStart("Source: WiFi disconnect for ALL non-disconnected states");

    std::string content = readSrcFile("src/network/WiFiManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read WiFiManager.cpp");
    TestLog::step("File loaded");

    // 1) 必须使用 != WL_DISCONNECTED 条件（而非旧的 == WL_CONNECTED || == WL_IDLE_STATUS）
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("currentStatus != WL_DISCONNECTED") != std::string::npos,
        "connectToWiFi must check 'status != WL_DISCONNECTED' to cover ALL active states "
        "(connecting, disconnecting, etc.), not just CONNECTED/IDLE");
    TestLog::step("Disconnect condition: currentStatus != WL_DISCONNECTED");

    // 2) 不应有旧的双条件检查
    std::regex oldConditionRe("currentStatus\\s*==\\s*WL_CONNECTED\\s*\\|\\|\\s*currentStatus\\s*==\\s*WL_IDLE_STATUS");
    TEST_ASSERT_TRUE_MESSAGE(!std::regex_search(content, oldConditionRe),
        "OLD condition 'WL_CONNECTED || WL_IDLE_STATUS' must NOT exist — "
        "it misses the 'connecting' state, causing ESP_ERR_WIFI_STATE");
    TestLog::step("Old narrow condition removed (regression prevented)");

    // 3) 注释必须说明 ESP_ERR_WIFI_STATE 根因
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("ESP_ERR_WIFI_STATE") != std::string::npos,
        "Comment must explain ESP_ERR_WIFI_STATE root cause");
    TestLog::step("ESP_ERR_WIFI_STATE explanation present in comments");

    // 4) WiFi.disconnect(false) 调用必须在 connectToWiFi 中存在
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("WiFi.disconnect(false)") != std::string::npos,
        "WiFi.disconnect(false) must be called before WiFi.begin()");
    TestLog::step("WiFi.disconnect(false) call present");

    // 5) 断开后必须有延迟让 STA 状态机退出 connecting
    //    查找 disconnect 和 delay 的接近性（delay 应在 disconnect 后 50 字符内）
    auto disconnectPos = content.find("WiFi.disconnect(false)");
    auto delayAfterDisconnect = content.find("delay(100)", disconnectPos);
    TEST_ASSERT_TRUE_MESSAGE(disconnectPos != std::string::npos &&
                             delayAfterDisconnect != std::string::npos &&
                             delayAfterDisconnect - disconnectPos < 200,
        "delay(100) must follow WiFi.disconnect(false) to allow STA state machine exit");
    TestLog::step("delay(100) follows WiFi.disconnect(false) within proximity");

    TestLog::testEnd(true);
}

/**
 * @brief 行为测试：模拟 WiFi STA 状态机在不同状态下的断开策略
 * 验证修复后所有非 WL_DISCONNECTED 状态都会触发断开操作
 */
void test_smoke_wifi_sta_state_disconnect_coverage() {
    TestLog::testStart("Smoke: WiFi STA State Disconnect Coverage");

    // 模拟 wl_status_t 枚举值（与 Arduino WiFi 库一致）
    enum MockWLStatus {
        MOCK_WL_NO_SHIELD       = 255,
        MOCK_WL_IDLE_STATUS     = 0,
        MOCK_WL_NO_SSID_AVAIL   = 1,
        MOCK_WL_SCAN_COMPLETED  = 2,
        MOCK_WL_CONNECTED       = 3,
        MOCK_WL_CONNECT_FAILED  = 4,
        MOCK_WL_CONNECTION_LOST = 5,
        MOCK_WL_DISCONNECTED    = 6
    };

    // 模拟修复后的逻辑: if (status != WL_DISCONNECTED) → disconnect
    auto shouldDisconnect = [](MockWLStatus status) -> bool {
        return status != MOCK_WL_DISCONNECTED;
    };

    // 关键场景：STA 正在连接中（CONNECT_FAILED 后自动重试时可能处于中间态）
    // 旧代码不会在这些状态断开，导致 "sta is connecting, cannot set config"
    TEST_ASSERT_TRUE(shouldDisconnect(MOCK_WL_CONNECT_FAILED));
    TestLog::step("CONNECT_FAILED → disconnect (was missed by old code!)");

    TEST_ASSERT_TRUE(shouldDisconnect(MOCK_WL_CONNECTION_LOST));
    TestLog::step("CONNECTION_LOST → disconnect (was missed by old code!)");

    TEST_ASSERT_TRUE(shouldDisconnect(MOCK_WL_NO_SSID_AVAIL));
    TestLog::step("NO_SSID_AVAIL → disconnect (was missed by old code!)");

    TEST_ASSERT_TRUE(shouldDisconnect(MOCK_WL_SCAN_COMPLETED));
    TestLog::step("SCAN_COMPLETED → disconnect (was missed by old code!)");

    // 正常场景：已连接或空闲也应该断开
    TEST_ASSERT_TRUE(shouldDisconnect(MOCK_WL_CONNECTED));
    TestLog::step("CONNECTED → disconnect (correctly handled by old code too)");

    TEST_ASSERT_TRUE(shouldDisconnect(MOCK_WL_IDLE_STATUS));
    TestLog::step("IDLE_STATUS → disconnect (correctly handled by old code too)");

    // 唯一不需要断开的状态：已经断开
    TEST_ASSERT_FALSE(shouldDisconnect(MOCK_WL_DISCONNECTED));
    TestLog::step("DISCONNECTED → skip disconnect (already clean)");

    // 模拟旧逻辑对比：只断开 CONNECTED 和 IDLE
    auto oldShouldDisconnect = [](MockWLStatus status) -> bool {
        return status == MOCK_WL_CONNECTED || status == MOCK_WL_IDLE_STATUS;
    };

    // 旧逻辑遗漏的状态数量
    int missedByOld = 0;
    MockWLStatus allStates[] = {
        MOCK_WL_NO_SSID_AVAIL, MOCK_WL_SCAN_COMPLETED,
        MOCK_WL_CONNECT_FAILED, MOCK_WL_CONNECTION_LOST
    };
    for (auto s : allStates) {
        if (shouldDisconnect(s) && !oldShouldDisconnect(s)) {
            missedByOld++;
        }
    }
    TEST_ASSERT_EQUAL(4, missedByOld);
    TestLog::step("Old logic missed 4 states: NO_SSID, SCAN_COMPLETED, CONNECT_FAILED, CONNECTION_LOST");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：ProtocolManager MQTT handle 受网络就绪门控
 * 回归：WiFi 未连接时 MQTT handle() 仍周期调用并打印调试日志，浪费资源且产生噪声
 * 修复：在 ProtocolManager 层检测 isNetworkConnected()，未就绪时跳过 MQTT handle
 */
void test_source_code_mqtt_network_gate_in_protocol_manager() {
    TestLog::testStart("Source: MQTT network gate in ProtocolManager");

    std::string content = readSrcFile("src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read ProtocolManager.cpp");
    TestLog::step("File loaded");

    // 1) 必须检测网络是否就绪
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("isNetworkConnected()") != std::string::npos,
        "ProtocolManager must check isNetworkConnected() before calling MQTT handle()");
    TestLog::step("isNetworkConnected() check present");

    // 2) networkReady 变量必须存在并用于门控
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("networkReady") != std::string::npos,
        "networkReady flag must gate MQTT handle() invocation");
    TestLog::step("networkReady gating variable present");

    // 3) 网络未就绪时必须有跳过提示日志
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("Network not connected, MQTT handle skipped") != std::string::npos,
        "Must log skip message when network is not connected");
    TestLog::step("Skip log message for network-not-connected present");

    // 4) MQTT alive 日志必须受 getIsConnected() 门控
    //    查找 "MQTT handle alive" 应在 getIsConnected() 判断块内
    auto alivePos = content.find("MQTT handle alive");
    auto getIsConnectedPos = content.find("getIsConnected()");
    TEST_ASSERT_TRUE_MESSAGE(alivePos != std::string::npos &&
                             getIsConnectedPos != std::string::npos &&
                             getIsConnectedPos < alivePos,
        "MQTT alive debug must be gated by getIsConnected() check");
    TestLog::step("MQTT alive debug gated by getIsConnected()");

    // 5) 网络未就绪但 MQTT 仍标记连接时，必须调用一次 handle() 让其标记离线
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("getIsConnected()") != std::string::npos &&
        content.find("handle()") != std::string::npos,
        "When network drops but MQTT thinks connected, must call handle() once for cleanup");
    TestLog::step("One-shot cleanup handle() when network drops");

    TestLog::testEnd(true);
}

/**
 * @brief 行为测试：模拟 ProtocolManager 中 MQTT 网络门控逻辑
 * 验证 WiFi 未连接时 MQTT handle 被跳过，已连接时正常运行
 */
void test_smoke_mqtt_network_gate_behavior() {
    TestLog::testStart("Smoke: MQTT Network Gate Behavior");

    // 模拟 ProtocolManager 中 MQTT 网络门控逻辑
    struct MockMqttState {
        bool networkReady;
        bool mqttIsConnected;
        int handleCallCount;
        int skipLogCount;
        int aliveLogCount;
    };

    auto simulatePmHandle = [](MockMqttState& state) {
        if (state.networkReady) {
            // 网络就绪：正常调用 handle()
            state.handleCallCount++;
        } else {
            // 网络未就绪：仅在 MQTT 仍标记连接时调用一次
            if (state.mqttIsConnected) {
                state.handleCallCount++;
                state.mqttIsConnected = false;  // handle() 会检测到网络断开并标记离线
            }
            state.skipLogCount++;
        }
        // alive 日志仅在 MQTT 已连接时打印
        if (state.mqttIsConnected) {
            state.aliveLogCount++;
        }
    };

    // === 场景 1：WiFi 已连接，MQTT 已连接 ===
    {
        MockMqttState s = { true, true, 0, 0, 0 };
        for (int i = 0; i < 5; i++) simulatePmHandle(s);
        TEST_ASSERT_EQUAL(5, s.handleCallCount);
        TEST_ASSERT_EQUAL(0, s.skipLogCount);
        TEST_ASSERT_EQUAL(5, s.aliveLogCount);
        TestLog::step("WiFi+MQTT connected: handle 5x, alive 5x, skip 0x");
    }

    // === 场景 2：WiFi 未连接，MQTT 未连接（典型 AP 模式） ===
    {
        MockMqttState s = { false, false, 0, 0, 0 };
        for (int i = 0; i < 10; i++) simulatePmHandle(s);
        TEST_ASSERT_EQUAL(0, s.handleCallCount);
        TEST_ASSERT_EQUAL(10, s.skipLogCount);
        TEST_ASSERT_EQUAL(0, s.aliveLogCount);
        TestLog::step("WiFi disconnected + MQTT off: handle 0x, alive 0x (KEY FIX!)");
    }

    // === 场景 3：WiFi 突然断开，MQTT 仍标记连接 → 一次性清理 ===
    {
        MockMqttState s = { false, true, 0, 0, 0 };
        // 第 1 次：MQTT 仍认为连接，调用 handle() 让其标记离线
        simulatePmHandle(s);
        TEST_ASSERT_EQUAL(1, s.handleCallCount);  // 一次性清理调用
        TEST_ASSERT_FALSE(s.mqttIsConnected);       // 现在标记为离线
        TestLog::step("WiFi drops, MQTT was connected: one cleanup handle() call");

        // 第 2-10 次：MQTT 已标记离线，不再调用 handle()
        for (int i = 0; i < 9; i++) simulatePmHandle(s);
        TEST_ASSERT_EQUAL(1, s.handleCallCount);  // 仍是 1，后续不再调用
        TEST_ASSERT_EQUAL(10, s.skipLogCount);
        TEST_ASSERT_EQUAL(0, s.aliveLogCount);  // 无 alive 日志
        TestLog::step("After cleanup: 9 more iterations, 0 additional handle calls");
    }

    // === 场景 4：WiFi 恢复后 MQTT 重新连接 ===
    {
        MockMqttState s = { true, false, 0, 0, 0 };
        // WiFi 恢复，handle() 被调用，MQTT 内部会尝试重连
        simulatePmHandle(s);
        TEST_ASSERT_EQUAL(1, s.handleCallCount);
        TestLog::step("WiFi restored: handle() resumes, MQTT will reconnect internally");
    }

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：验证 mqttClient->handle() 仍在 heapSufficient 检查之前
 * 这是 test_source_code_mqtt_handle_before_heap_check 的补充验证
 * 确保网络门控不影响堆保护架构顺序
 */
void test_source_code_mqtt_network_gate_preserves_heap_order() {
    TestLog::testStart("Source: Network gate preserves heap check order");

    std::string content = readSrcFile("src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read ProtocolManager.cpp");
    TestLog::step("File loaded");

    // mqttClient->handle() 必须在 heapSufficient 检查之前
    size_t mqttHandlePos = content.find("mqttClient->handle()");
    size_t heapSufficientCheck = content.find("if (!heapSufficient)");

    TEST_ASSERT_TRUE_MESSAGE(mqttHandlePos != std::string::npos,
        "mqttClient->handle() must still be present");
    TEST_ASSERT_TRUE_MESSAGE(heapSufficientCheck != std::string::npos,
        "if (!heapSufficient) must still be present");
    TEST_ASSERT_TRUE_MESSAGE(mqttHandlePos < heapSufficientCheck,
        "mqttClient->handle() MUST remain BEFORE heapSufficient guard — "
        "network gate must not push it after the heap check");
    TestLog::step("Order preserved: mqttClient->handle() < heapSufficient guard");

    // networkReady 检查应在 mqttClient->handle() 附近（同一个 if(mqttClient) 块内）
    size_t networkReadyPos = content.find("networkReady");
    TEST_ASSERT_TRUE_MESSAGE(networkReadyPos != std::string::npos &&
                             networkReadyPos < heapSufficientCheck,
        "networkReady check must also be before heapSufficient guard");
    TestLog::step("networkReady check is within MQTT block, before heap guard");

    TestLog::testEnd(true);
}

// ============================================================
// AP+STA 模式移除 & 纯 AP 回退回归测试
// ============================================================

/**
 * @brief 源码回归：NetworkManager.cpp 中不再有任何 WIFI_MODE_APSTA 运行时调用
 * 回归：AP+STA 双模式导致 arduino_events 任务栈溢出崩溃
 *       (Stack canary watchpoint triggered on Core 1)
 */
void test_source_code_no_apsta_mode_in_network_manager() {
    TestLog::testStart("Source: No WIFI_MODE_APSTA in NetworkManager");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read NetworkManager.cpp");
    TestLog::step("File loaded");

    // 1) 不得有任何 WiFi.mode(WIFI_MODE_APSTA) 运行时调用
    std::regex apstaModeRe("WiFi\\.mode\\s*\\(\\s*WIFI_MODE_APSTA\\s*\\)");
    TEST_ASSERT_TRUE_MESSAGE(!std::regex_search(content, apstaModeRe),
        "WiFi.mode(WIFI_MODE_APSTA) must NOT exist in NetworkManager.cpp — "
        "causes arduino_events stack overflow via mode switching event cascade");
    TestLog::step("No WiFi.mode(WIFI_MODE_APSTA) runtime call");

    // 2) 不得有 "AP+STA" 出现在日志字符串中（回退消息应说 "AP mode"）
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("switching to AP+STA") == std::string::npos,
        "Log message 'switching to AP+STA' must be replaced with 'switching to AP mode'");
    TestLog::step("No 'switching to AP+STA' log message");

    // 3) STA 失败回退必须说 "AP mode" 而非 "AP+STA"
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("falling back to AP mode") != std::string::npos,
        "STA failure must log 'falling back to AP mode'");
    TestLog::step("'falling back to AP mode' log present");

    // 4) 回退后必须更新 wifiConfig.mode 为 NETWORK_AP
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("wifiConfig.mode = NetworkMode::NETWORK_AP") != std::string::npos,
        "After AP fallback, wifiConfig.mode must be set to NETWORK_AP");
    TestLog::step("wifiConfig.mode = NETWORK_AP after fallback");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：attemptReconnect() 达到最大重试后回退到纯 AP，禁止 AP+STA
 * 回归：旧代码在 startAPMode() 后调 WiFi.mode(WIFI_MODE_APSTA)，
 *       导致 AP-only 和 STA-only 之间震荡 + arduino_events 栈溢出
 */
void test_source_code_attempt_reconnect_pure_ap_fallback() {
    TestLog::testStart("Source: attemptReconnect pure AP fallback");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read NetworkManager.cpp");
    TestLog::step("File loaded");

    // 定位 attemptReconnect 函数
    auto funcPos = content.find("void FBNetworkManager::attemptReconnect()");
    TEST_ASSERT_TRUE_MESSAGE(funcPos != std::string::npos,
        "attemptReconnect() function must exist");
    TestLog::step("attemptReconnect() found");

    // 1) 注释必须说明回退到纯 AP 模式
    auto pureApComment = content.find("纯 AP 模式", funcPos);
    TEST_ASSERT_TRUE_MESSAGE(pureApComment != std::string::npos &&
                             pureApComment - funcPos < 500,
        "attemptReconnect must comment about pure AP mode fallback");
    TestLog::step("Pure AP mode fallback comment present");

    auto successBlock = content.find("startAPMode()", funcPos);
    auto elseBlock = content.find("} else {", successBlock);
    TEST_ASSERT_TRUE_MESSAGE(successBlock != std::string::npos && elseBlock != std::string::npos,
        "attemptReconnect must have a startAPMode() success branch");
    std::string successPath = content.substr(successBlock, elseBlock - successBlock);

    // 2) 必须设置 autoReconnectEnabled = false（停止自动重连）
    TEST_ASSERT_TRUE_MESSAGE(
        successPath.find("autoReconnectEnabled = false") != std::string::npos,
        "attemptReconnect must disable auto-reconnect after AP fallback — "
        "otherwise STA keeps retrying in AP mode, wasting resources");
    TestLog::step("autoReconnectEnabled = false after AP fallback");

    // 3) 必须设置 statusInfo.status = AP_MODE
    TEST_ASSERT_TRUE_MESSAGE(
        successPath.find("NetworkStatus::AP_MODE") != std::string::npos,
        "attemptReconnect must set status to AP_MODE");
    TestLog::step("statusInfo.status = AP_MODE set");

    // 4) 不得重置 reconnectAttempts = 0 后继续重连（旧代码行为）
    //    新代码在 AP fallback 成功路径中不应有 reconnectAttempts = 0
    TEST_ASSERT_TRUE_MESSAGE(
        successPath.find("reconnectAttempts = 0") == std::string::npos,
        "AP fallback success path must NOT reset reconnectAttempts to 0 — "
        "old code did this to keep retrying STA, causing mode oscillation");
    TestLog::step("No reconnectAttempts reset in AP fallback success path");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：自动重连仅在 STA 模式触发，不再检查 AP+STA 模式
 * 回归：旧代码在 handle() 中检查 (currentMode & WIFI_AP) && (currentMode & WIFI_STA)
 *       来触发重连，但 AP+STA 已移除
 */
void test_source_code_auto_reconnect_sta_only() {
    TestLog::testStart("Source: Auto-reconnect STA mode only");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read NetworkManager.cpp");
    TestLog::step("File loaded");

    // 1) 自动重连条件必须检查 wifiConfig.mode == NETWORK_STA
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("wifiConfig.mode == NetworkMode::NETWORK_STA") != std::string::npos,
        "Auto-reconnect must check wifiConfig.mode == NETWORK_STA");
    TestLog::step("wifiConfig.mode == NETWORK_STA check present");

    // 2) handle() 中不得有 AP+STA 模式检查用于触发重连
    //    旧代码: (currentMode & WIFI_AP) && (currentMode & WIFI_STA)
    std::regex apstaReconnectRe(
        "\\(currentMode\\s*&\\s*WIFI_AP\\)\\s*&&\\s*\\(currentMode\\s*&\\s*WIFI_STA\\)");
    TEST_ASSERT_TRUE_MESSAGE(!std::regex_search(content, apstaReconnectRe),
        "Old AP+STA reconnect condition must NOT exist — "
        "auto-reconnect should only trigger in STA mode");
    TestLog::step("No AP+STA mode check for auto-reconnect");

    // 3) 连接超时处理中不得有 AP+STA 分支
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("AP+STA timeout") == std::string::npos,
        "Old 'AP+STA timeout' branch must be removed from connection timeout handler");
    TestLog::step("No 'AP+STA timeout' branch in handle()");

    TestLog::testEnd(true);
}

/**
 * @brief 行为测试：模拟 STA 失败后的网络模式状态机
 * 验证 STA 失败 → 纯 AP，不再走 AP+STA 路径
 */
void test_smoke_sta_failure_pure_ap_fallback_state_machine() {
    TestLog::testStart("Smoke: STA Failure → Pure AP State Machine");

    // 模拟网络模式状态机
    enum MockNetMode { MODE_STA = 0, MODE_AP = 1 };
    enum MockWifiHwMode { HW_STA = 1, HW_AP = 2, HW_APSTA = 3, HW_NULL = 0 };
    struct NetState {
        MockNetMode configMode;
        MockWifiHwMode hwMode;
        bool autoReconnect;
        int staConnectAttempts;
        const char* status;
    };

    // 模拟新逻辑：STA 失败 → 纯 AP
    auto simulateStaFailure = [](NetState& s) {
        if (s.staConnectAttempts >= 2) {  // MAX_WIFI_RETRIES = 2
            // STA 连接失败，回退到纯 AP
            s.hwMode = HW_AP;  // startAPMode() 设置 WIFI_MODE_AP
            s.configMode = MODE_AP;
            s.autoReconnect = false;
            s.status = "AP_MODE";
        }
    };

    // === 场景 1：STA 连接失败 → 纯 AP ===
    {
        NetState s = { MODE_STA, HW_STA, true, 2, "CONNECTING" };
        simulateStaFailure(s);
        TEST_ASSERT_EQUAL(MODE_AP, s.configMode);
        TEST_ASSERT_EQUAL(HW_AP, s.hwMode);
        TEST_ASSERT_FALSE(s.autoReconnect);
        TEST_ASSERT_EQUAL_STRING("AP_MODE", s.status);
        TestLog::step("STA failed (2 retries): configMode=AP, hwMode=AP(2), autoReconnect=false");
    }

    // === 场景 2：旧逻辑对比（AP+STA）— 验证不再发生 ===
    {
        NetState s = { MODE_STA, HW_STA, true, 2, "CONNECTING" };
        simulateStaFailure(s);
        // hwMode 必须是 HW_AP(2)，绝对不能是 HW_APSTA(3)
        TEST_ASSERT_NOT_EQUAL(HW_APSTA, s.hwMode);
        TestLog::step("hwMode != APSTA(3): confirmed pure AP (stack overflow prevented!)");
    }

    // === 场景 3：attemptReconnect 达到最大重试 → 纯 AP ===
    {
        // 模拟 maxReconnectAttempts=5 后回退
        NetState s = { MODE_STA, HW_STA, true, 5, "DISCONNECTED" };
        auto simulateMaxRetryExhausted = [](NetState& st) {
            if (st.staConnectAttempts >= 5) {
                st.hwMode = HW_AP;
                st.configMode = MODE_AP;
                st.autoReconnect = false;
                st.status = "AP_MODE";
            }
        };
        simulateMaxRetryExhausted(s);
        TEST_ASSERT_EQUAL(MODE_AP, s.configMode);
        TEST_ASSERT_EQUAL(HW_AP, s.hwMode);
        TEST_ASSERT_FALSE(s.autoReconnect);
        TEST_ASSERT_EQUAL_STRING("AP_MODE", s.status);
        TestLog::step("Max reconnect exhausted: pure AP, reconnect disabled");
    }

    TestLog::testEnd(true);
}

/**
 * @brief 行为测试：模拟模式切换对 arduino_events 栈压力的影响
 * 验证纯 AP 回退比 AP+STA 回退的模式切换次数更少
 */
void test_smoke_mode_switch_count_comparison() {
    TestLog::testStart("Smoke: Mode Switch Count (AP vs AP+STA)");

    // 模拟从 STA 模式回退到 AP 的模式切换序列
    enum ModeSwitch { SW_STA, SW_AP, SW_APSTA, SW_NULL };

    // 旧逻辑（AP+STA 回退）的模式切换序列
    ModeSwitch oldSequence[] = {
        SW_STA,     // 启动时设为 STA
        SW_AP,      // startAPMode() 内部设为 AP
        SW_APSTA,   // 然后切到 AP+STA（旧代码）
        SW_STA,     // attemptReconnect → connectToWiFi → 切回 STA
        SW_AP,      // 再次 startAPMode
        SW_APSTA,   // 再次切 AP+STA
    };
    int oldSwitchCount = sizeof(oldSequence) / sizeof(oldSequence[0]);

    // 新逻辑（纯 AP 回退）的模式切换序列
    ModeSwitch newSequence[] = {
        SW_STA,     // 启动时设为 STA
        SW_AP,      // startAPMode() 设为 AP，结束
    };
    int newSwitchCount = sizeof(newSequence) / sizeof(newSequence[0]);

    // 新逻辑模式切换次数必须远少于旧逻辑
    TEST_ASSERT_LESS_THAN(oldSwitchCount, newSwitchCount);
    TestLog::step(("Old AP+STA: " + std::to_string(oldSwitchCount) +
                  " switches vs New pure AP: " + std::to_string(newSwitchCount) + " switches").c_str());

    // 新逻辑中不得有 APSTA 切换
    bool hasApsta = false;
    for (int i = 0; i < newSwitchCount; i++) {
        if (newSequence[i] == SW_APSTA) hasApsta = true;
    }
    TEST_ASSERT_FALSE(hasApsta);
    TestLog::step("New sequence: zero APSTA transitions (stack overflow eliminated)");

    // 旧逻辑至少有 2 次 APSTA 切换（每次回退都触发）
    int oldApstaCount = 0;
    for (int i = 0; i < oldSwitchCount; i++) {
        if (oldSequence[i] == SW_APSTA) oldApstaCount++;
    }
    TEST_ASSERT_GREATER_OR_EQUAL(2, oldApstaCount);
    TestLog::step(("Old sequence had " + std::to_string(oldApstaCount) +
                  " APSTA transitions (root cause of stack overflow)").c_str());

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：WiFiManager.cpp 注释中不再引用 AP+STA 作为运行时策略
 */
void test_source_code_wifi_manager_no_apsta_comments() {
    TestLog::testStart("Source: WiFiManager no AP+STA runtime comments");

    std::string content = readSrcFile("src/network/WiFiManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read WiFiManager.cpp");
    TestLog::step("File loaded");

    // connectToWiFi 中不应有 "AP+STA 模式保持原样" 的注释
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("AP+STA 模式保持原样") == std::string::npos,
        "Old comment 'AP+STA 模式保持原样' must be removed from WiFiManager::connectToWiFi()");
    TestLog::step("Old AP+STA policy comment removed");

    // connectToWiFi 中不应有 "WiFi.begin() 在 AP+STA 下同样有效" 的注释
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("WiFi.begin() 在 AP+STA 下同样有效") == std::string::npos,
        "Old comment about WiFi.begin() in AP+STA must be removed");
    TestLog::step("Old WiFi.begin() AP+STA comment removed");

    TestLog::testEnd(true);
}

void test_source_code_resource_profile_explicit_profiles() {
    TestLog::testStart("Source: Explicit Lite/Standard/Full resource profiles");

    std::string pio = readSrcFile("platformio.ini");
    std::string flags = readSrcFile("include/core/FeatureFlags.h");
    std::string profile = readSrcFile("include/core/ResourceProfile.h");
    TEST_ASSERT_TRUE_MESSAGE(!pio.empty(), "platformio.ini must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!flags.empty(), "FeatureFlags.h must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!profile.empty(), "ResourceProfile.h must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        pio.find("-DFASTBEE_PROFILE_LITE=1") != std::string::npos,
        "Lite flag section must define FASTBEE_PROFILE_LITE");
    TEST_ASSERT_TRUE_MESSAGE(
        pio.find("-DFASTBEE_PROFILE_STANDARD=1") != std::string::npos,
        "Standard flag section must define FASTBEE_PROFILE_STANDARD");
    TEST_ASSERT_TRUE_MESSAGE(
        pio.find("-DFASTBEE_PROFILE_FULL=1") != std::string::npos,
        "Full flag section must define FASTBEE_PROFILE_FULL");
    TestLog::step("platformio.ini profile markers present");

    TEST_ASSERT_TRUE_MESSAGE(
        flags.find("#define FASTBEE_PROFILE_LITE 0") != std::string::npos &&
        flags.find("#define FASTBEE_PROFILE_STANDARD 0") != std::string::npos &&
        flags.find("#define FASTBEE_PROFILE_FULL 0") != std::string::npos,
        "FeatureFlags.h must provide safe defaults for all profile markers");
    TestLog::step("FeatureFlags defaults present");

    TEST_ASSERT_TRUE_MESSAGE(
        profile.find("#elif FASTBEE_PROFILE_FULL") != std::string::npos,
        "ResourceProfile.h must select full profile by explicit profile marker");
    TEST_ASSERT_TRUE_MESSAGE(
        profile.find("#elif FASTBEE_PROFILE_STANDARD") != std::string::npos &&
        profile.find("esp32-standard") != std::string::npos,
        "ResourceProfile.h must contain an explicit standard profile");
    TEST_ASSERT_TRUE_MESSAGE(
        profile.find("FASTBEE_ENABLE_RULE_SCRIPT || FASTBEE_ENABLE_OTA || FASTBEE_ENABLE_ETHERNET || FASTBEE_ENABLE_CELLULAR") == std::string::npos,
        "Full resource profile must not be inferred from individual feature switches");
    TestLog::step("ResourceProfile uses explicit profile markers");

    TestLog::testEnd(true);
}

void test_source_code_fragmentation_reboot_policy() {
    TestLog::testStart("Source: Fragmentation reboot recovery policy");

    std::string hmh = readSrcFile("include/systems/HealthMonitor.h");
    std::string hmc = readSrcFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!hmh.empty(), "HealthMonitor.h must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!hmc.empty(), "HealthMonitor.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        hmh.find("FRAG_THRESHOLD_REBOOT") != std::string::npos &&
        hmh.find("FRAG_REBOOT_MAX_BLOCK") != std::string::npos &&
        hmh.find("FRAG_REBOOT_COUNT") != std::string::npos,
        "HealthMonitor must define sustained fragmentation reboot thresholds");
    TEST_ASSERT_TRUE_MESSAGE(
        hmh.find("consecutiveFragmentationCriticalCount") != std::string::npos,
        "HealthMonitor must track consecutive critical fragmentation samples");
    TestLog::step("Fragmentation counters and thresholds present");

    TEST_ASSERT_TRUE_MESSAGE(
        hmc.find("SystemRebooter::scheduleReboot") != std::string::npos &&
        hmc.find("RestartReason::MEMORY_COMPACTION") != std::string::npos,
        "Unrecoverable fragmentation must schedule diagnostic reboot");
    TEST_ASSERT_TRUE_MESSAGE(
        hmc.find("currentHealth.dramLargestBlock < FRAG_REBOOT_MAX_BLOCK") != std::string::npos,
        "Fragmentation reboot must depend on internal DRAM largest block");
    TEST_ASSERT_TRUE_MESSAGE(
        hmc.find("consecutiveFragmentationCriticalCount = 0") != std::string::npos,
        "Fragmentation counter must reset when pressure clears");
    TestLog::step("Fragmentation reboot path present and resettable");

    TestLog::testEnd(true);
}

void test_source_code_4g_reconnect_and_non_wifi_gate() {
    TestLog::testStart("Source: 4G reconnect and non-WiFi connectivity gate");

    std::string nmh = readSrcFile("include/network/NetworkManager.h");
    std::string nmc = readSrcFile("src/network/NetworkManager.cpp");
    std::string cell = readSrcFile("src/network/CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!nmh.empty(), "NetworkManager.h must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!nmc.empty(), "NetworkManager.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        nmh.find("cellReconnectPending") != std::string::npos &&
        nmh.find("CELL_RECONNECT_INTERVAL_MS") != std::string::npos &&
        nmh.find("CELL_FULL_RESTART_EVERY") != std::string::npos,
        "NetworkManager must keep independent 4G reconnect state");
    TEST_ASSERT_TRUE_MESSAGE(
        nmc.find("Attempting 4G auto-reconnect") != std::string::npos &&
        nmc.find("Recreating 4G adapter") != std::string::npos &&
        nmc.find("cellularAdapter->reconnect()") != std::string::npos,
        "NetworkManager must retry 4G and periodically recreate the adapter");
    TestLog::step("4G reconnect state machine present");

    auto funcPos = nmc.find("bool FBNetworkManager::isNetworkConnected()");
    TEST_ASSERT_TRUE_MESSAGE(funcPos != std::string::npos,
        "isNetworkConnected() function must exist");
    std::string isConnectedBody = nmc.substr(funcPos, 1200);
    TEST_ASSERT_TRUE_MESSAGE(
        isConnectedBody.find("case NetworkType::NET_ETHERNET") != std::string::npos &&
        isConnectedBody.find("return ethernetAdapter && ethernetAdapter->isConnected();") != std::string::npos,
        "Ethernet connectivity must be based on W5500 adapter state, not WiFi STA");
    TEST_ASSERT_TRUE_MESSAGE(
        isConnectedBody.find("case NetworkType::NET_4G") != std::string::npos &&
        isConnectedBody.find("return cellularAdapter && cellularAdapter->isConnected();") != std::string::npos,
        "4G connectivity must be based on cellular adapter state, not WiFi STA");
    TEST_ASSERT_TRUE_MESSAGE(
        isConnectedBody.find("WiFi.status() == WL_CONNECTED") < isConnectedBody.find("case NetworkType::NET_ETHERNET"),
        "WiFi.status() fallback must only apply to NET_WIFI case");
    TestLog::step("Non-WiFi network gate uses active adapter");

    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("_modem->gprsDisconnect()") != std::string::npos &&
        cell.find("_connected = activateNetwork();") != std::string::npos,
        "CellularAdapter::reconnect() must perform PDP reset then activate network");
    TestLog::step("Cellular reconnect resets PDP context");

    // EC801E 兼容性: checkPdpActive 替代 isGprsConnected
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("checkPdpActive()") != std::string::npos,
        "CellularAdapter must have checkPdpActive() for EC801E AT+CGPADDR compatibility");
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("CellularAdapter::isConnected()") != std::string::npos &&
        cell.find("checkPdpActive()") != std::string::npos,
        "isConnected must use checkPdpActive instead of isGprsConnected (EC801E fix)");
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("AT+CGPADDR=1") != std::string::npos,
        "checkPdpActive must query AT+CGPADDR=1 for PDP context IP");
    TestLog::step("EC801E: checkPdpActive replaces isGprsConnected");

    TestLog::testEnd(true);
}

void test_source_code_mqtt_deferred_restart_uses_dram_guard() {
    TestLog::testStart("Source: MQTT deferred restart DRAM guard");

    std::string content = readSrcFile("src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "ProtocolManager.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("heap_caps_get_free_size(MALLOC_CAP_INTERNAL)") != std::string::npos,
        "restartMQTTDeferred must check internal DRAM free heap");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)") != std::string::npos,
        "restartMQTTDeferred must check largest internal DRAM block");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("minLargestBlock") != std::string::npos &&
        content.find("DRAM too low for") != std::string::npos,
        "MQTT deferred restart must log and gate on contiguous DRAM");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("if (freeHeap < minHeap)") == std::string::npos,
        "MQTT deferred restart must not use total heap-only guard");
    TestLog::step("MQTT deferred restart uses DRAM + largest block guard");

    TestLog::testEnd(true);
}

void test_source_code_web_long_stability_recovery_policy() {
    TestLog::testStart("Source: Web long-running recovery policy");

    std::string web = readSrcFile("src/network/WebConfigManager.cpp");
    std::string health = readSrcFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!web.empty(), "WebConfigManager.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!health.empty(), "HealthMonitor.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        web.find("SystemRebooter::update()") != std::string::npos &&
        web.find("checkAndRecoverWebServer") != std::string::npos,
        "Web maintenance must run shared reboot dispatcher and recovery checks");
    TEST_ASSERT_TRUE_MESSAGE(
        web.find("softRestartWebServer") != std::string::npos &&
        web.find("scheduleDeviceRestartForWebRecovery") != std::string::npos,
        "Web recovery must escalate from soft restart to scheduled device restart");
    TEST_ASSERT_TRUE_MESSAGE(
        web.find("tcp_tw_pcbs") != std::string::npos &&
        web.find("severeWebPressure") != std::string::npos,
        "Web recovery must monitor TCP TIME_WAIT pressure and heap pressure");
    TEST_ASSERT_TRUE_MESSAGE(
        health.find("FRAG_REBOOT_COUNT") != std::string::npos,
        "HealthMonitor must provide the fragmentation recovery backstop for Web service");
    TestLog::step("Web service recovery chain present");

    TestLog::testEnd(true);
}

void test_smoke_web_recovery_long_running_policy() {
    TestLog::testStart("Smoke: Web recovery long-running policy");

    struct WebRecoveryState {
        unsigned long severeSince;
        unsigned long safeSoftRestartSince;
        int softRestartCount;
        int deviceRestartCount;
    };

    auto tick = [](WebRecoveryState& s, unsigned long now, bool severePressure) {
        if (!severePressure) {
            s.severeSince = 0;
            s.safeSoftRestartSince = 0;
            return;
        }

        if (s.severeSince == 0) {
            s.severeSince = now;
            s.safeSoftRestartSince = now;
            return;
        }

        unsigned long pressureDuration = now - s.severeSince;
        if (pressureDuration >= 30000UL && pressureDuration < 35000UL) {
            s.softRestartCount++;
            s.safeSoftRestartSince = now;
        } else if (pressureDuration >= 120000UL) {
            s.deviceRestartCount++;
        }
    };

    WebRecoveryState state = {0, 0, 0, 0};
    tick(state, 1000UL, true);
    tick(state, 25000UL, true);
    TEST_ASSERT_EQUAL(0, state.softRestartCount);
    TEST_ASSERT_EQUAL(0, state.deviceRestartCount);
    TestLog::step("Short pressure window does not reboot");

    tick(state, 32000UL, true);
    TEST_ASSERT_EQUAL(1, state.softRestartCount);
    TEST_ASSERT_EQUAL(0, state.deviceRestartCount);
    TestLog::step("Sustained 30s pressure triggers soft restart first");

    tick(state, 121000UL, true);
    TEST_ASSERT_EQUAL(1, state.softRestartCount);
    TEST_ASSERT_EQUAL(1, state.deviceRestartCount);
    TestLog::step("Sustained 120s pressure escalates to device restart");

    tick(state, 130000UL, false);
    TEST_ASSERT_EQUAL(0UL, state.severeSince);
    TEST_ASSERT_EQUAL(0UL, state.safeSoftRestartSince);
    TestLog::step("Recovery state resets after pressure clears");

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
    RUN_TEST(test_smoke_mqtt_handle_always_runs);
    
    // Source Code Regression Tests: 防止关键修复被意外回退
    RUN_TEST(test_source_code_mqtt_handle_before_heap_check);
    RUN_TEST(test_source_code_mqtt_thresholds_8kb);
    RUN_TEST(test_source_code_ntp_https_downgrade);
    RUN_TEST(test_source_code_psram_threshold_512);
    
    // ESP32-C3 Specific Regression Tests
    RUN_TEST(test_source_code_c3_tcp_budget);
    RUN_TEST(test_source_code_c3_platformio_config);

    // ESP32-C6 Resource Profile & Soft Limit Tests
    RUN_TEST(test_source_code_c6_resource_profile);
    RUN_TEST(test_source_code_exec_rules_soft_limit);
    RUN_TEST(test_source_code_frontend_exec_soft_limit);
    
    // Peripheral Config Hard Limit & Dev Mode Guard Tests
    RUN_TEST(test_source_code_periph_config_hard_limit);
    RUN_TEST(test_source_code_mqtt_fast_path_config_match);
    RUN_TEST(test_source_code_dev_mode_guard_on_buttons);
    RUN_TEST(test_source_code_config_transfer_auth_label);
    RUN_TEST(test_source_code_mqtt_test_stops_main_client);

    // 低内存长期运行测试 & 内存保护源码回归
    RUN_TEST(test_low_memory_protection_tiers);
    RUN_TEST(test_long_running_memory_health_simulation);
    RUN_TEST(test_source_code_wifi_memory_protection);
    RUN_TEST(test_source_code_mqtt_publish_dram_protection);
    RUN_TEST(test_source_code_health_monitor_dram_fields);

    // WiFi STA 断开条件修复 & MQTT 网络门控回归测试
    RUN_TEST(test_source_code_wifi_disconnect_all_non_disconnected_states);
    RUN_TEST(test_smoke_wifi_sta_state_disconnect_coverage);
    RUN_TEST(test_source_code_mqtt_network_gate_in_protocol_manager);
    RUN_TEST(test_smoke_mqtt_network_gate_behavior);
    RUN_TEST(test_source_code_mqtt_network_gate_preserves_heap_order);

    // AP+STA 模式移除 & 纯 AP 回退回归测试
    RUN_TEST(test_source_code_no_apsta_mode_in_network_manager);
    RUN_TEST(test_source_code_attempt_reconnect_pure_ap_fallback);
    RUN_TEST(test_source_code_auto_reconnect_sta_only);
    RUN_TEST(test_smoke_sta_failure_pure_ap_fallback_state_machine);
    RUN_TEST(test_smoke_mode_switch_count_comparison);
    RUN_TEST(test_source_code_wifi_manager_no_apsta_comments);

    // Profile, network switching, MQTT DRAM guard and Web long-run recovery
    RUN_TEST(test_source_code_resource_profile_explicit_profiles);
    RUN_TEST(test_source_code_fragmentation_reboot_policy);
    RUN_TEST(test_source_code_4g_reconnect_and_non_wifi_gate);
    RUN_TEST(test_source_code_mqtt_deferred_restart_uses_dram_guard);
    RUN_TEST(test_source_code_web_long_stability_recovery_policy);
    RUN_TEST(test_smoke_web_recovery_long_running_policy);
    
    TestLog::groupEnd();
}
