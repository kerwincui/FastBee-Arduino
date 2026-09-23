/**
 * @file PeriphExecScheduler.cpp
 * @brief 外设执行调度器实现
 * @author kerwincui
 * @copyright FastBee All rights reserved.
 */

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC

#include "core/PeriphExecScheduler.h"
#include "core/ChipConfig.h"
#include "core/MemoryBudget.h"
#include "core/PeriphExecManager.h"
#include "core/PeriphExecExecutor.h"
#include "core/FastBeeFramework.h"
#include "protocols/ProtocolManager.h"
#if FASTBEE_ENABLE_MODBUS
#include "protocols/ModbusHandler.h"
#endif
#if FASTBEE_ENABLE_MQTT
#include "protocols/MQTTClient.h"
#endif
#include "systems/LoggerSystem.h"
#include "utils/HeapFragmentation.h"
#include "core/PeripheralManager.h"
#include "core/SensorConfigEpoch.h"
#include <cstring>
#include <esp_heap_caps.h>
#include <WiFi.h>

// 注意：不再使用静态 JsonDocument，因为 ArduinoJson v7 的 JsonDocument
// 内存池只增不减（clear() 不释放内存），长时间运行会导致堆碎片化。
// 改为每次使用局部 JsonDocument，用完即销毁释放内存。
//
// 碎片治理（2026-08-18）：序列化输出使用静态复用 String _serializeBuffer，
// 仅清空 length 不释放底层 buffer，消除高频 alloc/free 抖动。
// JsonDocument 仍为局部变量（v7 clear() 不释放），但序列化目标复用。

// 静态复用缓冲定义（单线程串行使用，Scheduler 在 loopTask 中运行）
String PeriphExecScheduler::_serializeBuffer;

// 协议连接状态标志位
#define PROTOCOL_MQTT_CONNECTED    (1 << 0)
#define PROTOCOL_MODBUS_CONNECTED  (1 << 1)

namespace {
// web-reserve 阈值（滞回设计，防边界振荡）—— 8BIT DRAM 口径，与静态资源门控一致：
// 各芯片 DRAM 基线不同，阈值按芯片差异化定义统一收口在 ChipConfig.h
// （CHIP_WEB_RESERVE_*），业务代码禁止再按 CONFIG_IDF_TARGET 自行派生。
constexpr uint32_t WEB_RESERVE_SUSPEND_FREE_HEAP_BYTES = CHIP_WEB_RESERVE_SUSPEND_FREE_HEAP_BYTES;
constexpr uint32_t WEB_RESERVE_SUSPEND_LARGEST_BLOCK_BYTES = CHIP_WEB_RESERVE_SUSPEND_LARGEST_BLOCK;
constexpr uint32_t WEB_RESERVE_RESUME_FREE_HEAP_BYTES = CHIP_WEB_RESERVE_RESUME_FREE_HEAP_BYTES;
constexpr uint32_t WEB_RESERVE_RESUME_LARGEST_BLOCK_BYTES = CHIP_WEB_RESERVE_RESUME_LARGEST_BLOCK;
// 心跳放行：持续暂停期间每 90s 放行一次检查，让后台采集规则跑一轮刷新
// 传感器缓存（TTL 120s），保证实时监测不丢失温湿度等缓存型数据；
// 单次执行的内存跌落是瞬时的，与 Web 请求碰撞概率极低。
// 真机事故（2026-08）：只靠阈值二选一——规则跑则 heap 跌至 29KB 导致 Web 断流，
// 规则停则缓存过期丢失温湿度。心跳放行是两者兼顾的解。
constexpr unsigned long WEB_RESERVE_HEARTBEAT_ALLOW_MS = 90000UL;
constexpr uint32_t WEB_RESERVE_FREE_HEAP_BYTES = 8192U;      // 极端底线 free8 < 8KB（低于主暂停 12KB）
constexpr uint32_t WEB_RESERVE_LARGEST_BLOCK_BYTES = 6144U;
constexpr uint32_t WEB_RESERVE_FRAGMENTED_BLOCK_BYTES = 12288U;
constexpr uint8_t WEB_RESERVE_FRAGMENTATION_PERCENT = 65U;
constexpr unsigned long SERIAL_CHECK_INTERVAL_MS = 250UL;
constexpr unsigned long SERIAL_FRAME_IDLE_MS = 500UL;
constexpr size_t SERIAL_READ_CHUNK_BYTES = 64;
constexpr size_t SERIAL_FRAME_MAX_BYTES = 160;
// 内存保护暂停心跳：HealthMonitor 暂停定时器后，每 120s 放行一次（非 CRITICAL
// 且 DRAM 高于外设执行门控时），避免无 PSRAM 板启用 MQTT 后 DRAM 基线长期
// 停留 SEVERE、遥测定时规则永久停摆（NORMAL 恢复线在该机型不可达）
constexpr unsigned long MEM_PAUSE_HEARTBEAT_ALLOW_MS = 120000UL;

bool shouldSuspendBackgroundPolling(MemoryGuardLevel level,
                                    uint32_t freeHeap,
                                    uint32_t largestBlock,
                                    uint8_t fragmentation,
                                    bool currentlySuspended) {
    if (level >= MemoryGuardLevel::SEVERE) {
        return true;
    }

    if (freeHeap < WEB_RESERVE_SUSPEND_FREE_HEAP_BYTES) {
        return true;
    }

    if (largestBlock < WEB_RESERVE_SUSPEND_LARGEST_BLOCK_BYTES) {
        return true;
    }

    // 兼容旧底线：极端碎片化仍直接暂停
    if (freeHeap < WEB_RESERVE_FREE_HEAP_BYTES || largestBlock < WEB_RESERVE_LARGEST_BLOCK_BYTES) {
        return true;
    }

    if (fragmentation >= WEB_RESERVE_FRAGMENTATION_PERCENT &&
        largestBlock < WEB_RESERVE_FRAGMENTED_BLOCK_BYTES) {
        return true;
    }

    // Recovery must clear every suspension condition, including fragmentation.
    // Otherwise one unchanged heap snapshot can alternate suspended/resumed.
    if (currentlySuspended) {
        return !(freeHeap >= WEB_RESERVE_RESUME_FREE_HEAP_BYTES &&
                 largestBlock >= WEB_RESERVE_RESUME_LARGEST_BLOCK_BYTES);
    }
    return false;
}
}

PeriphExecScheduler::PeriphExecScheduler() = default;
PeriphExecScheduler::~PeriphExecScheduler() = default;

