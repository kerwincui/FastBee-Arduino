/**
 * @file test_wifi_ip_dns.cpp
 * @brief WiFi/IP/DNS 网络子模块单元测试
 * 
 * 测试内容：
 * - WiFi 管理器: STA/AP/AP_STA 模式切换、连接/断开、信号强度、扫描
 * - IP 管理器: DHCP/静态 IP、子网/网关/DNS 配置、IP 冲突检测
 * - DNS 管理器: 域名解析、mDNS 注册、解析失败处理、缓存机制
 * - 网络状态: 状态机转换、重连逻辑、事件通知
 */

#include <unity.h>
#include <Arduino.h>
#include "mocks/MockWiFi.h"

void test_wifi_ip_dns_group();

// ========== Mock: IP 管理器 ==========

struct IPConfig {
    bool useDHCP;
    String staticIP;
    String subnet;
    String gateway;
    String dns1;
    String dns2;
    
    IPConfig() : useDHCP(true), subnet("255.255.255.0"), 
                 gateway("192.168.1.1"), dns1("8.8.8.8"), dns2("8.8.4.4") {}
};

class MockIPManager {
public:
    MockIPManager() : _configured(false), _hasConflict(false),
        autoFailover(true), conflictCheckInterval(30000),
        maxFailoverAttempts(3), conflictThreshold(2),
        fallbackToDHCP(true), _conflictCount(0), _failoverCount(0),
        _switchedToDHCP(false) {}

    bool configure(const IPConfig& config) {
        if (!config.useDHCP && config.staticIP.isEmpty()) {
            return false;  // 静态 IP 模式必须提供 IP
        }
        _config = config;
        _configured = true;
        return true;
    }

    IPConfig getConfig() { return _config; }
    bool isConfigured() { return _configured; }

    String getCurrentIP() {
        if (_config.useDHCP) {
            return "192.168.1.100";  // DHCP 分配的模拟 IP
        }
        return _config.staticIP;
    }

    bool checkIPConflict(const String& ip) {
        return _hasConflict;
    }

    bool validateIPAddress(const String& ip) {
        // 简单 IP 格式验证
        int dots = 0;
        for (int i = 0; i < (int)ip.length(); i++) {
            if (ip[i] == '.') dots++;
            else if (ip[i] < '0' || ip[i] > '9') return false;
        }
        return dots == 3;
    }

    bool validateSubnet(const String& subnet) {
        return subnet == "255.255.255.0" || subnet == "255.255.0.0" || 
               subnet == "255.0.0.0";
    }

    // 模拟 IP 冲突检测+故障转移行为
    // 每次调用算一次冲突检测，达到 conflictThreshold 后触发故障转移
    bool simulateConflictCycle() {
        if (_hasConflict) {
            _conflictCount++;
            if (_conflictCount >= conflictThreshold) {
                if (autoFailover) {
                    return performFailover();
                }
                return false;  // 冲突但未启用故障转移
            }
        } else {
            _conflictCount = 0;
        }
        return true;  // 无冲突
    }

    bool performFailover() {
        if (_failoverCount >= maxFailoverAttempts) {
            if (fallbackToDHCP) {
                _switchedToDHCP = true;
                _config.useDHCP = true;
                return true;
            }
            return false;  // 达到上限且不允许 DHCP 回退
        }
        _failoverCount++;
        return true;
    }

    // 配置同步方法（镜像 NetworkManager::syncIPManagerConfig）
    void syncFromConfig(bool af, uint16_t cci, uint8_t mfa, uint8_t ct, bool ftd) {
        autoFailover = af;
        conflictCheckInterval = cci;
        maxFailoverAttempts = mfa;
        conflictThreshold = ct;
        fallbackToDHCP = ftd;
    }

    int getConflictCount() { return _conflictCount; }
    int getFailoverCount() { return _failoverCount; }
    bool isSwitchedToDHCP() { return _switchedToDHCP; }

    // 测试控制
    void setHasConflict(bool conflict) { _hasConflict = conflict; }
    void reset() {
        _configured = false; _hasConflict = false; _config = IPConfig();
        autoFailover = true; conflictCheckInterval = 30000;
        maxFailoverAttempts = 3; conflictThreshold = 2; fallbackToDHCP = true;
        _conflictCount = 0; _failoverCount = 0; _switchedToDHCP = false;
    }

