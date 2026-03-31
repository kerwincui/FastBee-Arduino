/**
 * @description: 任务管理器实�?
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:32:59
 */

#include "systems/TaskManager.h"
#include <core/SystemConstants.h>
#include <core/FeatureFlags.h>
#include "systems/LoggerSystem.h"

TaskManager::TaskManager() {
    tasks.reserve(TaskScheduler::MAX_TASKS);
    isRunning = true;
}

TaskManager::~TaskManager() {
    stopAllTasks();
    tasks.clear();
}

bool TaskManager::initialize() {
    LOG_INFO("Task Manager: Initializing...");
    tasks.clear();
    isRunning = true;
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
    if (tasks.size() >= TaskScheduler::MAX_TASKS) {
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
            return false;
        }
    }
    
    ScheduledTask newTask;
    strncpy(newTask.name, name.c_str(), sizeof(newTask.name) - 1);
    newTask.name[sizeof(newTask.name) - 1] = '\0'; // 确保字符串结�?
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
    
    tasks.push_back(newTask);

    char buf[80];
    snprintf(buf, sizeof(buf), "Task Manager: Added '%s' interval=%lums priority=%d", 
             name.c_str(), interval, static_cast<int>(priority));
    LOG_INFO(buf);
    return true;
}

bool TaskManager::removeTask(const String& name) {
    for (auto it = tasks.begin(); it != tasks.end(); ++it) {
        if (strcmp(it->name, name.c_str()) == 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Task Manager: Removed '%s'", name.c_str());
            LOG_INFO(buf);
            tasks.erase(it);
            return true;
        }
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "Task Manager: Task '%s' not found", name.c_str());
    LOG_WARNING(buf);
    return false;
}

bool TaskManager::enableTask(const String& name) {
    for (auto& task : tasks) {
        if (strcmp(task.name, name.c_str()) == 0) {
            if (!task.enabled) {
                task.enabled = true;
                task.lastRun = millis();
                char buf[64];
                snprintf(buf, sizeof(buf), "Task Manager: Enabled '%s'", name.c_str());
                LOG_INFO(buf);
            }
            return true;
        }
    }
    return false;
}

bool TaskManager::disableTask(const String& name) {
    for (auto& task : tasks) {
        if (strcmp(task.name, name.c_str()) == 0) {
            if (task.enabled) {
                task.enabled = false;
                char buf[64];
                snprintf(buf, sizeof(buf), "Task Manager: Disabled '%s'", name.c_str());
                LOG_INFO(buf);
            }
            return true;
        }
    }
    return false;
}

void TaskManager::run() {
    if (!isRunning) {
        return;
    }
    
    unsigned long currentTime = millis();
    
    // 按优先级排序任务（从高到低）
    std::sort(tasks.begin(), tasks.end(), [](const ScheduledTask& a, const ScheduledTask& b) {
        return a.priority > b.priority;
    });
    
    for (auto& task : tasks) {
        if (!task.enabled) {
            continue;
        }
        
        // 处理时间溢出（大�?0天后millis()会重置）
        unsigned long timeSinceLastRun;
        if (currentTime < task.lastRun) {
            // 时间溢出处理
            timeSinceLastRun = (0xFFFFFFFF - task.lastRun) + currentTime;
        } else {
            timeSinceLastRun = currentTime - task.lastRun;
        }
        
        // 检查是否到了执行时�?
        if (timeSinceLastRun >= task.interval) {
            // 记录任务开始执行时�?
            unsigned long startTime = millis();
            
            // 执行任务
            task.function(task.parameter);
            
            // 计算执行时间
            unsigned long executionTime = millis() - startTime;
            task.lastRun = currentTime;
            task.lastExecutionTime = executionTime;
            
            // 更新最大执行时�?
            if (executionTime > task.maxExecutionTime) {
                task.maxExecutionTime = executionTime;
            }
            
            // 记录任务执行统计
            task.executionCount++;
            
            // 检查任务执行时间是否过�?
            if (executionTime > task.interval * 0.8) { // 执行时间超过间隔�?0%
                char buf[128];
                snprintf(buf, sizeof(buf), 
                         "Task Manager: Task '%s' execution time (%lu ms) is close to interval (%lu ms)",
                         task.name, executionTime, task.interval);
                LOG_WARNING(buf);
            }
        }
    }
}

void TaskManager::listTasks() {
    LOG_INFO("=== Scheduled Tasks ===");
    for (const auto& task : tasks) {
        unsigned long ago = millis() - task.lastRun;
        // 使用格式化日志宏，避免多�?String 拼接
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "  [%s] interval=%lums priority=%d enabled=%s lastRun=%lums ago exec=%lu lastExec=%lums maxExec=%lums",
                 task.name,
                 task.interval,
                 static_cast<int>(task.priority),
                 task.enabled ? "Y" : "N",
                 ago,
                 (unsigned long)task.executionCount,
                 task.lastExecutionTime,
                 task.maxExecutionTime);
        LOG_INFO(buf);
    }
}

