/**
 * @file test_network_mqtt_integration.cpp
 * @brief 网络切换与 MQTT 重连集成测试
 *
 * 覆盖场景：
 * 1. WiFi STA / 以太网 / 4G 之间切换后 MQTT 能重建连接
 * 2. 非 WiFi 联网方式失败后回退到 AP 模式，并同步更新 networkType
 * 3. 以太网断线自动重连期间 MQTT 状态保持正确
 * 4. 多轮网络切换后无内存泄漏、状态不混乱
 * 5. 网络不可用时 MQTT 不重连，恢复后自动重连
 *
 * 这些测试主要使用 MockMultiNetworkManager + MockMQTTClient，
 * 不依赖真实硬件，可在 native 环境中快速回归。
 */

#include <unity.h>
#include <Arduino.h>
#include "mocks/MockWiFi.h"
#include "mocks/MockMQTTClient.h"
#include "mocks/MockMultiNetwork.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"
#include "helpers/NetworkMQTTHelper.h"

void test_network_mqtt_integration_group();

using namespace NetMQTTHelper;

// ========== 辅助函数 ==========

/**
 * @brief 模拟 restartNetwork 并验证 networkType 同步（回归关键 bug）
 *
 * 来源：NetworkManager 中 restartNetwork() 在以太网/4G 失败后回退到 AP 模式时，
 * 必须同步更新 wifiConfig.networkType = NET_WIFI，否则 isNetworkConnected() 仍检查
 * 已失效的适配器导致永远返回 false。
 */
static bool restartNetworkAndSyncType(MockMultiNetworkManager& mgr,
                                      MockMQTTClient& mqtt,
                                      const MQTTConfig& config,
                                      bool adapterSuccess)
{
    MockMultiNetworkManager::NetType prevType = mgr.networkType;

    // 切换前如果 MQTT 还连着，先断开
    if (mqtt.getIsConnected() || !mqtt.isStopped()) {
        mqtt.disconnect();
        mqtt.setStopped(true);
    }

    bool ok = mgr.restartNetwork(adapterSuccess);

    // 关键回归验证：非 WiFi 模式失败后，networkType 必须同步回 NET_WIFI
    if (!ok && prevType != MockMultiNetworkManager::NetType::NET_WIFI) {
        TEST_ASSERT_EQUAL_MESSAGE(
            (int)MockMultiNetworkManager::NetType::NET_WIFI,
            (int)mgr.networkType,
            "restartNetwork fallback must sync networkType to NET_WIFI"
        );
    }

    // 如果网络可用，重建 MQTT
    if (ok && mgr.internetAvailable) {
        mqtt.setStopped(false);
        mqtt.initialize(config);
        return mqtt.connect();
    }
    return false;
}

/**
 * @brief 切换到目标网络并尝试重建 MQTT
 */
static bool switchToNetwork(MockMultiNetworkManager& mgr,
                            MockMQTTClient& mqtt,
                            const MQTTConfig& config,
                            MockMultiNetworkManager::NetType target,
                            bool adapterSuccess)
{
    return switchNetworkAndReconnectMQTT(mgr, mqtt, config, target, adapterSuccess);
}

// ========== 测试用例 ==========

/**
 * @brief WiFi STA -> 以太网切换后 MQTT 重连
 */
static void test_switch_wifi_to_ethernet_reconnects_mqtt() {
    TestLog::testStart("Switch WiFi STA -> Ethernet -> MQTT reconnects");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    MQTTConfig config = buildDefaultMQTTConfig();

    // 初始：WiFi STA
    bool ok = switchToNetwork(mgr, mqtt, config,
                              MockMultiNetworkManager::NetType::NET_WIFI, true);
    TEST_ASSERT_TRUE(ok);
    assertMQTTConnected(mqtt);
    assertNetworkConnected(mgr);
    TestLog::step("WiFi STA connected, MQTT connected");

    // 切换到以太网
    ok = switchToNetwork(mgr, mqtt, config,
                         MockMultiNetworkManager::NetType::NET_ETHERNET, true);
    TEST_ASSERT_TRUE(ok);
    assertMQTTConnected(mqtt);
    assertNetworkConnected(mgr);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetType::NET_ETHERNET,
                      (int)mgr.networkType);
    TestLog::step("Switched to Ethernet, MQTT reconnected");

    TestLog::testEnd(true);
}