    // 配置属性（镜像 IPManager.h 的 public 字段）
    bool autoFailover;
    uint16_t conflictCheckInterval;
    uint8_t maxFailoverAttempts;
    uint8_t conflictThreshold;
    bool fallbackToDHCP;

private:
    IPConfig _config;
    bool _configured;
    bool _hasConflict;
    int _conflictCount;
    int _failoverCount;
    bool _switchedToDHCP;
};

// ========== Mock: DNS 管理器 ==========

class MockDNSManager {
public:
    MockDNSManager() : _mdnsStarted(false), _shouldFail(false),
                       _cacheHits(0), _cacheMisses(0) {}

    bool resolve(const String& hostname, String& result) {
        // 检查缓存
        auto it = _cache.find(hostname);
        if (it != _cache.end()) {
            result = it->second;
            _cacheHits++;
            return true;
        }
        
        _cacheMisses++;
        
        if (_shouldFail) {
            return false;
        }
        
        // 模拟解析
        if (hostname == "mqtt.fastbee.cn") {
            result = "120.55.96.2";
            _cache[hostname] = result;
            return true;
        }
        if (hostname == "api.fastbee.cn") {
            result = "47.96.152.8";
            _cache[hostname] = result;
            return true;
        }
        
        // 未知主机
        return false;
    }

    bool startMDNS(const String& hostname) {
        if (hostname.isEmpty()) return false;
        _mdnsHostname = hostname;
        _mdnsStarted = true;
        return true;
    }

    bool stopMDNS() {
        _mdnsStarted = false;
        _mdnsHostname = "";
        return true;
    }

    bool addMDNSService(const String& service, const String& protocol, uint16_t port) {
        if (!_mdnsStarted) return false;
        _mdnsServices.push_back(service + "." + protocol + ":" + String(port));
        return true;
    }

    bool isMDNSRunning() { return _mdnsStarted; }
    String getMDNSHostname() { return _mdnsHostname; }
    int getCacheHits() { return _cacheHits; }
    int getCacheMisses() { return _cacheMisses; }
    int getCacheSize() { return _cache.size(); }

    void clearCache() { _cache.clear(); _cacheHits = 0; _cacheMisses = 0; }

    // 测试控制
    void setShouldFail(bool fail) { _shouldFail = fail; }
    void reset() {
        _cache.clear();
        _mdnsStarted = false;
        _mdnsHostname = "";
        _mdnsServices.clear();
        _shouldFail = false;
        _cacheHits = 0;
        _cacheMisses = 0;
    }

private:
    std::map<String, String> _cache;
    bool _mdnsStarted;
    bool _shouldFail;
    String _mdnsHostname;
    std::vector<String> _mdnsServices;
    int _cacheHits;
    int _cacheMisses;
};

// ========== WiFi 模式切换测试 ==========

static void test_wifi_sta_mode() {
    MockWiFi.mode(WIFI_STA);
    TEST_ASSERT_EQUAL((int)WIFI_STA, (int)MockWiFi.getMode());
}

static void test_wifi_ap_mode() {
    MockWiFi.mode(WIFI_AP);
    TEST_ASSERT_EQUAL((int)WIFI_AP, (int)MockWiFi.getMode());
    
    TEST_ASSERT_TRUE(MockWiFi.softAP("FastBee-AP", "12345678", 6));
    TEST_ASSERT_EQUAL_STRING("FastBee-AP", MockWiFi.softAPSSID().c_str());
}

static void test_wifi_ap_sta_mode() {
    MockWiFi.mode(WIFI_AP_STA);
    TEST_ASSERT_EQUAL((int)WIFI_AP_STA, (int)MockWiFi.getMode());
}

static void test_wifi_off_mode() {
    MockWiFi.mode(WIFI_OFF);
    TEST_ASSERT_EQUAL((int)WIFI_OFF, (int)MockWiFi.getMode());
    TEST_ASSERT_EQUAL((int)WL_DISCONNECTED, (int)MockWiFi.status());
}

// ========== WiFi STA 连接测试 ==========

static void test_wifi_connect_success() {
    MockWiFi.mode(WIFI_STA);
    MockWiFi.setShouldFail(false);
    
    int result = MockWiFi.begin("TestSSID", "TestPass");
    
    TEST_ASSERT_EQUAL((int)WL_CONNECTED, result);
    TEST_ASSERT_EQUAL((int)WL_CONNECTED, (int)MockWiFi.status());
    TEST_ASSERT_EQUAL_STRING("TestSSID", MockWiFi.SSID().c_str());
}

