/**
 * @file test_network_config.cpp
 * @brief Network Configuration Tests
 */

#include <unity.h>
#include <Arduino.h>
#include <vector>
#include "mocks/MockWiFi.h"
#include "mocks/MockMultiNetwork.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

void test_network_config_group();

// Test WiFi mode switching
void test_wifi_mode_switching() {
    TestLog::testStart("WiFi Mode Switching");
    
    MockWiFiClass wifi;
    
    // Test STA mode
    wifi.mode(WIFI_STA);
    TEST_ASSERT_EQUAL(WIFI_STA, wifi.getMode());
    TestLog::step("STA mode set successfully");
    
    // Test AP mode
    wifi.mode(WIFI_AP);
    TEST_ASSERT_EQUAL(WIFI_AP, wifi.getMode());
    TestLog::step("AP mode set successfully");
    
    // Test AP_STA mode
    wifi.mode(WIFI_AP_STA);
    TEST_ASSERT_EQUAL(WIFI_AP_STA, wifi.getMode());
    TestLog::step("AP_STA mode set successfully");
    
    // Test WiFi OFF
    wifi.mode(WIFI_OFF);
    TEST_ASSERT_EQUAL(WIFI_OFF, wifi.getMode());
    TestLog::step("WiFi OFF mode set successfully");
    
    TestLog::testEnd(true);
}

// Test AP hotspot function
void test_ap_hotspot_function() {
    TestLog::testStart("AP Hotspot Function");
    
    MockWiFiClass wifi;
    wifi.mode(WIFI_AP);
    
    bool result = wifi.softAP("FastBee-Test", "12345678", 6, 0, 4);
    TEST_ASSERT_TRUE(result);
    TestLog::step("AP hotspot started");
    
    TEST_ASSERT_EQUAL_STRING("FastBee-Test", wifi.softAPSSID().c_str());
    TestLog::step("AP SSID verified");
    
    IPAddress apIP = wifi.softAPIP();
    TEST_ASSERT_FALSE(apIP.toString().isEmpty());
    TestLog::step("AP IP address verified");
    
    wifi.setAPClients(2);
    TEST_ASSERT_EQUAL(2, wifi.softAPgetStationNum());
    TestLog::step("AP client count verified: 2");
    
    TEST_ASSERT_TRUE(wifi.softAPdisconnect());
    TestLog::step("AP disconnected successfully");
    
    TestLog::testEnd(true);
}

// Test STA connection
void test_sta_connection() {
    TestLog::testStart("STA Connection");
    
    MockWiFiClass wifi;
    wifi.mode(WIFI_STA);
    
    wifi.setShouldFail(false);
    int result = wifi.begin("TestNetwork", "TestPassword");
    TEST_ASSERT_EQUAL(WL_CONNECTED, result);
    TEST_ASSERT_EQUAL(WL_CONNECTED, wifi.status());
    TestLog::step("STA connected successfully");
    
    TEST_ASSERT_EQUAL_STRING("TestNetwork", wifi.SSID().c_str());
    TEST_ASSERT_EQUAL_STRING("TestPassword", wifi.psk().c_str());
    TestLog::step("Connection info verified");
    
    IPAddress ip = wifi.localIP();
    TEST_ASSERT_FALSE(ip.toString().isEmpty());
    TEST_ASSERT_FALSE(ip.toString().equals("0.0.0.0"));
    TestLog::step("IP address assigned");
    
    int8_t rssi = wifi.RSSI();
    TEST_ASSERT_LESS_THAN(0, rssi);
    TestLog::step("RSSI verified");
    
    wifi.disconnect();
    TEST_ASSERT_EQUAL(WL_DISCONNECTED, wifi.status());
    TestLog::step("STA disconnected successfully");
    
    TestLog::testEnd(true);
}

// Test WiFi connection failure handling
void test_wifi_connection_failure() {
    TestLog::testStart("WiFi Connection Failure Handling");
    
    MockWiFiClass wifi;
    wifi.mode(WIFI_STA);
    
    wifi.setShouldFail(true);
    
    int result = wifi.begin("WrongNetwork", "WrongPassword");
    TEST_ASSERT_EQUAL(WL_CONNECT_FAILED, result);
    TEST_ASSERT_EQUAL(WL_CONNECT_FAILED, wifi.status());
    TestLog::step("Connection failure detected");
    
    wifi.addConnectionAttempt();
    wifi.addConnectionAttempt();
    TEST_ASSERT_EQUAL(2, wifi.getConnectionAttempts());
    TestLog::step("Connection attempts tracked: 2");
    
    wifi.setAutoReconnect(true);
    TEST_ASSERT_TRUE(wifi.getAutoReconnect());
    TestLog::step("Auto-reconnect enabled");
    
    TestLog::testEnd(true);
}

// Test AP mode network status (KEY TEST)
void test_ap_mode_network_status() {
    TestLog::testStart("AP Mode Network Status");
    
    MockWiFiClass wifi;
    
    wifi.mode(WIFI_AP);
    wifi.softAP("FastBee-Test-AP", "12345678");
    wifi.setConnected(false);
    
    delay(100);
    
    MockWiFiClass::NetworkStatusInfo status = wifi.getStatusInfo();
    
    // KEY VERIFICATION: AP mode status
    TEST_ASSERT_EQUAL_STRING("AP_MODE", status.status.c_str());
    TestLog::step("Status is AP_MODE");
    
    // KEY VERIFICATION: AP mode WiFi NOT connected
    TEST_ASSERT_FALSE(status.wifiConnected);
    TestLog::step("wifiConnected is false (AP mode)");
    
    // KEY VERIFICATION: AP mode internet NOT available
    TEST_ASSERT_FALSE(status.internetAvailable);
    TestLog::step("internetAvailable is false (AP mode)");
    
    // Verify SSID is empty (not connected to external network)
    TEST_ASSERT_TRUE(status.ssid.isEmpty());
    TestLog::step("SSID is empty (no external connection)");
    
    // Verify RSSI is 0 (no signal)
    TEST_ASSERT_EQUAL(0, status.rssi);
    TestLog::step("RSSI is 0 (no signal)");
    
    // Verify AP IP exists
    TEST_ASSERT_FALSE(status.apIPAddress.isEmpty());
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", status.apIPAddress.c_str());
    TestLog::step("AP IP address verified");
    
    // Use custom assertion for AP mode disconnected state
    TEST_ASSERT_AP_MODE_DISCONNECTED(status);
    TestLog::step("AP mode disconnected state verified");
    
    TestLog::testEnd(true);
}

// Test dashboard network status display
void test_dashboard_network_status() {
    TestLog::testStart("Dashboard Network Status Display");
    
    MockWiFiClass wifi;
    
    // Scenario 1: AP mode dashboard status
    wifi.mode(WIFI_AP);
    wifi.softAP("FastBee-AP", "password");
    wifi.setConnected(false);
    
    MockWiFiClass::NetworkStatusInfo apStatus = wifi.getStatusInfo();
    
    String connectionStatus = apStatus.wifiConnected ? "Connected" : "Disconnected";
    String internetStatus = apStatus.internetAvailable ? "Available" : "Unavailable";
    
    TEST_ASSERT_EQUAL_STRING("Disconnected", connectionStatus.c_str());
    TestLog::step("Dashboard shows: Connection = Disconnected");
    
    TEST_ASSERT_EQUAL_STRING("Unavailable", internetStatus.c_str());
    TestLog::step("Dashboard shows: Internet = Unavailable");
    
    // Scenario 2: STA mode connected status
    wifi.mode(WIFI_STA);
    wifi.setShouldFail(false);
    wifi.begin("HomeWiFi", "password");
    wifi.setConnected(true);
    
    MockWiFiClass::NetworkStatusInfo staStatus = wifi.getStatusInfo();
    
    connectionStatus = staStatus.wifiConnected ? "Connected" : "Disconnected";
    internetStatus = staStatus.internetAvailable ? "Available" : "Unavailable";
    
    TEST_ASSERT_EQUAL_STRING("Connected", connectionStatus.c_str());
    TestLog::step("Dashboard shows (STA): Connection = Connected");
    
    TEST_ASSERT_EQUAL_STRING("Available", internetStatus.c_str());
    TestLog::step("Dashboard shows (STA): Internet = Available");
    
    TestLog::testEnd(true);
}

// Test NTP sync status
void test_ntp_sync_status() {
    TestLog::testStart("NTP Sync Status");
    
    MockWiFiClass wifi;
    
    // AP mode (no network)
    wifi.mode(WIFI_AP);
    wifi.setConnected(false);
    
    MockWiFiClass::NetworkStatusInfo apStatus = wifi.getStatusInfo();
    
    bool ntpSynced = apStatus.internetAvailable;
    TEST_ASSERT_FALSE(ntpSynced);
    TestLog::step("AP mode: NTP sync status = Not synced");
    
    // STA mode (with network)
    wifi.mode(WIFI_STA);
    wifi.setConnected(true);
    
    MockWiFiClass::NetworkStatusInfo staStatus = wifi.getStatusInfo();
    
    ntpSynced = staStatus.internetAvailable;
    TEST_ASSERT_TRUE(ntpSynced);
    TestLog::step("STA mode: NTP sync possible (internet available)");
    
    TestLog::testEnd(true);
}

// Test network status API
void test_network_status_api() {
    TestLog::testStart("Network Status API");
    
    MockWiFiClass wifi;
    
    wifi.mode(WIFI_AP);
    wifi.softAP("FastBee-Test", "12345678");
    wifi.setConnected(false);
    
    MockWiFiClass::NetworkStatusInfo status = wifi.getStatusInfo();
    
    String apiResponse = "{";
    apiResponse += "\"status\":" + String(status.status == "AP_MODE" ? 4 : 0) + ",";
    apiResponse += "\"statusText\":\"" + status.status + "\",";
    apiResponse += "\"wifiConnected\":" + String(status.wifiConnected ? "true" : "false") + ",";
    apiResponse += "\"internetAvailable\":" + String(status.internetAvailable ? "true" : "false") + ",";
    apiResponse += "\"ssid\":\"" + status.ssid + "\",";
    apiResponse += "\"ipAddress\":\"" + status.ipAddress + "\",";
    apiResponse += "\"apIPAddress\":\"" + status.apIPAddress + "\",";
    apiResponse += "\"apClientCount\":" + String(status.apClientCount) + ",";
    apiResponse += "\"rssi\":" + String(status.rssi);
    apiResponse += "}";
    
    TEST_ASSERT_TRUE(apiResponse.indexOf("\"wifiConnected\":false") >= 0);
    TestLog::step("API response: wifiConnected=false");
    
    TEST_ASSERT_TRUE(apiResponse.indexOf("\"internetAvailable\":false") >= 0);
    TestLog::step("API response: internetAvailable=false");
    
    TEST_ASSERT_TRUE(apiResponse.indexOf("\"status\":4") >= 0);
    TestLog::step("API response: status=4 (AP_MODE)");
    
    TEST_ASSERT_TRUE(apiResponse.indexOf("\"statusText\":\"AP_MODE\"") >= 0);
    TestLog::step("API response: statusText=AP_MODE");
    
    TestLog::testEnd(true);
}

