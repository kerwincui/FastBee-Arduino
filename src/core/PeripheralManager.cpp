/**
 * @description: 外设接口管理器实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-03-03
 */

#include "core/PeripheralManager.h"
#include "core/Pca9685ConfigSupport.h"
#include "core/H1ParameterContract.h"
#include "core/AsyncExecTypes.h"
#include "core/FeatureFlags.h"
#include "core/ChipConfig.h"
#include "core/MemoryBudget.h"
#include "core/FeatureBudget.h"
#include "core/FeatureInstanceRegistry.h"
#include "core/RuntimeFeatureManager.h"
#include "core/SensorDriverCache.h"
#include "core/SensorConfigSupport.h"
#include "core/SensorConfigEpoch.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecManager.h"
#endif
#include "PeripheralManagerConfigHelpers.h"
#include "PeripheralManagerUartSupport.h"
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
#include "peripherals/NeoPixelTransport.h"
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
#include "peripherals/DigitalIoProviderFactory.h"
#include "core/RuleOutputIdentity.h"
#include "core/I2CBusManager.h"
#include "core/SpiBusTeardownPolicy.h"
#include "core/SpiBusInitializer.h"
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
using IoProviderMap = std::map<String, std::unique_ptr<FastBee::DigitalIoProvider>>;
IoProviderMap& ioProviders() { static IoProviderMap providers; return providers; }
std::map<String, uint16_t>& ioPendingSyncs() {
    // Bits are channels whose previous ownership has not yet been retired on
    // hardware. Reusing one before that acknowledgement would inherit its latch.
    static std::map<String, uint16_t> pending;
    return pending;
}
bool isMcp23017(const PeripheralConfig& config) {
    return config.type == PeripheralType::IO_EXPANDER &&
        memchr(config.params.ioExpander.driver, '\0', sizeof(config.params.ioExpander.driver)) &&
        strcasecmp(config.params.ioExpander.driver, "MCP23017") == 0;
}
bool ioResetReferenceEquals(const PeripheralConfig& config, const String& id) {
    return isMcp23017(config) && id.length() < sizeof(config.params.ioExpander.resetPeripheralId) &&
        std::strncmp(config.params.ioExpander.resetPeripheralId, id.c_str(),
                     sizeof(config.params.ioExpander.resetPeripheralId)) == 0;
}
bool ioChannelPending(const PeripheralConfig& config) {
    if (!config.isExpanderChannel() || config.params.gpio.channel >= 16) return false;
    const auto pending = ioPendingSyncs().find(config.params.gpio.providerId);
    return pending != ioPendingSyncs().end() &&
        (pending->second & static_cast<uint16_t>(1U << config.params.gpio.channel));
}
struct IoBlink {
    uint32_t last = 0;
    uint16_t interval = 0;
    FastBee::UartEventOrigin uartOrigin = {};
    FastBee::OrdinaryControlContext context = {};
    FastBee::OrdinaryControl::Permit permit = {};
    const FastBee::DigitalIoProvider* provider = nullptr;
    uint8_t channel = 255;
};
std::map<String, IoBlink>& ioBlinks() { static std::map<String, IoBlink> blinks; return blinks; }
std::atomic<bool>& ioShutdownRequested() { static std::atomic<bool> requested{false}; return requested; }
std::atomic<SemaphoreHandle_t>& ioObservationMutex() {
    static std::atomic<SemaphoreHandle_t> mutex{nullptr};
    return mutex;
}
FastBee::FeatureInstanceSnapshot observeIoExpanderInstances(const void* context) {
    using namespace FastBee;
    if (!context) return {};
    const auto mutex = static_cast<const std::atomic<SemaphoreHandle_t>*>(context)->load(std::memory_order_acquire);
    if (!mutex) return {};
    // Observe the already-created manager lock, including initialization retry.
    // Do not allocate a lock, call feature admission, poll I2C, or wait for I/O.
    RecursiveMutexGuard guard(mutex, 0);
    if (!guard.isLocked()) return {FeatureInstanceState::Busy, 0};
    if (ioProviders().size() > IO_EXPANDER_MAX_PROVIDERS) return {};
    uint16_t initialized = 0;
    for (const auto& item : ioProviders()) {
        if (!item.second) return {};
        if (item.second->closing()) return {FeatureInstanceState::CleanupPending, 0};
        if (item.second->initialized()) ++initialized;
    }
    const auto pwm = pca9685InstanceSnapshotLocked();
    if (!pwm.countKnown()) return pwm;
    initialized += pwm.initializedCount;
    return {initialized ? FeatureInstanceState::Initialized
                        : FeatureInstanceState::NotInitialized, initialized};
}
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
           config.type == PeripheralType::PCA9685_PWM ||
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
    if (config.type == PeripheralType::PCA9685_PWM) return config.params.pca9685.address;
    if (config.type == PeripheralType::SENSOR_GENERIC) return config.params.sensor.address;
    if (config.type == PeripheralType::LCD1602) return config.params.lcd1602.address;
    if (config.type == PeripheralType::SCREEN_DISPLAY)
        return strcasecmp(config.params.display.touchBackend, "none") != 0
            ? 0x5D : config.params.display.controlAddress;
    return 0x3c;
}
bool managedI2cClaimsAddress(const PeripheralConfig& config, uint8_t address) {
    if (!address) return false;
    if (config.type == PeripheralType::PCA9685_PWM && address == 0x70) return true;
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
    if (config.type == PeripheralType::PCA9685_PWM) return config.params.pca9685.busPeripheralId;
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
        case PeripheralType::PCA9685_PWM:
            return true;
        default:
            return false;
    }
}

struct NeoPixelRgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};


using FastBee::PeripheralUart::serialForPort;
using FastBee::PeripheralUart::boundedUartAvailable;





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
// Publish lifetime transitions from the actual allocation owner. A query must
// never dereference this replaceable SDK pointer or initiate RMT/LED traffic.
static std::atomic<FastBee::FeatureInstanceState> g_neopixelObservation{FastBee::FeatureInstanceState::Stopped};
static std::atomic<int> g_neopixelRetainedPin{-1};
// PM mutex protects the identity and result ledger. It survives config-map
// rollback; passive queries never reconstruct an owner from current config.
static FastBee::ResourceConsumerIdentity g_neopixelConsumer{};
static bool g_neopixelWriteAttempted = false, g_neopixelReleaseAttempted = false;
static bool g_neopixelBackendRetained = false;
static FastBeeNeoPixelResult g_neopixelLastWrite = FASTBEE_NEO_OK, g_neopixelLastRelease = FASTBEE_NEO_OK;

void clearNeoPixelResourceLedger() {
    g_neopixelConsumer = FastBee::ResourceConsumerIdentity{};
    g_neopixelWriteAttempted = g_neopixelReleaseAttempted = false;
    g_neopixelBackendRetained = false;
    g_neopixelLastWrite = g_neopixelLastRelease = FASTBEE_NEO_OK;
}

FastBee::FeatureInstanceSnapshot observeNeoPixelInstance(const void*) {
    return FastBee::FeatureInstanceSnapshot::lifecycle(g_neopixelObservation.load(std::memory_order_acquire));
}

bool neoPixelRequiresRestart(FastBeeNeoPixelResult result) {
    return result != FASTBEE_NEO_OK && result != FASTBEE_NEO_BUSY && result != FASTBEE_NEO_NO_MEMORY;
}

void retainNeoPixelInstanceForRestart() {
    g_neopixelRetainedPin.store(g_neopixelPin, std::memory_order_release);
    g_neopixelObservation.store(FastBee::FeatureInstanceState::CleanupPending, std::memory_order_release);
}

bool showNeoPixelChecked(Adafruit_NeoPixel* strip) {
    if (!strip || strip != g_neopixelInstance ||
        g_neopixelObservation.load(std::memory_order_acquire) == FastBee::FeatureInstanceState::CleanupPending)
        return false;
    g_neopixelObservation.store(FastBee::FeatureInstanceState::Busy, std::memory_order_release);
    const auto result = fastbeeNeoPixelWrite(g_neopixelPin, strip->getPixels(),
        static_cast<uint32_t>(g_neopixelCount) * 3U, true);
    g_neopixelWriteAttempted = true;
    g_neopixelLastWrite = result;
    // This ABI query is a single acquire atomic load (esp-idf5.c), with no SDK
    // calls. Publish its result here; resource snapshots only copy our ledger.
    g_neopixelBackendRetained = fastbeeNeoPixelPinRetained(g_neopixelPin);
    g_neopixelObservation.store(neoPixelRequiresRestart(result) ? FastBee::FeatureInstanceState::CleanupPending
        : FastBee::FeatureInstanceState::Allocated, std::memory_order_release);
    if (result != FASTBEE_NEO_OK)
        LOGGER.warningf("[NeoPixel] Transport failed (%u); restartRequired=%u", (unsigned)result,
                        neoPixelRequiresRestart(result) ? 1U : 0U);
    return result == FASTBEE_NEO_OK;
}

bool releaseNeoPixelInstance(uint8_t pin) {
    if (!g_neopixelInstance || g_neopixelPin != pin) return !fastbeeNeoPixelProjectPinRetained(pin);
    // Clear only the actual existing strip; teardown must not create a driver.
    if (g_neopixelObservation.load(std::memory_order_acquire) == FastBee::FeatureInstanceState::CleanupPending)
        return false;
    for (uint16_t i = 0; i < g_neopixelCount; ++i) g_neopixelInstance->setPixelColor(i, 0);
    if (!showNeoPixelChecked(g_neopixelInstance)) {
        // Even a pre-transmit OOM/busy failure leaves a live project object.
        // Do not let a config rollback make its pin available to another owner
        // and later issue a stale "off" frame during replacement/retirement.
        retainNeoPixelInstanceForRestart();
        return false;
    }
    g_neopixelObservation.store(FastBee::FeatureInstanceState::Retiring, std::memory_order_release);
    const auto result = fastbeeNeoPixelRelease(pin);
    g_neopixelReleaseAttempted = true;
    g_neopixelLastRelease = result;
    g_neopixelBackendRetained = fastbeeNeoPixelPinRetained(pin);
    if (result != FASTBEE_NEO_OK) {
        retainNeoPixelInstanceForRestart();
        return false;
    }
    // Adafruit's destructor touches GPIO and issues zero-length espShow: call
    // it only after proven release. On failure retain object, symbols and pin.
    delete g_neopixelInstance;
    g_neopixelInstance = nullptr;
    g_neopixelPin = 255;
    g_neopixelCount = 0;
    clearNeoPixelResourceLedger();
    g_neopixelObservation.store(FastBee::FeatureInstanceState::Stopped, std::memory_order_release);
    return true;
}

Adafruit_NeoPixel* getNeoPixelInstance(uint8_t pin, uint16_t count, const char* ownerId) {
    FastBee::ResourceConsumerIdentity consumer;
    if (!ownerId || !*ownerId || !consumer.assign(FastBee::ResourceConsumerRole::Peripheral, ownerId))
        return nullptr;
    static const FastBee::FeatureInstanceObserver observer{"neopixel", observeNeoPixelInstance, nullptr};
    FastBee::FeatureInstanceRegistry::instance().registerObserver(&observer);
    if (!count || !FastBee::RuntimeFeatureManager::instance().shouldEnable("neopixel") ||
        FastBee::FeatureBudgetRegistry::instance().isDenied("neopixel") ||
        g_neopixelObservation.load(std::memory_order_acquire) == FastBee::FeatureInstanceState::CleanupPending)
        return nullptr;
    // Gate an existing instance too; allocation is never RMT health evidence.
    if (g_neopixelInstance && g_neopixelPin == pin && g_neopixelCount == count) {
        return g_neopixelConsumer.equals(consumer) ? g_neopixelInstance : nullptr;
    }

    if (g_neopixelInstance && !releaseNeoPixelInstance(g_neopixelPin)) return nullptr;

    // RGB bytes + 24 four-byte RMT symbols per LED, plus fixed safety margin.
    size_t neededBytes = static_cast<size_t>(count) * (3U + 24U * 4U) + 512U;
    size_t neededKB = (neededBytes + 1023) / 1024;
    uint32_t free8 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (free8 < neededKB * 1024) {
        LOGGER.warningf("[NeoPixel] Insufficient DRAM: need %uKB, free %luKB",
                        (unsigned)neededKB, (unsigned long)(free8 / 1024));
        return nullptr;
    }

    // 创建新实例
    g_neopixelObservation.store(FastBee::FeatureInstanceState::Busy, std::memory_order_release);
    g_neopixelInstance = new (std::nothrow) Adafruit_NeoPixel(count, pin, NEO_GRB + NEO_KHZ800);
    if (!g_neopixelInstance) {
        g_neopixelObservation.store(FastBee::FeatureInstanceState::Stopped, std::memory_order_release);
        return nullptr;
    }
    g_neopixelPin = pin;
    g_neopixelCount = count;
    clearNeoPixelResourceLedger();
    g_neopixelConsumer = consumer;
    if (!g_neopixelInstance->getPixels() || g_neopixelInstance->numPixels() != count ||
        !g_neopixelInstance->begin()) {
        g_neopixelObservation.store(FastBee::FeatureInstanceState::Retiring, std::memory_order_release);
        delete g_neopixelInstance; // No RMT write has been attempted.
        g_neopixelInstance = nullptr;
        g_neopixelPin = 255;
        g_neopixelCount = 0;
        clearNeoPixelResourceLedger();
        g_neopixelObservation.store(FastBee::FeatureInstanceState::Stopped, std::memory_order_release);
        return nullptr;
    }
    if (!showNeoPixelChecked(g_neopixelInstance)) return nullptr;
    return g_neopixelInstance;
}
#endif

