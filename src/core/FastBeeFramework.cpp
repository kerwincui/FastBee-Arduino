/**
 * @file FastBeeFramework.cpp
 * @brief FastBee 物联网平台框架核心实现
 * @author kerwincui
 * @copyright FastBee All rights reserved.
 * @date 2025-12-02
 */

#include "core/FastBeeFramework.h"
#include "core/SystemConstants.h"
#include "core/FeatureFlags.h"
#include "core/PeripheralManager.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecManager.h"
#endif
#if FASTBEE_ENABLE_RULE_SCRIPT
#include "core/RuleScriptManager.h"
#endif
#include "systems/LoggerSystem.h"
#include "network/NetworkManager.h"
#include "network/WebConfigManager.h"
#include "network/OTAManager.h"
#include "systems/TaskManager.h"
#include "systems/HealthMonitor.h"
#include "systems/RestartDiagnostics.h"
#include "security/UserManager.h"
#include "security/AuthManager.h"
#include "protocols/ProtocolManager.h"
#include "systems/ConfigStorage.h"
#include "utils/TimeUtils.h"
#include "utils/FileUtils.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>
#include <esp_task_wdt.h>

// 启动期 heap 采样：定位哪个 STEP 吞掉了堆（OOM 排查辅助）
// tag 仅作日志标记，与上一行 LOGGER.infof("[Boot] xxx: ms") 配合使用
#define LOG_BOOT_HEAP(tag) LOGGER.infof("[BootHeap] %s heap=%u maxBlock=%u", \
    (tag), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap())

// 构造函数
FastBeeFramework::FastBeeFramework()
    : systemInitialized(false),
      lastHealthCheck(0) {
}

// 析构函数
FastBeeFramework::~FastBeeFramework() {
    shutdown();
}

// 获取单例实例
// 使用 Meyers' Singleton（局部静态变量），C++11 标准保证线程安全初始化，
// 无需 unique_ptr + mutex 的 DCL 双重检查锁定，消除非 std::atomic 内存序风险。
FastBeeFramework* FastBeeFramework::getInstance() {
    static FastBeeFramework instance;
    return &instance;
}

