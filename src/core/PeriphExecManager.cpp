/**
 * @file PeriphExecManager.cpp
 * @brief 外设执行管理器实现
 * @author kerwincui
 * @copyright FastBee All rights reserved.
 */

#include "core/FeatureFlags.h"
#include "network/LiteralStringMatch.h"
#if FASTBEE_ENABLE_PERIPH_EXEC

#include "core/PeriphExecManager.h"
#include "core/RuleOutputIdentity.h"
#include "core/NativeOutputDispatch.h"
#include "core/ActionReportPayload.h"
#include "core/PeriphExecRemoteBindings.h"
#include "core/PeriphExecConfigValidation.h"
#include "core/RtcAction.h"
#include "core/UartAction.h"
#include "core/SdStatusAction.h"
#include "core/Ds18b20Selection.h"
#include "core/DailyTriggerState.h"
#include "core/Pca9685ConfigSupport.h"
#include "core/PeripheralActionContract.h"
#include "core/MemoryBudget.h"
#include "core/PeriphExecExecutor.h"
#include "core/PeriphExecScheduler.h"
#include "core/PeriphExecWorkerPool.h"
#include "core/PointHistory.h"
#include "core/RuleScript.h"
#include "core/FastBeeFramework.h"
#include "core/ChipConfig.h"
#include "systems/LoggerSystem.h"
#include "systems/HealthMonitor.h"
#include "core/PeripheralManager.h"
#include "core/SensorConfigEpoch.h"
#include "core/UartTextDiagnostics.h"
#include <esp_timer.h>
#include "utils/StringUtils.h"
#include "utils/FileUtils.h"
#if FASTBEE_ENABLE_REMOTE_RESOURCES
#include "protocols/RemoteResourceManager.h"
#endif
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
constexpr uint32_t PERIPH_EXEC_MIN_TIMER_INTERVAL_SEC = 1;
constexpr uint32_t PERIPH_EXEC_MIN_POLL_INTERVAL_SEC = 5;
constexpr uint32_t PERIPH_EXEC_MAX_INTERVAL_SEC = 86400UL;
constexpr uint16_t PERIPH_EXEC_MIN_POLL_TIMEOUT_MS = 100;
constexpr uint16_t PERIPH_EXEC_MAX_POLL_TIMEOUT_MS = 5000;
constexpr uint16_t PERIPH_EXEC_HEAVY_POLL_TIMEOUT_MS = 3000;
constexpr uint8_t PERIPH_EXEC_MAX_POLL_RETRIES = 3;
constexpr uint8_t PERIPH_EXEC_HEAVY_POLL_RETRIES = 2;
constexpr uint16_t PERIPH_EXEC_MIN_POLL_INTER_DELAY_MS = 20;
constexpr uint16_t PERIPH_EXEC_MAX_POLL_INTER_DELAY_MS = 1000;
constexpr uint16_t PERIPH_EXEC_HEAVY_POLL_INTER_DELAY_MS = 100;
constexpr unsigned long PERIPH_EXEC_POLL_TRIGGER_MIN_INTERVAL_MS = 1000;
constexpr unsigned long PERIPH_EXEC_HEAVY_POLL_TRIGGER_MIN_INTERVAL_MS = 2000;
constexpr unsigned long PERIPH_EXEC_MODBUS_POLL_INGRESS_MIN_INTERVAL_MS = 1000;
constexpr unsigned long PERIPH_EXEC_POLL_THROTTLE_LOG_INTERVAL_MS = 5000;
constexpr uint32_t PERIPH_EXEC_RULE_MUTATION_LOCK_TIMEOUT_MS = 2000;
constexpr unsigned long SENSOR_CACHE_LOCK_WARN_INTERVAL_MS = 5000;

static bool configError(String& error, const char* code) { error = code; return false; }

#if FASTBEE_ENABLE_REMOTE_RESOURCES
static bool canonicalRemoteRuleNumber(const String& text) {
    if (text.isEmpty() || text.length() > 24 || text.charAt(0) == '+') return false;
    size_t index = text.charAt(0) == '-' ? 1U : 0U;
    if (index >= text.length()) return false;
    const size_t integerStart = index;
    while (index < text.length() &&
           isdigit(static_cast<unsigned char>(text.charAt(index)))) {
        ++index;
    }
    if (index == integerStart ||
        (index - integerStart > 1U && text.charAt(integerStart) == '0')) {
        return false;
    }
    if (index < text.length() && text.charAt(index) == '.') {
        const size_t fractionStart = ++index;
        while (index < text.length() &&
               isdigit(static_cast<unsigned char>(text.charAt(index)))) {
            ++index;
        }
        if (index == fractionStart || index - fractionStart > 6U) return false;
    }
    if (index != text.length()) return false;
    char* end = nullptr;
    const double value = strtod(text.c_str(), &end);
    return end != text.c_str() && !*end && std::isfinite(value);
}
#endif

static bool validateRuleProfileLimits(const PeriphExecRule& rule, const char* operation, String* errorMsg = nullptr) {
    const char* issue = FastBee::PeriphExecConfig::validateRule(rule);
    if (!issue) return true;
    LOGGER.warningf("[PeriphExec] %s: %s", operation, issue);
    if (errorMsg) *errorMsg = issue;
    return false;
}

static bool actionRequiresPeripheralTarget(uint8_t actionType) {
    return FastBee::PeripheralActionContract::requiresPeripheralTarget(actionType);
}

static bool isTerminalSystemAction(uint8_t actionType) {
    return actionType == static_cast<uint8_t>(ExecActionType::ACTION_SYS_RESTART) ||
           actionType == static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_ENABLE) ||
           actionType == static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_DISABLE);
}

static bool isPanelSafeAction(uint8_t actionType) {
    switch (static_cast<ExecActionType>(actionType)) {
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
        case ExecActionType::ACTION_SENSOR_READ:
        case ExecActionType::ACTION_DISPLAY_NUMBER:
        case ExecActionType::ACTION_DISPLAY_TEXT:
        case ExecActionType::ACTION_DISPLAY_CLEAR:
        case ExecActionType::ACTION_OLED_DISPLAY:
        case ExecActionType::ACTION_GPIO_TOGGLE:
        case ExecActionType::ACTION_REMOTE_CHANNEL:
        case ExecActionType::ACTION_REMOTE_READ:
            return true;
        default:
            return false;
    }
}

static bool isTerminalExecStatus(AsyncExecStatus status) {
    return status == AsyncExecStatus::COMPLETED ||
           status == AsyncExecStatus::FAILED ||
           status == AsyncExecStatus::CANCELLED ||
           status == AsyncExecStatus::REJECTED ||
           status == AsyncExecStatus::TIMED_OUT;
}


static bool actionMatchesPeripheralTarget(uint8_t actionType, const PeripheralConfig& target) {
    return FastBee::PeripheralActionContract::matchesPeripheralTarget(actionType, target);
}

// 判断字符串是否为有效数值（整数或浮点数，含负数）
static bool isNumericString(const String& s) {
    if (s.isEmpty()) return false;
    bool hasDecimal = false;
    size_t start = 0;
    if (s.charAt(0) == '-' || s.charAt(0) == '+') {
        if (s.length() == 1) return false;
        start = 1;
    }
    for (size_t i = start; i < s.length(); i++) {
        char c = s.charAt(i);
        if (c == '.') {
            if (hasDecimal) return false;
            hasDecimal = true;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

static bool tryParseBoolLike(const String& rawValue, bool& outValue) {
    return StringUtils::tryParseBoolLike(rawValue, outValue);
}

static bool executeDirectOutputCommand(PeripheralManager& pm,
                                       const PeripheralConfig& config,
                                       const String& requestedValue,
                                       String& actualValue, uint32_t configEpoch,
                                       FastBee::OrdinaryControlResult& controlResult) {
    if (!config.enabled) return false;
    static const uint64_t mqttActor = FastBee::RuleOutputIdentity::allocateActor();

    if (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT && !FastBee::isOrdinaryScalarTarget(config)) {
        bool state = false;
        if (!tryParseBoolLike(requestedValue, state)) return false;
        actualValue = state ? "1" : "0";
        if (FastBee::isOrdinaryDigitalTarget(config)) {
            FastBee::OrdinaryControlContext context;
            if (!pm.beginOrdinaryControl(FastBee::OrdinaryControl::Source::Mqtt,
                mqttActor, 0, false, 0, String(), configEpoch, {}, context))
                return FastBee::rejectOrdinaryDigitalAction(config.id,
                    "CONTROL_CONTEXT_UNAVAILABLE", &controlResult);
            const bool accepted = FastBee::dispatchOrdinaryDigitalAction(pm, config.id,
                state ? FastBee::DigitalOutputCommand::High : FastBee::DigitalOutputCommand::Low,
                0, &context, &controlResult);
            pm.finishOrdinaryControl(context, !accepted);
            return accepted;
        }
        pm.stopActionTicker(config.id);
        return pm.writePin(config.id, state ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW);
    }

    if (FastBee::isOrdinaryScalarTarget(config)) {
        uint32_t value = 0;
        bool state = false;
        const bool dac = config.type == PeripheralType::DAC;
        const bool passive = config.type == PeripheralType::GPIO_DIGITAL_OUTPUT && config.params.gpio.passive;
        const bool boolean = !dac && tryParseBoolLike(requestedValue, state);
        if (passive && !boolean) return FastBee::rejectOrdinaryDigitalAction(config.id,
            "CONTROL_PARAMETER_INVALID", &controlResult);
        if (boolean) {
            if (state && !FastBee::ordinaryScalarHighValue(config, value))
                return FastBee::rejectOrdinaryDigitalAction(config.id,
                    "CONTROL_PARAMETER_INVALID", &controlResult);
        } else if (!FastBee::parseOrdinaryScalarValue(config, requestedValue, value))
            return FastBee::rejectOrdinaryDigitalAction(config.id,
                "CONTROL_PARAMETER_INVALID", &controlResult);
        FastBee::OrdinaryControlContext context;
        if (!pm.beginOrdinaryControl(FastBee::OrdinaryControl::Source::Mqtt,
            mqttActor, 0, false, 0, String(), configEpoch, {}, context))
            return FastBee::rejectOrdinaryDigitalAction(config.id,
                "CONTROL_CONTEXT_UNAVAILABLE", &controlResult);
        const bool accepted = FastBee::dispatchOrdinaryScalarAction(pm, config.id,
            dac ? FastBee::ScalarOutputCommand::Dac : FastBee::ScalarOutputCommand::Pwm,
            value, &context, &controlResult);
        pm.finishOrdinaryControl(context, !accepted);
        if (accepted) actualValue = passive ? (state ? "1" : "0") : String(value);
        return accepted;
    }
    if (config.type == PeripheralType::GPIO_PWM_OUTPUT ||
        config.type == PeripheralType::GPIO_ANALOG_OUTPUT) {
        uint32_t duty = 0;
        bool state = false;
        if (tryParseBoolLike(requestedValue, state)) {
            duty = state ? ((1U << config.params.gpio.pwmResolution) - 1U) : 0U;
        } else {
            duty = (uint32_t)requestedValue.toInt();
        }
        actualValue = String(duty);
        return pm.writePWM(config.id, duty);
    }

    return false;
}

static bool triggersMatch(const ExecTrigger& actual, const ExecTrigger& expected) {
    return actual.triggerType == expected.triggerType &&
           actual.triggerPeriphId == expected.triggerPeriphId &&
           actual.operatorType == expected.operatorType &&
           actual.compareValue == expected.compareValue &&
           actual.timerMode == expected.timerMode &&
           actual.intervalSec == expected.intervalSec &&
           actual.timePoint == expected.timePoint &&
           actual.eventId == expected.eventId &&
           actual.pollResponseTimeout == expected.pollResponseTimeout &&
           actual.pollMaxRetries == expected.pollMaxRetries &&
           actual.pollInterPollDelay == expected.pollInterPollDelay &&
           actual.lastTriggerTime == expected.lastTriggerTime &&
           actual.triggerCount == expected.triggerCount &&
           actual.lastCalendarDay == expected.lastCalendarDay &&
           actual._cachedHour == expected._cachedHour &&
           actual._cachedMinute == expected._cachedMinute;
}

static bool actionsMatch(const ExecAction& actual, const ExecAction& expected) {
    return actual.targetPeriphId == expected.targetPeriphId &&
           actual.actionType == expected.actionType &&
           actual.actionValue == expected.actionValue &&
           actual.useReceivedValue == expected.useReceivedValue &&
           actual.syncDelayMs == expected.syncDelayMs &&
           actual.execMode == expected.execMode;
}

static bool rulesMatch(const PeriphExecRule& actual,
                       const PeriphExecRule& expected) {
    if (actual.id != expected.id ||
        actual.name != expected.name ||
        actual.enabled != expected.enabled ||
        actual.execMode != expected.execMode ||
        actual.protocolType != expected.protocolType ||
        actual.scriptContent != expected.scriptContent ||
        actual.reportAfterExec != expected.reportAfterExec ||
        actual.allowPanelControl != expected.allowPanelControl ||
        actual.recordHistory != expected.recordHistory ||
        actual._cachedNeedsModbus != expected._cachedNeedsModbus ||
        actual._cachedHasPollCollectionAction != expected._cachedHasPollCollectionAction ||
        actual.triggers.size() != expected.triggers.size() ||
        actual.actions.size() != expected.actions.size()) {
        return false;
    }

    for (size_t i = 0; i < actual.triggers.size(); ++i) {
        if (!triggersMatch(actual.triggers[i], expected.triggers[i])) return false;
    }
    for (size_t i = 0; i < actual.actions.size(); ++i) {
        if (!actionsMatch(actual.actions[i], expected.actions[i])) return false;
    }
    return true;
}

}

// This is the single save/preflight contract for remote-channel rule actions.
// RemoteResourceManager still revalidates the value and live channel state at
// execution time before it can enqueue a publish.
bool validateRemoteChannelRuleAction(const ExecAction& action,
                                     String& errorCode,
                                     String& errorMessage) {
    errorCode = "";
    errorMessage = "";
    if (action.targetPeriphId.isEmpty()) {
        errorCode = "REMOTE_TARGET_REQUIRED";
        errorMessage = "Remote channel ID is required";
        return false;
    }

#if !FASTBEE_ENABLE_REMOTE_RESOURCES
    errorCode = "REMOTE_RESOURCES_NOT_COMPILED";
    errorMessage = "Remote resources are not compiled into this firmware";
    return false;
#else
    String channelId = action.targetPeriphId;
    if (channelId.startsWith("remote.")) channelId = channelId.substring(7);

    JsonDocument remoteConfig;
    FastBee::RemoteResourceManager::instance().getConfig(
        remoteConfig.to<JsonObject>());
    JsonObjectConst channel;
    for (JsonObjectConst candidate :
         remoteConfig["remoteChannels"].as<JsonArrayConst>()) {
        if (channelId == String(candidate["id"] | "")) {
            channel = candidate;
            break;
        }
    }
    if (channel.isNull()) {
        errorCode = "REMOTE_TARGET_NOT_FOUND";
        errorMessage = String("Remote channel '") + channelId + "' does not exist";
        return false;
    }
    if (!(channel["enabled"] | false)) {
        errorCode = "REMOTE_TARGET_DISABLED";
        errorMessage = String("Remote channel '") + channelId + "' is disabled";
        return false;
    }

    const char* channelType = channel["type"] | "";
    const bool numericChannel = strcmp(channelType, "number") == 0;
    const bool commandChannel = strcmp(channelType, "switch") == 0 ||
                                strcmp(channelType, "action") == 0;
    if (!numericChannel && !commandChannel) {
        errorCode = "REMOTE_CHANNEL_TYPE_UNSUPPORTED";
        errorMessage = String("Remote channel '") + channelId +
                       "' has an unsupported type";
        return false;
    }
    if (numericChannel && String(channel["commandTopic"] | "").isEmpty()) {
        errorCode = "REMOTE_TARGET_NOT_WRITABLE";
        errorMessage = String("Remote number channel '") + channelId +
                       "' has no command topic";
        return false;
    }

    if (action.useReceivedValue) {
        if (action.actionValue.isEmpty() || action.actionValue == "$value") return true;
        errorCode = "INVALID_REMOTE_VALUE";
        errorMessage = "Received-value remote action only supports an empty value or $value";
        return false;
    }
    if (numericChannel) {
        if (canonicalRemoteRuleNumber(action.actionValue)) return true;
        errorCode = "INVALID_REMOTE_VALUE";
        errorMessage = "Remote number value must be a canonical finite decimal";
        return false;
    }
    if (action.actionValue == "on" || action.actionValue == "off" ||
        action.actionValue == "trigger") {
        return true;
    }
    errorCode = "INVALID_REMOTE_VALUE";
    errorMessage = "Remote switch/action value must be on, off or trigger";
    return false;
#endif
}

PeriphExecManager& PeriphExecManager::getInstance() {
    static PeriphExecManager instance;
    return instance;
}

bool PeriphExecManager::initialize() {
    // 创建 FreeRTOS 同步原语
    _rulesMutex = xSemaphoreCreateMutex();
    _resultsMutex = xSemaphoreCreateMutex();
    _taskSlotSemaphore = xSemaphoreCreateCounting(MAX_ASYNC_TASKS, MAX_ASYNC_TASKS);
    _runningRulesMutex = xSemaphoreCreateMutex();
    _pollIngressMutex = xSemaphoreCreateMutex();
    _sensorCacheMutex = xSemaphoreCreateMutex();
    if (!_pendingReports.begin()) LOGGER.warning("[PeriphExec] Pending report queue unavailable");
#if FASTBEE_ENABLE_POINT_HISTORY
    FastBee::PointHistory::instance().initialize();
#endif
    // Result history is allocated only when the first operation is admitted.
    // Unused execution capability does not retain six full action traces.

    // 创建子模块
    _executor.reset(new PeriphExecExecutor());
    _executor->initialize(this);

    _scheduler.reset(new PeriphExecScheduler());
    _scheduler->initialize(this, _executor.get());

    bool loaded = loadConfiguration();

    initializeWorkerPool();

    // 注册 AsyncExecContext 对象池到 HealthMonitor（碎片预防监控）
    {
        auto* fw = FastBeeFramework::getInstance();
        HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
        if (monitor) {
            HealthMonitor::PoolStatsEntry poolEntry;
            poolEntry.name = "async_ctx";
            poolEntry.blockSize = sizeof(AsyncExecContext);
            poolEntry.capacity = AsyncExecContextPool::POOL_SIZE;
            poolEntry.getUsed = []() -> uint8_t {
                return static_cast<uint8_t>(
                    PeriphExecManager::getInstance().getContextPool().usedCount());
            };
            poolEntry.getExhaust = []() -> uint32_t {
                return PeriphExecManager::getInstance().getContextPool().exhaustCount();
            };
            monitor->registerPool(poolEntry);
            LOGGER.info("[PeriphExec] Context pool registered with HealthMonitor");
        }
    }

    // 配置加载后安全校验：检测并修正危险的任务数/轮询间隔组合
    if (loaded && _scheduler) {
        _scheduler->validateLoadedConfig();
    }

    LOGGER.infof("[PeriphExec] Initialized, loaded %d rules (async: max %d tasks)",
                 (int)rules.size(), MAX_ASYNC_TASKS);

    // 初始化完成：调度一次性启动上报（实际上报等 MQTT 连接成功后由 processBootReport 执行）
    scheduleBootReport();

    return loaded;
}

// ========== CRUD（带互斥量保护） ==========

bool PeriphExecManager::ruleNeedsModbus(const PeriphExecRule& rule) const {
    PeripheralManager& pm = PeripheralManager::getInstance();
    for (const auto& action : rule.actions) {
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_POLL) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_COIL_WRITE) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_REG_WRITE)) {
            return true;
        }
        // 检查目标外设是否为 Modbus 类型
        if (!action.targetPeriphId.isEmpty()) {
            PeripheralConfig cfg;
            if (pm.getPeripheralSnapshot(action.targetPeriphId, cfg) && cfg.isModbusPeripheral()) return true;
        }
    }
    return false;
}

bool PeriphExecManager::ruleHasPollCollectionAction(const PeriphExecRule& rule) const {
    for (const auto& action : rule.actions) {
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_POLL)) {
            return true;
        }
    }
    return false;
}

bool PeriphExecManager::ruleHasReportableStateAction(const PeriphExecRule& rule) const {
    // 与 Executor::reportRuleCurrentState 的采集范围保持一致：
    //   传感器读取 / Modbus 轮询 / 可无副作用回读的 GPIO 输出控制。
    PeripheralManager& pm = PeripheralManager::getInstance();
    for (const auto& action : rule.actions) {
        switch (static_cast<ExecActionType>(action.actionType)) {
            case ExecActionType::ACTION_SENSOR_READ:
            case ExecActionType::ACTION_MODBUS_POLL:
            case ExecActionType::ACTION_HIGH:
            case ExecActionType::ACTION_LOW:
            case ExecActionType::ACTION_HIGH_INVERTED:
            case ExecActionType::ACTION_LOW_INVERTED:
            case ExecActionType::ACTION_SET_PWM:
            case ExecActionType::ACTION_SET_DAC:
            case ExecActionType::ACTION_GPIO_TOGGLE:
                return true;
            case ExecActionType::ACTION_CALL_PERIPHERAL: {
                if (action.targetPeriphId.isEmpty()) break;
                PeripheralConfig cfg;
                if (!pm.getPeripheralSnapshot(action.targetPeriphId, cfg)) break;
                if (cfg.type == PeripheralType::GPIO_DIGITAL_OUTPUT ||
                    cfg.type == PeripheralType::GPIO_PWM_OUTPUT ||
                    cfg.type == PeripheralType::GPIO_ANALOG_OUTPUT) {
                    return true;
                }
                break;
            }
            default:
                break;
        }
    }
    return false;
}

