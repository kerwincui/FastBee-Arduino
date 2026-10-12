#ifndef LOGGER_SYSTEM_H
#define LOGGER_SYSTEM_H

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <vector>
#include <atomic>
#include <memory>
#include "core/interfaces/ILoggerSystem.h"
#include "core/FeatureInstanceRegistry.h"
#include "esp_log.h"
#include <freertos/semphr.h>

// 使用ILoggerSystem中定义的LogLevel枚举

// ============================================================================
// LoggerSystem - 单例日志系统
// 优化点：
//  1. 日志方法改为 const char* 接口，减少 String 临时对象
//  2. 内部格式化使用栈上 char 缓冲区，避免堆分配
//  3. 文件写入使用固定路径常量，不再运行时拼接
//  4. 日志宏使用模块名字符串字面量，不再传递 __FILE__ 完整路径
//  5. 支持捕获 ESP-IDF 系统日志
// ============================================================================

// 前向声明：自定义串口包装器
class LoggingSerial;

class LoggerSystem : public ILoggerSystem {
public:
    LoggerSystem(const LoggerSystem&) = delete;
    LoggerSystem& operator=(const LoggerSystem&) = delete;

    static LoggerSystem& getInstance() {
        static LoggerSystem instance;
        static const FastBee::FeatureInstanceObserver observer{
            "fileLogging", &LoggerSystem::observeFeatureInstance, &instance};
        FastBee::FeatureInstanceRegistry::instance().registerObserver(&observer);
        return instance;
    }

    // ILoggerSystem 接口实现
    bool initialize() override;
    void setLogLevel(LogLevel level) override;
    LogLevel getLogLevel() const override { return currentLevel; }

    void enableSerialLogging(bool enable);
    void enableFileLogging(bool enable);
    void enableLogViewer(bool enable);
    bool isLogViewerEnabled() const {
        return (_recentLogsState.load() & RECENT_LOG_STATE_MASK) == static_cast<uint32_t>(RecentLogState::Enabled);
    }
    bool isLogViewerRequested() const {
        return (_recentLogsState.load() & RECENT_LOG_STATE_MASK) != static_cast<uint32_t>(RecentLogState::Disabled);
    }
    bool isSerialEnabled() const { return serialEnabled; }
    bool isFileLoggingEnabled() const {
        return (fileLoggingState.load() & FILE_LOGGING_STATE_MASK) == static_cast<uint32_t>(FileLoggingState::Enabled);
    }
    // Includes a pending allocation/lock attempt, so memory degradation can
    // cancel it before it becomes active.
    bool isFileLoggingRequested() const {
        return (fileLoggingState.load() & FILE_LOGGING_STATE_MASK) != static_cast<uint32_t>(FileLoggingState::Disabled);
    }

    // ── 主日志接口（const char* 避免 String 拷贝）─────────────────────────
    void error  (const char* message, const char* module = "");
    void warning(const char* message, const char* module = "");
    void info   (const char* message, const char* module = "");
    void debug  (const char* message, const char* module = "");
    void verbose(const char* message, const char* module = "");

    // ── String 重载（兼容旧调用）──────────────────────────────────────────
    void error  (const String& message, const char* module = "") { error  (message.c_str(), module); }
    void warning(const String& message, const char* module = "") { warning(message.c_str(), module); }
    void info   (const String& message, const char* module = "") { info   (message.c_str(), module); }
    void debug  (const String& message, const char* module = "") { debug  (message.c_str(), module); }
    void verbose(const String& message, const char* module = "") { verbose(message.c_str(), module); }

    // ── 格式化日志（printf 风格）─────────────────────────────────────────
    void errorf  (const char* format, ...);
    void warningf(const char* format, ...);
    void infof   (const char* format, ...);
    void debugf  (const char* format, ...);
    void verbosef(const char* format, ...);
    
    // ILoggerSystem 接口实现
    void logDebug(const String& message) override { debug(message); }
    void logInfo(const String& message) override { info(message); }
    void logWarning(const String& message) override { warning(message); }
    void logError(const String& message) override { error(message); }
    void logFatal(const String& message) override { error(message); }

    // 设置输出流
    void setOutputStream(Stream& stream) { outputStream = &stream; }

    // Explicit flush for shutdown/file management; producers only enqueue.
    void flushBuffer();
    // Called from the main-loop task scheduler, never a logging producer.
    void flushBufferIfDue();

