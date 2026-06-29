/**
 * @file MockWiFi.h
 * @brief WiFi模拟对象，用于网络配置测试
 * 
 * 提供WiFi功能的模拟实现，支持STA/AP/AP_STA模式切换
 * 网络状态模拟，连接成功/失败场景
 */

#ifndef MOCK_WIFI_H
#define MOCK_WIFI_H

#include <Arduino.h>
#include <IPAddress.h>

// WiFi模式枚举
enum WiFiMode_t {
    WIFI_OFF = 0,
    WIFI_STA = 1,
    WIFI_AP = 2,
    WIFI_AP_STA = 3
};

// WiFi状态枚举
enum wl_status_t {
    WL_NO_SHIELD = 255,
    WL_IDLE_STATUS = 0,
    WL_NO_SSID_AVAIL = 1,
    WL_SCAN_COMPLETED = 2,
    WL_CONNECTED = 3,
    WL_CONNECT_FAILED = 4,
    WL_CONNECTION_LOST = 5,
    WL_DISCONNECTED = 6
};

// WiFi加密类型枚举（与 ESP-IDF wifi_auth_mode_t 对齐）
typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WEP,
    WIFI_AUTH_WPA_PSK,
    WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK,
    WIFI_AUTH_WAPI_PSK,
    WIFI_AUTH_WPA3_PSK,
    WIFI_AUTH_WPA2_WPA3_PSK,
    WIFI_AUTH_MAX
} wifi_auth_mode_t;

// 模拟WiFi类
class MockWiFiClass {
public:
    MockWiFiClass() : _mode(WIFI_OFF), _status(WL_DISCONNECTED), 
                      _autoReconnect(true), _connectionAttempts(0),
                      _shouldFail(false), _apClients(0) {
        _staIP = IPAddress(0, 0, 0, 0);
        _apIP = IPAddress(192, 168, 4, 1);
        _subnet = IPAddress(255, 255, 255, 0);
        _gateway = IPAddress(192, 168, 4, 1);
    }

    // 模式设置
    void mode(WiFiMode_t mode) { 
        _mode = mode;
        if (mode == WIFI_OFF) {
            _status = WL_DISCONNECTED;
        }
    }
    
    WiFiMode_t getMode() { return _mode; }

    // STA模式
    int begin(const char* ssid, const char* password = nullptr, 
              int32_t channel = 0, const uint8_t* bssid = nullptr, 
              bool connect = true) {
        _ssid = String(ssid);
        _password = password ? String(password) : "";
        _connectionAttempts = 0;
        
        if (_shouldFail) {
            _status = WL_CONNECT_FAILED;
            return WL_CONNECT_FAILED;
        }
        
        // 模拟连接过程
        if (connect) {
            _status = WL_CONNECTED;
            _staIP = IPAddress(192, 168, 1, 100);
            _gateway = IPAddress(192, 168, 1, 1);
        }
        
        return WL_CONNECTED;
    }

    void disconnect(bool wifioff = false) {
        _status = WL_DISCONNECTED;
        _staIP = IPAddress(0, 0, 0, 0);
        if (wifioff) {
            _mode = WIFI_OFF;
        }
    }

    wl_status_t status() { return _status; }
    
    String SSID() { return _ssid; }
    String psk() { return _password; }
    
    // IP地址
    IPAddress localIP() { 
        if (_mode == WIFI_AP || _mode == WIFI_AP_STA) {
            return _apIP;
        }
        return _staIP; 
    }
    
    IPAddress subnetMask() { return _subnet; }
    IPAddress gatewayIP() { return _gateway; }
    IPAddress dnsIP(uint8_t dns_no = 0) { 
        return IPAddress(8, 8, 8, 8); 
    }

    // AP模式
    bool softAP(const char* ssid, const char* passphrase = nullptr, 
                int channel = 1, int ssid_hidden = 0, 
                int max_connection = 4) {
        _apSSID = String(ssid);
        _apPassword = passphrase ? String(passphrase) : "";
        _apChannel = channel;
        _apHidden = (ssid_hidden != 0);
        _apClients = 0;
        return true;
    }

    bool softAPConfig(IPAddress local_ip, IPAddress gateway, IPAddress subnet) {
        _apIP = local_ip;
        _gateway = gateway;
        _subnet = subnet;
        return true;
    }

    bool softAPdisconnect(bool wifioff = false) {
        _apClients = 0;
        if (wifioff && _mode == WIFI_AP) {
            _mode = WIFI_OFF;
        }
        return true;
    }

    IPAddress softAPIP() { return _apIP; }
    uint8_t softAPgetStationNum() { return _apClients; }
    String softAPSSID() { return _apSSID; }
    String softAPPassword() { return _apPassword; }
    bool softAPHidden() { return _apHidden; }

    // 信号强度
    int8_t RSSI() { 
        if (_status == WL_CONNECTED) {
            return -45;  // 模拟良好信号
        }
        return 0;
    }

    // 重连设置
    void setAutoReconnect(bool autoReconnect) { 
        _autoReconnect = autoReconnect; 
    }
    
    bool getAutoReconnect() { return _autoReconnect; }

    // 主机名
    bool setHostname(const char* hostname) {
        _hostname = String(hostname);
        return true;
    }

    String getHostname() { return _hostname; }