bool PeriphExecManager::shouldAvoidSyncFallback(const PeriphExecRule& rule) const {
    PeripheralManager& pm = PeripheralManager::getInstance();
    for (const auto& action : rule.actions) {
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_POLL) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_COIL_WRITE) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_REG_WRITE) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ)) {
            return true;
        }
        // 检查目标外设是否为 Modbus 类型（Modbus 操作需要异步执行避免阻塞）
        if (!action.targetPeriphId.isEmpty()) {
            PeripheralConfig cfg;
            if (pm.getPeripheralSnapshot(action.targetPeriphId, cfg) && cfg.isModbusPeripheral()) return true;
        }
    }
    return false;
}

bool PeriphExecManager::sanitizeTriggerForSafety(ExecTrigger& trigger,
                                                 bool hasPollCollectionAction,
                                                 const String& ruleName) const {
    uint32_t originalIntervalSec = trigger.intervalSec;
    uint16_t originalTimeout = trigger.pollResponseTimeout;
    uint8_t originalRetries = trigger.pollMaxRetries;
    uint16_t originalInterDelay = trigger.pollInterPollDelay;

    if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER)) {
        if (trigger.intervalSec < PERIPH_EXEC_MIN_TIMER_INTERVAL_SEC) {
            trigger.intervalSec = PERIPH_EXEC_MIN_TIMER_INTERVAL_SEC;
        } else if (trigger.intervalSec > PERIPH_EXEC_MAX_INTERVAL_SEC) {
            trigger.intervalSec = PERIPH_EXEC_MAX_INTERVAL_SEC;
        }
        // 缓存 timePoint 解析结果，避免 checkTimerTriggers 每秒在锁内重复解析字符串
        if (trigger.timerMode == 1 && trigger.timePoint.length() >= 5) {
            int colonIdx = trigger.timePoint.indexOf(':');
            if (colonIdx > 0) {
                trigger._cachedHour = static_cast<int8_t>(trigger.timePoint.substring(0, colonIdx).toInt());
                trigger._cachedMinute = static_cast<int8_t>(trigger.timePoint.substring(colonIdx + 1).toInt());
            } else {
                trigger._cachedHour = -1;
                trigger._cachedMinute = -1;
            }
        } else {
            trigger._cachedHour = -1;
            trigger._cachedMinute = -1;
        }
    }

    if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER)) {
        // Poll 会占用串行总线和 worker；与 Scheduler 的 5 秒下限保持一致，
        // 使保存值、API 回显和实际运行值不再出现 1s/5s 分裂。
        if (trigger.intervalSec < PERIPH_EXEC_MIN_POLL_INTERVAL_SEC) {
            trigger.intervalSec = PERIPH_EXEC_MIN_POLL_INTERVAL_SEC;
        } else if (trigger.intervalSec > PERIPH_EXEC_MAX_INTERVAL_SEC) {
            trigger.intervalSec = PERIPH_EXEC_MAX_INTERVAL_SEC;
        }

        if (trigger.pollResponseTimeout < PERIPH_EXEC_MIN_POLL_TIMEOUT_MS) {
            trigger.pollResponseTimeout = PERIPH_EXEC_MIN_POLL_TIMEOUT_MS;
        } else if (trigger.pollResponseTimeout > PERIPH_EXEC_MAX_POLL_TIMEOUT_MS) {
            trigger.pollResponseTimeout = PERIPH_EXEC_MAX_POLL_TIMEOUT_MS;
        }

        if (trigger.pollMaxRetries > PERIPH_EXEC_MAX_POLL_RETRIES) {
            trigger.pollMaxRetries = PERIPH_EXEC_MAX_POLL_RETRIES;
        }

        if (trigger.pollInterPollDelay < PERIPH_EXEC_MIN_POLL_INTER_DELAY_MS) {
            trigger.pollInterPollDelay = PERIPH_EXEC_MIN_POLL_INTER_DELAY_MS;
        } else if (trigger.pollInterPollDelay > PERIPH_EXEC_MAX_POLL_INTER_DELAY_MS) {
            trigger.pollInterPollDelay = PERIPH_EXEC_MAX_POLL_INTER_DELAY_MS;
        }

        if (hasPollCollectionAction) {
            if (trigger.pollResponseTimeout > PERIPH_EXEC_HEAVY_POLL_TIMEOUT_MS) {
                trigger.pollResponseTimeout = PERIPH_EXEC_HEAVY_POLL_TIMEOUT_MS;
            }
            if (trigger.pollMaxRetries > PERIPH_EXEC_HEAVY_POLL_RETRIES) {
                trigger.pollMaxRetries = PERIPH_EXEC_HEAVY_POLL_RETRIES;
            }
            if (trigger.pollInterPollDelay < PERIPH_EXEC_HEAVY_POLL_INTER_DELAY_MS) {
                trigger.pollInterPollDelay = PERIPH_EXEC_HEAVY_POLL_INTER_DELAY_MS;
            }
        }
    }

    const bool modified = originalIntervalSec != trigger.intervalSec ||
                          originalTimeout != trigger.pollResponseTimeout ||
                          originalRetries != trigger.pollMaxRetries ||
                          originalInterDelay != trigger.pollInterPollDelay;
    if (modified) {
        LOGGER.warningf("[PeriphExec] Sanitized trigger params for rule '%s' (type=%d, interval=%lu, timeout=%u, retries=%u, delay=%u)",
                        ruleName.c_str(),
                        trigger.triggerType,
                        static_cast<unsigned long>(trigger.intervalSec),
                        static_cast<unsigned int>(trigger.pollResponseTimeout),
                        static_cast<unsigned int>(trigger.pollMaxRetries),
                        static_cast<unsigned int>(trigger.pollInterPollDelay));
    }
    return modified;
}

bool PeriphExecManager::sanitizeRuleForSafety(PeriphExecRule& rule) const {
    bool hasPollCollectionAction = ruleHasPollCollectionAction(rule);
    bool modified = false;
    for (const auto& action : rule.actions) {
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_CHANNEL) && rule.enabled) {
            rule.enabled = false;
            modified = true;
        }
    }
    for (auto& trigger : rule.triggers) {
        if (hasRetiredRemoteSource(trigger) && rule.enabled) {
            rule.enabled = false; modified = true;
        }
        modified = sanitizeTriggerForSafety(trigger, hasPollCollectionAction,
                                             rule.name) || modified;
    }
    // 缓存规则级别标志，避免热路径重复遍历 actions
    rule._cachedHasPollCollectionAction = hasPollCollectionAction;
    rule._cachedNeedsModbus = ruleNeedsModbus(rule);
    return modified;
}

unsigned long PeriphExecManager::getPollTriggerCooldownMs(const PeriphExecRule& rule, const String& source) const {
    // These are already debounced edges, not periodic sensor samples. A
    // cooldown silently loses the edge because stable inputs are not replayed.
    if (source == "digital_input") return 0;
    if ((source == "modbus" || source == "modbus_poll") && rule._cachedHasPollCollectionAction) {
        return PERIPH_EXEC_HEAVY_POLL_TRIGGER_MIN_INTERVAL_MS;
    }
    return PERIPH_EXEC_POLL_TRIGGER_MIN_INTERVAL_MS;
}

bool PeriphExecManager::shouldThrottlePollIngress(const String& source, unsigned long now) {
    if (source != "modbus_poll" || _pollIngressMutex == nullptr) {
        return false;
    }

    MutexGuard lock(_pollIngressMutex);
    if (!lock.isLocked()) {
        return false;
    }

    unsigned long& lastAccepted = _pollSourceLastAccepted[source];
    if (lastAccepted > 0 && (now - lastAccepted) < PERIPH_EXEC_MODBUS_POLL_INGRESS_MIN_INTERVAL_MS) {
        unsigned long& lastLog = _pollSourceLastThrottleLog[source];
        if (lastLog == 0 || (now - lastLog) >= PERIPH_EXEC_POLL_THROTTLE_LOG_INTERVAL_MS) {
            LOGGER.warningf("[PeriphExec] Throttle poll ingress from %s (%lums since last accept)",
                            source.c_str(),
                            now - lastAccepted);
            lastLog = now;
        }
        return true;
    }

    lastAccepted = now;
    return false;
}

bool PeriphExecManager::addRule(const PeriphExecRule& rule) {
    String errorMsg;
    return addRule(rule, errorMsg);
}

bool PeriphExecManager::addRule(const PeriphExecRule& rule, String& errorMsg) {
    errorMsg = "";
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) {
        errorMsg = "Rule service is busy, try again";
        return false;
    }

    const bool added = addRuleLocked(rule, errorMsg);
    if (added) refreshRuleRuntimeIndexesLocked();
    return added;
}

bool PeriphExecManager::validateMigrationRule(const PeriphExecRule& rule,
    const std::vector<PeripheralConfig>& targets, const std::vector<String>& ruleIds,
    String& errorMsg) const {
    return validateRuleActionsLocked(rule, errorMsg, &targets, &ruleIds);
}

bool PeriphExecManager::validateRuleActionsLocked(
    const PeriphExecRule& rule, String& errorMsg,
    const std::vector<PeripheralConfig>* targets, const std::vector<String>* ruleIds) const {
    for (const auto& trigger : rule.triggers) if (hasRetiredRemoteSource(trigger)) {
        errorMsg = "REMOTE_CHANNEL_RETIRED: replace the retired remote source"; return false;
    }
    PeripheralManager& peripherals = PeripheralManager::getInstance();
    bool hasRecordableAction = false;
    if (!FastBee::validateThermostatRule(rule, errorMsg)) return false;
    if (!FastBee::validateSdRecorderRule(rule, errorMsg)) return false;
    if (rule.enabled && FastBee::hasSdRecorderAction(rule)) {
        if (targets) { errorMsg = "SD_RECORD_IMPORT_DISABLED_REQUIRED"; return false; }
        FastBee::SdRecorderConfig recorder;
        String binding;
        if (!FastBee::parseSdRecorderConfig(rule.actions[0].actionValue, recorder, errorMsg)) return false;
        if (!sdRecorderSourceBindingLocked(recorder.sourceRef, binding)) {
            errorMsg = "SD_RECORD_UNIQUE_ENABLED_HISTORY_COLLECTOR_REQUIRED"; return false;
        }
        for (const auto& entry : rules) if (entry.first != rule.id && entry.second.enabled &&
            FastBee::hasSdRecorderAction(entry.second)) {
            errorMsg = "SD_RECORD_SINGLE_RECORDER_REQUIRED"; return false;
        }
    }
    if (!targets && rule.enabled && FastBee::hasThermostatAction(rule)) {
        FastBee::ThermostatConfig thermostat;
        String binding;
        if (!FastBee::parseThermostatConfig(rule.actions[0].actionValue, thermostat, errorMsg)) return false;
        if (!thermostatSourceBindingLocked(thermostat.sourceRef, binding)) {
            errorMsg = "THERMOSTAT_UNIQUE_ENABLED_CELSIUS_COLLECTOR_REQUIRED"; return false;
        }
        unsigned controllers = 0;
        for (const auto& entry : rules) {
            if (entry.first == rule.id || !entry.second.enabled ||
                !FastBee::hasThermostatAction(entry.second)) continue;
            ++controllers;
            if (entry.second.actions[0].targetPeriphId == rule.actions[0].targetPeriphId) {
                errorMsg = "THERMOSTAT_OUTPUT_ALREADY_CONFIGURED"; return false;
            }
        }
        if (controllers >= 4) { errorMsg = "THERMOSTAT_CONTROLLER_LIMIT"; return false; }
    }

    for (size_t index = 0; index < rule.actions.size(); ++index) {
        const ExecAction& action = rule.actions[index];
        const String actionPath = String("Action ") + String(index + 1U);

        if (action.actionType >
                static_cast<uint8_t>(ExecActionType::ACTION_SD_RECORD) ||
            action.actionType ==
                static_cast<uint8_t>(ExecActionType::ACTION_RESERVED_20)) {
            errorMsg = "ACTION_TYPE_UNSUPPORTED: " + actionPath;
            return false;
        }

        if (rule.allowPanelControl && !isPanelSafeAction(action.actionType)) {
            errorMsg = "PANEL_ACTION_NOT_ALLOWED: " + actionPath;
            return false;
        }
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SD_STATUS)) {
            if (const char* error = FastBee::validateSdStatusAction(action)) {
                errorMsg = error; return false;
            }
        }
        if (FastBee::isRtcAction(action.actionType)) {
            if (const char* error = FastBee::validateRtcAction(action)) {
                errorMsg = error; return false;
            }
        }

        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ)) {
            JsonDocument parameters;
            if (deserializeJson(parameters, action.actionValue.c_str()) || !parameters.is<JsonObject>()) {
                errorMsg = "SENSOR_CONFIG_JSON_INVALID"; return false;
            }
            String category = parameters["sensorCategory"] | "analog";
            category.trim(); category.toLowerCase();
            if (category == "ds18b20") {
                uint8_t deviceIndex = 0, address[8]; bool addressed = false;
                if (const char* error = FastBee::parseDs18b20Selection(parameters.as<JsonVariantConst>(),
                                                                     deviceIndex, address, addressed)) {
                    errorMsg = error; return false;
                }
            }
        }

        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_POLL) ||
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_READ)) {
            hasRecordableAction = true;
        }

        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_CHANNEL)) {
            errorMsg = "REMOTE_CHANNEL_RETIRED: replace the retired remote channel action";
            return false;
        }
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_READ)) {
            if (action.targetPeriphId.isEmpty() || !action.actionValue.isEmpty() ||
                action.useReceivedValue) {
                errorMsg = "ACTION_VALUE_INVALID: remote read requires an ID and no value";
                return false;
            }
            continue;
        }

        // Factory reset is deliberately not an automatable rule action. It is
        // only legal through the dedicated reset endpoint with fresh password
        // authentication and a one-time RESET confirmation.
        if (action.actionType == static_cast<uint8_t>(
                                     ExecActionType::ACTION_SYS_FACTORY_RESET)) {
            errorMsg = "ACTION_NOT_AUTOMATABLE: factory reset cannot be saved in a rule";
            return false;
        }
        if (isTerminalSystemAction(action.actionType) &&
            index + 1U != rule.actions.size()) {
            errorMsg = "ACTION_ORDER_INVALID: restart and feature actions must be last";
            return false;
        }

        if (action.actionType == static_cast<uint8_t>(
                                     ExecActionType::ACTION_TRIGGER_EVENT)) {
            if (action.targetPeriphId.isEmpty()) {
                errorMsg = "ACTION_TARGET_REQUIRED: trigger-event action requires an event ID";
                return false;
            }
            continue;
        }

        if (action.actionType == static_cast<uint8_t>(
                                     ExecActionType::ACTION_ENABLE_EXEC_RULE) ||
            action.actionType == static_cast<uint8_t>(
                                     ExecActionType::ACTION_DISABLE_EXEC_RULE)) {
            if (action.targetPeriphId.isEmpty()) {
                errorMsg = "ACTION_TARGET_REQUIRED: rule-control action requires a rule ID";
                return false;
            }
            bool found = false;
            if (ruleIds) {
                for (const auto& id : *ruleIds) if (id == action.targetPeriphId) { found = true; break; }
            } else found = rules.count(action.targetPeriphId) != 0;
            if (!found) {
                errorMsg = String("ACTION_TARGET_NOT_FOUND: target rule '") +
                           action.targetPeriphId + "' does not exist";
                return false;
            }
            continue;
        }

        const bool requiresPeripheral =
            actionRequiresPeripheralTarget(action.actionType);
        if (requiresPeripheral && action.targetPeriphId.isEmpty()) {
            errorMsg = "ACTION_TARGET_REQUIRED: " + actionPath +
                       " requires a target peripheral";
            return false;
        }

        // SENSOR_READ and CALL_PERIPHERAL can carry an embedded periphId.
        // Never allow that secondary field to redirect execution away from the
        // target which was validated and shown to the user.
        String jsonValue = action.actionValue;
        jsonValue.trim();
        const bool jsonAction = !jsonValue.isEmpty() && jsonValue[0] == '{';
        const bool sensorRead = action.actionType == static_cast<uint8_t>(
                                    ExecActionType::ACTION_SENSOR_READ);
        const bool jsonRequired = sensorRead;
        String embeddedPeriphId;
        if (jsonAction) {
            JsonDocument actionDocument;
            const DeserializationError jsonError =
                deserializeJson(actionDocument, jsonValue.c_str(), jsonValue.length());
            if (jsonError) {
                if (jsonRequired || action.actionType == static_cast<uint8_t>(
                        ExecActionType::ACTION_CALL_PERIPHERAL)) {
                    errorMsg = "ACTION_VALUE_INVALID: " + actionPath +
                               " contains invalid JSON";
                    return false;
                }
            } else if (actionDocument["periphId"].is<const char*>()) {
                embeddedPeriphId =
                    actionDocument["periphId"].as<const char*>();
            }
        } else if (jsonRequired) {
            errorMsg = "ACTION_VALUE_INVALID: sensor-read action requires JSON parameters";
            return false;
        }

        if (sensorRead && embeddedPeriphId.isEmpty()) {
            errorMsg = "ACTION_TARGET_REQUIRED: sensor-read actionValue.periphId is required";
            return false;
        }
        if (!embeddedPeriphId.isEmpty() &&
            embeddedPeriphId != action.targetPeriphId) {
            errorMsg = "ACTION_TARGET_CONFLICT: actionValue.periphId must match targetPeriphId";
            return false;
        }

        if (!requiresPeripheral) continue;

        PeripheralConfig target;
        bool found = false;
        if (targets) {
            for (const auto& item : *targets) if (item.id == action.targetPeriphId) {
                target = item;
                found = target.id == item.id && target.name == item.name;
                break;
            }
        } else found = peripherals.getPeripheralSnapshot(action.targetPeriphId, target);
        if (!found) {
            errorMsg = String("ACTION_TARGET_NOT_FOUND: peripheral '") +
                       action.targetPeriphId + "' does not exist";
            return false;
        }
        if (!actionMatchesPeripheralTarget(action.actionType, target)) {
            errorMsg = String("ACTION_TARGET_TYPE_MISMATCH: action ") +
                       String(action.actionType) + " is incompatible with " +
                       getPeripheralTypeName(target.type);
            return false;
        }
        if (target.type == PeripheralType::UART &&
            (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_UART_SEND) ||
             action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL))) {
            if (const char* error = FastBee::validateUartAction(action)) {
                errorMsg = error; return false;
            }
        }
        if (target.type == PeripheralType::PCA9685_PWM &&
            action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL)) {
            FastBee::Pca9685Command command;
            if (action.useReceivedValue || !FastBee::parsePca9685Command(action.actionValue, command, errorMsg)) {
                if (errorMsg.isEmpty()) errorMsg = "PCA_ACTION_LITERAL_REQUIRED";
                return false;
            }
            if ((strcmp(command.action, "duty") == 0 || strcmp(command.action, "pulse") == 0 || strcmp(command.action, "angle") == 0) &&
                !(target.params.pca9685.channelMask & (uint16_t(1) << command.channel))) {
                errorMsg = "PCA_CHANNEL_UNDECLARED"; return false;
            }
        }
        // A disabled target is valid for pre-configuration. Runtime execution
        // remains fail-closed until the peripheral is enabled and initialized.
    }
    if (rule.recordHistory && !hasRecordableAction) {
        errorMsg = "HISTORY_SOURCE_REQUIRED: recordHistory requires a numeric collection action";
        return false;
    }
    return true;
}

bool PeriphExecManager::addRuleLocked(const PeriphExecRule& rule,
                                      String& errorMsg,
                                      String* storedId) {
    if (const char* issue = configurationWriteError()) return configError(errorMsg, issue);
    PeriphExecRule newRule = rule;
    if (newRule.id.isEmpty()) {
        newRule.id = generateUniqueId();
    }
    if (rules.count(newRule.id)) {
        LOGGER.warningf("[PeriphExec] Rule ID already exists: %s", newRule.id.c_str());
        errorMsg = "Rule ID already exists";
        return false;
    }

    // Rules and their indexes are resident for the lifetime of the device. A
    // hard profile cap keeps RAM use bounded and matches loadConfiguration().
    if (rules.size() >= FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES) {
        LOGGER.warningf("[PeriphExec] Rule limit reached for profile %s (max=%u)",
                        FastBee::ResourceProfile::NAME,
                        static_cast<unsigned int>(FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES));
        errorMsg = String("Rule limit reached for profile ") +
                   FastBee::ResourceProfile::NAME + " (max " +
                   String(FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES) + ")";
        return false;
    }

    // 上限检查
    if (newRule.triggers.size() > MAX_TRIGGERS_PER_RULE) {
        LOGGER.warningf("[PeriphExec] Too many triggers: %d", (int)newRule.triggers.size());
        errorMsg = String("Too many triggers (max ") + String(MAX_TRIGGERS_PER_RULE) + ")";
        return false;
    }
    if (newRule.actions.size() > MAX_ACTIONS_PER_RULE) {
        LOGGER.warningf("[PeriphExec] Too many actions: %d", (int)newRule.actions.size());
        errorMsg = String("Too many actions (max ") + String(MAX_ACTIONS_PER_RULE) + ")";
        return false;
    }
    if (!validateRuleProfileLimits(newRule, "addRule", &errorMsg)) {
        return false;
    }
    if (!validateRuleActionsLocked(newRule, errorMsg)) return false;
    sanitizeRuleForSafety(newRule);
    if (const char* issue = FastBee::validateRuleCompletionGraph(rules, &newRule)) {
        errorMsg = issue; return false;
    }
    // 重置运行时字段
    for (auto& t : newRule.triggers) {
        t.lastTriggerTime = 0;
        t.triggerCount = 0;
        t.lastCalendarDay = 0;
    }
    rules[newRule.id] = newRule;
    if (storedId) *storedId = newRule.id;
    LOGGER.infof("[PeriphExec] Added rule: %s (%s)", newRule.id.c_str(), newRule.name.c_str());
    return true;
}

bool PeriphExecManager::updateRule(const String& id, const PeriphExecRule& rule) {
    String errorMsg;
    return updateRule(id, rule, errorMsg);
}

