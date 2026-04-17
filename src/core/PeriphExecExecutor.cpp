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

namespace {
bool tryParseBoolLike(const String& rawValue, bool& outValue) {
    String value = rawValue;
    value.trim();
    value.toLowerCase();
    if (value.isEmpty()) return false;

    if (value == "1" || value == "true" || value == "on" ||
        value == "high" || value == "open") {
        outValue = true;
        return true;
    }

    if (value == "0" || value == "false" || value == "off" ||
        value == "low" || value == "close") {
        outValue = false;
        return true;
    }

    if (value == "+1") {
        outValue = true;
        return true;
    }

    if (value == "-1") {
        outValue = false;
        return true;
    }

    bool isNumeric = true;
    bool hasDigit = false;
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value.charAt(i);
        if (c >= '0' && c <= '9') {
            hasDigit = true;
            continue;
        }
        if ((c == '+' || c == '-') && i == 0) {
            continue;
        }
        isNumeric = false;
        break;
    }

    if (isNumeric && hasDigit) {
        outValue = value.toInt() != 0;
        return true;
    }

    return false;
}

String normalizeBinaryReportValue(const String& preferredValue,
                                  const String& fallbackValue,
                                  bool defaultState) {
    bool parsedState = defaultState;
    if (tryParseBoolLike(preferredValue, parsedState) ||
        tryParseBoolLike(fallbackValue, parsedState)) {
        return parsedState ? "1" : "0";
    }
    return defaultState ? "1" : "0";
}

String normalizeScalarReportValue(const String& preferredValue,
                                  const String& fallbackValue,
                                  const char* defaultValue = "") {
    String reportValue = preferredValue;
    reportValue.trim();
    if (!reportValue.isEmpty()) return reportValue;

    reportValue = fallbackValue;
    reportValue.trim();
    if (!reportValue.isEmpty()) return reportValue;

    return String(defaultValue);
}

String buildModbusReportValue(uint16_t channel,
                              const String& preferredValue,
                              const String& fallbackValue,
                              bool defaultState) {
    return String(channel) + ":" +
           normalizeBinaryReportValue(preferredValue, fallbackValue, defaultState);
}

String buildActionReportValue(const ExecAction& action,
                              const String& effectiveValue,
                              const String& receivedValue) {
    ExecActionType actType = static_cast<ExecActionType>(action.actionType);
    switch (actType) {
        case ExecActionType::ACTION_HIGH:
        case ExecActionType::ACTION_HIGH_INVERTED:
            return normalizeBinaryReportValue(effectiveValue, receivedValue, true);

        case ExecActionType::ACTION_LOW:
        case ExecActionType::ACTION_LOW_INVERTED:
            return normalizeBinaryReportValue(effectiveValue, receivedValue, false);

        case ExecActionType::ACTION_SET_PWM:
        case ExecActionType::ACTION_SET_DAC:
            return normalizeScalarReportValue(effectiveValue, receivedValue, "0");

        case ExecActionType::ACTION_MODBUS_COIL_WRITE: {
            String rawValue = normalizeScalarReportValue(effectiveValue, receivedValue, "");
            uint16_t channel = 0;
            String stateValue = rawValue;
            int sepIdx = rawValue.indexOf(':');
            if (sepIdx > 0) {
                channel = rawValue.substring(0, sepIdx).toInt();
                stateValue = rawValue.substring(sepIdx + 1);
            }
            return buildModbusReportValue(channel, stateValue, receivedValue, false);
        }

        case ExecActionType::ACTION_MODBUS_REG_WRITE: {
            String rawValue = normalizeScalarReportValue(effectiveValue, receivedValue, "0");
            int sepIdx = rawValue.indexOf(':');
            if (sepIdx > 0) {
                uint16_t channel = rawValue.substring(0, sepIdx).toInt();
                String regValue = rawValue.substring(sepIdx + 1);
                regValue.trim();
                return String(channel) + ":" + regValue;
            }
            return rawValue;
        }

        default:
            return normalizeScalarReportValue(effectiveValue, receivedValue, "");
    }
}

