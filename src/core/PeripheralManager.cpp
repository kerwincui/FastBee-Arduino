/**
 * @description: 外设接口管理器实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-03-03
 */

#include "core/PeripheralManager.h"
#include "core/AnalogInputCalibration.h"
#include "core/AsyncExecTypes.h"
#include "core/FeatureFlags.h"
#include "core/ChipConfig.h"
#include "core/MemoryBudget.h"
#include "core/FeatureBudget.h"
#include "core/RuntimeFeatureManager.h"
#include "core/SensorDriverCache.h"
#include "core/SensorConfigSupport.h"
#include "core/SensorConfigEpoch.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecManager.h"
#endif
#include "PeripheralManagerConfigHelpers.h"
#include "systems/LoggerSystem.h"
#include "peripherals/SDCardManager.h"
#include "screen/ScreenDevice.h"
#include "screen/ScreenRuntime.h"
#if FASTBEE_ENABLE_SCREEN_PANEL
#include "core/ScreenPanelManager.h"
#endif
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <algorithm>
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
#include "peripherals/IoExpanderProvider.h"
#include "core/I2CBusManager.h"
#include <memory>
#include <SPI.h>
#include <new>
#include <cstdlib>
#include <cstring>
#include <freertos/queue.h>

using namespace FastBee::PeripheralManagerConfigDetail;

// 静态成员定义：ISR 中断事件队列
QueueHandle_t PeripheralManager::_isrQueue = nullptr;
// 静态成员定义：ISR 队列溢出计数（队列满时丢弃的事件数）
volatile uint32_t PeripheralManager::_isrQueueOverruns = 0;

namespace {
constexpr size_t IO_EXPANDER_MAX_PROVIDERS = 4;
constexpr uint32_t IO_MAINTENANCE_BUDGET_MS = 8;
using IoProviderMap = std::map<String, std::unique_ptr<FastBee::IoExpanderProvider>>;
IoProviderMap& ioProviders() { static IoProviderMap providers; return providers; }
std::map<String, uint8_t>& ioPendingSyncs() {
    // Bits are channels whose previous ownership has not yet been retired on
    // hardware. Reusing one before that acknowledgement would inherit its latch.
    static std::map<String, uint8_t> pending;
    return pending;
}
bool ioChannelPending(const PeripheralConfig& config) {
    if (!config.isExpanderChannel() || config.params.gpio.channel >= 8) return false;
    const auto pending = ioPendingSyncs().find(config.params.gpio.providerId);
    return pending != ioPendingSyncs().end() &&
        (pending->second & static_cast<uint8_t>(1U << config.params.gpio.channel));
}
struct IoBlink { uint32_t last; uint16_t interval; };
std::map<String, IoBlink>& ioBlinks() { static std::map<String, IoBlink> blinks; return blinks; }
std::atomic<bool>& ioShutdownRequested() { static std::atomic<bool> requested{false}; return requested; }
std::map<String, std::unique_ptr<uint8_t>>& i2cOwners() {
    static std::map<String, std::unique_ptr<uint8_t>> owners;
    return owners;
}
const char* ioExpanderUnavailableReason() {
    if (ioShutdownRequested().load(std::memory_order_relaxed)) return "IO_SHUTDOWN_REQUESTED";
#if FASTBEE_ENABLE_IO_EXPANDER
    if (FastBee::FeatureBudgetRegistry::instance().isDenied("ioExpander")) {
        return "IO_EXPANDER_BUDGET_DENIED";
    }
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("ioExpander")) {
        return "IO_EXPANDER_DISABLED";
    }
    return nullptr;
#else
    return "IO_EXPANDER_NOT_COMPILED";
#endif
}
template <size_t N>
bool fixedIdEquals(const char (&storedId)[N], const String& id) {
    return id.length() < N && std::strncmp(storedId, id.c_str(), N) == 0;
}
bool isManagedI2cUser(const PeripheralConfig& config) {
    return config.type == PeripheralType::I2C || config.type == PeripheralType::IO_EXPANDER ||
           config.type == PeripheralType::LCD1602 ||
           (config.type == PeripheralType::LCD && config.params.lcd.interface == 2) ||
           (config.type == PeripheralType::SCREEN_DISPLAY &&
            (strcasecmp(config.params.display.touchBackend, "none") != 0 ||
             strcasecmp(config.params.display.controlBackend, "none") != 0)) ||
           (config.type == PeripheralType::SENSOR_GENERIC && config.params.sensor.driver[0]);
}
uint8_t managedI2cAddress(const PeripheralConfig& config) {
    if (config.type == PeripheralType::I2C) return config.params.i2c.address;
    if (config.type == PeripheralType::IO_EXPANDER) return config.params.ioExpander.address;
    if (config.type == PeripheralType::SENSOR_GENERIC) return config.params.sensor.address;
    if (config.type == PeripheralType::LCD1602) return config.params.lcd1602.address;
    if (config.type == PeripheralType::SCREEN_DISPLAY)
        return strcasecmp(config.params.display.touchBackend, "none") != 0
            ? 0x5D : config.params.display.controlAddress;
    return 0x3c;
}
bool managedI2cClaimsAddress(const PeripheralConfig& config, uint8_t address) {
    if (!address) return false;
    if (config.type == PeripheralType::SCREEN_DISPLAY) {
        const auto& display = config.params.display;
        if (strcasecmp(display.touchBackend, "none") != 0 &&
            (address == 0x5D || address == 0x14)) return true;
        return strcasecmp(display.backend, "rgb") == 0 &&
               strcasecmp(display.controlBackend, "none") != 0 &&
               address == display.controlAddress;
    }
    const uint8_t configuredAddress = managedI2cAddress(config);
    if (config.type == PeripheralType::LCD1602 && !configuredAddress)
        return address >= 0x20 && address <= 0x3f;
    return address == configuredAddress;
}
bool managedI2cAddressesOverlap(const PeripheralConfig& left, const PeripheralConfig& right) {
    // A display claims its control chip plus both GT911 probe addresses. This
    // check also runs for persisted configurations, independently of Web preflight.
    for (uint8_t address = 8; address <= 0x77; ++address)
        if (managedI2cClaimsAddress(left, address) && managedI2cClaimsAddress(right, address)) return true;
    return false;
}
const char* managedI2cReference(const PeripheralConfig& config) {
    if (config.type == PeripheralType::IO_EXPANDER) return config.params.ioExpander.busPeripheralId;
    if (config.type == PeripheralType::SENSOR_GENERIC) return config.params.sensor.busPeripheralId;
    return "";
}
bool managedI2cReferenceEquals(const PeripheralConfig& config, const String& id) {
    const char* storedId = managedI2cReference(config);
    const size_t capacity = sizeof(config.params.ioExpander.busPeripheralId);
    return id.length() < capacity && std::strncmp(storedId, id.c_str(), capacity) == 0;
}
constexpr uint8_t UART_PORT_UNASSIGNED = 255;
constexpr uint32_t SERVO_RELEASE_DELAY_MS = 500;