    // ── 诊断最近日志（内存环形，不随 flush 清空）───────────────────────────
    // 将最近日志按时间顺序（最旧在前）拷贝到 out，每行以 '\n' 结尾，
    // 返回写入字节数；空间不足时截断。供 /api/system/diag-bundle 使用。
    size_t snapshotRecentLogs(char* out, size_t outSize) const;
    // 当前环形中已保存的日志行数（≤ RECENT_LOG_LINES）
    size_t recentLogLineCount() const;
    // 环形容量（行数）
    static constexpr size_t recentLogCapacity() { return RECENT_LOG_LINES; }
    size_t recentLogAvailableCapacity() const {
        return isLogViewerEnabled() ? RECENT_LOG_LINES : CRITICAL_LOG_LINES;
    }
    // Optional allocations only; the 8-line critical ring is always resident.
    static constexpr size_t recentLogOptionalBytes() { return sizeof(RecentLogBuffer); }
    static constexpr size_t fileLoggingBudgetBytes() {
        // Include task-control/allocator reserve as well as the PSRAM board's
        // temporary task stack, which still consumes internal RAM.
        return sizeof(LogRingBuffer) + 512U
#if FASTBEE_USE_PSRAM
            + LOG_FLUSH_TASK_STACK_BYTES
#endif
            ;
    }

    // 日志文件管理
    bool   lockLogFiles(uint32_t timeoutMs = 250);
    void   unlockLogFiles() const;
    bool   rotateLogFile(const char* newName = nullptr);
    bool   clearLogFile(const char* fileName = nullptr);
    size_t getLogFileSize() const;
    bool   deleteLogFile();
    
    // 日志文件大小限制
    void   setLogFileSizeLimit(size_t limit);
    size_t getLogFileSizeLimit() const;
    
    // 日志模块过滤
    void   addLogModuleFilter(const char* module);
    void   removeLogModuleFilter(const char* module);
    void   clearLogModuleFilters();
    bool   isModuleFiltered(const char* module) const;
    
    // ── ESP-IDF 日志捕获 ─────────────────────────────────────────────────
    void enableEspLogCapture(bool enable);
    bool isEspLogCaptureEnabled() const { return espLogCaptureEnabled; }
    
    // 写入原始日志行（供串口捕获使用）
    void writeRawLog(const char* message);

private:
    LoggerSystem();
    ~LoggerSystem();
    static FastBee::FeatureInstanceSnapshot observeFeatureInstance(const void* context);

    // 内部实现：使用栈缓冲区格式化，零堆分配
    void log(LogLevel level, const char* message, const char* module);
    void logFormatted(LogLevel level, const char* format, va_list args);
    void cleanupOldLogFiles();  // 清理多余的旧日志文件

    // 环形缓冲内部方法
    void logToBuffer(const char* entry, size_t len);
    void flushBufferInternal();  // Consumer only; caller must hold _logMutex.
    static void flushTask(void* context);
    bool rotateLogFileInternal(const char* newName);  // 调用方必须持有 _logMutex
    size_t bufferedDataSize() const;
    void checkLogFileSize();     // 检查文件大小并轮转

    // 诊断最近日志：将一行截断保存到环形（供 snapshotRecentLogs 读取）
    void captureRecentLine(const char* entry, bool critical = true);
    static void appendRecentLine(char (*lines)[128], size_t capacity,
                                 uint8_t& writeIndex, uint8_t& count, const char* entry);
    void cancelFileLoggingRequest();

    static const char* getLevelString(LogLevel level);
    
    // ESP-IDF 日志回调
    static int espLogCallback(const char* format, va_list args);

    // ── 成员变量 ──────────────────────────────────────────────────────────
    LogLevel currentLevel;
    Stream*  outputStream;
    bool     serialEnabled;
    enum class FileLoggingState : uint32_t { Disabled, Pending, Enabled };
    static constexpr uint32_t FILE_LOGGING_STATE_MASK = 3;
    // Low bits are state; upper bits identify cancellations. A disable followed
    // by a new enable cannot be mistaken for the old pending allocation (ABA).
    std::atomic<uint32_t> fileLoggingState;
    bool     initialized;
    bool     espLogCaptureEnabled;   // ESP-IDF 日志捕获开关
    
    // 原始 ESP 日志输出函数
    static vprintf_like_t originalEspLogFunc;

    // 格式化缓冲区大小（栈上分配，避免堆碎片）
    static constexpr uint16_t LOG_BUF_SIZE = 256;
    
    // 日志文件大小限制
    size_t logFileSizeLimit;
    
    // 日志模块过滤列表
    std::vector<const char*> moduleFilters;

    // ── 环形缓冲区 ──────────────────────────────────────────────────────────
    static constexpr size_t LOG_RING_BUFFER_SIZE = 2048;   // 2KB
    static constexpr size_t LOG_FLUSH_THRESHOLD  = 1638;   // Consumer flushes at 80%.
    static constexpr unsigned long LOG_FLUSH_INTERVAL_MS = 5000; // 5秒定时 flush

    struct LogRingBuffer {
        char data[LOG_RING_BUFFER_SIZE];
        volatile size_t writePos = 0;
        size_t readPos = 0;
        unsigned long lastFlushMs = 0;
    };