bool writeNeoPixelSolid(uint8_t pin, uint16_t count, NeoPixelRgb color, const char* ownerId) {
#if FASTBEE_ENABLE_NEOPIXEL
    count = clampNeoPixelCount(count);

    Adafruit_NeoPixel* strip = getNeoPixelInstance(pin, count, ownerId);
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
    return showNeoPixelChecked(strip);
#else
    (void)pin;
    (void)count;
    (void)color;
    (void)ownerId;
    return false;
#endif
}
}

// 单例实现
#if FASTBEE_ENABLE_NEOPIXEL
bool fastbeeNeoPixelProjectPinRetained(uint8_t pin) {
    return g_neopixelRetainedPin.load(std::memory_order_acquire) == pin || fastbeeNeoPixelPinRetained(pin);
}
#endif

void PeripheralManager::snapshotNeoPixelResources(FastBee::NeoPixelResourceSnapshot& result) const {
    result = FastBee::NeoPixelResourceSnapshot{};
#if FASTBEE_ENABLE_NEOPIXEL
    result.supported = true;
    result.managerInitialized = _mutex != nullptr;
    if (!_mutex) { result.status = FastBee::ResourceObservation::Busy; return; }
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) { result.status = FastBee::ResourceObservation::Busy; return; }
    result.objectAllocated = g_neopixelInstance != nullptr;
    result.consumer = g_neopixelConsumer;
    result.pin = g_neopixelPin;
    result.ledCount = g_neopixelCount;
    result.lifecycle = g_neopixelObservation.load(std::memory_order_acquire);
    result.projectPinRetained = g_neopixelRetainedPin.load(std::memory_order_acquire) == g_neopixelPin;
    result.backendPinRetained = g_neopixelBackendRetained;
    result.writeAttempted = g_neopixelWriteAttempted;
    result.lastWriteResult = g_neopixelLastWrite;
    result.releaseAttempted = g_neopixelReleaseAttempted;
    result.lastReleaseResult = g_neopixelLastRelease;
#endif
}

PeripheralManager& PeripheralManager::getInstance() {
    static PeripheralManager instance;
    return instance;
}