// 初始化框架
bool FastBeeFramework::initialize() {
    if (systemInitialized) {
        LOG_WARNING("Framework already initialized");
        return true;
    }

    // 记录启动时间戳（用于日志）
    unsigned long startTime = millis();
    unsigned long stepStart;  // 分步骤计时

    ets_printf("\n========================================\n");
    ets_printf("  FastBee IoT Platform Starting...\n");
    ets_printf("  Version: %s (%s)\n", SystemInfo::VERSION, SystemInfo::FIRMWARE_TYPE);
    ets_printf("  Build: %s %s\n", __DATE__, __TIME__);
    ets_printf("========================================\n\n");

    // 步骤1: 初始化配置存储（Logger 之前，只能用 Serial）
    stepStart = millis();
    Serial.println("[STEP1] Initializing Config Storage...");
    if (!ConfigStorage::getInstance().initialize()) {
        Serial.println("[FATAL] Failed to initialize config storage!");
        return false;
    }
    unsigned long configStorageMs = millis() - stepStart;
    Serial.printf("[Boot] ConfigStorage: %lu ms\n", configStorageMs);

#if FASTBEE_ENABLE_STORAGE_CACHE
    // 内存约束：不在启动时预加载配置，仅依赖按需加载+LRU缓存，
    // 避免 5+ 个 JsonDocument 常驻堆内存导致 AsyncWebServer 无法分配请求缓冲区。
#endif

    // 初始化文件系统工具
    stepStart = millis();
    Serial.println("[STEP1] Initializing FileUtils...");
    FileUtils::initialize();
    unsigned long fileUtilsMs = millis() - stepStart;
    Serial.printf("[Boot] FileUtils: %lu ms\n", fileUtilsMs);

    // 步骤2: 初始化日志系统（之后统一用 LOG 宏）
    stepStart = millis();
    Serial.println("[STEP2] Initializing Logger System...");
    if (!LOGGER.initialize()) {
        Serial.println("[FATAL] Failed to initialize logger system!");
        return false;
    }
    LOGGER.setLogLevel(LOG_DEBUG);
    unsigned long loggerMs = millis() - stepStart;

    // 日志系统初始化完成后，补充记录早期启动信息到日志文件
    LOGGER.info("========================================");
    LOGGER.info("  FastBee IoT Platform Boot Log");
    LOGGER.info("========================================");
    LOGGER.infof("Boot started at %lu ms", startTime);
    LOGGER.infof("[Boot] ConfigStorage: %lu ms", configStorageMs);
    LOGGER.infof("[Boot] FileUtils: %lu ms", fileUtilsMs);
    LOGGER.infof("[Boot] Logger: %lu ms", loggerMs);
    LOG_INFO("Logger system initialized and file logging enabled");

    // 步骤2.5: 初始化外设管理器（尽早初始化，确保GPIO输出引脚在WiFi连接前就位）
    // 依赖：LittleFS（STEP1）、Logger（STEP2），无需网络
    stepStart = millis();
    Serial.println("[STEP2.5] Initializing PeripheralManager...");
    if (!PeripheralManager::getInstance().initialize()) {
        Serial.println("[STEP2.5] WARN: Peripheral manager init failed");
    } else {
        Serial.println("[STEP2.5] Peripheral manager OK");
    }
    Serial.printf("[Boot] PeripheralManager: %lu ms\n", millis() - stepStart);
    LOG_BOOT_HEAP("PeripheralManager");

    // 步骤3: 创建HTTP服务器
    stepStart = millis();
    Serial.println("[STEP3] Creating HTTP server...");
    server.reset(new AsyncWebServer(80));
    if (!server) {
        Serial.println("[STEP3] FATAL: Failed to create HTTP server");
        return false;
    }
    Serial.printf("[Boot] AsyncWebServer: %lu ms\n", millis() - stepStart);

#if FASTBEE_WEB_START_EARLY
    // ---- 提前启动模式：Web服务器在WiFi连接前启动（AP模式直接可用）----

    // 步骤4-E: 初始化用户管理器（提前，Web服务需要）
    stepStart = millis();
    Serial.println("[STEP4-E] Initializing UserManager...");
    userManager.reset(new UserManager());
    if (!userManager || !userManager->initialize()) {
        Serial.println("[STEP4-E] FATAL: Failed to initialize user manager");
        return false;
    }
    Serial.println("[STEP4-E] User manager OK");

    // 步骤5-E: 初始化认证管理器
    Serial.println("[STEP5-E] Initializing AuthManager...");
    authManager.reset(new AuthManager(userManager.get(), nullptr));
    if (!authManager || !authManager->initialize()) {
        Serial.println("[STEP5-E] FATAL: Failed to initialize auth manager!");
        return false;
    }
    Serial.println("[STEP5-E] Auth manager OK");
    Serial.printf("[Boot] User/AuthManager (early): %lu ms\n", millis() - stepStart);

    // 步骤6-E: 初始化 Web配置管理器（仅注册路由，暂不调用 server->begin()）
    // 注意：server->begin() 需要 LwIP TCP/IP 栈就绪，必须先等 NetworkManager 初始化 WiFi，
    //       否则会触发 lwip tcpip_api_call "Invalid mbox" 断言导致重启。
    stepStart = millis();
    Serial.println("[STEP6-E] Initializing WebConfigManager (routes only)...");
    webConfig.reset(new WebConfigManager(server.get(), authManager.get(), userManager.get()));
    if (!webConfig || !webConfig->initialize()) {
        Serial.println("[STEP6-E] FATAL: Failed to init web config manager");
        return false;
    }
    // NetworkManager 尚未创建，暂不注入，在步骤7-E中注入
    Serial.printf("[Boot] WebConfigManager routes: %lu ms\n", millis() - stepStart);
    LOG_BOOT_HEAP("WebConfigManager-routes");

    // 步骤7-E: 初始化网络管理器（启动 WiFi，带起 LwIP TCP/IP 栈）
    stepStart = millis();
    Serial.println("[STEP7-E] Creating NetworkManager...");
    network.reset(new FBNetworkManager(server.get()));
    if (!network) {
        Serial.println("[STEP7-E] FATAL: Failed to create network manager");
        return false;
    }

    // 初始化网络（会从 network.json 加载配置，可能阻塞等待WiFi连接）
    if (!network->initialize()) {
        Serial.println("[STEP7-E] WARN: Network initialization returned false");
    }

    // 回注 NetworkManager 到 WebConfigManager
    webConfig->setNetworkManager(network.get());

    unsigned long networkMs = millis() - stepStart;
    // 检查网络状态
#if FASTBEE_ENABLE_ETHERNET
    if (network->getNetworkType() == NetworkType::NET_ETHERNET && network->getEthernetAdapter()) {
        auto* eth = network->getEthernetAdapter();
        if (eth->isConnected()) {
            Serial.printf("[STEP7-E] Ethernet Connected! IP: %s\n", eth->localIP().toString().c_str());
        } else {
            Serial.println("[STEP7-E] WARN: Ethernet link down");
        }
    } else
#endif
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[STEP7-E] WiFi Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else if (WiFi.softAPIP() != IPAddress(0,0,0,0)) {
        Serial.printf("[STEP7-E] AP Mode IP: %s\n", WiFi.softAPIP().toString().c_str());
    } else {
        Serial.println("[STEP7-E] WARN: Network not connected");
    }
    Serial.printf("[Boot] NetworkManager: %lu ms\n", networkMs);
    LOG_BOOT_HEAP("NetworkManager");

    // 步骤7.5-E: 启动 HTTP 服务器（此时 TCP/IP 栈已就绪，AP 或 STA 模式均可用）
    stepStart = millis();
    Serial.println("[STEP7.5-E] Starting web server...");
    if (!webConfig->start()) {
        Serial.println("[STEP7.5-E] FATAL: Failed to start web server");
        return false;
    }
    Serial.printf("[Boot] WebServer start: %lu ms\n", millis() - stepStart);

#else  // !FASTBEE_WEB_START_EARLY
    // ---- 常规模式：WiFi连接完成后再启动Web服务器 ----

    // 步骤 4: 初始化网络管理器（可能阻塞：STA模式下 connectToWiFiBlocking 最长等待 connectTimeout ms）
    stepStart = millis();
    Serial.println("[STEP4] Creating NetworkManager...");
    network.reset(new FBNetworkManager(server.get()));
    if (!network) {
        Serial.println("[STEP4] FATAL: Failed to create network manager");
        return false;
    }

    // 初始化网络（会从 network.json 加载配置）
    if (!network->initialize()) {
        Serial.println("[STEP4] WARN: Network initialization returned false");
    }

    unsigned long networkMs = millis() - stepStart;
    // 检查网络状态
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[STEP4] WiFi Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else if (WiFi.softAPIP() != IPAddress(0,0,0,0)) {
        Serial.printf("[STEP4] AP Mode IP: %s\n", WiFi.softAPIP().toString().c_str());
    } else {
        Serial.println("[STEP4] WARN: Network not connected");
    }
    Serial.printf("[Boot] NetworkManager: %lu ms\n", networkMs);

    // 步骤5: 初始化用户管理器
    stepStart = millis();
    Serial.println("[STEP5] Initializing UserManager...");
    userManager.reset(new UserManager());
    if (!userManager || !userManager->initialize()) {
        Serial.println("[STEP5] FATAL: Failed to initialize user manager");
        return false;
    }
    Serial.println("[STEP5] User manager OK");

    // 步骤6: 初始化认证管理器
    Serial.println("[STEP6] Initializing AuthManager...");
    authManager.reset(new AuthManager(userManager.get(), nullptr));
    if (!authManager || !authManager->initialize()) {
        Serial.println("[STEP6] FATAL: Failed to initialize auth manager!");
        return false;
    }
    Serial.println("[STEP6] Auth manager OK");
    Serial.printf("[Boot] User/AuthManager: %lu ms\n", millis() - stepStart);

    // 步骤 7: 初始化 Web配置管理器
    stepStart = millis();
    Serial.println("[STEP7] Initializing WebConfigManager...");
    webConfig.reset(new WebConfigManager(server.get(), authManager.get(), userManager.get()));
    if (!webConfig || !webConfig->initialize()) {
        Serial.println("[STEP7] FATAL: Failed to init web config manager");
        return false;
    }
    webConfig->setNetworkManager(network.get());
    // 启动 HTTP 服务器（监听端口 80）
    if (!webConfig->start()) {
        Serial.println("[STEP7] FATAL: Failed to start web server");
        return false;
    }
    Serial.printf("[Boot] WebConfigManager: %lu ms\n", millis() - stepStart);

#endif  // FASTBEE_WEB_START_EARLY

#if FASTBEE_ENABLE_OTA
    // 步骤8: 初始化OTA管理器
    stepStart = millis();
    Serial.println("[STEP8] Initializing OTAManager...");
    ota.reset(new OTAManager(server.get()));
    if (!ota || !ota->initialize()) {
        Serial.println("[STEP8] FATAL: Failed to initialize OTA manager");
        return false;
    }
    Serial.printf("[Boot] OTAManager: %lu ms\n", millis() - stepStart);
#else
    Serial.println("[STEP8] OTAManager disabled by FASTBEE_ENABLE_OTA=0");
#endif

    // 步骤9: 初始化任务管理器
    stepStart = millis();
    Serial.println("[STEP9] Initializing TaskManager...");
    taskManager.reset(new TaskManager());
    if (!taskManager || !taskManager->initialize()) {
        Serial.println("[STEP9] FATAL: Failed to initialize task manager");
        return false;
    }
    Serial.printf("[Boot] TaskManager: %lu ms\n", millis() - stepStart);

    // 步骤10: 初始化健康监控器
    stepStart = millis();
    Serial.println("[STEP10] Initializing HealthMonitor...");
    healthMonitor.reset(new HealthMonitor());
    if (!healthMonitor || !healthMonitor->initialize()) {
        Serial.println("[STEP10] FATAL: Failed to initialize health monitor");
        return false;
    }
    Serial.printf("[Boot] HealthMonitor: %lu ms\n", millis() - stepStart);

    // 步骤10.5: 确保设备身份标识（deviceId / MQTT clientId 空值自动生成并写回 JSON 配置）
    // 依赖：LittleFS（STEP1）、Logger（STEP2）、WiFi 栏已起（STEP4/STEP7-E 后 MAC 可读）
    // 必须在 ProtocolManager (STEP11) 之前，确保 MQTTClient loadMqttConfig() 读取到已回填的配置
    stepStart = millis();
    Serial.println("[STEP10.5] Ensuring device identity...");
    ensureDeviceIdentity();
    Serial.printf("[Boot] ensureDeviceIdentity: %lu ms\n", millis() - stepStart);

    // 步骤11: 初始化协议管理器
    stepStart = millis();
    Serial.println("[STEP11] Initializing ProtocolManager...");
    protocolManager.reset(new ProtocolManager());
    if (!protocolManager || !protocolManager->initialize()) {
        Serial.println("[STEP11] FATAL: Failed to initialize protocol manager");
        return false;
    }
    Serial.printf("[Boot] ProtocolManager: %lu ms\n", millis() - stepStart);

    // 步骤11.1: 启动协议客户端（MQTT、Modbus 等）
    // ProtocolManager::initialize() 只设置标志位，不创建协议客户端
    // 必须显式调用 restartMQTTDeferred() / restartModbus() 来加载配置并创建客户端
#if FASTBEE_ENABLE_MQTT
    {
        unsigned long mqttStart = millis();
        ets_printf("[STEP11.1] Starting MQTT client...\n");
        if (protocolManager->restartMQTTDeferred()) {
            ets_printf("[Boot] MQTT client created: %lu ms\n", millis() - mqttStart);
        } else {
            ets_printf("[STEP11.1] WARN: MQTT deferred restart failed (will retry on API query)\n");
        }
    }
#endif
#if FASTBEE_ENABLE_MODBUS
    {
        unsigned long modbusStart = millis();
        ets_printf("[STEP11.2] Starting Modbus handler...\n");
        if (protocolManager->restartModbus()) {
            ets_printf("[Boot] Modbus handler created: %lu ms\n", millis() - modbusStart);
        } else {
            ets_printf("[STEP11.2] WARN: Modbus restart failed\n");
        }
    }
#endif

    // 注入 ProtocolManager 到 WebConfig，供 API 路由访问
    if (webConfig && protocolManager) {
        webConfig->setProtocolManager(protocolManager.get());
    }

    // 注入 TX/RX 计数回调：协议层消息收发时递增网络层计数器
    if (network && protocolManager) {
        FBNetworkManager* netMgr = network.get();
        protocolManager->setTxCallback([netMgr]() { netMgr->incrementTxCount(); });
        protocolManager->setRxCallback([netMgr]() { netMgr->incrementRxCount(); });
    }

    // 步骤11.5: 初始化外设执行管理器
#if FASTBEE_ENABLE_PERIPH_EXEC
    stepStart = millis();
    Serial.println("[STEP11.5] Initializing PeriphExecManager...");
    if (!PeriphExecManager::getInstance().initialize()) {
        Serial.println("[STEP11.5] WARN: Periph exec manager init failed");
    } else {
        Serial.println("[STEP11.5] Periph exec manager OK");
    }
    Serial.printf("[Boot] PeriphExecManager: %lu ms\n", millis() - stepStart);
    LOG_BOOT_HEAP("PeriphExecManager");

    // 注入协议层回调到 PeriphExecManager（解耦 core → protocols 依赖）
    if (protocolManager) {
        auto& pem = PeriphExecManager::getInstance();
        ProtocolManager* pm = protocolManager.get();

        // MQTT 连接状态检查回调
        pem.setMqttIsConnectedCallback([pm]() -> bool {
            MQTTClient* mqtt = pm->getMQTTClient();
            return mqtt && mqtt->getIsConnected();
        });

        // MQTT 队列上报数据回调
        pem.setMqttQueueReportCallback([pm](const String& reportData) -> bool {
            MQTTClient* mqtt = pm->getMQTTClient();
            if (!mqtt) return false;
            return mqtt->queueReportData(reportData);
        });

        // MQTT 设备事件上报回调（DEVICE_EVENT 主题）
        pem.setMqttEventPublishCallback([pm](const String& eventId, const String& eventName, const String& eventData) -> bool {
            MQTTClient* mqtt = pm->getMQTTClient();
            if (!mqtt) return false;
            return mqtt->publishDeviceEvent(eventId, eventName, eventData);
        });

#if FASTBEE_ENABLE_MODBUS
        // Modbus sensorId 构建回调
        pem.setModbusBuildSensorIdCallback([pm](uint8_t deviceIndex, uint16_t channel) -> String {
            ModbusHandler* modbus = pm->getModbusHandler();
            if (!modbus) return String();
            return modbus->buildSensorId(deviceIndex, channel);
        });

        // Modbus 未匹配 sensorId 直接控制回调
        pem.setModbusDirectControlCallback([pm](const String& sensorId, const String& value, JsonArray& reportArr) -> bool {
            ModbusHandler* modbus = pm->getModbusHandler();
            if (!modbus) return false;

            ModbusHandler::SensorIdMatch match;
            if (!modbus->findBySensorIdEx(sensorId, match)) return false;

            const ModbusSubDevice& dev = modbus->getSubDevice(match.deviceIndex);
            char periphIdBuf[16];
            snprintf(periphIdBuf, sizeof(periphIdBuf), "modbus:%d", match.deviceIndex);
            String periphId(periphIdBuf);
            bool writeOk = false;
            bool state = false;
            String devType(dev.deviceType);
            String reportValue = value;
            PeripheralManager& periMgr = PeripheralManager::getInstance();

            auto tryParseBool = [](const String& v, bool& out) -> bool {
                if (v == "1" || v.equalsIgnoreCase("true") || v.equalsIgnoreCase("on")) { out = true; return true; }
                if (v == "0" || v.equalsIgnoreCase("false") || v.equalsIgnoreCase("off")) { out = false; return true; }
                return false;
            };

            if (match.action.isEmpty()) {
                if (devType == "relay") {
                    if (!tryParseBool(value, state)) state = false;
                    bool val = dev.ncMode ? !state : state;
                    uint16_t addr = dev.coilBase + match.channel;
                    if (dev.controlProtocol == 0) {
                        writeOk = periMgr.writeModbusCoil(periphId, addr, val);
                    } else {
                        writeOk = periMgr.writeModbusReg(periphId, addr, val ? 1 : 0);
                    }
                    reportValue = state ? "1" : "0";
                } else if (devType == "pwm") {
                    writeOk = periMgr.writeModbusReg(periphId, dev.pwmRegBase + match.channel, (uint16_t)value.toInt());
                } else if (devType == "pid") {
                    writeOk = periMgr.writeModbusReg(periphId, dev.pidAddrs[1], (uint16_t)value.toInt());
                } else if (devType == "motor") {
                    writeOk = periMgr.writeModbusReg(periphId, dev.motorRegs[0], (uint16_t)value.toInt());
                }
            } else if (match.action == "all") {
                if (devType == "relay") {
                    if (!tryParseBool(value, state)) state = false;
                    bool val = dev.ncMode ? !state : state;
                    if (dev.batchRegister > 0) {
                        if (dev.batchRegType == 1) {
                            writeOk = periMgr.writeModbusReg(periphId, dev.batchRegister, val ? 1 : 0);
                        } else {
                            uint16_t bitmask = (dev.channelCount >= 16) ? 0xFFFF : (uint16_t)((1 << dev.channelCount) - 1);
                            writeOk = periMgr.writeModbusReg(periphId, dev.batchRegister, val ? bitmask : 0);
                        }
                    } else {
                        writeOk = true;
                        for (uint8_t ch = 0; ch < dev.channelCount; ch++) {
                            uint16_t addr = dev.coilBase + ch;
                            bool ok = (dev.controlProtocol == 0)
                                ? periMgr.writeModbusCoil(periphId, addr, val)
                                : periMgr.writeModbusReg(periphId, addr, val ? 1 : 0);
                            if (!ok) writeOk = false;
                        }
                    }
                    reportValue = state ? "1" : "0";
                    // 批量操作：上报每个通道独立状态
                    String stateStr = state ? "1" : "0";
                    String remarkText = state ? "\xE5\xB7\xB2\xE6\x89\x93\xE5\xBC\x80" : "\xE5\xB7\xB2\xE5\x85\xB3\xE9\x97\xAD";
                    for (uint8_t ch = 0; ch < dev.channelCount; ch++) {
                        String chSid = modbus->buildSensorId(match.deviceIndex, ch);
                        if (chSid.isEmpty()) {
                            char chBuf[32];
                            snprintf(chBuf, sizeof(chBuf), "%s_ch%d", sensorId.c_str(), ch);
                            chSid = chBuf;
                        }
                        JsonObject chReport = reportArr.add<JsonObject>();
                        chReport["id"] = chSid;
                        chReport["value"] = stateStr;
                        chReport["remark"] = remarkText;
                    }
                    // 触发 mc: 事件（使用 c/a 格式与规则匹配逻辑一致）
                    char mcIdBuf[16];
                    snprintf(mcIdBuf, sizeof(mcIdBuf), "mc:%d", match.deviceIndex);
                    char mcDataBuf[160];
                    const char* allAct = state ? "on" : "off";
                    snprintf(mcDataBuf, sizeof(mcDataBuf), "{\"d\":%d,\"c\":%d,\"a\":\"%s\"}",
                        match.deviceIndex, match.channel, allAct);
                    PeriphExecManager::getInstance().triggerEventById(String(mcIdBuf), String(mcDataBuf));
                    return true;  // 批量已自行构建报告
                } else if (devType == "pwm") {
                    uint16_t regVal = (uint16_t)value.toInt();
                    writeOk = true;
                    for (uint8_t ch = 0; ch < dev.channelCount; ch++) {
                        if (!periMgr.writeModbusReg(periphId, dev.pwmRegBase + ch, regVal)) writeOk = false;
                    }
                } else if (devType == "pid") {
                    writeOk = periMgr.writeModbusReg(periphId, dev.pidAddrs[1], (uint16_t)value.toInt());
                }
            } else if (devType == "motor") {
                if (match.action == "oper") {
                    int operVal = value.toInt();
                    int regIdx = (operVal == 1) ? 0 : (operVal == 2) ? 1 : 2;
                    writeOk = periMgr.writeModbusReg(periphId, dev.motorRegs[regIdx], 1);
                } else {
                    writeOk = periMgr.writeModbusReg(periphId, dev.motorRegs[match.channel], (uint16_t)value.toInt());
                }
            } else if (devType == "pid") {
                if (match.action == "pv" || match.action == "out") {
                    writeOk = false;
                } else {
                    writeOk = periMgr.writeModbusReg(periphId, dev.pidAddrs[match.channel], (uint16_t)value.toInt());
                }
            }

            // 控制成功后触发 mc: 事件（使用 c/a 格式与规则匹配逻辑一致）
            if (writeOk) {
                char mcIdBuf[16];
                snprintf(mcIdBuf, sizeof(mcIdBuf), "mc:%d", match.deviceIndex);
                // 根据设备类型和动作构建事件数据
                String devTypeStr(dev.deviceType);
                String evAct;
                if (devTypeStr == "relay") {
                    evAct = (value == "1" || value.equalsIgnoreCase("on") || value.equalsIgnoreCase("true")) ? "on" : "off";
                } else if (devTypeStr == "pwm") {
                    evAct = "pwm";
                } else if (devTypeStr == "pid") {
                    evAct = "pid";
                } else if (devTypeStr == "motor") {
                    if (match.action == "oper") {
                        int operVal = value.toInt();
                        evAct = (operVal == 1) ? "forward" : (operVal == 2) ? "reverse" : "stop";
                    } else {
                        evAct = "motor";
                    }
                } else {
                    evAct = match.action.isEmpty() ? "on" : match.action.c_str();
                }
                char mcDataBuf[160];
                snprintf(mcDataBuf, sizeof(mcDataBuf), "{\"d\":%d,\"c\":%d,\"a\":\"%s\"}",
                    match.deviceIndex, match.channel, evAct.c_str());
                PeriphExecManager::getInstance().triggerEventById(String(mcIdBuf), String(mcDataBuf));
            }

            JsonObject reportItem = reportArr.add<JsonObject>();
            reportItem["id"] = sensorId;
            reportItem["value"] = reportValue;
            reportItem["remark"] = writeOk ? "direct_modbus" : "modbus_error";
            return true;
        });

        // Modbus 动态事件列表回调
        pem.setModbusDynamicEventsCallback([pm](JsonArray& arr) {
            ModbusHandler* modbus = pm->getModbusHandler();
            if (!modbus || modbus->getMode() != MODBUS_MASTER) return;
            uint8_t devCount = modbus->getSubDeviceCount();
            for (uint8_t i = 0; i < devCount; i++) {
                const ModbusSubDevice& dev = modbus->getSubDevice(i);
                if (!dev.enabled) continue;
                String devType = String(dev.deviceType);
                if (devType == "relay" || devType == "pwm" || devType == "pid" || devType == "motor") {
                    JsonObject obj = arr.add<JsonObject>();
                    obj["id"] = "mc:" + String(i);
                    obj["name"] = String(dev.name);
                    obj["category"] = "Modbus\u5B50\u8BBE\u5907";
                    obj["deviceType"] = devType;
                    obj["channelCount"] = dev.channelCount;
                    obj["isDynamic"] = true;
                }
            }
        });
#endif  // FASTBEE_ENABLE_MODBUS

        Serial.println("[STEP11.5] Protocol callbacks injected into PeriphExecManager");
    }
#endif

    // 步骤11.6: 初始化规则脚本管理器
#if FASTBEE_ENABLE_RULE_SCRIPT
    stepStart = millis();
    Serial.println("[STEP11.6] Initializing RuleScriptManager...");
    if (!RuleScriptManager::getInstance().initialize()) {
        Serial.println("[STEP11.6] WARN: Rule script manager init failed");
    } else {
        Serial.println("[STEP11.6] Rule script manager OK");
    }
    Serial.printf("[Boot] RuleScriptManager: %lu ms\n", millis() - stepStart);
    LOG_BOOT_HEAP("RuleScriptManager");
#endif

    // 注入 MQTT 消息回调：消息到达时匹配外设执行
    if (protocolManager) {
        protocolManager->setMessageCallback([](ProtocolType type, const String& topic, const String& msg) {
            if (type == ProtocolType::MQTT) {
#if FASTBEE_ENABLE_PERIPH_EXEC
                PeriphExecManager::getInstance().handleMqttMessage(topic, msg);
#endif
            }
        });
    }

    // 步骤12: 添加系统任务
    stepStart = millis();
    Serial.println("[STEP12] Adding system tasks...");
    if (!addSystemTasks()) {
        Serial.println("[STEP12] WARN: Failed to add some system tasks");
    }
    Serial.printf("[Boot] addSystemTasks: %lu ms\n", millis() - stepStart);
    LOG_BOOT_HEAP("addSystemTasks");

    // 步骤13: 检查系统健康状态
    Serial.println("[STEP13] Checking system health...");
    if (!healthMonitor->isSystemHealthy()) {
        char buf[160];
        healthMonitor->getHealthReport(buf, sizeof(buf));
        Serial.printf("[STEP13] WARN: %s\n", buf);
    }
    Serial.println("[STEP13] Health check completed");

    // 计算启动耗时
    unsigned long bootTime = millis() - startTime;

    systemInitialized = true;

    // 输出 Boot Performance Report
    ets_printf("[Boot] === Performance Report ===\n");
    ets_printf("[Boot] Total boot time: %lu ms\n", bootTime);
    if (networkMs > 1000) {
        ets_printf("[Boot] WARNING: NetworkManager took %lu ms (may block on WiFi connect)\n", networkMs);
    }
    ets_printf("[Boot] === End Report ===\n");

    // 将启动耗时记录到 HealthMonitor
#if FASTBEE_ENABLE_HEALTH_MONITOR
    if (healthMonitor) {
        healthMonitor->setBootTime(bootTime);
    }
#endif

    Serial.println("========================================");
    ets_printf("  System initialized in %lu ms\n", bootTime);
    ets_printf("========================================\n");
    ets_printf("[BOOT] Platform ready! Heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
    ets_printf("========================================\n");
    ets_printf("  FastBee IoT Platform Ready!\n");
    ets_printf("========================================\n");

#if FASTBEE_ENABLE_ETHERNET
    if (network && network->getNetworkType() == NetworkType::NET_ETHERNET && network->getEthernetAdapter()) {
        auto* eth = network->getEthernetAdapter();
        ets_printf("Mode: Ethernet (W5500)\n");
        ets_printf("Ethernet IP: %s\n", eth->localIP().toString().c_str());
        ets_printf("Access URL: http://%s\n", eth->localIP().toString().c_str());
        if (WiFi.softAPIP() != IPAddress(0,0,0,0)) {
            ets_printf("Config AP: %s (IP: %s)\n", WiFi.softAPSSID().c_str(), WiFi.softAPIP().toString().c_str());
        }
        if (network->getConfig().enableMDNS) {
            ets_printf("mDNS URL: http://%s.local\n", network->getConfig().customDomain.c_str());
        }
    } else
#endif
    if (WiFi.status() == WL_CONNECTED) {
        ets_printf("Mode: STA (WiFi Client)\n");
        ets_printf("SSID: %s\n", WiFi.SSID().c_str());
        ets_printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
        ets_printf("Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
        ets_printf("DNS: %s\n", WiFi.dnsIP(0).toString().c_str());
        ets_printf("RSSI: %d dBm\n", WiFi.RSSI());
        ets_printf("Access URL: http://%s\n", WiFi.localIP().toString().c_str());
        ets_printf("mDNS URL: http://fastbee.local\n");
    } else if (WiFi.softAPIP() != IPAddress(0,0,0,0)) {
        ets_printf("Mode: AP (Access Point)\n");
        String apSSID = NetConst::DEFAULT_AP_SSID;
        String apPass = NetConst::DEFAULT_AP_PASSWORD;
        if (network) {
            WiFiConfig cfg = network->getConfig();
            if (cfg.apSSID.length() > 0) apSSID = cfg.apSSID;
            if (cfg.apPassword.length() > 0) apPass = cfg.apPassword;
        }
        ets_printf("WiFi Name: %s\n", apSSID.c_str());
        ets_printf("WiFi Pass: %s\n", apPass.c_str());
        ets_printf("IP Address: %s\n", WiFi.softAPIP().toString().c_str());
        ets_printf("Setup URL: http://%s/setup\n", WiFi.softAPIP().toString().c_str());
    } else {
        ets_printf("Network: Not connected\n");
    }

    ets_printf("----------------------------------------\n");
    ets_printf("Default Login: admin / admin123\n");
    ets_printf("========================================\n");

    return true;
}

// 添加系统任务
bool FastBeeFramework::addSystemTasks() {
    if (!taskManager) {
        LOG_ERROR("Task manager not initialized");
        return false;
    }

    // 健康检查任务（每30秒）
#if FASTBEE_ENABLE_HEALTH_MONITOR
    if (!taskManager->addTask("health_check", [](void* param) {
        FastBeeFramework* framework = static_cast<FastBeeFramework*>(param);
        if (!framework || !framework->healthMonitor) {
            LOG_DEBUG("health_check: framework or healthMonitor null");
            return;
        }

        framework->healthMonitor->update();

        // 每5分钟输出一次健康报告
        static unsigned long lastReport = 0;
        unsigned long now = millis();
        if (now - lastReport > 300000UL) {
            lastReport = now;
            char buf[160];
            framework->healthMonitor->getHealthReport(buf, sizeof(buf));
            if (!framework->healthMonitor->isSystemHealthy()) {
                LOG_WARNING(buf);
            } else {
                LOG_DEBUG(buf);
            }
        }
    }, this, 5000, TaskPriority::PRIORITY_HIGH)) {
        LOG_ERROR("Failed to add health check task");
        return false;
    }
#endif

    // Web客户端处理任务（每100ms）
#if FASTBEE_ENABLE_WEB_SERVER
    if (!taskManager->addTask("web_client_handler", [](void* param) {
        // 空任务，由AsyncWebServer自动处理
        // 这里可以添加额外的Web客户端处理逻辑
    }, nullptr, 100)) {
        LOG_WARNING("Failed to add web client handler task");
        // 这个任务不是关键的，所以不返回错误
    }

    // 网络状态更新任务（每5秒）
    if (!taskManager->addTask("network_update", [](void* param) {
        FastBeeFramework* framework = (FastBeeFramework*)param;
        if (framework && framework->network) {
            framework->network->update();

            // 标记网络已连接，触发NTP同步任务（仅执行一次）
            // 支持 WiFi STA / 以太网 / 4G 等所有联网方式
            bool networkReady = framework->network->isNetworkConnected();
            if (!framework->ntpSynced && !framework->ntpSyncPending && !framework->ntpSyncStarted &&
                framework->ntpRetryCount == 0 && networkReady) {
                static unsigned long netConnectedTime = 0;
                if (netConnectedTime == 0) {
                    netConnectedTime = millis();
                } else if (millis() - netConnectedTime > 3000) {
                    // 网络连接稳定3秒后标记需要同步
                    LOG_INFO("[NTP] Network connected, NTP sync scheduled");
                    framework->ntpSyncPending = true;
                }
            }

            // WiFi连接后自动启动协议（MQTT + Modbus，仅执行一次）
            // 合并读取 protocol.json，减少一次 JsonDocument 分配（~8KB 栈空间）
            static unsigned long protocolWaitStart = 0;
            static bool lastProtocolNetworkReady = false;
            static NetworkType lastProtocolNetworkType = NetworkType::NET_WIFI;
            bool protocolNetworkReady = framework->network->isNetworkConnected();
            NetworkType protocolNetworkType = framework->network->getNetworkType();

            if (!protocolNetworkReady) {
                protocolWaitStart = 0;
                lastProtocolNetworkReady = false;
            } else if (!lastProtocolNetworkReady || protocolNetworkType != lastProtocolNetworkType) {
                framework->mqttAutoStarted = false;
                protocolWaitStart = millis();
                lastProtocolNetworkReady = true;
                lastProtocolNetworkType = protocolNetworkType;
            }

            if ((!framework->mqttAutoStarted || !framework->modbusAutoStarted)
                && protocolNetworkReady && framework->protocolManager) {
                if (protocolWaitStart == 0) {
                    protocolWaitStart = millis();
                } else if (millis() - protocolWaitStart > 5000) {
                    // WiFi稳定5秒后，一次性读取 protocol.json 并启动所有协议
                    bool shouldStartMqtt = !framework->mqttAutoStarted;
                    bool shouldStartModbus = !framework->modbusAutoStarted;
                    framework->mqttAutoStarted = true;
                    framework->modbusAutoStarted = true;
                    LOG_INFO("[Protocol] Network stable, auto-starting protocols...");

                    // 堆保护：内部堆低于安全阈值时跳过协议启动，防止 new/malloc 失败触发 abort()
                    uint32_t protoStartHeap = ESP.getFreeHeap();
                    uint32_t protoStartMaxBlock = ESP.getMaxAllocHeap();
                    if (protoStartHeap < 30000 || protoStartMaxBlock < 8192) {
                        LOG_WARNINGF("[Protocol] Heap too low for protocol start, deferring (heap=%lu maxBlock=%lu)",
                                     (unsigned long)protoStartHeap, (unsigned long)protoStartMaxBlock);
                        // 允许下次重试
                        framework->mqttAutoStarted = !shouldStartMqtt;
                        framework->modbusAutoStarted = !shouldStartModbus;
                    } else {

                    // 单次读取 protocol.json，同时检查 MQTT 和 Modbus 配置
#if FASTBEE_ENABLE_MQTT
                    bool mqttEnabled = false;
#endif
#if FASTBEE_ENABLE_MODBUS
                    bool modbusEnabled = false;
#endif
                    if (LittleFS.exists("/config/protocol.json")) {
                        File f = LittleFS.open("/config/protocol.json", "r");
                        if (f) {
                            FastBeeJsonDocLarge doc;
                            DeserializationError err = deserializeJson(doc, f);
                            f.close();
                            if (!err) {
#if FASTBEE_ENABLE_MQTT
                                mqttEnabled = doc["mqtt"]["enabled"].as<bool>();
#endif
#if FASTBEE_ENABLE_MODBUS
                                modbusEnabled = doc["modbusRtu"]["enabled"].as<bool>();
#endif
                            }
                        }
                    }  // doc 在此处销毁，释放 8KB 栈空间

                    // 启动 MQTT（延迟连接：仅 begin，由 reconnectTask 30s 后发起首次连接）
                    // 避免 boot 期同步 connect() 抢占资源导致 web/SSE 不可访问
#if FASTBEE_ENABLE_MQTT
                    if (shouldStartMqtt && mqttEnabled) {
                        LOG_INFO("[MQTT] MQTT enabled, auto-starting (deferred connect)...");
                        framework->protocolManager->restartMQTTDeferred();
                    } else if (!mqttEnabled) {
                        LOG_INFO("[MQTT] MQTT not enabled in config, skipping auto-start");
                    }
#endif

                    // 启动 Modbus
#if FASTBEE_ENABLE_MODBUS
                    if (shouldStartModbus && modbusEnabled) {
                        LOG_INFO("[Modbus] Modbus enabled, auto-starting...");
                        framework->protocolManager->restartModbus();
                    } else if (!modbusEnabled) {
                        LOG_INFO("[Modbus] Modbus not enabled in config, skipping auto-start");
                    }
#endif
                    } // end of heap-safe else block
                }
            }
        }
    }, this, 5000)) {
        LOG_WARNING("Failed to add network update task");
    }

    // NTP同步任务（每10秒检查一次）
    if (!taskManager->addTask("ntp_sync", [](void* param) {
        FastBeeFramework* framework = (FastBeeFramework*)param;
        if (!framework) return;

        // 检查是否需要启动同步（支持所有联网方式）
        if (framework->ntpSyncPending && framework->network && framework->network->isNetworkConnected()) {
            framework->ntpSyncPending = false;
            LOG_INFO("[NTP] Starting NTP sync...");
            framework->syncTimeFromConfig();
            framework->ntpSyncStarted = true;  // 标记已启动同步
            return;  // 等待下一次任务检查结果
        }

        // 检查同步结果（同步启动后）
        if (framework->ntpSyncStarted) {
            struct tm timeinfo;
            if (getLocalTime(&timeinfo, 100) && timeinfo.tm_year >= 100) {
                // 同步成功，确保时区设置生效
                tzset();
                // 重新获取本地时间（应用时区后）
                getLocalTime(&timeinfo, 0);
                // 同步成功
                char timeStr[32];
                strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
                LOG_INFOF("[NTP] Sync successful: %s", timeStr);
                framework->ntpSyncStarted = false;
                framework->ntpSynced = true;
                framework->ntpRetryCount = 0;
#if FASTBEE_ENABLE_PERIPH_EXEC
                PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_NTP_SYNCED, timeStr);
#endif
            } else {
                // 同步失败，重试
                framework->ntpRetryCount++;
                if (framework->ntpRetryCount < 10) {
                    LOG_WARNINGF("[NTP] Sync failed (attempt %d/10), will retry", framework->ntpRetryCount);
                    framework->ntpSyncPending = true;  // 标记需要重试
                } else {
                    LOG_ERROR("[NTP] Sync failed after 10 attempts, giving up");
                    framework->ntpSyncStarted = false;
                }
            }
        }
    }, this, 10000)) {
        LOG_WARNING("Failed to add NTP sync task");
    }

    // 内存监控任务（每60秒）
    if (!taskManager->addTask("memory_monitor", [](void* param) {
        static uint32_t lastHeap = 0;
        uint32_t currentHeap = ESP.getFreeHeap();

        if (lastHeap > 0) {
            int32_t diff = currentHeap - lastHeap;
            if (abs(diff) > 1024) { // 变化超过1KB时记录
                static char buffer[100];
                snprintf(buffer, sizeof(buffer), "Memory: %luKB free (%+ld bytes)",
                         currentHeap / 1024, diff);
                LOG_DEBUG(buffer);
            }
        }
        lastHeap = currentHeap;
    }, nullptr, 60000)) {
        LOG_WARNING("Failed to add memory monitor task");
    }

    // Web服务器维护任务（每5分钟）
    if (!taskManager->addTask("web_maintenance", [](void* param) {
        FastBeeFramework* framework = static_cast<FastBeeFramework*>(param);
        if (!framework || !framework->webConfig) return;

        framework->webConfig->performMaintenance();
    }, this, 1000, TaskPriority::PRIORITY_HIGH)) {
        LOG_WARNING("Failed to add web maintenance task");
    }
#endif

#if FASTBEE_ENABLE_PERIPH_EXEC
    // 外设执行定时器检查任务（每1秒）
    if (!taskManager->addTask("periph_exec_timer", [](void* param) {
        PeriphExecManager::getInstance().checkTimers();
    }, nullptr, 1000)) {
        LOG_WARNING("Failed to add periph exec timer task");
    }

    // 设备触发轮询检查任务（每200ms）
    if (!taskManager->addTask("periph_device_trigger", [](void* param) {
        // 设备触发检测已移除，统一使用触发事件
    }, nullptr, 200)) {
        LOG_WARNING("Failed to add periph device trigger task");
    }

    // 按键事件检测任务（每100ms）- 优先级 HIGH 确保 MEMGUARD SEVERE 时仍执行
    // 注意：50ms间隔过于激进，可能导致执行超时和看门狗复位
    if (!taskManager->addTask("button_event_check", [](void* param) {
        PeriphExecManager::getInstance().checkButtonEvents();
    }, nullptr, 100, TaskPriority::PRIORITY_HIGH)) {
        LOG_WARNING("Failed to add button event check task");
    }
#endif

    // 协议处理任务（每100ms）— 维持MQTT心跳和消息收发
    if (!taskManager->addTask("protocol_handle", [](void* param) {
        FastBeeFramework* framework = static_cast<FastBeeFramework*>(param);
        if (framework && framework->protocolManager) {
            framework->protocolManager->handle();
        }
    }, this, 100)) {
        LOG_WARNING("Failed to add protocol handle task");
    }

    LOG_INFO("System tasks added");

#if FASTBEE_ENABLE_STORAGE_CACHE
    // 配置缓存定时 flush 任务（每5秒）
    if (!taskManager->addTask("config_cache_flush", [](void* param) {
        ConfigStorage::getInstance().flushDirtyEntries();
    }, nullptr, 5000)) {
        LOG_WARNING("Failed to add config cache flush task");
    }
#endif

#if FASTBEE_ENABLE_LOGGER
    // 日志环形缓冲定时 flush 任务（每5秒）
    if (!taskManager->addTask("log_flush", [](void* param) {
        LOGGER.flushBuffer();
    }, nullptr, 5000)) {
        LOG_WARNING("Failed to add log flush task");
    }
#endif

    return true;
}

// 运行框架主循环
void FastBeeFramework::run() {
    if (!systemInitialized) {
        LOG_ERROR("Framework not initialized, cannot run");
        return;
    }

    // 周期性系统状态串口输出（每 30 秒，提供完整运行状态）
    static unsigned long lastStatusPrint = 0;
    unsigned long nowMs = millis();
    if (nowMs - lastStatusPrint > 30000) {
        lastStatusPrint = nowMs;
        unsigned long uptimeSec = nowMs / 1000;
        unsigned long hours = uptimeSec / 3600;
        unsigned long mins = (uptimeSec % 3600) / 60;
        unsigned long secs = uptimeSec % 60;

        // 内存状态
        uint32_t freeHeap = ESP.getFreeHeap();
        uint32_t maxBlock = ESP.getMaxAllocHeap();
        ets_printf("[STATUS] uptime=%luh%lum%lus heap=%lu/%lu",
                      hours, mins, secs, (unsigned long)freeHeap, (unsigned long)maxBlock);
#ifdef BOARD_HAS_PSRAM
        ets_printf(" psram=%lu", (unsigned long)ESP.getFreePsram());
#endif
        ets_printf("\n");

        // 网络状态
#if FASTBEE_ENABLE_ETHERNET
        if (network && network->getNetworkType() == NetworkType::NET_ETHERNET) {
            auto* eth = network->getEthernetAdapter();
            if (eth && eth->isConnected()) {
                ets_printf("[STATUS] ETH=CONNECTED ip=%s", eth->localIP().toString().c_str());
                if (WiFi.getMode() & WIFI_AP) {
                    ets_printf(" ap=%s", WiFi.softAPIP().toString().c_str());
                }
                ets_printf("\n");
            } else {
                ets_printf("[STATUS] ETH=DISCONNECTED");
                if (WiFi.getMode() & WIFI_AP) {
                    ets_printf(" ap=%s", WiFi.softAPIP().toString().c_str());
                }
                ets_printf("\n");
            }
        } else
#endif
        {
            wl_status_t wifiSt = WiFi.status();
            if (wifiSt == WL_CONNECTED) {
                ets_printf("[STATUS] WiFi=CONNECTED ssid=%s ip=%s rssi=%d ch=%d\n",
                              WiFi.SSID().c_str(),
                              WiFi.localIP().toString().c_str(),
                              WiFi.RSSI(),
                              WiFi.channel());
            } else if (WiFi.getMode() & WIFI_AP) {
                ets_printf("[STATUS] WiFi=AP_MODE ap_ip=%s clients=%d\n",
                              WiFi.softAPIP().toString().c_str(),
                              WiFi.softAPgetStationNum());
            } else {
                const char* stStr = "UNKNOWN";
                switch (wifiSt) {
                    case WL_IDLE_STATUS:    stStr = "IDLE"; break;
                    case WL_NO_SSID_AVAIL: stStr = "NO_SSID"; break;
                    case WL_CONNECT_FAILED:stStr = "CONN_FAIL"; break;
                    case WL_DISCONNECTED:  stStr = "DISCONNECTED"; break;
                    default: break;
                }
                ets_printf("[STATUS] WiFi=%s mode=%d\n", stStr, (int)WiFi.getMode());
            }
        }
    }

    // 执行任务调度
    if (taskManager) {
        taskManager->run();
    }

    // 定期健康检查（除了任务调度中的检查外）
    unsigned long currentTime = millis();
    if (currentTime - lastHealthCheck > 60000UL) { // 每分钟额外检查一次
        lastHealthCheck = currentTime;
        if (healthMonitor && !healthMonitor->isSystemHealthy()) {
            char buf[160];
            healthMonitor->getHealthReport(buf, sizeof(buf));
            LOG_WARNING(buf);
        }
    }

    // 检查是否需要重启（例如在OTA更新后）
    checkForRestart();
}

// 检查是否需要重启
void FastBeeFramework::checkForRestart() {
    // HealthMonitor 宸茬粡璐熻矗鍐呭瓨瀹堟姢鍜屾渶鍚庣殑閲嶅惎淇濇姢锛岄伩鍏嶈繖閲屽啀鍙犲姞涓€灞傞噸鍚垽鏂?
    if (healthMonitor) {
        return;
    }

    static unsigned long lastRestartCheck = 0;
    static unsigned long criticalSince = 0;   // CRITICAL 状态持续时间
    unsigned long currentTime = millis();

    uint32_t freeHeap = ESP.getFreeHeap();

    // CRITICAL 状态（< 15KB）持续 30 秒以上则强制重启
    // 避免 Web 服务长时间返回 503 导致设备不可用
    if (freeHeap < MEM_THRESHOLD_SEVERE) {
        if (criticalSince == 0) {
            criticalSince = currentTime;
            char buf[80];
            snprintf(buf, sizeof(buf),
                     "[MEMGUARD] CRITICAL detected: heap=%lu, starting 30s restart timer",
                     (unsigned long)freeHeap);
            LOG_ERROR(buf);
        } else if (currentTime - criticalSince >= 30000UL) {
            LOG_ERROR("[MEMGUARD] CRITICAL for 30s, force restarting...");
            RestartDiagnostics::savePreRestartState(
                RestartReason::FRAMEWORK_LOW_MEMORY,
                "Framework: heap < 15KB for 30s");
            delay(100);
            ESP.restart();
        }
    } else {
        criticalSince = 0;  // 内存恢复正常，重置计时器
    }

    // 每5分钟常规检查一次（比原来的1小时更频繁）
    if (currentTime - lastRestartCheck > 300000UL) {
        lastRestartCheck = currentTime;
        if (freeHeap < HealthCheck::MIN_FREE_HEAP) {
            char buf[64];
            snprintf(buf, sizeof(buf),
                     "Low memory: %lu bytes free, restarting...", (unsigned long)freeHeap);
            LOG_ERROR(buf);
            RestartDiagnostics::savePreRestartState(
                RestartReason::FRAMEWORK_LOW_MEMORY,
                "Framework: periodic check — heap below MIN_FREE_HEAP");
            delay(500);
            ESP.restart();
        }
    }
}

// 关闭框架
void FastBeeFramework::shutdown() {
    if (!systemInitialized) {
        return;
    }

    LOG_INFO("Shutting down FastBee IoT Platform...");

    // 关机前 flush 日志缓冲数据
#if FASTBEE_ENABLE_LOGGER
    LOGGER.flushBuffer();
#endif

    // 按依赖顺序反向关闭各子系统
    if (taskManager) {
        taskManager->stopAllTasks();
    }

    if (protocolManager) {
        protocolManager->shutdown();
    }

    if (webConfig) {
        webConfig->stop();
    }

    if (ota) {
        // OTAManager 无需额外关闭（无长连接）
    }

    if (authManager) {
        authManager->shutdown();
    }

    if (healthMonitor) {
        healthMonitor->shutdown();
    }

    if (network) {
        network->disconnect();
    }

    // AsyncWebServer 无 stop() 方法，析构时自动释放
    // 各 unique_ptr 成员随 FastBeeFramework 析构自动释放

    systemInitialized = false;
    LOG_INFO("FastBee IoT Platform shutdown complete");
}

// 获取子系统指针
INetworkManager* FastBeeFramework::getNetworkManager() const { return network.get(); }
WebConfigManager* FastBeeFramework::getWebConfigManager() const { return webConfig.get(); }
OTAManager* FastBeeFramework::getOTAManager() const { return ota.get(); }
TaskManager* FastBeeFramework::getTaskManager() const { return taskManager.get(); }
HealthMonitor* FastBeeFramework::getHealthMonitor() const { return healthMonitor.get(); }
UserManager* FastBeeFramework::getUserManager() const { return userManager.get(); }
AuthManager* FastBeeFramework::getAuthManager() const { return authManager.get(); }

ProtocolManager* FastBeeFramework::getProtocolManager() const { return protocolManager.get(); }

// 检查系统是否已初始化
bool FastBeeFramework::isInitialized() const {
    return systemInitialized;
}

// 获取系统运行时间
unsigned long FastBeeFramework::getUptime() const {
    return millis();
}

// 从配置同步NTP时间（非阻塞，使用短超时）
void FastBeeFramework::syncTimeFromConfig() {
    File cfgFile = LittleFS.open("/config/device.json", "r");
    if (!cfgFile) {
        LOG_WARNING("[NTP] Failed to open device.json");
        return;
    }

    FastBeeJsonDoc cfg;
    if (deserializeJson(cfg, cfgFile) != DeserializationError::Ok) {
        cfgFile.close();
        LOG_WARNING("[NTP] Failed to parse device.json");
        return;
    }
    cfgFile.close();

    bool enableNTP = cfg["enableNTP"] | true;
    LOG_INFOF("[NTP] enableNTP=%s", enableNTP ? "true" : "false");

    if (!enableNTP) {
        LOG_INFO("[NTP] NTP is disabled");
        return;
    }

    const char* tz = cfg["timezone"] | "CST-8";
    const char* s1 = cfg["ntpServer1"] | "cn.pool.ntp.org";
    const char* s2 = cfg["ntpServer2"] | "time.windows.com";

    // 设置时区 - 使用 POSIX 格式 CST-8 (中国标准时间, UTC+8)
    // 注意：POSIX 格式中，负号表示东时区，正号表示西时区
    // 先设置环境变量，configTzTime 会使用这个设置
    setenv("TZ", tz, 1);
    tzset();

    LOG_INFOF("[NTP] Timezone set to: %s", tz);

    String s1Str = s1;
    if (s1Str.startsWith("http://") || s1Str.startsWith("https://")) {
        // HTTP NTP同步
        long long ts = 0;
        if (TimeUtils::syncNTPFromHTTPWithTimestamp(s1Str, ts, 3000)) {
            LOG_INFO("[NTP] HTTP sync successful");
            // HTTP同步成功后，重新应用时区设置
            tzset();
        }
        // 同时启动标准NTP作为备份
        configTzTime(tz, s2, "time.google.com", nullptr);
    } else {
        // 标准NTP同步（异步，需要等待后台完成）
        LOG_INFOF("[NTP] Starting SNTP sync with %s, %s", s1, s2);
        configTzTime(tz, s1, s2);
    }

    // 确保时区设置生效
    tzset();
}

// 启动时回填 deviceId 和 MQTT clientId（与 DeviceRouteHandler / ProtocolRouteHandler 的自动生成逻辑一致）
// - deviceId 为空时：使用 "FBE" + MAC（去冒号），并写回 /config/device.json
// - MQTT clientId 为空时：按 "S&deviceId&productNumber&userId"（authType==1 时前缀为 "E"），并写回 /config/protocol.json
void FastBeeFramework::ensureDeviceIdentity() {
    const char* devicePath = "/config/device.json";
    const char* protocolPath = "/config/protocol.json";

    // --------- 1. 处理 device.json 的 deviceId ---------
    JsonDocument devDoc;
    bool devDirty = false;
    bool devParsed = false;

    if (LittleFS.exists(devicePath)) {
        File f = LittleFS.open(devicePath, "r");
        if (f) {
            DeserializationError err = deserializeJson(devDoc, f);
            f.close();
            if (err) {
                LOG_WARNINGF("[Identity] device.json parse failed: %s, will regenerate", err.c_str());
                devDoc.clear();
            } else {
                devParsed = true;
            }
        }
    }

    if (!devParsed) {
        // 文件不存在或解析失败：创建最小默认配置
        devDoc["deviceName"] = "FastBee-Device";
        devDoc["userId"] = "1";
        devDirty = true;
    }

    String deviceId = devDoc["deviceId"].is<const char*>() ? String((const char*)devDoc["deviceId"]) : String("");
    deviceId.trim();
    if (deviceId.isEmpty()) {
        String mac = WiFi.macAddress();
        mac.replace(":", "");
        // 防御 WiFi 未启时返回全零或空
        if (mac.length() == 0 || mac == "000000000000") {
            uint64_t efuseMac = ESP.getEfuseMac();
            char macBuf[13];
            snprintf(macBuf, sizeof(macBuf), "%04X%08X",
                     (uint16_t)(efuseMac >> 32), (uint32_t)efuseMac);
            mac = macBuf;
        }
        deviceId = "FBE" + mac;
        devDoc["deviceId"] = deviceId;
        devDirty = true;
        LOG_INFOF("[Identity] deviceId auto-generated: %s", deviceId.c_str());
    }

    if (devDirty) {
        File f = LittleFS.open(devicePath, "w");
        if (f) {
            serializeJsonPretty(devDoc, f);
            f.close();
            LOG_INFO("[Identity] device.json persisted");
        } else {
            LOG_ERROR("[Identity] Failed to write device.json");
        }
    }

    // --------- 2. 处理 protocol.json 的 mqtt.clientId ---------
    // 若文件不存在，不主动创建（避免伪造一堆默认协议字段）
    if (!LittleFS.exists(protocolPath)) {
        return;
    }

    JsonDocument protoDoc;
    {
        File pf = LittleFS.open(protocolPath, "r");
        if (!pf) return;
        DeserializationError perr = deserializeJson(protoDoc, pf);
        pf.close();
        if (perr) {
            LOG_WARNINGF("[Identity] protocol.json parse failed: %s", perr.c_str());
            return;
        }
    }

    if (!protoDoc["mqtt"].is<JsonObject>()) {
        return;  // 没有 mqtt 节，跳过
    }

    String clientId = protoDoc["mqtt"]["clientId"].is<const char*>()
                          ? String((const char*)protoDoc["mqtt"]["clientId"])
                          : String("");
    clientId.trim();
    if (clientId.isEmpty()) {
        int authType = protoDoc["mqtt"]["authType"] | 0;

        // 产品编号：优先从 device.json 的 productNumber 读取，默认 "1"
        String productId = "1";
        int pn = devDoc["productNumber"] | 0;
        if (pn > 0) productId = String(pn);

        // userId：从 device.json 读取，默认 "1"
        String userId = devDoc["userId"].is<const char*>()
                            ? String((const char*)devDoc["userId"])
                            : String("1");
        if (userId.isEmpty()) userId = "1";

        String prefix = (authType == 1) ? "E" : "S";
        clientId = prefix + "&" + deviceId + "&" + productId + "&" + userId;
        protoDoc["mqtt"]["clientId"] = clientId;
        LOG_INFOF("[Identity] MQTT clientId auto-generated: %s", clientId.c_str());

        File pf2 = LittleFS.open(protocolPath, "w");
        if (pf2) {
            serializeJsonPretty(protoDoc, pf2);
            pf2.close();
            LOG_INFO("[Identity] protocol.json persisted");
        } else {
            LOG_ERROR("[Identity] Failed to write protocol.json");
        }
    }
}