static void test_wifi_connect_failure() {
    MockWiFi.mode(WIFI_STA);
    MockWiFi.setShouldFail(true);
    
    int result = MockWiFi.begin("BadSSID", "BadPass");
    
    TEST_ASSERT_EQUAL((int)WL_CONNECT_FAILED, result);
    TEST_ASSERT_FALSE(MockWiFi.status() == WL_CONNECTED);
    
    MockWiFi.setShouldFail(false);  // 恢复
}

static void test_wifi_disconnect() {
    MockWiFi.mode(WIFI_STA);
    MockWiFi.begin("TestSSID", "TestPass");
    
    MockWiFi.disconnect();
    TEST_ASSERT_EQUAL((int)WL_DISCONNECTED, (int)MockWiFi.status());
}

static void test_wifi_disconnect_with_off() {
    MockWiFi.mode(WIFI_STA);
    MockWiFi.begin("TestSSID", "TestPass");
    
    MockWiFi.disconnect(true);  // wifioff = true
    TEST_ASSERT_EQUAL((int)WIFI_OFF, (int)MockWiFi.getMode());
}

// ========== WiFi IP 和信号测试 ==========

static void test_wifi_ip_after_connect() {
    MockWiFi.mode(WIFI_STA);
    MockWiFi.begin("TestSSID", "TestPass");
    
    IPAddress ip = MockWiFi.localIP();
    // 连接后应有有效 IP（非 0.0.0.0）
    TEST_ASSERT_TRUE(ip[0] != 0 || ip[1] != 0);
}

static void test_wifi_rssi_connected() {
    MockWiFi.setConnected(true);
    
    int8_t rssi = MockWiFi.RSSI();
    TEST_ASSERT_TRUE(rssi < 0);  // RSSI 应为负值
    TEST_ASSERT_TRUE(rssi > -100);  // 合理范围
}

static void test_wifi_rssi_disconnected() {
    MockWiFi.setConnected(false);
    
    int8_t rssi = MockWiFi.RSSI();
    TEST_ASSERT_EQUAL(0, rssi);
}

// ========== WiFi AP 模式测试 ==========

static void test_wifi_softap_config() {
    MockWiFi.mode(WIFI_AP);
    
    IPAddress apIP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    
    TEST_ASSERT_TRUE(MockWiFi.softAPConfig(apIP, gateway, subnet));
    
    IPAddress resultIP = MockWiFi.softAPIP();
    TEST_ASSERT_EQUAL(192, resultIP[0]);
    TEST_ASSERT_EQUAL(168, resultIP[1]);
    TEST_ASSERT_EQUAL(4, resultIP[2]);
    TEST_ASSERT_EQUAL(1, resultIP[3]);
}

static void test_wifi_ap_client_count() {
    MockWiFi.mode(WIFI_AP);
    MockWiFi.softAP("TestAP");
    
    MockWiFi.setAPClients(3);
    TEST_ASSERT_EQUAL(3, MockWiFi.softAPgetStationNum());
}

// ========== WiFi 扫描和主机名测试 ==========

static void test_wifi_scan_networks() {
    int8_t count = MockWiFi.scanNetworks();
    TEST_ASSERT_TRUE(count > 0);
}

// 辅助函数：将 wifi_auth_mode_t 映射为字符串，与 WiFiManager.cpp 逻辑一致
static const char* authModeToEncString(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN:          return "open";
        case WIFI_AUTH_WEP:           return "wep";
        case WIFI_AUTH_WPA_PSK:       return "wpa";
        case WIFI_AUTH_WPA2_PSK:      return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK:  return "wpa2";  // WPA/WPA2混合归并为wpa2
        case WIFI_AUTH_WPA3_PSK:      return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa3";  // WPA2/WPA3混合归并为wpa3
        default:                      return "wpa2";  // 未知默认wpa2
    }
}

// 辅助函数：将加密字符串转为前端安全下拉值，与 network.js 逻辑一致
static const char* encStringToSecurityValue(const char* enc) {
    // open → "none"，其他直接使用
    if (strcmp(enc, "open") == 0) return "none";
    return enc;  // "wpa", "wpa2", "wpa3" 直接使用
}

