#include <Arduino.h>
#include <ArduinoJson.h>
#include <cassert>
#define private public
#include "core/RuntimeFeatureManager.h"
#undef private

static constexpr const char* kConfigPath = "/config/features.json";
struct FeatureBundle { const char* primary; const char* member; };
static constexpr FeatureBundle kFeatureBundles[] = {
    {"ruleScript", "commandScript"}, {"logViewer", "fileLogging"}
};
struct TestLogger {
    void info(const char*) {}
    void warning(const char*) {}
    void error(const char*) {}
} LOGGER;
struct ConfigStorage {
    JsonDocument stored;
    bool readable = true, exists = true, writable = true;
    static ConfigStorage& getInstance() { static ConfigStorage storage; return storage; }
    bool loadJSONConfig(const char*, JsonDocument& output) {
        if (!readable || !exists) return false;
        output.set(stored.as<JsonVariantConst>()); return true;
    }
};
struct TestFileSystem {
    bool exists(const String& path) {
        return path == kConfigPath && ConfigStorage::getInstance().exists;
    }
    bool remove(const String&) { return true; }
} LittleFS;

namespace FastBee {
// Unrelated registry entries, storage hardware and budget probes are excluded.
// The production loader, setter/save/rollback and status methods run unchanged.
void RuntimeFeatureManager::generateDefaults() {
    count_ = 0;
    registerFeature({"ioExpander", "IO", "peripheral", 6, 1024,
                     true, false, nullptr, nullptr, 12, false});
    registerFeature({"i2cSensors", "Sensors", "peripheral", 15, 500,
                     true, false, nullptr, nullptr, 23, true});
    registerFeature({"mdns", "mDNS", "network", 3, 100,
                     true, true, nullptr, nullptr, 0, false});
}
void RuntimeFeatureManager::syncBudgetState() {}
bool RuntimeFeatureManager::applyDependencyRules() { return false; }
bool RuntimeFeatureManager::mergeLegacyFeatureBundles() { return false; }
bool RuntimeFeatureManager::saveConfig(const JsonDocument& doc) {
    auto& storage = ConfigStorage::getInstance();
    if (!storage.writable) return false;
    storage.stored.set(doc.as<JsonVariantConst>());
    storage.exists = storage.readable = true;
    return true;
}
#include "feature_boot_methods.inc"
}
using Manager = FastBee::RuntimeFeatureManager;
static void saved(bool enabled) {
    auto& storage = ConfigStorage::getInstance();
    storage.readable = storage.exists = storage.writable = true;
    storage.stored.clear(); storage.stored["version"] = 1;
    storage.stored["features"]["ioExpander"]["enabled"] = enabled;
    storage.stored["features"]["i2cSensors"]["enabled"] = enabled;
    storage.stored["features"]["mdns"]["enabled"] = true;
}
static void status(Manager& manager, bool intent, bool boot) {
    for (size_t index = 0; index < 2; ++index) {
        JsonDocument doc;
        assert(manager.getFeatureStatusJson(index, doc.to<JsonObject>()));
        assert(doc["requiresRestart"].as<bool>() && doc["onDemand"].as<bool>());
        assert(doc["userEnabled"].as<bool>() == intent);
        assert(doc["bootEnabled"].as<bool>() == boot);
        assert(doc["pendingRestart"].as<bool>() == (intent != boot));
        assert(doc["active"].as<bool>() == boot);
    }
}
static Manager::ConfigUpdateResult toggle(Manager& manager, bool enabled) {
    JsonDocument request;
    request["features"]["ioExpander"]["enabled"] = enabled;
    request["features"]["i2cSensors"]["enabled"] = enabled;
    return manager.updateAndSave(request);
}
static void enabledBootSurvivesDisableAndReload() {
    saved(true); Manager manager;
    // A pre-load default query must not prevent the first real load from latching.
    assert(!manager.shouldEnable("ioExpander"));
    assert(manager.loadConfig());
    assert(manager.shouldEnable("ioExpander") && !manager.shouldEnable("i2cSensors"));
    manager.setNetworkReady(true); status(manager, true, true);
    assert(toggle(manager, false) == Manager::ConfigUpdateResult::SAVED);
    status(manager, false, true);
    assert(manager.loadConfig()); status(manager, false, true);
    assert(manager.resetToDefaults()); status(manager, false, true);
    // A damaged file repaired in the same boot changes saved intent only.
    ConfigStorage::getInstance().readable = false;
    assert(!manager.loadConfig()); status(manager, false, true);
    manager.setRuntimeDisabled("i2cSensors", "memory pressure");
    assert(!manager.shouldEnable("i2cSensors"));
    manager.clearRuntimeDisabled("i2cSensors");
    assert(manager.shouldEnable("i2cSensors"));
    manager.features_[1].budgetPassed = false;
    assert(!manager.shouldEnable("i2cSensors"));
    manager.features_[1].budgetPassed = true;
    assert(manager.setUserEnabled("mdns", false) && !manager.shouldEnable("mdns"));
    // A new boot finally adopts the disabled saved snapshot.
    Manager rebooted; rebooted.setNetworkReady(true);
    assert(rebooted.loadConfig()); status(rebooted, false, false);
}
static void disabledBootStagesEnableAndRollsBackSaveFailure() {
    saved(false); Manager manager; manager.setNetworkReady(true);
    assert(manager.loadConfig()); status(manager, false, false);
    ConfigStorage::getInstance().writable = false;
    assert(toggle(manager, true) == Manager::ConfigUpdateResult::SAVE_FAILED);
    status(manager, false, false);
    ConfigStorage::getInstance().writable = true;
    assert(toggle(manager, true) == Manager::ConfigUpdateResult::SAVED);
    status(manager, true, false);
    assert(manager.loadConfig()); status(manager, true, false);
    const auto before = manager.captureUserConfig();
    assert(manager.setUserEnabled("i2cSensors", false));
    assert(manager.restoreUserConfig(before)); status(manager, true, false);
    Manager rebooted; rebooted.setNetworkReady(true);
    assert(rebooted.loadConfig()); status(rebooted, true, true);
}
static void missingAndInvalidInitialConfigLatchSafeDefaults() {
    for (bool fileExists : {false, true}) {
        saved(true);
        auto& storage = ConfigStorage::getInstance();
        storage.exists = fileExists; storage.readable = false;
        Manager manager; manager.setNetworkReady(true);
        assert(manager.loadConfig() == !fileExists); status(manager, false, false);
        assert(toggle(manager, true) == Manager::ConfigUpdateResult::SAVED);
        assert(manager.loadConfig()); status(manager, true, false);
    }
}
int main() {
    enabledBootSurvivesDisableAndReload();
    disabledBootStagesEnableAndRollsBackSaveFailure();
    missingAndInvalidInitialConfigLatchSafeDefaults();
}
