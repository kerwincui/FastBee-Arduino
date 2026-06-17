/**
 * @description: 外设接口管理器实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-03-03
 */

#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/FeatureFlags.h"
#include "core/ChipConfig.h"
#include "utils/FileUtils.h"
#include "systems/LoggerSystem.h"
#include <driver/gpio.h>
// WS2812 NeoPixel 支持：使用 Adafruit_NeoPixel 库（跨芯片兼容）
#if FASTBEE_ENABLE_NEOPIXEL
#include <Adafruit_NeoPixel.h>
#endif
#if FASTBEE_ENABLE_LCD
#include "peripherals/LCDManager.h"
#endif
#if FASTBEE_ENABLE_SEVEN_SEGMENT
#include "peripherals/SevenSegmentDriver.h"
#endif
#include <Wire.h>
#include <SPI.h>
#include <new>
#include <cstdlib>
#include <freertos/queue.h>

// 静态成员定义：ISR 中断事件队列
QueueHandle_t PeripheralManager::_isrQueue = nullptr;

namespace {
constexpr uint16_t STEPPER_DEFAULT_STEPS_PER_REV = 2048;
constexpr uint16_t STEPPER_DEFAULT_RPM = 8;
constexpr uint16_t STEPPER_MIN_RPM = 1;
constexpr uint16_t STEPPER_MAX_RPM = 30;
constexpr uint16_t NEOPIXEL_DEFAULT_COUNT = 1;
constexpr uint16_t NEOPIXEL_MAX_COUNT = FASTBEE_NEOPIXEL_MAX_LEDS;
constexpr uint8_t NEOPIXEL_DEFAULT_BRIGHTNESS = 64;
constexpr uint8_t UART_PORT_UNASSIGNED = 255;
constexpr uint8_t RF_MODE_TX = 0;
constexpr uint8_t RF_MODE_RX = 1;
constexpr uint16_t RF_DEFAULT_PULSE_WIDTH_US = 350;
constexpr uint16_t RF_MIN_PULSE_WIDTH_US = 100;
constexpr uint16_t RF_MAX_PULSE_WIDTH_US = 2000;
constexpr uint8_t RF_DEFAULT_REPEAT = 8;
constexpr uint8_t RF_MIN_REPEAT = 1;
constexpr uint8_t RF_MAX_REPEAT = 20;
constexpr uint8_t RF_DEFAULT_BIT_LENGTH = 24;
constexpr uint8_t RF_MIN_BIT_LENGTH = 1;
constexpr uint8_t RF_MAX_BIT_LENGTH = 32;
constexpr uint8_t RADAR_MODE_DIGITAL = 0;
constexpr uint16_t RADAR_DEFAULT_DEBOUNCE_MS = 50;
constexpr uint16_t RADAR_DEFAULT_HOLD_MS = 2000;

struct NeoPixelRgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

uint16_t clampStepperSpeed(uint16_t rpm) {
    if (rpm < STEPPER_MIN_RPM) return STEPPER_MIN_RPM;
    if (rpm > STEPPER_MAX_RPM) return STEPPER_MAX_RPM;
    return rpm;
}

uint16_t clampRfPulseWidth(uint16_t pulseWidth) {
    if (pulseWidth < RF_MIN_PULSE_WIDTH_US) return RF_DEFAULT_PULSE_WIDTH_US;
    if (pulseWidth > RF_MAX_PULSE_WIDTH_US) return RF_MAX_PULSE_WIDTH_US;
    return pulseWidth;
}

uint8_t clampRfRepeat(uint8_t repeat) {
    if (repeat < RF_MIN_REPEAT) return RF_DEFAULT_REPEAT;
    if (repeat > RF_MAX_REPEAT) return RF_MAX_REPEAT;
    return repeat;
}

uint8_t clampRfBitLength(uint8_t bitLength) {
    if (bitLength < RF_MIN_BIT_LENGTH) return RF_DEFAULT_BIT_LENGTH;
    if (bitLength > RF_MAX_BIT_LENGTH) return RF_MAX_BIT_LENGTH;
    return bitLength;
}

bool isPrintablePayload(const uint8_t* data, size_t len) {
    if (!data || len == 0 || len > 64) return false;
    for (size_t i = 0; i < len; i++) {
        if (data[i] < 32 || data[i] > 126) return false;
    }
    return true;
}

bool parseRfCodeText(String text, uint32_t& code, uint8_t& inferredBits) {
    text.trim();
    text.replace(" ", "");
    text.replace("_", "");
    if (text.isEmpty()) return false;

    inferredBits = 0;
    if (text.startsWith("0b") || text.startsWith("0B")) {
        text.remove(0, 2);
        if (text.length() == 0 || text.length() > RF_MAX_BIT_LENGTH) return false;
        uint32_t value = 0;
        for (size_t i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c != '0' && c != '1') return false;
            value = (value << 1) | (c == '1' ? 1UL : 0UL);
        }
        code = value;
        inferredBits = (uint8_t)text.length();
        return true;
    }

    int base = 10;
    if (text.startsWith("0x") || text.startsWith("0X")) {
        base = 16;
        text.remove(0, 2);
    } else {
        for (size_t i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                base = 16;
                break;
            }
        }
    }

    if (text.isEmpty()) return false;
    char* endPtr = nullptr;
    unsigned long parsed = strtoul(text.c_str(), &endPtr, base);
    if (!endPtr || *endPtr != '\0') return false;
    code = (uint32_t)parsed;
    return true;
}

uint32_t stepperIntervalMs(uint16_t rpm, uint16_t stepsPerRev) {
    rpm = clampStepperSpeed(rpm == 0 ? STEPPER_DEFAULT_RPM : rpm);
    if (stepsPerRev == 0) stepsPerRev = STEPPER_DEFAULT_STEPS_PER_REV;
    uint32_t interval = 60000UL / ((uint32_t)rpm * stepsPerRev);
    if (interval < 2) interval = 2;
    if (interval > 1000) interval = 1000;
    return interval;
}

void writeStepperPhase(const PeripheralConfig& config, uint8_t phase) {
    static const uint8_t HALF_STEP_SEQ[8][4] = {
        {1, 0, 0, 0},
        {1, 1, 0, 0},
        {0, 1, 0, 0},
        {0, 1, 1, 0},
        {0, 0, 1, 0},
        {0, 0, 1, 1},
        {0, 0, 0, 1},
        {1, 0, 0, 1}
    };
    for (uint8_t i = 0; i < 4 && i < config.pinCount; i++) {
        digitalWrite(config.pins[i], HALF_STEP_SEQ[phase & 0x07][i] ? HIGH : LOW);
    }
}

void releaseStepperCoils(const PeripheralConfig& config) {
    for (uint8_t i = 0; i < 4 && i < config.pinCount; i++) {
        digitalWrite(config.pins[i], LOW);
    }
}

HardwareSerial* serialForPort(uint8_t port) {
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
    // ESP32-S3: Serial is HWCDC (USB), Serial0 is UART0
    if (port == 0) return &Serial0;
#else
    if (port == 0) return &Serial;
#endif
#if CHIP_UART_COUNT >= 2
    if (port == 1) return &Serial1;
#endif
#if CHIP_UART_COUNT >= 3
    if (port == 2) return &Serial2;
#endif
    return nullptr;
}

uint32_t uartSerialConfig(const PeripheralConfig& config) {
    uint8_t parity = config.params.uart.parity;
    uint8_t stopBits = config.params.uart.stopBits;
    if (parity == 1) return stopBits == 2 ? SERIAL_8O2 : SERIAL_8O1;
    if (parity == 2) return stopBits == 2 ? SERIAL_8E2 : SERIAL_8E1;
    return stopBits == 2 ? SERIAL_8N2 : SERIAL_8N1;
}

bool isUsbSerialPins(uint8_t rxPin, uint8_t txPin) {
    // Normal order is RX,TX. The legacy default stored UART0 as TX,RX, so accept both.
    return (rxPin == 3 && txPin == 1) || (rxPin == 1 && txPin == 3);
}

bool isProtocolOwnedUartId(const String& id) {
    String lowered = id;
    lowered.toLowerCase();
    return lowered.startsWith("modbus") || lowered.startsWith("rs485");
}

struct ModbusMotorLimitResult {
    bool enabled = false;
    bool allowed = true;
    bool isMove = false;
    uint16_t pulse = 0;
    int32_t nextPosition = 0;
};

ModbusMotorLimitResult evaluateModbusMotorLimit(const PeripheralConfig& config, uint16_t regAddr, uint16_t value) {
    ModbusMotorLimitResult r;
    r.nextPosition = config.params.modbus.motorCurrentPosition;
    if (config.params.modbus.deviceType != 3) return r;
    if (config.params.modbus.motorMaxPosition <= config.params.modbus.motorMinPosition) return r;

    const bool forward = (regAddr == config.params.modbus.motorRegs[0]);
    const bool reverse = (regAddr == config.params.modbus.motorRegs[1]);
    if (!forward && !reverse) return r;

    r.enabled = true;
    r.isMove = true;
    int32_t desiredPulse = value > 1 ? value : 0;
    if (desiredPulse <= 0 && config.params.modbus.motorLastPulse > 0) {
        desiredPulse = config.params.modbus.motorLastPulse;
    }
    if (desiredPulse <= 0 && config.params.modbus.motorMoveStep > 0) {
        desiredPulse = config.params.modbus.motorMoveStep;
    }
    if (desiredPulse <= 0) {
        r.allowed = false;
        return r;
    }

    int32_t room = forward
        ? (config.params.modbus.motorMaxPosition - config.params.modbus.motorCurrentPosition)
        : (config.params.modbus.motorCurrentPosition - config.params.modbus.motorMinPosition);
    if (room <= 0) {
        r.allowed = false;
        return r;
    }

    if (desiredPulse > room) desiredPulse = room;
    if (desiredPulse > 65535) desiredPulse = 65535;
    r.pulse = static_cast<uint16_t>(desiredPulse);
    r.nextPosition = forward
        ? config.params.modbus.motorCurrentPosition + desiredPulse
        : config.params.modbus.motorCurrentPosition - desiredPulse;
    return r;
}

void stepperTickerCallback(PeripheralManager::StepperTickerData* data) {
    if (!data || !data->mgr || !data->running || data->direction == 0) return;
    SemaphoreHandle_t mtx = data->mgr->getMutex();
    if (!mtx || xSemaphoreTakeRecursive(mtx, 0) != pdTRUE) return;

    PeripheralConfig* config = data->mgr->getPeripheral(data->id);
    if (config && config->enabled && config->type == PeripheralType::STEPPER_MOTOR && config->pinCount >= 4) {
        data->phase = (data->direction > 0) ? ((data->phase + 1) & 0x07) : ((data->phase + 7) & 0x07);
        writeStepperPhase(*config, data->phase);
        auto* state = data->mgr->getRuntimeState(data->id);
        if (state) {
            state->status = PeripheralStatus::PERIPHERAL_RUNNING;
            state->lastActivity = millis();
        }
    }

    xSemaphoreGiveRecursive(mtx);
}

uint16_t clampNeoPixelCount(uint16_t count) {
    if (count == 0) return NEOPIXEL_DEFAULT_COUNT;
    if (count > NEOPIXEL_MAX_COUNT) return NEOPIXEL_MAX_COUNT;
    return count;
}

uint8_t clampNeoPixelBrightness(uint16_t brightness) {
    if (brightness > 255) return 255;
    return static_cast<uint8_t>(brightness);
}

bool parseNeoPixelColor(const String& raw, NeoPixelRgb& color) {
    String value = raw;
    value.trim();
    value.toLowerCase();
    if (value.isEmpty()) return false;

    if (value == "red" || value == "r") {
        color = {255, 0, 0};
        return true;
    }
    if (value == "orange") {
        color = {255, 96, 0};
        return true;
    }
    if (value == "yellow") {
        color = {255, 220, 0};
        return true;
    }
    if (value == "green" || value == "g") {
        color = {0, 255, 0};
        return true;
    }
    if (value == "cyan") {
        color = {0, 180, 180};
        return true;
    }
    if (value == "blue" || value == "b") {
        color = {0, 0, 255};
        return true;
    }
    if (value == "purple" || value == "violet") {
        color = {128, 0, 255};
        return true;
    }
    if (value == "white" || value == "w") {
        color = {255, 255, 255};
        return true;
    }
    if (value == "off" || value == "black" || value == "0") {
        color = {0, 0, 0};
        return true;
    }

    int comma1 = value.indexOf(',');
    if (comma1 > 0) {
        int comma2 = value.indexOf(',', comma1 + 1);
        if (comma2 > comma1) {
            int r = value.substring(0, comma1).toInt();
            int g = value.substring(comma1 + 1, comma2).toInt();
            int b = value.substring(comma2 + 1).toInt();
            color = {
                static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r)),
                static_cast<uint8_t>(g < 0 ? 0 : (g > 255 ? 255 : g)),
                static_cast<uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b))
            };
            return true;
        }
    }

    if (value.startsWith("#")) value = value.substring(1);
    if (value.length() == 3) {
        String expanded;
        expanded.reserve(6);
        for (uint8_t i = 0; i < 3; i++) {
            expanded += value[i];
            expanded += value[i];
        }
        value = expanded;
    }
    if (value.length() == 6) {
        char* end = nullptr;
        uint32_t rgb = strtoul(value.c_str(), &end, 16);
        if (end && *end == '\0') {
            color = {
                static_cast<uint8_t>((rgb >> 16) & 0xFF),
                static_cast<uint8_t>((rgb >> 8) & 0xFF),
                static_cast<uint8_t>(rgb & 0xFF)
            };
            return true;
        }
    }

    return false;
}

NeoPixelRgb rainbowColorAt(uint8_t index) {
    static const NeoPixelRgb COLORS[] = {
        {255, 0, 0},     // 赤
        {255, 96, 0},    // 橙
        {255, 220, 0},   // 黄
        {0, 220, 0},     // 绿
        {0, 180, 180},   // 青
        {0, 0, 255},     // 蓝
        {128, 0, 255}    // 紫
    };
    return COLORS[index % (sizeof(COLORS) / sizeof(COLORS[0]))];
}

NeoPixelRgb applyNeoPixelBrightness(NeoPixelRgb color, uint8_t brightness) {
    color.r = static_cast<uint8_t>((static_cast<uint16_t>(color.r) * brightness) / 255);
    color.g = static_cast<uint8_t>((static_cast<uint16_t>(color.g) * brightness) / 255);
    color.b = static_cast<uint8_t>((static_cast<uint16_t>(color.b) * brightness) / 255);
    return color;
}