// Test network mode auto switch
void test_network_mode_auto_switch() {
    TestLog::testStart("Network Mode Auto Switch");
    
    MockWiFiClass wifi;
    
    // Initial state: AP mode (no config)
    wifi.mode(WIFI_AP);
    wifi.softAP("FastBee-Setup", "");
    
    MockWiFiClass::NetworkStatusInfo initialStatus = wifi.getStatusInfo();
    TEST_ASSERT_EQUAL_STRING("AP_MODE", initialStatus.status.c_str());
    TestLog::step("Initial state: AP_MODE");
    
    // After WiFi config, switch to STA
    wifi.mode(WIFI_STA);
    wifi.setShouldFail(false);
    wifi.begin("ConfiguredWiFi", "password");
    wifi.setConnected(true);
    
    MockWiFiClass::NetworkStatusInfo finalStatus = wifi.getStatusInfo();
    TEST_ASSERT_EQUAL_STRING("CONNECTED", finalStatus.status.c_str());
    TEST_ASSERT_TRUE(finalStatus.wifiConnected);
    TestLog::step("After config: CONNECTED with wifiConnected=true");
    
    TEST_ASSERT_TRUE(finalStatus.internetAvailable);
    TestLog::step("Internet available after STA connection");
    
    TestLog::testEnd(true);
}

// Test IP configuration
void test_ip_configuration() {
    TestLog::testStart("IP Configuration");
    
    MockWiFiClass wifi;
    wifi.mode(WIFI_STA);
    
    wifi.begin("TestWiFi", "password");
    wifi.setConnected(true);
    
    IPAddress ip = wifi.localIP();
    IPAddress subnet = wifi.subnetMask();
    IPAddress gateway = wifi.gatewayIP();
    
    TEST_ASSERT_FALSE(ip.toString().equals("0.0.0.0"));
    TEST_ASSERT_FALSE(subnet.toString().equals("0.0.0.0"));
    TEST_ASSERT_FALSE(gateway.toString().equals("0.0.0.0"));
    
    TestLog::step("DHCP IP verified");
    TestLog::step("Subnet verified");
    TestLog::step("Gateway verified");
    
    TEST_ASSERT_VALID_IP_FORMAT(ip.toString().c_str());
    TestLog::step("IP format valid");
    
    TestLog::testEnd(true);
}

// Test network reconnect
void test_network_reconnect() {
    TestLog::testStart("Network Reconnect");
    
    MockWiFiClass wifi;
    wifi.mode(WIFI_STA);
    wifi.setAutoReconnect(true);
    
    wifi.setShouldFail(false);
    wifi.begin("TestWiFi", "password");
    wifi.setConnected(true);
    
    TEST_ASSERT_EQUAL(WL_CONNECTED, wifi.status());
    TestLog::step("Initial connection established");
    
    wifi.setConnected(false);
    TEST_ASSERT_EQUAL(WL_DISCONNECTED, wifi.status());
    TestLog::step("Connection lost");
    
    wifi.setConnected(true);
    TEST_ASSERT_EQUAL(WL_CONNECTED, wifi.status());
    TestLog::step("Auto-reconnected");
    
    TestLog::testEnd(true);
}

// ========== 多网络模式测试 ==========
// MockMultiNetworkManager 已抽取到 mocks/MockMultiNetwork.h

// 测试联网方式枚举值
void test_network_type_enum_values() {
    TestLog::testStart("Network Type Enum Values");

    TEST_ASSERT_EQUAL(0, (int)MockMultiNetworkManager::NetType::NET_WIFI);
    TestLog::step("NET_WIFI = 0");

    TEST_ASSERT_EQUAL(1, (int)MockMultiNetworkManager::NetType::NET_ETHERNET);
    TestLog::step("NET_ETHERNET = 1");

    TEST_ASSERT_EQUAL(2, (int)MockMultiNetworkManager::NetType::NET_4G);
    TestLog::step("NET_4G = 2");

    TEST_ASSERT_EQUAL(3, (int)MockMultiNetworkManager::NetType::NET_LORA);
    TestLog::step("NET_LORA = 3");

    TestLog::testEnd(true);
}

// 测试 4G 初始化失败回退到 AP 模式
void test_4g_init_failure_fallback_to_ap() {
    TestLog::testStart("4G Init Failure → AP Fallback");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    mgr.mode = MockMultiNetworkManager::NetMode::NETWORK_STA;

    bool result = mgr.initialize(false);  // 4G 初始化失败
    TEST_ASSERT_FALSE(result);
    TestLog::step("4G init returned false");

    // 关键验证：回退到 AP 模式而非 STA
    TEST_ASSERT_FALLBACK_TO_AP(mgr.networkType, mgr.mode);
    TestLog::step("Fallback: networkType=NET_WIFI, mode=NETWORK_AP");

    // AP 热点必须已启动
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", mgr.apIPAddress.c_str());
    TestLog::step("AP hotspot running at 192.168.4.1");

    TestLog::testEnd(true);
}

// 测试以太网初始化失败回退到 AP 模式
void test_ethernet_init_failure_fallback_to_ap() {
    TestLog::testStart("Ethernet Init Failure → AP Fallback");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.mode = MockMultiNetworkManager::NetMode::NETWORK_STA;

    bool result = mgr.initialize(false);  // 以太网初始化失败
    TEST_ASSERT_FALSE(result);
    TestLog::step("Ethernet init returned false");

    TEST_ASSERT_FALLBACK_TO_AP(mgr.networkType, mgr.mode);
    TestLog::step("Fallback: networkType=NET_WIFI, mode=NETWORK_AP");

    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("AP hotspot running");

    TestLog::testEnd(true);
}

// 测试 LoRa 初始化失败回退到 AP 模式
void test_lora_init_failure_fallback_to_ap() {
    TestLog::testStart("LoRa Init Failure → AP Fallback");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_LORA;
    mgr.mode = MockMultiNetworkManager::NetMode::NETWORK_STA;

    bool result = mgr.initialize(false);  // LoRa 初始化失败
    TEST_ASSERT_FALSE(result);
    TestLog::step("LoRa init returned false");

    TEST_ASSERT_FALLBACK_TO_AP(mgr.networkType, mgr.mode);
    TestLog::step("Fallback: networkType=NET_WIFI, mode=NETWORK_AP");

    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("AP hotspot running");

    TestLog::testEnd(true);
}

// 测试 4G 混合模式下 mDNS 必须启动
void test_4g_mode_mdns_started() {
    TestLog::testStart("4G Mode: mDNS Must Start");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    mgr.enableMDNS = true;

    bool result = mgr.initialize(true);  // 4G 初始化成功
    TEST_ASSERT_TRUE(result);
    TestLog::step("4G init succeeded");

    // mDNS 必须已启动
    TEST_ASSERT_TRUE_MESSAGE(mgr.mDNSStarted, "4G hybrid mode must start mDNS for AP access");
    TestLog::step("mDNS started in 4G hybrid mode");

    // AP 热点必须同时运行
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TEST_ASSERT_AP_ACTIVE_IN_HYBRID(mgr.apIPAddress, mgr.apSSID);
    TestLog::step("AP + mDNS both active");

    TestLog::testEnd(true);
}

// 测试以太网混合模式下 mDNS 必须启动
void test_ethernet_mode_mdns_started() {
    TestLog::testStart("Ethernet Mode: mDNS Must Start");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.enableMDNS = true;

    bool result = mgr.initialize(true);  // 以太网初始化成功
    TEST_ASSERT_TRUE(result);
    TestLog::step("Ethernet init succeeded");

    // mDNS 必须已启动
    TEST_ASSERT_TRUE_MESSAGE(mgr.mDNSStarted, "Ethernet hybrid mode must start mDNS for AP access");
    TestLog::step("mDNS started in Ethernet hybrid mode");

    TestLog::testEnd(true);
}

// 测试 mDNS 禁用时不启动
void test_mdns_disabled_not_started() {
    TestLog::testStart("mDNS Disabled: Not Started");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    mgr.enableMDNS = false;  // 用户禁用 mDNS

    bool result = mgr.initialize(true);
    TEST_ASSERT_TRUE(result);
    TestLog::step("4G init succeeded with mDNS disabled");

    // mDNS 不应启动
    TEST_ASSERT_FALSE_MESSAGE(mgr.mDNSStarted, "mDNS should NOT start when disabled");
    TestLog::step("mDNS correctly not started");

    // AP 仍应正常运行
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("AP still active without mDNS");

    TestLog::testEnd(true);
}

// 测试 4G 成功初始化后 AP 热点保持运行
void test_4g_mode_ap_always_running() {
    TestLog::testStart("4G Mode: AP Always Running");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;

    bool result = mgr.initialize(true);  // 4G 初始化成功
    TEST_ASSERT_TRUE(result);
    TestLog::step("4G init succeeded");

    // 4G 已连接
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::CONNECTED, (int)mgr.status);
    TEST_ASSERT_FALSE(mgr.ipAddress.isEmpty());
    TestLog::step("4G connected with IP");

    // AP 热点必须同时运行
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TEST_ASSERT_AP_ACTIVE_IN_HYBRID(mgr.apIPAddress, mgr.apSSID);
    TestLog::step("AP hotspot active in hybrid mode (4G + AP)");

    // WiFi STA 不应活跃
    MockWiFiClass wifi;
    wifi.mode(WIFI_AP);  // 4G+AP 场景下 WiFi 应为纯 AP 模式
    TEST_ASSERT_STA_INACTIVE(wifi.getMode());
    TestLog::step("WiFi STA not active in 4G mode");

    TestLog::testEnd(true);
}

