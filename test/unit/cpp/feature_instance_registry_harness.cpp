#include "core/FeatureInstanceRegistry.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
using namespace FastBee;
static FeatureInstanceSnapshot snapshot;
static std::atomic<unsigned> reads{0};
static FeatureInstanceSnapshot readSnapshot(const void*) { ++reads; return snapshot; }
static FeatureInstanceSnapshot readInitialized(const void*) { return {FeatureInstanceState::Initialized, 1}; }
static const FeatureInstanceObserver observer{"sample", readSnapshot, nullptr};
static void states() {
    auto& registry = FeatureInstanceRegistry::instance();
    assert(!registry.observe("sample").observed() && reads == 0);
    assert(registry.registerObserver(&observer));
    assert(registry.registerObserver(&observer));
    static const FeatureInstanceObserver duplicate{"sample", readInitialized, nullptr};
    assert(!registry.registerObserver(&duplicate));
    snapshot = {FeatureInstanceState::NotInitialized, 0};
    auto value = registry.observe("sample");
    assert(value.observed() && value.countKnown() && value.initializedCount == 0);
    assert(std::string(value.name()) == "not_initialized");
    snapshot = {FeatureInstanceState::Initialized, 42};
    value = registry.observe("sample");
    assert(value.observed() && value.countKnown() && value.initializedCount == 42);
    for (auto state : {FeatureInstanceState::Busy, FeatureInstanceState::CleanupPending}) {
        snapshot = {state, 42}; value = registry.observe("sample");
        assert(value.state == state && !value.countKnown() && value.initializedCount == 0);
        assert(value.observed() == (state == FeatureInstanceState::CleanupPending));
    }
    const char* names[] = {"allocated", "running", "stopped", "retiring"};
    size_t index = 0;
    for (auto state : {FeatureInstanceState::Allocated, FeatureInstanceState::Running,
                       FeatureInstanceState::Stopped, FeatureInstanceState::Retiring}) {
        snapshot = FeatureInstanceSnapshot::lifecycle(state);
        value = registry.observe("sample");
        assert(value.state == state && value.observed() && !value.countKnown());
        assert(value.initializedCount == 0 && std::string(value.name()) == names[index++]);
        assert(std::string(value.scopeName()) == "service_lifecycle" && !value.reasonName()[0]);
    }
    for (auto reason : {FeatureInstanceReason::SynchronizationUnavailable,
                        FeatureInstanceReason::BackendUnavailable, FeatureInstanceReason::InvalidSnapshot}) {
        snapshot = FeatureInstanceSnapshot::unavailable(reason, FeatureInstanceScope::ServiceLifecycle);
        value = registry.observe("sample");
        assert(!value.observed() && !value.countKnown() && value.reason == reason);
        assert(value.scope == FeatureInstanceScope::ServiceLifecycle && value.reasonName()[0]);
    }
    snapshot = FeatureInstanceSnapshot::lifecycle(FeatureInstanceState::Busy);
    value = registry.observe("sample");
    assert(!value.observed() && std::string(value.reasonName()) == "observation_busy");
    assert(value.scope == FeatureInstanceScope::ServiceLifecycle);
    for (auto state : {FeatureInstanceState::Running, FeatureInstanceState::Idle}) {
        snapshot = {state, 0, FeatureInstanceScope::ExecutionActivity};
        value = registry.observe("sample");
        assert(value.observed() && !value.countKnown() && std::string(value.scopeName()) == "execution_activity");
    }
    snapshot = FeatureInstanceSnapshot::unavailable(FeatureInstanceReason::ObserverMissing, FeatureInstanceScope::StaticResource);
    value = registry.observe("sample");
    assert(!value.observed() && !value.countKnown() && std::string(value.scopeName()) == "static_resource");
    for (auto state : {FeatureInstanceState::Allocated, FeatureInstanceState::Retiring, FeatureInstanceState::OwnerAbsent}) {
        snapshot = {state, 0, FeatureInstanceScope::OwnerLifetime};
        value = registry.observe("sample");
        assert(value.observed() && !value.countKnown() && std::string(value.scopeName()) == "owner_lifetime");
    }
    for (auto state : {FeatureInstanceState::Allocated, FeatureInstanceState::Retiring, FeatureInstanceState::Stopped}) {
        snapshot = {state, 0, FeatureInstanceScope::TransportAllocation};
        value = registry.observe("sample");
        assert(value.observed() && !value.countKnown() && std::string(value.scopeName()) == "transport_allocation");
    }
    for (auto invalid : {FeatureInstanceSnapshot{FeatureInstanceState::Initialized, 0},
                         FeatureInstanceSnapshot{FeatureInstanceState::NotInitialized, 1},
                         FeatureInstanceSnapshot{static_cast<FeatureInstanceState>(255), 5},
                         FeatureInstanceSnapshot{FeatureInstanceState::Running, 0},
                         FeatureInstanceSnapshot{FeatureInstanceState::Stopped, 1, FeatureInstanceScope::ServiceLifecycle},
                         FeatureInstanceSnapshot{FeatureInstanceState::Initialized, 1, FeatureInstanceScope::ServiceLifecycle},
                         FeatureInstanceSnapshot{FeatureInstanceState::Idle, 0, FeatureInstanceScope::ServiceLifecycle},
                         FeatureInstanceSnapshot{FeatureInstanceState::Running, 0, FeatureInstanceScope::StaticResource},
                         FeatureInstanceSnapshot{FeatureInstanceState::Busy, 0, FeatureInstanceScope::StaticResource},
                         FeatureInstanceSnapshot{FeatureInstanceState::Running, 1, FeatureInstanceScope::ExecutionActivity},
                         FeatureInstanceSnapshot{FeatureInstanceState::Running, 0, FeatureInstanceScope::OwnerLifetime},
                         FeatureInstanceSnapshot{FeatureInstanceState::Allocated, 1, FeatureInstanceScope::OwnerLifetime},
                         FeatureInstanceSnapshot{FeatureInstanceState::OwnerAbsent, 0, FeatureInstanceScope::ServiceLifecycle},
                         FeatureInstanceSnapshot{FeatureInstanceState::OwnerAbsent, 0, FeatureInstanceScope::TransportAllocation},
                         FeatureInstanceSnapshot{FeatureInstanceState::Running, 0, FeatureInstanceScope::TransportAllocation},
                         FeatureInstanceSnapshot{FeatureInstanceState::Allocated, 1, FeatureInstanceScope::TransportAllocation},
                         FeatureInstanceSnapshot{FeatureInstanceState::Running, 0, static_cast<FeatureInstanceScope>(255)}}) {
        snapshot = invalid; assert(!registry.observe("sample").observed());
        assert(!registry.observe("sample").countKnown());
        assert(std::string(registry.observe("sample").reasonName()) == "invalid_snapshot");
    }
    const auto before = reads.load();
    assert(!registry.observe("missing").observed() && reads == before);
}
static void coverage() {
    for (const auto* key : {"ds1302", "rfid", "ethernet", "ioExpander", "i2cSensors",
            "lcd", "lcd1602", "irRemote", "rfLearn", "ruleScript", "screenPanel", "ds18b20", "neopixel", "sevenSegment",
            "mdns", "fileLogging", "userAdmin", "commandScript",
            "webServer", "modbus", "ota", "logViewer", "ble", "cellular", "mqtts"})
        assert(std::string(missingFeatureInstanceReason(key, true)) == "observer_not_registered");
    for (const auto* key : {"otaFs"})
        assert(std::string(missingFeatureInstanceReason(key, true)) == "owner_snapshot_unavailable");
    for (const auto* key : {"lcdGb2312", "deviceControl"})
        assert(std::string(missingFeatureInstanceReason(key, true)) == "no_dedicated_instance");
    assert(missingFeatureInstanceScope("commandScript") == FeatureInstanceScope::ExecutionActivity);
    assert(missingFeatureInstanceScope("lcdGb2312") == FeatureInstanceScope::StaticResource);
    assert(missingFeatureInstanceScope("deviceControl") == FeatureInstanceScope::StaticResource);
    assert(missingFeatureInstanceScope("mqtts") == FeatureInstanceScope::TransportAllocation);
    for (const auto* key : {"webServer", "modbus", "ota", "logViewer", "ble", "cellular"})
        assert(missingFeatureInstanceScope(key) == FeatureInstanceScope::OwnerLifetime);
    assert(std::string(missingFeatureInstanceReason("lcd", false)) == "not_compiled");
    assert(std::string(missingFeatureInstanceReason("deviceControl", false)) == "not_compiled");
    assert(std::string(missingFeatureInstanceReason(nullptr, true)) == "invalid_snapshot");
    assert(std::string(missingFeatureInstanceReason("futureFeature", true)) == "owner_snapshot_unavailable");
}
static void validation() {
    auto& registry = FeatureInstanceRegistry::instance();
    assert(!registry.registerObserver(nullptr));
    static const FeatureInstanceObserver invalid[] = {
        {nullptr, readSnapshot, nullptr}, {"", readSnapshot, nullptr}, {"key", nullptr, nullptr},
        {"01234567890123456789012345678901234567890", readSnapshot, nullptr}};
    for (const auto& entry : invalid) assert(!registry.registerObserver(&entry));
    assert(!registry.observe(nullptr).observed());
    assert(!registry.observe("").observed());
    static const FeatureInstanceObserver longest{"0123456789012345678901234567890123456789", readInitialized, nullptr};
    assert(registry.registerObserver(&longest));
    assert(registry.observe(longest.key).initializedCount == 1);
}
static void concurrentCapacity() {
    auto& registry = FeatureInstanceRegistry::instance();
    static char keys[FeatureInstanceRegistry::CAPACITY + 1][16];
    static FeatureInstanceObserver entries[FeatureInstanceRegistry::CAPACITY + 1];
    for (size_t i = 0; i <= FeatureInstanceRegistry::CAPACITY; ++i) {
        std::snprintf(keys[i], sizeof(keys[i]), "feature%u", static_cast<unsigned>(i));
        entries[i] = {keys[i], readInitialized, nullptr};
    }
    std::array<std::thread, 8> workers;
    for (auto& worker : workers) worker = std::thread([&] {
        for (size_t pass = 0; pass < 100; ++pass) {
            for (size_t i = 0; i < FeatureInstanceRegistry::CAPACITY; ++i) {
                assert(registry.registerObserver(&entries[i]));
                assert(registry.observe(keys[i]).initializedCount == 1);
            }
        }
    });
    for (auto& worker : workers) worker.join();
    assert(!registry.registerObserver(&entries[FeatureInstanceRegistry::CAPACITY]));
    for (size_t i = 0; i < FeatureInstanceRegistry::CAPACITY; ++i)
        assert(registry.observe(keys[i]).state == FeatureInstanceState::Initialized);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode(argv[1]);
    if (mode == "states") states();
    else if (mode == "validation") validation();
    else if (mode == "concurrency") concurrentCapacity();
    else if (mode == "coverage") coverage();
    else assert(false);
}
