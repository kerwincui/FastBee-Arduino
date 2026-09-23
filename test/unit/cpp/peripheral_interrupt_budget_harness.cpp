#include <Arduino.h>
#include <cmath>
using std::isnan;
using std::isinf;
#include "core/PeripheralConfig.h"
#include <freertos/FreeRTOS.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

using SemaphoreHandle_t = std::recursive_timed_mutex*;
static thread_local unsigned stateDepth = 0;
static TickType_t lastTimeout = portMAX_DELAY;
static BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t timeout) {
    assert(timeout != portMAX_DELAY);
    lastTimeout = timeout;
    if (!mutex || !mutex->try_lock_for(std::chrono::milliseconds(timeout))) return pdFALSE;
    ++stateDepth; return pdTRUE;
}
static BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t mutex) {
    assert(stateDepth); --stateDepth; mutex->unlock(); return pdTRUE;
}
#include "irq_guard.inc"

struct TestQueue {
    struct Event { uint8_t pin, rawLevel; };
    std::deque<Event> events;
    bool refill = false;
    size_t receives = 0;
};
using QueueHandle_t = TestQueue*;
static BaseType_t xQueueReceive(QueueHandle_t queue, void* output, TickType_t timeout) {
    assert(stateDepth && timeout == 0); // Admission must happen before dequeue.
    if (queue->events.empty()) return pdFALSE;
    const auto event = queue->events.front();
    queue->events.pop_front();
    std::memcpy(output, &event, sizeof(event));
    ++queue->receives;
    if (queue->refill) queue->events.push_back(event);
    return pdTRUE;
}
struct RuntimeState {
    struct { struct { GPIOState currentState = GPIOState::STATE_UNDEFINED; } gpio; } state;
    unsigned long lastActivity = 0;
};
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
class PeripheralManager {
public:
    std::recursive_timed_mutex mutex;
    SemaphoreHandle_t _mutex = &mutex;
    std::map<uint8_t, String> pinToPeripheral;
    std::map<String, PeripheralConfig> peripherals;
    std::map<String, RuntimeState> runtimeStates;
    String getPinPeripheralId(uint8_t) const;
    static bool shouldDispatchIsrEvent(unsigned long, unsigned long, uint16_t);
    static uint16_t isrDebounceMsForType(PeripheralType);
#include "irq_members.inc"
};
QueueHandle_t PeripheralManager::_isrQueue = nullptr;
volatile uint32_t PeripheralManager::_isrQueueOverruns = 0;
static uint32_t nowMs = 0;
static uint32_t testMillis() { return nowMs; }
#define millis testMillis
#include "irq_methods.inc"
#undef millis

static unsigned callbacks = 0, callbackCost = 0;
static std::vector<GPIOState> delivered;
static void callback(uint8_t pin, GPIOState state) {
    assert(!stateDepth && pin == 7);
    ++callbacks; delivered.push_back(state); nowMs += callbackCost;
}
static void configure(PeripheralManager& pm, TestQueue& queue, PeripheralType type) {
    PeripheralManager::_isrQueue = &queue;
    pm.pinToPeripheral[7] = "input";
    auto& input = pm.peripherals["input"];
    input.type = type;
    input.params.gpio.debounceMs = 5;
    input.params.gpio.invertLogic = false;
    input.params.gpio.interruptCallback = callback;
    pm.runtimeStates["input"] = RuntimeState{};
    callbacks = callbackCost = 0; nowMs = 0; delivered.clear();
}
static void continuouslyRefilledQueue() {
    PeripheralManager pm; TestQueue queue;
    configure(pm, queue, PeripheralType::RF_MODULE);
    queue.refill = true; queue.events.push_back({7, 1});
    pm.processInterruptQueue();
    assert(callbacks == PeripheralManager::ISR_MAX_EVENTS_PER_CYCLE);
    assert(queue.receives == callbacks && queue.events.size() == 1);
    assert(lastTimeout == pdMS_TO_TICKS(PeripheralManager::ISR_STATE_LOCK_TIMEOUT_MS));
    pm.processInterruptQueue();
    assert(callbacks == 2 * PeripheralManager::ISR_MAX_EVENTS_PER_CYCLE);
}
static void timeBudgetAndClockWrap() {
    PeripheralManager pm; TestQueue queue;
    configure(pm, queue, PeripheralType::IR_RECEIVER);
    queue.refill = true; queue.events.push_back({7, 1});
    callbackCost = PeripheralManager::ISR_MAINTENANCE_BUDGET_MS;
    nowMs = UINT32_MAX - 1;
    pm.processInterruptQueue();
    assert(callbacks == 1 && queue.receives == 1 && queue.events.size() == 1);
    // A slow callback completes outside the PM lock, then ends this round.
    callbackCost = 100;
    pm.processInterruptQueue();
    assert(callbacks == 2 && queue.receives == 2);
}
static void slowConfigurationWriterPreservesEvents() {
    PeripheralManager pm; TestQueue queue;
    configure(pm, queue, PeripheralType::RF_MODULE);
    queue.events.push_back({7, 1}); queue.events.push_back({7, 0});
    std::atomic<bool> locked{false}, release{false};
    std::thread writer([&] {
        pm.mutex.lock(); locked = true;
        while (!release.load()) std::this_thread::yield();
        pm.mutex.unlock();
    });
    while (!locked.load()) std::this_thread::yield();
    const auto started = std::chrono::steady_clock::now();
    pm.processInterruptQueue();
    assert(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count() < 100);
    assert(lastTimeout == pdMS_TO_TICKS(PeripheralManager::ISR_STATE_LOCK_TIMEOUT_MS));
    assert(queue.receives == 0 && queue.events.size() == 2 && callbacks == 0);
    release = true; writer.join();
    pm.processInterruptQueue();
    assert(queue.events.empty() && callbacks == 2);
    assert(delivered[0] == GPIOState::STATE_HIGH && delivered[1] == GPIOState::STATE_LOW);
}
static void debounceAndLogicalLevelRemainCorrect() {
    PeripheralManager pm; TestQueue queue;
    configure(pm, queue, PeripheralType::GPIO_INTERRUPT_RISING);
    pm.peripherals["input"].params.gpio.invertLogic = true;
    queue.events.push_back({7, 0}); queue.events.push_back({7, 0});
    pm.processInterruptQueue();
    assert(queue.events.empty() && queue.receives == 2 && callbacks == 1);
    assert(pm._digitalInterruptCount == 1);
    assert(pm._digitalInterruptEvents[0].state == GPIOState::STATE_HIGH);
    nowMs += 6; queue.events.push_back({7, 1});
    pm.processInterruptQueue();
    assert(callbacks == 2 && pm._digitalInterruptCount == 1);
    assert(pm.runtimeStates["input"].state.gpio.currentState == GPIOState::STATE_LOW);
}
int main() {
    continuouslyRefilledQueue();
    timeBudgetAndClockWrap();
    slowConfigurationWriterPreservesEvents();
    debounceAndLogicalLevelRemainCorrect();
    PeripheralManager::_isrQueue = nullptr;
}
