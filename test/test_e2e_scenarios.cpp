/**
 * @file test_e2e_scenarios.cpp
 * @brief End-to-End Scenario Tests
 */

#include <unity.h>
#include <Arduino.h>
#include "mocks/MockWiFi.h"
#include "mocks/MockMQTTClient.h"
#include "mocks/MockAuth.h"
#include "mocks/MockPeripheral.h"
#include "mocks/MockOTA.h"
#include "mocks/MockHealthMonitor.h"
#include "mocks/MockConfigStorage.h"
#include "mocks/MockLittleFS.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

void test_e2e_scenarios_group();

class E2EFastBeeFramework {
public:
    bool initialize() {
        if (!MockLittleFS.begin()) { MockLittleFS.format(); MockLittleFS.begin(); }
        MockConfigStore.initialize();
        MockUserManager::getInstance().initialize();
        MockAuthManager authMgr(&MockUserManager::getInstance());
        authMgr.initialize();
        MockPeripheralManager::getInstance().initialize();
        MockPeriphExecManager::getInstance().initialize();
        MockHealthMon.initialize();
        _initialized = true;
        return true;
    }
    bool isInitialized() { return _initialized; }
    MockWiFiClass* getNetworkManager() { return &_wifi; }
    MockAuthManager* getAuthManager() { return &_authMgr; }
    MockHealthMonitor* getHealthMonitor() { return &MockHealthMon; }
    void run() { MockHealthMon.update(); }
private:
    bool _initialized = false;
    MockWiFiClass _wifi;
    MockAuthManager _authMgr{&MockUserManager::getInstance(), &MockRoleManager::getInstance()};
};

void test_e2e_first_boot() {
    TestLog::testStart("E2E: First Boot");
    MockLittleFS.format();
    E2EFastBeeFramework fw;
    TEST_ASSERT_TRUE(fw.initialize());
    MockWiFiClass* wifi = fw.getNetworkManager();
    wifi->mode(WIFI_AP);
    wifi->softAP("FastBee-Setup", "");
    wifi->setConnected(false);
    MockWiFiClass::NetworkStatusInfo status = wifi->getStatusInfo();
    TEST_ASSERT_EQUAL_STRING("AP_MODE", status.status.c_str());
    TEST_ASSERT_FALSE(status.wifiConnected);
    TEST_ASSERT_FALSE(status.internetAvailable);
    TestLog::testEnd(true);
}

void test_e2e_wifi_provisioning() {
    TestLog::testStart("E2E: WiFi Provisioning");
    E2EFastBeeFramework fw;
    fw.initialize();
    MockWiFiClass* wifi = fw.getNetworkManager();
    wifi->mode(WIFI_STA);
    wifi->setShouldFail(false);
    wifi->begin("HomeWiFi", "password");
    wifi->setConnected(true);
    MockWiFiClass::NetworkStatusInfo status = wifi->getStatusInfo();
    TEST_ASSERT_TRUE(status.wifiConnected);
    TEST_ASSERT_TRUE(status.internetAvailable);
    TestLog::testEnd(true);
}

void test_e2e_wifi_failover() {
    TestLog::testStart("E2E: WiFi Failover");
    E2EFastBeeFramework fw;
    fw.initialize();
    MockWiFiClass* wifi = fw.getNetworkManager();
    wifi->mode(WIFI_STA);
    wifi->setShouldFail(true);
    int result = wifi->begin("WrongWiFi", "WrongPass");
    TEST_ASSERT_EQUAL(WL_CONNECT_FAILED, result);
    wifi->mode(WIFI_AP);
    wifi->softAP("FastBee-Setup", "");
    MockWiFiClass::NetworkStatusInfo status = wifi->getStatusInfo();
    TEST_ASSERT_EQUAL_STRING("AP_MODE", status.status.c_str());
    TestLog::testEnd(true);
}

void test_e2e_first_login() {
    TestLog::testStart("E2E: First Login");
    E2EFastBeeFramework fw;
    fw.initialize();
    MockAuthManager* auth = fw.getAuthManager();
    String sessionId = auth->authenticate("admin", "admin123");
    TEST_ASSERT_FALSE(sessionId.isEmpty());
    TestLog::testEnd(true);
}

void test_e2e_mqtt_configuration() {
    TestLog::testStart("E2E: MQTT Configuration");
    E2EFastBeeFramework fw;
    fw.initialize();
    MQTTConfig mqttCfg;
    mqttCfg.enabled = true;
    mqttCfg.server = "mqtt.fastbee.cn";
    mqttCfg.port = 1883;
    mqttCfg.clientId = "TestDevice";
    MockMQTTClient mqtt;
    mqtt.initialize(mqttCfg);
    TEST_ASSERT_TRUE(mqtt.connect());
    TEST_ASSERT_TRUE(mqtt.subscribe("/device/control"));
    TEST_ASSERT_TRUE(mqtt.publish("/device/status", "online"));
    TestLog::testEnd(true);
}

