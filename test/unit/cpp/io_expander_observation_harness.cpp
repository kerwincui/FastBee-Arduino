#include <Arduino.h>
#include <freertos/semphr.h>
#include "core/FeatureInstanceRegistry.h"
#include "core/I2CBusManager.h"
#include "peripherals/IoExpanderProvider.h"
#include <cassert>
#include <map>
#include <memory>
#include <thread>
namespace FastBee {
static FeatureInstanceSnapshot pwmSnapshot{FeatureInstanceState::NotInitialized, 0};
FeatureInstanceSnapshot pca9685InstanceSnapshotLocked() { return pwmSnapshot; }
}
#include "io_observation_under_test.inc"

int main() {
    using namespace FastBee;
    auto& registry = FeatureInstanceRegistry::instance();
    assert(registry.observe("ioExpander").state == FeatureInstanceState::Unobserved);
    static std::atomic<SemaphoreHandle_t> managerMutex{nullptr};
    static const FeatureInstanceObserver observer{"ioExpander", observeIoExpanderInstances, &managerMutex};
    assert(registry.registerObserver(&observer));
    assert(registry.observe("ioExpander").state == FeatureInstanceState::Unobserved);
    auto mutex = xSemaphoreCreateRecursiveMutex();
    managerMutex.store(mutex);
    assert(registry.observe("ioExpander").state == FeatureInstanceState::NotInitialized);
    auto provider = std::make_unique<IoExpanderProvider>(0x20);
    String error;
    assert(provider->acquire(4, 5, error));
    assert(provider->driver.begin(1, 0, 20, 0));
    ioProviders()["provider"] = std::move(provider);
    const unsigned writes = Wire.transmissions, reads = Wire.requests, begins = Wire.begins;
    for (int i = 0; i < 1000; ++i) {
        const auto state = registry.observe("ioExpander");
        assert(state.state == FeatureInstanceState::Initialized && state.initializedCount == 1);
        assert(lastSemaphoreTimeout == 0);
    }
    assert(Wire.transmissions == writes && Wire.requests == reads && Wire.begins == begins);
    pwmSnapshot = {FeatureInstanceState::Initialized, 1};
    assert(registry.observe("ioExpander").initializedCount == 2);
    for (auto state : {FeatureInstanceState::Busy, FeatureInstanceState::CleanupPending}) {
        pwmSnapshot = {state, 0};
        const auto observed = registry.observe("ioExpander");
        assert(observed.state == state && !observed.countKnown());
    }
    pwmSnapshot = {FeatureInstanceState::NotInitialized, 0};
    assert(Wire.transmissions == writes && Wire.requests == reads && Wire.begins == begins);
    std::atomic<bool> entered{false}, release{false};
    std::thread holder([&] {
        RecursiveMutexGuard guard(mutex, 100); assert(guard.isLocked());
        entered.store(true);
        while (!release.load()) std::this_thread::yield();
    });
    while (!entered.load()) std::this_thread::yield();
    for (int i = 0; i < 100; ++i) {
        const auto state = registry.observe("ioExpander");
        assert(state.state == FeatureInstanceState::Busy && !state.observed() && !state.countKnown());
        assert(lastSemaphoreTimeout == 0);
    }
    // Assert the RTOS zero-wait contract, not host scheduler wall-clock latency.
    assert(lastSemaphoreTimeout == 0 && Wire.transmissions == writes && Wire.requests == reads);
    release.store(true); holder.join();
    // The callback never attempts the I2C lock even when another task holds it.
    entered = false; release = false;
    std::thread busHolder([&] {
        auto& bus = I2CBusManager::instance(); assert(bus.lock(100));
        entered.store(true);
        while (!release.load()) std::this_thread::yield();
        bus.unlock();
    });
    while (!entered.load()) std::this_thread::yield();
    assert(registry.observe("ioExpander").initializedCount == 1);
    assert(ioProviders().at("provider")->shutdown(0xff) == IoExpanderResult::Busy);
    assert(registry.observe("ioExpander").state == FeatureInstanceState::CleanupPending);
    assert(Wire.transmissions == writes && Wire.requests == reads);
    release.store(true); busHolder.join();
    assert(ioProviders().at("provider")->shutdown(0xff) == IoExpanderResult::Ok);
    ioProviders().clear();
    assert(registry.observe("ioExpander").state == FeatureInstanceState::NotInitialized);
    // Read/reset races are serialized by the same production recursive guard.
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (int i = 0; i < 1000; ++i) {
            RecursiveMutexGuard guard(mutex, 100); assert(guard.isLocked());
            ioProviders()["uninitialized"] = std::make_unique<IoExpanderProvider>(0x20);
            ioProviders().clear();
        }
        done.store(true);
    });
    do {
        const auto state = registry.observe("ioExpander");
        assert(state.state == FeatureInstanceState::Busy || state.state == FeatureInstanceState::NotInitialized);
    } while (!done.load());
    writer.join();
    managerMutex.store(nullptr);
    assert(registry.observe("ioExpander").state == FeatureInstanceState::Unobserved);
    vSemaphoreDelete(mutex);
}
