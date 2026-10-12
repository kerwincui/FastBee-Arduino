#include "core/FeatureInstanceRegistry.h"
#include "core/RuleScriptLifetime.h"
#include <cassert>
#include <mutex>
#include <string>
#include <thread>

using namespace FastBee;
using TickType_t = unsigned;
constexpr int pdTRUE = 1;
constexpr TickType_t portMAX_DELAY = ~0U;
struct Semaphore { std::recursive_mutex mutex; };
using SemaphoreHandle_t = Semaphore*;
static Semaphore pool[16];
static std::atomic<unsigned> allocations{0}, attempts{0}, takes{0}, gives{0}, deletions{0};
static std::atomic<bool> failMutex{false}, failBinary{false};
static std::atomic<bool> blockAllocation{false}, allocationEntered{false};
SemaphoreHandle_t xSemaphoreCreateMutex() {
    ++attempts;
    if (blockAllocation.load()) {
        allocationEntered.store(true);
        while (blockAllocation.load()) std::this_thread::yield();
    }
    if (failMutex.load()) return nullptr;
    const auto index = allocations.fetch_add(1); assert(index < 16);
    return &pool[index];
}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return xSemaphoreCreateMutex(); }
SemaphoreHandle_t xSemaphoreCreateBinary() {
    ++attempts;
    if (failBinary.load()) return nullptr;
    const auto index = allocations.fetch_add(1); assert(index < 16);
    return &pool[index];
}
int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout) {
    // Any blocking timeout added to an observer is a deterministic failure.
    assert(mutex && timeout == 0); ++takes;
    return mutex->mutex.try_lock() ? pdTRUE : 0;
}
void xSemaphoreGive(SemaphoreHandle_t mutex) { assert(mutex); ++gives; mutex->mutex.unlock(); }
int xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t timeout) { return xSemaphoreTake(mutex, timeout); }
void xSemaphoreGiveRecursive(SemaphoreHandle_t mutex) { xSemaphoreGive(mutex); }
void vSemaphoreDelete(SemaphoreHandle_t mutex) { assert(mutex); ++deletions; }
#include "owner_observation_guard.inc"

// These declarations expose owner state for controlled transitions; only the
// business constructors, getters, and observation methods come from production.
struct LCDManager {
    static LCDManager& getInstance();
    LCDManager();
    FeatureInstanceSnapshot observeFeatureInstance() const;
    SemaphoreHandle_t _displayMutex = nullptr;
    bool _initialized = false, _busAcquired = false;
    void* _display = nullptr;
};
struct LCD1602Driver {
    static LCD1602Driver& getInstance();
    FeatureInstanceSnapshot observeFeatureInstance() const;
    std::atomic<bool> _initialized{false};
};
struct IRRemoteDriver {
    static IRRemoteDriver& getInstance();
    bool ensureMutex();
    FeatureInstanceSnapshot observeFeatureInstance() const;
    bool _initialized = false;
    void* _irRecv = nullptr;
    void* _results = nullptr;
    std::atomic<SemaphoreHandle_t> _mutex{nullptr};
};
struct RFLearnDriver {
    static RFLearnDriver& getInstance();
    bool ensureMutex();
    FeatureInstanceSnapshot observeFeatureInstance() const;
    bool _initialized = false;
    std::atomic<SemaphoreHandle_t> _mutex{nullptr};
};
struct RuleScriptManager {
    static RuleScriptManager& getInstance();
    FeatureInstanceSnapshot observeFeatureInstance() const;
    FastBee::RuleScriptLifetime _lifetime;
    std::atomic<bool> _startFailed{false};
};
namespace FastBee { namespace Screen {
static unsigned runtimeConstructions = 0;
struct RuntimeState {
    RuntimeState() { ++runtimeConstructions; }
    SemaphoreHandle_t mutex = nullptr, started = nullptr;
    std::atomic<SemaphoreHandle_t> observationMutex{nullptr};
    std::atomic<bool> stopRequested{false};
    void* task = nullptr;
    bool running = false;
    size_t allocated = 0;
};
RuntimeState& runtimeState() { static RuntimeState state; return state; }
struct ScreenRuntime {
    static ScreenRuntime& instance();
    FeatureInstanceSnapshot observeFeatureInstance() const;
};
}}
#include "owner_observation_methods.inc"

