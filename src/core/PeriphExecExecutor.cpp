/**
 * @file PeriphExecExecutor.cpp
 * @brief 外设执行动作执行器实现
 * @author kerwincui
 * @copyright FastBee All rights reserved.
 */

#include "core/FeatureFlags.h"
#include "network/LiteralStringMatch.h"
#if FASTBEE_ENABLE_PERIPH_EXEC

#include <cctype>
#include <cstring>
#include "core/PeriphExecExecutor.h"
#include "core/NativeOutputDispatch.h"
#include "core/MotionOutputDispatch.h"
#include "core/RuleOutputIdentity.h"
#include "core/Pca9685ConfigSupport.h"
#include "core/RfOutputAction.h"
#include "core/ActionValueContract.h"
#include "core/ReportInstanceBinding.h"
#include "core/SensorConfigEpoch.h"
#include "core/MotorControlService.h"
#include "core/PeriphExecManager.h"
#if FASTBEE_ENABLE_REMOTE_RESOURCES
#include "protocols/RemoteResourceManager.h"
#endif
#include "core/RuleScript.h"
#include "core/ScriptEngine.h"
#include "core/FastBeeFramework.h"
#include "utils/StringUtils.h"
#include "utils/CheckedText.h"
#include "utils/SystemTimeQuality.h"
#include "utils/SystemTimeZone.h"
#include "protocols/ProtocolManager.h"
#if FASTBEE_ENABLE_MODBUS
#include "protocols/ModbusHandler.h"
#endif
#if FASTBEE_ENABLE_MQTT
#include "protocols/MQTTClient.h"
#endif
#include "systems/LoggerSystem.h"
#include "systems/ConfigStorage.h"
#include "systems/HealthMonitor.h"
#include "systems/SystemRebooter.h"
#include "core/MemoryBudget.h"
#include "core/RuntimeFeatureManager.h"
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include "core/PeripheralManager.h"
#include "core/ActionReportPayload.h"
#if FASTBEE_ENABLE_MQTT
static_assert(FastBee::kActionReportPayloadBytes == MQTT_SLOT_MAX_PAYLOAD,
              "Action report serialization must match the MQTT slot budget");
#endif
#include "core/ChipConfig.h"
#if FASTBEE_ENABLE_SENSOR_DRIVER
#include "peripherals/SensorDriver.h"
#endif
#include "core/SensorDriverCache.h"
#include "core/SensorConfigSupport.h"
#include "core/Ds18b20Selection.h"
#include "core/DriverRegistry.h"
#include "network/WebConfigManager.h"
#include "network/NetworkManager.h"
#if FASTBEE_ENABLE_LCD
#include "peripherals/LCDManager.h"
#endif
#if FASTBEE_ENABLE_SEVEN_SEGMENT
#include "peripherals/SevenSegmentDriver.h"
#endif
#include <freertos/task.h>

struct PreparedSensorRead {
    ObservationUpdate observation;
    uint32_t configVersion = 0;
    float processed = 0;
    uint8_t decimals = 0;
};

namespace {
#if FASTBEE_ENABLE_SENSOR_DRIVER
// Preserve each branch's snapshot point and warning; do not reuse boundConfig.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noclone))
#elif defined(__clang__)
__attribute__((noinline))
#endif
bool loadSensorReadSnapshot(PeripheralManager& pm, const char* periphId, PeripheralConfig& cfg) {
    if (!pm.getPeripheralSnapshot(String(periphId), cfg)) {
        LOGGER.warningf("[PeriphExec] Sensor read: peripheral '%s' not found", periphId);
        return false;
    }
    return true;
}

#if FASTBEE_ENABLE_LCD
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noclone))
#elif defined(__clang__)
__attribute__((noinline))
#endif
void invalidateSensorReadDisplay(const char* periphId, const char* dataField) {
    if (LCDManager::getInstance().isInitialized()) {
        LCDManager::getInstance().invalidateSensorEntry(String(periphId) + "_" + String(dataField));
    }
}
#endif
#endif

size_t sensorReadGroupSize(const PeriphExecRule& rule, size_t first) {
    const auto& initial = rule.actions[first];
    if (first + 1 >= rule.actions.size() || initial.syncDelayMs || initial.useReceivedValue ||
        initial.targetPeriphId.isEmpty()) return 1;
    const auto& next = rule.actions[first + 1];
    if (next.actionType != static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ) ||
        next.syncDelayMs || next.useReceivedValue || next.execMode != initial.execMode ||
        next.targetPeriphId != initial.targetPeriphId) return 1;
    PeripheralConfig config;
    if (!PeripheralManager::getInstance().getPeripheralSnapshot(initial.targetPeriphId, config) ||
        !config.enabled || config.type != PeripheralType::SENSOR_GENERIC) return 1;
    const auto* metadata = FastBee::DriverRegistry::getInstance().getMetadata(config.params.sensor.driver);
    const size_t limit = metadata ? std::min<size_t>(4, metadata->activeChannelCount(config.params.sensor.mode, config.params.sensor.scanMask)) : 0;
    if (limit < 2) return 1;
    String channels[4];
    String mappings[4];
    size_t count = 0;
    for (size_t i = first; i < rule.actions.size() && count < limit; ++i) {
        const auto& action = rule.actions[i];
        if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ) ||
            action.syncDelayMs || action.useReceivedValue || action.execMode != initial.execMode ||
            action.targetPeriphId != initial.targetPeriphId) break;
        JsonDocument parameters;
        if (deserializeJson(parameters, action.actionValue.c_str(), action.actionValue.length()) ||
            initial.targetPeriphId != (parameters["periphId"] | "")) break;
        const char* channel = parameters["channelField"] | "";
        String selected, error;
        if (!channel[0] || !FastBee::resolveSensorChannel(config.params.sensor.driver,
                config.params.sensor.mode, channel, selected, error, config.params.sensor.scanMask)) break;
        bool duplicate = false;
        const char* mapping = parameters["dataField"] | "";
        for (size_t j = 0; j < count; ++j) {
            duplicate |= channels[j] == selected || (mapping[0] && mappings[j] == mapping);
        }
        if (duplicate) break;
        mappings[count] = mapping;
        channels[count++] = selected;
    }
    return count > 1 ? count : 1;
}

#if FASTBEE_ENABLE_MODBUS
// Share parsing/copying only. The callers retain their distinct failure policy.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noclone))
#elif defined(__clang__)
__attribute__((noinline))
#endif
bool appendModbusPollReadings(JsonDocument& taskDoc, const String& encoded, JsonArray resultsArr) {
    if (deserializeJson(taskDoc, encoded.c_str(), encoded.length())) return false;
    JsonArray taskArr = taskDoc.as<JsonArray>();
    for (JsonVariant item : taskArr) {
        resultsArr.add(item);
    }
    return true;
}
#endif

bool tryParseBoolLike(const String& rawValue, bool& outValue) {
    return StringUtils::tryParseBoolLike(rawValue, outValue);
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
    char buf[16];
    snprintf(buf, sizeof(buf), "%u:%s", channel,
             normalizeBinaryReportValue(preferredValue, fallbackValue, defaultState).c_str());
    return String(buf);
}


/** 判断动作是否产生可作为设备属性上报的控制结果。
 *  采集类（SENSOR_READ/MODBUS_POLL）在调用处单独标记；系统指令、瞬时传输、
 *  规则控制和显示类动作不代表设备属性，不进入属性上报。 */
bool isReportableOutputAction(uint8_t actionType) {
    switch (static_cast<ExecActionType>(actionType)) {
        case ExecActionType::ACTION_HIGH:
        case ExecActionType::ACTION_LOW:
        case ExecActionType::ACTION_HIGH_INVERTED:
        case ExecActionType::ACTION_LOW_INVERTED:
        case ExecActionType::ACTION_GPIO_TOGGLE:
        case ExecActionType::ACTION_SET_PWM:
        case ExecActionType::ACTION_SET_DAC:
        case ExecActionType::ACTION_NEOPIXEL_EFFECT:
        case ExecActionType::ACTION_MODBUS_COIL_WRITE:
        case ExecActionType::ACTION_MODBUS_REG_WRITE:
            return true;
        default:
            return false;
    }
}

/** 启动状态同步只能读取不会产生副作用的 GPIO 输出状态。
 *  NeoPixel 和 Modbus 写动作虽然运行后可上报，但不能在启动阶段按 GPIO 回读。 */
bool isBootReadableGpioOutputAction(uint8_t actionType) {
    switch (static_cast<ExecActionType>(actionType)) {
        case ExecActionType::ACTION_HIGH:
        case ExecActionType::ACTION_LOW:
        case ExecActionType::ACTION_HIGH_INVERTED:
        case ExecActionType::ACTION_LOW_INVERTED:
        case ExecActionType::ACTION_GPIO_TOGGLE:
        case ExecActionType::ACTION_SET_PWM:
        case ExecActionType::ACTION_SET_DAC:
            return true;
        default:
            return false;
    }
}

/** 判断输出动作是否为“电平反转”类（低电平有效外设，如低电平点亮的 LED / 继电器）。
 *  反转与否完全由规则动作配置决定，不在代码中对具体外设的电平极性做任何硬编码假设。 */
bool isInvertedOutputAction(uint8_t actionType) {
    ExecActionType t = static_cast<ExecActionType>(actionType);
    return t == ExecActionType::ACTION_HIGH_INVERTED ||
           t == ExecActionType::ACTION_LOW_INVERTED;
}

/** 将“物理引脚电平”翻译为“上报逻辑值”，与正常执行经 buildActionReportValue 的语义一致：
 *  非反转外设 物理HIGH=逻辑"1"；反转外设（低电平有效）物理LOW=逻辑"1"。
 *  反转与否由动作配置决定，不假设 LED 总是高/低电平点亮，保证不同接法的外设通用。 */
const char* physicalStateToLogicalValue(GPIOState physical, uint8_t actionType) {
    bool physicalHigh = (physical == GPIOState::STATE_HIGH);
    bool logicalOn = physicalHigh ^ isInvertedOutputAction(actionType);
    return logicalOn ? "1" : "0";
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

        case ExecActionType::ACTION_GPIO_TOGGLE: {
            // 翻转结果取决于执行前电平，执行后回读实际引脚状态作为上报值；
            // 极性按非反转语义翻译（与 CALL_PERIPHERAL "toggle" 上报行为一致）
            GPIOState physical = PeripheralManager::getInstance().readPin(action.targetPeriphId);
            if (physical != GPIOState::STATE_UNDEFINED) {
                return String(physicalStateToLogicalValue(physical, action.actionType));
            }
            return normalizeBinaryReportValue(effectiveValue, receivedValue, false);
        }

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
                char buf[24];
                snprintf(buf, sizeof(buf), "%u:%s", channel, regValue.c_str());
                return String(buf);
            }
            return rawValue;
        }

        default: {
            // 模板类动作（如 OLED 显示 actionValue="$value"）：上报值必须是替换后的实际内容，
            // 否则平台会收到字面量 "$value"（显示正确但上报错误的历史缺陷根因）
            String reportValue = effectiveValue;
            if (!receivedValue.isEmpty() && reportValue.indexOf("$value") >= 0) {
                reportValue.replace("$value", receivedValue);
            }
            return normalizeScalarReportValue(reportValue, receivedValue, "");
        }
    }
}

void appendActionResult(std::vector<ActionExecResult>& results,
                        const String& targetPeriphId,
                        const String& actualValue,
                        bool success,
                        const String& remark = "") {
    ActionExecResult result;
    if (strlen(targetPeriphId.c_str()) == targetPeriphId.length())
        FastBee::captureLiteralReportIdentity(result, targetPeriphId.c_str());
    FastBee::completeCapturedReport(result, actualValue, success, remark);
    results.push_back(std::move(result));
}

// Capture output mapping before callbacks/I/O. A changed/busy configuration
// owner suppresses this report; it cannot undo an already executed output.
void prepareOutputReport(const ExecAction& action, const String& effectiveValue,
                          const String& receivedValue, ActionExecResult& binding,
                          uint64_t& owner, bool& stripChannel) {
    auto& pm = PeripheralManager::getInstance();
    owner = pm.getConfigurationReviewVersion();
    stripChannel = false;
    PeripheralConfig config;
    if (!owner || strlen(action.targetPeriphId.c_str()) != action.targetPeriphId.length() ||
        !pm.getPeripheralSnapshot(action.targetPeriphId, config) ||
        pm.getConfigurationReviewVersion() != owner) return;
    if (!config.isModbusPeripheral()) {
        FastBee::captureLiteralReportIdentity(binding, action.targetPeriphId.c_str());
        return;
    }
    uint16_t channel = 0;
    size_t offset = 0;
    const auto type = static_cast<ExecActionType>(action.actionType);
    const bool channelCommand = type == ExecActionType::ACTION_MODBUS_COIL_WRITE ||
        type == ExecActionType::ACTION_MODBUS_REG_WRITE;
    if (channelCommand) {
        const String value = normalizeScalarReportValue(effectiveValue, receivedValue, "");
        if (!FastBee::splitModbusReportValue(value, channel, offset)) return;
    }
    FastBee::captureModbusReportIdentity(binding, action.targetPeriphId.c_str(),
        config.params.modbus.sensorId, config.params.modbus.channelCount, channel);
    stripChannel = channelCommand && binding.reportIdentity == FastBee::ActionReportIdentity::Field;
}

void finishOutputReport(ActionExecResult& result, ActionExecResult&& binding,
                         uint64_t owner, bool stripChannel) {
    if (result.reportIdentity == FastBee::ActionReportIdentity::Invalid) return;
    if (!owner || PeripheralManager::getInstance().getConfigurationReviewVersion() != owner) {
        result.reportIdentity = FastBee::ActionReportIdentity::Invalid;
        result.reportIdBytes = 0;
        return;
    }
    if (stripChannel) {
        uint16_t channel = 0;
        size_t offset = 0;
        if (!FastBee::splitModbusReportValue(result.actualValue, channel, offset)) {
            binding.reportIdentity = FastBee::ActionReportIdentity::Invalid;
        } else if (offset) {
            String value = result.actualValue.substring(offset);
            if (value.length() != result.actualValue.length() - offset)
                binding.reportIdentity = FastBee::ActionReportIdentity::Invalid;
            else result.actualValue = std::move(value);
        }
    }
    result.reportIdentity = binding.reportIdentity;
    result.reportIdBytes = binding.reportIdBytes;
    result.targetPeriphId = std::move(binding.targetPeriphId);
    result.dataField = std::move(binding.dataField);
}

template <size_t N>
void copyTraceText(char (&destination)[N], const String& source) {
    snprintf(destination, N, "%s", source.c_str());
}

bool operationDeadlineReached(unsigned long deadlineAt, unsigned long now = millis()) {
    return deadlineAt != 0 && static_cast<int32_t>(now - deadlineAt) >= 0;
}

void appendActionTrace(AsyncExecResult* traceOut,
                       size_t actionIndex,
                       const ExecAction& action,
                       const String& valueSummary,
                       bool success,
                       unsigned long durationMs,
                       size_t detailCount,
                       const String& remark) {
    if (!traceOut) return;
    if (traceOut->actionTraceCount >= ASYNC_EXEC_ACTION_TRACE_LIMIT) {
        traceOut->actionTraceTruncated = true;
        return;
    }

    AsyncExecActionTrace& trace =
        traceOut->actionTraces[traceOut->actionTraceCount++];
    trace.index = static_cast<uint8_t>(actionIndex > 255 ? 255 : actionIndex);
    trace.actionType = action.actionType;
    trace.success = success;
    trace.durationMs = durationMs;
    trace.detailCount = static_cast<uint8_t>(detailCount > 255 ? 255 : detailCount);
    copyTraceText(trace.targetPeriphId, action.targetPeriphId);
    copyTraceText(trace.valueSummary, valueSummary);
    copyTraceText(trace.remark, remark);
}

const String* safeActionTraceValue(const ExecAction& action,
                                   const String& effectiveValue,
                                   const std::vector<ActionExecResult>& actionResults) {
    switch (static_cast<ExecActionType>(action.actionType)) {
        case ExecActionType::ACTION_HIGH:
        case ExecActionType::ACTION_LOW:
        case ExecActionType::ACTION_BLINK:
        case ExecActionType::ACTION_BREATHE:
        case ExecActionType::ACTION_SET_PWM:
        case ExecActionType::ACTION_SET_DAC:
        case ExecActionType::ACTION_NEOPIXEL_EFFECT:
        case ExecActionType::ACTION_STEPPER_CONTROL:
        case ExecActionType::ACTION_HIGH_INVERTED:
        case ExecActionType::ACTION_LOW_INVERTED:
        case ExecActionType::ACTION_MODBUS_COIL_WRITE:
        case ExecActionType::ACTION_MODBUS_REG_WRITE:
        case ExecActionType::ACTION_MODBUS_POLL:
        case ExecActionType::ACTION_SENSOR_READ:
        case ExecActionType::ACTION_DISPLAY_NUMBER:
        case ExecActionType::ACTION_GPIO_TOGGLE:
        case ExecActionType::ACTION_RTC_READ:
        case ExecActionType::ACTION_RTC_SET:
        case ExecActionType::ACTION_SD_STATUS:
            return actionResults.empty()
                ? &effectiveValue : &actionResults.front().actualValue;
        default:
            // Scripts, OTA URLs, generic peripheral/UART/RF payloads and free
            // text may contain credentials or private data. Their presence is
            // traceable without retaining the payload in the authenticated
            // read-only results endpoint.
            return nullptr;
    }
}

/** 判断字符串是否全为数字（非空），用于识别平台直接下发的数值指令（如占空比 125）。 */
bool isAllDigits(const String& s) {
    if (s.isEmpty()) return false;
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s.charAt(i);
        if (c < '0' || c > '9') return false;
    }
    return true;
}

/** 判断 CALL_PERIPHERAL 动作的目标外设是否为可上报控制结果的输出类。
 *  此类调用语义上等同 GPIO 控制动作，执行结果同样应作为设备属性上报平台，
 *  否则平台下发 LED、蜂鸣器或 NeoPixel 控制后状态永远不同步。 */
bool isCallPeripheralOutputAction(const ExecAction& action) {
    if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL)) return false;
    if (action.targetPeriphId.isEmpty()) return false;
    PeripheralConfig cfg;
    if (!PeripheralManager::getInstance().getPeripheralSnapshot(action.targetPeriphId, cfg)) return false;
    return cfg.type == PeripheralType::GPIO_DIGITAL_OUTPUT ||
           cfg.type == PeripheralType::GPIO_PWM_OUTPUT ||
           cfg.type == PeripheralType::GPIO_ANALOG_OUTPUT ||
           cfg.type == PeripheralType::NEO_PIXEL;
}

/** CALL_PERIPHERAL 的启动状态同步仅限能够无副作用回读引脚的 GPIO 输出。 */
bool isBootReadableCallPeripheralOutputAction(const ExecAction& action) {
    if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL)) return false;
    if (action.targetPeriphId.isEmpty()) return false;
    PeripheralConfig cfg;
    if (!PeripheralManager::getInstance().getPeripheralSnapshot(action.targetPeriphId, cfg)) return false;
    return cfg.type == PeripheralType::GPIO_DIGITAL_OUTPUT ||
           cfg.type == PeripheralType::GPIO_PWM_OUTPUT ||
           cfg.type == PeripheralType::GPIO_ANALOG_OUTPUT;
}

}

PeriphExecExecutor::PeriphExecExecutor() = default;
PeriphExecExecutor::~PeriphExecExecutor() = default;

// ========== Modbus 缓冲池管理 ==========

ModbusBuffer* PeriphExecExecutor::acquireBuffer() {
    for (uint8_t i = 0; i < MODBUS_BUFFER_POOL_SIZE; i++) {
        if (!_bufferPool[i].inUse) {
            _bufferPool[i].inUse = true;
            memset(_bufferPool[i].data, 0, sizeof(_bufferPool[i].data));
            return &_bufferPool[i];
        }
    }
    LOGGER.warning("[EXEC] Modbus buffer pool exhausted! All 4 buffers in use");
    return nullptr;
}

