#include "core/PeriphExecExecutor.h"
#include "core/PeriphExecManager.h"
#include "core/RuleScript.h"
#include "core/ScriptEngine.h"
#include "core/FastBeeFramework.h"
#include "protocols/ProtocolManager.h"
#include "protocols/ModbusHandler.h"
#include "protocols/MQTTClient.h"
#include "systems/LoggerSystem.h"
#include "core/PeripheralManager.h"
#include "core/ChipConfig.h"
#include <freertos/task.h>

PeriphExecExecutor::PeriphExecExecutor() = default;
PeriphExecExecutor::~PeriphExecExecutor() = default;

void PeriphExecExecutor::initialize(PeriphExecManager* manager) {
    _manager = manager;
}

// ========== 动作执行接口 ==========

bool PeriphExecExecutor::executeActionItem(const ExecAction& action, const String& effectiveValue) {
    // 系统功能 (actionType 6-11)
    if (action.actionType >= 6 && action.actionType <= 11) {
        return executeSystemAction(action);
    }
    // 命令脚本 (actionType 15)
    if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT)) {
        return executeScriptAction(action);
    }
    // 调用其他外设 (actionType 12)
    if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL)) {
        return executeCallPeripheralAction(action, effectiveValue);
    }
    // Modbus 子设备动作 (targetPeriphId 以 "modbus:" 开头)
    if (action.targetPeriphId.startsWith("modbus:")) {
        return executeModbusAction(action, effectiveValue);
    }
    // 外设动作
    return executePeripheralAction(action, effectiveValue);
}

std::vector<ActionExecResult> PeriphExecExecutor::executeAllActions(
    const PeriphExecRule& rule, const String& receivedValue) {

    std::vector<ActionExecResult> results;
    results.reserve(rule.actions.size());

    for (const auto& action : rule.actions) {
        // 堆守卫：防止动作执行过程中堆耗尽导致 abort()
        if (ESP.getFreeHeap() < 15000) {
            LOGGER.warningf("[PeriphExec] Heap too low (%d), skipping remaining actions",
                            (int)ESP.getFreeHeap());
            break;
        }

        // 执行前延时
        if (action.syncDelayMs > 0) {
            delay(action.syncDelayMs > 10000 ? 10000 : action.syncDelayMs);
        }

        // 确定有效值：useReceivedValue 时用接收值，否则用 actionValue
        String effectiveValue = (action.useReceivedValue && !receivedValue.isEmpty())
                                ? receivedValue : action.actionValue;

        LOGGER.infof("[PeriphExec] Exec action: target=%s type=%d val=%s (useRecv=%d)",
            action.targetPeriphId.c_str(), action.actionType,
            effectiveValue.c_str(), action.useReceivedValue);

        bool ok;
        ActionExecResult ar;
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ)) {
            ok = executeSensorReadAction(action, ar);
            // ar is fully populated by executeSensorReadAction
        } else if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_POLL)) {
            ok = executeModbusPollAction(action, rule);
            ar.targetPeriphId = action.targetPeriphId;
            ar.actualValue = effectiveValue;
        } else {
            ok = executeActionItem(action, effectiveValue);
            ar.targetPeriphId = action.targetPeriphId;
            ar.actualValue = effectiveValue;
        }
        ar.success = ok;
        results.push_back(ar);
    }

    // 精准上报执行结果
    if (rule.reportAfterExec && !results.empty()) {
        reportActionResults(results);
    }

    return results;
}

// ========== 具体动作执行方法 ==========