bool PeriphExecManager::updateRule(const String& id, const PeriphExecRule& rule, String& errorMsg) {
    errorMsg = "";
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) {
        errorMsg = "Rule service is busy, try again";
        return false;
    }

    const bool updated = updateRuleLocked(id, rule, errorMsg);
    if (updated) refreshRuleRuntimeIndexesLocked();
    return updated;
}

bool PeriphExecManager::updateRuleLocked(const String& id,
                                         const PeriphExecRule& rule,
                                         String& errorMsg) {
    if (const char* issue = configurationWriteError()) return configError(errorMsg, issue);
    auto it = rules.find(id);
    if (it == rules.end()) {
        LOGGER.warningf("[PeriphExec] Rule not found for update: %s", id.c_str());
        errorMsg = "Rule not found";
        return false;
    }
    // 上限检查
    if (rule.triggers.size() > MAX_TRIGGERS_PER_RULE) {
        errorMsg = String("Too many triggers (max ") + String(MAX_TRIGGERS_PER_RULE) + ")";
        return false;
    }
    if (rule.actions.size() > MAX_ACTIONS_PER_RULE) {
        errorMsg = String("Too many actions (max ") + String(MAX_ACTIONS_PER_RULE) + ")";
        return false;
    }
    if (!validateRuleProfileLimits(rule, "updateRule", &errorMsg)) return false;
    if (!validateRuleActionsLocked(rule, errorMsg)) return false;

    // 字段校验：名称不能为空，至少有一个触发器和一个动作
    if (rule.name.isEmpty()) {
        LOGGER.warning("[PeriphExec] updateRule: name is empty");
        errorMsg = "Rule name is required";
        return false;
    }
    if (rule.triggers.empty()) {
        LOGGER.warning("[PeriphExec] updateRule: no triggers");
        errorMsg = "At least one trigger is required";
        return false;
    }
    if (rule.actions.empty()) {
        LOGGER.warning("[PeriphExec] updateRule: no actions");
        errorMsg = "At least one action is required";
        return false;
    }
    // 校验触发器类型有效性 (0~4)
    for (const auto& t : rule.triggers) {
        if (t.triggerType > 5) {
            LOGGER.warningf("[PeriphExec] updateRule: invalid triggerType %d", (int)t.triggerType);
            errorMsg = "Invalid trigger type";
            return false;
        }
    }

    // 保留 per-trigger 运行时字段：按索引匹配旧触发器
    const auto& oldTriggers = it->second.triggers;
    PeriphExecRule updated = rule;
    updated.id = id;
    sanitizeRuleForSafety(updated);
    if (const char* issue = FastBee::validateRuleCompletionGraph(rules, &updated, &id)) {
        errorMsg = issue; return false;
    }
    for (size_t i = 0; i < updated.triggers.size(); i++) {
        if (i < oldTriggers.size()) {
            updated.triggers[i].lastTriggerTime = oldTriggers[i].lastTriggerTime;
            updated.triggers[i].triggerCount = oldTriggers[i].triggerCount;
        } else {
            updated.triggers[i].lastTriggerTime = 0;
            updated.triggers[i].triggerCount = 0;
        }
        FastBee::DailyTriggerState::inherit(updated.triggers[i], oldTriggers);
    }
    rules[id] = updated;
    LOGGER.infof("[PeriphExec] Updated rule: %s", id.c_str());
    return true;
}

bool PeriphExecManager::removeRule(const String& id) {
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) return false;

    String errorMsg;
    const bool removed = removeRuleLocked(id, errorMsg);
    if (removed) refreshRuleRuntimeIndexesLocked();
    return removed;
}

bool PeriphExecManager::removeRuleLocked(const String& id, String& errorMsg) {
    if (const char* issue = configurationWriteError()) return configError(errorMsg, issue);
    auto it = rules.find(id);
    if (it == rules.end()) {
        errorMsg = "Rule not found";
        return false;
    }
    rules.erase(it);
    LOGGER.infof("[PeriphExec] Removed rule: %s", id.c_str());
    return true;
}

void PeriphExecManager::refreshRuleRuntimeIndexesLocked(bool validateScheduler) {
    // Conservatively retire all captured work when the graph changes. Failed
    // writes/rollback also advance: an old snapshot must never become live again.
    if (_ruleRuntimeRevision != UINT64_MAX) ++_ruleRuntimeRevision;
    retireThermostatsLocked();
    retireSdRecorderLocked();
    FastBee::RuleOutputIdentity::invalidate();
    for (auto& entry : rules) {
        entry.second.runtimeRevision = _ruleRuntimeRevision;
        entry.second.outputActor = FastBee::RuleOutputIdentity::allocateActor();
    }
    rebuildButtonEventCache();
    rebuildDataSourceCache();
    if (validateScheduler && _scheduler) _scheduler->validateLoadedConfig();
}

bool PeriphExecManager::addRuleAndSave(const PeriphExecRule& rule,
                                       String& errorMsg) {
    return applyRuleMutationTransactional(RuleMutationType::ADD, rule.id,
                                          &rule, errorMsg);
}

bool PeriphExecManager::updateRuleAndSave(const String& id,
                                          const PeriphExecRule& rule,
                                          String& errorMsg) {
    return applyRuleMutationTransactional(RuleMutationType::UPDATE, id,
                                          &rule, errorMsg);
}

bool PeriphExecManager::removeRuleAndSave(const String& id, String& errorMsg) {
    return applyRuleMutationTransactional(RuleMutationType::REMOVE, id,
                                          nullptr, errorMsg);
}

bool PeriphExecManager::enableRuleAndSave(const String& id, String& errorMsg) {
    return applyRuleMutationTransactional(RuleMutationType::ENABLE, id,
                                          nullptr, errorMsg);
}

bool PeriphExecManager::disableRuleAndSave(const String& id, String& errorMsg) {
    return applyRuleMutationTransactional(RuleMutationType::DISABLE, id,
                                          nullptr, errorMsg);
}

bool PeriphExecManager::applyRuleMutationTransactional(
    RuleMutationType operation, const String& id,
    const PeriphExecRule* rule, String& errorMsg) {
    errorMsg = "";
    MutexGuard lock(_rulesMutex,
                    pdMS_TO_TICKS(PERIPH_EXEC_RULE_MUTATION_LOCK_TIMEOUT_MS));
    if (!lock.isLocked()) {
        errorMsg = "RULE_SERVICE_BUSY: rule configuration is locked; retry in 2s";
        return false;
    }
    if (const char* issue = configurationWriteError()) return configError(errorMsg, issue);

    RuleMutationSnapshot snapshot;
    snapshot.id = id;
    if (!id.isEmpty()) {
        auto existing = rules.find(id);
        if (existing != rules.end()) {
            snapshot.existed = true;
            snapshot.rule = existing->second;
        }
    }

    String resultId = id;
    bool mutated = false;
    switch (operation) {
        case RuleMutationType::ADD:
            if (!rule) {
                errorMsg = "INVALID_RULE_MUTATION: add requires a rule";
                return false;
            }
            mutated = addRuleLocked(*rule, errorMsg, &resultId);
            break;
        case RuleMutationType::UPDATE:
            if (!rule) {
                errorMsg = "INVALID_RULE_MUTATION: update requires a rule";
                return false;
            }
            mutated = updateRuleLocked(id, *rule, errorMsg);
            break;
        case RuleMutationType::REMOVE:
            mutated = removeRuleLocked(id, errorMsg);
            break;
        case RuleMutationType::ENABLE:
            mutated = setRuleEnabledLocked(id, true, errorMsg);
            break;
        case RuleMutationType::DISABLE:
            mutated = setRuleEnabledLocked(id, false, errorMsg);
            break;
    }

    if (!mutated) return false;

    if (saveConfigurationLocked()) {
        // Derived indexes are read without _rulesMutex. Publish the mutation
        // only after the atomic file commit succeeds.
        refreshRuleRuntimeIndexesLocked(false);
        if (_scheduler) _scheduler->validateLoadedConfig();
        return true;
    }

    const bool restored = restoreRuleMutationLocked(snapshot, resultId);
    if (restored) {
        errorMsg = "PERSISTENCE_ROLLBACK_OK: rule was not saved; "
                   "in-memory rule and runtime indexes were restored";
    } else {
        errorMsg = "ROLLBACK_FAILED: rule was not saved and runtime state "
                   "could not be restored completely";
    }
    return false;
}

bool PeriphExecManager::restoreRuleMutationLocked(
    const RuleMutationSnapshot& snapshot, const String& resultId) {
    if (snapshot.existed) {
        // UPDATE/ENABLE/DISABLE keep the map node alive so rollback does not
        // require a fresh allocation on an already memory-constrained device.
        auto existing = rules.find(snapshot.id);
        if (existing != rules.end()) {
            existing->second = snapshot.rule;
        } else {
            rules[snapshot.id] = snapshot.rule;
        }
        if (!resultId.isEmpty() && resultId != snapshot.id) {
            rules.erase(resultId);
        }
    } else {
        if (!resultId.isEmpty()) rules.erase(resultId);
        if (!snapshot.id.isEmpty() && snapshot.id != resultId) {
            rules.erase(snapshot.id);
        }
    }

    bool restored = false;
    if (snapshot.existed) {
        auto it = rules.find(snapshot.id);
        restored = it != rules.end() && rulesMatch(it->second, snapshot.rule);
    } else {
        restored = (resultId.isEmpty() || rules.count(resultId) == 0) &&
                   (snapshot.id.isEmpty() || rules.count(snapshot.id) == 0);
    }

    // The snapshot is the exact pre-mutation runtime state. Scheduler safety
    // validation could modify it again, so rollback only rebuilds derived indexes.
    refreshRuleRuntimeIndexesLocked(false);
    return restored;
}

bool PeriphExecManager::updateSensorReadCache(
    const String& key, const String& label, const String& value, const String& unit,
    const String& dataField, const String& periphId, uint32_t sampleTimestamp,
    uint32_t maxAgeMs, bool recordable, uint32_t sampleIdentity,
    const String& sourceRevision, uint32_t configVersion, const FastBee::UartEventOrigin& uartOrigin,
    uint64_t sampledAtMs) {
    ObservationUpdate update;
    update.uartOrigin = uartOrigin;
    update.sourceRef = key;
    update.label = label;
    update.value = value;
    update.unit = unit;
    update.valueType = "number";
    update.dataField = dataField;
    update.periphId = periphId;
    update.sourceRevision = sourceRevision.isEmpty()
        ? periphId + ":" + dataField + ":" + unit : sourceRevision;
    update.quality = ObservationQuality::GOOD;
    update.sampleTimestamp = sampleTimestamp;
    update.sampledAtMs = sampledAtMs;
    update.maxAgeMs = maxAgeMs;
    update.sampleIdentity = sampleIdentity;
    update.recordable = recordable;
    return upsertObservation(update, configVersion);
}

bool PeriphExecManager::upsertObservation(const ObservationUpdate& update, uint32_t configVersion) {
    static std::atomic<uint32_t> lastLockWarn{0};
    if (update.sourceRef.isEmpty() || update.sourceRef.length() > 96 ||
        !_sensorCacheMutex) return false;
    if (configVersion && (update.label.length() > 128 || update.value.length() > 96 ||
        update.unit.length() > 32 || update.dataField.length() > 64 || update.periphId.length() > 40 ||
        update.sourceRevision.length() > 192 || update.valueType.length() > 16 || update.error.length() > 160)) return false;

    const unsigned long now = millis();
    ObservationUpdate accepted;
    try { accepted = update; }
    catch (const std::bad_alloc&) { return false; }
    uint32_t publicationToken = 0;
    auto commit = [&]() {
        if (!PeripheralManager::getInstance().isUartEventOriginCurrent(update.uartOrigin)) return false;
        MutexGuard guard(_sensorCacheMutex, pdMS_TO_TICKS(10));
        if (!guard.isLocked()) return false;
        auto existing = _sensorReadCache.find(update.sourceRef);
        SensorReadCache cache;
        if (existing != _sensorReadCache.end()) cache = existing->second;
        cache.label = update.label;
        cache.unit = update.unit;
        cache.dataField = update.dataField;
        cache.periphId = update.periphId;
        cache.valueType = update.valueType.isEmpty() ? String("number") : update.valueType;
        cache.sourceRevision = update.sourceRevision;
        cache.maxAgeMs = update.maxAgeMs;
        cache.recordable = update.recordable;
        cache.bindingValid = update.bindingValid;
        cache.quality = update.quality;
        cache.error = update.error;
        cache.configVersion = configVersion;
        if (_nextSensorPublication == UINT32_MAX ||
            (!update.sampleIdentity && _nextObservationIdentity == UINT32_MAX)) return false;
        cache.publicationToken = publicationToken = _nextSensorPublication + 1;
        cache.pendingPublication = true;
        if (update.quality == ObservationQuality::GOOD) {
            cache.value = update.value;
            const int64_t nowUs = esp_timer_get_time();
            if (nowUs < 0) return false;
            const uint64_t nowMs = static_cast<uint64_t>(nowUs) / 1000;
            const uint32_t legacyAge = update.sampleTimestamp
                ? static_cast<uint32_t>(millis()) - update.sampleTimestamp : 0;
            cache.sampledAtMs = update.sampledAtMs != UINT64_MAX ? update.sampledAtMs :
                legacyAge <= nowMs ? nowMs - legacyAge : UINT64_MAX;
            const uint32_t maxAge = update.maxAgeMs ? update.maxAgeMs : SENSOR_CACHE_TTL_MS;
            if (cache.sampledAtMs > nowMs || nowMs - cache.sampledAtMs > maxAge) return false;
            cache.timestamp = static_cast<uint32_t>(cache.sampledAtMs);
            cache.sampleIdentity = update.sampleIdentity
                ? update.sampleIdentity : _nextObservationIdentity + 1;
            accepted.sampleTimestamp = cache.timestamp;
            accepted.sampledAtMs = cache.sampledAtMs;
            accepted.sampleIdentity = cache.sampleIdentity;
            cache.error = "";
        }
        if (cache.label != update.label || cache.unit != update.unit || cache.dataField != update.dataField ||
            cache.periphId != update.periphId || cache.sourceRevision != update.sourceRevision ||
            (update.quality == ObservationQuality::GOOD && cache.value != update.value) ||
            accepted.sourceRef != update.sourceRef || accepted.value != update.value || accepted.unit != update.unit ||
            accepted.label != update.label || accepted.sourceRevision != update.sourceRevision ||
            accepted.dataField != update.dataField || accepted.periphId != update.periphId ||
            accepted.valueType != update.valueType || accepted.error != update.error ||
            cache.valueType != (update.valueType.isEmpty() ? String("number") : update.valueType) ||
            (update.quality != ObservationQuality::GOOD && cache.error != update.error)) return false;
        const auto inserted = _sensorReadCache.try_emplace(update.sourceRef);
        if (inserted.first->first != update.sourceRef) {
            if (inserted.second) _sensorReadCache.erase(inserted.first);
            return false;
        }
        if (_sensorReadCache.size() > SENSOR_CACHE_MAX_ENTRIES) {
            auto oldest = _sensorReadCache.end();
            for (auto it = _sensorReadCache.begin(); it != _sensorReadCache.end(); ++it) {
                if (it == inserted.first) continue;
                if (oldest == _sensorReadCache.end() || it->second.sampledAtMs < oldest->second.sampledAtMs) oldest = it;
            }
            if (oldest != _sensorReadCache.end()) _sensorReadCache.erase(oldest);
        }
        inserted.first->second = std::move(cache);
        ++_nextSensorPublication;
        if (update.quality == ObservationQuality::GOOD && !update.sampleIdentity) ++_nextObservationIdentity;
        return true;
    };
    // Only bounded cache work is performed under the configuration guard.
    // Unbound observations (e.g. network sources) keep their existing API.
    bool cacheUpdated = false;
    try { cacheUpdated = configVersion
        ? PeripheralManager::getInstance().commitSensorRead(configVersion, commit) : commit(); }
    catch (const std::bad_alloc&) { return false; }

    if (!cacheUpdated) {
        if (configVersion && !FastBee::SensorConfigEpoch::matches(configVersion)) return false;
        uint32_t lastWarning = lastLockWarn.load(std::memory_order_relaxed);
        if ((lastWarning == 0 || (now - lastWarning) >= SENSOR_CACHE_LOCK_WARN_INTERVAL_MS) &&
            lastLockWarn.compare_exchange_strong(lastWarning, static_cast<uint32_t>(now))) {
            LOGGER.warning("[PeriphExec] Observation update skipped: mutex unavailable");
        }
        return false;
    }

    auto claim = [&]() {
        if (!PeripheralManager::getInstance().isUartEventOriginCurrent(update.uartOrigin)) return false;
        MutexGuard guard(_sensorCacheMutex, pdMS_TO_TICKS(10));
        if (!guard.isLocked()) return false;
        const auto it = _sensorReadCache.find(update.sourceRef);
        if (it == _sensorReadCache.end() || !it->second.pendingPublication ||
            it->second.configVersion != configVersion ||
            it->second.publicationToken != publicationToken) return false;
        const unsigned long maxAge = it->second.maxAgeMs
            ? it->second.maxAgeMs : SENSOR_CACHE_TTL_MS;
        const int64_t nowUs = esp_timer_get_time();
        const uint64_t nowMs = nowUs < 0 ? 0 : static_cast<uint64_t>(nowUs) / 1000;
        if (it->second.quality == ObservationQuality::GOOD && (nowUs < 0 ||
            nowMs < it->second.sampledAtMs || nowMs - it->second.sampledAtMs > maxAge)) return false;
        it->second.pendingPublication = false;
        return true;
    };
    // Claim is the publication linearization point. Mutations discard events
    // not yet claimed; transport delivery and callbacks occur outside locks.
    const bool claimed = configVersion
        ? PeripheralManager::getInstance().commitSensorRead(configVersion, claim) : claim();
    if (!claimed) return false;

    publishObservation(accepted);
    return true;
}

void PeriphExecManager::publishObservation(const ObservationUpdate& accepted) {
    const auto sourceCurrent = [&]() {
        return PeripheralManager::getInstance().isUartEventOriginCurrent(accepted.uartOrigin);
    };
    if (!sourceCurrent()) return;
    if (accepted.quality == ObservationQuality::GOOD) {
        notifySensorDataSSE(accepted.sourceRef, accepted.label, accepted.value, accepted.unit, &accepted);
        if (_scheduler && sourceCurrent()) {
            JsonDocument doc;
            JsonArray arr = doc.to<JsonArray>();
            JsonObject item = arr.add<JsonObject>();
            if (item.isNull() || !item["id"].set(accepted.sourceRef) ||
                !item["value"].set(accepted.value) || doc.overflowed()) return;
            if (sourceCurrent()) _scheduler->handlePollArray("sensor_cache", arr, accepted.uartOrigin);
        }

        if (accepted.recordable && sourceCurrent() &&
            (accepted.valueType.isEmpty() || accepted.valueType == "number")) {
            const char* begin = accepted.value.c_str();
            char* end = nullptr;
            const double numeric = strtod(begin, &end);
            while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
            if (end && end != begin && *end == '\0' && std::isfinite(numeric)) {
#if FASTBEE_ENABLE_POINT_HISTORY
                FastBee::PointHistory::instance().recordSample(
                    accepted.sourceRef, numeric, accepted.unit,
                    accepted.sourceRevision, accepted.sampleIdentity,
                    accepted.sampleTimestamp);
#endif
            }
        }
    }
}

