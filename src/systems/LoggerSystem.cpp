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
#include "systems/HealthMonitor.h"
#include "core/FastBeeFramework.h"
#include "core/FeatureFlags.h"
#include <Arduino.h>
#include <vector>
#include <string.h>
#include <time.h>
#include <core/SystemConstants.h>

#if FASTBEE_ENABLE_LOGGER

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
      espLogCaptureEnabled(false),   // 默认禁用 ESP 日志捕获（降低 LittleFS/堆碎片压力）
      logFileSizeLimit(8192) {      // 限制日志文件大小为 8KB
    memset(_ringBuffer.data, 0, LOG_RING_BUFFER_SIZE);
}

LoggerSystem::~LoggerSystem() {
    // 关机前 flush 缓冲数据
    flushBuffer();
    if (_logMutex) {
        vSemaphoreDelete(_logMutex);
        _logMutex = nullptr;
    }
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

    // 创建日志互斥量
    if (!_logMutex) {
        _logMutex = xSemaphoreCreateMutex();
    }
    memset(_ringBuffer.data, 0, LOG_RING_BUFFER_SIZE);
    _ringBuffer.writePos = 0;
    _ringBuffer.readPos = 0;
    _ringBuffer.lastFlushMs = millis();

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

    // 写入环形缓冲而非直接写文件
    if (fileLoggingEnabled && initialized && _logMutex) {
        // MemGuard: 低内存时限制文件日志写入
        auto* fw = FastBeeFramework::getInstance();
        HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
        if (monitor) {
            if (monitor->isMemoryCritical()) {
                return;
            }
            if (monitor->isMemorySevere() && level < LOG_ERROR) {
                return;
            }
        }

        size_t len = strlen(entry);
        if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            logToBuffer(entry, len);
            logToBuffer("\n", 1);
            
            // 缓冲满 80% 时立即 flush
            if (bufferedDataSize() >= LOG_FLUSH_THRESHOLD) {
                flushBufferInternal();
            }
            xSemaphoreGive(_logMutex);
        }
    }
}

