/**
 * @description: FastBee物联网平台框架核心
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:28:57
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
#include "security/UserManager.h"
#include "security/RoleManager.h"
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
    
    Serial.println();
    Serial.println("========================================");
    Serial.println("  FastBee IoT Platform Starting...");
    Serial.println("========================================");
    Serial.println();

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
    LOG_INFO("[STEP2.5] Initializing PeripheralManager...");
    if (!PeripheralManager::getInstance().initialize()) {
        LOG_WARNING("[STEP2.5] Failed to initialize peripheral manager");
    } else {
        LOG_INFO("[STEP2.5] Peripheral manager OK");
    }
    LOGGER.infof("[Boot] PeripheralManager: %lu ms", millis() - stepStart);
    
    // 步骤3: 创建HTTP服务器
    stepStart = millis();
    LOG_INFO("[STEP3] Creating HTTP server...");
    server.reset(new AsyncWebServer(80));
    if (!server) {
        LOG_ERROR("[STEP3] Failed to create HTTP server");
        return false;
    }
    LOGGER.infof("[Boot] AsyncWebServer: %lu ms", millis() - stepStart);
    
#if FASTBEE_WEB_START_EARLY
    // ---- 提前启动模式：Web服务器在WiFi连接前启动（AP模式直接可用）----

    // 步骤4-E: 初始化用户管理器（提前，Web服务需要）
    stepStart = millis();
    LOG_INFO("[STEP4-E] Initializing UserManager...");
    userManager.reset(new UserManager());
    if (!userManager || !userManager->initialize()) {
        LOG_ERROR("[STEP4-E] Failed to initialize user manager");
        return false;
    }
    LOG_INFO("[STEP4-E] User manager OK");

    // 步骤4-E.5: 初始化角色管理器（提前，Web服务需要）
    LOG_INFO("[STEP4-E.5] Initializing RoleManager...");
    roleManager.reset(new RoleManager());
    if (!roleManager || !roleManager->initialize()) {
        LOG_ERROR("[STEP4-E.5] Failed to initialize role manager");
        return false;
    }
    LOG_INFO("[STEP4-E.5] Role manager OK");

    // 步骤5-E: 初始化认证管理器（提前，Web服务需要）
    LOG_INFO("[STEP5-E] Initializing AuthManager...");
    authManager.reset(new AuthManager(userManager.get(), roleManager.get()));
    if (!authManager || !authManager->initialize()) {
        LOG_ERROR("[STEP5-E] Failed to initialize auth manager!");
        return false;
    }
    LOG_INFO("[STEP5-E] Auth manager OK");
    LOGGER.infof("[Boot] User/Role/AuthManager (early): %lu ms", millis() - stepStart);

    // 步骤6-E: 初始化 Web配置管理器（仅注册路由，暂不调用 server->begin()）
    // 注意：server->begin() 需要 LwIP TCP/IP 栈就绪，必须先等 NetworkManager 初始化 WiFi，
    //       否则会触发 lwip tcpip_api_call "Invalid mbox" 断言导致重启。
    stepStart = millis();
    LOG_INFO("[STEP6-E] Initializing WebConfigManager (routes only)...");
    webConfig.reset(new WebConfigManager(server.get(), authManager.get(), userManager.get()));
    if (!webConfig || !webConfig->initialize()) {
        LOG_ERROR("[STEP6-E] Failed to initialize web config manager");
        return false;
    }
    webConfig->setRoleManager(roleManager.get());
    // NetworkManager 尚未创建，暂不注入，在步骤7-E中注入
    LOGGER.infof("[Boot] WebConfigManager routes: %lu ms", millis() - stepStart);

    // 步骤7-E: 初始化网络管理器（启动 WiFi，带起 LwIP TCP/IP 栈）
    stepStart = millis();
    LOG_INFO("[STEP7-E] Creating NetworkManager...");
    network.reset(new NetworkManager(server.get()));
    if (!network) {
        LOG_ERROR("[STEP7-E] Failed to create network manager");
        return false;
    }
    
    // 初始化网络（会从 network.json 加载配置，可能阻塞等待WiFi连接）
    if (!network->initialize()) {
        LOG_WARNING("[STEP7-E] Network initialization returned false");
    }
    
    // 回注 NetworkManager 到 WebConfigManager
    webConfig->setNetworkManager(network.get());
    
    unsigned long networkMs = millis() - stepStart;
    // 检查网络状态
    if (WiFi.status() == WL_CONNECTED) {
        LOGGER.infof("[STEP7-E] WiFi Connected! IP: %s", WiFi.localIP().toString().c_str());
    } else if (WiFi.softAPIP() != IPAddress(0,0,0,0)) {
        LOGGER.infof("[STEP7-E] AP Mode IP: %s", WiFi.softAPIP().toString().c_str());
    } else {
        LOG_WARNING("[STEP7-E] Network not connected");
    }
    LOGGER.infof("[Boot] NetworkManager: %lu ms", networkMs);

    // 步骤7.5-E: 启动 HTTP 服务器（此时 TCP/IP 栈已就绪，AP 或 STA 模式均可用）
    stepStart = millis();
    LOG_INFO("[STEP7.5-E] Starting web server...");
    if (!webConfig->start()) {
        LOG_ERROR("[STEP7.5-E] Failed to start web server");
        return false;
    }
    LOGGER.infof("[Boot] WebServer start: %lu ms", millis() - stepStart);

#else  // !FASTBEE_WEB_START_EARLY
    // ---- 常规模式：WiFi连接完成后再启动Web服务器 ----

    // 步骤 4: 初始化网络管理器（可能阻塞：STA模式下 connectToWiFiBlocking 最长等待 connectTimeout ms）
    stepStart = millis();
    LOG_INFO("[STEP4] Creating NetworkManager...");
    network.reset(new NetworkManager(server.get()));
    if (!network) {
        LOG_ERROR("[STEP4] Failed to create network manager");
        return false;
    }
    
    // 初始化网络（会从 network.json 加载配置）
    if (!network->initialize()) {
        LOG_WARNING("[STEP4] Network initialization returned false");
    }
    
    unsigned long networkMs = millis() - stepStart;
    // 检查网络状态
    if (WiFi.status() == WL_CONNECTED) {
        LOGGER.infof("[STEP4] WiFi Connected! IP: %s", WiFi.localIP().toString().c_str());
    } else if (WiFi.softAPIP() != IPAddress(0,0,0,0)) {
        LOGGER.infof("[STEP4] AP Mode IP: %s", WiFi.softAPIP().toString().c_str());
    } else {
        LOG_WARNING("[STEP4] Network not connected");
    }
    LOGGER.infof("[Boot] NetworkManager: %lu ms", networkMs);

    // 步骤5: 初始化用户管理器
    stepStart = millis();
    LOG_INFO("[STEP5] Initializing UserManager...");
    userManager.reset(new UserManager());
    if (!userManager || !userManager->initialize()) {
        LOG_ERROR("[STEP5] Failed to initialize user manager");
        return false;
    }
    LOG_INFO("[STEP5] User manager OK");

    // 步骤5.5: 初始化角色管理器
    LOG_INFO("[STEP5.5] Initializing RoleManager...");
    roleManager.reset(new RoleManager());
    if (!roleManager || !roleManager->initialize()) {
        LOG_ERROR("[STEP5.5] Failed to initialize role manager");
        return false;
    }
    LOG_INFO("[STEP5.5] Role manager OK");

    // 步骤6: 初始化认证管理器（注入 RoleManager）
    LOG_INFO("[STEP6] Initializing AuthManager...");
    authManager.reset(new AuthManager(userManager.get(), roleManager.get()));
    if (!authManager || !authManager->initialize()) {
        LOG_ERROR("[STEP6] Failed to initialize auth manager!");
        return false;
    }
    LOG_INFO("[STEP6] Auth manager OK");
    LOGGER.infof("[Boot] User/Role/AuthManager: %lu ms", millis() - stepStart);

    // 步骤 7: 初始化 Web配置管理器
    stepStart = millis();
    LOG_INFO("[STEP7] Initializing WebConfigManager...");
    webConfig.reset(new WebConfigManager(server.get(), authManager.get(), userManager.get()));
    if (!webConfig || !webConfig->initialize()) {
        LOG_ERROR("[STEP7] Failed to initialize web config manager");
        return false;
    }
    // 注入 RoleManager 和 NetworkManager 给 WebConfigManager
    webConfig->setRoleManager(roleManager.get());
    webConfig->setNetworkManager(network.get());
    // 启动 HTTP 服务器（监听端口 80）
    if (!webConfig->start()) {
        LOG_ERROR("[STEP7] Failed to start web server");
        return false;
    }
    LOGGER.infof("[Boot] WebConfigManager: %lu ms", millis() - stepStart);

#endif  // FASTBEE_WEB_START_EARLY
    
    // 步骤8: 初始化OTA管理器
    stepStart = millis();
    LOG_INFO("[STEP8] Initializing OTAManager...");
    ota.reset(new OTAManager(server.get()));
    if (!ota || !ota->initialize()) {
        LOG_ERROR("[STEP8] Failed to initialize OTA manager");
        return false;
    }
    LOGGER.infof("[Boot] OTAManager: %lu ms", millis() - stepStart);
    
    // 步骤9: 初始化任务管理器
    stepStart = millis();
    LOG_INFO("[STEP9] Initializing TaskManager...");
    taskManager.reset(new TaskManager());
    if (!taskManager || !taskManager->initialize()) {
        LOG_ERROR("[STEP9] Failed to initialize task manager");
        return false;
    }
    LOGGER.infof("[Boot] TaskManager: %lu ms", millis() - stepStart);
    
    // 步骤10: 初始化健康监控器
    stepStart = millis();
    LOG_INFO("[STEP10] Initializing HealthMonitor...");
    healthMonitor.reset(new HealthMonitor());
    if (!healthMonitor || !healthMonitor->initialize()) {
        LOG_ERROR("[STEP10] Failed to initialize health monitor");
        return false;
    }
    LOGGER.infof("[Boot] HealthMonitor: %lu ms", millis() - stepStart);
    
    // 步骤11: 初始化协议管理器
    stepStart = millis();
    LOG_INFO("[STEP11] Initializing ProtocolManager...");
    protocolManager.reset(new ProtocolManager());
    if (!protocolManager || !protocolManager->initialize()) {
        LOG_ERROR("[STEP11] Failed to initialize protocol manager");
        return false;
    }
    LOGGER.infof("[Boot] ProtocolManager: %lu ms", millis() - stepStart);
    
    // 注入 ProtocolManager 到 WebConfig，供 API 路由访问
    if (webConfig && protocolManager) {
        webConfig->setProtocolManager(protocolManager.get());
    }

    // 注入 TX/RX 计数回调：协议层消息收发时递增网络层计数器
    if (network && protocolManager) {
        NetworkManager* netMgr = network.get();
        protocolManager->setTxCallback([netMgr]() { netMgr->incrementTxCount(); });
        protocolManager->setRxCallback([netMgr]() { netMgr->incrementRxCount(); });
    }
    
    // 步骤11.5: 初始化外设执行管理器
#if FASTBEE_ENABLE_PERIPH_EXEC
    stepStart = millis();
    LOG_INFO("[STEP11.5] Initializing PeriphExecManager...");
    if (!PeriphExecManager::getInstance().initialize()) {
        LOG_WARNING("[STEP11.5] Failed to initialize periph exec manager");
    } else {
        LOG_INFO("[STEP11.5] Periph exec manager OK");
    }
    LOGGER.infof("[Boot] PeriphExecManager: %lu ms", millis() - stepStart);

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
                    // 触发 mc: 事件
                    char mcIdBuf[16];
                    snprintf(mcIdBuf, sizeof(mcIdBuf), "mc:%d", match.deviceIndex);
                    char mcDataBuf[160];
                    snprintf(mcDataBuf, sizeof(mcDataBuf), "{\"d\":%d,\"id\":\"%s\",\"v\":\"%s\"}",
                        match.deviceIndex, sensorId.c_str(), reportValue.c_str());
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

            // 控制成功后触发 mc: 事件
            if (writeOk) {
                char mcIdBuf[16];
                snprintf(mcIdBuf, sizeof(mcIdBuf), "mc:%d", match.deviceIndex);
                char mcDataBuf[160];
                snprintf(mcDataBuf, sizeof(mcDataBuf), "{\"d\":%d,\"id\":\"%s\",\"v\":\"%s\"}",
                    match.deviceIndex, sensorId.c_str(), reportValue.c_str());
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

        LOG_INFO("[STEP11.5] Protocol callbacks injected into PeriphExecManager");
    }
#endif

    // 步骤11.6: 初始化规则脚本管理器
#if FASTBEE_ENABLE_RULE_SCRIPT
    stepStart = millis();
    LOG_INFO("[STEP11.6] Initializing RuleScriptManager...");
    if (!RuleScriptManager::getInstance().initialize()) {
        LOG_WARNING("[STEP11.6] Failed to initialize rule script manager");
    } else {
        LOG_INFO("[STEP11.6] Rule script manager OK");
    }
    LOGGER.infof("[Boot] RuleScriptManager: %lu ms", millis() - stepStart);
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
    LOG_INFO("[STEP12] Adding system tasks...");
    if (!addSystemTasks()) {
        LOG_WARNING("[STEP12] Failed to add some system tasks");
    }
    LOGGER.infof("[Boot] addSystemTasks: %lu ms", millis() - stepStart);
    
    // 步骤13: 检查系统健康状态
    LOG_INFO("[STEP13] Checking system health...");
    if (!healthMonitor->isSystemHealthy()) {
        char buf[160];
        healthMonitor->getHealthReport(buf, sizeof(buf));
        LOG_WARNING(buf);
    }
    LOG_INFO("[STEP13] Health check completed");
    
    // 计算启动耗时
    unsigned long bootTime = millis() - startTime;
    
    systemInitialized = true;
    
    // 输出 Boot Performance Report
    LOGGER.info("[Boot] === Performance Report ===");
    LOGGER.infof("[Boot] Total boot time: %lu ms", bootTime);
    if (networkMs > 1000) {
        LOGGER.infof("[Boot] WARNING: NetworkManager took %lu ms (may block on WiFi connect)", networkMs);
    }
    LOGGER.info("[Boot] === End Report ===");
    
    // 将启动耗时记录到 HealthMonitor
#if FASTBEE_ENABLE_HEALTH_MONITOR
    if (healthMonitor) {
        healthMonitor->setBootTime(bootTime);
    }
#endif
    
    LOGGER.info("========================================");
    LOGGER.infof("System initialization completed in %lu ms", bootTime);
    LOGGER.info("========================================");
    LOG_INFO("FastBee IoT Platform initialized successfully");
    LOG_INFO("Device ready for operation");
    
    // 输出访问信息（同时输出到串口和日志）
    LOGGER.info("========================================");
    LOGGER.info("  FastBee IoT Platform Ready!");
    LOGGER.info("========================================");
    
    if (WiFi.status() == WL_CONNECTED) {
        LOGGER.info("Mode: STA (WiFi Client)");
        LOGGER.infof("IP Address: %s", WiFi.localIP().toString().c_str());
        LOGGER.infof("Access URL: http://%s", WiFi.localIP().toString().c_str());
        LOGGER.info("mDNS URL: http://fastbee.local");
    } else if (WiFi.softAPIP() != IPAddress(0,0,0,0)) {
        LOGGER.info("Mode: AP (Access Point)");
        // 以 NetworkManager 的实际配置为准，避免误导用户
        String apSSID = Network::DEFAULT_AP_SSID;
        String apPass = Network::DEFAULT_AP_PASSWORD;
        if (network) {
            WiFiConfig cfg = network->getConfig();
            if (cfg.apSSID.length() > 0) apSSID = cfg.apSSID;
            if (cfg.apPassword.length() > 0) apPass = cfg.apPassword;
        }
        LOGGER.infof("WiFi Name: %s", apSSID.c_str());
        LOGGER.infof("WiFi Pass: %s", apPass.c_str());
        LOGGER.infof("IP Address: %s", WiFi.softAPIP().toString().c_str());
        LOGGER.infof("Setup URL: http://%s/setup", WiFi.softAPIP().toString().c_str());
    } else {
        LOGGER.info("Network: Not connected");
    }
    
    LOGGER.info("----------------------------------------");
    LOGGER.info("Default Login: admin / admin123");
    LOGGER.info("========================================");
    
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
    }, this, 30000)) {
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
            
            // 标记WiFi已连接，触发NTP同步任务（仅执行一次）
            if (!framework->ntpSynced && WiFi.status() == WL_CONNECTED) {
                static unsigned long wifiConnectedTime = 0;
                if (wifiConnectedTime == 0) {
                    wifiConnectedTime = millis();
                } else if (millis() - wifiConnectedTime > 3000) {
                    // WiFi连接稳定3秒后标记需要同步
                    LOG_INFO("[NTP] WiFi connected, NTP sync scheduled");
                    framework->ntpSyncPending = true;
                    framework->ntpSynced = true; // 标记已处理，避免重复
                }
            }
            
            // WiFi连接后自动启动协议（MQTT + Modbus，仅执行一次）
            // 合并读取 protocol.json，减少一次 JsonDocument 分配（~8KB 栈空间）
            if ((!framework->mqttAutoStarted || !framework->modbusAutoStarted) 
                && WiFi.status() == WL_CONNECTED && framework->protocolManager) {
                static unsigned long protocolWaitStart = 0;
                if (protocolWaitStart == 0) {
                    protocolWaitStart = millis();
                } else if (millis() - protocolWaitStart > 5000) {
                    // WiFi稳定5秒后，一次性读取 protocol.json 并启动所有协议
                    framework->mqttAutoStarted = true;
                    framework->modbusAutoStarted = true;
                    LOG_INFO("[Protocol] WiFi stable, auto-starting protocols...");
                    
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
                    
                    // 启动 MQTT
#if FASTBEE_ENABLE_MQTT
                    if (mqttEnabled) {
                        LOG_INFO("[MQTT] MQTT enabled, auto-starting...");
                        framework->protocolManager->restartMQTT();
                    } else {
                        LOG_INFO("[MQTT] MQTT not enabled in config, skipping auto-start");
                    }
#endif
                    
                    // 启动 Modbus
#if FASTBEE_ENABLE_MODBUS
                    if (modbusEnabled) {
                        LOG_INFO("[Modbus] Modbus enabled, auto-starting...");
                        framework->protocolManager->restartModbus();
                    } else {
                        LOG_INFO("[Modbus] Modbus not enabled in config, skipping auto-start");
                    }
#endif
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
        
        // 检查是否需要启动同步
        if (framework->ntpSyncPending && WiFi.status() == WL_CONNECTED) {
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
                framework->ntpRetryCount = 0;
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
    }, this, 300000)) {
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
        
    // 按键事件检测任务（每100ms）- 平衡响应灵敏度和系统稳定性
    // 注意：50ms间隔过于激进，可能导致执行超时和看门狗复位
    if (!taskManager->addTask("button_event_check", [](void* param) {
        PeriphExecManager::getInstance().checkButtonEvents();
    }, nullptr, 100)) {
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
    static unsigned long lastRestartCheck = 0;
    unsigned long currentTime = millis();

    // 每小时检查一次（3600000ms）
    if (currentTime - lastRestartCheck > 3600000UL) {
        lastRestartCheck = currentTime;
        uint32_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < HealthCheck::MIN_FREE_HEAP) {
            char buf[64];
            snprintf(buf, sizeof(buf),
                     "Low memory: %lu bytes free, restarting...", (unsigned long)freeHeap);
            LOG_ERROR(buf);
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
RoleManager* FastBeeFramework::getRoleManager() const { return roleManager.get(); }
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