// Optional drivers are the first recovery targets because they may retain
// display/RMT buffers or bus-driver state. GPIO, ADC and UART remain available
// so the device can still be diagnosed and controlled while memory recovers.
bool isMemoryOptionalPeripheralType(PeripheralType type) {
    switch (type) {
        case PeripheralType::LCD:
        case PeripheralType::LCD1602:
        case PeripheralType::SCREEN_DISPLAY:
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

// Calibration handles are bounded and reused. All callers hold the manager
// mutex; API, execution and reporting convert the same acquired raw sample.
float convertAnalogRaw(const PeripheralConfig& config, uint16_t raw,
                       AnalogInputQuality* quality = nullptr, float* pinVoltage = nullptr) {
    if (pinVoltage) *pinVoltage = NAN;
    const auto& adc = config.params.adc;
    if (adc.calibratedVoltage && adc.conversionEnabled && adc.signalMode >= 1 && adc.signalMode <= 3) {
        float voltage = NAN;
        FastBee::calibratedAdcVoltage(config.getPrimaryPin(), adc.attenuation, adc.resolution, raw, voltage);
        if (pinVoltage) *pinVoltage = voltage;
        return convertAnalogInputRaw(config, raw, quality, voltage);
    }
    if (pinVoltage && adc.signalMode >= 1 && adc.signalMode <= 3) {
        const uint8_t bits = adc.resolution >= 9 && adc.resolution <= 12 ? adc.resolution : 12;
        *pinVoltage = raw * positiveAnalogConfigValue(adc.vRef, 3.3f) / ((1UL << bits) - 1);
    }
    return convertAnalogInputRaw(config, raw, quality);
}

void prepareAnalogInput(const PeripheralConfig& config) {
    const auto& adc = config.params.adc;
    if (!adc.calibratedVoltage) return;
    analogReadResolution(adc.resolution >= 9 && adc.resolution <= 12 ? adc.resolution : 12);
    analogSetPinAttenuation(config.getPrimaryPin(), static_cast<adc_attenuation_t>(adc.attenuation <= 3 ? adc.attenuation : 3));
}

struct NeoPixelRgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

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
    if (!_memoryRestoringDisplayId.isEmpty() && config.type == PeripheralType::SCREEN_DISPLAY) {
        errorMsg = "SCREEN_MEMORY_RESTORE_IN_PROGRESS";
        return false;
    }
    // 验证配置
    if (!validateConfig(config, errorMsg)) {
        LOG_ERRORF("Peripheral Manager: Invalid config - %s", errorMsg.c_str());
        return false;
    }
    if (!validateIoDependencies(config, config.id, errorMsg)) return false;

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
        for (uint8_t i = 0; i < config.configuredPinCount(); i++) {
            const uint8_t pin = config.configuredPinAt(i);
            if (pin != 255 && checkPinConflict(pin, config.id) &&
                !canShareI2cPin(config, pin, config.id)) {
                // 基于真实数据查找占用者（仅启用的外设算占用，与 checkPinConflict 语义一致）
                String owner;
                for (const auto& p : peripherals) {
                    if (p.first == config.id) continue;
                    if (!p.second.enabled) continue;
                    if (p.second.isModbusPeripheral()) continue;
                    if (p.second.usesPin(pin)) owner = p.first;
                    if (!owner.isEmpty()) break;
                }
                LOG_ERRORF("Peripheral Manager: Pin %d is already in use by '%s'",
                           pin, owner.isEmpty() ? "<stale-mapping>" : owner.c_str());
                // 如果实际找不到占用者，说明是缓存残留，自动修复后重试一次
                if (owner.isEmpty()) {
                    LOG_WARNING("Peripheral Manager: stale pin mapping detected, rebuilding cache");
                    rebuildPinMapping();
                    // 重建后再检一次，如果真的不占用，放行
                    if (!checkPinConflict(pin, config.id)) {
                        continue;
                    }
                }
                errorMsg = String("引脚 ") + pin + " 已被外设 '" +
                           (owner.isEmpty() ? "<未知>" : owner.c_str()) + "' 占用";
                return false;
            }
        }
    }

    // 添加外设
    invalidateSensorConfigurationLocked();
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
    if (!_memoryRestoringDisplayId.isEmpty() &&
        (id == _memoryRestoringDisplayId || config.type == PeripheralType::SCREEN_DISPLAY)) {
        errorMsg = "SCREEN_MEMORY_RESTORE_IN_PROGRESS";
        return false;
    }
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
    if (!validateIoDependencies(config, id, errorMsg)) return false;
    // Provider identity is a reference, not a mutable display label.
    for (const auto& item : peripherals) {
        if (item.second.isExpanderChannel() && fixedIdEquals(item.second.params.gpio.providerId, id) &&
            (config.id != id || config.type != PeripheralType::IO_EXPANDER)) {
            errorMsg = "IO_PROVIDER_REFERENCED: remove channel references first";
            return false;
        }
        if (managedI2cReferenceEquals(item.second, id) &&
            (config.id != id || config.type != PeripheralType::I2C ||
             !config.params.i2c.isMaster || config.params.i2c.address != 0 ||
             config.pinCount != oldIt->second.pinCount ||
             memcmp(config.pins, oldIt->second.pins, sizeof(config.pins)) != 0 ||
             config.params.i2c.frequency != oldIt->second.params.i2c.frequency)) {
            errorMsg = "I2C_PARENT_REFERENCED: remove bus references before changing bus identity, pins or frequency";
            return false;
        }
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
        for (uint8_t i = 0; i < config.configuredPinCount(); ++i) {
            uint8_t pin = config.configuredPinAt(i);
            if (pin != 255 && checkPinConflict(pin, id) && !canShareI2cPin(config, pin, id)) {
                errorMsg = String("引脚 ") + pin + " 已被其他已启用外设占用";
                return false;
            }
        }
    }

    const PeripheralConfig oldConfig = oldIt->second;
    invalidateSensorConfigurationLocked();
    if (targetId == id && config.type == oldConfig.type &&
        config.enabled == oldConfig.enabled && config.pinCount == oldConfig.pinCount &&
        memcmp(config.pins, oldConfig.pins, sizeof(config.pins)) == 0 &&
        memcmp(&config.params, &oldConfig.params, sizeof(config.params)) == 0) {
        // A display-name/description edit cannot reset any physical outputs.
        oldIt->second = config;
        return true;
    }
    PeripheralRuntimeState oldState;
    bool hadOldState = false;
    auto oldStateIt = runtimeStates.find(id);
    if (oldStateIt != runtimeStates.end()) {
        oldState = oldStateIt->second;
        hadOldState = true;
    }

    if (oldConfig.enabled) {
        const bool stopped = deinitHardware(id);
        if (!stopped && (oldConfig.type == PeripheralType::IO_EXPANDER ||
                         oldConfig.type == PeripheralType::SCREEN_DISPLAY)) {
            errorMsg = runtimeStates[id].lastError;
            if (errorMsg.isEmpty()) errorMsg = "HARDWARE_SHUTDOWN_FAILED";
            return false;
        }
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
    nextState.lastError = "";
    runtimeStates[targetId] = nextState;
    updatePinMapping(targetId, config);

    if (config.enabled && !initHardware(targetId)) {
        const String initError = runtimeStates[targetId].lastError;
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
        if (initError.startsWith("IO_CONFIG_PENDING")) {
            errorMsg = initError + "; " + errorMsg;
        }
        LOG_ERRORF("Peripheral Manager: Update '%s' rolled back after hardware init failure", id.c_str());
        return false;
    }

    LOG_INFOF("Peripheral Manager: Updated peripheral '%s' -> '%s'", id.c_str(), targetId.c_str());
    return true;
}

bool PeripheralManager::removePeripheral(const String& id) {
    RecursiveMutexGuard lock(_mutex);
    if (!_memoryRestoringDisplayId.isEmpty() && id == _memoryRestoringDisplayId) return false;
    if (!hasPeripheral(id)) {
        return false;
    }
    for (const auto& item : peripherals) {
        if ((item.second.isExpanderChannel() && fixedIdEquals(item.second.params.gpio.providerId, id)) ||
            managedI2cReferenceEquals(item.second, id)) return false;
    }

    // 先禁用并释放硬件
    if (!deinitHardware(id)) {
        const PeripheralType type = peripherals.at(id).type;
        if (type == PeripheralType::IO_EXPANDER ||
            type == PeripheralType::SCREEN_DISPLAY) return false;
    }

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

bool PeripheralManager::getPeripheralSnapshot(const String& id, PeripheralConfig& snapshot,
                                               uint32_t* sensorConfigVersion) const {
    if (sensorConfigVersion) *sensorConfigVersion = 0;
    RecursiveMutexGuard lock(_mutex, pdMS_TO_TICKS(50));
    if (!lock.isLocked()) return false;
    auto it = peripherals.find(id);
    if (it == peripherals.end()) return false;
    snapshot = it->second;
    if (sensorConfigVersion) *sensorConfigVersion = FastBee::SensorConfigEpoch::current();
    return true;
}

bool PeripheralManager::commitSensorRead(uint32_t sensorConfigVersion,
                                          const std::function<bool()>& commit) const {
    RecursiveMutexGuard lock(_mutex, pdMS_TO_TICKS(50));
    return lock.isLocked() && FastBee::SensorConfigEpoch::matches(sensorConfigVersion) && commit();
}

void PeripheralManager::invalidateSensorConfigurationLocked() {
    FastBee::SensorConfigEpoch::advance();
#if FASTBEE_ENABLE_PERIPH_EXEC
    // If this short cleanup cannot acquire the cache lock, epoch filtering
    // still hides all old records/events immediately; maintenance reclaims them.
    PeriphExecManager::getInstance().invalidateAllSensorReadCache();
#endif
}

bool PeripheralManager::releaseSensorCache(const String& id) {
    {
        RecursiveMutexGuard lock(_mutex, pdMS_TO_TICKS(50));
        if (!lock.isLocked()) return false;
        const auto it = peripherals.find(id);
        if (it == peripherals.end() || it->second.type != PeripheralType::SENSOR_GENERIC) return false;
        invalidateSensorConfigurationLocked();
    }
    FastBee::SensorDriverCache::instance().release(id);
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
    if (!_memoryRestoringDisplayId.isEmpty() && id == _memoryRestoringDisplayId) {
        lastEnableError = "SCREEN_MEMORY_RESTORE_IN_PROGRESS";
        return false;
    }
    auto config = getPeripheral(id);
    if (!config) return false;
    PeripheralConfig dependencyCandidate = *config;
    dependencyCandidate.enabled = true;
    if (!validateConfig(dependencyCandidate, lastEnableError)) return false;
    if (!validateIoDependencies(dependencyCandidate, id, lastEnableError)) return false;

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
        for (uint8_t i = 0; i < config->configuredPinCount(); i++) {
            const uint8_t pin = config->configuredPinAt(i);
            if (pin != 255 && checkPinConflict(pin, id) &&
                !canShareI2cPin(*config, pin, id)) {
                LOG_ERRORF("Peripheral Manager: Cannot enable '%s', pin %d is in use by another enabled peripheral",
                           id.c_str(), pin);
                lastEnableError = "Pin ";
                lastEnableError += String(pin);
                lastEnableError += " conflict with another peripheral";
                return false;
            }
        }
    }

    invalidateSensorConfigurationLocked();
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
    if (!_memoryRestoringDisplayId.isEmpty() && id == _memoryRestoringDisplayId) return false;
    auto config = getPeripheral(id);
    if (!config) return false;

    const bool previousEnabled = config->enabled;
    invalidateSensorConfigurationLocked();
    config->enabled = false;

    auto stateIt = runtimeStates.find(id);
    if (stateIt != runtimeStates.end()) {
        if (stateIt->second.memorySuspended && _memorySuspendedCount > 0) {
            _memorySuspendedCount--;
        }
        stateIt->second.memorySuspended = false;
    }

    const bool stopped = deinitHardware(id);
    if (!stopped) config->enabled = previousEnabled;
    return stopped;
}

bool PeripheralManager::applyMutationTransactional(
    const String& snapshotId, const String& resultId,
    const std::function<bool(String&)>& mutation, String& errorMsg) {
    RecursiveMutexGuard lock(_mutex);
    if (!_memoryRestoringDisplayId.isEmpty() &&
        (snapshotId == _memoryRestoringDisplayId || resultId == _memoryRestoringDisplayId)) {
        errorMsg = "SCREEN_MEMORY_RESTORE_IN_PROGRESS";
        return false;
    }

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
    invalidateSensorConfigurationLocked();

    auto unchanged = peripherals.find(snapshot.id);
    if (snapshot.existed && resultId == snapshot.id && unchanged != peripherals.end() &&
        unchanged->second.type == snapshot.config.type &&
        unchanged->second.enabled == snapshot.config.enabled &&
        unchanged->second.pinCount == snapshot.config.pinCount &&
        memcmp(unchanged->second.pins, snapshot.config.pins, sizeof(snapshot.config.pins)) == 0 &&
        memcmp(&unchanged->second.params, &snapshot.config.params, sizeof(snapshot.config.params)) == 0) {
        // A failed metadata save must preserve live output commands too.
        unchanged->second = snapshot.config;
        if (snapshot.hadRuntimeState) runtimeStates[snapshot.id] = snapshot.runtimeState;
        _memorySuspendedCount = snapshot.memorySuspendedCount;
        errorMsg = "";
        return true;
    }

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
            if (!isPeripheralTypeUserConfigurable(config.type)) {
                mutationError = config.type == PeripheralType::MODBUS_DEVICE
                    ? "EXTERNALLY_MANAGED_PERIPHERAL_TYPE: Modbus devices are managed by protocol.modbus"
                    : "UNSUPPORTED_PERIPHERAL_TYPE: current firmware does not implement this type";
                return false;
            }
            return addPeripheral(config, mutationError);
        }, errorMsg);
}

