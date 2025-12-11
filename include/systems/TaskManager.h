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
#include <functional>
#include <ArduinoJson.h>

// 任务函数类型定义
typedef std::function<void(void*)> TaskFunction;

// 任务统计结构
struct TaskStatistics {
    String name;
    uint32_t executionCount;
    unsigned long lastExecutionTime;
    unsigned long createdTime;
    unsigned long uptime;
};

// 任务结构定义
struct ScheduledTask {
    String name;                  // 任务名称
    TaskFunction function;        // 任务函数
    void* parameter;             // 任务参数
    unsigned long interval;       // 执行间隔（毫秒）
    unsigned long lastRun;        // 上次执行时间
    bool enabled;                 // 是否启用
    unsigned long createdTime;    // 创建时间
    uint32_t executionCount;      // 执行次数统计
    unsigned long lastExecutionTime; // 最后执行时间
};

class TaskManager {
public:
    TaskManager();
    ~TaskManager();
    
    bool initialize();
    
    // 任务管理
    bool addTask(const String& name, TaskFunction func, void* param, 
                unsigned long interval, bool enabled = true);
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

private:
    std::vector<ScheduledTask> tasks;
    bool isRunning;
};

#endif