// 测试：WiFi 扫描返回不同加密类型的网络
static void test_wifi_scan_encryption_types() {
    // 配置混合加密类型的扫描结果
    const char* ssids[]    = {"OpenNet", "WPA2-Home", "WPA3-Office", "WPA-Mixed", "WEP-Old"};
    const int32_t rssis[]  = {-40, -55, -65, -70, -80};
    const wifi_auth_mode_t encs[] = {
        WIFI_AUTH_OPEN, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA3_PSK,
        WIFI_AUTH_WPA_WPA2_PSK, WIFI_AUTH_WEP
    };
    const uint8_t chs[]    = {1, 6, 11, 3, 9};
    const char* bssids[]   = {"11:11:11:11:11:11", "22:22:22:22:22:22", "33:33:33:33:33:33",
                              "44:44:44:44:44:44", "55:55:55:55:55:55"};
    MockWiFi.setScanResults(ssids, rssis, encs, chs, bssids, 5);

    int8_t count = MockWiFi.scanNetworks();
    TEST_ASSERT_EQUAL(5, count);

    // 验证每个网络的加密类型
    TEST_ASSERT_EQUAL(WIFI_AUTH_OPEN,          MockWiFi.encryptionType(0));
    TEST_ASSERT_EQUAL(WIFI_AUTH_WPA2_PSK,      MockWiFi.encryptionType(1));
    TEST_ASSERT_EQUAL(WIFI_AUTH_WPA3_PSK,      MockWiFi.encryptionType(2));
    TEST_ASSERT_EQUAL(WIFI_AUTH_WPA_WPA2_PSK,  MockWiFi.encryptionType(3));
    TEST_ASSERT_EQUAL(WIFI_AUTH_WEP,           MockWiFi.encryptionType(4));
}

// 测试：加密类型枚举到字符串的映射（与后端 WiFiManager.cpp 逻辑保持一致）
static void test_wifi_encryption_to_string_mapping() {
    // 开放网络
    TEST_ASSERT_EQUAL_STRING("open", authModeToEncString(WIFI_AUTH_OPEN));
    // WEP（老旧加密）
    TEST_ASSERT_EQUAL_STRING("wep",  authModeToEncString(WIFI_AUTH_WEP));
    // 纯 WPA
    TEST_ASSERT_EQUAL_STRING("wpa",  authModeToEncString(WIFI_AUTH_WPA_PSK));
    // WPA2（最常见）
    TEST_ASSERT_EQUAL_STRING("wpa2", authModeToEncString(WIFI_AUTH_WPA2_PSK));
    // WPA/WPA2 混合 → 归并为 wpa2
    TEST_ASSERT_EQUAL_STRING("wpa2", authModeToEncString(WIFI_AUTH_WPA_WPA2_PSK));
    // WPA3
    TEST_ASSERT_EQUAL_STRING("wpa3", authModeToEncString(WIFI_AUTH_WPA3_PSK));
    // WPA2/WPA3 混合 → 归并为 wpa3
    TEST_ASSERT_EQUAL_STRING("wpa3", authModeToEncString(WIFI_AUTH_WPA2_WPA3_PSK));
}

// 测试：加密字符串到前端安全下拉选项值的映射
static void test_wifi_encryption_to_security_select() {
    // open → "none"
    TEST_ASSERT_EQUAL_STRING("none", encStringToSecurityValue(authModeToEncString(WIFI_AUTH_OPEN)));
    // WPA → "wpa"
    TEST_ASSERT_EQUAL_STRING("wpa",  encStringToSecurityValue(authModeToEncString(WIFI_AUTH_WPA_PSK)));
    // WPA2 → "wpa2"
    TEST_ASSERT_EQUAL_STRING("wpa2", encStringToSecurityValue(authModeToEncString(WIFI_AUTH_WPA2_PSK)));
    // WPA/WPA2 混合 → "wpa2"
    TEST_ASSERT_EQUAL_STRING("wpa2", encStringToSecurityValue(authModeToEncString(WIFI_AUTH_WPA_WPA2_PSK)));
    // WPA3 → "wpa3"
    TEST_ASSERT_EQUAL_STRING("wpa3", encStringToSecurityValue(authModeToEncString(WIFI_AUTH_WPA3_PSK)));
    // WPA2/WPA3 混合 → "wpa3"
    TEST_ASSERT_EQUAL_STRING("wpa3", encStringToSecurityValue(authModeToEncString(WIFI_AUTH_WPA2_WPA3_PSK)));
}

