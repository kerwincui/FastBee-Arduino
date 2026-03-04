#ifndef I_LOGGER_SYSTEM_H
#define I_LOGGER_SYSTEM_H

/**
 * @brief 日志级别枚举
 */
enum LogLevel {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR,
    LOG_FATAL,
    LOG_VERBOSE
};

/**
 * @brief 日志系统接口
 * @details 定义日志系统的基本操作
 */
class ILoggerSystem {
public:
    virtual ~ILoggerSystem() = default;
    
    /**
     * @brief 初始化日志系统
     * @return 是否初始化成功
     */
    virtual bool initialize() = 0;
    
    /**
     * @brief 设置日志级别
     * @param level 日志级别
     */
    virtual void setLogLevel(LogLevel level) = 0;
    
    /**
     * @brief 获取当前日志级别
     * @return 日志级别
     */
    virtual LogLevel getLogLevel() const = 0;
    
    /**
     * @brief 记录调试日志
     * @param message 日志消息
     */
    virtual void logDebug(const String& message) = 0;
    
    /**
     * @brief 记录信息日志
     * @param message 日志消息
     */
    virtual void logInfo(const String& message) = 0;
    
    /**
     * @brief 记录警告日志
     * @param message 日志消息
     */
    virtual void logWarning(const String& message) = 0;
    
    /**
     * @brief 记录错误日志
     * @param message 日志消息
     */
    virtual void logError(const String& message) = 0;
    
    /**
     * @brief 记录致命错误日志
     * @param message 日志消息
     */
    virtual void logFatal(const String& message) = 0;
};

#endif // I_LOGGER_SYSTEM_H