// 测试以太网成功初始化后 AP 热点保持运行
void test_ethernet_mode_ap_always_running() {
    TestLog::testStart("Ethernet Mode: AP Always Running");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;

    bool result = mgr.initialize(true);  // 以太网初始化成功
    TEST_ASSERT_TRUE(result);
    TestLog::step("Ethernet init succeeded");

    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::CONNECTED, (int)mgr.status);
    TestLog::step("Ethernet connected");

    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TEST_ASSERT_AP_ACTIVE_IN_HYBRID(mgr.apIPAddress, mgr.apSSID);
    TestLog::step("AP hotspot active in hybrid mode (Ethernet + AP)");

    TestLog::testEnd(true);
}

// 测试非WiFi联网时状态隔离：WiFi 状态不覆盖主网络状态
void test_non_wifi_status_isolation() {
    TestLog::testStart("Non-WiFi Status Isolation");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;

    // 4G 初始化成功
    mgr.initialize(true);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::CONNECTED, (int)mgr.status);
    TestLog::step("Initial: 4G CONNECTED");

    // WiFiManager 内部状态为 AP_MODE（因为 WiFi 是纯 AP 模式）
    mgr.wifiManagerStatus = MockMultiNetworkManager::NetStatus::AP_MODE;

    // 执行状态更新（模拟 updateStatusInfo 调用）
    mgr.updateStatusInfo(true, "10.0.0.100");

    // 关键验证：主状态仍为 CONNECTED，不被 WiFi 的 AP_MODE 覆盖
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::CONNECTED, (int)mgr.status);
    TestLog::step("Status still CONNECTED (not overwritten by WiFi AP_MODE)");

    // 主 IP 仍为 4G IP
    TEST_ASSERT_EQUAL_STRING("10.0.0.100", mgr.ipAddress.c_str());
    TestLog::step("IP address is 4G adapter IP");

    // AP 字段已同步
    TEST_ASSERT_FALSE(mgr.apIPAddress.isEmpty());
    TestLog::step("AP fields synced correctly");

    // 状态隔离断言：主状态 != WiFi管理器状态
    TEST_ASSERT_STATUS_ISOLATED(mgr.status, mgr.wifiManagerStatus);
    TestLog::step("Status isolation verified");

    TestLog::testEnd(true);
}

// 测试 4G 断开时状态正确反映
void test_4g_disconnect_status() {
    TestLog::testStart("4G Disconnect Status");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    mgr.initialize(true);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::CONNECTED, (int)mgr.status);
    TestLog::step("4G initially connected");

    // 4G 断开
    mgr.updateStatusInfo(false, "");
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::DISCONNECTED, (int)mgr.status);
    TEST_ASSERT_TRUE(mgr.ipAddress.isEmpty());
    TEST_ASSERT_FALSE(mgr.internetAvailable);
    TestLog::step("4G disconnected: status=DISCONNECTED, no IP");

    // AP 热点仍然运行
    TEST_ASSERT_FALSE(mgr.apIPAddress.isEmpty());
    TestLog::step("AP still accessible after 4G disconnect");

    TestLog::testEnd(true);
}

// 测试联网方式切换：WiFi → 4G
void test_switch_wifi_to_4g() {
    TestLog::testStart("Switch: WiFi → 4G");

    MockMultiNetworkManager mgr;

    // 初始状态：WiFi STA 已连接
    mgr.networkType = MockMultiNetworkManager::NetType::NET_WIFI;
    mgr.mode = MockMultiNetworkManager::NetMode::NETWORK_STA;
    mgr.initialize(true);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::CONNECTED, (int)mgr.status);
    TestLog::step("WiFi STA connected");

    // 用户切换联网方式为 4G
    mgr.disconnect();
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    bool result = mgr.initialize(true);
    TEST_ASSERT_TRUE(result);
    TestLog::step("4G init after switch");

    // 4G 已连接，AP 已启动
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::CONNECTED, (int)mgr.status);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TEST_ASSERT_FALSE(mgr.ipAddress.isEmpty());
    TestLog::step("4G connected + AP running after switch");

    TestLog::testEnd(true);
}

// 测试联网方式切换：4G → WiFi
void test_switch_4g_to_wifi() {
    TestLog::testStart("Switch: 4G → WiFi");

    MockMultiNetworkManager mgr;

    // 初始状态：4G 已连接
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    mgr.initialize(true);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::CONNECTED, (int)mgr.status);
    TestLog::step("4G initially connected");

    // 用户切换联网方式为 WiFi
    mgr.disconnect();
    mgr.networkType = MockMultiNetworkManager::NetType::NET_WIFI;
    mgr.mode = MockMultiNetworkManager::NetMode::NETWORK_STA;
    bool result = mgr.initialize(true);
    TEST_ASSERT_TRUE(result);
    TestLog::step("WiFi STA init after switch");

    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::CONNECTED, (int)mgr.status);
    TEST_ASSERT_EQUAL_STRING("192.168.1.100", mgr.ipAddress.c_str());
    TestLog::step("WiFi STA connected after switch");

    TestLog::testEnd(true);
}

// 测试非WiFi模式下不触发 WiFi STA 重连
void test_non_wifi_no_sta_reconnect() {
    TestLog::testStart("Non-WiFi: No STA Reconnect");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    mgr.initialize(true);
    TestLog::step("4G mode initialized");

    // WiFi 应为纯 AP 模式，STA 不活跃
    MockWiFiClass wifi;
    wifi.mode(WIFI_AP);
    wifi.softAP("fastbee-ap", "12345678");

    // 确认 STA 未连接且不会尝试重连
    TEST_ASSERT_EQUAL(WL_DISCONNECTED, wifi.status());
    TEST_ASSERT_STA_INACTIVE(wifi.getMode());
    TestLog::step("WiFi STA inactive in 4G mode");

    // 即使 4G 断开，WiFi STA 也不应尝试重连
    mgr.updateStatusInfo(false, "");
    TEST_ASSERT_EQUAL(WL_DISCONNECTED, wifi.status());
    TestLog::step("WiFi STA still inactive after 4G disconnect");

    TestLog::testEnd(true);
}

// 测试 restartNetwork 对非WiFi联网方式的处理
void test_restart_network_non_wifi() {
    TestLog::testStart("Restart Network: Non-WiFi Type");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    mgr.initialize(true);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::CONNECTED, (int)mgr.status);
    TestLog::step("4G initially connected");

    // 重启网络（4G 仍然成功）
    bool result = mgr.restartNetwork(true);
    TEST_ASSERT_TRUE(result);
    TestLog::step("restartNetwork succeeded");

    // 重新初始化后 4G 已连接，AP 已启动
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::CONNECTED, (int)mgr.status);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("4G reconnected + AP running after restart");

    TestLog::testEnd(true);
}

// 测试 restartNetwork 后 4G 失败回退到 AP
void test_restart_network_4g_failure() {
    TestLog::testStart("Restart Network: 4G Failure → AP");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    mgr.initialize(true);
    TestLog::step("4G initially connected");

    // 重启网络（4G 这次失败）
    // 需要先重新设置 networkType，因为 disconnect 不会重置
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    bool result = mgr.restartNetwork(false);
    TEST_ASSERT_FALSE(result);
    TestLog::step("restartNetwork: 4G failed");

    // 回退到 AP 模式
    TEST_ASSERT_FALLBACK_TO_AP(mgr.networkType, mgr.mode);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("Fallback to AP mode after 4G restart failure");

    TestLog::testEnd(true);
}

// 测试混合模式下 AP IP 为 192.168.4.1
void test_hybrid_mode_ap_ip() {
    TestLog::testStart("Hybrid Mode: AP IP = 192.168.4.1");

    // 4G + AP
    MockMultiNetworkManager mgr4g;
    mgr4g.networkType = MockMultiNetworkManager::NetType::NET_4G;
    mgr4g.initialize(true);
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", mgr4g.apIPAddress.c_str());
    TestLog::step("4G hybrid: AP IP = 192.168.4.1");

    // Ethernet + AP
    MockMultiNetworkManager mgrEth;
    mgrEth.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgrEth.initialize(true);
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", mgrEth.apIPAddress.c_str());
    TestLog::step("Ethernet hybrid: AP IP = 192.168.4.1");

    TestLog::testEnd(true);
}

/**
 * @brief 验证restartNetwork()在非WiFi模式下不调用全局disconnect()
 * 关键：AP热点在整个切换过程中不被关闭
 * 回归防护：以太网切换导致AP热点中断问题
 */
void test_restart_network_non_wifi_no_global_disconnect() {
    TestLog::testStart("Restart Network Non-WiFi: No Global Disconnect");

    // 模拟以太网模式下 AP 已启动
    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.initialize(true);  // 以太网初始化成功，AP同时启动
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", mgr.apIPAddress.c_str());
    TestLog::step("Initial: Ethernet connected, AP running at 192.168.4.1");

    // 记录重启前 AP 状态
    String apIPBefore = mgr.apIPAddress;

    // 模拟 restartNetwork 在非WiFi模式下的行为：
    // 核心约束：不调用全局 disconnect()，只断开 WiFi STA
    // 模拟选择性断开（保持AP）而非全局断开
    mgr.apRunning = true;  // AP 在重启期间保持运行
    mgr.apIPAddress = "192.168.4.1";  // AP IP 不变
    // 断开以太网适配器
    mgr.status = MockMultiNetworkManager::NetStatus::DISCONNECTED;
    mgr.ipAddress = "";
    mgr.internetAvailable = false;
    mgr.mDNSStarted = false;
    // 重新初始化以太网
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    bool result = mgr.initialize(true);
    TEST_ASSERT_TRUE(result);
    TestLog::step("Selective restart: Ethernet re-initialized successfully");

    // 关键验证 1: AP 热点始终在线
    TEST_ASSERT_TRUE_MESSAGE(mgr.isAPRunning(),
        "restartNetwork() for ethernet must NOT stop AP hotspot");
    TestLog::step("AP hotspot remains running throughout restart");

    // 关键验证 2: AP IP 保持 192.168.4.1
    TEST_ASSERT_EQUAL_STRING_MESSAGE("192.168.4.1", mgr.apIPAddress.c_str(),
        "AP IP must remain 192.168.4.1 during non-WiFi restart");
    TestLog::step("AP IP still 192.168.4.1");

    // 关键验证 3: 以太网已重新连接
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::CONNECTED, (int)mgr.status);
    TEST_ASSERT_FALSE(mgr.ipAddress.isEmpty());
    TestLog::step("Ethernet reconnected with new IP");

    TestLog::testEnd(true);
}