bool PeripheralManager::updatePeripheralAndSave(const String& id,
                                                const PeripheralConfig& config,
                                                String& errorMsg) {
    return applyMutationTransactional(id, config.id,
        [this, &id, &config](String& mutationError) {
            auto existing = peripherals.find(id);
            if (existing != peripherals.end() &&
                existing->second.type == PeripheralType::MODBUS_DEVICE) {
                mutationError = "EXTERNALLY_MANAGED_PERIPHERAL_TYPE: Modbus devices are managed by protocol.modbus";
                return false;
            }
            if (!isPeripheralTypeUserConfigurable(config.type)) {
                mutationError = config.type == PeripheralType::MODBUS_DEVICE
                    ? "EXTERNALLY_MANAGED_PERIPHERAL_TYPE: Modbus devices are managed by protocol.modbus"
                    : "UNSUPPORTED_PERIPHERAL_TYPE: current firmware does not implement this type";
                return false;
            }
            return updatePeripheral(id, config, mutationError);
        }, errorMsg);
}

bool PeripheralManager::removePeripheralAndSave(const String& id, String& errorMsg) {
    return applyMutationTransactional(id, id,
        [this, &id](String& mutationError) {
            auto existing = peripherals.find(id);
            if (existing != peripherals.end() &&
                existing->second.type == PeripheralType::MODBUS_DEVICE) {
                mutationError = "EXTERNALLY_MANAGED_PERIPHERAL_TYPE: Modbus devices are managed by protocol.modbus";
                return false;
            }
            if (removePeripheral(id)) return true;
            const auto state = runtimeStates.find(id);
            if (state != runtimeStates.end() && state->second.lastError.startsWith("IO_SHUTDOWN_UNCONFIRMED")) {
                mutationError = state->second.lastError;
                return false;
            }
            mutationError = existing != peripherals.end()
                ? "PERIPHERAL_REFERENCED: remove logical channels and bus references first"
                : String("外设 '") + id + "' 不存在";
            return false;
        }, errorMsg);
}