// NeoPixel 全局实例管理（懒加载，节省内存）
#if FASTBEE_ENABLE_NEOPIXEL
static Adafruit_NeoPixel* g_neopixelInstance = nullptr;
static uint8_t g_neopixelPin = 255;
static uint16_t g_neopixelCount = 0;

Adafruit_NeoPixel* getNeoPixelInstance(uint8_t pin, uint16_t count) {
    // 如果已初始化且参数相同，直接返回
    if (g_neopixelInstance && g_neopixelPin == pin && g_neopixelCount == count) {
        return g_neopixelInstance;
    }

    // 如果参数不同，释放旧实例
    if (g_neopixelInstance) {
        delete g_neopixelInstance;
        g_neopixelInstance = nullptr;
    }

    // 创建新实例
    g_neopixelInstance = new (std::nothrow) Adafruit_NeoPixel(count, pin, NEO_GRB + NEO_KHZ800);
    if (g_neopixelInstance) {
        g_neopixelInstance->begin();
        g_neopixelInstance->show(); // 初始化所有灯珠为关闭状态
        g_neopixelPin = pin;
        g_neopixelCount = count;
    }

    return g_neopixelInstance;
}
#endif

bool writeNeoPixelSolid(uint8_t pin, uint16_t count, NeoPixelRgb color) {
#if FASTBEE_ENABLE_NEOPIXEL
    count = clampNeoPixelCount(count);

    // 检查内存
    if (ESP.getFreeHeap() < (count * 3 + 2048)) {
        LOG_WARNINGF("Peripheral Manager: NeoPixel heap low (need=%u free=%u)",
                     static_cast<unsigned int>(count * 3),
                     static_cast<unsigned int>(ESP.getFreeHeap()));
        return false;
    }

    Adafruit_NeoPixel* strip = getNeoPixelInstance(pin, count);
    if (!strip) {
        LOG_WARNING("Peripheral Manager: Failed to create NeoPixel instance");
        return false;
    }

    // 设置所有灯珠颜色
    uint32_t neoColor = strip->Color(color.r, color.g, color.b);
    for (uint16_t i = 0; i < count; i++) {
        strip->setPixelColor(i, neoColor);
    }
    strip->setBrightness(clampNeoPixelBrightness(NEOPIXEL_DEFAULT_BRIGHTNESS));
    strip->show();

    return true;
#else
    (void)pin;
    (void)count;
    (void)color;
    return false;
#endif
}
}

// 单例实现
PeripheralManager& PeripheralManager::getInstance() {
    static PeripheralManager instance;
    return instance;
}

// 初始化
bool PeripheralManager::initialize() {
    LOG_INFO("Peripheral Manager: Initializing...");

    // 创建递归互斥量（支持同一任务嵌套加锁，如 togglePin → readPin → writePin）
    _mutex = xSemaphoreCreateRecursiveMutex();

    // 创建ISR中断事件队列（pin号从ISR传递到主循环处理）
    _isrQueue = xQueueCreate(ISR_QUEUE_SIZE, sizeof(uint8_t));

    // 加载配置
    if (loadConfiguration()) {
        LOG_INFO("Peripheral Manager: Configuration loaded successfully");

        // 初始化所有启用的外设
        initAllEnabledPeripherals();

        LOG_INFO("Peripheral Manager: Initialization complete");
        return true;
    }

    LOG_WARNING("Peripheral Manager: Failed to load configuration, starting with empty config");
    return true;
}

// ========== 外设管理（增删改查） ==========

bool PeripheralManager::addPeripheral(const PeripheralConfig& config) {
    String errorMsg;
    return addPeripheral(config, errorMsg);
}

bool PeripheralManager::addPeripheral(const PeripheralConfig& config, String& errorMsg) {
    // 验证配置
    if (!validateConfig(config, errorMsg)) {
        LOG_ERRORF("Peripheral Manager: Invalid config - %s", errorMsg.c_str());
        return false;
    }

    // 检查ID是否已存在
    if (hasPeripheral(config.id)) {
        errorMsg = String("外设 ID '") + config.id + "' 已存在";
        LOG_ERRORF("Peripheral Manager: Peripheral with ID '%s' already exists", config.id.c_str());
        return false;
    }

    // 检查引脚冲突（Modbus 外设不使用 GPIO 引脚，设备事件虚拟外设无引脚，跳过此检查）
    // 加固：传入 excludeId=config.id 防止同 ID 自冲突；冲突时报告实际占用者便于定位
    if (peripherals.size() >= FastBee::ResourceProfile::MAX_PERIPHERALS) {
        errorMsg = String("Peripheral limit reached for profile ") +
                   FastBee::ResourceProfile::NAME + " (max " +
                   String(FastBee::ResourceProfile::MAX_PERIPHERALS) + ")";
        LOG_ERRORF("Peripheral Manager: %s", errorMsg.c_str());
        return false;
    }
    if (!config.isModbusPeripheral() && !isDeviceEventType(config.type)) {
        for (int i = 0; i < config.pinCount && i < 8; i++) {
            if (config.pins[i] != 255 && checkPinConflict(config.pins[i], config.id)) {
                // 基于真实数据查找占用者（仅启用的外设算占用，与 checkPinConflict 语义一致）
                String owner;
                for (const auto& p : peripherals) {
                    if (p.first == config.id) continue;
                    if (!p.second.enabled) continue;
                    if (p.second.isModbusPeripheral()) continue;
                    for (uint8_t k = 0; k < p.second.pinCount && k < 8; k++) {
                        if (p.second.pins[k] == config.pins[i]) { owner = p.first; break; }
                    }
                    if (!owner.isEmpty()) break;
                }
                LOG_ERRORF("Peripheral Manager: Pin %d is already in use by '%s'",
                           config.pins[i], owner.isEmpty() ? "<stale-mapping>" : owner.c_str());
                // 如果实际找不到占用者，说明是缓存残留，自动修复后重试一次
                if (owner.isEmpty()) {
                    LOG_WARNING("Peripheral Manager: stale pin mapping detected, rebuilding cache");
                    rebuildPinMapping();
                    // 重建后再检一次，如果真的不占用，放行
                    if (!checkPinConflict(config.pins[i], config.id)) {
                        continue;
                    }
                }
                errorMsg = String("引脚 ") + config.pins[i] + " 已被外设 '" +
                           (owner.isEmpty() ? "<未知>" : owner.c_str()) + "' 占用";
                return false;
            }
        }
    }

    // 添加外设
    peripherals[config.id] = config;

    // 初始化运行时状态
    PeripheralRuntimeState state;
    state.id = config.id;
    state.status = config.enabled ? PeripheralStatus::PERIPHERAL_ENABLED : PeripheralStatus::PERIPHERAL_DISABLED;
    runtimeStates[config.id] = state;

    // 更新引脚映射
    updatePinMapping(config.id, config);

    LOG_INFOF("Peripheral Manager: Added peripheral '%s' (%s)",
              config.name.c_str(), config.id.c_str());

    // 如果启用，初始化硬件
    if (config.enabled) {
        initHardware(config.id);
    }

    return true;
}

bool PeripheralManager::updatePeripheral(const String& id, const PeripheralConfig& config) {
    if (!hasPeripheral(id)) {
        LOG_ERRORF("Peripheral Manager: Peripheral '%s' not found", id.c_str());
        return false;
    }

    // 如果ID改变，需要特殊处理
    if (id != config.id && !config.id.isEmpty()) {
        // 删除旧的
        removePeripheral(id);
        // 添加新的
        return addPeripheral(config);
    }

    // 先禁用并释放硬件
    bool wasEnabled = isPeripheralEnabled(id);
    if (wasEnabled) {
        deinitHardware(id);
    }

    // 移除旧引脚映射
    removePinMapping(id);

    // 更新配置
    peripherals[id] = config;

    // 更新引脚映射
    updatePinMapping(id, config);

    // 更新运行时状态
    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].status = config.enabled ? PeripheralStatus::PERIPHERAL_ENABLED : PeripheralStatus::PERIPHERAL_DISABLED;
    }

    LOG_INFOF("Peripheral Manager: Updated peripheral '%s'", id.c_str());

    // 如果之前是启用的，重新初始化
    if (wasEnabled && config.enabled) {
        initHardware(id);
    }

    return true;
}

bool PeripheralManager::removePeripheral(const String& id) {
    if (!hasPeripheral(id)) {
        return false;
    }

    // 先禁用并释放硬件
    deinitHardware(id);

    // 移除运行时状态
    runtimeStates.erase(id);

    // 移除配置
    peripherals.erase(id);

    // 加固：基于最新 peripherals 数据完全重建引脚映射缓存，
    // 避免 removePinMapping 在多外设共享同引脚（缓存被覆盖）场景下漏删或误删
    rebuildPinMapping();

    LOG_INFOF("Peripheral Manager: Removed peripheral '%s'", id.c_str());
    return true;
}

PeripheralConfig* PeripheralManager::getPeripheral(const String& id) {
    auto it = peripherals.find(id);
    if (it != peripherals.end()) {
        return &(it->second);
    }
    return nullptr;
}

const PeripheralConfig* PeripheralManager::getPeripheral(const String& id) const {
    auto it = peripherals.find(id);
    if (it != peripherals.end()) {
        return &(it->second);
    }
    return nullptr;
}

std::vector<PeripheralConfig> PeripheralManager::getPeripheralsByType(PeripheralType type) const {
    std::vector<PeripheralConfig> result;
    for (const auto& pair : peripherals) {
        if (pair.second.type == type) {
            result.push_back(pair.second);
        }
    }
    return result;
}

std::vector<PeripheralConfig> PeripheralManager::getPeripheralsByCategory(PeripheralCategory category) const {
    std::vector<PeripheralConfig> result;
    for (const auto& pair : peripherals) {
        if (getPeripheralCategory(pair.second.type) == category) {
            result.push_back(pair.second);
        }
    }
    return result;
}

std::vector<PeripheralConfig> PeripheralManager::getAllPeripherals() const {
    std::vector<PeripheralConfig> result;
    for (const auto& pair : peripherals) {
        result.push_back(pair.second);
    }
    return result;
}

void PeripheralManager::forEachPeripheral(std::function<void(const PeripheralConfig&)> callback) const {
    for (const auto& pair : peripherals) {
        callback(pair.second);
    }
}

bool PeripheralManager::hasPeripheral(const String& id) const {
    return peripherals.find(id) != peripherals.end();
}

// ========== 外设操作 ==========

bool PeripheralManager::enablePeripheral(const String& id) {
    auto config = getPeripheral(id);
    if (!config) return false;

    // 启用前检查引脚冲突（禁用状态下允许共享引脚定义，此处重新校验）
    if (!config->isModbusPeripheral()) {
        for (int i = 0; i < config->pinCount && i < 8; i++) {
            if (config->pins[i] != 255 && checkPinConflict(config->pins[i], id)) {
                LOG_ERRORF("Peripheral Manager: Cannot enable '%s', pin %d is in use by another enabled peripheral",
                           id.c_str(), config->pins[i]);
                lastEnableError = "Pin ";
                lastEnableError += String(config->pins[i]);
                lastEnableError += " conflict with another peripheral";
                return false;
            }
        }
    }

    config->enabled = true;

    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].status = PeripheralStatus::PERIPHERAL_ENABLED;
    }

    if (!initHardware(id)) {
        lastEnableError = "Hardware init failed for '" + id + "'";
        config->enabled = false;
        return false;
    }
    lastEnableError = "";
    return true;
}

bool PeripheralManager::disablePeripheral(const String& id) {
    auto config = getPeripheral(id);
    if (!config) return false;

    config->enabled = false;

    return deinitHardware(id);
}

bool PeripheralManager::isPeripheralEnabled(const String& id) const {
    auto config = getPeripheral(id);
    return config ? config->enabled : false;
}

PeripheralStatus PeripheralManager::getPeripheralStatus(const String& id) const {
    auto it = runtimeStates.find(id);
    if (it != runtimeStates.end()) {
        return it->second.status;
    }
    return PeripheralStatus::PERIPHERAL_DISABLED;
}

PeripheralRuntimeState* PeripheralManager::getRuntimeState(const String& id) {
    auto it = runtimeStates.find(id);
    if (it != runtimeStates.end()) {
        return &(it->second);
    }
    return nullptr;
}

// ========== 硬件初始化 ==========

bool PeripheralManager::initHardware(const String& id) {
    auto config = getPeripheral(id);
    if (!config) return false;

    if (!config->enabled) {
        LOG_WARNINGF("Peripheral Manager: Cannot init disabled peripheral '%s'", id.c_str());
        return false;
    }

    bool success = setupHardware(*config);

    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].status = success ? PeripheralStatus::PERIPHERAL_INITIALIZED : PeripheralStatus::PERIPHERAL_ERROR;
        if (success) {
            runtimeStates[id].initTime = millis();
        }
    }

    return success;
}

bool PeripheralManager::deinitHardware(const String& id) {
    auto config = getPeripheral(id);
    if (!config) return false;

    bool success = teardownHardware(*config);

    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].status = PeripheralStatus::PERIPHERAL_DISABLED;
    }

    return success;
}

bool PeripheralManager::initAllEnabledPeripherals() {
    int successCount = 0;
    int failCount = 0;

    for (auto& pair : peripherals) {
        if (pair.second.enabled) {
            if (initHardware(pair.first)) {
                successCount++;
            } else {
                failCount++;
            }
        }
    }

    LOG_INFOF("Peripheral Manager: Initialized %d peripherals, %d failed",
              successCount, failCount);
    return failCount == 0;
}

// ========== GPIO兼容层 ==========

bool PeripheralManager::configurePin(uint8_t pin, PeripheralType type) {
    if (!isValidPin(pin)) return false;

    // 生成唯一ID
    String id = generateUniqueId(type);

    PeripheralConfig config;
    config.id = id;
    config.name = "Pin" + String(pin);
    config.type = type;
    config.enabled = true;
    config.pinCount = 1;
    config.pins[0] = pin;

    // 根据类型设置默认参数
    if (type == PeripheralType::GPIO_PWM_OUTPUT) {
        config.params.gpio.pwmChannel = pin % 16;  // 使用引脚号模16作为通道
        config.params.gpio.pwmFrequency = 1000;
        config.params.gpio.pwmResolution = 8;
    }

    return addPeripheral(config);
}

bool PeripheralManager::configurePin(const PeripheralConfig& config) {
    return addPeripheral(config);
}