// 测试：扫描结果的信道和 BSSID 正确返回
static void test_wifi_scan_channel_and_bssid() {
    const char* ssids[]    = {"Net-A", "Net-B"};
    const int32_t rssis[]  = {-50, -60};
    const wifi_auth_mode_t encs[] = {WIFI_AUTH_WPA2_PSK, WIFI_AUTH_OPEN};
    const uint8_t chs[]    = {6, 11};
    const char* bssids[]   = {"DE:AD:BE:EF:00:01", "DE:AD:BE:EF:00:02"};
    MockWiFi.setScanResults(ssids, rssis, encs, chs, bssids, 2);

    TEST_ASSERT_EQUAL(2, MockWiFi.scanNetworks());
    TEST_ASSERT_EQUAL(6,  MockWiFi.channel(0));
    TEST_ASSERT_EQUAL(11, MockWiFi.channel(1));
    TEST_ASSERT_EQUAL_STRING("DE:AD:BE:EF:00:01", MockWiFi.BSSIDstr(0).c_str());
    TEST_ASSERT_EQUAL_STRING("DE:AD:BE:EF:00:02", MockWiFi.BSSIDstr(1).c_str());
}

// 测试：扫描结果索引越界时返回安全默认值
static void test_wifi_scan_out_of_bounds() {
    const char* ssids[]    = {"Only-Net"};
    const int32_t rssis[]  = {-50};
    const wifi_auth_mode_t encs[] = {WIFI_AUTH_WPA2_PSK};
    const uint8_t chs[]    = {1};
    const char* bssids[]   = {"00:00:00:00:00:00"};
    MockWiFi.setScanResults(ssids, rssis, encs, chs, bssids, 1);

    TEST_ASSERT_EQUAL(1, MockWiFi.scanNetworks());
    // 越界索引应返回安全默认值
    TEST_ASSERT_EQUAL(WIFI_AUTH_OPEN, MockWiFi.encryptionType(5));
    TEST_ASSERT_EQUAL_STRING("", MockWiFi.SSID(5).c_str());
    TEST_ASSERT_EQUAL(-100, MockWiFi.RSSI(5));
    TEST_ASSERT_EQUAL(1,    MockWiFi.channel(5));
    TEST_ASSERT_EQUAL_STRING("", MockWiFi.BSSIDstr(5).c_str());
}

static void test_wifi_hostname() {
    TEST_ASSERT_TRUE(MockWiFi.setHostname("fastbee-device"));
    TEST_ASSERT_EQUAL_STRING("fastbee-device", MockWiFi.getHostname().c_str());
}

static void test_wifi_auto_reconnect() {
    MockWiFi.setAutoReconnect(true);
    TEST_ASSERT_TRUE(MockWiFi.getAutoReconnect());
    
    MockWiFi.setAutoReconnect(false);
    TEST_ASSERT_FALSE(MockWiFi.getAutoReconnect());
}

// ========== WiFi 状态信息测试 ==========

static void test_wifi_status_info_connected() {
    MockWiFi.mode(WIFI_STA);
    MockWiFi.setConnected(true);
    
    auto info = MockWiFi.getStatusInfo();
    TEST_ASSERT_TRUE(info.wifiConnected);
    TEST_ASSERT_EQUAL_STRING("CONNECTED", info.status.c_str());
}

static void test_wifi_status_info_ap_mode() {
    MockWiFi.mode(WIFI_AP);
    
    auto info = MockWiFi.getStatusInfo();
    TEST_ASSERT_EQUAL_STRING("AP_MODE", info.status.c_str());
}

static void test_wifi_status_info_disconnected() {
    MockWiFi.mode(WIFI_STA);
    MockWiFi.setConnected(false);
    
    auto info = MockWiFi.getStatusInfo();
    TEST_ASSERT_FALSE(info.wifiConnected);
    TEST_ASSERT_EQUAL_STRING("DISCONNECTED", info.status.c_str());
}

// ========== IP 管理器测试 ==========

static MockIPManager ipMgr;

static void test_ip_dhcp_config() {
    ipMgr.reset();
    
    IPConfig config;
    config.useDHCP = true;
    
    TEST_ASSERT_TRUE(ipMgr.configure(config));
    TEST_ASSERT_TRUE(ipMgr.isConfigured());
    TEST_ASSERT_EQUAL_STRING("192.168.1.100", ipMgr.getCurrentIP().c_str());
}

