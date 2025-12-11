#ifndef LOGGER_SYSTEM_H
#define LOGGER_SYSTEM_H

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>

enum LogLevel {
    LOG_ERROR = 0,
    LOG_WARNING = 1,
    LOG_INFO = 2,
    LOG_DEBUG = 3,
    LOG_VERBOSE = 4
};

class LoggerSystem {
public:
    // 删除拷贝构造函数和赋值操作符
    LoggerSystem(const LoggerSystem&) = delete;
    LoggerSystem& operator=(const LoggerSystem&) = delete;
    
    // 获取单例实例的静态方法
    static LoggerSystem& getInstance() {
        static LoggerSystem instance;
        return instance;
    }
    
    bool initialize();
    
    void setLogLevel(LogLevel level);
    LogLevel getLogLevel() const { return currentLevel; }
    
    void enableSerialLogging(bool enable);
    void enableFileLogging(bool enable);
    bool isSerialEnabled() const { return serialEnabled; }
    bool isFileLoggingEnabled() const { return fileLoggingEnabled; }
    
    // 日志方法
    void error(const String& message, const String& module = "");
    void warning(const String& message, const String& module = "");
    void info(const String& message, const String& module = "");
    void debug(const String& message, const String& module = "");
    void verbose(const String& message, const String& module = "");
    
    // 格式化日志
    void errorf(const char* format, ...);
    void warningf(const char* format, ...);
    void infof(const char* format, ...);
    void debugf(const char* format, ...);
    void verbosef(const char* format, ...);
    
    // 设置输出流（可选）
    void setOutputStream(Stream& stream) { outputStream = &stream; }
    
    // 日志文件管理
    bool rotateLogFile(const String& newName = "");
    size_t getLogFileSize() const;
    bool deleteLogFile();
    
private:
    LoggerSystem();  // 私有构造函数
    ~LoggerSystem() = default;
    
    void log(LogLevel level, const String& message, const String& module);
    void logFormatted(LogLevel level, const char* format, va_list args);
    String getLevelString(LogLevel level);
    String getTimestamp();
    
    // 成员变量
    LogLevel currentLevel;
    Stream* outputStream;
    bool serialEnabled;
    bool fileLoggingEnabled;
    mutable bool initialized;
};

// 可选的全局宏定义，方便使用
#define LOGGER LoggerSystem::getInstance()
#define LOG_ERROR(msg) LOGGER.error(msg, __FILE__)
#define LOG_WARNING(msg) LOGGER.warning(msg, __FILE__)
#define LOG_INFO(msg) LOGGER.info(msg, __FILE__)
#define LOG_DEBUG(msg) LOGGER.debug(msg, __FILE__)
#define LOG_VERBOSE(msg) LOGGER.verbose(msg, __FILE__)

#endif 