GPIOState PeripheralManager::readPin(uint8_t pin) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return GPIOState::STATE_UNDEFINED;
    return readPin(id);
}

GPIOState PeripheralManager::readPin(const String& peripheralId) {
    RecursiveMutexGuard lock(_mutex);
    auto config = getPeripheral(peripheralId);
    if (!config) return GPIOState::STATE_UNDEFINED;

    // Modbus 外设：返回缓存的状态（无法实时读取远端设备）
    if (config->isModbusPeripheral()) {
        if (runtimeStates.find(peripheralId) != runtimeStates.end()) {
            return runtimeStates[peripheralId].state.gpio.currentState;
        }
        return GPIOState::STATE_UNDEFINED;
    }

    if (config->type == PeripheralType::RADAR_SENSOR) {
        bool detected = false;
        if (!readRadarState(peripheralId, detected)) return GPIOState::STATE_UNDEFINED;
        return detected ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
    }

    if (config->type == PeripheralType::RF_MODULE && config->params.rf.mode == RF_MODE_RX) {
        bool level = false;
        if (!readRfLevel(peripheralId, level)) return GPIOState::STATE_UNDEFINED;
        return level ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
    }

    if (!config->isGPIOPeripheral()) {
        return GPIOState::STATE_UNDEFINED;
    }

    uint8_t pin = config->getPrimaryPin();
    if (pin == 255) return GPIOState::STATE_UNDEFINED;

    GPIOState state;

    switch (config->type) {
        case PeripheralType::GPIO_DIGITAL_INPUT:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLUP:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN:
            state = digitalRead(pin) ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
            break;

        case PeripheralType::GPIO_ANALOG_INPUT:
            state = analogRead(pin) > 2048 ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
            break;

        default:
            // 对于输出类型，返回最后设置的状态
            if (runtimeStates.find(peripheralId) != runtimeStates.end()) {
                state = runtimeStates[peripheralId].state.gpio.currentState;
            } else {
                state = GPIOState::STATE_UNDEFINED;
            }
            break;
    }

    // 返回物理状态（电平反转已迁移至外设执行）
    return state;
}

bool PeripheralManager::writePin(uint8_t pin, GPIOState state) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return false;
    return writePin(id, state);
}

bool PeripheralManager::writePin(const String& peripheralId, GPIOState state) {
    RecursiveMutexGuard lock(_mutex);
    auto config = getPeripheral(peripheralId);
    if (!config) return false;

    // Modbus 外设：通过委托回调写入
    if (config->isModbusPeripheral()) {
        return writeModbusPin(peripheralId, *config, state);
    }

    if (!config->isGPIOPeripheral()) {
        return false;
    }

    uint8_t pin = config->getPrimaryPin();
    if (pin == 255) return false;

    // 直接写入物理状态（电平反转已迁移至外设执行层处理）
    GPIOState physicalState = state;

    bool success = true;

    switch (config->type) {
        case PeripheralType::GPIO_DIGITAL_OUTPUT:
            digitalWrite(pin, physicalState == GPIOState::STATE_HIGH ? HIGH : LOW);
            break;

        case PeripheralType::GPIO_PWM_OUTPUT:
            if (state == GPIOState::STATE_HIGH) {
                ledcWrite(config->params.gpio.pwmChannel, (1 << config->params.gpio.pwmResolution) - 1);
            } else {
                ledcWrite(config->params.gpio.pwmChannel, 0);
            }
            break;

        case PeripheralType::GPIO_ANALOG_OUTPUT:
            ledcWrite(config->params.gpio.pwmChannel,
                physicalState == GPIOState::STATE_HIGH
                    ? (uint32_t)((1U << config->params.gpio.pwmResolution) - 1) : 0U);
            break;

        default:
            success = false;
            break;
    }

    if (success && runtimeStates.find(peripheralId) != runtimeStates.end()) {
        runtimeStates[peripheralId].state.gpio.currentState = state;
        runtimeStates[peripheralId].lastActivity = millis();
    }

    return success;
}

bool PeripheralManager::togglePin(uint8_t pin) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return false;
    return togglePin(id);
}

bool PeripheralManager::togglePin(const String& peripheralId) {
    RecursiveMutexGuard lock(_mutex);
    GPIOState current = readPin(peripheralId);
    if (current == GPIOState::STATE_UNDEFINED) return false;
    return writePin(peripheralId, current == GPIOState::STATE_HIGH
                                ? GPIOState::STATE_LOW : GPIOState::STATE_HIGH);
}

bool PeripheralManager::writePWM(uint8_t pin, uint32_t dutyCycle) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return false;
    return writePWM(id, dutyCycle);
}

bool PeripheralManager::writePWM(const String& peripheralId, uint32_t dutyCycle) {
    RecursiveMutexGuard lock(_mutex);
    auto config = getPeripheral(peripheralId);
    if (!config) return false;

    // Modbus PWM 外设：通过委托回调写入寄存器
    if (config->isModbusPeripheral()) {
        return writeModbusPWM(peripheralId, *config, dutyCycle);
    }

    if (config->type != PeripheralType::GPIO_PWM_OUTPUT &&
        config->type != PeripheralType::GPIO_ANALOG_OUTPUT &&
        config->type != PeripheralType::PWM_SERVO) {
        return false;
    }

    uint32_t maxVal = (1U << config->params.gpio.pwmResolution) - 1;
    ledcWrite(config->params.gpio.pwmChannel, dutyCycle > maxVal ? maxVal : dutyCycle);

    return true;
}

uint16_t PeripheralManager::readAnalog(uint8_t pin) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return 0;
    return readAnalog(id);
}

uint16_t PeripheralManager::readAnalog(const String& peripheralId) {
    auto config = getPeripheral(peripheralId);
    if (!config) return 0;

    if (config->type != PeripheralType::GPIO_ANALOG_INPUT &&
        config->type != PeripheralType::ADC) {
        return 0;
    }

    uint8_t pin = config->getPrimaryPin();
    if (pin == 255) return 0;

    return (uint16_t)analogRead(pin);
}

// ========== 持久化 ==========

bool PeripheralManager::saveConfiguration() {
    FastBeeJsonDocLarge doc;
    JsonArray periphs = doc["peripherals"].to<JsonArray>();

    for (const auto& pair : peripherals) {
        const PeripheralConfig& config = pair.second;

        // Modbus 外设由 protocol.json 管理，不保存到 peripherals.json
        if (config.isModbusPeripheral()) continue;

        JsonObject obj = periphs.add<JsonObject>();
        obj["id"] = config.id;
        obj["name"] = config.name;
        obj["type"] = static_cast<int>(config.type);
        obj["enabled"] = config.enabled;

        // 引脚配置
        JsonArray pins = obj["pins"].to<JsonArray>();
        for (int i = 0; i < config.pinCount && i < 8; i++) {
            if (config.pins[i] != 255) {
                pins.add(config.pins[i]);
            }
        }

        // 类型特定参数
        JsonObject params = obj["params"].to<JsonObject>();

        if (config.type == PeripheralType::UART) {
            params["baudRate"] = config.params.uart.baudRate;
            params["dataBits"] = config.params.uart.dataBits;
            params["stopBits"] = config.params.uart.stopBits;
            params["parity"] = config.params.uart.parity;
        }
        else if (config.type == PeripheralType::I2C) {
            params["frequency"] = config.params.i2c.frequency;
            params["address"] = config.params.i2c.address;
            params["isMaster"] = config.params.i2c.isMaster;
        }
        else if (config.type == PeripheralType::SPI) {
            params["frequency"] = config.params.spi.frequency;
            params["mode"] = config.params.spi.mode;
            params["msbFirst"] = config.params.spi.msbFirst;
        }
        else if (config.isGPIOPeripheral()) {
            params["initialState"] = static_cast<int>(config.params.gpio.initialState);
            params["pwmChannel"] = config.params.gpio.pwmChannel;
            params["pwmFrequency"] = config.params.gpio.pwmFrequency;
            params["pwmResolution"] = config.params.gpio.pwmResolution;
            params["defaultDuty"] = config.params.gpio.defaultDuty;
        }
        else if (config.type == PeripheralType::ADC) {
            params["attenuation"] = config.params.adc.attenuation;
            params["resolution"] = config.params.adc.resolution;
            params["sampleRate"] = config.params.adc.sampleRate;
        }
        else if (config.type == PeripheralType::DAC) {
            params["channel"] = config.params.dac.channel;
        }
        else if (config.type == PeripheralType::STEPPER_MOTOR) {
            params["stepsPerRevolution"] = config.params.stepper.stepsPerRevolution;
            params["speed"] = config.params.stepper.speed;
        }
#if FASTBEE_ENABLE_LCD
        else if (config.type == PeripheralType::LCD) {
            params["width"]     = config.params.lcd.width;
            params["height"]    = config.params.lcd.height;
            params["interface"] = config.params.lcd.interface;
        }
#endif
#if FASTBEE_ENABLE_SEVEN_SEGMENT
        else if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
            params["brightness"] = config.params.segment.brightness;
        }
#endif
        else if (config.type == PeripheralType::NEO_PIXEL) {
            params["count"] = clampNeoPixelCount(config.params.neopixel.count);
            params["brightness"] = config.params.neopixel.brightness;
        }
        else if (config.type == PeripheralType::RF_MODULE) {
            params["mode"] = config.params.rf.mode;
            params["pulseWidth"] = clampRfPulseWidth(config.params.rf.pulseWidth);
            params["repeat"] = clampRfRepeat(config.params.rf.repeat);
            params["bitLength"] = clampRfBitLength(config.params.rf.bitLength);
            params["activeHigh"] = config.params.rf.activeHigh;
        }
        else if (config.type == PeripheralType::RADAR_SENSOR) {
            params["mode"] = config.params.radar.mode;
            params["activeHigh"] = config.params.radar.activeHigh;
            params["debounceMs"] = config.params.radar.debounceMs;
            params["holdMs"] = config.params.radar.holdMs;
        }
    }

    String jsonStr;
    serializeJson(doc, jsonStr);

    // 原子写入：先写临时文件再 rename，防止断电导致配置损坏
    if (FileUtils::atomicWriteFile(PERIPHERAL_CONFIG_FILE, jsonStr)) {
        LOG_INFO("Peripheral Manager: Configuration saved successfully");
        return true;
    } else {
        LOG_ERROR("Peripheral Manager: Failed to save configuration (atomic write)");
        return false;
    }
}

