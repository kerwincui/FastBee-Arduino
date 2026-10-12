class LogDirectoryScanBudget {
public:
    LogDirectoryScanBudget() : started_(millis()) {}
    bool withinDeadline() const {
        return static_cast<uint32_t>(millis() - started_) <= LOG_DIRECTORY_SCAN_BUDGET_MS;
    }
    bool admitNextFile() {
        if (scanned_ >= LOG_DIRECTORY_MAX_ENTRIES || !withinDeadline()) return false;
        ++scanned_;
        return true;
    }
private:
    uint32_t started_;
    uint32_t scanned_{0};
};

void readLogList(WebHandlerContext* ctx, AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!requireDeviceLogs(ctx, request)) return;

    auto doc = FastBee::makeJsonDocument();
    doc["success"] = true;
    JsonArray files = doc["data"].to<JsonArray>();

    {
        ScopedLogFileAccess access;
        if (!requireLogFileAccess(ctx, request, access)) return;

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
        errno = 0;
        std::unique_ptr<DIR, decltype(&closedir)> root(opendir(directoryPath), &closedir);
        const int openError = errno;
        if (!scanBudget.withinDeadline()) {
            sendLogReadError(ctx, request, 503, "Log directory scan limit exceeded");
            return;
        }
        if (!root && openError != ENOENT) {
            sendLogReadError(ctx, request, 503, "Log directory unavailable");
            return;
        }
        if (root) {
            if (!requireLogReadBudget(ctx, request, logReadPeakBytes())) {
                return;
            }
            while (true) {
                errno = 0;
                const dirent* entry = readdir(root.get());
                const int readError = errno;
                if (!scanBudget.withinDeadline()) {
                    sendLogReadError(ctx, request, 503, "Log directory scan limit exceeded");
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
                    sendLogReadError(ctx, request, 503, "Log directory scan limit exceeded");
                    return;
                }
                if (!requireLogReadBudget(ctx, request, logReadPeakBytes())) {
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
                    struct stat metadata;
                    if (stat(entryPath, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0) {
                        sendLogReadError(ctx, request, 503, "Log entry metadata unavailable");
                        return;
                    }
                    if (!scanBudget.withinDeadline()) {
                        sendLogReadError(ctx, request, 503, "Log directory scan limit exceeded");
                        return;
                    }
                    if (!requireLogReadBudget(ctx, request, logReadPeakBytes())) return;
                    JsonObject item = files.add<JsonObject>();
                    item["name"] = name;
                    item["size"] = static_cast<size_t>(metadata.st_size);
                    item["current"] = strcmp(name, DEFAULT_LOG_FILE) == 0;
                }
                if (!scanBudget.withinDeadline()) {
                    sendLogReadError(ctx, request, 503, "Log directory scan limit exceeded");
                    return;
                }
                if (!requireLogReadBudget(ctx, request, logReadPeakBytes())) {
                    return;
                }
            }
        }
        if (!scanBudget.withinDeadline()) {
            sendLogReadError(ctx, request, 503, "Log directory scan limit exceeded");
            return;
        }
    }

    sendLogJson(ctx, request, doc);
}