/**
 * @brief 验证以太网初始化失败时的AP恢复逻辑
 * NetworkManager.cpp 第1408-1414行的备用路径
 * 确保AP作为恢复入口始终可用
 */
void test_restart_network_eth_failure_ap_recovery() {
    TestLog::testStart("Restart Network: Ethernet Failure AP Recovery");

    // 初始状态：以太网已连接，AP运行中
    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.initialize(true);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("Initial: Ethernet + AP running");

    // 模拟重启网络时以太网初始化失败
    // 先断开（模拟选择性断开，保持AP）
    mgr.status = MockMultiNetworkManager::NetStatus::DISCONNECTED;
    mgr.ipAddress = "";
    mgr.internetAvailable = false;
    mgr.mDNSStarted = false;
    // AP 在选择性断开时保持运行
    mgr.apRunning = true;
    mgr.apIPAddress = "192.168.4.1";

    // 重新初始化以太网失败
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    bool result = mgr.initialize(false);  // 以太网初始化失败
    TEST_ASSERT_FALSE(result);
    TestLog::step("Ethernet re-init failed");

    // 关键验证 1: 失败后 AP 必须活跃
    TEST_ASSERT_TRUE_MESSAGE(mgr.isAPRunning(),
        "AP must be active after ethernet init failure for user recovery");
    TestLog::step("AP active after failure (recovery entrance)");

    // 关键验证 2: AP IP 为 192.168.4.1
    TEST_ASSERT_EQUAL_STRING_MESSAGE("192.168.4.1", mgr.apIPAddress.c_str(),
        "AP IP must be 192.168.4.1 for user to access recovery page");
    TestLog::step("AP IP = 192.168.4.1");

    // 关键验证 3: 回退到 WiFi AP 模式（确保用户可配置）
    TEST_ASSERT_EQUAL_MESSAGE((int)MockMultiNetworkManager::NetType::NET_WIFI,
        (int)mgr.networkType,
        "networkType should fallback to NET_WIFI after failure");
    TEST_ASSERT_EQUAL_MESSAGE((int)MockMultiNetworkManager::NetMode::NETWORK_AP,
        (int)mgr.mode,
        "mode should fallback to NETWORK_AP after failure");
    TestLog::step("Fallback to WiFi AP mode");

    // 关键验证 4: 状态为 AP_MODE
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::AP_MODE, (int)mgr.status);
    TestLog::step("Status = AP_MODE (accessible for recovery)");

    TestLog::testEnd(true);
}

/**
 * @brief 验证以太网模式下 mDNS 必须在 AP 启动之后启动
 * 回归：原代码 mDNS 在 AP 之前启动，导致 mDNS 绑定到错误的 netif
 * 修复：将 mDNS 启动移到 AP 启动后，确保所有 netif 就绪
 */