void PeriphExecExecutor::releaseBuffer(ModbusBuffer* buf) {
    if (buf) {
        buf->release();
    }
}

void PeriphExecExecutor::initialize(PeriphExecManager* manager) {
    _manager = manager;
}

// ========== 动作执行接口 ==========

bool PeriphExecExecutor::executeActionItem(const ExecAction& action, const String& effectiveValue,
                                           const String& receivedValue, const FastBee::UartEventOrigin& uartOrigin,
                                           bool* sourceUnconfirmed,
                                           const FastBee::OrdinaryControlContext* control,
                                           FastBee::OrdinaryControlResult* controlResult) {
    if (sourceUnconfirmed) *sourceUnconfirmed = false;
    if (!PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) {
        if (sourceUnconfirmed) *sourceUnconfirmed = true;
        return false;
    }
    if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SD_STATUS)) {
        ActionExecResult result;
        return executeSdStatusAction(action, result);
    }
    if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_RTC_CONTROL)) {
        return executeRtcAction(action);
    }
    if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_RTC_READ)) {
        ActionExecResult result;
        return executeRtcReadAction(action, result);
    }
    if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_RTC_SET)) {
        ActionExecResult result;
        return executeRtcSetAction(action, result);
    }
    if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_CHANNEL)) {
        LOGGER.warning("[PeriphExec] Remote channel action is retired");
        return false;
    }
#if FASTBEE_ENABLE_REMOTE_RESOURCES
    if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_READ)) {
        String error;
        const bool accepted = FastBee::RemoteResourceManager::instance().requestRead(
            action.targetPeriphId, error);
        if (!accepted) {
            LOGGER.warningf("[PeriphExec] Remote read '%s' rejected: %s",
                            action.targetPeriphId.c_str(), error.c_str());
        }
        return accepted;
    }
#else
    if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_CHANNEL) ||
        action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_READ)) {
        return false;
    }
#endif
    // 调用其他外设 (actionType 10) 必须先于系统动作范围判断。
    if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL)) {
        return executeCallPeripheralAction(action, effectiveValue, receivedValue, nullptr, 0, uartOrigin, sourceUnconfirmed, control, controlResult);
    }
    // 专用外设控制动作 (11=灯效控制, 12=电机控制, 28=射频发送, 29=串口发送)
    if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_NEOPIXEL_EFFECT) ||
        action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_STEPPER_CONTROL) ||
        action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_RF_SEND) ||
        action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_UART_SEND)) {
        return executeCallPeripheralAction(action, effectiveValue, receivedValue, nullptr, 0, uartOrigin, sourceUnconfirmed, control, controlResult);
    }
    // 系统功能 (actionType 6-9) 与运行时功能开关 (actionType 31/32)
    if ((action.actionType >= 6 && action.actionType <= 9) ||
        action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_ENABLE) ||
        action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_DISABLE)) {
        return executeSystemAction(action, uartOrigin, sourceUnconfirmed);
    }
    // 命令脚本 (actionType 15)
    if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT)) {
        return executeScriptAction(action, uartOrigin, sourceUnconfirmed, control, controlResult);
    }
    // 外设执行规则启用/禁用 (actionType 22/23)
    if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_ENABLE_EXEC_RULE) ||
        action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_DISABLE_EXEC_RULE)) {
        return executeRuleControlAction(action);
    }
    // 外设动作（GPIO、Modbus子设备等统一由 PeripheralManager 管理）
    // receivedValue 透传：供 ACTION_OLED_DISPLAY 等需要访问原始下发值的动作替换 $value 占位符
    return executePeripheralAction(action, effectiveValue, receivedValue, uartOrigin, control, controlResult);
}

std::vector<ActionExecResult> PeriphExecExecutor::executeAllActions(
    const PeriphExecRule& rule, const String& receivedValue, bool suppressReport,
    size_t* reportCountOut, bool* completedAllOut, unsigned long deadlineAt,
    AsyncExecResult* traceOut) {

    bool localCompleted = true;
    bool ordinaryFailed = false;
    if (!completedAllOut) completedAllOut = &localCompleted;
    *completedAllOut = true;
    FastBee::OrdinaryControlContext localControl;
    auto* ordinaryControl = traceOut ? &traceOut->ordinaryControl : &localControl;
    if (!traceOut) {
        PeripheralManager::getInstance().beginOrdinaryControl(
            FastBee::OrdinaryControl::Source::Rule, rule.outputActor, 0, false, 0, String(),
            FastBee::SensorConfigEpoch::current(), {}, localControl);
        localControl.ruleBound = true;
        localControl.ruleEpoch = FastBee::RuleOutputIdentity::currentEpoch();
    }
    // Carry the operation's existing deadline through trusted output context.
    // RF checks it at frame boundaries; no new duration or retry budget exists.
    ordinaryControl->operationDeadlineAt = static_cast<uint32_t>(deadlineAt);
    // Keep one operation through actions/delays. Only terminal acknowledgement
    // permits a surviving producer to become independently owned.
    struct OrdinaryTerminal {
        const FastBee::OrdinaryControlContext& context;
        const bool& completed;
        const bool& failed;
        ~OrdinaryTerminal() {
            PeripheralManager::getInstance().finishOrdinaryControl(context, !completed || failed);
        }
    } ordinaryTerminal{*ordinaryControl, *completedAllOut, ordinaryFailed};
    if (traceOut) traceOut->ordinaryControlResult = FastBee::OrdinaryControlResult{};

    const uint32_t collectionStartedAt = static_cast<uint32_t>(millis());
    const uint32_t reportOrigin = rule.reportAfterExec && !suppressReport
        ? captureReportInstanceId() : 0;
    // Queue-backed operations carry their admission epoch. Synchronous direct
    // callers bind at the start of the entire rule, before delays/earlier actions.
    const uint32_t dispatchConfigEpoch = traceOut && traceOut->peripheralConfigEpoch
        ? traceOut->peripheralConfigEpoch : FastBee::SensorConfigEpoch::current();
    if (completedAllOut) *completedAllOut = true;
    if (traceOut) {
        traceOut->actionCount = static_cast<uint8_t>(
            rule.actions.size() > 255 ? 255 : rule.actions.size());
        traceOut->actionTraceCount = 0;
        traceOut->actionTraceTruncated =
            rule.actions.size() > ASYNC_EXEC_ACTION_TRACE_LIMIT;
    }
    std::vector<ActionExecResult> results;
    results.reserve(rule.actions.size());
    // 可上报动作的结果进入上报队列：采集类（SENSOR_READ / MODBUS_POLL）
    // 以及属性控制类（GPIO/PWM/DAC、NeoPixel、Modbus 写）。系统指令、
    // 瞬时传输、规则控制和显示类动作不上报。
    std::vector<ActionExecResult> reportableResults;

    // Recheck captured source after queue waits, earlier actions and delays.
    // The PM lock is released by the validator before arbitrary callbacks/I/O.
    // This deliberately does not claim atomicity with each hardware action.
    auto sourceCurrent = [&](size_t index, const ExecAction& action) {
        if (!_manager || !_manager->isRuleExecutionCurrent(rule)) {
            if (completedAllOut) *completedAllOut = false;
            if (traceOut) traceOut->errorCode = AsyncExecError::RULE_REVISION_UNCONFIRMED;
            appendActionTrace(traceOut, index, action, "", false, 0, 0, "rule_revision_unconfirmed");
            return false;
        }
        if (!traceOut || PeripheralManager::getInstance().isUartEventOriginCurrent(traceOut->uartOrigin)) return true;
        if (completedAllOut) *completedAllOut = false;
        traceOut->errorCode = AsyncExecError::UART_SOURCE_UNCONFIRMED;
        appendActionTrace(traceOut, index, action, "", false, 0, 0, "uart_source_unconfirmed");
        return false;
    };

    for (size_t actionIndex = 0; actionIndex < rule.actions.size(); ++actionIndex) {
        const auto& action = rule.actions[actionIndex];
        if (!sourceCurrent(actionIndex, action)) break;
        if (operationDeadlineReached(deadlineAt)) {
            if (completedAllOut) *completedAllOut = false;
            if (traceOut) traceOut->errorCode = AsyncExecError::DEADLINE_EXCEEDED;
            appendActionTrace(traceOut, actionIndex, action, "", false, 0, 0,
                              "deadline_exceeded");
            break;
        }

        // MemGuard 堆守卫：使用 DRAM（MALLOC_CAP_INTERNAL）而非总堆，避免 PSRAM 干扰判断；
        // 2026-08-05 fix: 另加 8BIT 口径（INTERNAL 含 IRAM/RTC 虚高 ~28KB，
        // 崩溃现场 INTERNAL 29KB 时 8BIT 仅剩 <1KB，传感器 JSON 解析小分配直接 abort）
        auto* fw = FastBeeFramework::getInstance();
        HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
        uint32_t dramFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        uint32_t dram8Free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        if (monitor) {
            if (monitor->isMemoryCritical()) {
                LOGGER.warning("[PeriphExec] Heap CRITICAL, skipping remaining actions");
                if (completedAllOut) *completedAllOut = false;
                if (traceOut) traceOut->errorCode = AsyncExecError::LOW_MEMORY;
                appendActionTrace(traceOut, actionIndex, action, "", false, 0, 0,
                                  "memory_pressure");
                break;
            }
            if (monitor->isMemorySevere() && dramFree < FastBee::MemoryBudget::PERIPH_EXEC_MIN_DRAM) {
                LOGGER.warningf("[PeriphExec] DRAM SEVERE (%lu), skipping remaining actions",
                                (unsigned long)dramFree);
                if (completedAllOut) *completedAllOut = false;
                if (traceOut) traceOut->errorCode = AsyncExecError::LOW_MEMORY;
                appendActionTrace(traceOut, actionIndex, action, "", false, 0, 0,
                                  "memory_pressure");
                break;
            }
        } else if (dramFree < FastBee::MemoryBudget::PERIPH_EXEC_MIN_DRAM) {
            LOGGER.warningf("[PeriphExec] DRAM too low (%lu), skipping remaining actions",
                            (unsigned long)dramFree);
            if (completedAllOut) *completedAllOut = false;
            if (traceOut) traceOut->errorCode = AsyncExecError::LOW_MEMORY;
            appendActionTrace(traceOut, actionIndex, action, "", false, 0, 0,
                              "memory_pressure");
            break;
        }
        // 4096：每次动作前刷新水位；MQTT 连接稳态 8BIT ~7KB 时允许规则
        // 正常执行，跌破 4KB 才中止剩余动作（再低 MQTT 强制断开兜底）
        if (dram8Free < 4096) {
            LOGGER.warningf("[PeriphExec] DRAM(8bit) too low (%lu), skipping remaining actions",
                            (unsigned long)dram8Free);
            if (completedAllOut) *completedAllOut = false;
            if (traceOut) traceOut->errorCode = AsyncExecError::LOW_MEMORY;
            appendActionTrace(traceOut, actionIndex, action, "", false, 0, 0,
                              "memory_pressure");
            break;
        }

        const unsigned long actionStartedAt = millis();
        // 执行前延时
        if (action.syncDelayMs > 0) {
            const unsigned long delayMs = action.syncDelayMs > 10000
                ? 10000UL : static_cast<unsigned long>(action.syncDelayMs);
            if (deadlineAt != 0) {
                const unsigned long now = millis();
                const unsigned long remaining = deadlineAt - now;
                if (operationDeadlineReached(deadlineAt, now) || delayMs >= remaining) {
                    if (completedAllOut) *completedAllOut = false;
                    if (traceOut) traceOut->errorCode = AsyncExecError::DEADLINE_EXCEEDED;
                    appendActionTrace(traceOut, actionIndex, action, "", false,
                                      millis() - actionStartedAt, 0,
                                      "deadline_before_delay");
                    break;
                }
            }
            delay(delayMs);
        }

        // Received values keep their legacy direct-replacement behavior. The
        // only templates recognized here are the bounded panel-safe forms.
        String effectiveValue = action.actionValue;
        if (action.useReceivedValue && !receivedValue.isEmpty()) {
            if (effectiveValue == "$value") {
                effectiveValue = receivedValue;
            } else if (action.actionType == static_cast<uint8_t>(
                           ExecActionType::ACTION_MODBUS_REG_WRITE) &&
                       effectiveValue.endsWith(":$value")) {
                const String address = effectiveValue.substring(
                    0, effectiveValue.length() - strlen(":$value"));
                bool validAddress = !address.isEmpty() && address.length() <= 5;
                for (size_t i = 0; validAddress && i < address.length(); ++i) {
                    validAddress = isdigit(static_cast<unsigned char>(address.charAt(i)));
                }
                effectiveValue = validAddress && address.toInt() <= 65535
                    ? address + ":" + receivedValue : receivedValue;
            } else {
                effectiveValue = receivedValue;
            }
        }

        if (!sourceCurrent(actionIndex, action)) break;
        LOGGER.infof("[PeriphExec] Executing action: type=%d on %s, value=%s (useRecv=%d)",
            action.actionType, action.targetPeriphId.c_str(),
            (action.actionType == 10 || action.actionType == 29) ? "[omitted]" : effectiveValue.c_str(),
            action.useReceivedValue);

        bool ok = false;
        bool sourceUnconfirmed = false;
        std::vector<ActionExecResult> actionResults;
        // 标记本次动作是否属于采集类（可上报实时监测）
        bool isReportableAction = false;
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ)) {
            const size_t groupCount = sensorReadGroupSize(rule, actionIndex);
            if (groupCount > 1) {
                const bool committed = executeSensorReadGroup(rule, actionIndex, groupCount,
                    collectionStartedAt, deadlineAt, actionResults,
                    traceOut ? traceOut->uartOrigin : FastBee::UartEventOrigin{}, &sourceUnconfirmed);
                for (size_t i = 0; i < actionResults.size(); ++i) {
                    const auto& result = actionResults[i];
                    appendActionTrace(traceOut, actionIndex + i, rule.actions[actionIndex + i],
                        result.actualValue, result.success, millis() - actionStartedAt, 1, result.remark);
                }
                results.insert(results.end(), actionResults.begin(), actionResults.end());
                reportableResults.insert(reportableResults.end(), actionResults.begin(), actionResults.end());
                if (sourceUnconfirmed && traceOut) {
                    traceOut->errorCode = AsyncExecError::UART_SOURCE_UNCONFIRMED;
                } else if (!committed && traceOut && traceOut->errorCode == AsyncExecError::NONE) {
                    traceOut->errorCode = operationDeadlineReached(deadlineAt)
                        ? AsyncExecError::DEADLINE_EXCEEDED : AsyncExecError::ACTION_FAILED;
                }
                actionIndex += groupCount - 1;
                if (sourceUnconfirmed || operationDeadlineReached(deadlineAt)) {
                    if (completedAllOut) *completedAllOut = false;
                    break;
                }
                continue;
            }
            ActionExecResult ar;
            ok = executeSensorReadAction(action, ar, rule.recordHistory, &collectionStartedAt, nullptr,
                traceOut ? traceOut->uartOrigin : FastBee::UartEventOrigin{}, &sourceUnconfirmed);
            ar.success = ok;
            if (ar.remark.isEmpty()) ar.remark = ok ? "success" : "sensor_read_failed";
            actionResults.push_back(ar);
            isReportableAction = true;
        } else if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SD_RECORD)) {
            ActionExecResult ar;
            ok = _manager->getSdRecorderStatus(rule, ar);
            actionResults.push_back(ar); // Passive status only; never arm, recover or replay.
        } else if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_THERMOSTAT)) {
            ActionExecResult ar;
            ok = _manager->getThermostatStatus(rule, ar);
            actionResults.push_back(ar); // Status only, no manual output/rearm or numeric history.
        } else if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_RTC_READ)) {
            ActionExecResult ar;
            ok = executeRtcReadAction(action, ar);
            actionResults.push_back(ar);
            isReportableAction = true;
        } else if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SD_STATUS)) {
            ActionExecResult ar;
            ok = executeSdStatusAction(action, ar);
            actionResults.push_back(ar);
            isReportableAction = true;
        } else if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_RTC_SET)) {
            ActionExecResult ar;
            ok = executeRtcSetAction(action, ar);
            actionResults.push_back(ar); // Trace only; never a numeric/history reading.
        } else if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_POLL)) {
            FastBee::OrdinaryControlResult relayResult;
            ok = executeModbusPollAction(action, rule, &actionResults, FastBee::ModbusActionMode::ExecuteRule,
                traceOut ? traceOut->uartOrigin : FastBee::UartEventOrigin{}, &sourceUnconfirmed,
                ordinaryControl, &relayResult);
            if (relayResult.evaluated) {
                if (!relayResult.accepted) ordinaryFailed = true;
                if (traceOut) FastBee::rememberOrdinaryControlResult(relayResult, &traceOut->ordinaryControlResult);
            }
            if (!ok && actionResults.empty()) {
                appendActionResult(actionResults, action.targetPeriphId, effectiveValue, false, "modbus_poll_failed");
            }
            isReportableAction = true;
        } else if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_TRIGGER_EVENT)) {
            // Legacy received-value IDs cannot be covered by the local fixed
            // event graph. Reject even manual execution of a disabled draft;
            // retain its configuration so an explicit target can be supplied.
            if (action.targetPeriphId.isEmpty() && action.useReceivedValue) {
                ok = false;
                appendActionResult(actionResults, action.targetPeriphId, effectiveValue, false,
                    "CONFIG_RULE_EVENT_DYNAMIC_TARGET");
            } else {
                // Canonical schema: targetPeriphId is the event ID and actionValue
                // is payload. Keep a legacy fallback for old files, while all new
                // Manager saves require the explicit target.
                String triggerEventId = action.targetPeriphId;
                if (triggerEventId.isEmpty()) {
                    triggerEventId = effectiveValue.isEmpty() ? rule.id : effectiveValue;
                }
                LOGGER.infof("[PeriphExec] Trigger device event: id=%s name=%s (data=%s)",
                             triggerEventId.c_str(), rule.name.c_str(), effectiveValue.c_str());
                // 分发给规则（供其他规则订阅事件作为触发源），不受 reportAfterExec 控制
                const FastBee::UartEventOrigin eventOrigin = traceOut ? traceOut->uartOrigin : FastBee::UartEventOrigin{};
                // The executor owns action21 reporting. Scheduler dispatch must not
                // publish implicitly or bypass reportAfterExec/suppressReport.
                PeriphExecManager::getInstance().triggerEventById(triggerEventId, effectiveValue, eventOrigin, false);
                // 仅在勾选“是否上报数据”且未被外部抑制时，通过 DEVICE_EVENT 主题上报
                if (rule.reportAfterExec && !suppressReport) {
                    PeriphExecManager::getInstance().notifyMqttEventPublish(
                        triggerEventId, rule.name, effectiveValue, eventOrigin);
                }
                ok = true;
                appendActionResult(
                    actionResults,
                    triggerEventId,
                    effectiveValue,
                    ok,
                    "event_triggered"
                );
            }
        } else {
            // 将原始 receivedValue 透传给下层动作，便于 OLED_DISPLAY 等动作中 $value 占位符替换
            ActionExecResult detail;
            ActionExecResult reportBinding;
            uint64_t reportOwner = 0;
            bool stripReportChannel = false;
            // Classify CALL_PERIPHERAL against the same pre-action generation.
            isReportableAction = isReportableOutputAction(action.actionType) || isCallPeripheralOutputAction(action);
            if (isReportableAction) prepareOutputReport(action, effectiveValue, receivedValue,
                reportBinding, reportOwner, stripReportChannel);
            const bool callAction = action.actionType == 10 || action.actionType == 11 ||
                action.actionType == 12 || action.actionType == 28 || action.actionType == 29;
            FastBee::OrdinaryControlResult outputResult;
            ok = callAction
                ? executeCallPeripheralAction(action, effectiveValue, receivedValue, &detail, dispatchConfigEpoch,
                    traceOut ? traceOut->uartOrigin : FastBee::UartEventOrigin{}, &sourceUnconfirmed,
                    ordinaryControl, &outputResult)
                : executeActionItem(action, effectiveValue, receivedValue,
                    traceOut ? traceOut->uartOrigin : FastBee::UartEventOrigin{}, &sourceUnconfirmed,
                    ordinaryControl, &outputResult);
            if (outputResult.evaluated) {
                if (!outputResult.accepted) detail.remark = outputResult.reason;
                if (traceOut) FastBee::rememberOrdinaryControlResult(outputResult, &traceOut->ordinaryControlResult);
            }
            if (!ok && traceOut && !PeripheralManager::getInstance().isUartEventOriginCurrent(traceOut->uartOrigin))
                sourceUnconfirmed = true;
            appendActionResult(
                actionResults,
                action.targetPeriphId,
                detail.remark.startsWith("UART_") ? String() :
                    buildActionReportValue(action, effectiveValue, receivedValue),
                ok,
                sourceUnconfirmed ? String("uart_source_unconfirmed") :
                    detail.remark.isEmpty() ? String(ok ? "success" : "execute_failed") : detail.remark
            );
            // 属性控制动作的结果也作为设备属性上报：用户勾选“上报数据”后，
            // GPIO、NeoPixel、Modbus 继电器/寄存器等控制状态可同步到物联网平台。
            if (isReportableAction) finishOutputReport(actionResults.back(), std::move(reportBinding),
                reportOwner, stripReportChannel);
        }

        if (!ok) ordinaryFailed = true;
        results.insert(results.end(), actionResults.begin(), actionResults.end());
        // 采集类（SENSOR_READ / MODBUS_POLL）与属性控制类结果进入上报列表
        if (isReportableAction) {
            reportableResults.insert(reportableResults.end(),
                                     actionResults.begin(), actionResults.end());
        }

        // 打印每个动作的执行结果
        for (const auto& ar : actionResults) {
            LOGGER.infof("[PeriphExec] Action completed: target=%s, value=%s, result=%s",
                ar.targetPeriphId.c_str(), ar.actualValue.c_str(),
                ar.success ? "SUCCESS" : "FAILED");
        }

        const String* traceValue = safeActionTraceValue(
            action, effectiveValue, actionResults);
        const String traceRemark = sourceUnconfirmed ? String("uart_source_unconfirmed") : actionResults.empty()
            ? String(ok ? "success" : "execute_failed")
            : actionResults.front().remark;
        if (traceValue) {
            appendActionTrace(traceOut, actionIndex, action, *traceValue, ok,
                              millis() - actionStartedAt, actionResults.size(), traceRemark);
        } else {
            appendActionTrace(traceOut, actionIndex, action,
                              effectiveValue.isEmpty() ? String() : String("[omitted]"), ok,
                              millis() - actionStartedAt, actionResults.size(), traceRemark);
        }
        if (sourceUnconfirmed) {
            if (completedAllOut) *completedAllOut = false;
            if (traceOut) traceOut->errorCode = AsyncExecError::UART_SOURCE_UNCONFIRMED;
            break;
        }
        if (!ok && traceOut && traceOut->errorCode == AsyncExecError::NONE) {
            traceOut->errorCode = AsyncExecError::ACTION_FAILED;
        }
        if (operationDeadlineReached(deadlineAt)) {
            if (completedAllOut) *completedAllOut = false;
            if (traceOut) traceOut->errorCode = AsyncExecError::DEADLINE_EXCEEDED;
            break;
        }
    }

    // 精准上报执行结果（采集类 + 属性控制类动作的结果）
    if (rule.reportAfterExec && !suppressReport && !reportableResults.empty()) {
        LOGGER.infof("[PeriphExec] Rule '%s' execution done, reporting %d action results",
            rule.name.c_str(), reportableResults.size());
        reportActionResults(reportableResults, reportOrigin);
        // Count results offered to reporting, not MQTT acceptance/delivery.
        if (reportCountOut) {
            *reportCountOut = reportableResults.size();
        }
    } else if (!results.empty()) {
        const char* reportState = !rule.reportAfterExec
            ? "reporting disabled"
            : (suppressReport ? "reporting suppressed" : "no reportable action results");
        LOGGER.infof("[PeriphExec] Rule '%s' execution done, %d results (%s)",
            rule.name.c_str(), results.size(), reportState);
    }

    return results;
}

