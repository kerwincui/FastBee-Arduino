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
#include "core/MemoryBudget.h"
#include "core/FeatureBudget.h"
#include "core/SensorDriverCache.h"
#include "utils/FileUtils.h"
#include "systems/LoggerSystem.h"
#include "peripherals/SDCardManager.h"
#include <driver/gpio.h>
#include <esp_heap_caps.h>
// WS2812 NeoPixel 支持：使用 Adafruit_NeoPixel 库（跨芯片兼容）
#if FASTBEE_ENABLE_NEOPIXEL
#include <Adafruit_NeoPixel.h>
#include "core/RuntimeFeatureManager.h"
#endif
#if FASTBEE_ENABLE_LCD
#include "peripherals/LCDManager.h"
#endif
#if FASTBEE_ENABLE_SEVEN_SEGMENT
#include "peripherals/SevenSegmentDriver.h"
#endif
#if FASTBEE_ENABLE_IR_REMOTE
#include "peripherals/drivers/IRRemoteDriver.h"
#endif
#if FASTBEE_ENABLE_RF_LEARN
#include "peripherals/drivers/RFLearnDriver.h"
#endif
#if FASTBEE_ENABLE_DS1302
#include "peripherals/drivers/DS1302Driver.h"
#endif
#if FASTBEE_ENABLE_LCD1602
#include "peripherals/drivers/LCD1602Driver.h"
#endif
#if FASTBEE_ENABLE_RFID
#include "peripherals/drivers/RFIDDriver.h"
#endif
#include <Wire.h>
#include <SPI.h>
#include <new>
#include <cstdlib>
#include <freertos/queue.h>

// 静态成员定义：ISR 中断事件队列
QueueHandle_t PeripheralManager::_isrQueue = nullptr;
// 静态成员定义：ISR 队列溢出计数（队列满时丢弃的事件数）
volatile uint32_t PeripheralManager::_isrQueueOverruns = 0;

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
constexpr uint32_t SENSOR_DEFAULT_SAMPLE_INTERVAL_MS = 1000;
constexpr uint32_t SENSOR_MIN_SAMPLE_INTERVAL_MS = 100;
constexpr uint32_t SENSOR_MAX_SAMPLE_INTERVAL_MS = 86400000UL;
constexpr uint32_t SERVO_RELEASE_DELAY_MS = 500;

// Optional drivers are the first recovery targets because they may retain
// display/RMT buffers or bus-driver state. GPIO, ADC and UART remain available
// so the device can still be diagnosed and controlled while memory recovers.
bool isMemoryOptionalPeripheralType(PeripheralType type) {
    switch (type) {
        case PeripheralType::LCD:
        case PeripheralType::LCD1602:
        case PeripheralType::NEO_PIXEL:
        case PeripheralType::SEVEN_SEGMENT_TM1637:
        case PeripheralType::IR_RECEIVER:
        case PeripheralType::DS1302_RTC:
        case PeripheralType::STEPPER_MOTOR:
        case PeripheralType::CAMERA:
        case PeripheralType::SDIO:
            return true;
        default:
            return false;
    }
}

bool isAdcInputType(PeripheralType type) {
    return type == PeripheralType::ADC || type == PeripheralType::GPIO_ANALOG_INPUT;
}

uint8_t normalizeAdcSampleCount(int value, uint8_t fallback = 1) {
    if (value <= 0) value = fallback > 0 ? fallback : 1;
    if (value > 16) value = 16;
    return static_cast<uint8_t>(value);
}

uint8_t normalizeAdcFilterMode(int value) {
    return value == 1 ? 1 : 0;
}

void initializeAdcDefaults(PeripheralConfig& config) {
    config.params.adc.attenuation = 3;
    config.params.adc.resolution = 12;
    config.params.adc.sampleRate = 0;
    config.params.adc.sampleCount = 1;
    config.params.adc.filterMode = 0;
    config.params.adc.signalMode = static_cast<uint8_t>(AnalogSignalMode::RAW);
    config.params.adc.conversionEnabled = false;
    config.params.adc.clampEnabled = false;
    config.params.adc.decimalPlaces = 2;
    config.params.adc.inputMin = 0.0f;
    config.params.adc.inputMax = 0.0f;
    config.params.adc.engineeringMin = 0.0f;
    config.params.adc.engineeringMax = 100.0f;
    config.params.adc.engineeringOffset = 0.0f;
    config.params.adc.vRef = 0.0f;
    config.params.adc.voltageRatio = 0.0f;
    config.params.adc.shuntResistance = 0.0f;
    config.params.adc.inputGain = 0.0f;
    config.params.adc.inputOffset = 0.0f;
    config.params.adc.rawMin = 0;
    config.params.adc.rawMax = 0;
}

// 将 AI 原始计数转换为工程量。该函数只做算术，不分配堆内存，便于在
// 外设执行轮询、协议上报和 Web 状态读取中复用。
float convertAnalogRaw(const PeripheralConfig& config, uint16_t raw,
                       AnalogInputQuality* quality = nullptr) {
    return convertAnalogInputRaw(config, raw, quality);
}

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
    const uint8_t dataBits = config.params.uart.dataBits;
    const uint8_t parity = config.params.uart.parity;
    const bool twoStopBits = config.params.uart.stopBits == 2;

    switch (dataBits) {
        case 5:
            if (parity == 1) return twoStopBits ? SERIAL_5O2 : SERIAL_5O1;
            if (parity == 2) return twoStopBits ? SERIAL_5E2 : SERIAL_5E1;
            return twoStopBits ? SERIAL_5N2 : SERIAL_5N1;
        case 6:
            if (parity == 1) return twoStopBits ? SERIAL_6O2 : SERIAL_6O1;
            if (parity == 2) return twoStopBits ? SERIAL_6E2 : SERIAL_6E1;
            return twoStopBits ? SERIAL_6N2 : SERIAL_6N1;
        case 7:
            if (parity == 1) return twoStopBits ? SERIAL_7O2 : SERIAL_7O1;
            if (parity == 2) return twoStopBits ? SERIAL_7E2 : SERIAL_7E1;
            return twoStopBits ? SERIAL_7N2 : SERIAL_7N1;
        case 8:
        default:
            if (parity == 1) return twoStopBits ? SERIAL_8O2 : SERIAL_8O1;
            if (parity == 2) return twoStopBits ? SERIAL_8E2 : SERIAL_8E1;
            return twoStopBits ? SERIAL_8N2 : SERIAL_8N1;
    }
}

uint16_t boundedUartAvailable(HardwareSerial* serial) {
    if (!serial) return 0;
    const int available = serial->available();
    if (available <= 0) return 0;
    return available > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(available);
}

