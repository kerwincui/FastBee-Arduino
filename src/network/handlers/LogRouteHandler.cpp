#include "network/handlers/LogRouteHandler.h"
#include "core/MemoryBudget.h"
#include "core/RuntimeFeatureManager.h"
#include "network/handlers/HandlerUtils.h"
#include "network/WebHandlerContext.h"
#include "network/JsonDocumentResponse.h"
#include "core/FeatureFlags.h"
#include "systems/LoggerSystem.h"
#include "core/FastBeeFramework.h"
#include "systems/HealthMonitor.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cstring>
#include <atomic>
#include <new>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <dirent.h>
#include <sys/stat.h>
#include <esp_heap_caps.h>

extern "C" int ets_printf(const char* fmt, ...);

namespace {
constexpr const char* LOG_DIR = "/logs";
constexpr const char* DEFAULT_LOG_FILE = "system.log";

bool hasLogPermission(WebHandlerContext* ctx, AsyncWebServerRequest* request) {
    return ctx && ctx->requiresAuth(request);
}

bool requireDeviceLogs(WebHandlerContext* ctx, AsyncWebServerRequest* request) {
    auto& features = FastBee::RuntimeFeatureManager::instance();
    if (features.shouldEnable("logViewer") || features.shouldEnable("fileLogging")) {
        return true;
    }
    ctx->sendError(request, 503, "Device logs disabled by runtime feature config");
    return false;
}


// File I/O may wait for an append/rotation. Never hold the AsyncTCP task
// through that transaction: defer the authenticated request as login does.
using LogFileOperation = void (*)(WebHandlerContext*, AsyncWebServerRequest*);
enum class LogReadKind : uint8_t { Info, List, Content };
constexpr uint32_t LOG_READ_STACK_BYTES = 4096U;
constexpr uint32_t LOG_READ_REMAINING_DRAM_BYTES = 25600U;
constexpr uint32_t LOG_READ_MIN_LARGEST_BYTES = 12000U;
constexpr uint32_t LOG_READ_TASK_CONTROL_BYTES = 768U;
constexpr uint32_t LOG_READ_CONTENT_PEAK_BYTES = 4096U + 1U;
constexpr uint32_t LOG_READ_TCP_PEAK_BYTES = 6144U;
constexpr uint32_t LOG_READ_RESPONSE_METADATA_BYTES = 512U;
constexpr uint32_t LOG_READ_INTERNAL_JSON_PEAK_BYTES = 6144U;
// Info contains eight numeric/boolean fields and literal keys: one 1024-byte
// ArduinoJson pool on ESP32, plus 512 bytes for allocation headroom.
constexpr uint32_t LOG_INFO_INTERNAL_JSON_PEAK_BYTES = 1536U;
constexpr uint32_t LOG_DIRECTORY_MAX_ENTRIES = 64U;
constexpr uint32_t LOG_DIRECTORY_SCAN_BUDGET_MS = 120U;
constexpr uint32_t LOG_DIRECTORY_BATCH_ENTRIES = 4U;
constexpr uint32_t LOG_DIRECTORY_YIELD_MS = LOG_DIRECTORY_SCAN_BUDGET_MS / 4U;
constexpr uint32_t LOG_DIRECTORY_TOTAL_BUDGET_MS = 4000U;

void sendLogReadError(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                      int status, const char* message, int retrySeconds);

class LogDirectoryScanBudget {
public:
    LogDirectoryScanBudget() : started_(millis()), batchStarted_(started_), operationStarted_(started_) {}
    bool withinDeadline() const {
        return static_cast<uint32_t>(millis() - started_) <= LOG_DIRECTORY_TOTAL_BUDGET_MS;
    }
    bool beginOperation() {
        operationStarted_ = millis();
        return withinDeadline();
    }
    bool completeOperation() const {
        return static_cast<uint32_t>(millis() - operationStarted_) <= LOG_DIRECTORY_SCAN_BUDGET_MS &&
               withinDeadline();
    }
    bool admitNextFile() {
        if (scanned_ >= LOG_DIRECTORY_MAX_ENTRIES || !withinDeadline()) return false;
        ++scanned_;
        return true;
    }
    bool checkpoint() {
        if (!withinDeadline()) return false;
        if (batchEntries_ >= LOG_DIRECTORY_BATCH_ENTRIES ||
            static_cast<uint32_t>(millis() - batchStarted_) >= LOG_DIRECTORY_YIELD_MS) {
            // The existing log_read worker owns the directory and JSON through
            // every batch. Yield to network, sampling and display tasks; a slow
            // individual VFS call is timed separately with the 120 ms limit.
            vTaskDelay(1);
            batchStarted_ = millis();
            batchEntries_ = 0;
        }
        return withinDeadline();
    }
    bool completeFile() {
        ++batchEntries_;
        return checkpoint();
    }
    void reject(WebHandlerContext* ctx, AsyncWebServerRequest* request, const char* phase) const {
        const uint32_t now = millis();
        const uint32_t operationMs = static_cast<uint32_t>(now - operationStarted_);
        const uint32_t batchMs = static_cast<uint32_t>(now - batchStarted_);
        const uint32_t totalMs = static_cast<uint32_t>(now - started_);
        const char* reason = totalMs > LOG_DIRECTORY_TOTAL_BUDGET_MS ? "total-deadline" :
            operationMs > LOG_DIRECTORY_SCAN_BUDGET_MS ? "operation-deadline" :
            scanned_ >= LOG_DIRECTORY_MAX_ENTRIES ? "entry-limit" : "checkpoint";
        char message[176];
        snprintf(message, sizeof(message),
                 "Log directory scan limit exceeded: phase=%s reason=%s entries=%lu operationMs=%lu batchMs=%lu totalMs=%lu",
                 phase, reason, static_cast<unsigned long>(scanned_), static_cast<unsigned long>(operationMs),
                 static_cast<unsigned long>(batchMs), static_cast<unsigned long>(totalMs));
        sendLogReadError(ctx, request, 503, message, 0);
    }
private:
    uint32_t started_;
    uint32_t batchStarted_;
    uint32_t operationStarted_;
    uint32_t scanned_{0};
    uint32_t batchEntries_{0};
};

uint32_t logReadPeakBytes(LogReadKind kind = LogReadKind::Content) {
    // Internal String content is trimmed in place; the existing response owns
    // a moved document and serializes bounded ranges without a body copy.
    const uint32_t contentBytes = kind == LogReadKind::Content ? LOG_READ_CONTENT_PEAK_BYTES : 0U;
    const uint32_t jsonBytes = kind == LogReadKind::Info ?
        LOG_INFO_INTERNAL_JSON_PEAK_BYTES : LOG_READ_INTERNAL_JSON_PEAK_BYTES;
    return contentBytes + LOG_READ_TCP_PEAK_BYTES +
           LOG_READ_RESPONSE_METADATA_BYTES +
           (FastBee::psramAvailableForJson(16384U) ? 0U : jsonBytes);
}

bool logClientLive(AsyncWebServerRequest* request) {
    // A strong ordinary HTTP request also retains its AsyncClient after a
    // disconnect. This route never calls clientRelease(). Skip stale replies.
    auto* client = request ? request->client() : nullptr;
    return client && client->connected();
}

void sendLogReadError(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                      int status, const char* message, int retrySeconds = 0) {
    if (!logClientLive(request)) return;
    const uint32_t free8 = FastBee::MemoryBudget::dram8Free();
    const uint32_t largest8 = FastBee::MemoryBudget::dram8Largest();
    if (free8 < 4096U || largest8 < 1024U) {
        // Same critical reject floor as WebHandlerContext. pause() disabled
        // RX timeout: restore the server's 3s inactivity timeout so an idle
        // abandoned request is reclaimed by AsyncTCP's existing poll path.
        // Never close synchronously while another callback may own the client.
        ets_printf("[LOG_READ_REJECT] free8=%u largest8=%u no response\n",
                   static_cast<unsigned>(free8), static_cast<unsigned>(largest8));
        request->client()->setRxTimeout(3);
        return;
    }
    if (retrySeconds > 0) HandlerUtils::sendJsonError(request, status, message, retrySeconds);
    else ctx->sendError(request, status, message);
}

bool requireLogReadBudget(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                          uint32_t pendingInternalBytes) {
    if (!logClientLive(request)) return false;
    if (FastBee::MemoryBudget::dram8Largest() < LOG_READ_MIN_LARGEST_BYTES ||
        FastBee::MemoryBudget::dram8Free() < LOG_READ_REMAINING_DRAM_BYTES + pendingInternalBytes) {
        sendLogReadError(ctx, request, 503, "Insufficient internal memory for log response");
        return false;
    }
    return true;
}

struct LogReadJob {
    WebHandlerContext* ctx;
    AsyncWebServerRequestPtr held;
    LogFileOperation operation;
    std::atomic<bool>* completed;
    LogReadKind kind;
};

void reapLogReadTask(std::atomic<TaskHandle_t>& task, std::atomic<bool>& completed,
                    std::atomic<bool>& active, std::atomic<bool>& cleanupBusy) {
    bool idle = false;
    if (!cleanupBusy.compare_exchange_strong(idle, true)) return;
    auto handle = task.load(std::memory_order_acquire);
    if (completed.load(std::memory_order_acquire) && handle &&
        eTaskGetState(handle) == eSuspended) {
        // IDF 5.5: deleting a non-running suspended task frees its stack and
        // TCB synchronously. A self-delete would defer both to Idle, allowing
        // repeated HTTP reads to accumulate unreaped stacks. No other caller
        // owns/resumes this handle; the cleanup flag serializes all reapers.
        vTaskDelete(handle);
        task.store(nullptr, std::memory_order_release);
        completed.store(false, std::memory_order_release);
        active.store(false, std::memory_order_release);
    }
    cleanupBusy.store(false, std::memory_order_release);
}

void logReadTask(void* context) {
    auto* job = static_cast<LogReadJob*>(context);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    {
        if (auto request = job->held.lock()) {
            if (logClientLive(request.get())) {
                if (FastBee::MemoryBudget::dram8Largest() < LOG_READ_MIN_LARGEST_BYTES ||
                    FastBee::MemoryBudget::dram8Free() < LOG_READ_REMAINING_DRAM_BYTES + logReadPeakBytes(job->kind)) {
                    sendLogReadError(job->ctx, request.get(), 503, "Insufficient memory for log response", 1);
                } else {
                    job->operation(job->ctx, request.get());
                }
            }
        }
    }
    auto* completed = job->completed;
    delete job;
    ets_printf("[LOG_READ_METRICS] stack=%lu free=%lu bytes\n",
                  static_cast<unsigned long>(LOG_READ_STACK_BYTES),
                  static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
    // Last reference to job/context is gone. Keep the admission slot occupied
    // until an external reaper sees suspension and actually frees task memory.
    completed->store(true, std::memory_order_release);
    for (;;) vTaskSuspend(nullptr);
}

void deferLogFileOperation(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                          LogFileOperation operation, std::atomic<bool>& active,
                          const std::atomic<bool>& stopping, std::atomic<TaskHandle_t>& task,
                          std::atomic<bool>& completed, std::atomic<bool>& cleanupBusy,
                          LogReadKind kind = LogReadKind::Content) {
    if (!ctx->requireAuth(request) || !requireDeviceLogs(ctx, request)) return;
    reapLogReadTask(task, completed, active, cleanupBusy);
    bool idle = false;
    if (!active.compare_exchange_strong(idle, true)) {
        sendLogReadError(ctx, request, 503, "Log read already in progress", 1);
        return;
    }
    if (stopping.load(std::memory_order_acquire)) {
        active.store(false, std::memory_order_release);
        sendLogReadError(ctx, request, 503, "Log reader is stopping", 1);
        return;
    }
    constexpr uint32_t requiredBlock = LOG_READ_STACK_BYTES + 256U;
    const uint32_t requiredFree = requiredBlock + sizeof(LogReadJob) +
        LOG_READ_TASK_CONTROL_BYTES + LOG_READ_REMAINING_DRAM_BYTES + logReadPeakBytes(kind);
    if (FastBee::MemoryBudget::dram8Largest() < requiredBlock + LOG_READ_MIN_LARGEST_BYTES ||
        FastBee::MemoryBudget::dram8Free() < requiredFree) {
        active.store(false, std::memory_order_release);
        sendLogReadError(ctx, request, 503, "Insufficient internal memory for log read", 1);
        return;
    }
    auto* job = new (std::nothrow) LogReadJob{ctx, {}, operation, &completed, kind};
    if (!job) {
        active.store(false, std::memory_order_release);
        sendLogReadError(ctx, request, 503, "Insufficient memory for log request", 1);
        return;
    }
    TaskHandle_t handle = nullptr;
    if (xTaskCreate(logReadTask, "log_read", LOG_READ_STACK_BYTES, job, 1, &handle) != pdPASS) {
        delete job;
        active.store(false, std::memory_order_release);
        sendLogReadError(ctx, request, 503, "Log read task unavailable", 1);
        return;
    }
    task.store(handle, std::memory_order_release);
    job->held = request->pause();
    xTaskNotifyGive(handle);
}

class ScopedLogFileAccess {
public:
    // A background flush holds the file lock through append, close and rotation.
    // File readers wait on their dedicated worker. A bounded four-second
    // deadline permits append/rotation without stalling AsyncTCP.
    explicit ScopedLogFileAccess(uint32_t timeoutMs = 4000)
        : _locked(LOGGER.lockLogFiles(timeoutMs)) {}

    ~ScopedLogFileAccess() {
        if (_locked) LOGGER.unlockLogFiles();
    }

    bool locked() const { return _locked; }

private:
    bool _locked;
};

bool requireLogFileAccess(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                          ScopedLogFileAccess& access,
                          uint32_t pendingInternalBytes = logReadPeakBytes()) {
    if (!logClientLive(request)) return false;
    if (access.locked()) return requireLogReadBudget(ctx, request, pendingInternalBytes);
    sendLogReadError(ctx, request, 503, "Log file is busy, retry shortly");
    return false;
}

void sendLogJson(WebHandlerContext* ctx, AsyncWebServerRequest* request, JsonDocument& doc) {
    if (!requireLogReadBudget(ctx, request,
            LOG_READ_TCP_PEAK_BYTES + LOG_READ_RESPONSE_METADATA_BYTES)) return;
    if (doc.overflowed()) {
        sendLogReadError(ctx, request, 503, "Log JSON allocation failed", 1);
        return;
    }
    auto* response = new (std::nothrow) FastBee::JsonDocumentResponse(std::move(doc));
    if (!response) {
        sendLogReadError(ctx, request, 503, "Log response allocation failed", 1);
        return;
    }
    HandlerUtils::sendWithClose(request, response);
}

void sendLowMemoryLogPlaceholder(AsyncWebServerRequest* request) {
    if (!logClientLive(request)) return;
    request->send(200, "application/json",
        F("{\"success\":true,\"data\":{\"content\":\"[Low memory - log temporarily unavailable]\","
          "\"size\":0,\"lines\":0,\"truncated\":true,\"degraded\":true}}"));
}

String sanitizeLogFileName(WebHandlerContext* ctx, AsyncWebServerRequest* request) {
    String name = ctx->getParamValue(request, "file", DEFAULT_LOG_FILE);
    if (name.length() <= 4 || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 ||
        !name.endsWith(".log")) {
        return String();
    }
    return name;
}
}

void* LogRouteHandler::operator new(size_t bytes) noexcept {
    return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void LogRouteHandler::operator delete(void* storage) noexcept {
    heap_caps_free(storage);
}

void LogRouteHandler::operator delete(void* storage, size_t bytes) noexcept {
    (void)bytes;
    heap_caps_free(storage);
}

LogRouteHandler::LogRouteHandler(WebHandlerContext* ctx) : ctx(ctx) { featureOwner_.ready(); }
LogRouteHandler::~LogRouteHandler() {
    fileReadStopping_.store(true, std::memory_order_release);
    featureOwner_.retiring();
    // WebConfigManager stops routes before destruction; drain this handler's
    // single admitted reader before its context is destroyed. File-lock waits
    // have a deadline, and the read loop retains its 120 ms budget.
    while (fileReadActive_.load(std::memory_order_acquire) ||
           fileReadCleanupBusy_.load(std::memory_order_acquire)) {
        performMaintenance();
        delay(1);
    }
}

void LogRouteHandler::performMaintenance() {
    reapLogReadTask(fileReadTask_, fileReadCompleted_, fileReadActive_, fileReadCleanupBusy_);
}

void LogRouteHandler::setupRoutes(AsyncWebServer* server) {
#if FASTBEE_ENABLE_LOG_VIEWER || FASTBEE_ENABLE_FILE_LOGGING
    server->on("/api/logs/info", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleInfo(request);
    });
    server->on("/api/logs/list", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleList(request);
    });
    server->on("/api/logs", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleRead(request);
    });
    server->on("/api/logs/clear", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleClear(request);
    });
    server->on("/api/system/logs", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleRead(request);
    });
    server->on("/api/system/logs", HTTP_DELETE, [this](AsyncWebServerRequest* request) {
        handleClear(request);
    });
