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
