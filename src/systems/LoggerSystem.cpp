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
#include "core/MemoryBudget.h"
#include "core/FastBeeFramework.h"
#include "core/ChipConfig.h"
#include "core/FeatureFlags.h"
#include <Arduino.h>
#include <vector>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <new>
#include <freertos/task.h>
#include <core/SystemConstants.h>

#if FASTBEE_ENABLE_LOGGER

// 日志文件固定路径（与 SystemConstants::FileSystem 对应）
static constexpr const char* LOG_FILE_PATH = "/logs/system.log";
static constexpr const char* LOG_DIR_PATH = "/logs";
static constexpr const char* LOG_FILE_NAME = "system.log";

namespace {

const char* logBaseName(const char* path) {
    if (!path) return "";
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool parseBackupSequence(const char* path, uint64_t& sequence) {
    const char* name = logBaseName(path);
    static constexpr const char* PREFIX = "system_";
    static constexpr const char* SUFFIX = ".log";
    const size_t prefixLen = strlen(PREFIX);

    if (strncmp(name, PREFIX, prefixLen) != 0) return false;

    const char* digits = name + prefixLen;
    const char* suffix = strstr(digits, SUFFIX);
    if (!suffix || suffix == digits || suffix[strlen(SUFFIX)] != '\0') return false;

    uint64_t value = 0;
    for (const char* p = digits; p < suffix; ++p) {
        if (*p < '0' || *p > '9') return false;
        const uint8_t digit = static_cast<uint8_t>(*p - '0');
        if (value > (UINT64_MAX - digit) / 10U) return false;
        value = value * 10U + digit;
    }

    sequence = value;
    return true;
}

bool isSafeLogFileName(const char* fileName) {
    if (!fileName || fileName[0] == '\0') return false;
    if (strchr(fileName, '/') || strchr(fileName, '\\')) return false;
    const size_t len = strlen(fileName);
    return len > 4 && strcmp(fileName + len - 4, ".log") == 0;
}

bool buildSafeLogPath(const char* value, char* out, size_t outSize) {
    if (!value || !out || outSize == 0) return false;

    static constexpr const char* PREFIX = "/logs/";
    const size_t prefixLen = strlen(PREFIX);
    const char* fileName = value;
    if (strncmp(value, PREFIX, prefixLen) == 0) {
        fileName = value + prefixLen;
    } else if (value[0] == '/') {
        return false;
    }

    if (!isSafeLogFileName(fileName)) return false;
    const int written = snprintf(out, outSize, "%s/%s", LOG_DIR_PATH, fileName);
    return written > 0 && static_cast<size_t>(written) < outSize;
}

bool findNextBackupSequence(uint64_t& nextSequence) {
    uint64_t maxSequence = 0;
    bool found = false;

    File root = LittleFS.open(LOG_DIR_PATH);
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        return false;
    }

    File file = root.openNextFile();
    while (file) {
        uint64_t sequence = 0;
        if (parseBackupSequence(file.name(), sequence) && (!found || sequence > maxSequence)) {
            maxSequence = sequence;
            found = true;
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();

    if (found && maxSequence == UINT64_MAX) return false;
    nextSequence = found ? maxSequence + 1U : 1U;
    return true;
}

}  // namespace

// Logger starts before features.json is loaded. Optional file/recent buffers
// are admitted only after the saved user choice and runtime budget are known.
static constexpr bool LOGGER_STARTUP_FILE_LOGGING = false;

// 静态成员初始化
vprintf_like_t LoggerSystem::originalEspLogFunc = nullptr;

// ── 构造 / 初始化 ─────────────────────────────────────────────────────────

LoggerSystem::LoggerSystem()
    : currentLevel(LOG_INFO),  // 生产默认 INFO 级别（启动后由 device.json 覆盖）
      outputStream(&Serial),
      serialEnabled(true),
      fileLoggingState(static_cast<uint32_t>(LOGGER_STARTUP_FILE_LOGGING
          ? FileLoggingState::Pending : FileLoggingState::Disabled)),
      initialized(false),
      espLogCaptureEnabled(false),   // 默认禁用 ESP 日志捕获（降低 LittleFS/堆碎片压力）
      logFileSizeLimit(CHIP_DEFAULT_LOG_FILE_SIZE_LIMIT) {}

LoggerSystem::~LoggerSystem() {
#if FASTBEE_USE_PSRAM
    // Close admission before joining: even a queued task still owns this object.
    _flushTaskState.fetch_or(LOG_FLUSH_TASK_STOPPING);
#endif
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
    initialized = true;

    // 检查文件系统状态
    // 保留 Serial 直出（D5 白名单）：LoggerSystem 初始化自检，不经过自身日志路径。
    if (isFileLoggingRequested()) {
        size_t total = LittleFS.totalBytes();
        size_t used = LittleFS.usedBytes();
        size_t freeSpace = (total > used) ? (total - used) : 0;

        Serial.printf("[Logger] FileSystem: total=%lu, used=%lu, free=%lu\n",
                      (unsigned long)total, (unsigned long)used,
                      (unsigned long)freeSpace);

        if (total == 0) {
            cancelFileLoggingRequest();
            Serial.println("[Logger] WARNING: LittleFS not mounted, file logging disabled");
        } else if (freeSpace < 1024) {
            // 空闲空间小于 1KB 时禁用文件日志
            cancelFileLoggingRequest();
            Serial.println("[Logger] WARNING: LittleFS full (free < 1KB), file logging disabled");
        } else {
            Serial.printf("[Logger] File logging enabled, log file: %s\n", LOG_FILE_PATH);
        }
    }

    // Allocate only after storage and the mutex have been checked; OFF boot
    // keeps the diagnostic ring, but consumes no file ring heap.
    enableFileLogging(isFileLoggingRequested());
    enableLogViewer(isLogViewerRequested());

    // 启用 ESP-IDF 日志捕获
    if (espLogCaptureEnabled) {
        enableEspLogCapture(true);
        // 保留 Serial 直出（D5 白名单）：LoggerSystem 初始化自检，不经过自身日志路径。
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

void LoggerSystem::cancelFileLoggingRequest() {
    uint32_t previous = fileLoggingState.load();
    while (!fileLoggingState.compare_exchange_weak(previous,
               (previous + FILE_LOGGING_STATE_MASK + 1U) & ~FILE_LOGGING_STATE_MASK)) {}
}

void LoggerSystem::enableLogViewer(bool enable) {
    uint32_t request = _recentLogsState.load();
    if (!enable) {
        // Cancel before taking the lock. Readers already holding it retain
        // their buffer until unlock; no new producer may retain ordinary lines.
        while (!_recentLogsState.compare_exchange_weak(request,
            (request + RECENT_LOG_STATE_MASK + 1U) & ~RECENT_LOG_STATE_MASK)) {}
    } else {
        for (;;) {
            const auto state = static_cast<RecentLogState>(request & RECENT_LOG_STATE_MASK);
            if (state == RecentLogState::Enabled) return;
            if (state == RecentLogState::Pending) break;
            const uint32_t pending = (request & ~RECENT_LOG_STATE_MASK) |
                static_cast<uint32_t>(RecentLogState::Pending);
            if (_recentLogsState.compare_exchange_weak(request, pending)) {
                request = pending;
                break;
            }
        }
    }
    if (!initialized || !_logMutex) return;
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    bool allocationFailed = false;
    if (enable && _recentLogsState.load() == request) {
        if (!_recentLogBuffer) {
            _recentLogBuffer.reset(new (std::nothrow) RecentLogBuffer());
        }
        if (_recentLogBuffer) {
            // A disable/re-enable while a reader held the mutex may have left
            // a retained allocation. New intent starts a fresh ordinary ring.
            _recentLogBuffer->writeIndex = 0;
            _recentLogBuffer->count = 0;
        }
        const uint32_t completed = (request & ~RECENT_LOG_STATE_MASK) |
            static_cast<uint32_t>(_recentLogBuffer ? RecentLogState::Enabled : RecentLogState::AllocationFailed);
        allocationFailed = !_recentLogBuffer;
        _recentLogsState.compare_exchange_strong(request, completed);
    }
    unlockLogFiles();
    if (allocationFailed && serialEnabled && outputStream) {
        outputStream->println("[Logger] WARNING: recent log allocation failed; critical diagnostics retained");
    }
}

void LoggerSystem::enableFileLogging(bool enable) {
    // LittleFS 由 ConfigStorage 统一挂载，此处只检测是否已挂载
    const bool storageReady = !enable || !initialized || LittleFS.totalBytes() != 0;
    const bool targetState = enable && storageReady;
    if (!targetState) cancelFileLoggingRequest();
    uint32_t request = fileLoggingState.load();
    if (targetState) {
        // Concurrent ON requests share one intent. A timeout must not cancel
        // another caller's allocation, or disturb an already active logger.
        for (;;) {
            const auto state = static_cast<FileLoggingState>(request & FILE_LOGGING_STATE_MASK);
            if (state == FileLoggingState::Enabled) return;
            if (state == FileLoggingState::Pending) break;
            const uint32_t pending = request | static_cast<uint32_t>(FileLoggingState::Pending);
            if (fileLoggingState.compare_exchange_weak(request, pending)) {
                request = pending;
                break;
            }
        }
    }

    if (!initialized) {
        // Save intent only; initialize owns allocation after checking storage.
        return;
    }
    // A timed-out disable must stop new writers without freeing a buffer that
    // another task still owns. Its next unlock/flush reclaims the buffer.
    if (!_logMutex) {
        cancelFileLoggingRequest();
    } else if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (targetState && fileLoggingState == request) {
            if (!_ringBuffer) {
                _ringBuffer.reset(new (std::nothrow) LogRingBuffer());
            }
            if (_ringBuffer) {
                // A previous lock-timeout disable may have left deferred data.
                _ringBuffer->readPos = _ringBuffer->writePos;
                _ringBuffer->lastFlushMs = millis();
            }
            // A disable can time out while allocation holds this mutex. Only
            // publish ON if no such disable canceled the pending request.
            const uint32_t completed = _ringBuffer
                ? ((request & ~FILE_LOGGING_STATE_MASK) | static_cast<uint32_t>(FileLoggingState::Enabled))
                : ((request + FILE_LOGGING_STATE_MASK + 1U) & ~FILE_LOGGING_STATE_MASK);
            fileLoggingState.compare_exchange_strong(request, completed);
        }
        unlockLogFiles();
    }

    if (enable && !storageReady) {
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

    // Only warning/error lines consume the resident diagnostic ring. The
    // optional recent ring retains all admitted levels while logViewer runs.
    captureRecentLine(entry, level >= LOG_WARNING);

    // 输出到串口
    if (serialEnabled && outputStream) {
        outputStream->println(entry);
    }

    // 写入环形缓冲而非直接写文件
    if (isFileLoggingEnabled() && initialized && _logMutex) {
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
            // 状态可能在等待锁期间被关闭，进入临界区后必须再次确认。
            if (isFileLoggingEnabled()) {
                logToBuffer(entry, len);
                logToBuffer("\n", 1);

                // File I/O and rotation belong to the periodic consumer. A
                // Modbus worker can already be deep in its execution stack.
            }
            unlockLogFiles();
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
    if (!_ringBuffer || !isFileLoggingEnabled()) return;
    for (size_t i = 0; i < len; i++) {
        _ringBuffer->data[_ringBuffer->writePos] = entry[i];
        _ringBuffer->writePos = (_ringBuffer->writePos + 1) % LOG_RING_BUFFER_SIZE;
        // 写指针追上读指针，丢弃最旧数据
        if (_ringBuffer->writePos == _ringBuffer->readPos) {
            _ringBuffer->readPos = (_ringBuffer->readPos + 1) % LOG_RING_BUFFER_SIZE;
        }
    }
}

size_t LoggerSystem::bufferedDataSize() const {
    if (!_ringBuffer) return 0;
    if (_ringBuffer->writePos >= _ringBuffer->readPos) {
        return _ringBuffer->writePos - _ringBuffer->readPos;
    }
    return LOG_RING_BUFFER_SIZE - _ringBuffer->readPos + _ringBuffer->writePos;
}

void LoggerSystem::checkLogFileSize() {
    // 内存保护：低内存时跳过文件大小检查，避免 VFS abort()
    // 2026-08-05：改用 8BIT 口径（INTERNAL 虚高 ~28KB，守卫失效）
    if (FastBee::MemoryBudget::dram8Free() < 10240) return;
    if (getLogFileSize() >= logFileSizeLimit) {
        rotateLogFileInternal(nullptr);
    }
}

void LoggerSystem::flushBufferInternal() {
    if (!isFileLoggingEnabled()) return;

    size_t dataSize = bufferedDataSize();
    if (dataSize == 0) return;

    // 内存保护：VFS 打开文件需要分配互斥锁，堆不足时会触发 abort()
    // 2026-08-05：改用 8BIT 口径。exec 规则窗口内 INTERNAL 仍 42KB 但
    // 8BIT 只剩 ~6KB，旧门控放行导致 LittleFS 写入继续挤占 8BIT。
    if (FastBee::MemoryBudget::dram8Free() < 10240) {
        return;  // 内存不足，跳过本次flush，数据保留在环形缓冲区中
    }

    File logFile = LittleFS.open(LOG_FILE_PATH, FILE_APPEND);
    if (!logFile) {
        cancelFileLoggingRequest();
        // 文件系统故障后丢弃待写数据，避免定时任务持续重试同一缓冲。
        _ringBuffer->readPos = _ringBuffer->writePos;
        _ringBuffer->lastFlushMs = millis();
        if (serialEnabled && outputStream) {
            outputStream->println("[Logger] ERROR: Failed to open log file during flush, file logging disabled");
        }
        return;
    }

    // 批量写入
    if (_ringBuffer->readPos < _ringBuffer->writePos) {
        logFile.write((const uint8_t*)&_ringBuffer->data[_ringBuffer->readPos], dataSize);
    } else {
        // 环绕情况
        size_t tailSize = LOG_RING_BUFFER_SIZE - _ringBuffer->readPos;
        logFile.write((const uint8_t*)&_ringBuffer->data[_ringBuffer->readPos], tailSize);
        logFile.write((const uint8_t*)&_ringBuffer->data[0], _ringBuffer->writePos);
    }

    _ringBuffer->readPos = _ringBuffer->writePos;
    _ringBuffer->lastFlushMs = millis();
    logFile.close();
    // 记录实际落盘时间，PointHistory 借此把周期落盘合并进同一 Flash 写窗口。
    _lastFlashWriteMs.store(_ringBuffer->lastFlushMs);

    // 检查文件大小（现有的 rotate 逻辑）
    checkLogFileSize();
}

void LoggerSystem::flushBuffer() {
#if FASTBEE_USE_PSRAM
    // Explicit shutdown/file-management flush must include a queued drain and
    // must not destroy its mutex or buffer while LittleFS still uses them.
    while (_flushTaskState.load() & LOG_FLUSH_TASK_ACTIVE) delay(1);
#endif
    if (!_logMutex) return;
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        flushBufferInternal();
        unlockLogFiles();
    }
}

void LoggerSystem::flushBufferIfDue() {
    if (!_logMutex || !isFileLoggingEnabled()) return;
    if (xSemaphoreTake(_logMutex, 0) == pdTRUE) {
        if (_ringBuffer && isFileLoggingEnabled() &&
            (bufferedDataSize() >= LOG_FLUSH_THRESHOLD ||
             millis() - _ringBuffer->lastFlushMs >= LOG_FLUSH_INTERVAL_MS)) {
#if FASTBEE_USE_PSRAM
            // Reserve internal RAM for the stack/TCB plus the existing VFS
            // low-memory guard. Failure retains data; never write on loopTask.
            if (bufferedDataSize() != 0 &&
                FastBee::MemoryBudget::dram8Free() >= LOG_FLUSH_TASK_STACK_BYTES + 16384U &&
                FastBee::MemoryBudget::dram8Largest() >= LOG_FLUSH_TASK_STACK_BYTES + 1024U) {
                uint32_t idle = 0;
                if (_flushTaskState.compare_exchange_strong(idle, LOG_FLUSH_TASK_ACTIVE)) {
                    _flushTaskGeneration = fileLoggingState.load();
                    if (xTaskCreate(flushTask, "log_flush", LOG_FLUSH_TASK_STACK_BYTES,
                                    this, 0, nullptr) != pdPASS) {
                        _flushTaskState.fetch_and(~LOG_FLUSH_TASK_ACTIVE);
                    }
                }
            }
#else
            flushBufferInternal();
#endif
        }
        unlockLogFiles();
    }
}

#if FASTBEE_USE_PSRAM
void LoggerSystem::flushTask(void* context) {
    auto* logger = static_cast<LoggerSystem*>(context);
    const unsigned long started = millis();
    bool attempted = false;
    if (xSemaphoreTake(logger->_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // A disable/re-enable cancels old work even when it was still queued.
        if (logger->fileLoggingState.load() == logger->_flushTaskGeneration &&
            logger->isFileLoggingEnabled() && logger->bufferedDataSize() != 0) {
            attempted = true;
            logger->flushBufferInternal();
        }
        logger->unlockLogFiles();
    }
    if (attempted) {
        // 保留 Serial 直出（D5 白名单）：flush 任务栈指标，避免在 flush 任务内
        // 递归经过 LoggerSystem（互斥锁已被该任务持有/竞争）。
        Serial.printf("[LOG_FLUSH_METRICS] elapsed=%lu stackFree=%lu\n",
                      millis() - started,
                      static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
    }
    // Last access to the object: its destructor may continue after this store.
    logger->_flushTaskState.fetch_and(~LOG_FLUSH_TASK_ACTIVE);
    vTaskDelete(nullptr);
}
#endif

// ── 诊断最近日志环形实现 ─────────────────────────────────────────

void LoggerSystem::appendRecentLine(char (*lines)[128], size_t capacity,
                                   uint8_t& writeIndex, uint8_t& count, const char* entry) {
    size_t len = strlen(entry);
    if (len >= RECENT_LOG_LINE_LEN) len = RECENT_LOG_LINE_LEN - 1;
    char* slot = lines[writeIndex];
    memcpy(slot, entry, len);
    slot[len] = '\0';
    writeIndex = static_cast<uint8_t>((writeIndex + 1) % capacity);
    if (count < capacity) ++count;
}

void LoggerSystem::captureRecentLine(const char* entry, bool critical) {
    if (!entry || entry[0] == '\0') return;
    if (!critical && !isLogViewerEnabled()) return;
    // 短超时拿锁：拿不到则跳过，避免阻塞日志热路径
    if (!_logMutex || xSemaphoreTake(_logMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    if (critical) {
        appendRecentLine(_criticalLogs, CRITICAL_LOG_LINES,
                         _criticalLogWriteIdx, _criticalLogCount, entry);
    }
    if (isLogViewerEnabled() && _recentLogBuffer) {
        appendRecentLine(_recentLogBuffer->lines, RECENT_LOG_LINES,
                         _recentLogBuffer->writeIndex, _recentLogBuffer->count, entry);
    }
    unlockLogFiles();
}

size_t LoggerSystem::recentLogLineCount() const {
    if (!_logMutex || xSemaphoreTake(_logMutex, pdMS_TO_TICKS(10)) != pdTRUE) return 0;
    const size_t count = isLogViewerEnabled() && _recentLogBuffer
        ? _recentLogBuffer->count : _criticalLogCount;
    unlockLogFiles();
    return count;
}

size_t LoggerSystem::snapshotRecentLogs(char* out, size_t outSize) const {
    if (!out || outSize == 0) return 0;
    out[0] = '\0';
    if (!_logMutex || xSemaphoreTake(_logMutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
    const bool optional = isLogViewerEnabled() && _recentLogBuffer;
    const size_t capacity = optional ? RECENT_LOG_LINES : CRITICAL_LOG_LINES;
    const uint8_t count = optional ? _recentLogBuffer->count : _criticalLogCount;
    const uint8_t writeIndex = optional ? _recentLogBuffer->writeIndex : _criticalLogWriteIdx;
    const auto* lines = optional ? _recentLogBuffer->lines : _criticalLogs;
    size_t written = 0;
    if (count > 0) {
        // 环形中最早的行位于 (writeIdx - count) mod LINES
        const uint8_t start = static_cast<uint8_t>(
            (writeIndex + capacity - count) % capacity);
        for (uint8_t i = 0; i < count; i++) {
            const char* line = lines[(start + i) % capacity];
            size_t len = strlen(line);
            if (written + len + 1 >= outSize) break;  // 空间不足，截断
            memcpy(out + written, line, len);
            written += len;
            out[written++] = '\n';
        }
    }
    out[written] = '\0';
    unlockLogFiles();
    return written;
}

// ── 日志文件管理 ─────────────────────────────────────────────────────────

// 清理多余的日志备份文件，保留最新的MAX_LOG_FILES个
void LoggerSystem::cleanupOldLogFiles() {
    constexpr size_t MAX_LOG_FILES = 20;
    constexpr size_t MAX_DELETES_PER_PASS = 2;

    // 内存保护：目录遍历和 VFS 操作需要连续内存
    if (FastBee::MemoryBudget::dram8Free() < 12288 ||
        FastBee::MemoryBudget::dram8Largest() < 4096) return;

    // 文件名序号按数值比较；每轮只保留最旧路径，避免 vector/String 堆分配。
    size_t deleted = 0;
    while (deleted < MAX_DELETES_PER_PASS) {
        size_t backupCount = 0;
        bool foundOldest = false;
        uint64_t oldestSequence = 0;
        char oldestPath[64] = {0};

        File root = LittleFS.open(LOG_DIR_PATH);
        if (!root || !root.isDirectory()) {
            if (root) root.close();
            return;
        }

        File file = root.openNextFile();
        while (file) {
            uint64_t sequence = 0;
            const char* name = logBaseName(file.name());
            if (parseBackupSequence(name, sequence)) {
                ++backupCount;
                if (!foundOldest || sequence < oldestSequence) {
                    const int written = snprintf(oldestPath, sizeof(oldestPath), "%s/%s", LOG_DIR_PATH, name);
                    if (written > 0 && static_cast<size_t>(written) < sizeof(oldestPath)) {
                        oldestSequence = sequence;
                        foundOldest = true;
                    }
                }
            }
            file.close();
            file = root.openNextFile();
        }
        root.close();

        if (backupCount <= MAX_LOG_FILES || !foundOldest) return;
        if (!LittleFS.remove(oldestPath)) return;
        ++deleted;
    }
}

FastBee::FeatureInstanceSnapshot LoggerSystem::observeFeatureInstance(const void* context) {
    using namespace FastBee;
    const auto* logger = static_cast<const LoggerSystem*>(context);
    if (!logger) return FeatureInstanceSnapshot::unavailable(FeatureInstanceReason::InvalidSnapshot,
        FeatureInstanceScope::ServiceLifecycle);
    // This callback is registered only for the process-lifetime singleton. It
    // cannot initialize a mutex, probe LittleFS or release the retained ring.
    const auto mutex = logger->_logMutex.load(std::memory_order_acquire);
    if (!mutex) return FeatureInstanceSnapshot::unavailable(FeatureInstanceReason::SynchronizationUnavailable,
        FeatureInstanceScope::ServiceLifecycle);
    if (xSemaphoreTake(mutex, 0) != pdTRUE)
        return FeatureInstanceSnapshot::lifecycle(FeatureInstanceState::Busy);
    const auto intent = logger->fileLoggingState.load(std::memory_order_acquire);
    const auto task = logger->_flushTaskState.load(std::memory_order_acquire);
    const bool retained = bool(logger->_ringBuffer);
    const auto state = intent & FILE_LOGGING_STATE_MASK;
    auto result = FeatureInstanceSnapshot::lifecycle(FeatureInstanceState::Stopped);
    if (state == static_cast<uint32_t>(FileLoggingState::Pending)) {
        result = FeatureInstanceSnapshot::lifecycle(FeatureInstanceState::Busy);
    } else if (state == static_cast<uint32_t>(FileLoggingState::Disabled)) {
        if (retained || (task & LOG_FLUSH_TASK_ACTIVE))
            result = FeatureInstanceSnapshot::lifecycle(FeatureInstanceState::Retiring);
    } else if (state != static_cast<uint32_t>(FileLoggingState::Enabled) || !retained) {
        result = FeatureInstanceSnapshot::unavailable(FeatureInstanceReason::InvalidSnapshot,
            FeatureInstanceScope::ServiceLifecycle);
    } else {
        // A queued drain slot is not proof that the worker or disk is running.
        result = FeatureInstanceSnapshot::lifecycle((task & LOG_FLUSH_TASK_STOPPING)
            ? FeatureInstanceState::Retiring : FeatureInstanceState::Allocated);
    }
    if (intent != logger->fileLoggingState.load(std::memory_order_acquire) ||
        task != logger->_flushTaskState.load(std::memory_order_acquire))
        result = FeatureInstanceSnapshot::lifecycle(FeatureInstanceState::Busy);
    xSemaphoreGive(mutex); // unlockLogFiles() performs cleanup; observation must not.
    return result;
}

bool LoggerSystem::lockLogFiles(uint32_t timeoutMs) {
    return _logMutex &&
           xSemaphoreTake(_logMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void LoggerSystem::unlockLogFiles() const {
    if (_logMutex) {
        if (!isFileLoggingEnabled()) _ringBuffer.reset();
        if (!isLogViewerEnabled()) _recentLogBuffer.reset();
        xSemaphoreGive(_logMutex);
    }
}

bool LoggerSystem::rotateLogFile(const char* newName) {
    if (!lockLogFiles()) return false;
    const bool result = rotateLogFileInternal(newName);
    unlockLogFiles();
    return result;
}

bool LoggerSystem::rotateLogFileInternal(const char* newName) {
    if (!isFileLoggingEnabled() || !LittleFS.exists(LOG_FILE_PATH)) {
        return false;
    }

    char targetPath[64] = {0};
    const char* targetName = targetPath;
    if (newName && newName[0] != '\0') {
        if (!buildSafeLogPath(newName, targetPath, sizeof(targetPath))) return false;
    } else {
        // 从已有备份的最大序号递增，设备重启后仍保持唯一和顺序稳定。
        uint64_t nextSequence = 0;
        if (!findNextBackupSequence(nextSequence)) return false;
        const int written = snprintf(targetPath, sizeof(targetPath),
                                     "%s/system_%010llu.log", LOG_DIR_PATH,
                                     static_cast<unsigned long long>(nextSequence));
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(targetPath)) return false;
    }

    if (LittleFS.exists(targetName)) return false;

    if (!LittleFS.rename(LOG_FILE_PATH, targetName)) {
        // 禁止用截断伪装轮转成功：保留原文件并停止继续增长。
        cancelFileLoggingRequest();
        if (serialEnabled && outputStream) {
            outputStream->println("[Logger] ERROR: Log rotation failed, file logging disabled");
        }
        return false;
    }

    // rename 后原路径不再存在，用 FILE_WRITE 创建新的空日志文件。
    File newLog = LittleFS.open(LOG_FILE_PATH, FILE_WRITE);
    if (!newLog) {
        cancelFileLoggingRequest();
        if (serialEnabled && outputStream) {
            outputStream->println("[Logger] ERROR: Failed to create new log file, file logging disabled");
        }
        return false;
    }
    newLog.close();

    cleanupOldLogFiles();
    return true;
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

bool LoggerSystem::clearLogFile(const char* fileName) {
    const char* name = (fileName && fileName[0] != '\0') ? fileName : LOG_FILE_NAME;
    if (!isSafeLogFileName(name) || !lockLogFiles()) return false;

    char path[64];
    const int written = snprintf(path, sizeof(path), "%s/%s", LOG_DIR_PATH, name);
    bool cleared = written > 0 && static_cast<size_t>(written) < sizeof(path);

    if (cleared && strcmp(name, LOG_FILE_NAME) == 0) {
        // 丢弃尚未落盘的数据，防止清空后旧日志被下一次 flush 写回。
        if (_ringBuffer) {
            _ringBuffer->readPos = _ringBuffer->writePos;
            _ringBuffer->lastFlushMs = millis();
        }
        File logFile = LittleFS.open(LOG_FILE_PATH, FILE_WRITE);
        cleared = static_cast<bool>(logFile);
        if (logFile) logFile.close();
    } else if (cleared) {
        cleared = LittleFS.exists(path) && LittleFS.remove(path);
    }

    unlockLogFiles();
    return cleared;
}

bool LoggerSystem::deleteLogFile() {
    return clearLogFile(LOG_FILE_NAME);
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
    if (!format) return 0;

    // 格式化日志消息到临时缓冲区
    char buffer[512];
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    if (len < 0) {
        buffer[0] = '\0';
        return len;
    }

    size_t actualLen = static_cast<size_t>(len);
    if (actualLen >= sizeof(buffer)) actualLen = sizeof(buffer) - 1;

    // 去除末尾换行符
    while (actualLen > 0 && (buffer[actualLen - 1] == '\n' || buffer[actualLen - 1] == '\r')) {
        buffer[--actualLen] = '\0';
    }

    // 写入到日志文件（通过环形缓冲）
    if (logger.isFileLoggingEnabled() && logger.initialized && logger._logMutex) {
        if (xSemaphoreTake(logger._logMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (logger.isFileLoggingEnabled()) {
                logger.logToBuffer(buffer, actualLen);
                logger.logToBuffer("\n", 1);
                // Never perform flash I/O on the ESP logging caller's stack.
            }
            logger.unlockLogFiles();
        }
    }

    // 同时输出到原始串口（保持串口输出）
    // 保留 Serial 直出（D5 白名单）：此处即 ESP-IDF 日志的串口透传出口。
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
    if (!message || !isFileLoggingEnabled() || !initialized) {
        return;
    }

    // 通过环形缓冲写入
    if (_logMutex && xSemaphoreTake(_logMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (isFileLoggingEnabled()) {
            size_t len = strlen(message);
            logToBuffer(message, len);
            logToBuffer("\n", 1);
            // The periodic consumer owns flush/rotation, even above threshold.
        }
        unlockLogFiles();
    }
}

#endif // FASTBEE_ENABLE_LOGGER