static void test_ip_static_config() {
    ipMgr.reset();
    
    IPConfig config;
    config.useDHCP = false;
    config.staticIP = "192.168.1.50";
    config.subnet = "255.255.255.0";
    config.gateway = "192.168.1.1";
    config.dns1 = "8.8.8.8";
    
    TEST_ASSERT_TRUE(ipMgr.configure(config));
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", ipMgr.getCurrentIP().c_str());
}

static void test_ip_static_no_ip_fails() {
    ipMgr.reset();
    
    IPConfig config;
    config.useDHCP = false;
    config.staticIP = "";  // 缺少静态 IP
    
    TEST_ASSERT_FALSE(ipMgr.configure(config));
}

static void test_ip_validate_valid() {
    ipMgr.reset();
    
    TEST_ASSERT_TRUE(ipMgr.validateIPAddress("192.168.1.1"));
    TEST_ASSERT_TRUE(ipMgr.validateIPAddress("10.0.0.1"));
    TEST_ASSERT_TRUE(ipMgr.validateIPAddress("255.255.255.0"));
}

static void test_ip_validate_invalid() {
    ipMgr.reset();
    
    TEST_ASSERT_FALSE(ipMgr.validateIPAddress("abc.def.ghi.jkl"));
    TEST_ASSERT_FALSE(ipMgr.validateIPAddress("192.168.1"));
    TEST_ASSERT_FALSE(ipMgr.validateIPAddress(""));
}

static void test_ip_validate_subnet() {
    ipMgr.reset();
    
    TEST_ASSERT_TRUE(ipMgr.validateSubnet("255.255.255.0"));
    TEST_ASSERT_TRUE(ipMgr.validateSubnet("255.255.0.0"));
    TEST_ASSERT_FALSE(ipMgr.validateSubnet("255.255.128.0"));
}

static void test_ip_conflict_detection() {
    ipMgr.reset();
    
    ipMgr.setHasConflict(false);
    TEST_ASSERT_FALSE(ipMgr.checkIPConflict("192.168.1.50"));
    
    ipMgr.setHasConflict(true);
    TEST_ASSERT_TRUE(ipMgr.checkIPConflict("192.168.1.50"));
}

/**
 * @brief 验证 IP 冲突阈值：达到阈值后才触发故障转移
 */
static void test_ip_conflict_threshold() {
    ipMgr.reset();
    ipMgr.conflictThreshold = 3;
    ipMgr.setHasConflict(true);

    // 第 1、2 次冲突未达阈值，不触发故障转移
    TEST_ASSERT_TRUE(ipMgr.simulateConflictCycle());
    TEST_ASSERT_EQUAL(1, ipMgr.getConflictCount());
    TEST_ASSERT_TRUE(ipMgr.simulateConflictCycle());
    TEST_ASSERT_EQUAL(2, ipMgr.getConflictCount());

    // 第 3 次达到阈值，触发故障转移
    TEST_ASSERT_TRUE(ipMgr.simulateConflictCycle());
    TEST_ASSERT_EQUAL(1, ipMgr.getFailoverCount());
}

/**
 * @brief 验证故障转移次数限制：达到上限后不再转移
 */
static void test_ip_failover_max_attempts() {
    ipMgr.reset();
    ipMgr.maxFailoverAttempts = 2;
    ipMgr.fallbackToDHCP = false;  // 不允许回退 DHCP
    ipMgr.setHasConflict(true);
    ipMgr.conflictThreshold = 1;

    // 第 1、2 次故障转移成功
    TEST_ASSERT_TRUE(ipMgr.simulateConflictCycle());
    TEST_ASSERT_EQUAL(1, ipMgr.getFailoverCount());
    TEST_ASSERT_TRUE(ipMgr.simulateConflictCycle());
    TEST_ASSERT_EQUAL(2, ipMgr.getFailoverCount());

    // 第 3 次达到上限且不允许 DHCP 回退，返回失败
    TEST_ASSERT_FALSE(ipMgr.simulateConflictCycle());
    TEST_ASSERT_FALSE(ipMgr.isSwitchedToDHCP());
}

/**
 * @brief 验证故障转移 DHCP 回退：达到上限后自动切换到 DHCP
 */