bool PeripheralManager::loadConfiguration() {
    if (!LittleFS.exists(PERIPHERAL_CONFIG_FILE)) {
        LOG_INFO("Peripheral Manager: No configuration file found");
        return true;
    }

    // 检查文件大小，避免一次性读取过大文件
    File file = LittleFS.open(PERIPHERAL_CONFIG_FILE, "r");
    if (!file) {
        LOG_ERROR("Peripheral Manager: Failed to open config file for reading");
        return false;
    }

    size_t fileSize = file.size();
    LOG_DEBUGF("Peripheral Manager: Config file size: %d bytes", fileSize);

    // 检查可用内存
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < fileSize + 4096) {
        LOG_ERRORF("Peripheral Manager: Insufficient memory (free: %d, needed: %d)",
                   freeHeap, fileSize + 4096);
        file.close();
        return false;
    }

    // 使用流式解析，避免一次性读取整个文件
    FastBeeJsonDocLarge doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        LOG_ERRORF("Peripheral Manager: Failed to parse config - %s", error.c_str());
        return false;
    }

    JsonArray periphs = doc["peripherals"].as<JsonArray>();
    if (periphs.isNull()) {
        LOG_WARNING("Peripheral Manager: No peripherals found in configuration");
        return true;
    }

    peripherals.clear();
    runtimeStates.clear();
    pinToPeripheral.clear();

    int loadedCount = 0;
    for (JsonObject obj : periphs) {
        if (peripherals.size() >= FastBee::ResourceProfile::MAX_PERIPHERALS) {
            LOG_WARNINGF("Peripheral Manager: profile %s allows max %u peripherals, skipping remaining config entries",
                         FastBee::ResourceProfile::NAME,
                         static_cast<unsigned int>(FastBee::ResourceProfile::MAX_PERIPHERALS));
            break;
        }

        PeripheralConfig config;

        config.id = obj["id"] | "";
        config.name = obj["name"] | "";
        config.type = static_cast<PeripheralType>(obj["type"] | 0);
        config.enabled = obj["enabled"] | false;

        if (config.type == PeripheralType::RESERVED_46) {
            LOG_WARNINGF("Peripheral Manager: Skipping removed peripheral type 46 for '%s'", config.id.c_str());
            continue;
        }
        if (config.type == PeripheralType::STEPPER_MOTOR) {
            config.params.stepper.stepsPerRevolution = STEPPER_DEFAULT_STEPS_PER_REV;
            config.params.stepper.speed = STEPPER_DEFAULT_RPM;
        } else if (config.type == PeripheralType::NEO_PIXEL) {
            config.params.neopixel.count = NEOPIXEL_DEFAULT_COUNT;
            config.params.neopixel.brightness = NEOPIXEL_DEFAULT_BRIGHTNESS;
        } else if (config.type == PeripheralType::RF_MODULE) {
            config.params.rf.mode = RF_MODE_TX;
            config.params.rf.pulseWidth = RF_DEFAULT_PULSE_WIDTH_US;
            config.params.rf.repeat = RF_DEFAULT_REPEAT;
            config.params.rf.bitLength = RF_DEFAULT_BIT_LENGTH;
            config.params.rf.activeHigh = true;
        } else if (config.type == PeripheralType::RADAR_SENSOR) {
            config.params.radar.mode = RADAR_MODE_DIGITAL;
            config.params.radar.activeHigh = true;
            config.params.radar.debounceMs = RADAR_DEFAULT_DEBOUNCE_MS;
            config.params.radar.holdMs = RADAR_DEFAULT_HOLD_MS;
        }

        // 引脚配置
        JsonArray pins = obj["pins"].as<JsonArray>();
        config.pinCount = 0;
        for (int i = 0; i < 8 && i < pins.size(); i++) {
            config.pins[i] = pins[i] | 255;
            if (config.pins[i] != 255) {
                config.pinCount++;
            }
        }

        // 类型特定参数
        JsonObject params = obj["params"].as<JsonObject>();
        if (!params.isNull()) {
            if (config.type == PeripheralType::UART) {
                config.params.uart.baudRate = params["baudRate"] | 115200;
                config.params.uart.dataBits = params["dataBits"] | 8;
                config.params.uart.stopBits = params["stopBits"] | 1;
                config.params.uart.parity = params["parity"] | 0;
            }
            else if (config.type == PeripheralType::I2C) {
                config.params.i2c.frequency = params["frequency"] | 100000;
                config.params.i2c.address = params["address"] | 0;
                config.params.i2c.isMaster = params["isMaster"] | true;
            }
            else if (config.type == PeripheralType::SPI) {
                config.params.spi.frequency = params["frequency"] | 1000000;
                config.params.spi.mode = params["mode"] | 0;
                config.params.spi.msbFirst = params["msbFirst"] | true;
            }
            else if (config.isGPIOPeripheral()) {
                config.params.gpio.initialState = static_cast<GPIOState>(params["initialState"] | 0);
                config.params.gpio.pwmChannel = params["pwmChannel"] | 0;
                config.params.gpio.pwmFrequency = params["pwmFrequency"] | 1000;
                config.params.gpio.pwmResolution = params["pwmResolution"] | 8;
                config.params.gpio.defaultDuty = params["defaultDuty"] | 0;
            }
            else if (config.type == PeripheralType::ADC) {
                config.params.adc.attenuation = params["attenuation"] | 0;
                config.params.adc.resolution = params["resolution"] | 12;
                config.params.adc.sampleRate = params["sampleRate"] | 0;
            }
            else if (config.type == PeripheralType::DAC) {
                config.params.dac.channel = params["channel"] | 1;
            }
            else if (config.type == PeripheralType::STEPPER_MOTOR) {
                config.params.stepper.stepsPerRevolution = params["stepsPerRevolution"] | STEPPER_DEFAULT_STEPS_PER_REV;
                config.params.stepper.speed = clampStepperSpeed(params["speed"] | STEPPER_DEFAULT_RPM);
            }
#if FASTBEE_ENABLE_LCD
            else if (config.type == PeripheralType::LCD) {
                // 支持扩展宽度/高度（用 int 读取避免 uint8_t 默认值裁剪）
                int w = params["width"]  | 128;
                int h = params["height"] | 64;
                int iface = params["interface"] | 2; // 0=Parallel, 1=SPI, 2=I2C
                config.params.lcd.width     = (uint8_t)(w > 255 ? 255 : (w < 0 ? 0 : w));
                config.params.lcd.height    = (uint8_t)(h > 255 ? 255 : (h < 0 ? 0 : h));
                config.params.lcd.interface = (uint8_t)(iface < 0 ? 2 : iface);
            }
#endif
#if FASTBEE_ENABLE_SEVEN_SEGMENT
            else if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
                int b = params["brightness"] | 2;
                if (b < 0) b = 0;
                if (b > 7) b = 7;
                config.params.segment.brightness = (uint8_t)b;
            }
#endif
            else if (config.type == PeripheralType::NEO_PIXEL) {
                int count = params["count"] | NEOPIXEL_DEFAULT_COUNT;
                int brightness = params["brightness"] | NEOPIXEL_DEFAULT_BRIGHTNESS;
                config.params.neopixel.count = clampNeoPixelCount(count < 0 ? 0 : (uint16_t)count);
                config.params.neopixel.brightness = clampNeoPixelBrightness(brightness < 0 ? 0 : (uint16_t)brightness);
            }
            else if (config.type == PeripheralType::RF_MODULE) {
                int mode = params["mode"] | RF_MODE_TX;
                config.params.rf.mode = (mode == RF_MODE_RX) ? RF_MODE_RX : RF_MODE_TX;
                config.params.rf.pulseWidth = clampRfPulseWidth(params["pulseWidth"] | RF_DEFAULT_PULSE_WIDTH_US);
                config.params.rf.repeat = clampRfRepeat(params["repeat"] | RF_DEFAULT_REPEAT);
                config.params.rf.bitLength = clampRfBitLength(params["bitLength"] | RF_DEFAULT_BIT_LENGTH);
                config.params.rf.activeHigh = params["activeHigh"] | true;
            }
            else if (config.type == PeripheralType::RADAR_SENSOR) {
                config.params.radar.mode = RADAR_MODE_DIGITAL;
                config.params.radar.activeHigh = params["activeHigh"] | true;
                config.params.radar.debounceMs = params["debounceMs"] | RADAR_DEFAULT_DEBOUNCE_MS;
                config.params.radar.holdMs = params["holdMs"] | RADAR_DEFAULT_HOLD_MS;
            }
        }

        if (!config.id.isEmpty() && config.type != PeripheralType::UNCONFIGURED) {
            String errorMsg;
            if (!validateConfig(config, errorMsg)) {
                LOG_WARNINGF("Peripheral Manager: Skipping invalid config '%s': %s",
                             config.id.c_str(),
                             errorMsg.c_str());
                continue;
            }

            peripherals[config.id] = config;

            PeripheralRuntimeState state;
            state.id = config.id;
            state.status = config.enabled ? PeripheralStatus::PERIPHERAL_ENABLED : PeripheralStatus::PERIPHERAL_DISABLED;
            runtimeStates[config.id] = state;

            updatePinMapping(config.id, config);
            loadedCount++;
        }
    }

    // 加固：加载完成后基于真实数据重建引脚映射缓存，确保一致性
    // （保险套：如果配置文件历史遗留同引脚多外设，updatePinMapping 会覆盖后者，
    //   由 checkPinConflict 基于真实数据的逻辑兼容此情况）
    rebuildPinMapping();

    LOG_INFOF("Peripheral Manager: Loaded %d peripheral configurations", loadedCount);
    return true;
}

// ========== 内部辅助方法 ==========

bool PeripheralManager::validateConfig(const PeripheralConfig& config, String& errorMsg) {
    // 基本字段验证
    if (config.id.isEmpty()) {
        errorMsg = "ID 不能为空";
        return false;
    }

    if (config.name.isEmpty()) {
        errorMsg = "名称不能为空";
        return false;
    }

    if (config.id.length() > FastBee::ResourceProfile::MAX_PERIPHERAL_ID_LEN) {
        errorMsg = "Peripheral ID too long (max " + String(FastBee::ResourceProfile::MAX_PERIPHERAL_ID_LEN) + ")";
        return false;
    }

    if (config.name.length() > FastBee::ResourceProfile::MAX_PERIPHERAL_NAME_LEN) {
        errorMsg = "Peripheral name too long (max " + String(FastBee::ResourceProfile::MAX_PERIPHERAL_NAME_LEN) + ")";
        return false;
    }

    if (config.type == PeripheralType::UNCONFIGURED) {
        errorMsg = "外设类型不能为空";
        return false;
    }

    if (config.type == PeripheralType::RESERVED_46) {
        errorMsg = "该外设类型已移除";
        return false;
    }

    // Modbus 外设不需要引脚配置
    if (config.isModbusPeripheral()) {
        if (config.params.modbus.slaveAddress < 1 || config.params.modbus.slaveAddress > 247) {
            errorMsg = "Modbus 从站地址无效 (有效范围: 1-247)";
            return false;
        }
        return true;
    }

    // 设备事件虚拟外设：无需引脚/参数校验，仅用 id+name
    if (isDeviceEventType(config.type)) {
        return true;
    }

    if (config.pinCount == 0) {
        errorMsg = "至少需要配置一个引脚";
        return false;
    }

    // 引脚验证
    for (int i = 0; i < config.pinCount && i < 8; i++) {
        if (config.pins[i] != 255) {
            // 使用增强的引脚验证
            if (!validatePinForType(config.pins[i], config.type, errorMsg)) {
                return false;
            }
        }
    }

    // 类型特定参数验证
    switch (config.type) {
        case PeripheralType::UART:
            // UART 波特率验证
            if (config.params.uart.baudRate == 0 || config.params.uart.baudRate > 5000000) {
                errorMsg = "UART 波特率无效 (有效范围: 1-5000000)";
                return false;
            }
            if (config.params.uart.dataBits < 5 || config.params.uart.dataBits > 8) {
                errorMsg = "UART 数据位无效 (有效值: 5-8)";
                return false;
            }
            if (config.params.uart.stopBits < 1 || config.params.uart.stopBits > 2) {
                errorMsg = "UART 停止位无效 (有效值: 1, 2)";
                return false;
            }
            if (config.params.uart.parity > 2) {
                errorMsg = "UART 校验位无效 (0=无, 1=奇, 2=偶)";
                return false;
            }
            break;

        case PeripheralType::I2C:
            // I2C 频率验证
            if (config.params.i2c.frequency != 100000 &&
                config.params.i2c.frequency != 400000 &&
                config.params.i2c.frequency != 1000000) {
                errorMsg = "I2C 频率无效 (支持: 100000, 400000, 1000000)";
                return false;
            }
            if (!config.params.i2c.isMaster && config.params.i2c.address == 0) {
                errorMsg = "I2C 从机模式需要设置地址";
                return false;
            }
            if (config.params.i2c.address > 127) {
                errorMsg = "I2C 地址无效 (有效范围: 0-127)";
                return false;
            }
            break;

        case PeripheralType::SPI:
            // SPI 频率验证
            if (config.params.spi.frequency == 0 || config.params.spi.frequency > 80000000) {
                errorMsg = "SPI 频率无效 (有效范围: 1-80000000)";
                return false;
            }
            if (config.params.spi.mode > 3) {
                errorMsg = "SPI 模式无效 (有效值: 0-3)";
                return false;
            }
            break;

        case PeripheralType::GPIO_PWM_OUTPUT:
        case PeripheralType::GPIO_ANALOG_OUTPUT:
        case PeripheralType::PWM_SERVO:
            // PWM 参数验证
            if (config.params.gpio.pwmChannel >= CHIP_MAX_PWM_CH) {
                errorMsg = "PWM 通道无效 (有效范围: 0-" + String(CHIP_MAX_PWM_CH - 1) + ")";
                return false;
            }
            if (config.params.gpio.pwmFrequency == 0 || config.params.gpio.pwmFrequency > 40000000) {
                errorMsg = "PWM 频率无效 (有效范围: 1-40000000)";
                return false;
            }
            if (config.params.gpio.pwmResolution < 1 || config.params.gpio.pwmResolution > 16) {
                errorMsg = "PWM 分辨率无效 (有效范围: 1-16 位)";
                return false;
            }
            // 检查频率和分辨率组合是否有效
            // ESP32: freq * (2^resolution) <= 80MHz
            {
                uint64_t maxFreqResProduct = 80000000ULL;
                uint64_t freqResProduct = (uint64_t)config.params.gpio.pwmFrequency * (1ULL << config.params.gpio.pwmResolution);
                if (freqResProduct > maxFreqResProduct) {
                    errorMsg = "PWM 频率和分辨率组合无效 (freq * 2^resolution 不能超过 80MHz)";
                    return false;
                }
            }
            break;

        case PeripheralType::ADC:
        case PeripheralType::GPIO_ANALOG_INPUT:
            // ADC 参数验证
            if (config.params.adc.attenuation > 3) {
                errorMsg = "ADC 衰减系数无效 (有效值: 0-3)";
                return false;
            }
            if (config.params.adc.resolution < 9 || config.params.adc.resolution > 12) {
                errorMsg = "ADC 分辨率无效 (有效范围: 9-12 位)";
                return false;
            }
            break;

        case PeripheralType::DAC:
            // DAC 参数验证
            if (config.params.dac.channel != 1 && config.params.dac.channel != 2) {
                errorMsg = "DAC 通道无效 (有效值: 1, 2)";
                return false;
            }
            break;

        case PeripheralType::STEPPER_MOTOR:
            if (config.pinCount < 4) {
                errorMsg = "步进电机需要 4 个引脚 (IN1,IN2,IN3,IN4)";
                return false;
            }
            for (uint8_t i = 0; i < 4; i++) {
                if (isReservedPin(config.pins[i])) {
                    errorMsg = String("GPIO") + String(config.pins[i]) + " 是当前芯片保留引脚，不能用于步进电机";
                    return false;
                }
            }
            if (config.params.stepper.stepsPerRevolution == 0) {
                errorMsg = "步进电机每圈步数必须大于 0";
                return false;
            }
            if (config.params.stepper.speed > STEPPER_MAX_RPM) {
                errorMsg = "步进电机默认转速过高 (最大 " + String(STEPPER_MAX_RPM) + " RPM)";
                return false;
            }
            break;

        case PeripheralType::NEO_PIXEL:
            if (config.pinCount < 1) {
                errorMsg = "WS2812B 需要 1 个数据引脚";
                return false;
            }
            if (isReservedPin(config.pins[0]) || isInputOnlyPin(config.pins[0])) {
                errorMsg = String("GPIO") + String(config.pins[0]) + " 不适合输出 WS2812B 信号";
                return false;
            }
            if (config.params.neopixel.count == 0 || config.params.neopixel.count > NEOPIXEL_MAX_COUNT) {
                errorMsg = "WS2812B 灯珠数量无效 (1-" + String(NEOPIXEL_MAX_COUNT) + ")";
                return false;
            }
            break;

        case PeripheralType::RF_MODULE:
            if (config.pinCount < 1) {
                errorMsg = "RF module requires 1 data pin";
                return false;
            }
            if (config.params.rf.mode > RF_MODE_RX) {
                errorMsg = "RF module mode must be 0(TX) or 1(RX)";
                return false;
            }
            if (config.params.rf.mode == RF_MODE_TX &&
                (isReservedPin(config.pins[0]) || isInputOnlyPin(config.pins[0]))) {
                errorMsg = String("GPIO") + String(config.pins[0]) + " cannot output RF signal";
                return false;
            }
            if (config.params.rf.pulseWidth < RF_MIN_PULSE_WIDTH_US ||
                config.params.rf.pulseWidth > RF_MAX_PULSE_WIDTH_US) {
                errorMsg = "RF pulseWidth must be 100-2000 us";
                return false;
            }
            if (config.params.rf.repeat < RF_MIN_REPEAT ||
                config.params.rf.repeat > RF_MAX_REPEAT) {
                errorMsg = "RF repeat must be 1-20";
                return false;
            }
            if (config.params.rf.bitLength < RF_MIN_BIT_LENGTH ||
                config.params.rf.bitLength > RF_MAX_BIT_LENGTH) {
                errorMsg = "RF bitLength must be 1-32";
                return false;
            }
            break;

        case PeripheralType::RADAR_SENSOR:
            if (config.pinCount < 1) {
                errorMsg = "Radar sensor requires 1 OUT pin";
                return false;
            }
            if (config.params.radar.mode != RADAR_MODE_DIGITAL) {
                errorMsg = "Radar sensor mode currently supports digital OUT only";
                return false;
            }
            if (config.params.radar.debounceMs > 5000 || config.params.radar.holdMs > 60000) {
                errorMsg = "Radar debounceMs/holdMs is out of range";
                return false;
            }
            break;

        default:
            // 其他类型暂不需要特殊验证
            break;
    }

    return true;
}