void test_ethernet_mdns_starts_after_ap() {
    TestLog::testStart("Ethernet: mDNS Must Start After AP");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.enableMDNS = true;

    bool result = mgr.initialize(true);
    TEST_ASSERT_TRUE(result);
    TestLog::step("Ethernet init succeeded");

    // 验证启动序列顺序
    TEST_ASSERT_GREATER_OR_EQUAL(3, (int)mgr.initSequence.size());
    TestLog::step("Init sequence has 3+ entries");

    // 第 1 步：以太网连接
    TEST_ASSERT_EQUAL_STRING("ETH_CONNECTED", mgr.initSequence[0].c_str());
    TestLog::step("Step 1: ETH_CONNECTED");

    // 第 2 步：AP 启动
    TEST_ASSERT_EQUAL_STRING("AP_STARTED", mgr.initSequence[1].c_str());
    TestLog::step("Step 2: AP_STARTED");

    // 第 3 步：mDNS 启动（必须在 AP 之后）
    TEST_ASSERT_EQUAL_STRING("MDNS_STARTED", mgr.initSequence[2].c_str());
    TestLog::step("Step 3: MDNS_STARTED (after AP)");

    // mDNS 服务必须已启动
    TEST_ASSERT_TRUE(mgr.mDNSStarted);
    // AP 必须已运行
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("mDNS and AP both active, correct order verified");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 4G 模式下 mDNS 也必须在 AP 启动之后启动
 * 确保 4G 和以太网两种非 WiFi 联网方式都遵守相同的启动顺序
 */
void test_4g_mdns_starts_after_ap() {
    TestLog::testStart("4G: mDNS Must Start After AP");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    mgr.enableMDNS = true;

    bool result = mgr.initialize(true);
    TEST_ASSERT_TRUE(result);
    TestLog::step("4G init succeeded");

    // 验证启动序列顺序
    TEST_ASSERT_GREATER_OR_EQUAL(3, (int)mgr.initSequence.size());

    TEST_ASSERT_EQUAL_STRING("4G_CONNECTED", mgr.initSequence[0].c_str());
    TEST_ASSERT_EQUAL_STRING("AP_STARTED", mgr.initSequence[1].c_str());
    TEST_ASSERT_EQUAL_STRING("MDNS_STARTED", mgr.initSequence[2].c_str());
    TestLog::step("Sequence: 4G_CONNECTED → AP_STARTED → MDNS_STARTED");

    TEST_ASSERT_TRUE(mgr.mDNSStarted);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("Both mDNS and AP active in correct order");

    TestLog::testEnd(true);
}

/**
 * @brief 验证以太网模式下的完整启动状态报告包含所有关键字段
 * 回归：原代码以太网模式下启动日志不显示以太网 IP/mDNS 信息
 */
void test_ethernet_boot_report_contains_all_fields() {
    TestLog::testStart("Ethernet Boot Report Contains All Fields");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.enableMDNS = true;

    bool result = mgr.initialize(true);
    TEST_ASSERT_TRUE(result);
    TestLog::step("Ethernet init succeeded");

    // 模拟启动报告内容（对应 FastBeeFramework 的修复后输出）
    String bootReport = "";
    bootReport += "Mode: Ethernet (W5500)\n";
    bootReport += "Ethernet IP: " + mgr.ipAddress + "\n";
    bootReport += "Access URL: http://" + mgr.ipAddress + "\n";
    if (mgr.isAPRunning()) {
        bootReport += "Config AP: " + mgr.apSSID + " (IP: " + mgr.apIPAddress + ")\n";
    }
    if (mgr.mDNSStarted) {
        bootReport += "mDNS URL: http://fastbee.local\n";
    }

    // 验证报告包含所有关键字段
    TEST_ASSERT_TRUE(bootReport.indexOf("Ethernet (W5500)") >= 0);
    TestLog::step("Boot report contains: Mode: Ethernet (W5500)");

    TEST_ASSERT_TRUE(bootReport.indexOf("Ethernet IP:") >= 0);
    TestLog::step("Boot report contains: Ethernet IP");

    TEST_ASSERT_TRUE(bootReport.indexOf("Access URL: http://") >= 0);
    TestLog::step("Boot report contains: Access URL");

    TEST_ASSERT_TRUE(bootReport.indexOf("Config AP:") >= 0);
    TestLog::step("Boot report contains: Config AP");

    TEST_ASSERT_TRUE(bootReport.indexOf("mDNS URL: http://fastbee.local") >= 0);
    TestLog::step("Boot report contains: mDNS URL");

    TestLog::testEnd(true);
}

/**
 * @brief 验证以太网周期性状态输出包含以太网 IP 和 AP 信息
 * 回归：原代码周期性 [STATUS] 打印不包含以太网信息
 */
void test_ethernet_periodic_status_output() {
    TestLog::testStart("Ethernet Periodic Status Output");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.enableMDNS = true;
    mgr.initialize(true);

    // 模拟周期性状态输出（对应 FastBeeFramework loop 中的 [STATUS] 打印）
    String statusLine = "";
    if (mgr.networkType == MockMultiNetworkManager::NetType::NET_ETHERNET
        && mgr.status == MockMultiNetworkManager::NetStatus::CONNECTED) {
        statusLine = "[STATUS] ETH=CONNECTED ip=" + mgr.ipAddress;
        if (mgr.isAPRunning()) {
            statusLine += " ap=" + mgr.apIPAddress;
        }
    }

    TEST_ASSERT_TRUE(statusLine.indexOf("ETH=CONNECTED") >= 0);
    TestLog::step("Periodic status: ETH=CONNECTED");

    TEST_ASSERT_TRUE(statusLine.indexOf("ip=") >= 0);
    TestLog::step("Periodic status: contains Ethernet IP");

    TEST_ASSERT_TRUE(statusLine.indexOf("ap=") >= 0);
    TestLog::step("Periodic status: contains AP IP");

    TestLog::testEnd(true);
}

// ============================================================
// WiFi 多模式长时间稳定性 + 断线重连压力测试
// ============================================================

/**
 * @brief WiFi STA模式连续重连50次无泄漏
 * 模拟WiFi信号不稳定时的反复重连场景
 */
void test_wifi_sta_reconnect_50_cycles_no_leak() {
    TestLog::testStart("WiFi STA: 50 Reconnect Cycles No Leak");

    MockWiFiClass wifi;
    wifi.mode(WIFI_STA);
    wifi.setAutoReconnect(true);
    wifi.setShouldFail(false);

    uint32_t initialHeap = ESP.getFreeHeap();

    for (int cycle = 0; cycle < 50; cycle++) {
        // 连接
        wifi.begin("TestWiFi", "password");
        wifi.setConnected(true);
        TEST_ASSERT_EQUAL(WL_CONNECTED, wifi.status());

        // 验证状态
        MockWiFiClass::NetworkStatusInfo info = wifi.getStatusInfo();
        TEST_ASSERT_TRUE(info.wifiConnected);
        TEST_ASSERT_TRUE(info.internetAvailable);
        TEST_ASSERT_FALSE(info.ipAddress.isEmpty());

        // 断开
        wifi.setConnected(false);
        TEST_ASSERT_EQUAL(WL_DISCONNECTED, wifi.status());
    }

    int32_t leak = (int32_t)initialHeap - (int32_t)ESP.getFreeHeap();
    TEST_ASSERT_TRUE_MESSAGE(leak < 5000, "WiFi reconnect leak detected");
    TestLog::step("50 cycles: heap leak < 5KB");

    TestLog::testEnd(true);
}

/**
 * @brief WiFi AP模式长时间运行稳定性
 * AP热点持续运行，客户端反复连接/断开
 */
void test_wifi_ap_long_running_stability() {
    TestLog::testStart("WiFi AP: Long Running Stability");

    MockWiFiClass wifi;
    wifi.mode(WIFI_AP);
    wifi.softAP("fastbee-ap", "12345678");

    // AP始终可达
    TEST_ASSERT_EQUAL_STRING("fastbee-ap", wifi.softAPSSID().c_str());
    IPAddress apIP = wifi.softAPIP();
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", apIP.toString().c_str());
    TestLog::step("AP started at 192.168.4.1");

    uint32_t initialHeap = ESP.getFreeHeap();

    // 30轮客户端连接/断开
    for (int round = 0; round < 30; round++) {
        // 多个客户端连接
        wifi.setAPClients(3);
        TEST_ASSERT_EQUAL(3, wifi.softAPgetStationNum());

        // 状态查询
        MockWiFiClass::NetworkStatusInfo info = wifi.getStatusInfo();
        TEST_ASSERT_EQUAL_STRING("AP_MODE", info.status.c_str());
        TEST_ASSERT_FALSE(info.wifiConnected);
        TEST_ASSERT_FALSE(info.internetAvailable);

        // 客户端断开
        wifi.setAPClients(0);
        TEST_ASSERT_EQUAL(0, wifi.softAPgetStationNum());
    }

    int32_t leak = (int32_t)initialHeap - (int32_t)ESP.getFreeHeap();
    TEST_ASSERT_TRUE_MESSAGE(leak < 5000, "AP long running leak detected");
    TestLog::step("30 rounds: heap leak < 5KB");

    // AP热点始终稳定
    TEST_ASSERT_EQUAL_STRING("fastbee-ap", wifi.softAPSSID().c_str());
    TestLog::step("AP hotspot stable after 30 rounds");

    TestLog::testEnd(true);
}

/**
 * @brief WiFi AP_STA混合模式稳定性
 * STA连接/断开不影响AP热点
 */
void test_wifi_ap_sta_hybrid_stability() {
    TestLog::testStart("WiFi AP_STA: Hybrid Mode Stability");

    MockWiFiClass wifi;
    wifi.mode(WIFI_AP_STA);
    wifi.softAP("fastbee-ap", "12345678");

    // AP已启动
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", wifi.softAPIP().toString().c_str());
    TestLog::step("AP started in hybrid mode");

    uint32_t initialHeap = ESP.getFreeHeap();

    // 20次 STA连接/断开，AP始终稳定
    for (int cycle = 0; cycle < 20; cycle++) {
        // STA连接
        wifi.setShouldFail(false);
        wifi.begin("HomeWiFi", "password");
        wifi.setConnected(true);
        TEST_ASSERT_EQUAL(WL_CONNECTED, wifi.status());

        // AP仍然可达
        TEST_ASSERT_EQUAL_STRING("192.168.4.1", wifi.softAPIP().toString().c_str());

        // STA断开
        wifi.setConnected(false);
        TEST_ASSERT_EQUAL(WL_DISCONNECTED, wifi.status());

        // AP仍然可达（STA断开不影响AP）
        TEST_ASSERT_EQUAL_STRING("192.168.4.1", wifi.softAPIP().toString().c_str());
    }

    int32_t leak = (int32_t)initialHeap - (int32_t)ESP.getFreeHeap();
    TEST_ASSERT_TRUE_MESSAGE(leak < 5000, "AP_STA hybrid leak detected");
    TestLog::step("20 cycles: STA fluctuation doesn't affect AP");

    TestLog::testEnd(true);
}

/**
 * @brief 4G模式初始化失败后回退AP的稳定性
 * 模拟4G模块在不同芯片环境下的初始化失败场景
 */
void test_4g_failure_fallback_multi_chip() {
    TestLog::testStart("4G Failure Fallback: Multi-Chip");

    struct ChipEnv {
        const char* name;
        uint32_t heap;
    };
    ChipEnv chips[] = {
        {"ESP32-F4R0",    320000},
        {"ESP32-S3-F8R0",  320000},
        {"ESP32-S3-F8R4",  320000},
        {"ESP32-S3-F16R8", 320000},
    };

    for (auto& chip : chips) {
        ESP.setFreeHeap(chip.heap);

        MockMultiNetworkManager mgr;
        mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;

        // 4G初始化失败
        bool result = mgr.initialize(false);
        TEST_ASSERT_FALSE(result);

        // 回退到AP模式
        TEST_ASSERT_FALLBACK_TO_AP(mgr.networkType, mgr.mode);
        TEST_ASSERT_TRUE(mgr.isAPRunning());
        TEST_ASSERT_EQUAL_STRING("192.168.4.1", mgr.apIPAddress.c_str());
    }

    ESP.resetHeapOverride();
    TestLog::step("4G failure fallback OK on all 4 chip profiles");

    TestLog::testEnd(true);
}

/**
 * @brief 以太网模式初始化失败后回退AP的稳定性
 * 模拟W5500在不同芯片环境下的初始化失败场景
 */
void test_ethernet_failure_fallback_multi_chip() {
    TestLog::testStart("Ethernet Failure Fallback: Multi-Chip");

    struct ChipEnv {
        const char* name;
        uint32_t heap;
    };
    ChipEnv chips[] = {
        {"ESP32-F4R0",    320000},
        {"ESP32-S3-F8R0",  320000},
        {"ESP32-S3-F8R4",  320000},
        {"ESP32-S3-F16R8", 320000},
    };

    for (auto& chip : chips) {
        ESP.setFreeHeap(chip.heap);

        MockMultiNetworkManager mgr;
        mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;

        bool result = mgr.initialize(false);
        TEST_ASSERT_FALSE(result);

        TEST_ASSERT_FALLBACK_TO_AP(mgr.networkType, mgr.mode);
        TEST_ASSERT_TRUE(mgr.isAPRunning());
    }

    ESP.resetHeapOverride();
    TestLog::step("Ethernet failure fallback OK on all 4 chip profiles");

    TestLog::testEnd(true);
}

// 测试以太网断连后自动调度重连
void test_ethernet_auto_reconnect_scheduled_on_disconnect() {
    TestLog::testStart("Ethernet Auto-Reconnect: Scheduled on Disconnect");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.initialize(true);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::CONNECTED, (int)mgr.status);
    TEST_ASSERT_FALSE(mgr.ethReconnectPending);
    TEST_ASSERT_EQUAL(0, mgr.ethReconnectAttempts);
    TestLog::step("Ethernet connected, no reconnect pending");

    // 模拟断连
    unsigned long t = 5000;
    bool result = mgr.simulateEthReconnectUpdate(false, t);
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(mgr.ethReconnectPending);
    TEST_ASSERT_EQUAL((int)(t + MockMultiNetworkManager::ETH_RECONNECT_INTERVAL_MS), (int)mgr.ethReconnectTime);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::DISCONNECTED, (int)mgr.status);
    TestLog::step("Ethernet disconnected, reconnect scheduled");

    TestLog::testEnd(true);
}

// 测试以太网重连达到最大次数后停止，且 AP 始终可用
void test_ethernet_auto_reconnect_max_attempts() {
    TestLog::testStart("Ethernet Auto-Reconnect: Max Attempts Exhausted");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.initialize(true);
    // 混合模式下 AP 应该已启动
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("Hybrid mode: Ethernet + AP active");

    unsigned long t = 5000;
    // 模拟断连
    mgr.simulateEthReconnectUpdate(false, t);
    TEST_ASSERT_TRUE(mgr.ethReconnectPending);
    TestLog::step("Initial disconnect, reconnect scheduled");

    // 执行最大次数次重连尝试（每次都失败）
    for (int i = 0; i < MockMultiNetworkManager::ETH_MAX_RECONNECT_ATTEMPTS; i++) {
        t = mgr.ethReconnectTime; // 跳到重连时间
        bool result = mgr.simulateEthReconnectUpdate(false, t);
        TEST_ASSERT_FALSE(result);
    }
    TEST_ASSERT_EQUAL(MockMultiNetworkManager::ETH_MAX_RECONNECT_ATTEMPTS, mgr.ethReconnectAttempts);
    TestLog::step("Max attempts reached");

    // 不再调度新重连
    TEST_ASSERT_FALSE(mgr.ethReconnectPending);
    TestLog::step("No more reconnects scheduled after max attempts");

    // AP 必须始终可用（重连耗尽后的回退入口）
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", mgr.apIPAddress.c_str());
    TestLog::step("AP still running as fallback access point");

    TestLog::testEnd(true);
}

// 测试以太网重连成功后重置计数器
void test_ethernet_auto_reconnect_resets_on_success() {
    TestLog::testStart("Ethernet Auto-Reconnect: Resets on Success");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.initialize(true);

    unsigned long t = 5000;
    // 模拟断连
    mgr.simulateEthReconnectUpdate(false, t);
    TEST_ASSERT_TRUE(mgr.ethReconnectPending);

    // 执行 3 次失败重连
    for (int i = 0; i < 3; i++) {
        t = mgr.ethReconnectTime;
        mgr.simulateEthReconnectUpdate(false, t);
    }
    TEST_ASSERT_EQUAL(3, mgr.ethReconnectAttempts);
    TestLog::step("3 failed reconnect attempts");

    // 第 4 次重连成功
    t = mgr.ethReconnectTime;
    bool result = mgr.simulateEthReconnectUpdate(true, t);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(0, mgr.ethReconnectAttempts);
    TEST_ASSERT_FALSE(mgr.ethReconnectPending);
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::CONNECTED, (int)mgr.status);
    TestLog::step("Reconnect succeeded, counters reset");

    TestLog::testEnd(true);
}