// ========== 启动后当前状态只读采集与上报 ==========
// 与 executeAllActions 严格区分：本方法“只读”采集规则内可上报动作的当前实际状态，
// 绝不重新驱动任何输出（无副作用），用于设备重启后一次性对齐平台显示状态。
size_t PeriphExecExecutor::reportRuleCurrentState(const PeriphExecRule& rule) {
    // 只上报启用且勾选“上报数据”的规则，与正常 reportAfterExec 语义一致
    if (!rule.enabled || !rule.reportAfterExec || !_manager || !_manager->isRuleExecutionCurrent(rule)) return 0;

    const uint32_t reportOrigin = captureReportInstanceId();

    PeripheralManager& pm = PeripheralManager::getInstance();
    std::vector<ActionExecResult> reportableResults;
    reportableResults.reserve(rule.actions.size());

    for (const auto& action : rule.actions) {
        if (!_manager->isRuleExecutionCurrent(rule)) break;
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ)) {
            // 传感器读取：采集当前实时值（无副作用）
            ActionExecResult ar;
            if (executeSensorReadAction(action, ar, rule.recordHistory)) {
                ar.success = true;
                ar.remark = "boot_state";
                reportableResults.push_back(ar);
            }
        } else if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_POLL)) {
            // Modbus 轮询：只读采集从站寄存器当前值
            std::vector<ActionExecResult> pollResults;
            executeModbusPollAction(action, rule, &pollResults, FastBee::ModbusActionMode::ReadOnlySnapshot);
            for (auto& pr : pollResults) {
                if (pr.success) {
                    pr.remark = "boot_state";
                    reportableResults.push_back(pr);
                }
            }
        } else if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_RTC_READ)) {
            ActionExecResult ar;
            if (executeRtcReadAction(action, ar)) reportableResults.push_back(ar);
        } else if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SD_STATUS)) {
            ActionExecResult ar;
            if (executeSdStatusAction(action, ar)) reportableResults.push_back(ar);
        } else if (isBootReadableGpioOutputAction(action.actionType)) {
            // 物理输出控制：只读当前引脚电平，绝不重写输出（保持设备当前实际状态）
            if (action.targetPeriphId.isEmpty()) continue;
            ActionExecResult binding;
            uint64_t owner = 0;
            bool stripChannel = false;
            prepareOutputReport(action, "", "", binding, owner, stripChannel);
            GPIOState st = pm.readPin(action.targetPeriphId);
            if (st == GPIOState::STATE_UNDEFINED) {
                // 无法确定实际电平（如 PWM/DAC 或引脚未初始化）：跳过，避免上报无效/缓存值
                continue;
            }
            // 按动作配置的反转属性将物理电平翻译为逻辑上报值，与正常执行一致：
            // 低电平点亮（*_INVERTED）的 LED 物理LOW=亮=逻辑"1"，避免重启后平台显示与实际相反。
            appendActionResult(reportableResults, action.targetPeriphId,
                               physicalStateToLogicalValue(st, action.actionType),
                               true, "boot_state");
            finishOutputReport(reportableResults.back(), std::move(binding), owner, false);
        } else if (isBootReadableCallPeripheralOutputAction(action)) {
            // CALL_PERIPHERAL 指向 GPIO 数字输出外设：只读当前电平上报启动状态。
            // 极性按非反转语义翻译（CALL_PERIPHERAL 无独立反转配置），与运行时上报一致。
            ActionExecResult binding;
            uint64_t owner = 0;
            bool stripChannel = false;
            prepareOutputReport(action, "", "", binding, owner, stripChannel);
            GPIOState st = pm.readPin(action.targetPeriphId);
            if (st == GPIOState::STATE_UNDEFINED) continue;
            appendActionResult(reportableResults, action.targetPeriphId,
                               physicalStateToLogicalValue(st, static_cast<uint8_t>(ExecActionType::ACTION_HIGH)),
                               true, "boot_state");
            finishOutputReport(reportableResults.back(), std::move(binding), owner, false);
        }
        // 其它动作类型（系统/脚本/事件/显示/其它调用）不代表可上报的物模型状态，跳过
    }

    if (reportableResults.empty()) return 0;

    LOGGER.infof("[PeriphExec] Boot state report for rule '%s': %d items",
                 rule.name.c_str(), (int)reportableResults.size());
    // 复用现有上报路径：保证 id/value 格式、dataField 物模型标识、MQTT 门控与未连接时缓存重试
    reportActionResults(reportableResults, reportOrigin);
    return reportableResults.size();
}

// ========== 具体动作执行方法 ==========

bool PeriphExecExecutor::executePeripheralAction(const ExecAction& action, const String& effectiveValue,
                                                 const String& receivedValue,
                                                 const FastBee::UartEventOrigin& uartOrigin,
                                                 const FastBee::OrdinaryControlContext* control,
                                                 FastBee::OrdinaryControlResult* controlResult) {
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
    PeripheralConfig digitalTarget;
    const bool digitalTargetFound = pm.getPeripheralSnapshot(action.targetPeriphId, digitalTarget);
    if (digitalTargetFound && FastBee::isOrdinaryDigitalTarget(digitalTarget)) {
        using Command = FastBee::DigitalOutputCommand;
        Command command;
        uint32_t intervalMs = 0;
        switch (actType) {
            case ExecActionType::ACTION_HIGH:
            case ExecActionType::ACTION_LOW_INVERTED: command = Command::High; break;
            case ExecActionType::ACTION_LOW:
            case ExecActionType::ACTION_HIGH_INVERTED: command = Command::Low; break;
            case ExecActionType::ACTION_GPIO_TOGGLE: command = Command::Toggle; break;
            case ExecActionType::ACTION_BLINK:
                command = Command::Blink;
                if (!FastBee::parseOrdinaryBlinkInterval(effectiveValue, intervalMs))
                    return FastBee::rejectOrdinaryDigitalAction(action.targetPeriphId,
                        "CONTROL_PARAMETER_INVALID", controlResult);
                break;
            default:
                return FastBee::rejectOrdinaryDigitalAction(action.targetPeriphId,
                    "CONTROL_COMMAND_UNSUPPORTED", controlResult);
        }
        return FastBee::dispatchOrdinaryDigitalAction(pm, action.targetPeriphId,
            command, intervalMs, control, controlResult);
    }
    if (digitalTargetFound && FastBee::isOrdinaryScalarTarget(digitalTarget)) {
        using Scalar = FastBee::ScalarOutputCommand;
        Scalar command = digitalTarget.type == PeripheralType::DAC ? Scalar::Dac : Scalar::Pwm;
        uint32_t value = 0;
        switch (actType) {
            case ExecActionType::ACTION_HIGH:
            case ExecActionType::ACTION_LOW_INVERTED:
                if (!FastBee::ordinaryScalarHighValue(digitalTarget, value))
                    return FastBee::rejectOrdinaryDigitalAction(action.targetPeriphId,
                        "CONTROL_PARAMETER_INVALID", controlResult);
                break;
            case ExecActionType::ACTION_LOW:
            case ExecActionType::ACTION_HIGH_INVERTED: break;
            case ExecActionType::ACTION_GPIO_TOGGLE: command = Scalar::Toggle; break;
            case ExecActionType::ACTION_BLINK:
                command = Scalar::Blink;
                if (!FastBee::parseOrdinaryBlinkInterval(effectiveValue, value))
                    return FastBee::rejectOrdinaryDigitalAction(action.targetPeriphId,
                        "CONTROL_PARAMETER_INVALID", controlResult);
                break;
            case ExecActionType::ACTION_SET_PWM:
            case ExecActionType::ACTION_SET_DAC:
                if ((actType == ExecActionType::ACTION_SET_DAC) !=
                        (digitalTarget.type == PeripheralType::DAC) ||
                    !FastBee::parseOrdinaryScalarValue(digitalTarget, effectiveValue, value))
                    return FastBee::rejectOrdinaryDigitalAction(action.targetPeriphId,
                        "CONTROL_PARAMETER_INVALID", controlResult);
                break;
            case ExecActionType::ACTION_BREATHE:
                command = Scalar::Breathe;
                if (digitalTarget.type == PeripheralType::DAC ||
                    !FastBee::parseOrdinaryBreathePeriod(effectiveValue, value))
                    return FastBee::rejectOrdinaryDigitalAction(action.targetPeriphId,
                        "CONTROL_PARAMETER_INVALID", controlResult);
                break;
            default:
                return FastBee::rejectOrdinaryDigitalAction(action.targetPeriphId,
                    "CONTROL_COMMAND_UNSUPPORTED", controlResult);
        }
        return FastBee::dispatchOrdinaryScalarAction(pm, action.targetPeriphId,
            command, value, control, controlResult);
    }
    if (digitalTargetFound && digitalTarget.type == PeripheralType::PWM_SERVO) {
        uint32_t duty = 0;
        if (actType != ExecActionType::ACTION_SET_PWM ||
            !FastBee::parseServoRawDuty(digitalTarget, effectiveValue, duty))
            return FastBee::rejectOrdinaryDigitalAction(action.targetPeriphId,
                "CONTROL_PARAMETER_INVALID", controlResult);
        return pm.submitOrdinaryServo(action.targetPeriphId, duty, true, control, controlResult);
    }
    if (digitalTargetFound && digitalTarget.isExpanderChannel()) {
        if (digitalTarget.type != PeripheralType::GPIO_DIGITAL_OUTPUT ||
            (actType != ExecActionType::ACTION_HIGH && actType != ExecActionType::ACTION_LOW &&
             actType != ExecActionType::ACTION_HIGH_INVERTED &&
             actType != ExecActionType::ACTION_LOW_INVERTED &&
             actType != ExecActionType::ACTION_GPIO_TOGGLE &&
             actType != ExecActionType::ACTION_BLINK)) return false;
        if (actType == ExecActionType::ACTION_BLINK) {
            const long interval = effectiveValue.isEmpty() ? 500L : effectiveValue.toInt();
            if (interval < 200L || interval > 65535L ||
                pm.readPin(action.targetPeriphId) == GPIOState::STATE_UNDEFINED) return false;
        }
    }

    if (digitalTargetFound && digitalTarget.isModbusPeripheral()) {
        FastBee::OrdinaryControlResult result;
        result.evaluated=true;
        auto done=[&](bool accepted) {
            FastBee::rememberOrdinaryControlResult(result,controlResult);return accepted;
        };
        auto reject=[&](const char* reason) {result.reason=reason;return done(false);};
        if(!control)return reject("CONTROL_CONTEXT_REQUIRED");
        if(!digitalTarget.enabled)return reject("CONTROL_TARGET_DISABLED");
        auto decimal=[](const String& text,uint16_t& value) {
            if(text.isEmpty() || text.length()>5 || text.length()!=std::strlen(text.c_str()))return false;
            uint32_t number=0;
            for(size_t i=0;i<text.length();++i) {
                const char c=text.charAt(i);if(c<'0' || c>'9')return false;
                number=number*10U+static_cast<uint32_t>(c-'0');if(number>65535)return false;
            }
            value=static_cast<uint16_t>(number);return true;
        };
        switch(actType) {
        case ExecActionType::ACTION_HIGH:
        case ExecActionType::ACTION_LOW_INVERTED:
            return done(pm.writeModbusPin(action.targetPeriphId,GPIOState::STATE_HIGH,*control,result));
        case ExecActionType::ACTION_LOW:
        case ExecActionType::ACTION_HIGH_INVERTED:
            return done(pm.writeModbusPin(action.targetPeriphId,GPIOState::STATE_LOW,*control,result));
        case ExecActionType::ACTION_SET_PWM: {
            uint16_t value=0;
            if(!effectiveValue.isEmpty() && !decimal(effectiveValue,value))return reject("CONTROL_PARAMETER_INVALID");
            return done(pm.writeModbusPWM(action.targetPeriphId,value,*control,result));
        }
        case ExecActionType::ACTION_MODBUS_COIL_WRITE: {
            uint16_t channel=0;
            String stateText=effectiveValue;
            const int separator=effectiveValue.indexOf(':');
            if(separator>=0) {
                if(!decimal(effectiveValue.substring(0,separator),channel))return reject("CONTROL_PARAMETER_INVALID");
                stateText=effectiveValue.substring(separator+1);
            }
            // ESP32 String comparisons may stop at an embedded NUL.
            if(stateText.length()!=std::strlen(stateText.c_str()))
                return reject("CONTROL_PARAMETER_INVALID");
            bool value=false;
            if(stateText=="1" || FastBee::stringEqualsIgnoreCaseLiteral(stateText, "true") || FastBee::stringEqualsIgnoreCaseLiteral(stateText, "on"))value=true;
            else if(stateText!="0" && !FastBee::stringEqualsIgnoreCaseLiteral(stateText, "false") && !FastBee::stringEqualsIgnoreCaseLiteral(stateText, "off"))
                return reject("CONTROL_PARAMETER_INVALID");
            const auto& configured=digitalTarget.params.modbus;
            if(configured.deviceType!=0 || configured.controlProtocol>1 ||
                channel>=configured.channelCount || uint32_t(configured.coilBase)+channel>65535U)
                return reject("CONTROL_PARAMETER_INVALID");
            if(configured.ncMode)value=!value;
            const uint16_t address=static_cast<uint16_t>(configured.coilBase+channel);
            return done(configured.controlProtocol==0?
                pm.writeModbusCoil(action.targetPeriphId,address,value,*control,result):
                pm.writeModbusReg(action.targetPeriphId,address,value?1:0,*control,result));
        }
        case ExecActionType::ACTION_MODBUS_REG_WRITE: {
            uint16_t address=digitalTarget.params.modbus.coilBase,value=0;
            const int separator=effectiveValue.indexOf(':');
            if(separator>=0) {
                if(!decimal(effectiveValue.substring(0,separator),address) ||
                    !decimal(effectiveValue.substring(separator+1),value))return reject("CONTROL_PARAMETER_INVALID");
            } else if(!decimal(effectiveValue,value))return reject("CONTROL_PARAMETER_INVALID");
            return done(pm.writeModbusReg(action.targetPeriphId,address,value,*control,result));
        }
        default:return reject("CONTROL_COMMAND_UNSUPPORTED");
        }
    }

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
            return pm.startActionTicker(action.targetPeriphId, 1, interval, uartOrigin);
        }

        case ExecActionType::ACTION_BREATHE: {
            LOGGER.infof("[PeriphExec] Execute BREATHE on %s", action.targetPeriphId.c_str());
            uint16_t speed = effectiveValue.isEmpty() ? 2000 : effectiveValue.toInt();
            return pm.startActionTicker(action.targetPeriphId, 2, speed, uartOrigin);
        }

        case ExecActionType::ACTION_SET_PWM: {
            uint32_t duty = effectiveValue.isEmpty() ? 0 : effectiveValue.toInt();
            LOGGER.infof("[PeriphExec] Execute SetPWM(%d) on %s", (int)duty, action.targetPeriphId.c_str());
            return pm.writePWM(action.targetPeriphId, duty);
        }

        case ExecActionType::ACTION_SET_DAC: {
#if CHIP_HAS_DAC
            uint8_t dacVal = 0;
            if (!FastBee::parseDacActionValue(effectiveValue.c_str(), effectiveValue.length(), dacVal))
                return false;
            LOGGER.infof("[PeriphExec] Execute SetDAC(%d) on %s", (int)dacVal, action.targetPeriphId.c_str());
            // Keep type/readiness/source validation and the checked write under
            // one PM lock: reconfiguration must not turn this byte into UART or
            // another peripheral's command between snapshot and dispatch.
            RecursiveMutexGuard lock(pm.getMutex(), 0);
            if (!lock.isLocked()) return false;
            PeripheralConfig cfg;
            if (!pm.getPeripheralSnapshot(action.targetPeriphId, cfg) ||
                !cfg.enabled || cfg.type != PeripheralType::DAC) return false;
            const auto* state = pm.getRuntimeState(action.targetPeriphId);
            if (!state || state->memorySuspended || state->memoryCleanupPending ||
                (state->status != PeripheralStatus::PERIPHERAL_INITIALIZED &&
                 state->status != PeripheralStatus::PERIPHERAL_RUNNING) ||
                !pm.isUartEventOriginCurrent(uartOrigin)) return false;
            // SDK acknowledgement only; no voltage/readback or physical claim.
            return pm.writeData(action.targetPeriphId, &dacVal, 1);
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

        case ExecActionType::ACTION_GPIO_TOGGLE: {
            pm.stopActionTicker(action.targetPeriphId);
            return pm.togglePin(action.targetPeriphId);
        }

#if FASTBEE_ENABLE_SEVEN_SEGMENT
        case ExecActionType::ACTION_DISPLAY_NUMBER: {
            // TM1637/数码管显示数字：支持 "12.34" / "12:34" / "1234" / "-12"
            // 支持 ${periphId.field} 模板，自动从传感器读取缓存替换
            String resolved = resolveSensorTemplate(effectiveValue);
            LOGGER.infof("[PeriphExec] Execute DisplayNumber(%s -> %s) on %s",
                         effectiveValue.c_str(), resolved.c_str(), action.targetPeriphId.c_str());
            PeripheralConfig cfg;
            if (!pm.getPeripheralSnapshot(action.targetPeriphId, cfg)) return false;
            if (cfg.type != PeripheralType::SEVEN_SEGMENT_TM1637) {
                LOGGER.warningf("[PeriphExec] DISPLAY_NUMBER target '%s' is not TM1637",
                                action.targetPeriphId.c_str());
                return false;
            }
            return pm.submitOrdinaryDisplay(action.targetPeriphId, FastBee::DisplayOutputCommand::Number,
                resolved, 0, control, controlResult);
        }

        case ExecActionType::ACTION_DISPLAY_TEXT: {
            // TM1637/数码管显示文本：最多 4 字符，支持 ${periphId.field} 模板
            String resolved = resolveSensorTemplate(effectiveValue);
            LOGGER.infof("[PeriphExec] Execute DisplayText(%s -> %s) on %s",
                         effectiveValue.c_str(), resolved.c_str(), action.targetPeriphId.c_str());
            PeripheralConfig cfg;
            if (!pm.getPeripheralSnapshot(action.targetPeriphId, cfg)) return false;
            if (cfg.type != PeripheralType::SEVEN_SEGMENT_TM1637) {
                LOGGER.warningf("[PeriphExec] DISPLAY_TEXT target '%s' is not TM1637",
                                action.targetPeriphId.c_str());
                return false;
            }
            return pm.submitOrdinaryDisplay(action.targetPeriphId, FastBee::DisplayOutputCommand::Text,
                resolved, 0, control, controlResult);
        }

        case ExecActionType::ACTION_DISPLAY_CLEAR: {
            LOGGER.infof("[PeriphExec] Execute DisplayClear on %s", action.targetPeriphId.c_str());
            PeripheralConfig cfg;
            if (!pm.getPeripheralSnapshot(action.targetPeriphId, cfg)) return false;
            if (cfg.type != PeripheralType::SEVEN_SEGMENT_TM1637) {
                LOGGER.warningf("[PeriphExec] DISPLAY_CLEAR target '%s' is not TM1637",
                                action.targetPeriphId.c_str());
                return false;
            }
            return pm.submitOrdinaryDisplay(action.targetPeriphId, FastBee::DisplayOutputCommand::Clear,
                String(), 0, control, controlResult);
        }
#endif

#if FASTBEE_ENABLE_LCD
        case ExecActionType::ACTION_OLED_DISPLAY: {
            // OLED 自定义多行显示：
            //   - 支持 ${periphId.field} 模板（从传感器读缓存替换）
            //   - 支持 $value 占位符（替换为 MQTT/规则下发的原始接收值）
            //   - 首行以 # 开头为居中标题 + 分隔线
            //   - actionValue 多行文本，\n 分行
            PeripheralConfig cfg;
            if (!pm.getPeripheralSnapshot(action.targetPeriphId, cfg)) {
                LOGGER.warningf("[PeriphExec] OLED_DISPLAY target not found: %s",
                                action.targetPeriphId.c_str());
                return false;
            }
            if (cfg.type != PeripheralType::LCD) {
                LOGGER.warningf("[PeriphExec] OLED_DISPLAY target '%s' is not LCD",
                                action.targetPeriphId.c_str());
                return false;
            }
            String resolved = resolveSensorTemplate(effectiveValue);
            if (resolved.indexOf("$value") >= 0) {
                resolved.replace("$value", receivedValue);
            }
            LOGGER.infof("[PeriphExec] Execute OledDisplay(%d bytes) on %s",
                         (int)resolved.length(), action.targetPeriphId.c_str());
            FastBee::LcdOutputRequest output;
            output.command = FastBee::LcdOutputCommand::CustomText;
            output.text = resolved;
            return pm.submitOrdinaryLcd(action.targetPeriphId, output, control, controlResult);
        }
#endif

        default:
            LOGGER.warningf("[PeriphExec] Unknown peripheral action: %d", action.actionType);
            return false;
    }
}