void PeriphExecScheduler::initialize(PeriphExecManager* manager, PeriphExecExecutor* executor) {
    _manager = manager;
    _executor = executor;
}

// ========== 配置安全校验 ==========

void PeriphExecScheduler::validateLoadedConfig() {
    if (!_manager) return;

    const auto& rulesMap = _manager->getRules();

    // 统计活跃定时/轮询任务数（enabled 且触发类型为 TIMER 或 POLL）
    size_t activeTimerTaskCount = 0;
    for (const auto& kv : rulesMap) {
        if (!kv.second.enabled) continue;
        for (const auto& trigger : kv.second.triggers) {
            if (trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER) ||
                trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER)) {
                activeTimerTaskCount++;
                break;  // 每个规则只计数一次
            }
        }
    }

    // 校验活跃任务数上限
    if (activeTimerTaskCount > MAX_ACTIVE_TASKS) {
        LOGGER.warningf("[WARN] Active timer/poll tasks (%u) exceeds max (%u), "
                        "excess rules may cause resource pressure",
                        static_cast<unsigned int>(activeTimerTaskCount), MAX_ACTIVE_TASKS);
    } else if (activeTimerTaskCount > WARN_TASK_THRESHOLD) {
        LOGGER.warningf("[WARN] High number of active timer/poll tasks: %u (threshold=%u)",
                        static_cast<unsigned int>(activeTimerTaskCount), WARN_TASK_THRESHOLD);
    }

    // Manager 是间隔规范化的唯一写入方。Scheduler 只诊断，不得在运行期
    // 改写配置，否则文件/API 回显与实际调度值会产生分叉。
    for (const auto& kv : rulesMap) {
        if (!kv.second.enabled) continue;

        for (const auto& trigger : kv.second.triggers) {
            const bool isTimer =
                trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER);
            const bool isPoll =
                trigger.triggerType == static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER);
            if (!isTimer && !isPoll) continue;

            const uint32_t minimumSec =
                isPoll ? MIN_POLL_INTERVAL_SEC : MIN_TIMER_INTERVAL_SEC;
            if (trigger.intervalSec < minimumSec ||
                trigger.intervalSec > MAX_INTERVAL_SEC) {
                LOGGER.warningf(
                    "[WARN] Non-canonical %s interval (%lus) for rule '%s'; "
                    "expected %lu..%lus. Manager persistence must normalize it",
                    isPoll ? "poll" : "timer",
                    static_cast<unsigned long>(trigger.intervalSec),
                    kv.second.name.c_str(),
                    static_cast<unsigned long>(minimumSec),
                    static_cast<unsigned long>(MAX_INTERVAL_SEC));
                continue;
            }

            if (activeTimerTaskCount > WARN_TASK_THRESHOLD &&
                trigger.intervalSec < AGGRESSIVE_INTERVAL_WARN_SEC) {
                LOGGER.warningf(
                    "[WARN] Aggressive %s interval (%lus) with %u active tasks "
                    "for rule '%s'; value preserved, adjust the saved rule explicitly if needed",
                    isPoll ? "poll" : "timer",
                    static_cast<unsigned long>(trigger.intervalSec),
                    static_cast<unsigned int>(activeTimerTaskCount),
                    kv.second.name.c_str());
            }
        }
    }

    LOGGER.infof("[PeriphExec] Config validation complete: %u active timer/poll tasks",
                 static_cast<unsigned int>(activeTimerTaskCount));
}

// ========== 定时触发 ==========

uint32_t PeriphExecScheduler::getDynamicCheckPeriod(MemoryGuardLevel level) {
    switch (level) {
        case MemoryGuardLevel::WARN:    return CHECK_PERIOD_WARN_MS;
        case MemoryGuardLevel::SEVERE:  return CHECK_PERIOD_SEVERE_MS;
        case MemoryGuardLevel::CRITICAL: return CHECK_PERIOD_SEVERE_MS;  // CRITICAL 由 checkTimers 单独处理
        default:                        return CHECK_PERIOD_NORMAL_MS;
    }
}