bool PeriphExecManager::upsertSensorObservationBatch(
    const ObservationUpdate* const* updates, size_t count, uint32_t configVersion) {
    if (!updates || count < 2 || count > 4 || !_sensorCacheMutex ||
        !configVersion || !FastBee::SensorConfigEpoch::matches(configVersion)) return false;
    // Keep preparation off the worker's small stack. Steady-state updates do
    // not allocate map nodes; only first publication needs staged new nodes.
    std::unique_ptr<SensorReadCache[]> prepared(new (std::nothrow) SensorReadCache[count]);
    std::unique_ptr<ObservationUpdate[]> accepted(new (std::nothrow) ObservationUpdate[count]);
    if (!prepared || !accepted) return false;
    std::map<String, SensorReadCache> additions;
    bool missing[4] = {};
    {
        MutexGuard guard(_sensorCacheMutex, pdMS_TO_TICKS(10));
        if (!guard.isLocked()) return false;
        for (size_t i = 0; i < count; ++i) {
            if (!updates[i]) return false;
            missing[i] = _sensorReadCache.find(updates[i]->sourceRef) == _sensorReadCache.end();
        }
    }
    try {
        for (size_t i = 0; i < count; ++i) {
            const auto& update = *updates[i];
            if (update.sourceRef.isEmpty() || update.sourceRef.length() > 96 ||
                update.label.length() > 128 || update.value.isEmpty() || update.value.length() > 96 ||
                update.unit.length() > 32 || update.dataField.length() > 64 ||
                update.periphId.isEmpty() || update.periphId.length() > 40 ||
                update.sourceRevision.length() > 192 || update.valueType != "number" ||
                update.quality != ObservationQuality::GOOD || !update.error.isEmpty() ||
                (update.sampledAtMs == UINT64_MAX && !update.sampleTimestamp) || !update.maxAgeMs ||
                update.periphId != updates[0]->periphId ||
                update.sampleGroup != updates[0]->sampleGroup ||
                (!update.sampleGroup && (update.sampleTimestamp != updates[0]->sampleTimestamp ||
                                        update.sampledAtMs != updates[0]->sampledAtMs))) return false;
            for (size_t j = 0; j < i; ++j) {
                if (update.sourceRef == updates[j]->sourceRef) return false;
            }
            auto& cache = prepared[i];
            cache.label = update.label; cache.value = update.value; cache.unit = update.unit;
            cache.dataField = update.dataField; cache.periphId = update.periphId;
            cache.valueType = update.valueType; cache.sourceRevision = update.sourceRevision;
            // Arduino String reports allocation failure by becoming empty,
            // whereas STL allocation throws. Reject either before committing.
            if (cache.label != update.label || cache.value != update.value || cache.unit != update.unit ||
                cache.dataField != update.dataField || cache.periphId != update.periphId ||
                cache.valueType != update.valueType || cache.sourceRevision != update.sourceRevision) return false;
            cache.timestamp = update.sampleTimestamp; cache.maxAgeMs = update.maxAgeMs;
            const int64_t nowUs = esp_timer_get_time();
            if (nowUs < 0) return false;
            const uint64_t nowMs = static_cast<uint64_t>(nowUs) / 1000;
            const uint32_t legacyAge = static_cast<uint32_t>(millis()) - update.sampleTimestamp;
            cache.sampledAtMs = update.sampledAtMs != UINT64_MAX ? update.sampledAtMs :
                legacyAge <= nowMs ? nowMs - legacyAge : UINT64_MAX;
            cache.timestamp = static_cast<uint32_t>(cache.sampledAtMs);
            accepted[i] = update;
            if (accepted[i].sourceRef != update.sourceRef || accepted[i].label != update.label ||
                accepted[i].value != update.value || accepted[i].unit != update.unit ||
                accepted[i].dataField != update.dataField || accepted[i].periphId != update.periphId ||
                accepted[i].sourceRevision != update.sourceRevision || accepted[i].valueType != update.valueType)
                return false;
            accepted[i].sampleTimestamp = cache.timestamp;
            accepted[i].sampledAtMs = cache.sampledAtMs;
            cache.sampleIdentity = update.sampleIdentity;
            cache.recordable = update.recordable; cache.bindingValid = update.bindingValid;
            cache.quality = update.quality; cache.configVersion = configVersion;
            if (missing[i]) {
                additions.try_emplace(update.sourceRef);
                if (additions.find(update.sourceRef) == additions.end()) return false;
            }
        }
    } catch (const std::bad_alloc&) {
        return false;
    }
    uint32_t publicationToken = 0;
    auto commit = [&]() {
        for (size_t i = 0; i < count; ++i)
            if (!PeripheralManager::getInstance().isUartEventOriginCurrent(updates[i]->uartOrigin)) return false;
        MutexGuard guard(_sensorCacheMutex, pdMS_TO_TICKS(10));
        if (!guard.isLocked()) return false;
        size_t newCount = 0;
        const int64_t nowUs = esp_timer_get_time();
        if (nowUs < 0) return false;
        const uint64_t now = static_cast<uint64_t>(nowUs) / 1000;
        for (size_t i = 0; i < count; ++i) {
            const auto& update = *updates[i];
            if (now < prepared[i].sampledAtMs || now - prepared[i].sampledAtMs > update.maxAgeMs) return false;
            if (_sensorReadCache.find(update.sourceRef) == _sensorReadCache.end()) {
                if (additions.find(update.sourceRef) == additions.end()) return false;
                ++newCount;
            }
        }
        if (_sensorReadCache.size() + newCount > SENSOR_CACHE_MAX_ENTRIES) return false;
        if (_nextSensorPublication == UINT32_MAX) return false;
        ++_nextSensorPublication;
        publicationToken = _nextSensorPublication;
        for (size_t i = 0; i < count; ++i) {
            const auto& update = *updates[i];
            auto found = _sensorReadCache.find(update.sourceRef);
            if (found == _sensorReadCache.end()) {
                found = _sensorReadCache.insert(additions.extract(update.sourceRef)).position;
            }
            prepared[i].publicationToken = publicationToken;
            prepared[i].pendingPublication = true;
            // Every allocation completed above. A reader cannot observe a
            // partially replaced group while this cache lock is held.
            found->second = std::move(prepared[i]);
        }
        return true;
    };
    try {
        if (!PeripheralManager::getInstance().commitSensorRead(configVersion, commit)) return false;
    } catch (const std::bad_alloc&) {
        return false; // std::function construction must also precede mutation.
    }
    auto claim = [&]() {
        for (size_t i = 0; i < count; ++i)
            if (!PeripheralManager::getInstance().isUartEventOriginCurrent(updates[i]->uartOrigin)) return false;
        MutexGuard guard(_sensorCacheMutex, pdMS_TO_TICKS(10));
        if (!guard.isLocked()) return false;
        const int64_t nowUs = esp_timer_get_time();
        if (nowUs < 0) return false;
        const uint64_t now = static_cast<uint64_t>(nowUs) / 1000;
        for (size_t i = 0; i < count; ++i) {
            const auto found = _sensorReadCache.find(updates[i]->sourceRef);
            if (found == _sensorReadCache.end() || !found->second.pendingPublication ||
                found->second.configVersion != configVersion ||
                found->second.publicationToken != publicationToken ||
                now < found->second.sampledAtMs ||
                now - found->second.sampledAtMs > found->second.maxAgeMs) return false;
        }
        for (size_t i = 0; i < count; ++i) {
            _sensorReadCache.find(updates[i]->sourceRef)->second.pendingPublication = false;
        }
        return true;
    };
    try {
        if (!PeripheralManager::getInstance().commitSensorRead(configVersion, claim)) return false;
    } catch (const std::bad_alloc&) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) publishObservation(accepted[i]);
    return true;
}

void PeriphExecManager::invalidateSensorReadCache(const String& periphId, const String& reason,
                                                  uint32_t configVersion) {
    if (periphId.isEmpty()) return;
    const String keyPrefix = periphId + "_";
    size_t marked = 0;
    auto invalidate = [&]() {
        MutexGuard guard(_sensorCacheMutex, pdMS_TO_TICKS(10));
        if (!guard.isLocked()) return false;
        for (auto& entry : _sensorReadCache) {
            if (entry.second.periphId == periphId ||
                (entry.second.periphId.isEmpty() && entry.first.startsWith(keyPrefix))) {
                entry.second.quality = ObservationQuality::ERROR;
                entry.second.error = reason;
                entry.second.pendingPublication = false;
                ++marked;
            }
        }
        return true;
    };
    if (configVersion) PeripheralManager::getInstance().commitSensorRead(configVersion, invalidate);
    else invalidate();

    if (marked > 0) {
        LOGGER.warningf("[PeriphExec] Marked %u cached sensor value(s) invalid for %s: %s",
                        static_cast<unsigned>(marked), periphId.c_str(), reason.c_str());
    }
}

void PeriphExecManager::invalidateAllSensorReadCache() {
    MutexGuard guard(_sensorCacheMutex, pdMS_TO_TICKS(10));
    if (!guard.isLocked()) return;
    for (auto it = _sensorReadCache.begin(); it != _sensorReadCache.end();) {
        if (it->second.configVersion) it = _sensorReadCache.erase(it);
        else ++it;
    }
}

std::map<String, PeriphExecManager::SensorReadCache> PeriphExecManager::getSensorReadCacheCopy() const {
    std::map<String, SensorReadCache> copy;
    RecursiveMutexGuard configGuard(PeripheralManager::getInstance().getMutex(), pdMS_TO_TICKS(50));
    if (!configGuard.isLocked()) return copy;
    if (_sensorCacheMutex && xSemaphoreTake(_sensorCacheMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        const int64_t nowUs = esp_timer_get_time();
        const uint64_t now = nowUs < 0 ? 0 : static_cast<uint64_t>(nowUs) / 1000;
        for (const auto& entry : _sensorReadCache) {
            const unsigned long maxAge = entry.second.maxAgeMs
                ? min(entry.second.maxAgeMs, SENSOR_CACHE_TTL_MS) : SENSOR_CACHE_TTL_MS;
            if ((!entry.second.configVersion || FastBee::SensorConfigEpoch::matches(entry.second.configVersion)) &&
                entry.second.quality == ObservationQuality::GOOD &&
                nowUs >= 0 && now >= entry.second.sampledAtMs && now - entry.second.sampledAtMs <= maxAge) {
                copy.emplace(entry.first, entry.second);
            }
        }
        xSemaphoreGive(_sensorCacheMutex);
    }
    return copy;
}

bool PeriphExecManager::lookupSensorCache(const String& key, SensorReadCache& out) const {
    RecursiveMutexGuard configGuard(PeripheralManager::getInstance().getMutex(), pdMS_TO_TICKS(50));
    if (!configGuard.isLocked()) return false;
    if (!_sensorCacheMutex || xSemaphoreTake(_sensorCacheMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    bool found = false;
    auto it = _sensorReadCache.find(key);
    if (it != _sensorReadCache.end()) {
        const unsigned long maxAge = it->second.maxAgeMs
            ? min(it->second.maxAgeMs, SENSOR_CACHE_TTL_MS) : SENSOR_CACHE_TTL_MS;
        const int64_t nowUs = esp_timer_get_time();
        const uint64_t now = nowUs < 0 ? 0 : static_cast<uint64_t>(nowUs) / 1000;
        if ((!it->second.configVersion || FastBee::SensorConfigEpoch::matches(it->second.configVersion)) &&
            it->second.quality == ObservationQuality::GOOD &&
            nowUs >= 0 && now >= it->second.sampledAtMs && now - it->second.sampledAtMs <= maxAge) {
            out = it->second;
            found = true;
        }
    }
    xSemaphoreGive(_sensorCacheMutex);
    return found;
}

bool PeriphExecManager::getObservation(const String& sourceRef,
                                       ObservationSnapshot& out) const {
    RecursiveMutexGuard configGuard(PeripheralManager::getInstance().getMutex(), pdMS_TO_TICKS(50));
    if (!configGuard.isLocked()) return false;
    if (!_sensorCacheMutex || xSemaphoreTake(_sensorCacheMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    const auto it = _sensorReadCache.find(sourceRef);
    if (it == _sensorReadCache.end() || (it->second.configVersion &&
        !FastBee::SensorConfigEpoch::matches(it->second.configVersion))) {
        xSemaphoreGive(_sensorCacheMutex);
        return false;
    }
    const SensorReadCache& cache = it->second;
    out = ObservationSnapshot();
    out.sourceRef = sourceRef;
    out.label = cache.label;
    out.value = cache.value;
    out.unit = cache.unit;
    out.valueType = cache.valueType;
    out.sourceRevision = cache.sourceRevision;
    out.error = cache.error;
    out.dataField = cache.dataField;
    out.periphId = cache.periphId;
    out.quality = cache.quality;
    out.sampleTimestamp = cache.timestamp;
    out.sampledAtMs = cache.sampledAtMs;
    out.maxAgeMs = cache.maxAgeMs;
    out.sampleIdentity = cache.sampleIdentity;
    out.recordable = cache.recordable;
    out.bindingValid = cache.bindingValid;
    out.hasValue = !cache.value.isEmpty();
    const unsigned long maxAge = cache.maxAgeMs ? cache.maxAgeMs : SENSOR_CACHE_TTL_MS;
    const int64_t nowUs = esp_timer_get_time();
    const uint64_t now = nowUs < 0 ? 0 : static_cast<uint64_t>(nowUs) / 1000;
    out.stale = nowUs < 0 || now < cache.sampledAtMs || now - cache.sampledAtMs > maxAge;
    if (out.stale && out.quality == ObservationQuality::GOOD) {
        out.quality = ObservationQuality::STALE;
    }
    xSemaphoreGive(_sensorCacheMutex);
    return true;
}

bool PeriphExecManager::invalidateObservation(
    const String& sourceRef, const String& reason,
    const String& nextSourceRevision) {
    if (sourceRef.isEmpty() || reason.isEmpty() || reason.length() > 96 ||
        nextSourceRevision.length() > 64) {
        return false;
    }
    bool found = false;
    if (_sensorCacheMutex) {
        MutexGuard guard(_sensorCacheMutex, pdMS_TO_TICKS(20));
        if (!guard.isLocked()) return false;
        auto it = _sensorReadCache.find(sourceRef);
        if (it != _sensorReadCache.end()) {
            it->second.quality = ObservationQuality::OFFLINE;
            it->second.error = reason;
            it->second.pendingPublication = false;
            it->second.bindingValid = false;
            found = true;
        }
    }
#if FASTBEE_ENABLE_POINT_HISTORY
    const bool historyInvalidated = FastBee::PointHistory::instance().invalidateSource(
        sourceRef, reason, nextSourceRevision);
#else
    (void)nextSourceRevision;
    const bool historyInvalidated = false;
#endif
    return found || historyInvalidated;
}

std::vector<ObservationSnapshot> PeriphExecManager::getObservationSnapshots(size_t limit) const {
    std::vector<ObservationSnapshot> observations;
    RecursiveMutexGuard configGuard(PeripheralManager::getInstance().getMutex(), pdMS_TO_TICKS(50));
    if (!configGuard.isLocked()) return observations;
    if (limit > SENSOR_CACHE_MAX_ENTRIES) limit = SENSOR_CACHE_MAX_ENTRIES;
    if (!_sensorCacheMutex || xSemaphoreTake(_sensorCacheMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return observations;
    }
    observations.reserve(limit);
    const int64_t nowUs = esp_timer_get_time();
    const uint64_t now = nowUs < 0 ? 0 : static_cast<uint64_t>(nowUs) / 1000;
    for (const auto& entry : _sensorReadCache) {
        if (entry.second.configVersion && !FastBee::SensorConfigEpoch::matches(entry.second.configVersion)) continue;
        if (observations.size() >= limit) break;
        ObservationSnapshot snapshot;
        snapshot.sourceRef = entry.first;
        snapshot.label = entry.second.label;
        snapshot.value = entry.second.value;
        snapshot.unit = entry.second.unit;
        snapshot.valueType = entry.second.valueType;
        snapshot.sourceRevision = entry.second.sourceRevision;
        snapshot.error = entry.second.error;
        snapshot.dataField = entry.second.dataField;
        snapshot.periphId = entry.second.periphId;
        snapshot.quality = entry.second.quality;
        snapshot.sampleTimestamp = entry.second.timestamp;
        snapshot.sampledAtMs = entry.second.sampledAtMs;
        snapshot.maxAgeMs = entry.second.maxAgeMs;
        snapshot.sampleIdentity = entry.second.sampleIdentity;
        snapshot.recordable = entry.second.recordable;
        snapshot.bindingValid = entry.second.bindingValid;
        snapshot.hasValue = !entry.second.value.isEmpty();
        const unsigned long maxAge = entry.second.maxAgeMs
            ? entry.second.maxAgeMs : SENSOR_CACHE_TTL_MS;
        snapshot.stale = nowUs < 0 || now < entry.second.sampledAtMs || now - entry.second.sampledAtMs > maxAge;
        if (snapshot.stale && snapshot.quality == ObservationQuality::GOOD) {
            snapshot.quality = ObservationQuality::STALE;
        }
        observations.push_back(std::move(snapshot));
    }
    xSemaphoreGive(_sensorCacheMutex);
    return observations;
}

std::vector<ObservationSnapshot> PeriphExecManager::getObservationSnapshots(
    const std::vector<const char*>& sourceRefs) const {
    std::vector<ObservationSnapshot> observations;
    try {
        observations.reserve(std::min(sourceRefs.size(), SENSOR_CACHE_MAX_ENTRIES));
        RecursiveMutexGuard configGuard(PeripheralManager::getInstance().getMutex(), pdMS_TO_TICKS(50));
        if (!configGuard.isLocked()) return observations;
        MutexGuard cacheGuard(_sensorCacheMutex, pdMS_TO_TICKS(20));
        if (!cacheGuard.isLocked()) return observations;
        const int64_t nowUs = esp_timer_get_time();
        const uint64_t now = nowUs < 0 ? 0 : static_cast<uint64_t>(nowUs) / 1000;
        for (const char* source : sourceRefs) {
            if (!source || !source[0]) continue;
            bool duplicate = false;
            for (const auto& copied : observations) duplicate |= copied.sourceRef == source;
            if (duplicate) continue;
            const auto found = _sensorReadCache.find(String(source));
            if (found == _sensorReadCache.end() || (found->second.configVersion &&
                !FastBee::SensorConfigEpoch::matches(found->second.configVersion))) continue;
            if (observations.size() >= SENSOR_CACHE_MAX_ENTRIES) {
                observations.clear(); return observations;
            }
            const auto& cache = found->second;
            observations.emplace_back();
            auto& out = observations.back();
            out.sourceRef = source; out.label = cache.label; out.value = cache.value;
            out.unit = cache.unit; out.valueType = cache.valueType;
            out.sourceRevision = cache.sourceRevision; out.error = cache.error;
            out.dataField = cache.dataField; out.periphId = cache.periphId;
            if (out.sourceRef != source || out.label != cache.label || out.value != cache.value ||
                out.unit != cache.unit || out.valueType != cache.valueType ||
                out.sourceRevision != cache.sourceRevision || out.error != cache.error ||
                out.dataField != cache.dataField || out.periphId != cache.periphId) {
                observations.clear(); return observations;
            }
            out.quality = cache.quality; out.sampleTimestamp = cache.timestamp;
            out.sampledAtMs = cache.sampledAtMs;
            out.maxAgeMs = cache.maxAgeMs; out.sampleIdentity = cache.sampleIdentity;
            out.recordable = cache.recordable; out.bindingValid = cache.bindingValid;
            out.hasValue = !cache.value.isEmpty();
            const uint32_t maxAge = cache.maxAgeMs ? cache.maxAgeMs : SENSOR_CACHE_TTL_MS;
            out.stale = nowUs < 0 || now < cache.sampledAtMs || now - cache.sampledAtMs > maxAge;
            if (out.stale && out.quality == ObservationQuality::GOOD) out.quality = ObservationQuality::STALE;
        }
    } catch (const std::bad_alloc&) {
        observations.clear();
    }
    return observations;
}

void PeriphExecManager::evictStaleSensorCache() {
    if (!_sensorCacheMutex) return;
    if (xSemaphoreTake(_sensorCacheMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    if (_sensorReadCache.empty()) {
        xSemaphoreGive(_sensorCacheMutex);
        return;
    }

    const int64_t nowUs = esp_timer_get_time();
    const uint64_t now = nowUs < 0 ? 0 : static_cast<uint64_t>(nowUs) / 1000;
    // Epoch invalidation is immediate even when mutation-time cleanup was
    // contended. Physically reclaim those records during bounded maintenance.
    for (auto it = _sensorReadCache.begin(); it != _sensorReadCache.end();) {
        if (it->second.configVersion && !FastBee::SensorConfigEpoch::matches(it->second.configVersion)) {
            it = _sensorReadCache.erase(it);
        } else ++it;
    }
    for (auto& entry : _sensorReadCache) {
        const unsigned long maxAge = entry.second.maxAgeMs
            ? entry.second.maxAgeMs : SENSOR_CACHE_TTL_MS;
        if ((nowUs < 0 || now < entry.second.sampledAtMs || now - entry.second.sampledAtMs > maxAge) &&
            entry.second.quality == ObservationQuality::GOOD) {
            entry.second.quality = ObservationQuality::STALE;
            entry.second.error = "STALE";
        }
    }
    xSemaphoreGive(_sensorCacheMutex);
}

void PeriphExecManager::notifySensorDataSSE(const String& key, const String& label, const String& value, const String& unit,
                                          const ObservationUpdate* sample) {
    if (!_sensorSSECb) return;
    JsonDocument doc;
    if (!doc["key"].set(key) || !doc["label"].set(label) || !doc["value"].set(value) || !doc["unit"].set(unit)) return;
    const int64_t nowUs = esp_timer_get_time();
    const uint64_t now = nowUs < 0 ? 0 : static_cast<uint64_t>(nowUs) / 1000;
    const bool known = sample && nowUs >= 0 && sample->sampledAtMs <= now;
    const uint32_t maxAge = sample && sample->maxAgeMs ? sample->maxAgeMs : SENSOR_CACHE_TTL_MS;
    const bool valid = known && sample->quality == ObservationQuality::GOOD && now - sample->sampledAtMs <= maxAge;
    doc["valid"] = valid; doc["valueValid"] = valid;
    doc["quality"] = valid ? "good" : known ? "stale" : "unknown";
    if (known) {
        doc["sampledAtMs"] = sample->sampledAtMs;
        doc["ageMs"] = now - sample->sampledAtMs;
        doc["maxAgeMs"] = maxAge;
    }
    if (doc.overflowed()) return;
    String payload;
    const size_t bytes = measureJson(doc);
    if (!payload.reserve(bytes) || serializeJson(doc, payload) != bytes || payload.length() != bytes) return;
    _sensorSSECb(payload);
}


// ========== 委托给 Scheduler 的方法 ==========

void PeriphExecManager::handleMqttMessage(const String& topic, const String& message) {
    if (_scheduler) _scheduler->handleMqttMessage(topic, message);
}

void PeriphExecManager::handlePollData(const String& source, const String& data,
                                      const FastBee::UartEventOrigin& uartOrigin) {
    unsigned long now = millis();
    if (shouldThrottlePollIngress(source, now)) {
        return;
    }
    if (_scheduler) _scheduler->handlePollData(source, data, uartOrigin);
}

String PeriphExecManager::handleDataCommand(const String& message) {
    if (_scheduler) return _scheduler->handleDataCommand(message);
    return "";
}

void PeriphExecManager::checkTimers() {
    // Safety evaluation precedes the scheduler's low-memory/worker pause gates.
    checkThermostats();
    checkSdRecorder();
    if (_scheduler) _scheduler->checkTimers();
    // 重试待上报数据（MQTT 恢复后自动发送缓存的报告）
    retryPendingReports();
#if FASTBEE_ENABLE_POINT_HISTORY
    FastBee::PointHistory::instance().tick();
#endif
}

void PeriphExecManager::triggerEvent(EventType eventType, const String& eventData,
                                    const FastBee::UartEventOrigin& uartOrigin) {
    if (_scheduler) _scheduler->triggerEvent(eventType, eventData, uartOrigin);
}

void PeriphExecManager::triggerEventById(const String& eventId, const String& eventData,
                                        const FastBee::UartEventOrigin& uartOrigin, bool publishToMqtt) {
    if (_scheduler) _scheduler->triggerEventById(eventId, eventData, uartOrigin, publishToMqtt);
}

String PeriphExecManager::getStaticEventsJson() {
    return PeriphExecScheduler::getStaticEventsJson();
}

String PeriphExecManager::getDynamicEventsJson() {
    if (_scheduler) return _scheduler->getDynamicEventsJson();
    return "[]";
}

void PeriphExecManager::setModbusReadCallback(std::function<String(const String&)> callback) {
    _modbusReadCallback = callback;
    if (_executor) _executor->setModbusReadCallback(callback);
}

void PeriphExecManager::setModbusRawSendCallback(std::function<String(const String&)> callback) {
    _modbusRawSendCallback = callback;
}

void PeriphExecManager::setMqttIsConnectedCallback(MqttIsConnectedCallback cb) {
    _mqttIsConnectedCb = cb;
}

void PeriphExecManager::setMqttQueueReportCallback(MqttQueueReportCallback cb) {
    _mqttQueueReportCb = cb;
}

void PeriphExecManager::setModbusBuildSensorIdCallback(ModbusBuildSensorIdCallback cb) {
    _modbusBuildSensorIdCb = cb;
}

void PeriphExecManager::setModbusDirectControlCallback(ModbusDirectControlCallback cb) {
    _modbusDirectControlCb = cb;
}

void PeriphExecManager::setModbusDynamicEventsCallback(ModbusDynamicEventsCallback cb) {
    _modbusDynamicEventsCb = cb;
}

void PeriphExecManager::setMqttEventPublishCallback(MqttEventPublishCallback cb) {
    _mqttEventPublishCb = cb;
}

bool PeriphExecManager::notifyMqttEventPublish(const String& eventId, const String& eventName, const String& eventData,
                                             const FastBee::UartEventOrigin& uartOrigin) {
    if (!_mqttEventPublishCb) return false;
    if (eventId.isEmpty()) return false;
    // Rule callbacks may have reconfigured the UART since event dispatch.
    if (!PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) return false;
    return _mqttEventPublishCb(eventId, eventName, eventData);
}

void PeriphExecManager::checkButtonEvents() {
    if (_scheduler) _scheduler->checkButtonEvents();
}

// ========== 待上报数据缓存与重试 ==========

bool PeriphExecManager::queuePendingReport(const String& reportData, uint32_t instanceId,
                                         uint32_t currentInstanceId) {
    using Admission = FastBee::PendingReportQueue::Admission;
    const Admission result = _pendingReports.enqueueAt(reportData, [] { return static_cast<uint32_t>(millis()); },
                                                     FastBee::MemoryBudget::dramFree() >= 10000,
                                                     instanceId, currentInstanceId);
    if (result == Admission::ReplacedOldest)
        LOGGER.warning("[PeriphExec] Pending report queue full, evicted oldest unpinned report");
    const bool accepted = result == Admission::Accepted || result == Admission::ReplacedOldest;
    if (!accepted) LOGGER.warningf("[PeriphExec] Pending report admission rejected (%u)",
                                   static_cast<unsigned>(result));
    return accepted;
}

void PeriphExecManager::retryPendingReports() {
    // Protocol callbacks are configured before scheduling starts. The queue
    // does not hold its mutex while asking MQTT to admit a report.
    if (!_mqttIsConnectedCb || !_mqttQueueReportCb) return;
    const auto result = _pendingReports.retryOneAt([] { return static_cast<uint32_t>(millis()); },
        _mqttIsConnectedCb(), [&](const String& report, uint32_t instanceId) {
            return _mqttQueueReportCb(report, instanceId);
        });
    if (result == FastBee::PendingReportQueue::Retry::Rejected)
        LOGGER.warning("[PeriphExec] Pending retry rejected locally; report retained");
    else if (result == FastBee::PendingReportQueue::Retry::InstanceChanged)
        LOGGER.warning("[PeriphExec] Pending report discarded: MQTT instance changed; not replayed");
}

size_t PeriphExecManager::getPendingReportCount() const {
    return getPendingReportStatus().depth; // legacy count; new callers must check available
}

FastBee::PendingReportQueue::Status PeriphExecManager::getPendingReportStatus() const {
    return _pendingReports.snapshotAt([] { return static_cast<uint32_t>(millis()); });
}

// ========== 启动后一次性物模型状态上报 ==========

void PeriphExecManager::scheduleBootReport() {
    // 仅置位标志；实际上报时机由 processBootReport() 在 MQTT 连接成功后把控
    _bootReportPending = true;
    _bootReportDone = false;
    _bootReportListBuilt = false;
    _bootReportQueue.clear();
}

void PeriphExecManager::processBootReport() {
    // 幂等门控：未调度或已完成→直接返回（整个生命周期只上报一次）
    if (!_bootReportPending || _bootReportDone) return;

    // 启动保护：未初始化（mutex 未创建）时跳过
    if (!isInitialized()) return;

    // 上报时机门控：需网络且 MQTT 已连接后才上报（未连接则等待，每周期重入重试）
    if (!_mqttIsConnectedCb || !_mqttIsConnectedCb()) return;

    if (!_executor) {
        _bootReportDone = true;
        _bootReportPending = false;
        return;
    }

    // 首次进入：在持锁下快照 “启用 && 勾选上报 && 含可上报动作” 的规则 ID
    if (!_bootReportListBuilt) {
        if (xSemaphoreTake(_rulesMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            return;  // 拿不到锁：下周期重试
        }
        for (auto& kv : rules) {
            const PeriphExecRule& r = kv.second;
            if (!r.enabled || !r.reportAfterExec) continue;
            if (!ruleHasReportableStateAction(r)) continue;
            _bootReportQueue.push_back(r.id);
        }
        xSemaphoreGive(_rulesMutex);
        _bootReportListBuilt = true;
        LOGGER.infof("[PeriphExec] Boot report scheduled for %d rule(s)", (int)_bootReportQueue.size());
    }

    // 队列清空 → 完成
    if (_bootReportQueue.empty()) {
        _bootReportDone = true;
        _bootReportPending = false;
        LOGGER.info("[PeriphExec] Boot report completed");
        return;
    }

    // 每周期只处理一条规则（节流：避免一次性占用主循环与堆，也避免一次性洪水式上报）
    String ruleId = _bootReportQueue.back();
    _bootReportQueue.pop_back();

    PeriphExecRule ruleCopy;
    bool found = false;
    if (xSemaphoreTake(_rulesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = rules.find(ruleId);
        if (it != rules.end() && it->second.enabled && it->second.reportAfterExec) {
            ruleCopy = it->second;  // 锁内拷贝，锁外采集，避免长时间持锁
            found = true;
        }
        xSemaphoreGive(_rulesMutex);
    } else {
        _bootReportQueue.push_back(ruleId);  // 拿不到锁：放回队列下周期重试
        return;
    }

    if (found) {
        _executor->reportRuleCurrentState(ruleCopy);
    }
}

bool PeriphExecManager::tryReportDeviceData() {
    if (_scheduler) return _scheduler->tryReportDeviceData();
    return false;
}

String PeriphExecManager::getValidTriggerTypes() {
    return PeriphExecScheduler::getValidTriggerTypes();
}

String PeriphExecManager::getEventCategoriesJson() {
    return PeriphExecScheduler::getEventCategoriesJson();
}

String PeriphExecManager::getValidActionTypes(const String& periphId) {
    // 此静态方法需要访问 PeripheralManager，保留在 Manager 中
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    PeripheralManager& pm = PeripheralManager::getInstance();
    PeripheralConfig configSnapshot;
    const PeripheralConfig* config = nullptr;

    if (!periphId.isEmpty()) {
        if (pm.getPeripheralSnapshot(periphId, configSnapshot)) config = &configSnapshot;
    }

    auto addAction = [&arr](uint8_t type, const char* name, const char* category = "") {
        JsonObject obj = arr.add<JsonObject>();
        obj["type"] = type;
        obj["name"] = name;
        if (strlen(category) > 0) {
            obj["category"] = category;
        }
    };

    if (config) {
        PeripheralType pType = config->type;

        if (pType == PeripheralType::STEPPER_MOTOR) {
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "步进电机控制(forward/reverse/stop/faster/slower/setSpeed)", "步进电机");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
        } else if (pType == PeripheralType::NEO_PIXEL) {
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "WS2812B控制(color/off/rainbow/brightness)", "灯珠");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
        } else if (pType == PeripheralType::RF_MODULE) {
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "射频发送(send code/bits/pulseWidth/repeat)", "射频");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
        } else if (pType == PeripheralType::RADAR_SENSOR) {
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ), "雷达读取(radar presence)", "雷达");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "调用其他外设", "外设");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
        } else if (pType == PeripheralType::PCA9685_PWM) {
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "PCA9685 已初始化会话(duty/pulse/angle/enable/disable/stop)", "PWM");
        } else if (pType == PeripheralType::SDIO) {
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SD_STATUS), "SD 状态快照（只读）", "存储");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SD_RECORD), "SD 持续记录（执行只读状态）", "存储");
        } else if (pType == PeripheralType::DS1302_RTC) {
#if FASTBEE_ENABLE_DS1302
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_RTC_CONTROL), "RTC 计时启停", "RTC");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_RTC_READ), "RTC 日历读取（无时区）", "RTC");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_RTC_SET), "RTC 复制系统本地时间", "RTC");
#endif
        } else if (pType == PeripheralType::UART) {
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "串口发送(send/text)", "串口");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
        } else if (isInputType(pType)) {
            // 输入类型外设：不支持GPIO输出动作，只支持系统功能和脚本
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_RESTART), "系统重启", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_NTP_SYNC), "NTP时间同步", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_ENABLE), "启用功能(webServer/mdns)", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_DISABLE), "禁用功能(webServer/mdns)", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "调用其他外设", "外设");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
        } else if (isOutputType(pType)) {
            if (FastBee::PeripheralActionContract::matchesPeripheralTarget(
                    static_cast<uint8_t>(ExecActionType::ACTION_THERMOSTAT), *config))
                addAction(static_cast<uint8_t>(ExecActionType::ACTION_THERMOSTAT), "温控继电器（低电平吸合）", "持续控制");
            // 输出类型外设：支持所有GPIO输出动作
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_HIGH), "设置高电平", "GPIO");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_LOW), "设置低电平", "GPIO");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_HIGH_INVERTED), "设置高电平(反转)", "GPIO");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_LOW_INVERTED), "设置低电平(反转)", "GPIO");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_GPIO_TOGGLE), "电平翻转", "GPIO");

            // PWM输出
            if (config->isExpanderChannel()) {
                addAction(static_cast<uint8_t>(ExecActionType::ACTION_BLINK), "闪烁(至少200ms)", "GPIO");
            }
            if (pType == PeripheralType::GPIO_PWM_OUTPUT) {
                addAction(static_cast<uint8_t>(ExecActionType::ACTION_SET_PWM), "设置PWM占空比", "PWM");
                addAction(static_cast<uint8_t>(ExecActionType::ACTION_BLINK), "闪烁", "PWM");
                addAction(static_cast<uint8_t>(ExecActionType::ACTION_BREATHE), "呼吸灯", "PWM");
            }

            // DAC输出
            if (pType == PeripheralType::DAC || pType == PeripheralType::GPIO_ANALOG_OUTPUT) {
                addAction(static_cast<uint8_t>(ExecActionType::ACTION_SET_DAC), "设置DAC值", "DAC");
            }

            // 系统功能和脚本
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_RESTART), "系统重启", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_NTP_SYNC), "NTP时间同步", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_ENABLE), "启用功能(webServer/mdns)", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_DISABLE), "禁用功能(webServer/mdns)", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "调用其他外设", "外设");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
        } else {
            // 其他类型外设：只支持系统功能和脚本
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_RESTART), "系统重启", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_NTP_SYNC), "NTP时间同步", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_ENABLE), "启用功能(webServer/mdns)", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_DISABLE), "禁用功能(webServer/mdns)", "系统");
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "调用其他外设", "外设");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
            addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
        }
    } else {
        // 未指定外设，返回所有动作类型
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SD_RECORD), "SD 持续记录（执行只读状态）", "存储");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_THERMOSTAT), "温控继电器（低电平吸合）", "持续控制");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_HIGH), "设置高电平", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_LOW), "设置低电平", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_BLINK), "闪烁", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_BREATHE), "呼吸灯", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SET_PWM), "设置PWM占空比", "PWM");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SET_DAC), "设置DAC值", "DAC");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_RESTART), "系统重启", "系统");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_NTP_SYNC), "NTP时间同步", "系统");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_ENABLE), "启用功能(webServer/mdns)", "系统");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_FEATURE_DISABLE), "禁用功能(webServer/mdns)", "系统");
#if FASTBEE_ENABLE_OTA
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SYS_OTA), "OTA升级", "系统");
#endif
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL), "调用其他外设", "外设");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_HIGH_INVERTED), "设置高电平(反转)", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_LOW_INVERTED), "设置低电平(反转)", "GPIO");
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_GPIO_TOGGLE), "电平翻转", "GPIO");
#if FASTBEE_ENABLE_COMMAND_SCRIPT
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_SCRIPT), "脚本命令", "脚本");
#endif
#if FASTBEE_ENABLE_REMOTE_RESOURCES
        addAction(static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_READ), "更新心知天气", "天气");
