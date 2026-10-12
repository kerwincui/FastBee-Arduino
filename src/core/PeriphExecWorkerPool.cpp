#include "core/PeriphExecWorkerPool.h"
#include "core/PeriphExecManager.h"
#include "systems/LoggerSystem.h"
#if FASTBEE_WORKER_BUDGETED
#include "core/MemoryBudget.h"
#endif

PeriphExecWorkerPool::PeriphExecWorkerPool()
    : _queue(nullptr), _started(false), _shuttingDown(false),
      _exitSemaphore(nullptr), _workersCreated(0), _workersExited(0),
      _sentinelsQueued(0) {
    for (size_t i = 0; i < WORKER_COUNT; ++i) {
        _workers[i] = nullptr;
    }
}

PeriphExecWorkerPool::~PeriphExecWorkerPool() {
    shutdown();
}

bool PeriphExecWorkerPool::begin(size_t initialCount) {
    if (_started) {
        return true;
    }

    if (_queue || _workersCreated || initialCount == 0 || initialCount > WORKER_COUNT) return false;
    // 创建队列：存储 AsyncExecContext* 指针
    _queue = xQueueCreate(QUEUE_CAPACITY, sizeof(AsyncExecContext*));
    if (!_queue) {
        LOGGER.error("[WorkerPool] Failed to create queue");
        return false;
    }
    _exitSemaphore = xSemaphoreCreateCounting(WORKER_COUNT, 0);
    if (!_exitSemaphore) {
        vQueueDelete(_queue);
        _queue = nullptr;
        LOGGER.error("[WorkerPool] Failed to create exit semaphore");
        return false;
    }

    _workersExited = 0;
    _sentinelsQueued = 0;
    _shuttingDown = false;

    for (size_t i = 0; i < initialCount; ++i) {
        if (!createWorker(i)) {
            // shutdown uses actual created/exited counts. A timeout leaves
            // ownership intact; manager must retain this candidate for retry.
            shutdown(2000);
            return false;
        }
    }

    _started = true;
    LOGGER.infof("[WorkerPool] Started %u workers, stack=%u, queue=%u (heap=%d, largest=%d)",
                 (unsigned)_workersCreated,
                 (unsigned)WORKER_STACK,
                 (unsigned)QUEUE_CAPACITY,
                 (int)ESP.getFreeHeap(),
                 (int)ESP.getMaxAllocHeap());
    return true;
}

bool PeriphExecWorkerPool::createWorker(size_t index) {
    char taskName[16];
    snprintf(taskName, sizeof(taskName), "pexec_w%u", (unsigned)index);
        BaseType_t created =
#if CHIP_DUAL_CORE
            xTaskCreatePinnedToCore(
                workerLoopThunk,
                taskName,
                WORKER_STACK,
                this,
                ASYNC_TASK_PRIORITY,
                &_workers[index],
                1                       // Core 1，与原 dispatchAsync 一致
            );
#else
            xTaskCreate(
                workerLoopThunk,
                taskName,
                WORKER_STACK,
                this,
                ASYNC_TASK_PRIORITY,
                &_workers[index]
            );
#endif

    if (created != pdPASS) {
        LOGGER.errorf("[WorkerPool] Failed to create worker #%u (heap=%d, largest=%d)",
                      (unsigned)index, (int)ESP.getFreeHeap(), (int)ESP.getMaxAllocHeap());
        return false;
    }
    ++_workersCreated;
    return true;
}

#if FASTBEE_WORKER_BUDGETED
bool PeriphExecWorkerPool::tryGrow(bool concurrencyDemand, bool baselineReady) {
    if (!_started || _shuttingDown || !concurrencyDemand) return false;
    if (!FastBee::WorkerPoolBudget::canAdd(
            FastBee::MemoryBudget::dram8Free(), FastBee::MemoryBudget::dram8Largest(),
            WORKER_STACK, baselineReady, _workersCreated, WORKER_COUNT, concurrencyDemand)) return false;
    const uint32_t now = static_cast<uint32_t>(millis());
    if (_growthAttempted && static_cast<uint32_t>(now - _lastGrowthAttemptMs) < 10000U) return false;
    _growthAttempted = true;
    _lastGrowthAttemptMs = now;
    // Failed growth leaves the admitted first worker and queued jobs intact.
    return createWorker(_workersCreated);
}
#endif

bool PeriphExecWorkerPool::enqueue(AsyncExecContext* ctx) {
    if (!_started || !_queue || !ctx || _shuttingDown) {
        return false;
    }
    // 非阻塞投递：队列满直接返回 false，由调用方处理回退
    if (xQueueSend(_queue, &ctx, 0) != pdTRUE) {
        LOGGER.warningf("[WorkerPool] Queue full (cap=%u), reject job: '%s'",
                        (unsigned)QUEUE_CAPACITY,
                        ctx->ruleCopy.name.c_str());
        return false;
    }
    return true;
}

