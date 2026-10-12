/**
 * @description Peripheral pin mapping, interrupt, RF, and diagnostics implementation
 * @author kerwincui
 * @copyright FastBee All rights reserved.
 */

#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/ChipConfig.h"
#include "systems/LoggerSystem.h"
#include "PeripheralManagerConfigHelpers.h"
#include <driver/gpio.h>
#include <cstdlib>
#if FASTBEE_ENABLE_NEOPIXEL
#include "peripherals/NeoPixelTransport.h"
#endif

using namespace FastBee::PeripheralManagerConfigDetail;

String PeripheralManager::generateUniqueId(PeripheralType type) {
    static int counter = 0;
    const char* typeName = getPeripheralTypeName(type);
    return String(typeName) + "_" + String(millis()) + "_" + String(counter++);
}

void PeripheralManager::updatePinMapping(const String& id, const PeripheralConfig& config) {
    for (uint8_t i = 0; i < config.configuredPinCount(); i++) {
        const uint8_t pin = config.configuredPinAt(i);
        if (pin != 255) {
            pinToPeripheral[pin] = id;
        }
    }
}

void PeripheralManager::removePinMapping(const String& id) {
    auto it = pinToPeripheral.begin();
    while (it != pinToPeripheral.end()) {
        if (it->second == id) {
            it = pinToPeripheral.erase(it);
        } else {
            ++it;
        }
    }
}

// ========== 引脚验证与冲突检测 ==========

// 获取引脚保留原因（使用 ChipConfig.h 中的定义）
static String getPinReservedReason(uint8_t pin) {
    // 检查是否为保留引脚
    for (uint8_t i = 0; i < CHIP_RESERVED_PIN_COUNT; i++) {
        if (CHIP_RESERVED_PINS[i] == pin) {
            switch (pin) {
                case 0: return "Boot 模式选择引脚，建议保留";
                case 1: return "TX0 调试串口";
                case 3: return "RX0 调试串口";
                case 6: case 7: case 8: case 9: case 10: case 11:
                    return "内部 Flash SPI，禁止使用";
                case 19: case 20:
                    return "USB 接口，建议保留";
                case 26: case 27: case 28: case 29: case 30: case 31: case 32:
                    return "Octal Flash/PSRAM，建议保留";
                default: return "系统保留引脚";
            }
        }
    }
    // 检查是否为输入专用引脚
    for (uint8_t i = 0; i < CHIP_INPUT_ONLY_PIN_COUNT; i++) {
        if (CHIP_INPUT_ONLY_PINS[i] == pin) {
            return "仅支持输入模式";
        }
    }
    return "";
}

bool PeripheralManager::isValidPin(uint8_t pin) const {
    // 使用 ChipConfig.h 中的最大 GPIO 编号
    if (pin > CHIP_MAX_GPIO) return false;
    if (!GPIO_IS_VALID_GPIO(static_cast<gpio_num_t>(pin))) return false;

    // Flash SPI / PSRAM 总线绝对禁区（芯片差异统一由 ChipConfig.h 提供）
    if (chipIsAbsoluteForbiddenPin(pin)) return false;

    return true;
}

// 检查引脚是否为系统保留引脚
bool PeripheralManager::isReservedPin(uint8_t pin) const {
    for (uint8_t i = 0; i < CHIP_RESERVED_PIN_COUNT; i++) {
        if (CHIP_RESERVED_PINS[i] == pin) {
            return true;
        }
    }
    return false;
}

// 检查引脚是否只能用于输入
bool PeripheralManager::isInputOnlyPin(uint8_t pin) const {
    for (uint8_t i = 0; i < CHIP_INPUT_ONLY_PIN_COUNT; i++) {
        if (CHIP_INPUT_ONLY_PINS[i] == pin) {
            return true;
        }
    }
    return false;
}

bool PeripheralManager::checkPinConflict(uint8_t pin, const String& excludeId) const {
#if FASTBEE_ENABLE_NEOPIXEL
    // A rolled-back configuration must not erase a quarantined SDK resource.
    if (fastbeeNeoPixelProjectPinRetained(pin)) return true;
#endif
    // 加固：基于 peripherals 真实数据扫描，避免 pinToPeripheral 缓存不一致导致的
    // “实际未占用但被误报占用”或“已占用但被误报空闲”
    // 语义：只有“启用”的非 Modbus 外设才算占用引脚；禁用的外设让出引脚，
    // 允许新外设复用；启用时再做一次冲突检查
    for (const auto& pair : peripherals) {
        if (!excludeId.isEmpty() && pair.first == excludeId) continue;
        const PeripheralConfig& cfg = pair.second;
        if (!cfg.enabled) continue;
        // Modbus 外设不占用实际 GPIO
        if (cfg.isModbusPeripheral()) continue;
        if (cfg.usesPin(pin)) return true;
    }
    return false;
}

