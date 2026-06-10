/**
 * @file MockTaskManager.h
 * @brief 任务调度模拟对象
 * 
 * 提供任务调度功能的模拟实现，支持任务CRUD、优先级调度、统计等
 */

#ifndef MOCK_TASK_MANAGER_H
#define MOCK_TASK_MANAGER_H

#include <Arduino.h>
#include <vector>
#include <map>
#include <functional>
#include <queue>

// 任务优先级枚举
enum class TaskPriority {
    PRIORITY_IDLE = 0,      // 空闲时执行
    PRIORITY_LOW = 1,       // 低优先级
    PRIORITY_NORMAL = 2,    // 普通优先级
    PRIORITY_HIGH = 3,      // 高优先级
    PRIORITY_CRITICAL = 4   // 关键优先级
};

// 任务状态枚举
enum class TaskState {
    TASK_IDLE = 0,          // 空闲
    TASK_RUNNING = 1,       // 运行中
    TASK_PAUSED = 2,        // 暂停
    TASK_STOPPED = 3,       // 停止
    TASK_ERROR = 4          // 错误
};

// 任务统计结构
struct TaskStatistics {
    String taskId;
    int executionCount;
    unsigned long totalExecutionTime;
    unsigned long lastExecutionTime;
    unsigned long minExecutionTime;
    unsigned long maxExecutionTime;
    time_t lastExecution;
    int errorCount;
    
    TaskStatistics() : executionCount(0), totalExecutionTime(0),
                       lastExecutionTime(0), minExecutionTime(ULONG_MAX),
                       maxExecutionTime(0), lastExecution(0), errorCount(0) {}
    
    unsigned long getAverageExecutionTime() {
        if (executionCount == 0) return 0;
        return totalExecutionTime / executionCount;
    }
};

// 任务结构
typedef std::function<void(void*)> TaskFunction;

struct Task {
    String id;
    String name;
    TaskFunction function;
    void* parameter;
    unsigned long interval;      // 执行间隔（毫秒）
    TaskPriority priority;
    TaskState state;
    bool enabled;
    unsigned long lastRun;       // 上次执行时间
    unsigned long nextRun;       // 下次执行时间
    int maxExecutions;           // 最大执行次数（0=无限）
    bool runOnce;                // 只执行一次
    
    Task() : parameter(nullptr), interval(1000), 
             priority(TaskPriority::PRIORITY_NORMAL),
             state(TaskState::TASK_IDLE), enabled(true),
             lastRun(0), nextRun(0), maxExecutions(0), runOnce(false) {}
};

// 任务比较器（用于优先级队列）
struct TaskComparator {
    bool operator()(const Task* a, const Task* b) {
        // 优先级高的先执行
        if (a->priority != b->priority) {
            return (int)a->priority < (int)b->priority;
        }
        // 相同优先级，先到期先执行
        return a->nextRun > b->nextRun;
    }
};

// 模拟任务管理器
class MockTaskManager {
public:
    static MockTaskManager& getInstance() {
        static MockTaskManager instance;
        return instance;
    }

    bool initialize() {
        _tasks.clear();
        _statistics.clear();
        _initialized = true;
        _currentTime = 0;
        return true;
    }

    // 任务CRUD
    bool addTask(const String& id, TaskFunction function, void* parameter,
                 unsigned long interval, TaskPriority priority = TaskPriority::PRIORITY_NORMAL) {
        if (id.isEmpty()) return false;
        if (_tasks.find(id) != _tasks.end()) return false;
        
        Task task;
        task.id = id;
        task.name = id;
        task.function = function;
        task.parameter = parameter;
        task.interval = interval;
        task.priority = priority;
        task.state = TaskState::TASK_IDLE;
        task.enabled = true;
        task.lastRun = 0;
        task.nextRun = _currentTime + interval;
        
        _tasks[id] = task;
        _statistics[id] = TaskStatistics();
        _statistics[id].taskId = id;
        
        return true;
    }

    bool addTask(const Task& task) {
        if (task.id.isEmpty()) return false;
        if (_tasks.find(task.id) != _tasks.end()) return false;
        
        _tasks[task.id] = task;
        _statistics[task.id] = TaskStatistics();
        _statistics[task.id].taskId = task.id;
        
        return true;
    }

    bool removeTask(const String& id) {
        auto it = _tasks.find(id);
        if (it == _tasks.end()) return false;
        
        _tasks.erase(it);
        _statistics.erase(id);
        return true;
    }

    Task* getTask(const String& id) {
        auto it = _tasks.find(id);
        if (it != _tasks.end()) {
            return &(it->second);
        }
        return nullptr;
    }

    std::vector<String> getTaskIds() {
        std::vector<String> ids;
        for (auto& entry : _tasks) {
            ids.push_back(entry.first);
        }
        return ids;
    }

    int getTaskCount() {
        return _tasks.size();
    }

    // 任务控制
    bool enableTask(const String& id) {
        Task* task = getTask(id);
        if (!task) return false;
        task->enabled = true;
        return true;
    }

    bool disableTask(const String& id) {
        Task* task = getTask(id);
        if (!task) return false;
        task->enabled = false;
        task->state = TaskState::TASK_STOPPED;
        return true;
    }

    bool pauseTask(const String& id) {
        Task* task = getTask(id);
        if (!task) return false;
        task->state = TaskState::TASK_PAUSED;
        return true;
    }