bool PeriphExecExecutor::executeModbusPollAction(const ExecAction& action,
                                                 const PeriphExecRule& rule,
                                                 std::vector<ActionExecResult>* controlResults,
                                                 FastBee::ModbusActionMode mode,
                                                 const FastBee::UartEventOrigin& uartOrigin,
                                                 bool* sourceUnconfirmedOut,
                                                 const FastBee::OrdinaryControlContext* control,
                                                 FastBee::OrdinaryControlResult* controlResult) {
    if (sourceUnconfirmedOut) *sourceUnconfirmedOut = false;
#if !FASTBEE_ENABLE_MODBUS
    (void)action; (void)rule; (void)controlResults; (void)mode; (void)uartOrigin;
    (void)control; (void)controlResult;
    return false;
#else
    if (mode != FastBee::ModbusActionMode::ExecuteRule &&
        mode != FastBee::ModbusActionMode::ReadOnlySnapshot) return false;
    bool sourceUnconfirmed = false;
    auto sourceCurrent = [&]() -> bool {
        if (!sourceUnconfirmed && PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) return true;
        if (!sourceUnconfirmed && controlResults) {
            appendActionResult(*controlResults, action.targetPeriphId, "", false, "uart_source_unconfirmed");
        }
        sourceUnconfirmed = true; // A transient busy check cannot silently resume this batch.
        if (sourceUnconfirmedOut) *sourceUnconfirmedOut = true;
        return false;
    };
    if (!sourceCurrent()) return false;
    const unsigned long batchStart = millis();
    constexpr unsigned long MODBUS_POLL_BATCH_TIMEOUT_MS = 30000; // 30s
    FastBee::OrdinaryControlContext boundedControl;
    if(control) {
        boundedControl=*control;
        uint32_t deadline=static_cast<uint32_t>(batchStart)+MODBUS_POLL_BATCH_TIMEOUT_MS;
        if(!deadline)deadline=1; // Zero means no deadline in the existing context.
        if(!boundedControl.operationDeadlineAt ||
            static_cast<int32_t>(boundedControl.operationDeadlineAt-deadline)>0)
            boundedControl.operationDeadlineAt=deadline;
        control=&boundedControl;
    }
    auto remainingBatchBudgetMs = [&]() -> uint32_t {
        const uint32_t elapsed = millis() - batchStart;
        return elapsed >= MODBUS_POLL_BATCH_TIMEOUT_MS
            ? 0U
            : MODBUS_POLL_BATCH_TIMEOUT_MS - elapsed;
    };

    auto* fw = FastBeeFramework::getInstance();
    if (!fw) { LOGGER.warning("[PeriphExec] Framework not available"); return false; }
    auto* protMgr = fw->getProtocolManager();
    if (!protMgr) { LOGGER.warning("[PeriphExec] ProtocolManager not available"); return false; }
    const uint32_t remainingBeforeLease = remainingBatchBudgetMs();
    const uint32_t leaseWaitMs = remainingBeforeLease < 2000U
        ? remainingBeforeLease
        : 2000U;
    if (leaseWaitMs == 0) return false;
    auto modbusLease = protMgr->acquireModbusHandler(pdMS_TO_TICKS(leaseWaitMs));
    ModbusHandler* modbus = modbusLease.get();
    if (!modbus) {
        LOGGER.warning("[PeriphExec] ModbusHandler unavailable or restarting for poll action");
        return false;
    }
    if (!sourceCurrent()) return false; // Lease acquisition can wait.

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

    // MemGuard 等级检查：使用 DRAM（非总堆），避免 PSRAM 干扰 Modbus 轮询判断
    {
        auto* fw = FastBeeFramework::getInstance();
        HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
        uint32_t modbusDramFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (monitor) {
            if (monitor->isMemoryCritical()) {
                Serial.printf("[PeriphExec] Modbus poll skipped - DRAM CRITICAL (dram=%lu)\n",
                              (unsigned long)modbusDramFree);
                return false;
            }
            if (monitor->isMemorySevere() && modbusDramFree < FastBee::MemoryBudget::MODBUS_POLL_MIN_DRAM) {
                Serial.printf("[PeriphExec] Modbus poll skipped - DRAM SEVERE (dram=%lu)\n",
                              (unsigned long)modbusDramFree);
                return false;
            }
        } else if (modbusDramFree < FastBee::MemoryBudget::MODBUS_POLL_MIN_DRAM) {
            Serial.printf("[PeriphExec] Insufficient DRAM for Modbus poll: %lu bytes free\n",
                          (unsigned long)modbusDramFree);
            return false;
        }
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
        Serial.println("[PeriphExec] Poll action has empty actionValue");
        return false;
    }

    const bool rawTransfer = modbus->getConfigRef().transferType == 1;
    size_t rawFramesDispatched = 0;
    auto dispatchRawFrame = [&](uint8_t slaveAddress, const String& frame) {
        if (!sourceCurrent()) return;
        rawFramesDispatched++;
        protMgr->dispatchModbusData(
            slaveAddress, frame, ModbusDataSource::PeriphExecPoll);
    };

    // 检测 JSON 格式 vs 旧逗号分隔格式
    if (actionVal.charAt(0) == '{') {
        // === 新 JSON 格式: {"poll":[0,1],"ctrl":[{"d":0,"a":"on","c":0}]} ===
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, actionVal.c_str(), actionVal.length());
        if (err) {
            Serial.printf("[PeriphExec] Failed to parse JSON actionValue: %s\n", err.c_str());
            return false;
        }

        bool anySuccess = false;
        bool requestedOperation = false;
        bool allOperationsComplete = true;
        const size_t initialControlResultCount = controlResults ? controlResults->size() : 0;

        // 批次总超时从函数入口计时，包含 handler lease 等待与 poll+ctrl。
        auto prepareControlPolicy = [&](uint16_t& effectiveTimeout,
                                        uint8_t& effectiveRetries) -> bool {
            const uint32_t remaining = remainingBatchBudgetMs();
            constexpr uint16_t MIN_TRANSACTION_TIMEOUT_MS = 100;
            constexpr uint16_t MAX_TRANSACTION_TIMEOUT_MS = 5000;
            constexpr uint16_t BUS_ACQUIRE_TIMEOUT_MS = 2000;
            constexpr uint8_t MAX_TRANSACTION_RETRIES = 3;
            if (remaining <= BUS_ACQUIRE_TIMEOUT_MS +
                                 MIN_TRANSACTION_TIMEOUT_MS) {
                return false;
            }
            const uint32_t transactionBudget =
                remaining - BUS_ACQUIRE_TIMEOUT_MS;

            effectiveTimeout = timeout == 0 ? MIN_TRANSACTION_TIMEOUT_MS : timeout;
            if (effectiveTimeout < MIN_TRANSACTION_TIMEOUT_MS) {
                effectiveTimeout = MIN_TRANSACTION_TIMEOUT_MS;
            }
            if (effectiveTimeout > MAX_TRANSACTION_TIMEOUT_MS) {
                effectiveTimeout = MAX_TRANSACTION_TIMEOUT_MS;
            }
            if (effectiveTimeout > transactionBudget) {
                effectiveTimeout = static_cast<uint16_t>(transactionBudget);
            }

            effectiveRetries = retries > MAX_TRANSACTION_RETRIES
                ? MAX_TRANSACTION_RETRIES
                : retries;
            const uint32_t attemptBudget =
                static_cast<uint32_t>(effectiveTimeout) + 50U;
            uint32_t maxAttempts = transactionBudget / attemptBudget;
            if (maxAttempts == 0) maxAttempts = 1;
            if (static_cast<uint32_t>(effectiveRetries) + 1U > maxAttempts) {
                effectiveRetries = static_cast<uint8_t>(maxAttempts - 1U);
            }
            return true;
        };
        FastBee::ModbusControlTarget relayTarget;
        bool activeRelay = false;
        FastBee::OrdinaryControlResult relayResult;
        auto writeRelayWithinBudget = [&](uint8_t function, uint16_t address, uint16_t value,
                                         uint16_t timeout) {
            const FastBee::OrdinaryControlContext absent;
            OneShotResult written = PeripheralManager::getInstance().submitOrdinaryModbusRelay(
                *modbus, relayTarget, function, address, value, control ? *control : absent, relayResult, timeout);
            if (controlResult) FastBee::rememberOrdinaryControlResult(relayResult, controlResult);
            return written;
        };
        auto writeFrameWithinBudget = [&](uint8_t slave,uint8_t function,uint16_t address,
                                          uint16_t value,uint16_t effectiveTimeout) {
            FastBee::ModbusOutputFrame frame;
            FastBee::ModbusControlTarget target;
            OneShotResult written;
            written.error=ONESHOT_CONTROL_REJECTED;
            relayResult={};relayResult.evaluated=true;
            if(!control || !frame.single(slave,function,address,value) ||
                !modbus->snapshotOrdinaryFrame(frame,target) ||
                target.instance!=relayTarget.instance || target.configuration!=relayTarget.configuration ||
                target.slave!=relayTarget.slave) {
                relayResult.reason=!control?"CONTROL_CONTEXT_REQUIRED":"CONTROL_FRAME_MAPPING_UNCONFIRMED";
                written.controlReason=relayResult.reason;
            } else written=PeripheralManager::getInstance().submitOrdinaryModbusFrame(
                *modbus,target,frame,*control,relayResult,effectiveTimeout);
            if(controlResult)FastBee::rememberOrdinaryControlResult(relayResult,controlResult);
            return written;
        };
        auto writeCoilWithinBudget = [&](uint8_t slave, uint16_t address,
                                         bool value, OneShotResult& result) -> bool {
            uint16_t effectiveTimeout = 0;
            uint8_t effectiveRetries = 0;
            if (!prepareControlPolicy(effectiveTimeout, effectiveRetries)) return false;
            if (activeRelay) {
                result = writeRelayWithinBudget(5, address, value ? 0xff00 : 0, effectiveTimeout); return true;
            }
            result = writeFrameWithinBudget(slave,5,address,value?0xff00:0,effectiveTimeout);
            return true;
        };
        auto writeRegisterWithinBudget = [&](uint8_t slave, uint16_t address,
                                             uint16_t value,
                                             OneShotResult& result) -> bool {
            uint16_t effectiveTimeout = 0;
            uint8_t effectiveRetries = 0;
            if (!prepareControlPolicy(effectiveTimeout, effectiveRetries)) return false;
            if (activeRelay) {
                result = writeRelayWithinBudget(6, address, value, effectiveTimeout); return true;
            }
            result = writeFrameWithinBudget(slave,6,address,value,effectiveTimeout);
            return true;
        };

        // 处理 poll 数组: 执行采集任务
        JsonArray pollArr = doc["poll"].as<JsonArray>();
        if (pollArr) {
            // 使用 JsonDocument 结构化数组代替 String 拼接
            JsonDocument resultDoc;
            JsonArray resultsArr = resultDoc.to<JsonArray>();

            // 收集任务索引，交给 ModbusHandler 统一调度（同从站/同功能码相邻寄存器合并读）
            uint8_t batchIdx[Protocols::MODBUS_MAX_POLL_TASKS];
            uint8_t batchCount = 0;
            for (JsonVariant v : pollArr) {
                if (batchCount >= Protocols::MODBUS_MAX_POLL_TASKS) {
                    Serial.printf("[PeriphExec] Poll batch truncated at %d tasks\n",
                                  Protocols::MODBUS_MAX_POLL_TASKS);
                    allOperationsComplete = false;
                    break;
                }
                batchIdx[batchCount++] = v.as<uint8_t>();
            }
            requestedOperation = requestedOperation || batchCount > 0;

            // emitCallback=false: PeriphExec 有自己的数据处理流程，不需要通过 dataCallback 重复分发
            bool pollBatchComplete = false;
            const uint32_t elapsedBeforePoll = millis() - batchStart;
            const uint32_t pollBudget = elapsedBeforePoll >= MODBUS_POLL_BATCH_TIMEOUT_MS
                ? 0
                : MODBUS_POLL_BATCH_TIMEOUT_MS - elapsedBeforePoll;
            String result = "[]";
            if (pollBudget > 0) {
                if (!sourceCurrent()) return false;
                result = modbus->executePollTaskBatch(
                    batchIdx, batchCount, timeout, retries, interDelay, false,
                    pollBudget, &pollBatchComplete, dispatchRawFrame);
            }
            if (!sourceCurrent()) return false;
            if (!pollBatchComplete) {
                allOperationsComplete = false;
                if (controlResults) {
                    appendActionResult(*controlResults, action.targetPeriphId, "", false,
                                       pollBudget == 0 ? "modbus_poll_deadline" : "modbus_poll_incomplete");
                }
            }
            Serial.printf("[PeriphExec] Poll batch (%d tasks) len=%u\n", batchCount, (unsigned)result.length());
            if (rawTransfer) {
                anySuccess = rawFramesDispatched > 0;
            } else if (result != "[]") {
                anySuccess = true;
                // 解析批量结果并追加到结构化数组（避免字符串拼接）
                JsonDocument taskDoc;
                if (!appendModbusPollReadings(taskDoc, result, resultsArr)) {
                    Serial.printf("[PeriphExec] Poll batch result JSON parse failed\n");
                    allOperationsComplete = false;
                }
            }
            if (rawTransfer) {
                Serial.printf("[PeriphExec] Poll completed, dispatched %u raw frame(s)\n",
                              static_cast<unsigned>(rawFramesDispatched));
            } else if (anySuccess && resultsArr.size() > 0) {
                // 只在最终出口做一次 serializeJson
                String mergedJson;
                serializeJson(resultDoc, mergedJson);
                Serial.printf("[PeriphExec] Poll completed, dispatching %d bytes\n", mergedJson.length());
                // 统一通过 dispatchModbusData 分发（PeriphExecPoll: 仅 MQTT+SSE，不重复规则匹配）
                if (protMgr) {
                    protMgr->dispatchModbusData(0, mergedJson, ModbusDataSource::PeriphExecPoll);
                }
            } else {
                Serial.printf("[PeriphExec] Poll: no valid data (anySuccess=%d, size=%d)\n",
                              (int)anySuccess, (int)resultsArr.size());
            }
        }

        // 处理 ctrl 数组: 执行控制命令
        JsonArray ctrlArr = doc["ctrl"].as<JsonArray>();
        if (ctrlArr.size() && mode == FastBee::ModbusActionMode::ReadOnlySnapshot) {
            LOGGER.info("[PeriphExec] Boot snapshot skipped Modbus control commands");
        }
        // Boot snapshots may use the poll selection but cannot replay relay,
        // PWM, PID or motor commands, nor produce their completion events.
        if (ctrlArr && mode == FastBee::ModbusActionMode::ExecuteRule) {
            for (JsonVariant cv : ctrlArr) {
                if (!sourceCurrent()) return false;
                requestedOperation = true;
                // 批次总超时检查（poll+ctrl 共享同一时间预算）
                if (remainingBatchBudgetMs() == 0) {
                    Serial.printf("[PeriphExec] Ctrl batch timeout (%lums), stopping early\n",
                                  (unsigned long)MODBUS_POLL_BATCH_TIMEOUT_MS);
                    allOperationsComplete = false;
                    if (controlResults) {
                        appendActionResult(*controlResults, action.targetPeriphId, "", false,
                                           "modbus_ctrl_deadline");
                    }
                    break;
                }
                // MemGuard 堆守卫：与 pollArr 保持一致的防护
                {
                    auto* fwCtrl = FastBeeFramework::getInstance();
                    HealthMonitor* monCtrl = fwCtrl ? fwCtrl->getHealthMonitor() : nullptr;
                    if ((monCtrl && monCtrl->isMemoryCritical()) || FastBee::MemoryBudget::dramFree() < 15000) {
                        Serial.printf("[PeriphExec] Heap dropped to %d during ctrl, stopping early\n",
                                      (int)ESP.getFreeHeap());
                        allOperationsComplete = false;
                        if (controlResults) {
                            appendActionResult(*controlResults, action.targetPeriphId, "", false,
                                               "memory_pressure");
                        }
                        break;
                    }
                }
                if (!cv["d"].is<uint8_t>() || (!cv["c"].isNull() && !cv["c"].is<uint16_t>()) ||
                    (!cv["a"].isNull() && (!cv["a"].is<const char*>() ||
                        cv["a"].as<JsonString>().size()!=std::strlen(cv["a"].as<const char*>())))) {
                    allOperationsComplete = false;
                    if (controlResults) appendActionResult(*controlResults, "", "", false, "CONTROL_PARAMETER_INVALID");
                    break;
                }
                uint8_t devIdx = cv["d"].as<uint8_t>();
                ModbusSubDevice dev;
                if (!modbus->snapshotControlDevice(devIdx, dev, relayTarget)) {
                    Serial.printf("[PeriphExec] ctrl device index %d out of range\n", devIdx);
                    allOperationsComplete = false;
                    if (controlResults) {
                        appendActionResult(*controlResults, "modbus:" + String(devIdx), "", false,
                                           "invalid_device_index");
                    }
                    break; // A rejected target retires the remaining output suffix.
                }
                activeRelay = std::strcmp(dev.deviceType, "relay") == 0;
                if (activeRelay && !relayTarget.valid()) {
                    allOperationsComplete = false;
                    if (controlResults) appendActionResult(*controlResults, "modbus:" + String(devIdx), "", false,
                        "CONTROL_RELAY_MAPPING_UNCONFIRMED");
                    break;
                }
                if((!cv["c"].isNull() && !cv["c"].is<uint16_t>()) ||
                    (!cv["v"].isNull() && !cv["v"].is<uint16_t>()) ||
                    (!cv["a"].isNull() && !cv["a"].is<const char*>())) {
                    allOperationsComplete=false;
                    if(controlResults)appendActionResult(*controlResults,"modbus:"+String(devIdx),"",false,
                        "CONTROL_PARAMETER_INVALID");
                    break;
                }
                const char* act = cv["a"] | "on";
                uint16_t ch = cv["c"] | 0;
                uint16_t val = cv["v"] | 0;
                ActionExecResult controlReport;
                char controlTarget[16];
                snprintf(controlTarget, sizeof(controlTarget), "modbus:%u", static_cast<unsigned>(devIdx));
                FastBee::captureModbusReportIdentity(controlReport, controlTarget,
                    dev.sensorId, dev.channelCount, strcmp(dev.deviceType, "motor") == 0 ? 0 : ch);
                auto recordControl = [&](const String& value, bool success, const String& remark,
                                         bool includeChannel = false) {
                    if (!controlResults) return;
                    // Unmapped legacy subdevices keep their channel:value form.
                    String reportValue;
                    if (includeChannel && controlReport.reportIdentity == FastBee::ActionReportIdentity::Peripheral) {
                        char prefix[8];
                        const int count = snprintf(prefix, sizeof(prefix), "%u:", static_cast<unsigned>(ch));
                        if (count <= 0 || static_cast<size_t>(count) >= sizeof(prefix) ||
                            !FastBee::copyCheckedText(reportValue, prefix, static_cast<size_t>(count), 512) ||
                            !FastBee::appendCheckedText(reportValue, value.c_str(), value.length(), 512))
                            controlReport.reportIdentity = FastBee::ActionReportIdentity::Invalid;
                    } else reportValue = value;
                    if (reportValue.length() < value.length())
                        controlReport.reportIdentity = FastBee::ActionReportIdentity::Invalid;
                    FastBee::completeCapturedReport(controlReport, reportValue, success, remark);
                    controlResults->push_back(std::move(controlReport));
                };

                if (interDelay > 0) {
                    const uint32_t remainingBeforeDelay = remainingBatchBudgetMs();
                    if (remainingBeforeDelay <= interDelay) {
                        allOperationsComplete = false;
                        if (controlResults) {
                            appendActionResult(*controlResults,
                                               "modbus:" + String(devIdx), "", false,
                                               "modbus_ctrl_deadline");
                        }
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(interDelay));
                }
                if (!sourceCurrent()) return false;

                String devType(dev.deviceType);
                if (devType == "relay") {
                    bool desiredState = false;
                    if (!tryParseBoolLike(String(act), desiredState)) {
                        allOperationsComplete = false;
                        recordControl("", false, "CONTROL_PARAMETER_INVALID"); break;
                    }
                    if (ch >= dev.channelCount || uint32_t(dev.coilBase) + ch > 65535U) {
                        allOperationsComplete = false;
                        recordControl("", false, "CONTROL_PARAMETER_INVALID"); break;
                    }
                    bool coilVal = dev.ncMode ? !desiredState : desiredState;
                    uint16_t addr = dev.coilBase + ch;
                    bool ok = false;
                    if (dev.controlProtocol == 0) {
                        OneShotResult res;
                        if (!writeCoilWithinBudget(dev.slaveAddress, addr,
                                                   coilVal, res)) {
                            allOperationsComplete = false;
                            if (controlResults) {
                                appendActionResult(*controlResults,
                                                   "modbus:" + String(devIdx), "", false,
                                                   "modbus_ctrl_deadline");
                            }
                            break;
                        }
                        ok = (res.error == ONESHOT_SUCCESS);
                        anySuccess = anySuccess || ok;
                        Serial.printf("[PeriphExec] Relay ctrl: slave=%d coil=%d val=%d ok=%d\n",
                            dev.slaveAddress, addr, coilVal, ok);
                    } else {
                        uint16_t regVal = coilVal ? 1 : 0;
                        OneShotResult res;
                        if (!writeRegisterWithinBudget(dev.slaveAddress, addr,
                                                       regVal, res)) {
                            allOperationsComplete = false;
                            if (controlResults) {
                                appendActionResult(*controlResults,
                                                   "modbus:" + String(devIdx), "", false,
                                                   "modbus_ctrl_deadline");
                            }
                            break;
                        }
                        ok = (res.error == ONESHOT_SUCCESS);
                        anySuccess = anySuccess || ok;
                        Serial.printf("[PeriphExec] Relay reg ctrl: slave=%d reg=%d val=0x%04X ok=%d\n",
                            dev.slaveAddress, addr, regVal, ok);
                    }
                    if (controlResults) {
                        recordControl(ok ? String(desiredState ? 1 : 0) : String(), ok,
                                      relayResult.reason, true);
                    }
                    if (!ok) { allOperationsComplete = false; break; } // Keep prior ACKs; no suffix writes/events.
                    // 控制成功后触发 mc: 事件，供其他规则监听
                    if (ok && _manager) {
                        char mcIdBuf[16];
                        snprintf(mcIdBuf, sizeof(mcIdBuf), "mc:%d", devIdx);
                        char mcDataBuf[96];
                        snprintf(mcDataBuf, sizeof(mcDataBuf), "{\"d\":%d,\"c\":%d,\"a\":\"%s\"}", devIdx, ch, act);
                        _manager->triggerEventById(String(mcIdBuf), String(mcDataBuf), uartOrigin);
                    }
                } else if (devType == "pwm") {
                    if(ch>=dev.channelCount || uint32_t(dev.pwmRegBase)+ch>65535U) {
                        allOperationsComplete=false;recordControl("",false,"CONTROL_PARAMETER_INVALID");break;
                    }
                    uint16_t regAddr = static_cast<uint16_t>(dev.pwmRegBase + ch);
                    OneShotResult res;
                    if (!writeRegisterWithinBudget(dev.slaveAddress, regAddr,
                                                   val, res)) {
                        allOperationsComplete = false;
                        if (controlResults) {
                            appendActionResult(*controlResults,
                                               "modbus:" + String(devIdx), "", false,
                                               "modbus_ctrl_deadline");
                        }
                        break;
                    }
                    bool ok = (res.error == ONESHOT_SUCCESS);
                    anySuccess = anySuccess || ok;
                    Serial.printf("[PeriphExec] PWM ctrl: slave=%d reg=%d val=%d ok=%d\n",
                        dev.slaveAddress, regAddr, val, ok);
                    if (controlResults) {
                        recordControl(ok ? String(val) : String(), ok, relayResult.reason, true);
                    }
                    if(!ok) {allOperationsComplete=false;break;}
                    // 控制成功后触发 mc: 事件
                    if (ok && _manager) {
                        char mcIdBuf[16];
                        snprintf(mcIdBuf, sizeof(mcIdBuf), "mc:%d", devIdx);
                        char mcDataBuf[96];
                        snprintf(mcDataBuf, sizeof(mcDataBuf), "{\"d\":%d,\"c\":%d,\"a\":\"pwm\",\"v\":%d}", devIdx, ch, val);
                        _manager->triggerEventById(String(mcIdBuf), String(mcDataBuf), uartOrigin);
                    }
                } else if (devType == "pid") {
                    const char* param = cv["p"] | "P";
                    if((!cv["p"].isNull() && !cv["p"].is<const char*>()) ||
                        (!cv["p"].isNull() && cv["p"].as<JsonString>().size()!=std::strlen(param)) ||
                        (std::strcmp(param,"P") && std::strcmp(param,"I") && std::strcmp(param,"D"))) {
                        allOperationsComplete=false;recordControl("",false,"CONTROL_PARAMETER_INVALID");break;
                    }
                    uint8_t pidIdx = 3;
                    if (strcmp(param, "I") == 0) pidIdx = 4;
                    else if (strcmp(param, "D") == 0) pidIdx = 5;
                    uint16_t regAddr = dev.pidAddrs[pidIdx];
                    OneShotResult res;
                    if (!writeRegisterWithinBudget(dev.slaveAddress, regAddr,
                                                   val, res)) {
                        allOperationsComplete = false;
                        if (controlResults) {
                            appendActionResult(*controlResults,
                                               "modbus:" + String(devIdx), "", false,
                                               "modbus_ctrl_deadline");
                        }
                        break;
                    }
                    bool ok = (res.error == ONESHOT_SUCCESS);
                    anySuccess = anySuccess || ok;
                    Serial.printf("[PeriphExec] PID ctrl: slave=%d param=%s reg=%d val=%d ok=%d\n",
                        dev.slaveAddress, param, regAddr, val, ok);
                    if (controlResults) {
                        recordControl(ok ? String(val) : String(), ok, relayResult.reason, true);
                    }
                    if(!ok) {allOperationsComplete=false;break;}
                    // 控制成功后触发 mc: 事件
                    if (ok && _manager) {
                        char mcIdBuf[16];
                        snprintf(mcIdBuf, sizeof(mcIdBuf), "mc:%d", devIdx);
                        char mcDataBuf[96];
                        snprintf(mcDataBuf, sizeof(mcDataBuf), "{\"d\":%d,\"a\":\"pid\",\"p\":\"%s\",\"v\":%d}", devIdx, param, val);
                        _manager->triggerEventById(String(mcIdBuf), String(mcDataBuf), uartOrigin);
                    }
                } else if (devType == "motor") {
                    // Accepted is not completed: await the shared command result.
                    const char* act = cv["a"] | "stop";
                    if ((strcmp(act, "forward") && strcmp(act, "reverse") && strcmp(act, "stop")) ||
                        (!cv["v"].isNull() && !cv["v"].is<uint16_t>())) {
                        allOperationsComplete = false;
                        recordControl("", false, "CONTROL_MOTOR_PARAMETER_INVALID"); break;
                    }
                    const uint16_t motorValue = cv["v"] | 1;
                    auto& motors = MotorControlService::instance();
                    char requestId[64];
                    snprintf(requestId, sizeof(requestId), "rule-%08lx-%08lx",
                             static_cast<unsigned long>(millis()), static_cast<unsigned long>(esp_random()));
                    uint32_t operationId = 0; String error;
                    const String ref = motors.referenceForSlave(dev.slaveAddress);
                    bool accepted = motors.submitRule(ref, String(act), motorValue, String(requestId),
                                                       remainingBatchBudgetMs(), timeout, operationId, error, uartOrigin, control);
                    bool ok = false, terminalKnown = !accepted, attempted = false;
                    uint16_t acknowledgedSteps = 0;
                    auto observeMotor = [&]() {
                        JsonDocument result;
                        if (!motors.operation(operationId, result.to<JsonObject>())) return false;
                        // The bounded worker record retains all outputSteps.
                        // A pending observation cannot prove zero UART writes.
                        JsonArrayConst steps = result["outputSteps"].as<JsonArrayConst>();
                        uint16_t acknowledgements = 0;
                        size_t count = 0;
                        for (JsonObjectConst step : steps) {
                            if (++count > 8) break;
                            attempted |= step["attempted"] | false;
                            if (step["commandAcknowledged"] | false) ++acknowledgements;
                        }
                        acknowledgedSteps = std::max(acknowledgedSteps, acknowledgements);
                        if (result["pending"] | true) return false;
                        terminalKnown = true; ok = result["success"] | false;
                        error = result["error"] | ""; return true;
                    };
                    while (accepted && remainingBatchBudgetMs()) {
                        if (observeMotor()) break;
                        vTaskDelay(pdMS_TO_TICKS(20));
                    }
                    if (accepted && !terminalKnown) observeMotor(); // Read-only final cache observation.
                    FastBee::OrdinaryControlResult motorResult;
                    motorResult.evaluated = true; motorResult.attemptKnown = terminalKnown;
                    motorResult.attempted = attempted; motorResult.accepted = ok;
                    motorResult.backendOperationId = operationId; motorResult.acknowledgedSteps = acknowledgedSteps;
                    if (relayTarget.valid()) relayTarget.key(motorResult.targetId);
                    if (control) { motorResult.actor = control->request.actor; motorResult.operation = control->request.operation; }
                    motorResult.phase = terminalKnown ? FastBee::OrdinaryControl::Phase::Idle : FastBee::OrdinaryControl::Phase::Executing;
                    motorResult.reason = ok ? "CONTROL_ACKNOWLEDGED_TRANSPORT_ONLY" :
                        terminalKnown ? "CONTROL_MOTOR_FAILED" : "CONTROL_MOTOR_RESULT_UNCONFIRMED";
                    if (controlResult) FastBee::rememberOrdinaryControlResult(motorResult, controlResult);
                    if (!ok && error.isEmpty()) error = "motor_result_unconfirmed";
                    allOperationsComplete = allOperationsComplete && ok;
                    anySuccess = anySuccess || ok;
                    if (controlResults) {
                        const char* commandValue = !strcmp(act, "forward") ? "1" : !strcmp(act, "reverse") ? "2" : "0";
                        recordControl(ok ? String(commandValue) : String(), ok, ok ? String("success") : error);
                    }
                    // Only the shared service emits the motor-result event.
                    if (error == "uart_source_unconfirmed" || error == "CONTROL_MOTOR_SOURCE_UNCONFIRMED") {
                        // Preserve a worker/admission failure even if the PM
                        // lock is available again at the next outer check.
                        if (sourceUnconfirmedOut) *sourceUnconfirmedOut = true;
                        return false;
                    }
                    if (!ok) break; // Preserve accepted prefix; caller timeout never releases the worker.
                }
            }
        }

        if (!sourceCurrent()) return false;
        if (controlResults) {
            for (size_t i = initialControlResultCount; i < controlResults->size(); ++i) {
                if (!(*controlResults)[i].success) {
                    allOperationsComplete = false;
                    break;
                }
            }
        }
        return requestedOperation && allOperationsComplete;
    }

    // === 旧格式兼容: 逗号分隔的任务索引，如 "0,1,3" ===
    // 使用 JsonDocument 结构化数组代替 String 拼接
    JsonDocument resultDoc;
    JsonArray resultsArr = resultDoc.to<JsonArray>();
    bool anySuccess = false;

    // 收集任务索引，交给 ModbusHandler 统一调度（同从站/同功能码相邻寄存器合并读）
    uint8_t legacyBatchIdx[Protocols::MODBUS_MAX_POLL_TASKS];
    uint8_t legacyBatchCount = 0;
    int start = 0;
    while (start <= (int)actionVal.length()) {
        int comma = actionVal.indexOf(',', start);
        if (comma < 0) comma = actionVal.length();
        String token = actionVal.substring(start, comma);
        token.trim();
        if (token.length() > 0) {
            if (legacyBatchCount >= Protocols::MODBUS_MAX_POLL_TASKS) {
                LOGGER.warningf("[PeriphExec] Legacy poll batch truncated at %d tasks",
                                Protocols::MODBUS_MAX_POLL_TASKS);
                break;
            }
            legacyBatchIdx[legacyBatchCount++] = (uint8_t)token.toInt();
        }
        start = comma + 1;
    }

    // emitCallback=false: PeriphExec 有自己的数据处理流程，不需要通过 dataCallback 重复分发
    bool legacyBatchComplete = false;
    const uint32_t legacyBudget = remainingBatchBudgetMs();
    if (legacyBudget == 0) return false;
    if (!sourceCurrent()) return false;
    String legacyResult = modbus->executePollTaskBatch(
        legacyBatchIdx, legacyBatchCount, timeout, retries, interDelay, false,
        legacyBudget, &legacyBatchComplete, dispatchRawFrame);
    if (!sourceCurrent()) return false;
    if (!rawTransfer && legacyResult != "[]") {
        anySuccess = true;
        // 解析批量结果并追加到结构化数组（避免字符串拼接）
        JsonDocument taskDoc;
        (void)appendModbusPollReadings(taskDoc, legacyResult, resultsArr);
    }

    if (rawTransfer) {
        LOGGER.infof("[PeriphExec] Poll action completed, dispatched %u raw frame(s)",
                     static_cast<unsigned>(rawFramesDispatched));
    } else if (anySuccess && resultsArr.size() > 0) {
        // 只在最终出口做一次 serializeJson
        String mergedJson;
        serializeJson(resultDoc, mergedJson);
        LOGGER.infof("[PeriphExec] Poll action completed, dispatching %d bytes via unified outlet", mergedJson.length());
        // 统一通过 dispatchModbusData 分发（PeriphExecPoll: 仅 MQTT+SSE，不重复规则匹配）
        if (protMgr) {
            protMgr->dispatchModbusData(0, mergedJson, ModbusDataSource::PeriphExecPoll);
        }
    }

    return legacyBatchCount > 0 && legacyBatchComplete;
