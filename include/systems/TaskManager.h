/**
 * @file TaskManager.h
 * @brief 任务调度管理器头文件
 * @author kerwincui
 * @date 2025-12-02
 */

#ifndef TASK_MANAGER_H
#define TASK_MANAGER_H

#include <Arduino.h>
#include <vector>
#include <array>
#include <functional>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// 任务函数类型定义
#include "core/interfaces/ITaskManager.h"
#include "core/SystemConstants.h"

// 任务优先级枚举
enum class TaskPriority {
    PRIORITY_LOW = 0,
    PRIORITY_NORMAL = 1,
    PRIORITY_HIGH = 2,
    PRIORITY_CRITICAL = 3
};

// 任务统计结构
struct TaskStatistics {
    String name;
    TaskPriority priority;         // 任务优先级
    uint32_t executionCount;
    unsigned long lastExecutionTime; // 上次执行耗时
    unsigned long maxExecutionTime; // 最大执行耗时
    unsigned long executionBudget;  // 单次执行预算
    uint32_t overrunCount;           // 累计超预算次数
    uint32_t deferredCount;          // 因调度轮预算延后的次数
    uint8_t consecutiveOverruns;     // 连续超预算次数
    unsigned long createdTime;
    unsigned long uptime;
};

// 任务结构定义
struct ScheduledTask {
    char name[32];                 // 任务名称（固定长度，减少内存碎片）
    TaskFunction function;         // 任务函数
    void* parameter;               // 任务参数
    unsigned long interval;        // 执行间隔（毫秒）
    unsigned long lastRun;         // 上次执行时间
    bool enabled;                  // 是否启用
    TaskPriority priority;         // 任务优先级
    unsigned long createdTime;     // 创建时间
    unsigned long executionCount;  // 执行次数
    unsigned long lastExecutionTime; // 上次执行耗时（毫秒）
    unsigned long maxExecutionTime; // 最大执行耗时（毫秒）
    unsigned long totalExecutionTime; // 累计执行时间，用于计算平均值
    unsigned long executionBudget;    // 单次执行软超时预算（毫秒）
    uint32_t overrunCount;             // 累计超预算次数
    uint32_t deferredCount;            // 调度轮预算耗尽后的延后次数
    uint8_t consecutiveOverruns;       // 连续超预算次数
};

class TaskManager : public ITaskManager {
public:
    TaskManager();
    ~TaskManager();
    
    bool initialize();
    
    // 任务管理
    bool addTask(const String& name, TaskFunction func, void* param, 
                unsigned long interval, bool enabled = true) override;
    bool addTask(const String& name, TaskFunction func, void* param, 
                unsigned long interval, TaskPriority priority,
                bool enabled = true);
    bool removeTask(const String& name);
    bool enableTask(const String& name);
    bool disableTask(const String& name);
    bool taskExists(const String& name);
    
    // 任务执行控制
    void run();
    void stopAllTasks();               // 停止所有任务
    void restartAllTasks();            // 重新启动所有任务
    void pause();                      // 暂停任务管理器
    void resume();                     // 恢复任务管理器
    void clearAllTasks();              // 清空所有任务
    
    // 状态查询
    bool isRunningStatus() const;      // 获取运行状态
    size_t getActiveTaskCount() const; // 获取活动任务数
    
    // 任务信息
    void listTasks();
    String getTasksJSON();             // 获取JSON格式任务列表
    TaskStatistics getTaskStatistics(const String& name); // 获取任务统计
    
    // 任务配置更新
    bool updateTaskInterval(const String& name, unsigned long newInterval); // 更新任务间隔
    bool setTaskExecutionBudget(const String& name, unsigned long budgetMs);

private:
    // Keep only fields consumed by run(), not the full statistics record.
    // Resident storage avoids both loop-time heap allocation and a large
    // stack snapshot while callbacks run outside tasksMutex.
    struct DueTask {
        char name[32];
        TaskFunction function;
        void* parameter;
        TaskPriority priority;
        unsigned long executionBudget;
    };
    std::array<DueTask, TaskScheduler::MAX_TASKS> dueTasks;
    bool runInProgress = false; // protected by tasksMutex
    std::vector<ScheduledTask> tasks;
    bool isRunning;
    SemaphoreHandle_t tasksMutex = nullptr;
};

#endif
