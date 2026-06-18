/**
 * @file MockMultiNetwork.h
 * @brief 多网络模式模拟器，用于非WiFi联网方式测试
 * 
 * 模拟 NetworkManager 中多网络模式切换、失败回退、状态隔离的核心逻辑。
 * 支持 4G/以太网/LoRa 等非 WiFi 联网方式的测试。
 */

#ifndef MOCK_MULTI_NETWORK_H
#define MOCK_MULTI_NETWORK_H

#include <Arduino.h>
#include <vector>
#include "MockWiFi.h"

class MockMultiNetworkManager {
public:
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
    bool mDNSStarted = false;
    bool enableMDNS = true;

    std::vector<String> initSequence;
    NetStatus wifiManagerStatus = NetStatus::AP_MODE;

    bool initialize(bool adapterSuccess) {
        initSequence.clear();

        if (networkType == NetType::NET_4G) {
            if (adapterSuccess) {
                status = NetStatus::CONNECTED;
                ipAddress = "10.0.0.100";
                internetAvailable = true;
                initSequence.push_back("4G_CONNECTED");
                startAPForHybrid();
                initSequence.push_back("AP_STARTED");
                if (enableMDNS) {
                    mDNSStarted = true;
                    initSequence.push_back("MDNS_STARTED");
                }
                return true;
            } else {
                networkType = NetType::NET_WIFI;
                mode = NetMode::NETWORK_AP;
                startAP();
                return false;
            }
        }

        if (networkType == NetType::NET_ETHERNET) {
            if (adapterSuccess) {
                status = NetStatus::CONNECTED;
                ipAddress = "192.168.1.200";
                internetAvailable = true;
                initSequence.push_back("ETH_CONNECTED");
                startAPForHybrid();
                initSequence.push_back("AP_STARTED");
                if (enableMDNS) {
                    mDNSStarted = true;
                    initSequence.push_back("MDNS_STARTED");
                }
                return true;
            } else {
                networkType = NetType::NET_WIFI;
                mode = NetMode::NETWORK_AP;
                startAP();
                return false;
            }
        }

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

        if (networkType == NetType::NET_WIFI) {
            if (mode == NetMode::NETWORK_AP) {
                startAP();
                return true;
            }
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

    void updateStatusInfo(bool adapterConnected, const String& adapterIP) {
        if (networkType != NetType::NET_WIFI) {
            apClientCount = 1;
            apIPAddress = "192.168.4.1";
            status = adapterConnected ? NetStatus::CONNECTED : NetStatus::DISCONNECTED;
            ipAddress = adapterConnected ? adapterIP : "";
            internetAvailable = adapterConnected;
            return;
        }
        status = wifiManagerStatus;
    }

    bool restartNetwork(bool adapterSuccess) {
        disconnect();
        return initialize(adapterSuccess);
    }

    void disconnect() {
        status = NetStatus::DISCONNECTED;
        ipAddress = "";
        internetAvailable = false;
        apRunning = false;
        mDNSStarted = false;
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
    }
};

#endif // MOCK_MULTI_NETWORK_H