#endif // FASTBEE_ENABLE_MODBUS
}

// 按传感器类别推断默认数据通道名：仅多字段传感器（DHT/SHT3x/BMP280/MPU6050 等
// 需选择温度/湿度/轴等通道）使用，供读取通道选择与 LCD 条目等内部用途。
// 推断值绝不作为上报 ID：上报 ID 的通用规则是"显式 dataField 优先，否则回退
// 外设配置 id"，避免写死的默认字段与平台物模型标识符不匹配。
String PeriphExecExecutor::guessSensorDataField(const String& category) {
    String normalized = category;
    normalized.trim();
    normalized.toLowerCase();
    if (const auto* metadata = FastBee::DriverRegistry::getInstance().getMetadata(normalized.c_str())) {
        const auto* channel = metadata->getChannel(0);
        if (channel) return channel->name;
    }
    if (normalized == "dht11" || normalized == "dht22" || normalized == "ds18b20") {
        return "temperature";
    } else if (normalized == "sht30" || normalized == "sht31" ||
               normalized == "aht20" || normalized == "bmp280") {
        return "temperature";
    } else if (normalized == "mpu6050") {
        return "accelX";
    } else if (normalized == "ultrasonic") {
        return "distance";
    } else if (normalized == "current" || normalized == "current_loop" ||
               normalized == "4-20ma" || normalized == "4_20ma") {
        return "current";
    } else if (normalized == "voltage") {
        return "voltage";
    } else if (normalized == "radar") {
        return "presence";
    } else if (normalized == "ntc") {
        return "temperature";
    }
    return "value"; // 其它类别的内部通道名兜底
}

