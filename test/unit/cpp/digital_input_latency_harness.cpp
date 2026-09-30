#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct String : std::string {
    using std::string::string;
    bool isEmpty() const { return empty(); }
};
static unsigned long clockMs = 0;
unsigned long millis() { return clockMs; }
struct Log {
    template<typename... T> void debugf(const char*, T...) {}
    template<typename... T> void infof(const char*, T...) {}
} LOGGER;
enum class PeripheralType { GPIO_DIGITAL_INPUT, GPIO_DIGITAL_INPUT_PULLUP,
    GPIO_DIGITAL_INPUT_PULLDOWN, INTERRUPT };
enum class DigitalInputMode { AUTO, FLOATING, PULL_UP, PULL_DOWN };
enum class GPIOState { STATE_UNDEFINED, STATE_LOW, STATE_HIGH };
enum class EventType { EVENT_BUTTON_PRESS, EVENT_BUTTON_RELEASE, EVENT_BUTTON_CLICK,
    EVENT_BUTTON_DOUBLE_CLICK, EVENT_BUTTON_LONG_PRESS_2S, EVENT_BUTTON_LONG_PRESS_5S,
    EVENT_BUTTON_LONG_PRESS_10S };
bool isGpioDigitalInputType(PeripheralType) { return true; }
bool supportsButtonEvent(PeripheralType type) { return type != PeripheralType::GPIO_DIGITAL_INPUT; }
bool isGpioInterruptInputType(PeripheralType type) { return type == PeripheralType::INTERRUPT; }
bool matchesDigitalInterruptEdge(PeripheralType, bool) { return true; }
struct PeripheralConfig {
    String id = "di1";
    bool enabled = true;
    PeripheralType type = PeripheralType::GPIO_DIGITAL_INPUT;
    struct { struct { uint8_t inputMode = 2; uint16_t debounceMs = 50;
        bool invertLogic = false; } gpio; } params;
    bool isExpanderChannel() const { return false; }
};
struct PeripheralManager {
    PeripheralConfig config;
    bool high = true;
    static PeripheralManager& getInstance() { static PeripheralManager instance; return instance; }
    bool getPeripheralSnapshot(const String&, PeripheralConfig& out) { out = config; return true; }
    void forEachPeripheral(std::function<void(const PeripheralConfig&)> f) { f(config); }
    GPIOState readPin(const String&) { return high ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW; }
    uint32_t digitalInputGeneration(const PeripheralConfig&) { return 0; }
    bool popDigitalInputInterruptEvent(uint8_t&, GPIOState&) { return false; }
    String getPinPeripheralId(uint8_t) { return "di1"; }
};
// PRODUCTION_DECLARATIONS
struct PeriphExecRule { bool _cachedHasPollCollectionAction = false; };
struct PeriphExecManager {
    unsigned long getPollTriggerCooldownMs(const PeriphExecRule&, const String&) const;
    bool hasButtonEventRule(const String&, const char*) { return false; }
};
struct PeriphExecScheduler {
    unsigned long _lastButtonCheck = 0;
    std::map<String, ButtonRuntimeState> _buttonStates;
    ButtonEventConfig _buttonConfig;
    PeriphExecManager manager;
    PeriphExecManager* _manager = &manager;
    std::vector<unsigned long> fallingEdges;
    unsigned long lastTrigger = 0;
    void checkButtonEvents();
    void checkComplementaryInputs(uint32_t) {}
    void triggerButtonEvent(const String&, EventType) {}
    void handleStableDigitalInputChange(const PeripheralConfig&, bool high) {
        const auto cooldown = manager.getPollTriggerCooldownMs(PeriphExecRule{}, "digital_input");
        if (!high && (!lastTrigger || millis() - lastTrigger >= cooldown)) {
            fallingEdges.push_back(millis());
            lastTrigger = millis();
        }
    }
};
// PRODUCTION_METHODS

void sample(PeriphExecScheduler& scheduler, unsigned long at, bool high) {
    clockMs = at;
    PeripheralManager::getInstance().high = high;
    scheduler.checkButtonEvents();
}

int main() {
    // Sweep every edge phase relative to the real registered task period.
    // 50 ms stable debounce should deliver within 60-79 ms, not 100-200 ms.
    for (unsigned long phase = 1; phase <= TASK_INTERVAL; ++phase) {
        PeriphExecScheduler scheduler;
        const unsigned long edge = 100 + phase;
        for (unsigned long at = TASK_INTERVAL; at <= 300; at += TASK_INTERVAL)
            sample(scheduler, at, at < edge);
        assert(scheduler.fallingEdges.size() == 1);
        const auto latency = scheduler.fallingEdges.front() - edge;
        assert(latency >= 50 && latency <= 80);
    }
    // Three separate 120 ms occlusions within one second must each toggle.
    PeriphExecScheduler repeated;
    for (unsigned long at = TASK_INTERVAL; at <= 900; at += TASK_INTERVAL) {
        bool high = !((at >= 100 && at < 220) || (at >= 360 && at < 480) ||
                      (at >= 620 && at < 740));
        sample(repeated, at, high);
    }
    assert(repeated.fallingEdges.size() == 3);
    // A 40 ms glitch remains rejected; stable hold is emitted only once.
    PeriphExecScheduler bounce;
    for (unsigned long at = TASK_INTERVAL; at <= 1200; at += TASK_INTERVAL)
        sample(bounce, at, !((at >= 100 && at < 140) || at >= 300));
    assert(bounce.fallingEdges.size() == 1 && bounce.fallingEdges[0] == 360);
    // Existing periodic/Modbus load protection remains in force.
    PeriphExecManager manager;
    for (bool heavy : {false, true}) {
        PeriphExecRule rule{heavy};
        assert(manager.getPollTriggerCooldownMs(rule, "digital_input") == 0);
        assert(manager.getPollTriggerCooldownMs(rule, "sensor_cache") == 1000);
        assert(manager.getPollTriggerCooldownMs(rule, "modbus_poll") == (heavy ? 2000 : 1000));
        assert(manager.getPollTriggerCooldownMs(rule, "modbus") == (heavy ? 2000 : 1000));
    }
}