/**
 * @brief 以太网 -> 4G 切换后 MQTT 重连
 */
static void test_switch_ethernet_to_4g_reconnects_mqtt() {
    TestLog::testStart("Switch Ethernet -> 4G -> MQTT reconnects");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    MQTTConfig config = buildDefaultMQTTConfig();

    // 初始：以太网
    bool ok = switchToNetwork(mgr, mqtt, config,
                              MockMultiNetworkManager::NetType::NET_ETHERNET, true);
    TEST_ASSERT_TRUE(ok);
    assertMQTTConnected(mqtt);
    TestLog::step("Ethernet connected, MQTT connected");

    // 切换到 4G
    ok = switchToNetwork(mgr, mqtt, config,
                         MockMultiNetworkManager::NetType::NET_4G, true);
    TEST_ASSERT_TRUE(ok);
    assertMQTTConnected(mqtt);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetType::NET_4G,
                      (int)mgr.networkType);
    TestLog::step("Switched to 4G, MQTT reconnected");

    TestLog::testEnd(true);
}

/**
 * @brief 4G -> WiFi STA 切换后 MQTT 重连（验证回退路径）
 */
static void test_switch_4g_to_wifi_reconnects_mqtt() {
    TestLog::testStart("Switch 4G -> WiFi STA -> MQTT reconnects");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    MQTTConfig config = buildDefaultMQTTConfig();

    // 初始：4G
    bool ok = switchToNetwork(mgr, mqtt, config,
                              MockMultiNetworkManager::NetType::NET_4G, true);
    TEST_ASSERT_TRUE(ok);
    assertMQTTConnected(mqtt);
    TestLog::step("4G connected, MQTT connected");

    // 切换到 WiFi STA
    ok = switchToNetwork(mgr, mqtt, config,
                         MockMultiNetworkManager::NetType::NET_WIFI, true);
    TEST_ASSERT_TRUE(ok);
    assertMQTTConnected(mqtt);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetType::NET_WIFI,
                      (int)mgr.networkType);
    TestLog::step("Switched to WiFi STA, MQTT reconnected");

    TestLog::testEnd(true);
}

/**
 * @brief 4G 初始化失败后回退 AP，MQTT 停止并在网络恢复后可重连
 */
static void test_4g_failure_fallback_ap_mqtt_stops_then_recovers() {
    TestLog::testStart("4G failure -> AP fallback -> MQTT stops then recovers");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    MQTTConfig config = buildDefaultMQTTConfig();

    // 4G 成功 -> MQTT 连上
    bool ok = switchToNetwork(mgr, mqtt, config,
                              MockMultiNetworkManager::NetType::NET_4G, true);
    TEST_ASSERT_TRUE(ok);
    assertMQTTConnected(mqtt);
    TestLog::step("4G connected, MQTT online");

    // 4G 失败后回退 AP
    ok = switchToNetwork(mgr, mqtt, config,
                         MockMultiNetworkManager::NetType::NET_4G, false);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetType::NET_WIFI,
                      (int)mgr.networkType);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetMode::NETWORK_AP,
                      (int)mgr.mode);
    // MQTT 应该已经停止
    TEST_ASSERT_TRUE(mqtt.isStopped());
    TestLog::step("4G failed -> AP fallback, MQTT stopped");

    // 从 AP 恢复为 WiFi STA，MQTT 重新连接
    ok = switchToNetwork(mgr, mqtt, config,
                         MockMultiNetworkManager::NetType::NET_WIFI, true);
    TEST_ASSERT_TRUE(ok);
    assertMQTTConnected(mqtt);
    TestLog::step("Recovered to WiFi STA, MQTT reconnected");

    TestLog::testEnd(true);
}

/**
 * @brief restartNetwork 4G 失败后必须同步 networkType（回归测试）
 */
static void test_restart_network_4g_failure_syncs_network_type() {
    TestLog::testStart("restartNetwork 4G failure syncs networkType");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    MQTTConfig config = buildDefaultMQTTConfig();

    // 先让 4G 成功
    bool ok = switchToNetwork(mgr, mqtt, config,
                              MockMultiNetworkManager::NetType::NET_4G, true);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetType::NET_4G,
                      (int)mgr.networkType);
    TestLog::step("4G initialized");

    // restartNetwork 失败，验证 networkType 同步
    ok = restartNetworkAndSyncType(mgr, mqtt, config, false);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetType::NET_WIFI,
                      (int)mgr.networkType);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("restartNetwork 4G failure -> networkType synced to NET_WIFI");

    TestLog::testEnd(true);
}