// 内部通道字段统一解析：显式 dataField 优先；多字段类别按类别推断默认通道
// （需选择温/湿/轴等读取通道）；单值传感器直接用外设配置 id，
// 使缓存键/模板变量与平台物模型标识符自然对齐
String PeriphExecExecutor::resolveChannelField(const String& category, const String& periphId, const String& explicitField) {
    if (!explicitField.isEmpty()) {
        return explicitField;
    }
    String normalized = category;
    normalized.trim();
    normalized.toLowerCase();
    if (normalized == "dht11" || normalized == "dht22" ||
        normalized == "sht30" || normalized == "sht31" ||
        normalized == "aht20" || normalized == "bmp280" ||
        normalized == "mpu6050") {
        return guessSensorDataField(normalized);
    }
    return periphId;
}

bool PeriphExecExecutor::executeSensorReadGroup(const PeriphExecRule& rule,
    size_t first, size_t count, uint32_t collectionStartedAt, unsigned long deadlineAt,
    std::vector<ActionExecResult>& results, const FastBee::UartEventOrigin& uartOrigin,
    bool* sourceUnconfirmed) {
    if (sourceUnconfirmed) *sourceUnconfirmed = false;
    bool sourceRejected = false;
    auto sourceCurrent = [&]() {
        if (!sourceRejected && PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) return true;
        sourceRejected = true;
        if (sourceUnconfirmed) *sourceUnconfirmed = true;
        return false;
    };
    if (count < 2 || count > 4 || first + count > rule.actions.size()) return false;
    std::unique_ptr<PreparedSensorRead[]> prepared(new (std::nothrow) PreparedSensorRead[count]);
    try { results.resize(count); } catch (const std::bad_alloc&) { return false; }
    bool ready = sourceCurrent() && static_cast<bool>(prepared);
    const ObservationUpdate* updates[4] = {};
    for (size_t i = 0; i < count; ++i) {
        results[i].targetPeriphId = rule.actions[first + i].targetPeriphId;
        if (!ready) continue;
        if (!sourceCurrent()) { ready = false; continue; }
        // Keep the same per-action memory/deadline admission, including when
        // the first physical conversion temporarily consumes internal RAM.
        auto* framework = FastBeeFramework::getInstance();
        auto* monitor = framework ? framework->getHealthMonitor() : nullptr;
        if (operationDeadlineReached(deadlineAt) ||
            (monitor && monitor->isMemoryCritical()) ||
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < FastBee::MemoryBudget::PERIPH_EXEC_MIN_DRAM ||
            heap_caps_get_free_size(MALLOC_CAP_8BIT) < 4096) {
            ready = false;
            continue;
        }
        ready = executeSensorReadAction(rule.actions[first + i], results[i],
            rule.recordHistory, &collectionStartedAt, &prepared[i], uartOrigin, sourceUnconfirmed);
        if (sourceUnconfirmed && *sourceUnconfirmed) { sourceRejected = true; ready = false; }
        if (!sourceCurrent()) ready = false;
        updates[i] = &prepared[i].observation;
        if (ready && (prepared[i].configVersion != prepared[0].configVersion ||
            updates[i]->sampleGroup != prepared[0].observation.sampleGroup ||
            (!updates[i]->sampleGroup && updates[i]->sampleTimestamp != prepared[0].observation.sampleTimestamp))) ready = false;
    }
    const bool committed = ready && sourceCurrent() && !operationDeadlineReached(deadlineAt) &&
        PeriphExecManager::getInstance().upsertSensorObservationBatch(
            updates, count, prepared[0].configVersion);
    for (size_t i = 0; i < count; ++i) {
        results[i].success = committed;
        results[i].remark = sourceRejected ? "uart_source_unconfirmed" :
            committed ? "success" : "sensor_group_not_published";
        if (!committed) results[i].actualValue = "";
#if FASTBEE_ENABLE_LCD
        if (committed && LCDManager::getInstance().isInitialized() &&
            !prepared[i].observation.label.isEmpty()) {
            const auto& value = prepared[i].observation;
            LCDManager::getInstance().updateSensorEntry(value.sourceRef, value.label,
                prepared[i].processed, value.unit, prepared[i].decimals, prepared[i].configVersion);
        }
#endif
    }
    return committed;
}

bool PeriphExecExecutor::executeSensorReadAction(const ExecAction& action, ActionExecResult& result,
                                                 bool recordHistory,
                                                 const uint32_t* collectionStartedAt,
                                                 PreparedSensorRead* prepared,
                                                 const FastBee::UartEventOrigin& uartOrigin,
                                                 bool* sourceUnconfirmed) {
    if (sourceUnconfirmed) *sourceUnconfirmed = false;
    result = ActionExecResult{};
    result.hasSensorSample = true;
    auto sourceCurrent = [&]() {
        if (PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) return true;
        if (sourceUnconfirmed) *sourceUnconfirmed = true;
        result.remark = "uart_source_unconfirmed";
        return false;
    };
    if (!sourceCurrent()) return false;
    result.reportIdentity = FastBee::ActionReportIdentity::Invalid;
    String actionVal = action.actionValue;
    if (actionVal.isEmpty() || actionVal.charAt(0) != '{') {
        LOGGER.warning("[PeriphExec] Sensor read: empty or invalid actionValue");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, actionVal.c_str(), actionVal.length());
    if (err) {
        LOGGER.warningf("[PeriphExec] Sensor read: JSON parse failed: %s", err.c_str());
        return false;
    }

    const char* periphId = doc["periphId"] | "";
    const char* reportField = doc["dataField"] | "";
    if (!FastBee::captureLiteralReportIdentity(result, periphId, reportField)) return false;
    PeripheralManager& pm = PeripheralManager::getInstance();
    PeripheralConfig boundConfig;
    uint32_t sensorConfigVersion = 0;
    if (!periphId[0] || !pm.getPeripheralSnapshot(String(periphId), boundConfig, &sensorConfigVersion) || !boundConfig.enabled) {
        if (sensorConfigVersion) PeriphExecManager::getInstance().invalidateSensorReadCache(
            String(periphId), "SENSOR_NOT_READY", sensorConfigVersion);
        return false;
    }
    bool sampleAccepted = false;
    struct InvalidateFailedRead {
        bool& accepted;
        ActionExecResult& result;
        const char* id;
        uint32_t version;
        ~InvalidateFailedRead() {
            if (!accepted) PeriphExecManager::getInstance().invalidateSensorReadCache(
                String(id), result.remark.isEmpty() ? String("SENSOR_READ_FAILED") : result.remark, version);
        }
    } failureGuard{sampleAccepted, result, periphId, sensorConfigVersion};
    const bool configuredI2cSensor = boundConfig.type == PeripheralType::SENSOR_GENERIC &&
        boundConfig.params.sensor.driver[0] != '\0';
    // Normalize imported/UI values once so aliases and mixed-case categories
    // follow exactly the same execution path.
    String categoryValue = configuredI2cSensor ? String(boundConfig.params.sensor.driver)
        : String(doc["sensorCategory"] | "analog");
    categoryValue.trim();
    categoryValue.toLowerCase();
    const char* category = categoryValue.c_str();
    float scaleFactor = doc["scaleFactor"] | 1.0f;
    float offset = doc["offset"] | 0.0f;
    uint8_t decimals = doc["decimalPlaces"] | 2;
    const char* label = doc["sensorLabel"] | "";
    const char* unit = doc["unit"] | "";
    // 显式配置的 dataField：唯一可用作上报 ID 的来源（多字段外设如 DHT11 温/湿）。
    // channelField 可选地指定驱动内部通道，使多个物理传感器能使用全局唯一
    // dataField，而不影响 SHT/DHT 的 temperature/humidity 通道选择。
    String explicitFieldStr = doc["dataField"] | "";
    // 内部通道字段（缓存键/LCD 条目/通道选择）：与 Web 数据源 sourceId 同规则，
    // 推断值不进入 result.dataField，不会顶替外设配置 id 成为上报 ID
    String explicitChannelField = doc["channelField"] | "";
    String channelFieldStr = explicitChannelField.isEmpty()
                                 ? resolveChannelField(categoryValue, String(periphId), explicitFieldStr)
                                 : explicitChannelField;
    const char* dataField = channelFieldStr.c_str();

    if (strlen(periphId) == 0) {
        LOGGER.warning("[PeriphExec] Sensor read: no periphId specified");
        return false;
    }

    float rawValue = 0;
    String detectedUnit;
    uint32_t sensorSampleTimestamp = 0;
    uint32_t sensorMaxAgeMs = 0;
    if (!sourceCurrent()) return false;
    const int64_t acquisitionUs = esp_timer_get_time();
    if (acquisitionUs < 0) { result.remark = "sensor_clock_invalid"; return false; }
    auto& sample = result.sensorSample;
    sample.sampledAtMs = static_cast<uint64_t>(acquisitionUs) / 1000;
    sample.capturedAtMs = sample.sampledAtMs;
    sample.maxAgeMs = PeriphExecManager::SENSOR_CACHE_TTL_MS;
    sample.quality = FastBee::SensorSampleQuality::Good;

    if (strcmp(category, "analog") == 0) {
        // 热路径零分配：避免 String(periphId) 每周期堆构造
        rawValue = pm.readAnalogValueByCStr(periphId);
        if (isnan(rawValue) || isinf(rawValue)) {
            LOGGER.warningf("[PeriphExec] Sensor read: analog conversion failed on '%s'", periphId);
            return false;
        }
    } else if (strcmp(category, "digital") == 0) {
        GPIOState state = pm.readPinByCStr(periphId);
        if (state == GPIOState::STATE_UNDEFINED) return false;
        rawValue = (state == GPIOState::STATE_HIGH) ? 1.0f : 0.0f;
    } else if (strcmp(category, "radar") == 0) {
        bool detected = false;
        if (!pm.readRadarState(String(periphId), detected)) {
            LOGGER.warningf("[PeriphExec] Sensor read: radar read failed on '%s'", periphId);
            return false;
        }
        rawValue = detected ? 1.0f : 0.0f;
    } else if (strcmp(category, "rf") == 0 || strcmp(category, "rfLevel") == 0) {
        bool level = false;
        if (!pm.readRfLevel(String(periphId), level)) {
            LOGGER.warningf("[PeriphExec] Sensor read: RF level read failed on '%s'", periphId);
            return false;
        }
        rawValue = level ? 1.0f : 0.0f;
    } else if (strcmp(category, "pulse") == 0 || strcmp(category, "frequency") == 0) {
        EncoderReading reading;
        if (!pm.getEncoderReading(String(periphId), reading)) {
            LOGGER.warningf("[PeriphExec] Sensor read: encoder '%s' is not ready", periphId);
            return false;
        }
        if (strcmp(category, "frequency") == 0) {
            rawValue = strcmp(dataField, "rpm") == 0
                ? reading.rpm : reading.frequencyHz;
        } else if (strcmp(dataField, "delta") == 0) {
            rawValue = static_cast<float>(reading.delta);
        } else if (strcmp(dataField, "direction") == 0) {
            rawValue = static_cast<float>(reading.direction);
        } else {
            rawValue = static_cast<float>(reading.count);
        }
#if FASTBEE_ENABLE_SENSOR_DRIVER
    } else if (strcmp(category, "dht11") == 0 || strcmp(category, "dht22") == 0) {
        // DHT11/DHT22 温湿度传感器
        // Web 忙让位：DHT 位时序读取需关中断忙等（InterruptLock），
        // WiFi 高负载下时序被拉伸会触发 "Interrupt wdt timeout on CPU1"
        // panic（2026-08-10 现场：Web 响应期间 worker 读 DHT → 整机复位）。
        // 前台窗口/静态传输在途时跳过物理读取，等下个轮询周期再读。
        {
            FastBeeFramework* fw = FastBeeFramework::getInstance();
            WebConfigManager* wcm = fw ? fw->getWebConfigManager() : nullptr;
            if (wcm && wcm->isWebBusy()) {
                static unsigned long s_lastSkipLog = 0;
                if (millis() - s_lastSkipLog > 30000UL) {
                    LOGGER.info("[PeriphExec] Sensor read deferred: web busy (DHT timing-sensitive)");
                    s_lastSkipLog = millis();
                }
                return false;
            }
        }
        PeripheralConfig cfg;
        if (!loadSensorReadSnapshot(pm, periphId, cfg)) return false;
        uint8_t pin = cfg.getPrimaryPin();
        if (pin == 255) {
            LOGGER.warningf("[PeriphExec] Sensor read: peripheral '%s' has no valid pin", periphId);
            return false;
        }
        // 保留引脚拦截：DHT 位时序读取会关中断忙等，若引脚落在芯片保留区
        // （如 S3 Octal Flash/PSRAM 总线 GPIO26-37）直接踩坏总线触发
        // TG1WDT/INT_WDT 复位循环（2026-08-13 实测）
        if (pm.isReservedPin(pin)) {
            static unsigned long s_lastResvLog = 0;
            if (millis() - s_lastResvLog > 30000UL) {
                LOGGER.warningf("[PeriphExec] Sensor read blocked: '%s' GPIO%d reserved on %s", periphId, pin, CHIP_NAME);
                s_lastResvLog = millis();
            }
            return false;
        }
        SensorDriverType dhtType = (strcmp(category, "dht11") == 0) ?
            SensorDriverType::DHT11 : SensorDriverType::DHT22;
        rawValue = SensorDriver::getInstance().readDHT(pin, dhtType, String(dataField), &sample);
        detectedUnit = strcmp(dataField, "humidity") == 0 ? "%" : "°C";
        if (isnan(rawValue)) {
            LOGGER.warningf("[PeriphExec] Sensor read: DHT read failed on '%s' pin %d", periphId, pin);
#if FASTBEE_ENABLE_LCD
            invalidateSensorReadDisplay(periphId, dataField);
#endif
            return false;
        }
    } else if (strcmp(category, "ds18b20") == 0) {
        // DS18B20 数字温度传感器（同为单总线时序敏感，Web 忙时同样让位）
        {
            FastBeeFramework* fw = FastBeeFramework::getInstance();
            WebConfigManager* wcm = fw ? fw->getWebConfigManager() : nullptr;
            if (wcm && wcm->isWebBusy()) {
                return false;
            }
        }
        PeripheralConfig cfg;
        if (!loadSensorReadSnapshot(pm, periphId, cfg)) return false;
        uint8_t pin = cfg.getPrimaryPin();
        if (pin == 255) {
            LOGGER.warningf("[PeriphExec] Sensor read: peripheral '%s' has no valid pin", periphId);
            return false;
        }
        if (pm.isReservedPin(pin)) {
            LOGGER.warningf("[PeriphExec] Sensor read blocked: '%s' GPIO%d reserved on %s", periphId, pin, CHIP_NAME);
            return false;
        }
        uint8_t deviceIndex = 0, deviceAddress[8];
        bool addressed = false;
        if (const char* error = FastBee::parseDs18b20Selection(doc.as<JsonVariantConst>(), deviceIndex,
                                                             deviceAddress, addressed)) {
            result.remark = error;
            return false;
        }
        rawValue = SensorDriver::getInstance().readDS18B20(pin, deviceIndex, &sample,
            addressed ? deviceAddress : nullptr);
        detectedUnit = "°C";
        if (isnan(rawValue)) {
            LOGGER.warningf("[PeriphExec] Sensor read: DS18B20 read failed on '%s' pin %d", periphId, pin);
#if FASTBEE_ENABLE_LCD
            invalidateSensorReadDisplay(periphId, dataField);
#endif
            return false;
        }
    } else if (strcmp(category, "ultrasonic") == 0) {
        PeripheralConfig cfg;
        if (!loadSensorReadSnapshot(pm, periphId, cfg)) return false;
        if (cfg.pinCount < 2 || cfg.pins[0] == 255 || cfg.pins[1] == 255) {
            LOGGER.warningf("[PeriphExec] Sensor read: ultrasonic '%s' needs trig+echo pins", periphId);
            return false;
        }
        rawValue = SensorDriver::getInstance().readUltrasonic(cfg.pins[0], cfg.pins[1], &sample);
        detectedUnit = "cm";
        if (isnan(rawValue)) {
            LOGGER.warningf("[PeriphExec] Sensor read: ultrasonic read failed on '%s'", periphId);
#if FASTBEE_ENABLE_LCD
            invalidateSensorReadDisplay(periphId, dataField);
#endif
            return false;
        }
    } else if (strcmp(category, "current") == 0) {
        PeripheralConfig cfg;
        if (!loadSensorReadSnapshot(pm, periphId, cfg)) return false;
        uint8_t pin = cfg.getPrimaryPin();
        if (pin == 255) {
            LOGGER.warningf("[PeriphExec] Sensor read: current sensor '%s' has no valid pin", periphId);
            return false;
        }
        ADCSensorCalibration cal;
        cal.sensitivity = doc["sensitivity"] | 0.100f;
        cal.offset = doc["zeroOffset"] | (doc["sensorOffset"] | 1.65f);
        cal.vRef = doc["vRef"] | 3.3f;
        cal.adcMax = doc["adcMax"] | 4095;
        rawValue = SensorDriver::getInstance().readCurrent(pin, cal, &sample);
        detectedUnit = "A";
        if (isnan(rawValue)) {
            LOGGER.warningf("[PeriphExec] Sensor read: current read failed on '%s'", periphId);
            return false;
        }
    } else if (strcmp(category, "current_loop") == 0 ||
               strcmp(category, "4-20ma") == 0 ||
               strcmp(category, "4_20ma") == 0) {
        // Industrial 4-20 mA input. The loop is converted through an
        // external shunt/front-end and is rejected when it is disconnected or
        // outside the configured range.
        PeripheralConfig cfg;
        if (!loadSensorReadSnapshot(pm, periphId, cfg)) return false;
        const uint8_t pin = cfg.getPrimaryPin();
        if (pin == 255) {
            LOGGER.warningf("[PeriphExec] Sensor read: 4-20mA sensor '%s' has no valid pin", periphId);
            return false;
        }
        if (pm.isReservedPin(pin)) {
            LOGGER.warningf("[PeriphExec] Sensor read blocked: '%s' GPIO%d reserved on %s", periphId, pin, CHIP_NAME);
            return false;
        }

        CurrentLoopCalibration cal;
        auto readFloat = [&doc](const char* primary, const char* alias, float fallback) {
            JsonVariantConst value = doc[primary];
            if (value.isNull() && alias) value = doc[alias];
            return value | fallback;
        };
        auto readBool = [&doc](const char* primary, const char* alias, bool fallback) {
            JsonVariantConst value = doc[primary];
            if (value.isNull() && alias) value = doc[alias];
            return value | fallback;
        };
        cal.shuntResistance = readFloat("shuntResistance", "shuntOhms", 165.0f);
        cal.inputGain = readFloat("inputGain", "gain", 1.0f);
        cal.inputOffset = readFloat("inputOffset", "voltageOffset", 0.0f);
        cal.loopMinMa = readFloat("loopMinMa", "minMa", 4.0f);
        cal.loopMaxMa = readFloat("loopMaxMa", "maxMa", 20.0f);
        cal.engineeringMin = readFloat("engineeringMin", "rangeMin", 0.0f);
        cal.engineeringMax = readFloat("engineeringMax", "rangeMax", 100.0f);
        cal.vRef = readFloat("vRef", nullptr, 3.3f);
        cal.adcMax = static_cast<uint16_t>(doc["adcMax"] | 4095);
        cal.sampleCount = static_cast<uint8_t>(doc["sampleCount"] | 8);
        cal.mapToEngineering = readBool("mapToEngineering", "engineeringMapping", false);

        rawValue = SensorDriver::getInstance().readCurrentLoop(pin, cal, &sample);
        if (!cal.mapToEngineering) detectedUnit = "mA";
        if (isnan(rawValue)) {
            LOGGER.warningf("[PeriphExec] Sensor read: 4-20mA read failed on '%s'", periphId);
            return false;
        }
    } else if (strcmp(category, "voltage") == 0) {
        PeripheralConfig cfg;
        if (!loadSensorReadSnapshot(pm, periphId, cfg)) return false;
        uint8_t pin = cfg.getPrimaryPin();
        if (pin == 255) {
            LOGGER.warningf("[PeriphExec] Sensor read: voltage sensor '%s' has no valid pin", periphId);
            return false;
        }
        ADCSensorCalibration cal;
        cal.ratio = doc["ratio"] | 1.0f;
        cal.vRef = doc["vRef"] | 3.3f;
        cal.adcMax = doc["adcMax"] | 4095;
        rawValue = SensorDriver::getInstance().readVoltage(pin, cal, &sample);
        detectedUnit = "V";
        if (isnan(rawValue)) {
            LOGGER.warningf("[PeriphExec] Sensor read: voltage read failed on '%s'", periphId);
            return false;
        }
    } else if (strcmp(category, "ntc") == 0) {
        // NTC 热敏电阻（分压器 + B 参数法测温）
        PeripheralConfig cfg;
        if (!loadSensorReadSnapshot(pm, periphId, cfg)) return false;
        uint8_t pin = cfg.getPrimaryPin();
        if (pin == 255) {
            LOGGER.warningf("[PeriphExec] Sensor read: NTC sensor '%s' has no valid pin", periphId);
            return false;
        }
        NTCSensorCalibration cal;
        cal.seriesResistor = doc["seriesResistor"] | 10000.0f;
        cal.nominalResistance = doc["nominalResistance"] | 10000.0f;
        cal.nominalTempC = doc["nominalTempC"] | 25.0f;
        cal.bCoefficient = doc["bCoefficient"] | 3950.0f;
        cal.vRef = doc["vRef"] | 3.3f;
        cal.adcMax = doc["adcMax"] | 4095;
        rawValue = SensorDriver::getInstance().readNTC(pin, cal, &sample);
        detectedUnit = "°C";
        if (isnan(rawValue)) {
            LOGGER.warningf("[PeriphExec] Sensor read: NTC read failed on '%s'", periphId);
            return false;
        }
#endif // FASTBEE_ENABLE_SENSOR_DRIVER
    } else {
        String sensorError;
        auto failed = [&](const String& error) {
            PeriphExecManager::getInstance().invalidateSensorReadCache(String(periphId), error, sensorConfigVersion);
            LOGGER.warningf("[PeriphExec] Sensor read: %s", error.c_str());
            return false;
        };
        if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("i2cSensors")) {
            return failed("I2C_SENSORS_UNAVAILABLE: enable the feature and reboot");
        }
        PeripheralConfig cfg = boundConfig;
        if (!configuredI2cSensor) {
            // Legacy rules may attach a registered driver to a configured I2C
            // bus/container. Keep their address/mode/driverParams, but resolve
            // physical wiring through the existing peripheral configuration.
            if (cfg.type != PeripheralType::I2C && cfg.type != PeripheralType::SENSOR_GENERIC) {
                return failed("SENSOR_I2C_REFERENCE_REQUIRED");
            }
            uint8_t sda = 255, scl = 255;
            uint32_t frequency = 0;
            if (cfg.type == PeripheralType::I2C &&
                (!cfg.params.i2c.isMaster || cfg.params.i2c.address != 0)) {
                return failed("SENSOR_I2C_MASTER_REQUIRED");
            }
            if (!pm.resolveI2cConfig(cfg, sda, scl, frequency, sensorError)) return failed(sensorError);
            JsonDocument legacy;
            JsonVariantConst supplied = doc["driverParams"];
            if (supplied.is<const char*>()) {
                if (deserializeJson(legacy, supplied.as<const char*>()) || !legacy.is<JsonObject>()) {
                    return failed("SENSOR_PARAMS_INVALID");
                }
            } else if (supplied.is<JsonObjectConst>()) legacy.set(supplied);
            else if (!supplied.isNull()) return failed("SENSOR_PARAMS_INVALID");
            legacy["driver"] = category;
            if (!legacy["addr"].isNull()) legacy["address"] = legacy["addr"];
            legacy["frequency"] = frequency;
            initializeSensorDefaults(cfg);
            cfg.type = PeripheralType::SENSOR_GENERIC;
            cfg.pinCount = 2; cfg.pins[0] = sda; cfg.pins[1] = scl;
            if (!FastBee::parseSensorConfig(legacy.as<JsonObjectConst>(), cfg, sensorError)) return failed(sensorError);
        }
        if (!FastBee::validateSensorConfig(cfg, sensorError)) return failed(sensorError);
        uint8_t sda = 255, scl = 255;
        uint32_t frequency = 0;
        if (!pm.resolveI2cConfig(cfg, sda, scl, frequency, sensorError)) return failed(sensorError);
        // An explicit platform property ID is not a driver channel. For old
        // rules only, accept dataField as the channel when it is a real one.
        String selectedChannel;
        if (explicitChannelField.isEmpty()) {
            if (!explicitFieldStr.isEmpty() && FastBee::resolveSensorChannel(
                    cfg.params.sensor.driver, cfg.params.sensor.mode,
                    explicitFieldStr.c_str(), selectedChannel, sensorError, cfg.params.sensor.scanMask)) {
                channelFieldStr = selectedChannel;
            } else if (!FastBee::resolveSensorChannel(cfg.params.sensor.driver, cfg.params.sensor.mode,
                                                      "", channelFieldStr, sensorError, cfg.params.sensor.scanMask)) return failed(sensorError);
        } else if (!FastBee::resolveSensorChannel(cfg.params.sensor.driver, cfg.params.sensor.mode,
                                                   explicitChannelField.c_str(), channelFieldStr, sensorError, cfg.params.sensor.scanMask)) {
            return failed(sensorError);
        }
        dataField = channelFieldStr.c_str();
        JsonDocument paramsDoc;
        FastBee::buildSensorDriverParams(paramsDoc, cfg, sda, scl, frequency);
        if (paramsDoc.overflowed()) return failed("SENSOR_PARAMS_MEMORY_UNAVAILABLE");
        String driverParamsBuffer;
        serializeJson(paramsDoc, driverParamsBuffer);
        sensorMaxAgeMs = FastBee::sensorSampleIntervalMs(cfg);
        if (!FastBee::SensorDriverCache::instance().readChannel(
                String(periphId), cfg.params.sensor.driver, sda,
                driverParamsBuffer.c_str(), dataField, rawValue, detectedUnit, sensorError,
                sensorMaxAgeMs, &sensorSampleTimestamp, sensorConfigVersion, collectionStartedAt, &sample)) {
            return failed(sensorError);
        }
    }

    if (!sourceCurrent()) return false;
    if (!unit[0] && !detectedUnit.isEmpty()) unit = detectedUnit.c_str();
    result.sampleUnit = unit;
    if (strlen(unit) > 32 || result.sampleUnit != unit || result.sampleUnit.length() != strlen(unit)) {
        result.reportIdentity = FastBee::ActionReportIdentity::Invalid;
        return false;
    }
    result.sampleUnitBytes = static_cast<uint8_t>(result.sampleUnit.length());
    const int64_t capturedUs = esp_timer_get_time();
    if (capturedUs < 0) { sample.quality = FastBee::SensorSampleQuality::Unknown; return false; }
    const uint64_t sampleGroup = sample.sampleGroup;
    FastBee::describeSensorSample(rawValue, sample.sampledAtMs, static_cast<uint64_t>(capturedUs) / 1000,
        sample.maxAgeMs, sample.quality == FastBee::SensorSampleQuality::Good, &sample);
    if (sample.quality != FastBee::SensorSampleQuality::Good) {
        result.remark = sample.quality == FastBee::SensorSampleQuality::Stale ? "sensor_read_stale" : "sensor_sample_unknown";
        return false;
    }
    sample.sampleGroup = sampleGroup;
    sensorSampleTimestamp = static_cast<uint32_t>(sample.sampledAtMs);
    sensorMaxAgeMs = sample.maxAgeMs;
    float processed = rawValue * scaleFactor + offset;
    if (!std::isfinite(processed)) {
        PeriphExecManager::getInstance().invalidateSensorReadCache(String(periphId), "SENSOR_VALUE_NOT_FINITE", sensorConfigVersion);
        return false;
    }
    if (decimals > 6) decimals = 6;

    result.actualValue = String(processed, (unsigned int)decimals);
    // 仅当动作配置显式指定 dataField（多字段外设如 DHT11 温/湿）时携带；
    // 未显式配置时留空，上报路径将回退用外设配置 id 作为物模型标识符，
    // 使平台物模型 ID 始终与设备外设配置 id 动态匹配，而非写死的默认字段
    // Identity was captured from the rule before the read; do not overwrite it
    // with a later copied field or reinterpret it through a Modbus config.
    if (result.actualValue.isEmpty()) return false;

    if (prepared) {
        auto& value = prepared->observation;
        value.sourceRef = String(periphId) + "_" + String(dataField);
        value.label = label; value.value = result.actualValue; value.unit = unit;
        value.dataField = explicitFieldStr; value.periphId = periphId;
        value.sourceRevision = value.periphId + ":" + value.dataField + ":" + value.unit;
        value.sampleTimestamp = sensorSampleTimestamp;
        value.sampledAtMs = sample.sampledAtMs;
        value.sampleIdentity = sensorSampleTimestamp;
        value.sampleGroup = sample.sampleGroup;
        value.maxAgeMs = sensorMaxAgeMs; value.recordable = recordHistory;
        value.uartOrigin = uartOrigin;
        prepared->configVersion = sensorConfigVersion;
        prepared->processed = processed; prepared->decimals = decimals;
        sampleAccepted = value.value == result.actualValue && value.label == label && value.unit == unit &&
            value.periphId == periphId && value.dataField == explicitFieldStr &&
            value.sourceRef == String(periphId) + "_" + String(dataField) &&
            value.sourceRevision == value.periphId + ":" + value.dataField + ":" + value.unit;
        return sampleAccepted;
    }

    LOGGER.infof("[PeriphExec] Sensor read: periph=%s cat=%s raw=%.1f processed=%s%s label=%s",
        periphId, category, rawValue, result.actualValue.c_str(), unit, label);

    // 更新传感器读取值缓存（供 controls API、SSE 和实时监测使用）：
    // 键用内部通道字段保证 DHT 温/湿双条目唯一；dataField 仅存显式配置值，
    // 并携带外设配置 id 供实时监测上报回退（与平台物模型标识符对应）
    {
        String cacheKey = String(periphId) + "_" + String(dataField);
        if (!PeriphExecManager::getInstance().updateSensorReadCache(cacheKey, String(label), result.actualValue,
            String(unit), explicitFieldStr, String(periphId), sensorSampleTimestamp, sensorMaxAgeMs,
            recordHistory, sensorSampleTimestamp, "", sensorConfigVersion, uartOrigin, sample.sampledAtMs)) return false;
    }
    if (!sourceCurrent()) return false;

    // Publication has been claimed; display work must stay outside PM/cache
    // guards. The display also filters the epoch when rendering cached values.