#else
    (void)server;
#endif
}

namespace {
void readLogInfo(WebHandlerContext* ctx, AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!requireDeviceLogs(ctx, request)) return;

    auto doc = FastBee::makeJsonDocument();

    {
        ScopedLogFileAccess access;
        if (!requireLogFileAccess(ctx, request, access, logReadPeakBytes(LogReadKind::Info))) return;

        doc["success"] = true;
        doc["data"]["enabled"] = true;
        doc["data"]["fileLogging"] = LOGGER.isFileLoggingEnabled();
        doc["data"]["level"] = static_cast<int>(LOGGER.getLogLevel());
        doc["data"]["maxSize"] = LOGGER.getLogFileSizeLimit();
        String path = String(LOG_DIR) + "/" + DEFAULT_LOG_FILE;
        if (LittleFS.exists(path)) {
            File file = LittleFS.open(path, "r");
            doc["data"]["exists"] = (bool)file;
            doc["data"]["size"] = file ? file.size() : 0;
            if (file) file.close();
        } else {
            doc["data"]["exists"] = false;
            doc["data"]["size"] = 0;
        }
    }

    sendLogJson(ctx, request, doc);
}

} // namespace

void LogRouteHandler::handleInfo(AsyncWebServerRequest* request) {
    deferLogFileOperation(ctx, request, readLogInfo, fileReadActive_, fileReadStopping_,
                          fileReadTask_, fileReadCompleted_, fileReadCleanupBusy_, LogReadKind::Info);
}

