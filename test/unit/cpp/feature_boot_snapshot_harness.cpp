#include <Arduino.h>
#include <ArduinoJson.h>
#include "network/FixedJsonField.h"
#include <cassert>
#include "core/FeatureInstanceRegistry.h"
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
        assert(doc["activeMeaning"].as<std::string>() == "admission_only");
        assert(doc["instanceState"].as<std::string>() == "unobserved");
        assert(doc["admissionState"].as<std::string>() ==
               (boot ? "admitted" : intent ? "restart_required" : "user_disabled"));
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
static void dependencyAdmissionIsBoundedAndPreservesIntent() {
    saved(true); Manager manager;
    assert(manager.loadConfig());
    manager.setNetworkReady(true);
    manager.registerFeature({"parent", "Parent", "system", 1, 64,
                             true, true, nullptr, nullptr, 0, true});
    manager.registerFeature({"child", "Child", "system", 1, 64,
                             true, true, nullptr, "parent", 0, false});
    manager.registerFeature({"grandchild", "Grandchild", "system", 1, 64,
                             true, true, nullptr, "child", 0, false});
    const auto before = manager.captureUserConfig();
    auto* parent = manager.findFeature("parent");
    auto* child = manager.findFeature("child");
    const auto* grandchild = manager.findFeature("grandchild");
    assert(manager.shouldEnable("grandchild"));
    manager.setRuntimeDisabled("parent", "memory pressure");
    assert(!manager.shouldEnable("child") && !manager.shouldEnable("grandchild"));
    assert(std::string(manager.admissionState(*grandchild)) == "dependency_blocked");
    assert(std::string(manager.admissionState(*parent)) == "runtime_disabled");
    manager.clearRuntimeDisabled("parent");
    assert(manager.shouldEnable("grandchild"));
    manager.setNetworkReady(false);
    assert(!manager.shouldEnable("grandchild"));
    assert(std::string(manager.admissionState(*parent)) == "waiting_for_network");
    manager.setNetworkReady(true);
    parent->budgetPassed = false;
    assert(!manager.shouldEnable("grandchild"));
    assert(std::string(manager.admissionState(*parent)) == "budget_denied");
    parent->budgetPassed = true;
    parent->meta.compiled = false;
    assert(!manager.shouldEnable("grandchild"));
    assert(std::string(manager.admissionState(*parent)) == "not_compiled");
    parent->meta.compiled = true;
    child->meta.dependsOn = "missing";
    assert(!manager.shouldEnable("grandchild"));
    assert(std::string(manager.admissionState(*grandchild)) == "dependency_missing");
    child->meta.dependsOn = "grandchild";
    assert(!manager.shouldEnable("grandchild"));
    assert(std::string(manager.admissionState(*grandchild)) == "dependency_cycle");
    child->meta.dependsOn = "child";
    assert(!manager.shouldEnable("child"));
    child->meta.dependsOn = "parent";
    assert(manager.shouldEnable("grandchild"));
    assert(manager.captureUserConfig().enabledMask == before.enabledMask);
    assert(manager.captureUserConfig().explicitMask == before.explicitMask);
    assert(!manager.shouldEnable("unknown"));
    // Restart-only dependency uses the running boot snapshot, not newly saved intent.
    child->meta.dependsOn = "ioExpander";
    assert(manager.setUserEnabled("ioExpander", false));
    assert(manager.shouldEnable("child"));
    manager.ioExpanderBootEnabled_ = false;
    assert(manager.setUserEnabled("ioExpander", true));
    assert(!manager.shouldEnable("child"));
    JsonDocument summary;
    manager.getStatusSummaryJson(summary.to<JsonObject>());
    assert(summary["resourceAccounting"].as<std::string>() == "static_estimate");
    assert(summary["instanceMemoryMeasured"].is<bool>());
    assert(!summary["instanceMemoryMeasured"].as<bool>());
}

