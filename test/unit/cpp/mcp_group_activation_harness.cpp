// Only storage, physical I/O and validation are seams. The two transaction
// methods are extracted verbatim from PeripheralManager.cpp by the runner.
#include <Arduino.h>
#include <cassert>
#include <cstring>
#include <map>
#include <vector>
#include <utility>

enum class GPIOState { STATE_LOW, STATE_HIGH, STATE_UNDEFINED };
enum class PeripheralStatus { PERIPHERAL_DISABLED, PERIPHERAL_INITIALIZED, PERIPHERAL_ERROR };
struct PeripheralConfig {
    String id;
    bool enabled = false, child = false, mcp = true, output = true, valid = true;
    struct { struct { char providerId[41] = {}; uint8_t channel = 0; } gpio; } params;
    bool isExpanderChannel() const { return child; }
};
struct PeripheralRuntimeState {
    bool memorySuspended = false;
    String lastError;
    unsigned long initTime = 0;
    PeripheralStatus status = PeripheralStatus::PERIPHERAL_DISABLED;
    struct { struct { GPIOState currentState = GPIOState::STATE_UNDEFINED; } gpio; } state;
};
struct RecursiveMutexGuard { explicit RecursiveMutexGuard(int) {} };
bool isMcp23017(const PeripheralConfig& config) { return config.mcp && !config.child; }
template<size_t N> bool fixedIdEquals(const char (&value)[N], const String& id) {
    return id.length() < N && std::strncmp(value, id.c_str(), N) == 0;
}
std::map<String, int>& ioProviders() { static std::map<String, int> value; return value; }
std::map<String, int>& ioPendingSyncs() { static std::map<String, int> value; return value; }
const char* unavailable = nullptr;
const char* ioExpanderUnavailableReason() { return unavailable; }

struct PeripheralManager {
    int _mutex = 0;
    bool _configurationImportPending = false;
    std::map<String, PeripheralConfig> peripherals;
    std::map<String, PeripheralRuntimeState> runtimeStates;
    std::map<String, bool> disk;
    std::vector<int> events;
    bool startOk = true, saveOk = true, stopOk = true;
    unsigned starts = 0, stops = 0, saves = 0;
    uint16_t stoppedMask = 0;
    bool enableIoExpanderGroupAndSave(const String&, const std::vector<String>&, String&);
    bool disableIoExpanderGroupAndSave(const String&, String&);
    void rebuildPinMapping() {}
    void invalidateSensorConfigurationLocked() {}
    bool validateConfig(const PeripheralConfig& config, String& error) {
        if (config.valid) return true;
        error = "INVALID_TEST_CONFIG"; return false;
    }
    bool validateIoDependencies(const PeripheralConfig& config, const String&, String& error) {
        if (config.child) return true;
        uint16_t outputs = 0;
        for (const auto& item : peripherals) {
            const auto& child = item.second;
            if (child.enabled && child.child && child.output && child.params.gpio.channel < 16)
                outputs |= uint16_t(1) << child.params.gpio.channel;
        }
        if ((outputs & 0x8080) == 0x8080) return true;
        error = "IO_GROUP_INCOMPLETE"; return false;
    }
    bool initHardware(const String& id) {
        ++starts; events.push_back(1); ioProviders()[id] = 1; return startOk;
    }
    bool saveConfiguration() {
        ++saves; events.push_back(2);
        if (!saveOk) return false;
        for (const auto& item : peripherals) disk[item.first] = item.second.enabled;
        return true;
    }
    bool teardownIoExpander(const String& id) {
        ++stops; events.push_back(3); stoppedMask = 0;
        for (const auto& item : peripherals)
            if (item.second.child && item.second.enabled)
                stoppedMask |= uint16_t(1) << item.second.params.gpio.channel;
        if (stopOk) ioProviders().erase(id);
        return stopOk;
    }
};
#include "mcp_group_under_test.inc"