// 完全从 peripherals 真实数据重建 pinToPeripheral 缓存
// 用途：删除/加载配置/检测到残留时保证缓存与真实数据一致
void PeripheralManager::rebuildPinMapping() {
    pinToPeripheral.clear();
    for (const auto& pair : peripherals) {
        const PeripheralConfig& cfg = pair.second;
        if (cfg.isModbusPeripheral()) continue;  // Modbus 不占 GPIO
        for (uint8_t i = 0; i < cfg.configuredPinCount(); i++) {
            const uint8_t pin = cfg.configuredPinAt(i);
            if (pin != 255) {
                pinToPeripheral[pin] = pair.first;
            }
        }
    }
}

// 获取引脚冲突详细信息
String PeripheralManager::getPinConflictInfo(uint8_t pin, const String& excludeId) const {
#if FASTBEE_ENABLE_NEOPIXEL
    if (fastbeeNeoPixelProjectPinRetained(pin))
        return String("GPIO") + String(pin) + " 由 NeoPixel 待清理对象保留，需排障并受控重启";
#endif
    // 检查是否为无效引脚
    if (!isValidPin(pin)) {
        return String("GPIO") + String(pin) + " 不是有效的 GPIO 引脚";
    }

    // 检查系统保留引脚
    String reservedReason = getPinReservedReason(pin);
    if (!reservedReason.isEmpty() && (pin >= 6 && pin <= 11)) {
        return String("GPIO") + String(pin) + ": " + reservedReason;
    }

    // 只有已启用外设才占用资源；直接扫描真实配置，避免缓存中禁用项产生误报。
    for (const auto& pair : peripherals) {
        if ((!excludeId.isEmpty() && pair.first == excludeId) ||
            !pair.second.enabled || pair.second.isModbusPeripheral()) {
            continue;
        }
        if (pair.second.usesPin(pin)) {
            return String("GPIO") + String(pin) + " 已被外设 \"" + pair.second.name +
                   "\" (" + pair.first + ") 使用";
        }
    }

    // 系统保留引脚警告（非错误）
    if (!reservedReason.isEmpty()) {
        return String("警告: GPIO") + String(pin) + " - " + reservedReason;
    }

    return "";  // 无冲突
}

// 验证引脚配置是否与外设类型兼容
bool PeripheralManager::validatePinForType(uint8_t pin, PeripheralType type, String& errorMsg) const {
    if (!isValidPin(pin)) {
        errorMsg = String("GPIO") + String(pin) + " 不是有效的引脚";
        return false;
    }

    // initHardware rejects reserved pins for every physical peripheral. Apply
    // the same policy at validation time so bad configs never persist.
    if (isReservedPin(pin)) {
        errorMsg = String("GPIO") + String(pin) + " 是当前芯片保留引脚";
        return false;
    }

    if (isInputOnlyPin(pin)) {
        // GPIO34-39 只能用于输入类型
        int typeVal = static_cast<int>(type);
        bool isInputType = (type == PeripheralType::GPIO_DIGITAL_INPUT ||
                           type == PeripheralType::GPIO_DIGITAL_INPUT_PULLUP ||
                           type == PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN ||
                           type == PeripheralType::GPIO_ANALOG_INPUT ||
                           type == PeripheralType::ADC ||
                           type == PeripheralType::GPIO_INTERRUPT_RISING ||
                           type == PeripheralType::GPIO_INTERRUPT_FALLING ||
                           type == PeripheralType::GPIO_INTERRUPT_CHANGE ||
                           type == PeripheralType::RADAR_SENSOR ||
                           type == PeripheralType::IR_RECEIVER ||
                           type == PeripheralType::SENSOR_GENERIC ||
                           type == PeripheralType::RF_MODULE);

        if (!isInputType) {
            errorMsg = String("GPIO") + String(pin) + " 只能用于输入模式，不能配置为 " + getPeripheralTypeName(type);
            return false;
        }
    }

    if ((type == PeripheralType::GPIO_ANALOG_INPUT || type == PeripheralType::ADC) &&
        digitalPinToAnalogChannel(pin) < 0) {
        errorMsg = String("GPIO") + String(pin) + " 不支持 ADC 输入";
        return false;
    }

    // DAC 引脚验证
#if CHIP_HAS_DAC
    if (type == PeripheralType::DAC && pin != 25 && pin != 26) {
        errorMsg = "DAC 只能使用 GPIO25 或 GPIO26";
        return false;
    }
#else
    if (type == PeripheralType::DAC) {
        errorMsg = "此芯片不支持 DAC 功能";
        return false;
    }
#endif

    // 触摸引脚验证
#if CHIP_HAS_TOUCH
    if (type == PeripheralType::GPIO_TOUCH) {
        bool isTouchPin = false;
        for (uint8_t i = 0; i < CHIP_TOUCH_PIN_COUNT; i++) {
            if (pin == CHIP_TOUCH_PINS[i]) { isTouchPin = true; break; }
        }
        if (!isTouchPin) {
            errorMsg = String("GPIO") + String(pin) + " 不支持触摸功能";
            return false;
        }
    }
#else
    if (type == PeripheralType::GPIO_TOUCH) {
        errorMsg = "此芯片不支持触摸功能";
        return false;
    }
#endif

    return true;
}