void PeriphExecScheduler::checkTimers() {
    // 内存保护暂停：HealthMonitor 检测到内存压力时暂停所有定时器；
    // 心跳放行：暂停期间每 120s 放行一次（CRITICAL 或 DRAM 低于外设执行
    // 门控时除外），保证遥测定时规则在长期 SEVERE 机型上不完全停摆
    if (_memoryPressurePaused) {
        const uint32_t pausedDram = FastBee::MemoryBudget::dram8Free();
        MemoryGuardLevel pausedLevel = MemoryGuardLevel::NORMAL;
        auto* pausedFw = FastBeeFramework::getInstance();
        if (pausedFw && pausedFw->getHealthMonitor()) {
            pausedLevel = pausedFw->getHealthMonitor()->getMemoryGuardLevel();
        }
        const bool heartbeatDue =
            (millis() - _memoryPressurePausedSince >= MEM_PAUSE_HEARTBEAT_ALLOW_MS);
        if (pausedLevel >= MemoryGuardLevel::CRITICAL ||
            pausedDram < FastBee::MemoryBudget::PERIPH_EXEC_MIN_DRAM ||
            !heartbeatDue) {
            return;
        }
        _memoryPressurePausedSince = millis();
        // 同步强制 web-reserve 一并放行：两套暂停机制串行检查，若各自独立
        // 心跳周期(120s/90s)，放行需两周期同时命中(LCM=360s)，遥测近乎停摆。
        // 将 web-reserve 计时回拨至已到期，保证本次内存心跳两套同时放行
        if (_webReserveSuspended) {
            _webReserveSuspendedSince = millis() - WEB_RESERVE_HEARTBEAT_ALLOW_MS;
        }
        Serial.println("[PeriphExec] Memory-pause heartbeat allow: timers run once");
        // 不 return：本次检查放行，保持 _memoryPressurePaused=true，下次检查继续暂停
    }

    unsigned long now = millis();

    if (!_manager) return;

    // ========== 动态降频：根据 MemGuard 级别调整检查周期 ==========
    MemoryGuardLevel currentLevel = MemoryGuardLevel::NORMAL;
    auto* fw = FastBeeFramework::getInstance();
    if (fw) {
        HealthMonitor* monitor = fw->getHealthMonitor();
        if (monitor) {
            currentLevel = monitor->getMemoryGuardLevel();
        }
    }

    // Match Web/network admission: only byte-addressable internal DRAM can
    // satisfy their allocations. INTERNAL alone includes non-8-bit regions;
    // 8BIT alone includes PSRAM and can mask exhausted internal memory.
    const uint32_t freeHeap = FastBee::MemoryBudget::dram8Free();
    const uint32_t largestBlock = FastBee::MemoryBudget::dram8Largest();
    const uint8_t fragmentation = calculateHeapFragmentationPercent(freeHeap, largestBlock);

    if (shouldSuspendBackgroundPolling(currentLevel, freeHeap, largestBlock, fragmentation,
                                       _webReserveSuspended)) {
        if (!_webReserveSuspended) {
            // 仅在进入暂停状态时输出一次日志，避免周期性刷屏
            _webReserveSuspendedSince = now;
            LOGGER.warningf("[PeriphExec] Background timer/poll suspended for web reserve "
                            "(guard=%d heap=%lu largest=%lu frag=%u%%)",
                            static_cast<int>(currentLevel),
                            static_cast<unsigned long>(freeHeap),
                            static_cast<unsigned long>(largestBlock),
                            static_cast<unsigned int>(fragmentation));
            _webReserveSuspended = true;
        }
        // 心跳放行：持续暂停满 90s 后放行一次（CRITICAL 极端缺内存时不放行）
        bool heartbeatAllow = (currentLevel < MemoryGuardLevel::CRITICAL) &&
                              (now - _webReserveSuspendedSince >= WEB_RESERVE_HEARTBEAT_ALLOW_MS);
        if (heartbeatAllow) {
            _webReserveSuspendedSince = now;  // 重置，为下次放行重新计时
            LOGGER.info("[PeriphExec] Heartbeat allow: background rules run once to refresh sensor cache");
            // 不 return：本次检查放行，保持 _webReserveSuspended=true，下次检查继续暂停
        } else {
            _lastMemGuardLevel = currentLevel;
            _currentCheckPeriodMs = CHECK_PERIOD_SEVERE_MS;
            return;
        }
    }

    if (_webReserveSuspended && currentLevel < MemoryGuardLevel::SEVERE &&
        !shouldSuspendBackgroundPolling(currentLevel, freeHeap, largestBlock, fragmentation,
                                        _webReserveSuspended)) {
        LOGGER.infof("[PeriphExec] Background timer/poll resumed "
                     "(guard=%d heap=%lu largest=%lu frag=%u%%)",
                     static_cast<int>(currentLevel),
                     static_cast<unsigned long>(freeHeap),
                     static_cast<unsigned long>(largestBlock),
                     static_cast<unsigned int>(fragmentation));
        _webReserveSuspended = false;
        _webReserveSuspendedSince = 0;
    }

    // CRITICAL: 暂停非关键轮询，直接返回
    if (currentLevel == MemoryGuardLevel::CRITICAL) {
        // 仅在等级变化时输出日志，避免周期性刷屏
        if (_lastMemGuardLevel != MemoryGuardLevel::CRITICAL) {
            LOGGER.warning("[PeriphExec] MemGuard CRITICAL — non-critical polling suspended");
        }
        _lastMemGuardLevel = currentLevel;
        _currentCheckPeriodMs = CHECK_PERIOD_SEVERE_MS;
        return;
    }

    // 等级变化时更新检查周期并输出日志
    if (currentLevel != _lastMemGuardLevel) {
        _currentCheckPeriodMs = getDynamicCheckPeriod(currentLevel);
        const char* prevName = "NORMAL";
        const char* curName = "NORMAL";
        switch (_lastMemGuardLevel) {
            case MemoryGuardLevel::WARN:    prevName = "WARN"; break;
            case MemoryGuardLevel::SEVERE:  prevName = "SEVERE"; break;
            case MemoryGuardLevel::CRITICAL: prevName = "CRITICAL"; break;
            default: break;
        }
        switch (currentLevel) {
            case MemoryGuardLevel::WARN:    curName = "WARN"; break;
            case MemoryGuardLevel::SEVERE:  curName = "SEVERE"; break;
            default: break;
        }
        LOGGER.infof("[PeriphExec] MemGuard level changed: %s -> %s, check period=%lums",
                     prevName, curName, (unsigned long)_currentCheckPeriodMs);
        _lastMemGuardLevel = currentLevel;
    }

    // 按动态周期检查
    if (now - _lastTimerCheck < _currentCheckPeriodMs) return;
    _lastTimerCheck = now;

    // 预先获取 ModbusHandler 状态（避免在锁内调用）
    bool modbusAvailable = false;
#if FASTBEE_ENABLE_MODBUS
    ProtocolManager::ModbusHandlerLease modbusLease;
    ModbusHandler* modbus = nullptr;
    if (fw) {
        auto* protMgr = fw->getProtocolManager();
        if (protMgr) {
            modbusLease = protMgr->acquireModbusHandler(pdMS_TO_TICKS(100));
            modbus = modbusLease.get();
            modbusAvailable = (modbus != nullptr && modbus->getMode() == MODBUS_MASTER);
        }
    }
#endif

    // 通过 manager 获取规则列表并检查定时触发
    checkSerialEvents();
    _manager->checkTimerTriggers(now, modbusAvailable);

    // 启动后一次性物模型状态上报（MQTT 连接成功后分批执行，每周期最多一条规则，节流）
    _manager->processBootReport();

    // 定期淘汰过期传感器缓存（每次定时器检查周期顺带执行，开销忽略不计）
    _manager->evictStaleSensorCache();
}

// ========== 事件触发 ==========

void PeriphExecScheduler::triggerEvent(EventType eventType, const String& eventData) {
    if (!_manager) return;

    // 启动保护：PeriphExecManager 未初始化时（mutex 未创建），跳过事件处理
    if (!_manager->isInitialized()) return;

    // 查找匹配的事件ID / 名称
    const char* targetEventId = nullptr;
    const char* targetEventName = nullptr;
    for (size_t i = 0; STATIC_EVENTS[i].id != nullptr; i++) {
        if (STATIC_EVENTS[i].type == eventType) {
            targetEventId = STATIC_EVENTS[i].id;
            targetEventName = STATIC_EVENTS[i].name;
            break;
        }
    }

    if (!targetEventId) {
        LOGGER.warningf("[PeriphExec] Unknown event type: %d", (int)eventType);
        return;
    }

    LOGGER.infof("[PeriphExec] Event triggered: %s (data=%s)", targetEventId, eventData.c_str());

    // 通过 manager 分发事件匹配的规则
    _manager->dispatchEventMatchedRules(targetEventId, eventData);

    // 同步通知 MQTT 层：若有订阅了此事件的 DEVICE_EVENT 主题，则上报
    _manager->notifyMqttEventPublish(String(targetEventId),
                                     targetEventName ? String(targetEventName) : String(),
                                     eventData);
}

