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

// 脚本类任务栈大小（含 JSON 解析 / Modbus Poll）
// MQTT publish 已改为 queueReportData 队列化，不再在任务栈上执行 TCP/IP 发送
// Poll 路径已改用 Serial.printf 替代 LOGGER（避免 LittleFS flush 文件 I/O 加剧栈压力）
// 20480 覆盖 Modbus 读取 + JSON 解析 + 多层嵌套调用链
#ifndef SCRIPT_TASK_STACK
#define SCRIPT_TASK_STACK     20480
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
#define MIN_HEAP_FOR_ASYNC    24576
#endif

// ========== 异步执行状态 ==========

enum class AsyncExecStatus : uint8_t {
    PENDING   = 0,
    RUNNING   = 1,
    COMPLETED = 2,
    FAILED    = 3,
    CANCELLED = 4,
    REJECTED  = 5,
    TIMED_OUT = 6
};

enum class AsyncExecError : uint8_t {
    NONE = 0,
    LOW_MEMORY,
    DEADLINE_EXCEEDED,
    ACTION_FAILED,
    INCOMPLETE_ACTIONS,
    CANCELLED_BY_SYSTEM,
    WORKER_QUEUE_FULL
};

// Results are intentionally fixed-size. A rule contains at most four actions;
// keeping the trace inline prevents long-running devices from fragmenting the
// heap when recent operation history is updated.
struct AsyncExecActionTrace {
    static constexpr size_t TARGET_CAPACITY = 41;
    static constexpr size_t VALUE_CAPACITY = 65;
    static constexpr size_t REMARK_CAPACITY = 49;

    uint8_t       index = 0;
    uint8_t       actionType = 0;
    bool          success = false;
    unsigned long durationMs = 0;
    uint8_t       detailCount = 0;
    char          targetPeriphId[TARGET_CAPACITY] = {};
    char          valueSummary[VALUE_CAPACITY] = {};
    char          remark[REMARK_CAPACITY] = {};
};

static constexpr size_t ASYNC_EXEC_ACTION_TRACE_LIMIT = MAX_ACTIONS_PER_RULE;
static constexpr size_t ASYNC_EXEC_RESULT_HISTORY_LIMIT = MAX_ASYNC_TASKS * 2;
static constexpr unsigned long ASYNC_EXEC_OPERATION_TIMEOUT_MS = 30000UL;

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
    uint32_t         operationId = 0;
    unsigned long    queuedAt = 0;
    unsigned long    deadlineAt = 0;
    uint8_t          queuePosition = 0;
};

// ========== 异步执行结果 ==========

struct AsyncExecResult {
    uint32_t         operationId = 0;
    String           ruleId;
    String           ruleName;
    AsyncExecStatus  status = AsyncExecStatus::PENDING;
    AsyncExecError   errorCode = AsyncExecError::NONE;
    unsigned long    queuedAt = 0;
    unsigned long    startTime = 0;
    unsigned long    endTime = 0;
    unsigned long    deadlineAt = 0;
    uint8_t          queuePosition = 0;
    bool             asyncExecution = true;
    // 本次执行实际上报物联网平台的动作结果数量（reportAfterExec 勾选且产生可上报结果时 > 0）。
    // 用于回归验证：GPIO/PWM/DAC、NeoPixel、Modbus 写等属性控制动作
    // 勾选“上报数据”后确实生成上报。
    uint8_t          reportCount = 0;
    uint8_t          actionCount = 0;
    uint8_t          actionTraceCount = 0;
    bool             actionTraceTruncated = false;
    AsyncExecActionTrace actionTraces[ASYNC_EXEC_ACTION_TRACE_LIMIT];
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

    AsyncExecContextPool() : _mutex(nullptr), _exhaustCount(0) {
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
        // 池内临界区只有固定数组扫描和一次对象重置。这里必须等待锁，
        // 否则把短暂锁竞争误报为池耗尽会造成规则被无故拒绝。
        MutexGuard lock(_mutex);
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
        _exhaustCount++;
        return nullptr;  // 池满
    }

    // 归还上下文
    void release(AsyncExecContext* ctx) {
        if (!ctx) return;

        // release 是所有权归还路径，不能因超时静默放弃，否则固定池槽会
        // 永久泄漏并在长期运行后耗尽。临界区固定且极短，可靠等待锁。
        MutexGuard lock(_mutex);
        if (!lock.isLocked()) {
            return;
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
        MutexGuard lock(_mutex);
        if (!lock.isLocked()) return 0;
        size_t count = 0;
        for (size_t i = 0; i < POOL_SIZE; i++) {
            if (_inUse[i]) count++;
        }
        return count;
    }

    // 获取累计耗尽次数
    uint32_t exhaustCount() const {
        MutexGuard lock(_mutex);
        return lock.isLocked() ? _exhaustCount : 0;
    }

    // 获取池大小
    static constexpr size_t poolSize() { return POOL_SIZE; }

private:
    AsyncExecContext _pool[POOL_SIZE];
    bool _inUse[POOL_SIZE];
    mutable SemaphoreHandle_t _mutex;
    uint32_t _exhaustCount;  // 累计耗尽次数（acquire 返回 nullptr）
};

#endif // ASYNC_EXEC_TYPES_H