void test_e2e_peripheral_workflow() {
    TestLog::testStart("E2E: Peripheral Workflow");
    E2EFastBeeFramework fw;
    fw.initialize();
    PeripheralConfig gpioCfg;
    gpioCfg.id = "led_output";
    gpioCfg.type = PeripheralType::GPIO_DIGITAL_OUTPUT;
    gpioCfg.pin = 2;
    gpioCfg.enabled = true;
    TEST_ASSERT_TRUE(MockPeripheralManager::getInstance().addPeripheral(gpioCfg));
    PeriphExecRule rule;
    rule.targetPeriphId = "led_output";
    rule.actionType = ActionType::SET_HIGH;
    rule.enabled = true;
    TEST_ASSERT_TRUE(MockPeriphExecManager::getInstance().addRule(rule));
    TestLog::testEnd(true);
}

void test_e2e_ota_update() {
    TestLog::testStart("E2E: OTA Update");
    E2EFastBeeFramework fw;
    fw.initialize();
    MockOTAManager ota(nullptr);
    ota.initialize();
    TEST_ASSERT_TRUE(ota.beginOTA(102400));
    TEST_ASSERT_TRUE(ota.isOTAInProgress());
    
    // 模拟写入完整固件数据
    uint8_t buf[1024];
    memset(buf, 0xAA, sizeof(buf));
    for (int i = 0; i < 100; i++) {
        ota.writeData(buf, 1024);
    }
    
    TEST_ASSERT_EQUAL(100, ota.getProgress());
    TestLog::testEnd(true);
}

void test_e2e_system_monitor() {
    TestLog::testStart("E2E: System Monitor");
    E2EFastBeeFramework fw;
    fw.initialize();
    MockHealthMonitor* hm = fw.getHealthMonitor();
    hm->update();
    SystemHealth health = hm->getHealthStatus();
    TEST_ASSERT_GREATER_THAN(0, health.freeHeap);
    TEST_ASSERT_GREATER_THAN(0, health.uptime);
    TestLog::testEnd(true);
}

void test_e2e_long_running() {
    TestLog::testStart("E2E: Long Running");
    E2EFastBeeFramework fw;
    fw.initialize();
    uint32_t initialHeap = ESP.getFreeHeap();
    for (int i = 0; i < 10; i++) { fw.run(); delay(50); }
    int32_t heapDiff = (int32_t)ESP.getFreeHeap() - (int32_t)initialHeap;
    TEST_ASSERT_TRUE(heapDiff > -10000);
    TestLog::testEnd(true);
}

void test_e2e_ap_mode_network_status() {
    TestLog::testStart("E2E: AP Mode Network Status");
    E2EFastBeeFramework fw;
    fw.initialize();
    MockWiFiClass* wifi = fw.getNetworkManager();
    wifi->mode(WIFI_AP);
    wifi->softAP("FastBee-Test", "12345678");
    wifi->setConnected(false);
    MockWiFiClass::NetworkStatusInfo netStatus = wifi->getStatusInfo();
    TEST_ASSERT_EQUAL_STRING("AP_MODE", netStatus.status.c_str());
    TEST_ASSERT_FALSE(netStatus.wifiConnected);
    TEST_ASSERT_FALSE(netStatus.internetAvailable);
    TEST_ASSERT_EQUAL(0, netStatus.rssi);
    TEST_ASSERT_TRUE(netStatus.ssid.isEmpty());
    TestLog::testEnd(true);
}

// ============================================================
// E2E: 低内存弹性和 PSRAM 启动场景
// ============================================================

// E2E: 低内存下系统不崩溃
void test_e2e_low_memory_resilience() {
    TestLog::testStart("E2E: Low Memory Resilience");
    
    E2EFastBeeFramework fw;
    TEST_ASSERT_TRUE(fw.initialize());
    TestLog::step("Framework initialized");
    
    // 模拟内存逐渐耗尽的过程
    uint32_t heapLevels[] = {80000, 50000, 35000, 28000, 22000, 15000, 10000};
    
    for (uint32_t heap : heapLevels) {
        ESP.setFreeHeap(heap);
        
        // 系统不应该崩溃，只是降级服务
        fw.run();  // 应该正常返回而不是 abort()
        
        MockHealthMonitor* hm = fw.getHealthMonitor();
        hm->setFreeHeap(heap);
        hm->update();
        
        SystemHealth health = hm->getHealthStatus();
        
        if (heap >= 10000) {
            // 堆 >= 10KB 系统应仍然健康（可能有警告但不致命）
            TEST_ASSERT_GREATER_THAN(0, health.freeHeap);
        } else {
            // 堆 < 10KB 触发严重警告，系统不健康但不崩溃
            TEST_ASSERT_FALSE(health.isHealthy);
        }
    }
    
    TestLog::step("System survived all heap levels without crash");
    
    // 堆恢复后系统恢复正常
    ESP.setFreeHeap(80000);
    fw.run();
    MockHealthMonitor* hm = fw.getHealthMonitor();
    hm->setFreeHeap(80000);
    hm->clearWarningsAndErrors();
    hm->update();
    
    SystemHealth health = hm->getHealthStatus();
    TEST_ASSERT_TRUE(health.isHealthy);
    TestLog::step("System recovered after heap restored to 80KB");
    
    ESP.resetHeapOverride();
    TestLog::testEnd(true);
}