void PeriphExecScheduler::triggerEventById(const String& eventId, const String& eventData) {
    const EventDef* def = findStaticEvent(eventId.c_str());
    if (def) {
        triggerEvent(def->type, eventData);
        return;
    }

    // 非静态事件：判断前缀或外设类型
    if (!_manager) return;
    if (!_manager->isInitialized()) return;

    if (eventId.startsWith("exec_")) {
        triggerPeriphExecEvent(eventId, eventData);
        return;
    }
    if (eventId.startsWith("mc:")) {
        // Modbus 控制类子设备事件
        LOGGER.infof("[PeriphExec] Modbus control event triggered: %s (data=%s)", eventId.c_str(), eventData.c_str());
        _manager->dispatchEventMatchedRules(eventId, eventData);
        return;
    }

    // 可能是用户自定义的 DEVICE_EVENT 外设事件：尝试查找外设
    String eventName;
    PeripheralConfig cfg;
    if (PeripheralManager::getInstance().getPeripheralSnapshot(eventId, cfg) &&
        cfg.type == PeripheralType::DEVICE_EVENT) {
        eventName = cfg.name;
        LOGGER.infof("[PeriphExec] Device event triggered: %s (%s, data=%s)",
                     eventId.c_str(), eventName.c_str(), eventData.c_str());
        // 同时分发给规则（若有规则以此外设为触发源）与 MQTT
        _manager->dispatchEventMatchedRules(eventId, eventData);
        _manager->notifyMqttEventPublish(eventId, eventName, eventData);
        return;
    }

    // 未匹配：仍尝试通知 MQTT（用户可能手动配置了某个非外设的事件 ID）
    LOGGER.warningf("[PeriphExec] Unknown event ID: %s (forwarding to MQTT only)", eventId.c_str());
    _manager->notifyMqttEventPublish(eventId, String(), eventData);
}

void PeriphExecScheduler::triggerPeriphExecEvent(const String& ruleId, const String& eventData) {
    if (!_manager) return;
    if (!_manager->isInitialized()) return;

    LOGGER.infof("[PeriphExec] Periph exec event triggered: %s", ruleId.c_str());

    // 通过 manager 分发外设执行事件匹配的规则
    _manager->dispatchPeriphExecEvent(ruleId, eventData);
}

// ========== 消息/数据处理 ==========

void PeriphExecScheduler::handleDataEvent(const String& source, const String& data, DataSourceType sourceType) {
    if (!_manager) return;

    // 使用局部 JsonDocument 解析 JSON 数组: [{"id":"temperature","value":"27.43"}, ...]
    // 局部变量用完即销毁，避免静态 JsonDocument 内存池只增不减导致堆碎片化
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data);
    if (err || !doc.is<JsonArray>()) return;

    JsonArray arr = doc.as<JsonArray>();

    // 根据数据源类型调用对应的处理方法
    if (sourceType == DataSourceType::MQTT) {
        _manager->processMqttMessageMatch(arr);
    } else {
        _manager->processPollDataMatch(arr, source);
    }
}

void PeriphExecScheduler::handleMqttMessage(const String& topic, const String& message) {
    handleDataEvent(topic, message, DataSourceType::MQTT);
}

void PeriphExecScheduler::handlePollData(const String& source, const String& data) {
    handleDataEvent(source, data, DataSourceType::POLL);
}

String PeriphExecScheduler::handleDataCommand(const String& message) {
    if (!_manager || !_executor) return "";

    JsonDocument cmdDoc;
    DeserializationError err = deserializeJson(cmdDoc, message);
    if (err || !cmdDoc.is<JsonArray>()) {
        LOGGER.warning("[PeriphExec] Received command: INVALID JSON (parse failed)");
        return "";
    }

    JsonArray cmdArr = cmdDoc.as<JsonArray>();
    LOGGER.infof("[PeriphExec] Received command: DATA_COMMAND with %d items", cmdArr.size());

    // 预处理阶段：处理 modbus_read 指令（阻塞操作，必须在持锁之前完成）
    JsonDocument modbusReportDoc;
    JsonArray modbusReportArr = modbusReportDoc.to<JsonArray>();
    std::vector<int> processedIndices;

    {
        int idx = 0;
        for (JsonObject item : cmdArr) {
            String itemId = item["id"].as<String>();
            if (itemId == "modbus_read" || itemId == "modbus_raw_send") {
                String itemValue = item["value"].as<String>();
                LOGGER.infof("[PeriphExec] DataCommand: %s command detected", itemId.c_str());
                // Note: This is handled by the manager now
                processedIndices.push_back(idx);
            }
            idx++;
        }
    }

    // 通过 manager 处理数据命令匹配和执行
    return _manager->processDataCommandMatch(cmdArr, processedIndices);
}