    // Allocate only while file logging is enabled. Mutable permits deferred
    // cleanup at the end of a const diagnostic snapshot holding the same lock.
    mutable std::unique_ptr<LogRingBuffer> _ringBuffer;
    std::atomic<SemaphoreHandle_t> _logMutex{nullptr};
    // Temporary PSRAM-board drain task: bit 0 owns the single slot; bit 1
    // prevents new submissions during destruction. No permanent task stack.
    static constexpr uint32_t LOG_FLUSH_TASK_ACTIVE = 1;
    static constexpr uint32_t LOG_FLUSH_TASK_STOPPING = 2;
    static constexpr uint32_t LOG_FLUSH_TASK_STACK_BYTES = 4096;
    std::atomic<uint32_t> _flushTaskState{0};
    uint32_t _flushTaskGeneration = 0;  // Protected by _logMutex.

    // Warning/error diagnostics survive disabled optional logging. Ordinary
    // recent lines are retained only while the existing logViewer is enabled.
    static constexpr size_t  CRITICAL_LOG_LINES = 8;
    static constexpr size_t  RECENT_LOG_LINES    = 32;
    static constexpr size_t  RECENT_LOG_LINE_LEN = 128;
    char     _criticalLogs[CRITICAL_LOG_LINES][RECENT_LOG_LINE_LEN] = {{0}};
    uint8_t  _criticalLogWriteIdx = 0;
    uint8_t  _criticalLogCount = 0;
    struct RecentLogBuffer {
        char lines[RECENT_LOG_LINES][RECENT_LOG_LINE_LEN] = {{0}};
        uint8_t writeIndex = 0;
        uint8_t count = 0;
    };
    enum class RecentLogState : uint32_t { Disabled, Pending, Enabled, AllocationFailed };
    static constexpr uint32_t RECENT_LOG_STATE_MASK = 3;
    std::atomic<uint32_t> _recentLogsState{static_cast<uint32_t>(RecentLogState::Disabled)};
    // Every reader/producer holds _logMutex; a timed-out disable defers deletion
    // to unlockLogFiles rather than invalidating an in-flight snapshot.
    mutable std::unique_ptr<RecentLogBuffer> _recentLogBuffer;
};

// ============================================================================
// 日志宏（使用模块名字符串字面量，不传 __FILE__）
// ============================================================================
#define LOGGER LoggerSystem::getInstance()

#define LOG_ERROR(msg)   LOGGER.error  (msg)
#define LOG_WARNING(msg) LOGGER.warning(msg)

// INFO 级别日志：FASTBEE_STRIP_INFO_LOGS=1 时编译时移除（节省字符串常量 Flash 占用）
#if FASTBEE_STRIP_INFO_LOGS
    #define LOG_INFO(msg)    ((void)0)
#else
    #define LOG_INFO(msg)    LOGGER.info   (msg)
#endif

// DEBUG 级别日志：生产版本编译时移除
#if FASTBEE_DEBUG_LOG
    #define LOG_DEBUG(msg)    LOGGER.debug  (msg)
    #define LOG_VERBOSE(msg)  LOGGER.verbose(msg)
    #define LOG_DEBUGF(fmt, ...)   LOGGER.debugf  (fmt, ##__VA_ARGS__)
    #define LOG_VERBOSEF(fmt, ...) LOGGER.verbosef(fmt, ##__VA_ARGS__)
#else
    #define LOG_DEBUG(msg)    ((void)0)
    #define LOG_VERBOSE(msg)  ((void)0)
    #define LOG_DEBUGF(fmt, ...)   ((void)0)
    #define LOG_VERBOSEF(fmt, ...) ((void)0)
#endif

// 带模块名的变体
#define LOG_ERROR_M(msg, mod)   LOGGER.error  (msg, mod)
#define LOG_WARNING_M(msg, mod) LOGGER.warning(msg, mod)

#if FASTBEE_STRIP_INFO_LOGS
    #define LOG_INFO_M(msg, mod)    ((void)0)
#else
    #define LOG_INFO_M(msg, mod)    LOGGER.info   (msg, mod)
#endif

#if FASTBEE_DEBUG_LOG
    #define LOG_DEBUG_M(msg, mod)   LOGGER.debug  (msg, mod)
    #define LOG_VERBOSE_M(msg, mod) LOGGER.verbose(msg, mod)
#else
    #define LOG_DEBUG_M(msg, mod)   ((void)0)
    #define LOG_VERBOSE_M(msg, mod) ((void)0)
#endif

// 格式化日志宏
#define LOG_ERRORF(fmt, ...)   LOGGER.errorf  (fmt, ##__VA_ARGS__)
#define LOG_WARNINGF(fmt, ...) LOGGER.warningf(fmt, ##__VA_ARGS__)

#if FASTBEE_STRIP_INFO_LOGS
    #define LOG_INFOF(fmt, ...)    ((void)0)
#else
    #define LOG_INFOF(fmt, ...)    LOGGER.infof   (fmt, ##__VA_ARGS__)
#endif

#endif