static void instanceStatusDoesNotChangeAdmissionOrRetainOldCounts() {
    saved(true); Manager manager; manager.setNetworkReady(true); assert(manager.loadConfig());
    static FastBee::FeatureInstanceSnapshot observation{FastBee::FeatureInstanceState::Initialized, 3};
    static const FastBee::FeatureInstanceObserver observer{"i2cSensors", [](const void*) { return observation; }, nullptr};
    assert(FastBee::FeatureInstanceRegistry::instance().registerObserver(&observer));
    JsonDocument doc; auto object = doc.to<JsonObject>();
    const auto before = manager.captureUserConfig();
    assert(manager.getFeatureStatusJson(1, object));
    assert(doc["instanceObserved"].as<bool>());
    assert(doc["instanceScope"].as<std::string>() == "driver_initialization");
    assert(doc["initializedInstances"].as<unsigned>() == 3);
    assert(doc["instanceReason"].as<std::string>().empty());
    manager.setRuntimeDisabled("i2cSensors", "pressure");
    assert(manager.getFeatureStatusJson(1, object));
    assert(!doc["active"].as<bool>() && doc["userEnabled"].as<bool>());
    assert(doc["initializedInstances"].as<unsigned>() == 3);
    assert(doc["instanceState"].as<std::string>() == "initialized");
    for (auto state : {FastBee::FeatureInstanceState::Busy, FastBee::FeatureInstanceState::CleanupPending}) {
        observation = {state, 25};
        assert(manager.getFeatureStatusJson(1, object));
        assert(doc["initializedInstances"].isNull());
        assert(doc["instanceObserved"].as<bool>() == (state == FastBee::FeatureInstanceState::CleanupPending));
    }
    observation = {FastBee::FeatureInstanceState::NotInitialized, 0};
    assert(manager.getFeatureStatusJson(1, object));
    assert(doc["initializedInstances"].is<unsigned>() && doc["initializedInstances"].as<unsigned>() == 0);
    // Reuse the same JSON object across driver, service, busy, and unknown.
    // This deliberately tests transport semantics, not a particular owner's
    // lifecycle: admission remains independent even for retained resources.
    for (auto state : {FastBee::FeatureInstanceState::Allocated, FastBee::FeatureInstanceState::Running,
                       FastBee::FeatureInstanceState::Stopped, FastBee::FeatureInstanceState::Retiring}) {
        observation = FastBee::FeatureInstanceSnapshot::lifecycle(state);
        assert(manager.getFeatureStatusJson(1, object));
        assert(doc["instanceObserved"].as<bool>() && doc["initializedInstances"].isNull());
        assert(doc["instanceScope"].as<std::string>() == "service_lifecycle");
        assert(doc["instanceState"].as<std::string>() == observation.name());
        assert(doc["instanceReason"].as<std::string>().empty());
        assert(!doc["active"].as<bool>() && doc["userEnabled"].as<bool>());
    }
    observation = FastBee::FeatureInstanceSnapshot::lifecycle(FastBee::FeatureInstanceState::Busy);
    assert(manager.getFeatureStatusJson(1, object));
    assert(!doc["instanceObserved"].as<bool>() && doc["initializedInstances"].isNull());
    assert(doc["instanceReason"].as<std::string>() == "observation_busy");
    observation = FastBee::FeatureInstanceSnapshot::unavailable(
        FastBee::FeatureInstanceReason::SynchronizationUnavailable, FastBee::FeatureInstanceScope::ServiceLifecycle);
    assert(manager.getFeatureStatusJson(1, object));
    assert(doc["instanceReason"].as<std::string>() == "synchronization_unavailable");
    assert(doc["instanceScope"].as<std::string>() == "service_lifecycle");
    assert(manager.getFeatureStatusJson(0, object));
    assert(!doc["instanceObserved"].as<bool>() && doc["initializedInstances"].isNull());
    assert(doc["instanceState"].as<std::string>() == "unobserved");
    assert(doc["instanceReason"].as<std::string>() == "observer_not_registered");
    assert(doc["instanceScope"].as<std::string>() == "driver_initialization");
    assert(manager.getFeatureStatusJson(2, object));
    assert(doc["instanceReason"].as<std::string>() == "observer_not_registered");
    manager.features_[2].meta.compiled = false;
    assert(manager.getFeatureStatusJson(2, object));
    assert(doc["instanceReason"].as<std::string>() == "not_compiled");
    assert(manager.captureUserConfig().enabledMask == before.enabledMask);
    assert(manager.captureUserConfig().explicitMask == before.explicitMask);
    manager.features_[2].meta.key = "deviceControl";
    manager.features_[2].meta.compiled = true;
    assert(manager.getFeatureStatusJson(2, object));
    assert(doc["instanceScope"].as<std::string>() == "static_resource");
    assert(doc["instanceReason"].as<std::string>() == "no_dedicated_instance");
    assert(!doc["instanceObserved"].as<bool>() && doc["initializedInstances"].isNull());
    manager.features_[2].meta.key = "commandScript";
    assert(manager.getFeatureStatusJson(2, object));
    assert(doc["instanceScope"].as<std::string>() == "execution_activity");
    assert(doc["bundleRequired"].as<bool>());
    for (const auto* key : {"webServer", "modbus", "ota", "logViewer", "ble", "cellular"}) {
        manager.features_[2].meta.key = key;
        assert(manager.getFeatureStatusJson(2, object));
        assert(doc["instanceScope"].as<std::string>() == "owner_lifetime");
        assert(doc["instanceReason"].as<std::string>() == "observer_not_registered");
        assert(!doc["instanceObserved"].as<bool>() && doc["initializedInstances"].isNull());
    }
    manager.features_[2].meta.key = "mqtts";
    assert(manager.getFeatureStatusJson(2, object));
    assert(doc["instanceScope"].as<std::string>() == "transport_allocation");
    assert(doc["instanceReason"].as<std::string>() == "observer_not_registered");
    manager.features_[2].meta.key = "otaFs";
    manager.features_[2].meta.compiled = false;
    assert(manager.getFeatureStatusJson(2, object));
    assert(doc["instanceReason"].as<std::string>() == "implementation_missing");
    assert(!doc["bundleRequired"].as<bool>());
}

int main() {
    enabledBootSurvivesDisableAndReload();
    disabledBootStagesEnableAndRollsBackSaveFailure();
    missingAndInvalidInitialConfigLatchSafeDefaults();
    dependencyAdmissionIsBoundedAndPreservesIntent();
    instanceStatusDoesNotChangeAdmissionOrRetainOldCounts();
}
