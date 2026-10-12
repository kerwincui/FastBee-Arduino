#include <Arduino.h>
#undef malloc
#undef free
#include <ArduinoJson.h>
#include "network/FixedJsonField.h"
#include "core/FeatureInstanceRegistry.h"
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include "serializer_trace_allocator.h"
#define private public
#include "core/RuntimeFeatureManager.h"
#include "oracle_feature_manager.h"
#undef private

static std::vector<std::string> reads;
static FastBee::FeatureInstanceSnapshot observation;
constexpr unsigned MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2;
size_t heap_caps_get_free_size(unsigned caps) {
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    reads.push_back("heap_free");
    return 65537;
}
size_t heap_caps_get_largest_free_block(unsigned caps) {
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    reads.push_back("heap_largest");
    return 16385;
}

#include "feature_bundle_types.inc"
namespace FastBee {
// Loaded fixtures use the actual admission/read methods; unexpected lazy init
// must fail instead of silently replacing the configured state.
void RuntimeFeatureManager::generateDefaults() { assert(false); }
void RuntimeFeatureManager::syncBudgetState() { assert(false); }
bool RuntimeFeatureManager::applyDependencyRules() { assert(false); return false; }
void OracleFeatureManager::generateDefaults() { assert(false); }
void OracleFeatureManager::syncBudgetState() { assert(false); }
bool OracleFeatureManager::applyDependencyRules() { assert(false); return false; }
#include "feature_status_actual.inc"
#include "feature_status_oracle.inc"
}

template<class Manager> void configure(Manager& manager, unsigned keyCase, unsigned stateCase) {
    static const char* const keys[] = {"ioExpander", "i2cSensors", "ota", "otaFs",
        "parent", "commandScript", "lcdGb2312", "mqtts"};
    assert(keyCase < 8 && stateCase < 12);
    manager.loaded_ = true;
    manager.count_ = 3;
    manager.networkReady_ = stateCase != 5;
    manager.ioExpanderBootEnabled_ = stateCase != 1;
    manager.i2cSensorsBootEnabled_ = stateCase != 1;
    manager.runtimeDegraded_ = stateCase == 4;
    for (size_t i = 0; i < manager.count_; ++i) manager.features_[i] = {};
    auto& current = manager.features_[0];
    current.meta = {keys[keyCase], stateCase == 11 ? nullptr : "状态 \"quoted\" name",
        "peripheral", UINT16_MAX, UINT32_MAX, stateCase != 0, true,
        stateCase & 1 ? "GPIO/I2C hardware label" : nullptr,
        stateCase == 6 ? "absent-parent" : stateCase == 7 ? keys[keyCase] :
            stateCase == 8 || stateCase == 11 ? "dependency" : nullptr, 0, stateCase == 5};
    current.userEnabled = stateCase != 1;
    current.budgetPassed = stateCase != 3;
    current.runtimeDisabled = stateCase == 4;
    std::strcpy(current.runtimeReason, "memory pressure: old diagnostic string must preserve ownership");
    if (stateCase == 2) {
        current.userEnabled = true;
        manager.ioExpanderBootEnabled_ = manager.i2cSensorsBootEnabled_ = false;
    }
    manager.features_[1].meta = {"dependency", "Dependency", "system", 17, 1234,
        true, true, nullptr, nullptr, 0, false};
    manager.features_[1].userEnabled = stateCase != 8;
    manager.features_[1].budgetPassed = true;
    // Invisible children are accounted by the displayed parent, unless uncompiled.
    manager.features_[2].meta = {"child", nullptr, "system", 9, 5678,
        stateCase != 9, true, nullptr, keys[keyCase], 0, false};
    manager.features_[2].userEnabled = true;
    manager.features_[2].budgetPassed = true;
    if (stateCase == 10) manager.features_[2].meta.key = "fileLogging";
}

static void selectObservation(unsigned variant) {
    using namespace FastBee;
    if (variant == 0) observation = FeatureInstanceSnapshot::unavailable(FeatureInstanceReason::ObserverMissing);
    else if (variant == 1) observation = {FeatureInstanceState::NotInitialized, 0};
    else if (variant == 2) observation = {FeatureInstanceState::Initialized, UINT16_MAX};
    else if (variant == 3) observation = {FeatureInstanceState::Busy, 0};
    else if (variant == 4) observation = {FeatureInstanceState::CleanupPending, 0};
    else if (variant == 5) observation = FeatureInstanceSnapshot::lifecycle(FeatureInstanceState::Running);
    else if (variant == 6) observation = FeatureInstanceSnapshot::lifecycle(FeatureInstanceState::Retiring);
    else observation = FeatureInstanceSnapshot::unavailable(FeatureInstanceReason::SynchronizationUnavailable);
}