bool PeriphExecWorkerPool::shutdown(uint32_t timeoutMs) {
    if (!_started && !_queue && _workersCreated == 0) {
        return true;
    }
    _shuttingDown = true;

    // 每个 worker 只需一个哨兵。重复调用 shutdown() 时不要无限追加
    // 哨兵，否则第一次超时留下的 semaphore 计数会掩盖仍在运行的 worker。
    if (_queue && _sentinelsQueued < _workersCreated) {
        // Jobs still waiting in the queue own a task slot and a context-pool
        // entry. Cancel them before inserting sentinels; otherwise a memory
        // pressure pause can permanently exhaust the async pool even though
        // no worker will ever execute those jobs.
        AsyncExecContext* pending = nullptr;
        while (_sentinelsQueued == 0 && xQueueReceive(_queue, &pending, 0) == pdTRUE) {
            if (pending) {
                if (pending->manager) {
                    pending->manager->cancelWorkerJob(pending);
                } else if (pending->taskSlot) {
                    xSemaphoreGive(pending->taskSlot);
                }
            }
            pending = nullptr;
        }

        AsyncExecContext* sentinel = nullptr;
        for (size_t i = _sentinelsQueued; i < _workersCreated; ++i) {
            if (xQueueSend(_queue, &sentinel, pdMS_TO_TICKS(1000)) != pdTRUE) {
                LOGGER.errorf("[WorkerPool] Failed to queue shutdown sentinel #%u",
                              (unsigned)i);
                // Do not claim that all sentinels were queued. A later retry
                // can finish the request after workers free queue capacity.
                break;
            }
            ++_sentinelsQueued;
        }
    }

    _started = false;
    const size_t target = _workersCreated;
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeoutTicks = pdMS_TO_TICKS(timeoutMs);
    while (_workersExited < target) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeoutTicks) break;
        TickType_t remaining = timeoutTicks - elapsed;
        // The semaphore is only a wake-up; _workersExited is the authoritative
        // consumed count, so tickets from an earlier retry cannot make us
        // free the queue while a worker is still using it.
        if (_exitSemaphore) {
            if (xSemaphoreTake(_exitSemaphore, remaining > pdMS_TO_TICKS(100)
                                               ? pdMS_TO_TICKS(100) : remaining) == pdTRUE) {
                ++_workersExited;
            }
        } else {
            vTaskDelay(remaining > pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : remaining);
        }
    }
    const bool allExited = (_workersExited >= target);
    // Never delete a queue that a still-running worker may touch.  Keeping
    // the objects alive is safer than turning a long-running rule into a
    // use-after-free during memory recovery; the next health sample can retry.
    if (!allExited) {
        _started = false;
        LOGGER.errorf("[WorkerPool] %u worker(s) did not exit before timeout; keeping pool alive",
                      (unsigned)(target - _workersExited));
        return false;
    }
    if (_queue) vQueueDelete(_queue);
    _queue = nullptr;
    if (_exitSemaphore) vSemaphoreDelete(_exitSemaphore);
    _exitSemaphore = nullptr;
    _workersCreated = 0;
    _workersExited = 0;
    _sentinelsQueued = 0;
    _shuttingDown = false;
    for (size_t i = 0; i < WORKER_COUNT; ++i) {
        _workers[i] = nullptr;
    }
    _queue = nullptr;
    return true;
}

size_t PeriphExecWorkerPool::pendingCount() const {
    if (!_queue) return 0;
    return (size_t)uxQueueMessagesWaiting(_queue);
}

// ========== Worker 主循环 ==========

void PeriphExecWorkerPool::workerLoopThunk(void* arg) {
    auto* self = static_cast<PeriphExecWorkerPool*>(arg);
    if (self) {
        self->workerLoop();
        // This unique ticket is the worker's last access to pool storage.
        // The shutdown owner alone updates exited count; no volatile RMW race.
        if (self->_exitSemaphore) xSemaphoreGive(self->_exitSemaphore);
    }
    vTaskDelete(nullptr);
}

void PeriphExecWorkerPool::workerLoop() {
    AsyncExecContext* ctx = nullptr;
    while (true) {
        // 阻塞等待新任务
        if (xQueueReceive(_queue, &ctx, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        // nullptr 哨兵 → 退出 worker
        if (ctx == nullptr) {
            break;
        }
        // 执行业务体
        executeOneJob(ctx);
        ctx = nullptr;
    }
}

// ========== 单任务执行（等价原 asyncExecTaskFunc 业务体）==========
// 注意：这里完全复用 PeriphExecManager 的现有方法（executeWorkerJob），
//      所有清理逻辑（_runningRuleIds 移除、信号量归还、对象池归还、
//      dispatchPeriphExecEvent、栈高水位日志）都在 manager 侧完成，
//      WorkerPool 只负责调度。
void PeriphExecWorkerPool::executeOneJob(AsyncExecContext* ctx) {
    if (!ctx) return;
    if (!ctx->manager) {
        // 上下文异常，直接释放信号量避免泄漏（无法访问对象池）
        if (ctx->taskSlot) {
            xSemaphoreGive(ctx->taskSlot);
        }
        return;
    }
    ctx->manager->executeWorkerJob(ctx);
}
