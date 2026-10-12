#include <Arduino.h>
#include "core/I2CBusManager.h"
#include "peripherals/IoExpanderProvider.h"
#include <freertos/semphr.h>
#include <cassert>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include "retirement_guard.inc"

#define LOG_INFO(...) ((void)0)
#define LOG_INFOF(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_ERRORF(...) ((void)0)
#define LOG_WARNINGF(...) ((void)0)

enum class PeripheralType { I2C, IO_EXPANDER, SENSOR_GENERIC, PCA9685_PWM };
enum class PeripheralStatus { PERIPHERAL_ERROR };
struct PeripheralConfig {
    String id = "bus";
    PeripheralType type = PeripheralType::I2C;
    uint8_t pinCount = 2, pins[2] = {4, 5};
    struct {
        struct { uint32_t frequency = 100000; bool isMaster = true; } i2c;
        struct { char busPeripheralId[32] = {}; } ioExpander, sensor, pca9685;
    } params;
};
struct RuntimeState { String lastError; PeripheralStatus status{}; };
static bool fixedIdEquals(const char* value, const String& id) { return id == value; }
namespace FastBee {
struct SensorDriverCache {
    static SensorDriverCache& instance() { static SensorDriverCache cache; return cache; }
    void release(const String&) {}
};
struct RuntimeFeatureManager {
    static RuntimeFeatureManager& instance() { static RuntimeFeatureManager manager; return manager; }
    bool shouldEnable(const char*) { return true; }
};
}
class PeripheralManager {
public:
    std::map<String, PeripheralConfig> peripherals;
    std::map<String, RuntimeState> runtimeStates;
    bool isReservedPin(uint8_t) { return false; }
    bool setupHardware(const PeripheralConfig&);
    bool teardownHardware(const PeripheralConfig&);
};

static const void* g_lcdI2cOwner = nullptr;
static bool g_lcdI2cTransferLocked = false;
static bool g_lcdI2cTransferFailed = false;
class LCDManager {
public:
    int* _display = nullptr;
    SemaphoreHandle_t _displayMutex = xSemaphoreCreateRecursiveMutex();
    bool _initialized = true, _contentChanged = true, _busAcquired = false;
    int _sdaPin = 4, _sclPin = 5;
    ~LCDManager();
    String _activeId;
    bool _foregroundFailed=false;
    bool deinitialize(const String& expectedId="");
};
#define LCD_GUARD_OR_RETURN(value) \
    RecursiveMutexGuard guard(_displayMutex, pdMS_TO_TICKS(250)); \
    if (!guard.isLocked()) return value
#define LCD_GUARD_OR_RETURN_VOID() \
    RecursiveMutexGuard guard(_displayMutex, pdMS_TO_TICKS(250)); \
    if (!guard.isLocked()) return
struct LCD1602Config { uint8_t cols = 16, rows = 2, i2cAddress = 0; bool backlight = true; };
class LCD1602Driver {
public:
    bool _initialized = false, _backlightOn = true, _busAcquired = false, _writeFailed = false;
    uint8_t _sdaPin = 255, _sclPin = 255, _cols = 16, _rows = 2, _address = 0;
    std::function<uint8_t()> onDetect;
    uint8_t detectAddress() { return onDetect ? onDetect() : 0; }
    void backlightOff() {}
    void clear() {}
    void end();
    bool begin(uint8_t, uint8_t, const LCD1602Config&);
};
#include "retirement_methods.inc"