bool TaskManager::taskExists(const String& name) {
    for (auto& task : tasks) {
        if (strcmp(task.name, name.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

// �?新增：停止所有任�?
void TaskManager::stopAllTasks() {
    if (!isRunning) {
        return;
    }
    
    LOG_INFO("Task Manager: Stopping all tasks...");
    
    // 禁用所有任�?
    for (auto& task : tasks) {
        task.enabled = false;
    }
    
    isRunning = false;
    LOG_INFO("Task Manager: All tasks stopped");
}

// �?新增：重新启动所有任�?
void TaskManager::restartAllTasks() {
    LOG_INFO("Task Manager: Restarting all tasks...");
    
    // 重新启用所有任务并重置执行时间
    unsigned long currentTime = millis();
    for (auto& task : tasks) {
        task.enabled = true;
        task.lastRun = currentTime;
    }
    
    isRunning = true;
    LOG_INFO("Task Manager: All tasks restarted");
}

// �?新增：暂停任务管理器（不执行任何任务�?
void TaskManager::pause() {
    LOG_INFO("Task Manager: Paused");
    isRunning = false;
}

// �?新增：恢复任务管理器
void TaskManager::resume() {
    LOG_INFO("Task Manager: Resumed");
    isRunning = true;
}

// �?新增：获取任务管理器状�?
bool TaskManager::isRunningStatus() const {
    return isRunning;
}

// �?新增：获取活动任务数�?
size_t TaskManager::getActiveTaskCount() const {
    size_t count = 0;
    for (const auto& task : tasks) {
        if (task.enabled) {
            count++;
        }
    }
    return count;
}

// �?新增：清空所有任�?
void TaskManager::clearAllTasks() {
    LOG_INFO("Task Manager: Clearing all tasks...");
    tasks.clear();
    LOG_INFO("Task Manager: All tasks cleared");
}

// �?新增：获取特定任务的统计信息
TaskStatistics TaskManager::getTaskStatistics(const String& name) {
    for (auto& task : tasks) {
        if (strcmp(task.name, name.c_str()) == 0) {
            TaskStatistics stats;
            stats.name = task.name;
            stats.priority = task.priority;
            stats.executionCount = task.executionCount;
            stats.lastExecutionTime = task.lastExecutionTime;
            stats.maxExecutionTime = task.maxExecutionTime;
            stats.createdTime = task.createdTime;
            stats.uptime = millis() - task.createdTime;
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
    emptyStats.createdTime = 0;
    emptyStats.uptime = 0;
    return emptyStats;
}

// 获取任务列表的JSON表示
// ArduinoJson 7.x 使用 JsonDocument（替代废弃的 DynamicJsonDocument）
String TaskManager::getTasksJSON() {
    FastBeeJsonDocLarge doc;
    JsonArray taskArray = doc.to<JsonArray>();


    unsigned long currentTime = millis();

    for (const auto& task : tasks) {
        JsonObject taskObj = taskArray.add<JsonObject>();
        taskObj["name"]    = task.name;
        taskObj["enabled"] = task.enabled;
        taskObj["priority"] = static_cast<int>(task.priority);
        taskObj["interval"]= task.interval;
        taskObj["lastExecutionTime"] = task.lastExecutionTime;
        taskObj["maxExecutionTime"] = task.maxExecutionTime;

        // 计算下次执行时间（处�?millis() 溢出�?
        unsigned long elapsed = (currentTime >= task.lastRun)
            ? (currentTime - task.lastRun)
            : (0xFFFFFFFFUL - task.lastRun + currentTime);

        unsigned long nextRun = (task.enabled && elapsed < task.interval)
            ? (task.interval - elapsed)
            : 0;

        taskObj["next_run_in"]      = nextRun;
        taskObj["execution_count"]  = task.executionCount;
        taskObj["created_time"]     = task.createdTime;
        taskObj["last_execution"]   = task.lastExecutionTime;
    }

    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

// 修改任务间隔
bool TaskManager::updateTaskInterval(const String& name, unsigned long newInterval) {
    for (auto& task : tasks) {
        if (strcmp(task.name, name.c_str()) == 0) {
            char buf[80];
            snprintf(buf, sizeof(buf), "Task Manager: '%s' interval %lu->%lums",
                     name.c_str(), task.interval, newInterval);
            task.interval = newInterval;
            LOG_INFO(buf);
            return true;
        }
    }
    return false;
}
