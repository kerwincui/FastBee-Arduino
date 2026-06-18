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
    String customDomain = "fastbee";
    String actualHostname = "";

    // 以太网自动重连状态
    bool ethReconnectPending = false;
    int ethReconnectAttempts = 0;
    unsigned long ethReconnectTime = 0;
    static constexpr unsigned long ETH_RECONNECT_INTERVAL_MS = 10000;
    static constexpr int ETH_MAX_RECONNECT_ATTEMPTS = 10;

    // 最后保障 AP 状态
    bool lastResortAPStarted = false;
    int initFailCount = 0;
    bool factoryResetTriggered = false;
    bool forceLastResortAPFail = false;  // 测试用：强制最后保障 AP 失败
    bool forceAPFail = false;             // 测试用：强制正常 AP 启动失败

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
                    actualHostname = customDomain;
                    initSequence.push_back("MDNS_STARTED");
                }
                return true;
            } else {
                networkType = NetType::NET_WIFI;
                mode = NetMode::NETWORK_AP;
                startAP();
                if (!apRunning) ensureLastResortAP();  // 模拟真实代码中的最后保障
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
                    actualHostname = customDomain;
                    initSequence.push_back("MDNS_STARTED");
                }
                return true;
            } else {
                networkType = NetType::NET_WIFI;
                mode = NetMode::NETWORK_AP;
                startAP();
                if (!apRunning) ensureLastResortAP();  // 模拟真实代码中的最后保障
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
                if (!apRunning) ensureLastResortAP();  // 模拟真实代码中的最后保障
                return false;
            }
        }

        if (networkType == NetType::NET_WIFI) {
            if (mode == NetMode::NETWORK_AP) {
                startAP();
                if (!apRunning) ensureLastResortAP();  // 模拟真实代码中的最后保障
                if (apRunning && enableMDNS) {
                    mDNSStarted = true;
                    actualHostname = customDomain;
                }
                return apRunning;  // AP 模式失败则返回 false
            }
            if (adapterSuccess) {
                status = NetStatus::CONNECTED;
                ipAddress = "192.168.1.100";
                internetAvailable = true;
                initFailCount = 0;  // 初始化成功，清除失败计数
                if (enableMDNS) {
                    mDNSStarted = true;
                    actualHostname = customDomain;
                }
                return true;
            }
            status = NetStatus::CONNECTION_FAILED;
            // STA 连接失败，回退到 AP
            startAP();
            if (!apRunning) ensureLastResortAP();  // 模拟真实代码中的最后保障
            return false;
        }

        // 未知网络类型，回退到最后保障 AP
        ensureLastResortAP();
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
        NetType prevType = networkType;
        disconnect();
        bool ok = initialize(adapterSuccess);
        // 模拟真实代码：非 WiFi 模式下 restartNetwork 失败时调用 ensureLastResortAP
        if (!ok && prevType != NetType::NET_WIFI) {
            if (!isAPRunning()) {
                ensureLastResortAP();
            }
        }
        return ok;
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
        if (forceAPFail) return;  // 模拟 AP 启动失败
        apRunning = true;
        apIPAddress = "192.168.4.1";
        status = NetStatus::AP_MODE;
    }

    void startAPForHybrid() {
        if (forceAPFail) return;  // 模拟 AP 启动失败
        apRunning = true;
        apIPAddress = "192.168.4.1";
    }

    /**
     * 模拟 ensureLastResortAP() 逻辑
     * @return true=AP 成功启动，false=AP 也失败（触发重启）
     */
    bool ensureLastResortAP() {
        // 如果 AP 已在运行，直接成功
        if (apRunning && apIPAddress != "") {
            return true;
        }
        // 尝试启动最后保障 AP
        if (!forceLastResortAPFail) {
            apRunning = true;
            apIPAddress = "192.168.4.1";
            lastResortAPStarted = true;
            initFailCount = 0;
            return true;
        }
        // AP 也失败，记录连续失败次数
        initFailCount++;
        if (initFailCount >= 3) {
            factoryResetTriggered = true;
            initFailCount = 0;
        }
        return false;  // 模拟 ESP.restart() 前返回
    }

    /**
     * 模拟 updateConfig 中 mDNS/域名配置变更的处理逻辑
     * 对应 NetworkManager.cpp updateConfig() 中的 mdnsConfigChanged 分支
     * @param newDomain 新自定义域名
     * @param newEnableMDNS 新 mDNS 启用状态
     * @return true=配置已应用
     */
    bool updateMDNSConfig(const String& newDomain, bool newEnableMDNS) {
        bool domainChanged = (newDomain != customDomain);
        bool enableChanged = (newEnableMDNS != enableMDNS);
        bool mdnsConfigChanged = domainChanged || enableChanged;

        if (!mdnsConfigChanged) return true;

        customDomain = newDomain;
        enableMDNS = newEnableMDNS;

        if (enableMDNS) {
            // mDNS 启用：重启 mDNS 服务（模拟 restartMDNS）
            mDNSStarted = true;
            actualHostname = customDomain;
            return true;
        } else {
            // mDNS 禁用：停止 mDNS 服务
            mDNSStarted = false;
            actualHostname = "";
            return true;
        }
    }

    /**
     * 模拟 setCustomDomain 行为：域名变更时如果 mDNS 已启动则自动重启
     */
    void setCustomDomain(const String& domain) {
        if (customDomain != domain) {
            customDomain = domain;
            if (mDNSStarted && actualHostname != domain) {
                // 自动重启 mDNS 以应用新域名
                actualHostname = domain;
            }
        }
    }

    String getActualHostname() const { return actualHostname; }

    /**
     * 模拟 update() 中的以太网自动重连状态机
     * @param adapterConnected 以太网适配器当前连接状态
     * @param currentTime 当前时间戳 (ms)
     * @param doRestart    是否实际执行重启 (true=执行重连，false=仅调度)
     * @return 重连结果 (true=重连成功)
     */
    bool simulateEthReconnectUpdate(bool adapterConnected, unsigned long currentTime, bool doRestart = true) {
        if (networkType != NetType::NET_ETHERNET) return false;

        bool wasConnected = (status == NetStatus::CONNECTED);

        // 断连 → 调度重连
        if (!adapterConnected && wasConnected) {
            status = NetStatus::DISCONNECTED;
            if (ethReconnectAttempts < ETH_MAX_RECONNECT_ATTEMPTS) {
                ethReconnectPending = true;
                ethReconnectTime = currentTime + ETH_RECONNECT_INTERVAL_MS;
            }
        }

        // 初始断连状态（wasConnected=false）也触发调度
        if (!adapterConnected && !ethReconnectPending &&
            ethReconnectAttempts < ETH_MAX_RECONNECT_ATTEMPTS) {
            ethReconnectPending = true;
            ethReconnectTime = currentTime + ETH_RECONNECT_INTERVAL_MS;
        }

        // 连接成功 → 重置计数器
        if (adapterConnected && !wasConnected) {
            status = NetStatus::CONNECTED;
            ethReconnectAttempts = 0;
            ethReconnectPending = false;
            return true;
        }

        // 执行重连
        if (ethReconnectPending && currentTime >= ethReconnectTime && doRestart) {
            ethReconnectPending = false;
            ethReconnectAttempts++;
            // 模拟重启结果
            if (adapterConnected) {
                status = NetStatus::CONNECTED;
                ethReconnectAttempts = 0;
                ethReconnectPending = false;
                return true;
            } else {
                // 重连失败，调度下一次
                if (ethReconnectAttempts < ETH_MAX_RECONNECT_ATTEMPTS) {
                    ethReconnectPending = true;
                    ethReconnectTime = currentTime + ETH_RECONNECT_INTERVAL_MS;
                } else {
                    // 重连耗尽：确保 AP 始终可用（模拟 ensureLastResortAP 调用）
                    ensureLastResortAP();
                }
                return false;
            }
        }

        return adapterConnected;
    }
};

#endif // MOCK_MULTI_NETWORK_H
