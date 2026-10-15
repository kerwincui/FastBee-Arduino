#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "core/FeatureInstanceRegistry.h"

using String = std::string;
using vprintf_like_t = int (*)(const char*, va_list);
static bool failAllocation = false;
static std::function<void()> duringAllocation;
static void* ringAllocation = nullptr;
static size_t allocationCount = 0, releaseCount = 0;
// The throwing form must be replaced too: without it, throwing-new expressions
// in the exe (std::string/std::function storage, shared_ptr control blocks)
// resolve to libstdc++-6.dll's operator new while the deletes below free() from
// the exe's CRT — a cross-domain free the Windows heap reports as 0xC0000374.
void* operator new(size_t count) {
    if (void* result = std::malloc(count)) return result;
    throw std::bad_alloc();
}
void* operator new(size_t count, const std::nothrow_t&) noexcept {
    if (failAllocation) return nullptr;
    if (duringAllocation) { auto hook = std::move(duringAllocation); duringAllocation = {}; hook(); }
    void* result = std::malloc(count);
    assert(!ringAllocation);
    ringAllocation = result;
    if (result) ++allocationCount;
    return result;
}
void operator delete(void* pointer) noexcept {
    if (pointer && pointer == ringAllocation) {
        ringAllocation = nullptr;
        ++releaseCount;
    }
    std::free(pointer);
}
void operator delete(void* pointer, size_t) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }

struct Mutex { bool held = false, reject = false; };
using SemaphoreHandle_t = Mutex*;
static Mutex mutex;
static bool failMutex = false;
static std::function<void()> beforeLock;
static constexpr int pdTRUE = 1;
#define pdMS_TO_TICKS(value) (value)
SemaphoreHandle_t xSemaphoreCreateMutex() { return failMutex ? nullptr : &mutex; }
static bool observing = false;
int xSemaphoreTake(SemaphoreHandle_t value, int timeout) {
    if (observing) assert(timeout == 0);
    if (beforeLock) { auto hook = std::move(beforeLock); beforeLock = {}; hook(); }
    if (!value || value->held || value->reject) return 0;
    value->held = true;
    return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t value) { assert(value && value->held); value->held = false; }
void vSemaphoreDelete(SemaphoreHandle_t value) { assert(value && !value->held); }
static unsigned long clockMs = 100;
unsigned long millis() { return clockMs; }
void delay(int) {}
struct Stream {
    explicit operator bool() const { return true; }
    void begin(int) {}
    void println(const char*) {}
    int printf(const char*, ...) { return 0; }
} Serial;
bool getLocalTime(struct tm*, int) { return false; }
struct HealthMonitor {
    bool isMemoryCritical() const { return false; }
    bool isMemorySevere() const { return false; }
};
struct FastBeeFramework {
    static FastBeeFramework* getInstance() { return nullptr; }
    HealthMonitor* getHealthMonitor() { return nullptr; }
};
namespace Hardware { constexpr int SERIAL_BAUDRATE = 115200; }
namespace FastBee { namespace MemoryBudget {
static size_t freeBytes = 20000;
size_t dram8Free() { return freeBytes; }
}}
static bool mounted = true, openSucceeds = true, renameSucceeds = true;
static bool newFileSucceeds = true;
static size_t opens = 0;
static String written;
static constexpr int FILE_APPEND = 1, FILE_WRITE = 2;
static constexpr const char* LOG_FILE_PATH = "/logs/system.log";
static constexpr const char* LOG_FILE_NAME = "system.log";
static constexpr const char* LOG_DIR_PATH = "/logs";
struct File {
    bool valid;
    explicit operator bool() const { return valid; }
    void write(const uint8_t* data, size_t count) { written.append(reinterpret_cast<const char*>(data), count); }
    void close() {}
};
struct Filesystem {
    size_t totalBytes() { return mounted ? 100000 : 0; }
    size_t usedBytes() { return 100; }
    File open(const char*, int mode) { ++opens; return {mounted && openSucceeds && (mode != FILE_WRITE || newFileSucceeds)}; }
    bool exists(const char* path) { return std::strcmp(path, LOG_FILE_PATH) == 0; }
    bool rename(const char*, const char*) { return renameSucceeds; }
    bool remove(const char*) { return true; }
} LittleFS;
bool isSafeLogFileName(const char*) { return true; }
bool buildSafeLogPath(const char*, char* result, size_t count) { std::snprintf(result, count, "/logs/test.log"); return true; }
bool findNextBackupSequence(uint64_t& value) { value = 1; return true; }
static constexpr bool LOGGER_STARTUP_FILE_LOGGING = false;
static constexpr size_t CHIP_DEFAULT_LOG_FILE_SIZE_LIMIT = 10000;

