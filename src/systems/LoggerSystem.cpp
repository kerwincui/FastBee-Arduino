/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:32:50
 */

#include "systems/LoggerSystem.h"
#include "utils/TimeUtils.h"
#include <Arduino.h>

LoggerSystem::LoggerSystem() {
    currentLevel = LOG_INFO;
    outputStream = &Serial;
    serialEnabled = true;
    fileLoggingEnabled = true;
    initialized = false;
}

bool LoggerSystem::initialize() {
    if (initialized) {
        return true;
    }
    
    if (serialEnabled) {
        Serial.begin(115200);
        delay(100);  // 给串口一些时间初始化
        // 等待串口连接（仅当需要调试时）
        #ifdef WAIT_FOR_SERIAL
        while (!Serial) {
            delay(10);
        }
        #endif
    }
    
    initialized = true;
    info("Logger system initialized", "Logger");
    return true;
}

void LoggerSystem::setLogLevel(LogLevel level) {
    currentLevel = level;
    String levelStr = getLevelString(level);
    info("Log level set to: " + levelStr, "Logger");
}

void LoggerSystem::enableSerialLogging(bool enable) {
    serialEnabled = enable;
    if (initialized && enable && !Serial) {
        Serial.begin(115200);
    }
}

void LoggerSystem::enableFileLogging(bool enable) {
    fileLoggingEnabled = enable;
    if (initialized && enable && !LittleFS.begin()) {
        error("Failed to initialize LittleFS for file logging", "Logger");
        fileLoggingEnabled = false;
    }
}

void LoggerSystem::error(const String& message, const String& module) {
    log(LOG_ERROR, message, module);
}

void LoggerSystem::warning(const String& message, const String& module) {
    log(LOG_WARNING, message, module);
}

void LoggerSystem::info(const String& message, const String& module) {
    log(LOG_INFO, message, module);
}

void LoggerSystem::debug(const String& message, const String& module) {
    log(LOG_DEBUG, message, module);
}

void LoggerSystem::verbose(const String& message, const String& module) {
    log(LOG_VERBOSE, message, module);
}

// 格式化日志方法
void LoggerSystem::errorf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    logFormatted(LOG_ERROR, format, args);
    va_end(args);
}

void LoggerSystem::warningf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    logFormatted(LOG_WARNING, format, args);
    va_end(args);
}

void LoggerSystem::infof(const char* format, ...) {
    va_list args;
    va_start(args, format);
    logFormatted(LOG_INFO, format, args);
    va_end(args);
}

void LoggerSystem::debugf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    logFormatted(LOG_DEBUG, format, args);
    va_end(args);
}

void LoggerSystem::verbosef(const char* format, ...) {
    va_list args;
    va_start(args, format);
    logFormatted(LOG_VERBOSE, format, args);
    va_end(args);
}

void LoggerSystem::log(LogLevel level, const String& message, const String& module) {
    if (level > currentLevel) {
        return;
    }
    
    String timestamp = getTimestamp();
    String levelStr = getLevelString(level);
    String logEntry = "[" + timestamp + "] [" + levelStr + "]";
    
    if (module.length() > 0) {
        logEntry += " [" + module + "]";
    }
    
    logEntry += " " + message;
    
    // 输出到串口
    if (serialEnabled && outputStream) {
        outputStream->println(logEntry);
    }
    
    // 输出到文件
    if (fileLoggingEnabled && initialized) {
        File logFile = LittleFS.open("/logs/system.log", FILE_APPEND);
        if (logFile) {
            logFile.println(logEntry);
            logFile.close();
        }
    }
}

void LoggerSystem::logFormatted(LogLevel level, const char* format, va_list args) {
    if (level > currentLevel) {
        return;
    }
    
    // 计算需要的缓冲区大小
    va_list argsCopy;
    va_copy(argsCopy, args);
    int needed = vsnprintf(nullptr, 0, format, argsCopy);
    va_end(argsCopy);
    
    if (needed < 0) {
        return;
    }
    
    // 创建缓冲区并格式化消息
    char* buffer = new char[needed + 1];
    vsnprintf(buffer, needed + 1, format, args);
    
    // 记录日志
    log(level, String(buffer), "");
    
    delete[] buffer;
}

String LoggerSystem::getLevelString(LogLevel level) {
    switch (level) {
        case LOG_ERROR: return "ERROR";
        case LOG_WARNING: return "WARN";
        case LOG_INFO: return "INFO";
        case LOG_DEBUG: return "DEBUG";
        case LOG_VERBOSE: return "VERBOSE";
        default: return "UNKNOWN";
    }
}

String LoggerSystem::getTimestamp() {
    return String(TimeUtils::getUptime());
}

bool LoggerSystem::rotateLogFile(const String& newName) {
    if (!fileLoggingEnabled || !LittleFS.exists("/logs/system.log")) {
        return false;
    }
    
    String backupName = newName;
    if (backupName.isEmpty()) {
        backupName = "/logs/system_" + String(millis()) + ".log";
    }
    
    return LittleFS.rename("/logs/system.log", backupName);
}

size_t LoggerSystem::getLogFileSize() const {
    if (!LittleFS.exists("/logs/system.log")) {
        return 0;
    }
    
    File logFile = LittleFS.open("/logs/system.log", FILE_READ);
    if (!logFile) {
        return 0;
    }
    
    size_t size = logFile.size();
    logFile.close();
    return size;
}

bool LoggerSystem::deleteLogFile() {
    if (LittleFS.exists("/logs/system.log")) {
        return LittleFS.remove("/logs/system.log");
    }
    return true;
}