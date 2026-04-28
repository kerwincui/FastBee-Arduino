#ifndef ASYNC_EXEC_TYPES_H
#define ASYNC_EXEC_TYPES_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "PeripheralExecution.h"

// ========== 异步执行常量 ==========

// 最大并行异步任务数
#ifndef MAX_ASYNC_TASKS
#define MAX_ASYNC_TASKS       3
#endif

// 脚本类任务栈大小（含 MQTT / JSON 解析 / LoggerSystem 文件 I/O）
// 从 12288 增至 14336：透传模式下 publishReportData→LoggerSystem→fs::open 调用链深，需更大栈
#ifndef SCRIPT_TASK_STACK
#define SCRIPT_TASK_STACK     14336
#endif

// 简单外设动作任务栈大小（增加以避免 reportActionResults 中的 JSON/MQTT 操作导致栈溢出）
#ifndef SIMPLE_TASK_STACK
#define SIMPLE_TASK_STACK     8192
#endif

// 异步任务优先级（低于主循环 loopTask=1）
#ifndef ASYNC_TASK_PRIORITY
#define ASYNC_TASK_PRIORITY   0
#endif

// 最低可用堆内存阈值，低于此值回退同步执行（字节）
#ifndef MIN_HEAP_FOR_ASYNC
#define MIN_HEAP_FOR_ASYNC    30000
#endif

// ========== 异步执行状态 ==========

enum class AsyncExecStatus : uint8_t {
    PENDING   = 0,
    RUNNING   = 1,
    COMPLETED = 2,
    FAILED    = 3
};

// ========== 异步执行上下文 ==========
// 传递给 FreeRTOS 任务的参数结构体（堆分配，任务结束时自行释放）

class PeriphExecManager;   // 前向声明
class MQTTClient;          // 前向声明

struct AsyncExecContext {
    PeriphExecRule   ruleCopy;       // 规则的深拷贝（任务独立副本）
    String           receivedValue;  // 触发时捕获的值（用于 useReceivedValue 传递）
    PeriphExecManager* manager;      // 回指管理器（用于记录结果）
    MQTTClient*      mqtt;           // MQTT 客户端指针
    SemaphoreHandle_t taskSlot;      // 完成后归还的计数信号量
};

// ========== 异步执行结果 ==========

struct AsyncExecResult {
    String           ruleId;
    String           ruleName;
    AsyncExecStatus  status;
    unsigned long    startTime;
    unsigned long    endTime;
};

// ========== RAII Mutex 守护 ==========

class MutexGuard {
public:
    // 阻塞获取互斥量，timeout 为最大等待 tick 数
    explicit MutexGuard(SemaphoreHandle_t mutex, TickType_t timeout = portMAX_DELAY)
        : _mutex(mutex), _locked(false)
    {
        if (_mutex) {
            _locked = (xSemaphoreTake(_mutex, timeout) == pdTRUE);
        }
    }

    ~MutexGuard() {
        if (_locked && _mutex) {
            xSemaphoreGive(_mutex);
        }
    }

    // 是否成功获取锁
    bool isLocked() const { return _locked; }

    // 禁止拷贝
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

private:
    SemaphoreHandle_t _mutex;
    bool _locked;
};

// ========== RAII 递归 Mutex 守护 ==========

class RecursiveMutexGuard {
public:
    explicit RecursiveMutexGuard(SemaphoreHandle_t mutex, TickType_t timeout = portMAX_DELAY)
        : _mutex(mutex), _locked(false)
    {
        if (_mutex) {
            _locked = (xSemaphoreTakeRecursive(_mutex, timeout) == pdTRUE);
        }
    }

    ~RecursiveMutexGuard() {
        if (_locked && _mutex) {
            xSemaphoreGiveRecursive(_mutex);
        }
    }

    bool isLocked() const { return _locked; }

    RecursiveMutexGuard(const RecursiveMutexGuard&) = delete;
    RecursiveMutexGuard& operator=(const RecursiveMutexGuard&) = delete;

private:
    SemaphoreHandle_t _mutex;
    bool _locked;
};

// ========== 固定大小的 AsyncExecContext 对象池 ==========
// 消除频繁 new/delete 导致的内存碎片化

class AsyncExecContextPool {
public:
    static constexpr size_t POOL_SIZE = 4;  // 最多4个并发异步任务

    AsyncExecContextPool() : _mutex(nullptr) {
        for (size_t i = 0; i < POOL_SIZE; i++) {
            _inUse[i] = false;
        }
        // 创建互斥锁用于线程安全
        _mutex = xSemaphoreCreateMutex();
    }

    ~AsyncExecContextPool() {
        if (_mutex) {
            vSemaphoreDelete(_mutex);
            _mutex = nullptr;
        }
    }

    // 禁止拷贝
    AsyncExecContextPool(const AsyncExecContextPool&) = delete;
    AsyncExecContextPool& operator=(const AsyncExecContextPool&) = delete;

    // 获取一个空闲的上下文（返回 nullptr 如果池满）
    AsyncExecContext* acquire() {
        MutexGuard lock(_mutex, pdMS_TO_TICKS(100));
        if (!lock.isLocked()) {
            return nullptr;  // 无法获取锁
        }

        for (size_t i = 0; i < POOL_SIZE; i++) {
            if (!_inUse[i]) {
                _inUse[i] = true;
                _pool[i] = AsyncExecContext();  // 重置为默认状态
                return &_pool[i];
            }
        }
        return nullptr;  // 池满
    }

    // 归还上下文
    void release(AsyncExecContext* ctx) {
        if (!ctx) return;

        MutexGuard lock(_mutex, pdMS_TO_TICKS(100));
        if (!lock.isLocked()) {
            return;  // 无法获取锁，但继续执行释放逻辑（避免内存泄漏）
        }

        for (size_t i = 0; i < POOL_SIZE; i++) {
            if (&_pool[i] == ctx) {
                _inUse[i] = false;
                return;
            }
        }
    }

    // 获取当前使用数
    size_t usedCount() const {
        size_t count = 0;
        for (size_t i = 0; i < POOL_SIZE; i++) {
            if (_inUse[i]) count++;
        }
        return count;
    }

    // 获取池大小
    static constexpr size_t poolSize() { return POOL_SIZE; }

private:
    AsyncExecContext _pool[POOL_SIZE];
    bool _inUse[POOL_SIZE];
    SemaphoreHandle_t _mutex;
};

#endif // ASYNC_EXEC_TYPES_H