bool PeriphExecExecutor::executePeripheralAction(const ExecAction& action, const String& effectiveValue) {
    PeripheralManager& pm = PeripheralManager::getInstance();

    if (action.targetPeriphId.isEmpty()) {
        LOGGER.warning("[PeriphExec] No target peripheral specified");
        return false;
    }

    if (!pm.hasPeripheral(action.targetPeriphId)) {
        LOGGER.warningf("[PeriphExec] Target peripheral not found: %s", action.targetPeriphId.c_str());
        return false;
    }

    ExecActionType actType = static_cast<ExecActionType>(action.actionType);

    switch (actType) {
        case ExecActionType::ACTION_HIGH: {
            LOGGER.infof("[PeriphExec] Execute HIGH on %s", action.targetPeriphId.c_str());
            pm.stopActionTicker(action.targetPeriphId);
            return pm.writePin(action.targetPeriphId, GPIOState::STATE_HIGH);
        }

        case ExecActionType::ACTION_LOW: {
            LOGGER.infof("[PeriphExec] Execute LOW on %s", action.targetPeriphId.c_str());
            pm.stopActionTicker(action.targetPeriphId);
            return pm.writePin(action.targetPeriphId, GPIOState::STATE_LOW);
        }

        case ExecActionType::ACTION_BLINK: {
            LOGGER.infof("[PeriphExec] Execute BLINK on %s", action.targetPeriphId.c_str());
            uint16_t interval = effectiveValue.isEmpty() ? 500 : effectiveValue.toInt();
            pm.startActionTicker(action.targetPeriphId, 1, interval);
            return true;
        }

        case ExecActionType::ACTION_BREATHE: {
            LOGGER.infof("[PeriphExec] Execute BREATHE on %s", action.targetPeriphId.c_str());
            uint16_t speed = effectiveValue.isEmpty() ? 2000 : effectiveValue.toInt();
            pm.startActionTicker(action.targetPeriphId, 2, speed);
            return true;
        }

        case ExecActionType::ACTION_SET_PWM: {
            uint32_t duty = effectiveValue.isEmpty() ? 0 : effectiveValue.toInt();
            LOGGER.infof("[PeriphExec] Execute SetPWM(%d) on %s", (int)duty, action.targetPeriphId.c_str());
            return pm.writePWM(action.targetPeriphId, duty);
        }

        case ExecActionType::ACTION_SET_DAC: {
#if CHIP_HAS_DAC
            uint8_t dacVal = effectiveValue.isEmpty() ? 0 : effectiveValue.toInt();
            LOGGER.infof("[PeriphExec] Execute SetDAC(%d) on %s", (int)dacVal, action.targetPeriphId.c_str());
            PeripheralConfig* cfg = pm.getPeripheral(action.targetPeriphId);
            if (!cfg) return false;
            uint8_t pin = cfg->getPrimaryPin();
            dacWrite(pin, dacVal);
            return true;
#else
            LOGGER.warning("[PeriphExec] DAC not supported on this chip");
            return false;
#endif
        }

        case ExecActionType::ACTION_HIGH_INVERTED: {
            LOGGER.infof("[PeriphExec] Execute HIGH(inverted) on %s", action.targetPeriphId.c_str());
            pm.stopActionTicker(action.targetPeriphId);
            return pm.writePin(action.targetPeriphId, GPIOState::STATE_LOW);
        }

        case ExecActionType::ACTION_LOW_INVERTED: {
            LOGGER.infof("[PeriphExec] Execute LOW(inverted) on %s", action.targetPeriphId.c_str());
            pm.stopActionTicker(action.targetPeriphId);
            return pm.writePin(action.targetPeriphId, GPIOState::STATE_HIGH);
        }

        default:
            LOGGER.warningf("[PeriphExec] Unknown peripheral action: %d", action.actionType);
            return false;
    }
}

