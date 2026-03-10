/**
 * @description: 日志系统实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:32:50
 *
 * 优化说明：
 *  1. log() 改为 const char* 参数，消除 String 临时对象创建
 *  2. 内部格式化使用固定大小栈缓冲区，不再 new/delete 堆内存
 *  3. 日志条目拼接改为 snprintf 到栈缓冲区，替代 String 多次 += 拼接
 *  4. 文件路径使用字面量，不做运行时拼接
 *  5. getLevelString() 改为返回 const char*，避免 String 返回值拷贝
 *  6. 支持捕获 ESP-IDF 系统日志到文件
 */

#include "systems/LoggerSystem.h"
#include <Arduino.h>
#include <vector>
#include <string.h>
#include <time.h>
#include <core/SystemConstants.h>

// 日志文件固定路径（与 SystemConstants::FileSystem 对应）
static constexpr const char* LOG_FILE_PATH = "/logs/system.log";

// 默认日志文件大小限制（1MB）
static constexpr size_t DEFAULT_LOG_FILE_SIZE_LIMIT = 1024 * 1024;

// 静态成员初始化
vprintf_like_t LoggerSystem::originalEspLogFunc = nullptr;

// ── 构造 / 初始化 ─────────────────────────────────────────────────────────

LoggerSystem::LoggerSystem()
    : currentLevel(LOG_DEBUG),  // 调试阶段显示所有日志
      outputStream(&Serial),
      serialEnabled(true),
      fileLoggingEnabled(true),   // 启用文件日志
      initialized(false),
      espLogCaptureEnabled(true),   // 默认启用 ESP 日志捕获
      logFileSizeLimit(8192) {      // 限制日志文件大小为 8KB
}

bool LoggerSystem::initialize() {
    if (initialized) {
        return true;
    }

    // Serial 由 main.cpp::setup() 初始化，LoggerSystem 直接使用已有串口实例。
    // 仅在 Serial 尚未就绪时（独立单元测试或忘记在 setup() 中调用 Serial.begin）才补充初始化。
    if (serialEnabled && !Serial) {
        Serial.begin(Hardware::SERIAL_BAUDRATE);
        delay(100);
#ifdef WAIT_FOR_SERIAL
        while (!Serial) { delay(10); }
#endif
    }

    initialized = true;
    
    // 检查文件系统状态
    if (fileLoggingEnabled) {
        size_t total = LittleFS.totalBytes();
        size_t used = LittleFS.usedBytes();
        size_t freeSpace = (total > used) ? (total - used) : 0;
        
        Serial.printf("[Logger] FileSystem: total=%lu, used=%lu, free=%lu\n", 
                      (unsigned long)total, (unsigned long)used, 
                      (unsigned long)freeSpace);
        
        if (total == 0) {
            fileLoggingEnabled = false;
            Serial.println("[Logger] WARNING: LittleFS not mounted, file logging disabled");
        } else if (freeSpace < 1024) {
            // 空闲空间小于 1KB 时禁用文件日志
            fileLoggingEnabled = false;
            Serial.println("[Logger] WARNING: LittleFS full (free < 1KB), file logging disabled");
        } else {
            Serial.printf("[Logger] File logging enabled, log file: %s\n", LOG_FILE_PATH);
        }
    }
    
    // 启用 ESP-IDF 日志捕获
    if (espLogCaptureEnabled) {
        enableEspLogCapture(true);
        Serial.println("[Logger] ESP-IDF log capture enabled");
    }
    
    info("Logger system initialized", "Logger");
    return true;
}

// ── 级别设置 ─────────────────────────────────────────────────────────────

void LoggerSystem::setLogLevel(LogLevel level) {
    currentLevel = level;
    infof("[Logger] Log level set to: %s", getLevelString(level));
}

void LoggerSystem::enableSerialLogging(bool enable) {
    serialEnabled = enable;
    if (initialized && enable && !Serial) {
        Serial.begin(115200);
    }
}

void LoggerSystem::enableFileLogging(bool enable) {
    fileLoggingEnabled = enable;
    // LittleFS 由 ConfigStorage 统一挂载，此处只检测是否已挂载
    if (initialized && enable && LittleFS.totalBytes() == 0) {
        fileLoggingEnabled = false;
        error("LittleFS not mounted, file logging disabled", "Logger");
    }
}

// ── const char* 主接口 ────────────────────────────────────────────────────

void LoggerSystem::error  (const char* message, const char* module) { log(LOG_ERROR,   message, module); }
void LoggerSystem::warning(const char* message, const char* module) { log(LOG_WARNING, message, module); }
void LoggerSystem::info   (const char* message, const char* module) { log(LOG_INFO,    message, module); }
void LoggerSystem::debug  (const char* message, const char* module) { log(LOG_DEBUG,   message, module); }
void LoggerSystem::verbose(const char* message, const char* module) { log(LOG_VERBOSE, message, module); }