namespace {
void readLogList(WebHandlerContext* ctx, AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!requireDeviceLogs(ctx, request)) return;

    auto doc = FastBee::makeJsonDocument();
    doc["success"] = true;
    JsonArray files = doc["data"].to<JsonArray>();

    {
        ScopedLogFileAccess access;
        if (!requireLogFileAccess(ctx, request, access, logReadPeakBytes(LogReadKind::List))) return;

        LogDirectoryScanBudget scanBudget;
        const char* mountpoint = LittleFS.mountpoint();
        char directoryPath[64];
        const int directoryLength = mountpoint ? snprintf(directoryPath, sizeof(directoryPath),
                                                          "%s%s", mountpoint, LOG_DIR) : -1;
        if (directoryLength < 0 || static_cast<size_t>(directoryLength) >= sizeof(directoryPath)) {
            sendLogReadError(ctx, request, 503, "Log directory path unavailable");
            return;
        }
        // Directory metadata provides every name and size without opening log
        // contents. File::openNextFile performs an extra fopen/fclose per entry.
        if (!scanBudget.beginOperation()) {
            scanBudget.reject(ctx, request, "before-opendir");
            return;
        }
        errno = 0;
        std::unique_ptr<DIR, decltype(&closedir)> root(opendir(directoryPath), &closedir);
        const int openError = errno;
        if (!scanBudget.completeOperation()) {
            scanBudget.reject(ctx, request, "after-opendir");
            return;
        }
        if (!root && openError != ENOENT) {
            sendLogReadError(ctx, request, 503, "Log directory unavailable");
            return;
        }
        if (root) {
            if (!requireLogReadBudget(ctx, request, logReadPeakBytes(LogReadKind::List))) {
                return;
            }
            while (true) {
                if (!scanBudget.checkpoint()) {
                    scanBudget.reject(ctx, request, "before-readdir");
                    return;
                }
                if (!requireLogReadBudget(ctx, request, logReadPeakBytes(LogReadKind::List))) return;
                if (!scanBudget.beginOperation()) {
                    scanBudget.reject(ctx, request, "before-readdir-vfs");
                    return;
                }
                errno = 0;
                const dirent* entry = readdir(root.get());
                const int readError = errno;
                if (!scanBudget.completeOperation()) {
                    scanBudget.reject(ctx, request, "after-readdir");
                    return;
                }
                if (!entry) {
                    if (readError != 0) {
                        sendLogReadError(ctx, request, 503, "Log directory scan failed");
                        return;
                    }
                    break;
                }
                if (!scanBudget.admitNextFile()) {
                    scanBudget.reject(ctx, request, "admit-entry");
                    return;
                }
                if (!requireLogReadBudget(ctx, request, logReadPeakBytes(LogReadKind::List))) {
                    return;
                }
                const char* name = entry->d_name;
                if (strstr(name, ".log")) {
                    // readdir's storage is capture-local. Copy its bounded path
                    // before stat and retain the existing logger rotation lock.
                    char entryPath[320];
                    const int pathLength = snprintf(entryPath, sizeof(entryPath), "%s/%s", directoryPath, name);
                    if (pathLength < 0 || static_cast<size_t>(pathLength) >= sizeof(entryPath)) {
                        sendLogReadError(ctx, request, 503, "Log entry path too long");
                        return;
                    }
                    if (!scanBudget.checkpoint()) {
                        scanBudget.reject(ctx, request, "before-stat");
                        return;
                    }
                    if (!requireLogReadBudget(ctx, request, logReadPeakBytes(LogReadKind::List))) return;
                    if (!scanBudget.beginOperation()) {
                        scanBudget.reject(ctx, request, "before-stat-vfs");
                        return;
                    }
                    struct stat metadata;
                    if (stat(entryPath, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0) {
                        sendLogReadError(ctx, request, 503, "Log entry metadata unavailable");
                        return;
                    }
                    if (!scanBudget.completeOperation()) {
                        scanBudget.reject(ctx, request, "after-stat");
                        return;
                    }
                    if (!requireLogReadBudget(ctx, request, logReadPeakBytes(LogReadKind::List))) return;
                    JsonObject item = files.add<JsonObject>();
                    item["name"] = name;
                    item["size"] = static_cast<size_t>(metadata.st_size);
                    item["current"] = strcmp(name, DEFAULT_LOG_FILE) == 0;
                }
                if (!scanBudget.completeFile()) {
                    scanBudget.reject(ctx, request, "complete-entry");
                    return;
                }
                if (!requireLogReadBudget(ctx, request, logReadPeakBytes(LogReadKind::List))) {
                    return;
                }
            }
        }
        if (!scanBudget.withinDeadline()) {
            scanBudget.reject(ctx, request, "completed-scan");
            return;
        }
    }

    sendLogJson(ctx, request, doc);
}

} // namespace