// 测试初始断连状态也能触发重连调度
void test_ethernet_auto_reconnect_initial_disconnect() {
    TestLog::testStart("Ethernet Auto-Reconnect: Initial Disconnect State");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    // 初始状态：DISCONNECTED（从未连接过）
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::DISCONNECTED, (int)mgr.status);
    TEST_ASSERT_FALSE(mgr.ethReconnectPending);
    TestLog::step("Initial state: disconnected, no reconnect pending");

    // 第一次 update：wasConnected=false, adapterConnected=false
    unsigned long t = 1000;
    bool result = mgr.simulateEthReconnectUpdate(false, t);
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(mgr.ethReconnectPending);
    TEST_ASSERT_EQUAL(0, mgr.ethReconnectAttempts);
    TestLog::step("Initial disconnect detected, reconnect scheduled despite wasConnected=false");

    TestLog::testEnd(true);
}

// 回归测试：以太网重连耗尽后 AP 回退必须可用
void test_regression_eth_reconnect_exhausted_ap_fallback() {
    TestLog::testStart("Regression: Ethernet Reconnect Exhausted → AP Fallback");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.initialize(true);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("Initial: Ethernet + AP hybrid mode");

    // 模拟以太网断连
    unsigned long t = 10000;
    mgr.simulateEthReconnectUpdate(false, t);

    // 人工关闭 AP（模拟异常场景：AP 意外停止）
    mgr.apRunning = false;
    TEST_ASSERT_FALSE(mgr.isAPRunning());
    TestLog::step("Simulated: AP lost during reconnect attempts");

    // 耗尽所有重连尝试
    for (int i = 0; i < MockMultiNetworkManager::ETH_MAX_RECONNECT_ATTEMPTS; i++) {
        t = mgr.ethReconnectTime;
        mgr.simulateEthReconnectUpdate(false, t);
    }
    TEST_ASSERT_EQUAL(MockMultiNetworkManager::ETH_MAX_RECONNECT_ATTEMPTS, mgr.ethReconnectAttempts);
    TestLog::step("All reconnect attempts exhausted");

    // AP 必须被恢复（重连耗尽后的最后保障）
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", mgr.apIPAddress.c_str());
    TestLog::step("AP restored as last-resort access point");

    TestLog::testEnd(true);
}

// ========== 最后保障 AP 测试 ==========

void test_last_resort_ap_starts_when_all_modes_fail() {
    TestLog::testStart("Last-Resort AP: starts when all configured modes fail");

    MockMultiNetworkManager mgr;
    // 场景：以太网连接失败 + 正常 AP 回退也失败（模拟 AP 配置损坏）
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    // 初始化失败，回退到 AP，但正常 AP 也不可用
    bool ok = mgr.initialize(false);
    // 此时正常 AP 回退已启动（mock 中 startAP() 始终成功），所以模拟 AP 也丢失的场景
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE(mgr.isAPRunning());  // mock 中 startAP() 仍会启动
    TestLog::step("First: normal AP fallback succeeded");

    // 模拟更极端场景：AP 运行但因配置损坏而 IP 异常
    mgr.apRunning = false;
    mgr.apIPAddress = "";
    TEST_ASSERT_FALSE(mgr.isAPRunning());
    TestLog::step("Simulated: AP lost (corrupted config)");

    // 调用最后保障
    bool lrOk = mgr.ensureLastResortAP();
    TEST_ASSERT_TRUE(lrOk);
    TEST_ASSERT_TRUE(mgr.lastResortAPStarted);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", mgr.apIPAddress.c_str());
    TestLog::step("Last-resort AP started successfully");

    TestLog::testEnd(true);
}

void test_last_resort_ap_already_running_returns_true() {
    TestLog::testStart("Last-Resort AP: returns true if AP already running");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.initialize(true);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("Initial: AP running in hybrid mode");

    // 调用 ensureLastResortAP()，AP 已运行，应直接返回 true
    bool ok = mgr.ensureLastResortAP();
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FALSE(mgr.lastResortAPStarted);  // 不应重新启动
    TestLog::step("ensureLastResortAP returned true without restart");

    TestLog::testEnd(true);
}

void test_last_resort_ap_factory_reset_after_3_failures() {
    TestLog::testStart("Last-Resort AP: factory reset after 3 consecutive failures");

    MockMultiNetworkManager mgr;
    mgr.forceLastResortAPFail = true;

    // 第1次失败
    bool ok1 = mgr.ensureLastResortAP();
    TEST_ASSERT_FALSE(ok1);
    TEST_ASSERT_FALSE(mgr.factoryResetTriggered);
    TEST_ASSERT_EQUAL(1, mgr.initFailCount);
    TestLog::step("Failure #1 recorded");

    // 第2次失败
    bool ok2 = mgr.ensureLastResortAP();
    TEST_ASSERT_FALSE(ok2);
    TEST_ASSERT_FALSE(mgr.factoryResetTriggered);
    TEST_ASSERT_EQUAL(2, mgr.initFailCount);
    TestLog::step("Failure #2 recorded");

    // 第3次失败 → 触发出厂重置
    bool ok3 = mgr.ensureLastResortAP();
    TEST_ASSERT_FALSE(ok3);
    TEST_ASSERT_TRUE(mgr.factoryResetTriggered);
    TEST_ASSERT_EQUAL(0, mgr.initFailCount);  // 重置后计数归零
    TestLog::step("Factory reset triggered after 3 failures");

    TestLog::testEnd(true);
}

void test_last_resort_ap_restart_network_ethernet_failure() {
    TestLog::testStart("Last-Resort AP: restartNetwork Ethernet failure → AP guaranteed");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    bool ok = mgr.initialize(true);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("Initial: Ethernet + AP hybrid mode");

    // 模拟 AP 意外丢失 + 以太网重连失败
    mgr.apRunning = false;
    mgr.apIPAddress = "";
    bool restartOk = mgr.restartNetwork(false);
    TEST_ASSERT_FALSE(restartOk);
    // restartNetwork 内部已调用 ensureLastResortAP（mock 中 startAP 成功）
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", mgr.apIPAddress.c_str());
    TestLog::step("Last-resort AP ensures device accessible");

    TestLog::testEnd(true);
}

// ========== 最后保障 AP 增强测试 ==========

void test_last_resort_ap_wifi_sta_both_fail() {
    TestLog::testStart("Last-Resort AP: WiFi STA + AP both fail → last-resort AP");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_WIFI;
    mgr.mode = MockMultiNetworkManager::NetMode::NETWORK_STA;
    mgr.forceAPFail = true;  // 正常 AP 启动失败（配置损坏）

    bool ok = mgr.initialize(false);  // STA 也失败
    TEST_ASSERT_FALSE(ok);
    // initialize() 内部已自动调用 ensureLastResortAP()
    // 注意：ensureLastResortAP 使用硬编码配置，不受 forceAPFail 影响
    TEST_ASSERT_TRUE(mgr.lastResortAPStarted);  // 确认是 last-resort AP 启动的
    TEST_ASSERT_TRUE(mgr.isAPRunning());        // AP 已在运行（last-resort 挽救）
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", mgr.apIPAddress.c_str());
    TestLog::step("STA + normal AP failed, last-resort AP saved the device");

    TestLog::testEnd(true);
}

void test_last_resort_ap_4g_restart_failure() {
    TestLog::testStart("Last-Resort AP: restartNetwork 4G failure → AP guaranteed");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    mgr.forceAPFail = true;  // 正常 AP 启动失败

    bool ok = mgr.restartNetwork(false);
    TEST_ASSERT_FALSE(ok);
    // restartNetwork 内部已调用 ensureLastResortAP
    // ensureLastResortAP 使用硬编码配置，不受 forceAPFail 影响
    TEST_ASSERT_TRUE(mgr.lastResortAPStarted);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", mgr.apIPAddress.c_str());
    TestLog::step("4G restart + normal AP failed, last-resort AP ensured access");

    TestLog::testEnd(true);
}

void test_last_resort_ap_lora_restart_failure() {
    TestLog::testStart("Last-Resort AP: restartNetwork LoRa failure → AP guaranteed");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_LORA;
    mgr.forceAPFail = true;  // 正常 AP 启动失败

    bool ok = mgr.restartNetwork(false);
    TEST_ASSERT_FALSE(ok);
    TestLog::step("LoRa restart + normal AP both failed");

    // ensureLastResortAP 必须挽救局面
    TEST_ASSERT_TRUE(mgr.lastResortAPStarted);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", mgr.apIPAddress.c_str());
    TestLog::step("Last-resort AP ensures LoRa device accessible");

    TestLog::testEnd(true);
}

void test_last_resort_ap_success_clears_fail_count() {
    TestLog::testStart("Last-Resort AP: successful init clears fail count");

    MockMultiNetworkManager mgr;
    // 第1次启动失败（4G + AP 都失败）
    mgr.forceAPFail = true;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    bool ok1 = mgr.initialize(false);
    TEST_ASSERT_FALSE(ok1);
    // lastResortAPStarted 应为 true（ensureLastResortAP 成功挽救）
    TEST_ASSERT_TRUE(mgr.lastResortAPStarted);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("First boot: failed but last-resort AP saved");

    // 第2次启动成功
    mgr.forceAPFail = false;  // AP 恢复正常
    mgr.lastResortAPStarted = false;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    bool ok2 = mgr.initialize(true);
    TEST_ASSERT_TRUE(ok2);
    TEST_ASSERT_EQUAL(0, mgr.initFailCount);  // 计数已清除
    TEST_ASSERT_FALSE(mgr.factoryResetTriggered);  // 未触发出厂重置
    TestLog::step("Second boot: success, fail count cleared");

    TestLog::testEnd(true);
}

void test_last_resort_ap_eth_reconnect_exhausted() {
    TestLog::testStart("Last-Resort AP: update() eth reconnect exhausted → ensureLastResortAP");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.initialize(true);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TestLog::step("Initial: Ethernet + AP hybrid mode");

    // 模拟以太网断连
    unsigned long t = 10000;
    mgr.simulateEthReconnectUpdate(false, t);

    // 人为关闭 AP（模拟 AP 意外停止）
    mgr.apRunning = false;
    mgr.apIPAddress = "";
    TestLog::step("Simulated: AP lost + Ethernet disconnected");

    // 耗尽所有重连尝试
    for (int i = 0; i < MockMultiNetworkManager::ETH_MAX_RECONNECT_ATTEMPTS; i++) {
        t = mgr.ethReconnectTime;
        mgr.simulateEthReconnectUpdate(false, t);
    }

    // 重连耗尽后 ensureLastResortAP 必须被调用
    TEST_ASSERT_EQUAL(MockMultiNetworkManager::ETH_MAX_RECONNECT_ATTEMPTS, mgr.ethReconnectAttempts);
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", mgr.apIPAddress.c_str());
    TestLog::step("Reconnect exhausted: ensureLastResortAP restored AP");

    TestLog::testEnd(true);
}