bool PeripheralManager::setupHardware(const PeripheralConfig& config) {
    // 加固：统一校验外设声明使用的所有引脚是否在当前芯片有效范围内
    // （对 Modbus 等不占 GPIO 的外设，pinCount 通常为 0，校验自然通过）
    for (uint8_t i = 0; i < config.pinCount && i < 8; i++) {
        uint8_t p = config.pins[i];
        if (p == 255) continue;  // 未使用位
        if (!isValidPin(p)) {
            LOG_ERRORF("Peripheral Manager: '%s' pin %u invalid on %s (max GPIO=%d) - skipping hardware init",
                       config.id.c_str(), (unsigned)p, CHIP_NAME, (int)CHIP_MAX_GPIO);
            return false;
        }
    }

    if (config.isGPIOPeripheral()) {
        return setupGPIOPin(config);
    }

    if (config.type == PeripheralType::UART) {
        return setupUartHardware(config);
    }

    if (config.type == PeripheralType::STEPPER_MOTOR) {
        if (config.pinCount < 4) {
            LOG_WARNINGF("Peripheral Manager: Stepper '%s' requires 4 pins (IN1-IN4)", config.id.c_str());
            return false;
        }
        for (uint8_t i = 0; i < 4; i++) {
            if (isReservedPin(config.pins[i])) {
                LOG_ERRORF("Peripheral Manager: Stepper '%s' rejects reserved GPIO%d on %s",
                           config.id.c_str(), config.pins[i], CHIP_NAME);
                return false;
            }
            pinMode(config.pins[i], OUTPUT);
            digitalWrite(config.pins[i], LOW);
        }
        LOG_INFOF("Peripheral Manager: Stepper '%s' initialized IN1=%d IN2=%d IN3=%d IN4=%d speed=%d rpm steps=%d",
                  config.id.c_str(), config.pins[0], config.pins[1], config.pins[2], config.pins[3],
                  clampStepperSpeed(config.params.stepper.speed), config.params.stepper.stepsPerRevolution);
        return true;
    }

    if (config.type == PeripheralType::NEO_PIXEL) {
        if (config.pinCount < 1) {
            LOG_WARNINGF("Peripheral Manager: NeoPixel '%s' requires 1 data pin", config.id.c_str());
            return false;
        }
        uint8_t pin = config.pins[0];
        if (isReservedPin(pin) || isInputOnlyPin(pin)) {
            LOG_ERRORF("Peripheral Manager: NeoPixel '%s' rejects GPIO%d on %s",
                       config.id.c_str(), pin, CHIP_NAME);
            return false;
        }
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
        NeoPixelRgb off = {0, 0, 0};
        writeNeoPixelSolid(pin, clampNeoPixelCount(config.params.neopixel.count), off);
        LOG_INFOF("Peripheral Manager: NeoPixel '%s' initialized pin=%d count=%d brightness=%d",
                  config.id.c_str(), pin,
                  clampNeoPixelCount(config.params.neopixel.count),
                  config.params.neopixel.brightness);
        return true;
    }

    if (config.type == PeripheralType::RF_MODULE) {
        if (config.pinCount < 1) {
            LOG_WARNINGF("Peripheral Manager: RF module '%s' requires 1 data pin", config.id.c_str());
            return false;
        }
        uint8_t pin = config.pins[0];
        if (config.params.rf.mode == RF_MODE_RX) {
            pinMode(pin, INPUT);
            LOG_INFOF("Peripheral Manager: RF RX '%s' initialized pin=%d activeHigh=%d",
                      config.id.c_str(), pin, config.params.rf.activeHigh ? 1 : 0);
        } else {
            if (isReservedPin(pin) || isInputOnlyPin(pin)) {
                LOG_ERRORF("Peripheral Manager: RF TX '%s' rejects GPIO%d on %s",
                           config.id.c_str(), pin, CHIP_NAME);
                return false;
            }
            pinMode(pin, OUTPUT);
            digitalWrite(pin, config.params.rf.activeHigh ? LOW : HIGH);
            LOG_INFOF("Peripheral Manager: RF TX '%s' initialized pin=%d bits=%d pulse=%dus repeat=%d activeHigh=%d",
                      config.id.c_str(), pin,
                      clampRfBitLength(config.params.rf.bitLength),
                      clampRfPulseWidth(config.params.rf.pulseWidth),
                      clampRfRepeat(config.params.rf.repeat),
                      config.params.rf.activeHigh ? 1 : 0);
        }
        return true;
    }

    if (config.type == PeripheralType::RADAR_SENSOR) {
        if (config.pinCount < 1) {
            LOG_WARNINGF("Peripheral Manager: Radar '%s' requires 1 OUT pin", config.id.c_str());
            return false;
        }
        pinMode(config.pins[0], INPUT);
        LOG_INFOF("Peripheral Manager: Radar '%s' initialized OUT=%d activeHigh=%d hold=%ums",
                  config.id.c_str(), config.pins[0],
                  config.params.radar.activeHigh ? 1 : 0,
                  (unsigned)config.params.radar.holdMs);
        return true;
    }

#if FASTBEE_ENABLE_SEVEN_SEGMENT
    // TM1637 数码管：使用 CLK + DIO 两个引脚，bit-bang 初始化
    if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        if (config.pinCount < 2) {
            LOG_WARNINGF("Peripheral Manager: TM1637 '%s' requires 2 pins (CLK, DIO)", config.id.c_str());
            return false;
        }
        uint8_t clk = config.pins[0];
        uint8_t dio = config.pins[1];
        if (!isValidPin(clk) || !isValidPin(dio)) {
            LOG_WARNINGF("Peripheral Manager: TM1637 '%s' invalid pins CLK=%d DIO=%d",
                         config.id.c_str(), clk, dio);
            return false;
        }
        // 亮度从配置 params.segment.brightness 读取（0-7，越界自动限制）
        uint8_t bri = config.params.segment.brightness;
        if (bri > 7) bri = 2;
        bool ok = SevenSegmentDriver::instance().begin(config.id, clk, dio, bri);
        LOG_INFOF("Peripheral Manager: TM1637 '%s' init CLK=%d DIO=%d bri=%d -> %s",
                  config.id.c_str(), clk, dio, bri, ok ? "ok" : "fail");
        return ok;
    }
#endif

    // Modbus 外设不需要本地硬件初始化（通过 RS485 总线通信）
    if (config.isModbusPeripheral()) {
        LOG_INFOF("Peripheral Manager: Modbus device '%s' (slave=%d) registered, no local HW init needed",
                  config.name.c_str(), config.params.modbus.slaveAddress);
        return true;
    }

    // 设备事件虚拟外设（无物理 GPIO）：仅作为事件发射源，直接返回成功
    if (isDeviceEventType(config.type)) {
        LOG_INFOF("Peripheral Manager: Device event emitter '%s' (%s) registered",
                  config.name.c_str(), config.id.c_str());
        return true;
    }

    // LCD/OLED 显示屏初始化
#if FASTBEE_ENABLE_LCD
    if (config.type == PeripheralType::LCD) {
        return LCDManager::getInstance().initialize(config);
    }
#endif

    // ========== I2C 总线初始化 ==========
    if (config.type == PeripheralType::I2C) {
        if (config.pinCount < 2) {
            LOG_WARNINGF("Peripheral Manager: I2C '%s' requires 2 pins (SDA, SCL)", config.id.c_str());
            return false;
        }
        uint8_t sda = config.pins[0];
        uint8_t scl = config.pins[1];
        if (isReservedPin(sda) || isReservedPin(scl)) {
            LOG_ERRORF("Peripheral Manager: I2C '%s' rejects reserved pins SDA=%d SCL=%d on %s",
                       config.id.c_str(), sda, scl, CHIP_NAME);
            return false;
        }
        uint32_t freq = config.params.i2c.frequency;
        if (freq == 0) freq = 100000;  // 默认 100kHz
        Wire.begin(static_cast<int>(sda), static_cast<int>(scl), freq);
        LOG_INFOF("Peripheral Manager: I2C '%s' initialized SDA=%d SCL=%d freq=%u master=%d addr=0x%02X",
                  config.id.c_str(), sda, scl, (unsigned)freq,
                  config.params.i2c.isMaster ? 1 : 0, config.params.i2c.address);
        return true;
    }

    // ========== SPI 总线初始化 ==========
    if (config.type == PeripheralType::SPI) {
        if (config.pinCount < 4) {
            LOG_WARNINGF("Peripheral Manager: SPI '%s' requires 4 pins (MISO, MOSI, SCK, CS)", config.id.c_str());
            return false;
        }
        uint8_t miso = config.pins[0];
        uint8_t mosi = config.pins[1];
        uint8_t sck  = config.pins[2];
        uint8_t cs   = config.pins[3];
        // 校验 CS 引脚（MISO 可为输入专用引脚）
        if (isReservedPin(cs)) {
            LOG_ERRORF("Peripheral Manager: SPI '%s' rejects reserved CS GPIO%d on %s",
                       config.id.c_str(), cs, CHIP_NAME);
            return false;
        }
        pinMode(cs, OUTPUT);
        digitalWrite(cs, HIGH);  // CS 默认高（未选中）
        uint32_t freq = config.params.spi.frequency;
        if (freq == 0) freq = 1000000;  // 默认 1MHz
        SPI.begin(sck, miso, mosi, cs);
        LOG_INFOF("Peripheral Manager: SPI '%s' initialized MISO=%d MOSI=%d SCK=%d CS=%d freq=%u mode=%d",
                  config.id.c_str(), miso, mosi, sck, cs, (unsigned)freq, config.params.spi.mode);
        return true;
    }

    // ========== DAC 数模转换初始化 ==========
    if (config.type == PeripheralType::DAC) {
        return setupDACPin(config);
    }

    // ========== ADC 模数转换初始化 ==========
    if (config.type == PeripheralType::ADC) {
        uint8_t pin = config.getPrimaryPin();
        if (pin == 255) {
            LOG_WARNINGF("Peripheral Manager: ADC '%s' requires a valid analog pin", config.id.c_str());
            return false;
        }
        // 设置 ADC 分辨率（衰减在 PeriphExecExecutor 的传感器读取中按需配置）
#if defined(SOC_ADC_MAX_BITWIDTH) && SOC_ADC_MAX_BITWIDTH >= 9
        analogReadResolution(config.params.adc.resolution);
#endif
        // 执行一次空读以稳定 ADC
        analogRead(pin);
        LOG_INFOF("Peripheral Manager: ADC '%s' initialized pin=%d atten=%d res=%d",
                  config.id.c_str(), pin, config.params.adc.attenuation, config.params.adc.resolution);
        return true;
    }

    // ========== PWM_SERVO 舵机初始化（使用 LEDC，但不在 GPIO 类型范围内）==========
    if (config.type == PeripheralType::PWM_SERVO) {
        return setupPWMPin(config);
    }

    // ========== ONE_WIRE 单总线初始化 ==========
    if (config.type == PeripheralType::ONE_WIRE) {
        uint8_t pin = config.getPrimaryPin();
        if (pin == 255) {
            LOG_WARNINGF("Peripheral Manager: OneWire '%s' requires a valid pin", config.id.c_str());
            return false;
        }
        // OneWire 需要外部上拉，启用内部上拉作为辅助
        pinMode(pin, INPUT_PULLUP);
        LOG_INFOF("Peripheral Manager: OneWire '%s' initialized pin=%d (internal pull-up)",
                  config.id.c_str(), pin);
        return true;
    }

    // ========== SENSOR 通用传感器初始化 ==========
    if (config.type == PeripheralType::SENSOR) {
        uint8_t pin = config.getPrimaryPin();
        if (pin != 255) {
            // 根据传感器类型设置引脚模式
            pinMode(pin, INPUT_PULLUP);
            LOG_INFOF("Peripheral Manager: Sensor '%s' initialized pin=%d sensorType=%d interval=%ums",
                      config.id.c_str(), pin, config.params.sensor.sensorType,
                      (unsigned)config.params.sensor.sampleInterval);
        } else {
            LOG_INFOF("Peripheral Manager: Sensor '%s' registered (no local pin) sensorType=%d",
                      config.id.c_str(), config.params.sensor.sensorType);
        }
        return true;
    }

    // 未实现硬件驱动的外设类型：CAN/USB/JTAG/SWD/SDIO/CAMERA/ETHERNET/ENCODER
    // 注册成功但不做硬件初始化，记录警告以便前端“待驱动”标记一致
    int typeVal = static_cast<int>(config.type);
    bool isUnimplemented = (typeVal == 4 || typeVal == 5 || typeVal == 31 || typeVal == 32 ||
                            typeVal == 37 || typeVal == 39 || typeVal == 40 || typeVal == 43);
    LOG_WARNINGF("Peripheral Manager: Type %d ('%s') registered - hardware driver not yet implemented%s",
              typeVal, config.id.c_str(), isUnimplemented ? " (前端已标记为“待驱动”)" : "");
    return true;
}

bool PeripheralManager::teardownHardware(const PeripheralConfig& config) {
    if (config.isGPIOPeripheral()) {
        uint8_t pin = config.getPrimaryPin();
        if (pin != 255) {
            detachInterrupt(pin);
        }
    }

    if (config.type == PeripheralType::UART) {
        auto it = uartPortById.find(config.id);
        if (it != uartPortById.end()) {
            HardwareSerial* serial = serialForPort(it->second);
            if (serial && it->second != 0) {
                serial->end();
            }
            uartPortById.erase(it);
        }
        return true;
    }

    // I2C 总线清理
    if (config.type == PeripheralType::I2C) {
        Wire.end();
        LOG_INFOF("Peripheral Manager: I2C '%s' bus released", config.id.c_str());
        return true;
    }

    // SPI 总线清理
    if (config.type == PeripheralType::SPI) {
        SPI.end();
        // 释放 CS 引脚
        if (config.pinCount >= 4) {
            uint8_t cs = config.pins[3];
            if (cs != 255 && !isReservedPin(cs)) {
                pinMode(cs, INPUT);
            }
        }
        LOG_INFOF("Peripheral Manager: SPI '%s' bus released", config.id.c_str());
        return true;
    }

    // LCD/OLED 显示屏清理
#if FASTBEE_ENABLE_LCD
    if (config.type == PeripheralType::LCD) {
        LCDManager::getInstance().deinitialize();
    }
#endif

    // TM1637 数码管清理
#if FASTBEE_ENABLE_SEVEN_SEGMENT
    if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        SevenSegmentDriver::instance().release(config.id);
    }
