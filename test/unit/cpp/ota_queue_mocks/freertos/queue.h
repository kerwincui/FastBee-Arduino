#pragma once
#include "FreeRTOS.h"
#include <atomic>
#include <functional>
#include <set>
struct MockQueue {
    unsigned capacity;
    size_t itemSize;
    std::atomic<unsigned> count{0};
};
using QueueHandle_t = MockQueue*;
inline std::mutex queueTrackingMux;
inline std::set<QueueHandle_t> liveQueues;
inline unsigned createCalls = 0, deleteCalls = 0, failQueueCall = 0;
inline std::function<void()> createHook, deleteHook;
inline QueueHandle_t xQueueCreate(unsigned capacity, size_t size) {
    assert(criticalDepth == 0);
    if (createHook) { auto hook = std::move(createHook); createHook = {}; hook(); }
    std::lock_guard<std::mutex> lock(queueTrackingMux);
    if (++createCalls == failQueueCall) return nullptr;
    auto queue = new MockQueue{capacity, size};
    assert(liveQueues.insert(queue).second);
    return queue;
}
inline void vQueueDelete(QueueHandle_t queue) {
    assert(criticalDepth == 0);
    if (deleteHook) { auto hook = std::move(deleteHook); deleteHook = {}; hook(); }
    std::lock_guard<std::mutex> lock(queueTrackingMux);
    assert(queue && liveQueues.erase(queue) == 1);
    ++deleteCalls;
    delete queue;
}
inline unsigned uxQueueMessagesWaiting(QueueHandle_t queue) {
    assert(queue);
    return queue->count.load();
}