/**
 * @brief restartNetwork 以太网失败后必须同步 networkType（回归测试）
 */
static void test_restart_network_ethernet_failure_syncs_network_type() {
    TestLog::testStart("restartNetwork Ethernet failure syncs networkType");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    MQTTConfig config = buildDefaultMQTTConfig();

    // 先让以太网成功
    bool ok = switchToNetwork(mgr, mqtt, config,
                              MockMultiNetworkManager::NetType::NET_ETHERNET, true);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetType::NET_ETHERNET,
                      (int)mgr.networkType);
    TestLog::step("Ethernet initialized");

    // restartNetwork 失败
    ok = restartNetworkAndSyncType(mgr, mqtt, config, false);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetType::NET_WIFI,
                      (int)mgr.networkType);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("restartNetwork Ethernet failure -> networkType synced to NET_WIFI");

    TestLog::testEnd(true);
}

/**
 * @brief 以太网断线自动重连期间，MQTT 应停止并在重连成功后恢复
 */
static void test_ethernet_auto_reconnect_preserves_mqtt_state() {
    TestLog::testStart("Ethernet auto-reconnect preserves MQTT state");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    MQTTConfig config = buildDefaultMQTTConfig();

    // 以太网成功 + MQTT 连上
    bool ok = switchToNetwork(mgr, mqtt, config,
                              MockMultiNetworkManager::NetType::NET_ETHERNET, true);
    TEST_ASSERT_TRUE(ok);
    assertMQTTConnected(mqtt);
    TestLog::step("Ethernet + MQTT connected");

    // 模拟以太网断线
    mgr.simulateEthReconnectUpdate(false, 0, false);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::DISCONNECTED,
                      (int)mgr.status);
    TEST_ASSERT_TRUE(mgr.ethReconnectPending);
    TestLog::step("Ethernet disconnected, reconnect scheduled");

    // MQTT 断开后应进入自动重连调度
    mqtt.disconnect();
    TEST_ASSERT_FALSE(mqtt.getIsConnected());
    TEST_ASSERT_TRUE(mqtt.handleAutoReconnect(
        mqtt.getConfig().reconnectInterval + 1));
    TestLog::step("MQTT auto-reconnect scheduled");

    // 以太网恢复
    bool reconnected = mgr.simulateEthReconnectUpdate(true, 20000, true);
    TEST_ASSERT_TRUE(reconnected);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::CONNECTED,
                      (int)mgr.status);
    TestLog::step("Ethernet restored");

    // MQTT 重建并连接
    mqtt.clearReconnectPending();
    mqtt.setStopped(false);
    mqtt.initialize(config);
    TEST_ASSERT_TRUE(mqtt.connect());
    assertMQTTConnected(mqtt);
    TestLog::step("MQTT reconnected after Ethernet recovery");

    TestLog::testEnd(true);
}

/**
 * @brief 多轮网络切换稳定性：WiFi -> ETH -> 4G -> ETH(fail) -> WiFi 重复 3 轮
 */
static void test_network_switch_sequence_stability() {
    TestLog::testStart("Network switch sequence stability (3 rounds)");

    SwitchStep steps[] = {
        {MockMultiNetworkManager::NetType::NET_WIFI, true, true},
        {MockMultiNetworkManager::NetType::NET_ETHERNET, true, true},
        {MockMultiNetworkManager::NetType::NET_4G, true, true},
        {MockMultiNetworkManager::NetType::NET_ETHERNET, false, false},
        {MockMultiNetworkManager::NetType::NET_WIFI, true, true},
    };

    int pass = executeNetworkSwitchSequence(steps, 5, 3);
    TEST_ASSERT_EQUAL(15, pass);
    TestLog::step("3 rounds x 5 steps all passed");

    TestLog::testEnd(true);
}

/**
 * @brief 网络切换后不应出现内存泄漏（模拟）
 */
