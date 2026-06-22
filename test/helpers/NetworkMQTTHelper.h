/**
 * @file NetworkMQTTHelper.h
 * @brief 网络切换 + MQTT 重建 可复用测试辅助函数
 *
 * 提取自 test_e2e_scenarios.cpp 中大量重复的"切换网络→重建MQTT"模式，
 * 消除测试代码耦合，降低维护成本。
 */

#ifndef NETWORK_MQTT_HELPER_H
#define NETWORK_MQTT_HELPER_H

#include <Arduino.h>
#include <unity.h>
#include "mocks/MockWiFi.h"
#include "mocks/MockMQTTClient.h"
#include "mocks/MockMultiNetwork.h"
#include "helpers/TestConfig.h"
#include "helpers/TestLogger.h"

namespace NetMQTTHelper {

    // ========== 网络切换 + MQTT 重建 ==========

    /**
     * @brief 切换联网方式并重建 MQTT 客户端
     *
     * 模拟真实代码中的网络切换流程：
     *   1. 停止/断开旧 MQTT
     *   2. 切换网络
     *   3. 若网络可用，重新初始化并连接 MQTT
     *
     * @param mgr     网络管理器
     * @param mqtt    MQTT 客户端（会被重新初始化）
     * @param config  MQTT 配置
     * @param targetNetType 目标联网方式
     * @param adapterSuccess 适配器是否成功连接
     * @return true=网络可用且 MQTT 已连接
     */
    inline bool switchNetworkAndReconnectMQTT(
        MockMultiNetworkManager& mgr,
        MockMQTTClient& mqtt,
        const MQTTConfig& config,
        MockMultiNetworkManager::NetType targetNetType,
        bool adapterSuccess)
    {
        // 1. 停止旧 MQTT
        if (mqtt.getIsConnected() || !mqtt.isStopped()) {
            mqtt.disconnect();
            mqtt.setStopped(true);
        }

        // 2. 切换网络
        mgr.disconnect();
        mgr.networkType = targetNetType;
        if (targetNetType == MockMultiNetworkManager::NetType::NET_WIFI) {
            mgr.mode = MockMultiNetworkManager::NetMode::NETWORK_STA;
        }
        bool netOk = mgr.initialize(adapterSuccess);

        // 3. 重建 MQTT
        if (netOk && mgr.internetAvailable) {
            mqtt.setStopped(false);
            mqtt.initialize(config);
            bool connected = mqtt.connect();
            return connected;
        }
        return false;
    }

    /**
     * @brief 验证 AP 模式全链路可访问性
     *
     * 检查 AP 已启动、IP 正确、mDNS 可用、Web 服务可达
     *
     * @param mgr 网络管理器（应处于 AP 模式）
     * @param checkMDNS 是否检查 mDNS
     */
    inline void assertAPAccessible(
        const MockMultiNetworkManager& mgr,
        bool checkMDNS = true)
    {
        TEST_ASSERT_TRUE_MESSAGE(mgr.isAPRunning(),
            "AP should be running for fallback access");
        TEST_ASSERT_EQUAL_STRING(TestConfig::DEFAULT_AP_IP,
            mgr.apIPAddress.c_str());

        if (checkMDNS && mgr.enableMDNS) {
            TEST_ASSERT_TRUE_MESSAGE(mgr.mDNSStarted,
                "mDNS should be running for device discovery");
        }
    }

    /**
     * @brief 验证网络已连接状态（STA/以太网/4G）
     */
    inline void assertNetworkConnected(const MockMultiNetworkManager& mgr) {
        TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetStatus::CONNECTED,
            (int)mgr.status);
        TEST_ASSERT_TRUE_MESSAGE(mgr.internetAvailable,
            "internetAvailable should be true when connected");
        TEST_ASSERT_FALSE_MESSAGE(mgr.ipAddress.isEmpty(),
            "IP address should not be empty when connected");
    }

    /**
     * @brief 验证 MQTT 客户端已连接
     */
    inline void assertMQTTConnected(MockMQTTClient& mqtt) {
        TEST_ASSERT_TRUE_MESSAGE(mqtt.getIsConnected(),
            "MQTT should be connected");
        TEST_ASSERT_FALSE_MESSAGE(mqtt.isStopped(),
            "MQTT should not be stopped");
    }

    /**
     * @brief 构建标准 MQTT 测试配置
     */
    inline MQTTConfig buildDefaultMQTTConfig(
        const char* clientId = "TestHelperClient",
        uint16_t reconnectInterval = 100)
    {
        MQTTConfig config;
        config.enabled = true;
        config.server = "iot.fastbee.cn";
        config.port = 1883;
        config.clientId = clientId;
        config.username = "admin";
        config.password = "password123";
        config.autoReconnect = true;
        config.reconnectInterval = reconnectInterval;
        return config;
    }

    /**
     * @brief 设置 ESP 堆内存模拟特定芯片环境
     * @return 之前的堆值（用于恢复）
     */
    inline uint32_t setChipEnvironment(const TestConfig::ChipProfile& chip) {
        uint32_t prevHeap = ESP.getFreeHeap();
        ESP.setFreeHeap(chip.totalHeap);
        if (chip.psramSize > 0) {
            ESP.setPsramSize(chip.psramSize);
            ESP.setFreePsram(chip.psramSize - 1024);
        }
        return prevHeap;
    }

    /**
     * @brief 恢复 ESP 堆内存设置
     */
    inline void restoreChipEnvironment(uint32_t prevHeap) {
        if (prevHeap > 0) {
            ESP.setFreeHeap(prevHeap);
        } else {
            ESP.resetHeapOverride();
        }
    }

    // ========== 网络切换步骤描述结构 ==========

    /**
     * @brief 批量网络切换步骤定义（用于长循环切换测试）
     */
    struct SwitchStep {
        MockMultiNetworkManager::NetType type;
        bool adapterSuccess;
        bool expectMQTT;   // 预期 MQTT 能否连上
    };

    /**
     * @brief 批量执行网络切换序列，验证每步后 MQTT 状态正确
     * @param steps 切换步骤数组
     * @param stepCount 步骤数量
     * @param rounds 重复轮数
     * @return 总通过的切换次数
     */
    inline int executeNetworkSwitchSequence(
        const SwitchStep* steps,
        int stepCount,
        int rounds = 1)
    {
        MockMultiNetworkManager mgr;
        MockMQTTClient mqtt;
        MQTTConfig config = buildDefaultMQTTConfig();

        int totalPass = 0;

        for (int round = 0; round < rounds; round++) {
            for (int i = 0; i < stepCount; i++) {
                bool result = switchNetworkAndReconnectMQTT(
                    mgr, mqtt, config, steps[i].type, steps[i].adapterSuccess);

                if (steps[i].expectMQTT) {
                    TEST_ASSERT_TRUE_MESSAGE(result,
                        (String("Round ") + round + " Step " + i +
                         ": expected MQTT to connect").c_str());
                    assertMQTTConnected(mqtt);
                } else {
                    TEST_ASSERT_FALSE_MESSAGE(result,
                        (String("Round ") + round + " Step " + i +
                         ": expected MQTT NOT to connect").c_str());
                }
                totalPass++;
            }
        }
        return totalPass;
    }
}

#endif // NETWORK_MQTT_HELPER_H
