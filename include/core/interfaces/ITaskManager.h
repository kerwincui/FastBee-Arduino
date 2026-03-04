#ifndef I_TASK_MANAGER_H
#define I_TASK_MANAGER_H

/**
 * @brief 任务函数类型
 */
typedef void (*TaskFunction)(void* param);

/**
 * @brief 任务管理器接口
 * @details 定义任务管理的基本操作
 */
class ITaskManager {
public:
    virtual ~ITaskManager() = default;
    
    /**
     * @brief 初始化任务管理器
     * @return 是否初始化成功
     */
    virtual bool initialize() = 0;
    
    /**
     * @brief 添加任务
     * @param name 任务名称
     * @param func 任务函数
     * @param param 任务参数
     * @param interval 执行间隔（毫秒）
     * @param enabled 是否启用
     * @return 是否添加成功
     */
    virtual bool addTask(const String& name, TaskFunction func, void* param, 
                        unsigned long interval, bool enabled = true) = 0;
    
    /**
     * @brief 移除任务
     * @param name 任务名称
     * @return 是否移除成功
     */
    virtual bool removeTask(const String& name) = 0;
    
    /**
     * @brief 启用任务
     * @param name 任务名称
     * @return 是否启用成功
     */
    virtual bool enableTask(const String& name) = 0;
    
    /**
     * @brief 禁用任务
     * @param name 任务名称
     * @return 是否禁用成功
     */
    virtual bool disableTask(const String& name) = 0;
    
    /**
     * @brief 运行任务调度
     */
    virtual void run() = 0;
    
    /**
     * @brief 停止所有任务
     */
    virtual void stopAllTasks() = 0;
    
    /**
     * @brief 重启所有任务
     */
    virtual void restartAllTasks() = 0;
    
    /**
     * @brief 暂停任务管理器
     */
    virtual void pause() = 0;
    
    /**
     * @brief 恢复任务管理器
     */
    virtual void resume() = 0;
};

#endif // I_TASK_MANAGER_H