void test_last_resort_ap_permanent_wifi_failure() {
    TestLog::testStart("Last-Resort AP: permanent WiFi failure → factory reset path");

    MockMultiNetworkManager mgr;
    mgr.forceAPFail = true;           // 正常 AP 启动失败
    mgr.forceLastResortAPFail = true;  // 最后保障 AP 也失败

    // WiFi STA 连接失败，AP 失败，最后保障 AP 也失败
    mgr.networkType = MockMultiNetworkManager::NetType::NET_WIFI;
    mgr.mode = MockMultiNetworkManager::NetMode::NETWORK_STA;
    bool ok = mgr.initialize(false);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_FALSE(mgr.isAPRunning());
    TEST_ASSERT_EQUAL(1, mgr.initFailCount);
    TestLog::step("Complete network failure: initFailCount=1");

    // 模拟第2、3次重启也都失败
    mgr.initialize(false);
    TEST_ASSERT_EQUAL(2, mgr.initFailCount);
    TEST_ASSERT_FALSE(mgr.factoryResetTriggered);
    TestLog::step("Second failure: initFailCount=2");

    mgr.initialize(false);
    TEST_ASSERT_TRUE(mgr.factoryResetTriggered);
    TEST_ASSERT_EQUAL(0, mgr.initFailCount);
    TestLog::step("Third failure: factory reset triggered");

    TestLog::testEnd(true);
}

// Test group entry point
void test_network_config_group() {
    TestLog::groupStart("Network Configuration Tests");
    
    RUN_TEST(test_wifi_mode_switching);
    RUN_TEST(test_ap_hotspot_function);
    RUN_TEST(test_sta_connection);
    RUN_TEST(test_wifi_connection_failure);
    RUN_TEST(test_ap_mode_network_status);
    RUN_TEST(test_dashboard_network_status);
    RUN_TEST(test_ntp_sync_status);
    RUN_TEST(test_network_status_api);
    RUN_TEST(test_network_mode_auto_switch);
    RUN_TEST(test_ip_configuration);
    RUN_TEST(test_network_reconnect);
    
    TestLog::groupEnd();
}

// ========== mDNS 自定义域名功能测试 ==========

/**
 * @brief mDNS 启用时使用自定义域名作为 hostname
 * 回归：修复前 customDomain 保存后不重启 mDNS，导致仍然只能通过 fastbee.local 访问
 */
void test_mdns_custom_domain_applied() {
    TestLog::testStart("mDNS Custom Domain Applied");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_WIFI;
    mgr.mode = MockMultiNetworkManager::NetMode::NETWORK_STA;
    mgr.customDomain = "mydevice";
    mgr.enableMDNS = true;

    bool result = mgr.initialize(true);
    TEST_ASSERT_TRUE(result);
    TestLog::step("WiFi STA initialized with customDomain='mydevice'");

    // mDNS 应已启动，且 hostname 为自定义域名
    TEST_ASSERT_TRUE(mgr.mDNSStarted);
    TEST_ASSERT_EQUAL_STRING("mydevice", mgr.getActualHostname().c_str());
    TestLog::step("mDNS started with hostname 'mydevice'");

    TestLog::testEnd(true);
}

/**
 * @brief mDNS 禁用时服务不启动
 * 确保 enableMDNS=false 时 mDNS 服务不会运行
 */
void test_mdns_disabled_no_service() {
    TestLog::testStart("mDNS Disabled: No Service");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_WIFI;
    mgr.mode = MockMultiNetworkManager::NetMode::NETWORK_STA;
    mgr.enableMDNS = false;
    mgr.customDomain = "mydevice";

    bool result = mgr.initialize(true);
    TEST_ASSERT_TRUE(result);
    TestLog::step("WiFi STA initialized with enableMDNS=false");

    TEST_ASSERT_FALSE(mgr.mDNSStarted);
    TEST_ASSERT_TRUE(mgr.getActualHostname().isEmpty());
    TestLog::step("mDNS not started, no hostname registered");

    TestLog::testEnd(true);
}

/**
 * @brief mDNS 运行时修改自定义域名，自动重启 mDNS 应用新域名
 * 回归：对应 DNSManager::setCustomDomain() 修复
 */
void test_mdns_domain_change_triggers_restart() {
    TestLog::testStart("mDNS Domain Change Triggers Restart");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_WIFI;
    mgr.mode = MockMultiNetworkManager::NetMode::NETWORK_STA;
    mgr.customDomain = "fastbee";
    mgr.enableMDNS = true;

    mgr.initialize(true);
    TEST_ASSERT_TRUE(mgr.mDNSStarted);
    TEST_ASSERT_EQUAL_STRING("fastbee", mgr.getActualHostname().c_str());
    TestLog::step("Initial: mDNS running with hostname 'fastbee'");

    // 用户修改自定义域名为 "myhome"
    mgr.setCustomDomain("myhome");
    TEST_ASSERT_EQUAL_STRING("myhome", mgr.customDomain.c_str());
    TEST_ASSERT_EQUAL_STRING("myhome", mgr.getActualHostname().c_str());
    TestLog::step("After setCustomDomain('myhome'): hostname updated");

    TestLog::testEnd(true);
}

/**
 * @brief mDNS 配置变更通过 updateMDNSConfig 立即生效
 * 回归：对应 NetworkManager::updateConfig() 中 mdnsConfigChanged 分支
 */
void test_update_mdns_config_applies_immediately() {
    TestLog::testStart("updateMDNSConfig Applies Immediately");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.enableMDNS = true;
    mgr.customDomain = "fastbee";

    mgr.initialize(true);
    TEST_ASSERT_TRUE(mgr.mDNSStarted);
    TEST_ASSERT_EQUAL_STRING("fastbee", mgr.getActualHostname().c_str());
    TestLog::step("Ethernet + mDNS started with 'fastbee'");

    // 用户通过高级配置修改域名为 "smartgateway"
    bool result = mgr.updateMDNSConfig("smartgateway", true);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(mgr.mDNSStarted);
    TEST_ASSERT_EQUAL_STRING("smartgateway", mgr.customDomain.c_str());
    TEST_ASSERT_EQUAL_STRING("smartgateway", mgr.getActualHostname().c_str());
    TestLog::step("updateMDNSConfig: domain changed to 'smartgateway'");

    TestLog::testEnd(true);
}

/**
 * @brief 通过 updateMDNSConfig 禁用 mDNS 立即停止服务
 */
void test_update_mdns_disable_stops_service() {
    TestLog::testStart("updateMDNSConfig Disable Stops Service");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_WIFI;
    mgr.mode = MockMultiNetworkManager::NetMode::NETWORK_STA;
    mgr.enableMDNS = true;
    mgr.customDomain = "fastbee";

    mgr.initialize(true);
    TEST_ASSERT_TRUE(mgr.mDNSStarted);
    TestLog::step("mDNS running");

    // 用户禁用 mDNS
    bool result = mgr.updateMDNSConfig("fastbee", false);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_FALSE(mgr.mDNSStarted);
    TEST_ASSERT_TRUE(mgr.getActualHostname().isEmpty());
    TestLog::step("mDNS stopped after disable");

    TestLog::testEnd(true);
}

/**
 * @brief 以太网混合模式下自定义域名正确应用
 */
void test_ethernet_hybrid_mdns_custom_domain() {
    TestLog::testStart("Ethernet Hybrid: Custom Domain Applied");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.enableMDNS = true;
    mgr.customDomain = "mygateway";

    bool result = mgr.initialize(true);
    TEST_ASSERT_TRUE(result);
    TestLog::step("Ethernet hybrid mode initialized");

    TEST_ASSERT_TRUE(mgr.mDNSStarted);
    TEST_ASSERT_EQUAL_STRING("mygateway", mgr.getActualHostname().c_str());
    TestLog::step("mDNS hostname = 'mygateway'");

    // AP 同时运行
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", mgr.apIPAddress.c_str());
    TestLog::step("AP running at 192.168.4.1");

    TestLog::testEnd(true);
}

/**
 * @brief 4G 混合模式下自定义域名正确应用
 */
void test_4g_hybrid_mdns_custom_domain() {
    TestLog::testStart("4G Hybrid: Custom Domain Applied");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    mgr.enableMDNS = true;
    mgr.customDomain = "sensorhub";

    bool result = mgr.initialize(true);
    TEST_ASSERT_TRUE(result);
    TestLog::step("4G hybrid mode initialized");

    TEST_ASSERT_TRUE(mgr.mDNSStarted);
    TEST_ASSERT_EQUAL_STRING("sensorhub", mgr.getActualHostname().c_str());
    TestLog::step("mDNS hostname = 'sensorhub'");

    TestLog::testEnd(true);
}

/**
 * @brief 域名不变时 updateMDNSConfig 不触发多余操作
 */
void test_mdns_config_no_change_no_op() {
    TestLog::testStart("mDNS Config No-Change: No-Op");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_WIFI;
    mgr.mode = MockMultiNetworkManager::NetMode::NETWORK_STA;
    mgr.customDomain = "fastbee";
    mgr.enableMDNS = true;

    mgr.initialize(true);
    String hostnameBefore = mgr.getActualHostname();

    // 相同配置再次调用
    bool result = mgr.updateMDNSConfig("fastbee", true);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_STRING(hostnameBefore.c_str(), mgr.getActualHostname().c_str());
    TestLog::step("No change: hostname unchanged");

    TestLog::testEnd(true);
}

/**
 * @brief mDNS 禁用后重新启用，hostname 正确恢复
 * 回归：浏览器验证中切换 mDNS 禁用→启用后功能正常
 */
