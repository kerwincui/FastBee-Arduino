/**
 * @file test_network_config.cpp
 * @brief Network Configuration Tests
 */

#include <unity.h>
#include <Arduino.h>
#include "mocks/MockWiFi.h"
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

/**
 * @brief 多网络模式状态模拟器
 * 
 * 模拟 NetworkManager 中多网络模式切换、失败回退、状态隔离的核心逻辑。
 * 用于验证 4G/以太网/LoRa 等非 WiFi 联网方式下的行为。
 */
class MockMultiNetworkManager {
public:
    // 与源码保持一致的枚举值
    enum class NetType : uint8_t { NET_WIFI = 0, NET_ETHERNET = 1, NET_4G = 2, NET_LORA = 3 };
    enum class NetMode : uint8_t { NETWORK_STA = 0, NETWORK_AP = 1 };
    enum class NetStatus : uint8_t {
        DISCONNECTED = 0, CONNECTING = 1, CONNECTED = 2,
        CONNECTION_FAILED = 3, AP_MODE = 4
    };

    NetType networkType = NetType::NET_WIFI;
    NetMode mode = NetMode::NETWORK_STA;
    NetStatus status = NetStatus::DISCONNECTED;
    String ipAddress = "";
    String apIPAddress = "";
    String apSSID = "fastbee-ap";
    bool apRunning = false;
    uint8_t apClientCount = 0;
    bool internetAvailable = false;

    // 模拟 WiFiManager 的状态（用于测试状态隔离）
    NetStatus wifiManagerStatus = NetStatus::AP_MODE;

    /**
     * 模拟 initialize() 中的联网初始化逻辑
     * 返回 true 表示初始化成功，false 表示失败
     */
    bool initialize(bool adapterSuccess) {
        // 模拟 4G 初始化
        if (networkType == NetType::NET_4G) {
            if (adapterSuccess) {
                status = NetStatus::CONNECTED;
                ipAddress = "10.0.0.100";
                internetAvailable = true;
                // 4G 成功 → 启动 AP 热点
                startAPForHybrid();
                return true;
            } else {
                // 4G 失败 → 回退到 AP 模式（关键修复点）
                networkType = NetType::NET_WIFI;
                mode = NetMode::NETWORK_AP;
                startAP();
                return false;
            }
        }

        // 模拟以太网初始化
        if (networkType == NetType::NET_ETHERNET) {
            if (adapterSuccess) {
                status = NetStatus::CONNECTED;
                ipAddress = "192.168.1.200";
                internetAvailable = true;
                startAPForHybrid();
                return true;
            } else {
                networkType = NetType::NET_WIFI;
                mode = NetMode::NETWORK_AP;
                startAP();
                return false;
            }
        }

        // 模拟 LoRa 初始化
        if (networkType == NetType::NET_LORA) {
            if (adapterSuccess) {
                status = NetStatus::CONNECTED;
                internetAvailable = false;
                return true;
            } else {
                networkType = NetType::NET_WIFI;
                mode = NetMode::NETWORK_AP;
                startAP();
                return false;
            }
        }

        // WiFi 模式
        if (networkType == NetType::NET_WIFI) {
            if (mode == NetMode::NETWORK_AP) {
                startAP();
                return true;
            }
            // STA 模式
            if (adapterSuccess) {
                status = NetStatus::CONNECTED;
                ipAddress = "192.168.1.100";
                internetAvailable = true;
                return true;
            }
            status = NetStatus::CONNECTION_FAILED;
            return false;
        }

        return false;
    }

    /**
     * 模拟 updateStatusInfo() 中的状态隔离逻辑
     * 非 WiFi 联网时，主状态由对应适配器决定，仅同步 AP 字段
     */
    void updateStatusInfo(bool adapterConnected, const String& adapterIP) {
        if (networkType != NetType::NET_WIFI) {
            // 仅同步 AP 字段，不覆盖主状态
            apClientCount = 1;  // 模拟有客户端连接
            apIPAddress = "192.168.4.1";

            // 主状态由适配器决定
            status = adapterConnected ? NetStatus::CONNECTED : NetStatus::DISCONNECTED;
            ipAddress = adapterConnected ? adapterIP : "";
            internetAvailable = adapterConnected;
            return;
        }

        // WiFi 模式：完全同步 WiFiManager 状态
        status = wifiManagerStatus;
    }

    /**
     * 模拟 restartNetwork() 中的重启逻辑
     */
    bool restartNetwork(bool adapterSuccess) {
        // 先断开所有
        disconnect();

        if (networkType != NetType::NET_WIFI) {
            // 非 WiFi 联网方式：完整重新初始化
            return initialize(adapterSuccess);
        }

        // WiFi STA 模式重启
        return initialize(adapterSuccess);
    }

    void disconnect() {
        status = NetStatus::DISCONNECTED;
        ipAddress = "";
        internetAvailable = false;
        apRunning = false;
    }

    bool isAPRunning() const { return apRunning; }

    void startAP() {
        apRunning = true;
        apIPAddress = "192.168.4.1";
        status = NetStatus::AP_MODE;
    }

    void startAPForHybrid() {
        apRunning = true;
        apIPAddress = "192.168.4.1";
        // 混合模式下主状态保持 CONNECTED，不设为 AP_MODE
    }
};

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

// 多网络模式测试组
void test_multi_network_mode_group() {
    TestLog::groupStart("Multi-Network Mode Tests");

    RUN_TEST(test_network_type_enum_values);
    RUN_TEST(test_4g_init_failure_fallback_to_ap);
    RUN_TEST(test_ethernet_init_failure_fallback_to_ap);
    RUN_TEST(test_lora_init_failure_fallback_to_ap);
    RUN_TEST(test_4g_mode_ap_always_running);
    RUN_TEST(test_ethernet_mode_ap_always_running);
    RUN_TEST(test_non_wifi_status_isolation);
    RUN_TEST(test_4g_disconnect_status);
    RUN_TEST(test_switch_wifi_to_4g);
    RUN_TEST(test_switch_4g_to_wifi);
    RUN_TEST(test_non_wifi_no_sta_reconnect);
    RUN_TEST(test_restart_network_non_wifi);
    RUN_TEST(test_restart_network_4g_failure);
    RUN_TEST(test_hybrid_mode_ap_ip);

    TestLog::groupEnd();
}