// Arduino exports common uppercase macros even when scoped C++ enums are used.
// Keep them defined while compiling the real Logger declaration and methods.
#define DISABLED 0x00
#define ENABLED 0x01
#define PENDING 0x02
#include "logger_types.inc"
vprintf_like_t LoggerSystem::originalEspLogFunc = nullptr;
// Unrelated presentation and filesystem retention backends are substituted;
// allocation, synchronization, buffering and failure transitions remain real.
void LoggerSystem::error(const char* message, const char*) { captureRecentLine(message); }
void LoggerSystem::warning(const char*, const char*) {}
void LoggerSystem::info(const char* message, const char*) { captureRecentLine(message); }
void LoggerSystem::debug(const char*, const char*) {}
void LoggerSystem::verbose(const char*, const char*) {}
void LoggerSystem::setLogLevel(LogLevel value) { currentLevel = value; }
void LoggerSystem::enableEspLogCapture(bool) {}
void LoggerSystem::cleanupOldLogFiles() {}
void LoggerSystem::checkLogFileSize() {}
bool LoggerSystem::isModuleFiltered(const char*) const { return false; }
#include "logger_methods.inc"
// D5：生产代码迁移到 LOG_* 宏；抽取的 degradeFileLogging 代码块依赖这些宏。
// 块内持有 LoggerSystem& logger 形参，宏直接映射到该实例的方法（与设备上
// LOG_* 走 LOGGER 单例等价），避免在重入回调里构造第二个单例及注册
// FeatureInstanceObserver（会引入静态析构顺序问题），include 后 #undef，
// 避免遮蔽下方 LogLevel 枚举名。
#define LOG_INFO(msg) logger.info(msg)
#define LOG_INFOF(fmt, ...) logger.infof(fmt, ##__VA_ARGS__)
#define LOG_WARNING(msg) logger.warning(msg)
#define LOG_WARNINGF(fmt, ...) logger.warningf(fmt, ##__VA_ARGS__)
#define LOG_ERROR(msg) logger.error(msg)
#define LOG_ERRORF(fmt, ...) logger.errorf(fmt, ##__VA_ARGS__)
#include "logger_degradation.inc"
#undef LOG_INFO
#undef LOG_INFOF
#undef LOG_WARNING
#undef LOG_WARNINGF
#undef LOG_ERROR
#undef LOG_ERRORF

void formattedLog(LoggerSystem& logger, const char* format, ...) {
    va_list args;
    va_start(args, format);
    logger.logFormatted(LOG_WARNING, format, args);
    va_end(args);
}

void capturedLog(const char* format, ...) {
    va_list args;
    va_start(args, format);
    LoggerSystem::espLogCallback(format, args);
    va_end(args);
}

void verifyDeferredFileIO(LoggerSystem& logger, int producer) {
    assert(logger.initialize());
    logger.enableFileLogging(true);
    logger.flushBuffer();
    written.clear();
    const size_t before = opens;
    const std::string line(190, 'x');
    for (int i = 0; i < 12; ++i) {
        if (producer == 0) formattedLog(logger, "%s", line.c_str());
        else if (producer == 1) logger.writeRawLog(line.c_str());
        else capturedLog("%s", line.c_str());
    }
    // Exceed both the old 80% threshold and the bounded ring capacity.
    // No producer may enter the filesystem, even on timeout-warning bursts.
    assert(opens == before && written.empty());
    assert(logger.bufferedDataSize() == LoggerSystem::LOG_RING_BUFFER_SIZE - 1);
    logger.flushBufferIfDue();
    assert(opens == before + 1 && !written.empty());
    assert(logger.bufferedDataSize() == 0);
    written.clear();
    logger.writeRawLog("small batch");
    const size_t after = opens;
    logger.flushBufferIfDue();
    assert(opens == after && written.empty());
    clockMs += LoggerSystem::LOG_FLUSH_INTERVAL_MS;
    logger.flushBufferIfDue();
    assert(opens == after + 1 && written == "small batch\n");
    logger.enableFileLogging(false);
}

