/**
 * @file MockLogger.h
 * @brief 日志系统模拟对象
 * 
 * 提供日志记录功能的模拟实现
 */

#ifndef MOCK_LOGGER_H
#define MOCK_LOGGER_H

#include <Arduino.h>
#include <vector>
#include <map>
#include <queue>

// 日志级别枚举
enum LogLevel {
    LOG_NONE = 0,
    LOG_ERROR = 1,
    LOG_WARNING = 2,
    LOG_INFO = 3,
    LOG_DEBUG = 4,
    LOG_VERBOSE = 5
};

// 日志条目结构
struct LogEntry {
    LogLevel level;
    String module;
    String message;
    unsigned long timestamp;
    
    LogEntry() : level(LOG_INFO), timestamp(0) {}
    
    LogEntry(LogLevel lvl, const String& mod, const String& msg, unsigned long ts)
        : level(lvl), module(mod), message(msg), timestamp(ts) {}
    
    String toString() const {
        const char* levelStr[] = {"NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE"};
        return "[" + String(levelStr[level]) + "][" + module + "] " + message;
    }
};

// 日志配置结构
struct LoggerConfig {
    LogLevel minLevel;
    bool logToSerial;
    bool logToFile;
    size_t maxFileSize;
    int maxFiles;
    std::vector<String> moduleBlacklist;
    
    LoggerConfig() : minLevel(LOG_INFO), logToSerial(true), logToFile(true),
                     maxFileSize(1024 * 1024), maxFiles(5) {}
};

// 模拟日志系统
class MockLoggerSystem {
public:
    static MockLoggerSystem& getInstance() {
        static MockLoggerSystem instance;
        return instance;
    }

    bool initialize() {
        _initialized = true;
        _config = LoggerConfig();
        _entries.clear();
        _currentFileSize = 0;
        _logFilePath = "/logs/system.log";
        return true;
    }

    // 配置
    void setLogLevel(LogLevel level) {
        _config.minLevel = level;
    }

    LogLevel getLogLevel() {
        return _config.minLevel;
    }

    void setConfig(const LoggerConfig& config) {
        _config = config;
    }

    LoggerConfig getConfig() {
        return _config;
    }

    // 日志记录
    void log(LogLevel level, const String& message, const String& module = "SYS") {
        if (level > _config.minLevel) return;
        if (isModuleBlacklisted(module)) return;
        
        LogEntry entry(level, module, message, millis());
        _entries.push_back(entry);
        
        // 输出到串口
        if (_config.logToSerial) {
            Serial.println(entry.toString());
        }
        
        // 模拟文件写入
        if (_config.logToFile) {
            _currentFileSize += entry.toString().length();
        }
    }

    void logError(const String& message, const String& module = "SYS") {
        log(LOG_ERROR, message, module);
    }

    void logWarning(const String& message, const String& module = "SYS") {
        log(LOG_WARNING, message, module);
    }

    void logInfo(const String& message, const String& module = "SYS") {
        log(LOG_INFO, message, module);
    }

    void logDebug(const String& message, const String& module = "SYS") {
        log(LOG_DEBUG, message, module);
    }

    void logVerbose(const String& message, const String& module = "SYS") {
        log(LOG_VERBOSE, message, module);
    }

    // 模块过滤
    void addModuleToBlacklist(const String& module) {
        for (auto& m : _config.moduleBlacklist) {
            if (m == module) return;
        }
        _config.moduleBlacklist.push_back(module);
    }

    void removeModuleFromBlacklist(const String& module) {
        for (auto it = _config.moduleBlacklist.begin(); 
             it != _config.moduleBlacklist.end(); ++it) {
            if (*it == module) {
                _config.moduleBlacklist.erase(it);
                return;
            }
        }
    }

    bool isModuleBlacklisted(const String& module) {
        for (auto& m : _config.moduleBlacklist) {
            if (m == module) return true;
        }
        return false;
    }

    // 日志查询
    std::vector<LogEntry> getEntries() {
        return _entries;
    }

    std::vector<LogEntry> getEntriesByLevel(LogLevel level) {
        std::vector<LogEntry> result;
        for (auto& entry : _entries) {
            if (entry.level == level) {
                result.push_back(entry);
            }
        }
        return result;
    }

    std::vector<LogEntry> getEntriesByModule(const String& module) {
        std::vector<LogEntry> result;
        for (auto& entry : _entries) {
            if (entry.module == module) {
                result.push_back(entry);
            }
        }
        return result;
    }

    std::vector<LogEntry> getRecentEntries(int count) {
        std::vector<LogEntry> result;
        int start = _entries.size() - count;
        if (start < 0) start = 0;
        
        for (int i = start; i < _entries.size(); i++) {
            result.push_back(_entries[i]);
        }
        return result;
    }

    // 日志文件管理
    size_t getLogFileSize() {
        return _currentFileSize;
    }

    bool clearLogFile() {
        _entries.clear();
        _currentFileSize = 0;
        return true;
    }

    // 日志轮转（模拟）
    bool rotateLogFile() {
        // 模拟日志轮转
        _entries.clear();
        _currentFileSize = 0;
        return true;
    }

    // 统计
    int getEntryCount() {
        return _entries.size();
    }

    int getEntryCountByLevel(LogLevel level) {
        int count = 0;
        for (auto& entry : _entries) {
            if (entry.level == level) count++;
        }
        return count;
    }

    void clear() {
        _entries.clear();
        _currentFileSize = 0;
    }

    // 格式化日志
    String formatLogEntry(const LogEntry& entry) {
        return entry.toString();
    }

    // 导出日志
    String exportLogs() {
        String result;
        for (auto& entry : _entries) {
            result += entry.toString() + "\n";
        }
        return result;
    }

private:
    MockLoggerSystem() : _initialized(false), _currentFileSize(0) {}

    bool _initialized;
    LoggerConfig _config;
    std::vector<LogEntry> _entries;
    size_t _currentFileSize;
    String _logFilePath;
};

// 全局实例引用
#define MockLogger MockLoggerSystem::getInstance()

#endif // MOCK_LOGGER_H