void PeriphExecScheduler::checkSerialEvents() {
    if (!_manager) return;

    unsigned long now = millis();
    if (_lastSerialCheck > 0 && (now - _lastSerialCheck) < SERIAL_CHECK_INTERVAL_MS) return;
    _lastSerialCheck = now;

    if (FastBee::MemoryBudget::dramFree() < 16000) return;

    PeripheralManager& pm = PeripheralManager::getInstance();

    // Drop buffers for deleted, disabled, or repurposed UARTs so repeated
    // configuration churn cannot grow these maps indefinitely.
    for (auto it = _serialRxBuffers.begin(); it != _serialRxBuffers.end();) {
        PeripheralConfig snapshot;
        if (!pm.getPeripheralSnapshot(it->first, snapshot) || !snapshot.enabled ||
            snapshot.type != PeripheralType::UART || !_manager->hasDataSourceRule(it->first)) {
            _serialRxLastByteAt.erase(it->first);
            it = _serialRxBuffers.erase(it);
        } else {
            ++it;
        }
    }

    auto dispatchFrame = [&](const String& id, String frame) {
        frame.trim();
        if (frame.isEmpty()) return;
        if (frame.length() > SERIAL_FRAME_MAX_BYTES) {
            frame = frame.substring(0, SERIAL_FRAME_MAX_BYTES);
        }

        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        JsonObject item = arr.add<JsonObject>();
        item["id"] = id;
        item["value"] = frame;

        // 复用缓冲：避免每帧 alloc/free String
        _serializeBuffer.reserve(128);
        _serializeBuffer = "";
        serializeJson(doc, _serializeBuffer);
        _manager->handlePollData("serial", _serializeBuffer);
        LOGGER.debugf("[PeriphExec] Serial data source: %s='%s'", id.c_str(), frame.c_str());
    };

    pm.forEachPeripheral([&](const PeripheralConfig& config) {
        if (!config.enabled || config.type != PeripheralType::UART) return;
        if (!_manager->hasDataSourceRule(config.id)) {
            _serialRxBuffers.erase(config.id);
            _serialRxLastByteAt.erase(config.id);
            return;
        }

        uint8_t bytes[SERIAL_READ_CHUNK_BYTES];
        size_t len = sizeof(bytes);
        if (!pm.readData(config.id, bytes, len)) return;

        String& rx = _serialRxBuffers[config.id];
        if (len > 0) {
            _serialRxLastByteAt[config.id] = now;
            for (size_t i = 0; i < len; i++) {
                char ch = static_cast<char>(bytes[i]);
                if (ch == '\r' || ch == '\n') {
                    if (!rx.isEmpty()) {
                        dispatchFrame(config.id, rx);
                        rx = "";
                    }
                    continue;
                }
                if (rx.length() < SERIAL_FRAME_MAX_BYTES) {
                    rx += ch;
                } else {
                    dispatchFrame(config.id, rx);
                    rx = "";
                }
            }
        }

        auto lastIt = _serialRxLastByteAt.find(config.id);
        if (!rx.isEmpty() && lastIt != _serialRxLastByteAt.end() &&
            (now - lastIt->second) >= SERIAL_FRAME_IDLE_MS) {
            dispatchFrame(config.id, rx);
            rx = "";
        }
    });
}

// ========== 数据上报 ==========

bool PeriphExecScheduler::tryReportDeviceData() {
    uint8_t connectedProtocols = 0;

    // 检查网络和协议连接状态
    if (!checkNetworkAndProtocolStatus(connectedProtocols)) {
        LOGGER.debug("[PeriphExec] No connected protocol, skip data report");
        return true;  // 无连接不算失败，只是跳过
    }

    // 收集外设数据
    String periphData = collectPeripheralData();
    if (periphData.isEmpty() || periphData == "[]") {
        LOGGER.debug("[PeriphExec] No peripheral data to report");
        return true;  // 无数据不算失败
    }

    // 优先通过 MQTT 上报
    if (connectedProtocols & PROTOCOL_MQTT_CONNECTED) {
        auto* fw = FastBeeFramework::getInstance();
        if (fw) {
            auto* protocolMgr = fw->getProtocolManager();
            if (protocolMgr) {
                MQTTClient* mqtt = protocolMgr->getMQTTClient();
                if (mqtt && mqtt->getIsConnected()) {
                    bool ok = mqtt->publishReportData(periphData);
                    if (ok) {
                        LOGGER.infof("[PeriphExec] Data reported via MQTT, size=%d", periphData.length());
                        return true;
                    }
                }
            }
        }
    }

    LOGGER.warning("[PeriphExec] Data report failed, no available protocol");
    return false;
}

// ========== 按键事件检测 ==========