void LoggerSystem::logFormatted(LogLevel level, const char* format, va_list args) {
    if (level < currentLevel || !format) {
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

// ── 环形缓冲实现 ─────────────────────────────────────────────────────────

void LoggerSystem::logToBuffer(const char* entry, size_t len) {
    for (size_t i = 0; i < len; i++) {
        _ringBuffer.data[_ringBuffer.writePos] = entry[i];
        _ringBuffer.writePos = (_ringBuffer.writePos + 1) % LOG_RING_BUFFER_SIZE;
        // 写指针追上读指针，丢弃最旧数据
        if (_ringBuffer.writePos == _ringBuffer.readPos) {
            _ringBuffer.readPos = (_ringBuffer.readPos + 1) % LOG_RING_BUFFER_SIZE;
        }
    }
}

size_t LoggerSystem::bufferedDataSize() const {
    if (_ringBuffer.writePos >= _ringBuffer.readPos) {
        return _ringBuffer.writePos - _ringBuffer.readPos;
    }
    return LOG_RING_BUFFER_SIZE - _ringBuffer.readPos + _ringBuffer.writePos;
}

void LoggerSystem::checkLogFileSize() {
    if (getLogFileSize() >= logFileSizeLimit) {
        rotateLogFile();
    }
}

void LoggerSystem::flushBufferInternal() {
    size_t dataSize = bufferedDataSize();
    if (dataSize == 0) return;
    
    File logFile = LittleFS.open(LOG_FILE_PATH, FILE_APPEND);
    if (!logFile) {
        fileLoggingEnabled = false;
        if (serialEnabled && outputStream) {
            outputStream->println("[Logger] ERROR: Failed to open log file during flush, file logging disabled");
        }
        return;
    }
    
    // 批量写入
    if (_ringBuffer.readPos < _ringBuffer.writePos) {
        logFile.write((const uint8_t*)&_ringBuffer.data[_ringBuffer.readPos], dataSize);
    } else {
        // 环绕情况
        size_t tailSize = LOG_RING_BUFFER_SIZE - _ringBuffer.readPos;
        logFile.write((const uint8_t*)&_ringBuffer.data[_ringBuffer.readPos], tailSize);
        logFile.write((const uint8_t*)&_ringBuffer.data[0], _ringBuffer.writePos);
    }
    
    _ringBuffer.readPos = _ringBuffer.writePos;
    _ringBuffer.lastFlushMs = millis();
    logFile.close();
    
    // 检查文件大小（现有的 rotate 逻辑）
    checkLogFileSize();
}

void LoggerSystem::flushBuffer() {
    if (!_logMutex) return;
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        flushBufferInternal();
        xSemaphoreGive(_logMutex);
    }
}

// ── 日志文件管理 ─────────────────────────────────────────────────────────

// 清理多余的日志备份文件，保留最新的MAX_LOG_FILES个
void LoggerSystem::cleanupOldLogFiles() {
    constexpr size_t MAX_LOG_FILES = 20;
    
    // 收集所有 system_*.log 备份文件
    std::vector<String> logFiles;
    File root = LittleFS.open("/logs");
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        return;
    }
    
    File file = root.openNextFile();
    while (file) {
        const char* name = file.name();
        // 只收集 system_*.log 备份文件（不含当前日志文件）
        if (strncmp(name, "system_", 7) == 0 && strstr(name, ".log") != nullptr) {
            logFiles.push_back(String("/logs/") + name);
        }
        file = root.openNextFile();
    }
    root.close();
    
    // 如果文件数量超过限制，删除最旧的
    while (logFiles.size() > MAX_LOG_FILES) {
        // 文件名包含时间戳，按字母序排序即可找到最旧的
        String oldest = logFiles[0];
        for (const auto& f : logFiles) {
            if (f < oldest) {
                oldest = f;
            }
        }
        LittleFS.remove(oldest);
        // 从列表中移除
        for (auto it = logFiles.begin(); it != logFiles.end(); ++it) {
            if (*it == oldest) {
                logFiles.erase(it);
                break;
            }
        }
    }
}

bool LoggerSystem::rotateLogFile(const char* newName) {
    if (!fileLoggingEnabled || !LittleFS.exists(LOG_FILE_PATH)) {
        return false;
    }

    // 临时禁用文件日志，确保 espLogCallback / writeRawLog 等并发路径
    // 不会在 rename 期间持有文件句柄（修复 "Cannot rename; src is open" 错误）
    fileLoggingEnabled = false;
    delay(5);  // 等待正在进行的文件写入完成

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
        // 清理多余的旧日志文件，保留最新的10个
        cleanupOldLogFiles();
    } else {
        // rename 失败（文件可能仍被系统内部引用），回退为截断清空
        File logFile = LittleFS.open(LOG_FILE_PATH, FILE_WRITE);
        if (logFile) {
            logFile.close();
        }
        if (serialEnabled && outputStream) {
            outputStream->println("[Logger] WARN: rename failed, log file truncated instead");
        }
        renamed = true;  // 截断视为成功
    }

    // 恢复文件日志
    fileLoggingEnabled = true;
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
    
    // 写入到日志文件（通过环形缓冲）
    if (logger.fileLoggingEnabled && logger.initialized && logger._logMutex) {
        size_t bufLen = strlen(buffer);
        if (xSemaphoreTake(logger._logMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            logger.logToBuffer(buffer, bufLen);
            logger.logToBuffer("\n", 1);
            if (logger.bufferedDataSize() >= LOG_FLUSH_THRESHOLD) {
                logger.flushBufferInternal();
            }
            xSemaphoreGive(logger._logMutex);
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
    
    // 通过环形缓冲写入
    if (_logMutex && xSemaphoreTake(_logMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        size_t len = strlen(message);
        logToBuffer(message, len);
        logToBuffer("\n", 1);
        if (bufferedDataSize() >= LOG_FLUSH_THRESHOLD) {
            flushBufferInternal();
        }
        xSemaphoreGive(_logMutex);
    }
}

#endif // FASTBEE_ENABLE_LOGGER