void appendActionResult(std::vector<ActionExecResult>& results,
                        const String& targetPeriphId,
                        const String& actualValue,
                        bool success) {
    ActionExecResult result;
    result.targetPeriphId = targetPeriphId;
    result.actualValue = actualValue;
    result.success = success;
    results.push_back(result);
}

bool looksLikeHexPayload(const String& rawValue) {
    String value = rawValue;
    value.trim();
    if (value.length() < 4 || (value.length() % 2) != 0) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value.charAt(i);
        bool isHex =
            (c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F');
        if (!isHex) return false;
    }
    return true;
}
}

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
    // 外设动作（GPIO、Modbus子设备等统一由 PeripheralManager 管理）
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

        bool ok = false;
        std::vector<ActionExecResult> actionResults;
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ)) {
            ActionExecResult ar;
            ok = executeSensorReadAction(action, ar);
            ar.success = ok;
            actionResults.push_back(ar);
        } else if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_POLL)) {
            ok = executeModbusPollAction(action, rule, &actionResults);
            if (!ok && actionResults.empty()) {
                appendActionResult(actionResults, action.targetPeriphId, effectiveValue, false);
            }
        } else {
            ok = executeActionItem(action, effectiveValue);
            appendActionResult(
                actionResults,
                action.targetPeriphId,
                buildActionReportValue(action, effectiveValue, receivedValue),
                ok
            );
        }

        results.insert(results.end(), actionResults.begin(), actionResults.end());
    }

    // 精准上报执行结果
    if (rule.reportAfterExec && !results.empty()) {
        Serial.printf("[DEBUG] Before reportActionResults, heap=%d\n", ESP.getFreeHeap());
        reportActionResults(results);
        Serial.printf("[DEBUG] After reportActionResults, heap=%d\n", ESP.getFreeHeap());
    }

    Serial.printf("[DEBUG] Before return results, heap=%d\n", ESP.getFreeHeap());
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
            Serial.printf("[DEBUG] Execute HIGH on %s\n", action.targetPeriphId.c_str());
            pm.stopActionTicker(action.targetPeriphId);
            return pm.writePin(action.targetPeriphId, GPIOState::STATE_HIGH);
        }

        case ExecActionType::ACTION_LOW: {
            LOGGER.infof("[PeriphExec] Execute LOW on %s", action.targetPeriphId.c_str());
            Serial.printf("[DEBUG] Execute LOW on %s\n", action.targetPeriphId.c_str());
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

        // Modbus 线圈写入 (FC05) — 支持 "channel:state" 或 "state" 格式
        case ExecActionType::ACTION_MODBUS_COIL_WRITE: {
            PeripheralConfig* cfg = pm.getPeripheral(action.targetPeriphId);
            if (!cfg || !cfg->isModbusPeripheral()) {
                LOGGER.warningf("[PeriphExec] Modbus coil write: '%s' is not a Modbus peripheral",
                                action.targetPeriphId.c_str());
                return false;
            }
            uint16_t channel = 0;
            bool coilState = false;
            int sepIdx = effectiveValue.indexOf(':');
            if (sepIdx > 0) {
                channel = effectiveValue.substring(0, sepIdx).toInt();
                coilState = effectiveValue.substring(sepIdx + 1).toInt() != 0;
            } else {
                coilState = effectiveValue.toInt() != 0;
            }
            // 通过 PeripheralManager 的 Modbus 写入接口
            bool value = coilState;
            if (cfg->params.modbus.ncMode) value = !value;
            uint16_t coilAddr = cfg->params.modbus.coilBase + channel;
            if (cfg->params.modbus.controlProtocol == 0) {
                // 线圈模式 (FC05)
                LOGGER.infof("[PeriphExec] Modbus FC05: slave=%d coil=%d state=%d",
                    cfg->params.modbus.slaveAddress, coilAddr, value);
                return pm.writeModbusCoil(action.targetPeriphId, coilAddr, value);
            }
            // 寄存器协议模式 (FC06)
            uint16_t regVal = value ? 0xFF00 : 0x0000;
            LOGGER.infof("[PeriphExec] Modbus FC06 (coil): slave=%d reg=%d val=0x%04X",
                cfg->params.modbus.slaveAddress, coilAddr, regVal);
            return pm.writeModbusReg(action.targetPeriphId, coilAddr, regVal);
        }

        // Modbus 寄存器写入 (FC06) — 支持 "addr:value" 或 "value" 格式
        case ExecActionType::ACTION_MODBUS_REG_WRITE: {
            PeripheralConfig* cfg = pm.getPeripheral(action.targetPeriphId);
            if (!cfg || !cfg->isModbusPeripheral()) {
                LOGGER.warningf("[PeriphExec] Modbus reg write: '%s' is not a Modbus peripheral",
                                action.targetPeriphId.c_str());
                return false;
            }
            uint16_t regAddr = cfg->params.modbus.coilBase;
            uint16_t regVal = 0;
            int sepIdx = effectiveValue.indexOf(':');
            if (sepIdx > 0) {
                regAddr = effectiveValue.substring(0, sepIdx).toInt();
                regVal = effectiveValue.substring(sepIdx + 1).toInt();
            } else {
                regVal = effectiveValue.toInt();
            }
            LOGGER.infof("[PeriphExec] Modbus FC06: slave=%d reg=%d val=%d",
                cfg->params.modbus.slaveAddress, regAddr, regVal);
            return pm.writeModbusReg(action.targetPeriphId, regAddr, regVal);
        }

        default:
            LOGGER.warningf("[PeriphExec] Unknown peripheral action: %d", action.actionType);
            return false;
    }
}