#endif
    }

    String result;
    serializeJson(doc, result);
    return result;
}

// ========== 异步执行结果管理 ==========

bool PeriphExecManager::ensureResultHistoryCapacity() {
    // The caller holds _resultsMutex. Existing records must remain updatable
    // under pressure; only first allocation needs this additional admission.
    if (executionResults.hasCapacity()) return true;
    constexpr size_t storageBytes =
        sizeof(AsyncExecResult) * ASYNC_EXEC_RESULT_HISTORY_LIMIT;
    // dramFree() includes internal regions which cannot back a byte-addressed
    // array on classic ESP32. Reserve the existing 12KB admission headroom in
    // the real 8-bit pool, conservatively assuming allocation uses DRAM even
    // when the platform allocator may select PSRAM.
    if (FastBee::MemoryBudget::dram8Free() < 12000U + storageBytes ||
        FastBee::MemoryBudget::dram8Largest() < storageBytes) return false;
    if (!executionResults.ensureCapacity()) return false;
    // Include allocator overhead and concurrent consumption in the check.
    // Roll back the empty array rather than admitting an untrackable operation.
    if (FastBee::MemoryBudget::dram8Free() < 12000U) {
        executionResults.releaseIfEmpty();
        return false;
    }
    return true;
}

bool PeriphExecManager::recordResult(const AsyncExecResult& result) {
    MutexGuard lock(_resultsMutex);
    if (!lock.isLocked() || result.operationId == 0) return false;

    for (auto& existing : executionResults) {
        if (existing.operationId == result.operationId) {
            // The record already owns its String capacity. All remaining trace
            // fields are fixed-size, so a terminal transition remains possible
            // even after DRAM falls below the admission threshold.
            existing = result;
            return true;
        }
    }

    // Only new operations allocate rule-id/name storage. Refuse their admission
    // under pressure instead of returning an operationId that can never appear.
    if (FastBee::MemoryBudget::dram8Free() < 12000) return false;
    if (!ensureResultHistoryCapacity()) return false;

    while (executionResults.size() >= ASYNC_EXEC_RESULT_HISTORY_LIMIT) {
        auto evict = executionResults.end();
        for (auto it = executionResults.begin(); it != executionResults.end(); ++it) {
            if (isTerminalExecStatus(it->status)) {
                evict = it;
                break;
            }
        }
        // At most MAX_ASYNC_TASKS operations can be active, so this means the
        // lifecycle invariant was violated. Do not hide an active operation.
        if (evict == executionResults.end()) return false;
        executionResults.erase(evict);
    }

    executionResults.push_back(result);
    return true;
}

bool PeriphExecManager::beginOperation(const PeriphExecRule& rule,
                                       bool asyncExecution,
                                       uint8_t queuePosition,
                                       AsyncExecResult& operation,
                                       const FastBee::UartEventOrigin& uartOrigin,
                                       const FastBee::OrdinaryControlContext& control) {
    operation = AsyncExecResult();
    operation.ordinaryControl = control;
    if (!PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) {
        operation.errorCode = AsyncExecError::UART_SOURCE_UNCONFIRMED;
        return false;
    }
    operation.uartOrigin = uartOrigin;
    operation.peripheralConfigEpoch = uartOrigin.required
        ? uartOrigin.configEpoch : FastBee::SensorConfigEpoch::current();
    {
        MutexGuard lock(_resultsMutex);
        if (!lock.isLocked()) return false;
        // The retained history array does not cover new rule-id/name Strings.
        // Non-byte-addressable internal RAM cannot satisfy their headroom.
        if (FastBee::MemoryBudget::dram8Free() < 12000) return false;
        if (!ensureResultHistoryCapacity()) return false;

        while (executionResults.size() >= ASYNC_EXEC_RESULT_HISTORY_LIMIT) {
            auto evict = executionResults.end();
            for (auto it = executionResults.begin(); it != executionResults.end(); ++it) {
                if (isTerminalExecStatus(it->status)) {
                    evict = it;
                    break;
                }
            }
            if (evict == executionResults.end()) return false;
            executionResults.erase(evict);
        }

        do {
            ++_nextOperationId;
        } while (_nextOperationId == 0);

        const unsigned long now = millis();
        operation.operationId = _nextOperationId;
        operation.ruleId = rule.id;
        operation.ruleName = rule.name;
        operation.status = AsyncExecStatus::PENDING;
        operation.queuedAt = now;
        operation.deadlineAt = now + ASYNC_EXEC_OPERATION_TIMEOUT_MS;
        operation.queuePosition = queuePosition;
        operation.asyncExecution = asyncExecution;
        operation.actionCount = static_cast<uint8_t>(
            rule.actions.size() > 255 ? 255 : rule.actions.size());
        executionResults.push_back(operation);
    }
    return true;
}

std::vector<AsyncExecResult> PeriphExecManager::getRecentResults() {
    MutexGuard lock(_resultsMutex);
    if (!lock.isLocked()) return {};
    if (executionResults.size() == 0) return {};
    return {executionResults.begin(), executionResults.end()};
}

// ========== 手动执行 ==========

bool PeriphExecManager::runOnce(const String& id, const String& receivedValue,
                                String* rejectionReason,
                                AsyncExecResult* admittedOperation,
                                const FastBee::OrdinaryControlContext* control) {
    // A direct-target review cannot authorize all outputs in a multi-action rule.
    if (control && control->request.overrideConfirmed) {
        if (rejectionReason) *rejectionReason = "control_rule_override_not_supported";
        return false;
    }
    PeriphExecRule ruleCopy;
    if (!getRuleSnapshot(id, ruleCopy)) {
        LOGGER.warning("[PeriphExec] Rule not found for runOnce: " + id);
        if (rejectionReason) *rejectionReason = "rule_not_found";
        return false;
    }

    // 创建副本并异步分发，避免阻塞 Web 请求线程
    String resolvedValue = receivedValue;
    if (resolvedValue.isEmpty()) {
        for (const auto& trigger : ruleCopy.triggers) {
            if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::PLATFORM_TRIGGER)) continue;
            if (!trigger.compareValue.isEmpty()) {
                resolvedValue = trigger.compareValue;
                break;
            }
        }
    }
    LOGGER.infof("[PeriphExec] runOnce async dispatch: '%s' (id=%s, value='%s')",
                 ruleCopy.name.c_str(), id.c_str(), resolvedValue.c_str());
    return dispatchAsync(ruleCopy, resolvedValue, rejectionReason, admittedOperation, {}, control);
}

// ========== 异步调度 ==========

bool PeriphExecManager::shouldRunAsync() const {
    if (_workersSuspendedForMemory || _workerSuspensionPending ||
        !_workerPool || !_workerPool->isStarted()) {
        return false;
    }
    // 检查可用堆内存
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_ASYNC) {
        return false;
    }
    // 检查是否有空闲任务槽（非阻塞 peek）
    if (uxSemaphoreGetCount(_taskSlotSemaphore) == 0) {
        return false;
    }
    return true;
}