struct FeatureResult {
    std::string bytes, ownership;
    JsonAllocationTrace allocation;
    std::vector<std::string> reads;
    bool overflow = false, returned = false;
    unsigned allocations = 0;
};
template<class Manager> FeatureResult run(unsigned keyCase, unsigned stateCase,
        unsigned observationCase, unsigned limit, bool summary) {
    SerializerTraceAllocator allocator(limit);
    FeatureResult result;
    reads.clear();
    selectObservation(observationCase);
    {
        Manager manager;
        configure(manager, keyCase, stateCase);
        JsonDocument doc(&allocator);
        auto object = doc.to<JsonObject>();
        object["untouched"] = "keep";
        object["initializedInstances"] = 999;
        object["runtimeDisableReason"] = "old";
        if (summary) manager.getStatusSummaryJson(object);
        else result.returned = manager.getFeatureStatusJson(0, object);
        result.overflow = doc.overflowed();
        serializeJson(doc, result.bytes);
        for (JsonPair pair : object) {
            result.ownership += pair.key().isStatic() ? 'K' : 'k';
            if (pair.value().is<const char*>())
                result.ownership += pair.value().as<JsonString>().isStatic() ? 'S' : 's';
            else result.ownership += '-';
        }
        result.reads = reads;
        if (!result.overflow && !summary) {
            assert(object["activeMeaning"] == "admission_only");
            assert(reads.size() == (keyCase == 0 || keyCase == 1 || keyCase == 4 || keyCase == 5 ? 1U : 0U));
            assert(object["instanceObserved"].as<bool>() ==
                (keyCase >= 2 && keyCase != 4 && keyCase != 5 ? false : observation.observed()));
        }
    }
    assert(allocator.live.empty());
    result.allocation = allocator.trace;
    result.allocations = allocator.calls;
    return result;
}
static void equal(const FeatureResult& before, const FeatureResult& after) {
    assert(before.bytes == after.bytes && before.ownership == after.ownership);
    assert(before.overflow == after.overflow && before.returned == after.returned);
    assert(before.allocations == after.allocations && before.allocation == after.allocation);
    assert(before.reads == after.reads);
}
static void compare(unsigned key, unsigned state, unsigned observer, bool summary) {
    bool failure = false, complete = false;
    for (unsigned limit = 0; limit < 256; ++limit) {
        const auto before = run<FastBee::OracleFeatureManager>(key, state, observer, limit, summary);
        const auto after = run<FastBee::RuntimeFeatureManager>(key, state, observer, limit, summary);
        equal(before, after);
        if (!after.overflow) {
            if (summary) {
#if defined(ESP_PLATFORM)
                assert((after.reads == std::vector<std::string>{"heap_free", "heap_largest"}));
#else
                assert(after.reads.empty());
#endif
            }
            complete = true;
            break;
        }
        failure = true;
    }
    assert(failure && complete);
}
static void guards() {
    FastBee::RuntimeFeatureManager current;
    FastBee::OracleFeatureManager previous;
    configure(current, 0, 0); configure(previous, 0, 0);
    reads.clear();
    assert(!current.getFeatureStatusJson(0, JsonObject()));
    assert(!previous.getFeatureStatusJson(0, JsonObject()));
    JsonDocument a, b;
    assert(!current.getFeatureStatusJson(3, a.to<JsonObject>()));
    assert(!previous.getFeatureStatusJson(3, b.to<JsonObject>()));
    current.getStatusSummaryJson(JsonObject());
    previous.getStatusSummaryJson(JsonObject());
    assert(reads.empty() && a.size() == 0 && b.size() == 0);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    static const FastBee::FeatureInstanceObserver observers[] = {
        {"ioExpander", [](const void*) { reads.push_back("observe:ioExpander"); return observation; }, nullptr},
        {"i2cSensors", [](const void*) { reads.push_back("observe:i2cSensors"); return observation; }, nullptr},
        {"parent", [](const void*) { reads.push_back("observe:parent"); return observation; }, nullptr},
        {"commandScript", [](const void*) { reads.push_back("observe:commandScript"); return observation; }, nullptr}
    };
    for (const auto& descriptor : observers)
        assert(FastBee::FeatureInstanceRegistry::instance().registerObserver(&descriptor));
    guards();
    const std::string mode(argv[1]);
    if (mode == "feature") {
        for (unsigned key = 0; key < 8; ++key)
            for (unsigned state = 0; state < 12; ++state)
                for (unsigned observer = 0; observer < 8; ++observer)
                    compare(key, state, observer, false);
    } else if (mode == "summary") {
        for (unsigned key = 0; key < 8; ++key)
            for (unsigned state = 0; state < 12; ++state)
                compare(key, state, 0, true);
    } else assert(false);
    return 0;
}
