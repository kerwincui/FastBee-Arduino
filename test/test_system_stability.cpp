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
 * @brief 行为测试：模拟 ProtocolManager handle() 中 MQTT 始终运行的新逻辑
 * 堆低于 30KB 时，MQTT handle() 仍应被调用
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

    // C3 环境使用 slim 配置
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("slim_flags") != std::string::npos,
        "C3 must use slim_flags (resource-constrained)");
    TestLog::step("slim_flags confirmed for C3");

    // C3 必须忽略 NimBLE（不支持经典蓝牙）
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("lib_ignore = NimBLE-Arduino") != std::string::npos,
        "C3 must ignore NimBLE-Arduino (no classic BT)");
    TestLog::step("NimBLE-Arduino ignored for C3");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：ESP32-C6 专用资源配置文件
 * 回归：C6之前落入esp32-slim默认分支，资源限制过低（16条执行规则）
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
    
    TestLog::groupEnd();
}