bool PeriphExecManager::dispatchAsync(const PeriphExecRule& rule,
                                      const String& receivedValue,
                                      String* rejectionReason,
                                      AsyncExecResult* admittedOperation,
                                      const FastBee::UartEventOrigin& uartOrigin,
                                      const FastBee::OrdinaryControlContext* control,
                                      FastBee::OrdinaryControl::Source automaticSource) {
    FastBee::OrdinaryControlContext capturedControl;
    if (control) capturedControl = *control;
    else {
        // Invalid context rejects outputs without blocking read-only actions.
        PeripheralManager::getInstance().beginOrdinaryControl(
            automaticSource, rule.outputActor, 0, false, 0, String(),
            FastBee::SensorConfigEpoch::current(), uartOrigin, capturedControl);
    }
    capturedControl.ruleBound = true;
    capturedControl.ruleEpoch = FastBee::RuleOutputIdentity::currentEpoch();
    if (rejectionReason) *rejectionReason = "";
    if (admittedOperation) *admittedOperation = AsyncExecResult();
    if (!isRuleExecutionCurrent(rule)) {
        if (rejectionReason) *rejectionReason = "rule_revision_unconfirmed";
        return false;
    }
    if (!PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) {
        if (rejectionReason) *rejectionReason = "uart_source_unconfirmed";
        return false;
    }
    for (const auto& trigger : rule.triggers) if (hasRetiredRemoteSource(trigger)) {
        if (rejectionReason) *rejectionReason = "remote_source_retired"; return false;
    }
    for (const auto& action : rule.actions) {
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_CHANNEL)) {
            if (rejectionReason) *rejectionReason = "remote_channel_retired";
            return false;
        }
    }
    // Defense in depth for legacy/imported files which predate the save-time
    // validator. Destructive reset must never be reachable through automation.
    for (const auto& action : rule.actions) {
        if (action.actionType == static_cast<uint8_t>(
                                     ExecActionType::ACTION_SYS_FACTORY_RESET)) {
            LOGGER.warningf("[PeriphExec] Reject rule '%s': factory reset is not automatable",
                            rule.name.c_str());
            if (rejectionReason) *rejectionReason = "action_not_automatable";
            return false;
        }
    }
    for (size_t index = 0; index < rule.actions.size(); ++index) {
        if (isTerminalSystemAction(rule.actions[index].actionType) &&
            index + 1U != rule.actions.size()) {
            if (rejectionReason) *rejectionReason = "action_order_invalid";
            return false;
        }
    }

    const bool requiresWorker = ruleRequiresWorkerPool(rule);
    // MemGuard SEVERE/CRITICAL pauses timer polling, but event sources (button,
    // MQTT and platform commands) can still arrive.  Do not let a heavy rule
    // fall back to synchronous execution during that window: script, sensor
    // and Modbus paths can hold the Web/network task for seconds and create a
    // second memory spike.  Lightweight GPIO/system actions remain available.
    {
        FastBeeFramework* fw = FastBeeFramework::getInstance();
        HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
        if (monitor && monitor->isMemorySevere() && requiresWorker) {
            LOGGER.warningf("[PeriphExec] Skip heavy rule during memory pressure: '%s'",
                            rule.name.c_str());
            MutexGuard runLock(_runningRulesMutex);
            _failureBackoff[rule.id] = millis() + 5000UL;
            if (rejectionReason) *rejectionReason = "memory_pressure";
            return false;
        }
    }

    // A dormant no-PSRAM pool is created only for genuine worker demand.
    // Reject before recording an operation or reserving a running-rule entry.
    if (!ensureWorkerPoolForRule(rule)) {
        if (rejectionReason) *rejectionReason = "worker_memory_pending";
        return false;
    }

    // 原子检查并预留运行标记。系统动作与普通动作走同一准入路径，
    // 避免两个并发来源同时通过“先检查、后插入”的窗口。
    {
        MutexGuard runLock(_runningRulesMutex);
        if (!runLock.isLocked()) {
            if (rejectionReason) *rejectionReason = "rule_state_busy";
            return false;
        }
        if (_runningRuleIds.count(rule.id) > 0) {
            auto itTime = _runningStartTime.find(rule.id);
            unsigned long now = millis();
            bool startTimeMissing = (itTime == _runningStartTime.end());
            unsigned long elapsed = startTimeMissing ? 0 : (now - itTime->second);
            // 慢 Modbus 从站的合法批次可能超过 60 秒。仅凭墙钟时间删除
            // running 标记会让同一规则重入并并发操作同一硬件。worker 的
            // 正常/取消出口负责唯一清理；这里始终拒绝重复分发并保留诊断。
            LOGGER.warningf("[PeriphExec] Skip dispatch: task '%s' already running (%s, elapsed=%lums)",
                            rule.name.c_str(),
                            startTimeMissing ? "start-time-missing" : "active",
                            elapsed);
            if (rejectionReason) *rejectionReason = "already_running";
            return false;
        }
        _runningRuleIds.insert(rule.id);
        _runningStartTime[rule.id] = millis();
    }
    auto releaseReservation = [&](bool addBackoff) {
        MutexGuard runLock(_runningRulesMutex);
        if (!runLock.isLocked()) return;
        _runningRuleIds.erase(rule.id);
        _runningStartTime.erase(rule.id);
        if (addBackoff && FastBee::MemoryBudget::dramFree() > 10000) {
            _failureBackoff[rule.id] = millis() + 5000UL;
        }
    };
    auto runTrackedSync = [&]() -> bool {
        AsyncExecResult operation;
        if (!beginOperation(rule, false, 0, operation, uartOrigin, capturedControl)) {
            releaseReservation(true);
            if (rejectionReason) *rejectionReason = operation.errorCode == AsyncExecError::UART_SOURCE_UNCONFIRMED
                ? "uart_source_unconfirmed" : "tracking_unavailable";
            return false;
        }
        // Admission and execution outcome are separate contracts. Once the
        // operation record exists, the request is accepted even if an action
        // later reaches a failed terminal state.
        executeSyncWithCompletion(rule, receivedValue, operation);
        if (admittedOperation) *admittedOperation = operation;
        return true;
    };

    // 异步模式：判断资源是否充足
    if (!shouldRunAsync()) {
        if (requiresWorker) {
            LOGGER.warningf("[PeriphExec] Skip heavy rule sync fallback (heap=%d, slots=%d): '%s'",
                            (int)ESP.getFreeHeap(),
                            (int)uxSemaphoreGetCount(_taskSlotSemaphore),
                            rule.name.c_str());
            releaseReservation(true);
            if (rejectionReason) *rejectionReason = "async_unavailable";
            return false;
        }
        LOGGER.infof("[PeriphExec] Fallback sync (heap=%d, slots=%d): '%s'",
                     (int)ESP.getFreeHeap(),
                     (int)uxSemaphoreGetCount(_taskSlotSemaphore),
                     rule.name.c_str());
        return runTrackedSync();
    }

    // 获取一个任务槽（非阻塞）
    if (xSemaphoreTake(_taskSlotSemaphore, 0) != pdTRUE) {
        if (requiresWorker) {
            LOGGER.warningf("[PeriphExec] Skip heavy rule without async slot: '%s'", rule.name.c_str());
            releaseReservation(true);
            if (rejectionReason) *rejectionReason = "no_task_slot";
            return false;
        }
        LOGGER.warningf("[PeriphExec] No async slot, fallback sync: '%s'", rule.name.c_str());
        return runTrackedSync();
    }

    // 从对象池获取异步上下文（避免频繁堆分配）
    AsyncExecContext* ctx = _contextPool.acquire();
    if (!ctx) {
        if (requiresWorker) {
            LOGGER.warningf("[PeriphExec] Context pool exhausted, skip heavy rule: '%s'", rule.name.c_str());
            xSemaphoreGive(_taskSlotSemaphore);
            releaseReservation(true);
            if (rejectionReason) *rejectionReason = "context_pool_exhausted";
            return false;
        }
        LOGGER.warning("[PeriphExec] Context pool exhausted, fallback to sync execution");
        xSemaphoreGive(_taskSlotSemaphore);
        return runTrackedSync();
    }

    ctx->ruleCopy = rule;
    ctx->receivedValue = receivedValue;
    ctx->manager = this;
    ctx->mqtt = nullptr;  // Will be set by executor if needed
    ctx->taskSlot = _taskSlotSemaphore;

    const size_t pendingBeforeEnqueue = _workerPool
        ? _workerPool->pendingCount() : 0;
    const uint8_t queuePosition = static_cast<uint8_t>(
        pendingBeforeEnqueue >= 254 ? 255 : pendingBeforeEnqueue + 1);
    AsyncExecResult operation;
    if (!beginOperation(rule, true, queuePosition, operation, uartOrigin, capturedControl)) {
        _contextPool.release(ctx);
        xSemaphoreGive(_taskSlotSemaphore);
        releaseReservation(true);
        if (rejectionReason) *rejectionReason = operation.errorCode == AsyncExecError::UART_SOURCE_UNCONFIRMED
            ? "uart_source_unconfirmed" : "tracking_unavailable";
        return false;
    }
    ctx->operationId = operation.operationId;
    ctx->peripheralConfigEpoch = operation.peripheralConfigEpoch;
    ctx->uartOrigin = operation.uartOrigin;
    ctx->ordinaryControl = operation.ordinaryControl;
    ctx->queuedAt = operation.queuedAt;
    ctx->deadlineAt = operation.deadlineAt;
    ctx->queuePosition = operation.queuePosition;

    // D 方案：投递到常驻 worker pool，避免运行期 xTaskCreate 碎片
    // 常驻任务复用有界队列；S3 第二 worker 仅在实际积压且预算充足时创建
    bool dispatched = false;
    if (_workerPool && _workerPool->isStarted()) {
        dispatched = _workerPool->enqueue(ctx);
    }

    if (!dispatched) {
        LOGGER.errorf("[PeriphExec] Failed to dispatch to worker pool: '%s' (pool=%s)",
                      rule.name.c_str(),
                      _workerPool ? "queue-full" : "unavailable");
        _contextPool.release(ctx);  // 归还对象池
        xSemaphoreGive(_taskSlotSemaphore);
        // 重资源规则（脚本/Modbus/多动作）绝对不能同步 fallback：
        // 9s+ 同步执行会阻塞 timer 任务，饯死 web_client_handler / network_update 等 100ms 调度，
        // 导致 Web 服务少响应。此处与前面三个失败分支保持一致：heavy rule 丢入 backoff 跳过本轮。
        if (requiresWorker) {
            operation.status = AsyncExecStatus::REJECTED;
            operation.errorCode = AsyncExecError::WORKER_QUEUE_FULL;
            operation.endTime = millis();
            recordResult(operation);
            if (admittedOperation) *admittedOperation = operation;
            releaseReservation(true);
            if (rejectionReason) *rejectionReason = "worker_queue_full";
            return false;
        }
        operation.asyncExecution = false;
        operation.queuePosition = 0;
        recordResult(operation);
        executeSyncWithCompletion(rule, receivedValue, operation);
        if (admittedOperation) *admittedOperation = operation;
        return true;
    }

    if (admittedOperation) *admittedOperation = operation;

    LOG_DEBUGF("[PeriphExec] Async dispatched: '%s' (heap=%d, queue=%u/%u)",
                 rule.name.c_str(),
                 (int)ESP.getFreeHeap(),
                 (unsigned)(_workerPool ? _workerPool->pendingCount() : 0),
                 (unsigned)PeriphExecWorkerPool::capacity());
    return true;
}

// 单任务执行（由 PeriphExecWorkerPool 在 worker 线程内调用）
// 等价于原 asyncExecTaskFunc 的业务体，但不再调用 vTaskDelete
void PeriphExecManager::executeWorkerJob(AsyncExecContext* ctx) {
    AsyncExecResult result;
    result.operationId = ctx->operationId;
    result.peripheralConfigEpoch = ctx->peripheralConfigEpoch;
    result.uartOrigin = ctx->uartOrigin;
    result.ordinaryControl = ctx->ordinaryControl;
    result.ruleId = ctx->ruleCopy.id;
    result.ruleName = ctx->ruleCopy.name;
    result.queuedAt = ctx->queuedAt;
    result.startTime = millis();
    result.deadlineAt = ctx->deadlineAt;
    result.queuePosition = ctx->queuePosition;
    result.asyncExecution = true;
    result.actionCount = static_cast<uint8_t>(
        ctx->ruleCopy.actions.size() > 255 ? 255 : ctx->ruleCopy.actions.size());
    result.status = AsyncExecStatus::RUNNING;
    ctx->manager->recordResult(result);

    LOG_DEBUGF("[PeriphExec] Async task started: '%s'", result.ruleName.c_str());

    // 堆守卫：任务开始时检查堆内存，过低时直接放弃执行防止 abort()
    // 2026-08-05 fix: INTERNAL 口径含 IRAM/RTC 虚高 ~28KB，实测崩溃现场
    // INTERNAL 还有 29KB 时 8BIT DRAM 仅剩 <1KB → JSON 解析小分配直接
    // new OOM abort。双口径门控，8BIT 才是真实可分配水位。
    // 阈值校准：MQTT 连接稳态 8BIT 常态 ~7KB，门限取 5KB 保证用户规则
    // 在常态下可执行；窗口内挤出交给 per-action 守卫 + MQTT 强制断开。
    bool allOk = false;
    bool completedAll = false;
    size_t reportCount = 0;
    uint32_t const dram8Free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (result.deadlineAt != 0 &&
        static_cast<int32_t>(result.startTime - result.deadlineAt) >= 0) {
        LOGGER.warningf("[PeriphExec] Queued operation expired before start: '%s'",
                        result.ruleName.c_str());
        result.status = AsyncExecStatus::TIMED_OUT;
        result.errorCode = AsyncExecError::DEADLINE_EXCEEDED;
    } else if (FastBee::MemoryBudget::dramFree() < 16384 || dram8Free < 5120) {
        LOGGER.warningf("[PeriphExec] Heap too low (dram=%d dram8=%u), skip execution: '%s'",
                        (int)ESP.getFreeHeap(), (unsigned)dram8Free, result.ruleName.c_str());
        result.status = AsyncExecStatus::FAILED;
        result.errorCode = AsyncExecError::LOW_MEMORY;
    } else {
        // 执行所有动作（reportCount 回填本次实际上报数量）
        auto actionResults = ctx->manager->executeAllActions(
            ctx->ruleCopy, ctx->receivedValue, false, &reportCount, &completedAll,
            result.deadlineAt, &result);

        // 统计结果
        allOk = completedAll && !ctx->ruleCopy.actions.empty();
        for (const auto& ar : actionResults) {
            if (!ar.success) { allOk = false; break; }
        }
        if (result.errorCode == AsyncExecError::DEADLINE_EXCEEDED) {
            result.status = AsyncExecStatus::TIMED_OUT;
        } else {
            result.status = allOk ? AsyncExecStatus::COMPLETED : AsyncExecStatus::FAILED;
            if (!allOk && result.errorCode == AsyncExecError::NONE) {
                result.errorCode = completedAll
                    ? AsyncExecError::ACTION_FAILED
                    : AsyncExecError::INCOMPLETE_ACTIONS;
            }
        }
    }

    result.endTime = millis();
    result.reportCount = static_cast<uint8_t>(reportCount > 255 ? 255 : reportCount);

    LOGGER.infof("[PeriphExec] Async task finished: '%s' %s (%lums)",
                 result.ruleName.c_str(),
                 allOk ? "OK" : "FAILED",
                 result.endTime - result.startTime);

    // 记录执行结果
    ctx->manager->recordResult(result);

    // 从运行中集合移除规则ID
    {
        MutexGuard runLock(ctx->manager->_runningRulesMutex);
        ctx->manager->_runningRuleIds.erase(ctx->ruleCopy.id);
        ctx->manager->_runningStartTime.erase(ctx->ruleCopy.id);
    }

    // 如果执行失败，设置退避时间（30秒后才能再次触发）
    // 堆守卫：低堆时跳过 map 插入，防止 abort()
    if (FastBee::MemoryBudget::dramFree() > 10000 && ctx->manager->isRuleExecutionCurrent(ctx->ruleCopy)) {
        if (!allOk) {
            MutexGuard runLock(ctx->manager->_runningRulesMutex);
            ctx->manager->_failureBackoff[ctx->ruleCopy.id] = millis() + 30000;
            LOGGER.infof("[PeriphExec] Rule '%s' failed, backoff for 30s", result.ruleName.c_str());
        } else {
            // 成功时清除退避记录
            MutexGuard runLock(ctx->manager->_runningRulesMutex);
            ctx->manager->_failureBackoff.erase(ctx->ruleCopy.id);
        }
    }

    // 触发外设执行完成事件（低堆时跳过，避免内部 STL 分配导致 abort）
    if (FastBee::MemoryBudget::dramFree() > 10000 && ctx->manager->isRuleExecutionCurrent(ctx->ruleCopy)) {
        ctx->manager->dispatchPeriphExecEvent(ctx->ruleCopy.id, allOk ? "success" : "failed", ctx->uartOrigin);
    }

    // 释放任务槽
    xSemaphoreGive(ctx->taskSlot);

    // 栈高水位标记（worker 任务自身，跨多个 job 的历史最低可用栈）
    UBaseType_t hwmBytes = uxTaskGetStackHighWaterMark(nullptr);
    LOGGER.infof("[PExec] Worker after '%s' stack HWM: %u bytes free",
                 result.ruleName.c_str(), (unsigned int)hwmBytes);

    // 归还上下文到对象池
    ctx->manager->_contextPool.release(ctx);
    // 注意：D 方案下 worker 是常驻任务，不再 vTaskDelete；
    // 控制权返回 PeriphExecWorkerPool::workerLoop 继续接下一个 job
}

// 执行规则的所有动作（供异步任务调用）
std::vector<ActionExecResult> PeriphExecManager::executeAllActions(
    const PeriphExecRule& rule, const String& receivedValue, bool suppressReport,
    size_t* reportCountOut, bool* completedAllOut, unsigned long deadlineAt,
    AsyncExecResult* traceOut) {
    if (_executor) {
        return _executor->executeAllActions(
            rule, receivedValue, suppressReport, reportCountOut, completedAllOut,
            deadlineAt, traceOut);
    }
    if (completedAllOut) *completedAllOut = false;
    return {};
}

// 同步执行并触发完成事件（用于同步降级路径，确保链式执行正常工作）
bool PeriphExecManager::executeSyncWithCompletion(const PeriphExecRule& rule,
                                                  const String& receivedValue,
                                                  AsyncExecResult& operation) {
    // dispatchAsync 已原子预留运行标记；同步降级只负责执行和统一收尾。
    operation.status = AsyncExecStatus::RUNNING;
    operation.startTime = millis();
    operation.queuePosition = 0;
    operation.asyncExecution = false;
    recordResult(operation);
    size_t reportCount = 0;
    bool completedAll = false;
    auto actionResults = executeAllActions(
        rule, receivedValue, false, &reportCount, &completedAll,
        operation.deadlineAt, &operation);

    // 3. 统计结果
    bool allOk = completedAll && !rule.actions.empty();
    for (const auto& ar : actionResults) {
        if (!ar.success) { allOk = false; break; }
    }

    LOGGER.infof("[PeriphExec] Sync+completion: '%s' %s", rule.name.c_str(), allOk ? "OK" : "FAILED");

    // 4. 从运行中集合移除
    {
        MutexGuard runLock(_runningRulesMutex);
        _runningRuleIds.erase(rule.id);
        _runningStartTime.erase(rule.id);
    }

    // 5. 设置或清除退避记录
    if (FastBee::MemoryBudget::dramFree() > 10000 && isRuleExecutionCurrent(rule)) {
        if (!allOk) {
            MutexGuard runLock(_runningRulesMutex);
            _failureBackoff[rule.id] = millis() + 30000;
        } else {
            MutexGuard runLock(_runningRulesMutex);
            _failureBackoff.erase(rule.id);
        }
    }

    // 6. 触发外设执行完成事件（链式执行关键）
    if (FastBee::MemoryBudget::dramFree() > 10000 && isRuleExecutionCurrent(rule)) {
        dispatchPeriphExecEvent(rule.id, allOk ? "success" : "failed", operation.uartOrigin);
    }

    // 7. 记录执行结果
    operation.endTime = millis();
    if (operation.errorCode == AsyncExecError::DEADLINE_EXCEEDED) {
        operation.status = AsyncExecStatus::TIMED_OUT;
    } else {
        operation.status = allOk ? AsyncExecStatus::COMPLETED : AsyncExecStatus::FAILED;
        if (!allOk && operation.errorCode == AsyncExecError::NONE) {
            operation.errorCode = completedAll
                ? AsyncExecError::ACTION_FAILED
                : AsyncExecError::INCOMPLETE_ACTIONS;
        }
    }
    operation.reportCount = static_cast<uint8_t>(reportCount > 255 ? 255 : reportCount);
    recordResult(operation);
    return allOk;
}

// ========== 内部调度接口实现 ==========