void LogRouteHandler::handleList(AsyncWebServerRequest* request) {
    deferLogFileOperation(ctx, request, readLogList, fileReadActive_, fileReadStopping_,
                          fileReadTask_, fileReadCompleted_, fileReadCleanupBusy_, LogReadKind::List);
}

namespace {
void readLogRead(WebHandlerContext* ctx, AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!requireDeviceLogs(ctx, request)) return;

    const bool psramBacked = HandlerUtils::psramCanServeJson(8192);
    auto* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    if (!psramBacked &&
        ((monitor && monitor->getMemoryGuardLevel() >= MemoryGuardLevel::SEVERE) ||
         FastBee::MemoryBudget::dramFree() < 24576 || FastBee::MemoryBudget::dramLargest() < 6144)) {
        sendLowMemoryLogPlaceholder(request);
        return;
    }

    if (!psramBacked && HandlerUtils::checkLowMemory(request, 24576)) return;

    String name = sanitizeLogFileName(ctx, request);
    if (name.isEmpty()) {
        ctx->sendBadRequest(request, "Invalid log file");
        return;
    }

    // WP-74: Adaptive read size based on available memory
    size_t maxSize = FastBee::MemoryBudget::dramFree() < 61440 ? 1536 : 4096;
    if (psramBacked && ESP.getMaxAllocHeap() < 4096) {
        maxSize = 1024;  // Extremely low memory scenario
    }
    