class BusHolder {
    std::atomic<bool> locked{false}, stop{false};
    std::thread worker;
public:
    BusHolder() : worker([this] {
        auto& bus = FastBee::I2CBusManager::instance();
        assert(bus.lock(100)); locked = true;
        while (!stop.load()) std::this_thread::yield();
        bus.unlock();
    }) { while (!locked.load()) std::this_thread::yield(); }
    ~BusHolder() { stop = true; worker.join(); }
};
template<class F> static void boundedRetirement(F action, unsigned timeout) {
    const auto started = std::chrono::steady_clock::now();
    action();
    assert(lastSemaphoreTimeout == pdMS_TO_TICKS(timeout));
    assert(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count() < 500);
}
static void peripheralTokenLifecycle() {
    auto& bus = FastBee::I2CBusManager::instance();
    PeripheralManager pm;
    PeripheralConfig config;
    assert(pm.setupHardware(config));
    const void* token = i2cOwners().at(config.id).get();
    {
        BusHolder holder;
        assert(!pm.setupHardware(config));
        assert(i2cOwners().at(config.id).get() == token);
    }
    assert(bus.clientCount() == 1 && bus.isAcquired(token));
    assert(pm.setupHardware(config) && i2cOwners().at(config.id).get() == token);
    PeripheralConfig conflict = config;
    conflict.id = "conflict"; conflict.pins[0] = 18;
    assert(!pm.setupHardware(conflict));
    assert(i2cOwners().count(conflict.id) == 0 && bus.clientCount() == 1);
    {
        BusHolder holder;
        boundedRetirement([&] { assert(pm.teardownHardware(config)); }, 100);
        assert(i2cOwners().empty());
    }
    assert(bus.clientCount() == 0 && !bus.isAcquired(token));
    config.pins[0] = 18; config.pins[1] = 19; config.params.i2c.frequency = 400000;
    assert(pm.setupHardware(config) && bus.sda() == 18 && bus.frequency() == 400000);
    assert(pm.teardownHardware(config) && bus.clientCount() == 0);
}
static void temporaryAndDisplayClients() {
    auto& bus = FastBee::I2CBusManager::instance();
    String error;
    alignas(ScanLease) unsigned char storage[sizeof(ScanLease)];
    auto* scan = new (storage) ScanLease;
    assert(bus.acquire(scan, 4, 5, 100000, error)); scan->acquired = true;
    {
        BusHolder holder;
        boundedRetirement([&] { scan->~ScanLease(); }, 2);
    }
    assert(bus.clientCount() == 0);
    scan = new (storage) ScanLease;
    assert(bus.acquire(scan, 18, 19, 400000, error)); scan->acquired = true;
    assert(bus.clientCount() == 1 && bus.sda() == 18 && bus.frequency() == 400000);
    scan->~ScanLease();

    auto lcd = std::make_unique<LCDManager>();
    lcd->_activeId = "owned";
    assert(!lcd->deinitialize("other") && lcd->_activeId == "owned" && lcd->_initialized);
    assert(bus.acquire(lcd.get(), 4, 5, 100000, error)); lcd->_busAcquired = true;
    {
        BusHolder holder;
        boundedRetirement([&] { lcd->deinitialize(); }, 100);
        assert(!lcd->_busAcquired && !lcd->_initialized);
    }
    assert(bus.clientCount() == 0);
    assert(bus.acquire(lcd.get(), 18, 19, 400000, error)); lcd->_busAcquired = true;
    {
        BusHolder holder;
        boundedRetirement([&] { lcd.reset(); }, 100);
    }
    assert(bus.clientCount() == 0);

    LCD1602Driver character;
    assert(bus.acquire(&character, 4, 5, 100000, error)); character._busAcquired = true;
    {
        BusHolder holder;
        boundedRetirement([&] { character.end(); }, 100);
        assert(!character._busAcquired);
    }
    assert(bus.clientCount() == 0);
    std::unique_ptr<BusHolder> detectHolder;
    character.onDetect = [&] { detectHolder.reset(new BusHolder); return 0; };
    boundedRetirement([&] { assert(!character.begin(18, 19, LCD1602Config{})); }, 100);
    assert(!character._busAcquired);
    detectHolder.reset();
    assert(bus.clientCount() == 0);
}
static void expanderDestructor() {
    auto& bus = FastBee::I2CBusManager::instance();
    String error;
    auto provider = std::make_unique<FastBee::IoExpanderProvider>(0x20);
    assert(provider->acquire(4, 5, error));
    {
        BusHolder holder;
        boundedRetirement([&] { provider.reset(); }, 100);
    }
    assert(bus.clientCount() == 0);
    int nextOwner;
    assert(bus.acquire(&nextOwner, 18, 19, 400000, error));
    assert(bus.release(&nextOwner, 10) && bus.clientCount() == 0);
}
int main() {
    peripheralTokenLifecycle();
    temporaryAndDisplayClients();
    expanderDestructor();
}