bool PeripheralManager::enablePeripheralAndSave(const String& id, String& errorMsg) {
    return applyMutationTransactional(id, id,
        [this, &id](String& mutationError) {
            auto existing = peripherals.find(id);
            if (existing != peripherals.end() &&
                existing->second.type == PeripheralType::MODBUS_DEVICE) {
                mutationError = "EXTERNALLY_MANAGED_PERIPHERAL_TYPE: Modbus devices are managed by protocol.modbus";
                return false;
            }
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
            auto existing = peripherals.find(id);
            if (existing != peripherals.end() &&
                existing->second.type == PeripheralType::MODBUS_DEVICE) {
                mutationError = "EXTERNALLY_MANAGED_PERIPHERAL_TYPE: Modbus devices are managed by protocol.modbus";
                return false;
            }
            if (disablePeripheral(id)) return true;
            const auto state = runtimeStates.find(id);
            mutationError = state != runtimeStates.end() &&
                state->second.lastError.startsWith("IO_SHUTDOWN_UNCONFIRMED")
                    ? state->second.lastError : String("无法禁用外设 '") + id + "'";
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
    if (!_memoryRestoringDisplayId.isEmpty() && id == _memoryRestoringDisplayId) return false;
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
    for (uint8_t i = 0; i < config->configuredPinCount(); i++) {
        const uint8_t configuredPin = config->configuredPinAt(i);
        if (configuredPin != 255 && isReservedPin(configuredPin)) {
            LOG_ERRORF("Peripheral Manager: '%s' rejected, GPIO%d is reserved on %s",
                       id.c_str(), configuredPin, CHIP_NAME);
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
    if (!_memoryRestoringDisplayId.isEmpty() && id == _memoryRestoringDisplayId) return false;
    auto config = getPeripheral(id);
    if (!config) return false;

    bool success = teardownHardware(*config);

    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].status = success ? PeripheralStatus::PERIPHERAL_DISABLED
                                          : PeripheralStatus::PERIPHERAL_ERROR;
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
    if (!_mutex || !_memoryRestoringDisplayId.isEmpty()) return 0;

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
    size_t restored = 0;
    String displayToStart;
    {
        RecursiveMutexGuard lock(_mutex);
        if (!_mutex || !_memoryRestoringDisplayId.isEmpty()) return 0;
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

            invalidateSensorConfigurationLocked();
            config.enabled = true;
            stateIt->second.status = PeripheralStatus::PERIPHERAL_ENABLED;
            if (initHardware(pair.first)) {
                if (config.type == PeripheralType::SCREEN_DISPLAY) {
                    // configure() alone does not create the UI task or activate
                    // display/touch backends. Keep recovery pending until start.
                    displayToStart = pair.first;
                    _memoryRestoringDisplayId = pair.first;
                    stateIt->second.lastError = "memory_pressure_restore_pending";
                } else {
                    stateIt->second.memorySuspended = false;
                    stateIt->second.lastError = "";
                    restored++;
                }
            } else {
                // Keep the runtime suspension if the driver still cannot be
                // initialized (for example a user-disabled compile-time feature).
                config.enabled = false;
                stateIt->second.status = PeripheralStatus::PERIPHERAL_DISABLED;
                stateIt->second.lastError = "memory_pressure_restore_pending";
            }
        }
    }

    if (!displayToStart.isEmpty()) {
        // The new screen task obtains PeripheralManager's mutex while loading
        // its first snapshot. Never wait for that task with _mutex held.
        String screenError;
        const bool started = FastBee::Screen::ScreenRuntime::instance().start(screenError);
        if (!started) FastBee::Screen::ScreenRuntime::instance().stop();
#if FASTBEE_ENABLE_SCREEN_PANEL
        ScreenPanelManager::instance().reportApplication(started, false, screenError);
#endif
        RecursiveMutexGuard lock(_mutex);
        auto config = peripherals.find(displayToStart);
        auto state = runtimeStates.find(displayToStart);
        if (_memoryRestoringDisplayId == displayToStart &&
            config != peripherals.end() && state != runtimeStates.end() &&
            config->second.type == PeripheralType::SCREEN_DISPLAY &&
            state->second.memorySuspended) {
            if (started) {
                state->second.memorySuspended = false;
                state->second.status = PeripheralStatus::PERIPHERAL_INITIALIZED;
                state->second.lastError = "";
                restored++;
            } else {
                config->second.enabled = false;
                state->second.status = PeripheralStatus::PERIPHERAL_DISABLED;
                state->second.lastError = "memory_pressure_restore_pending: " + screenError;
            }
        }
        _memoryRestoringDisplayId = "";
    }

    RecursiveMutexGuard lock(_mutex);
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
// 说明：注册 I2C 传感器与 RFID 为惰性初始化（轮询/使用时才 begin），
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

TwoWire* PeripheralManager::getManagedI2cWire(const String& id) {
    auto owner = i2cOwners().find(id);
    return owner == i2cOwners().end() ? nullptr :
        FastBee::I2CBusManager::instance().wire(owner->second.get());
}

bool PeripheralManager::resolveI2cConfig(const PeripheralConfig& config, uint8_t& sda,
                                        uint8_t& scl, uint32_t& frequency,
                                        String& errorMsg) const {
    RecursiveMutexGuard lock(_mutex);
    const char* reference = config.type == PeripheralType::IO_EXPANDER
        ? config.params.ioExpander.busPeripheralId
        : config.type == PeripheralType::SENSOR_GENERIC
            ? config.params.sensor.busPeripheralId : "";
    const PeripheralConfig* busConfig = &config;
    if (reference[0]) {
        const auto bus = peripherals.find(reference);
        const auto busState = bus == peripherals.end()
            ? runtimeStates.end() : runtimeStates.find(bus->first);
        const bool busConfigInvalid = busState != runtimeStates.end() &&
            busState->second.lastError.startsWith("CONFIG_INVALID:");
        if (bus == peripherals.end() || bus->second.type != PeripheralType::I2C ||
            !bus->second.enabled || !bus->second.params.i2c.isMaster ||
            bus->second.params.i2c.address != 0 || config.pinCount != 0 ||
            busConfigInvalid) {
            errorMsg = "I2C_REFERENCE_INVALID: enabled master bus without device address required";
            return false;
        }
        busConfig = &bus->second;
    }
    const bool displayTouch = busConfig->type == PeripheralType::SCREEN_DISPLAY;
    const bool touchBus = displayTouch && strcasecmp(busConfig->params.display.touchBackend, "none") != 0;
    const int configuredSda = displayTouch ? (touchBus ? busConfig->params.display.touchSda : busConfig->params.display.controlSda)
                                           : (busConfig->pinCount > 0 ? busConfig->pins[0] : -1);
    const int configuredScl = displayTouch ? (touchBus ? busConfig->params.display.touchScl : busConfig->params.display.controlScl)
                                           : (busConfig->pinCount > 1 ? busConfig->pins[1] : -1);
    if (configuredSda < 0 || configuredScl < 0 || configuredSda == configuredScl ||
        !isValidPin(static_cast<uint8_t>(configuredSda)) ||
        !isValidPin(static_cast<uint8_t>(configuredScl)) ||
        isReservedPin(static_cast<uint8_t>(configuredSda)) ||
        isReservedPin(static_cast<uint8_t>(configuredScl)) ||
        isInputOnlyPin(static_cast<uint8_t>(configuredSda)) ||
        isInputOnlyPin(static_cast<uint8_t>(configuredScl))) {
        errorMsg = "I2C_PINS_INVALID: valid output-capable SDA/SCL required";
        return false;
    }
    sda = static_cast<uint8_t>(configuredSda);
    scl = static_cast<uint8_t>(configuredScl);
    frequency = busConfig->type == PeripheralType::I2C ? busConfig->params.i2c.frequency
        : config.type == PeripheralType::SENSOR_GENERIC ? config.params.sensor.frequency
        : config.type == PeripheralType::SCREEN_DISPLAY ? config.params.display.i2cFrequency : 100000;
    if (frequency == 0) frequency = 100000;
    if (config.type == PeripheralType::IO_EXPANDER && frequency != 100000) {
        errorMsg = "I2C_FREQUENCY_INVALID: PCF8574 requires 100kHz";
        return false;
    }
    if (config.type == PeripheralType::SENSOR_GENERIC &&
        frequency != 100000 && frequency != 400000) {
        errorMsg = "SENSOR_FREQUENCY_INVALID: referenced bus must use 100000 or 400000 Hz";
        return false;
    }
    errorMsg = "";
    return true;
}

bool PeripheralManager::canShareI2cPin(const PeripheralConfig& candidate, uint8_t pin,
                                      const String& excludeId) const {
    if (!isManagedI2cUser(candidate)) return false;
    uint8_t candidateSda = 255, candidateScl = 255;
    uint32_t candidateFrequency = 0;
    String ignored;
    if (!resolveI2cConfig(candidate, candidateSda, candidateScl,
                          candidateFrequency, ignored) ||
        (pin != candidateSda && pin != candidateScl)) return false;
    for (const auto& item : peripherals) {
        const auto& other = item.second;
        if (item.first == excludeId || !other.enabled || !other.usesPin(pin)) continue;
        uint8_t otherSda = 255, otherScl = 255;
        uint32_t otherFrequency = 0;
        if (!isManagedI2cUser(other) ||
            !resolveI2cConfig(other, otherSda, otherScl, otherFrequency, ignored) ||
            otherSda != candidateSda || otherScl != candidateScl ||
            otherFrequency != candidateFrequency) return false;
        if (managedI2cAddressesOverlap(candidate, other)) return false;
    }
    return true;
}

bool PeripheralManager::validateIoDependencies(const PeripheralConfig& config,
                                               const String& excludeId,
                                               String& errorMsg) const {
    if (config.isExpanderChannel()) {
        if (ioChannelPending(config)) {
            errorMsg = "IO_CONFIG_PENDING: previous channel retirement has not completed; retry later";
            return false;
        }
        auto parent = peripherals.find(config.params.gpio.providerId);
        const auto parentState = parent == peripherals.end()
            ? runtimeStates.end() : runtimeStates.find(parent->first);
        const bool parentConfigInvalid = parentState != runtimeStates.end() &&
            parentState->second.lastError.startsWith("CONFIG_INVALID:");
        if (parent == peripherals.end() || parent->second.type != PeripheralType::IO_EXPANDER ||
            (config.enabled && !parent->second.enabled) || parentConfigInvalid) {
            errorMsg = "IO_PROVIDER_UNAVAILABLE: select an enabled IO expander";
            return false;
        }
        for (const auto& item : peripherals) {
            const auto& other = item.second;
            if (item.first != excludeId && other.isExpanderChannel() &&
                strcmp(other.params.gpio.providerId, config.params.gpio.providerId) == 0 &&
                other.params.gpio.channel == config.params.gpio.channel) {
                errorMsg = "IO_CHANNEL_CONFLICT: channel already has a logical owner";
                return false;
            }
        }
    }
    if (config.type == PeripheralType::IO_EXPANDER) {
        size_t count = 0;
        for (const auto& item : peripherals) {
            if (item.first != excludeId && item.second.type == PeripheralType::IO_EXPANDER) ++count;
        }
        if (count >= IO_EXPANDER_MAX_PROVIDERS) {
            errorMsg = "IO_PROVIDER_CAPACITY: maximum four expanders";
            return false;
        }
        if (config.params.ioExpander.busPeripheralId[0]) {
            auto bus = peripherals.find(config.params.ioExpander.busPeripheralId);
            if (bus == peripherals.end() || bus->second.type != PeripheralType::I2C ||
                !bus->second.params.i2c.isMaster ||
                (bus->second.params.i2c.frequency != 0 && bus->second.params.i2c.frequency != 100000) ||
                bus->second.params.i2c.address != 0 ||
                (config.enabled && !bus->second.enabled)) {
                errorMsg = "I2C_REFERENCE_INVALID: PCF8574 requires enabled 100kHz master bus";
                return false;
            }
        }
    }
    if (config.enabled && isManagedI2cUser(config)) {
        uint8_t sda, scl;
        uint32_t frequency;
        if (!resolveI2cConfig(config, sda, scl, frequency, errorMsg)) return false;
        for (const auto& item : peripherals) {
            const auto& other = item.second;
            if (item.first == excludeId || !other.enabled || !isManagedI2cUser(other)) continue;
            uint8_t otherSda, otherScl;
            uint32_t otherFrequency;
            String ignored;
            if (!resolveI2cConfig(other, otherSda, otherScl, otherFrequency, ignored)) continue;
            if (sda != otherSda || scl != otherScl || frequency != otherFrequency) {
                errorMsg = "I2C_BUS_CONFLICT: all clients must share the physical bus pins and frequency";
                return false;
            }
            if (managedI2cAddressesOverlap(config, other)) {
                errorMsg = "I2C_ADDRESS_CONFLICT: address is owned by another peripheral";
                return false;
            }
        }
    }
    return true;
}

bool PeripheralManager::setupIoExpander(const PeripheralConfig& config) {
    String error;
    const auto state = runtimeStates.find(config.id);
    if (state != runtimeStates.end() && state->second.lastError.startsWith("CONFIG_INVALID:")) return false;
    if (ioExpanderUnavailableReason() || !validateIoDependencies(config, config.id, error)) return false;
    // Initialization order does not depend on alphabetical peripheral IDs.
    uint8_t sda, scl;
    uint32_t frequency;
    if (!resolveI2cConfig(config, sda, scl, frequency, error)) return false;
    const auto existing = ioProviders().find(config.id);
    if (existing != ioProviders().end()) {
        if (existing->second->closing()) return false;
        return syncIoExpander(config.id);
    }
    FastBee::I2CBusLock busGuard(0);
    if (!busGuard.locked()) return false;
    std::unique_ptr<FastBee::IoExpanderProvider> provider(
        new (std::nothrow) FastBee::IoExpanderProvider(config.params.ioExpander.address));
    if (!provider || !provider->acquire(sda, scl, error)) return false;
    uint8_t mask = 0, inputMask = 0, startup = 0xff;
    for (const auto& item : peripherals) {
        const auto& child = item.second;
        const auto state = runtimeStates.find(item.first);
        if (state != runtimeStates.end() && state->second.lastError.startsWith("CONFIG_INVALID:")) continue;
        if (!child.enabled || !child.isExpanderChannel() ||
            !fixedIdEquals(child.params.gpio.providerId, config.id) || child.params.gpio.channel >= 8) continue;
        const uint8_t bit = 1U << child.params.gpio.channel;
        if (child.type == PeripheralType::GPIO_DIGITAL_INPUT) { inputMask |= bit; continue; }
        if (child.type != PeripheralType::GPIO_DIGITAL_OUTPUT) continue;
        mask |= bit;
        const bool high = digitalInputLogicalLevel(child.params.gpio.initialState == GPIOState::STATE_HIGH,
                                                  child.params.gpio.invertLogic);
        if (!high) startup &= ~bit;
    }
    const bool ready = provider->driver.begin(mask, startup, config.params.ioExpander.sampleInterval, millis(), inputMask);
    ioProviders()[config.id] = std::move(provider);
    return ready;
}

bool PeripheralManager::syncIoExpander(const String& providerId, const String& excludeChild) {
    if (ioExpanderUnavailableReason()) return false;
    auto parent = peripherals.find(providerId);
    if (parent == peripherals.end() || !parent->second.enabled ||
        parent->second.type != PeripheralType::IO_EXPANDER) return false;
    auto provider = ioProviders().find(providerId);
    if (provider == ioProviders().end()) {
        if (!setupIoExpander(parent->second)) return false;
        provider = ioProviders().find(providerId);
    }
    if (provider->second->closing()) return false;
    uint8_t mask = 0, inputMask = 0, startup = 0xff;
    for (const auto& item : peripherals) {
        const auto& child = item.second;
        const auto state = runtimeStates.find(item.first);
        if (state != runtimeStates.end() && state->second.lastError.startsWith("CONFIG_INVALID:")) continue;
        if (item.first == excludeChild || !child.enabled || !child.isExpanderChannel() ||
            !fixedIdEquals(child.params.gpio.providerId, providerId) || child.params.gpio.channel >= 8) continue;
        const uint8_t bit = 1U << child.params.gpio.channel;
        if (child.type == PeripheralType::GPIO_DIGITAL_INPUT) { inputMask |= bit; continue; }
        if (child.type != PeripheralType::GPIO_DIGITAL_OUTPUT) continue;
        mask |= bit;
        if (!digitalInputLogicalLevel(child.params.gpio.initialState == GPIOState::STATE_HIGH,
                                      child.params.gpio.invertLogic)) startup &= ~bit;
    }
    return provider->second->driver.configure(mask, startup, millis(), inputMask);
}

bool PeripheralManager::teardownIoExpander(const String& providerId) {
    const auto provider = ioProviders().find(providerId);
    if (provider == ioProviders().end()) {
        const auto state = runtimeStates.find(providerId);
        return state == runtimeStates.end() ||
            !state->second.lastError.startsWith("IO_SHUTDOWN_UNCONFIRMED");
    }
    uint8_t safeByte = 0xff;
    for (const auto& item : peripherals) {
        const auto& child = item.second;
        const auto state = runtimeStates.find(item.first);
        if (!child.enabled || !child.isExpanderChannel() ||
            child.type != PeripheralType::GPIO_DIGITAL_OUTPUT || child.params.gpio.channel >= 8 ||
            !fixedIdEquals(child.params.gpio.providerId, providerId) ||
            (state != runtimeStates.end() && state->second.lastError.startsWith("CONFIG_INVALID:"))) continue;
        if (!digitalInputLogicalLevel(child.params.gpio.initialState == GPIOState::STATE_HIGH,
                                      child.params.gpio.invertLogic)) {
            safeByte &= static_cast<uint8_t>(~(1U << child.params.gpio.channel));
        }
    }
    const bool wasClosing = provider->second->closing();
    const auto result = provider->second->shutdown(safeByte);
    const bool confirmed = result == FastBee::IoExpanderResult::Ok;
    const char* error = confirmed ? "IO_PROVIDER_UNAVAILABLE" :
        result == FastBee::IoExpanderResult::Busy
            ? "IO_SHUTDOWN_UNCONFIRMED: bus busy; physical state not confirmed"
            : "IO_SHUTDOWN_UNCONFIRMED: write failed; physical state not confirmed";
    auto parentState = runtimeStates.find(providerId);
    if (parentState != runtimeStates.end()) {
        parentState->second.status = confirmed ? PeripheralStatus::PERIPHERAL_DISABLED
                                               : PeripheralStatus::PERIPHERAL_ERROR;
        parentState->second.lastError = error;
    }
    for (const auto& item : peripherals) {
        if (!item.second.isExpanderChannel() ||
            !fixedIdEquals(item.second.params.gpio.providerId, providerId)) continue;
        stopActionTicker(item.first);
        const auto state = runtimeStates.find(item.first);
        if (state == runtimeStates.end()) continue;
        state->second.state.gpio.currentState = GPIOState::STATE_UNDEFINED;
        if (item.second.enabled) state->second.status = PeripheralStatus::PERIPHERAL_ERROR;
        if (!state->second.lastError.startsWith("CONFIG_INVALID:")) state->second.lastError = error;
    }
    if (!confirmed && (!wasClosing || result != FastBee::IoExpanderResult::Busy)) {
        LOG_WARNINGF("Peripheral Manager: '%s' %s", providerId.c_str(), error);
    }
    ioPendingSyncs().erase(providerId);
    // Busy must retain the object/lease: destroying it here would invoke the
    // default blocking release. Success/Error already released under the lock.
    if (result != FastBee::IoExpanderResult::Busy) ioProviders().erase(provider);
    return confirmed;
}

bool PeripheralManager::shutdownIoExpanders() {
    ioShutdownRequested().store(true, std::memory_order_relaxed);
    RecursiveMutexGuard lock(_mutex, pdMS_TO_TICKS(5));
    if (!lock.isLocked()) {
        LOG_WARNING("Peripheral Manager: IO_SHUTDOWN_UNCONFIRMED: manager busy; physical state not confirmed");
        return false;
    }
    ioBlinks().clear();
    bool confirmed = true;
    for (auto it = ioProviders().begin(); it != ioProviders().end();) {
        auto current = it++;
        if (!teardownIoExpander(current->first)) confirmed = false;
    }
    for (const auto& state : runtimeStates) {
        if (state.second.lastError.startsWith("IO_SHUTDOWN_UNCONFIRMED")) confirmed = false;
    }
    return confirmed;
}

uint32_t PeripheralManager::digitalInputGeneration(const PeripheralConfig& config) const {
    RecursiveMutexGuard lock(_mutex);
    if (!config.isExpanderChannel()) return 0;
    auto provider = ioProviders().find(config.params.gpio.providerId);
    return provider != ioProviders().end() ? provider->second->driver.generation() : 0;
}

void PeripheralManager::pollIoExpanders() {
    RecursiveMutexGuard lock(_mutex, pdMS_TO_TICKS(5));
    if (!lock.isLocked()) return;
    const uint32_t now = millis();
    const uint32_t cycleStarted = now;
    const char* unavailable = ioExpanderUnavailableReason();
    if (unavailable) {
        ioBlinks().clear();
        ioPendingSyncs().clear();
        for (auto it = ioProviders().begin(); it != ioProviders().end();) {
            if (static_cast<uint32_t>(millis() - cycleStarted) >= IO_MAINTENANCE_BUDGET_MS) break;
            auto current = it++;
            teardownIoExpander(current->first);
        }
        for (const auto& item : peripherals) {
            if (!item.second.enabled ||
                (item.second.type != PeripheralType::IO_EXPANDER && !item.second.isExpanderChannel())) continue;
            auto state = runtimeStates.find(item.first);
            if (state == runtimeStates.end() || state->second.lastError.startsWith("CONFIG_INVALID:")) continue;
            state->second.status = PeripheralStatus::PERIPHERAL_ERROR;
            if (!state->second.lastError.startsWith("IO_SHUTDOWN_UNCONFIRMED")) state->second.lastError = unavailable;
            if (item.second.isExpanderChannel()) state->second.state.gpio.currentState = GPIOState::STATE_UNDEFINED;
        }
        return;
    }
    for (auto it = ioProviders().begin(); it != ioProviders().end();) {
        if (static_cast<uint32_t>(millis() - cycleStarted) >= IO_MAINTENANCE_BUDGET_MS) break;
        auto current = it++;
        if (current->second->closing()) teardownIoExpander(current->first);
    }
    // Re-enabling a feature/bus restores eligible providers. Failed leases are
    // retried once per second; failed device transactions use the driver cadence.
    static uint32_t lastRetry = 0;
    if (static_cast<uint32_t>(now - lastRetry) >= 1000) {
        lastRetry = now;
        for (const auto& item : peripherals) {
            if (static_cast<uint32_t>(millis() - cycleStarted) >= IO_MAINTENANCE_BUDGET_MS) break;
            if (!item.second.enabled || item.second.type != PeripheralType::IO_EXPANDER ||
                ioProviders().find(item.first) != ioProviders().end()) continue;
            const auto rt = runtimeStates.find(item.first);
            if (rt != runtimeStates.end() && rt->second.lastError.startsWith("CONFIG_INVALID:")) continue;
            setupIoExpander(item.second);
        }
    }
    for (auto it = ioPendingSyncs().begin(); it != ioPendingSyncs().end();) {
        if (static_cast<uint32_t>(millis() - cycleStarted) >= IO_MAINTENANCE_BUDGET_MS) break;
        if (syncIoExpander(it->first)) {
            it = ioPendingSyncs().erase(it);
        } else {
            ++it;
        }
        if (static_cast<uint32_t>(millis() - cycleStarted) >= IO_MAINTENANCE_BUDGET_MS) break;
    }

    // Healthy providers normally all fit within the budget. If one transaction
    // consumes its timeout, resume from the next provider on the following tick.
    auto& providers = ioProviders();
    static size_t providerOffset = 0;
    const size_t providerCount = providers.size();
    size_t processed = 0;
    size_t startOffset = providerCount ? providerOffset % providerCount : 0;
    auto provider = providers.begin();
    for (size_t i = 0; i < startOffset && provider != providers.end(); ++i) ++provider;
    while (processed < providerCount && provider != providers.end() &&
           static_cast<uint32_t>(millis() - cycleStarted) < IO_MAINTENANCE_BUDGET_MS) {
        if (provider->second->closing()) {
            ++processed;
            providerOffset = (startOffset + processed) % providerCount;
            if (++provider == providers.end() && processed < providerCount) provider = providers.begin();
            continue;
        }
        auto& item = *provider;
        item.second->driver.poll(millis());
        const bool ok = item.second->driver.healthy(millis());
        const bool configPending = ioPendingSyncs().find(item.first) != ioPendingSyncs().end();
        auto runtime = runtimeStates.find(item.first);
        if (runtime != runtimeStates.end()) {
            runtime->second.status = ok && !configPending
                ? PeripheralStatus::PERIPHERAL_RUNNING : PeripheralStatus::PERIPHERAL_ERROR;
            runtime->second.lastError = configPending ? "IO_CONFIG_PENDING"
                                                      : (ok ? "" : "IO_SAMPLE_UNAVAILABLE");
            if (ok) runtime->second.lastActivity = item.second->driver.sampledAt();
        }
        for (const auto& child : peripherals) {
            if (!child.second.isExpanderChannel() ||
                !fixedIdEquals(child.second.params.gpio.providerId, item.first)) continue;
            auto state = runtimeStates.find(child.first);
            if (state == runtimeStates.end()) continue;
            if (!child.second.enabled) continue;
            if (state->second.lastError.startsWith("CONFIG_INVALID:")) continue;
            state->second.status = ok ? PeripheralStatus::PERIPHERAL_RUNNING : PeripheralStatus::PERIPHERAL_ERROR;
            state->second.lastError = ok ? "" : "IO_PROVIDER_UNAVAILABLE";
            if (!ok) {
                state->second.state.gpio.currentState = GPIOState::STATE_UNDEFINED;
                // Contention and sample age are retryable. Only a real transport
                // failure terminates an otherwise valid blink action.
                if (item.second->driver.failures() != 0) ioBlinks().erase(child.first);
            } else {
                state->second.state.gpio.currentState = readGpioStateLocked(child.second, &child.first, nullptr);
                state->second.lastActivity = item.second->driver.sampledAt();
            }
        }

        ++processed;
        providerOffset = (startOffset + processed) % providerCount;
        ++provider;
        if (provider == providers.end() && processed < providerCount) provider = providers.begin();
        if (static_cast<uint32_t>(millis() - cycleStarted) >= IO_MAINTENANCE_BUDGET_MS) break;
    }
    for (auto it = ioBlinks().begin(); it != ioBlinks().end();) {
        if (static_cast<uint32_t>(millis() - cycleStarted) >= IO_MAINTENANCE_BUDGET_MS) break;
        const auto child = peripherals.find(it->first);
        const auto state = runtimeStates.find(it->first);
        if (child == peripherals.end() || !child->second.enabled ||
            !child->second.isExpanderChannel() ||
            child->second.type != PeripheralType::GPIO_DIGITAL_OUTPUT ||
            child->second.params.gpio.channel >= 8 || ioChannelPending(child->second) ||
            (state != runtimeStates.end() && state->second.lastError.startsWith("CONFIG_INVALID:"))) {
            it = ioBlinks().erase(it);
            continue;
        }
        const auto parent = peripherals.find(child->second.params.gpio.providerId);
        const auto provider = ioProviders().find(child->second.params.gpio.providerId);
        const auto parentState = runtimeStates.find(child->second.params.gpio.providerId);
        if (parent == peripherals.end() || !parent->second.enabled ||
            parent->second.type != PeripheralType::IO_EXPANDER || provider == ioProviders().end() ||
            (parentState != runtimeStates.end() && parentState->second.lastError.startsWith("CONFIG_INVALID:")) ||
            provider->second->driver.failures() != 0) {
            it = ioBlinks().erase(it);
            continue;
        }
        if (static_cast<uint32_t>(millis() - it->second.last) < it->second.interval) { ++it; continue; }
        if (!togglePin(it->first)) {
            if (provider->second->driver.failures() != 0) it = ioBlinks().erase(it);
            else ++it; // Keep the deadline due so Busy/stale retries next tick.
            continue;
        }
        it->second.last = millis();
        ++it;
    }
}

GPIOState PeripheralManager::readGpioStateLocked(const PeripheralConfig& config,
                                                  const String* peripheralId,
                                                  const char* peripheralIdCStr) {
    if (!config.isGPIOPeripheral()) {
        return GPIOState::STATE_UNDEFINED;
    }
    if (config.isExpanderChannel()) {
        if (!config.enabled || ioExpanderUnavailableReason()) return GPIOState::STATE_UNDEFINED;
        auto state = runtimeStates.find(config.id);
        if (state != runtimeStates.end() && state->second.lastError.startsWith("CONFIG_INVALID:")) {
            return GPIOState::STATE_UNDEFINED;
        }
        auto provider = ioProviders().find(config.params.gpio.providerId);
        bool high = false;
        if (provider == ioProviders().end() ||
            !provider->second->driver.readChannel(config.params.gpio.channel,
                config.type == PeripheralType::GPIO_DIGITAL_OUTPUT, millis(), high)) {
            return GPIOState::STATE_UNDEFINED;
        }
        return digitalInputLogicalLevel(high, config.params.gpio.invertLogic)
            ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
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
    if (state != GPIOState::STATE_HIGH && state != GPIOState::STATE_LOW) return false;
    if (config.isExpanderChannel()) {
        if (!config.enabled || config.type != PeripheralType::GPIO_DIGITAL_OUTPUT ||
            ioExpanderUnavailableReason()) return false;
        auto runtime = runtimeStates.find(config.id);
        if (runtime != runtimeStates.end() &&
            runtime->second.lastError.startsWith("CONFIG_INVALID:")) return false;
        auto provider = ioProviders().find(config.params.gpio.providerId);
        if (provider == ioProviders().end()) return false;
        const bool physicalHigh = digitalInputLogicalLevel(
            state == GPIOState::STATE_HIGH, config.params.gpio.invertLogic);
        const bool ok = provider->second->driver.writeChannel(
            config.params.gpio.channel, physicalHigh, millis());
        if (runtime != runtimeStates.end()) {
            runtime->second.lastError = ok ? "" : "IO_WRITE_FAILED";
            runtime->second.state.gpio.currentState = ok ? state : GPIOState::STATE_UNDEFINED;
            if (ok) runtime->second.lastActivity = millis();
        }
        if (!ok) {
            for (const auto& item : peripherals) {
                if (!item.second.enabled || !item.second.isExpanderChannel() ||
                    std::strncmp(item.second.params.gpio.providerId, config.params.gpio.providerId,
                                 sizeof(config.params.gpio.providerId)) != 0) continue;
                auto sibling = runtimeStates.find(item.first);
                if (sibling == runtimeStates.end()) continue;
                sibling->second.status = PeripheralStatus::PERIPHERAL_ERROR;
                sibling->second.state.gpio.currentState = GPIOState::STATE_UNDEFINED;
                sibling->second.lastError = "IO_WRITE_FAILED";
            }
        }
        return ok;
    }

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
    if (!config || !config->enabled || config->isExpanderChannel()) return false;

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
        prepareAnalogInput(*config);
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
        prepareAnalogInput(*config);
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
    prepareAnalogInput(*config);
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
    prepareAnalogInput(*config);
    return convertAnalogRaw(*config, adcFilterRead(pin, samples, filterMode));
}

float PeripheralManager::convertAnalogRawValue(const String& peripheralId, uint16_t rawValue,
                                               AnalogInputQuality* quality, float* pinVoltage) {
    RecursiveMutexGuard lock(_mutex);
    const auto* config = getPeripheralCStrLocked(peripheralId.c_str());
    if (!config || !isAdcInputType(config->type)) {
        if (quality) *quality = AnalogInputQuality::INVALID_CONFIG;
        return NAN;
    }
    return convertAnalogRaw(*config, rawValue, quality, pinVoltage);
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

    if (config.type == PeripheralType::SCREEN_DISPLAY && config.enabled) {
        for (const auto& pair : peripherals) {
            if (pair.first != config.id && pair.second.type == PeripheralType::SCREEN_DISPLAY &&
                pair.second.enabled) {
                errorMsg = "SINGLETON_RESOURCE_BUSY: only one graphical display can be active";
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

    if (isGpioDigitalInputType(config.type)) {
        const auto& gpio = config.params.gpio;
        if (!memchr(gpio.pairedInputId, '\0', sizeof(gpio.pairedInputId)) ||
            config.id == gpio.pairedInputId ||
            (gpio.pairDebounceMs != 0 && (gpio.pairDebounceMs < 20 || gpio.pairDebounceMs > 5000))) {
            errorMsg = "DI_PAIR_INVALID: choose another DI ID (max 40 chars), stable time 20..5000ms";
            return false;
        }
        // The peer can be loaded later. Missing/disabled/unreadable peers are
        // detected as a fault by the runtime monitor rather than ignored.
    }
    if (config.isGPIOPeripheral() && config.params.gpio.ioSource > 1) {
        errorMsg = "IO_SOURCE_INVALID";
        return false;
    }
    if (config.isExpanderChannel()) {
        if (config.pinCount != 0 || config.params.gpio.providerId[0] == '\0' ||
            !memchr(config.params.gpio.providerId, '\0', sizeof(config.params.gpio.providerId)) ||
            config.params.gpio.channel >= 8 ||
            (config.type != PeripheralType::GPIO_DIGITAL_INPUT &&
             config.type != PeripheralType::GPIO_DIGITAL_OUTPUT) ||
            config.params.gpio.passive || config.params.gpio.inputMode != 0 ||
            (config.params.gpio.initialState != GPIOState::STATE_LOW &&
             config.params.gpio.initialState != GPIOState::STATE_HIGH) ||
            config.params.gpio.debounceMs > 5000 || config.params.gpio.reportMode > 2) {
            errorMsg = "IO_CHANNEL_INVALID: PCF8574 supports plain digital input/output, no MCU pins or configurable pulls";
            return false;
        }
        return true;
    }
    if (config.type == PeripheralType::IO_EXPANDER) {
        if (!memchr(config.params.ioExpander.driver, '\0', sizeof(config.params.ioExpander.driver)) ||
            strcasecmp(config.params.ioExpander.driver, "PCF8574") != 0 ||
            config.params.ioExpander.address < 0x20 || config.params.ioExpander.address > 0x27 ||
            (config.params.ioExpander.sampleInterval != 0 &&
             (config.params.ioExpander.sampleInterval < 20 || config.params.ioExpander.sampleInterval > 1000))) {
            errorMsg = "IO_PROVIDER_INVALID: PCF8574 address 0x20..0x27, interval 20..1000ms required";
            return false;
        }
        if (config.params.ioExpander.busPeripheralId[0]) {
            if (!memchr(config.params.ioExpander.busPeripheralId, '\0',
                        sizeof(config.params.ioExpander.busPeripheralId)) || config.pinCount != 0) {
                errorMsg = "I2C_REFERENCE_INVALID: choose bus reference or two pins";
                return false;
            }
            return true;
        }
        if (config.pinCount != 2 || config.pins[0] == config.pins[1]) {
            errorMsg = "I2C_PINS_INVALID: SDA/SCL required";
            return false;
        }
    }

    // Generic sensors may represent an external/logical data source and are
    // therefore valid without a local GPIO. When a pin is supplied, still run
    // the normal chip/reserved-pin checks below.
    if (config.type == PeripheralType::SENSOR_GENERIC) {
        if (!FastBee::validateSensorConfig(config, errorMsg)) return false;
        if (config.pinCount == 0) return true;
    }

    if (config.type == PeripheralType::SCREEN_DISPLAY) {
        const auto& display = config.params.display;
        const bool rgb = memchr(display.backend, '\0', sizeof(display.backend)) &&
                         strcasecmp(display.backend, "rgb") == 0;
        if (config.pinCount != 0) {
            errorMsg = "DISPLAY_PINS_INVALID: use params.pins named display pins";
            return false;
        }
        if (!memchr(display.backend, '\0', sizeof(display.backend)) ||
            !memchr(display.driver, '\0', sizeof(display.driver)) ||
            !memchr(display.touchBackend, '\0', sizeof(display.touchBackend)) ||
            (!rgb && strcasecmp(display.backend, "spi_tft") != 0) ||
            (rgb ? strcasecmp(display.driver, "st7701") != 0 :
                (strcasecmp(display.driver, "st7789") != 0 &&
                 strcasecmp(display.driver, "ili9341") != 0)) ||
            (strcasecmp(display.touchBackend, "none") != 0 &&
             strcasecmp(display.touchBackend, "gt911") != 0)) {
            errorMsg = "DISPLAY_BACKEND_UNSUPPORTED: supported spi_tft with st7789/ili9341 and none/gt911 touch";
            return false;
        }
        if (display.width < 128 || display.width > (rgb ? 480 : 320) ||
            display.height < 128 || display.height > (rgb ? 480 : 320) ||
            display.rotation > 3 || display.colorDepth != 16 ||
            display.bufferLines < 1 || display.bufferLines > 64 ||
            display.brightness > 100 ||
            display.touchBuzzerDurationMs < 20 || display.touchBuzzerDurationMs > 150 ||
            (display.backlight >= 0 && display.backlightChannel >= CHIP_MAX_PWM_CH) ||
            display.spiFrequency < 1000000UL || display.spiFrequency > 80000000UL ||
            (display.i2cFrequency != 100000UL && display.i2cFrequency != 400000UL)) {
            errorMsg = "DISPLAY_PARAMS_INVALID: dimensions 128..320, RGB565, 1..64 lines and valid bus rates required";
            return false;
        }
        const uint16_t shortSide = std::min(display.width, display.height);
        const uint16_t longSide = std::max(display.width, display.height);
        if (!rgb && (shortSide > 240 || longSide > 320)) {
            errorMsg = "DISPLAY_DIMENSIONS_UNSUPPORTED: ST7789/ILI9341 controller window is at most 240x320";
            return false;
        }
        const bool touchEnabled = strcasecmp(display.touchBackend, "none") != 0;
        const bool controlEnabled = rgb && memchr(display.controlBackend, '\0', sizeof(display.controlBackend)) &&
                                    strcasecmp(display.controlBackend, "none") != 0;
        if (rgb) {
#if !CHIP_IS_ESP32S3 && !defined(FASTBEE_NATIVE_TEST)
            errorMsg = "DISPLAY_RGB_UNAVAILABLE: RGB output requires ESP32-S3";
            return false;
#endif
            if (!memchr(display.rgbInitProfile, '\0', sizeof(display.rgbInitProfile)) ||
                strcmp(display.rgbInitProfile, "st7701_type1") != 0 ||
                display.width != 480 || display.height != 480 ||
                display.rgbFrequency < 1000000UL || display.rgbFrequency > 16000000UL ||
                !display.hsyncPulseWidth || !display.vsyncPulseWidth ||
                display.hsyncFrontPorch > 255 || display.hsyncPulseWidth > 255 || display.hsyncBackPorch > 255 ||
                display.vsyncFrontPorch > 255 || display.vsyncPulseWidth > 255 || display.vsyncBackPorch > 255 ||
                !memchr(display.controlBackend, '\0', sizeof(display.controlBackend)) ||
                (controlEnabled && strcasecmp(display.controlBackend, "tca9554") != 0 &&
                                   strcasecmp(display.controlBackend, "ch32v003") != 0)) {
                errorMsg = "DISPLAY_RGB_PARAMS_INVALID: supported profile, 480x480 and bounded timings required";
                return false;
            }
            for (uint8_t index = 10; index < 30; ++index) {
                if (display.pinAt(index) < 0) {
                    errorMsg = "DISPLAY_RGB_PINS_REQUIRED: sixteen data pins and four timing pins required";
                    return false;
                }
            }
            if (controlEnabled && (display.controlSda < 0 || display.controlScl < 0 ||
                display.controlAddress < 8 || display.controlAddress > 0x77 ||
                (touchEnabled && (display.controlAddress == 0x5D || display.controlAddress == 0x14)) ||
                (touchEnabled && (display.controlSda != display.touchSda ||
                                  display.controlScl != display.touchScl)) ||
                !display.controlMasksDisjoint() ||
                (display.controlBuzzerMask != 0 &&
                 strcasecmp(display.controlBackend, "ch32v003") != 0) ||
                (strcasecmp(display.controlBackend, "ch32v003") == 0 && display.controlBacklightMask != 0))) {
                errorMsg = "DISPLAY_CONTROL_INVALID: valid shared I2C bus and disjoint control masks required";
                return false;
            }
            if (!controlEnabled && (display.controlSda >= 0 || display.controlScl >= 0)) {
                errorMsg = "DISPLAY_CONTROL_INVALID: unused control bus must not claim pins";
                return false;
            }
        }
        if (display.mosi < 0 || display.sclk < 0 || display.cs < 0 || (!rgb && display.dc < 0) ||
            (touchEnabled && (display.touchSda < 0 || display.touchScl < 0)) ||
            (!touchEnabled && (display.touchSda >= 0 || display.touchScl >= 0 ||
                               display.touchInt >= 0 || display.touchRst >= 0))) {
            errorMsg = "DISPLAY_PINS_INVALID: SPI pins and enabled touch SDA/SCL are required";
            return false;
        }
        for (uint8_t i = 0; i < 32; ++i) {
            if (display.pinAt(i) < 0) continue;
            const uint8_t pin = static_cast<uint8_t>(display.pinAt(i));
            if (!isValidPin(pin) || isReservedPin(pin) ||
                (i != 8 && isInputOnlyPin(pin))) {
                errorMsg = String("DISPLAY_PIN_UNUSABLE: GPIO") + String(pin);
                return false;
            }
            for (uint8_t j = 0; j < i; ++j) {
                if (display.pinAt(j) == display.pinAt(i)) {
                    errorMsg = String("DISPLAY_PIN_DUPLICATED: GPIO") + String(pin);
                    return false;
                }
            }
            if (config.enabled && checkPinConflict(pin, config.id) &&
                !((i == 6 || i == 7 || i >= 30) && canShareI2cPin(config, pin, config.id))) {
                errorMsg = String("DISPLAY_PIN_CONFLICT: GPIO") + String(pin);
                return false;
            }
        }
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
    if (config.type == PeripheralType::IO_EXPANDER || config.isExpanderChannel()) {
        const char* unavailable = ioExpanderUnavailableReason();
        if (unavailable) {
            auto rt = runtimeStates.find(config.id);
            if (rt != runtimeStates.end()) rt->second.lastError = unavailable;
            return false;
        }
    }
    if (config.type == PeripheralType::IO_EXPANDER) return setupIoExpander(config);
    if (config.isExpanderChannel()) {
        // Updates can enqueue retirement after their initial validation. Check
        // again before synchronizing the new logical owner into the port mask.
        if (ioChannelPending(config)) {
            auto rt = runtimeStates.find(config.id);
            if (rt != runtimeStates.end()) rt->second.lastError = "IO_CONFIG_PENDING";
            return false;
        }
        return syncIoExpander(config.params.gpio.providerId);
    }
    // 加固：统一校验外设声明使用的所有引脚是否在当前芯片有效范围内
    // （对 Modbus 等不占 GPIO 的外设，pinCount 通常为 0，校验自然通过）
    for (uint8_t i = 0; i < config.configuredPinCount(); i++) {
        uint8_t p = config.configuredPinAt(i);
        if (p == 255) continue;  // 未使用位
        if (!isValidPin(p)) {
            LOG_ERRORF("Peripheral Manager: '%s' pin %u invalid on %s (max GPIO=%d) - skipping hardware init",
                       config.id.c_str(), (unsigned)p, CHIP_NAME, (int)CHIP_MAX_GPIO);
            return false;
        }
    }

    if (config.type == PeripheralType::SCREEN_DISPLAY) {
        String error;
        if (!FastBee::Screen::ScreenDevice::instance().configure(config, error)) {
            auto rt = runtimeStates.find(config.id);
            if (rt != runtimeStates.end()) rt->second.lastError = error;
            LOG_ERRORF("Peripheral Manager: Display '%s' configuration failed: %s",
                       config.id.c_str(), error.c_str());
            return false;
        }
        return true;
    }

    // Touch needs threshold/calibration semantics that are not implemented yet.
    // Reject legacy persisted entries explicitly instead of treating them as
    // ordinary digital inputs and reporting a false successful initialization.
    if (config.type == PeripheralType::GPIO_TOUCH) {
        LOG_ERRORF("Peripheral Manager: Touch peripheral '%s' is unavailable in this firmware",
                   config.id.c_str());
        return false;
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
        if (!config.params.i2c.isMaster) return false;
        auto owner = i2cOwners().find(config.id);
        const bool newOwner = owner == i2cOwners().end();
        if (newOwner) {
            std::unique_ptr<uint8_t> token(new (std::nothrow) uint8_t(0));
            if (!token) return false;
            owner = i2cOwners().emplace(config.id, std::move(token)).first;
        }
        String error;
        if (!FastBee::I2CBusManager::instance().acquire(owner->second.get(), sda, scl, freq, error)) {
            runtimeStates[config.id].lastError = error;
            // A failed reinitialization must not discard an existing lease.
            if (newOwner) i2cOwners().erase(owner);
            return false;
        }
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
        if (isReservedPin(cs) || isInputOnlyPin(cs)) {
            LOG_ERRORF("Peripheral Manager: SPI '%s' rejects unusable CS GPIO%d on %s",
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
            // SensorDriverCache initializes the driver on first read. Each
            // driver leases the shared bus; PM must never restart Wire here.
            if (config.pinCount < 2 && !config.params.sensor.busPeripheralId[0]) return false;
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
    invalidateSensorConfigurationLocked();
    stopActionTicker(config.id);
    if (config.isExpanderChannel()) {
        // Retire only this channel. Other output bits retain their command shadow.
        auto provider = ioProviders().find(config.params.gpio.providerId);
        if (provider != ioProviders().end()) {
            const String providerId(config.params.gpio.providerId);
            if (!syncIoExpander(providerId, config.id)) {
                if (config.params.gpio.channel < 8) {
                    ioPendingSyncs()[providerId] |= static_cast<uint8_t>(1U << config.params.gpio.channel);
                }
                auto parentState = runtimeStates.find(providerId);
                if (parentState != runtimeStates.end()) {
                    parentState->second.status = PeripheralStatus::PERIPHERAL_ERROR;
                    parentState->second.lastError = "IO_CONFIG_PENDING";
                }
            }
        }
        return true;
    }
    if (config.type == PeripheralType::IO_EXPANDER) {
        return teardownIoExpander(config.id);
    }
    if (config.type == PeripheralType::SCREEN_DISPLAY) {
        FastBee::Screen::ScreenDevice& device =
            FastBee::Screen::ScreenDevice::instance();
        String stopError;
        if (!FastBee::Screen::ScreenRuntime::instance().stop(&stopError)) {
            auto state = runtimeStates.find(config.id);
            if (state != runtimeStates.end()) state->second.lastError = stopError;
            LOG_ERRORF("Peripheral Manager: Display '%s' shutdown failed: %s",
                       config.id.c_str(), stopError.c_str());
            return false;
        }
        const bool unconfigured = !device.configured() ||
                                  device.peripheralId() != config.id ||
                                  device.unconfigure(config.id);
        if (!unconfigured) {
            auto state = runtimeStates.find(config.id);
            if (state != runtimeStates.end()) {
                state->second.lastError = "SCREEN_DEVICE_STILL_ACTIVE";
            }
            return false;
        }
#if FASTBEE_ENABLE_SCREEN_PANEL
        ScreenPanelManager::instance().reportApplication(
            false, true, "SCREEN_DISPLAY_RESTART_REQUIRED");
#endif
        return true;
    }
    if (config.isGPIOPeripheral()) {
        uint8_t pin = config.getPrimaryPin();
        if (pin != 255) {
            detachInterrupt(pin);
        }
    }

    if (config.type == PeripheralType::SDIO) {
        String errorMsg;
        const bool unmounted = SDCardManager::getInstance().unmount(
            config.id, &errorMsg, true);
        if (!unmounted) {
            LOG_WARNINGF("Peripheral Manager: SD '%s' unmount failed: %s",
                         config.id.c_str(), errorMsg.c_str());
            return false;
        }
        if (config.pinCount >= 4) {
            const uint8_t cs = config.pins[3];
            if (cs != 255 && !isReservedPin(cs) && !isInputOnlyPin(cs)) {
                pinMode(cs, INPUT);
            }
        }
        return true;
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
        // The driver deliberately retains the shared SPI bus. Only release its
        // dedicated select/reset pins after the device has stopped.
        for (uint8_t i = 3; i < config.pinCount && i < 5; ++i) {
            const uint8_t pin = config.pins[i];
            if (pin != 255 && !isReservedPin(pin) && !isInputOnlyPin(pin)) {
                pinMode(pin, INPUT);
            }
        }
        return true;
    }

    if (config.type == PeripheralType::SENSOR_GENERIC) {
        FastBee::SensorDriverCache::instance().release(config.id);
        return true;
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
        // Referenced providers become unavailable with their configured parent.
        // Independent devices keep their own leases on the physical bus.
        bool outputsConfirmed = true;
        for (const auto& pair : peripherals) {
            const PeripheralConfig& other = pair.second;
            if (other.type == PeripheralType::IO_EXPANDER &&
                fixedIdEquals(other.params.ioExpander.busPeripheralId, config.id)) {
                if (!teardownHardware(other)) outputsConfirmed = false;
                auto rt = runtimeStates.find(pair.first);
                if (rt != runtimeStates.end()) {
                    rt->second.status = PeripheralStatus::PERIPHERAL_ERROR;
                    if (!rt->second.lastError.startsWith("IO_SHUTDOWN_UNCONFIRMED")) {
                        rt->second.lastError = "I2C_PARENT_UNAVAILABLE";
                    }
                }
            } else if (other.type == PeripheralType::SENSOR_GENERIC &&
                       fixedIdEquals(other.params.sensor.busPeripheralId, config.id)) {
                FastBee::SensorDriverCache::instance().release(pair.first);
            }
        }
        if (!outputsConfirmed) {
            const auto state = runtimeStates.find(config.id);
            if (state != runtimeStates.end()) {
                state->second.lastError = "IO_SHUTDOWN_UNCONFIRMED: referenced expander physical state not confirmed";
            }
            return false;
        }
        auto owner = i2cOwners().find(config.id);
        if (owner != i2cOwners().end()) {
            auto& bus = FastBee::I2CBusManager::instance();
            if (!bus.release(owner->second.get(), 100)) {
                bus.deferRelease(owner->second.get());
            }
            i2cOwners().erase(owner);
        }
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
        case PeripheralType::GPIO_TOUCH:
            return false;

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
    ioBlinks().erase(id);
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
    if (config->isExpanderChannel()) {
        if (actionMode != 1 || paramValue < 200 || !config->enabled ||
            config->type != PeripheralType::GPIO_DIGITAL_OUTPUT ||
            readPin(id) == GPIOState::STATE_UNDEFINED) return;
        ioBlinks()[id] = {millis(), paramValue};
        return; // periodic task performs I2C; never a system Ticker callback
    }

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