    // Enforce hard limit for user requests (getParam 可能返回 nullptr，直接解引用会导致 LoadProhibited 崩溃)
    int requestedLimit = 0;
    if (request->hasParam("limit", true)) {
        requestedLimit = request->getParam("limit", true)->value().toInt();
    }
    if (requestedLimit > 0) {
        maxSize = min(maxSize, static_cast<size_t>(requestedLimit));  // Respect smaller of enforced/requested limit
    }
    
    size_t fileSize = 0;
    size_t start = 0;
    String content;
    if (!requireLogReadBudget(ctx, request, logReadPeakBytes())) return;
    if (!content.reserve(maxSize + 1)) {
        sendLogReadError(ctx, request, 503, "Insufficient memory for log content");
        return;
    }
    bool readBudgetExceeded = false;
    {
        ScopedLogFileAccess access;
        // Content storage is already reserved; recheck the remaining JSON/TCP
        // peak after waiting for the writer, before touching another resource.
        if (!requireLogFileAccess(ctx, request, access,
                logReadPeakBytes() - LOG_READ_CONTENT_PEAK_BYTES)) return;

        String path = String(LOG_DIR) + "/" + name;
        if (!LittleFS.exists(path)) {
            request->send(200, "application/json",
                F("{\"success\":true,\"data\":{\"content\":\"Log file does not exist\",\"size\":0,\"lines\":0,\"truncated\":false}}"));
            return;
        }

        File file = LittleFS.open(path, "r");
        if (!file) {
            sendLogReadError(ctx, request, 500, "Failed to open log file");
            return;
        }

        fileSize = file.size();
        start = fileSize > maxSize ? fileSize - maxSize : 0;
        file.seek(start);

        unsigned long readStartMs = millis();
        size_t remaining = maxSize;
        char buffer[129];
        while (remaining > 0 && file.available()) {
            size_t chunk = remaining < (sizeof(buffer) - 1) ? remaining : (sizeof(buffer) - 1);
            size_t bytesRead = file.read(reinterpret_cast<uint8_t*>(buffer), chunk);
            if (bytesRead == 0) break;
            buffer[bytesRead] = '\0';
            content.concat(buffer, bytesRead);
            remaining -= bytesRead;
            if (millis() - readStartMs > 120) {
                readBudgetExceeded = true;
                break;
            }
        }
        file.close();
    }