#if FASTBEE_ENABLE_LCD
    if (LCDManager::getInstance().isInitialized() && strlen(label) > 0) {
        String entryId = String(periphId) + "_" + String(dataField);
        LCDManager::getInstance().updateSensorEntry(entryId, String(label), processed,
            String(unit), decimals, sensorConfigVersion);
    }
#endif

    sampleAccepted = true;
    return true;
}

bool PeriphExecExecutor::executeSystemAction(const ExecAction& action, const FastBee::UartEventOrigin& uartOrigin,
                                           bool* sourceUnconfirmed) {
    if (sourceUnconfirmed) *sourceUnconfirmed = false;
    bool retired = false;
    const auto sourceCurrent = [&]() {
        if (!retired && PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) return true;
        retired = true;
        if (sourceUnconfirmed) *sourceUnconfirmed = true;
        return false;
    };
    if (!sourceCurrent()) return false;
    ExecActionType actType = static_cast<ExecActionType>(action.actionType);

    switch (actType) {
        case ExecActionType::ACTION_FEATURE_ENABLE:
        case ExecActionType::ACTION_FEATURE_DISABLE: {
            // 运行时功能开关：白名单仅限 webServer/mdns（支持外设规则动态恢复/关闭）
            const bool enable = (actType == ExecActionType::ACTION_FEATURE_ENABLE);
            String key = action.actionValue;
            key.trim();
            if (key != "webServer" && key != "mdns") {
                LOGGER.warningf("[PeriphExec] Feature toggle: unsupported key '%s' (allowed: webServer/mdns)", key.c_str());
                return false;
            }
            JsonDocument doc;
            if (!doc["features"][key.c_str()]["enabled"].set(enable) || doc.overflowed()) return false;
            auto& fm = FastBee::RuntimeFeatureManager::instance();
            FastBee::RuntimeFeatureManager::UserConfigSnapshot previousState;
            if (!sourceCurrent()) return false;
            const auto updateResult = fm.updateAndSave(doc, &previousState);
            if (updateResult == FastBee::RuntimeFeatureManager::ConfigUpdateResult::NO_VALID_ENTRIES) {
                LOGGER.warningf("[PeriphExec] Feature toggle rejected for '%s'", key.c_str());
                return false;
            }
            if (updateResult == FastBee::RuntimeFeatureManager::ConfigUpdateResult::SAVE_FAILED) {
                LOGGER.errorf("[PeriphExec] Feature toggle: failed to save features.json (%s)", key.c_str());
                return false;
            }

            // Retirement does not undo prior persistence or replay this action.
            if (!sourceCurrent()) return false;
            if (key == "mdns") {
                FastBeeFramework* framework = FastBeeFramework::getInstance();
                FBNetworkManager* network = framework
                    ? static_cast<FBNetworkManager*>(framework->getNetworkManager())
                    : nullptr;
                if (!network) {
                    fm.restoreUserConfig(previousState);
                    if (!fm.saveCurrentConfig()) fm.loadConfig();
                    LOGGER.error("[PeriphExec] Feature toggle: network manager unavailable for mDNS mirror");
                    return false;
                }
                WiFiConfig networkConfig = network->getConfig();
                if (!sourceCurrent()) return false;
                if (networkConfig.enableMDNS != enable) {
                    networkConfig.enableMDNS = enable;
                    if (!network->updateConfig(networkConfig, true)) {
                        fm.restoreUserConfig(previousState);
                        if (!fm.saveCurrentConfig()) fm.loadConfig();
                        LOGGER.error("[PeriphExec] Feature toggle: failed to save mDNS network mirror");
                        return false;
                    }
                }
            }
            LOGGER.infof("[PeriphExec] Feature '%s' %s, rebooting to apply...", key.c_str(), enable ? "enabled" : "disabled");
            if (!sourceCurrent()) return false;
            PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_SYS_RESTART, "", uartOrigin);
            if (!sourceCurrent()) return false;
            SystemRebooter::scheduleReboot("PeriphExec feature toggle", 1000UL, RestartReason::CONFIG_CHANGE);
            return true;
        }

        case ExecActionType::ACTION_SYS_RESTART:
            LOGGER.info("[PeriphExec] Executing system restart...");
            // 触发 restart 事件以便 DEVICE_EVENT 主题上报
            PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_SYS_RESTART, "", uartOrigin);
            if (!sourceCurrent()) return false;
            SystemRebooter::scheduleReboot("PeriphExec system restart", 1000UL, RestartReason::USER_COMMAND);
            return true;

        case ExecActionType::ACTION_SYS_FACTORY_RESET: {
            LOGGER.warning("[PeriphExec] Factory reset rejected: use authenticated reset endpoint");
            return false;
        }

        case ExecActionType::ACTION_SYS_NTP_SYNC: {
            LOGGER.info("[PeriphExec] Executing NTP sync...");
            FastBee::SystemTimeQuality::installSntpObserver();
            if (!sourceCurrent()) return false;
            return FastBee::SystemTimeZone::startSntp(nullptr, "cn.pool.ntp.org", "time.nist.gov");
        }

#if FASTBEE_ENABLE_OTA
        case ExecActionType::ACTION_SYS_OTA:
            LOGGER.info("[PeriphExec] OTA action - reserved for future implementation");
            return false;
#endif

        default:
            LOGGER.warningf("[PeriphExec] Unknown system action: %d", action.actionType);
            return false;
    }
}

bool PeriphExecExecutor::executeScriptAction(const ExecAction& action,
    const FastBee::UartEventOrigin& uartOrigin, bool* sourceUnconfirmed,
    const FastBee::OrdinaryControlContext* control, FastBee::OrdinaryControlResult* controlResult) {
    if (sourceUnconfirmed) *sourceUnconfirmed = false;
#if FASTBEE_ENABLE_COMMAND_SCRIPT
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("commandScript")) {
        LOGGER.warning("[PeriphExec] Command script disabled by runtime feature config");
        return false;
    }
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

    ScriptMqttPublisher publisher;
#if FASTBEE_ENABLE_MQTT
    // Capture identity before any script command, release immediately. A busy
    // or absent original instance stays unknown even if one appears later.
    bool needsMqtt = false;
    for (const auto& cmd : cmds) needsMqtt |= cmd.type == ScriptCmdType::CMD_MQTT;
    struct PublishContext {
        PeriphExecExecutor* executor;
        uint32_t instance;
        const FastBee::UartEventOrigin* origin;
        bool* sourceUnconfirmed;
    } context{this, needsMqtt ? captureReportInstanceId() : 0, &uartOrigin, sourceUnconfirmed};
    publisher.context = &context;
    publisher.publish = [](void* raw, size_t topic, const String& message) {
        auto& bound = *static_cast<PublishContext*>(raw);
        auto mqtt = bound.executor->getMqttClient();
        if (!PeripheralManager::getInstance().isUartEventOriginCurrent(*bound.origin)) {
            if (bound.sourceUnconfirmed) *bound.sourceUnconfirmed = true;
            return FastBee::ScriptPublishResult::Rejected;
        }
        return FastBee::publishBoundScriptTopic(mqtt.get(), bound.instance, topic, message);
    };
#endif
    return ScriptEngine::execute(cmds, publisher, uartOrigin, sourceUnconfirmed, control, controlResult);
#else
    (void)action; (void)uartOrigin;
    LOGGER.warning("[PeriphExec] Script action disabled (FASTBEE_ENABLE_COMMAND_SCRIPT=0)");
    return false;