bool PeriphExecExecutor::executeModbusPollAction(const ExecAction& action,
                                                 const PeriphExecRule& rule,
                                                 std::vector<ActionExecResult>* controlResults) {
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
                    bool desiredState = false;
                    if (!tryParseBoolLike(String(act), desiredState)) {
                        desiredState = (strcmp(act, "on") == 0);
                    }
                    bool coilVal = dev.ncMode ? !desiredState : desiredState;
                    uint16_t addr = dev.coilBase + ch;
                    bool ok = false;
                    if (dev.controlProtocol == 0) {
                        auto res = modbus->writeCoilOnce(dev.slaveAddress, addr, coilVal, true);
                        ok = (res.error == ONESHOT_SUCCESS);
                        anySuccess = anySuccess || ok;
                        LOGGER.infof("[PeriphExec] Relay ctrl: slave=%d coil=%d val=%d ok=%d",
                            dev.slaveAddress, addr, coilVal, ok);
                    } else {
                        uint16_t regVal = coilVal ? 0xFF00 : 0x0000;
                        auto res = modbus->writeRegisterOnce(dev.slaveAddress, addr, regVal, true);
                        ok = (res.error == ONESHOT_SUCCESS);
                        anySuccess = anySuccess || ok;
                        LOGGER.infof("[PeriphExec] Relay reg ctrl: slave=%d reg=%d val=0x%04X ok=%d",
                            dev.slaveAddress, addr, regVal, ok);
                    }
                    if (controlResults) {
                        appendActionResult(*controlResults, "modbus:" + String(devIdx),
                                           buildModbusReportValue(ch, "", String(desiredState ? 1 : 0),
                                                                  desiredState),
                                           ok);
                    }
                } else if (devType == "pwm") {
                    uint16_t regAddr = dev.pwmRegBase + ch;
                    auto res = modbus->writeRegisterOnce(dev.slaveAddress, regAddr, val, true);
                    bool ok = (res.error == ONESHOT_SUCCESS);
                    anySuccess = anySuccess || ok;
                    LOGGER.infof("[PeriphExec] PWM ctrl: slave=%d reg=%d val=%d ok=%d",
                        dev.slaveAddress, regAddr, val, ok);
                    if (controlResults) {
                        appendActionResult(*controlResults, "modbus:" + String(devIdx),
                                           String(ch) + ":" + String(val), ok);
                    }
                } else if (devType == "pid") {
                    const char* param = cv["p"] | "P";
                    uint8_t pidIdx = 3;
                    if (strcmp(param, "I") == 0) pidIdx = 4;
                    else if (strcmp(param, "D") == 0) pidIdx = 5;
                    uint16_t regAddr = dev.pidAddrs[pidIdx];
                    auto res = modbus->writeRegisterOnce(dev.slaveAddress, regAddr, val, true);
                    bool ok = (res.error == ONESHOT_SUCCESS);
                    anySuccess = anySuccess || ok;
                    LOGGER.infof("[PeriphExec] PID ctrl: slave=%d param=%s reg=%d val=%d ok=%d",
                        dev.slaveAddress, param, regAddr, val, ok);
                    if (controlResults) {
                        appendActionResult(*controlResults, "modbus:" + String(devIdx),
                                           String(ch) + ":" + String(val), ok);
                    }
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

    // 检查 Modbus 传输类型（0=JSON, 1=透传HEX）并获取 ModbusHandler 指针
    uint8_t modbusTransferType = 0;
    ModbusHandler* modbus = nullptr;
    auto* fw = FastBeeFramework::getInstance();
    if (fw) {
        auto* protMgr = fw->getProtocolManager();
        if (protMgr) {
            modbus = protMgr->getModbusHandler();
            if (modbus) {
                ModbusConfig cfg = modbus->getConfig();
                modbusTransferType = cfg.transferType;
            }
        }
    }

    PeripheralManager& pm = PeripheralManager::getInstance();

    // 构建上报数据：[{"id":"led1","value":"1","remark":""}, ...]
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (const auto& ar : results) {
        if (ar.targetPeriphId.isEmpty() || !ar.success) continue;

        // 检查目标外设是否为 Modbus 子设备，且传输类型为透传 HEX
        bool isModbusTarget = false;
        const PeripheralConfig* cfg = pm.getPeripheral(ar.targetPeriphId);
        if (!cfg) continue;
        if (cfg && cfg->isModbusPeripheral()) {
            isModbusTarget = true;
        }

        // 确定上报 ID 和 value
        String reportId = ar.targetPeriphId;
        String reportValue = ar.actualValue;

        if (isModbusTarget && cfg && modbus && cfg->params.modbus.sensorId[0] != '\0') {
            // 从 actualValue 解析 channel（格式 "channel:state" 或 "state"）
            uint16_t channel = 0;
            int sepIdx = ar.actualValue.indexOf(':');
            if (sepIdx > 0) {
                channel = ar.actualValue.substring(0, sepIdx).toInt();
                reportValue = ar.actualValue.substring(sepIdx + 1);
            }
            String sid = modbus->buildSensorId(cfg->params.modbus.deviceIndex, channel);
            if (!sid.isEmpty()) {
                reportId = sid;
            }
        }

        JsonObject obj = arr.add<JsonObject>();
        if (isModbusTarget && modbusTransferType == 1 && looksLikeHexPayload(ar.actualValue)) {
            // 透传模式：直接上报原始 HEX 值
            obj["id"] = reportId;
            obj["value"] = ar.actualValue;
            obj["remark"] = "";
        } else {
            obj["id"] = reportId;
            obj["value"] = reportValue;
            obj["remark"] = "";
        }
    }

    if (arr.size() == 0) return;

    String reportData;
    serializeJson(doc, reportData);

    // 通过 MQTT 上报（使用线程安全队列，避免从异步任务直接调用 PubSubClient）
    MQTTClient* mqtt = getMqttClient();
    if (mqtt && mqtt->getIsConnected()) {
        bool ok = mqtt->queueReportData(reportData);
        LOGGER.infof("[PeriphExec] Report action results queued: %s (%s)",
                     reportData.c_str(), ok ? "ok" : "queue_failed");
    } else {
        LOGGER.debug("[PeriphExec] MQTT not connected, skip action results report");
    }
}


