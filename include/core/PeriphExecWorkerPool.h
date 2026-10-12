#ifndef PERIPH_EXEC_WORKER_POOL_H
#define PERIPH_EXEC_WORKER_POOL_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "AsyncExecTypes.h"
#include "ChipConfig.h"
#if CHIP_IS_ESP32S3 && defined(BOARD_HAS_PSRAM) && FASTBEE_USE_PSRAM
#define FASTBEE_WORKER_BUDGETED 1
#include "WorkerPoolBudget.h"
#else
#define FASTBEE_WORKER_BUDGETED 0
#endif

// ========== 常驻 Worker Task Pool（D 方案）==========
//
// 背景：原 dispatchAsync 每次触发都 xTaskCreate 创建临时任务，
//      运行期不断申请 ~20KB 连续栈，碰到 largestFreeBlock < 8KB
//      就随机失败，按键/定时事件被无声丢弃。
//
// 方案：启动期一次性预创建 WORKER_COUNT 个常驻 worker 任务，
//      运行期只投递 AsyncExecContext* 到队列，消除栈分配抖动。
//
// 参数：C3/经典 ESP32 使用一个 worker，C6/S3 使用两个 worker。
// 每个栈仍由 SDK 在内部 8BIT DRAM 中分配，PSRAM 不能代替该预算。
// 2026-10-07 实机：COM4 在 6144 字节栈上自然采集仅余 164 字节；
// COM15 在 7168 字节栈上出现 pexec_w0/w1 canary。提高本次候选下限，
// 保留更大的 SIMPLE_TASK_STACK；实际深层调用和稳定余量仍须实机验证。
// QUEUE_CAPACITY = 16；启动/恢复准入按 WORKER_STACK * WORKER_COUNT
// 及连续块检查内部 DRAM，未修改 Web 预留或运行期保护门槛。
//

class PeriphExecWorkerPool {
public:
    static constexpr size_t   WORKER_COUNT    =
        (CHIP_IS_CLASSIC_ESP32 || CHIP_IS_ESP32C3) ? 1 : 2;
    static constexpr size_t   QUEUE_CAPACITY  = 16;
    // 按编译配置保留栈下限，PSRAM 初始化失败时也不能缩小执行栈。
    // SDK 的 worker 栈实际占用内部 RAM，准入会计入增加的全部成本。
#if defined(BOARD_HAS_PSRAM) && FASTBEE_USE_PSRAM
    static constexpr uint32_t WORKER_STACK    =
        (SIMPLE_TASK_STACK < 12288) ? 12288 : SIMPLE_TASK_STACK;
#else
    static constexpr uint32_t WORKER_STACK    =
        (SIMPLE_TASK_STACK < 8192) ? 8192 : SIMPLE_TASK_STACK;
#endif

    PeriphExecWorkerPool();
    ~PeriphExecWorkerPool();

    // 禁止拷贝
    PeriphExecWorkerPool(const PeriphExecWorkerPool&) = delete;
    PeriphExecWorkerPool& operator=(const PeriphExecWorkerPool&) = delete;

    // 启动 worker 任务池（在 PeriphExecManager::initialize() 内调用）
    // false 时调用方须确认 shutdown()，未退出 worker 的 pool 必须保活。
    bool begin(size_t initialCount = WORKER_COUNT);
#if FASTBEE_WORKER_BUDGETED
    // Caller serializes management via the existing rules mutex. One added
    // worker stays resident until ordinary pool shutdown; no temporary tasks.
    bool tryGrow(bool concurrencyDemand, bool baselineReady);
#endif
    size_t workerCount() const { return _workersCreated; }
    bool hasLiveResources() const { return _queue || _workersCreated; }

    // 投递一个执行上下文到队列
    // ctx 所有权移交 worker pool（由 worker 在 executeOneJob 末尾归还到对象池）
    // 返回 false 表示队列满或未启动，调用方负责清理 ctx 与释放信号量/退避
    bool enqueue(AsyncExecContext* ctx);

    // 关闭 pool（投递哨兵 + 等待任务退出）。只有返回 true 后，调用方
    // 才可以释放 pool 对象；false 表示仍有 worker 在执行，队列和对象
    // 必须保持存活并在后续周期重试。
    bool shutdown(uint32_t timeoutMs = 10000);

    // 是否已启动
    bool isStarted() const { return _started; }

    // 当前队列待执行项数（诊断用）
    size_t pendingCount() const;

    // 队列容量
    static constexpr size_t capacity() { return QUEUE_CAPACITY; }

private:
    bool createWorker(size_t index);
    // FreeRTOS 任务入口（trampoline）
    static void workerLoopThunk(void* arg);

    // worker 主循环：阻塞接收 ctx → executeOneJob → 循环
    void workerLoop();

    // 执行单个任务（业务体，等价于原 asyncExecTaskFunc 的核心逻辑）
    static void executeOneJob(AsyncExecContext* ctx);

    QueueHandle_t  _queue;
    TaskHandle_t   _workers[WORKER_COUNT];
    volatile bool  _started;
    volatile bool  _shuttingDown;
    SemaphoreHandle_t _exitSemaphore;
    size_t _workersCreated;
    size_t _workersExited; // Shutdown owner consumes one exit ticket per worker.
    size_t _sentinelsQueued;
#if FASTBEE_WORKER_BUDGETED
    uint32_t _lastGrowthAttemptMs = 0;
    bool _growthAttempted = false;
#endif
};

#endif // PERIPH_EXEC_WORKER_POOL_H