void PeriphExecScheduler::checkButtonEvents() {
    unsigned long now = millis();

    // Provider sampling is task based; no I2C transaction runs in a GPIO ISR.
    if (now - _lastButtonCheck < 20) return;
    _lastButtonCheck = now;

    checkComplementaryInputs(static_cast<uint32_t>(now));

    PeripheralManager& pm = PeripheralManager::getInstance();

    for (auto it = _buttonStates.begin(); it != _buttonStates.end();) {
        PeripheralConfig snapshot;
        if (!pm.getPeripheralSnapshot(it->first, snapshot) || !snapshot.enabled ||
            !isGpioDigitalInputType(snapshot.type)) {
            it = _buttonStates.erase(it);
        } else {
            ++it;
        }
    }

    pm.forEachPeripheral([this, &pm, now](const PeripheralConfig& config) {
        if (!config.enabled) return;
        if (!isGpioDigitalInputType(config.type)) return;

        const bool isButton = supportsButtonEvent(config.type) || config.isExpanderChannel();

        uint8_t inputMode = config.params.gpio.inputMode;
        if (inputMode == static_cast<uint8_t>(DigitalInputMode::AUTO)) {
            inputMode = config.type == PeripheralType::GPIO_DIGITAL_INPUT_PULLUP
                            ? static_cast<uint8_t>(DigitalInputMode::PULL_UP)
                            : config.type == PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN
                                ? static_cast<uint8_t>(DigitalInputMode::PULL_DOWN)
                                : static_cast<uint8_t>(DigitalInputMode::FLOATING);
        }
        bool idleLevel = config.isExpanderChannel() ||
                         inputMode == static_cast<uint8_t>(DigitalInputMode::PULL_UP);
        if (config.params.gpio.invertLogic) idleLevel = !idleLevel;
        const uint16_t debounceMs = config.isExpanderChannel() && config.params.gpio.debounceMs == 0
            ? 40 : effectiveDigitalInputDebounceMs(config.params.gpio.debounceMs);

        // 读取当前按键状态
        GPIOState gpioState = pm.readPin(config.id);
        if (gpioState == GPIOState::STATE_UNDEFINED) {
            if (config.isExpanderChannel()) _buttonStates.erase(config.id);
            return;
        }

        bool currentLevel = (gpioState == GPIOState::STATE_HIGH);
        // 获取或创建状态。普通 DI 首次采样只建立基线；按键以空闲电平
        // 为基线，使上电时已按下仍能在稳定一个 debounce 窗口后产生 PRESS。
        auto stateIt = _buttonStates.find(config.id);
        const uint32_t generation = pm.digitalInputGeneration(config);
        if (stateIt == _buttonStates.end() ||
            (config.isExpanderChannel() && stateIt->second.sourceGeneration != generation) ||
            (isButton && stateIt->second.idleLevel != idleLevel)) {
            ButtonRuntimeState state;
            state.sourceGeneration = generation;
            state.awaitIdle = config.isExpanderChannel() && currentLevel != idleLevel;
            state.periphId = config.id;
            state.idleLevel = idleLevel;
            state.lastState = isButton && !config.isExpanderChannel() ? idleLevel : currentLevel;
            state.currentState = currentLevel;
            state.lastChangeTime = now;
            _buttonStates[config.id] = state;
            return;
        }

        ButtonRuntimeState& btnState = stateIt->second;

        // 标准稳定态去抖：电平每次抖动都重启候选计时，只有候选值持续
        // debounceMs 后才提交，避免长时间静止后的首个毛刺被立即接受。
        if (currentLevel != btnState.currentState) {
            btnState.currentState = currentLevel;
            btnState.lastChangeTime = now;
            return;
        }

        if (btnState.awaitIdle) {
            if (currentLevel == btnState.idleLevel &&
                now - btnState.lastChangeTime >= debounceMs) {
                btnState.awaitIdle = false;
                btnState.lastState = currentLevel;
            }
            return;
        }

        if (currentLevel != btnState.lastState) {
            if (now - btnState.lastChangeTime < debounceMs) return;
            btnState.lastState = currentLevel;

            if (!isGpioInterruptInputType(config.type) ||
                matchesDigitalInterruptEdge(config.type, currentLevel)) {
                handleStableDigitalInputChange(config, currentLevel);
            }

            if (!isButton) return;

            if (currentLevel != btnState.idleLevel) {
                btnState.pressStartTime = now;
                triggerButtonEvent(config.id, EventType::EVENT_BUTTON_PRESS);
                LOGGER.debugf("[PeriphExec] Button PRESS: %s", config.id.c_str());
            } else {
                unsigned long pressDuration = now - btnState.pressStartTime;
                triggerButtonEvent(config.id, EventType::EVENT_BUTTON_RELEASE);

                if (pressDuration < _buttonConfig.longPress2sMs) {
                    btnState.clickCount++;
                    btnState.lastClickTime = now;
                    LOGGER.debugf("[PeriphExec] Button CLICK (%d): %s, duration=%lums",
                                  btnState.clickCount, config.id.c_str(), pressDuration);

                    if (btnState.clickCount == 1 && _manager &&
                        !_manager->hasButtonEventRule(config.id, "button_double_click")) {
                        triggerButtonEvent(config.id, EventType::EVENT_BUTTON_CLICK);
                        btnState.clickCount = 0;
                        LOGGER.debugf("[PeriphExec] Button CLICK (immediate, no double-click rule): %s",
                                      config.id.c_str());
                    }
                }

                btnState.longPress2sTriggered = false;
                btnState.longPress5sTriggered = false;
                btnState.longPress10sTriggered = false;
            }
            return;
        }

        if (!isButton) return;

        // 稳定状态未变化，继续处理长按和多击窗口。
        if (currentLevel != btnState.idleLevel) {
            unsigned long pressDuration = now - btnState.pressStartTime;
            if (!btnState.longPress2sTriggered && pressDuration >= _buttonConfig.longPress2sMs) {
                btnState.longPress2sTriggered = true;
                triggerButtonEvent(config.id, EventType::EVENT_BUTTON_LONG_PRESS_2S);
                LOGGER.infof("[PeriphExec] Button LONG_PRESS_2S: %s", config.id.c_str());
            }
            if (!btnState.longPress5sTriggered && pressDuration >= _buttonConfig.longPress5sMs) {
                btnState.longPress5sTriggered = true;
                triggerButtonEvent(config.id, EventType::EVENT_BUTTON_LONG_PRESS_5S);
                LOGGER.infof("[PeriphExec] Button LONG_PRESS_5S: %s", config.id.c_str());
            }
            if (!btnState.longPress10sTriggered && pressDuration >= _buttonConfig.longPress10sMs) {
                btnState.longPress10sTriggered = true;
                triggerButtonEvent(config.id, EventType::EVENT_BUTTON_LONG_PRESS_10S);
                LOGGER.infof("[PeriphExec] Button LONG_PRESS_10S: %s", config.id.c_str());
            }
        } else if (btnState.clickCount > 0 &&
                   now - btnState.lastClickTime >= _buttonConfig.clickIntervalMs) {
            if (btnState.clickCount == 2) {
                triggerButtonEvent(config.id, EventType::EVENT_BUTTON_DOUBLE_CLICK);
                LOGGER.infof("[PeriphExec] Button DOUBLE_CLICK: %s", config.id.c_str());
            } else if (btnState.clickCount == 1) {
                triggerButtonEvent(config.id, EventType::EVENT_BUTTON_CLICK);
                LOGGER.infof("[PeriphExec] Button CLICK: %s", config.id.c_str());
            }
            btnState.clickCount = 0;
        }
    });

    // Interrupt types also have an edge-captured path. It runs after the
    // stable-state scan so a short pulse is delivered immediately and the
    // local polling baseline can be synchronized to avoid duplicate reports.
    uint8_t interruptPin = 0;
    GPIOState interruptState = GPIOState::STATE_UNDEFINED;
    while (pm.popDigitalInputInterruptEvent(interruptPin, interruptState)) {
        const String peripheralId = pm.getPinPeripheralId(interruptPin);
        PeripheralConfig config;
        if (peripheralId.isEmpty() ||
            !pm.getPeripheralSnapshot(peripheralId, config) ||
            !config.enabled || !isGpioInterruptInputType(config.type)) {
            continue;
        }

        const bool logicalLevel = interruptState == GPIOState::STATE_HIGH;
        if (!matchesDigitalInterruptEdge(config.type, logicalLevel)) continue;

        ButtonRuntimeState& state = _buttonStates[config.id];
        state.periphId = config.id;
        state.lastState = logicalLevel;
        state.currentState = logicalLevel;
        state.lastChangeTime = now;
        handleStableDigitalInputChange(config, logicalLevel);
    }
}

