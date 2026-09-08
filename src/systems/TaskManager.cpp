/**
 * @description: Task manager implementation
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:32:59
 */

#include "systems/TaskManager.h"
#include <core/SystemConstants.h>
#include <core/FeatureFlags.h>
#include "systems/LoggerSystem.h"
#if FASTBEE_ENABLE_HEALTH_MONITOR
#include "systems/HealthMonitor.h"
#include "core/FastBeeFramework.h"
#endif
#include <esp_task_wdt.h>

namespace {
constexpr unsigned long TASK_RUN_CYCLE_BUDGET_MS = 50;
constexpr uint8_t LOW_PRIORITY_OVERRUN_DISABLE_THRESHOLD = 3;

unsigned long defaultExecutionBudget(unsigned long interval, TaskPriority priority) {
    const unsigned long priorityCap = priority == TaskPriority::PRIORITY_CRITICAL ? 3000UL :
                                      priority == TaskPriority::PRIORITY_HIGH ? 1000UL :
                                      priority == TaskPriority::PRIORITY_NORMAL ? 250UL : 100UL;
    const unsigned long intervalBudget = interval > 0 ? max(20UL, interval / 2UL) : priorityCap;
    return min(priorityCap, intervalBudget);
}
}  // namespace

TaskManager::TaskManager() {
    tasks.reserve(TaskScheduler::MAX_TASKS);
    isRunning = true;
    tasksMutex = xSemaphoreCreateMutex();
}

TaskManager::~TaskManager() {
    stopAllTasks();
    tasks.clear();
    if (tasksMutex) {
        vSemaphoreDelete(tasksMutex);
        tasksMutex = nullptr;
    }
}

bool TaskManager::initialize() {
    LOG_INFO("Task Manager: Initializing...");
    if (!tasksMutex) tasksMutex = xSemaphoreCreateMutex();
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    tasks.clear();
    isRunning = true;
    if (tasksMutex) xSemaphoreGive(tasksMutex);
    char buf[64];
    snprintf(buf, sizeof(buf), "Task Manager: Ready, capacity=%d", TaskScheduler::MAX_TASKS);
    LOG_INFO(buf);
    return true;
}

bool TaskManager::addTask(const String& name, TaskFunction func, void* param, 
                         unsigned long interval, bool enabled) {
    return addTask(name, func, param, interval, TaskPriority::PRIORITY_NORMAL, enabled);
}

bool TaskManager::addTask(const String& name, TaskFunction func, void* param, 
                         unsigned long interval, TaskPriority priority,
                         bool enabled) {
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    if (tasks.size() >= TaskScheduler::MAX_TASKS) {
        if (tasksMutex) xSemaphoreGive(tasksMutex);
        char buf[80];
        snprintf(buf, sizeof(buf), "Task Manager: Cannot add '%s' - max tasks reached", name.c_str());
        LOG_WARNING(buf);
        return false;
    }
    
    // 检查任务是否已存在
    for (const auto& task : tasks) {
        if (strcmp(task.name, name.c_str()) == 0) {
            char buf[80];
            snprintf(buf, sizeof(buf), "Task Manager: Task '%s' already exists", name.c_str());
            LOG_WARNING(buf);
            if (tasksMutex) xSemaphoreGive(tasksMutex);
            return false;
        }
    }
    
    ScheduledTask newTask;
    strncpy(newTask.name, name.c_str(), sizeof(newTask.name) - 1);
    newTask.name[sizeof(newTask.name) - 1] = '\0'; // Ensure null termination
    newTask.function       = func;
    newTask.parameter      = param;
    newTask.interval       = interval;
    newTask.lastRun        = 0;
    newTask.enabled        = enabled;
    newTask.priority       = priority;
    newTask.createdTime    = millis();
    newTask.executionCount = 0;
    newTask.lastExecutionTime = 0;
    newTask.maxExecutionTime = 0;
    newTask.totalExecutionTime = 0;
    newTask.executionBudget = defaultExecutionBudget(interval, priority);
    newTask.overrunCount = 0;
    newTask.deferredCount = 0;
    newTask.consecutiveOverruns = 0;
    
    // 有序插入：高优先级排在前面（CRITICAL=3 > HIGH=2 > NORMAL=1 > LOW=0）
    auto it = tasks.begin();
    while (it != tasks.end() && it->priority >= newTask.priority) {
        ++it;
    }
    tasks.insert(it, newTask);
    if (tasksMutex) xSemaphoreGive(tasksMutex);

    char buf[80];
    snprintf(buf, sizeof(buf), "Task Manager: Added '%s' interval=%lums priority=%d", 
             name.c_str(), interval, static_cast<int>(priority));
    LOG_INFO(buf);
    return true;
}