// ── printf 风格格式化接口 ─────────────────────────────────────────────────

void LoggerSystem::errorf(const char* format, ...) {
    va_list args; va_start(args, format);
    logFormatted(LOG_ERROR, format, args);
    va_end(args);
}

void LoggerSystem::warningf(const char* format, ...) {
    va_list args; va_start(args, format);
    logFormatted(LOG_WARNING, format, args);
    va_end(args);
}

void LoggerSystem::infof(const char* format, ...) {
    va_list args; va_start(args, format);
    logFormatted(LOG_INFO, format, args);
    va_end(args);
}

void LoggerSystem::debugf(const char* format, ...) {
    va_list args; va_start(args, format);
    logFormatted(LOG_DEBUG, format, args);
    va_end(args);
}

void LoggerSystem::verbosef(const char* format, ...) {
    va_list args; va_start(args, format);
    logFormatted(LOG_VERBOSE, format, args);
    va_end(args);
}

// ── 内部实现 ─────────────────────────────────────────────────────────────

void LoggerSystem::log(LogLevel level, const char* message, const char* module) {
    // 日志级别：LOG_DEBUG(0) < LOG_INFO(1) < LOG_WARNING(2) < LOG_ERROR(3)
    // 只输出 >= currentLevel 的日志（数值越大越重要）
    if (level < currentLevel || !message) {
        return;
    }

    // 检查模块是否被过滤
    if (isModuleFiltered(module)) {
        return;
    }

    // 使用栈缓冲区拼接日志条目，避免 String 堆碎片
    // 格式：[YYYY-MM-DD HH:MM:SS] [LEVEL] [MODULE] message
    char entry[LOG_BUF_SIZE];
    const char* levelStr = getLevelString(level);
    
    // 获取当前时间
    struct tm timeinfo;
    char timeStr[32] = "1970-01-01 00:00:00";
    if (getLocalTime(&timeinfo, 0)) {
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    }

    if (module && module[0] != '\0') {
        snprintf(entry, sizeof(entry), "[%s] [%s] [%s] %s",
                 timeStr, levelStr, module, message);
    } else {
        snprintf(entry, sizeof(entry), "[%s] [%s] %s",
                 timeStr, levelStr, message);
    }

    // 输出到串口
    if (serialEnabled && outputStream) {
        outputStream->println(entry);
    }

    // 追加写入日志文件（不频繁 begin/end，LittleFS 挂载后直接 open）
    if (fileLoggingEnabled && initialized) {
        // 检查日志文件大小，如果超过限制则旋转
        if (getLogFileSize() >= logFileSizeLimit) {
            if (rotateLogFile()) {
                info("Log file rotated due to size limit", "Logger");
            }
        }

        File logFile = LittleFS.open(LOG_FILE_PATH, FILE_APPEND);
        if (logFile) {
            logFile.println(entry);
            logFile.close();
        }
    }
}

void LoggerSystem::logFormatted(LogLevel level, const char* format, va_list args) {
    if (level > currentLevel || !format) {
        return;
    }

    // 格式化到栈缓冲区（固定大小，无堆分配）
    char msg[LOG_BUF_SIZE];
    vsnprintf(msg, sizeof(msg), format, args);

    log(level, msg, "");
}

// static：返回字符串字面量，无内存分配
const char* LoggerSystem::getLevelString(LogLevel level) {
    switch (level) {
        case LOG_ERROR:   return "ERROR";
        case LOG_WARNING: return "WARN ";
        case LOG_INFO:    return "INFO ";
        case LOG_DEBUG:   return "DEBUG";
        case LOG_VERBOSE: return "VERB ";
        default:          return "?????";
    }
}

// ── 日志文件管理 ─────────────────────────────────────────────────────────

bool LoggerSystem::rotateLogFile(const char* newName) {
    if (!fileLoggingEnabled || !LittleFS.exists(LOG_FILE_PATH)) {
        return false;
    }

    bool renamed = false;
    if (newName && newName[0] != '\0') {
        renamed = LittleFS.rename(LOG_FILE_PATH, newName);
    } else {
        // 生成带时间戳的备份文件名
        char backup[48];
        snprintf(backup, sizeof(backup), "/logs/system_%lu.log", millis());
        renamed = LittleFS.rename(LOG_FILE_PATH, backup);
    }

    if (renamed) {
        // rename 后原路径不再存在，用 FILE_WRITE 创建新的空日志文件
        File newLog = LittleFS.open(LOG_FILE_PATH, FILE_WRITE);
        if (newLog) {
            newLog.close();
        }
    }
    return renamed;
}