void PeriphExecScheduler::checkComplementaryInputs(uint32_t now) {
    if (!_manager || !_manager->isInitialized()) return;
    auto& pm = PeripheralManager::getInstance();
    const uint32_t version = FastBee::SensorConfigEpoch::current();
    if (version != _pairConfigVersion) {
        _pairedInputStates.clear();
        pm.forEachPeripheral([this](const PeripheralConfig& config) {
            if (config.enabled && isGpioDigitalInputType(config.type) &&
                config.params.gpio.pairedInputId[0]) {
                _pairedInputStates.emplace(config.id, FastBee::ComplementaryInputMonitor{});
            }
        });
        _pairConfigVersion = version;
    }
    // Read and publish outside forEachPeripheral's manager lock.
    for (auto& entry : _pairedInputStates) {
        PeripheralConfig primary, peer;
        if (!pm.getPeripheralSnapshot(entry.first, primary) || !primary.enabled ||
            !isGpioDigitalInputType(primary.type) || !primary.params.gpio.pairedInputId[0]) continue;
        const bool peerValid = pm.getPeripheralSnapshot(primary.params.gpio.pairedInputId, peer) &&
            peer.enabled && isGpioDigitalInputType(peer.type);
        const GPIOState a = pm.readPin(primary.id);
        const GPIOState b = peerValid ? pm.readPin(peer.id) : GPIOState::STATE_UNDEFINED;
        if (FastBee::SensorConfigEpoch::current() != version) return;
        const int first = a == GPIOState::STATE_UNDEFINED ? -1 : a == GPIOState::STATE_HIGH;
        const int second = b == GPIOState::STATE_UNDEFINED ? -1 : b == GPIOState::STATE_HIGH;
        const uint16_t stableMs = primary.params.gpio.pairDebounceMs ? primary.params.gpio.pairDebounceMs : 200;
        const bool entered = entry.second.sample(first, second, now, stableMs);
        bool publishReady = false;
#if FASTBEE_ENABLE_MQTT
        MQTTClient* mqtt = getMqttClient();
        publishReady = entry.second.publishDue(now) && mqtt && mqtt->getIsConnected();
#endif
        if (!entered && !publishReady) continue;
        StaticJsonDocument<256> details;
        details["source"] = primary.id;
        details["peer"] = primary.params.gpio.pairedInputId;
        details["first"] = first;
        details["second"] = second;
        String payload;
        serializeJson(details, payload);
        const EventDef* event = findStaticEvent("break_down");
        if (entered) {
            LOGGER.warningf("[PeriphExec] Complementary input fault: %s / %s (%d,%d)",
                            primary.id.c_str(), primary.params.gpio.pairedInputId, first, second);
            _manager->dispatchEventMatchedRules("break_down", payload);
        }
#if FASTBEE_ENABLE_MQTT
        if (publishReady) {
            entry.second.published(now, _manager->notifyMqttEventPublish(
                "break_down", event ? String(event->name) : String("Device fault"), payload));
        }
#else
        (void)event;
#endif
    }
}

void PeriphExecScheduler::handleStableDigitalInputChange(const PeripheralConfig& config, bool level) {
    if (!_manager) return;

    StaticJsonDocument<192> doc;
    JsonArray arr = doc.to<JsonArray>();
    JsonObject item = arr.add<JsonObject>();
    item["id"] = config.id;
    item["value"] = level ? "1" : "0";
    item["remark"] = "change";

    char payload[192];
    const size_t payloadLen = serializeJson(doc, payload, sizeof(payload));
    if (payloadLen == 0 || payloadLen >= sizeof(payload)) return;

    // 规则事件与云端上报解耦：EVENT_ONLY 仍可驱动 ds:/POLL 规则，
    // 但不会产生自动 MQTT 上报。
    if (_manager->hasDataSourceRule(config.id)) {
        _manager->handlePollData("digital_input", String(payload));
    }

#if FASTBEE_ENABLE_MQTT
    if (!shouldReportDigitalInputChange(config.params.gpio.reportMode)) return;
    MQTTClient* mqtt = getMqttClient();
    if (!mqtt) return;
    if (mqtt->getIsConnected()) {
        mqtt->queueReportData(payload, static_cast<uint16_t>(payloadLen));
    } else {
        _manager->queuePendingReport(String(payload));
    }
#endif
}

void PeriphExecScheduler::triggerButtonEvent(const String& periphId, EventType eventType) {
    // 查找事件ID
    const char* targetEventId = nullptr;
    for (size_t i = 0; STATIC_EVENTS[i].id != nullptr; i++) {
        if (STATIC_EVENTS[i].type == eventType) {
            targetEventId = STATIC_EVENTS[i].id;
            break;
        }
    }

    if (!targetEventId) return;

    if (!_manager) return;

    // 碎片治理：用栈缓冲替代 String 拼接，消除 3-4 次小 alloc
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"periphId\":\"%s\"}", periphId.c_str());

    // 通过 manager 分发按键事件匹配的规则
    _manager->dispatchButtonEventRules(targetEventId, periphId);
}

// ========== 配置辅助方法 ==========

String PeriphExecScheduler::getStaticEventsJson() {
    // 碎片治理：内容基于编译期 STATIC_EVENTS，首次构建后缓存，后续直接返回
    static String cached;
    if (!cached.isEmpty()) return cached;

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (size_t i = 0; STATIC_EVENTS[i].id != nullptr; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = STATIC_EVENTS[i].id;
        obj["name"] = STATIC_EVENTS[i].name;
        obj["category"] = STATIC_EVENTS[i].category;
        obj["type"] = static_cast<uint8_t>(STATIC_EVENTS[i].type);
    }

    serializeJson(doc, cached);
    return cached;
}

String PeriphExecScheduler::getDynamicEventsJson() {
    if (!_manager) return "[]";
    return _manager->getDynamicEventsJsonInternal();
}

String PeriphExecScheduler::getValidTriggerTypes() {
    // 碎片治理：内容基于编译期常量，首次构建后缓存
    static String cached;
    if (!cached.isEmpty()) return cached;

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    auto addTrigger = [&arr](uint8_t type, const char* name, const char* desc) {
        JsonObject obj = arr.add<JsonObject>();
        obj["type"] = type;
        obj["name"] = name;
        obj["description"] = desc;
    };

    // 平台触发 - MQTT指令下发
    addTrigger(static_cast<uint8_t>(ExecTriggerType::PLATFORM_TRIGGER),
               "平台触发", "IoT平台通过MQTT下发指令时触发");

    // 定时触发
    addTrigger(static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER),
               "定时触发", "按指定时间间隔或每日时间点触发");

    // 触发事件（合并了原系统事件、按键事件、设备触发、外设执行事件、数据事件）
    addTrigger(static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER),
               "事件触发", "WiFi/MQTT/按键/数据/外设执行等事件触发");

    // 轮询触发（本地数据源条件评估）
    addTrigger(static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER),
               "轮询触发", "本地数据源（如Modbus传感器）周期轮询条件评估触发");

    serializeJson(doc, cached);
    return cached;
}

// 事件分类描述映射（用于 getEventCategoriesJson 生成分类说明）
static const char* eventCategoryDescription(const char* cat) {
    if (strcmp(cat, "WiFi") == 0) return "WiFi连接状态变化事件";
    if (strcmp(cat, "MQTT") == 0) return "MQTT连接状态变化事件";
    if (strcmp(cat, "网络") == 0) return "网络模式切换事件";
    if (strcmp(cat, "协议") == 0) return "协议启用事件";
    if (strcmp(cat, "系统") == 0) return "系统服务与状态事件";
    if (strcmp(cat, "规则") == 0) return "规则引擎事件";
    if (strcmp(cat, "按键") == 0) return "按键输入事件";
    if (strcmp(cat, "数据") == 0) return "协议数据收发事件";
    if (strcmp(cat, "外设执行") == 0) return "外设执行规则事件";
    if (strcmp(cat, "RFID") == 0) return "RFID射频卡识别事件";
    if (strcmp(cat, "红外") == 0) return "红外遥控编码接收事件";
    if (strcmp(cat, "RF") == 0) return "433MHz RF遥控编码接收事件";
    return "设备事件";
}