static void test_network_switch_no_memory_leak() {
    TestLog::testStart("Network switch no memory leak");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    MQTTConfig config = buildDefaultMQTTConfig();

    // 记录初始堆
    uint32_t initialHeap = ESP.getFreeHeap();

    // 执行多轮切换
    SwitchStep steps[] = {
        {MockMultiNetworkManager::NetType::NET_WIFI, true, true},
        {MockMultiNetworkManager::NetType::NET_ETHERNET, true, true},
        {MockMultiNetworkManager::NetType::NET_4G, true, true},
    };
    executeNetworkSwitchSequence(steps, 3, 5);

    // 强制清理
    mqtt.disconnect();
    mgr.disconnect();

    int32_t leak = (int32_t)initialHeap - (int32_t)ESP.getFreeHeap();
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(
        (int32_t)TestConfig::MAX_NETWORK_SWITCH_LEAK_BYTES,
        leak,
        (String("Memory leak detected: ") + leak + " bytes").c_str()
    );
    TestLog::step("Memory leak within tolerance");

    TestLog::testEnd(true);
}

/**
 * @brief 网络不可用时 MQTT 不应重连
 */
static void test_mqtt_not_reconnect_when_network_down() {
    TestLog::testStart("MQTT does not reconnect when network is down");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    MQTTConfig config = buildDefaultMQTTConfig();

    // 初始 WiFi 成功，MQTT 连上
    bool ok = switchToNetwork(mgr, mqtt, config,
                              MockMultiNetworkManager::NetType::NET_WIFI, true);
    TEST_ASSERT_TRUE(ok);
    assertMQTTConnected(mqtt);
    TestLog::step("Initial WiFi + MQTT connected");

    // 切换到不可用网络（适配器失败 -> AP）
    ok = switchToNetwork(mgr, mqtt, config,
                         MockMultiNetworkManager::NetType::NET_ETHERNET, false);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_FALSE(mgr.internetAvailable);
    TEST_ASSERT_TRUE(mqtt.isStopped());
    TestLog::step("Network down -> MQTT stopped");

    // 此时即使触发 handleAutoReconnect，由于网络不可用也不应真的连上
    mqtt.setStopped(false);  // 模拟用户没停但网络不可用
    mqtt.disconnect();
    mqtt.handleAutoReconnect(config.reconnectInterval + 1);
    TEST_ASSERT_FALSE(mqtt.getIsConnected());
    TestLog::step("MQTT did not connect while network unavailable");

    TestLog::testEnd(true);
}

/**
 * @brief 网络恢复后 MQTT 自动重连成功
 */
static void test_mqtt_reconnects_after_network_recovery() {
    TestLog::testStart("MQTT reconnects after network recovery");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    MQTTConfig config = buildDefaultMQTTConfig();

    // 先失败回退 AP
    bool ok = switchToNetwork(mgr, mqtt, config,
                              MockMultiNetworkManager::NetType::NET_4G, false);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("4G failed, AP fallback active");

    // 恢复为 WiFi STA
    ok = switchToNetwork(mgr, mqtt, config,
                         MockMultiNetworkManager::NetType::NET_WIFI, true);
    TEST_ASSERT_TRUE(ok);
    assertMQTTConnected(mqtt);
    assertNetworkConnected(mgr);
    TestLog::step("Network recovered to WiFi STA, MQTT reconnected");

    TestLog::testEnd(true);
}

// ========== 测试组入口 ==========

void test_network_mqtt_integration_group() {
    TestLog::groupStart("Network-MQTT Integration Tests");

    RUN_TEST(test_switch_wifi_to_ethernet_reconnects_mqtt);
    RUN_TEST(test_switch_ethernet_to_4g_reconnects_mqtt);
    RUN_TEST(test_switch_4g_to_wifi_reconnects_mqtt);
    RUN_TEST(test_4g_failure_fallback_ap_mqtt_stops_then_recovers);
    RUN_TEST(test_restart_network_4g_failure_syncs_network_type);
    RUN_TEST(test_restart_network_ethernet_failure_syncs_network_type);
    RUN_TEST(test_ethernet_auto_reconnect_preserves_mqtt_state);
    RUN_TEST(test_network_switch_sequence_stability);
    RUN_TEST(test_network_switch_no_memory_leak);
    RUN_TEST(test_mqtt_not_reconnect_when_network_down);
    RUN_TEST(test_mqtt_reconnects_after_network_recovery);

    TestLog::groupEnd();
}
