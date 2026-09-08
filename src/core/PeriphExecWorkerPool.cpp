#include "core/PeriphExecWorkerPool.h"
#include "core/PeriphExecManager.h"
#include "systems/LoggerSystem.h"

PeriphExecWorkerPool::PeriphExecWorkerPool()
    : _queue(nullptr), _started(false), _shuttingDown(false),
      _exitSemaphore(nullptr), _workersCreated(0) {
    for (size_t i = 0; i < WORKER_COUNT; ++i) {
        _workers[i] = nullptr;
    }
}

PeriphExecWorkerPool::~PeriphExecWorkerPool() {
    shutdown();
}

bool PeriphExecWorkerPool::begin() {
    if (_started) {
        return true;
    }

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

    // 创建 N 个常驻 worker 任务
    char taskName[16];
    for (size_t i = 0; i < WORKER_COUNT; ++i) {
        snprintf(taskName, sizeof(taskName), "pexec_w%u", (unsigned)i);

        BaseType_t created =
#if CHIP_DUAL_CORE
            xTaskCreatePinnedToCore(
                workerLoopThunk,
                taskName,
                WORKER_STACK,
                this,
                ASYNC_TASK_PRIORITY,
                &_workers[i],
                1                       // Core 1，与原 dispatchAsync 一致
            );
#else
            xTaskCreate(
                workerLoopThunk,
                taskName,
                WORKER_STACK,
                this,
                ASYNC_TASK_PRIORITY,
                &_workers[i]
            );
#endif

        if (created != pdPASS) {
            LOGGER.errorf("[WorkerPool] Failed to create worker #%u (heap=%d, largest=%d)",
                          (unsigned)i,
                          (int)ESP.getFreeHeap(),
                          (int)ESP.getMaxAllocHeap());
            // 清理已创建的部分 worker（投递哨兵并等待退出）
            _shuttingDown = true;
            AsyncExecContext* sentinel = nullptr;
            for (size_t j = 0; j < i; ++j) {
                xQueueSend(_queue, &sentinel, 0);
            }
            for (size_t j = 0; j < i; ++j) {
                xSemaphoreTake(_exitSemaphore, pdMS_TO_TICKS(2000));
            }
            _workersCreated = 0;
            vQueueDelete(_queue);
            _queue = nullptr;
            vSemaphoreDelete(_exitSemaphore);
            _exitSemaphore = nullptr;
            return false;
        }
        _workersCreated++;
    }

    _started = true;
    LOGGER.infof("[WorkerPool] Started %u workers, stack=%u, queue=%u (heap=%d, largest=%d)",
                 (unsigned)WORKER_COUNT,
                 (unsigned)WORKER_STACK,
                 (unsigned)QUEUE_CAPACITY,
                 (int)ESP.getFreeHeap(),
                 (int)ESP.getMaxAllocHeap());
    return true;
}

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

void PeriphExecWorkerPool::shutdown() {
    if (!_started && !_queue && _workersCreated == 0) {
        return;
    }
    _shuttingDown = true;

    // 投递 N 个哨兵唤醒 worker 退出
    if (_queue) {
        AsyncExecContext* sentinel = nullptr;
        for (size_t i = 0; i < WORKER_COUNT; ++i) {
            xQueueSend(_queue, &sentinel, pdMS_TO_TICKS(100));
        }
    }

    _started = false;
    for (size_t i = 0; i < _workersCreated; ++i) {
        if (_exitSemaphore && xSemaphoreTake(_exitSemaphore, pdMS_TO_TICKS(3000)) != pdTRUE) {
            LOGGER.errorf("[WorkerPool] Worker #%u did not exit before timeout", (unsigned)i);
        }
    }
    if (_queue) vQueueDelete(_queue);
    _queue = nullptr;
    if (_exitSemaphore) vSemaphoreDelete(_exitSemaphore);
    _exitSemaphore = nullptr;
    _workersCreated = 0;
    for (size_t i = 0; i < WORKER_COUNT; ++i) {
        _workers[i] = nullptr;
    }
    _queue = nullptr;
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