String PeriphExecScheduler::getEventCategoriesJson() {
    // 碎片治理：内容基于编译期 STATIC_EVENTS 推导，首次构建后缓存
    static String cached;
    if (!cached.isEmpty()) return cached;

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    // 从静态事件表(STATIC_EVENTS)推导事件分类，保证分类与事件定义始终同步，
    // 避免硬编码列表遗漏新增分类（如 红外/RF/RFID 编码接收事件）。
    // 按首次出现顺序去重收集。
    const size_t MAX_CATS = 24;
    const char* seen[MAX_CATS];
    size_t seenCount = 0;
    for (size_t i = 0; STATIC_EVENTS[i].id != nullptr; i++) {
        const char* cat = STATIC_EVENTS[i].category;
        if (!cat) continue;
        bool dup = false;
        for (size_t k = 0; k < seenCount; k++) {
            if (strcmp(seen[k], cat) == 0) { dup = true; break; }
        }
        if (!dup && seenCount < MAX_CATS) seen[seenCount++] = cat;
    }

    for (size_t i = 0; i < seenCount; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = seen[i];
        obj["description"] = eventCategoryDescription(seen[i]);
    }

    // 追加静态事件表中不存在的分类（Modbus子设备为动态事件分类）
    JsonObject modbusObj = arr.add<JsonObject>();
    modbusObj["name"] = "Modbus子设备";
    modbusObj["description"] = "Modbus采集与控制子设备触发事件";

    serializeJson(doc, cached);
    return cached;
}

// ========== 内部方法 ==========

bool PeriphExecScheduler::evaluateCondition(const String& value, uint8_t op, const String& compareValue) {
    // 委托给 PeriphExecManager 的统一实现（支持字符串/数值自动检测）
    return PeriphExecManager::evaluateCondition(value, op, compareValue);
}

String PeriphExecScheduler::collectPeripheralData() {
    PeripheralManager& pm = PeripheralManager::getInstance();

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    pm.forEachPeripheral([&](const PeripheralConfig& config) {
        // 只收集已启用的外设
        if (!config.enabled) return;

        // 只收集 GPIO 类型的外设状态
        int typeVal = static_cast<int>(config.type);
        if (typeVal < 11 || typeVal > 26) return;  // 跳过非GPIO/ADC/DAC类型

        if (isGpioDigitalInputType(config.type) &&
            !shouldReportDigitalInputPeriodically(config.params.gpio.reportMode)) {
            return;
        }
        if (config.type == PeripheralType::RADAR_SENSOR) return;

        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = config.id;
        obj["name"] = config.name;
        obj["type"] = typeVal;

        // 获取运行时状态
        PeripheralRuntimeState runtimeState;
        if (pm.getRuntimeStateSnapshot(config.id, runtimeState)) {
            obj["status"] = static_cast<int>(runtimeState.status);
        } else {
            obj["status"] = 0;
        }

        // 读取当前值
        if (isGpioDigitalInputType(config.type) ||
            config.type == PeripheralType::GPIO_DIGITAL_OUTPUT) {
            // 数字输入/输出类型
            GPIOState state = pm.readPin(config.id);
            if (state != GPIOState::STATE_UNDEFINED) {
                obj["value"] = (state == GPIOState::STATE_HIGH) ? "1" : "0";
            }
        } else if (typeVal == 15 || typeVal == 26) {
            // 模拟输入或 ADC
            if (config.params.adc.conversionEnabled &&
                config.params.adc.signalMode != static_cast<uint8_t>(AnalogSignalMode::RAW)) {
                const float value = pm.readAnalogValue(config.id);
                if (isnan(value) || isinf(value)) return;
                uint8_t decimals = config.params.adc.decimalPlaces;
                if (decimals > 6) decimals = 6;
                char valueBuf[32];
                snprintf(valueBuf, sizeof(valueBuf), "%.*f", decimals, value);
                obj["value"] = valueBuf;
            } else {
                obj["value"] = String(pm.readAnalog(config.id));
            }
        }
    });

    // 复用缓冲：reserve 仅扩容不缩容，已足够大时是 no-op
    _serializeBuffer.reserve(256);
    _serializeBuffer = "";
    serializeJson(doc, _serializeBuffer);
    return _serializeBuffer;
}

bool PeriphExecScheduler::checkNetworkAndProtocolStatus(uint8_t& connectedProtocols) {
    connectedProtocols = 0;

    // 检查 WiFi 连接状态
    if (WiFi.status() != WL_CONNECTED) {
        LOGGER.debug("[PeriphExec] WiFi not connected, skip report");
        return false;
    }

    bool hasConnectedProtocol = false;

    // 获取 ProtocolManager
    auto* fw = FastBeeFramework::getInstance();
    if (!fw) {
        return false;
    }
    auto* protocolMgr = fw->getProtocolManager();
    if (!protocolMgr) {
        return false;
    }

    // 检查 MQTT 连接状态
    MQTTClient* mqtt = protocolMgr->getMQTTClient();
    if (mqtt && mqtt->getIsConnected()) {
        connectedProtocols |= PROTOCOL_MQTT_CONNECTED;
        hasConnectedProtocol = true;
    }

    // 检查 Modbus 状态（使用 bool 方法替代字符串分配）
#if FASTBEE_ENABLE_MODBUS
    auto modbusLease = protocolMgr->acquireModbusHandler(pdMS_TO_TICKS(100));
    ModbusHandler* modbus = modbusLease.get();
    if (modbus && modbus->isRunning()) {
        connectedProtocols |= PROTOCOL_MODBUS_CONNECTED;
        hasConnectedProtocol = true;
    }
#endif

    return hasConnectedProtocol;
}

MQTTClient* PeriphExecScheduler::getMqttClient() {
    auto* fw = FastBeeFramework::getInstance();
    if (fw) {
        auto* pm = fw->getProtocolManager();
        if (pm) return pm->getMQTTClient();
    }
    return nullptr;
}

#endif // FASTBEE_ENABLE_PERIPH_EXEC