String PeripheralManager::getPinPeripheralId(uint8_t pin) const {
    auto it = pinToPeripheral.find(pin);
    if (it != pinToPeripheral.end()) {
        return it->second;
    }
    return "";
}

bool PeripheralManager::isPinConfigured(uint8_t pin) const {
    return pinToPeripheral.find(pin) != pinToPeripheral.end();
}

std::vector<uint8_t> PeripheralManager::getConfiguredPins() const {
    std::vector<uint8_t> pins;
    for (const auto& pair : pinToPeripheral) {
        pins.push_back(pair.first);
    }
    return pins;
}

void PeripheralManager::printStatus() const {
    LOG_INFO("=== Peripheral Status ===");
    LOG_INFOF("Total peripherals: %d", peripherals.size());

    for (const auto& pair : peripherals) {
        const auto& config = pair.second;
        const auto& state = runtimeStates.find(config.id);

        char buf[128];
        snprintf(buf, sizeof(buf), "  [%s] %s (Type: %s, Enabled: %s, Status: %d)",
                 config.id.c_str(),
                 config.name.c_str(),
                 getPeripheralTypeName(config.type),
                 config.enabled ? "Yes" : "No",
                 state != runtimeStates.end() ? static_cast<int>(state->second.status) : -1);
        LOG_INFO(buf);
    }
}

void PeripheralManager::performMaintenance() {
    retireControlOutputs();
    serviceOrdinaryControlExpiry();
    pollIoExpanders();
    // 处理ISR中断事件队列
    processInterruptQueue();
}

bool PeripheralManager::shouldDispatchIsrEvent(unsigned long nowMs, unsigned long lastDispatchMs,
                                               uint16_t debounceMs) {
    // 无符号减法天然处理 millis() 回绕（lastDispatch 初始为 0，启动初期前几 ms 内
    // 的中断会被合并，可接受；之后每 debounceMs 窗口最多派发一次）
    return (nowMs - lastDispatchMs) >= debounceMs;
}

uint16_t PeripheralManager::isrDebounceMsForType(PeripheralType type) {
    switch (type) {
        // 高速边沿外设：433MHz OOK/EV1527 与红外协议的脉冲宽度在百 us 级，
        // 任何毫秒级合并都会破坏解码，必须逐边沿派发
        case PeripheralType::RF_MODULE:
        case PeripheralType::IR_RECEIVER:
            return 0;
        default:
            // GPIO 中断/雷达等类型默认去抖窗口，抑制机械按键抖动与中断风暴
            return ISR_DEBOUNCE_MS;
    }
}

size_t PeripheralManager::getIsrQueueWaiting() const {
    if (!_isrQueue) return 0;
    return static_cast<size_t>(uxQueueMessagesWaiting(_isrQueue));
}

void PeripheralManager::resetIsrQueueStats() {
    _isrQueueOverruns = 0;
}