static void test_ip_failover_fallback_to_dhcp() {
    ipMgr.reset();
    ipMgr.maxFailoverAttempts = 1;
    ipMgr.fallbackToDHCP = true;
    ipMgr.conflictThreshold = 1;
    ipMgr.setHasConflict(true);

    // 第 1 次故障转移成功
    TEST_ASSERT_TRUE(ipMgr.simulateConflictCycle());
    TEST_ASSERT_EQUAL(1, ipMgr.getFailoverCount());

    // 第 2 次达到上限，自动回退到 DHCP
    TEST_ASSERT_TRUE(ipMgr.simulateConflictCycle());
    TEST_ASSERT_TRUE(ipMgr.isSwitchedToDHCP());
    TEST_ASSERT_TRUE(ipMgr.getConfig().useDHCP);
}

/**
 * @brief 验证 autoFailover 禁用时冲突不触发故障转移
 */
static void test_ip_auto_failover_disabled() {
    ipMgr.reset();
    ipMgr.autoFailover = false;
    ipMgr.conflictThreshold = 1;
    ipMgr.setHasConflict(true);

    // 冲突达到阈值但 autoFailover 关闭，不触发转移
    TEST_ASSERT_FALSE(ipMgr.simulateConflictCycle());
    TEST_ASSERT_EQUAL(0, ipMgr.getFailoverCount());
}

/**
 * @brief 验证配置同步：模拟 NetworkManager::syncIPManagerConfig 行为
 */
static void test_ip_config_sync() {
    ipMgr.reset();

    // 模拟 syncFromConfig（镜像 NetworkManager::syncIPManagerConfig）
    ipMgr.syncFromConfig(false, 60000, 5, 4, false);

    TEST_ASSERT_FALSE(ipMgr.autoFailover);
    TEST_ASSERT_EQUAL(60000, ipMgr.conflictCheckInterval);
    TEST_ASSERT_EQUAL(5, ipMgr.maxFailoverAttempts);
    TEST_ASSERT_EQUAL(4, ipMgr.conflictThreshold);
    TEST_ASSERT_FALSE(ipMgr.fallbackToDHCP);

    // 同步后的配置应影响行为
    ipMgr.setHasConflict(true);
    // 前 3 次不触发（阈值 = 4）
    TEST_ASSERT_TRUE(ipMgr.simulateConflictCycle());
    TEST_ASSERT_TRUE(ipMgr.simulateConflictCycle());
    TEST_ASSERT_TRUE(ipMgr.simulateConflictCycle());
    // 第 4 次达到阈值，但 autoFailover=false
    TEST_ASSERT_FALSE(ipMgr.simulateConflictCycle());
    TEST_ASSERT_EQUAL(0, ipMgr.getFailoverCount());
}

// ========== DNS 管理器测试 ==========

static MockDNSManager dnsMgr;

static void test_dns_resolve_known_host() {
    dnsMgr.reset();
    
    String ip;
    TEST_ASSERT_TRUE(dnsMgr.resolve("mqtt.fastbee.cn", ip));
    TEST_ASSERT_EQUAL_STRING("120.55.96.2", ip.c_str());
}

static void test_dns_resolve_unknown_host() {
    dnsMgr.reset();
    
    String ip;
    TEST_ASSERT_FALSE(dnsMgr.resolve("unknown.invalid", ip));
}

static void test_dns_resolve_failure() {
    dnsMgr.reset();
    dnsMgr.setShouldFail(true);
    
    String ip;
    TEST_ASSERT_FALSE(dnsMgr.resolve("mqtt.fastbee.cn", ip));
}

static void test_dns_cache_hit() {
    dnsMgr.reset();
    
    String ip1, ip2;
    dnsMgr.resolve("mqtt.fastbee.cn", ip1);  // cache miss
    dnsMgr.resolve("mqtt.fastbee.cn", ip2);  // cache hit
    
    TEST_ASSERT_EQUAL_STRING(ip1.c_str(), ip2.c_str());
    TEST_ASSERT_EQUAL(1, dnsMgr.getCacheHits());
    TEST_ASSERT_EQUAL(1, dnsMgr.getCacheMisses());
}

static void test_dns_cache_clear() {
    dnsMgr.reset();
    
    String ip;
    dnsMgr.resolve("mqtt.fastbee.cn", ip);
    TEST_ASSERT_EQUAL(1, dnsMgr.getCacheSize());
    
    dnsMgr.clearCache();
    TEST_ASSERT_EQUAL(0, dnsMgr.getCacheSize());
    TEST_ASSERT_EQUAL(0, dnsMgr.getCacheHits());
}