void PeriphExecManager::checkTimerTriggers(unsigned long now, bool modbusAvailable) {
    FastBee::DailyTriggerState::TickGuard tick(_timerDispatchBusy);
    if (!tick.held()) return;
    // MemGuard CRITICAL 安全底线：Scheduler 已做动态降频，此处仅保留 CRITICAL 防护
    // 防止被其他路径直接调用时跳过内存保护
    {
        auto* fw = FastBeeFramework::getInstance();
        HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
        if (monitor && monitor->isMemoryCritical()) {
            return;  // CRITICAL: 完全跳过轮询
        }
        if (!monitor && FastBee::MemoryBudget::dramFree() < 15000) {
            // 守卫回退：HealthMonitor 不可用时的硬编码阈值
            return;
        }
    }

    // 周期性清理 _failureBackoff 过期 entry：避免长期运行后 map 累积死 entry 导致碎片
    // entry->second 是“下次可执行时间戳”，超过其 5 分钟仍无请求清除时，视为无效
    if (now - _lastBackoffCleanupTime > 60000UL) {
        _lastBackoffCleanupTime = now;
        MutexGuard runLock(_runningRulesMutex, pdMS_TO_TICKS(20));
        if (runLock.isLocked() && !_failureBackoff.empty()) {
            size_t erasedCount = 0;
            for (auto it = _failureBackoff.begin(); it != _failureBackoff.end(); ) {
                if (now > it->second && (now - it->second) > 300000UL) {
                    it = _failureBackoff.erase(it);
                    erasedCount++;
                } else {
                    ++it;
                }
            }
            if (erasedCount > 0) {
                LOGGER.infof("[PeriphExec] Backoff GC: erased %u expired entries, remaining=%u",
                             (unsigned)erasedCount, (unsigned)_failureBackoff.size());
            }
        }
    }

    // 周期性诊断日志：每 5 分钟输出一次运行状态，便于长期追踪退化趋势
    // 关键指标：running 任务数 / backoff entry 数 / context pool 占用 / heap 状况
    if (now - _lastDiagLogTime > 300000UL) {
        _lastDiagLogTime = now;
        size_t runningSize = 0;
        size_t backoffSize = 0;
        {
            MutexGuard runLock(_runningRulesMutex, pdMS_TO_TICKS(20));
            if (runLock.isLocked()) {
                runningSize = _runningRuleIds.size();
                backoffSize = _failureBackoff.size();
            }
        }
        LOGGER.infof("[PeriphExec] Diag: running=%u backoff=%u ctxPool=%u/%u heap=%u largest=%u",
                     (unsigned)runningSize,
                     (unsigned)backoffSize,
                     (unsigned)_contextPool.usedCount(),
                     (unsigned)AsyncExecContextPool::poolSize(),
                     (unsigned)ESP.getFreeHeap(),
                     (unsigned)ESP.getMaxAllocHeap());
    }

    // 第一阶段：收集候选规则（短持锁，避免嵌套锁）
    struct CandidateRule {
        String id;
        String name;
        bool needsModbus;
        size_t triggerIdx;
        uint64_t revision;
        uint32_t calendarDay = 0;
        int8_t calendarHour = -1, calendarMinute = -1;
    };
    std::vector<CandidateRule> candidates;
    candidates.reserve(4);

    {
        MutexGuard lock(_rulesMutex, pdMS_TO_TICKS(100));
        if (!lock.isLocked()) return;  // 获取失败则跳过本轮

        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled) continue;
            if (FastBee::hasThermostatAction(rule)) continue; // Dedicated continuous controller.
            if (FastBee::hasSdRecorderAction(rule)) continue; // Dedicated bounded recording pipeline.

            // 遍历触发器，寻找定时触发或轮询触发类型
            for (size_t ti = 0; ti < rule.triggers.size(); ti++) {
                auto& trigger = rule.triggers[ti];
                uint8_t tt = trigger.triggerType;
                if (tt != static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER) &&
                    tt != static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER)) continue;

                bool shouldTrigger = false;
                uint32_t calendarDay = 0;

                if (trigger.timerMode == 0) {
                    // 间隔模式
                    if (trigger.intervalSec == 0) continue;
                    unsigned long intervalMs = (unsigned long)trigger.intervalSec * 1000UL;
                    if (trigger.lastTriggerTime == 0 || (now - trigger.lastTriggerTime) >= intervalMs) {
                        shouldTrigger = true;
                    }
                } else if (trigger.timerMode == 1) {
                    // 每日时间点模式：使用缓存的解析值，避免锁内重复解析字符串
                    if (!FastBee::DailyTriggerState::isDaily(trigger)) continue;

                    struct tm timeinfo{};
                    if (!getLocalTime(&timeinfo, 0)) continue;
                    calendarDay = FastBee::DailyTriggerState::dateKey(timeinfo);
                    if (!calendarDay || calendarDay <= trigger.lastCalendarDay) continue;

                    if (timeinfo.tm_hour == trigger._cachedHour && timeinfo.tm_min == trigger._cachedMinute) {
                        if (trigger.lastTriggerTime > 0 && (now - trigger.lastTriggerTime) < 60000) continue;
                        shouldTrigger = true;
                    }
                }

                if (shouldTrigger) {
                    CandidateRule c;
                    c.id = rule.id;
                    c.name = rule.name;
                    c.needsModbus = rule._cachedNeedsModbus;
                    c.triggerIdx = ti;
                    c.revision = rule.runtimeRevision;
                    c.calendarDay = calendarDay;
                    c.calendarHour = trigger._cachedHour;
                    c.calendarMinute = trigger._cachedMinute;
                    candidates.push_back(c);
                    break;  // 一个触发器匹配即可触发该规则
                }
            }
        }
    }
    // 锁已释放

    // 第二阶段：从上次检查的下一个规则开始轮转。规则存储是按 ID 排序
    // 的 map；若每轮都从头分发，前几个慢任务会持续占满有限 worker/context
    // 槽，后续规则永远只能收到队列拒绝。
    size_t roundRobinStart = 0;
    if (!_lastTimerDispatchRuleId.isEmpty()) {
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (candidates[i].id.compareTo(_lastTimerDispatchRuleId) > 0) {
                roundRobinStart = i;
                break;
            }
            if (i + 1 == candidates.size()) roundRobinStart = 0;
        }
    }

    // 锁外检查运行状态和退避并直接尝试接纳。只有接纳成功才推进
    // lastTriggerTime/triggerCount；资源拒绝的任务保持 due，下一轮可重试。
    for (size_t offset = 0; offset < candidates.size(); ++offset) {
        const CandidateRule& c =
            candidates[(roundRobinStart + offset) % candidates.size()];

        // 检查 Modbus 可用性
        if (c.needsModbus && !modbusAvailable) {
            LOGGER.warningf("[PeriphExec] Skip timer '%s': Modbus not available", c.name.c_str());
            continue;
        }

        // 检查失败退避和运行状态
        bool skip = false;
        {
            MutexGuard runLock(_runningRulesMutex, pdMS_TO_TICKS(10));
            if (runLock.isLocked()) {
                auto it = _failureBackoff.find(c.id);
                if (it != _failureBackoff.end() && now < it->second) {
                    skip = true;  // 仍在退避期内
                }
                if (!skip && _runningRuleIds.count(c.id) > 0) {
                    LOGGER.warningf("[PeriphExec] Skip timer '%s': task already running", c.name.c_str());
                    skip = true;
                }
            }
        }
        if (skip) continue;

        // A scan is not an admission. Do not replay a minute/date which was
        // skipped by a clock correction while checking resource availability.
        if (c.calendarDay) {
            struct tm current{};
            if (!getLocalTime(&current, 0) ||
                FastBee::DailyTriggerState::dateKey(current) != c.calendarDay ||
                current.tm_hour != c.calendarHour || current.tm_min != c.calendarMinute) continue;
        }
        String rejectionReason;
        const bool accepted = dispatchByRuleId(c.id, "", &rejectionReason, {}, c.revision);
        if (!accepted) {
            LOGGER.warningf("[PeriphExec] Timer admission rejected: '%s' (%s)",
                            c.name.c_str(),
                            rejectionReason.isEmpty() ? "unavailable" : rejectionReason.c_str());
            continue;
        }

        // The cursor represents the last admitted rule, not the last rule we
        // merely inspected. Advancing it on queue/context rejection makes a
        // full scan end at the same highest map key every time, so the next
        // cycle starts from the first rule again and short-period rules can
        // starve later peers.
        _lastTimerDispatchRuleId = c.id;

        // 接纳成功后必须完成触发时间戳提交。运行标记已经预留，因此这里
        // 可以等待规则锁而不会重复分发；静默跳过会让任务完成后仍保持 due。
        {
            MutexGuard lock(_rulesMutex, portMAX_DELAY);
            if (lock.isLocked()) {
                auto it = rules.find(c.id);
                if (it != rules.end() && c.triggerIdx < it->second.triggers.size() &&
                    it->second.runtimeRevision == c.revision) {
                    it->second.triggers[c.triggerIdx].lastTriggerTime = now;
                    it->second.triggers[c.triggerIdx].triggerCount++;
                    FastBee::DailyTriggerState::accept(it->second.triggers,
                        it->second.triggers[c.triggerIdx], c.calendarDay);
                }
            }
        }

        LOGGER.infof("[PeriphExec] Timer triggered: '%s'", c.name.c_str());
    }
}

bool PeriphExecManager::dispatchAndCommitTrigger(const String& ruleId,
                                                 size_t triggerIndex,
                                                 const String& receivedValue,
                                                 const char* sourceLabel,
                                                 const FastBee::UartEventOrigin& uartOrigin, uint64_t expectedRevision) {
    String rejectionReason;
    const auto outputSource = sourceLabel && strcmp(sourceLabel, "mqtt") == 0
        ? FastBee::OrdinaryControl::Source::Mqtt : FastBee::OrdinaryControl::Source::Rule;
    if (!dispatchByRuleId(ruleId, receivedValue, &rejectionReason, uartOrigin, expectedRevision, outputSource)) {
        LOGGER.warningf("[PeriphExec] %s admission rejected for rule '%s' (%s)",
                        sourceLabel ? sourceLabel : "trigger",
                        ruleId.c_str(),
                        rejectionReason.isEmpty() ? "unavailable" : rejectionReason.c_str());
        return false;
    }

    // Admission owns the execution slot before cooldown/counter state advances.
    // A rejected dispatch therefore remains eligible for the next source event.
    MutexGuard lock(_rulesMutex, portMAX_DELAY);
    if (!lock.isLocked()) {
        LOGGER.errorf("[PeriphExec] %s trigger commit lock failed for rule '%s'",
                      sourceLabel ? sourceLabel : "trigger", ruleId.c_str());
        return false;
    }
    auto it = rules.find(ruleId);
    if (it == rules.end() || triggerIndex >= it->second.triggers.size() ||
        (expectedRevision && it->second.runtimeRevision != expectedRevision)) {
        LOGGER.warningf("[PeriphExec] %s trigger disappeared after admission for rule '%s'",
                        sourceLabel ? sourceLabel : "trigger", ruleId.c_str());
        return false;
    }

    ExecTrigger& trigger = it->second.triggers[triggerIndex];
    trigger.lastTriggerTime = millis();
    trigger.triggerCount++;
    return true;
}

void PeriphExecManager::dispatchEventMatchedRules(const String& eventId, const String& eventData,
                                                const FastBee::UartEventOrigin& uartOrigin) {
    // Observe before taking rulesMutex; never hold the PM lock across callbacks.
    if (!PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) return;
    int motorResultIndex = -1;
    if (eventId.startsWith("mc:")) {
        JsonDocument result;
        if (!deserializeJson(result, eventData.c_str(), eventData.length()) && result["origin"] == "motor-result") motorResultIndex = result["d"] | -1;
    }
    struct MatchedRule {
        String ruleId;
        size_t triggerIndex;
        uint64_t revision;
    };
    std::vector<MatchedRule> triggeredRules;

    {
        MutexGuard lock(_rulesMutex);
        if (!lock.isLocked()) return;
        // Legacy/internal state can bypass persisted configuration admission.
        // Validate potential action21 and completion fanout before any event
        // child is queued; cooldown alone does not terminate delayed cycles.
        if (const char* eventIssue = FastBee::validateRuleCompletionGraph(rules)) {
            LOGGER.warningf("[PeriphExec] Event fanout rejected: %s", eventIssue);
            return;
        }

        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled) continue;

            // A motor acknowledgement must not start that motor again through
            // an event rule. Other targets can still react to the result.
            if (motorResultIndex >= 0) {
                bool feedback = false;
                for (const auto& action : rule.actions) {
                    if (action.targetPeriphId == "modbus:" + String(motorResultIndex)) feedback = true;
                    JsonDocument value;
                    if (!deserializeJson(value, action.actionValue.c_str(), action.actionValue.length())) {
                        for (JsonObjectConst control : value["ctrl"].as<JsonArrayConst>()) {
                            if ((control["d"] | -1) == motorResultIndex) feedback = true;
                        }
                    }
                }
                if (feedback) continue;
            }

            for (size_t triggerIndex = 0; triggerIndex < rule.triggers.size(); ++triggerIndex) {
                auto& trigger = rule.triggers[triggerIndex];
                if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER)) continue;
                if (trigger.eventId.isEmpty()) continue;
                if (trigger.eventId != eventId) continue;

                // Modbus 控制事件(mc:): 检查 compareValue 中的通道/动作是否匹配
                if (eventId.startsWith("mc:") && !trigger.compareValue.isEmpty()) {
                    bool ctrlMatched = false;
                    JsonDocument trigDoc;
                    if (!deserializeJson(trigDoc, trigger.compareValue.c_str(), trigger.compareValue.length()) && trigDoc.is<JsonObject>()) {
                        JsonArray trigCtrlArr = trigDoc["ctrl"].as<JsonArray>();
                        if (trigCtrlArr && trigCtrlArr.size() > 0) {
                            JsonObject trigCtrl = trigCtrlArr[0].as<JsonObject>();
                            // 解析事件数据
                            JsonDocument evDoc;
                            if (!deserializeJson(evDoc, eventData.c_str(), eventData.length()) && evDoc.is<JsonObject>()) {
                                JsonObject evObj = evDoc.as<JsonObject>();
                                // 匹配通道和动作
                                // 支持两种事件数据格式：
                                //   标准格式: {"d":0,"c":1,"a":"on"}
                                //   旧格式:   {"d":0,"id":"relay_0_1","v":"1"}
                                uint8_t trigCh = trigCtrl["c"] | 255;  // 255 表示不限制
                                const char* trigAct = trigCtrl["a"] | "";
                                uint8_t evCh = 255;   // 未知状态
                                String evActStr;      // 动作字符串（延用生命用）
                                // 标准格式：直接取 c 和 a 字段
                                if (evObj.containsKey("c")) {
                                    evCh = evObj["c"];
                                }
                                if (evObj.containsKey("a")) {
                                    evActStr = evObj["a"].as<String>();
                                }
                                // 旧格式降级：从 id 字段解析通道号
                                if (evCh == 255 && evObj.containsKey("id")) {
                                    String evId = evObj["id"].as<String>();
                                    int lastUnderscore = evId.lastIndexOf('_');
                                    if (lastUnderscore > 0) {
                                        evCh = evId.substring(lastUnderscore + 1).toInt();
                                    }
                                }
                                // 旧格式降级：从 v 字段推导动作
                                if (evActStr.isEmpty() && evObj.containsKey("v")) {
                                    String evVal = evObj["v"].as<String>();
                                    evActStr = (evVal == "1" || FastBee::stringEqualsIgnoreCaseLiteral(evVal, "on") || FastBee::stringEqualsIgnoreCaseLiteral(evVal, "true")) ? "on" : "off";
                                }
                                if (evCh == 255) evCh = 0;  // 无法解析通道时默认为 0
                                const char* evAct = evActStr.c_str();
                                if ((trigCh == 255 || trigCh == evCh) &&
                                    (strlen(trigAct) == 0 || strcmp(trigAct, evAct) == 0)) {
                                    ctrlMatched = true;
                                }
                            }
                        }
                    } else {
                        // compareValue 无法解析，仅匹配 eventId
                        ctrlMatched = true;
                    }
                    if (!ctrlMatched) continue;
                }

                // 红外事件(ir_code_received): compareValue 非空时按编码或按键名过滤
                // 支持格式：
                //   "key:名称"           —— 按已学习按键名匹配（推荐，可跨规则复用）
                //   "NEC:0x00FF6897"     —— 限定协议 + 编码
                //   "0x00FF6897"/"00FF6897" —— 任意协议 + 编码
                if (eventId == "ir_code_received" && !trigger.compareValue.isEmpty()) {
                    bool codeMatched = false;
                    JsonDocument evDoc;
                    if (!deserializeJson(evDoc, eventData.c_str(), eventData.length()) && evDoc.is<JsonObject>()) {
                        String want = trigger.compareValue;
                        want.trim();
                        int keyColon = want.indexOf(':');
                        String wantPrefix = keyColon > 0 ? want.substring(0, keyColon) : String("");
                        if (FastBee::stringEqualsIgnoreCaseLiteral(wantPrefix, "key")) {
                            // 按键名匹配（大小写不敏感）
                            String wantName = want.substring(keyColon + 1);
                            wantName.trim();
                            String evKey = evDoc["key"] | "";
                            codeMatched = (wantName.length() > 0) && wantName.equalsIgnoreCase(evKey);
                        } else {
                            // 编码匹配（兼容旧规则）
                            String evCode = evDoc["code"] | "";        // 如 "0x00FF6897"
                            String evProto = evDoc["protocol"] | "";   // 如 "NEC"
                            if (keyColon > 0) {
                                if (!wantPrefix.equalsIgnoreCase(evProto)) want = "";  // 协议不符
                                else want = want.substring(keyColon + 1);
                            }
                            if (want.length() > 0) {
                                if (!want.startsWith("0x") && !want.startsWith("0X")) want = "0x" + want;
                                codeMatched = want.equalsIgnoreCase(evCode);
                            }
                        }
                    }
                    if (!codeMatched) continue;
                }

                // RF事件(rf_code_received): compareValue 非空时按编码或按键名过滤
                // 支持格式：
                //   "key:名称"           —— 按已学习按键名匹配
                //   "0xABCDEF"/"ABCDEF"  —— 按编码值匹配
                if (eventId == "rf_code_received" && !trigger.compareValue.isEmpty()) {
                    bool rfMatched = false;
                    JsonDocument rfDoc;
                    if (!deserializeJson(rfDoc, eventData.c_str(), eventData.length()) && rfDoc.is<JsonObject>()) {
                        String want = trigger.compareValue;
                        want.trim();
                        if (want.startsWith("key:") || want.startsWith("KEY:")) {
                            String wantName = want.substring(4);
                            wantName.trim();
                            String evKey = rfDoc["key"] | "";
                            rfMatched = (wantName.length() > 0) && wantName.equalsIgnoreCase(evKey);
                        } else {
                            String evCode = rfDoc["code"] | "";
                            if (!want.startsWith("0x") && !want.startsWith("0X")) want = "0x" + want;
                            rfMatched = want.equalsIgnoreCase(evCode);
                        }
                    }
                    if (!rfMatched) continue;
                }

                // 防重复触发：同一触发器最小间隔 1 秒
                unsigned long now = millis();
                if (trigger.lastTriggerTime > 0 && (now - trigger.lastTriggerTime) < 1000) continue;

                LOGGER.infof("[PeriphExec] Event rule matched: '%s' (event=%s)",
                    rule.name.c_str(), eventId.c_str());

                triggeredRules.push_back({rule.id, triggerIndex, rule.runtimeRevision});
                break;  // 一个触发器匹配即可
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁异步分发
    for (const auto& matched : triggeredRules) {
        dispatchAndCommitTrigger(matched.ruleId, matched.triggerIndex,
                                 eventData, "event", uartOrigin, matched.revision);
    }
}

void PeriphExecManager::dispatchPeriphExecEvent(const String& ruleId, const String& eventData,
                                              const FastBee::UartEventOrigin& uartOrigin) {
    // Completion/cancellation remains recorded even when its source has retired;
    // it cannot spawn an unbound child rule after that retirement.
    if (!PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) return;
    struct MatchedRule {
        String ruleId;
        size_t triggerIndex;
        uint64_t revision;
    };
    std::vector<MatchedRule> triggeredRules;

    {
        MutexGuard lock(_rulesMutex);
        if (!lock.isLocked()) return;
        // Also protect internal callers/legacy state that bypassed CRUD. Only
        // completion fanout is stopped; other event routes keep their guards.
        if (const char* issue = FastBee::validateRuleCompletionGraph(rules)) {
            LOGGER.warningf("[PeriphExec] Completion fanout rejected: %s", issue);
            return;
        }

        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled) continue;

            for (size_t triggerIndex = 0; triggerIndex < rule.triggers.size(); ++triggerIndex) {
                auto& trigger = rule.triggers[triggerIndex];
                if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER)) continue;
                if (trigger.eventId.isEmpty()) continue;
                if (trigger.eventId != ruleId) continue;

                unsigned long now = millis();
                if (trigger.lastTriggerTime > 0 && (now - trigger.lastTriggerTime) < 500) continue;

                LOGGER.infof("[PeriphExec] Periph exec event rule matched: '%s' (triggerRule=%s)",
                    rule.name.c_str(), ruleId.c_str());

                triggeredRules.push_back({rule.id, triggerIndex, rule.runtimeRevision});
                break;
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁异步分发
    for (const auto& matched : triggeredRules) {
        dispatchAndCommitTrigger(matched.ruleId, matched.triggerIndex,
                                 eventData, "periph-event", uartOrigin, matched.revision);
    }
}

void PeriphExecManager::dispatchButtonEventRules(const String& eventId, const String& periphId) {
    struct MatchedRule {
        String ruleId;
        size_t triggerIndex;
        uint64_t revision;
    };
    std::vector<MatchedRule> triggeredRules;

    {
        MutexGuard lock(_rulesMutex, pdMS_TO_TICKS(50));
        if (!lock.isLocked()) {
            LOGGER.debug("[PeriphExec] Button dispatch: mutex busy, will retry next cycle");
            return;  // 下次 checkButtonEvents 会再次触发
        }

        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled) continue;

            for (size_t triggerIndex = 0; triggerIndex < rule.triggers.size(); ++triggerIndex) {
                auto& trigger = rule.triggers[triggerIndex];
                if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER)) continue;
                if (trigger.eventId != eventId) continue;
                // 若规则指定了触发外设，仅匹配该外设产生的按键事件；triggerPeriphId 为空则兼容任意按键
                if (!trigger.triggerPeriphId.isEmpty() && trigger.triggerPeriphId != periphId) continue;

                unsigned long now = millis();
                if (trigger.lastTriggerTime > 0 && (now - trigger.lastTriggerTime) < 100) continue;

                LOGGER.infof("[PeriphExec] Button event rule matched: '%s' (event=%s, periph=%s)",
                    rule.name.c_str(), eventId.c_str(), periphId.c_str());

                triggeredRules.push_back({rule.id, triggerIndex, rule.runtimeRevision});
                break;
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁异步分发
    for (const auto& matched : triggeredRules) {
        dispatchAndCommitTrigger(matched.ruleId, matched.triggerIndex,
                                 "", "button-event", {}, matched.revision);
    }
}