// 中断处理
void IRAM_ATTR PeripheralManager::isrHandler(void* arg) {
    // Capture the level at the interrupt edge. Reading it later in loopTask
    // loses pulses shorter than the maintenance interval.
    uint8_t pin = (uint8_t)(uintptr_t)arg;
    GpioIsrEvent event = {
        pin,
        static_cast<uint8_t>(gpio_get_level(static_cast<gpio_num_t>(pin)) != 0)
    };
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (_isrQueue) {
        if (xQueueSendFromISR(_isrQueue, &event, &xHigherPriorityTaskWoken) != pdTRUE) {
            // 队列满：计入溢出统计（旧实现静默丢事件，风暴场景无法诊断）
            _isrQueueOverruns++;
        }
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void PeripheralManager::processInterruptQueue() {
    if (!_isrQueue) return;
    const uint32_t started = millis();
    // Bound even a continuously refilled queue. The time budget is checked
    // between callbacks; application callbacks must themselves return promptly.
    for (uint8_t count = 0; count < ISR_MAX_EVENTS_PER_CYCLE &&
         static_cast<uint32_t>(millis() - started) < ISR_MAINTENANCE_BUDGET_MS; ++count) {
        if (!handleInterrupt()) break;
    }
}

bool PeripheralManager::handleInterrupt() {
    GPIOInterruptCallback callback = nullptr;
    uint8_t pin = 0;
    String id;
    GPIOState state = GPIOState::STATE_UNDEFINED;
    {
        // Admit before dequeueing so lock contention preserves event ordering
        // and cannot lose an event to a concurrently refilled ISR queue.
        RecursiveMutexGuard lock(_mutex, pdMS_TO_TICKS(ISR_STATE_LOCK_TIMEOUT_MS));
        if (!lock.isLocked() || !_isrQueue) return false;
        GpioIsrEvent event;
        if (xQueueReceive(_isrQueue, &event, 0) != pdTRUE) return false;
        pin = event.pin;
        const bool rawLevel = event.rawLevel != 0;
        id = getPinPeripheralId(pin);
        if (id.isEmpty()) return true;

        auto it = peripherals.find(id);
        if (it == peripherals.end()) return true;
        PeripheralConfig& config = it->second;

        // A queued edge cannot resurrect a failed or retired native handler.
        if (isGpioDigitalInputType(config.type) && (!config.enabled ||
            !_managedPinIo.interruptCurrent(config.id.c_str(), pin))) return true;

        // 按类型差异化去抖：高速边沿外设（RF/IR）窗口为 0，逐边沿派发；
        // GPIO 中断类在窗口内合并，避免重复的状态读取与回调触发
        uint16_t window = isGpioInterruptInputType(config.type)
                              ? effectiveDigitalInputDebounceMs(config.params.gpio.debounceMs)
                              : isrDebounceMsForType(config.type);
        if (window > 0 && pin < ISR_TRACKED_PIN_COUNT) {
            unsigned long now = millis();
            if (_hasIsrDispatch[pin] &&
                !shouldDispatchIsrEvent(now, _lastIsrDispatch[pin], window)) {
                return true;
            }
            _lastIsrDispatch[pin] = now;
            _hasIsrDispatch[pin] = true;
        }

        const bool logicalLevel = digitalInputLogicalLevel(
            rawLevel, config.params.gpio.invertLogic);
        state = logicalLevel ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;

        auto rt = runtimeStates.find(id);
        if (rt != runtimeStates.end()) {
            rt->second.state.gpio.currentState = state;
            rt->second.lastActivity = millis();
        }

        if (isGpioInterruptInputType(config.type) &&
            matchesDigitalInterruptEdge(config.type, logicalLevel)) {
            if (_digitalInterruptCount == ISR_QUEUE_SIZE) {
                // Preserve the newest state if the consumer is temporarily
                // delayed; count the dropped oldest event in ISR diagnostics.
                _digitalInterruptHead = (_digitalInterruptHead + 1) % ISR_QUEUE_SIZE;
                _digitalInterruptCount--;
                _isrQueueOverruns++;
            }
            _digitalInterruptEvents[_digitalInterruptTail] = {pin, state};
            _digitalInterruptTail = (_digitalInterruptTail + 1) % ISR_QUEUE_SIZE;
            _digitalInterruptCount++;
        }

        // 快照回调后再释放锁：避免用户回调长时间持锁阻塞 Web/worker 任务
        callback = config.params.gpio.interruptCallback;
    }

    if (callback && state != GPIOState::STATE_UNDEFINED) {
        callback(pin, state);
    }
    return true;
}

bool PeripheralManager::readRfLevel(const String& id, bool& level) {
    level = false;
    RecursiveMutexGuard lock(_mutex);
    PeripheralConfig* config = getPeripheral(id);
    if (!config || !config->enabled || config->type != PeripheralType::RF_MODULE ||
        config->params.rf.mode != RF_MODE_RX) {
        return false;
    }
    if (config->pinCount < 1 || config->pins[0] == 255) {
        return false;
    }
    bool physicalHigh = digitalRead(config->pins[0]) == HIGH;
    level = config->params.rf.activeHigh ? physicalHigh : !physicalHigh;
    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
        runtimeStates[id].state.gpio.currentState = level ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
    }
    return true;
}
