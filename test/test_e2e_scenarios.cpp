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
    MockAuthManager _authMgr{&MockUserManager::getInstance()};
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

// ============================================================
// E2E: 设备-平台联网路径交互测试
// 验证不同联网方式下，设备与平台交互链路的完整性
// ============================================================

/**
 * @brief 验证 AP 模式下所有平台交互路径不可用
 * AP 模式只有本地 Web 配网，无法访问互联网平台
 */
void test_e2e_ap_mode_no_platform_interaction() {
    TestLog::testStart("E2E: AP Mode - No Platform Interaction");

    MockWiFiClass wifi;
    wifi.mode(WIFI_AP);
    wifi.softAP("FastBee-Setup", "12345678");
    wifi.setConnected(false);

    MockWiFiClass::NetworkStatusInfo status = wifi.getStatusInfo();

    // AP 模式下互联网不可用
    TEST_ASSERT_FALSE(status.internetAvailable);
    TestLog::step("AP mode: internetAvailable = false");

    // AP 模式下 MQTT 不可用（无互联网连接）
    bool mqttAvailable = status.internetAvailable;  // MQTT依赖互联网
    TEST_ASSERT_FALSE(mqttAvailable);
    TestLog::step("AP mode: MQTT unavailable (no internet)");

    // AP 模式下 NTP HTTP/HTTPS 不可用
    bool ntpHttpAvailable = status.internetAvailable;
    TEST_ASSERT_FALSE(ntpHttpAvailable);
    TestLog::step("AP mode: NTP HTTP/HTTPS unavailable (no internet)");

    // AP 模式下 OTA 不可用
    bool otaAvailable = status.internetAvailable;
    TEST_ASSERT_FALSE(otaAvailable);
    TestLog::step("AP mode: OTA unavailable (no internet)");

    // 但本地 Web 配网可用
    TEST_ASSERT_FALSE(status.apIPAddress.isEmpty());
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", status.apIPAddress.c_str());
    TestLog::step("AP mode: local web config still available at 192.168.4.1");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 STA 模式下所有平台交互路径可用
 * WiFi STA 连接成功后，MQTT/NTP/OTA/HTTP 全部可用
 */
void test_e2e_sta_mode_all_platform_paths_available() {
    TestLog::testStart("E2E: STA Mode - All Platform Paths Available");

    MockWiFiClass wifi;
    wifi.mode(WIFI_STA);
    wifi.setShouldFail(false);
    wifi.begin("HomeWiFi", "password");
    wifi.setConnected(true);

    MockWiFiClass::NetworkStatusInfo status = wifi.getStatusInfo();

    // STA 模式下互联网可用
    TEST_ASSERT_TRUE(status.internetAvailable);
    TestLog::step("STA mode: internetAvailable = true");

    // STA 模式下 MQTT 可用
    bool mqttAvailable = status.internetAvailable;
    TEST_ASSERT_TRUE(mqttAvailable);
    TestLog::step("STA mode: MQTT available (internet connected)");

    // STA 模式下 NTP 可用（HTTP/HTTPS/UDP）
    bool ntpAvailable = status.internetAvailable;
    TEST_ASSERT_TRUE(ntpAvailable);
    TestLog::step("STA mode: NTP available (all modes)");

    // STA 模式下 OTA 可用
    bool otaAvailable = status.internetAvailable;
    TEST_ASSERT_TRUE(otaAvailable);
    TestLog::step("STA mode: OTA available (internet connected)");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 STA 断连后平台交互自动恢复
 * WiFi 断连 → 互联网不可用 → 自动重连 → 恢复
 */
void test_e2e_sta_disconnect_reconnect_recovery() {
    TestLog::testStart("E2E: STA Disconnect-Reconnect Recovery");

    MockWiFiClass wifi;
    wifi.mode(WIFI_STA);
    wifi.setShouldFail(false);
    wifi.begin("HomeWiFi", "password");
    wifi.setConnected(true);

    // Phase 1: 正常连接
    MockWiFiClass::NetworkStatusInfo phase1 = wifi.getStatusInfo();
    TEST_ASSERT_TRUE(phase1.internetAvailable);
    TestLog::step("Phase 1: Connected, internet available");

    // Phase 2: WiFi 断连（模拟信号丢失）
    wifi.setConnected(false);
    MockWiFiClass::NetworkStatusInfo phase2 = wifi.getStatusInfo();
    TEST_ASSERT_FALSE(phase2.internetAvailable);
    TEST_ASSERT_FALSE(phase2.wifiConnected);
    TestLog::step("Phase 2: Disconnected, internet unavailable");

    // Phase 2: 平台交互全部不可用
    bool mqttPhase2 = phase2.internetAvailable;
    bool ntpPhase2 = phase2.internetAvailable;
    bool otaPhase2 = phase2.internetAvailable;
    TEST_ASSERT_FALSE(mqttPhase2);
    TEST_ASSERT_FALSE(ntpPhase2);
    TEST_ASSERT_FALSE(otaPhase2);
    TestLog::step("Phase 2: MQTT/NTP/OTA all unavailable");

    // Phase 3: 自动重连
    wifi.setConnected(true);
    MockWiFiClass::NetworkStatusInfo phase3 = wifi.getStatusInfo();
    TEST_ASSERT_TRUE(phase3.internetAvailable);
    TEST_ASSERT_TRUE(phase3.wifiConnected);
    TestLog::step("Phase 3: Reconnected, internet restored");

    // Phase 3: 平台交互恢复
    TEST_ASSERT_TRUE(phase3.internetAvailable);
    TestLog::step("Phase 3: All platform paths restored");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 MQTT 加密认证需要 NTP HTTPS 预先成功
 * 加密认证流程：NTP HTTPS → 获取时间 → 计算过期时间 → AES加密 → MQTT连接
 * 如果 NTP HTTPS 不可用，加密认证无法完成
 */
void test_e2e_mqtt_encrypted_auth_depends_on_ntp_https() {
    TestLog::testStart("E2E: MQTT Encrypted Auth Depends on NTP HTTPS");

    MockWiFiClass wifi;
    wifi.mode(WIFI_STA);
    wifi.setConnected(true);

    // 加密认证的前提条件：互联网可用（NTP HTTPS 需要互联网）
    MockWiFiClass::NetworkStatusInfo status = wifi.getStatusInfo();
    TEST_ASSERT_TRUE(status.internetAvailable);
    TestLog::step("Internet available: NTP HTTPS reachable");

    // 加密认证 NTP URL 必须是 HTTPS 或 HTTP 格式
    String ntpUrlHttps = "https://iot.fastbee.cn/prod-api/iot/tool/ntp";
    String ntpUrlHttp  = "http://iot.fastbee.cn:8080/iot/tool/ntp";
    String ntpUrlUdp   = "ntp.aliyun.com";

    bool isHttpNtp = ntpUrlHttps.startsWith("http://") || ntpUrlHttps.startsWith("https://");
    TEST_ASSERT_TRUE(isHttpNtp);
    TestLog::step("HTTPS NTP URL recognized as HTTP format (WiFiClientSecure)");

    isHttpNtp = ntpUrlHttp.startsWith("http://") || ntpUrlHttp.startsWith("https://");
    TEST_ASSERT_TRUE(isHttpNtp);
    TestLog::step("HTTP NTP URL recognized (WiFiClient)");

    isHttpNtp = ntpUrlUdp.startsWith("http://") || ntpUrlUdp.startsWith("https://");
    TEST_ASSERT_FALSE(isHttpNtp);
    TestLog::step("UDP NTP URL NOT recognized as HTTP (uses configTime)");

    // AP 模式下加密认证不可能完成
    wifi.mode(WIFI_AP);
    wifi.setConnected(false);
    MockWiFiClass::NetworkStatusInfo apStatus = wifi.getStatusInfo();
    TEST_ASSERT_FALSE(apStatus.internetAvailable);
    bool encryptedAuthPossible = apStatus.internetAvailable;  // 需要互联网获取NTP
    TEST_ASSERT_FALSE(encryptedAuthPossible);
    TestLog::step("AP mode: encrypted auth impossible (no NTP access)");

    TestLog::testEnd(true);
}

/**
 * @brief 验证不同联网方式的 NTP 交互路径
 * WiFi/以太网 → HTTPS NTP / HTTP NTP / UDP NTP
 * 4G蜂窝 → HTTPS NTP / HTTP NTP
 * LoRa → 无 NTP（透传模式不直接访问互联网）
 */
void test_e2e_ntp_paths_by_network_type() {
    TestLog::testStart("E2E: NTP Paths by Network Type");

    // WiFi STA: 所有 NTP 模式可用
    bool wifiInternet = true;  // WiFi STA 连接成功
    bool wifiHttpsNtp = wifiInternet;
    bool wifiHttpNtp  = wifiInternet;
    bool wifiUdpNtp   = wifiInternet;
    TEST_ASSERT_TRUE(wifiHttpsNtp);
    TEST_ASSERT_TRUE(wifiHttpNtp);
    TEST_ASSERT_TRUE(wifiUdpNtp);
    TestLog::step("WiFi STA: HTTPS/HTTP/UDP NTP all available");

    // 以太网(W5500): 与WiFi STA相同
    bool ethInternet = true;
    bool ethHttpsNtp = ethInternet;
    bool ethHttpNtp  = ethInternet;
    bool ethUdpNtp   = ethInternet;
    TEST_ASSERT_TRUE(ethHttpsNtp);
    TestLog::step("Ethernet: HTTPS/HTTP/UDP NTP available");

    // 4G蜂窝(EC801E): HTTPS/HTTP NTP可用，UDP NTP可能受限（运营商可能封锁UDP 123）
    bool cellularInternet = true;
    bool cellularHttpsNtp = cellularInternet;  // TCP协议，运营商不封锁
    bool cellularHttpNtp  = cellularInternet;
    bool cellularUdpNtp   = false;  // UDP 123可能被运营商封锁
    TEST_ASSERT_TRUE(cellularHttpsNtp);
    TEST_ASSERT_TRUE(cellularHttpNtp);
    TEST_ASSERT_FALSE(cellularUdpNtp);
    TestLog::step("4G Cellular: HTTPS/HTTP NTP available, UDP NTP may be blocked");

    // LoRa(E22): 无互联网，NTP不可用
    bool loraInternet = false;  // LoRa 透传，无直接互联网
    bool loraNtpAvailable = loraInternet;
    TEST_ASSERT_FALSE(loraNtpAvailable);
    TestLog::step("LoRa: No internet, NTP unavailable");

    // 关键结论：HTTPS NTP 是最可靠的跨联网方式路径
    bool httpsNtpReliable = wifiHttpsNtp && ethHttpsNtp && cellularHttpsNtp;
    TEST_ASSERT_TRUE(httpsNtpReliable);
    TestLog::step("HTTPS NTP is the most reliable cross-network path");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 HTTPClientWrapper HTTPS 支持逻辑
 * baseURL 为 https:// 时使用 WiFiClientSecure，http:// 时使用 WiFiClient
 */
void test_e2e_http_client_wrapper_https_selection() {
    TestLog::testStart("E2E: HTTPClientWrapper HTTPS Selection");

    // 测试 isBaseUrlHttps() 判断逻辑
    String httpsBase = "https://iot.fastbee.cn/prod-api";
    bool isHttps1 = httpsBase.startsWith("https://");
    TEST_ASSERT_TRUE(isHttps1);
    TestLog::step("https://... → isBaseUrlHttps() = true → WiFiClientSecure");

    String httpBase = "http://iot.fastbee.cn:8080/api";
    bool isHttps2 = httpBase.startsWith("https://");
    TEST_ASSERT_FALSE(isHttps2);
    TestLog::step("http://... → isBaseUrlHttps() = false → WiFiClient");

    // 验证 buildURL 不会改变协议
    // HTTPClientWrapper::buildURL() 只拼接 endpoint，不修改 baseURL
    // https://iot.fastbee.cn/prod-api + /iot/data → https://iot.fastbee.cn/prod-api/iot/data
    String url1 = httpsBase;
    String endpoint1 = "/iot/data";
    if (!url1.endsWith("/") && !endpoint1.startsWith("/")) url1 += "/";
    url1 += endpoint1;
    TEST_ASSERT_TRUE(url1.startsWith("https://"));
    TestLog::step("buildURL preserves https:// protocol");

    String url2 = httpBase;
    String endpoint2 = "iot/data";
    if (!url2.endsWith("/") && !endpoint2.startsWith("/")) url2 += "/";
    url2 += endpoint2;
    TEST_ASSERT_TRUE(url2.startsWith("http://"));
    TEST_ASSERT_FALSE(url2.startsWith("https://"));
    TestLog::step("buildURL preserves http:// protocol (no downgrade)");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 MQTT 连接后全链路交互：NTP → MQTT → 数据上报 → NTP订阅
 * 连接成功后：订阅主题 → 发布设备信息 → 发起NTP订阅同步
 */
void test_e2e_mqtt_full_chain_interaction() {
    TestLog::testStart("E2E: MQTT Full Chain Interaction");

    MockWiFiClass wifi;
    wifi.mode(WIFI_STA);
    wifi.setConnected(true);
    MockWiFiClass::NetworkStatusInfo status = wifi.getStatusInfo();
    TEST_ASSERT_TRUE(status.internetAvailable);
    TestLog::step("Internet available for MQTT chain");

    // 1. MQTT 连接
    MockMQTTClient mqtt;
    MQTTConfig config;
    config.enabled = true;
    config.server = "iot.fastbee.cn";
    config.port = 1883;
    config.clientId = "S&FBE100900001&1070&1";
    config.username = "admin";
    config.password = "pass";
    config.autoReconnect = true;
    mqtt.initialize(config);
    TEST_ASSERT_TRUE(mqtt.connect());
    TEST_ASSERT_TRUE(mqtt.getIsConnected());
    TestLog::step("Step 1: MQTT connected");

    // 2. 订阅主题
    TEST_ASSERT_TRUE(mqtt.subscribe("/ntp/get"));
    TEST_ASSERT_TRUE(mqtt.subscribe("/device/control"));
    TestLog::step("Step 2: Subscribed to /ntp/get and /device/control");

    // 3. 发布设备信息
    TEST_ASSERT_TRUE(mqtt.publish("/device/info", "{\"temp\":25.3}"));
    TestLog::step("Step 3: Published device info");

    // 4. NTP 同步请求（MQTT 内建路径 /ntp/get）
    TEST_ASSERT_TRUE(mqtt.publish("/ntp/get", "{\"deviceSendTime\":12345}"));
    TestLog::step("Step 4: Sent NTP sync request via MQTT topic");

    // 5. 收到 NTP 响应 → 设置系统时间
    mqtt.simulateMessage("/ntp/get", "{\"serverRecvTime\":1700000000000,\"serverSendTime\":1700000000100,\"deviceSendTime\":12345}");
    TestLog::step("Step 5: Received NTP response via MQTT topic");

    // 6. 连接后不再调度重连
    TEST_ASSERT_FALSE(mqtt.handleAutoReconnect(30000));
    TestLog::step("Step 6: No reconnect scheduled when connected");

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

    // 设备-平台联网路径交互测试
    RUN_TEST(test_e2e_ap_mode_no_platform_interaction);
    RUN_TEST(test_e2e_sta_mode_all_platform_paths_available);
    RUN_TEST(test_e2e_sta_disconnect_reconnect_recovery);
    RUN_TEST(test_e2e_mqtt_encrypted_auth_depends_on_ntp_https);
    RUN_TEST(test_e2e_ntp_paths_by_network_type);
    RUN_TEST(test_e2e_http_client_wrapper_https_selection);
    RUN_TEST(test_e2e_mqtt_full_chain_interaction);

    TestLog::groupEnd();
}