    int maxLines = ctx->getParamInt(request, "tail", ctx->getParamInt(request, "lines", 80));
    if (maxLines <= 0) maxLines = 80;  // Default: show last 80 lines
    if (maxLines > 300) maxLines = 300;  // Hard cap to prevent JSON explosion

    int seen = 0;
    int cut = -1;
    for (int i = content.length() - 1; i >= 0; --i) {
        if (content[i] == '\n' && ++seen >= maxLines) {
            cut = i + 1;
            break;
        }
    }
    if (cut > 0) content.remove(0, cut);

    if (!requireLogReadBudget(ctx, request,
            logReadPeakBytes() - LOG_READ_CONTENT_PEAK_BYTES)) return;
    auto doc = FastBee::makeJsonDocument(maxSize + 2048);
    doc["success"] = true;
    doc["data"]["content"] = content;
    doc["data"]["size"] = fileSize;
    doc["data"]["lines"] = seen;
    doc["data"]["truncated"] = start > 0 || cut > 0 || readBudgetExceeded;
    doc["data"]["degraded"] = readBudgetExceeded;
    sendLogJson(ctx, request, doc);
}

} // namespace

void LogRouteHandler::handleRead(AsyncWebServerRequest* request) {
    deferLogFileOperation(ctx, request, readLogRead, fileReadActive_, fileReadStopping_,
                          fileReadTask_, fileReadCompleted_, fileReadCleanupBusy_);
}

void LogRouteHandler::handleClear(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!requireDeviceLogs(ctx, request)) return;

    String name = sanitizeLogFileName(ctx, request);
    if (name.isEmpty()) {
        ctx->sendBadRequest(request, "Invalid log file");
        return;
    }
    if (!LOGGER.clearLogFile(name.c_str())) {
        ctx->sendError(request, 500, "Failed to clear log file");
        return;
    }
    ctx->sendSuccess(request, "Log cleared");
}
