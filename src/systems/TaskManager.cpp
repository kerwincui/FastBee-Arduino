/**
 * @description: 任务管理器实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:32:59
 */

#include "systems/TaskManager.h"
#include <core/SystemConstants.h>
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
    
    // 清空任务列表（如果需要）
    tasks.clear();
    isRunning = true;
    
    LOG_INFO("Task Manager: Initialization completed");
    LOG_INFO("Task Manager: Capacity for " + String(TaskScheduler::MAX_TASKS) + " tasks");
    
    return true;
}

bool TaskManager::addTask(const String& name, TaskFunction func, void* param, 
                         unsigned long interval, bool enabled) {
    if (tasks.size() >= TaskScheduler::MAX_TASKS) {
        LOG_WARNING("Task Manager: Cannot add task '" + name + "' - maximum tasks reached");
        return false;
    }
    
    // 检查任务是否已存在
    for (auto& task : tasks) {
        if (task.name == name) {
            LOG_WARNING("Task Manager: Task '" + name + "' already exists");
            return false;
        }
    }
    
    ScheduledTask newTask;
    newTask.name = name;
    newTask.function = func;
    newTask.parameter = param;
    newTask.interval = interval;
    newTask.lastRun = 0;
    newTask.enabled = enabled;
    newTask.createdTime = millis();
    
    tasks.push_back(newTask);
    LOG_INFO("Task Manager: Added task '" + name + "' with interval " + String(interval) + "ms");
    return true;
}

bool TaskManager::removeTask(const String& name) {
    for (auto it = tasks.begin(); it != tasks.end(); ++it) {
        if (it->name == name) {
            LOG_INFO("Task Manager: Removed task '" + name + "'");
            tasks.erase(it);
            return true;
        }
    }
    LOG_WARNING("Task Manager: Task '" + name + "' not found for removal");
    return false;
}

bool TaskManager::enableTask(const String& name) {
    for (auto& task : tasks) {
        if (task.name == name) {
            if (!task.enabled) {
                task.enabled = true;
                task.lastRun = millis(); // 重置执行时间
                LOG_INFO("Task Manager: Enabled task '" + name + "'");
            }
            return true;
        }
    }
    return false;
}

bool TaskManager::disableTask(const String& name) {
    for (auto& task : tasks) {
        if (task.name == name) {
            if (task.enabled) {
                task.enabled = false;
                LOG_INFO("Task Manager: Disabled task '" + name + "'");
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
    
    for (auto& task : tasks) {
        if (!task.enabled) {
            continue;
        }
        
        // 处理时间溢出（大约50天后millis()会重置）
        unsigned long timeSinceLastRun;
        if (currentTime < task.lastRun) {
            // 时间溢出处理
            timeSinceLastRun = (0xFFFFFFFF - task.lastRun) + currentTime;
        } else {
            timeSinceLastRun = currentTime - task.lastRun;
        }
        
        // 检查是否到了执行时间
        if (timeSinceLastRun >= task.interval) {
            task.function(task.parameter);
            task.lastRun = currentTime;
            
            // 记录任务执行统计
            task.executionCount++;
            task.lastExecutionTime = currentTime;
        }
    }
}

void TaskManager::listTasks() {
    LOG_INFO("=== Scheduled Tasks ===");
    for (auto& task : tasks) {
        LOG_INFO("Task: " + task.name + 
                ", Interval: " + String(task.interval) + "ms" +
                ", Enabled: " + String(task.enabled ? "Yes" : "No") +
                ", Last Run: " + String(millis() - task.lastRun) + "ms ago" +
                ", Executions: " + String(task.executionCount));
    }
}

bool TaskManager::taskExists(const String& name) {
    for (auto& task : tasks) {
        if (task.name == name) {
            return true;
        }
    }
    return false;
}

// ✅ 新增：停止所有任务
void TaskManager::stopAllTasks() {
    if (!isRunning) {
        return;
    }
    
    LOG_INFO("Task Manager: Stopping all tasks...");
    
    // 禁用所有任务
    for (auto& task : tasks) {
        task.enabled = false;
    }
    
    isRunning = false;
    LOG_INFO("Task Manager: All tasks stopped");
}

// ✅ 新增：重新启动所有任务
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

// ✅ 新增：暂停任务管理器（不执行任何任务）
void TaskManager::pause() {
    LOG_INFO("Task Manager: Paused");
    isRunning = false;
}

// ✅ 新增：恢复任务管理器
void TaskManager::resume() {
    LOG_INFO("Task Manager: Resumed");
    isRunning = true;
}

// ✅ 新增：获取任务管理器状态
bool TaskManager::isRunningStatus() const {
    return isRunning;
}

// ✅ 新增：获取活动任务数量
size_t TaskManager::getActiveTaskCount() const {
    size_t count = 0;
    for (const auto& task : tasks) {
        if (task.enabled) {
            count++;
        }
    }
    return count;
}

// ✅ 新增：清空所有任务
void TaskManager::clearAllTasks() {
    LOG_INFO("Task Manager: Clearing all tasks...");
    tasks.clear();
    LOG_INFO("Task Manager: All tasks cleared");
}

// ✅ 新增：获取特定任务的统计信息
TaskStatistics TaskManager::getTaskStatistics(const String& name) {
    for (auto& task : tasks) {
        if (task.name == name) {
            TaskStatistics stats;
            stats.name = task.name;
            stats.executionCount = task.executionCount;
            stats.lastExecutionTime = task.lastExecutionTime;
            stats.createdTime = task.createdTime;
            stats.uptime = millis() - task.createdTime;
            return stats;
        }
    }
    
    // 返回空的统计信息
    TaskStatistics emptyStats;
    emptyStats.name = name;
    emptyStats.executionCount = 0;
    emptyStats.lastExecutionTime = 0;
    emptyStats.createdTime = 0;
    emptyStats.uptime = 0;
    return emptyStats;
}

// ✅ 新增：获取任务列表的JSON表示
String TaskManager::getTasksJSON() {
    DynamicJsonDocument doc(2048);
    JsonArray taskArray = doc.to<JsonArray>();
    
    unsigned long currentTime = millis();
    
    for (const auto& task : tasks) {
        JsonObject taskObj = taskArray.createNestedObject();
        taskObj["name"] = task.name;
        taskObj["enabled"] = task.enabled;
        taskObj["interval"] = task.interval;
        
        // 计算下次执行时间
        unsigned long nextRun = 0;
        if (task.enabled) {
            if (currentTime < task.lastRun) {
                // 时间溢出处理
                nextRun = task.interval - ((0xFFFFFFFF - task.lastRun) + currentTime);
            } else {
                nextRun = task.interval - (currentTime - task.lastRun);
            }
            if (nextRun > task.interval) nextRun = 0; // 应该立即执行
        }
        taskObj["next_run_in"] = nextRun;
        
        taskObj["execution_count"] = task.executionCount;
        taskObj["created_time"] = task.createdTime;
        taskObj["last_execution"] = task.lastExecutionTime;
    }
    
    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

// ✅ 新增：修改任务间隔
bool TaskManager::updateTaskInterval(const String& name, unsigned long newInterval) {
    for (auto& task : tasks) {
        if (task.name == name) {
            unsigned long oldInterval = task.interval;
            task.interval = newInterval;
            LOG_INFO("Task Manager: Updated task '" + name + "' interval from " + 
                    String(oldInterval) + "ms to " + String(newInterval) + "ms");
            return true;
        }
    }
    return false;
}