bool TaskManager::removeTask(const String& name) {
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    for (auto it = tasks.begin(); it != tasks.end(); ++it) {
        if (strcmp(it->name, name.c_str()) == 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Task Manager: Removed '%s'", name.c_str());
            LOG_INFO(buf);
            tasks.erase(it);
            if (tasksMutex) xSemaphoreGive(tasksMutex);
            return true;
        }
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "Task Manager: Task '%s' not found", name.c_str());
    LOG_WARNING(buf);
    if (tasksMutex) xSemaphoreGive(tasksMutex);
    return false;
}

bool TaskManager::enableTask(const String& name) {
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    for (auto& task : tasks) {
        if (strcmp(task.name, name.c_str()) == 0) {
            if (!task.enabled) {
                task.enabled = true;
                task.lastRun = millis();
                char buf[64];
                snprintf(buf, sizeof(buf), "Task Manager: Enabled '%s'", name.c_str());
                LOG_INFO(buf);
            }
            if (tasksMutex) xSemaphoreGive(tasksMutex);
            return true;
        }
    }
    if (tasksMutex) xSemaphoreGive(tasksMutex);
    return false;
}

bool TaskManager::disableTask(const String& name) {
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    for (auto& task : tasks) {
        if (strcmp(task.name, name.c_str()) == 0) {
            if (task.enabled) {
                task.enabled = false;
                char buf[64];
                snprintf(buf, sizeof(buf), "Task Manager: Disabled '%s'", name.c_str());
                LOG_INFO(buf);
            }
            if (tasksMutex) xSemaphoreGive(tasksMutex);
            return true;
        }
    }
    if (tasksMutex) xSemaphoreGive(tasksMutex);
    return false;
}

void TaskManager::run() {
    unsigned long now = millis();
    const unsigned long cycleStart = now;
    std::vector<ScheduledTask> due;
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    if (!isRunning) {
        if (tasksMutex) xSemaphoreGive(tasksMutex);
        return;
    }
    
#if FASTBEE_ENABLE_HEALTH_MONITOR
    // 获取内存保护等级，用于低内存时跳过低优先级任务
    MemoryGuardLevel guardLevel = MemoryGuardLevel::NORMAL;
    HealthMonitor* hm = FastBeeFramework::getInstance()->getHealthMonitor();
    if (hm) {
        guardLevel = hm->getMemoryGuardLevel();
    }
#endif
    
    // 顺序遍历（已按优先级从高到低有序插入，无需分级遍历）
    // PRIORITY_CRITICAL(3) > HIGH(2) > NORMAL(1) > LOW(0)
    for (auto& task : tasks) {
        if (!task.enabled) continue;
#if FASTBEE_ENABLE_HEALTH_MONITOR
        // SEVERE/CRITICAL 时跳过 LOW 和 NORMAL 优先级任务
        if (guardLevel >= MemoryGuardLevel::SEVERE && 
            task.priority <= TaskPriority::PRIORITY_NORMAL) {
            continue;
        }
#endif
        if (now - task.lastRun < task.interval) continue;
        
        task.lastRun = now;
        due.push_back(task);
    }
    if (tasksMutex) xSemaphoreGive(tasksMutex);

    // Execute callbacks outside the mutex: callbacks may add/remove tasks.
    for (const auto& scheduled : due) {
        if (!scheduled.function) continue;

        // Once higher-priority work has consumed this scheduler tick, defer
        // LOW/NORMAL callbacks to the next loop instead of extending latency.
        if (scheduled.priority <= TaskPriority::PRIORITY_NORMAL &&
            millis() - cycleStart >= TASK_RUN_CYCLE_BUDGET_MS) {
            if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
            for (auto& task : tasks) {
                if (strcmp(task.name, scheduled.name) == 0) {
                    task.lastRun = millis() - task.interval;
                    task.deferredCount++;
                    break;
                }
            }
            if (tasksMutex) xSemaphoreGive(tasksMutex);
            continue;
        }

        unsigned long taskStart = millis();
        scheduled.function(scheduled.parameter);
        unsigned long duration = millis() - taskStart;

        bool disableLowPriorityTask = false;
        uint8_t consecutiveOverruns = 0;

        if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
        for (auto& task : tasks) {
            if (strcmp(task.name, scheduled.name) == 0) {
                task.lastExecutionTime = duration;
                task.maxExecutionTime = max(task.maxExecutionTime, duration);
                task.executionCount++;
                task.totalExecutionTime += duration;
                if (duration > task.executionBudget) {
                    task.overrunCount++;
                    if (task.consecutiveOverruns < UINT8_MAX) task.consecutiveOverruns++;
                    consecutiveOverruns = task.consecutiveOverruns;
                    disableLowPriorityTask = task.priority == TaskPriority::PRIORITY_LOW &&
                        task.consecutiveOverruns >= LOW_PRIORITY_OVERRUN_DISABLE_THRESHOLD;
                    if (disableLowPriorityTask) task.enabled = false;
                } else {
                    task.consecutiveOverruns = 0;
                }
                break;
            }
        }
        if (tasksMutex) xSemaphoreGive(tasksMutex);
        
        if (duration > scheduled.executionBudget) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "Task Manager: '%s' over budget (%lu>%lu ms, consecutive=%u)%s",
                     scheduled.name, duration, scheduled.executionBudget,
                     (unsigned)consecutiveOverruns,
                     disableLowPriorityTask ? "; disabled" : "");
            LOG_WARNING(buf);
        }
    }
}