bool PeriphExecExecutor::executeModbusAction(const ExecAction& action, const String& effectiveValue) {
    // 解析 "modbus:<deviceIndex>" 格式
    int deviceIdx = action.targetPeriphId.substring(7).toInt();

    auto* fw = FastBeeFramework::getInstance();
    if (!fw) { LOGGER.warning("[PeriphExec] Framework not available"); return false; }
    auto* protMgr = fw->getProtocolManager();
    if (!protMgr) { LOGGER.warning("[PeriphExec] ProtocolManager not available"); return false; }
    ModbusHandler* modbus = protMgr->getModbusHandler();
    if (!modbus) {
        LOGGER.warning("[PeriphExec] ModbusHandler not available");
        return false;
    }
    if (deviceIdx < 0 || deviceIdx >= modbus->getSubDeviceCount()) {
        LOGGER.warningf("[PeriphExec] Modbus device index out of range: %d", deviceIdx);
        return false;
    }

    const ModbusSubDevice& dev = modbus->getSubDevice(deviceIdx);
    ExecActionType actType = static_cast<ExecActionType>(action.actionType);

    switch (actType) {
        case ExecActionType::ACTION_MODBUS_COIL_WRITE: {
            // 值格式: "channel:state" 如 "0:1" 表示通道0打开, 或单独 "1"/"0" 表示通道0的开关
            uint16_t channel = 0;
            bool coilState = false;
            int sepIdx = effectiveValue.indexOf(':');
            if (sepIdx > 0) {
                channel = effectiveValue.substring(0, sepIdx).toInt();
                coilState = effectiveValue.substring(sepIdx + 1).toInt() != 0;
            } else {
                coilState = effectiveValue.toInt() != 0;
            }
            uint16_t coilAddr = dev.coilBase + channel;
            LOGGER.infof("[PeriphExec] Modbus FC05: slave=%d coil=%d state=%d",
                dev.slaveAddress, coilAddr, coilState);
            OneShotResult result = modbus->writeCoilOnce(dev.slaveAddress, coilAddr, coilState);
            return result.error == ONESHOT_SUCCESS;
        }
        case ExecActionType::ACTION_MODBUS_REG_WRITE: {
            // 值格式: "addr:value" 指定寄存器地址和值, 或单独 "value" 使用 coilBase 作为地址
            uint16_t regAddr = dev.coilBase;
            uint16_t regVal = 0;
            int sepIdx = effectiveValue.indexOf(':');
            if (sepIdx > 0) {
                regAddr = effectiveValue.substring(0, sepIdx).toInt();
                regVal = effectiveValue.substring(sepIdx + 1).toInt();
            } else {
                regVal = effectiveValue.toInt();
            }
            LOGGER.infof("[PeriphExec] Modbus FC06: slave=%d reg=%d val=%d",
                dev.slaveAddress, regAddr, regVal);
            OneShotResult result = modbus->writeRegisterOnce(dev.slaveAddress, regAddr, regVal);
            return result.error == ONESHOT_SUCCESS;
        }
        default:
            LOGGER.warningf("[PeriphExec] Unknown Modbus action type: %d", action.actionType);
            return false;
    }
}