void PeriphExecManager::processMqttMessageMatch(JsonArray& arr) {
    LOGGER.infof("[PeriphExec] Received MQTT message: %d items (non-DataCommand)", arr.size());
    // 阶段1: 持锁匹配，收集规则ID及其匹配的接收值
    struct MatchedRule {
        String ruleId;
        String receivedValue;
        size_t triggerIndex;
        uint64_t revision;
    };
    std::vector<MatchedRule> matchedRules;

    {
        MutexGuard lock(_rulesMutex);
        if (rules.empty()) return;

        for (JsonObject item : arr) {
            if (!item.containsKey("id") || !item.containsKey("value")) continue;

            String itemId = item["id"].as<String>();
            String itemValue = item["value"].as<String>();

            for (auto& pair : rules) {
                PeriphExecRule& rule = pair.second;
                if (!rule.enabled) continue;

                bool alreadyMatched = false;
                for (const auto& pending : matchedRules) {
                    if (pending.ruleId == rule.id) {
                        alreadyMatched = true;
                        break;
                    }
                }
                if (alreadyMatched) continue;

                // 遍历所有触发器（OR 关系）
                bool triggerMatched = false;
                size_t matchedTriggerIndex = 0;
                for (size_t triggerIndex = 0; triggerIndex < rule.triggers.size(); ++triggerIndex) {
                    auto& trigger = rule.triggers[triggerIndex];
                    // 确定要匹配的数据源ID（支持平台触发和事件触发数据源）
                    String matchItemId;
                    bool isPlatformTrigger = (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::PLATFORM_TRIGGER));
                    if (isPlatformTrigger) {
                        matchItemId = trigger.triggerPeriphId;
                    } else if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER) &&
                               trigger.eventId.startsWith("ds:")) {
                        matchItemId = trigger.eventId.substring(3);
                    } else {
                        continue;
                    }

                    // 匹配数据源外设 ID（必须配置且匹配，空/null 不匹配任何数据）
                    if (matchItemId.isEmpty() || matchItemId == "null") continue;
                    if (matchItemId != itemId) continue;

                    if (trigger.lastTriggerTime > 0 &&
                        (millis() - trigger.lastTriggerTime) < PERIPH_EXEC_POLL_TRIGGER_MIN_INTERVAL_MS) continue;

                    // 条件评估
                    bool conditionMet = false;
                    if (isPlatformTrigger && trigger.operatorType == 1) {
                        // 平台触发"设置模式"：仅需 ID 匹配即可执行，无需条件评估
                        conditionMet = true;
                    } else if (isPlatformTrigger && trigger.operatorType == 0) {
                        // 平台触发"匹配模式"：字符串精确匹配
                        conditionMet = (itemValue == trigger.compareValue);
                    } else {
                        // 事件触发数据源：使用通用条件评估（支持全部 10 种操作符）
                        conditionMet = evaluateCondition(itemValue, trigger.operatorType, trigger.compareValue);
                    }

                    if (conditionMet) {
                        LOGGER.infof("[PeriphExec] Rule '%s' triggered by %s=%s (op=%d, cmp=%s)",
                            rule.name.c_str(), itemId.c_str(), itemValue.c_str(),
                            trigger.operatorType, trigger.compareValue.c_str());

                        matchedTriggerIndex = triggerIndex;
                        triggerMatched = true;
                        break;  // 一个触发器匹配即可
                    }
                }

                if (triggerMatched) {
                    matchedRules.push_back({rule.id, itemValue, matchedTriggerIndex, rule.runtimeRevision});
                }
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁异步分发
    for (const auto& matched : matchedRules) {
        dispatchAndCommitTrigger(matched.ruleId, matched.triggerIndex,
                                 matched.receivedValue, "mqtt", {}, matched.revision);
    }
}

void PeriphExecManager::processPollDataMatch(JsonArray& arr, const String& source,
                                            const FastBee::UartEventOrigin& uartOrigin) {
    // Validate outside _rulesMutex; never invert PM -> rule callback lock order.
    // Serial ingress must carry its captured identity, not bind to current state.
    if ((source == "serial" && !uartOrigin.required) ||
        (source != "serial" && source != "sensor_cache" && !uartOrigin.unbound()) ||
        !PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) {
        FastBee::UartTextDiagnostics::record(FastBee::UartTextDiagnostics::StaleFrame);
        return;
    }
    // 阶段1: 持锁匹配，收集规则ID及其匹配的接收值
    struct MatchedRule {
        String ruleId;
        String receivedValue;
        size_t triggerIndex;
        uint64_t revision;
    };
    std::vector<MatchedRule> matchedRules;

    {
        MutexGuard lock(_rulesMutex);
        if (rules.empty()) return;

        for (JsonObject item : arr) {
            if (!item.containsKey("id") || !item.containsKey("value")) continue;

            String itemId = item["id"].as<String>();
            String itemValue = item["value"].as<String>();

            for (auto& pair : rules) {
                PeriphExecRule& rule = pair.second;
                if (!rule.enabled) continue;

                bool alreadyMatched = false;
                for (const auto& pending : matchedRules) {
                    if (pending.ruleId == rule.id) {
                        alreadyMatched = true;
                        break;
                    }
                }
                if (alreadyMatched) continue;
                bool hasPollCollectionAction = rule._cachedHasPollCollectionAction;

                // 遍历所有触发器（OR 关系）
                bool triggerMatched = false;
                size_t matchedTriggerIndex = 0;
                for (size_t triggerIndex = 0; triggerIndex < rule.triggers.size(); ++triggerIndex) {
                    auto& trigger = rule.triggers[triggerIndex];
                    // 确定要匹配的数据源ID（支持轮询触发和事件触发数据源）
                    String matchItemId;
                    if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER)) {
                        if (source == "modbus_poll" && hasPollCollectionAction) continue;
                        matchItemId = trigger.triggerPeriphId;
                    } else if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER) &&
                               trigger.eventId.startsWith("ds:")) {
                        matchItemId = trigger.eventId.substring(3);
                    } else {
                        continue;
                    }

                    // 匹配数据源外设 ID（必须配置且匹配，空/null 不匹配任何数据）
                    if (matchItemId.isEmpty() || matchItemId == "null") continue;
                    if (matchItemId != itemId) continue;

                    unsigned long minTriggerIntervalMs = getPollTriggerCooldownMs(rule, source);
                    if (trigger.lastTriggerTime > 0 &&
                        (millis() - trigger.lastTriggerTime) < minTriggerIntervalMs) continue;

                    // 条件评估 - 使用通用方法（自动检测数值/字符串类型）
                    bool conditionMet = evaluateCondition(itemValue, trigger.operatorType, trigger.compareValue);

                    if (conditionMet) {
                        LOGGER.infof("[PeriphExec] Poll rule '%s' triggered by %s: %s=%s (op=%d, cmp=%s)",
                            rule.name.c_str(), source.c_str(), itemId.c_str(), itemValue.c_str(),
                            trigger.operatorType, trigger.compareValue.c_str());

                        matchedTriggerIndex = triggerIndex;
                        triggerMatched = true;
                        break;  // 一个触发器匹配即可
                    }
                }

                if (triggerMatched) {
                    matchedRules.push_back({rule.id, itemValue, matchedTriggerIndex, rule.runtimeRevision});
                }
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁异步分发
    for (const auto& matched : matchedRules) {
        dispatchAndCommitTrigger(matched.ruleId, matched.triggerIndex,
                                 matched.receivedValue, "poll-data", uartOrigin, matched.revision);
    }
}

String PeriphExecManager::processDataCommandMatch(JsonArray& cmdArr, const std::vector<int>& processedIndices) {
    // 打印完整 DATA_COMMAND 内容
    {
        static uint32_t _dcSeq = 0;
        uint32_t seq = ++_dcSeq;
        int n = 0;
        for (JsonObject item : cmdArr) {
            String iid = item["id"].as<String>();
            String ival = item["value"].as<String>();
            LOGGER.infof("[PeriphExec] Received command #%u [%d]: id='%s', value='%s'", seq, n, iid.c_str(), ival.c_str());
            n++;
        }
    }

    // 预处理阶段：处理 modbus_read 指令
    JsonDocument modbusReportDoc;
    JsonArray modbusReportArr = modbusReportDoc.to<JsonArray>();

    if (_modbusReadCallback) {
        int idx = 0;
        for (JsonObject item : cmdArr) {
            String itemId = item["id"].as<String>();
            if (itemId == "modbus_read") {
                String itemValue = item["value"].as<String>();
                LOGGER.infof("[PeriphExec] DataCommand: modbus_read command detected");
                String modbusResult = _modbusReadCallback(itemValue);
                JsonDocument tmpDoc;
                if (!deserializeJson(tmpDoc, modbusResult.c_str(), modbusResult.length()) && tmpDoc.is<JsonArray>()) {
                    for (JsonVariant v : tmpDoc.as<JsonArray>()) {
                        modbusReportArr.add(v);
                    }
                }
            }
            idx++;
        }
    }

    // 预处理阶段：处理 modbus_raw_send 指令（透传模式：平台下发 HEX 帧）
    String transparentHexResult;
    {
        for (JsonObject item : cmdArr) {
            String itemId = item["id"].as<String>();
            if (itemId == "modbus_raw_send") {
                String itemValue = item["value"].as<String>();
                LOGGER.infof("[PeriphExec] DataCommand: modbus_raw_send command detected, hex='%s'", itemValue.c_str());
                String rawResult = _modbusRawSendCallback ? _modbusRawSendCallback(itemValue) :
                    "[{\"id\":\"modbus_raw_send\",\"value\":\"\",\"remark\":\"MODBUS_UNAVAILABLE\",\"success\":false}]";
                JsonDocument tmpDoc;
                if (!deserializeJson(tmpDoc, rawResult.c_str(), rawResult.length()) && tmpDoc.is<JsonArray>()) {
                    for (JsonVariant v : tmpDoc.as<JsonArray>()) {
                        modbusReportArr.add(v);
                    }
                } else if (rawResult.length() > 0) {
                    // 透传模式：返回的是纯 HEX 字符串，非 JSON 数组
                    transparentHexResult = rawResult;
                }
            }
        }
    }

    // 阶段1: 持锁匹配，收集规则ID（避免深拷贝整个 PeriphExecRule）
    struct MatchedItem {
        String ruleId;
        String itemId;
        String itemValue;
        uint64_t revision;
    };
    std::vector<MatchedItem> matchedItems;
    std::vector<String> unmatchedIds;    // 未匹配的 item id
    std::vector<String> unmatchedValues; // 未匹配的 item value

    {
        MutexGuard lock(_rulesMutex);

        int itemIdx = 0;
        for (JsonObject item : cmdArr) {
            // 跳过已在预处理阶段处理的 modbus_read 项
            bool skip = false;
            for (int pi : processedIndices) { if (pi == itemIdx) { skip = true; break; } }
            itemIdx++;
            if (skip) continue;

            if (!item.containsKey("id") || !item.containsKey("value")) continue;

            String itemId = item["id"].as<String>();
            // Reserved raw commands are handled by the preprocessing callback,
            // including rejection/unavailability; never execute a same-ID rule
            // or direct-output fallback as a second command.
            if (itemId == "modbus_raw_send") {
                continue;
            }
            String itemValue = item["value"].as<String>();
            bool matched = false;

            for (auto& pair : rules) {
                PeriphExecRule& rule = pair.second;
                if (!rule.enabled) continue;

                // 遍历触发器匹配数据源
                for (auto& trigger : rule.triggers) {
                    if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::PLATFORM_TRIGGER)) continue;
                    if (trigger.triggerPeriphId.isEmpty() || trigger.triggerPeriphId == "null") continue;
                    if (trigger.triggerPeriphId != itemId) continue;

                    // 条件评估 - 区分匹配模式和设置模式
                    bool conditionMet = false;
                    if (trigger.operatorType == 1) {
                        // 设置模式：仅需 ID 匹配即可执行
                        conditionMet = true;
                    } else if (trigger.operatorType == 0) {
                        // 匹配模式：字符串精确匹配
                        conditionMet = (itemValue == trigger.compareValue);
                    } else {
                        // 其他操作符：使用通用条件评估（支持全部 10 种）
                        conditionMet = evaluateCondition(itemValue, trigger.operatorType, trigger.compareValue);
                    }

                    if (!conditionMet) continue;

                    matched = true;
                    trigger.lastTriggerTime = millis();
                    trigger.triggerCount++;
                    matchedItems.push_back({rule.id, itemId, itemValue, rule.runtimeRevision});
                    break;  // 一个触发器匹配即可
                }
            }

            if (!matched) {
                unmatchedIds.push_back(itemId);
                unmatchedValues.push_back(itemValue);
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁同步执行并构建响应
    JsonDocument reportDoc;
    JsonArray reportArr = reportDoc.to<JsonArray>();

    // 先添加 Modbus 读取结果
    for (JsonVariant v : modbusReportArr) {
        reportArr.add(v);
    }

    // 获取 Modbus sensorId 构建回调（用于动作结果中的 sensorId 映射）

    for (auto& mi : matchedItems) {
        // 锁外逐个获取规则副本并同步执行（每次只持有一份拷贝）
        PeriphExecRule ruleCopy;
        {
            MutexGuard lock(_rulesMutex, pdMS_TO_TICKS(50));
            if (!lock.isLocked()) continue;
            auto it = rules.find(mi.ruleId);
            if (it == rules.end() || it->second.runtimeRevision != mi.revision) continue;
            ruleCopy = it->second;
        }

        LOGGER.infof("[PeriphExec] Matched rule '%s': trigger id='%s', value='%s', actions=%d",
            ruleCopy.name.c_str(), mi.itemId.c_str(), mi.itemValue.c_str(), ruleCopy.actions.size());

        // 使用 suppressReport 抑制 executeAllActions 内部的 reportActionResults 上报，
        // 由 processDataCommandMatch 统一构建响应并返回给调用方上报，避免双重上报
        AsyncExecResult mqttTrace;
        PeripheralManager::getInstance().beginOrdinaryControl(
            FastBee::OrdinaryControl::Source::Mqtt, ruleCopy.outputActor, 0, false, 0, String(),
            FastBee::SensorConfigEpoch::current(), {}, mqttTrace.ordinaryControl);
        mqttTrace.ordinaryControl.ruleBound = true;
        mqttTrace.ordinaryControl.ruleEpoch = FastBee::RuleOutputIdentity::currentEpoch();
        mqttTrace.peripheralConfigEpoch = mqttTrace.ordinaryControl.configEpoch;
        auto results = executeAllActions(ruleCopy, mi.itemValue, true, nullptr, nullptr, 0, &mqttTrace);

        // 构建响应：每个动作的结果（Modbus 目标映射 sensorId）
        for (const auto& ar : results) {
            // Execution captured the report identity/value before I/O. Reuse
            // the same serializer as asynchronous reporting: no current PM/
            // Modbus lookup and no request value masquerading as observation.
            if (FastBee::appendCapturedReportRow(reportArr, ar) != FastBee::CapturedReportRow::Added) {
                LOGGER.warning("[PeriphExec] REPORT_IDENTITY_OR_SERIALIZATION_FAILED; matched batch not returned");
                return String();
            }
        }
        if (results.empty()) {
            JsonObject reportItem = reportArr.add<JsonObject>();
            if (reportItem.isNull() || !reportItem["id"].set(mi.itemId) ||
                !reportItem["value"].set("") || !reportItem["remark"].set("no actions")) {
                LOGGER.warning("[PeriphExec] REPORT_SERIALIZATION_FAILED; matched batch not returned");
                return String();
            }
        }
    }

    // 未匹配项：尝试通过 Modbus 直接控制回调处理，或回退到直接外设控制
    PeripheralManager& periMgr = PeripheralManager::getInstance();
    for (size_t i = 0; i < unmatchedIds.size(); i++) {
        // 优先尝试 Modbus 直接控制回调（协议层注入）
        if (_modbusDirectControlCb) {
            static const uint64_t directModbusActor = FastBee::RuleOutputIdentity::allocateActor();
            FastBee::OrdinaryControlContext control;
            periMgr.beginOrdinaryControl(FastBee::OrdinaryControl::Source::Mqtt,
                directModbusActor, 0, false, 0, String(), FastBee::SensorConfigEpoch::current(), {}, control);
            // Even unavailable admission is delivered as a matched rejection;
            // callback true still means handled and must never fall through.
            struct ModbusTerminal {
                PeripheralManager& manager;
                const FastBee::OrdinaryControlContext& control;
                ~ModbusTerminal() { manager.finishOrdinaryControl(control); }
            } terminal{periMgr, control};
            const bool handled = _modbusDirectControlCb(unmatchedIds[i], unmatchedValues[i], reportArr, control);
            if (handled) continue;
        }

        // 回退：尝试直接外设控制
        PeripheralConfig directSnapshot;
        uint32_t directConfigEpoch = 0;
        PeripheralConfig* directCfg = periMgr.getPeripheralSnapshot(unmatchedIds[i], directSnapshot, &directConfigEpoch)
            ? &directSnapshot : nullptr;
        if (!directCfg) {
            // 既无规则匹配、也不存在对应外设（如平台属性下发附带的
            // temperature/humidity 等只读项）：跳过，不回显到 property/post。
            // 原样回显会被平台当成设备新上报的传感器数据（假数据污染），
            // 频繁下发时还会带来无意义的 JSON 构建+MQTT 发布内存开销。
            LOGGER.warningf("[PeriphExec] DataCommand item '%s' has no rule/peripheral, skip echo",
                            unmatchedIds[i].c_str());
            continue;
        }
        String actualValue = unmatchedValues[i];
        FastBee::OrdinaryControlResult controlResult;
        bool directOk = executeDirectOutputCommand(periMgr, *directCfg, unmatchedValues[i], actualValue,
            directConfigEpoch, controlResult);

        JsonObject reportItem = reportArr.add<JsonObject>();
        reportItem["id"] = unmatchedIds[i];
        reportItem["value"] = actualValue;
        reportItem["remark"] = controlResult.evaluated ? controlResult.reason :
            directOk ? "direct_peripheral" : "no matching rule";
    }

    // 透传模式：若返回的是纯 HEX 字符串且无其他 JSON 内容，直接返回 HEX
    if (transparentHexResult.length() > 0 && reportArr.size() == 0) {
        return transparentHexResult;
    }

    String result;
    serializeJson(reportDoc, result);
    return result;
}

// 按规则ID安全分发（锁内拷贝规则，锁外调用dispatchAsync，避免死锁）
bool PeriphExecManager::dispatchByRuleId(const String& ruleId,
                                         const String& receivedValue,
                                         String* rejectionReason,
                                         const FastBee::UartEventOrigin& uartOrigin, uint64_t expectedRevision,
                                         FastBee::OrdinaryControl::Source automaticSource) {
    if (rejectionReason) *rejectionReason = "";
    PeriphExecRule ruleCopy;
    bool ruleAcquired = false;
    bool ruleMissing = false;
    // 最多尝试 3 次（每次 50ms，总超时约 150ms）避免按键等一次性事件因瞬时锁竞争
    // （例如 checkTimerTriggers 正持 rulesMutex 100ms）而被直接丢弃
    for (int attempt = 0; attempt < 3 && !ruleAcquired && !ruleMissing; ++attempt) {
        MutexGuard lock(_rulesMutex, pdMS_TO_TICKS(50));
        if (lock.isLocked()) {
            auto it = rules.find(ruleId);
            if (it == rules.end() || !it->second.enabled) {
                ruleMissing = true;  // 规则不存在或被禁用，不再重试
                break;
            }
            if (!expectedRevision || it->second.runtimeRevision != expectedRevision) {
                if (rejectionReason) *rejectionReason = "rule_revision_unconfirmed";
                return false;
            }
            ruleCopy = it->second;  // 锁内做一次深拷贝
            ruleAcquired = true;
        }
    }
    if (ruleMissing) {
        if (rejectionReason) *rejectionReason = "rule_not_found_or_disabled";
        return false;
    }
    if (!ruleAcquired) {
        LOGGER.warningf("[PeriphExec] dispatchByRuleId: rulesMutex busy, drop event for rule '%s'",
                        ruleId.c_str());
        if (rejectionReason) *rejectionReason = "rule_service_busy";
        return false;
    }
    // 锁已释放，安全调用 dispatchAsync
    return dispatchAsync(ruleCopy, receivedValue, rejectionReason, nullptr, uartOrigin, nullptr, automaticSource);
}

String PeriphExecManager::getDynamicEventsJsonInternal() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    // 添加外设执行规则作为动态事件
    MutexGuard lock(_rulesMutex);

    for (const auto& pair : rules) {
        const PeriphExecRule& rule = pair.second;
        if (!rule.enabled) continue;

        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = rule.id;
        obj["name"] = rule.name;
        obj["category"] = "外设执行";
        obj["type"] = static_cast<uint8_t>(EventType::EVENT_PERIPH_EXEC_COMPLETED);
        obj["isDynamic"] = true;
    }

    // 通过回调添加 Modbus 控制类子设备作为动态事件（mc:deviceIndex 格式）
    if (_modbusDynamicEventsCb) {
        _modbusDynamicEventsCb(arr);
    }

    String result;
    serializeJson(doc, result);
    return result;
}

// ========== 通用条件评估 ==========

bool PeriphExecManager::evaluateCondition(const String& value, uint8_t op, const String& compareValue) {
    ExecOperator oper = static_cast<ExecOperator>(op);

    // 字符串操作符（不需要数值转换）
    if (oper == ExecOperator::CONTAIN) {
        return value.indexOf(compareValue) >= 0;
    }
    if (oper == ExecOperator::NOT_CONTAIN) {
        return value.indexOf(compareValue) < 0;
    }

    // EQ / NEQ：自动检测数值/字符串类型
    if (oper == ExecOperator::EQ || oper == ExecOperator::NEQ) {
        bool bothNumeric = isNumericString(value) && isNumericString(compareValue);
        if (bothNumeric) {
            float val = value.toFloat();
            float cmp = compareValue.toFloat();
            return (oper == ExecOperator::EQ) ? (val == cmp) : (val != cmp);
        }
        // 非数值：字符串精确比较
        return (oper == ExecOperator::EQ) ? (value == compareValue) : (value != compareValue);
    }

    // 数值操作符（GT/LT/GTE/LTE/BETWEEN/NOT_BETWEEN）
    float val = value.toFloat();

    switch (oper) {
        case ExecOperator::GT:  return val > compareValue.toFloat();
        case ExecOperator::LT:  return val < compareValue.toFloat();
        case ExecOperator::GTE: return val >= compareValue.toFloat();
        case ExecOperator::LTE: return val <= compareValue.toFloat();
        case ExecOperator::BETWEEN:
        case ExecOperator::NOT_BETWEEN: {
            int commaIdx = compareValue.indexOf(',');
            if (commaIdx < 0) return false;
            float minVal = compareValue.substring(0, commaIdx).toFloat();
            float maxVal = compareValue.substring(commaIdx + 1).toFloat();
            bool inRange = (val >= minVal && val <= maxVal);
            return (oper == ExecOperator::BETWEEN) ? inRange : !inRange;
        }
        default: return false;
    }
}

// ========== 工具 ==========

String PeriphExecManager::generateUniqueId() {
    return "exec_" + String(millis());
}

#endif // FASTBEE_ENABLE_PERIPH_EXEC