#endif

    if (config.type == PeripheralType::STEPPER_MOTOR) {
        stopStepper(config.id);
        auto it = stepperTickers.find(config.id);
        if (it != stepperTickers.end()) {
            it->second->ticker.detach();
            delete it->second;
            stepperTickers.erase(it);
        }
        bool safeRelease = config.pinCount >= 4;
        for (uint8_t i = 0; safeRelease && i < 4; i++) {
            if (isReservedPin(config.pins[i])) safeRelease = false;
        }
        if (safeRelease) releaseStepperCoils(config);
    }

    if (config.type == PeripheralType::NEO_PIXEL) {
        if (config.pinCount >= 1 && !isReservedPin(config.pins[0]) && !isInputOnlyPin(config.pins[0])) {
            NeoPixelRgb off = {0, 0, 0};
            writeNeoPixelSolid(config.pins[0], clampNeoPixelCount(config.params.neopixel.count), off);
            digitalWrite(config.pins[0], LOW);
        }
        neopixelRainbowIndex.erase(config.id);
    }

    if (config.type == PeripheralType::RF_MODULE &&
        config.params.rf.mode == RF_MODE_TX &&
        config.pinCount >= 1 &&
        !isReservedPin(config.pins[0]) &&
        !isInputOnlyPin(config.pins[0])) {
        digitalWrite(config.pins[0], config.params.rf.activeHigh ? LOW : HIGH);
    }

    // GPIO 输出清理：置低电平后释放为输入，防止外设继续带电或浮空
    if (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT) {
        if (config.pinCount >= 1 && !isReservedPin(config.pins[0]) && !isInputOnlyPin(config.pins[0])) {
            digitalWrite(config.pins[0], LOW);
            pinMode(config.pins[0], INPUT);
        }
    }

    // PWM / 模拟输出 / 舵机清理：断开 LEDC 并释放引脚
    if (config.type == PeripheralType::GPIO_PWM_OUTPUT ||
        config.type == PeripheralType::GPIO_ANALOG_OUTPUT ||
        config.type == PeripheralType::PWM_SERVO) {
        if (config.pinCount >= 1 && !isReservedPin(config.pins[0]) && !isInputOnlyPin(config.pins[0])) {
            // ledcDetachPin 在较新 ESP-IDF 中已更名为 ledcDetach
            #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
                ledcDetach(config.pins[0]);
            #else
                ledcDetachPin(config.pins[0]);
            #endif
            pinMode(config.pins[0], INPUT);
        }
    }

    // DAC 清理：输出归零后释放引脚
#if CHIP_HAS_DAC
    if (config.type == PeripheralType::DAC) {
        uint8_t pin = config.getPrimaryPin();
        if (pin != 255 && (pin == 25 || pin == 26)) {
            dacWrite(pin, 0);
        }
    }
#endif

    // ONE_WIRE 单总线清理：释放上拉
    if (config.type == PeripheralType::ONE_WIRE) {
        uint8_t pin = config.getPrimaryPin();
        if (pin != 255) {
            pinMode(pin, INPUT);
        }
    }

    return true;
}

bool PeripheralManager::setupGPIOPin(const PeripheralConfig& config) {
    uint8_t pin = config.getPrimaryPin();
    if (pin == 255) return false;
    // 双保险：底层 pinMode 前再核验一次引脚，避免无效 pin 触发底层 abort
    if (!isValidPin(pin)) {
        LOG_ERRORF("Peripheral Manager: setupGPIOPin '%s' reject invalid pin %u on %s",
                   config.id.c_str(), (unsigned)pin, CHIP_NAME);
        return false;
    }

    switch (config.type) {
        case PeripheralType::GPIO_DIGITAL_INPUT:
            pinMode(pin, INPUT);
            break;

        case PeripheralType::GPIO_DIGITAL_INPUT_PULLUP:
            pinMode(pin, INPUT_PULLUP);
            break;

        case PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN:
            pinMode(pin, INPUT_PULLDOWN);
            break;

        case PeripheralType::GPIO_DIGITAL_OUTPUT:
            pinMode(pin, OUTPUT);
            writePin(config.id, config.params.gpio.initialState);
            break;

        case PeripheralType::GPIO_ANALOG_INPUT:
            // ESP32模拟输入不需要特殊设置
            break;

        case PeripheralType::GPIO_PWM_OUTPUT:
        case PeripheralType::GPIO_ANALOG_OUTPUT:
        case PeripheralType::PWM_SERVO:
            return setupPWMPin(config);

        default:
            break;
    }

    return true;
}

bool PeripheralManager::setupPWMPin(const PeripheralConfig& config) {
    uint8_t pin = config.getPrimaryPin();
    if (pin == 255) return false;
    if (!isValidPin(pin)) {
        LOG_ERRORF("Peripheral Manager: setupPWMPin '%s' reject invalid pin %u on %s",
                   config.id.c_str(), (unsigned)pin, CHIP_NAME);
        return false;
    }

    uint8_t channel = config.params.gpio.pwmChannel;
    if (channel >= CHIP_MAX_PWM_CH) {
        LOG_ERRORF("Peripheral Manager: Invalid PWM channel %d (max: %d)", channel, CHIP_MAX_PWM_CH - 1);
        return false;
    }

    ledcAttach(pin, config.params.gpio.pwmFrequency, config.params.gpio.pwmResolution);

    // 设置初始值
    if (config.params.gpio.initialState == GPIOState::STATE_HIGH) {
        ledcWrite(pin, (1 << config.params.gpio.pwmResolution) - 1);
    } else {
        ledcWrite(pin, 0);
    }

    return true;
}

HardwareSerial* PeripheralManager::getUartSerial(const String& id) {
    auto it = uartPortById.find(id);
    if (it == uartPortById.end()) return nullptr;
    return serialForPort(it->second);
}

bool PeripheralManager::setupUartHardware(const PeripheralConfig& config) {
    if (config.pinCount < 2) {
        LOG_WARNINGF("Peripheral Manager: UART '%s' requires RX/TX pins", config.id.c_str());
        return false;
    }

    if (isProtocolOwnedUartId(config.id)) {
        LOG_INFOF("Peripheral Manager: UART '%s' reserved for protocol stack, skip generic serial init",
                  config.id.c_str());
        return true;
    }

    uint8_t rxPin = config.pins[0];
    uint8_t txPin = config.pins[1];
    uint8_t port = UART_PORT_UNASSIGNED;

    auto existing = uartPortById.find(config.id);
    if (existing != uartPortById.end()) {
        port = existing->second;
    } else if (isUsbSerialPins(rxPin, txPin)) {
        port = 0;
    } else {
        bool used[CHIP_UART_COUNT] = {false};
        used[0] = true; // UART0 is shared with USB/logging.
        for (const auto& pair : uartPortById) {
            if (pair.second < CHIP_UART_COUNT) used[pair.second] = true;
        }
        for (uint8_t i = 1; i < CHIP_UART_COUNT; i++) {
            if (!used[i]) {
                port = i;
                break;
            }
        }
    }

    HardwareSerial* serial = serialForPort(port);
    if (!serial) {
        LOG_WARNINGF("Peripheral Manager: No free UART port for '%s'", config.id.c_str());
        return false;
    }

    uartPortById[config.id] = port;
    if (port == 0) {
        LOG_INFOF("Peripheral Manager: UART '%s' mapped to Serial0 (USB/log shared)", config.id.c_str());
        return true;
    }

    uint32_t baud = config.params.uart.baudRate > 0 ? config.params.uart.baudRate : 115200;
    serial->begin(baud, uartSerialConfig(config), rxPin, txPin);
    serial->setTimeout(0);
    LOG_INFOF("Peripheral Manager: UART '%s' initialized on Serial%d RX=%d TX=%d baud=%lu",
              config.id.c_str(), port, rxPin, txPin, (unsigned long)baud);
    return true;
}

// ========== 动作定时器管理 ==========
// ActionTickerData 生命周期：
//   - 创建：startActionTicker() 中 new，指针存储在 actionTickers map 中
//   - 释放：stopActionTicker() 中先 ticker.detach() 再 delete，然后从 map 移除
//   - Ticker 回调（blinkTickerCallback / breatheTickerCallback）仅引用 data 指针，不负责释放
//   - startActionTicker 开头调用 stopActionTicker 确保不会重复创建

void PeripheralManager::stopActionTicker(const String& id) {
    RecursiveMutexGuard lock(_mutex);
    auto it = actionTickers.find(id);
    if (it != actionTickers.end()) {
        it->second->ticker.detach();
        delete it->second;
        actionTickers.erase(it);
    }
}

// Ticker 回调：非阻塞尝试加锁，获取失败则跳过本次（下次重试）
static void blinkTickerCallback(PeripheralManager::ActionTickerData* data) {
    if (!data || !data->mgr) return;
    SemaphoreHandle_t mtx = data->mgr->getMutex();
    if (!mtx || xSemaphoreTakeRecursive(mtx, 0) != pdTRUE) return;
    data->mgr->togglePin(data->id);
    xSemaphoreGiveRecursive(mtx);
}

static void breatheTickerCallback(PeripheralManager::ActionTickerData* data) {
    if (!data || !data->mgr) return;
    SemaphoreHandle_t mtx = data->mgr->getMutex();
    if (!mtx || xSemaphoreTakeRecursive(mtx, 0) != pdTRUE) return;

    int16_t current = data->breatheState;
    bool increasing = (current >= 0);
    uint16_t duty = increasing ? current : (-current);

    if (increasing) {
        duty += data->stepSize;
        if (duty >= data->maxDuty) { duty = data->maxDuty; data->breatheState = -duty; }
        else { data->breatheState = duty; }
    } else {
        if (duty < data->stepSize) { duty = 0; data->breatheState = 0; }
        else { duty -= data->stepSize; data->breatheState = -duty; }
    }
    ledcWrite(data->channel, duty);
    xSemaphoreGiveRecursive(mtx);
}

void PeripheralManager::startActionTicker(const String& id, uint8_t actionMode, uint16_t paramValue) {
    RecursiveMutexGuard lock(_mutex);
    stopActionTicker(id);  // 先清理已有的

    auto config = getPeripheral(id);
    if (!config) return;

    if (actionMode == 1) {  // BLINK
        ActionTickerData* data = new ActionTickerData();
        data->mgr = this;
        data->id = id;
        actionTickers[id] = data;

        float intervalSec = paramValue / 1000.0f;
        data->ticker.attach(intervalSec, blinkTickerCallback, data);
        LOG_INFOF("Peripheral Manager: Blink ticker started for '%s' (interval=%dms)",
                  id.c_str(), paramValue);
    }
    else if (actionMode == 2) {  // BREATHE
        ActionTickerData* data = new ActionTickerData();
        data->mgr = this;
        data->id = id;
        data->channel = config->params.gpio.pwmChannel;
        data->maxDuty = (1 << config->params.gpio.pwmResolution) - 1;
        data->breatheState = 0;

        uint16_t speedMs = paramValue;
        uint16_t steps = speedMs / 40;  // 半周期步数
        if (steps == 0) steps = 1;
        data->stepSize = data->maxDuty / steps;
        if (data->stepSize == 0) data->stepSize = 1;

        actionTickers[id] = data;
        data->ticker.attach_ms(20, breatheTickerCallback, data);
        LOG_INFOF("Peripheral Manager: Breathe ticker started for '%s' (speed=%dms)",
                  id.c_str(), speedMs);
    }
}

bool PeripheralManager::stopStepper(const String& id) {
    RecursiveMutexGuard lock(_mutex);
    auto it = stepperTickers.find(id);
    if (it != stepperTickers.end()) {
        it->second->ticker.detach();
        it->second->running = false;
        it->second->direction = 0;
    }

    PeripheralConfig* config = getPeripheral(id);
    if (!config || config->type != PeripheralType::STEPPER_MOTOR) return false;
    bool safeRelease = config->pinCount >= 4;
    for (uint8_t i = 0; safeRelease && i < 4; i++) {
        if (isReservedPin(config->pins[i])) safeRelease = false;
    }
    if (safeRelease) releaseStepperCoils(*config);
    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].status = PeripheralStatus::PERIPHERAL_INITIALIZED;
        runtimeStates[id].lastActivity = millis();
    }
    LOG_INFOF("Peripheral Manager: Stepper '%s' stopped", id.c_str());
    return true;
}