int main() {
    {
        LoggerSystem logger;
        assert(!logger._ringBuffer && !ringAllocation);
        assert(logger.initialize());
        assert(!logger._ringBuffer && allocationCount == 0);
        assert(logger.clearLogFile(nullptr)); // Empty disabled ring is valid.
        logger.captureRecentLine("diagnostics survive file logging off");
        char diagnostics[512];
        assert(logger.snapshotRecentLogs(diagnostics, sizeof(diagnostics)) > 0);
        assert(std::strstr(diagnostics, "diagnostics survive"));

        logger.enableFileLogging(true);
        assert(logger.isFileLoggingEnabled() && logger._ringBuffer && allocationCount == 1);
        logger.writeRawLog("first");
        auto* original = logger._ringBuffer.get();
        logger.enableFileLogging(true);
        assert(logger._ringBuffer.get() == original && allocationCount == 1);
        logger.flushBuffer();
        assert(written == "first\n");
        logger.enableFileLogging(false);
        assert(!logger.isFileLoggingEnabled() && !ringAllocation && !logger._ringBuffer);
        assert(logger.snapshotRecentLogs(diagnostics, sizeof(diagnostics)) > 0);
        assert(std::strstr(diagnostics, "diagnostics survive"));

        failAllocation = true;
        logger.enableFileLogging(true);
        assert(!logger.isFileLoggingEnabled() && !logger._ringBuffer);
        logger.writeRawLog("not stored");
        logger.flushBuffer();
        failAllocation = false;
        duringAllocation = [&logger]() { logger.enableFileLogging(true); };
        logger.enableFileLogging(true);
        assert(logger.isFileLoggingEnabled() && ringAllocation); // Concurrent ON cannot cancel ON.
        logger.enableFileLogging(false);
        duringAllocation = [&logger]() { logger.enableFileLogging(false); };
        logger.enableFileLogging(true);
        assert(!logger.isFileLoggingEnabled() && !ringAllocation && !logger._ringBuffer);
        // A cancel and a fresh ON request can restore PENDING while the old
        // allocator holds the mutex. The old request must not publish the new one.
        duringAllocation = [&logger]() {
            logger.enableFileLogging(false);
            logger.enableFileLogging(true);
        };
        logger.enableFileLogging(true);
        assert(!logger.isFileLoggingEnabled() && logger.isFileLoggingRequested() && !ringAllocation);
        logger.enableFileLogging(true); // Explicit retry owns the new request.
        assert(logger.isFileLoggingEnabled() && ringAllocation);
        logger.enableFileLogging(false);
        duringAllocation = [&logger]() {
            assert(!logger.isFileLoggingEnabled() && logger.isFileLoggingRequested());
            degradeFileLogging(logger); // Actual HealthMonitor SEVERE cancellation branch.
        };
        logger.enableFileLogging(true);
        // The mutation test deliberately reaches this failure. Use a distinct
        // normal exit rather than abort(), whose Windows crash handling can
        // keep the executable open and block TemporaryDirectory cleanup.
        if (logger.isFileLoggingEnabled() || logger.isFileLoggingRequested() || ringAllocation) return 86;
        mounted = false;
        logger.enableFileLogging(true);
        assert(!logger.isFileLoggingEnabled() && !ringAllocation);
        mounted = true;
        for (int index = 0; index < 20; ++index) {
            logger.enableFileLogging(true);
            assert(logger.isFileLoggingEnabled());
            logger.writeRawLog("discard on disable");
            logger.enableFileLogging(false);
            assert(!ringAllocation && allocationCount == releaseCount);
        }

        logger.enableFileLogging(true);
        logger.writeRawLog("pending before timeout");
        assert(logger.lockLogFiles());
        original = logger._ringBuffer.get();
        logger.enableFileLogging(false); // A concurrent disable cannot acquire the held lock.
        assert(!logger.isFileLoggingEnabled() && logger._ringBuffer.get() == original);
        logger.writeRawLog("must not write after disable");
        logger.unlockLogFiles();
        assert(!logger._ringBuffer && !ringAllocation);

        logger.enableFileLogging(true);
        mutex.reject = true;
        logger.enableFileLogging(false);
        assert(logger._ringBuffer && !logger.isFileLoggingEnabled());
        mutex.reject = false;
        logger.flushBuffer(); // Even disabled flush reclaims a deferred buffer safely.
        assert(!logger._ringBuffer && !ringAllocation);
        mutex.reject = true;
        logger.enableFileLogging(true);
        assert(!logger.isFileLoggingEnabled() && !ringAllocation);
        mutex.reject = false;

        logger.enableFileLogging(true);
        logger.writeRawLog("file failure");
        openSucceeds = false;
        logger.flushBuffer();
        assert(!logger.isFileLoggingEnabled() && !ringAllocation);
        size_t failedOpens = opens;
        logger.flushBuffer();
        assert(opens == failedOpens);
        openSucceeds = true;

        for (bool renameFailure : {true, false}) {
            logger.enableFileLogging(true);
            renameSucceeds = !renameFailure;
            newFileSucceeds = renameFailure;
            assert(!logger.rotateLogFile(nullptr));
            assert(!logger.isFileLoggingEnabled() && !ringAllocation);
        }
        renameSucceeds = newFileSucceeds = true;

        logger.enableFileLogging(true);
        FastBee::MemoryBudget::freeBytes = 100;
        logger.writeRawLog("retain during low memory");
        logger.flushBuffer();
        assert(logger.isFileLoggingEnabled() && logger.bufferedDataSize() > 0);
        logger.enableFileLogging(false);
        assert(!ringAllocation); // Memory pressure never prevents safe release.
        FastBee::MemoryBudget::freeBytes = 20000;
        logger.enableFileLogging(true);
        written.clear();
        assert(logger.lockLogFiles());
        String many(2100, 'x');
        logger.logToBuffer(many.data(), many.size());
        assert(logger.bufferedDataSize() == 2047);
        logger.flushBufferInternal();
        logger.unlockLogFiles();
        assert(written == String(2047, 'x'));
        logger.enableFileLogging(false);
    }
    assert(!ringAllocation && allocationCount == releaseCount);
    {
        LoggerSystem enabledBeforeInit;
        enabledBeforeInit.enableFileLogging(true);
        assert(!ringAllocation && !enabledBeforeInit.isFileLoggingEnabled());
        assert(enabledBeforeInit.initialize());
        assert(ringAllocation && enabledBeforeInit.isFileLoggingEnabled());
    }
    assert(!ringAllocation && allocationCount == releaseCount);
    {
        LoggerSystem beforeInit;
        beforeInit.enableFileLogging(true);
        assert(!ringAllocation); // Save intent only, no allocation before initialization.
        beforeInit.enableFileLogging(false);
        assert(beforeInit.initialize() && !beforeInit.isFileLoggingEnabled());
        assert(!ringAllocation);
    }
    {
        failMutex = true;
        LoggerSystem withoutMutex;
        withoutMutex.enableFileLogging(true);
        withoutMutex.initialize();
        assert(!withoutMutex.isFileLoggingEnabled() && !ringAllocation);
        failMutex = false;
    }
    {
        LoggerSystem logger;
        verifyDeferredFileIO(logger, 0);
        verifyDeferredFileIO(logger, 1);
    }
    verifyDeferredFileIO(LoggerSystem::getInstance(), 2);
    assert(!ringAllocation && allocationCount == releaseCount);
    {
        using FastBee::FeatureInstanceState;
        LoggerSystem logger;
        auto observe = [&] {
            const auto beforeOpen = opens, beforeAllocation = allocationCount, beforeRelease = releaseCount;
            observing = true;
            const auto value = LoggerSystem::observeFeatureInstance(&logger);
            observing = false;
            assert(opens == beforeOpen && allocationCount == beforeAllocation && releaseCount == beforeRelease);
            assert(!value.countKnown() && value.initializedCount == 0);
            return value;
        };
        assert(!observe().observed());
        logger.initialize();
        assert(observe().state == FeatureInstanceState::Stopped);
        logger.enableFileLogging(true);
        assert(observe().state == FeatureInstanceState::Allocated);
        mutex.reject = true;
        logger.enableFileLogging(false);
        assert(ringAllocation && observe().state == FeatureInstanceState::Busy);
        mutex.reject = false;
        for (int i = 0; i < 1000; ++i) {
            assert(observe().state == FeatureInstanceState::Retiring);
            assert(ringAllocation); // An observer must never run unlockLogFiles cleanup.
        }
        logger.enableFileLogging(false);
        assert(!ringAllocation && observe().state == FeatureInstanceState::Stopped);
        logger.fileLoggingState = static_cast<uint32_t>(LoggerSystem::FileLoggingState::Pending);
        assert(observe().state == FeatureInstanceState::Busy);
        logger.fileLoggingState = static_cast<uint32_t>(LoggerSystem::FileLoggingState::Enabled);
        assert(!observe().observed()); // Enabled without a retained ring is inconsistent.
        logger.fileLoggingState = static_cast<uint32_t>(LoggerSystem::FileLoggingState::Disabled);
        assert(FastBee::FeatureInstanceRegistry::instance().observe("fileLogging").observed());
    }
}