#endif
}

bool PeriphExecExecutor::executeCallPeripheralAction(const ExecAction& action, const String& effectiveValue,
                                                      const String& receivedValue, ActionExecResult* detail,
                                                      uint32_t expectedConfigEpoch,
                                                      const FastBee::UartEventOrigin& uartOrigin, bool* sourceUnconfirmed,
                                                      const FastBee::OrdinaryControlContext* control,
                                                      FastBee::OrdinaryControlResult* controlResult) {
    // Decide UART routing from the declared target under the same configuration
    // generation used for final admission. Never let JSON redirect an action.
    if (detail) *detail = ActionExecResult{};
    if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_UART_SEND) ||
        action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL)) {
        bool isUart = false;
        uint32_t version = 0;
        if (!resolveUartActionTarget(action, expectedConfigEpoch, isUart, version, detail)) return false;
        if (!isUart) {
            auto& pm = PeripheralManager::getInstance();
            RecursiveMutexGuard guard(pm.getMutex(), 0);
            PeripheralConfig current;
            uint32_t currentVersion = 0;
            if (!guard.isLocked() || !pm.getPeripheralSnapshot(action.targetPeriphId, current, &currentVersion) || currentVersion != version) {
                if (detail) detail->remark = "CALL_CONFIG_UNCONFIRMED";
                return false;
            }
            if (current.type == PeripheralType::PCA9685_PWM) {
                FastBee::Pca9685Command command;
                String error;
                bool ok = !action.useReceivedValue && FastBee::parsePca9685Command(action.actionValue, command, error);
                if (ok && !pm.isUartEventOriginCurrent(uartOrigin)) {
                    if (sourceUnconfirmed) *sourceUnconfirmed = true;
                    error = "uart_source_unconfirmed"; ok = false;
                }
                if (ok) ok = pm.commandPca9685(action.targetPeriphId, command.action, command.channel, command.value, error,
                    control, controlResult);
                if (detail) {
                    detail->success = ok;
                    detail->remark = ok ? "PCA_LOCAL_ACCEPTED_OUTPUT_UNVERIFIED" : error.isEmpty() ? "PCA_ACTION_INVALID" : error;
                }
                return ok;
            }
        }
        if (isUart) {
            ActionExecResult uartResult;
            const bool ok = executeUartSendAction(action, effectiveValue, receivedValue, version, uartResult);
            if (detail) *detail = uartResult;
            return ok;
        }
    }
    // 支持两种格式:
    // RF uses the declared target before the legacy JSON/string conversion.
    // In particular no client actor, target rewrite or integer narrowing is admitted.
    {
        auto& rfManager = PeripheralManager::getInstance();
        PeripheralConfig rfTarget; uint32_t rfEpoch = 0;
        if (rfManager.getPeripheralSnapshot(action.targetPeriphId, rfTarget, &rfEpoch) &&
            rfTarget.type == PeripheralType::RF_MODULE) {
            if (!control || rfEpoch != expectedConfigEpoch || rfEpoch != control->configEpoch)
                return FastBee::rejectOrdinaryDigitalAction(action.targetPeriphId,
                    control ? "CONTROL_SOURCE_STALE" : "CONTROL_CONTEXT_REQUIRED", controlResult);
            return FastBee::executeRfOutputAction(rfManager, action.targetPeriphId, action.actionValue,
                effectiveValue, receivedValue, control, controlResult);
        }
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_RF_SEND))
            return FastBee::rejectOrdinaryDigitalAction(action.targetPeriphId, "CONTROL_TARGET_UNSUPPORTED", controlResult);
    }
    //   1) actionValue JSON: {"periphId":"xxx","action":"forward","value":"2"}
    //   2) targetPeriphId + actionValue 文本: forward / reverse / stop / faster / slower
    String targetId = action.targetPeriphId;
    String actionCmdStr;
    String valueString;

    // $value 替换优先级：receivedValue（平台下发值） > effectiveValue
    String substituteValue = !receivedValue.isEmpty() ? receivedValue : effectiveValue;

    if (!action.actionValue.isEmpty() && action.actionValue.charAt(0) == '{') {
        // JSON 格式: 支持 $value 模板替换
        String jsonStr = action.actionValue;
        if (!substituteValue.isEmpty() && substituteValue != action.actionValue) {
            int pos = jsonStr.indexOf("$value");
            while (pos >= 0) {
                jsonStr = jsonStr.substring(0, pos) + substituteValue + jsonStr.substring(pos + 6);
                pos = jsonStr.indexOf("$value", pos + substituteValue.length());
            }
        }
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, jsonStr.c_str(), jsonStr.length());
        if (err) {
            LOGGER.warningf("[PeriphExec] Call peripheral: JSON parse failed: %s", err.c_str());
            return false;
        }
        const char* jsonTarget = doc["periphId"] | "";
        if (strlen(jsonTarget) > 0 && targetId != jsonTarget) {
            LOGGER.warning("[PeriphExec] Call peripheral: JSON target differs from declared target");
            return false;
        }
        actionCmdStr = doc["action"] | "";
        valueString = doc["value"] | "";
        if (valueString.isEmpty()) {
            valueString = doc["code"] | "";
        }
        if (valueString.isEmpty()) {
            valueString = doc["color"] | "";
        }
        if (valueString.isEmpty()) {
            valueString = doc["text"] | "";
        }
        if (valueString.isEmpty()) {
            valueString = doc["message"] | "";
        }
        if (valueString.isEmpty() && !substituteValue.isEmpty() && substituteValue != action.actionValue) {
            valueString = substituteValue;
        }
    } else {
        // 纯文本格式: 支持 $value 占位符替换 (灯效控制/电机控制/射频发送/串口发送)
        actionCmdStr = action.actionValue;
        if (!substituteValue.isEmpty() && substituteValue != action.actionValue) {
            int pos = actionCmdStr.indexOf("$value");
            while (pos >= 0) {
                actionCmdStr = actionCmdStr.substring(0, pos) + substituteValue + actionCmdStr.substring(pos + 6);
                pos = actionCmdStr.indexOf("$value", pos + substituteValue.length());
            }
        }
        // 如果 actionValue 为空或纯 $value 已被替换，直接用 effectiveValue
        if (actionCmdStr.isEmpty()) {
            actionCmdStr = effectiveValue;
        }
    }

    if (targetId.isEmpty()) {
        LOGGER.warning("[PeriphExec] Call peripheral: no target periphId specified");
        return false;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    PeripheralConfig targetConfig;
    const PeripheralConfig* targetCfg =
        pm.getPeripheralSnapshot(targetId, targetConfig) ? &targetConfig : nullptr;
    if (!targetCfg) {
        LOGGER.warningf("[PeriphExec] Call peripheral: target not found: %s", targetId.c_str());
        return false;
    }

    LOGGER.infof("[PeriphExec] Call peripheral: %s action=%s value=%s",
        targetId.c_str(), actionCmdStr.c_str(), valueString.c_str());

    String cmd = actionCmdStr;
    cmd.toLowerCase();
    String valueStr = valueString;

    if (FastBee::isOrdinaryDigitalTarget(*targetCfg)) {
        using Command = FastBee::DigitalOutputCommand;
        Command command;
        uint32_t intervalMs = 0;
        if (cmd == "on" || cmd == "high" || cmd == "1") command = Command::High;
        else if (cmd == "off" || cmd == "low" || cmd == "0") command = Command::Low;
        else if (cmd == "toggle") command = Command::Toggle;
        else if (cmd == "stop") command = Command::Stop;
        else if (cmd == "blink") {
            command = Command::Blink;
            if (!FastBee::parseOrdinaryBlinkInterval(valueStr, intervalMs))
                return FastBee::rejectOrdinaryDigitalAction(targetId, "CONTROL_PARAMETER_INVALID", controlResult);
        } else if (isAllDigits(actionCmdStr)) {
            bool high = false;
            for (size_t i = 0; i < actionCmdStr.length(); ++i) high |= actionCmdStr.charAt(i) != '0';
            command = high ? Command::High : Command::Low;
        } else {
            return FastBee::rejectOrdinaryDigitalAction(targetId, "CONTROL_COMMAND_UNSUPPORTED", controlResult);
        }
        return FastBee::dispatchOrdinaryDigitalAction(pm, targetId, command, intervalMs, control, controlResult);
    }

    if (FastBee::isOrdinaryScalarTarget(*targetCfg)) {
        FastBee::ScalarOutputCommand command;
        uint32_t value = 0;
        if (!FastBee::parseOrdinaryScalarCommand(*targetCfg, cmd, valueStr, command, value))
            return FastBee::rejectOrdinaryDigitalAction(targetId, "CONTROL_PARAMETER_INVALID", controlResult);
        return FastBee::dispatchOrdinaryScalarAction(pm, targetId, command, value, control, controlResult);
    }
    if (targetCfg->type == PeripheralType::STEPPER_MOTOR) {
        int value = 0;
        if (!FastBee::parseStepperCommandValue(cmd, valueStr, value))
            return FastBee::rejectOrdinaryDigitalAction(targetId, "CONTROL_PARAMETER_INVALID", controlResult);
        return pm.controlStepper(targetId, cmd, value, uartOrigin, control, controlResult);
    }
    if (targetCfg->type == PeripheralType::PWM_SERVO) {
        uint32_t duty = 0;
        if (cmd != "pwm" || !FastBee::parseServoRawDuty(*targetCfg, valueStr, duty))
            return FastBee::rejectOrdinaryDigitalAction(targetId, "CONTROL_PARAMETER_INVALID", controlResult);
        return pm.submitOrdinaryServo(targetId, duty, true, control, controlResult);
    }

    if (targetCfg->type == PeripheralType::NEO_PIXEL) {
        return pm.controlNeoPixel(targetId, cmd, valueStr, control, controlResult);
    }

    if (targetCfg->type == PeripheralType::RF_MODULE)
        return FastBee::rejectOrdinaryDigitalAction(targetId, "CONTROL_SOURCE_STALE", controlResult);

    if (targetCfg->type == PeripheralType::UART) {
        // A non-UART target changed while the legacy command was parsed, or a
        // dedicated non-UART action selected UART. Do not bypass typed admission.
        if (detail) detail->remark = "UART_CONFIG_CHANGED";
        return false;
    }

    if (cmd == "on" || cmd == "high" || cmd == "1") {
        pm.stopActionTicker(targetId);
        return pm.writePin(targetId, GPIOState::STATE_HIGH);
    } else if (cmd == "off" || cmd == "low" || cmd == "0") {
        pm.stopActionTicker(targetId);
        return pm.writePin(targetId, GPIOState::STATE_LOW);
    } else if (cmd == "toggle") {
        pm.stopActionTicker(targetId);
        return pm.togglePin(targetId);
    } else if (cmd == "pwm") {
        if (targetCfg->isExpanderChannel()) return false;
        uint32_t duty = valueStr.toInt();
        return pm.writePWM(targetId, duty);
    } else if (cmd == "blink") {
        const long requestedInterval = valueStr.toInt();
        if (targetCfg->isExpanderChannel() &&
            (!valueStr.isEmpty() && (requestedInterval < 200 || requestedInterval > 65535))) return false;
        uint16_t interval = requestedInterval;
        if (interval == 0) interval = 500;
        if (targetCfg->isExpanderChannel() && (interval < 200 ||
            targetCfg->type != PeripheralType::GPIO_DIGITAL_OUTPUT ||
            pm.readPin(targetId) == GPIOState::STATE_UNDEFINED)) return false;
        return pm.startActionTicker(targetId, 1, interval, uartOrigin);
    } else if (cmd == "breathe") {
        if (targetCfg->isExpanderChannel()) return false;
        uint16_t speed = valueStr.toInt();
        if (speed == 0) speed = 2000;
        return pm.startActionTicker(targetId, 2, speed, uartOrigin);
    } else if (isAllDigits(actionCmdStr)) {
        // 纯数字直出：平台直接下发数值（如蜂鸣器占空比 0/125/250）。
        // PWM/模拟输出外设 → 按占空比 writePWM；数字输出外设 → 非零视为高电平。
        pm.stopActionTicker(targetId);
        if (targetCfg->type == PeripheralType::GPIO_PWM_OUTPUT ||
            targetCfg->type == PeripheralType::GPIO_ANALOG_OUTPUT) {
            uint32_t duty = (uint32_t)actionCmdStr.toInt();
            return pm.writePWM(targetId, duty);
        }
        if (targetCfg->type == PeripheralType::GPIO_DIGITAL_OUTPUT) {
            bool state = actionCmdStr.toInt() != 0;
            return pm.writePin(targetId, state ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW);
        }
        LOGGER.warningf("[PeriphExec] Call peripheral: numeric value '%s' unsupported for '%s' (type=%d)",
                        actionCmdStr.c_str(), targetId.c_str(), (int)targetCfg->type);
        return false;
    } else {
        LOGGER.warningf("[PeriphExec] Call peripheral: unknown action '%s'", actionCmdStr.c_str());
        return false;
    }
}

// ========== 规则控制动作（启用/禁用外设执行规则）==========

bool PeriphExecExecutor::executeRuleControlAction(const ExecAction& action) {
    if (!_manager) {
        LOGGER.warning("[PeriphExec] Rule control: manager not initialized");
        return false;
    }
    const String& ruleId = action.targetPeriphId;
    if (ruleId.isEmpty()) {
        LOGGER.warning("[PeriphExec] Rule control: no target rule ID");
        return false;
    }

    bool enable = (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_ENABLE_EXEC_RULE));
    String errorMsg;
    bool ok = enable ? _manager->enableRuleAndSave(ruleId, errorMsg)
                     : _manager->disableRuleAndSave(ruleId, errorMsg);
    if (ok) {
        LOGGER.infof("[PeriphExec] Rule control: %s rule '%s' success",
                     enable ? "enable" : "disable", ruleId.c_str());
    } else {
        LOGGER.warningf("[PeriphExec] Rule control: %s rule '%s' failed: %s",
                        enable ? "enable" : "disable", ruleId.c_str(),
                        errorMsg.isEmpty() ? "unknown error" : errorMsg.c_str());
    }
    return ok;
}

// ========== 工具方法 ==========

FastBee::MqttClientLease PeriphExecExecutor::getMqttClient(bool* absent) {
    auto* fw = FastBeeFramework::getInstance();
    if (fw) {
        auto* pm = fw->getProtocolManager();
        if (pm) return pm->acquireMQTTClient(absent);
    }
    if (absent) *absent = true;
    return {};
}

uint32_t PeriphExecExecutor::captureReportInstanceId() {
#if FASTBEE_ENABLE_MQTT
    return FastBee::captureReportInstance([this] { return getMqttClient(); });
#else
    return 0;
#endif
}

void PeriphExecExecutor::reportActionResults(const std::vector<ActionExecResult>& results,
                                            uint32_t originInstanceId) {
    if (results.empty()) return;

    // 早期短路：MQTT 完全未启用（mqttClient == nullptr）时跳过所有上报构造，
    //   避免每次采集都构造 1KB JsonDocument + 序列化字符串入队，
    //   减少 CPU 浪费与堆碎片。
    //   （“启用但临时未连接”走下方 enqueueOrCache 的缓存重试分支，仍按原逻辑入队。）
    bool mqttAbsent = false;
    (void)getMqttClient(&mqttAbsent);
    if (mqttAbsent) {
        static unsigned long s_lastWarn = 0;
        unsigned long now = millis();
        if (now - s_lastWarn > 60000) {
            LOGGER.infof("[PeriphExec] reportActionResults: MQTT disabled, dropping %d results (suppressed for 60s)",
                         (int)results.size());
            s_lastWarn = now;
        }
        return;
    }

    LOGGER.infof("[PeriphExec] reportActionResults: %d action results to process", results.size());

    // 构建上报数据：[{"id":"led1","value":"1","remark":"success"}, ...]
    // ArduinoJson 7 storage is elastic; allocation failure and exact wire-byte
    // budgets are checked below. Structured SD values can exceed 80 bytes.
    StaticJsonDocument<1024> doc;
    JsonArray arr = doc.to<JsonArray>();

    for (const auto& ar : results) {
        const auto captured = FastBee::appendCapturedReportRow(arr, ar);
        if (captured == FastBee::CapturedReportRow::Added) continue;
        if (captured == FastBee::CapturedReportRow::Invalid) {
            LOGGER.warning("[PeriphExec] REPORT_IDENTITY_OR_SERIALIZATION_FAILED; batch not queued");
            return;
        }
        // No late PM/Modbus lookup, inference from a value, or raw-ID fallback.
    }

    if (arr.size() == 0) {
        LOGGER.warning("[PeriphExec] Report: no valid results to report after filtering");
        return;
    }

    if (doc.overflowed()) {
        LOGGER.warning("[PeriphExec] REPORT_SERIALIZATION_FAILED before enqueue");
        return;
    }

    // 通过 MQTT 上报（使用线程安全队列，避免从异步任务直接调用 PubSubClient）
    auto mqtt = getMqttClient();

    auto enqueueOrCache = [&](const String& payload) {
        const auto admission = FastBee::submitBoundReport(mqtt.get(), originInstanceId, payload,
            [this](const String& value, uint32_t origin, uint32_t current) {
                return _manager && _manager->queuePendingReport(value, origin, current);
            });
        LOGGER.infof("[PeriphExec] Reporting status: %s",
            admission == FastBee::BoundReportAdmission::Queued ? "queued locally" :
            admission == FastBee::BoundReportAdmission::Retained ? "retained with original instance" :
            "not retained");
    };

    size_t emitted = 0;
    if (const char* error = FastBee::emitActionReportPayloads(arr, enqueueOrCache, emitted)) {
        LOGGER.warningf("[PeriphExec] %s (payload callbacks=%u); no automatic batch replay",
                        error, static_cast<unsigned>(emitted));
    }
}

// ========== 传感器模板变量解析 ==========
//
// 支持语法：
//   ${periphId.field}         -> 从 PeriphExecManager 的传感器缓存中读取 key=periphId_field 的 value
//   ${periphId.field.unit}    -> 同上，但取 unit 字段
//   ${periphId.field.label}   -> 同上，但取 label 字段
// 寻不到时保留原占位符文本。
//
// 考虑 TM1637 只有 4 位数码管，一般只会用到 ${id.field}。
String PeriphExecExecutor::resolveSensorTemplate(const String& input) {
    if (input.indexOf("${") < 0) return input;
    // 审计 P2-3：逐键持锁查询，避免每次解析全量深拷贝整表
    String out;
    out.reserve(input.length() + 8);
    int i = 0;
    const int n = input.length();
    while (i < n) {
        if (i + 1 < n && input[i] == '$' && input[i + 1] == '{') {
            int end = input.indexOf('}', i + 2);
            if (end < 0) {
                out += input.substring(i);
                break;
            }
            String expr = input.substring(i + 2, end);
            expr.trim();
            int dot1 = expr.indexOf('.');
            if (dot1 > 0) {
                String periphId = expr.substring(0, dot1);
                String rest = expr.substring(dot1 + 1);
                int dot2 = rest.indexOf('.');
                String field, attr;
                if (dot2 > 0) {
                    field = rest.substring(0, dot2);
                    attr = rest.substring(dot2 + 1);
                } else {
                    field = rest;
                    attr = "value";
                }
                String key = periphId + "_" + field;
                PeriphExecManager::SensorReadCache entry;
                if (PeriphExecManager::getInstance().lookupSensorCache(key, entry)) {
                    if (attr == "unit") out += entry.unit;
                    else if (attr == "label") out += entry.label;
                    else out += entry.value;
                } else {
                    // 未命中缓存：保留原占位符便于调试
                    out += input.substring(i, end + 1);
                }
            } else {
                out += input.substring(i, end + 1);
            }
            i = end + 1;
        } else {
            out += input[i];
            i++;
        }
    }
    return out;
}

#endif // FASTBEE_ENABLE_PERIPH_EXEC
