#ifndef PERIPH_EXEC_WORKER_POOL_H
#define PERIPH_EXEC_WORKER_POOL_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
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
//   WORKER_COUNT  = 2   符合实测并发峰值，对齐 AsyncExecContextPool=4 的边界
//   WORKER_STACK  = SIMPLE_TASK_STACK (6144) 实测 HWM 仅用 4036B，留 ~2KB 余量
//   QUEUE_CAPACITY= 16  突发缓冲（满则走原失败回退分支）
//
// 启动期一次性付出：2 × 6144 = 12KB 永久驻留栈（原 3×8192=24KB）
// 运行期增长：0
//

class PeriphExecWorkerPool {
public:
    static constexpr size_t   WORKER_COUNT    = 2;                     // 目标并发 2，与 AsyncExecContextPool 对齐
    static constexpr size_t   QUEUE_CAPACITY  = 16;
    static constexpr uint32_t WORKER_STACK    = SIMPLE_TASK_STACK;     // 6144 （platformio.ini 为各环境已统一）

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

    // 关闭 pool（投递哨兵 + 任务自删除），实际项目跑到 reboot 通常不需要
    void shutdown();

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
};

#endif // PERIPH_EXEC_WORKER_POOL_H