bool PeriphExecExecutor::executeModbusPollAction(const ExecAction& action, const PeriphExecRule& rule) {
    auto* fw = FastBeeFramework::getInstance();
    if (!fw) { LOGGER.warning("[PeriphExec] Framework not available"); return false; }
    auto* protMgr = fw->getProtocolManager();
    if (!protMgr) { LOGGER.warning("[PeriphExec] ProtocolManager not available"); return false; }
    ModbusHandler* modbus = protMgr->getModbusHandler();
    if (!modbus) {
        LOGGER.warning("[PeriphExec] ModbusHandler not available for poll action");
        return false;
    }

    // 检查 Modbus 是否在 Master 模式
    if (modbus->getMode() != MODBUS_MASTER) {
        LOGGER.warning("[PeriphExec] Modbus not in Master mode, skip poll action");
        return false;
    }

    // 检查是否有配置的轮询任务
    uint8_t pollTaskCount = modbus->getPollTaskCount();
    if (pollTaskCount == 0) {
        LOGGER.warning("[PeriphExec] No Modbus poll tasks configured");
        return false;
    }

    // 检查堆内存是否充足（降低阈值，避免传感器离线时堆略低就完全跳过轮询）
    if (ESP.getFreeHeap() < 25000) {
        LOGGER.warningf("[PeriphExec] Insufficient heap for Modbus poll: %d bytes free", ESP.getFreeHeap());
        return false;
    }

    // 从规则的 POLL_TRIGGER 触发器中提取通信参数
    uint16_t timeout = 1000;
    uint8_t retries = 2;
    uint16_t interDelay = 100;
    for (const auto& trigger : rule.triggers) {
        if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER)) {
            timeout = trigger.pollResponseTimeout;
            retries = trigger.pollMaxRetries;
            interDelay = trigger.pollInterPollDelay;
            break;
        }
    }

    String actionVal = action.actionValue;
    if (actionVal.isEmpty()) {
        LOGGER.warning("[PeriphExec] Poll action has empty actionValue");
        return false;
    }

    // 检测 JSON 格式 vs 旧逗号分隔格式
    if (actionVal.charAt(0) == '{') {
        // === 新 JSON 格式: {"poll":[0,1],"ctrl":[{"d":0,"a":"on","c":0}]} ===
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, actionVal);
        if (err) {
            LOGGER.warningf("[PeriphExec] Failed to parse JSON actionValue: %s", err.c_str());
            return false;
        }

        bool anySuccess = false;

        // 处理 poll 数组: 执行采集任务
        JsonArray pollArr = doc["poll"].as<JsonArray>();
        if (pollArr) {
            // 根据轮询任务数量动态计算缓冲区
            uint16_t estimatedSize = 128;  // 基础开销
            for (JsonVariant v : pollArr) {
                estimatedSize += 512;  // 每个轮询预留512字节
            }
            if (estimatedSize > 8192) estimatedSize = 8192;  // 上限8KB

            String mergedJson;
            mergedJson.reserve(estimatedSize);
            mergedJson = "[";
            bool first = true;
            for (JsonVariant v : pollArr) {
                // 动态堆检查：防止多任务迭代中堆逐渐耗尽导致 abort()
                if (ESP.getFreeHeap() < 15000) {
                    LOGGER.warningf("[PeriphExec] Heap dropped to %d during poll, stopping early",
                                    (int)ESP.getFreeHeap());
                    break;
                }
                uint8_t taskIdx = v.as<uint8_t>();
                if (!first && interDelay > 0) vTaskDelay(pdMS_TO_TICKS(interDelay));
                String result = modbus->executePollTaskByIndex(taskIdx, timeout, retries);
                if (result != "[]") {
                    anySuccess = true;
                    String inner = result.substring(1, result.length() - 1);
                    if (!first) mergedJson += ",";
                    mergedJson += inner;
                    first = false;
                }
            }
            mergedJson += "]";
            if (anySuccess) {
                LOGGER.infof("[PeriphExec] Poll completed, injecting %d bytes", mergedJson.length());
                // 通过 manager 调用 handlePollData
                if (_manager) {
                    _manager->handlePollData("modbus_poll", mergedJson);
                }
            }
        }

        // 处理 ctrl 数组: 执行控制命令
        JsonArray ctrlArr = doc["ctrl"].as<JsonArray>();
        if (ctrlArr) {
            ModbusConfig cfg = modbus->getConfig();
            for (JsonVariant cv : ctrlArr) {
                uint8_t devIdx = cv["d"].as<uint8_t>();
                if (devIdx >= cfg.master.deviceCount) {
                    LOGGER.warningf("[PeriphExec] ctrl device index %d out of range", devIdx);
                    continue;
                }
                const ModbusSubDevice& dev = cfg.master.devices[devIdx];
                const char* act = cv["a"] | "on";
                uint8_t ch = cv["c"] | 0;
                uint16_t val = cv["v"] | 0;

                if (interDelay > 0) vTaskDelay(pdMS_TO_TICKS(interDelay));

                String devType(dev.deviceType);
                if (devType == "relay") {
                    bool coilVal = (strcmp(act, "on") == 0);
                    if (dev.ncMode) coilVal = !coilVal;
                    uint16_t addr = dev.coilBase + ch;
                    if (dev.controlProtocol == 0) {
                        auto res = modbus->writeCoilOnce(dev.slaveAddress, addr, coilVal);
                        bool ok = (res.error == ONESHOT_SUCCESS);
                        anySuccess = anySuccess || ok;
                        LOGGER.infof("[PeriphExec] Relay ctrl: slave=%d coil=%d val=%d ok=%d",
                            dev.slaveAddress, addr, coilVal, ok);
                    } else {
                        uint16_t regVal = coilVal ? 0xFF00 : 0x0000;
                        auto res = modbus->writeRegisterOnce(dev.slaveAddress, addr, regVal);
                        bool ok = (res.error == ONESHOT_SUCCESS);
                        anySuccess = anySuccess || ok;
                        LOGGER.infof("[PeriphExec] Relay reg ctrl: slave=%d reg=%d val=0x%04X ok=%d",
                            dev.slaveAddress, addr, regVal, ok);
                    }
                } else if (devType == "pwm") {
                    uint16_t regAddr = dev.pwmRegBase + ch;
                    auto res = modbus->writeRegisterOnce(dev.slaveAddress, regAddr, val);
                    bool ok = (res.error == ONESHOT_SUCCESS);
                    anySuccess = anySuccess || ok;
                    LOGGER.infof("[PeriphExec] PWM ctrl: slave=%d reg=%d val=%d ok=%d",
                        dev.slaveAddress, regAddr, val, ok);
                } else if (devType == "pid") {
                    const char* param = cv["p"] | "P";
                    uint8_t pidIdx = 3;
                    if (strcmp(param, "I") == 0) pidIdx = 4;
                    else if (strcmp(param, "D") == 0) pidIdx = 5;
                    uint16_t regAddr = dev.pidAddrs[pidIdx];
                    auto res = modbus->writeRegisterOnce(dev.slaveAddress, regAddr, val);
                    bool ok = (res.error == ONESHOT_SUCCESS);
                    anySuccess = anySuccess || ok;
                    LOGGER.infof("[PeriphExec] PID ctrl: slave=%d param=%s reg=%d val=%d ok=%d",
                        dev.slaveAddress, param, regAddr, val, ok);
                }
            }
        }

        return anySuccess;
    }

    // === 旧格式兼容: 逗号分隔的任务索引，如 "0,1,3" ===
    String mergedJson = "[";
    bool first = true;
    bool anySuccess = false;

    int start = 0;
    while (start <= (int)actionVal.length()) {
        int comma = actionVal.indexOf(',', start);
        if (comma < 0) comma = actionVal.length();
        String token = actionVal.substring(start, comma);
        token.trim();
        if (token.length() > 0) {
            uint8_t taskIdx = (uint8_t)token.toInt();

            if (!first && interDelay > 0) {
                vTaskDelay(pdMS_TO_TICKS(interDelay));
            }

            String result = modbus->executePollTaskByIndex(taskIdx, timeout, retries);
            if (result != "[]") {
                anySuccess = true;
                String inner = result.substring(1, result.length() - 1);
                if (!first) mergedJson += ",";
                mergedJson += inner;
                first = false;
            }
        }
        start = comma + 1;
    }

    mergedJson += "]";

    if (anySuccess) {
        LOGGER.infof("[PeriphExec] Poll action completed, injecting %d bytes", mergedJson.length());
        if (_manager) {
            _manager->handlePollData("modbus_poll", mergedJson);
        }
    }

    return anySuccess;
}