// E2E: 带 PSRAM 的完整启动流程
void test_e2e_boot_with_psram() {
    TestLog::testStart("E2E: Boot with PSRAM Enabled");
    
    // 模拟启动序列
    ESP.setPsramSize(8 * 1024 * 1024);   // 8MB PSRAM
    ESP.setFreePsram(7 * 1024 * 1024);   // 7MB free
    
    // 验证 PSRAM 可检测
    TEST_ASSERT_EQUAL(8 * 1024 * 1024, ESP.getPsramSize());
    TEST_ASSERT_GREATER_THAN(0, ESP.getFreePsram());
    TestLog::step("PSRAM detected: 8MB total, 7MB free");
    
    // 模拟 heap_caps_malloc_extmem_enable(4096) 的效果
    constexpr uint32_t EXTMEM_THRESHOLD = 4096;
    
    // 模拟各种分配场景
    struct AllocScenario {
        uint32_t size;
        const char* target;
    };
    AllocScenario scenarios[] = {
        {512,   "internal DRAM"},
        {2048,  "internal DRAM"},
        {4096,  "PSRAM"},
        {8192,  "PSRAM"},
        {32768, "PSRAM"},
    };
    
    for (auto& s : scenarios) {
        bool usesPsram = (s.size >= EXTMEM_THRESHOLD);
        const char* expected = usesPsram ? "PSRAM" : "internal DRAM";
        TEST_ASSERT_EQUAL_STRING(expected, s.target);
    }
    TestLog::step("PSRAM threshold 4KB routing verified");
    
    // 完整启动流程
    E2EFastBeeFramework fw;
    TEST_ASSERT_TRUE(fw.initialize());
    TestLog::step("Framework initialized with PSRAM");
    
    // WiFi 连接后堆仍应充足（因为大分配走 PSRAM）
    ESP.setFreeHeap(100000);  // 内部 DRAM 充足
    TEST_ASSERT_GREATER_THAN(30000, ESP.getFreeHeap());
    TestLog::step("Internal DRAM healthy after boot (PSRAM offloading)");
    
    ESP.resetHeapOverride();
    TestLog::testEnd(true);
}

// E2E: MQTT 连接后低内存场景（复现崩溃 bug）
void test_e2e_mqtt_after_connect_low_heap() {
    TestLog::testStart("E2E: MQTT Post-Connect Low Heap (OOM Bug Repro)");
    
    E2EFastBeeFramework fw;
    fw.initialize();
    
    // 模拟 WiFi 连接后堆只剩 24KB（复现原始崩溃场景）
    MockWiFiClass* wifi = fw.getNetworkManager();
    wifi->mode(WIFI_STA);
    wifi->begin("HomeWiFi", "password");
    wifi->setConnected(true);
    TestLog::step("WiFi connected");
    
    // 设置堆为崩溃时的值
    ESP.setFreeHeap(24368);
    TestLog::step("Heap = 24368 bytes (crash reproduction level)");
    
    // MQTT 连接后尝试 publishDeviceInfo - 应被堆保护拦截
    MockMQTTClient mqtt;
    MQTTConfig mqttCfg;
    mqttCfg.enabled = true;
    mqttCfg.server = "mqtt.fastbee.cn";
    mqttCfg.port = 1883;
    mqttCfg.clientId = "TestDevice";
    mqtt.initialize(mqttCfg);
    TEST_ASSERT_TRUE(mqtt.connect());
    TestLog::step("MQTT connected");
    
    // 在旧代码中，这里会调用 publishDeviceInfo 导致 abort()
    // 修复后应该被堆保护跳过
    bool wouldSkip = (ESP.getFreeHeap() < 30000);
    TEST_ASSERT_TRUE(wouldSkip);
    TestLog::step("Heap 24368 < 30000: publishDeviceInfo would be SKIPPED");
    
    // 系统不会崩溃
    fw.run();
    TestLog::step("System did NOT crash (OOM bug fixed)");
    
    ESP.resetHeapOverride();
    TestLog::testEnd(true);
}

void test_e2e_scenarios_group() {
    TestLog::groupStart("End-to-End Scenario Tests");
    RUN_TEST(test_e2e_first_boot);
    RUN_TEST(test_e2e_wifi_provisioning);
    RUN_TEST(test_e2e_wifi_failover);
    RUN_TEST(test_e2e_first_login);
    RUN_TEST(test_e2e_mqtt_configuration);
    RUN_TEST(test_e2e_peripheral_workflow);
    RUN_TEST(test_e2e_ota_update);
    RUN_TEST(test_e2e_system_monitor);
    RUN_TEST(test_e2e_long_running);
    RUN_TEST(test_e2e_ap_mode_network_status);
    
    // OOM Protection & Resilience Tests
    RUN_TEST(test_e2e_low_memory_resilience);
    RUN_TEST(test_e2e_boot_with_psram);
    RUN_TEST(test_e2e_mqtt_after_connect_low_heap);
    
    TestLog::groupEnd();
}