void TaskManager::listTasks() {
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    LOG_INFO("=== Scheduled Tasks ===");
    for (const auto& task : tasks) {
        unsigned long ago = millis() - task.lastRun;
        // Use a fixed buffer to avoid repeated String concatenation
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "  [%s] interval=%lums budget=%lums priority=%d enabled=%s lastRun=%lums ago exec=%lu overruns=%lu deferred=%lu lastExec=%lums maxExec=%lums",
                 task.name,
                 task.interval,
                 task.executionBudget,
                 static_cast<int>(task.priority),
                 task.enabled ? "Y" : "N",
                 ago,
                 (unsigned long)task.executionCount,
                 (unsigned long)task.overrunCount,
                 (unsigned long)task.deferredCount,
                 task.lastExecutionTime,
                 task.maxExecutionTime);
        LOG_INFO(buf);
    }
    if (tasksMutex) xSemaphoreGive(tasksMutex);
}

bool TaskManager::taskExists(const String& name) {
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    for (auto& task : tasks) {
        if (strcmp(task.name, name.c_str()) == 0) {
            if (tasksMutex) xSemaphoreGive(tasksMutex);
            return true;
        }
    }
    if (tasksMutex) xSemaphoreGive(tasksMutex);
    return false;
}

// Stop all tasks
void TaskManager::stopAllTasks() {
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    if (!isRunning) {
        if (tasksMutex) xSemaphoreGive(tasksMutex);
        return;
    }
    
    LOG_INFO("Task Manager: Stopping all tasks...");
    
    // Disable all tasks
    for (auto& task : tasks) {
        task.enabled = false;
    }
    
    isRunning = false;
    if (tasksMutex) xSemaphoreGive(tasksMutex);
    LOG_INFO("Task Manager: All tasks stopped");
}

// Restart all tasks
void TaskManager::restartAllTasks() {
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    LOG_INFO("Task Manager: Restarting all tasks...");
    
    // 重新启用所有任务并重置执行时间
    unsigned long currentTime = millis();
    for (auto& task : tasks) {
        task.enabled = true;
        task.lastRun = currentTime;
    }
    
    isRunning = true;
    if (tasksMutex) xSemaphoreGive(tasksMutex);
    LOG_INFO("Task Manager: All tasks restarted");
}

// Pause the task manager
void TaskManager::pause() {
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    LOG_INFO("Task Manager: Paused");
    isRunning = false;
    if (tasksMutex) xSemaphoreGive(tasksMutex);
}

// Resume the task manager
void TaskManager::resume() {
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    LOG_INFO("Task Manager: Resumed");
    isRunning = true;
    if (tasksMutex) xSemaphoreGive(tasksMutex);
}

// Get task manager running state
bool TaskManager::isRunningStatus() const {
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    const bool running = isRunning;
    if (tasksMutex) xSemaphoreGive(tasksMutex);
    return running;
}

// Get active task count
size_t TaskManager::getActiveTaskCount() const {
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    size_t count = 0;
    for (const auto& task : tasks) {
        if (task.enabled) {
            count++;
        }
    }
    if (tasksMutex) xSemaphoreGive(tasksMutex);
    return count;
}