bool PeriphExecExecutor::executeSensorReadAction(const ExecAction& action, ActionExecResult& result) {
    String actionVal = action.actionValue;
    if (actionVal.isEmpty() || actionVal.charAt(0) != '{') {
        LOGGER.warning("[PeriphExec] Sensor read: empty or invalid actionValue");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, actionVal);
    if (err) {
        LOGGER.warningf("[PeriphExec] Sensor read: JSON parse failed: %s", err.c_str());
        return false;
    }

    const char* periphId = doc["periphId"] | "";
    const char* category = doc["sensorCategory"] | "analog";
    float scaleFactor = doc["scaleFactor"] | 1.0f;
    float offset = doc["offset"] | 0.0f;
    uint8_t decimals = doc["decimalPlaces"] | 2;
    const char* label = doc["sensorLabel"] | "";
    const char* unit = doc["unit"] | "";

    if (strlen(periphId) == 0) {
        LOGGER.warning("[PeriphExec] Sensor read: no periphId specified");
        return false;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    float rawValue = 0;

    if (strcmp(category, "analog") == 0) {
        rawValue = (float)pm.readAnalog(String(periphId));
    } else if (strcmp(category, "digital") == 0) {
        GPIOState state = pm.readPin(String(periphId));
        rawValue = (state == GPIOState::STATE_HIGH) ? 1.0f : 0.0f;
    } else if (strcmp(category, "pulse") == 0) {
        LOGGER.warning("[PeriphExec] Sensor read: pulse/frequency not yet implemented");
        rawValue = 0;
    } else {
        LOGGER.warningf("[PeriphExec] Sensor read: unknown category '%s'", category);
        return false;
    }

    float processed = rawValue * scaleFactor + offset;
    if (decimals > 6) decimals = 6;

    result.targetPeriphId = String(periphId);
    result.actualValue = String(processed, (unsigned int)decimals);

    LOGGER.infof("[PeriphExec] Sensor read: periph=%s cat=%s raw=%.1f processed=%s%s label=%s",
        periphId, category, rawValue, result.actualValue.c_str(), unit, label);

    return true;
}

bool PeriphExecExecutor::executeSystemAction(const ExecAction& action) {
    ExecActionType actType = static_cast<ExecActionType>(action.actionType);

    switch (actType) {
        case ExecActionType::ACTION_SYS_RESTART:
            LOGGER.info("[PeriphExec] Executing system restart...");
            delay(500);
            ESP.restart();
            return true;

        case ExecActionType::ACTION_SYS_FACTORY_RESET: {
            LOGGER.info("[PeriphExec] Executing factory reset...");
            const char* configFiles[] = {
                "/config/device.json", "/config/network.json", "/config/protocol.json",
                "/config/users.json", "/config/system.json",
                "/config/http.json", "/config/mqtt.json", "/config/tcp.json",
                "/config/modbus.json", "/config/coap.json", PERIPH_EXEC_CONFIG_FILE,
                RULE_SCRIPT_CONFIG_FILE, "/config/peripherals.json", "/config/roles.json"
            };
            for (int i = 0; i < (int)(sizeof(configFiles) / sizeof(configFiles[0])); i++) {
                if (LittleFS.exists(configFiles[i])) {
                    LittleFS.remove(configFiles[i]);
                }
            }
            delay(500);
            ESP.restart();
            return true;
        }

        case ExecActionType::ACTION_SYS_NTP_SYNC: {
            LOGGER.info("[PeriphExec] Executing NTP sync...");
            configTzTime("CST-8", "cn.pool.ntp.org", "time.nist.gov");
            return true;
        }

        case ExecActionType::ACTION_SYS_OTA:
            LOGGER.info("[PeriphExec] OTA action - reserved for future implementation");
            return true;

        case ExecActionType::ACTION_SYS_AP_PROVISION:
            LOGGER.info("[PeriphExec] AP provision action - reserved for future implementation");
            return true;

        case ExecActionType::ACTION_SYS_BLE_PROVISION:
            LOGGER.info("[PeriphExec] BLE provision action - reserved for future implementation");
            return true;

        default:
            LOGGER.warningf("[PeriphExec] Unknown system action: %d", action.actionType);
            return false;
    }
}

bool PeriphExecExecutor::executeScriptAction(const ExecAction& action) {
    if (action.actionValue.isEmpty()) {
        LOGGER.warning("[PeriphExec] Script is empty");
        return false;
    }

    auto cmds = ScriptEngine::parse(action.actionValue);
    if (cmds.empty()) {
        LOGGER.warning("[PeriphExec] Script parse failed");
        return false;
    }

    String errMsg;
    if (!ScriptEngine::validate(cmds, errMsg)) {
        LOGGER.warningf("[PeriphExec] Script validation failed: %s", errMsg.c_str());
        return false;
    }

    LOGGER.infof("[PeriphExec] Executing script (%d commands)", cmds.size());

    MQTTClient* mqtt = getMqttClient();
    return ScriptEngine::execute(cmds, mqtt);
}

bool PeriphExecExecutor::executeCallPeripheralAction(const ExecAction& action, const String& effectiveValue) {
    // 解析 actionValue 格式: {"periphId":"xxx","action":"on/off/toggle","value":"xxx"}
    if (action.actionValue.isEmpty() || action.actionValue.charAt(0) != '{') {
        LOGGER.warning("[PeriphExec] Call peripheral: invalid actionValue format");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, action.actionValue);
    if (err) {
        LOGGER.warningf("[PeriphExec] Call peripheral: JSON parse failed: %s", err.c_str());
        return false;
    }

    const char* targetPeriphId = doc["periphId"] | "";
    const char* actionCmd = doc["action"] | "";
    const char* valueStr = doc["value"] | "";

    if (strlen(targetPeriphId) == 0) {
        LOGGER.warning("[PeriphExec] Call peripheral: no target periphId specified");
        return false;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    if (!pm.hasPeripheral(targetPeriphId)) {
        LOGGER.warningf("[PeriphExec] Call peripheral: target not found: %s", targetPeriphId);
        return false;
    }

    LOGGER.infof("[PeriphExec] Call peripheral: %s action=%s value=%s",
        targetPeriphId, actionCmd, valueStr);

    String cmd = String(actionCmd);
    cmd.toLowerCase();

    if (cmd == "on" || cmd == "high" || cmd == "1") {
        pm.stopActionTicker(targetPeriphId);
        return pm.writePin(targetPeriphId, GPIOState::STATE_HIGH);
    } else if (cmd == "off" || cmd == "low" || cmd == "0") {
        pm.stopActionTicker(targetPeriphId);
        return pm.writePin(targetPeriphId, GPIOState::STATE_LOW);
    } else if (cmd == "toggle") {
        GPIOState current = pm.readPin(targetPeriphId);
        GPIOState next = (current == GPIOState::STATE_HIGH) ? GPIOState::STATE_LOW : GPIOState::STATE_HIGH;
        pm.stopActionTicker(targetPeriphId);
        return pm.writePin(targetPeriphId, next);
    } else if (cmd == "pwm") {
        uint32_t duty = String(valueStr).toInt();
        return pm.writePWM(targetPeriphId, duty);
    } else if (cmd == "blink") {
        uint16_t interval = String(valueStr).toInt();
        if (interval == 0) interval = 500;
        pm.startActionTicker(targetPeriphId, 1, interval);
        return true;
    } else if (cmd == "breathe") {
        uint16_t speed = String(valueStr).toInt();
        if (speed == 0) speed = 2000;
        pm.startActionTicker(targetPeriphId, 2, speed);
        return true;
    } else {
        LOGGER.warningf("[PeriphExec] Call peripheral: unknown action '%s'", actionCmd);
        return false;
    }
}

// ========== 工具方法 ==========

MQTTClient* PeriphExecExecutor::getMqttClient() {
    auto* fw = FastBeeFramework::getInstance();
    if (fw) {
        auto* pm = fw->getProtocolManager();
        if (pm) return pm->getMQTTClient();
    }
    return nullptr;
}

void PeriphExecExecutor::reportActionResults(const std::vector<ActionExecResult>& results) {
    if (results.empty()) return;

    // 构建上报数据：[{"id":"led1","value":"1","remark":"success"}, ...]
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (const auto& ar : results) {
        if (ar.targetPeriphId.isEmpty()) continue;
        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = ar.targetPeriphId;
        obj["value"] = ar.actualValue;
        obj["remark"] = ar.success ? "success" : "failed";
    }

    if (arr.size() == 0) return;

    String reportData;
    serializeJson(doc, reportData);

    // 通过 MQTT 上报
    MQTTClient* mqtt = getMqttClient();
    if (mqtt && mqtt->getIsConnected()) {
        bool ok = mqtt->publishReportData(reportData);
        LOGGER.infof("[PeriphExec] Report action results via MQTT: %s (%s)",
                     reportData.c_str(), ok ? "ok" : "failed");
    } else {
        LOGGER.debug("[PeriphExec] MQTT not connected, skip action results report");
    }
}


