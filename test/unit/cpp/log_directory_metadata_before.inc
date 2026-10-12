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
        File root = LittleFS.open(LOG_DIR);
        if (!scanBudget.withinDeadline()) {
            if (root) root.close();
            sendLogReadError(ctx, request, 503, "Log directory scan limit exceeded");
            return;
        }
        if (root && root.isDirectory()) {
            if (!requireLogReadBudget(ctx, request, logReadPeakBytes())) {
                root.close(); return;
            }
            File file = root.openNextFile();
            while (file) {
                if (!scanBudget.admitNextFile()) {
                    file.close(); root.close();
                    sendLogReadError(ctx, request, 503, "Log directory scan limit exceeded");
                    return;
                }
                if (!requireLogReadBudget(ctx, request, logReadPeakBytes())) {
                    file.close(); root.close(); return;
                }
                const char* name = file.name();
                if (name && strstr(name, ".log")) {
                    JsonObject item = files.add<JsonObject>();
                    item["name"] = name;
                    item["size"] = file.size();
                    item["current"] = strcmp(name, DEFAULT_LOG_FILE) == 0;
                }
                file.close();
                if (!scanBudget.withinDeadline()) {
                    root.close();
                    sendLogReadError(ctx, request, 503, "Log directory scan limit exceeded");
                    return;
                }
                if (!requireLogReadBudget(ctx, request, logReadPeakBytes())) {
                    root.close(); return;
                }
                file = root.openNextFile();
            }
        }
        if (root) root.close();
        if (!scanBudget.withinDeadline()) {
            sendLogReadError(ctx, request, 503, "Log directory scan limit exceeded");
            return;
        }
    }

    sendLogJson(ctx, request, doc);
}