static void test_mdns_start() {
    dnsMgr.reset();
    
    TEST_ASSERT_TRUE(dnsMgr.startMDNS("fastbee-device"));
    TEST_ASSERT_TRUE(dnsMgr.isMDNSRunning());
    TEST_ASSERT_EQUAL_STRING("fastbee-device", dnsMgr.getMDNSHostname().c_str());
}

static void test_mdns_start_empty_hostname() {
    dnsMgr.reset();
    
    TEST_ASSERT_FALSE(dnsMgr.startMDNS(""));
    TEST_ASSERT_FALSE(dnsMgr.isMDNSRunning());
}

static void test_mdns_stop() {
    dnsMgr.reset();
    dnsMgr.startMDNS("fastbee");
    
    TEST_ASSERT_TRUE(dnsMgr.stopMDNS());
    TEST_ASSERT_FALSE(dnsMgr.isMDNSRunning());
}

static void test_mdns_add_service() {
    dnsMgr.reset();
    dnsMgr.startMDNS("fastbee");
    
    TEST_ASSERT_TRUE(dnsMgr.addMDNSService("_http", "_tcp", 80));
    TEST_ASSERT_TRUE(dnsMgr.addMDNSService("_mqtt", "_tcp", 1883));
}

static void test_mdns_add_service_without_start() {
    dnsMgr.reset();
    // mDNS 未启动时不能添加服务
    TEST_ASSERT_FALSE(dnsMgr.addMDNSService("_http", "_tcp", 80));
}

// ========== 测试组入口 ==========

void test_wifi_ip_dns_group() {
    // WiFi 模式切换
    RUN_TEST(test_wifi_sta_mode);
    RUN_TEST(test_wifi_ap_mode);
    RUN_TEST(test_wifi_ap_sta_mode);
    RUN_TEST(test_wifi_off_mode);
    
    // WiFi STA 连接
    RUN_TEST(test_wifi_connect_success);
    RUN_TEST(test_wifi_connect_failure);
    RUN_TEST(test_wifi_disconnect);
    RUN_TEST(test_wifi_disconnect_with_off);
    
    // WiFi IP 和信号
    RUN_TEST(test_wifi_ip_after_connect);
    RUN_TEST(test_wifi_rssi_connected);
    RUN_TEST(test_wifi_rssi_disconnected);
    
    // WiFi AP 模式
    RUN_TEST(test_wifi_softap_config);
    RUN_TEST(test_wifi_ap_client_count);
    
    // WiFi 扫描和主机名
    RUN_TEST(test_wifi_scan_networks);
    RUN_TEST(test_wifi_scan_encryption_types);
    RUN_TEST(test_wifi_encryption_to_string_mapping);
    RUN_TEST(test_wifi_encryption_to_security_select);
    RUN_TEST(test_wifi_scan_channel_and_bssid);
    RUN_TEST(test_wifi_scan_out_of_bounds);
    RUN_TEST(test_wifi_hostname);
    RUN_TEST(test_wifi_auto_reconnect);
    
    // WiFi 状态信息
    RUN_TEST(test_wifi_status_info_connected);
    RUN_TEST(test_wifi_status_info_ap_mode);
    RUN_TEST(test_wifi_status_info_disconnected);
    
    // IP 管理器
    RUN_TEST(test_ip_dhcp_config);
    RUN_TEST(test_ip_static_config);
    RUN_TEST(test_ip_static_no_ip_fails);
    RUN_TEST(test_ip_validate_valid);
    RUN_TEST(test_ip_validate_invalid);
    RUN_TEST(test_ip_validate_subnet);
    RUN_TEST(test_ip_conflict_detection);
    RUN_TEST(test_ip_conflict_threshold);
    RUN_TEST(test_ip_failover_max_attempts);
    RUN_TEST(test_ip_failover_fallback_to_dhcp);
    RUN_TEST(test_ip_auto_failover_disabled);
    RUN_TEST(test_ip_config_sync);
    
    // DNS 管理器
    RUN_TEST(test_dns_resolve_known_host);
    RUN_TEST(test_dns_resolve_unknown_host);
    RUN_TEST(test_dns_resolve_failure);
    RUN_TEST(test_dns_cache_hit);
    RUN_TEST(test_dns_cache_clear);
    RUN_TEST(test_mdns_start);
    RUN_TEST(test_mdns_start_empty_hostname);
    RUN_TEST(test_mdns_stop);
    RUN_TEST(test_mdns_add_service);
    RUN_TEST(test_mdns_add_service_without_start);
}