    // MAC地址
    uint8_t* macAddress(uint8_t* mac) {
        mac[0] = 0xAA; mac[1] = 0xBB; mac[2] = 0xCC;
        mac[3] = 0xDD; mac[4] = 0xEE; mac[5] = 0xFF;
        return mac;
    }

    // 扫描网络
    int8_t scanNetworks(bool async = false, bool show_hidden = false, 
                        bool passive = false, uint32_t max_ms_per_chan = 300) {
        return _scanCount;  // 模拟发现的网络数量
    }

    String SSID(uint8_t networkItem) {
        if (networkItem < _scanCount) return _scanSSIDs[networkItem];
        return "";
    }

    int32_t RSSI(uint8_t networkItem) {
        if (networkItem < _scanCount) return _scanRSSIs[networkItem];
        return -100;
    }

    // 返回扫描结果中指定网络的加密类型
    wifi_auth_mode_t encryptionType(uint8_t networkItem) {
        if (networkItem < _scanCount) return _scanEncTypes[networkItem];
        return WIFI_AUTH_OPEN;
    }

    // 返回扫描结果中指定网络的信道
    uint8_t channel(uint8_t networkItem) {
        if (networkItem < _scanCount) return _scanChannels[networkItem];
        return 1;
    }

    // 返回扫描结果中指定网络的 BSSID 字符串
    String BSSIDstr(uint8_t networkItem) {
        if (networkItem < _scanCount) return _scanBSSIDs[networkItem];
        return "";
    }

    // 配置模拟扫描结果（测试辅助方法）
    void setScanResults(const char* ssids[], const int32_t rssis[],
                        const wifi_auth_mode_t encTypes[],
                        const uint8_t channels[], const char* bssids[],
                        uint8_t count) {
        _scanCount = (count > 5) ? 5 : count;
        for (uint8_t i = 0; i < _scanCount; i++) {
            _scanSSIDs[i]    = ssids[i];
            _scanRSSIs[i]    = rssis[i];
            _scanEncTypes[i] = encTypes[i];
            _scanChannels[i] = channels[i];
            _scanBSSIDs[i]   = bssids[i];
        }
    }

    // 测试控制方法
    void setShouldFail(bool fail) { _shouldFail = fail; }
    void setConnected(bool connected) { 
        _status = connected ? WL_CONNECTED : WL_DISCONNECTED;
        if (connected) {
            _staIP = IPAddress(192, 168, 1, 100);
        } else {
            _staIP = IPAddress(0, 0, 0, 0);
        }
    }
    void setAPClients(uint8_t clients) { _apClients = clients; }
    void addConnectionAttempt() { _connectionAttempts++; }
    int getConnectionAttempts() { return _connectionAttempts; }

    // 状态信息结构
    struct NetworkStatusInfo {
        bool wifiConnected;
        bool internetAvailable;
        String ssid;
        String ipAddress;
        String apIPAddress;
        int rssi;
        uint8_t apClientCount;
        String status;
    };

    NetworkStatusInfo getStatusInfo() {
        NetworkStatusInfo info;
        info.wifiConnected = (_status == WL_CONNECTED);
        info.internetAvailable = info.wifiConnected && !_shouldFail;
        info.ssid = _ssid;
        info.ipAddress = _staIP.toString();
        info.apIPAddress = _apIP.toString();
        info.rssi = RSSI();
        info.apClientCount = _apClients;
        
        if (_mode == WIFI_AP) {
            info.status = "AP_MODE";
        } else if (_mode == WIFI_STA) {
            info.status = (_status == WL_CONNECTED) ? "CONNECTED" : "DISCONNECTED";
        } else if (_mode == WIFI_AP_STA) {
            info.status = (_status == WL_CONNECTED) ? "AP_STA_CONNECTED" : "AP_MODE";
        } else {
            info.status = "OFF";
        }
        
        return info;
    }

private:
    WiFiMode_t _mode;
    wl_status_t _status;
    bool _autoReconnect;
    int _connectionAttempts;
    bool _shouldFail;
    
    String _ssid;
    String _password;
    IPAddress _staIP;
    IPAddress _apIP;
    IPAddress _subnet;
    IPAddress _gateway;
    String _hostname;
    
    String _apSSID;
    String _apPassword;
    int _apChannel;
    bool _apHidden;
    uint8_t _apClients;

    // 扫描结果模拟数据
    static const uint8_t _maxScan = 5;
    uint8_t _scanCount = 3;
    String _scanSSIDs[_maxScan]    = {"TestWiFi1", "TestWiFi2", "TestWiFi3", "", ""};
    int32_t _scanRSSIs[_maxScan]   = {-45, -60, -75, 0, 0};
    wifi_auth_mode_t _scanEncTypes[_maxScan] = {
        WIFI_AUTH_WPA2_PSK, WIFI_AUTH_OPEN, WIFI_AUTH_WPA_WPA2_PSK, WIFI_AUTH_OPEN, WIFI_AUTH_OPEN
    };
    uint8_t _scanChannels[_maxScan] = {1, 6, 11, 1, 1};
    String _scanBSSIDs[_maxScan] = {
        "AA:BB:CC:DD:EE:01", "AA:BB:CC:DD:EE:02", "AA:BB:CC:DD:EE:03", "", ""
    };
};

// 全局Mock WiFi实例
inline MockWiFiClass MockWiFi;

// 为了兼容性，提供WiFi宏定义
#define WiFi MockWiFi

#endif // MOCK_WIFI_H