// 初始化
bool PeripheralManager::initialize() {
    LOG_INFO("Peripheral Manager: Initializing...");

    // 创建递归互斥量（支持同一任务嵌套加锁，如 togglePin → readPin → writePin）
    _mutex = xSemaphoreCreateRecursiveMutex();
    ioObservationMutex().store(_mutex, std::memory_order_release);
    if (_mutex) {
        static const FastBee::FeatureInstanceObserver observer{
            "ioExpander", observeIoExpanderInstances, &ioObservationMutex()};
        FastBee::FeatureInstanceRegistry::instance().registerObserver(&observer);
    }

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


bool PeripheralManager::addPeripheral(const PeripheralConfig& config) {
    String errorMsg;
    return addPeripheral(config, errorMsg);
}

bool PeripheralManager::addPeripheral(const PeripheralConfig& config, String& errorMsg) {
    // 容器线程安全：Web handler(async_tcp)/worker pool/loopTask 并发增删改查
    RecursiveMutexGuard lock(_mutex);
    if (isMcp23017(config) && config.enabled) {
        errorMsg = "IO_GROUP_REQUIRED: save disabled provider and channels, then enable the group";
        return false;
    }
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
    if (config.isExpanderChannel() && config.enabled) {
        const auto parent = peripherals.find(config.params.gpio.providerId);
        if (parent != peripherals.end() && isMcp23017(parent->second)) {
            errorMsg = "IO_GROUP_REQUIRED: save disabled channels before group activation";
            return false;
        }
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
            const bool stopped = teardownHardware(config);
            if (!stopped) {
                runtimeStates[config.id].status = PeripheralStatus::PERIPHERAL_ERROR;
                if (runtimeStates[config.id].lastError.isEmpty())
                    runtimeStates[config.id].lastError = "HARDWARE_CLEANUP_PENDING";
                errorMsg = runtimeStates[config.id].lastError;
                return false; // Retain the failed configuration and pin owner.
            }
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
    if (pca9685Owns(id)) { errorMsg = "PCA_STOP_REQUIRED"; return false; }
    if (controlOutputIsClaimed(id)) { errorMsg = "CONTROL_OUTPUT_OWNED"; return false; }
    if (ordinaryDigitalClaimedLocked(id) || ordinaryDigitalClaimedLocked(config.id)) {
        errorMsg = "CONTROL_ORDINARY_STOP_REQUIRED"; return false;
    }
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

    // Live MCP ownership changes must not enter the generic stop/restart
    // rollback path, which can reset the chip or replay startup after failure.
    bool mcpLive = isMcp23017(oldIt->second) &&
        (oldIt->second.enabled || ioProviders().find(id) != ioProviders().end());
    if (oldIt->second.isExpanderChannel()) {
        const auto parent = peripherals.find(oldIt->second.params.gpio.providerId);
        mcpLive = parent != peripherals.end() && isMcp23017(parent->second) &&
            (parent->second.enabled || ioProviders().find(parent->first) != ioProviders().end());
    }
    if (config.isExpanderChannel()) {
        const auto parent = peripherals.find(config.params.gpio.providerId);
        mcpLive = mcpLive || (parent != peripherals.end() && isMcp23017(parent->second) &&
            (parent->second.enabled || ioProviders().count(parent->first)));
    }
    if ((mcpLive || isMcp23017(config)) &&
        (config.id != id || config.type != oldIt->second.type ||
         config.enabled != oldIt->second.enabled || config.pinCount != oldIt->second.pinCount ||
         memcmp(config.pins, oldIt->second.pins, sizeof(config.pins)) != 0 ||
         memcmp(&config.params, &oldIt->second.params, sizeof(config.params)) != 0) &&
        (mcpLive || config.enabled)) {
        errorMsg = "IO_GROUP_REQUIRED: disable the MCP group before changing hardware configuration";
        return false;
    }

    if (!validateConfig(config, errorMsg)) {
        LOG_ERRORF("Peripheral Manager: Invalid update for '%s' - %s", id.c_str(), errorMsg.c_str());
        return false;
    }
    if (!validateIoDependencies(config, id, errorMsg)) return false;
    // Provider identity is a reference, not a mutable display label.
    for (const auto& item : peripherals) {
        if (item.second.type == PeripheralType::PCA9685_PWM &&
            fixedIdEquals(item.second.params.pca9685.oePeripheralId, id) &&
            (config.id != id || config.type != oldIt->second.type ||
             config.pinCount != oldIt->second.pinCount ||
             memcmp(config.pins, oldIt->second.pins, sizeof(config.pins)) != 0 ||
             memcmp(&config.params, &oldIt->second.params, sizeof(config.params)) != 0)) {
            errorMsg = "PCA_OE_REFERENCED"; return false;
        }
        if (ioResetReferenceEquals(item.second, id) &&
            (config.id != id || config.type != PeripheralType::GPIO_DIGITAL_OUTPUT ||
             config.isExpanderChannel() || config.pinCount != oldIt->second.pinCount ||
             memcmp(config.pins, oldIt->second.pins, sizeof(config.pins)) != 0 ||
             memcmp(&config.params, &oldIt->second.params, sizeof(config.params)) != 0)) {
            errorMsg = "RESET_REFERENCED: remove expander references before changing the control pin";
            return false;
        }
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
    const size_t previousMemorySuspendedCount = _memorySuspendedCount;
    bool hadOldState = false;
    auto oldStateIt = runtimeStates.find(id);
    if (oldStateIt != runtimeStates.end()) {
        oldState = oldStateIt->second;
        hadOldState = true;
    }

    if (oldConfig.enabled || (hadOldState && oldState.memoryCleanupPending)) {
        const bool stopped = deinitHardware(id);
        if (!stopped) {
            errorMsg = runtimeStates[id].lastError;
            if (errorMsg.isEmpty()) errorMsg = "HARDWARE_SHUTDOWN_FAILED";
            return false;
        }
        oldState.memoryCleanupPending = false; // The old driver's cleanup is now confirmed.
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
    // A hardware edit replaces the old suspended intent. Keep the count with
    // the actual retained owner, including failure of the new owner's cleanup.
    if (nextState.memorySuspended && _memorySuspendedCount > 0) --_memorySuspendedCount;
    nextState.memorySuspended = false;
    nextState.memoryCleanupPending = false;
    nextState.lastError = "";
    runtimeStates[targetId] = nextState;
    updatePinMapping(targetId, config);

    if (config.enabled && !initHardware(targetId)) {
        const String initError = runtimeStates[targetId].lastError;
        const bool stopped = teardownHardware(config);
        if (!stopped) {
            runtimeStates[targetId].status = PeripheralStatus::PERIPHERAL_ERROR;
            if (runtimeStates[targetId].lastError.isEmpty())
                runtimeStates[targetId].lastError = "HARDWARE_CLEANUP_PENDING";
            errorMsg = runtimeStates[targetId].lastError;
            return false; // Do not restore another owner over a retained pin.
        }
        peripherals.erase(targetId);
        runtimeStates.erase(targetId);
        rebuildPinMapping();

        _memorySuspendedCount = previousMemorySuspendedCount;
        peripherals[id] = oldConfig;
        if (hadOldState) {
            runtimeStates[id] = oldState;
        } else {
            PeripheralRuntimeState restored;
            restored.id = id;
            runtimeStates[id] = restored;
        }
        updatePinMapping(id, oldConfig);
        const bool rollbackRestored = !oldConfig.enabled ||
            (hadOldState && oldState.memorySuspended) || initHardware(id);
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
    if (controlOutputIsClaimed(id)) return false;
    if (!_memoryRestoringDisplayId.isEmpty() && id == _memoryRestoringDisplayId) return false;
    if (!hasPeripheral(id)) {
        return false;
    }
    for (const auto& item : peripherals) {
        if ((item.second.isExpanderChannel() && fixedIdEquals(item.second.params.gpio.providerId, id)) ||
            (item.second.type == PeripheralType::PCA9685_PWM && fixedIdEquals(item.second.params.pca9685.oePeripheralId, id)) ||
            managedI2cReferenceEquals(item.second, id) || ioResetReferenceEquals(item.second, id)) return false;
    }
    const auto& removing = peripherals.at(id);
    if (isMcp23017(removing) && removing.enabled) return false;
    if (removing.isExpanderChannel()) {
        const auto parent = peripherals.find(removing.params.gpio.providerId);
        if (parent != peripherals.end() && isMcp23017(parent->second) &&
            (parent->second.enabled || ioProviders().count(parent->first))) return false;
    }

    // 先禁用并释放硬件
    if (!deinitHardware(id)) return false;

    const auto removedState = runtimeStates.find(id);
    if (removedState != runtimeStates.end() && removedState->second.memorySuspended &&
        _memorySuspendedCount > 0) --_memorySuspendedCount;

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
    if (_configurationReviewVersion) {
        _configurationReviewVersion = _configurationReviewVersion == UINT64_MAX
            ? 0 : _configurationReviewVersion + 1;
    }
    FastBee::SensorConfigEpoch::advance();
#if FASTBEE_ENABLE_PERIPH_EXEC
    // If this short cleanup cannot acquire the cache lock, epoch filtering
    // still hides all old records/events immediately; maintenance reclaims them.
    PeriphExecManager::getInstance().invalidateAllSensorReadCache();
#endif
}

uint64_t PeripheralManager::getConfigurationReviewVersion() const {
    RecursiveMutexGuard lock(_mutex, pdMS_TO_TICKS(50));
    return lock.isLocked() ? _configurationReviewVersion : 0;
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
    if (controlOutputIsClaimed(id)) { lastEnableError = "CONTROL_OUTPUT_OWNED"; return false; }
    if (ordinaryDigitalClaimedLocked(id)) { lastEnableError = "CONTROL_ORDINARY_STOP_REQUIRED"; return false; }
    if (!_memoryRestoringDisplayId.isEmpty() && id == _memoryRestoringDisplayId) {
        lastEnableError = "SCREEN_MEMORY_RESTORE_IN_PROGRESS";
        return false;
    }
    auto config = getPeripheral(id);
    if (!config) return false;
    if (isMcp23017(*config)) {
        lastEnableError = "IO_GROUP_REQUIRED: explicitly select and enable the MCP channel group";
        return false;
    }
    if (config->isExpanderChannel()) {
        const auto parent = peripherals.find(config->params.gpio.providerId);
        if (parent != peripherals.end() && isMcp23017(parent->second)) {
            lastEnableError = "IO_GROUP_REQUIRED: enable MCP channels with their provider";
            return false;
        }
    }
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
        if (stateIt->second.memoryCleanupPending) {
            if (!deinitHardware(id)) {
                lastEnableError = stateIt->second.lastError;
                return false;
            }
            previousState.memoryCleanupPending = false;
        }
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
        const bool stopped = teardownHardware(*config);
        if (!stopped) {
            runtimeStates[id].status = PeripheralStatus::PERIPHERAL_ERROR;
            if (runtimeStates[id].lastError.isEmpty())
                runtimeStates[id].lastError = "HARDWARE_CLEANUP_PENDING";
            lastEnableError = runtimeStates[id].lastError;
            return false;
        }
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
    if (controlOutputIsClaimed(id)) return false;
    if (!_memoryRestoringDisplayId.isEmpty() && id == _memoryRestoringDisplayId) return false;
    auto config = getPeripheral(id);
    if (!config) return false;
    if (config->isExpanderChannel()) {
        const auto parent = peripherals.find(config->params.gpio.providerId);
        if (parent != peripherals.end() && isMcp23017(parent->second) && parent->second.enabled) return false;
    }

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
    if (!stopped && !isMcp23017(*config)) config->enabled = previousEnabled;
    return stopped;
}

bool PeripheralManager::applyMutationTransactional(
    const String& snapshotId, const String& resultId,
    const std::function<bool(String&)>& mutation, String& errorMsg) {
    RecursiveMutexGuard lock(_mutex);
    // A transactional rollback must never restore a snapshot of an old grant.
    // End ordinary control explicitly before changing that target's config.
    if (ordinaryDigitalClaimedLocked(snapshotId) || ordinaryDigitalClaimedLocked(resultId)) {
        errorMsg = "CONTROL_ORDINARY_STOP_REQUIRED"; return false;
    }
    if (_configurationImportPending) {
        errorMsg = "CONFIG_IMPORT_PENDING_RESTART: imported peripherals await restart";
        return false;
    }
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
    const auto currentRuntime = runtimeStates.find(snapshot.id);
    const bool runtimeUnchanged = [&]() {
        if ((currentRuntime != runtimeStates.end()) != snapshot.hadRuntimeState ||
            _memorySuspendedCount != snapshot.memorySuspendedCount) return false;
        if (!snapshot.hadRuntimeState) return true;
        const auto& current = currentRuntime->second;
        const auto& before = snapshot.runtimeState;
        return current.id == before.id && current.status == before.status &&
            current.memorySuspended == before.memorySuspended &&
            current.memoryCleanupPending == before.memoryCleanupPending && current.initTime == before.initTime &&
            current.lastActivity == before.lastActivity && current.errorCount == before.errorCount &&
            current.lastError == before.lastError && current.controlOutputClaimed == before.controlOutputClaimed &&
            current.controlOutputFault == before.controlOutputFault &&
            current.controlOutputRetiring == before.controlOutputRetiring &&
            current.controlOutputRetirementAttempts == before.controlOutputRetirementAttempts &&
            current.controlOutputRetirementFailed == before.controlOutputRetirementFailed &&
            memcmp(&current.state, &before.state, sizeof(current.state)) == 0;
    }();
    if (runtimeUnchanged && snapshot.existed && resultId == snapshot.id && unchanged != peripherals.end() &&
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

    auto removeCurrent = [this, &errorMsg](const String& id) {
        auto it = peripherals.find(id);
        if (it == peripherals.end()) return true;
        const bool stopped = teardownHardware(it->second);
        if (!stopped) {
            auto& state = runtimeStates[id];
            state.status = PeripheralStatus::PERIPHERAL_ERROR;
            if (state.lastError.isEmpty()) state.lastError = "HARDWARE_CLEANUP_PENDING";
            errorMsg = state.lastError;
            return false;
        }
        peripherals.erase(it);
        runtimeStates.erase(id);
        return true;
    };
    if (!removeCurrent(resultId) || (snapshot.id != resultId && !removeCurrent(snapshot.id))) {
        rebuildPinMapping();
        return false;
    }

    _memorySuspendedCount = snapshot.memorySuspendedCount;
    if (snapshot.existed) {
        peripherals[snapshot.id] = snapshot.config;
        if (snapshot.hadRuntimeState) {
            runtimeStates[snapshot.id] = snapshot.runtimeState;
            // removeCurrent confirmed cleanup; do not restore an obsolete
            // failure marker along with the user's suspended intent.
            runtimeStates[snapshot.id].memoryCleanupPending = false;
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
            if (state != runtimeStates.end() && state->second.status == PeripheralStatus::PERIPHERAL_ERROR &&
                !state->second.lastError.isEmpty()) {
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

bool PeripheralManager::enableIoExpanderGroupAndSave(const String& id,
        const std::vector<String>& channelIds, String& errorMsg) {
    RecursiveMutexGuard lock(_mutex);
    if (_configurationImportPending) { errorMsg = "CONFIG_IMPORT_PENDING_RESTART"; return false; }
    auto parent = peripherals.find(id);
    if (parent == peripherals.end() || !isMcp23017(parent->second)) {
        errorMsg = "IO_GROUP_INVALID: MCP23017 provider required"; return false;
    }
    if (parent->second.enabled || ioProviders().count(id) || ioPendingSyncs().count(id)) {
        errorMsg = "IO_GROUP_BUSY: disable the group and finish retirement first"; return false;
    }
    if (const char* reason = ioExpanderUnavailableReason()) { errorMsg = reason; return false; }
    if (channelIds.empty() || channelIds.size() > 16) {
        errorMsg = "IO_GROUP_INVALID: select 1 to 16 distinct channels"; return false;
    }
    for (const auto& item : peripherals) {
        if (item.second.enabled && item.second.isExpanderChannel() &&
            fixedIdEquals(item.second.params.gpio.providerId, id)) {
            errorMsg = "IO_GROUP_BUSY: all channels must be disabled before activation"; return false;
        }
    }
    std::vector<PeripheralConfig*> selected;
    uint16_t channelMask = 0;
    for (const auto& childId : channelIds) {
        auto child = peripherals.find(childId);
        if (child == peripherals.end() || !child->second.isExpanderChannel() ||
            !fixedIdEquals(child->second.params.gpio.providerId, id) ||
            child->second.params.gpio.channel >= 16) {
            errorMsg = "IO_GROUP_INVALID: selected channel does not belong to this provider"; return false;
        }
        const uint16_t bit = uint16_t(1) << child->second.params.gpio.channel;
        if (channelMask & bit) { errorMsg = "IO_CHANNEL_CONFLICT"; return false; }
        channelMask |= bit;
        selected.push_back(&child->second);
    }
    // Stage only enabled intent under the manager lock. No hardware operation
    // is allowed until the complete group, RESET and shared bus are validated.
    parent->second.enabled = true;
    for (auto* child : selected) child->enabled = true;
    auto restoreDisabled = [&]() {
        parent->second.enabled = false;
        for (auto* child : selected) child->enabled = false;
        rebuildPinMapping();
    };
    auto validateMember = [&](const PeripheralConfig& config) {
        const auto state = runtimeStates.find(config.id);
        if (state == runtimeStates.end() || state->second.memorySuspended ||
            state->second.lastError.startsWith("CONFIG_INVALID:")) {
            errorMsg = "IO_GROUP_INVALID: configuration or runtime unavailable"; return false;
        }
        return validateConfig(config, errorMsg) && validateIoDependencies(config, config.id, errorMsg);
    };
    bool valid = validateMember(parent->second);
    for (auto* child : selected) if (valid) valid = validateMember(*child);
    if (!valid) { restoreDisabled(); return false; }
    invalidateSensorConfigurationLocked();
    const bool started = initHardware(id);
    if (started && saveConfiguration()) {
        for (auto* child : selected) {
            auto& state = runtimeStates[child->id];
            state.status = PeripheralStatus::PERIPHERAL_INITIALIZED;
            state.initTime = millis();
            state.lastError = "";
            state.state.gpio.currentState = GPIOState::STATE_UNDEFINED;
        }
        rebuildPinMapping();
        errorMsg = "";
        return true;
    }
    const String cause = started ? "IO_GROUP_SAVE_FAILED" : "IO_GROUP_INIT_FAILED";
    // Keep the staged children until shutdown has captured their physical
    // startup target. Never reconstruct or reset the previous instance here.
    const bool stopped = teardownIoExpander(id);
    restoreDisabled();
    errorMsg = stopped ? cause + ": group remains disabled; activation may have driven outputs"
                       : String("IO_SHUTDOWN_UNCONFIRMED: ") + cause + "; group disabled";
    auto& state = runtimeStates[id];
    state.status = PeripheralStatus::PERIPHERAL_ERROR;
    state.lastError = errorMsg;
    return false;
}

bool PeripheralManager::disableIoExpanderGroupAndSave(const String& id, String& errorMsg) {
    RecursiveMutexGuard lock(_mutex);
    if (_configurationImportPending) { errorMsg = "CONFIG_IMPORT_PENDING_RESTART"; return false; }
    auto parent = peripherals.find(id);
    if (parent == peripherals.end() || !isMcp23017(parent->second)) {
        errorMsg = "IO_GROUP_INVALID: MCP23017 provider required"; return false;
    }
    std::vector<std::pair<PeripheralConfig*, bool>> children;
    for (auto& item : peripherals) {
        if (item.second.isExpanderChannel() && fixedIdEquals(item.second.params.gpio.providerId, id))
            children.emplace_back(&item.second, item.second.enabled);
    }
    const bool wasEnabled = parent->second.enabled;
    parent->second.enabled = false;
    for (auto& child : children) child.first->enabled = false;
    // Persist disabled intent first. A storage rejection must not touch live
    // outputs or trigger the generic transaction's reinitialization rollback.
    if (!saveConfiguration()) {
        parent->second.enabled = wasEnabled;
        for (auto& child : children) child.first->enabled = child.second;
        errorMsg = "IO_GROUP_SAVE_FAILED: hardware unchanged"; return false;
    }
    invalidateSensorConfigurationLocked();
    // Only the shutdown target calculation sees the previous flags; the
    // manager lock excludes readers and disk already contains disabled intent.
    for (auto& child : children) child.first->enabled = child.second;
    const bool stopped = teardownIoExpander(id);
    for (auto& child : children) {
        child.first->enabled = false;
        auto state = runtimeStates.find(child.first->id);
        if (state != runtimeStates.end()) {
            state->second.state.gpio.currentState = GPIOState::STATE_UNDEFINED;
            if (stopped && !state->second.lastError.startsWith("CONFIG_INVALID:"))
                state->second.status = PeripheralStatus::PERIPHERAL_DISABLED;
        }
    }
    rebuildPinMapping();
    errorMsg = stopped ? "" : "IO_SHUTDOWN_UNCONFIRMED: disabled intent saved; physical state not confirmed";
    return stopped;
}

bool PeripheralManager::disablePeripheralAndSave(const String& id, String& errorMsg) {
    {
        RecursiveMutexGuard lock(_mutex);
        const auto current = peripherals.find(id);
        if (current != peripherals.end() && isMcp23017(current->second))
            return disableIoExpanderGroupAndSave(id, errorMsg);
    }
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
            HardwareSerial* serial = uartIt->second < FastBee::UartPortLease::PORT_COUNT &&
                uartPortLeases[uartIt->second].owned() ? serialForPort(uartIt->second) : nullptr;
            const bool receiveHealthy = serial && getUartReceiveGeneration(id) != 0;
            snapshot.state.comm.isConnected = receiveHealthy;
            snapshot.state.comm.port = uartIt->second;
            snapshot.state.comm.rxAvailable = receiveHealthy ? boundedUartAvailable(serial) : 0;
            if (serial && !receiveHealthy) snapshot.lastError = "UART_RX_FAULT_LATCHED: explicit UART restart required";
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
    if (controlOutputIsClaimed(id)) return false;
    if (ordinaryDigitalClaimedLocked(id)) return false;
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
    if (controlOutputIsClaimed(id)) return false;
    if (!_memoryRestoringDisplayId.isEmpty() && id == _memoryRestoringDisplayId) return false;
    auto config = getPeripheral(id);
    if (!config) return false;

    bool success = teardownHardware(*config);

    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].status = success ? PeripheralStatus::PERIPHERAL_DISABLED
                                          : PeripheralStatus::PERIPHERAL_ERROR;
        if (success) runtimeStates[id].memoryCleanupPending = false;
        if (!success && runtimeStates[id].lastError.isEmpty())
            runtimeStates[id].lastError = "HARDWARE_CLEANUP_PENDING";
    }

    return success;
}

bool PeripheralManager::initAllEnabledPeripherals() {
    int successCount = 0;
    int failCount = 0;

    // MCP activation is explicit and one-shot. Initialize parents before any
    // logical child, including when child IDs sort before the provider ID.
    for (auto& pair : peripherals) {
        if (!pair.second.enabled || !isMcp23017(pair.second)) continue;
        const auto state = runtimeStates.find(pair.first);
        if (state != runtimeStates.end() && state->second.status != PeripheralStatus::PERIPHERAL_ERROR &&
            initHardware(pair.first)) ++successCount;
        else ++failCount;
    }

    for (auto& pair : peripherals) {
        if (pair.second.enabled && !isMcp23017(pair.second)) {
            // A dependent MCP may already have initialized and claimed its
            // RESET output earlier in this pass. Never reinitialize that pad.
            if (controlOutputIsClaimed(pair.first)) {
                const auto state = runtimeStates.find(pair.first);
                if (state != runtimeStates.end() && !state->second.controlOutputFault &&
                    !state->second.controlOutputRetiring &&
                    (state->second.status == PeripheralStatus::PERIPHERAL_INITIALIZED ||
                     state->second.status == PeripheralStatus::PERIPHERAL_RUNNING)) ++successCount;
                else ++failCount;
                continue;
            }
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

        // Stop admission even if cleanup fails, but preserve explicit evidence
        // that the old owner must be retired before update or recovery.
        const bool stopped = teardownHardware(config);
        config.enabled = false;  // runtime-only; saveConfiguration is not called
        stateIt->second.status = stopped ? PeripheralStatus::PERIPHERAL_DISABLED : PeripheralStatus::PERIPHERAL_ERROR;
        stateIt->second.memorySuspended = true;
        stateIt->second.memoryCleanupPending = !stopped;
        if (stopped) stateIt->second.lastError = "memory_pressure_suspended";
        else if (stateIt->second.lastError.isEmpty()) stateIt->second.lastError = "memory_pressure_cleanup_pending";
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
            if (stateIt->second.memoryCleanupPending && !deinitHardware(pair.first)) continue;

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
        : config.type == PeripheralType::PCA9685_PWM ? config.params.pca9685.busPeripheralId
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
    if (config.type == PeripheralType::IO_EXPANDER && frequency != 100000 &&
        !(isMcp23017(config) && frequency == 400000)) {
        errorMsg = "I2C_FREQUENCY_INVALID: PCF8574 100kHz; MCP23017 100/400kHz";
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
    if (config.type == PeripheralType::PCA9685_PWM && !validatePca9685Dependencies(config, errorMsg)) return false;
    if (config.enabled && (config.type == PeripheralType::PCA9685_PWM || isMcp23017(config))) {
        const char* control = config.type == PeripheralType::PCA9685_PWM
            ? config.params.pca9685.oePeripheralId : config.params.ioExpander.resetPeripheralId;
        for (const auto& item : peripherals) {
            if (item.first == excludeId || !item.second.enabled) continue;
            if (ioResetReferenceEquals(item.second, control) ||
                (item.second.type == PeripheralType::PCA9685_PWM && fixedIdEquals(item.second.params.pca9685.oePeripheralId, control))) {
                errorMsg = "CONTROL_REFERENCE_CONFLICT: RESET/OE cannot be shared"; return false;
            }
        }
    }
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
        if (config.params.gpio.channel >= (isMcp23017(parent->second) ? 16 : 8) ||
            (isMcp23017(parent->second) && config.type == PeripheralType::GPIO_DIGITAL_INPUT &&
             (config.params.gpio.channel == 7 || config.params.gpio.channel == 15))) {
            errorMsg = "IO_CHANNEL_INVALID: channel exceeds provider capability or is output-only";
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
                (bus->second.params.i2c.frequency != 0 && bus->second.params.i2c.frequency != 100000 &&
                 !(isMcp23017(config) && bus->second.params.i2c.frequency == 400000)) ||
                bus->second.params.i2c.address != 0 ||
                (config.enabled && !bus->second.enabled)) {
                errorMsg = "I2C_REFERENCE_INVALID: enabled master bus at a supported frequency required";
                return false;
            }
        }
        if (isMcp23017(config)) {
            const auto reset = peripherals.find(config.params.ioExpander.resetPeripheralId);
            if (reset == peripherals.end() || reset->first == config.id ||
                reset->second.type != PeripheralType::GPIO_DIGITAL_OUTPUT || reset->second.isExpanderChannel() ||
                reset->second.pinCount != 1 || reset->second.params.gpio.passive ||
                reset->second.params.gpio.invertLogic ||
                reset->second.params.gpio.initialState != GPIOState::STATE_HIGH ||
                (config.enabled && !reset->second.enabled) || !validateConfig(reset->second, errorMsg)) {
                errorMsg = "RESET_REFERENCE_INVALID: dedicated native non-inverted inactive-HIGH output required";
                return false;
            }
            if (config.enabled) {
                uint16_t outputs = 0;
                for (const auto& item : peripherals) {
                    if (item.first != excludeId && item.second.enabled && ioResetReferenceEquals(item.second, reset->first)) {
                        errorMsg = "RESET_REFERENCE_CONFLICT"; return false;
                    }
                    const auto& child = item.second;
                    if (child.enabled && child.isExpanderChannel() &&
                        fixedIdEquals(child.params.gpio.providerId, config.id)) {
                        const auto childState = runtimeStates.find(item.first);
                        if ((childState != runtimeStates.end() && childState->second.lastError.startsWith("CONFIG_INVALID:")) ||
                            !validateConfig(child, errorMsg) || !validateIoDependencies(child, item.first, errorMsg)) {
                            errorMsg = "IO_GROUP_INVALID: invalid enabled child"; return false;
                        }
                        if (child.type == PeripheralType::GPIO_DIGITAL_OUTPUT)
                            outputs |= uint16_t(1) << child.params.gpio.channel;
                    }
                }
                if ((outputs & 0x8080) != 0x8080) {
                    errorMsg = "IO_GROUP_INCOMPLETE: explicitly configure output channels 7 and 15";
                    return false;
                }
            }
        }
    }
    if (config.enabled && isManagedI2cUser(config)) {
        uint8_t sda, scl;
        uint32_t frequency;
        if (!resolveI2cConfig(config, sda, scl, frequency, errorMsg)) return false;
        if (isMcp23017(config)) {
            const auto& reset = peripherals.at(config.params.ioExpander.resetPeripheralId);
            if (reset.pins[0] == sda || reset.pins[0] == scl) {
                errorMsg = "RESET_PIN_CONFLICT: RESET cannot share SDA/SCL"; return false;
            }
        }
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


bool PeripheralManager::ordinaryIoProducerPresentLocked(const String& id) const {
    const auto found = ioBlinks().find(id);
    return found != ioBlinks().end() && found->second.interval != 0;
}

bool PeripheralManager::prepareOrdinaryIoProducerLocked(const String& id, bool& inserted) {
    inserted = false;
    const auto config = peripherals.find(id);
    if (config == peripherals.end() || !config->second.isExpanderChannel() ||
        ioExpanderUnavailableReason() || ioChannelPending(config->second)) return false;
    const auto provider = ioProviders().find(config->second.params.gpio.providerId);
    if (provider == ioProviders().end() || provider->second->closing() ||
        !provider->second->healthy(millis()) ||
        !provider->second->supportsChannel(config->second.params.gpio.channel)) return false;
    try {
        const auto slot = ioBlinks().emplace(id, IoBlink{});
        if (slot.first->first != id) {
            if (slot.second) ioBlinks().erase(slot.first);
            return false;
        }
        inserted = slot.second;
    } catch (const std::bad_alloc&) { return false; }
    return true;
}

void PeripheralManager::discardOrdinaryIoReservationLocked(const String& id, bool inserted) {
    const auto slot = ioBlinks().find(id);
    if (inserted && slot != ioBlinks().end() && slot->second.interval == 0)
        ioBlinks().erase(slot);
}

bool PeripheralManager::retireOrdinaryIoProducerLocked(const String& id, bool keepSlot) {
    const auto slot = ioBlinks().find(id);
    if (slot == ioBlinks().end()) return true;
    // All provider calls and this removal hold the same recursive PM mutex.
    // Reacquiring it acknowledges any synchronous write from another task.
    // No SDK callback, bus write, guessed level, RESET or pending-sync cleanup.
    if (keepSlot) slot->second = IoBlink{};
    else ioBlinks().erase(slot);
    return true;
}

bool PeripheralManager::startOrdinaryIoProducerLocked(const String& id, uint16_t interval,
    const FastBee::OrdinaryControlContext& context, const FastBee::OrdinaryControl::Permit& permit) {
    const auto config = peripherals.find(id);
    const auto slot = ioBlinks().find(id);
    if (config == peripherals.end() || !config->second.isExpanderChannel() ||
        slot == ioBlinks().end() || slot->second.interval || interval < 200 ||
        ioExpanderUnavailableReason() || ioChannelPending(config->second) ||
        !ordinaryPermitCurrentLocked(id, permit)) return false;
    const auto provider = ioProviders().find(config->second.params.gpio.providerId);
    if (provider == ioProviders().end() || !provider->second->healthy(millis()) ||
        provider->second->closing() || !provider->second->supportsChannel(config->second.params.gpio.channel))
        return false;
    // MCP sample generation changes after an ordinary successful latch write;
    // it is not a provider-session identity. Every provider destruction/restart
    // below first retires all children, including held/executing grants.
    auto& blink = slot->second;
    blink.last = millis();
    blink.interval = interval;
    blink.uartOrigin = context.uartOrigin;
    blink.context = context;
    blink.permit = permit;
    blink.provider = provider->second.get();
    blink.channel = config->second.params.gpio.channel;
    return true;
}

bool PeripheralManager::retireOrdinaryIoGroupLocked(const String& providerId) {
    bool retired = true;
    for (const auto& child : peripherals) {
        if (!child.second.isExpanderChannel() ||
            !fixedIdEquals(child.second.params.gpio.providerId, providerId)) continue;
        // Remove legacy repetition too, without granting it a trusted identity.
        retireOrdinaryIoProducerLocked(child.first, false);
        if (ordinaryDigitalClaimedLocked(child.first)) {
            const auto state = runtimeStates.find(child.first);
            if (state == runtimeStates.end() ||
                !retireOrdinaryDigitalLocked(child.first, _nextOrdinaryControlOperation,
                    false, state->second.ordinaryControl.invocationActive) ||
                ordinaryDigitalClaimedLocked(child.first)) retired = false;
        }
    }
    return retired;
}

void PeripheralManager::serviceOrdinaryIoProducerLocked(const String& id) {
    auto blink = ioBlinks().find(id);
    if (blink == ioBlinks().end() || !blink->second.interval) return;
    const auto config = peripherals.find(id);
    const auto state = runtimeStates.find(id);
    const auto& context = blink->second.context;
    const bool sourceCurrent = blink->second.permit.generation &&
        ordinaryPermitCurrentLocked(id, blink->second.permit) &&
        FastBee::SensorConfigEpoch::matches(context.configEpoch) &&
        (!context.ruleBound || FastBee::RuleOutputIdentity::matches(context.ruleEpoch)) &&
        isUartEventOriginCurrent(context.uartOrigin);
    const auto provider = config == peripherals.end() ? ioProviders().end() :
        ioProviders().find(config->second.params.gpio.providerId);
    const auto parent = config == peripherals.end() ? peripherals.end() :
        peripherals.find(config->second.params.gpio.providerId);
    const auto parentState = config == peripherals.end() ? runtimeStates.end() :
        runtimeStates.find(config->second.params.gpio.providerId);
    const bool current = sourceCurrent && config != peripherals.end() && state != runtimeStates.end() &&
        config->second.enabled && config->second.isExpanderChannel() &&
        config->second.type == PeripheralType::GPIO_DIGITAL_OUTPUT &&
        !state->second.memorySuspended && !state->second.memoryCleanupPending &&
        !state->second.lastError.startsWith("CONFIG_INVALID:") &&
        !ioExpanderUnavailableReason() && !ioChannelPending(config->second) &&
        !controlOutputIsClaimed(id) && parent != peripherals.end() && parent->second.enabled &&
        parent->second.type == PeripheralType::IO_EXPANDER &&
        parentState != runtimeStates.end() &&
        !parentState->second.lastError.startsWith("CONFIG_INVALID:") &&
        provider != ioProviders().end() && provider->second.get() == blink->second.provider &&
        config->second.params.gpio.channel == blink->second.channel &&
        provider->second->supportsChannel(blink->second.channel) &&
        provider->second->initialized() && !provider->second->closing() && !provider->second->failures();
    if (!current) {
        if (state != runtimeStates.end()) {
            state->second.lastError = "CONTROL_IO_SOURCE_OR_SESSION_STALE";
            if (ordinaryDigitalClaimedLocked(id))
                retireOrdinaryDigitalLocked(id, _nextOrdinaryControlOperation, false,
                    state->second.ordinaryControl.invocationActive);
        }
        retireOrdinaryIoProducerLocked(id, false);
        return;
    }
    if (static_cast<uint32_t>(millis() - blink->second.last) < blink->second.interval) return;
    // Cache age and I2C contention may make a read unavailable; no write/replay.
    const GPIOState level = readGpioStateLocked(config->second, &id, nullptr);
    if (level == GPIOState::STATE_UNDEFINED) return;
    const bool written = writeGpioStateLocked(config->second, &id, nullptr,
        level == GPIOState::STATE_HIGH ? GPIOState::STATE_LOW : GPIOState::STATE_HIGH,
        &blink->second.permit);
    if (written) { blink->second.last = millis(); return; }
    if (!provider->second->failures() && provider->second->initialized() &&
        !provider->second->closing()) return; // Busy: deadline remains due.
    state->second.lastError = "CONTROL_IO_TICK_UNCONFIRMED";
    state->second.ordinaryControl.phase = FastBee::OrdinaryControl::Phase::Fault;
    retireOrdinaryDigitalLocked(id, _nextOrdinaryControlOperation, false,
        state->second.ordinaryControl.invocationActive);
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
    // No old child invocation/held grant may cross a provider replacement.
    if (!retireOrdinaryIoGroupLocked(config.id)) return false;
    if (isMcp23017(config)) {
        const String resetId = config.params.ioExpander.resetPeripheralId;
        const auto resetState = runtimeStates.find(resetId);
        if (resetState == runtimeStates.end() || resetState->second.memorySuspended ||
            resetState->second.status == PeripheralStatus::PERIPHERAL_ERROR ||
            resetState->second.controlOutputFault || resetState->second.controlOutputRetiring ||
            resetState->second.lastError.startsWith("CONFIG_INVALID:")) return false;
        if (resetState->second.status != PeripheralStatus::PERIPHERAL_INITIALIZED &&
            resetState->second.status != PeripheralStatus::PERIPHERAL_RUNNING && !initHardware(resetId)) return false;
    }
    FastBee::I2CBusLock busGuard(0);
    if (!busGuard.locked()) return false;
    auto provider = FastBee::createDigitalIoProvider(config, sda, scl, frequency, error);
    if (!provider) {
        if (state != runtimeStates.end()) state->second.lastError = error;
        return false;
    }
    uint16_t mask = 0, inputMask = 0, startup = provider->channelMask();
    for (const auto& item : peripherals) {
        const auto& child = item.second;
        const auto state = runtimeStates.find(item.first);
        if (state != runtimeStates.end() && state->second.lastError.startsWith("CONFIG_INVALID:")) continue;
        if (!child.enabled || !child.isExpanderChannel() ||
            !fixedIdEquals(child.params.gpio.providerId, config.id)) continue;
        if (!provider->supportsChannel(child.params.gpio.channel)) return false;
        const uint16_t bit = uint16_t(1) << child.params.gpio.channel;
        if (child.type == PeripheralType::GPIO_DIGITAL_INPUT) { inputMask |= bit; continue; }
        if (child.type != PeripheralType::GPIO_DIGITAL_OUTPUT) continue;
        mask |= bit;
        const bool high = digitalInputLogicalLevel(child.params.gpio.initialState == GPIOState::STATE_HIGH,
                                                  child.params.gpio.invertLogic);
        if (!high) startup &= ~bit;
    }
    const bool ready = provider->begin(mask, startup,
        config.params.ioExpander.sampleInterval ? config.params.ioExpander.sampleInterval :
            static_cast<uint16_t>(FastBee::H1Parameter::ioInterval.defaultValue), millis(), inputMask);
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
        // A child sync must never reset or replay a failed MCP parent.
        if (isMcp23017(parent->second)) return false;
        if (!setupIoExpander(parent->second)) return false;
        provider = ioProviders().find(providerId);
    }
    if (provider->second->closing()) return false;
    uint16_t mask = 0, inputMask = 0, startup = provider->second->channelMask();
    for (const auto& item : peripherals) {
        const auto& child = item.second;
        const auto state = runtimeStates.find(item.first);
        if (state != runtimeStates.end() && state->second.lastError.startsWith("CONFIG_INVALID:")) continue;
        if (item.first == excludeChild || !child.enabled || !child.isExpanderChannel() ||
            !fixedIdEquals(child.params.gpio.providerId, providerId)) continue;
        if (!provider->second->supportsChannel(child.params.gpio.channel)) return false;
        const uint16_t bit = uint16_t(1) << child.params.gpio.channel;
        if (child.type == PeripheralType::GPIO_DIGITAL_INPUT) { inputMask |= bit; continue; }
        if (child.type != PeripheralType::GPIO_DIGITAL_OUTPUT) continue;
        mask |= bit;
        if (!digitalInputLogicalLevel(child.params.gpio.initialState == GPIOState::STATE_HIGH,
                                      child.params.gpio.invertLogic)) startup &= ~bit;
    }
    return provider->second->configure(mask, startup, millis(), inputMask);
}

bool PeripheralManager::teardownIoExpander(const String& providerId) {
    if (!retireOrdinaryIoGroupLocked(providerId)) return false;
    const auto provider = ioProviders().find(providerId);
    if (provider == ioProviders().end()) {
        const auto state = runtimeStates.find(providerId);
        return state == runtimeStates.end() ||
            !state->second.lastError.startsWith("IO_SHUTDOWN_UNCONFIRMED");
    }
    uint16_t safeHigh = provider->second->channelMask();
    for (const auto& item : peripherals) {
        const auto& child = item.second;
        const auto state = runtimeStates.find(item.first);
        if (!child.enabled || !child.isExpanderChannel() ||
            child.type != PeripheralType::GPIO_DIGITAL_OUTPUT ||
            !provider->second->supportsChannel(child.params.gpio.channel) ||
            !fixedIdEquals(child.params.gpio.providerId, providerId) ||
            (state != runtimeStates.end() && state->second.lastError.startsWith("CONFIG_INVALID:"))) continue;
        if (!digitalInputLogicalLevel(child.params.gpio.initialState == GPIOState::STATE_HIGH,
                                      child.params.gpio.invertLogic)) {
            safeHigh &= static_cast<uint16_t>(~(1U << child.params.gpio.channel));
        }
    }
    const bool wasClosing = provider->second->closing();
    const auto result = provider->second->shutdown(safeHigh);
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
    // Busy must retain the object/lease: destroying it here would invoke the
    // default blocking release. A rejected shutdown that never entered closing
    // also retains the live owner and all pending channel retirements.
    if (provider->second->closing() && result != FastBee::IoExpanderResult::Busy) {
        ioPendingSyncs().erase(providerId);
        ioProviders().erase(provider);
    }
    return confirmed;
}

bool PeripheralManager::shutdownIoExpanders() {
    ioShutdownRequested().store(true, std::memory_order_relaxed);
    const bool pwmStopped = shutdownPca9685();
    RecursiveMutexGuard lock(_mutex, pdMS_TO_TICKS(5));
    if (!lock.isLocked()) {
        LOG_WARNING("Peripheral Manager: IO_SHUTDOWN_UNCONFIRMED: manager busy; physical state not confirmed");
        return false;
    }
    ioBlinks().clear();
    bool confirmed = pwmStopped;
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
    return provider != ioProviders().end() ? provider->second->generation() : 0;
}

void PeripheralManager::pollIoExpanders() {
    RecursiveMutexGuard lock(_mutex, pdMS_TO_TICKS(5));
    if (!lock.isLocked()) return;
    servicePca9685();
    const uint32_t now = millis();
    const uint32_t cycleStarted = now;
    const char* unavailable = ioExpanderUnavailableReason();
    if (unavailable) {
        // Pending channel ownership survives a Busy provider shutdown.
        // Each provider's terminal teardown is the only place that clears it.
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
            if (!item.second.enabled || item.second.type != PeripheralType::IO_EXPANDER || isMcp23017(item.second) ||
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
        // Revoke repetition before PCF recovery can restore startup values.
        // MCP failures remain terminal and never acquire fresh RESET evidence.
        if (item.second->failures() || !item.second->initialized())
            retireOrdinaryIoGroupLocked(item.first);
        item.second->poll(millis());
        if (item.second->failures() || !item.second->initialized())
            retireOrdinaryIoGroupLocked(item.first);
        const bool ok = item.second->healthy(millis());
        const bool configPending = ioPendingSyncs().find(item.first) != ioPendingSyncs().end();
        auto runtime = runtimeStates.find(item.first);
        if (runtime != runtimeStates.end()) {
            runtime->second.status = ok && !configPending
                ? PeripheralStatus::PERIPHERAL_RUNNING : PeripheralStatus::PERIPHERAL_ERROR;
            runtime->second.lastError = configPending ? "IO_CONFIG_PENDING"
                                                      : (ok ? "" : "IO_SAMPLE_UNAVAILABLE");
            if (ok) runtime->second.lastActivity = item.second->sampledAt();
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
                if (item.second->failures() != 0) ioBlinks().erase(child.first);
            } else {
                state->second.state.gpio.currentState = readGpioStateLocked(child.second, &child.first, nullptr);
                state->second.lastActivity = item.second->sampledAt();
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
        // service may erase its own node; other nodes stay valid.
        auto current = it++;
        const String id = current->first;
        if (id != current->first) break; // Do not pass an erased map key by reference.
        serviceOrdinaryIoProducerLocked(id);
    }
}

GPIOState PeripheralManager::readGpioStateLocked(const PeripheralConfig& config,
                                                  const String* peripheralId,
                                                  const char* peripheralIdCStr) {
    if (!config.isGPIOPeripheral()) {
        return GPIOState::STATE_UNDEFINED;
    }
    if (config.isExpanderChannel()) {
        if (!config.enabled || ioExpanderUnavailableReason() || ioChannelPending(config))
            return GPIOState::STATE_UNDEFINED;
        auto state = runtimeStates.find(config.id);
        if (state != runtimeStates.end() && state->second.lastError.startsWith("CONFIG_INVALID:")) {
            return GPIOState::STATE_UNDEFINED;
        }
        auto provider = ioProviders().find(config.params.gpio.providerId);
        bool high = false;
        if (provider == ioProviders().end() ||
            !provider->second->readChannel(config.params.gpio.channel,
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
            bool high = false;
            if (!config.enabled || !_managedPinIo.read(config.id.c_str(), pin, high)) return GPIOState::STATE_UNDEFINED;
            if (config.params.gpio.invertLogic) high = !high;
            return high ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
        }

        case PeripheralType::GPIO_ANALOG_INPUT: {
            AnalogReadSample sample;
            if (!readAnalogSampleByCStr(config.id.c_str(), sample)) return GPIOState::STATE_UNDEFINED;
            const uint8_t bits = config.params.adc.resolution >= 9 && config.params.adc.resolution <= 12
                ? config.params.adc.resolution : 12;
            return sample.raw > (1U << (bits - 1)) ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
        }

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
                                             GPIOState state,
                                             const FastBee::OrdinaryControl::Permit* permit) {
    if (controlOutputIsClaimed(config.id)) return false;
    if (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT &&
        !config.isModbusPeripheral() && !config.params.gpio.passive &&
        (!permit || !ordinaryPermitCurrentLocked(config.id, *permit))) return false;
    if (!config.isGPIOPeripheral()) return false;
    // Scalar output has its own typed value, ownership and SDK path. Legacy
    // HIGH/LOW/toggle cannot bypass it, even while the ordinary lease is idle.
    if (!config.isExpanderChannel() && !config.isModbusPeripheral() &&
        (config.type == PeripheralType::GPIO_PWM_OUTPUT ||
         config.type == PeripheralType::GPIO_ANALOG_OUTPUT ||
         (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT && config.params.gpio.passive))) return false;
    if (state != GPIOState::STATE_HIGH && state != GPIOState::STATE_LOW) return false;
    if (config.isExpanderChannel()) {
        if (!config.enabled || config.type != PeripheralType::GPIO_DIGITAL_OUTPUT ||
            ioExpanderUnavailableReason() || ioChannelPending(config)) return false;
        auto runtime = runtimeStates.find(config.id);
        if (runtime != runtimeStates.end() &&
            runtime->second.lastError.startsWith("CONFIG_INVALID:")) return false;
        auto provider = ioProviders().find(config.params.gpio.providerId);
        if (provider == ioProviders().end()) return false;
        const bool physicalHigh = digitalInputLogicalLevel(
            state == GPIOState::STATE_HIGH, config.params.gpio.invertLogic);
        const bool ok = provider->second->writeChannel(
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
                success = ledcWrite(pin, state == GPIOState::STATE_HIGH ? (maxDuty / 2) : 0);
            } else {
                success = _managedPinIo.write(config.id.c_str(), false, pin, state == GPIOState::STATE_HIGH);
            }
            break;

        case PeripheralType::GPIO_PWM_OUTPUT:
            success = ledcWrite(config.getPrimaryPin(), state == GPIOState::STATE_HIGH
                ? (1U << config.params.gpio.pwmResolution) - 1 : 0U);
            break;

        case PeripheralType::GPIO_ANALOG_OUTPUT:
            success = ledcWrite(config.getPrimaryPin(), state == GPIOState::STATE_HIGH
                ? (uint32_t)((1U << config.params.gpio.pwmResolution) - 1) : 0U);
            break;

        default:
            success = false;
            break;
    }

    // 仅更新已存在的运行时状态，避免错误 ID 触发堆分配。
    if (peripheralId) {
        auto it = runtimeStates.find(*peripheralId);
        if (it != runtimeStates.end()) {
            it->second.state.gpio.currentState = success ? state : GPIOState::STATE_UNDEFINED;
            it->second.lastError = success ? "" : "GPIO_WRITE_UNCONFIRMED";
            if (success) it->second.lastActivity = millis();
        }
    } else {
        auto* rt = getRuntimeStateCStrLocked(peripheralIdCStr);
        if (rt) {
            rt->state.gpio.currentState = success ? state : GPIOState::STATE_UNDEFINED;
            rt->lastError = success ? "" : "GPIO_WRITE_UNCONFIRMED";
            if (success) rt->lastActivity = millis();
        }
    }

    return success;
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
        if (!encoderSlotReady(slot)) {
            return GPIOState::STATE_UNDEFINED;
        }
        portENTER_CRITICAL(&_encoderMux);
        int64_t cnt = slot->count;
        portEXIT_CRITICAL(&_encoderMux);
        return cnt != 0 ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
    }

    if (config->type == PeripheralType::RF_MODULE) {
        // TX is a checked output, not an RX pad or remote-load observation.
        // Never reinterpret its comm.bytesSent union as a cached GPIO level.
        if (config->params.rf.mode != RF_MODE_RX) return GPIOState::STATE_UNDEFINED;
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
    if (!lock.isLocked()) return false;
    auto config = getPeripheral(peripheralId);
    if (!config || !config->enabled || config->isExpanderChannel()) return false;

    // Modbus PWM 外设：通过委托回调写入寄存器
    if (config->isModbusPeripheral()) {
        return writeModbusPWM(peripheralId, *config, dutyCycle);
    }
    if (config->type == PeripheralType::PWM_SERVO) return false; // Trusted servo context required.
    if (config->type == PeripheralType::GPIO_PWM_OUTPUT ||
        config->type == PeripheralType::GPIO_ANALOG_OUTPUT ||
        (config->type == PeripheralType::GPIO_DIGITAL_OUTPUT && config->params.gpio.passive))
        return false; // Trusted scalar context required.

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
            return scheduleServoRelease(peripheralId);
        }
    }
    return success;
}

// ========== 内部辅助方法 ==========


bool PeripheralManager::setupHardware(const PeripheralConfig& config) {
    if (managedPinConflict(config)) return false;
#if FASTBEE_ENABLE_NEOPIXEL
    for (uint8_t i = 0; i < config.configuredPinCount(); ++i) {
        if (fastbeeNeoPixelProjectPinRetained(config.configuredPinAt(i))) {
            runtimeStates[config.id].lastError = "NEOPIXEL_RETAINED_PIN_RESTART_REQUIRED";
            return false;
        }
    }
#endif
    if (controlOutputIsClaimed(config.id)) return false;
    if (ordinaryDigitalClaimedLocked(config.id)) return false;
    if (config.type == PeripheralType::PCA9685_PWM) {
        String error;
        if (!validatePca9685Dependencies(config, error)) return false;
        // Software configured only. A fresh runtime review/start is required;
        // no auto-begin, PWM replay or OE write during boot/re-enable/recovery.
        return true;
    }
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
        return setupStepperHardware(config);
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
        NeoPixelRgb off = {0, 0, 0};
        if (!writeNeoPixelSolid(pin, clampNeoPixelCount(config.params.neopixel.count), off, config.id.c_str())) {
            runtimeStates[config.id].lastError = "NEOPIXEL_TRANSPORT_FAILED";
            return false;
        }
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
#if FASTBEE_ENABLE_RF_LEARN
            if (!RFLearnDriver::getInstance().begin(pin)) return false;
#else
            pinMode(pin, INPUT);
#endif
            LOG_INFOF("Peripheral Manager: RF RX '%s' initialized pin=%d activeHigh=%d",
                      config.id.c_str(), pin, config.params.rf.activeHigh ? 1 : 0);
        } else {
            if (isReservedPin(pin) || isInputOnlyPin(pin)) {
                LOG_ERRORF("Peripheral Manager: RF TX '%s' rejects GPIO%d on %s",
                           config.id.c_str(), pin, CHIP_NAME);
                return false;
            }
            if (!setupRfTransmit(config)) return false;
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
        return setupRadarHardware(config);
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
        return setupEncoderHardware(config);
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
        if (!FastBee::I2CBusManager::instance().acquire(owner->second.get(), sda, scl, freq, error,
                FastBee::ResourceConsumerRole::Peripheral, config.id.c_str())) {
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
        return setupSpiHardware(config);
    }

    // ========== DAC 数模转换初始化 ==========
    if (config.type == PeripheralType::DAC) {
        return setupDACPin(config);
    }

    // ========== ADC 模数转换初始化 ==========
    if (isAdcInputType(config.type)) {
        return setupAnalogInput(config);
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
            // SensorDriver types 4..7 are current/voltage/NTC/current-loop.
            // Their checked ADC batch owns setup; a preliminary pinMode would
            // create an unrelated Arduino GPIO owner that ADC must not steal.
            if (config.params.sensor.sensorType < 4 || config.params.sensor.sensorType > 7)
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
    if (controlOutputIsClaimed(config.id)) return false;
    if (ordinaryDigitalClaimedLocked(config.id) &&
        !retireOrdinaryDigitalLocked(config.id, _nextOrdinaryControlOperation)) return false;
    if (isAdcInputType(config.type)) return teardownAnalogInput(config);
    invalidateSensorConfigurationLocked();
    stopActionTicker(config.id);
    if (config.isExpanderChannel()) {
        // Retire only this channel. Other output bits retain their command shadow.
        auto provider = ioProviders().find(config.params.gpio.providerId);
        if (provider != ioProviders().end()) {
            const String providerId(config.params.gpio.providerId);
            if (!syncIoExpander(providerId, config.id)) {
                if (config.params.gpio.channel < 16) {
                    ioPendingSyncs()[providerId] |= static_cast<uint16_t>(1U << config.params.gpio.channel);
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
    if (config.type == PeripheralType::PCA9685_PWM) return stopPca9685(config.id);
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
    if (config.type == PeripheralType::RADAR_SENSOR) {
        return teardownRadarHardware(config);
    }
    if (isGpioDigitalInputType(config.type) ||
        (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT && !config.params.gpio.passive)) {
        return releaseManagedDigital(config);
    }
    if (config.isGPIOPeripheral()) {
        uint8_t pin = config.getPrimaryPin();
        if (pin != 255) {
            if (!detachInterrupt(pin)) return false;
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
        FastBee::SpiBusOperation operation;
        String stopError;
        if (!operation.locked() || !RFIDDriver::getInstance().stop(&stopError)) {
            if (stopError.isEmpty()) stopError = "SPI_BUSY: RFID shutdown deferred";
            auto state = runtimeStates.find(config.id);
            if (state != runtimeStates.end()) state->second.lastError = stopError;
            LOG_WARNINGF("Peripheral Manager: RFID '%s' stop failed: %s",
                         config.id.c_str(), stopError.c_str());
            return false;
        }
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

    if (config.type == PeripheralType::ENCODER) {
        return teardownEncoderHardware(config);
    }

    if (config.type == PeripheralType::UART) {
        auto it = uartPortById.find(config.id);
        if (it != uartPortById.end()) {
            const uint8_t port = it->second;
            if (port >= FastBee::UartPortLease::PORT_COUNT || !uartPortLeases[port].owned()) {
                lastEnableError = "UART_LEASE_LOST: refusing to stop another owner";
                return false;
            }
            HardwareSerial* serial = serialForPort(port);
            if (!serial) return false;
            FastBee::UartReceiveFault::disarm(port, uartReceiveGenerations.current(port));
            serial->end();
            uartReceiveGenerations.end(port);
            uartPortLeases[port].releaseAfterStop();
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
            if ((other.type == PeripheralType::IO_EXPANDER &&
                 fixedIdEquals(other.params.ioExpander.busPeripheralId, config.id)) ||
                (other.type == PeripheralType::PCA9685_PWM &&
                 fixedIdEquals(other.params.pca9685.busPeripheralId, config.id))) {
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
        return teardownSpiHardware(config);
    }

    // LCD/OLED 显示屏清理
#if FASTBEE_ENABLE_LCD
    if (config.type == PeripheralType::LCD) {
        if (!LCDManager::getInstance().deinitialize(config.id)) {
            runtimeStates[config.id].lastError = "DISPLAY_RELEASE_UNCONFIRMED";
            return false;
        }
    }
#endif

    // TM1637 数码管清理
#if FASTBEE_ENABLE_SEVEN_SEGMENT
    if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        if (!SevenSegmentDriver::instance().release(config.id)) {
            runtimeStates[config.id].lastError = "DISPLAY_RELEASE_UNCONFIRMED";
            return false;
        }
    }
#endif

    if (config.type == PeripheralType::STEPPER_MOTOR) {
        if (!teardownStepperHardware(config.id)) return false;
    }

    if (config.type == PeripheralType::NEO_PIXEL) {
        if (config.pinCount >= 1 && !isReservedPin(config.pins[0]) && !isInputOnlyPin(config.pins[0])) {
#if FASTBEE_ENABLE_NEOPIXEL
            if (!releaseNeoPixelInstance(config.pins[0])) {
                runtimeStates[config.id].lastError = "NEOPIXEL_CLEANUP_PENDING_RESTART_REQUIRED";
                return false;
            }
#endif
        }
        neopixelRainbowIndex.erase(config.id);
    }

    if (config.type == PeripheralType::RF_MODULE && config.params.rf.mode == RF_MODE_TX)
        return releaseRfTransmit(config);
#if FASTBEE_ENABLE_RF_LEARN
    if (config.type == PeripheralType::RF_MODULE && config.params.rf.mode == RF_MODE_RX &&
        config.pinCount && !RFLearnDriver::getInstance().stop(config.pins[0])) return false;
#endif

    // 红外接收头清理：停止解码并释放接收缓冲区
#if FASTBEE_ENABLE_IR_REMOTE
    if (config.type == PeripheralType::IR_RECEIVER) {
        if (!IRRemoteDriver::getInstance().stop()) return false;
    }
#endif

    // GPIO 输出清理：置低电平后释放为输入，防止外设继续带电或浮空
    if (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT) {
        if (config.pinCount >= 1 && !isReservedPin(config.pins[0]) && !isInputOnlyPin(config.pins[0])) {
            if (config.params.gpio.passive) {
                ledcWrite(config.pins[0], 0);
                if (!detachManagedLedc(config.id, config.pins[0])) return false;
            } else {
                digitalWrite(config.pins[0], LOW);
            }
            pinMode(config.pins[0], INPUT);
        }
    }

    if (config.type == PeripheralType::PWM_SERVO) {
        if (!cancelServoRelease(config.id)) return false;
    }

    // PWM / 模拟输出 / 舵机清理：断开 LEDC 并释放引脚
    if (config.type == PeripheralType::GPIO_PWM_OUTPUT ||
        config.type == PeripheralType::GPIO_ANALOG_OUTPUT ||
        config.type == PeripheralType::PWM_SERVO) {
        if (config.pinCount >= 1 && !isReservedPin(config.pins[0]) && !isInputOnlyPin(config.pins[0])) {
            if (!detachManagedLedc(config.id, config.pins[0])) return false;
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
        return releaseManagedDac(config.id, config.getPrimaryPin());
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
            return setupManagedDigital(config);
        }

        case PeripheralType::GPIO_DIGITAL_OUTPUT:
            if (config.params.gpio.passive) {
                // 无源蜂鸣器：使用 PWM 驱动（默认 2700Hz / 8bit）
                uint32_t freq = config.params.gpio.pwmFrequency >= 100
                                    ? config.params.gpio.pwmFrequency : 2700;
                uint8_t res = config.params.gpio.pwmResolution > 0
                                    ? config.params.gpio.pwmResolution : 8;
                uint8_t channel = config.params.gpio.pwmChannel;
                if (!attachManagedLedc(config.id, pin, freq, res, channel)) {
                    LOG_ERRORF("Peripheral Manager: Passive buzzer LEDC attach failed pin=%u channel=%u",
                               (unsigned)pin, (unsigned)channel);
                    return false;
                }
                if (!ledcWrite(pin, 0)) {
                    detachManagedLedc(config.id, pin);
                    return false;
                }  // 初始静音
                auto state = runtimeStates.find(config.id);
                if (state != runtimeStates.end()) state->second.state.gpio.currentState = GPIOState::STATE_LOW;
                LOG_INFOF("Peripheral Manager: Passive buzzer '%s' pin=%d freq=%lu res=%d",
                          config.id.c_str(), pin, (unsigned long)freq, res);
                return true;
            }
            return setupManagedDigital(config);

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

    if (!attachManagedLedc(config.id, pin, config.params.gpio.pwmFrequency,
                           config.params.gpio.pwmResolution, channel)) {
        LOG_ERRORF("Peripheral Manager: LEDC attach failed pin=%u channel=%u",
                   (unsigned)pin, (unsigned)channel);
        return false;
    }

    // 设置初始值
    const uint32_t initialDuty = config.params.gpio.initialState == GPIOState::STATE_HIGH
        ? (1UL << config.params.gpio.pwmResolution) - 1 : 0;
    if (!ledcWrite(pin, initialDuty)) {
        detachManagedLedc(config.id, pin);
        return false;
    }
    auto state = runtimeStates.find(config.id);
    if (state != runtimeStates.end()) state->second.state.gpio.currentState = initialDuty
        ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;

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
    if (!attachManagedLedc(config.id, pin, config.params.servo.frequency,
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
        if (detachManagedLedc(config.id, config.getPrimaryPin())) {
            auto failedState = runtimeStates.find(config.id);
            if (failedState != runtimeStates.end()) failedState->second.state.servo.signalAttached = false;
        }
        return false;
    }

    auto stateIt = runtimeStates.find(config.id);
    if (stateIt != runtimeStates.end()) {
        stateIt->second.state.servo.currentAngle = config.params.servo.initialAngle;
        stateIt->second.state.servo.currentPulseUs = pulseUs;
        stateIt->second.state.servo.signalAttached = true;
    }
    if (config.params.servo.releaseAfterMove && !scheduleServoRelease(config.id)) return false;
    LOG_INFOF("Peripheral Manager: Servo '%s' initialized pin=%u angle=%d pulse=%uus",
              config.id.c_str(), static_cast<unsigned>(config.getPrimaryPin()),
              static_cast<int>(config.params.servo.initialAngle),
              static_cast<unsigned>(pulseUs));
    return true;
}

// ========== 动作定时器管理 ==========
// The SDK callback only carries a non-reused integer token. All context access
// and deletion happen under the PM mutex, so an already queued callback cannot
// dereference a context deleted by stop/replacement or affect its successor.

void PeripheralManager::stopActionTicker(const String& id) {
    RecursiveMutexGuard lock(_mutex);
    if (!lock.isLocked()) return;
    if (ordinaryDigitalClaimedLocked(id)) return; // Ordinary callers must submit an owned Stop.
    ioBlinks().erase(id);
    auto it = actionTickers.find(id);
    if (it != actionTickers.end()) {
        it->second->ticker.detach();
        delete it->second;
        actionTickers.erase(it);
    }
}

static void actionTickerCallback(uint32_t token) {
    PeripheralManager::getInstance().serviceActionTicker(token);
}

void PeripheralManager::serviceActionTicker(uint32_t token) {
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked() || !token) return;
    for (auto it = actionTickers.begin(); it != actionTickers.end(); ++it) {
        auto* data = it->second;
        if (!data || data->token != token) continue;
        const auto config = peripherals.find(it->first);
        // A pre-migration Ticker has no checked stop acknowledgement. Retain
        // its record, suppress every callback write, and prevent new takeover.
        if (config != peripherals.end() && !config->second.isModbusPeripheral() &&
            !config->second.isExpanderChannel() &&
            (config->second.type == PeripheralType::GPIO_PWM_OUTPUT ||
             config->second.type == PeripheralType::GPIO_ANALOG_OUTPUT ||
             (config->second.type == PeripheralType::GPIO_DIGITAL_OUTPUT && config->second.params.gpio.passive))) return;
        const auto runtime = runtimeStates.find(it->first);
        const bool sourceCurrent = isUartEventOriginCurrent(data->uartOrigin);
        const bool targetReady = config != peripherals.end() && runtime != runtimeStates.end() &&
            config->second.enabled && !config->second.isExpanderChannel() && !config->second.isModbusPeripheral() &&
            config->second.getPrimaryPin() == data->pin && !runtime->second.memorySuspended &&
            (runtime->second.status == PeripheralStatus::PERIPHERAL_INITIALIZED ||
             runtime->second.status == PeripheralStatus::PERIPHERAL_RUNNING) && !controlOutputIsClaimed(it->first);
        bool written = false;
        if (sourceCurrent && targetReady) {
            if (data->mode == 1) written = togglePin(it->first);
            else if (data->mode == 2) {
                // Unsigned magnitude supports 16-bit PWM without int16 overflow.
                if (data->increasing) {
                    const uint32_t remaining = data->maxDuty - data->duty;
                    data->duty += data->stepSize < remaining ? data->stepSize : remaining;
                    if (data->duty == data->maxDuty) data->increasing = false;
                } else {
                    data->duty -= data->stepSize < data->duty ? data->stepSize : data->duty;
                    if (!data->duty) data->increasing = true;
                }
                written = writePWM(it->first, data->duty);
            }
        }
        if (!written) {
            if (runtime != runtimeStates.end()) runtime->second.lastError =
                !sourceCurrent ? "UART_SOURCE_UNCONFIRMED" : "ACTION_TICKER_STOPPED";
            data->ticker.detach();
            delete data;
            actionTickers.erase(it);
        }
        return;
    }
}

bool PeripheralManager::startActionTicker(const String& id, uint8_t actionMode, uint16_t paramValue,
                                          const FastBee::UartEventOrigin& uartOrigin) {
    RecursiveMutexGuard lock(_mutex);
    if (controlOutputIsClaimed(id)) return false;
    if (!lock.isLocked() || !isUartEventOriginCurrent(uartOrigin) || id.isEmpty() ||
        id.length() != strlen(id.c_str()) || (actionMode != 1 && actionMode != 2) || !paramValue) return false;
    auto config = getPeripheral(id);
    if (config && !config->isModbusPeripheral() && !config->isExpanderChannel() &&
        (config->type == PeripheralType::GPIO_PWM_OUTPUT ||
         config->type == PeripheralType::GPIO_ANALOG_OUTPUT ||
         (config->type == PeripheralType::GPIO_DIGITAL_OUTPUT && config->params.gpio.passive))) return false;
    // Native and IOX DO producers require a trusted ordinary-control context.
    if (config && config->type == PeripheralType::GPIO_DIGITAL_OUTPUT &&
        !config->isModbusPeripheral() && !config->params.gpio.passive) return false;
    const auto runtime = runtimeStates.find(id);
    if (!config || !config->enabled || runtime == runtimeStates.end() || runtime->second.memorySuspended ||
        (runtime->second.status != PeripheralStatus::PERIPHERAL_INITIALIZED &&
         runtime->second.status != PeripheralStatus::PERIPHERAL_RUNNING)) return false;
    if (config->isExpanderChannel()) return false;
    // Ticker task may only drive native GPIO/PWM. Protocol/I2C transactions
    // belong to their own task/lease paths and must not block the SDK timer task.
    if (config->isModbusPeripheral() || !isValidPin(config->getPrimaryPin()) ||
        isReservedPin(config->getPrimaryPin()) || isInputOnlyPin(config->getPrimaryPin())) return false;
    if (actionMode == 1 && config->type != PeripheralType::GPIO_DIGITAL_OUTPUT &&
        config->type != PeripheralType::GPIO_PWM_OUTPUT) return false;
    if (actionMode == 2 && config->type != PeripheralType::GPIO_PWM_OUTPUT &&
        config->type != PeripheralType::GPIO_ANALOG_OUTPUT) return false;
    if (actionMode == 2 && (config->params.gpio.pwmResolution == 0 || config->params.gpio.pwmResolution > 16)) return false;
    if (_nextActionTickerToken == UINT32_MAX ||
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < FastBee::MemoryBudget::ACTION_TICKER_MIN_DRAM) return false;
    std::unique_ptr<ActionTickerData> data(new (std::nothrow) ActionTickerData());
    if (!data) { LOG_WARNING("Peripheral Manager: action ticker alloc failed"); return false; }
    data->id = id;
    if (data->id != id || data->id.length() != id.length()) return false;
    data->uartOrigin = uartOrigin;
    data->pin = config->getPrimaryPin();
    data->mode = actionMode;
    data->token = ++_nextActionTickerToken;
    if (actionMode == 2) {
        // writePWM consumes percent for calibrated AO, raw counts otherwise.
        data->maxDuty = config->params.gpio.aoMode ? 100U : (1U << config->params.gpio.pwmResolution) - 1U;
        const uint16_t steps = paramValue / 40 ? paramValue / 40 : 1;
        data->stepSize = data->maxDuty / steps;
        if (!data->stepSize) data->stepSize = 1;
    }
    // Allocate the map node before discarding an existing action. Failed SDK
    // timer creation still reports false, without claiming a running action.
    try {
        auto slot = actionTickers.emplace(id, nullptr);
        if (slot.first->first != id) { if (slot.second) actionTickers.erase(slot.first); return false; }
        auto* previous = slot.first->second;
        if (previous) { previous->ticker.detach(); delete previous; }
        slot.first->second = data.release();
        ioBlinks().erase(id);
        auto* installed = slot.first->second;
        installed->ticker.attach_ms(actionMode == 1 ? paramValue : 20, actionTickerCallback, installed->token);
        if (!installed->ticker.active()) {
            delete installed;
            actionTickers.erase(slot.first);
            return false;
        }
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}


bool PeripheralManager::ordinaryNeoPixelQuiescentLocked(const String& id) const {
    const auto config = peripherals.find(id);
    if (config == peripherals.end() || config->second.type != PeripheralType::NEO_PIXEL ||
        config->second.pinCount != 1) return false;
#if FASTBEE_ENABLE_NEOPIXEL
    const uint8_t pin = config->second.pins[0];
    if (fastbeeNeoPixelProjectPinRetained(pin)) return false;
    if (!g_neopixelInstance || g_neopixelPin != pin) return true;
    FastBee::ResourceConsumerIdentity consumer;
    if (!consumer.assign(FastBee::ResourceConsumerRole::Peripheral, id.c_str()) ||
        !g_neopixelConsumer.equals(consumer)) return false;
    const auto state = g_neopixelObservation.load(std::memory_order_acquire);
    // Each frame is synchronous. Retirement revokes future frames only; it
    // neither emits black nor releases the configured strip's allocation.
    return state == FastBee::FeatureInstanceState::Allocated && !g_neopixelBackendRetained;
#else
    return true; // This build cannot start a NeoPixel producer.
#endif
}

bool PeripheralManager::controlNeoPixel(const String& id, const String& action, const String& value,
    const FastBee::OrdinaryControlContext* context, FastBee::OrdinaryControlResult* output) {
    namespace OC = FastBee::OrdinaryControl;
    FastBee::OrdinaryControlResult result;
    result.evaluated = true;
    if (id.length() <= 40 && id.length() == strlen(id.c_str()))
        memcpy(result.targetId, id.c_str(), id.length() + 1);
    auto publish = [&]() {
        if (output && (!output->evaluated || output->accepted)) *output = result;
        return result.accepted;
    };
    if (!context) { result.reason = "CONTROL_CONTEXT_REQUIRED"; return publish(); }
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) { result.reason = "CONTROL_BUSY"; return publish(); }
    const auto config = peripherals.find(id);
    if (config == peripherals.end() || config->second.type != PeripheralType::NEO_PIXEL ||
        config->second.pinCount != 1 || config->second.isModbusPeripheral() ||
        config->second.isExpanderChannel() || isReservedPin(config->second.pins[0]) ||
        isInputOnlyPin(config->second.pins[0])) {
        result.reason = "CONTROL_TARGET_UNSUPPORTED"; return publish();
    }
    if (action.length() > 32 || value.length() > 64 ||
        action.length() != strlen(action.c_str()) || value.length() != strlen(value.c_str())) {
        result.reason = "CONTROL_PARAMETER_INVALID"; return publish();
    }
    String cmd = action, rawValue = value;
    if (cmd != action || rawValue != value) { result.reason = "CONTROL_CAPACITY"; return publish(); }
    cmd.trim(); cmd.toLowerCase(); rawValue.trim();
    const bool stop = cmd == "stop";
    const bool animated = cmd == "chase" || cmd == "theater_chase" || cmd == "strobe" ||
        cmd == "twinkle" || cmd == "fade" || cmd == "breathing" || cmd == "color_wipe" || cmd == "fire";
    const bool rainbow = cmd == "rainbow" || cmd == "cycle" || cmd == "next";
    unsigned brightness = 0;
    if (cmd == "brightness") {
        if (rawValue.isEmpty() || rawValue.length() > 3) {
            result.reason = "CONTROL_PARAMETER_INVALID"; return publish();
        }
        for (size_t i = 0; i < rawValue.length(); ++i) {
            const char digit = rawValue.charAt(i);
            if (digit < '0' || digit > '9') { result.reason = "CONTROL_PARAMETER_INVALID"; return publish(); }
            brightness = brightness * 10U + static_cast<unsigned>(digit - '0');
        }
        if (brightness > 255) { result.reason = "CONTROL_PARAMETER_INVALID"; return publish(); }
    } else if (!stop && !animated && !rainbow && cmd != "off" && cmd != "clear" && cmd != "black") {
        NeoPixelRgb ignored;
        const String& color = (cmd == "color" || cmd == "set" || cmd == "fill" || !rawValue.isEmpty())
            ? rawValue : cmd;
        if (!parseNeoPixelColor(color, ignored)) {
            result.reason = "CONTROL_PARAMETER_INVALID"; return publish();
        }
    }
#if !FASTBEE_ENABLE_NEOPIXEL
    if (!stop) { result.reason = "CONTROL_TARGET_UNSUPPORTED"; return publish(); }
#endif
    uint64_t now = 0;
    const auto intent = stop ? OC::Intent::Stop : OC::Intent::Write;
    if (!inspectOrdinaryControlLocked(id, *context, intent, result, now)) return publish();
    bool inserted = false;
    // Reserve the frame counter before changing ownership. SDK object/buffer
    // allocation happens only after admission and its failure remains visible.
    if (animated || rainbow) {
        try {
            auto slot = neopixelRainbowIndex.emplace(id, 0);
            if (slot.first->first != id) {
                if (slot.second) neopixelRainbowIndex.erase(slot.first);
                result.reason = "CONTROL_CAPACITY"; return publish();
            }
            inserted = slot.second;
        } catch (const std::bad_alloc&) { result.reason = "CONTROL_CAPACITY"; return publish(); }
    }
    OC::Permit permit;
    if (!commitOrdinaryControlLocked(id, *context, intent, result, now, false, permit)) {
        if (inserted) neopixelRainbowIndex.erase(id);
        return publish();
    }
    if (stop) return publish();
    if (!ordinaryPermitCurrentLocked(id, permit)) {
        auto& state = runtimeStates.find(id)->second;
        state.ordinaryControl.phase = OC::Phase::Fault;
        state.lastError = "CONTROL_PERMIT_STALE";
        result.reason = "CONTROL_PERMIT_STALE";
        updateOrdinaryControlResultLocked(id, now, result);
        return publish();
    }
    if (cmd == "brightness") {
        config->second.params.neopixel.brightness = static_cast<uint8_t>(brightness);
        result.accepted = true;
        result.reason = "CONTROL_ACCEPTED_CONFIG_ONLY";
        updateOrdinaryControlResultLocked(id, now, result);
        return publish(); // No SDK output was attempted.
    }
    const bool accepted = controlNeoPixelFrameLocked(id, cmd, rawValue);
    completeOrdinaryControlLocked(id, now, result, accepted, "NEOPIXEL_TRANSPORT_FAILED");
    return publish();
}

bool PeripheralManager::controlNeoPixelFrameLocked(const String& id, const String& action, const String& value) {
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

    // 动画每次提交一帧；只有 checked transport 确认完成后才推进帧序号。
#if FASTBEE_ENABLE_NEOPIXEL
    if (cmd == "chase" || cmd == "theater_chase" || cmd == "strobe" ||
        cmd == "twinkle" || cmd == "fade" || cmd == "breathing" ||
        cmd == "color_wipe" || cmd == "fire") {
        uint16_t count = clampNeoPixelCount(config->params.neopixel.count);
        uint8_t brightness = config->params.neopixel.brightness;
        uint8_t& idx = neopixelRainbowIndex[id];
        Adafruit_NeoPixel* strip = getNeoPixelInstance(pin, count, id.c_str());
        if (!strip) {
            auto state = runtimeStates.find(id);
            if (state != runtimeStates.end()) {
                state->second.status = PeripheralStatus::PERIPHERAL_ERROR;
                state->second.lastError = "NEOPIXEL_TRANSPORT_FAILED";
                state->second.lastActivity = millis();
                state->second.errorCount++;
            }
            return false;
        }

        if (cmd == "chase") {
            // 流水灯: 每次前进一灯，彩色循环
            NeoPixelRgb c = rainbowColorAt(idx);
            NeoPixelRgb bc = applyNeoPixelBrightness(c, brightness);
            uint32_t col = strip->Color(bc.r, bc.g, bc.b);
            for (uint16_t i = 0; i < count; i++) {
                strip->setPixelColor(i, (i == (idx % count)) ? col : 0);
            }
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
        } else if (cmd == "strobe") {
            // 频闪: 奇偶交替全亮/全灭
            NeoPixelRgb c = rainbowColorAt(idx / 8);
            NeoPixelRgb bc = (idx % 2 == 0) ? applyNeoPixelBrightness(c, brightness) : NeoPixelRgb{0,0,0};
            uint32_t col = strip->Color(bc.r, bc.g, bc.b);
            for (uint16_t i = 0; i < count; i++) strip->setPixelColor(i, col);
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
        } else if (cmd == "fade") {
            // 渐变: 亮度从0渐增到目标色
            NeoPixelRgb base = rainbowColorAt(idx / 32);
            uint8_t step = idx % 32;
            uint8_t fadeBri = (uint16_t)brightness * step / 31;
            NeoPixelRgb bc = applyNeoPixelBrightness(base, fadeBri);
            uint32_t col = strip->Color(bc.r, bc.g, bc.b);
            for (uint16_t i = 0; i < count; i++) strip->setPixelColor(i, col);
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
        }

        const bool ok = showNeoPixelChecked(strip);
        if (ok) idx++;
        if (runtimeStates.find(id) != runtimeStates.end()) {
            runtimeStates[id].status = ok ? PeripheralStatus::PERIPHERAL_RUNNING : PeripheralStatus::PERIPHERAL_ERROR;
            runtimeStates[id].lastActivity = millis();
            runtimeStates[id].lastError = ok ? "" : "NEOPIXEL_TRANSPORT_FAILED";
            if (!ok) runtimeStates[id].errorCount++;
        }
        LOG_INFOF("Peripheral Manager: NeoPixel '%s' animation=%s step=%d count=%d -> %s",
                  id.c_str(), action.c_str(), idx, count, ok ? "ok" : "fail");
        return ok;
    }
#endif

    NeoPixelRgb color = {0, 0, 0};
    if (cmd == "rainbow" || cmd == "cycle" || cmd == "next") {
        uint8_t& index = neopixelRainbowIndex[id];
        color = rainbowColorAt(index);
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
    bool ok = writeNeoPixelSolid(pin, count, color, id.c_str());
    if (ok && (cmd == "rainbow" || cmd == "cycle" || cmd == "next")) neopixelRainbowIndex[id]++;
    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].status = ok ? PeripheralStatus::PERIPHERAL_RUNNING : PeripheralStatus::PERIPHERAL_ERROR;
        runtimeStates[id].lastActivity = millis();
        runtimeStates[id].lastError = ok ? "" : "NEOPIXEL_TRANSPORT_FAILED";
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
    if (!writeManagedDac(config.id, pin, config.params.dac.defaultValue)) return false;
    LOG_INFOF("Peripheral Manager: DAC pin %d set to %d", pin, config.params.dac.defaultValue);
    return true;
#else
    LOG_WARNING("DAC not supported on this chip");
    return false;
#endif
}