static FeatureInstanceSnapshot observed(const char* key) {
    return FeatureInstanceRegistry::instance().observe(key);
}
static void expect(const char* key, FeatureInstanceState state) {
    const auto value = observed(key);
    assert(value.state == state);
    if (value.scope == FeatureInstanceScope::ServiceLifecycle)
        assert(!value.countKnown() && value.initializedCount == 0);
}
static void* token() { static int placeholder; return &placeholder; }
static void cold() {
    for (const auto* key : {"lcd", "lcd1602", "irRemote", "rfLearn", "ruleScript", "screenPanel"})
        expect(key, FeatureInstanceState::Unobserved);
    assert(attempts == 0 && takes == 0 && Screen::runtimeConstructions == 0);
}
static void registration() {
    cold();
    LCDManager::getInstance(); LCD1602Driver::getInstance(); IRRemoteDriver::getInstance();
    RFLearnDriver::getInstance(); RuleScriptManager::getInstance(); Screen::ScreenRuntime::instance();
    expect("lcd", FeatureInstanceState::Stopped);
    expect("lcd1602", FeatureInstanceState::NotInitialized);
    expect("irRemote", FeatureInstanceState::Stopped);
    expect("rfLearn", FeatureInstanceState::Stopped);
    expect("ruleScript", FeatureInstanceState::Stopped);
    expect("screenPanel", FeatureInstanceState::Unobserved);
    assert(observed("screenPanel").reason == FeatureInstanceReason::SynchronizationUnavailable);
    const auto before = attempts.load();
    for (unsigned i = 0; i < 1000; ++i) {
        LCDManager::getInstance(); IRRemoteDriver::getInstance(); RFLearnDriver::getInstance();
        for (const auto* key : {"lcd", "lcd1602", "irRemote", "rfLearn", "ruleScript", "screenPanel"}) observed(key);
    }
    assert(attempts == before && Screen::runtimeConstructions == 1);
}
static void owners() {
    auto& lcd = LCDManager::getInstance();
    expect("lcd", FeatureInstanceState::Stopped);
    lcd._busAcquired = true; expect("lcd", FeatureInstanceState::Allocated);
    lcd._busAcquired = false; lcd._display = token(); expect("lcd", FeatureInstanceState::Allocated);
    lcd._initialized = true; expect("lcd", FeatureInstanceState::Running);
    lcd._display = nullptr; expect("lcd", FeatureInstanceState::Unobserved);
    assert(observed("lcd").reason == FeatureInstanceReason::InvalidSnapshot);
    lcd._initialized = false; expect("lcd", FeatureInstanceState::Stopped);
    auto& ir = IRRemoteDriver::getInstance();
    ir._irRecv = token(); expect("irRemote", FeatureInstanceState::Allocated);
    ir._initialized = true; expect("irRemote", FeatureInstanceState::Unobserved);
    ir._results = token(); expect("irRemote", FeatureInstanceState::Running);
    ir._initialized = false; expect("irRemote", FeatureInstanceState::Allocated);
    ir._irRecv = ir._results = nullptr; expect("irRemote", FeatureInstanceState::Stopped);
    auto& rf = RFLearnDriver::getInstance();
    rf._initialized = true; expect("rfLearn", FeatureInstanceState::Running);
    rf._initialized = false; expect("rfLearn", FeatureInstanceState::Stopped);
}
static void scope() {
    auto& lcd = LCD1602Driver::getInstance();
    assert(observed("lcd1602").countKnown() && observed("lcd1602").initializedCount == 0);
    lcd._initialized.store(true);
    assert(observed("lcd1602").initializedCount == 1);
    assert(observed("lcd1602").scope == FeatureInstanceScope::DriverInitialization);
    lcd._initialized.store(false); expect("lcd1602", FeatureInstanceState::NotInitialized);
    RuleScriptManager::getInstance();
    const auto beforeTakes = takes.load(), beforeAllocations = attempts.load();
    for (unsigned i = 0; i < 1000; ++i) expect("ruleScript", FeatureInstanceState::Stopped);
    assert(takes == beforeTakes && attempts == beforeAllocations);
}
static void busy() {
    auto& lcd = LCDManager::getInstance(); auto& ir = IRRemoteDriver::getInstance();
    auto& rf = RFLearnDriver::getInstance(); Screen::ScreenRuntime::instance();
    auto& screen = Screen::runtimeState(); assert(Screen::ensureRuntimeSynchronization(screen));
    const char* keys[] = {"lcd", "irRemote", "rfLearn", "screenPanel"};
    SemaphoreHandle_t mutexes[] = {lcd._displayMutex, ir._mutex, rf._mutex, screen.mutex};
    for (unsigned i = 0; i < 4; ++i) {
        std::lock_guard<std::recursive_mutex> held(mutexes[i]->mutex);
        std::thread reader([&, i] {
            for (unsigned n = 0; n < 100; ++n) {
                const auto value = observed(keys[i]);
                assert(value.state == FeatureInstanceState::Busy && !value.observed());
                assert(!value.countKnown() && !value.initializedCount);
                assert(std::string(value.reasonName()) == "observation_busy");
            }
        });
        reader.join();
    }
    assert(gives == 0); // A failed try-lock must never release another owner's lock.
}
static void concurrent() {
    auto& ir = IRRemoteDriver::getInstance();
    const auto before = attempts.load();
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (unsigned n = 0; n < 2000; ++n) {
            std::lock_guard<std::recursive_mutex> held(ir._mutex.load()->mutex);
            ir._initialized = n % 2;
            ir._irRecv = ir._results = ir._initialized ? token() : nullptr;
        }
        done.store(true);
    });
    unsigned reads = 0;
    do {
        const auto value = observed("irRemote");
        assert(value.state == FeatureInstanceState::Busy || value.state == FeatureInstanceState::Running ||
               value.state == FeatureInstanceState::Stopped);
        assert(!value.countKnown()); ++reads;
    } while (!done.load() || reads < 1000);
    writer.join();
    assert(attempts == before);
}
static void allocationFailure() {
    failMutex.store(true);
    LCDManager::getInstance(); IRRemoteDriver::getInstance(); RFLearnDriver::getInstance();
    const auto before = attempts.load();
    for (const auto* key : {"lcd", "irRemote", "rfLearn"}) {
        expect(key, FeatureInstanceState::Unobserved);
        assert(observed(key).reason == FeatureInstanceReason::SynchronizationUnavailable);
    }
    assert(attempts == before && allocations == 0);
    failMutex.store(false);
    IRRemoteDriver::getInstance(); RFLearnDriver::getInstance();
    expect("irRemote", FeatureInstanceState::Stopped); expect("rfLearn", FeatureInstanceState::Stopped);
    assert(attempts == before + 2 && allocations == 2);
    const auto irLock = IRRemoteDriver::getInstance()._mutex.load();
    const auto rfLock = RFLearnDriver::getInstance()._mutex.load();
    IRRemoteDriver::getInstance(); RFLearnDriver::getInstance();
    assert(IRRemoteDriver::getInstance()._mutex.load() == irLock);
    assert(RFLearnDriver::getInstance()._mutex.load() == rfLock && attempts == before + 2);
}
static void ownerSyncConcurrent() {
    for (bool ir : {true, false}) {
        const auto before = attempts.load(); blockAllocation.store(true);
        std::thread workers[4];
        for (auto& worker : workers) worker = std::thread([&] {
            if (ir) IRRemoteDriver::getInstance(); else RFLearnDriver::getInstance();
        });
        while (attempts.load() < before + 4) std::this_thread::yield();
        blockAllocation.store(false);
        for (auto& worker : workers) worker.join();
        expect(ir ? "irRemote" : "rfLearn", FeatureInstanceState::Stopped);
    }
    assert(allocations == 8 && deletions == 6);
}
static void screenLifecycle() {
    Screen::ScreenRuntime::instance(); auto& state = Screen::runtimeState();
    expect("screenPanel", FeatureInstanceState::Unobserved);
    assert(Screen::ensureRuntimeSynchronization(state));
    expect("screenPanel", FeatureInstanceState::Stopped);
    state.task = token(); expect("screenPanel", FeatureInstanceState::Allocated);
    state.running = true; state.allocated = 1024; expect("screenPanel", FeatureInstanceState::Running);
    state.stopRequested.store(true); expect("screenPanel", FeatureInstanceState::Retiring);
    state.running = false; expect("screenPanel", FeatureInstanceState::Retiring);
    state.task = nullptr; expect("screenPanel", FeatureInstanceState::Allocated);
    state.allocated = 0; expect("screenPanel", FeatureInstanceState::Stopped);
    state.running = true; expect("screenPanel", FeatureInstanceState::Unobserved);
    assert(observed("screenPanel").reason == FeatureInstanceReason::InvalidSnapshot);
}
static void screenSync() {
    Screen::ScreenRuntime::instance(); auto& state = Screen::runtimeState();
    failMutex.store(true);
    assert(!Screen::ensureRuntimeSynchronization(state) && !state.observationMutex.load());
    assert(state.started && !state.mutex); expect("screenPanel", FeatureInstanceState::Unobserved);
    const auto started = state.started; failMutex.store(false);
    assert(Screen::ensureRuntimeSynchronization(state));
    assert(state.started == started && state.observationMutex.load() == state.mutex);
    const auto before = attempts.load(); const auto mutex = state.mutex;
    assert(Screen::ensureRuntimeSynchronization(state));
    assert(state.mutex == mutex && attempts == before);
    Screen::RuntimeState binaryFailure;
    failBinary.store(true);
    assert(!Screen::ensureRuntimeSynchronization(binaryFailure));
    const auto retainedMutex = binaryFailure.mutex;
    assert(retainedMutex && !binaryFailure.started && !binaryFailure.observationMutex.load());
    failBinary.store(false);
    assert(Screen::ensureRuntimeSynchronization(binaryFailure));
    assert(binaryFailure.mutex == retainedMutex && binaryFailure.observationMutex.load() == retainedMutex);
}
static void screenSyncConcurrent() {
    Screen::ScreenRuntime::instance(); auto& state = Screen::runtimeState();
    blockAllocation.store(true);
    std::thread starter([&] { assert(Screen::ensureRuntimeSynchronization(state)); });
    while (!allocationEntered.load()) std::this_thread::yield();
    assert(!Screen::ensureRuntimeSynchronization(state));
    expect("screenPanel", FeatureInstanceState::Unobserved);
    blockAllocation.store(false); starter.join();
    assert(attempts == 2 && allocations == 2 && state.observationMutex.load() == state.mutex);
    assert(Screen::ensureRuntimeSynchronization(state)); assert(attempts == 2);
}
static void disabled() {
    Screen::ScreenRuntime::instance();
    assert(observed("screenPanel").reason == FeatureInstanceReason::BackendUnavailable);
    assert(Screen::runtimeConstructions == 0 && attempts == 0);
}
int main(int argc, char** argv) {
    assert(argc == 2); const std::string mode(argv[1]);
    if (mode == "cold") cold();
    else if (mode == "registration") registration();
    else if (mode == "owners") owners();
    else if (mode == "scope") scope();
    else if (mode == "busy") busy();
    else if (mode == "concurrent") concurrent();
    else if (mode == "allocation_failure") allocationFailure();
    else if (mode == "owner_sync_concurrent") ownerSyncConcurrent();
    else if (mode == "screen") screenLifecycle();
    else if (mode == "screen_sync") screenSync();
    else if (mode == "screen_sync_concurrent") screenSyncConcurrent();
    else if (mode == "disabled") disabled();
    else assert(false);
}