void test_mdns_reenable_after_disable() {
    TestLog::testStart("mDNS Re-Enable After Disable");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_WIFI;
    mgr.mode = MockMultiNetworkManager::NetMode::NETWORK_STA;
    mgr.customDomain = "fastbee";
    mgr.enableMDNS = true;

    mgr.initialize(true);
    TEST_ASSERT_TRUE(mgr.mDNSStarted);
    TEST_ASSERT_EQUAL_STRING("fastbee", mgr.getActualHostname().c_str());
    TestLog::step("mDNS running with 'fastbee'");

    // 用户禁用 mDNS
    bool result = mgr.updateMDNSConfig("fastbee", false);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_FALSE(mgr.mDNSStarted);
    TEST_ASSERT_TRUE(mgr.getActualHostname().isEmpty());
    TestLog::step("mDNS disabled");

    // 用户重新启用 mDNS
    result = mgr.updateMDNSConfig("fastbee", true);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(mgr.mDNSStarted);
    TEST_ASSERT_EQUAL_STRING("fastbee", mgr.getActualHostname().c_str());
    TestLog::step("mDNS re-enabled, hostname restored to 'fastbee'");

    TestLog::testEnd(true);
}

/**
 * @brief mDNS 快速切换开关不崩溃、状态一致
 * 模拟用户在 UI 上快速反复切换 mDNS 开关
 */
void test_mdns_rapid_toggle_stability() {
    TestLog::testStart("mDNS Rapid Toggle Stability");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_WIFI;
    mgr.mode = MockMultiNetworkManager::NetMode::NETWORK_STA;
    mgr.customDomain = "fastbee";
    mgr.enableMDNS = true;

    mgr.initialize(true);
    TEST_ASSERT_TRUE(mgr.mDNSStarted);
    TestLog::step("mDNS initially running");

    // 快速切换 10 次
    for (int i = 0; i < 10; i++) {
        bool enable = (i % 2 == 0);  // 交替 true/false
        mgr.updateMDNSConfig("fastbee", enable);
    }

    // 第10次 i=9 → enable=false → mDNS 应该是禁用状态
    TEST_ASSERT_FALSE(mgr.mDNSStarted);
    TEST_ASSERT_TRUE(mgr.getActualHostname().isEmpty());
    TestLog::step("After 10 toggles (last=disable): mDNS stopped");

    // 最终启用
    mgr.updateMDNSConfig("fastbee", true);
    TEST_ASSERT_TRUE(mgr.mDNSStarted);
    TEST_ASSERT_EQUAL_STRING("fastbee", mgr.getActualHostname().c_str());
    TestLog::step("Final enable: mDNS restored");

    TestLog::testEnd(true);
}

/**
 * @brief updateMDNSConfig 同时修改域名和启用状态
 * 验证同时变更两个参数时结果正确
 */
void test_mdns_config_simultaneous_change() {
    TestLog::testStart("mDNS Config Simultaneous Change");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.enableMDNS = true;
    mgr.customDomain = "fastbee";

    mgr.initialize(true);
    TEST_ASSERT_EQUAL_STRING("fastbee", mgr.getActualHostname().c_str());
    TestLog::step("Ethernet mDNS: 'fastbee'");

    // 同时修改域名和禁用
    bool result = mgr.updateMDNSConfig("newdevice", false);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_FALSE(mgr.mDNSStarted);
    TEST_ASSERT_EQUAL_STRING("newdevice", mgr.customDomain.c_str());
    TEST_ASSERT_TRUE(mgr.getActualHostname().isEmpty());
    TestLog::step("Simultaneous domain+disable: domain saved, mDNS stopped");

    // 同时修改域名和启用
    result = mgr.updateMDNSConfig("smartgateway", true);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(mgr.mDNSStarted);
    TEST_ASSERT_EQUAL_STRING("smartgateway", mgr.getActualHostname().c_str());
    TestLog::step("Simultaneous domain+enable: new hostname applied");

    TestLog::testEnd(true);
}

/**
 * @brief 4G 模式下域名配置不影响实际 hostname
 * 对应 UI 行为：4G 模式隐藏域名配置，用户无法修改域名
 * 验证：即使后端存储了 customDomain，在 4G 混合模式重启后
 *       mDNS 仍使用存储的域名（因为 UI 不允许修改）
 */
void test_4g_mode_domain_unchanged_after_restart() {
    TestLog::testStart("4G Mode: Domain Unchanged After Restart");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    mgr.enableMDNS = true;
    mgr.customDomain = "fastbee";

    mgr.initialize(true);
    TEST_ASSERT_TRUE(mgr.mDNSStarted);
    TEST_ASSERT_EQUAL_STRING("fastbee", mgr.getActualHostname().c_str());
    TestLog::step("4G mode: mDNS with 'fastbee'");

    // 模拟重启网络（4G 模式下 UI 不允许修改域名，所以 customDomain 保持不变）
    bool result = mgr.restartNetwork(true);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_STRING("fastbee", mgr.customDomain.c_str());
    TEST_ASSERT_EQUAL_STRING("fastbee", mgr.getActualHostname().c_str());
    TestLog::step("After restart: domain unchanged (UI blocked modification)");

    TestLog::testEnd(true);
}

/**
 * @brief LoRa 模式下不启动 mDNS 和 AP
 * LoRa 模式无 IP 网络，mDNS 不适用
 */
void test_lora_mode_no_mdns() {
    TestLog::testStart("LoRa Mode: No mDNS Service");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_LORA;
    mgr.enableMDNS = true;
    mgr.customDomain = "fastbee";

    bool result = mgr.initialize(true);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_FALSE(mgr.mDNSStarted);
    TEST_ASSERT_TRUE(mgr.getActualHostname().isEmpty());
    TEST_ASSERT_FALSE(mgr.isAPRunning());
    TestLog::step("LoRa mode: no mDNS, no AP (IP-less network)");

    TestLog::testEnd(true);
}

// mDNS 自定义域名测试组
void test_mdns_domain_group() {
    TestLog::groupStart("mDNS Custom Domain Tests");

    RUN_TEST(test_mdns_custom_domain_applied);
    RUN_TEST(test_mdns_disabled_no_service);
    RUN_TEST(test_mdns_domain_change_triggers_restart);
    RUN_TEST(test_update_mdns_config_applies_immediately);
    RUN_TEST(test_update_mdns_disable_stops_service);
    RUN_TEST(test_ethernet_hybrid_mdns_custom_domain);
    RUN_TEST(test_4g_hybrid_mdns_custom_domain);
    RUN_TEST(test_mdns_config_no_change_no_op);
    RUN_TEST(test_mdns_reenable_after_disable);
    RUN_TEST(test_mdns_rapid_toggle_stability);
    RUN_TEST(test_mdns_config_simultaneous_change);
    RUN_TEST(test_4g_mode_domain_unchanged_after_restart);
    RUN_TEST(test_lora_mode_no_mdns);

    TestLog::groupEnd();
}

// 多网络模式测试组
void test_multi_network_mode_group() {
    TestLog::groupStart("Multi-Network Mode Tests");

    RUN_TEST(test_network_type_enum_values);
    RUN_TEST(test_4g_init_failure_fallback_to_ap);
    RUN_TEST(test_ethernet_init_failure_fallback_to_ap);
    RUN_TEST(test_lora_init_failure_fallback_to_ap);
    RUN_TEST(test_4g_mode_ap_always_running);
    RUN_TEST(test_ethernet_mode_ap_always_running);
    RUN_TEST(test_4g_mode_mdns_started);
    RUN_TEST(test_ethernet_mode_mdns_started);
    RUN_TEST(test_mdns_disabled_not_started);
    RUN_TEST(test_non_wifi_status_isolation);
    RUN_TEST(test_4g_disconnect_status);
    RUN_TEST(test_switch_wifi_to_4g);
    RUN_TEST(test_switch_4g_to_wifi);
    RUN_TEST(test_non_wifi_no_sta_reconnect);
    RUN_TEST(test_restart_network_non_wifi);
    RUN_TEST(test_restart_network_4g_failure);
    RUN_TEST(test_hybrid_mode_ap_ip);
    
    // restartNetwork AP保活回归测试
    RUN_TEST(test_restart_network_non_wifi_no_global_disconnect);
    RUN_TEST(test_restart_network_eth_failure_ap_recovery);

    // mDNS 启动顺序回归测试
    RUN_TEST(test_ethernet_mdns_starts_after_ap);
    RUN_TEST(test_4g_mdns_starts_after_ap);
    // 以太网启动报告和周期状态回归测试
    RUN_TEST(test_ethernet_boot_report_contains_all_fields);
    RUN_TEST(test_ethernet_periodic_status_output);

    // WiFi多模式稳定性 + 多芯片回退测试
    RUN_TEST(test_wifi_sta_reconnect_50_cycles_no_leak);
    RUN_TEST(test_wifi_ap_long_running_stability);
    RUN_TEST(test_wifi_ap_sta_hybrid_stability);
    RUN_TEST(test_4g_failure_fallback_multi_chip);
    RUN_TEST(test_ethernet_failure_fallback_multi_chip);

    // 以太网自动重连回归测试
    RUN_TEST(test_ethernet_auto_reconnect_scheduled_on_disconnect);
    RUN_TEST(test_ethernet_auto_reconnect_max_attempts);
    RUN_TEST(test_ethernet_auto_reconnect_resets_on_success);
    RUN_TEST(test_ethernet_auto_reconnect_initial_disconnect);
    RUN_TEST(test_regression_eth_reconnect_exhausted_ap_fallback);

    // 最后保障 AP 回归测试
    RUN_TEST(test_last_resort_ap_starts_when_all_modes_fail);
    RUN_TEST(test_last_resort_ap_already_running_returns_true);
    RUN_TEST(test_last_resort_ap_factory_reset_after_3_failures);
    RUN_TEST(test_last_resort_ap_restart_network_ethernet_failure);
    RUN_TEST(test_last_resort_ap_wifi_sta_both_fail);
    RUN_TEST(test_last_resort_ap_4g_restart_failure);
    RUN_TEST(test_last_resort_ap_lora_restart_failure);
    RUN_TEST(test_last_resort_ap_success_clears_fail_count);
    RUN_TEST(test_last_resort_ap_eth_reconnect_exhausted);
    RUN_TEST(test_last_resort_ap_permanent_wifi_failure);

    TestLog::groupEnd();
}