bool PeripheralManager::controlStepper(const String& id, const String& action, int value) {
    RecursiveMutexGuard lock(_mutex);
    PeripheralConfig* config = getPeripheral(id);
    if (!config || config->type != PeripheralType::STEPPER_MOTOR) {
        LOG_WARNINGF("Peripheral Manager: Stepper control target invalid: %s", id.c_str());
        return false;
    }
    if (!config->enabled) {
        LOG_WARNINGF("Peripheral Manager: Stepper '%s' is disabled", id.c_str());
        return false;
    }
    if (config->pinCount < 4) {
        LOG_WARNINGF("Peripheral Manager: Stepper '%s' requires 4 pins", id.c_str());
        return false;
    }

    String cmd = action;
    cmd.trim();
    cmd.toLowerCase();
    uint16_t rpm = clampStepperSpeed(config->params.stepper.speed == 0 ? STEPPER_DEFAULT_RPM : config->params.stepper.speed);
    uint16_t steps = config->params.stepper.stepsPerRevolution == 0 ? STEPPER_DEFAULT_STEPS_PER_REV : config->params.stepper.stepsPerRevolution;

    if (cmd == "stop" || cmd == "off" || cmd == "idle") {
        return stopStepper(id);
    }

    if (cmd == "setspeed" || cmd == "speed") {
        if (value <= 0) value = rpm;
        config->params.stepper.speed = clampStepperSpeed((uint16_t)value);
        rpm = config->params.stepper.speed;
    } else if (cmd == "faster" || cmd == "accelerate" || cmd == "speedup" || cmd == "inc") {
        uint16_t delta = value > 0 ? (uint16_t)value : 2;
        config->params.stepper.speed = clampStepperSpeed((uint16_t)(rpm + delta));
        rpm = config->params.stepper.speed;
    } else if (cmd == "slower" || cmd == "decelerate" || cmd == "speeddown" || cmd == "dec") {
        uint16_t delta = value > 0 ? (uint16_t)value : 2;
        config->params.stepper.speed = (rpm > delta) ? clampStepperSpeed((uint16_t)(rpm - delta)) : STEPPER_MIN_RPM;
        rpm = config->params.stepper.speed;
    }

    int8_t direction = 0;
    if (cmd == "forward" || cmd == "cw" || cmd == "start") {
        direction = 1;
    } else if (cmd == "reverse" || cmd == "backward" || cmd == "ccw") {
        direction = -1;
    } else if (cmd == "direction") {
        direction = (value < 0) ? -1 : 1;
    }

    auto it = stepperTickers.find(id);
    StepperTickerData* data = (it != stepperTickers.end()) ? it->second : nullptr;
    if (!data) {
        if (ESP.getFreeHeap() < 4096) {
            LOG_WARNINGF("Peripheral Manager: Not enough heap to start stepper '%s'", id.c_str());
            return false;
        }
        data = new StepperTickerData();
        data->mgr = this;
        data->id = id;
        data->phase = 0;
        data->direction = 0;
        data->rpm = rpm;
        data->stepsPerRev = steps;
        data->running = false;
        stepperTickers[id] = data;
    }

    data->ticker.detach();
    data->rpm = rpm;
    data->stepsPerRev = steps;
    if (direction != 0) {
        data->direction = direction;
        data->running = true;
    }

    if (!data->running || data->direction == 0) {
        LOG_INFOF("Peripheral Manager: Stepper '%s' speed set to %d rpm", id.c_str(), rpm);
        return true;
    }

    uint32_t interval = stepperIntervalMs(data->rpm, data->stepsPerRev);
    data->ticker.attach_ms(interval, stepperTickerCallback, data);
    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].status = PeripheralStatus::PERIPHERAL_RUNNING;
        runtimeStates[id].lastActivity = millis();
    }
    LOG_INFOF("Peripheral Manager: Stepper '%s' %s speed=%d rpm interval=%lums",
              id.c_str(), data->direction > 0 ? "forward" : "reverse", data->rpm, interval);
    return true;
}

bool PeripheralManager::controlNeoPixel(const String& id, const String& action, const String& value) {
    RecursiveMutexGuard lock(_mutex);
    PeripheralConfig* config = getPeripheral(id);
    if (!config || config->type != PeripheralType::NEO_PIXEL) {
        LOG_WARNINGF("Peripheral Manager: NeoPixel control target invalid: %s", id.c_str());
        return false;
    }
    if (!config->enabled) {
        LOG_WARNINGF("Peripheral Manager: NeoPixel '%s' is disabled", id.c_str());
        return false;
    }
    if (config->pinCount < 1) {
        LOG_WARNINGF("Peripheral Manager: NeoPixel '%s' requires data pin", id.c_str());
        return false;
    }
    uint8_t pin = config->pins[0];
    if (isReservedPin(pin) || isInputOnlyPin(pin)) {
        LOG_WARNINGF("Peripheral Manager: NeoPixel '%s' rejects GPIO%d", id.c_str(), pin);
        return false;
    }

    String cmd = action;
    cmd.trim();
    cmd.toLowerCase();
    String rawValue = value;
    rawValue.trim();

    if (cmd == "brightness") {
        int b = rawValue.toInt();
        config->params.neopixel.brightness = clampNeoPixelBrightness(b < 0 ? 0 : (uint16_t)b);
        LOG_INFOF("Peripheral Manager: NeoPixel '%s' brightness=%d",
                  id.c_str(), config->params.neopixel.brightness);
        return true;
    }

    NeoPixelRgb color = {0, 0, 0};
    if (cmd == "rainbow" || cmd == "cycle" || cmd == "next") {
        uint8_t& index = neopixelRainbowIndex[id];
        color = rainbowColorAt(index++);
    } else if (cmd == "off" || cmd == "clear" || cmd == "black") {
        color = {0, 0, 0};
    } else {
        String colorText = rawValue.isEmpty() ? cmd : rawValue;
        if (cmd == "color" || cmd == "set" || cmd == "fill") {
            colorText = rawValue;
        }
        if (!parseNeoPixelColor(colorText, color)) {
            LOG_WARNINGF("Peripheral Manager: NeoPixel '%s' unknown color/action '%s' value='%s'",
                         id.c_str(), action.c_str(), value.c_str());
            return false;
        }
    }

    uint8_t brightness = config->params.neopixel.brightness;
    color = applyNeoPixelBrightness(color, brightness);
    uint16_t count = clampNeoPixelCount(config->params.neopixel.count);
    bool ok = writeNeoPixelSolid(pin, count, color);
    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].status = ok ? PeripheralStatus::PERIPHERAL_RUNNING : PeripheralStatus::PERIPHERAL_ERROR;
        runtimeStates[id].lastActivity = millis();
        if (!ok) runtimeStates[id].errorCount++;
    }
    LOG_INFOF("Peripheral Manager: NeoPixel '%s' action=%s value=%s count=%d brightness=%d -> %s",
              id.c_str(), action.c_str(), value.c_str(), count, brightness, ok ? "ok" : "fail");
    return ok;
}

// ========== DAC 硬件初始化 ==========

bool PeripheralManager::setupDACPin(const PeripheralConfig& config) {
#if CHIP_HAS_DAC
    uint8_t pin = config.getPrimaryPin();
    if (pin == 255) return false;
    if (pin != 25 && pin != 26) {
        LOG_ERROR("Peripheral Manager: DAC only supports GPIO 25 and 26");
        return false;
    }
    dacWrite(pin, config.params.dac.defaultValue);
    LOG_INFOF("Peripheral Manager: DAC pin %d set to %d", pin, config.params.dac.defaultValue);
    return true;
#else
    LOG_WARNING("DAC not supported on this chip");
    return false;
#endif
}

String PeripheralManager::generateUniqueId(PeripheralType type) {
    static int counter = 0;
    const char* typeName = getPeripheralTypeName(type);
    return String(typeName) + "_" + String(millis()) + "_" + String(counter++);
}

void PeripheralManager::updatePinMapping(const String& id, const PeripheralConfig& config) {
    for (int i = 0; i < config.pinCount && i < 8; i++) {
        if (config.pins[i] != 255) {
            pinToPeripheral[config.pins[i]] = id;
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

    // 内部Flash使用的引脚（绝对禁止）- 检查是否为保留引脚
    for (uint8_t i = 0; i < CHIP_RESERVED_PIN_COUNT; i++) {
        if (CHIP_RESERVED_PINS[i] == pin) {
            // GPIO 6-11 是内部 Flash SPI，绝对禁止
            #if defined(CONFIG_IDF_TARGET_ESP32)
            if (pin >= 6 && pin <= 11) return false;
            #elif defined(CONFIG_IDF_TARGET_ESP32S3)
            // ESP32-S3: GPIO 26-32 是 Octal Flash/PSRAM
            if ((pin >= 19 && pin <= 20) || (pin >= 22 && pin <= 32)) return false;
            #elif defined(CONFIG_IDF_TARGET_ESP32C3)
            // ESP32-C3: GPIO 12-17 是 Flash SPI
            if (pin >= 12 && pin <= 17) return false;
            #elif defined(CONFIG_IDF_TARGET_ESP32C6)
            // ESP32-C6: GPIO 12-14 是 Flash SPI
            if (pin >= 12 && pin <= 14) return false;
            #else
            if (pin >= 6 && pin <= 11) return false;
            #endif
        }
    }

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
        for (uint8_t i = 0; i < cfg.pinCount && i < 8; i++) {
            if (cfg.pins[i] == pin) return true;
        }
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
        for (uint8_t i = 0; i < cfg.pinCount && i < 8; i++) {
            if (cfg.pins[i] != 255) {
                pinToPeripheral[cfg.pins[i]] = pair.first;
            }
        }
    }
}

// 获取引脚冲突详细信息
String PeripheralManager::getPinConflictInfo(uint8_t pin, const String& excludeId) const {
    // 检查是否为无效引脚
    if (!isValidPin(pin)) {
        return String("GPIO") + String(pin) + " 不是有效的 GPIO 引脚";
    }

    // 检查系统保留引脚
    String reservedReason = getPinReservedReason(pin);
    if (!reservedReason.isEmpty() && (pin >= 6 && pin <= 11)) {
        return String("GPIO") + String(pin) + ": " + reservedReason;
    }

    // 检查是否与现有外设冲突
    auto it = pinToPeripheral.find(pin);
    if (it != pinToPeripheral.end() && (excludeId.isEmpty() || it->second != excludeId)) {
        auto config = getPeripheral(it->second);
        if (config) {
            return String("GPIO") + String(pin) + " 已被外设 \"" + config->name + "\" (" + it->second + ") 使用";
        }
        return String("GPIO") + String(pin) + " 已被外设 " + it->second + " 使用";
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

    // 检查输入专用引脚
    if (isReservedPin(pin) &&
        (type == PeripheralType::GPIO_DIGITAL_OUTPUT ||
         type == PeripheralType::GPIO_ANALOG_OUTPUT ||
         type == PeripheralType::GPIO_PWM_OUTPUT ||
         type == PeripheralType::PWM_SERVO ||
         type == PeripheralType::DAC ||
         type == PeripheralType::NEO_PIXEL ||
         type == PeripheralType::SEVEN_SEGMENT_TM1637)) {
        errorMsg = String("GPIO") + String(pin) + " is reserved on this chip and cannot be used as an output";
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
                           type == PeripheralType::RF_MODULE);

        if (!isInputType) {
            errorMsg = String("GPIO") + String(pin) + " 只能用于输入模式，不能配置为 " + getPeripheralTypeName(type);
            return false;
        }
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
    // 处理ISR中断事件队列
    processInterruptQueue();
}

// 中断处理
void IRAM_ATTR PeripheralManager::isrHandler(void* arg) {
    // 中断上下文：仅记录触发的引脚号，通过队列传递到主循环
    uint8_t pin = (uint8_t)(uintptr_t)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (_isrQueue) {
        xQueueSendFromISR(_isrQueue, &pin, &xHigherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void PeripheralManager::processInterruptQueue() {
    if (!_isrQueue) return;
    uint8_t pin;
    // 一次性处理队列中所有待处理的中断事件
    while (xQueueReceive(_isrQueue, &pin, 0) == pdTRUE) {
        handleInterrupt(pin);
    }
}

void PeripheralManager::handleInterrupt(uint8_t pin) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return;

    auto config = getPeripheral(id);
    if (!config) return;

    if (config->params.gpio.interruptCallback) {
        GPIOState state = readPin(id);
        config->params.gpio.interruptCallback(pin, state);
    }
}

bool PeripheralManager::sendRfCode(const String& id, const String& codeText,
                                   uint8_t bitLength, uint16_t pulseWidth, uint8_t repeat) {
    RecursiveMutexGuard lock(_mutex);
    PeripheralConfig* config = getPeripheral(id);
    if (!config || !config->enabled || config->type != PeripheralType::RF_MODULE) {
        LOG_WARNINGF("RF send: peripheral '%s' not found, disabled, or not RF_MODULE", id.c_str());
        return false;
    }
    if (config->params.rf.mode != RF_MODE_TX) {
        LOG_WARNINGF("RF send: peripheral '%s' is not configured as TX", id.c_str());
        return false;
    }
    if (config->pinCount < 1 || config->pins[0] == 255) {
        LOG_WARNINGF("RF send: peripheral '%s' has no data pin", id.c_str());
        return false;
    }

    uint32_t code = 0;
    uint8_t inferredBits = 0;
    if (!parseRfCodeText(codeText, code, inferredBits)) {
        LOG_WARNINGF("RF send: invalid code '%s'", codeText.c_str());
        return false;
    }

    uint8_t bits = bitLength ? bitLength : (inferredBits ? inferredBits : config->params.rf.bitLength);
    bits = clampRfBitLength(bits);
    uint16_t pulse = clampRfPulseWidth(pulseWidth ? pulseWidth : config->params.rf.pulseWidth);
    uint8_t reps = clampRfRepeat(repeat ? repeat : config->params.rf.repeat);
    uint8_t pin = config->pins[0];
    const bool activeHigh = config->params.rf.activeHigh;
    const uint8_t onLevel = activeHigh ? HIGH : LOW;
    const uint8_t offLevel = activeHigh ? LOW : HIGH;

    auto sendPulse = [&](uint8_t highPulses, uint8_t lowPulses) {
        digitalWrite(pin, onLevel);
        delayMicroseconds((uint32_t)pulse * highPulses);
        digitalWrite(pin, offLevel);
        delayMicroseconds((uint32_t)pulse * lowPulses);
    };

    for (uint8_t r = 0; r < reps; r++) {
        sendPulse(1, 31); // sync
        for (int8_t bit = (int8_t)bits - 1; bit >= 0; bit--) {
            if ((code >> bit) & 0x1) {
                sendPulse(3, 1);
            } else {
                sendPulse(1, 3);
            }
        }
        digitalWrite(pin, offLevel);
        delay(1);
        yield();
    }

    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
        runtimeStates[id].state.comm.bytesSent += 4;
    }
    LOG_INFOF("RF send: '%s' code=0x%lX bits=%u pulse=%uus repeat=%u",
              id.c_str(), (unsigned long)code, bits, (unsigned)pulse, reps);
    return true;
}

bool PeripheralManager::readRfLevel(const String& id, bool& level) {
    RecursiveMutexGuard lock(_mutex);
    PeripheralConfig* config = getPeripheral(id);
    if (!config || !config->enabled || config->type != PeripheralType::RF_MODULE) {
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

bool PeripheralManager::readRadarState(const String& id, bool& detected) {
    RecursiveMutexGuard lock(_mutex);
    PeripheralConfig* config = getPeripheral(id);
    if (!config || !config->enabled || config->type != PeripheralType::RADAR_SENSOR) {
        return false;
    }
    if (config->pinCount < 1 || config->pins[0] == 255) {
        return false;
    }
    bool physicalHigh = digitalRead(config->pins[0]) == HIGH;
    detected = config->params.radar.activeHigh ? physicalHigh : !physicalHigh;
    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
        runtimeStates[id].state.gpio.currentState = detected ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
    }
    return true;
}

// ========== 通用数据读写接口 ==========

bool PeripheralManager::writeData(const String& id, const uint8_t* data, size_t len) {
    auto config = getPeripheral(id);
    if (!config || !config->enabled) {
        LOG_WARNINGF("writeData: Peripheral '%s' not found or disabled", id.c_str());
        return false;
    }

    bool success = false;

    switch (config->type) {
        // GPIO 数字输出
        case PeripheralType::GPIO_DIGITAL_OUTPUT:
            if (len >= 1) {
                GPIOState state = (data[0] != 0) ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
                success = writePin(id, state);
            }
            break;

        // PWM / 模拟输出
        case PeripheralType::GPIO_PWM_OUTPUT:
        case PeripheralType::GPIO_ANALOG_OUTPUT:
        case PeripheralType::PWM_SERVO:
            if (len >= 2) {
                uint32_t dutyCycle = data[0] | (data[1] << 8);
                if (len >= 4) {
                    dutyCycle |= (data[2] << 16) | (data[3] << 24);
                }
                success = writePWM(id, dutyCycle);
            } else if (len == 1) {
                // 单字节作为 0-255 占空比
                uint32_t maxVal = (1U << config->params.gpio.pwmResolution) - 1;
                uint32_t dutyCycle = (data[0] * maxVal) / 255;
                success = writePWM(id, dutyCycle);
            }
            break;

        // DAC 输出
        case PeripheralType::DAC:
#if CHIP_HAS_DAC
            if (len >= 1) {
                uint8_t pin = config->getPrimaryPin();
                if (pin == 25 || pin == 26) {
                    dacWrite(pin, data[0]);
                    success = true;
                }
            }
#else
            LOG_WARNING("DAC not supported on this chip");
#endif
            break;

        // UART 发送
        case PeripheralType::UART:
            if (HardwareSerial* serial = getUartSerial(id)) {
                success = (serial->write(data, len) == len);
            }
            break;

        // I2C 写入
        case PeripheralType::I2C:
            if (config->params.i2c.isMaster && config->params.i2c.address > 0) {
                Wire.beginTransmission(config->params.i2c.address);
                Wire.write(data, len);
                success = (Wire.endTransmission() == 0);
            }
            break;

        // SPI 传输
        case PeripheralType::SPI:
            SPI.beginTransaction(SPISettings(config->params.spi.frequency,
                config->params.spi.msbFirst ? MSBFIRST : LSBFIRST,
                config->params.spi.mode));
            for (size_t i = 0; i < len; i++) {
                SPI.transfer(data[i]);
            }
            SPI.endTransaction();
            success = true;
            break;

        case PeripheralType::RF_MODULE:
            if (len > 0) {
                if (isPrintablePayload(data, len)) {
                    String code((const char*)data, len);
                    success = sendRfCode(id, code);
                } else {
                    uint32_t code = 0;
                    size_t n = len > 4 ? 4 : len;
                    for (size_t i = 0; i < n; i++) {
                        code |= ((uint32_t)data[i]) << (i * 8);
                    }
                    success = sendRfCode(id, String(code));
                }
            }
            break;

        default:
            LOG_WARNINGF("writeData: Unsupported peripheral type %d", static_cast<int>(config->type));
            break;
    }

    // 更新运行时状态
    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
        if (config->type != PeripheralType::RF_MODULE) {
            runtimeStates[id].state.comm.bytesSent += len;
        }
    }

    return success;
}

bool PeripheralManager::readData(const String& id, uint8_t* buffer, size_t& len) {
    auto config = getPeripheral(id);
    if (!config || !config->enabled) {
        LOG_WARNINGF("readData: Peripheral '%s' not found or disabled", id.c_str());
        len = 0;
        return false;
    }

    bool success = false;
    size_t maxLen = len;
    len = 0;

    switch (config->type) {
        // GPIO 数字输入
        case PeripheralType::GPIO_DIGITAL_INPUT:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLUP:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN:
        case PeripheralType::GPIO_TOUCH:
            if (maxLen >= 1) {
                GPIOState state = readPin(id);
                buffer[0] = (state == GPIOState::STATE_HIGH) ? 1 : 0;
                len = 1;
                success = true;
            }
            break;

        // ADC / 模拟输入
        case PeripheralType::GPIO_ANALOG_INPUT:
        case PeripheralType::ADC:
            if (maxLen >= 2) {
                uint16_t value = readAnalog(id);
                buffer[0] = value & 0xFF;
                buffer[1] = (value >> 8) & 0xFF;
                len = 2;
                success = true;
            }
            break;

        // UART 接收
        case PeripheralType::UART:
            if (HardwareSerial* serial = getUartSerial(id)) {
                len = 0;
                while (serial->available() && len < maxLen) {
                    buffer[len++] = serial->read();
                }
                success = true;
            }
            break;

        // I2C 读取
        case PeripheralType::I2C:
            if (config->params.i2c.isMaster && config->params.i2c.address > 0) {
                size_t requestLen = (maxLen < 32) ? maxLen : 32;  // I2C 一次最多读取 32 字节
                Wire.requestFrom(config->params.i2c.address, (uint8_t)requestLen);
                len = 0;
                while (Wire.available() && len < maxLen) {
                    buffer[len++] = Wire.read();
                }
                success = (len > 0);
            }
            break;

        // SPI 读取（需要先发送才能读取）
        case PeripheralType::SPI:
            SPI.beginTransaction(SPISettings(config->params.spi.frequency,
                config->params.spi.msbFirst ? MSBFIRST : LSBFIRST,
                config->params.spi.mode));
            for (size_t i = 0; i < maxLen; i++) {
                buffer[i] = SPI.transfer(0xFF);  // 发送 dummy 字节读取数据
            }
            SPI.endTransaction();
            len = maxLen;
            success = true;
            break;

        case PeripheralType::RADAR_SENSOR:
            if (maxLen >= 1) {
                bool detected = false;
                success = readRadarState(id, detected);
                if (success) {
                    buffer[0] = detected ? 1 : 0;
                    len = 1;
                }
            }
            break;

        case PeripheralType::RF_MODULE:
            if (maxLen >= 1) {
                bool level = false;
                success = readRfLevel(id, level);
                if (success) {
                    buffer[0] = level ? 1 : 0;
                    len = 1;
                }
            }
            break;

        default:
            LOG_WARNINGF("readData: Unsupported peripheral type %d", static_cast<int>(config->type));
            break;
    }

    // 更新运行时状态
    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
        runtimeStates[id].state.comm.bytesReceived += len;
    }

    return success;
}

bool PeripheralManager::writeString(const String& id, const String& data) {
    return writeData(id, (const uint8_t*)data.c_str(), data.length());
}

String PeripheralManager::readString(const String& id) {
    uint8_t buffer[256];
    size_t len = sizeof(buffer);
    if (readData(id, buffer, len)) {
        return String((char*)buffer, len);
    }
    return "";
}

bool PeripheralManager::attachInterrupt(uint8_t pin, GPIOInterruptCallback callback) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return false;
    return attachInterrupt(id, callback);
}

bool PeripheralManager::attachInterrupt(const String& peripheralId, GPIOInterruptCallback callback) {
    auto config = getPeripheral(peripheralId);
    if (!config || !config->isGPIOPeripheral()) return false;

    uint8_t pin = config->getPrimaryPin();
    if (pin == 255) return false;

    config->params.gpio.interruptCallback = callback;

    uint8_t mode = CHANGE;
    if (config->type == PeripheralType::GPIO_INTERRUPT_RISING) mode = RISING;
    else if (config->type == PeripheralType::GPIO_INTERRUPT_FALLING) mode = FALLING;

    ::attachInterruptArg(digitalPinToInterrupt(pin), isrHandler, (void*)(uintptr_t)pin, mode);

    if (runtimeStates.find(peripheralId) != runtimeStates.end()) {
        runtimeStates[peripheralId].state.gpio.interruptAttached = true;
    }

    return true;
}

bool PeripheralManager::detachInterrupt(uint8_t pin) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return false;
    return detachInterrupt(id);
}

