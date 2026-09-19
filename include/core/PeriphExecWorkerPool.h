#ifndef PERIPH_EXEC_WORKER_POOL_H
#define PERIPH_EXEC_WORKER_POOL_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "AsyncExecTypes.h"
#include "ChipConfig.h"

// ========== 常驻 Worker Task Pool（D 方案）==========
//
// 背景：原 dispatchAsync 每次触发都 xTaskCreate 创建临时任务，
//      运行期不断申请 ~20KB 连续栈，碰到 largestFreeBlock < 8KB
//      就随机失败，按键/定时事件被无声丢弃。
//
// 方案：启动期一次性预创建 WORKER_COUNT 个常驻 worker 任务，
//      运行期只投递 AsyncExecContext* 到队列，消除栈分配抖动。
//
// 关键参数（经初版 3×8192 在线验证后下调以保留充足空闲 heap）：
//   WORKER_COUNT  = 1   ESP32-C3/经典 ESP32，优先保留 Web/认证连续内存
//                 = 2   ESP32-C6/S3，保留原有并发能力
//   WORKER_STACK  = max(SIMPLE_TASK_STACK, 7168) for PSRAM builds, otherwise 6144
//     注意：SIMPLE_TASK_STACK 在 C3/C6 环境为 4096（platformio.ini），
//     实测传感器采集+上报规则在 4096 栈上 HWM 仅剩 ~948B，频繁触发
//     （如高频开关灯 + 定时采集并发）时更深路径（DHT 重试/断连重试队列/
//     日志格式化）会击穿栈 canary 导致 panic 重启，故此处强制 6144 下限。
//   QUEUE_CAPACITY= 16  突发缓冲（满则走原失败回退分支）
//
// 启动期：C3/经典无 PSRAM 为 6KB，C6/无 PSRAM S3 为 12KB，PSRAM S3 为 14KB
// 运行期增长：0
//

class PeriphExecWorkerPool {
public:
    static constexpr size_t   WORKER_COUNT    =
        (CHIP_IS_CLASSIC_ESP32 || CHIP_IS_ESP32C3) ? 1 : 2;
    static constexpr size_t   QUEUE_CAPACITY  = 16;
    // 强制最小 6144：C3/C6 的 SIMPLE_TASK_STACK=4096 不足以覆盖
    // 传感器采集+JSON 上报路径（实测 HWM 剩 948B，濒临栈溢出重启）
#if defined(BOARD_HAS_PSRAM) && FASTBEE_USE_PSRAM
    // S3 panel + Modbus retries measured only 48 bytes free with 6144.
    // Flattening the batch table saves 336 bytes, still below the 512-byte
    // critical threshold. Add 1024 only to PSRAM build profiles (2KB for S3).
    // Keep this floor if PSRAM fails at runtime: shrinking a live execution
    // path is unsafe. The existing DRAM admission gate includes WORKER_STACK
    // and can decline worker creation if the configured stack cannot fit.
    static constexpr uint32_t WORKER_STACK    =
        (SIMPLE_TASK_STACK < 7168) ? 7168 : SIMPLE_TASK_STACK;
#else
    static constexpr uint32_t WORKER_STACK    =
        (SIMPLE_TASK_STACK < 6144) ? 6144 : SIMPLE_TASK_STACK;
#endif

    PeriphExecWorkerPool();
    ~PeriphExecWorkerPool();

    // 禁止拷贝
    PeriphExecWorkerPool(const PeriphExecWorkerPool&) = delete;
    PeriphExecWorkerPool& operator=(const PeriphExecWorkerPool&) = delete;

    // 启动 worker 任务池（在 PeriphExecManager::initialize() 内调用）
    // 返回 false 表示 worker 创建失败，调用方应回退老路径
    bool begin();

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
    volatile size_t _workersExited;
    bool _sentinelsSent;
};

#endif // PERIPH_EXEC_WORKER_POOL_H