// Clear all tasks
void TaskManager::clearAllTasks() {
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    LOG_INFO("Task Manager: Clearing all tasks...");
    tasks.clear();
    if (tasksMutex) xSemaphoreGive(tasksMutex);
    LOG_INFO("Task Manager: All tasks cleared");
}

// Get statistics for a specific task
TaskStatistics TaskManager::getTaskStatistics(const String& name) {
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    for (auto& task : tasks) {
        if (strcmp(task.name, name.c_str()) == 0) {
            TaskStatistics stats;
            stats.name = task.name;
            stats.priority = task.priority;
            stats.executionCount = task.executionCount;
            stats.lastExecutionTime = task.lastExecutionTime;
            stats.maxExecutionTime = task.maxExecutionTime;
            stats.executionBudget = task.executionBudget;
            stats.overrunCount = task.overrunCount;
            stats.deferredCount = task.deferredCount;
            stats.consecutiveOverruns = task.consecutiveOverruns;
            stats.createdTime = task.createdTime;
            stats.uptime = millis() - task.createdTime;
            if (tasksMutex) xSemaphoreGive(tasksMutex);
            return stats;
        }
    }
    
    // 返回空的统计信息
    TaskStatistics emptyStats;
    emptyStats.name = name;
    emptyStats.priority = TaskPriority::PRIORITY_NORMAL;
    emptyStats.executionCount = 0;
    emptyStats.lastExecutionTime = 0;
    emptyStats.maxExecutionTime = 0;
    emptyStats.executionBudget = 0;
    emptyStats.overrunCount = 0;
    emptyStats.deferredCount = 0;
    emptyStats.consecutiveOverruns = 0;
    emptyStats.createdTime = 0;
    emptyStats.uptime = 0;
    if (tasksMutex) xSemaphoreGive(tasksMutex);
    return emptyStats;
}

// 获取任务列表的JSON表示
// ArduinoJson 7.x 使用 JsonDocument（替代废弃的 DynamicJsonDocument）
String TaskManager::getTasksJSON() {
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    JsonDocument doc;
    JsonArray taskArray = doc.to<JsonArray>();

    unsigned long currentTime = millis();

    // 顺序输出（已按优先级从高到低有序插入，无需分级遍历）
    for (const auto& task : tasks) {
        JsonObject obj = taskArray.add<JsonObject>();
        obj["name"] = task.name;
        obj["enabled"] = task.enabled;
        obj["priority"] = static_cast<int>(task.priority);
        obj["interval"] = task.interval;
        obj["lastExecTime"] = task.lastExecutionTime;
        obj["maxExecTime"] = task.maxExecutionTime;
        obj["executionBudget"] = task.executionBudget;
        obj["overrunCount"] = task.overrunCount;
        obj["deferredCount"] = task.deferredCount;
        obj["consecutiveOverruns"] = task.consecutiveOverruns;
        obj["avgExecTime"] = task.executionCount > 0 
            ? task.totalExecutionTime / task.executionCount : 0;
        obj["execCount"] = task.executionCount;
        
        long nextRunIn = static_cast<long>(task.interval) - 
                        static_cast<long>(currentTime - task.lastRun);
        obj["nextRunIn"] = nextRunIn > 0 ? nextRunIn : 0;
    }

    String result;
    serializeJson(doc, result);
    if (tasksMutex) xSemaphoreGive(tasksMutex);
    return result;
}

// 修改任务间隔
bool TaskManager::updateTaskInterval(const String& name, unsigned long newInterval) {
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    for (auto& task : tasks) {
        if (strcmp(task.name, name.c_str()) == 0) {
            char buf[80];
            snprintf(buf, sizeof(buf), "Task Manager: '%s' interval %lu->%lums",
                     name.c_str(), task.interval, newInterval);
            task.interval = newInterval;
            LOG_INFO(buf);
            if (tasksMutex) xSemaphoreGive(tasksMutex);
            return true;
        }
    }
    if (tasksMutex) xSemaphoreGive(tasksMutex);
    return false;
}

bool TaskManager::setTaskExecutionBudget(const String& name, unsigned long budgetMs) {
    if (budgetMs == 0 || budgetMs > 60000) return false;
    if (tasksMutex) xSemaphoreTake(tasksMutex, portMAX_DELAY);
    for (auto& task : tasks) {
        if (strcmp(task.name, name.c_str()) == 0) {
            task.executionBudget = budgetMs;
            task.consecutiveOverruns = 0;
            if (tasksMutex) xSemaphoreGive(tasksMutex);
            return true;
        }
    }
    if (tasksMutex) xSemaphoreGive(tasksMutex);
    return false;
}