bool isProtocolOwnedUartId(const String& id) {
    String lowered = id;
    lowered.toLowerCase();
    return lowered.startsWith("modbus") || lowered.startsWith("rs485");
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

    // 运行时功能门控（优先级最高）：用户可在 Web UI 禁用 neopixel
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("neopixel")) {
        return nullptr;
    }

    // 内存预算门控：编译启用但启动时 DRAM 不足则不创建灯带实例
    if (FastBee::FeatureBudgetRegistry::instance().isDenied("neopixel")) {
        return nullptr;
    }

    // G3: 按灯珠数动态计算所需 DRAM（每颗 3B GRB + RMT 缓冲 2KB + 安全余量 512B）
    size_t neededBytes = static_cast<size_t>(count) * 3 + 2048 + 512;
    size_t neededKB = (neededBytes + 1023) / 1024;
    uint32_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (free8 < neededKB * 1024) {
        LOGGER.warningf("[NeoPixel] Insufficient DRAM: need %uKB, free %luKB",
                        (unsigned)neededKB, (unsigned long)(free8 / 1024));
        return nullptr;
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
    if (FastBee::MemoryBudget::dramFree() < (count * 3 + 2048)) {
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
    _isrQueue = xQueueCreate(ISR_QUEUE_SIZE, sizeof(GpioIsrEvent));

    // 复位 ISR 诊断统计与去抖时间戳（initialize 可能被测试环境重复调用）
    _isrQueueOverruns = 0;
    memset(_lastIsrDispatch, 0, sizeof(_lastIsrDispatch));
    memset(_hasIsrDispatch, 0, sizeof(_hasIsrDispatch));
    _digitalInterruptHead = 0;
    _digitalInterruptTail = 0;
    _digitalInterruptCount = 0;

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

bool PeripheralManager::validateUartCapacity(const PeripheralConfig& candidate,
                                             const String& excludeId,
                                             String& errorMsg) const {
    if (candidate.type != PeripheralType::UART || !candidate.enabled ||
        isProtocolOwnedUartId(candidate.id)) {
        return true;
    }

#if CHIP_UART_COUNT <= 1
    errorMsg = "UART_RESOURCE_EXHAUSTED: UART0 reserved for logging/USB; no generic UART port available";
    return false;
#else
    size_t enabledGenericUarts = 0;
    for (const auto& pair : peripherals) {
        if (!excludeId.isEmpty() && pair.first == excludeId) continue;
        const PeripheralConfig& existing = pair.second;
        if (existing.enabled && existing.type == PeripheralType::UART &&
            !isProtocolOwnedUartId(existing.id)) {
            ++enabledGenericUarts;
        }
    }

    const size_t genericCapacity = static_cast<size_t>(CHIP_UART_COUNT - 1);
    if (enabledGenericUarts >= genericCapacity) {
        errorMsg = String("UART_RESOURCE_EXHAUSTED: all ") + String(genericCapacity) +
                   " generic UART port(s) are in use; UART0 is reserved for logging/USB";
        return false;
    }
    return true;
#endif
}

bool PeripheralManager::addPeripheral(const PeripheralConfig& config) {
    String errorMsg;
    return addPeripheral(config, errorMsg);
}

bool PeripheralManager::addPeripheral(const PeripheralConfig& config, String& errorMsg) {
    // 容器线程安全：Web handler(async_tcp)/worker pool/loopTask 并发增删改查
    RecursiveMutexGuard lock(_mutex);
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

    // UART0 is never borrowed by generic peripherals. Reject capacity errors
    // before mutating the in-memory configuration or attempting persistence.
    if (!validateUartCapacity(config, "", errorMsg)) {
        LOG_ERRORF("Peripheral Manager: %s", errorMsg.c_str());
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
        if (!initHardware(config.id)) {
            teardownHardware(config);
            runtimeStates.erase(config.id);
            peripherals.erase(config.id);
            rebuildPinMapping();
            errorMsg = String("外设 '") + config.id + "' 硬件初始化失败";
            LOG_ERRORF("Peripheral Manager: Add '%s' rolled back after hardware init failure",
                       config.id.c_str());
            return false;
        }
    }

    return true;
}

bool PeripheralManager::updatePeripheral(const String& id, const PeripheralConfig& config) {
    String errorMsg;
    return updatePeripheral(id, config, errorMsg);
}

bool PeripheralManager::updatePeripheral(const String& id, const PeripheralConfig& config, String& errorMsg) {
    RecursiveMutexGuard lock(_mutex);
    auto oldIt = peripherals.find(id);
    if (oldIt == peripherals.end()) {
        errorMsg = String("外设 '") + id + "' 不存在";
        LOG_ERRORF("Peripheral Manager: Peripheral '%s' not found", id.c_str());
        return false;
    }

    if (!validateConfig(config, errorMsg)) {
        LOG_ERRORF("Peripheral Manager: Invalid update for '%s' - %s", id.c_str(), errorMsg.c_str());
        return false;
    }

    const String targetId = config.id;
    if (targetId != id && peripherals.find(targetId) != peripherals.end()) {
        errorMsg = String("外设 ID '") + targetId + "' 已存在";
        return false;
    }

    if (!validateUartCapacity(config, id, errorMsg)) {
        LOG_ERRORF("Peripheral Manager: %s", errorMsg.c_str());
        return false;
    }

    if (config.enabled && !config.isModbusPeripheral() && !isDeviceEventType(config.type)) {
        for (uint8_t i = 0; i < config.pinCount && i < 8; ++i) {
            uint8_t pin = config.pins[i];
            if (pin != 255 && checkPinConflict(pin, id)) {
                errorMsg = String("引脚 ") + pin + " 已被其他已启用外设占用";
                return false;
            }
        }
    }

    const PeripheralConfig oldConfig = oldIt->second;
    PeripheralRuntimeState oldState;
    bool hadOldState = false;
    auto oldStateIt = runtimeStates.find(id);
    if (oldStateIt != runtimeStates.end()) {
        oldState = oldStateIt->second;
        hadOldState = true;
    }

    if (oldConfig.enabled) {
        deinitHardware(id);
    }

    removePinMapping(id);
    if (targetId != id) {
        peripherals.erase(id);
        runtimeStates.erase(id);
    }

    peripherals[targetId] = config;
    PeripheralRuntimeState nextState = hadOldState ? oldState : PeripheralRuntimeState();
    nextState.id = targetId;
    nextState.status = config.enabled ? PeripheralStatus::PERIPHERAL_ENABLED
                                      : PeripheralStatus::PERIPHERAL_DISABLED;
    runtimeStates[targetId] = nextState;
    updatePinMapping(targetId, config);

    if (config.enabled && !initHardware(targetId)) {
        teardownHardware(config);
        peripherals.erase(targetId);
        runtimeStates.erase(targetId);
        rebuildPinMapping();

        peripherals[id] = oldConfig;
        if (hadOldState) {
            runtimeStates[id] = oldState;
        } else {
            PeripheralRuntimeState restored;
            restored.id = id;
            runtimeStates[id] = restored;
        }
        updatePinMapping(id, oldConfig);
        const bool rollbackRestored = !oldConfig.enabled || initHardware(id);
        if (!rollbackRestored) {
            errorMsg = String("ROLLBACK_FAILED: 外设 '") + targetId +
                       "' 硬件初始化失败，且原外设 '" + id +
                       "' 无法重新初始化";
            LOG_ERRORF("Peripheral Manager: rollback reinit failed for '%s'", id.c_str());
        } else {
            errorMsg = String("HARDWARE_INIT_FAILED: 外设 '") + targetId +
                       "' 硬件初始化失败，已恢复原配置";
        }
        LOG_ERRORF("Peripheral Manager: Update '%s' rolled back after hardware init failure", id.c_str());
        return false;
    }

    LOG_INFOF("Peripheral Manager: Updated peripheral '%s' -> '%s'", id.c_str(), targetId.c_str());
    return true;
}

bool PeripheralManager::removePeripheral(const String& id) {
    RecursiveMutexGuard lock(_mutex);
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
    // 注意：返回的裸指针仅在调用方持有 _mutex 期间保证有效（递归锁），
    // 外部调用应优先使用返回拷贝的 getAllPeripherals/getPeripheralsByType
    RecursiveMutexGuard lock(_mutex);
    auto it = peripherals.find(id);
    if (it != peripherals.end()) {
        return &(it->second);
    }
    return nullptr;
}

const PeripheralConfig* PeripheralManager::getPeripheral(const String& id) const {
    RecursiveMutexGuard lock(_mutex);
    auto it = peripherals.find(id);
    if (it != peripherals.end()) {
        return &(it->second);
    }
    return nullptr;
}

bool PeripheralManager::getPeripheralSnapshot(const String& id, PeripheralConfig& snapshot) const {
    RecursiveMutexGuard lock(_mutex);
    auto it = peripherals.find(id);
    if (it == peripherals.end()) return false;
    snapshot = it->second;
    return true;
}

// C 字符串零分配查找：std::less<String> 非透明比较器导致 find(const char*)
// 会隐式构造 String（堆分配），热路径改为线性扫描 strcmp 规避
PeripheralConfig* PeripheralManager::getPeripheralCStrLocked(const char* id) {
    if (!id || id[0] == '\0') return nullptr;
    for (auto& pair : peripherals) {
        if (strcmp(pair.first.c_str(), id) == 0) {
            return &pair.second;
        }
    }
    return nullptr;
}

PeripheralRuntimeState* PeripheralManager::getRuntimeStateCStrLocked(const char* id) {
    if (!id || id[0] == '\0') return nullptr;
    for (auto& pair : runtimeStates) {
        if (strcmp(pair.first.c_str(), id) == 0) {
            return &pair.second;
        }
    }
    return nullptr;
}

std::vector<PeripheralConfig> PeripheralManager::getPeripheralsByType(PeripheralType type) const {
    RecursiveMutexGuard lock(_mutex);
    std::vector<PeripheralConfig> result;
    for (const auto& pair : peripherals) {
        if (pair.second.type == type) {
            result.push_back(pair.second);
        }
    }
    return result;
}

std::vector<PeripheralConfig> PeripheralManager::getPeripheralsByCategory(PeripheralCategory category) const {
    RecursiveMutexGuard lock(_mutex);
    std::vector<PeripheralConfig> result;
    for (const auto& pair : peripherals) {
        if (getPeripheralCategory(pair.second.type) == category) {
            result.push_back(pair.second);
        }
    }
    return result;
}

std::vector<PeripheralConfig> PeripheralManager::getAllPeripherals() const {
    RecursiveMutexGuard lock(_mutex);
    std::vector<PeripheralConfig> result;
    for (const auto& pair : peripherals) {
        result.push_back(pair.second);
    }
    return result;
}

void PeripheralManager::forEachPeripheral(std::function<void(const PeripheralConfig&)> callback) const {
    RecursiveMutexGuard lock(_mutex);
    for (const auto& pair : peripherals) {
        callback(pair.second);
    }
}

bool PeripheralManager::hasPeripheral(const String& id) const {
    RecursiveMutexGuard lock(_mutex);
    return peripherals.find(id) != peripherals.end();
}

// ========== 外设操作 ==========

bool PeripheralManager::enablePeripheral(const String& id) {
    RecursiveMutexGuard lock(_mutex);
    auto config = getPeripheral(id);
    if (!config) return false;

    const PeripheralConfig previousConfig = *config;
    const size_t previousMemorySuspendedCount = _memorySuspendedCount;
    PeripheralRuntimeState previousState;
    bool hadPreviousState = false;
    auto stateIt = runtimeStates.find(id);
    if (stateIt != runtimeStates.end()) {
        previousState = stateIt->second;
        hadPreviousState = true;
        if (config->enabled &&
            stateIt->second.status == PeripheralStatus::PERIPHERAL_INITIALIZED) {
            lastEnableError = "";
            return true;
        }
    }

    String capacityError;
    if (!validateUartCapacity(*config, id, capacityError)) {
        lastEnableError = capacityError;
        LOG_ERRORF("Peripheral Manager: Cannot enable '%s': %s",
                   id.c_str(), capacityError.c_str());
        return false;
    }

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

    if (stateIt != runtimeStates.end()) {
        if (stateIt->second.memorySuspended && _memorySuspendedCount > 0) {
            _memorySuspendedCount--;
        }
        stateIt->second.status = PeripheralStatus::PERIPHERAL_ENABLED;
        stateIt->second.memorySuspended = false;
    }

    if (!initHardware(id)) {
        lastEnableError = "Hardware init failed for '" + id + "'";
        teardownHardware(*config);
        *config = previousConfig;
        _memorySuspendedCount = previousMemorySuspendedCount;
        if (hadPreviousState) {
            runtimeStates[id] = previousState;
        } else {
            runtimeStates.erase(id);
        }
        rebuildPinMapping();
        return false;
    }
    lastEnableError = "";
    return true;
}

bool PeripheralManager::disablePeripheral(const String& id) {
    RecursiveMutexGuard lock(_mutex);
    auto config = getPeripheral(id);
    if (!config) return false;

    config->enabled = false;

    auto stateIt = runtimeStates.find(id);
    if (stateIt != runtimeStates.end()) {
        if (stateIt->second.memorySuspended && _memorySuspendedCount > 0) {
            _memorySuspendedCount--;
        }
        stateIt->second.memorySuspended = false;
    }

    return deinitHardware(id);
}

bool PeripheralManager::applyMutationTransactional(
    const String& snapshotId, const String& resultId,
    const std::function<bool(String&)>& mutation, String& errorMsg) {
    RecursiveMutexGuard lock(_mutex);

    MutationSnapshot snapshot;
    snapshot.id = snapshotId;
    snapshot.memorySuspendedCount = _memorySuspendedCount;
    auto configIt = peripherals.find(snapshotId);
    if (configIt != peripherals.end()) {
        snapshot.existed = true;
        snapshot.config = configIt->second;
    }
    auto stateIt = runtimeStates.find(snapshotId);
    if (stateIt != runtimeStates.end()) {
        snapshot.hadRuntimeState = true;
        snapshot.runtimeState = stateIt->second;
    }

    String mutationError;
    if (!mutation(mutationError)) {
        errorMsg = mutationError;
        return false;
    }
    if (saveConfiguration()) {
        errorMsg = "";
        return true;
    }

    String rollbackError;
    const bool restored = restoreMutationSnapshot(snapshot, resultId, rollbackError);
    if (restored) {
        errorMsg = "PERSISTENCE_WRITE_FAILED: 配置未保存，运行态已回滚";
    } else {
        errorMsg = "PERSISTENCE_WRITE_FAILED: 配置未保存，运行态回滚失败";
        errorMsg += String("; ROLLBACK_FAILED: ") + rollbackError;
    }
    return false;
}

bool PeripheralManager::restoreMutationSnapshot(const MutationSnapshot& snapshot,
                                                const String& resultId,
                                                String& errorMsg) {
    RecursiveMutexGuard lock(_mutex);

    auto removeCurrent = [this](const String& id) {
        auto it = peripherals.find(id);
        if (it == peripherals.end()) return;
        teardownHardware(it->second);
        peripherals.erase(it);
        runtimeStates.erase(id);
    };
    removeCurrent(resultId);
    if (snapshot.id != resultId) removeCurrent(snapshot.id);

    _memorySuspendedCount = snapshot.memorySuspendedCount;
    if (snapshot.existed) {
        peripherals[snapshot.id] = snapshot.config;
        if (snapshot.hadRuntimeState) {
            runtimeStates[snapshot.id] = snapshot.runtimeState;
        } else {
            PeripheralRuntimeState state;
            state.id = snapshot.id;
            state.status = snapshot.config.enabled ? PeripheralStatus::PERIPHERAL_ENABLED
                                                   : PeripheralStatus::PERIPHERAL_DISABLED;
            runtimeStates[snapshot.id] = state;
        }
    }
    rebuildPinMapping();

    if (snapshot.existed && snapshot.config.enabled &&
        !(snapshot.hadRuntimeState && snapshot.runtimeState.memorySuspended)) {
        if (!initHardware(snapshot.id)) {
            errorMsg = String("无法重新初始化外设 '") + snapshot.id + "'";
            return false;
        }
    }
    errorMsg = "";
    return true;
}

bool PeripheralManager::addPeripheralAndSave(const PeripheralConfig& config, String& errorMsg) {
    return applyMutationTransactional(config.id, config.id,
        [this, &config](String& mutationError) {
            return addPeripheral(config, mutationError);
        }, errorMsg);
}

bool PeripheralManager::updatePeripheralAndSave(const String& id,
                                                const PeripheralConfig& config,
                                                String& errorMsg) {
    return applyMutationTransactional(id, config.id,
        [this, &id, &config](String& mutationError) {
            return updatePeripheral(id, config, mutationError);
        }, errorMsg);
}

bool PeripheralManager::removePeripheralAndSave(const String& id, String& errorMsg) {
    return applyMutationTransactional(id, id,
        [this, &id](String& mutationError) {
            if (removePeripheral(id)) return true;
            mutationError = String("外设 '") + id + "' 不存在";
            return false;
        }, errorMsg);
}

bool PeripheralManager::enablePeripheralAndSave(const String& id, String& errorMsg) {
    return applyMutationTransactional(id, id,
        [this, &id](String& mutationError) {
            if (enablePeripheral(id)) return true;
            mutationError = lastEnableError.isEmpty()
                ? String("无法启用外设 '") + id + "'"
                : lastEnableError;
            return false;
        }, errorMsg);
}

bool PeripheralManager::disablePeripheralAndSave(const String& id, String& errorMsg) {
    return applyMutationTransactional(id, id,
        [this, &id](String& mutationError) {
            if (disablePeripheral(id)) return true;
            mutationError = String("无法禁用外设 '") + id + "'";
            return false;
        }, errorMsg);
}

bool PeripheralManager::isPeripheralEnabled(const String& id) const {
    RecursiveMutexGuard lock(_mutex);
    auto config = getPeripheral(id);
    return config ? config->enabled : false;
}

PeripheralStatus PeripheralManager::getPeripheralStatus(const String& id) const {
    RecursiveMutexGuard lock(_mutex);
    auto it = runtimeStates.find(id);
    if (it != runtimeStates.end()) {
        return it->second.status;
    }
    return PeripheralStatus::PERIPHERAL_DISABLED;
}

PeripheralRuntimeState* PeripheralManager::getRuntimeState(const String& id) {
    RecursiveMutexGuard lock(_mutex);
    auto it = runtimeStates.find(id);
    if (it != runtimeStates.end()) {
        return &(it->second);
    }
    return nullptr;
}

bool PeripheralManager::getRuntimeStateSnapshot(const String& id, PeripheralRuntimeState& snapshot) const {
    RecursiveMutexGuard lock(_mutex);
    auto it = runtimeStates.find(id);
    if (it == runtimeStates.end()) return false;
    snapshot = it->second;

    auto configIt = peripherals.find(id);
    auto uartIt = uartPortById.find(id);
    if (configIt != peripherals.end() && configIt->second.type == PeripheralType::UART) {
        if (uartIt != uartPortById.end()) {
            HardwareSerial* serial = serialForPort(uartIt->second);
            snapshot.state.comm.isConnected = serial != nullptr;
            snapshot.state.comm.port = uartIt->second;
            snapshot.state.comm.rxAvailable = boundedUartAvailable(serial);
        } else {
            snapshot.state.comm.isConnected = false;
            snapshot.state.comm.port = UART_PORT_UNASSIGNED;
            snapshot.state.comm.rxAvailable = 0;
        }
    }
    return true;
}

// ========== 硬件初始化 ==========

bool PeripheralManager::initHardware(const String& id) {
    RecursiveMutexGuard lock(_mutex);
    auto config = getPeripheral(id);
    if (!config) return false;

    if (!config->enabled) {
        LOG_WARNINGF("Peripheral Manager: Cannot init disabled peripheral '%s'", id.c_str());
        return false;
    }

    // 2026-08-13: 保留引脚安全网。启动时从 peripherals.json 加载的配置不经过
    // validateConfig，若含芯片保留引脚（如 S3 Octal PSRAM 的 GPIO25/33-37），
    // pinMode/digitalRead 会踩坏 PSRAM 总线导致 TG1WDT 复位循环（实测）。
    // 此处统一拦截并标记 ERROR，后续轮询循环也不再触碰该外设。
    for (uint8_t i = 0; i < config->pinCount && i < 8; i++) {
        if (config->pins[i] != 255 && isReservedPin(config->pins[i])) {
            LOG_ERRORF("Peripheral Manager: '%s' rejected, GPIO%d is reserved on %s",
                       id.c_str(), config->pins[i], CHIP_NAME);
            if (runtimeStates.find(id) != runtimeStates.end()) {
                runtimeStates[id].status = PeripheralStatus::PERIPHERAL_ERROR;
            }
            return false;
        }
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
    RecursiveMutexGuard lock(_mutex);
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
            // 跳过验证失败的外设（已标记为 PERIPHERAL_ERROR），仅保留配置展示
            auto* rtState = getRuntimeState(pair.first);
            if (rtState && rtState->status == PeripheralStatus::PERIPHERAL_ERROR) {
                LOG_WARNINGF("Peripheral Manager: Skipping init for '%s' (validation failed)", pair.first.c_str());
                failCount++;
                continue;
            }
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

size_t PeripheralManager::suspendOptionalForMemory() {
    RecursiveMutexGuard lock(_mutex);
    if (!_mutex) return 0;

    size_t suspended = 0;
    for (auto& pair : peripherals) {
        PeripheralConfig& config = pair.second;
        auto stateIt = runtimeStates.find(pair.first);
        if (!config.enabled || !isMemoryOptionalPeripheralType(config.type) ||
            stateIt == runtimeStates.end() || stateIt->second.memorySuspended) {
            continue;
        }

        // Teardown is best-effort: the memory guard must still mark the
        // peripheral unavailable even if a driver cannot release every buffer.
        teardownHardware(config);
        config.enabled = false;  // runtime-only; saveConfiguration is not called
        stateIt->second.status = PeripheralStatus::PERIPHERAL_DISABLED;
        stateIt->second.memorySuspended = true;
        stateIt->second.lastError = "memory_pressure_suspended";
        suspended++;
    }

    _memorySuspendedCount += suspended;
    if (suspended > 0) {
        LOG_WARNINGF("Peripheral Manager: suspended %u optional peripherals for memory pressure",
                     (unsigned)suspended);
    }
    return suspended;
}

size_t PeripheralManager::restoreOptionalAfterMemory() {
    RecursiveMutexGuard lock(_mutex);
    if (!_mutex) return 0;

    size_t restored = 0;
    for (auto& pair : peripherals) {
        PeripheralConfig& config = pair.second;
        auto stateIt = runtimeStates.find(pair.first);
        if (stateIt == runtimeStates.end() || !stateIt->second.memorySuspended) {
            continue;
        }

#if defined(ESP_PLATFORM)
        // Leave enough contiguous DRAM for Web/MQTT while restoring one
        // optional driver at a time. A later health sample can retry.
        if (FastBee::MemoryBudget::dram8Free() <
                FastBee::MemoryBudget::PERIPH_EXEC_MIN_DRAM + 4096U ||
            FastBee::MemoryBudget::dram8Largest() <
                FastBee::MemoryBudget::PERIPH_EXEC_MIN_LARGEST) {
            break;
        }
#endif

        config.enabled = true;
        stateIt->second.status = PeripheralStatus::PERIPHERAL_ENABLED;
        if (initHardware(pair.first)) {
            stateIt->second.memorySuspended = false;
            stateIt->second.lastError = "";
            restored++;
        } else {
            // Keep the runtime suspension if the driver still cannot be
            // initialized (for example a user-disabled compile-time feature).
            config.enabled = false;
            stateIt->second.status = PeripheralStatus::PERIPHERAL_DISABLED;
            stateIt->second.lastError = "memory_pressure_restore_pending";
        }
    }

    _memorySuspendedCount = 0;
    for (const auto& pair : runtimeStates) {
        if (pair.second.memorySuspended) _memorySuspendedCount++;
    }
    if (restored > 0) {
        LOG_INFOF("Peripheral Manager: restored %u optional peripherals after memory recovery",
                  (unsigned)restored);
    }
    return restored;
}

// 外设类型 → 网络依赖功能 key 映射（仅列出 setupHardware 中早期 eager 初始化、
// 且被 requiresNetwork 门控的类型）。返回 nullptr 表示该类型不受网络延迟门控。
// 说明：I2C 传感器(BMP280/MPU6050)与 RFID 为惰性初始化（轮询/使用时才 begin），
//       天然落在网络就绪之后，无需在此重试；此处仅覆盖启动期被跳过的 eager 类型。
static const char* deferredFeatureKeyForType(PeripheralType type) {
    switch (type) {
        case PeripheralType::IR_RECEIVER: return "irRemote";
        case PeripheralType::RFID_MFRC522: return "rfid";
        default: return nullptr;
    }
}

int PeripheralManager::retryDeferredPeripherals() {
    int retried = 0, ok = 0;
    for (auto& pair : peripherals) {
        if (!pair.second.enabled) continue;
        auto* rtState = getRuntimeState(pair.first);
        if (!rtState || rtState->status != PeripheralStatus::PERIPHERAL_ERROR) continue;
        const char* featureKey = deferredFeatureKeyForType(pair.second.type);
        if (!featureKey) continue;  // 非网络延迟门控类型，不重试（避免误触真实硬件故障）
        // 网络就绪后门控解除才重试（此时 shouldEnable 应返回 true）
        if (!FastBee::RuntimeFeatureManager::instance().shouldEnable(featureKey)) continue;
        rtState->status = PeripheralStatus::PERIPHERAL_ENABLED;
        if (initHardware(pair.first)) ok++;
        retried++;
    }
    if (retried > 0) {
        LOG_INFOF("Peripheral Manager: Deferred init retry after network: %d/%d succeeded",
                  ok, retried);
    }
    return ok;
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
    } else if (isAdcInputType(type)) {
        initializeAdcDefaults(config);
    }

    return addPeripheral(config);
}

bool PeripheralManager::configurePin(const PeripheralConfig& config) {
    return addPeripheral(config);
}

GPIOState PeripheralManager::readGpioStateLocked(const PeripheralConfig& config,
                                                  const String* peripheralId,
                                                  const char* peripheralIdCStr) {
    if (!config.isGPIOPeripheral()) {
        return GPIOState::STATE_UNDEFINED;
    }

    uint8_t pin = config.getPrimaryPin();
    if (pin == 255) return GPIOState::STATE_UNDEFINED;
    // 保留引脚守卫：按键扫描等周期轮询会对未成功初始化的外设反复 read，
    // 触碰保留引脚（如 S3 Octal PSRAM 的 GPIO25）会产生错误风暴甚至踩坏总线
    if (isReservedPin(pin)) return GPIOState::STATE_UNDEFINED;

    switch (config.type) {
        case PeripheralType::GPIO_DIGITAL_INPUT:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLUP:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN:
        case PeripheralType::GPIO_INTERRUPT_RISING:
        case PeripheralType::GPIO_INTERRUPT_FALLING:
        case PeripheralType::GPIO_INTERRUPT_CHANGE: {
            bool high = digitalRead(pin) != 0;
            if (config.params.gpio.invertLogic) high = !high;
            return high ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
        }

        case PeripheralType::GPIO_ANALOG_INPUT:
            return analogRead(pin) > 2048 ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;

        default:
            break;
    }

    // 输出类型：返回最后设置的状态；C 字符串路径保持零分配。
    if (peripheralId) {
        auto it = runtimeStates.find(*peripheralId);
        return it != runtimeStates.end()
            ? it->second.state.gpio.currentState
            : GPIOState::STATE_UNDEFINED;
    }

    auto* rt = getRuntimeStateCStrLocked(peripheralIdCStr);
    return rt ? rt->state.gpio.currentState : GPIOState::STATE_UNDEFINED;
}

bool PeripheralManager::writeGpioStateLocked(const PeripheralConfig& config,
                                             const String* peripheralId,
                                             const char* peripheralIdCStr,
                                             GPIOState state) {
    if (!config.isGPIOPeripheral()) return false;

    uint8_t pin = config.getPrimaryPin();
    if (pin == 255) return false;

    bool success = true;
    switch (config.type) {
        case PeripheralType::GPIO_DIGITAL_OUTPUT:
            if (config.params.gpio.passive) {
                // 无源蜂鸣器：PWM 驱动，HIGH=50%占空比发声，LOW=0静音
                uint32_t maxDuty = (1U << config.params.gpio.pwmResolution) - 1;
                ledcWrite(pin, state == GPIOState::STATE_HIGH ? (maxDuty / 2) : 0);
            } else {
                digitalWrite(pin, state == GPIOState::STATE_HIGH ? HIGH : LOW);
            }
            break;

        case PeripheralType::GPIO_PWM_OUTPUT:
            ledcWrite(config.getPrimaryPin(), state == GPIOState::STATE_HIGH
                ? (1U << config.params.gpio.pwmResolution) - 1 : 0U);
            break;

        case PeripheralType::GPIO_ANALOG_OUTPUT:
            ledcWrite(config.getPrimaryPin(), state == GPIOState::STATE_HIGH
                ? (uint32_t)((1U << config.params.gpio.pwmResolution) - 1) : 0U);
            break;

        default:
            success = false;
            break;
    }

    if (!success) return false;

    // 仅更新已存在的运行时状态，避免错误 ID 触发堆分配。
    if (peripheralId) {
        auto it = runtimeStates.find(*peripheralId);
        if (it != runtimeStates.end()) {
            it->second.state.gpio.currentState = state;
            it->second.lastActivity = millis();
        }
    } else {
        auto* rt = getRuntimeStateCStrLocked(peripheralIdCStr);
        if (rt) {
            rt->state.gpio.currentState = state;
            rt->lastActivity = millis();
        }
    }

    return true;
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

    // 编码器：返回计数值（转换为GPIOState，HIGH表示非零计数）
    if (config->type == PeripheralType::ENCODER) {
        EncoderIsrSlot* slot = findEncoderSlot(peripheralId);
        if (!slot) {
            return GPIOState::STATE_UNDEFINED;
        }
        portENTER_CRITICAL(&_encoderMux);
        int64_t cnt = slot->count;
        portEXIT_CRITICAL(&_encoderMux);
        return cnt != 0 ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
    }

    if (config->type == PeripheralType::RF_MODULE && config->params.rf.mode == RF_MODE_RX) {
        bool level = false;
        if (!readRfLevel(peripheralId, level)) return GPIOState::STATE_UNDEFINED;
        return level ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
    }

    return readGpioStateLocked(*config, &peripheralId, nullptr);
}

// 热路径零分配版本：普通 GPIO 数字输入直接物理读取，全程无堆操作；
// Modbus/雷达/编码器/RF 等依赖 String 接口的特例外设回退委托原实现
GPIOState PeripheralManager::readPinByCStr(const char* peripheralId) {
    RecursiveMutexGuard lock(_mutex);
    auto config = getPeripheralCStrLocked(peripheralId);
    if (!config) return GPIOState::STATE_UNDEFINED;

    // 特例外设回退：构造一次 String 委托原实现（低频路径，可接受；
    // _mutex 为递归锁，原实现内部再次加锁安全）
    if (config->isModbusPeripheral() ||
        config->type == PeripheralType::RADAR_SENSOR ||
        config->type == PeripheralType::ENCODER ||
        config->type == PeripheralType::RF_MODULE) {
        return readPin(String(peripheralId));
    }

    return readGpioStateLocked(*config, nullptr, peripheralId);
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

    // 编码器：支持重置计数器（写入LOW表示重置）
    if (config->type == PeripheralType::ENCODER) {
        if (state == GPIOState::STATE_LOW) {
            return resetEncoder(peripheralId);
        }
        return true;
    }

    return writeGpioStateLocked(*config, &peripheralId, nullptr, state);
}

// 热路径零分配版本：普通 GPIO 数字输出直接写物理电平，全程无堆操作；
// Modbus/编码器回退委托原实现
bool PeripheralManager::writePinByCStr(const char* peripheralId, GPIOState state) {
    RecursiveMutexGuard lock(_mutex);
    auto config = getPeripheralCStrLocked(peripheralId);
    if (!config) return false;

    // 特例外设回退：构造一次 String 委托原实现（低频路径）
    if (config->isModbusPeripheral() || config->type == PeripheralType::ENCODER) {
        return writePin(String(peripheralId), state);
    }

    return writeGpioStateLocked(*config, nullptr, peripheralId, state);
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
    if (!config || !config->enabled) return false;

    // Modbus PWM 外设：通过委托回调写入寄存器
    if (config->isModbusPeripheral()) {
        return writeModbusPWM(peripheralId, *config, dutyCycle);
    }

    // 无源蜂鸣器（GPIO_DIGITAL_OUTPUT + passive）也支持 PWM 写入
    bool isPassiveGPIO = (config->type == PeripheralType::GPIO_DIGITAL_OUTPUT && config->params.gpio.passive);
    if (config->type != PeripheralType::GPIO_PWM_OUTPUT &&
        config->type != PeripheralType::GPIO_ANALOG_OUTPUT &&
        config->type != PeripheralType::PWM_SERVO &&
        !isPassiveGPIO) {
        return false;
    }

    if (config->type == PeripheralType::PWM_SERVO && !attachServoSignal(*config)) {
        return false;
    }

    uint8_t pwmResolution = config->type == PeripheralType::PWM_SERVO
        ? config->params.servo.pwmResolution
        : config->params.gpio.pwmResolution;
    if (pwmResolution < 1 || pwmResolution > 16) pwmResolution = 8;
    uint32_t maxVal = (1UL << pwmResolution) - 1UL;

    // AO 模拟量输出模式：输入值视为 0-100% 百分比，线性映射到
    // [aoMinPct, aoMaxPct] 区间的占空比（无 DAC 芯片的工业 AO 方案）。
    // 边界钳位：超范围输入收敛到区间两端，避免执行器过冲。
    if (config->type != PeripheralType::PWM_SERVO && config->params.gpio.aoMode) {
        uint32_t duty = mapAnalogOutputPercent(
            dutyCycle,
            config->params.gpio.aoMinPct,
            config->params.gpio.aoMaxPct,
            pwmResolution);
        return ledcWrite(config->getPrimaryPin(), duty);
    }

    uint32_t duty = dutyCycle > maxVal ? maxVal : dutyCycle;
    const bool success = ledcWrite(config->getPrimaryPin(), duty);
    if (success) {
        auto stateIt = runtimeStates.find(peripheralId);
        if (stateIt != runtimeStates.end()) {
            stateIt->second.lastActivity = millis();
            if (config->type == PeripheralType::PWM_SERVO) {
                stateIt->second.state.servo.signalAttached = true;
            }
        }
        if (config->type == PeripheralType::PWM_SERVO && config->params.servo.releaseAfterMove) {
            scheduleServoRelease(peripheralId);
        }
    }
    return success;
}

bool PeripheralManager::writeServoAngle(const String& peripheralId, int angle) {
    RecursiveMutexGuard lock(_mutex);
    PeripheralConfig* config = getPeripheral(peripheralId);
    if (!config || !config->enabled || config->type != PeripheralType::PWM_SERVO ||
        !isServoConfigValid(*config)) {
        return false;
    }

    if (angle < config->params.servo.minAngle) angle = config->params.servo.minAngle;
    if (angle > config->params.servo.maxAngle) angle = config->params.servo.maxAngle;
    if (!attachServoSignal(*config)) return false;

    const uint16_t pulseUs = servoAngleToPulseUs(
        angle, config->params.servo.minAngle, config->params.servo.maxAngle,
        config->params.servo.minPulse, config->params.servo.maxPulse,
        config->params.servo.inverted);
    const uint32_t duty = servoPulseUsToDuty(
        pulseUs, config->params.servo.frequency, config->params.servo.pwmResolution);
    if (!ledcWrite(config->getPrimaryPin(), duty)) return false;

    auto stateIt = runtimeStates.find(peripheralId);
    if (stateIt != runtimeStates.end()) {
        stateIt->second.status = PeripheralStatus::PERIPHERAL_RUNNING;
        stateIt->second.lastActivity = millis();
        stateIt->second.state.servo.currentAngle = static_cast<int16_t>(angle);
        stateIt->second.state.servo.currentPulseUs = pulseUs;
        stateIt->second.state.servo.signalAttached = true;
    }
    if (config->params.servo.releaseAfterMove) scheduleServoRelease(peripheralId);
    return true;
}

uint16_t PeripheralManager::readAnalog(uint8_t pin) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return 0;
    return readAnalog(id);
}

uint16_t PeripheralManager::readAnalog(const String& peripheralId) {
    // 持锁保护整个函数体：getPeripheral 返回的裸指针仅在持锁期间有效，
    // 与 readAnalogByCStr 对齐，防止并发 removePeripheral 导致指针失效
    RecursiveMutexGuard lock(_mutex);
    auto config = getPeripheralCStrLocked(peripheralId.c_str());
    if (!config) return 0;

    if (config->type != PeripheralType::GPIO_ANALOG_INPUT &&
        config->type != PeripheralType::ADC) {
        return 0;
    }

    uint8_t pin = config->getPrimaryPin();
    if (pin == 255) return 0;
    // 保留引脚守卫（同 readPin）：避免周期采样触碰 PSRAM/Flash 总线引脚
    if (isReservedPin(pin)) return 0;

    // ADC 与旧式 GPIO_ANALOG_INPUT 共用同一套滤波参数。
    if (isAdcInputType(config->type)) {
        uint8_t samples = config->params.adc.sampleCount;
        if (samples < 1) samples = 1;
        if (samples > 16) samples = 16;
        uint8_t mode = config->params.adc.filterMode > 1 ? 0 : config->params.adc.filterMode;
        return adcFilterRead(pin, samples, mode);
    }

    return (uint16_t)analogRead(pin);
}

// 热路径零分配版本：轮询执行器 AI 读取主入口，全程无堆操作
uint16_t PeripheralManager::readAnalogByCStr(const char* peripheralId) {
    RecursiveMutexGuard lock(_mutex);
    auto config = getPeripheralCStrLocked(peripheralId);
    if (!config) return 0;

    if (config->type != PeripheralType::GPIO_ANALOG_INPUT &&
        config->type != PeripheralType::ADC) {
        return 0;
    }

    uint8_t pin = config->getPrimaryPin();
    if (pin == 255) return 0;
    if (isReservedPin(pin)) return 0;

    if (isAdcInputType(config->type)) {
        uint8_t samples = config->params.adc.sampleCount;
        if (samples < 1) samples = 1;
        if (samples > 16) samples = 16;
        uint8_t mode = config->params.adc.filterMode > 1 ? 0 : config->params.adc.filterMode;
        return adcFilterRead(pin, samples, mode);
    }

    return (uint16_t)analogRead(pin);
}

float PeripheralManager::readAnalogValue(const String& peripheralId) {
    RecursiveMutexGuard lock(_mutex);
    auto config = getPeripheralCStrLocked(peripheralId.c_str());
    if (!config || !isAdcInputType(config->type)) return NAN;

    const uint8_t pin = config->getPrimaryPin();
    if (pin == 255 || isReservedPin(pin)) return NAN;
    uint8_t samples = config->params.adc.sampleCount;
    if (samples < 1) samples = 1;
    if (samples > 16) samples = 16;
    uint8_t filterMode = config->params.adc.filterMode > 1 ? 0 : config->params.adc.filterMode;
    return convertAnalogRaw(*config, adcFilterRead(pin, samples, filterMode));
}

float PeripheralManager::readAnalogValueByCStr(const char* peripheralId) {
    if (!peripheralId || !*peripheralId) return NAN;
    RecursiveMutexGuard lock(_mutex);
    auto config = getPeripheralCStrLocked(peripheralId);
    if (!config || !isAdcInputType(config->type)) return NAN;

    const uint8_t pin = config->getPrimaryPin();
    if (pin == 255 || isReservedPin(pin)) return NAN;
    uint8_t samples = config->params.adc.sampleCount;
    if (samples < 1) samples = 1;
    if (samples > 16) samples = 16;
    uint8_t filterMode = config->params.adc.filterMode > 1 ? 0 : config->params.adc.filterMode;
    return convertAnalogRaw(*config, adcFilterRead(pin, samples, filterMode));
}

float PeripheralManager::convertAnalogRawValue(const String& peripheralId, uint16_t rawValue,
                                               AnalogInputQuality* quality) {
    RecursiveMutexGuard lock(_mutex);
    const auto* config = getPeripheralCStrLocked(peripheralId.c_str());
    if (!config || !isAdcInputType(config->type)) {
        if (quality) *quality = AnalogInputQuality::INVALID_CONFIG;
        return NAN;
    }
    return convertAnalogRaw(*config, rawValue, quality);
}

// ADC 多采样滤波：均值滤波平滑随机噪声，中值滤波抑制脉冲尖峰
// 栈上缓冲（16×2B=32B），零堆分配；采样间隔 100us 降低通道串扰
uint16_t PeripheralManager::adcFilterRead(uint8_t pin, uint8_t samples, uint8_t filterMode) {
    if (samples <= 1) return (uint16_t)analogRead(pin);

    uint16_t buf[16];
    for (uint8_t i = 0; i < samples; i++) {
        buf[i] = (uint16_t)analogRead(pin);
        if (i + 1 < samples) delayMicroseconds(100);
    }

    if (filterMode == 1) {
        // 中值滤波：插入排序后取中间值（n<=16，排序开销可忽略）
        for (uint8_t i = 1; i < samples; i++) {
            uint16_t key = buf[i];
            int j = i - 1;
            while (j >= 0 && buf[j] > key) {
                buf[j + 1] = buf[j];
                j--;
            }
            buf[j + 1] = key;
        }
        return buf[samples / 2];
    }

    // 均值滤波
    uint32_t sum = 0;
    for (uint8_t i = 0; i < samples; i++) sum += buf[i];
    return (uint16_t)(sum / samples);
}

// ========== 持久化 ==========

bool PeripheralManager::saveConfiguration() {
    RecursiveMutexGuard lock(_mutex);
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
        else if (config.type == PeripheralType::ENCODER) {
            params["resolution"] = config.params.encoder.resolution;
            params["useInterrupt"] = config.params.encoder.useInterrupt;
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
        else if (config.type == PeripheralType::PWM_SERVO) {
            params["frequency"] = config.params.servo.frequency;
            params["minPulse"] = config.params.servo.minPulse;
            params["maxPulse"] = config.params.servo.maxPulse;
            params["minAngle"] = config.params.servo.minAngle;
            params["maxAngle"] = config.params.servo.maxAngle;
            params["initialAngle"] = config.params.servo.initialAngle;
            params["pwmChannel"] = config.params.servo.pwmChannel;
            params["pwmResolution"] = config.params.servo.pwmResolution;
            params["inverted"] = config.params.servo.inverted;
            params["releaseAfterMove"] = config.params.servo.releaseAfterMove;
        }
        else if (config.isGPIOPeripheral() && !isAdcInputType(config.type)) {
            params["initialState"] = static_cast<int>(config.params.gpio.initialState);
            params["pwmChannel"] = config.params.gpio.pwmChannel;
            params["pwmFrequency"] = config.params.gpio.pwmFrequency;
            params["pwmResolution"] = config.params.gpio.pwmResolution;
            params["defaultDuty"] = config.params.gpio.defaultDuty;
            if (config.params.gpio.passive) params["passive"] = true;
            // AO 模拟量输出模式（非默认值时才写入，保持配置精简）
            if (config.params.gpio.aoMode) {
                params["aoMode"] = true;
                params["aoMinPct"] = config.params.gpio.aoMinPct;
                params["aoMaxPct"] = config.params.gpio.aoMaxPct;
            }
            // DI 参数只在非默认值时写入，保持旧配置文件紧凑。
            if (config.type == PeripheralType::GPIO_DIGITAL_INPUT ||
                config.type == PeripheralType::GPIO_DIGITAL_INPUT_PULLUP ||
                config.type == PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN ||
                config.type == PeripheralType::GPIO_INTERRUPT_RISING ||
                config.type == PeripheralType::GPIO_INTERRUPT_FALLING ||
                config.type == PeripheralType::GPIO_INTERRUPT_CHANGE) {
                if (config.params.gpio.inputMode != static_cast<uint8_t>(DigitalInputMode::AUTO)) {
                    params["inputMode"] = config.params.gpio.inputMode;
                }
                if (config.params.gpio.invertLogic) params["invertLogic"] = true;
                if (config.params.gpio.debounceMs > 0) params["debounceMs"] = config.params.gpio.debounceMs;
                if (config.params.gpio.reportMode != static_cast<uint8_t>(DigitalReportMode::ON_CHANGE)) {
                    params["reportMode"] = config.params.gpio.reportMode;
                }
            }
        }
        else if (isAdcInputType(config.type)) {
            params["attenuation"] = config.params.adc.attenuation;
            params["resolution"] = config.params.adc.resolution;
            params["sampleRate"] = config.params.adc.sampleRate;
            // 多采样滤波（非默认值时才写入，向后兼容旧配置）
            if (config.params.adc.sampleCount > 1) params["sampleCount"] = config.params.adc.sampleCount;
            if (config.params.adc.filterMode > 0) params["filterMode"] = config.params.adc.filterMode;
            const bool hasAnalogCalibration =
                config.params.adc.signalMode != static_cast<uint8_t>(AnalogSignalMode::RAW) ||
                config.params.adc.conversionEnabled ||
                config.params.adc.clampEnabled ||
                config.params.adc.inputMin != 0.0f || config.params.adc.inputMax != 0.0f ||
                config.params.adc.engineeringMin != 0.0f || config.params.adc.engineeringMax != 100.0f ||
                config.params.adc.engineeringOffset != 0.0f || config.params.adc.decimalPlaces != 2 ||
                config.params.adc.vRef != 0.0f || config.params.adc.voltageRatio != 0.0f ||
                config.params.adc.shuntResistance != 0.0f || config.params.adc.inputGain != 0.0f ||
                config.params.adc.inputOffset != 0.0f || config.params.adc.rawMin != 0 ||
                config.params.adc.rawMax != 0;
            if (hasAnalogCalibration) {
                params["signalMode"] = config.params.adc.signalMode;
                params["conversionEnabled"] = config.params.adc.conversionEnabled;
                params["clampEnabled"] = config.params.adc.clampEnabled;
                params["decimalPlaces"] = config.params.adc.decimalPlaces;
                params["inputMin"] = config.params.adc.inputMin;
                params["inputMax"] = config.params.adc.inputMax;
                params["engineeringMin"] = config.params.adc.engineeringMin;
                params["engineeringMax"] = config.params.adc.engineeringMax;
                params["engineeringOffset"] = config.params.adc.engineeringOffset;
                params["vRef"] = config.params.adc.vRef;
                params["voltageRatio"] = config.params.adc.voltageRatio;
                params["shuntResistance"] = config.params.adc.shuntResistance;
                params["inputGain"] = config.params.adc.inputGain;
                params["inputOffset"] = config.params.adc.inputOffset;
                params["rawMin"] = config.params.adc.rawMin;
                params["rawMax"] = config.params.adc.rawMax;
            }
        }
        else if (config.type == PeripheralType::DAC) {
            params["channel"] = config.params.dac.channel;
        }
        else if (config.type == PeripheralType::STEPPER_MOTOR) {
            params["stepsPerRevolution"] = config.params.stepper.stepsPerRevolution;
            params["speed"] = config.params.stepper.speed;
        }
        else if (config.type == PeripheralType::SDIO) {
            params["interface"] = config.params.sdcard.interface;
            params["frequency"] = config.params.sdcard.frequency;
        }
        else if (config.type == PeripheralType::DS1302_RTC) {
            params["use24Hour"] = config.params.ds1302.use24Hour;
            params["trickleCharge"] = config.params.ds1302.trickleCharge;
        }
        else if (config.type == PeripheralType::LCD1602) {
            params["cols"] = config.params.lcd1602.cols;
            params["rows"] = config.params.lcd1602.rows;
            params["address"] = config.params.lcd1602.address;
            params["backlight"] = config.params.lcd1602.backlight;
        }
        else if (config.type == PeripheralType::RFID_MFRC522) {
            params["frequency"] = config.params.rfid.frequency;
        }
        else if (config.type == PeripheralType::LCD) {
            params["width"]     = config.params.lcd.width;
            params["height"]    = config.params.lcd.height;
            params["interface"] = config.params.lcd.interface;
            params["contrast"]  = config.params.lcd.contrast;
        }
        else if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
            params["brightness"] = config.params.segment.brightness;
        }
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
        else if (config.type == PeripheralType::SENSOR_GENERIC) {
            params["sensorType"] = config.params.sensor.sensorType;
            uint32_t interval = config.params.sensor.sampleInterval;
            if (interval < SENSOR_MIN_SAMPLE_INTERVAL_MS) interval = SENSOR_DEFAULT_SAMPLE_INTERVAL_MS;
            if (interval > SENSOR_MAX_SAMPLE_INTERVAL_MS) interval = SENSOR_MAX_SAMPLE_INTERVAL_MS;
            params["sampleInterval"] = interval;
            if (config.params.sensor.driver[0] != '\0') {
                params["driver"] = config.params.sensor.driver;
                params["address"] = config.params.sensor.address;
                params["mode"] = config.params.sensor.mode;
                params["frequency"] = config.params.sensor.frequency;
            }
        }
    }

    if (doc.overflowed()) {
        LOG_ERROR("Peripheral Manager: Configuration JSON overflowed; refusing partial save");
        return false;
    }

    const size_t expected = measureJson(doc);
    File out = FileUtils::openAtomicWrite(PERIPHERAL_CONFIG_FILE);
    if (!out) return false;
    const size_t written = serializeJson(doc, out);
    out.flush();
    out.close();

    const String tmpPath = String(PERIPHERAL_CONFIG_FILE) + ".tmp";
    if (written != expected) {
        LittleFS.remove(tmpPath);
        LOG_ERRORF("Peripheral Manager: Configuration write truncated (%u/%u)",
                   static_cast<unsigned>(written), static_cast<unsigned>(expected));
        return false;
    }
    if (!FileUtils::commitAtomicWrite(tmpPath, PERIPHERAL_CONFIG_FILE)) {
        LOG_ERROR("Peripheral Manager: Failed to commit configuration atomically");
        return false;
    }

    LOG_INFO("Peripheral Manager: Configuration saved successfully");
    return true;
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
        if (config.type == PeripheralType::UART) {
            initializeUartDefaults(config);
        } else if (config.type == PeripheralType::ENCODER) {
            initializeEncoderDefaults(config);
        } else if (config.type == PeripheralType::STEPPER_MOTOR) {
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
        } else if (config.type == PeripheralType::SENSOR_GENERIC) {
            initializeSensorDefaults(config);
        } else if (config.type == PeripheralType::SDIO) {
            initializeSdCardDefaults(config);
        } else if (config.type == PeripheralType::DS1302_RTC) {
            initializeDs1302Defaults(config);
        } else if (config.type == PeripheralType::LCD1602) {
            initializeLcd1602Defaults(config);
        } else if (config.type == PeripheralType::RFID_MFRC522) {
            initializeRfidDefaults(config);
        } else if (isAdcInputType(config.type)) {
            initializeAdcDefaults(config);
        } else if (config.type == PeripheralType::PWM_SERVO) {
            initializeServoDefaults(config);
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
                config.params.uart.baudRate = params["baudRate"] | UART_DEFAULT_BAUD_RATE;
                config.params.uart.dataBits = params["dataBits"] | UART_DEFAULT_DATA_BITS;
                config.params.uart.stopBits = params["stopBits"] | UART_DEFAULT_STOP_BITS;
                config.params.uart.parity = params["parity"] | UART_DEFAULT_PARITY;
            }
            else if (config.type == PeripheralType::ENCODER) {
                config.params.encoder.resolution = params["resolution"] | ENCODER_DEFAULT_RESOLUTION;
                config.params.encoder.useInterrupt = params["useInterrupt"] | true;
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
            else if (config.type == PeripheralType::PWM_SERVO) {
                config.params.servo.frequency = params["frequency"] | SERVO_DEFAULT_FREQUENCY_HZ;
                config.params.servo.minPulse = params["minPulse"] | SERVO_DEFAULT_MIN_PULSE_US;
                config.params.servo.maxPulse = params["maxPulse"] | SERVO_DEFAULT_MAX_PULSE_US;
                config.params.servo.minAngle = params["minAngle"] | SERVO_DEFAULT_MIN_ANGLE;
                config.params.servo.maxAngle = params["maxAngle"] | SERVO_DEFAULT_MAX_ANGLE;
                config.params.servo.initialAngle = params["initialAngle"] | SERVO_DEFAULT_INITIAL_ANGLE;
                config.params.servo.pwmChannel = params["pwmChannel"] | SERVO_DEFAULT_PWM_CHANNEL;
                config.params.servo.pwmResolution = params["pwmResolution"] | SERVO_DEFAULT_PWM_RESOLUTION;
                config.params.servo.inverted = params["inverted"] | false;
                config.params.servo.releaseAfterMove = params["releaseAfterMove"] | false;
            }
            else if (config.isGPIOPeripheral() && !isAdcInputType(config.type)) {
                config.params.gpio.initialState = static_cast<GPIOState>(params["initialState"] | 0);
                config.params.gpio.pwmChannel = params["pwmChannel"] | 0;
                config.params.gpio.pwmFrequency = params["pwmFrequency"] | 1000;
                config.params.gpio.pwmResolution = params["pwmResolution"] | 8;
                config.params.gpio.defaultDuty = params["defaultDuty"] | 0;
                config.params.gpio.passive = params["passive"] | false;
                // AO 模拟量输出模式（旧配置无此字段时默认关闭，保持原行为）
                config.params.gpio.aoMode = params["aoMode"] | false;
                config.params.gpio.aoMinPct = params["aoMinPct"] | 0;
                config.params.gpio.aoMaxPct = params["aoMaxPct"] | 100;
                config.params.gpio.inputMode = params["inputMode"] | static_cast<uint8_t>(DigitalInputMode::AUTO);
                if (config.params.gpio.inputMode > static_cast<uint8_t>(DigitalInputMode::PULL_DOWN)) {
                    config.params.gpio.inputMode = static_cast<uint8_t>(DigitalInputMode::AUTO);
                }
                config.params.gpio.invertLogic = params["invertLogic"] | false;
                int debounceMs = params["debounceMs"] | 0;
                if (debounceMs < 0) debounceMs = 0;
                if (debounceMs > 5000) debounceMs = 5000;
                config.params.gpio.debounceMs = static_cast<uint16_t>(debounceMs);
                config.params.gpio.reportMode = params["reportMode"] | static_cast<uint8_t>(DigitalReportMode::ON_CHANGE);
                if (config.params.gpio.reportMode > static_cast<uint8_t>(DigitalReportMode::EVENT_ONLY)) {
                    config.params.gpio.reportMode = static_cast<uint8_t>(DigitalReportMode::ON_CHANGE);
                }
            }
            else if (isAdcInputType(config.type)) {
                config.params.adc.attenuation = params["attenuation"] | 3;
                config.params.adc.resolution = params["resolution"] | 12;
                config.params.adc.sampleRate = params["sampleRate"] | 0;
                // 多采样滤波（旧配置无此字段时默认单次采样，保持原行为）
                config.params.adc.sampleCount = normalizeAdcSampleCount(params["sampleCount"] | 1);
                config.params.adc.filterMode = normalizeAdcFilterMode(params["filterMode"] | 0);
                config.params.adc.signalMode = params["signalMode"] | static_cast<uint8_t>(AnalogSignalMode::RAW);
                if (config.params.adc.signalMode > static_cast<uint8_t>(AnalogSignalMode::CUSTOM_LINEAR)) {
                    config.params.adc.signalMode = static_cast<uint8_t>(AnalogSignalMode::RAW);
                }
                config.params.adc.conversionEnabled = params["conversionEnabled"] | false;
                config.params.adc.clampEnabled = params["clampEnabled"] | false;
                int decimals = params["decimalPlaces"] | 2;
                if (decimals < 0) decimals = 0;
                if (decimals > 6) decimals = 6;
                config.params.adc.decimalPlaces = static_cast<uint8_t>(decimals);
                config.params.adc.inputMin = params["inputMin"] | 0.0f;
                config.params.adc.inputMax = params["inputMax"] | 0.0f;
                config.params.adc.engineeringMin = params["engineeringMin"] | 0.0f;
                config.params.adc.engineeringMax = params["engineeringMax"] | 100.0f;
                config.params.adc.engineeringOffset = params["engineeringOffset"] | 0.0f;
                config.params.adc.vRef = params["vRef"] | 0.0f;
                config.params.adc.voltageRatio = params["voltageRatio"] | 0.0f;
                config.params.adc.shuntResistance = params["shuntResistance"] | 0.0f;
                config.params.adc.inputGain = params["inputGain"] | 0.0f;
                config.params.adc.inputOffset = params["inputOffset"] | 0.0f;
                config.params.adc.rawMin = params["rawMin"] | 0;
                config.params.adc.rawMax = params["rawMax"] | 0;
            }
            else if (config.type == PeripheralType::DAC) {
                config.params.dac.channel = params["channel"] | 1;
            }
            else if (config.type == PeripheralType::STEPPER_MOTOR) {
                config.params.stepper.stepsPerRevolution = params["stepsPerRevolution"] | STEPPER_DEFAULT_STEPS_PER_REV;
                config.params.stepper.speed = clampStepperSpeed(params["speed"] | STEPPER_DEFAULT_RPM);
            }
            else if (config.type == PeripheralType::SDIO) {
                config.params.sdcard.interface = params["interface"] | SDCARD_INTERFACE_SPI;
                config.params.sdcard.frequency = params["frequency"] | SDCARD_DEFAULT_FREQUENCY;
            }
            else if (config.type == PeripheralType::DS1302_RTC) {
                config.params.ds1302.use24Hour = params["use24Hour"] | true;
                config.params.ds1302.trickleCharge = params["trickleCharge"] | 0;
            }
            else if (config.type == PeripheralType::LCD1602) {
                config.params.lcd1602.cols = params["cols"] | LCD1602_DEFAULT_COLS;
                config.params.lcd1602.rows = params["rows"] | LCD1602_DEFAULT_ROWS;
                config.params.lcd1602.address = params["address"] | LCD1602_DEFAULT_ADDRESS;
                config.params.lcd1602.backlight = params["backlight"] | true;
            }
            else if (config.type == PeripheralType::RFID_MFRC522) {
                config.params.rfid.frequency = params["frequency"] | RFID_DEFAULT_FREQUENCY;
            }
            else if (config.type == PeripheralType::LCD) {
                // 支持扩展宽度/高度（用 int 读取避免 uint8_t 默认值裁剪）
                int w = params["width"]  | 128;
                int h = params["height"] | 64;
                int iface = params["interface"] | 2; // 0=Parallel, 1=SPI, 2=I2C
                int contrast = params["contrast"] | 255; // 对比度，默认拉满
                config.params.lcd.width     = (uint8_t)(w > 255 ? 255 : (w < 0 ? 0 : w));
                config.params.lcd.height    = (uint8_t)(h > 255 ? 255 : (h < 0 ? 0 : h));
                config.params.lcd.interface = (uint8_t)(iface < 0 ? 2 : iface);
                config.params.lcd.contrast  = (uint8_t)(contrast > 255 ? 255 : (contrast < 0 ? 0 : contrast));
            }
            else if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
                int b = params["brightness"] | 2;
                if (b < 0) b = 0;
                if (b > 7) b = 7;
                config.params.segment.brightness = (uint8_t)b;
            }
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
            else if (config.type == PeripheralType::SENSOR_GENERIC) {
                config.params.sensor.sensorType = params["sensorType"] | 0;
                uint32_t interval = params["sampleInterval"] | SENSOR_DEFAULT_SAMPLE_INTERVAL_MS;
                if (interval < SENSOR_MIN_SAMPLE_INTERVAL_MS) interval = SENSOR_DEFAULT_SAMPLE_INTERVAL_MS;
                if (interval > SENSOR_MAX_SAMPLE_INTERVAL_MS) interval = SENSOR_MAX_SAMPLE_INTERVAL_MS;
                config.params.sensor.sampleInterval = interval;
                const char* driver = params["driver"] | "";
                strlcpy(config.params.sensor.driver, driver,
                        sizeof(config.params.sensor.driver));
                config.params.sensor.address = params["address"] | 0;
                config.params.sensor.mode = params["mode"] | 0;
                config.params.sensor.frequency = params["frequency"] | 100000;
            }
        }

        if (!config.id.isEmpty() && config.type != PeripheralType::UNCONFIGURED) {
            String errorMsg;
            bool valid = validateConfig(config, errorMsg);
            if (!valid) {
                LOG_WARNINGF("Peripheral Manager: Invalid config '%s' loaded with error status: %s",
                             config.id.c_str(),
                             errorMsg.c_str());
            }

            peripherals[config.id] = config;

            PeripheralRuntimeState state;
            state.id = config.id;
            if (valid) {
                state.status = config.enabled ? PeripheralStatus::PERIPHERAL_ENABLED : PeripheralStatus::PERIPHERAL_DISABLED;
            } else {
                state.status = PeripheralStatus::PERIPHERAL_ERROR;
            }
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

    if (!isPeripheralTypeImplemented(config.type)) {
        errorMsg = String("外设类型 ") + getPeripheralTypeName(config.type) + " 尚未实现硬件驱动";
        return false;
    }

    if (config.type == PeripheralType::SDIO ||
        config.type == PeripheralType::DS1302_RTC ||
        config.type == PeripheralType::LCD1602 ||
        config.type == PeripheralType::RFID_MFRC522) {
        for (const auto& pair : peripherals) {
            if (pair.first != config.id && pair.second.type == config.type) {
                errorMsg = String("SINGLETON_RESOURCE_BUSY: ") +
                           getPeripheralTypeName(config.type) +
                           " 当前固件仅允许一个实例";
                return false;
            }
        }
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

    // Generic sensors may represent an external/logical data source and are
    // therefore valid without a local GPIO. When a pin is supplied, still run
    // the normal chip/reserved-pin checks below.
    if (config.type == PeripheralType::SENSOR_GENERIC) {
        const uint32_t interval = config.params.sensor.sampleInterval;
        if (interval != 0 && (interval < SENSOR_MIN_SAMPLE_INTERVAL_MS ||
                              interval > SENSOR_MAX_SAMPLE_INTERVAL_MS)) {
            errorMsg = "通用传感器采样间隔必须在 100ms 到 24h 之间";
            return false;
        }
        if (config.params.sensor.driver[0] != '\0') {
            const String driver(config.params.sensor.driver);
            if (!driver.equalsIgnoreCase("BMP280") &&
                !driver.equalsIgnoreCase("MPU6050")) {
                errorMsg = "SENSOR_DRIVER_UNSUPPORTED: 仅支持 BMP280 或 MPU6050";
                return false;
            }
            if (config.pinCount < 2 || config.pins[0] == config.pins[1]) {
                errorMsg = "I2C 传感器需要不同的 SDA/SCL 两个引脚";
                return false;
            }
            if (config.params.sensor.address == 0 ||
                config.params.sensor.address > 0x7F) {
                errorMsg = "I2C 传感器地址必须在 0x01-0x7F";
                return false;
            }
            if (config.params.sensor.frequency != 100000 &&
                config.params.sensor.frequency != 400000) {
                errorMsg = "I2C 传感器频率仅支持 100000 或 400000";
                return false;
            }
            if (config.params.sensor.mode > 1) {
                errorMsg = "传感器模式无效";
                return false;
            }
        } else if (config.pinCount == 0) {
            return true;
        }
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

    if (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT && config.params.gpio.passive) {
        if (config.params.gpio.pwmChannel >= CHIP_MAX_PWM_CH ||
            config.params.gpio.pwmFrequency == 0 ||
            config.params.gpio.pwmResolution < 1 || config.params.gpio.pwmResolution > 16 ||
            (uint64_t)config.params.gpio.pwmFrequency *
                    (1ULL << config.params.gpio.pwmResolution) > 80000000ULL) {
            errorMsg = "无源蜂鸣器 PWM 参数无效";
            return false;
        }
    }

    // 类型特定参数验证
    switch (config.type) {
        case PeripheralType::GPIO_DIGITAL_INPUT:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLUP:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN:
        case PeripheralType::GPIO_INTERRUPT_RISING:
        case PeripheralType::GPIO_INTERRUPT_FALLING:
        case PeripheralType::GPIO_INTERRUPT_CHANGE:
            if (config.params.gpio.inputMode > static_cast<uint8_t>(DigitalInputMode::PULL_DOWN)) {
                errorMsg = "DI 输入模式无效 (有效值: 0=自动, 1=浮空, 2=上拉, 3=下拉)";
                return false;
            }
            if (config.params.gpio.debounceMs > 5000) {
                errorMsg = "DI 去抖时间无效 (有效范围: 0-5000ms)";
                return false;
            }
            if (config.params.gpio.reportMode > static_cast<uint8_t>(DigitalReportMode::EVENT_ONLY)) {
                errorMsg = "DI 上报模式无效 (有效值: 0-2)";
                return false;
            }
            break;

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

        case PeripheralType::PWM_SERVO:
            if (!isServoConfigValid(config)) {
                errorMsg = "舵机参数无效：频率 40-400Hz，脉宽 100-5000us，角度 0-360 度";
                return false;
            }
            if (config.params.servo.pwmChannel >= CHIP_MAX_PWM_CH) {
                errorMsg = "舵机 PWM 通道无效 (有效范围: 0-" + String(CHIP_MAX_PWM_CH - 1) + ")";
                return false;
            }
            if (static_cast<uint64_t>(config.params.servo.frequency) *
                    (1ULL << config.params.servo.pwmResolution) > 80000000ULL) {
                errorMsg = "舵机 PWM 频率和分辨率组合无效";
                return false;
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
            if (config.params.adc.sampleCount > 16) {
                errorMsg = "ADC 采样次数无效 (有效范围: 1-16)";
                return false;
            }
            if (config.params.adc.filterMode > 1) {
                errorMsg = "ADC 滤波模式无效 (有效值: 0=均值, 1=中值)";
                return false;
            }
            if (config.params.adc.signalMode > static_cast<uint8_t>(AnalogSignalMode::CUSTOM_LINEAR) ||
                config.params.adc.decimalPlaces > 6) {
                errorMsg = "AI 转换参数无效";
                return false;
            }
            if (config.params.adc.conversionEnabled &&
                config.params.adc.signalMode != static_cast<uint8_t>(AnalogSignalMode::RAW)) {
                const auto& a = config.params.adc;
                if (isnan(a.engineeringMin) || isinf(a.engineeringMin) ||
                    isnan(a.engineeringMax) || isinf(a.engineeringMax) ||
                    a.engineeringMin == a.engineeringMax) {
                    errorMsg = "AI 工程量上下限不能相同或为空";
                    return false;
                }
                const bool hasExplicitInputRange =
                    a.inputMin != 0.0f || a.inputMax != 0.0f;
                if ((hasExplicitInputRange &&
                     (isnan(a.inputMin) || isinf(a.inputMin) ||
                      isnan(a.inputMax) || isinf(a.inputMax) ||
                      a.inputMax <= a.inputMin)) ||
                    (!isnan(a.voltageRatio) && !isinf(a.voltageRatio) && a.voltageRatio < 0.0f) ||
                    (!isnan(a.shuntResistance) && !isinf(a.shuntResistance) && a.shuntResistance < 0.0f) ||
                    (!isnan(a.inputGain) && !isinf(a.inputGain) && a.inputGain < 0.0f)) {
                    errorMsg = "AI 输入量程或校准参数无效";
                    return false;
                }
                if (a.signalMode == static_cast<uint8_t>(AnalogSignalMode::CUSTOM_LINEAR) &&
                    a.rawMax > 0 && a.rawMax <= a.rawMin) {
                    errorMsg = "AI 自定义原始量程无效";
                    return false;
                }
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

        case PeripheralType::ENCODER:
            // 编码器参数验证
            if (config.pinCount < 2) {
                errorMsg = "编码器需要 2 个引脚 (A相, B相)";
                return false;
            }
            if (config.pins[0] == config.pins[1]) {
                errorMsg = "编码器 A相和B相不能使用同一引脚";
                return false;
            }
            if (config.params.encoder.resolution == 0) {
                errorMsg = "编码器每转脉冲数不能为0";
                return false;
            }
            if (!config.params.encoder.useInterrupt) {
                errorMsg = "ENCODER_INTERRUPT_REQUIRED: 当前固件仅支持 GPIO ISR 编码器模式";
                return false;
            }
            break;

        case PeripheralType::SDIO:
            if (config.params.sdcard.interface != SDCARD_INTERFACE_SPI) {
                errorMsg = "SDMMC_UNSUPPORTED: 当前固件仅支持 SDSPI";
                return false;
            }
            if (config.pinCount < 4) {
                errorMsg = "SDSPI 需要 4 个引脚 (SCK,MOSI,MISO,CS)";
                return false;
            }
            if (config.params.sdcard.frequency < 100000UL ||
                config.params.sdcard.frequency > 40000000UL) {
                errorMsg = "SDSPI 频率必须在 100kHz-40MHz";
                return false;
            }
            break;

        case PeripheralType::DS1302_RTC:
            if (config.pinCount < 3 || config.pins[0] == config.pins[1] ||
                config.pins[0] == config.pins[2] || config.pins[1] == config.pins[2]) {
                errorMsg = "DS1302 需要不同的 CE/IO/SCLK 三个引脚";
                return false;
            }
            switch (config.params.ds1302.trickleCharge) {
                case 0x00: case 0xA5: case 0xA6: case 0xA7:
                case 0xA9: case 0xAA: case 0xAB:
                    break;
                default:
                    errorMsg = "DS1302 涓流充电模式无效";
                    return false;
            }
            break;

        case PeripheralType::LCD1602:
            if (config.pinCount < 2 || config.pins[0] == config.pins[1]) {
                errorMsg = "LCD1602 需要不同的 SDA/SCL 两个引脚";
                return false;
            }
            if (!((config.params.lcd1602.cols == 16 && config.params.lcd1602.rows == 2) ||
                  (config.params.lcd1602.cols == 20 && config.params.lcd1602.rows == 4))) {
                errorMsg = "LCD1602 尺寸仅支持 16x2 或 20x4";
                return false;
            }
            if (config.params.lcd1602.address != 0 &&
                !((config.params.lcd1602.address >= 0x20 && config.params.lcd1602.address <= 0x27) ||
                  (config.params.lcd1602.address >= 0x38 && config.params.lcd1602.address <= 0x3F))) {
                errorMsg = "LCD1602 I2C 地址需为 0(自动)、0x20-0x27 或 0x38-0x3F";
                return false;
            }
            break;

        case PeripheralType::RFID_MFRC522:
            if (config.pinCount < 5) {
                errorMsg = "MFRC522 需要 5 个引脚 (SCK,MOSI,MISO,SS,RST)";
                return false;
            }
            for (uint8_t i = 0; i < 5; ++i) {
                for (uint8_t j = i + 1; j < 5; ++j) {
                    if (config.pins[i] == config.pins[j]) {
                        errorMsg = "MFRC522 的 SCK/MOSI/MISO/SS/RST 引脚不能重复";
                        return false;
                    }
                }
            }
            if (config.params.rfid.frequency != RFID_DEFAULT_FREQUENCY) {
                errorMsg = "MFRC522 SPI 频率固定为 4000000Hz";
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

        case PeripheralType::IR_RECEIVER:
            if (config.pinCount < 1) {
                errorMsg = "IR receiver requires 1 data pin";
                return false;
            }
#if !FASTBEE_ENABLE_IR_REMOTE
            errorMsg = "固件未启用红外接收支持 (FASTBEE_ENABLE_IR_REMOTE=0)";
            return false;
#else
            break;
#endif

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

    // Legacy GPIO_ANALOG_INPUT shares the ADC parameter layout and must reach
    // the ADC initializer instead of the generic GPIO path.
    if (config.isGPIOPeripheral() && !isAdcInputType(config.type)) {
        if (!setupGPIOPin(config)) return false;
        // JSON 中断类型 18/19/20 是可直接使用的外设类型：初始化时即注册
        // 对应边沿。回调可为空，队列处理仍会更新运行时状态；规则/MQTT
        // 上报由调度器统一做稳定态去抖，避免 ISR 中分配内存。
        if (isGpioInterruptInputType(config.type) &&
            !attachInterrupt(config.id, nullptr)) {
            LOG_ERRORF("Peripheral Manager: Failed to attach configured interrupt '%s'",
                       config.id.c_str());
            return false;
        }
        return true;
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

    if (config.type == PeripheralType::SDIO) {
        String errorMsg;
        const bool ok = SDCardManager::getInstance().unmount(config.id, &errorMsg);
        if (!ok) {
            LOG_WARNINGF("Peripheral Manager: SD '%s' unmount failed: %s",
                         config.id.c_str(), errorMsg.c_str());
        }
        return ok;
    }

    if (config.type == PeripheralType::DS1302_RTC) {
#if FASTBEE_ENABLE_DS1302
        DS1302Driver::getInstance().end();
#endif
        return true;
    }

    if (config.type == PeripheralType::LCD1602) {
#if FASTBEE_ENABLE_LCD1602
        LCD1602Driver::getInstance().end();
#endif
        return true;
    }

    if (config.type == PeripheralType::RFID_MFRC522) {
#if FASTBEE_ENABLE_RFID
        RFIDDriver::getInstance().stop();
#endif
        return true;
    }

    if (config.type == PeripheralType::SENSOR_GENERIC) {
        FastBee::SensorDriverCache::instance().release(config.id);
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
#if FASTBEE_ENABLE_RF_LEARN
            RFLearnDriver::getInstance().begin(pin);
#endif
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

    if (config.type == PeripheralType::IR_RECEIVER) {
        if (config.pinCount < 1) {
            LOG_WARNINGF("Peripheral Manager: IR receiver '%s' requires 1 data pin", config.id.c_str());
            return false;
        }
#if FASTBEE_ENABLE_IR_REMOTE
        if (!IRRemoteDriver::getInstance().begin(config.pins[0])) {
            LOG_ERRORF("Peripheral Manager: IR receiver '%s' init failed on GPIO%d",
                       config.id.c_str(), config.pins[0]);
            return false;
        }
        LOG_INFOF("Peripheral Manager: IR receiver '%s' initialized DATA=%d",
                  config.id.c_str(), config.pins[0]);
        return true;
#else
        LOG_WARNINGF("Peripheral Manager: IR receiver '%s' skipped - firmware built without FASTBEE_ENABLE_IR_REMOTE",
                     config.id.c_str());
        return false;
#endif
    }

    // ========== 编码器初始化（GPIO ISR） ==========
    if (config.type == PeripheralType::ENCODER) {
        if (config.pinCount < 2) {
            LOG_WARNINGF("Peripheral Manager: Encoder '%s' requires 2 pins (A, B)", config.id.c_str());
            return false;
        }
        uint8_t pinA = config.pins[0];
        uint8_t pinB = config.pins[1];
        if (isReservedPin(pinA) || isReservedPin(pinB)) {
            LOG_ERRORF("Peripheral Manager: Encoder '%s' rejects reserved pins A=%d B=%d on %s",
                       config.id.c_str(), pinA, pinB, CHIP_NAME);
            return false;
        }

        // 配置两个GPIO为输入模式（上拉）
        // A相和B相都需要上拉电阻以确保稳定读取
        pinMode(pinA, INPUT_PULLUP);
        pinMode(pinB, INPUT_PULLUP);
        
        if (!config.params.encoder.useInterrupt) {
            LOG_ERRORF("Peripheral Manager: Encoder '%s' requires GPIO interrupt mode",
                       config.id.c_str());
            return false;
        }

        // 分配静态 ISR 槽位（含计数器和测速状态），槽位不足必须失败，
        // 因为当前固件没有轮询或 PCNT 回退实现。
        EncoderIsrSlot* slot = allocEncoderSlot(config.id, pinA, pinB);
        if (!slot) {
            LOG_ERRORF("Peripheral Manager: ENCODER_RESOURCE_EXHAUSTED for '%s' (max %u)",
                       config.id.c_str(), (unsigned)MAX_ENCODER_SLOTS);
            return false;
        }

        // A 相上升沿和下降沿都触发；ISR 同时读取 A/B 相完成方向判定。
        attachInterruptArg(pinA, handleEncoderInterrupt, slot, CHANGE);

        auto stateIt = runtimeStates.find(config.id);
        if (stateIt != runtimeStates.end()) {
            memset(&stateIt->second.state.encoder, 0, sizeof(stateIt->second.state.encoder));
        }

        LOG_INFOF("Peripheral Manager: Encoder '%s' using GPIO interrupt mode (A=%d, B=%d)",
                  config.id.c_str(), pinA, pinB);

        LOG_INFOF("Peripheral Manager: Encoder '%s' initialized A=%d B=%d resolution=%d interrupt=%d",
                  config.id.c_str(), pinA, pinB,
                  config.params.encoder.resolution,
                  1);
        return true;
    }

    // ========== SD 卡（SDSPI）初始化 ==========
    if (config.type == PeripheralType::SDIO) {
        if (config.params.sdcard.interface != SDCARD_INTERFACE_SPI ||
            config.pinCount < 4) {
            LOG_ERRORF("Peripheral Manager: SD '%s' requires SDSPI SCK/MOSI/MISO/CS",
                       config.id.c_str());
            return false;
        }
        String errorMsg;
        const bool mounted = SDCardManager::getInstance().configureAndMount(
            config.id, config.pins[0], config.pins[1], config.pins[2],
            config.pins[3], config.params.sdcard.frequency, errorMsg);
        if (!mounted) {
            LOG_ERRORF("Peripheral Manager: SD '%s' mount failed: %s",
                       config.id.c_str(), errorMsg.c_str());
        }
        return mounted;
    }

    // ========== DS1302 RTC ==========
    if (config.type == PeripheralType::DS1302_RTC) {
#if FASTBEE_ENABLE_DS1302
        if (config.pinCount < 3 ||
            !DS1302Driver::getInstance().begin(config.pins[0], config.pins[1],
                                                config.pins[2])) {
            LOG_ERRORF("Peripheral Manager: DS1302 '%s' initialization failed",
                       config.id.c_str());
            return false;
        }
        DS1302Driver::getInstance().set24HourMode(config.params.ds1302.use24Hour);
        DS1302Driver::getInstance().setTrickleCharge(
            static_cast<DS1302TrickleCharge>(config.params.ds1302.trickleCharge));
        return true;
#else
        LOG_ERROR("Peripheral Manager: DS1302 support is not compiled");
        return false;
#endif
    }

    // ========== LCD1602 I2C ==========
    if (config.type == PeripheralType::LCD1602) {
#if FASTBEE_ENABLE_LCD1602
        if (config.pinCount < 2) return false;
        LCD1602Config lcdConfig;
        lcdConfig.cols = config.params.lcd1602.cols;
        lcdConfig.rows = config.params.lcd1602.rows;
        lcdConfig.i2cAddress = config.params.lcd1602.address;
        lcdConfig.backlight = config.params.lcd1602.backlight;
        return LCD1602Driver::getInstance().begin(config.pins[0], config.pins[1],
                                                   lcdConfig);
#else
        LOG_ERROR("Peripheral Manager: LCD1602 support is not compiled");
        return false;
#endif
    }

    // ========== MFRC522 RFID ==========
    if (config.type == PeripheralType::RFID_MFRC522) {
#if FASTBEE_ENABLE_RFID
        if (config.pinCount < 5) return false;
        // pins: SCK, MOSI, MISO, SS, RST
        return RFIDDriver::getInstance().begin(
            config.pins[3], config.pins[4], config.pins[0], config.pins[1],
            config.pins[2]);
#else
        LOG_ERROR("Peripheral Manager: RFID support is not compiled");
        return false;
#endif
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
    if (isAdcInputType(config.type)) {
        uint8_t pin = config.getPrimaryPin();
        if (pin == 255) {
            LOG_WARNINGF("Peripheral Manager: analog input '%s' requires a valid analog pin", config.id.c_str());
            return false;
        }
        // 设置 ADC 分辨率（衰减在 PeriphExecExecutor 的传感器读取中按需配置）
#if defined(SOC_ADC_MAX_BITWIDTH) && SOC_ADC_MAX_BITWIDTH >= 9
        analogReadResolution(config.params.adc.resolution);
#endif
        // 执行一次空读以稳定 ADC
        analogRead(pin);
        LOG_INFOF("Peripheral Manager: analog input '%s' initialized pin=%d atten=%d res=%d",
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

    // ========== SENSOR_GENERIC 通用传感器容器初始化 ==========
    if (config.type == PeripheralType::SENSOR_GENERIC) {
        if (config.params.sensor.driver[0] != '\0') {
            if (config.pinCount < 2) return false;
            Wire.begin(static_cast<int>(config.pins[0]),
                       static_cast<int>(config.pins[1]),
                       config.params.sensor.frequency);
            LOG_INFOF("Peripheral Manager: I2C sensor '%s' driver=%s SDA=%d SCL=%d addr=0x%02X",
                      config.id.c_str(), config.params.sensor.driver,
                      config.pins[0], config.pins[1], config.params.sensor.address);
        } else if (config.getPrimaryPin() != 255) {
            const uint8_t pin = config.getPrimaryPin();
            // 根据传感器类型设置引脚模式
            pinMode(pin, INPUT_PULLUP);
            LOG_INFOF("Peripheral Manager: Generic Sensor '%s' initialized pin=%d sensorType=%d interval=%ums",
                      config.id.c_str(), pin, config.params.sensor.sensorType,
                      (unsigned)config.params.sensor.sampleInterval);
        } else {
            LOG_INFOF("Peripheral Manager: Generic Sensor '%s' registered (no local pin) sensorType=%d",
                      config.id.c_str(), config.params.sensor.sensorType);
        }
        return true;
    }

    // Unknown or compile-time disabled types must fail closed. Reporting a
    // successful initialization here would allow rules to drive no hardware.
    int typeVal = static_cast<int>(config.type);
    LOG_ERRORF("Peripheral Manager: Type %d ('%s') has no available hardware driver",
               typeVal, config.id.c_str());
    return false;
}

bool PeripheralManager::teardownHardware(const PeripheralConfig& config) {
    if (config.isGPIOPeripheral()) {
        uint8_t pin = config.getPrimaryPin();
        if (pin != 255) {
            detachInterrupt(pin);
        }
    }

    // 编码器：先 detach A 相中断再释放 ISR 槽位，防止删除/更新后 ISR 悬垂
    if (config.type == PeripheralType::ENCODER) {
        if (config.pinCount >= 1 && config.pins[0] != 255) {
            detachInterrupt(config.pins[0]);
        }
        releaseEncoderSlot(config.id);
    }

    if (config.type == PeripheralType::UART) {
        auto it = uartPortById.find(config.id);
        if (it != uartPortById.end()) {
            HardwareSerial* serial = serialForPort(it->second);
            if (serial) serial->end();
            uartPortById.erase(it);
        }
        auto stateIt = runtimeStates.find(config.id);
        if (stateIt != runtimeStates.end()) {
            stateIt->second.state.comm.isConnected = false;
            stateIt->second.state.comm.port = UART_PORT_UNASSIGNED;
            stateIt->second.state.comm.rxAvailable = 0;
        }
        return true;
    }

    // I2C 总线清理
    if (config.type == PeripheralType::I2C) {
        bool sharedBusActive = false;
        for (const auto& pair : peripherals) {
            if (pair.first == config.id || !pair.second.enabled || pair.second.pinCount < 2) continue;
            const PeripheralConfig& other = pair.second;
            const bool i2cUser = other.type == PeripheralType::I2C ||
                                 other.type == PeripheralType::LCD1602 ||
                                 (other.type == PeripheralType::SENSOR_GENERIC &&
                                  other.params.sensor.driver[0] != '\0');
            if (i2cUser && other.pins[0] == config.pins[0] &&
                other.pins[1] == config.pins[1]) {
                sharedBusActive = true;
                break;
            }
        }
        if (!sharedBusActive) Wire.end();
        LOG_INFOF("Peripheral Manager: I2C '%s' %s", config.id.c_str(),
                  sharedBusActive ? "released; shared bus retained" : "bus released");
        return true;
    }

    // SPI 总线清理
    if (config.type == PeripheralType::SPI) {
        bool sharedBusActive = false;
        for (const auto& pair : peripherals) {
            if (pair.first == config.id || !pair.second.enabled) continue;
            const PeripheralConfig& other = pair.second;
            if (other.type == PeripheralType::SDIO && other.pinCount >= 4 &&
                other.pins[0] == config.pins[2] &&
                other.pins[1] == config.pins[1] &&
                other.pins[2] == config.pins[0]) {
                sharedBusActive = true;
                break;
            }
            if (other.type == PeripheralType::RFID_MFRC522 && other.pinCount >= 5 &&
                other.pins[0] == config.pins[2] &&
                other.pins[1] == config.pins[1] &&
                other.pins[2] == config.pins[0]) {
                sharedBusActive = true;
                break;
            }
        }
        if (!sharedBusActive) SPI.end();
        // 释放 CS 引脚
        if (config.pinCount >= 4) {
            uint8_t cs = config.pins[3];
            if (cs != 255 && !isReservedPin(cs)) {
                pinMode(cs, INPUT);
            }
        }
        LOG_INFOF("Peripheral Manager: SPI '%s' %s", config.id.c_str(),
                  sharedBusActive ? "released; shared bus retained" : "bus released");
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

    // 红外接收头清理：停止解码并释放接收缓冲区
#if FASTBEE_ENABLE_IR_REMOTE
    if (config.type == PeripheralType::IR_RECEIVER) {
        IRRemoteDriver::getInstance().stop();
    }
#endif

    // GPIO 输出清理：置低电平后释放为输入，防止外设继续带电或浮空
    if (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT) {
        if (config.pinCount >= 1 && !isReservedPin(config.pins[0]) && !isInputOnlyPin(config.pins[0])) {
            if (config.params.gpio.passive) {
                ledcWrite(config.pins[0], 0);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
                ledcDetach(config.pins[0]);
#else
                ledcDetachPin(config.pins[0]);
#endif
            } else {
                digitalWrite(config.pins[0], LOW);
            }
            pinMode(config.pins[0], INPUT);
        }
    }

    if (config.type == PeripheralType::PWM_SERVO) {
        cancelServoRelease(config.id);
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
            auto stateIt = runtimeStates.find(config.id);
            if (stateIt != runtimeStates.end() && config.type == PeripheralType::PWM_SERVO) {
                stateIt->second.state.servo.signalAttached = false;
            }
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
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLUP:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN:
        case PeripheralType::GPIO_INTERRUPT_RISING:
        case PeripheralType::GPIO_INTERRUPT_FALLING:
        case PeripheralType::GPIO_INTERRUPT_CHANGE: {
            uint8_t inputMode = config.params.gpio.inputMode;
            if (inputMode == static_cast<uint8_t>(DigitalInputMode::AUTO)) {
                inputMode = config.type == PeripheralType::GPIO_DIGITAL_INPUT_PULLUP
                                ? static_cast<uint8_t>(DigitalInputMode::PULL_UP)
                                : config.type == PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN
                                    ? static_cast<uint8_t>(DigitalInputMode::PULL_DOWN)
                                    : static_cast<uint8_t>(DigitalInputMode::FLOATING);
            }
            if (inputMode == static_cast<uint8_t>(DigitalInputMode::PULL_UP)) pinMode(pin, INPUT_PULLUP);
            else if (inputMode == static_cast<uint8_t>(DigitalInputMode::PULL_DOWN)) pinMode(pin, INPUT_PULLDOWN);
            else pinMode(pin, INPUT);
            break;
        }

        case PeripheralType::GPIO_DIGITAL_OUTPUT:
            if (config.params.gpio.passive) {
                // 无源蜂鸣器：使用 PWM 驱动（默认 2700Hz / 8bit）
                uint32_t freq = config.params.gpio.pwmFrequency >= 100
                                    ? config.params.gpio.pwmFrequency : 2700;
                uint8_t res = config.params.gpio.pwmResolution > 0
                                    ? config.params.gpio.pwmResolution : 8;
                uint8_t channel = config.params.gpio.pwmChannel;
                if (channel >= CHIP_MAX_PWM_CH || !ledcAttachChannel(pin, freq, res, channel)) {
                    LOG_ERRORF("Peripheral Manager: Passive buzzer LEDC attach failed pin=%u channel=%u",
                               (unsigned)pin, (unsigned)channel);
                    return false;
                }
                if (!ledcWrite(pin, 0)) return false;  // 初始静音
                LOG_INFOF("Peripheral Manager: Passive buzzer '%s' pin=%d freq=%lu res=%d",
                          config.id.c_str(), pin, (unsigned long)freq, res);
                return true;
            }
            pinMode(pin, OUTPUT);
            writePin(config.id, config.params.gpio.initialState);
            break;

        case PeripheralType::GPIO_ANALOG_INPUT:
            // ESP32模拟输入不需要特殊设置，但仍应用统一 ADC 参数，避免
            // GPIO_ANALOG_INPUT 与 ADC 两条配置路径行为不一致。
#if defined(SOC_ADC_MAX_BITWIDTH) && SOC_ADC_MAX_BITWIDTH >= 9
            analogReadResolution((config.params.adc.resolution >= 9 && config.params.adc.resolution <= 12)
                                     ? config.params.adc.resolution : 12);
#endif
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
    if (config.type == PeripheralType::PWM_SERVO) {
        return setupServoPin(config);
    }

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

    if (!ledcAttachChannel(pin, config.params.gpio.pwmFrequency,
                           config.params.gpio.pwmResolution, channel)) {
        LOG_ERRORF("Peripheral Manager: LEDC attach failed pin=%u channel=%u",
                   (unsigned)pin, (unsigned)channel);
        return false;
    }

    // 设置初始值
    if (config.params.gpio.initialState == GPIOState::STATE_HIGH) {
        ledcWrite(pin, (1 << config.params.gpio.pwmResolution) - 1);
    } else {
        ledcWrite(pin, 0);
    }

    return true;
}

bool PeripheralManager::attachServoSignal(const PeripheralConfig& config) {
    if (config.type != PeripheralType::PWM_SERVO || !isServoConfigValid(config)) return false;
    const uint8_t pin = config.getPrimaryPin();
    if (pin == 255 || !isValidPin(pin) || isReservedPin(pin) || isInputOnlyPin(pin)) {
        return false;
    }

    auto stateIt = runtimeStates.find(config.id);
    if (stateIt != runtimeStates.end() && stateIt->second.state.servo.signalAttached) {
        return true;
    }
    auto releaseIt = servoReleaseTickers.find(config.id);
    if (releaseIt != servoReleaseTickers.end()) releaseIt->second->ticker.detach();

    if (!ledcAttachChannel(pin, config.params.servo.frequency,
                           config.params.servo.pwmResolution,
                           config.params.servo.pwmChannel)) {
        LOG_ERRORF("Peripheral Manager: Servo LEDC attach failed id='%s' pin=%u channel=%u",
                   config.id.c_str(), static_cast<unsigned>(pin),
                   static_cast<unsigned>(config.params.servo.pwmChannel));
        return false;
    }
    if (stateIt != runtimeStates.end()) {
        stateIt->second.state.servo.signalAttached = true;
    }
    return true;
}

bool PeripheralManager::setupServoPin(const PeripheralConfig& config) {
    if (!attachServoSignal(config)) return false;
    const uint16_t pulseUs = servoAngleToPulseUs(
        config.params.servo.initialAngle,
        config.params.servo.minAngle, config.params.servo.maxAngle,
        config.params.servo.minPulse, config.params.servo.maxPulse,
        config.params.servo.inverted);
    const uint32_t duty = servoPulseUsToDuty(
        pulseUs, config.params.servo.frequency, config.params.servo.pwmResolution);
    if (!ledcWrite(config.getPrimaryPin(), duty)) {
        ledcDetach(config.getPrimaryPin());
        return false;
    }

    auto stateIt = runtimeStates.find(config.id);
    if (stateIt != runtimeStates.end()) {
        stateIt->second.state.servo.currentAngle = config.params.servo.initialAngle;
        stateIt->second.state.servo.currentPulseUs = pulseUs;
        stateIt->second.state.servo.signalAttached = true;
    }
    if (config.params.servo.releaseAfterMove) scheduleServoRelease(config.id);
    LOG_INFOF("Peripheral Manager: Servo '%s' initialized pin=%u angle=%d pulse=%uus",
              config.id.c_str(), static_cast<unsigned>(config.getPrimaryPin()),
              static_cast<int>(config.params.servo.initialAngle),
              static_cast<unsigned>(pulseUs));
    return true;
}

void PeripheralManager::servoReleaseTickerCallback(ServoReleaseTickerData* data) {
    if (data && data->mgr) data->mgr->releaseServoSignal(data->id);
}

void PeripheralManager::scheduleServoRelease(const String& id) {
    RecursiveMutexGuard lock(_mutex);
    ServoReleaseTickerData* data = nullptr;
    auto it = servoReleaseTickers.find(id);
    if (it != servoReleaseTickers.end()) {
        data = it->second;
        data->ticker.detach();
    } else {
        data = new (std::nothrow) ServoReleaseTickerData();
        if (!data) {
            LOG_WARNINGF("Peripheral Manager: Servo release timer allocation failed for '%s'", id.c_str());
            return;
        }
        data->mgr = this;
        data->id = id;
        servoReleaseTickers[id] = data;
    }
    data->ticker.once_ms(SERVO_RELEASE_DELAY_MS, servoReleaseTickerCallback, data);
}

void PeripheralManager::cancelServoRelease(const String& id) {
    RecursiveMutexGuard lock(_mutex);
    auto it = servoReleaseTickers.find(id);
    if (it == servoReleaseTickers.end()) return;
    it->second->ticker.detach();
    delete it->second;
    servoReleaseTickers.erase(it);
}

bool PeripheralManager::releaseServoSignal(const String& id) {
    RecursiveMutexGuard lock(_mutex);
    PeripheralConfig* config = getPeripheral(id);
    if (!config || !config->enabled || config->type != PeripheralType::PWM_SERVO ||
        !config->params.servo.releaseAfterMove) {
        return false;
    }
    const uint8_t pin = config->getPrimaryPin();
    if (pin == 255 || isReservedPin(pin) || isInputOnlyPin(pin)) return false;

    ledcWrite(pin, 0);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
    ledcDetach(pin);
#else
    ledcDetachPin(pin);
#endif
    pinMode(pin, INPUT);
    auto stateIt = runtimeStates.find(id);
    if (stateIt != runtimeStates.end()) {
        stateIt->second.status = PeripheralStatus::PERIPHERAL_INITIALIZED;
        stateIt->second.lastActivity = millis();
        stateIt->second.state.servo.signalAttached = false;
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
        auto stateIt = runtimeStates.find(config.id);
        if (stateIt != runtimeStates.end()) {
            stateIt->second.state.comm.isConnected = false;
            stateIt->second.state.comm.port = UART_PORT_UNASSIGNED;
            stateIt->second.state.comm.rxAvailable = 0;
        }
        return true;
    }

    String capacityError;
    if (!validateUartCapacity(config, config.id, capacityError)) {
        lastEnableError = capacityError;
        LOG_WARNINGF("Peripheral Manager: %s", capacityError.c_str());
        return false;
    }

    uint8_t rxPin = config.pins[0];
    uint8_t txPin = config.pins[1];
    uint8_t port = UART_PORT_UNASSIGNED;

    auto existing = uartPortById.find(config.id);
    if (existing != uartPortById.end()) {
        port = existing->second;
    } else {
        bool used[CHIP_UART_COUNT] = {false};
        used[0] = true; // UART0 is exclusively reserved for logging/USB.
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
    uint32_t baud = config.params.uart.baudRate > 0
        ? config.params.uart.baudRate : UART_DEFAULT_BAUD_RATE;
    serial->begin(baud, uartSerialConfig(config), rxPin, txPin);
    serial->setTimeout(0);
    auto stateIt = runtimeStates.find(config.id);
    if (stateIt != runtimeStates.end()) {
        stateIt->second.state.comm.bytesSent = 0;
        stateIt->second.state.comm.bytesReceived = 0;
        stateIt->second.state.comm.isConnected = true;
        stateIt->second.state.comm.port = port;
        stateIt->second.state.comm.rxAvailable = boundedUartAvailable(serial);
    }
    LOG_INFOF("Peripheral Manager: UART '%s' initialized on Serial%d RX=%d TX=%d baud=%lu framing=%s",
              config.id.c_str(), port, rxPin, txPin, (unsigned long)baud,
              uartFramingName(config).c_str());
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
    ledcWrite(data->pin, duty);
    xSemaphoreGiveRecursive(mtx);
}

void PeripheralManager::startActionTicker(const String& id, uint8_t actionMode, uint16_t paramValue) {
    RecursiveMutexGuard lock(_mutex);
    stopActionTicker(id);  // 先清理已有的

    auto config = getPeripheral(id);
    if (!config) return;

    if (actionMode == 1) {  // BLINK
        // 运行期分配三件套（审计 P2-7）：水位预检 + nothrow + 判空降级，
        // 避免 MemGuard CRITICAL 边缘一次 BLINK 动作 OOM → abort 整机重启
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < FastBee::MemoryBudget::ACTION_TICKER_MIN_DRAM) {
            LOG_WARNINGF("Peripheral Manager: skip blink ticker for '%s': DRAM below %u",
                         id.c_str(), (unsigned)FastBee::MemoryBudget::ACTION_TICKER_MIN_DRAM);
            return;
        }
        ActionTickerData* data = new (std::nothrow) ActionTickerData();
        if (!data) {
            LOG_WARNINGF("Peripheral Manager: blink ticker alloc failed for '%s'", id.c_str());
            return;
        }
        data->mgr = this;
        data->id = id;
        actionTickers[id] = data;

        float intervalSec = paramValue / 1000.0f;
        data->ticker.attach(intervalSec, blinkTickerCallback, data);
        LOG_INFOF("Peripheral Manager: Blink ticker started for '%s' (interval=%dms)",
                  id.c_str(), paramValue);
    }
    else if (actionMode == 2) {  // BREATHE
        // 同 BLINK：水位预检 + nothrow，OOM 时跳过本次动作而非重启
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < FastBee::MemoryBudget::ACTION_TICKER_MIN_DRAM) {
            LOG_WARNINGF("Peripheral Manager: skip breathe ticker for '%s': DRAM below %u",
                         id.c_str(), (unsigned)FastBee::MemoryBudget::ACTION_TICKER_MIN_DRAM);
            return;
        }
        ActionTickerData* data = new (std::nothrow) ActionTickerData();
        if (!data) {
            LOG_WARNINGF("Peripheral Manager: breathe ticker alloc failed for '%s'", id.c_str());
            return;
        }
        data->mgr = this;
        data->id = id;
        data->pin = config->getPrimaryPin();
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
        if (FastBee::MemoryBudget::dramFree() < 4096) {
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

    // === 动画灯效命令（非阻塞，每次调用推进一步）===
#if FASTBEE_ENABLE_NEOPIXEL
    if (cmd == "chase" || cmd == "theater_chase" || cmd == "strobe" ||
        cmd == "twinkle" || cmd == "fade" || cmd == "breathing" ||
        cmd == "color_wipe" || cmd == "fire") {
        uint16_t count = clampNeoPixelCount(config->params.neopixel.count);
        uint8_t brightness = config->params.neopixel.brightness;
        uint8_t& idx = neopixelRainbowIndex[id];
        Adafruit_NeoPixel* strip = getNeoPixelInstance(pin, count);
        if (!strip) return false;
        bool ok = true;

        if (cmd == "chase") {
            // 流水灯: 每次前进一灯，彩色循环
            NeoPixelRgb c = rainbowColorAt(idx);
            NeoPixelRgb bc = applyNeoPixelBrightness(c, brightness);
            uint32_t col = strip->Color(bc.r, bc.g, bc.b);
            for (uint16_t i = 0; i < count; i++) {
                strip->setPixelColor(i, (i == (idx % count)) ? col : 0);
            }
            strip->show();
            idx++;
        } else if (cmd == "theater_chase") {
            // 剧院追逐: 每3灯亮1灯，每次偏移；单灯珠时退化为彩色闪烁
            NeoPixelRgb c = rainbowColorAt(idx);
            NeoPixelRgb bc = applyNeoPixelBrightness(c, brightness);
            uint32_t col = strip->Color(bc.r, bc.g, bc.b);
            if (count <= 1) {
                // 单灯珠: 每2步交替亮/灭，产生明显闪烁效果
                strip->setPixelColor(0, (idx % 2 == 0) ? col : 0);
            } else {
                uint8_t offset = idx % 3;
                for (uint16_t i = 0; i < count; i++) {
                    strip->setPixelColor(i, ((i % 3) == offset) ? col : 0);
                }
            }
            strip->show();
            idx++;
        } else if (cmd == "strobe") {
            // 频闪: 奇偶交替全亮/全灭
            NeoPixelRgb c = rainbowColorAt(idx / 8);
            NeoPixelRgb bc = (idx % 2 == 0) ? applyNeoPixelBrightness(c, brightness) : NeoPixelRgb{0,0,0};
            uint32_t col = strip->Color(bc.r, bc.g, bc.b);
            for (uint16_t i = 0; i < count; i++) strip->setPixelColor(i, col);
            strip->show();
            idx++;
        } else if (cmd == "twinkle") {
            // 闪烁: 随机灯珠亮随机颜色
            for (uint16_t i = 0; i < count; i++) {
                if (rand() % 3 == 0) {
                    NeoPixelRgb rc = rainbowColorAt(rand() % 7);
                    NeoPixelRgb bc = applyNeoPixelBrightness(rc, brightness);
                    strip->setPixelColor(i, strip->Color(bc.r, bc.g, bc.b));
                } else {
                    strip->setPixelColor(i, 0);
                }
            }
            strip->show();
            idx++;
        } else if (cmd == "fade") {
            // 渐变: 亮度从0渐增到目标色
            NeoPixelRgb base = rainbowColorAt(idx / 32);
            uint8_t step = idx % 32;
            uint8_t fadeBri = (uint16_t)brightness * step / 31;
            NeoPixelRgb bc = applyNeoPixelBrightness(base, fadeBri);
            uint32_t col = strip->Color(bc.r, bc.g, bc.b);
            for (uint16_t i = 0; i < count; i++) strip->setPixelColor(i, col);
            strip->show();
            idx++;
        } else if (cmd == "breathing") {
            // 呼吸: 亮度来回循环
            NeoPixelRgb base = rainbowColorAt(idx / 32);
            uint8_t phase = idx % 64;
            uint8_t fadeBri = (phase < 32)
                ? (uint16_t)brightness * phase / 31
                : (uint16_t)brightness * (63 - phase) / 31;
            NeoPixelRgb bc = applyNeoPixelBrightness(base, fadeBri);
            uint32_t col = strip->Color(bc.r, bc.g, bc.b);
            for (uint16_t i = 0; i < count; i++) strip->setPixelColor(i, col);
            strip->show();
            idx++;
        } else if (cmd == "color_wipe") {
            // 色彩擦除: 逐灯填充，每次多亮一颗；单灯珠时退化为彩虹渐变
            if (count <= 1) {
                // 单灯珠: 每次调用切换颜色，产生彩虹轮播效果
                NeoPixelRgb c = rainbowColorAt(idx);
                NeoPixelRgb bc = applyNeoPixelBrightness(c, brightness);
                uint32_t col = strip->Color(bc.r, bc.g, bc.b);
                strip->setPixelColor(0, col);
            } else {
                NeoPixelRgb c = rainbowColorAt(idx / count);
                NeoPixelRgb bc = applyNeoPixelBrightness(c, brightness);
                uint32_t col = strip->Color(bc.r, bc.g, bc.b);
                uint16_t litCount = (idx % count) + 1;
                for (uint16_t i = 0; i < count; i++) {
                    strip->setPixelColor(i, (i < litCount) ? col : 0);
                }
            }
            strip->show();
            idx++;
        } else if (cmd == "fire") {
            // 火焰: 随机红/橙/黄色填充随机灯珠
            for (uint16_t i = 0; i < count; i++) {
                if (rand() % 3 != 0) {
                    uint8_t r = 160 + (rand() % 96);
                    uint8_t g = (rand() % 120);
                    NeoPixelRgb fc = applyNeoPixelBrightness({r, g, 0}, brightness);
                    strip->setPixelColor(i, strip->Color(fc.r, fc.g, fc.b));
                } else {
                    strip->setPixelColor(i, 0);
                }
            }
            strip->show();
            idx++;
        }

        if (runtimeStates.find(id) != runtimeStates.end()) {
            runtimeStates[id].status = ok ? PeripheralStatus::PERIPHERAL_RUNNING : PeripheralStatus::PERIPHERAL_ERROR;
            runtimeStates[id].lastActivity = millis();
        }
        LOG_INFOF("Peripheral Manager: NeoPixel '%s' animation=%s step=%d count=%d -> %s",
                  id.c_str(), action.c_str(), idx, count, ok ? "ok" : "fail");
        return ok;
    }
#endif

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
    GpioIsrEvent event;
    // 一次性处理队列中所有待处理的中断事件
    while (xQueueReceive(_isrQueue, &event, 0) == pdTRUE) {
        handleInterrupt(event.pin, event.rawLevel != 0);
    }
}

void PeripheralManager::handleInterrupt(uint8_t pin, bool rawLevel) {
    GPIOInterruptCallback callback = nullptr;
    String id;
    GPIOState state = GPIOState::STATE_UNDEFINED;
    {
        // 查找与去抖在锁内完成，防止并发 removePeripheral 导致迭代器/引用失效
        RecursiveMutexGuard lock(_mutex);
        id = getPinPeripheralId(pin);
        if (id.isEmpty()) return;

        auto it = peripherals.find(id);
        if (it == peripherals.end()) return;
        PeripheralConfig& config = it->second;

        // 按类型差异化去抖：高速边沿外设（RF/IR）窗口为 0，逐边沿派发；
        // GPIO 中断类在窗口内合并，避免重复的状态读取与回调触发
        uint16_t window = isGpioInterruptInputType(config.type)
                              ? effectiveDigitalInputDebounceMs(config.params.gpio.debounceMs)
                              : isrDebounceMsForType(config.type);
        if (window > 0 && pin < ISR_TRACKED_PIN_COUNT) {
            unsigned long now = millis();
            if (_hasIsrDispatch[pin] &&
                !shouldDispatchIsrEvent(now, _lastIsrDispatch[pin], window)) {
                return;
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