size_t LoggerSystem::getLogFileSize() const {
    if (!LittleFS.exists(LOG_FILE_PATH)) {
        return 0;
    }

    File logFile = LittleFS.open(LOG_FILE_PATH, FILE_READ);
    if (!logFile) {
        return 0;
    }

    size_t sz = logFile.size();
    logFile.close();
    return sz;
}

bool LoggerSystem::deleteLogFile() {
    // 清空日志文件内容，但保留文件本身
    File logFile = LittleFS.open(LOG_FILE_PATH, FILE_WRITE);  // FILE_WRITE 会清空文件
    if (logFile) {
        logFile.close();
        return true;
    }
    return false;
}

// ── 日志文件大小限制 ───────────────────────────────────────────────────────

void LoggerSystem::setLogFileSizeLimit(size_t limit) {
    logFileSizeLimit = limit;
    infof("[Logger] Log file size limit set to: %zu bytes", limit);
}

size_t LoggerSystem::getLogFileSizeLimit() const {
    return logFileSizeLimit;
}

// ── 日志模块过滤 ───────────────────────────────────────────────────────────

void LoggerSystem::addLogModuleFilter(const char* module) {
    if (module && module[0] != '\0') {
        moduleFilters.push_back(module);
        infof("[Logger] Added log module filter: %s", module);
    }
}

void LoggerSystem::removeLogModuleFilter(const char* module) {
    if (module && module[0] != '\0') {
        for (auto it = moduleFilters.begin(); it != moduleFilters.end(); ++it) {
            if (strcmp(*it, module) == 0) {
                moduleFilters.erase(it);
                infof("[Logger] Removed log module filter: %s", module);
                break;
            }
        }
    }
}

void LoggerSystem::clearLogModuleFilters() {
    moduleFilters.clear();
    infof("[Logger] Cleared all log module filters");
}

bool LoggerSystem::isModuleFiltered(const char* module) const {
    if (!module || module[0] == '\0') {
        return false;
    }
    for (const auto& filteredModule : moduleFilters) {
        if (strcmp(filteredModule, module) == 0) {
            return true;
        }
    }
    return false;
}

// ── ESP-IDF 日志捕获 ─────────────────────────────────────────────────────

/**
 * ESP-IDF 日志回调函数
 * 捕获所有 ESP_LOGI, ESP_LOGW, ESP_LOGE 等日志
 */
int LoggerSystem::espLogCallback(const char* format, va_list args) {
    LoggerSystem& logger = getInstance();
    
    // 格式化日志消息到临时缓冲区
    char buffer[512];
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    
    // 去除末尾换行符
    if (len > 0 && buffer[len-1] == '\n') {
        buffer[len-1] = '\0';
    }
    
    // 写入到日志文件
    if (logger.fileLoggingEnabled && logger.initialized) {
        // 检查日志文件大小
        if (logger.getLogFileSize() >= logger.logFileSizeLimit) {
            logger.rotateLogFile();
        }
        
        File logFile = LittleFS.open(LOG_FILE_PATH, FILE_APPEND);
        if (logFile) {
            logFile.println(buffer);
            logFile.close();
        }
    }
    
    // 同时输出到原始串口（保持串口输出）
    if (originalEspLogFunc) {
        // 重新创建 va_list 因为已经被使用过了
        return Serial.printf("%s\n", buffer);
    }
    
    return len;
}

/**
 * 启用/禁用 ESP-IDF 日志捕获
 */
void LoggerSystem::enableEspLogCapture(bool enable) {
    espLogCaptureEnabled = enable;
    
    if (enable) {
        // 保存原始输出函数并设置自定义回调
        if (!originalEspLogFunc) {
            originalEspLogFunc = esp_log_set_vprintf(espLogCallback);
            infof("[Logger] ESP-IDF log capture enabled");
        }
    } else {
        // 恢复原始输出函数
        if (originalEspLogFunc) {
            esp_log_set_vprintf(originalEspLogFunc);
            originalEspLogFunc = nullptr;
            infof("[Logger] ESP-IDF log capture disabled");
        }
    }
}

/**
 * 写入原始日志行（不添加格式化前缀）
 * 用于捕获其他来源的日志
 */
void LoggerSystem::writeRawLog(const char* message) {
    if (!message || !fileLoggingEnabled || !initialized) {
        return;
    }
    
    // 检查日志文件大小
    if (getLogFileSize() >= logFileSizeLimit) {
        rotateLogFile();
    }
    
    File logFile = LittleFS.open(LOG_FILE_PATH, FILE_APPEND);
    if (logFile) {
        logFile.println(message);
        logFile.close();
    }
}