    bool resumeTask(const String& id) {
        Task* task = getTask(id);
        if (!task) return false;
        if (task->state == TaskState::TASK_PAUSED) {
            task->state = TaskState::TASK_IDLE;
        }
        return true;
    }

    // 任务执行
    void run() {
        _currentTime = millis();
        
        // 获取可执行的任务
        std::vector<Task*> readyTasks;
        
        for (auto& entry : _tasks) {
            Task& task = entry.second;
            
            if (!task.enabled) continue;
            if (task.state == TaskState::TASK_PAUSED) continue;
            if (task.state == TaskState::TASK_RUNNING) continue;
            
            // 检查是否到期
            if (_currentTime >= task.nextRun) {
                readyTasks.push_back(&task);
            }
        }
        
        // 按优先级排序
        std::sort(readyTasks.begin(), readyTasks.end(),
                  [](Task* a, Task* b) {
                      if (a->priority != b->priority) {
                          return (int)a->priority > (int)b->priority;
                      }
                      return a->nextRun < b->nextRun;
                  });
        
        // 执行任务
        for (Task* task : readyTasks) {
            executeTask(task);
        }
    }

    void executeTask(const String& id) {
        Task* task = getTask(id);
        if (task) {
            executeTask(task);
        }
    }

    // 动态配置
    bool updateTaskInterval(const String& id, unsigned long newInterval) {
        Task* task = getTask(id);
        if (!task) return false;
        
        task->interval = newInterval;
        task->nextRun = _currentTime + newInterval;
        return true;
    }

    bool updateTaskPriority(const String& id, TaskPriority newPriority) {
        Task* task = getTask(id);
        if (!task) return false;
        
        task->priority = newPriority;
        return true;
    }

    // 统计信息
    TaskStatistics getTaskStatistics(const String& id) {
        auto it = _statistics.find(id);
        if (it != _statistics.end()) {
            return it->second;
        }
        return TaskStatistics();
    }

    std::map<String, TaskStatistics> getAllStatistics() {
        return _statistics;
    }

    void resetStatistics(const String& id) {
        auto it = _statistics.find(id);
        if (it != _statistics.end()) {
            it->second = TaskStatistics();
            it->second.taskId = id;
        }
    }

    void resetAllStatistics() {
        for (auto& entry : _statistics) {
            String id = entry.first;
            entry.second = TaskStatistics();
            entry.second.taskId = id;
        }
    }

    // 立即执行任务（忽略调度）
    bool runTaskNow(const String& id) {
        Task* task = getTask(id);
        if (!task) return false;
        
        executeTask(task);
        return true;
    }

    // 延迟执行任务
    bool scheduleTask(const String& id, unsigned long delayMs) {
        Task* task = getTask(id);
        if (!task) return false;
        
        task->nextRun = _currentTime + delayMs;
        return true;
    }

    // 清理
    void clearAll() {
        _tasks.clear();
        _statistics.clear();
    }

    // 测试辅助方法
    void setCurrentTime(unsigned long time) {
        _currentTime = time;
    }

    unsigned long getCurrentTime() {
        return _currentTime;
    }

    // 获取待执行任务数
    int getPendingTaskCount() {
        int count = 0;
        for (auto& entry : _tasks) {
            Task& task = entry.second;
            if (task.enabled && task.state != TaskState::TASK_PAUSED &&
                _currentTime >= task.nextRun) {
                count++;
            }
        }
        return count;
    }

    // 获取运行中任务数
    int getRunningTaskCount() {
        int count = 0;
        for (auto& entry : _tasks) {
            if (entry.second.state == TaskState::TASK_RUNNING) {
                count++;
            }
        }
        return count;
    }

private:
    MockTaskManager() : _initialized(false), _currentTime(0) {}

    void executeTask(Task* task) {
        if (!task || !task->function) return;
        
        task->state = TaskState::TASK_RUNNING;
        
        unsigned long startTime = millis();
        
        // 执行任务函数
        task->function(task->parameter);
        
        unsigned long executionTime = millis() - startTime;
        
        // 更新统计
        TaskStatistics& stats = _statistics[task->id];
        stats.executionCount++;
        stats.totalExecutionTime += executionTime;
        stats.lastExecutionTime = executionTime;
        stats.lastExecution = time(nullptr);
        
        if (executionTime < stats.minExecutionTime) {
            stats.minExecutionTime = executionTime;
        }
        if (executionTime > stats.maxExecutionTime) {
            stats.maxExecutionTime = executionTime;
        }
        
        // 更新任务状态
        task->lastRun = _currentTime;
        task->nextRun = _currentTime + task->interval;
        
        // 检查是否需要停止（只执行一次或达到最大次数）
        if (task->runOnce || 
            (task->maxExecutions > 0 && stats.executionCount >= task->maxExecutions)) {
            task->enabled = false;
            task->state = TaskState::TASK_STOPPED;
        } else {
            task->state = TaskState::TASK_IDLE;
        }
    }

    bool _initialized;
    unsigned long _currentTime;
    std::map<String, Task> _tasks;
    std::map<String, TaskStatistics> _statistics;
};

// 全局实例引用
#define MockTaskMgr MockTaskManager::getInstance()

#endif // MOCK_TASK_MANAGER_H