bool PeripheralManager::detachInterrupt(const String& peripheralId) {
    auto config = getPeripheral(peripheralId);
    if (!config || !config->isGPIOPeripheral()) return false;

    uint8_t pin = config->getPrimaryPin();
    if (pin == 255) return false;

    ::detachInterrupt(digitalPinToInterrupt(pin));
    config->params.gpio.interruptCallback = nullptr;

    if (runtimeStates.find(peripheralId) != runtimeStates.end()) {
        runtimeStates[peripheralId].state.gpio.interruptAttached = false;
    }

    return true;
}

// ========== Modbus 外设委托 ==========

void PeripheralManager::setModbusCallbacks(ModbusCoilWriteFunc coilWrite, ModbusRegWriteFunc regWrite) {
    RecursiveMutexGuard lock(_mutex);
    _modbusCoilWrite = coilWrite;
    _modbusRegWrite  = regWrite;
    LOG_INFO("Peripheral Manager: Modbus write callbacks registered");
}

void PeripheralManager::clearModbusCallbacks() {
    RecursiveMutexGuard lock(_mutex);
    _modbusCoilWrite = nullptr;
    _modbusRegWrite  = nullptr;
    LOG_INFO("Peripheral Manager: Modbus write callbacks cleared");
}

bool PeripheralManager::writeModbusPin(const String& id, const PeripheralConfig& config, GPIOState state) {
    if (!_modbusCoilWrite && !_modbusRegWrite) {
        LOG_WARNING("Peripheral Manager: Modbus write callbacks not set");
        return false;
    }

    bool value = (state == GPIOState::STATE_HIGH);
    if (config.params.modbus.ncMode) value = !value;

    uint16_t addr = config.params.modbus.coilBase;
    bool success = false;

    if (config.params.modbus.controlProtocol == 0) {
        // 线圈模式 (FC05)
        if (_modbusCoilWrite) {
            success = _modbusCoilWrite(config.params.modbus.slaveAddress, addr, value);
        }
    } else {
        // 寄存器模式 (FC06)
        uint16_t regVal = value ? 0xFF00 : 0x0000;
        if (_modbusRegWrite) {
            success = _modbusRegWrite(config.params.modbus.slaveAddress, addr, regVal);
        }
    }

    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].state.gpio.currentState = state;
        runtimeStates[id].lastActivity = millis();
    }

    LOG_INFOF("Peripheral Manager: Modbus writePin '%s' slave=%d addr=%d state=%s %s",
              id.c_str(), config.params.modbus.slaveAddress, addr,
              state == GPIOState::STATE_HIGH ? "HIGH" : "LOW",
              success ? "OK" : "FAIL");
    return success;
}

bool PeripheralManager::writeModbusPWM(const String& id, const PeripheralConfig& config, uint32_t dutyCycle) {
    if (!_modbusRegWrite) {
        LOG_WARNING("Peripheral Manager: Modbus register write callback not set");
        return false;
    }

    // PWM 类型设备：写入 pwmRegBase 寄存器
    if (config.params.modbus.deviceType != 1) {
        LOG_WARNINGF("Peripheral Manager: '%s' is not a PWM Modbus device", id.c_str());
        return false;
    }

    uint16_t regAddr = config.params.modbus.pwmRegBase;
    uint16_t regVal = (uint16_t)dutyCycle;
    bool success = _modbusRegWrite(config.params.modbus.slaveAddress, regAddr, regVal);

    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
    }

    LOG_INFOF("Peripheral Manager: Modbus writePWM '%s' slave=%d reg=%d duty=%d %s",
              id.c_str(), config.params.modbus.slaveAddress, regAddr, (int)dutyCycle,
              success ? "OK" : "FAIL");
    return success;
}

bool PeripheralManager::writeModbusCoil(const String& id, uint16_t coilAddr, bool value) {
    RecursiveMutexGuard lock(_mutex);
    if (!_modbusCoilWrite) {
        LOG_WARNING("Peripheral Manager: Modbus coil write callback not set");
        return false;
    }

    auto config = getPeripheral(id);
    if (!config || !config->isModbusPeripheral()) {
        LOG_WARNINGF("Peripheral Manager: '%s' is not a Modbus peripheral", id.c_str());
        return false;
    }

    bool success = _modbusCoilWrite(config->params.modbus.slaveAddress, coilAddr, value);

    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
    }

    LOG_INFOF("Peripheral Manager: Modbus writeCoil '%s' slave=%d coil=%d val=%d %s",
              id.c_str(), config->params.modbus.slaveAddress, coilAddr, value,
              success ? "OK" : "FAIL");
    return success;
}

bool PeripheralManager::writeModbusReg(const String& id, uint16_t regAddr, uint16_t value) {
    RecursiveMutexGuard lock(_mutex);
    if (!_modbusRegWrite) {
        LOG_WARNING("Peripheral Manager: Modbus register write callback not set");
        return false;
    }

    auto config = getPeripheral(id);
    if (!config || !config->isModbusPeripheral()) {
        LOG_WARNINGF("Peripheral Manager: '%s' is not a Modbus peripheral", id.c_str());
        return false;
    }

    ModbusMotorLimitResult motorLimit = evaluateModbusMotorLimit(*config, regAddr, value);
    if (motorLimit.enabled && !motorLimit.allowed) {
        LOG_WARNINGF("Peripheral Manager: Modbus motor '%s' blocked by soft limit (pos=%ld min=%ld max=%ld reg=%u)",
                     id.c_str(),
                     static_cast<long>(config->params.modbus.motorCurrentPosition),
                     static_cast<long>(config->params.modbus.motorMinPosition),
                     static_cast<long>(config->params.modbus.motorMaxPosition),
                     static_cast<unsigned int>(regAddr));
        return false;
    }
    if (motorLimit.enabled && motorLimit.isMove && config->params.modbus.motorRegs[4] != 0) {
        bool pulseOk = _modbusRegWrite(config->params.modbus.slaveAddress,
                                       config->params.modbus.motorRegs[4],
                                       motorLimit.pulse);
        if (!pulseOk) {
            LOG_WARNINGF("Peripheral Manager: Modbus motor '%s' failed to write bounded pulse=%u",
                         id.c_str(),
                         static_cast<unsigned int>(motorLimit.pulse));
            return false;
        }
    }

    bool success = _modbusRegWrite(config->params.modbus.slaveAddress, regAddr, value);

    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
    }
    if (success && config->params.modbus.deviceType == 3) {
        if (regAddr == config->params.modbus.motorRegs[4]) {
            config->params.modbus.motorLastPulse = value;
        } else if (motorLimit.enabled && motorLimit.isMove) {
            config->params.modbus.motorCurrentPosition = motorLimit.nextPosition;
            config->params.modbus.motorLastPulse = motorLimit.pulse;
        }
    }

    LOG_INFOF("Peripheral Manager: Modbus writeReg '%s' slave=%d reg=%d val=%d %s",
              id.c_str(), config->params.modbus.slaveAddress, regAddr, value,
              success ? "OK" : "FAIL");
    return success;
}