PeripheralManager fixture() {
    ioProviders().clear(); ioPendingSyncs().clear(); unavailable = nullptr;
    PeripheralManager manager;
    PeripheralConfig parent; parent.id = "parent";
    manager.peripherals[parent.id] = parent;
    for (const auto& entry : std::vector<std::pair<const char*, uint8_t>>{{"a7", 7}, {"b7", 15}, {"input", 2}}) {
        PeripheralConfig child; child.id = entry.first; child.child = true;
        child.output = child.id != "input"; child.params.gpio.channel = entry.second;
        std::strcpy(child.params.gpio.providerId, "parent"); manager.peripherals[child.id] = child;
    }
    for (const auto& item : manager.peripherals) {
        manager.runtimeStates[item.first] = PeripheralRuntimeState{}; manager.disk[item.first] = false;
    }
    return manager;
}
void disabled(const PeripheralManager& manager) {
    for (const auto& item : manager.peripherals) assert(!item.second.enabled);
}
int main() {
    String error;
    const std::vector<String> pair{"a7", "b7"};
    { auto m = fixture(); assert(!m.enableIoExpanderGroupAndSave("absent", pair, error)); assert(m.events.empty()); }
    for (const auto& ids : std::vector<std::vector<String>>{{}, {"a7"}, {"a7", "a7"}, {"a7", "missing"}, std::vector<String>(17, "a7")}) {
        auto m = fixture(); assert(!m.enableIoExpanderGroupAndSave("parent", ids, error));
        assert(m.events.empty()); disabled(m);
    }
    { auto m = fixture(); m.peripherals["b7"].valid = false;
      assert(!m.enableIoExpanderGroupAndSave("parent", pair, error)); assert(m.events.empty()); disabled(m); }
    { auto m = fixture(); m.runtimeStates["b7"].memorySuspended = true;
      assert(!m.enableIoExpanderGroupAndSave("parent", pair, error)); assert(m.events.empty()); disabled(m); }
    { auto m = fixture(); unavailable = "FEATURE_DISABLED";
      assert(!m.enableIoExpanderGroupAndSave("parent", pair, error)); assert(m.events.empty()); disabled(m); }
    { auto m = fixture(); ioProviders()["parent"] = 1;
      assert(!m.enableIoExpanderGroupAndSave("parent", pair, error)); assert(m.events.empty()); }
    { auto m = fixture(); ioPendingSyncs()["parent"] = 1;
      assert(!m.enableIoExpanderGroupAndSave("parent", pair, error)); assert(m.events.empty()); }
    { auto m = fixture(); assert(m.enableIoExpanderGroupAndSave("parent", pair, error));
      assert((m.events == std::vector<int>{1, 2})); assert(m.disk["parent"] && m.disk["a7"] && m.disk["b7"]);
      assert(!m.disk["input"] && !m.peripherals["input"].enabled);
      assert(!m.enableIoExpanderGroupAndSave("parent", pair, error)); assert(m.starts == 1); }
    { auto m = fixture(); m.saveOk = false;
      assert(!m.enableIoExpanderGroupAndSave("parent", pair, error)); disabled(m);
      assert((m.events == std::vector<int>{1, 2, 3})); assert(m.stoppedMask == 0x8080);
      assert(!m.disk["parent"] && m.starts == 1 && ioProviders().empty()); }
    { auto m = fixture(); m.startOk = false; m.stopOk = false;
      assert(!m.enableIoExpanderGroupAndSave("parent", pair, error)); disabled(m);
      assert(m.saves == 0 && m.starts == 1 && m.stoppedMask == 0x8080 && ioProviders().count("parent"));
      assert(!m.enableIoExpanderGroupAndSave("parent", pair, error)); assert(m.starts == 1); }
    { auto m = fixture(); assert(m.enableIoExpanderGroupAndSave("parent", pair, error));
      m.saveOk = false; assert(!m.disableIoExpanderGroupAndSave("parent", error));
      assert(m.stops == 0 && m.starts == 1 && m.disk["parent"] && m.peripherals["parent"].enabled); }
    { auto m = fixture(); assert(m.enableIoExpanderGroupAndSave("parent", pair, error));
      assert(m.disableIoExpanderGroupAndSave("parent", error)); disabled(m);
      assert((m.events == std::vector<int>{1, 2, 2, 3}));
      assert(!m.disk["parent"] && !m.disk["a7"] && m.stoppedMask == 0x8080 && ioProviders().empty()); }
    { auto m = fixture(); assert(m.enableIoExpanderGroupAndSave("parent", pair, error));
      m.stopOk = false; assert(!m.disableIoExpanderGroupAndSave("parent", error)); disabled(m);
      assert(!m.disk["parent"] && !m.disk["b7"] && ioProviders().count("parent"));
      assert(error.startsWith("IO_SHUTDOWN_UNCONFIRMED")); }
}
