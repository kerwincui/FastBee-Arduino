#include "./network/handlers/SystemRouteHandler.h"
#include "network/handlers/TaskStackSnapshotRoute.h"
#include "network/FixedJsonField.h"
#include "network/RunningImageSizeJson.h"
#include "network/AsyncTCPAdmissionSnapshot.h"
#include "core/MemoryBudget.h"
#include "core/MemoryPoolSnapshot.h"
#include "core/FeatureBudget.h"
#include "core/RuntimeFeatureManager.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "network/NetworkResourcePins.h"
#include "systems/LoggerSystem.h"
#include "systems/ConfigStorage.h"
#include "systems/HealthMonitor.h"
#include "systems/RestartDiagnostics.h"
#include "systems/SystemRebooter.h"
#include "core/FastBeeFramework.h"
#include "core/SystemConstants.h"
#include "utils/NetworkUtils.h"
#include "utils/StaticPoolAllocator.h"
#include "core/FeatureFlags.h"
#include "core/ConfigImportSession.h"
#include "systems/ConfigMigrationStorage.h"
#if FASTBEE_ENABLE_POINT_HISTORY
#include "core/PointHistory.h"
#endif
#include "./network/WebConfigManager.h"
#include "./network/handlers/SSERouteHandler.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>
#include <ctype.h>

// 文件系统统计缓存（60s TTL，避免反复 LittleFS 元数据扫描）
static size_t s_cachedFsTotal = 0;
static size_t s_cachedFsUsed = 0;
static unsigned long s_fsCacheTimeMs = 0;
static void getCachedFsStats(size_t& total, size_t& used) {
    unsigned long now = millis();
    if (s_cachedFsTotal == 0 || now - s_fsCacheTimeMs > 60000UL) {
        s_cachedFsTotal = LittleFS.totalBytes();
        s_cachedFsUsed = LittleFS.usedBytes();
        s_fsCacheTimeMs = now;
    }
    total = s_cachedFsTotal;
    used = s_cachedFsUsed;
}
// #include "core/MemoryProfile.h"  // Not needed - MemoryProfile feature disabled

namespace {
// Keep inline imports small because form parsing already holds the body in RAM.
// Larger config files are transferred through /api/config/transfer/import-chunk.
constexpr size_t kMaxConfigTransferBytes = 128 * 1024;
constexpr size_t kMaxConfigInlineImportBytes = 24 * 1024;
// Chunk limit raised from 8 KB to 16 KB: the frontend sends chunks via
// application/x-www-form-urlencoded, so a 4 KB raw JSON chunk expands to
// ~8-10 KB after URL-encoding.  The handler already requires 24 KB free
// heap before writing, so 16 KB decoded chunk is safe.
constexpr size_t kMaxConfigTransferChunkBytes = 16 * 1024;
constexpr int kMaxConfigTransferChunks = 128;  // 前端分片已缩至 1.5KB（弱网适配），需更多分片数承载 128KB 上限
#if FASTBEE_ENABLE_CONFIG_TRANSFER
FastBee::ConfigImportSession ordinaryImport;
bool parseConfigImportNumber(const String& text, uint32_t limit, uint32_t& value) {
    return text.length() == strlen(text.c_str()) &&
        FastBee::ConfigImportSession::decimal(text.c_str(), limit, value);
}
static_assert(kMaxConfigInlineImportBytes == FastBee::ConfigImportSession::MaxBytes,
              "Ordinary import size limits must agree");
static_assert(kMaxConfigTransferChunks == FastBee::ConfigImportSession::MaxChunks,
              "Ordinary import chunk limits must agree");
#endif

bool isSafeConfigFileName(const String& name) {
    if (name.isEmpty() || name.length() > 48) return false;
    if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf("..") >= 0) return false;
    if (name[0] == '.' || !name.endsWith(".json")) return false;
    for (size_t i = 0; i < name.length(); ++i) {
        char c = name[i];
        if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') continue;
        return false;
    }
    return true;
}

// Browser config transfer is intentionally limited to documents that never
// contain credentials.  Network, protocol and authentication state have
// dedicated APIs and must never be streamed as raw files.
static const char* const kAllowedTransferConfigFiles[] = {
    "device.json",
    "features.json",
    "peripherals.json",
    "periph_exec.json",
    "rule_scripts.json",
    "screen_panel.json",
    "point_history.json",
#if FASTBEE_ENABLE_IR_REMOTE
    "ir_keys.json",
#endif
};

constexpr int kAllowedTransferConfigFilesCount =
    (int)(sizeof(kAllowedTransferConfigFiles) / sizeof(kAllowedTransferConfigFiles[0]));

bool isAllowedConfigTransferFileName(const String& name) {
    if (!isSafeConfigFileName(name)) return false;
    for (int i = 0; i < kAllowedTransferConfigFilesCount; ++i) {
        if (name == kAllowedTransferConfigFiles[i]) return true;
    }
    return false;
}

bool isIgnoredConfigImportFileName(const String& name) {
    return name == "roles.json";
}

bool isManagedRuntimeConfigFileName(const String& name) {
    return name == "peripherals.json" ||
           name == "periph_exec.json" ||
           name == "rule_scripts.json" ||
           name == "screen_panel.json" ||
           name == "point_history.json";
}

bool isManagedRuntimeConfigPath(const String& path) {
    return path == "/config/peripherals.json" ||
           path == "/config/periph_exec.json" ||
           path == "/config/rule_scripts.json" ||
           path == "/config/screen_panel.json" ||
           path == "/config/point_history.json";
}

// 配置导入白名单：只允许导入已知的配置文件，防止在 /config/ 下生成非预期文件
static const char* const kAllowedImportConfigFiles[] = {
    "device.json",
    "features.json",
    "peripherals.json",
    "periph_exec.json",
    "rule_scripts.json",
    "screen_panel.json",
    "point_history.json",
#if FASTBEE_ENABLE_IR_REMOTE
    "ir_keys.json",
#endif
};
constexpr int kAllowedImportConfigFilesCount =
    (int)(sizeof(kAllowedImportConfigFiles) / sizeof(kAllowedImportConfigFiles[0]));

bool isAllowedConfigImportFileName(const String& name) {
    for (int i = 0; i < kAllowedImportConfigFilesCount; ++i) {
        if (name == kAllowedImportConfigFiles[i]) return true;
    }
    return false;
}

bool isKnownRuntimeConfigFileName(const String& name) {
    if (isAllowedConfigImportFileName(name) || isIgnoredConfigImportFileName(name)) return true;
    if (name == "network.json" || name == "protocol.json" ||
        name == "screen_panel.json.touch-v1.bak" ||
        name == "protocol.pre-screen-weather.json" || name == "periph_exec.pre-screen-remote.json" ||
        name == "auth.json" || name == "users.json") return true;
    static const char* const generationSuffixes[] = {
        ".factory.tmp", ".import.tmp", ".bak", ".tmp"
    };
    for (const char* suffix : generationSuffixes) {
        if (name.endsWith(suffix)) {
            return isKnownRuntimeConfigFileName(
                name.substring(0, name.length() - strlen(suffix)));
        }
    }
    return false;
}

bool isSensitiveRuntimeConfigPath(const String& path) {
    return path == "/config/network.json" || path == "/config/protocol.json" ||
           path == "/config/protocol.pre-screen-weather.json" ||
           path == "/config/auth.json" || path == "/config/users.json";
}

bool isSafeFileApiPath(const String& path) {
    return !FastBee::ConfigMigration::Storage::protectedPath(path) && path.startsWith("/") && path.indexOf("..") < 0 &&
           path.indexOf('\\') < 0 && path.indexOf("//") < 0 &&
           path.indexOf("/./") < 0;
}

String configPathForName(const String& name) {
    return String("/config/") + name;
}

String basenameOfFile(const char* path) {
    String name(path ? path : "");
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    return name;
}

// 清理 /config/ 目录中不在白名单内的残留文件（如之前错误命名的导入文件）
int cleanupStaleConfigFiles() {
    int removed = 0;
    File root = LittleFS.open("/config");
    if (!root || !root.isDirectory()) return 0;

    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String name = basenameOfFile(file.name());
            if (!isKnownRuntimeConfigFileName(name)) {
                String path = configPathForName(name);
                file.close();  // 先关闭文件句柄再删除
                if (LittleFS.remove(path)) {
                    removed++;
                    LOGGER.infof("[ConfigTransfer] Cleaned stale file: %s", name.c_str());
                }
                root = LittleFS.open("/config");  // 重新打开目录（删除后迭代器失效）
                if (!root) break;
            }
        }
        file = root.openNextFile();
    }
    root.close();
    return removed;
}

bool shouldUseCompactSystemInfo(bool explicitBrief) {
    return explicitBrief || FastBee::MemoryBudget::dramFree() < 12288 || FastBee::MemoryBudget::dramLargest() < 6144;
}

void fillMemoryPoolSummary(JsonObject memory) {
    const auto snapshot = FastBee::readMemoryPoolSnapshot();
    JsonObject pools = memory["capabilityPools"].to<JsonObject>();
    FastBee::setFixedJsonField(pools, "sampledAtUptimeMs", millis());
    FastBee::setFixedJsonField(pools, "units", "bytes");
    FastBee::setFixedJsonField(pools, "atomicSnapshot", false);
    FastBee::setFixedJsonField(pools, "overlappingPools", true);
    FastBee::setFixedJsonField(pools, "minimumScope", "sum_of_heap_low_watermarks");
    const auto append = [pools](const char* name, const FastBee::MemoryPoolSample& sample) {
        JsonObject item = pools[name].to<JsonObject>();
        FastBee::setFixedJsonField(item, "observed", sample.observed);
        if (sample.observed) {
            FastBee::setFixedJsonField(item, "free", sample.freeBytes);
            FastBee::setFixedJsonField(item, "minFree", sample.minimumFreeBytes);
            FastBee::setFixedJsonField(item, "largestBlock", sample.largestBlockBytes);
        } else {
            FastBee::setFixedJsonField(item, "free", nullptr);
            FastBee::setFixedJsonField(item, "minFree", nullptr);
            FastBee::setFixedJsonField(item, "largestBlock", nullptr);
        }
    };
    append("internal8", snapshot.internal8);
    append("internalDma", snapshot.internalDma);
    append("psram8", snapshot.psram8);
}

void fillMemorySummary(JsonObject memory, bool includeCapabilityPools = false) {
    size_t heapSize = ESP.getHeapSize();
    size_t freeHeap = ESP.getFreeHeap();
    size_t heapUsed = heapSize > freeHeap ? heapSize - freeHeap : 0;

    FastBee::setFixedJsonField(memory, "heapTotal", heapSize);
    FastBee::setFixedJsonField(memory, "heapFree", freeHeap);
    FastBee::setFixedJsonField(memory, "heapUsed", heapUsed);
    FastBee::setFixedJsonField(memory, "heapMinFree", ESP.getMinFreeHeap());
    FastBee::setFixedJsonField(memory, "heapMaxAlloc", ESP.getMaxAllocHeap());
    FastBee::setFixedJsonField(memory, "heapUsagePercent", heapSize > 0 ? (int)((heapUsed * 100) / heapSize) : 0);

    size_t psramSize = ESP.getPsramSize();
    size_t psramFree = psramSize > 0 ? ESP.getFreePsram() : 0;
    size_t psramUsed = psramSize > psramFree ? psramSize - psramFree : 0;
    if (psramSize > 0) {
        FastBee::setFixedJsonField(memory, "psramTotal", psramSize);
        FastBee::setFixedJsonField(memory, "psramFree", psramFree);
        FastBee::setFixedJsonField(memory, "psramUsed", psramUsed);
        FastBee::setFixedJsonField(memory, "psramMinFree", ESP.getMinFreePsram());
        FastBee::setFixedJsonField(memory, "psramUsagePercent", (int)((psramUsed * 100) / psramSize));
    }

    size_t total = heapSize + psramSize;
    size_t free = freeHeap + psramFree;
    size_t used = total > free ? total - free : 0;
    FastBee::setFixedJsonField(memory, "total", total);
    FastBee::setFixedJsonField(memory, "free", free);
    FastBee::setFixedJsonField(memory, "used", used);
    FastBee::setFixedJsonField(memory, "usagePercent", total > 0 ? (int)((used * 100) / total) : 0);
    // Keep compact low-memory/probe/health paths at their existing cost.
    if (includeCapabilityPools) fillMemoryPoolSummary(memory);
}

void fillCompactSystemInfo(JsonObject data, WebHandlerContext* ctx, bool degraded, uint32_t sketchSize) {
    JsonObject device = data["device"].to<JsonObject>();
    FastBee::setFixedJsonField(device, "id", String((uint32_t)ESP.getEfuseMac(), HEX));
    FastBee::setFixedJsonField(device, "chipModel", ESP.getChipModel());
    FastBee::setFixedJsonField(device, "chipRevision", ESP.getChipRevision());
    FastBee::setFixedJsonField(device, "cpuFreqMHz", ESP.getCpuFreqMHz());
    FastBee::setFixedJsonField(device, "sdkVersion", ESP.getSdkVersion());
    FastBee::setFixedJsonField(device, "freeHeap", ESP.getFreeHeap());
    FastBee::setFixedJsonField(device, "flashSize", ESP.getFlashChipSize());
    FastBee::setFixedJsonField(device, "firmwareVersion", SystemInfo::VERSION);

    size_t freeSketchSpace = ESP.getFreeSketchSpace();
    JsonObject flash = data["flash"].to<JsonObject>();
    FastBee::setFixedJsonField(flash, "total", ESP.getFlashChipSize());
    FastBee::setFixedJsonField(flash, "sketchSize", sketchSize);
    FastBee::setFixedJsonField(flash, "freeSketchSpace", freeSketchSpace);
    FastBee::setFixedJsonField(flash, "used", sketchSize);
    FastBee::setFixedJsonField(flash, "free", freeSketchSpace);
    FastBee::setFixedJsonField(flash, "usagePercent", (int)((sketchSize * 100) / (sketchSize + freeSketchSpace)));

    JsonObject memory = data["memory"].to<JsonObject>();
    fillMemorySummary(memory);
    HandlerUtils::addMemoryDegradationStatus(data);

    size_t fsTotal, fsUsed;
    getCachedFsStats(fsTotal, fsUsed);
    JsonObject filesystem = data["filesystem"].to<JsonObject>();
    FastBee::setFixedJsonField(filesystem, "total", fsTotal);
    FastBee::setFixedJsonField(filesystem, "used", fsUsed);
    FastBee::setFixedJsonField(filesystem, "free", fsTotal - fsUsed);
    FastBee::setFixedJsonField(filesystem, "usagePercent", fsTotal > 0 ? (int)((fsUsed * 100) / fsTotal) : 0);

    unsigned long uptime = millis();
    JsonObject uptimeObj = data["uptime"].to<JsonObject>();
    FastBee::setFixedJsonField(uptimeObj, "ms", uptime);
    FastBee::setFixedJsonField(uptimeObj, "seconds", uptime / 1000);
    FastBee::setFixedJsonField(uptimeObj, "formatted", ctx ? ctx->formatUptime(uptime) : String(uptime / 1000) + "s");

    if (ctx && ctx->networkManager) {
        NetworkStatusInfo info = ctx->networkManager->getStatusInfo();
        JsonObject network = data["network"].to<JsonObject>();
        FastBee::setFixedJsonField(network, "connected", (info.status == NetworkStatus::CONNECTED));
        FastBee::setFixedJsonField(network, "ipAddress", info.ipAddress);
        FastBee::setFixedJsonField(network, "ssid", info.ssid);
        FastBee::setFixedJsonField(network, "rssi", info.rssi);
        FastBee::setFixedJsonField(network, "macAddress", WiFi.macAddress());
    }

    FastBee::setFixedJsonField(data, "brief", true);
    FastBee::setFixedJsonField(data, "degraded", degraded);
}

void sendCompactSystemInfo(AsyncWebServerRequest* request, WebHandlerContext* ctx, bool degraded) {
    FastBee::RunningImageSizeResult imageSize;
    if (!FastBee::readRunningImageSizeForResponse(request, imageSize)) return;
    auto doc = FastBee::makeJsonDocument();
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    fillCompactSystemInfo(data, ctx, degraded, imageSize.size);
    HandlerUtils::sendJsonStream(request, doc);
}
}

SystemRouteHandler::SystemRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void SystemRouteHandler::setupRoutes(AsyncWebServer* server) {
    setupMigrationRoutes(server);
    server->on("/api/system/info", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleSystemInfo(request); });

    server->on("/api/system/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleSystemStatus(request); });

    server->on("/api/system/restart", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleSystemRestart(request); });

    server->on("/api/network/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleNetworkConfig(request); });

    server->on("/api/network/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleNetworkStatus(request); });

    // JSON body handler for network config update
    auto* networkJsonHandler = new AsyncCallbackJsonWebHandler("/api/network/config",
        [this](AsyncWebServerRequest* request, JsonVariant& json) { handleUpdateNetworkConfig(request, json); });
    networkJsonHandler->setMethod(HTTP_POST | HTTP_PUT);
    server->addHandler(networkJsonHandler);

    // Legacy form fallback. Register after the JSON handler so application/json
    // POST requests are parsed instead of returning the fallback hint.
    server->on("/api/network/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleSaveNetworkConfig(request); });

#if FASTBEE_ENABLE_CONFIG_TRANSFER
    server->on("/api/config/transfer/list", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleConfigTransferList(request); });

    server->on("/api/config/transfer/export", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleConfigTransferExport(request); });

    server->on("/api/config/transfer/import", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleConfigTransferImport(request); });

    server->on("/api/config/transfer/import-begin", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleConfigTransferImportBegin(request); });

    server->on("/api/config/transfer/import-chunk", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleConfigTransferImportChunk(request); });
#endif

#if FASTBEE_ENABLE_FILE_MANAGER
    // Files
    server->on(AsyncURIMatcher::exact("/api/files"), HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetFilesList(request); });

    server->on("/api/files/content", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetFileContent(request); });

    server->on("/api/files/save", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleSaveFile(request); });

    // Filesystem info
    server->on("/api/filesystem", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetFilesystem(request); });
#endif // FASTBEE_ENABLE_FILE_MANAGER

    // Factory reset - 恢复默认配置而非删除文件，避免 Web 系统因文件缺失报错
    server->on("/api/system/factory-reset", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleFactoryReset(request); });

    // Config
    server->on("/api/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetWebConfig(request); });

    server->on("/api/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleSaveWebConfig(request); });

    // Health check
    server->on("/api/health", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetHealth(request); });
    server->on("/api/system/health", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetHealth(request); });

    // System metrics
    server->on("/api/system/metrics", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleSystemMetrics(request); });

    // 诊断包导出：一次请求聚合设备/重启/内存/文件系统/网络/最近日志快照
    server->on("/api/system/diag-bundle", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleDiagBundle(request); });

    server->on("/api/system/web-runtime", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleWebRuntime(request); });

    // System capabilities (no auth required - public endpoint)
    server->on("/api/system/capabilities", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetCapabilities(request); });
    FastBee::setupTaskStackSnapshotRoute(server, ctx);
}

void SystemRouteHandler::handleSystemInfo(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    if (request->hasParam("probe")) {
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"success\":true,\"data\":{\"memory\":{\"heapFree\":%lu,\"heapMaxAlloc\":%lu,"
                 "\"psramTotal\":%lu,\"psramFree\":%lu},\"brief\":true,\"probe\":true}}",
                 (unsigned long)ESP.getFreeHeap(),
                 (unsigned long)ESP.getMaxAllocHeap(),
                 (unsigned long)ESP.getPsramSize(),
                 (unsigned long)ESP.getFreePsram());
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        response->addHeader("Connection", "close");
        request->send(response);
        return;
    }

    FastBeeFramework* fw = FastBeeFramework::getInstance();
    bool forceBrief = false;
    if (fw && fw->getHealthMonitor()) {
        MemoryGuardLevel level = fw->getHealthMonitor()->getMemoryGuardLevel();
        if (level >= MemoryGuardLevel::CRITICAL) {
            if (!FastBee::psramAvailableForJson(32768) && FastBee::MemoryBudget::dramFree() < 4096) {
                HandlerUtils::sendJsonError(request, 503, "Critical memory - system info unavailable");
                return;
            }
            forceBrief = true;
        }
        if (level >= MemoryGuardLevel::SEVERE) {
            forceBrief = true;
        }
    }

    bool explicitFull = request->hasParam("full") &&
        request->getParam("full")->value() == "1";
    bool explicitBrief = request->hasParam("brief");
    bool brief = !explicitFull || forceBrief || shouldUseCompactSystemInfo(explicitBrief);
    if (brief) {
        sendCompactSystemInfo(request, ctx, forceBrief || (explicitFull && !explicitBrief));
        return;
    }

    if (HandlerUtils::checkLowMemory(request)) return;

    FastBee::RunningImageSizeResult imageSize;
    if (!FastBee::readRunningImageSizeForResponse(request, imageSize)) return;

    auto doc = FastBee::makeJsonDocument();

    FastBee::setFixedJsonField(doc, "data", "device", "id", String((uint32_t)ESP.getEfuseMac(), HEX));
    FastBee::setFixedJsonField(doc, "data", "device", "chipModel", ESP.getChipModel());
    FastBee::setFixedJsonField(doc, "data", "device", "chipRevision", ESP.getChipRevision());
    FastBee::setFixedJsonField(doc, "data", "device", "cpuFreqMHz", ESP.getCpuFreqMHz());
    FastBee::setFixedJsonField(doc, "data", "device", "sdkVersion", ESP.getSdkVersion());
    FastBee::setFixedJsonField(doc, "data", "device", "freeHeap", ESP.getFreeHeap());
    FastBee::setFixedJsonField(doc, "data", "device", "flashSize", ESP.getFlashChipSize());
    FastBee::setFixedJsonField(doc, "data", "device", "firmwareVersion", SystemInfo::VERSION);

    size_t flashSize = ESP.getFlashChipSize();
    uint32_t sketchSize = imageSize.size;
    size_t freeSketchSpace = ESP.getFreeSketchSpace();
    FastBee::setFixedJsonField(doc, "data", "flash", "total", flashSize);
    FastBee::setFixedJsonField(doc, "data", "flash", "speed", ESP.getFlashChipSpeed());
    FastBee::setFixedJsonField(doc, "data", "flash", "sketchSize", sketchSize);
    FastBee::setFixedJsonField(doc, "data", "flash", "freeSketchSpace", freeSketchSpace);
    FastBee::setFixedJsonField(doc, "data", "flash", "used", sketchSize);
    FastBee::setFixedJsonField(doc, "data", "flash", "free", freeSketchSpace);
    FastBee::setFixedJsonField(doc, "data", "flash", "usagePercent", (int)((sketchSize * 100) / (sketchSize + freeSketchSpace)));

    JsonObject memory = doc["data"]["memory"].to<JsonObject>();
    fillMemorySummary(memory, true);

    // 闈欐€佹睜鍒╃敤鐜囩洃鎺э紙T1+T2 纰庣墖娌荤悊鎴愭灉鍙娴嬬偣锛?
    // 涓変釜姹狅細
    //   - (256, 32): peripherals + runtimeStates 鑺傜偣姹?锛?KB DRAM锛?
    //   - (192, 32): rules 鑺傜偣姹?锛?KB DRAM锛?
    //   - (64, 64) : SmallNodePool 楂橀灏忓鍣ㄨ妭鐐规睜 锛?KB DRAM锛?
    JsonObject pools = memory["staticPools"].to<JsonObject>();
#if defined(ARDUINO_ARCH_ESP32) && CHIP_IS_ESP32S3 && \
    defined(BOARD_HAS_PSRAM) && FASTBEE_USE_PSRAM
#define FB_FILL_POOL_STORAGE_STATS(P, O) do {                                        \
    FastBee::setFixedJsonField((O), "storageAvailable", (P).storageAvailable());       \
    FastBee::setFixedJsonField((O), "externalStorage", (P).externalStorage());         \
    FastBee::setFixedJsonField((O), "allocatedStorageBytes",                          \
        static_cast<uint32_t>((P).allocatedStorageBytes()));                          \
} while (0)
#else
#define FB_FILL_POOL_STORAGE_STATS(P, O)
#endif
#define FB_FILL_POOL_STATS(NAME, B, N) do {                                          \
    auto& _p = FastBee::sharedSlabPool<(B), (N)>();                                  \
    JsonObject _o = pools[NAME].to<JsonObject>();                                    \
    FastBee::setFixedJsonField(_o, "capacity", (uint32_t)_p.capacity());                                   \
    FastBee::setFixedJsonField(_o, "blockSize", (uint32_t)_p.blockSize());                                  \
    FastBee::setFixedJsonField(_o, "used", (uint32_t)_p.usedCount());                                  \
    FastBee::setFixedJsonField(_o, "peakUsed", (uint32_t)_p.peakUsed());                                   \
    FastBee::setFixedJsonField(_o, "overflowCount", (uint32_t)_p.overflowCount());                              \
    FastBee::setFixedJsonField(_o, "totalBytes", (uint32_t)_p.totalBytes());                                 \
    FB_FILL_POOL_STORAGE_STATS(_p, _o);                                              \
} while (0)
    FB_FILL_POOL_STATS("periph256", 256, 32);
    FB_FILL_POOL_STATS("rule192",   192, 32);
    FB_FILL_POOL_STATS("small64",   FastBee::SMALL_NODE_BLOCK_SIZE,
                                    FastBee::SMALL_NODE_BLOCK_COUNT);
#if defined(ARDUINO_ARCH_ESP32) && CHIP_IS_ESP32S3 && \
    defined(BOARD_HAS_PSRAM) && FASTBEE_USE_PSRAM
    // Diagnostic access initializes an unused shared pool once. The fixed
    // backing then remains reserved for the device lifetime, like map access.
    FB_FILL_POOL_STATS("sensor160", 160, 24);
#endif
#undef FB_FILL_POOL_STORAGE_STATS
#undef FB_FILL_POOL_STATS

    size_t fsTotal, fsUsed;
    getCachedFsStats(fsTotal, fsUsed);
    FastBee::setFixedJsonField(doc, "data", "filesystem", "total", fsTotal);
    FastBee::setFixedJsonField(doc, "data", "filesystem", "used", fsUsed);
    FastBee::setFixedJsonField(doc, "data", "filesystem", "free", fsTotal - fsUsed);
    FastBee::setFixedJsonField(doc, "data", "filesystem", "usagePercent", fsTotal > 0 ? (int)((fsUsed * 100) / fsTotal) : 0);

    unsigned long uptime = millis();
    FastBee::setFixedJsonField(doc, "data", "uptime", "ms", uptime);
    FastBee::setFixedJsonField(doc, "data", "uptime", "seconds", uptime / 1000);
    FastBee::setFixedJsonField(doc, "data", "uptime", "formatted", ctx->formatUptime(uptime));

    if (ctx->networkManager) {
        NetworkStatusInfo info = ctx->networkManager->getStatusInfo();
        FastBee::setFixedJsonField(doc, "data", "network", "connected", (info.status == NetworkStatus::CONNECTED));
        FastBee::setFixedJsonField(doc, "data", "network", "ipAddress", info.ipAddress);
        FastBee::setFixedJsonField(doc, "data", "network", "ssid", info.ssid);
        FastBee::setFixedJsonField(doc, "data", "network", "rssi", info.rssi);
        FastBee::setFixedJsonField(doc, "data", "network", "macAddress", WiFi.macAddress());
    }

#if FASTBEE_ENABLE_USER_ADMIN
    if (ctx->userManager) {
        FastBee::setFixedJsonField(doc, "data", "users", "total", ctx->userManager->getUserCount());
    }
    if (ctx->authManager) {
        FastBee::setFixedJsonField(doc, "data", "users", "activeSessions", ctx->authManager->getActiveSessionCount());
        FastBee::setFixedJsonField(doc, "data", "users", "online", ctx->authManager->getOnlineUserCount());
    }
#endif

    doc["success"] = true;
    HandlerUtils::sendJsonStream(request, doc);
}

void SystemRouteHandler::handleSystemStatus(AsyncWebServerRequest* request) {
    // This public route bypasses requireAuth and the shared response helpers.
    // Count cached, freshly built and rejected responses as request activity.
    ctx->noteWebRequestActivity();
    unsigned long now = millis();
    // Stability: invalidate the cache when memory pressure has escalated to CRITICAL
    // since the last build. Without this, the dashboard could see a stale "running"
    // response for up to STATUS_CACHE_TTL (5s) while the device is actually OOM-
    // fencing every heavy request. Re-checking the guard level is cheap (just reads
    // the cached level from HealthMonitor) and keeps the cache honest under load.
    if (_statusCache.valid) {
        FastBeeFramework* fw = FastBeeFramework::getInstance();
        HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
        if (monitor && monitor->getMemoryGuardLevel() >= MemoryGuardLevel::CRITICAL) {
            _statusCache.valid = false;
        }
    }
    if (_statusCache.valid && (now - _statusCache.timestamp) < STATUS_CACHE_TTL) {
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", _statusCache.json);
        response->addHeader("Connection", "close");
        request->send(response);
        return;
    }

    // Build in the response envelope: copying a separate document into data
    // doubled the JSON pool at exactly the point low-DRAM clients need status.
    auto wrapper = FastBee::makeJsonDocument();
    wrapper["success"] = true;
    JsonObject doc = wrapper["data"].to<JsonObject>();
    FastBee::setFixedJsonField(doc, "status", "running");
    FastBee::setFixedJsonField(doc, "timestamp", millis());
    FastBee::setFixedJsonField(doc, "freeHeap", ESP.getFreeHeap());
    FastBee::setFixedJsonField(doc, "uptime", millis());
    HandlerUtils::addMemoryDegradationStatus(doc);
    FastBee::setFixedJsonField(doc, "memory", "dram8Free", FastBee::MemoryBudget::dram8Free());
    FastBee::setFixedJsonField(doc, "memory", "dram8Largest", FastBee::MemoryBudget::dram8Largest());

    // 功能运行时状态：disabled=未编译或用户关闭 / no-memory=预算拒绝自动降级 / active=正常启用
    // 统一从 RuntimeFeatureManager 遍历全部已注册功能（2026-08 审计修复：覆盖 27/27 而非原 8/27）
    {
        auto& rfm = FastBee::RuntimeFeatureManager::instance();
        auto& budgetReg = FastBee::FeatureBudgetRegistry::instance();
        JsonObject features = doc["features"].to<JsonObject>();
        for (size_t i = 0; i < rfm.featureCount(); ++i) {
            const auto* meta = rfm.getFeatureAt(i);
            if (!meta) break;
            const bool userEnabled = rfm.isUserEnabled(meta->key);
            const char* state = (!meta->compiled || !userEnabled) ? "disabled"
                : (rfm.isRuntimeDisabled(meta->key) ? "no-memory"
                : (budgetReg.isDenied(meta->key) ? "no-memory"
                : (rfm.shouldEnable(meta->key) ? "active" : "disabled")));
            features[meta->key] = state;
            // Menu visibility for a temporarily degraded panel follows the
            // user's intent. Include only these two small fields so startup
            // does not need the full feature catalog just to resolve a menu.
            if (strcmp(meta->key, "screenPanel") == 0) {
                FastBee::setFixedJsonField(doc, "featureIntent", "screenPanel", "compiled", meta->compiled);
                FastBee::setFixedJsonField(doc, "featureIntent", "screenPanel", "userEnabled", userEnabled);
            }
        }
    }

    if (ctx->networkManager) {
        FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
        netMgr->updateStatusInfo();
        NetworkStatusInfo info = netMgr->getStatusInfo();
        netMgr->patchStatusInfoWithWiFiLink(info);
        const WiFiConfig cfg = netMgr->getConfig();
        const char* statusText = "unknown";
        switch (info.status) {
            case NetworkStatus::CONNECTED:         statusText = "connected"; break;
            case NetworkStatus::DISCONNECTED:      statusText = "disconnected"; break;
            case NetworkStatus::CONNECTING:        statusText = "connecting"; break;
            case NetworkStatus::AP_MODE:           statusText = "ap_mode"; break;
            case NetworkStatus::CONNECTION_FAILED: statusText = "failed"; break;
            default: break;
        }
        FastBee::setFixedJsonField(doc, "networkStatus", statusText);
        FastBee::setFixedJsonField(doc, "networkStatusCode", static_cast<uint8_t>(info.status));
        FastBee::setFixedJsonField(doc, "networkConnected", (info.status == NetworkStatus::CONNECTED));
        FastBee::setFixedJsonField(doc, "connected", (info.status == NetworkStatus::CONNECTED));
        FastBee::setFixedJsonField(doc, "deviceNetworkType", static_cast<uint8_t>(cfg.networkType));
        FastBee::setFixedJsonField(doc, "ipAddress", info.ipAddress);
        FastBee::setFixedJsonField(doc, "ssid", info.ssid);
        FastBee::setFixedJsonField(doc, "rssi", info.rssi);
        FastBee::setFixedJsonField(doc, "macAddress", info.macAddress.isEmpty() ? WiFi.macAddress() : info.macAddress);
        FastBee::setFixedJsonField(doc, "internetAvailable", info.internetAvailable);
    }

    if (wrapper.overflowed()) {
        wrapper.clear();
        HandlerUtils::sendJsonError(request, 503, "System status temporarily unavailable");
        return;
    }
    const size_t expectedSize = measureJson(wrapper);
    const size_t written = serializeJson(wrapper, _statusCache.json);
    // The cached String owns the serialized bytes; release the document before
    // beginResponse copies that String and allocates the TCP response buffers.
    wrapper.clear();
    // ArduinoStringWriter can report attempted bytes even when concat/flush
    // failed, so check the actual cached bytes as well as the write count.
    if (written != expectedSize || _statusCache.json.length() != expectedSize) {
        _statusCache.json = String();
        HandlerUtils::sendJsonError(request, 503, "System status temporarily unavailable");
        return;
    }
    _statusCache.timestamp = millis();
    _statusCache.valid = true;
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", _statusCache.json);
    response->addHeader("Connection", "close");
    request->send(response);
}

void SystemRouteHandler::handleSystemRestart(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    int delaySeconds = ctx->getParamInt(request, "delay", 3);
    delaySeconds = constrain(delaySeconds, 1, 30);

    LOG_INFOF("[System] Restart scheduled in %d seconds", delaySeconds);

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "System will restart in " + String(delaySeconds) + " seconds";
    doc["delay"] = delaySeconds;
    doc["timestamp"] = millis();

    String jsonStr;
    serializeJson(doc, jsonStr);

    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", jsonStr);
    response->addHeader("Connection", "close");

    // NOTE: static savedDelay is shared across concurrent restart requests. Only the
    // most recent request's delay wins, but SystemRebooter::scheduleReboot is itself
    // idempotent (it refuses to re-schedule once armed), so there is no crash/UB.
    // If per-request delay isolation is needed later, capture it via shared_ptr in
    // the onDisconnect lambda instead of a file-scope static.
    static int savedDelay = 0;
    savedDelay = delaySeconds;

    request->onDisconnect([this]() {
        SystemRebooter::scheduleReboot("User requested system restart",
                                       savedDelay * 1000UL,
                                       RestartReason::USER_COMMAND);
        LOG_INFO("[System] Connection closed, restart scheduled via SystemRebooter");
    });

    request->send(response);
}

void SystemRouteHandler::handleNetworkConfig(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    auto doc = FastBee::makeJsonDocument(8192);

    if (ctx->networkManager) {
        FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
        WiFiConfig cfg = netMgr->getConfig();

        FastBee::setFixedJsonField(doc, "data", "device", "macAddress", WiFi.macAddress());
        // deviceName 已移至设备配置，网络配置 GET 不再返回
        FastBee::setFixedJsonField(doc, "data", "network", "mode", static_cast<uint8_t>(cfg.mode));
        FastBee::setFixedJsonField(doc, "data", "network", "networkType", static_cast<uint8_t>(cfg.networkType));
        FastBee::setFixedJsonField(doc, "data", "network", "ipConfigType", static_cast<uint8_t>(cfg.ipConfigType));
        FastBee::setFixedJsonField(doc, "data", "network", "enableMDNS", cfg.enableMDNS);
        FastBee::setFixedJsonField(doc, "data", "network", "customDomain", cfg.customDomain);
        FastBee::setFixedJsonField(doc, "data", "sta", "ssid", cfg.staSSID);
        FastBee::setFixedJsonField(doc, "data", "sta", "password", cfg.staPassword.length() > 0 ? "********" : "");
        FastBee::setFixedJsonField(doc, "data", "sta", "hasPassword", cfg.staPassword.length() > 0);
        FastBee::setFixedJsonField(doc, "data", "sta", "staticIP", cfg.staticIP);
        FastBee::setFixedJsonField(doc, "data", "sta", "gateway", cfg.gateway);
        FastBee::setFixedJsonField(doc, "data", "sta", "subnet", cfg.subnet);
        FastBee::setFixedJsonField(doc, "data", "sta", "dns1", cfg.dns1);
        FastBee::setFixedJsonField(doc, "data", "sta", "dns2", cfg.dns2);
        FastBee::setFixedJsonField(doc, "data", "ap", "ssid", cfg.apSSID);
        FastBee::setFixedJsonField(doc, "data", "ap", "password", cfg.apPassword.length() > 0 ? "********" : "");
        FastBee::setFixedJsonField(doc, "data", "ap", "hasPassword", cfg.apPassword.length() > 0);
        FastBee::setFixedJsonField(doc, "data", "ap", "ip", cfg.apIP);
        FastBee::setFixedJsonField(doc, "data", "ap", "channel", cfg.apChannel);
        FastBee::setFixedJsonField(doc, "data", "ap", "hidden", cfg.apHidden);
        FastBee::setFixedJsonField(doc, "data", "ap", "maxConnections", cfg.apMaxConnections);
        FastBee::setFixedJsonField(doc, "data", "advanced", "connectTimeout", cfg.connectTimeout);
        FastBee::setFixedJsonField(doc, "data", "advanced", "reconnectInterval", cfg.reconnectInterval);
        FastBee::setFixedJsonField(doc, "data", "advanced", "maxReconnectAttempts", cfg.maxReconnectAttempts);
        FastBee::setFixedJsonField(doc, "data", "advanced", "conflictDetection", static_cast<uint8_t>(cfg.conflictDetection));
        FastBee::setFixedJsonField(doc, "data", "advanced", "failoverStrategy", static_cast<uint8_t>(cfg.failoverStrategy));
        FastBee::setFixedJsonField(doc, "data", "advanced", "autoFailover", cfg.autoFailover);
        FastBee::setFixedJsonField(doc, "data", "advanced", "conflictCheckInterval", cfg.conflictCheckInterval);
        FastBee::setFixedJsonField(doc, "data", "advanced", "maxFailoverAttempts", cfg.maxFailoverAttempts);
        FastBee::setFixedJsonField(doc, "data", "advanced", "conflictThreshold", cfg.conflictThreshold);
        FastBee::setFixedJsonField(doc, "data", "advanced", "fallbackToDHCP", cfg.fallbackToDHCP);

        // 浠ュお缃戦厤缃?
        FastBee::setFixedJsonField(doc, "data", "ethernet", "spiMosi", cfg.ethernet.spiMosi);
        FastBee::setFixedJsonField(doc, "data", "ethernet", "spiMiso", cfg.ethernet.spiMiso);
        FastBee::setFixedJsonField(doc, "data", "ethernet", "spiSck", cfg.ethernet.spiSck);
        FastBee::setFixedJsonField(doc, "data", "ethernet", "csPin", cfg.ethernet.csPin);
        FastBee::setFixedJsonField(doc, "data", "ethernet", "rstPin", cfg.ethernet.rstPin);
        FastBee::setFixedJsonField(doc, "data", "ethernet", "intPin", cfg.ethernet.intPin);

        // 4G 铚傜獫閰嶇疆
        FastBee::setFixedJsonField(doc, "data", "cellular", "txPin", cfg.cellular.txPin);
        FastBee::setFixedJsonField(doc, "data", "cellular", "rxPin", cfg.cellular.rxPin);
        FastBee::setFixedJsonField(doc, "data", "cellular", "pwrPin", cfg.cellular.pwrPin);
        FastBee::setFixedJsonField(doc, "data", "cellular", "baudRate", cfg.cellular.baudRate);
        FastBee::setFixedJsonField(doc, "data", "cellular", "apn", cfg.cellular.apn);

        NetworkStatusInfo info = netMgr->getStatusInfo();
        FastBee::setFixedJsonField(doc, "data", "status", "connected", (info.status == NetworkStatus::CONNECTED));
        FastBee::setFixedJsonField(doc, "data", "status", "statusCode", static_cast<uint8_t>(info.status));
        FastBee::setFixedJsonField(doc, "data", "status", "ipAddress", info.ipAddress);
        FastBee::setFixedJsonField(doc, "data", "status", "rssi", info.rssi);
        FastBee::setFixedJsonField(doc, "data", "status", "ssid", info.ssid);
    } else {
        doc["success"] = false;
        doc["error"] = "Network service unavailable";
        HandlerUtils::sendJsonStream(request, doc);
        return;
    }

    doc["success"] = true;
    HandlerUtils::sendJsonStream(request, doc);
}

void SystemRouteHandler::handleSaveNetworkConfig(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    ctx->sendSuccess(request, "Use JSON body for network config update");
}

#if FASTBEE_ENABLE_FILE_MANAGER
void SystemRouteHandler::handleGetFilesList(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    const bool psramBacked = FastBee::psramAvailableForJson(32768);
    if (!psramBacked) {
        if (HandlerUtils::rejectHeavyRequestOnPressure(request, "File list", MemoryGuardLevel::SEVERE, 8)) {
            return;
        }
        if (HandlerUtils::checkLowMemory(request, 4096)) return;
    } else if (FastBee::MemoryBudget::dramFree() < 2048 || FastBee::MemoryBudget::dramLargest() < 1024) {
        HandlerUtils::sendJsonError(request, 503, "Critical memory - file list unavailable", 5);
        return;
    }

    String path = "/";
    if (request->hasParam("path")) {
        path = request->getParam("path")->value();
    }
    if (!path.startsWith("/")) {
        path = "/" + path;
    }
    if (!isSafeFileApiPath(path)) {
        ctx->sendError(request, 403, "Invalid path");
        return;
    }

    auto doc = FastBee::makeJsonDocument();
    doc["success"] = true;
    FastBee::setFixedJsonField(doc, "data", "path", path);

    File root = LittleFS.open(path);
    if (!root || !root.isDirectory()) {
        doc["success"] = false;
        doc["error"] = "Invalid directory";
        HandlerUtils::sendJsonStream(request, doc);
        return;
    }

    JsonArray files = doc["data"]["files"].to<JsonArray>();
    JsonArray dirs = doc["data"]["dirs"].to<JsonArray>();

    File file = root.openNextFile();
    while (file) {
        const String entryName = file.name();
        if (entryName.indexOf(".config-migration") >= 0) { file = root.openNextFile(); continue; }
        JsonObject item;
        if (file.isDirectory()) {
            item = dirs.add<JsonObject>();
        } else {
            item = files.add<JsonObject>();
            FastBee::setFixedJsonField(item, "size", file.size());
        }
        FastBee::setFixedJsonField(item, "name", String(file.name()));
        file = root.openNextFile();
    }
    root.close();

    HandlerUtils::sendJsonStream(request, doc);
}
#endif // FASTBEE_ENABLE_FILE_MANAGER

void SystemRouteHandler::handleGetHealth(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["status"] = "healthy";
    doc["timestamp"] = millis();
    doc["freeHeap"] = ESP.getFreeHeap();
    JsonObject memory = doc["memory"].to<JsonObject>();
    fillMemorySummary(memory);
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    if (monitor) {
        MemoryGuardLevel level = monitor->getMemoryGuardLevel();
        memory["guardLevel"] = HandlerUtils::memoryGuardLevelName(level);
        memory["critical"] = monitor->isMemoryCritical();
    }
    HandlerUtils::addMemoryDegradationStatus(doc);
    ctx->sendSuccess(request, doc);
}

void SystemRouteHandler::handleSystemMetrics(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    FastBeeFramework* fw = FastBeeFramework::getInstance();
    if (!fw || !fw->getHealthMonitor()) {
        ctx->sendError(request, 503, "Health monitor unavailable");
        return;
    }

    String json = fw->getHealthMonitor()->getMetricsJson();
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
    response->addHeader("Connection", "close");
    request->send(response);
}

void SystemRouteHandler::handleDiagBundle(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Diag bundle", MemoryGuardLevel::SEVERE, 5)) return;
    if (HandlerUtils::checkLowMemory(request, 16384)) return;

    auto doc = FastBee::makeJsonDocument(20480);
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    const unsigned long now = millis();
    FastBee::setFixedJsonField(data, "timestamp", now);

    // ── 设备信息 ─────────────────────────────────────────────────
    JsonObject device = data["device"].to<JsonObject>();
    FastBee::setFixedJsonField(device, "chipModel", ESP.getChipModel());
    FastBee::setFixedJsonField(device, "chipRevision", ESP.getChipRevision());
    FastBee::setFixedJsonField(device, "cpuFreqMHz", ESP.getCpuFreqMHz());
    FastBee::setFixedJsonField(device, "sdkVersion", ESP.getSdkVersion());
    FastBee::setFixedJsonField(device, "firmwareVersion", SystemInfo::VERSION);
    FastBee::setFixedJsonField(device, "flashSize", ESP.getFlashChipSize());
    FastBee::setFixedJsonField(device, "uptimeMs", now);
    FastBee::setFixedJsonField(device, "uptimeFormatted", ctx->formatUptime(now));

    // ── 重启诊断（RestartDiagnostics，RTC_NOINIT 快照只读）──────────
    JsonObject restart = data["restart"].to<JsonObject>();
    const esp_reset_reason_t resetReason = esp_reset_reason();
    FastBee::setFixedJsonField(restart, "resetReason", RestartDiagnostics::getResetReasonString(resetReason));
    FastBee::setFixedJsonField(restart, "resetReasonCode", static_cast<uint8_t>(resetReason));
    FastBee::setFixedJsonField(restart, "abnormalRestart", RestartDiagnostics::wasAbnormalRestart());
    PreRestartSnapshot snapshot;
    if (RestartDiagnostics::getLastSnapshot(snapshot)) {
        FastBee::setFixedJsonField(restart, "snapshotValid", true);
        FastBee::setFixedJsonField(restart, "lastRestartReason",
            RestartDiagnostics::getRestartReasonString(static_cast<RestartReason>(snapshot.restartReason)));
        FastBee::setFixedJsonField(restart, "lastRestartReasonCode", snapshot.restartReason);
        FastBee::setFixedJsonField(restart, "lastRestartContext", snapshot.restartContext);
        FastBee::setFixedJsonField(restart, "lastUptimeMs", snapshot.timestamp);
        JsonObject pre = restart["preRestart"].to<JsonObject>();
        FastBee::setFixedJsonField(pre, "freeHeap", snapshot.freeHeap);
        FastBee::setFixedJsonField(pre, "minFreeHeap", snapshot.minFreeHeap);
        FastBee::setFixedJsonField(pre, "largestFreeBlock", snapshot.largestFreeBlock);
        FastBee::setFixedJsonField(pre, "heapFragmentation", snapshot.heapFragmentation);
        FastBee::setFixedJsonField(pre, "memGuardLevel", snapshot.memGuardLevel);
        FastBee::setFixedJsonField(pre, "loopTaskWatermark", nullptr);
        FastBee::setFixedJsonField(pre, "asyncTcpWatermark", nullptr);
        FastBee::setFixedJsonField(pre, "mqttReconnWatermark", nullptr);
        FastBee::setFixedJsonField(pre, "taskWatermarkStatus", "unknown");
        FastBee::setFixedJsonField(pre, "taskWatermarkReason", "NAMED_TASK_LIFETIME_UNCONFIRMED");
        FastBee::setFixedJsonField(pre, "wifiConnected", snapshot.wifiConnected != 0);
        FastBee::setFixedJsonField(pre, "wifiRssi", snapshot.wifiRssi);
        FastBee::setFixedJsonField(pre, "mqttQueueDepth", snapshot.mqttQueueDepth);
        FastBee::setFixedJsonField(pre, "sseClientCount", snapshot.sseClientCount);
        FastBee::setFixedJsonField(pre, "activeRuleCount", snapshot.activeRuleCount);
    } else {
        FastBee::setFixedJsonField(restart, "snapshotValid", false);
    }

    // ── 内存快照（heap 汇总 + DRAM/PSRAM 分档 + HealthMonitor 守卫级）──
    JsonObject memory = data["memory"].to<JsonObject>();
    fillMemorySummary(memory, true);
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    if (monitor) {
        const SystemHealth health = monitor->getHealthStatus();
        FastBee::setFixedJsonField(memory, "guardLevel", HandlerUtils::memoryGuardLevelName(monitor->getMemoryGuardLevel()));
        FastBee::setFixedJsonField(memory, "critical", monitor->isMemoryCritical());
        FastBee::setFixedJsonField(memory, "dramFree", health.dramFreeHeap);
        FastBee::setFixedJsonField(memory, "dramLargestBlock", health.dramLargestBlock);
        FastBee::setFixedJsonField(memory, "heapFragmentation", health.heapFragmentation);
        FastBee::setFixedJsonField(memory, "cpuUsage", health.cpuUsage);
    }

    // ── 文件系统（LittleFS，仅读元数据）────────────────────────
    JsonObject fs = data["filesystem"].to<JsonObject>();
    size_t fsTotal, fsUsed;
    getCachedFsStats(fsTotal, fsUsed);
    FastBee::setFixedJsonField(fs, "total", fsTotal);
    FastBee::setFixedJsonField(fs, "used", fsUsed);
    FastBee::setFixedJsonField(fs, "free", fsTotal > fsUsed ? fsTotal - fsUsed : 0);

    // ── 网络状态 ─────────────────────────────────────────────────
    if (ctx->networkManager) {
        FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
        NetworkStatusInfo info = netMgr->getStatusInfo();
        WiFiConfig cfg = netMgr->getConfig();
        JsonObject network = data["network"].to<JsonObject>();
        const char* statusText = "unknown";
        switch (info.status) {
            case NetworkStatus::CONNECTED:         statusText = "connected";    break;
            case NetworkStatus::DISCONNECTED:      statusText = "disconnected"; break;
            case NetworkStatus::CONNECTING:        statusText = "connecting";   break;
            case NetworkStatus::AP_MODE:           statusText = "ap_mode";      break;
            case NetworkStatus::CONNECTION_FAILED: statusText = "failed";       break;
            default: break;
        }
        const char* modeText = "unknown";
        switch (cfg.mode) {
            case NetworkMode::NETWORK_STA: modeText = "STA"; break;
            case NetworkMode::NETWORK_AP:  modeText = "AP";  break;
            default: break;
        }
        FastBee::setFixedJsonField(network, "status", statusText);
        FastBee::setFixedJsonField(network, "mode", modeText);
        FastBee::setFixedJsonField(network, "networkType", static_cast<uint8_t>(cfg.networkType));
        FastBee::setFixedJsonField(network, "ipAddress", info.ipAddress);
        FastBee::setFixedJsonField(network, "ssid", info.ssid);
        FastBee::setFixedJsonField(network, "rssi", info.rssi);
        FastBee::setFixedJsonField(network, "macAddress", info.macAddress.isEmpty() ? WiFi.macAddress() : info.macAddress);
        FastBee::setFixedJsonField(network, "internetAvailable", info.internetAvailable);
        FastBee::setFixedJsonField(network, "apClientCount", info.apClientCount);
    }

    // ── 运行时：任务数与活跃连接（低成本只读）──────────────────────
    JsonObject runtime = data["runtime"].to<JsonObject>();
    FastBee::setFixedJsonField(runtime, "taskCount", static_cast<uint32_t>(uxTaskGetNumberOfTasks()));
    WebConfigManager* webCfg = fw ? fw->getWebConfigManager() : nullptr;
    if (webCfg) {
        FastBee::setFixedJsonField(runtime, "sseClients", webCfg->getSseClientCount());
        FastBee::setFixedJsonField(runtime, "sseMaxClients", static_cast<uint32_t>(SSERouteHandler::MAX_SSE_CLIENTS));
    }

    // ── 最近日志（LoggerSystem 内存环形，最近 N 行）────────────────
    JsonObject logs = data["logs"].to<JsonObject>();
    constexpr size_t kDiagLogBufSize = 2048;  // 与日志环形缓冲同量级
    char* logBuf = static_cast<char*>(malloc(kDiagLogBufSize));
    if (logBuf) {
        const size_t bytes = LOGGER.snapshotRecentLogs(logBuf, kDiagLogBufSize);
        FastBee::setFixedJsonField(logs, "content", logBuf);
        FastBee::setFixedJsonField(logs, "bytes", bytes);
        free(logBuf);
    } else {
        FastBee::setFixedJsonField(logs, "content", "[Low memory - recent logs unavailable]");
        FastBee::setFixedJsonField(logs, "bytes", 0);
    }
    FastBee::setFixedJsonField(logs, "lines", LOGGER.recentLogLineCount());
    FastBee::setFixedJsonField(logs, "capacityLines", LOGGER.recentLogAvailableCapacity());
    FastBee::setFixedJsonField(logs, "optionalRecentLogsEnabled", LOGGER.isLogViewerEnabled());

    LOGGER.info("Diag bundle exported via web");
    HandlerUtils::sendJsonStream(request, doc);
}

void SystemRouteHandler::handleWebRuntime(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    if (request->hasParam("probe")) {
        char json[512];
        unsigned long now = millis();
        snprintf(json, sizeof(json),
            "{\"success\":true,\"data\":{"
            "\"server\":{\"nowMs\":%lu,\"scheduleRestart\":%s,\"scheduledRestartInMs\":%lu},"
            "\"memory\":{\"freeHeap\":%lu,\"minFreeHeap\":%lu,\"maxAlloc\":%lu,\"largestBlock\":%lu},"
            "\"compact\":true,\"probe\":true"
            "}}",
            now,
            ctx->scheduleRestart ? "true" : "false",
            (ctx->scheduleRestart && ctx->scheduledRestartTime > now)
                ? (ctx->scheduledRestartTime - now)
                : 0,
            (unsigned long)ESP.getFreeHeap(),
            (unsigned long)ESP.getMinFreeHeap(),
            (unsigned long)ESP.getMaxAllocHeap(),
            (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        response->addHeader("Connection", "close");
        request->send(response);
        return;
    }

    auto doc = FastBee::makeJsonDocument(24576);
    JsonObject data = doc["data"].to<JsonObject>();
    unsigned long now = millis();

    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t minFreeHeap = ESP.getMinFreeHeap();
    uint32_t maxAlloc = ESP.getMaxAllocHeap();
    uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    bool explicitFull = request->hasParam("full") && request->getParam("full")->value() == "1";
    bool compactRuntime = !explicitFull || freeHeap < 12288 || maxAlloc < 6144;
    uint8_t frag = calculateHeapFragmentationPercent(freeHeap, largestBlock);

    FastBee::setFixedJsonField(data, "server", "nowMs", now);
    FastBee::setFixedJsonField(data, "server", "scheduleRestart", ctx->scheduleRestart);
    FastBee::setFixedJsonField(data, "server", "scheduledRestartAtMs", ctx->scheduledRestartTime);
    data["server"]["scheduledRestartInMs"] =
        (ctx->scheduleRestart && ctx->scheduledRestartTime > now)
            ? (ctx->scheduledRestartTime - now)
            : 0;

    FastBee::setFixedJsonField(data, "memory", "freeHeap", freeHeap);
    FastBee::setFixedJsonField(data, "memory", "minFreeHeap", minFreeHeap);
    FastBee::setFixedJsonField(data, "memory", "maxAlloc", maxAlloc);
    FastBee::setFixedJsonField(data, "memory", "largestBlock", largestBlock);
    FastBee::setFixedJsonField(data, "memory", "fragmentation", frag);

    FastBeeFramework* fw = FastBeeFramework::getInstance();
    WebConfigManager* webCfg = fw ? fw->getWebConfigManager() : nullptr;
    FastBee::setFixedJsonField(data, "server", "webRunning", webCfg ? webCfg->isServerRunning() : false);

    if (fw && fw->getHealthMonitor()) {
        HealthMonitor* hm = fw->getHealthMonitor();
        MemoryGuardLevel level = hm->getMemoryGuardLevel();
        const char* levelText = "NORMAL";
        switch (level) {
            case MemoryGuardLevel::WARN:     levelText = "WARN"; break;
            case MemoryGuardLevel::SEVERE:   levelText = "SEVERE"; break;
            case MemoryGuardLevel::CRITICAL: levelText = "CRITICAL"; break;
            default: break;
        }
        FastBee::setFixedJsonField(data, "memory", "guardLevel", levelText);
        if (!compactRuntime) {
            char healthBuf[192];
            hm->getHealthReport(healthBuf, sizeof(healthBuf));
            FastBee::setFixedJsonField(data, "memory", "healthReport", healthBuf);
        }
    }

    if (webCfg && !compactRuntime) {
        // The current request itself occupies an accepted slot. Counts are
        // global inbound slots, sampled once, and do not enumerate live PCBs.
        const auto tcpAdmission = FastBee::captureAsyncTCPAdmissionSnapshot();
        FastBee::setFixedJsonField(data, "web", "tcpAdmissionAvailable", tcpAdmission.available);
        if (tcpAdmission.available) {
            FastBee::setFixedJsonField(data, "web", "tcpAcceptedSlots", tcpAdmission.accepted);
            FastBee::setFixedJsonField(data, "web", "tcpAcceptedSlotLimit", tcpAdmission.limit);
        }
        FastBee::setFixedJsonField(data, "web", "sseClients", webCfg->getSseClientCount());
        FastBee::setFixedJsonField(data, "web", "sseMaxClients", static_cast<uint32_t>(SSERouteHandler::MAX_SSE_CLIENTS));
        // 内存护栏（无 PSRAM 机型低 DRAM/碎片化时拒新 API 请求，见 WebMemoryGuard）
        FastBee::setFixedJsonField(data, "web", "memGuardActive", webCfg->isMemGuardActive());
        FastBee::setFixedJsonField(data, "web", "memGuardRejected", webCfg->getMemGuardRejectedCount());
        if (SSERouteHandler* sseHandler = webCfg->getSseRouteHandler()) {
            SSEStatsSnapshot sseStats = sseHandler->getStats();
            JsonObject sseStatsJson = data["web"]["sseStats"].to<JsonObject>();
            FastBee::setFixedJsonField(sseStatsJson, "acceptedConnections", sseStats.acceptedConnections);
            FastBee::setFixedJsonField(sseStatsJson, "rejectedLowMemory", sseStats.rejectedLowMemory);
            FastBee::setFixedJsonField(sseStatsJson, "rejectedGuard", sseStats.rejectedGuard);
            FastBee::setFixedJsonField(sseStatsJson, "rejectedCapacity", sseStats.rejectedCapacity);
            FastBee::setFixedJsonField(sseStatsJson, "evictedOldestClients", sseStats.evictedOldestClients);
            FastBee::setFixedJsonField(sseStatsJson, "forcedClosedClients", sseStats.forcedClosedClients);
            FastBee::setFixedJsonField(sseStatsJson, "timedOutClients", sseStats.timedOutClients);
            FastBee::setFixedJsonField(sseStatsJson, "disconnectedCleanups", sseStats.disconnectedCleanups);
            FastBee::setFixedJsonField(sseStatsJson, "skippedBroadcastLowMemory", sseStats.skippedBroadcastLowMemory);
            FastBee::setFixedJsonField(sseStatsJson, "skippedBroadcastGuard", sseStats.skippedBroadcastGuard);
            FastBee::setFixedJsonField(sseStatsJson, "lastConnectAtMs", sseStats.lastConnectAtMs);
            FastBee::setFixedJsonField(sseStatsJson, "lastRejectAtMs", sseStats.lastRejectAtMs);
            FastBee::setFixedJsonField(sseStatsJson, "lastForcedCloseAtMs", sseStats.lastForcedCloseAtMs);
            FastBee::setFixedJsonField(sseStatsJson, "lastCleanupAtMs", sseStats.lastCleanupAtMs);
            FastBee::setFixedJsonField(sseStatsJson, "lastSkipBroadcastAtMs", sseStats.lastSkipBroadcastAtMs);
            FastBee::setFixedJsonField(sseStatsJson, "lastRejectReason", sseStats.lastRejectReason);
            FastBee::setFixedJsonField(sseStatsJson, "lastSkipReason", sseStats.lastSkipReason);
        }
        FastBee::setFixedJsonField(data, "recovery", "softRestartCount", webCfg->getSoftRestartCount());
        FastBee::setFixedJsonField(data, "recovery", "lastSoftRestartAtMs", webCfg->getLastSoftRestartAtMs());
        data["recovery"]["lastSoftRestartAgeMs"] =
            (webCfg->getLastSoftRestartAtMs() > 0 && now >= webCfg->getLastSoftRestartAtMs())
                ? (now - webCfg->getLastSoftRestartAtMs())
                : 0;
        FastBee::setFixedJsonField(data, "recovery", "lastSoftRestartReason", webCfg->getLastSoftRestartReason());
        FastBee::setFixedJsonField(data, "recovery", "lastSoftRestartFreeHeap", webCfg->getLastSoftRestartFreeHeap());
        FastBee::setFixedJsonField(data, "recovery", "lastSoftRestartLargestBlock", webCfg->getLastSoftRestartLargestBlock());
        FastBee::setFixedJsonField(data, "recovery", "lastSoftRestartFragmentation", webCfg->getLastSoftRestartFragmentation());
        FastBee::setFixedJsonField(data, "recovery", "severePressureSinceMs", webCfg->getSeverePressureSinceMs());
        data["recovery"]["severePressureDurationMs"] =
            (webCfg->getSeverePressureSinceMs() > 0 && now >= webCfg->getSeverePressureSinceMs())
                ? (now - webCfg->getSeverePressureSinceMs())
                : 0;
        WebRecoveryEvent recoveryEvents[8];
        const size_t recoveryCount = webCfg->copyRecoveryEvents(recoveryEvents, 8);
        JsonArray recoveryEventsJson = data["recovery"]["events"].to<JsonArray>();
        for (size_t i = 0; i < recoveryCount; ++i) {
            JsonObject eventJson = recoveryEventsJson.add<JsonObject>();
            FastBee::setFixedJsonField(eventJson, "type", recoveryEvents[i].type);
            FastBee::setFixedJsonField(eventJson, "reason", recoveryEvents[i].reason);
            FastBee::setFixedJsonField(eventJson, "atMs", recoveryEvents[i].atMs);
            FastBee::setFixedJsonField(eventJson, "ageMs",
                (recoveryEvents[i].atMs > 0 && now >= recoveryEvents[i].atMs)
                    ? (now - recoveryEvents[i].atMs)
                    : 0);
            FastBee::setFixedJsonField(eventJson, "freeHeap", recoveryEvents[i].freeHeap);
            FastBee::setFixedJsonField(eventJson, "largestBlock", recoveryEvents[i].largestBlock);
            FastBee::setFixedJsonField(eventJson, "fragmentation", recoveryEvents[i].fragmentation);
            FastBee::setFixedJsonField(eventJson, "sseClients", recoveryEvents[i].sseClients);
        }
    }

    if (ctx->networkManager) {
        FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
        if (!compactRuntime) {
            netMgr->updateStatusInfo();
        }
        NetworkStatusInfo info = netMgr->getStatusInfo();
        WiFiConfig cfg = netMgr->getConfig();

        const char* statusText = "unknown";
        switch (info.status) {
            case NetworkStatus::CONNECTED:         statusText = "connected"; break;
            case NetworkStatus::DISCONNECTED:      statusText = "disconnected"; break;
            case NetworkStatus::CONNECTING:        statusText = "connecting"; break;
            case NetworkStatus::AP_MODE:           statusText = "ap_mode"; break;
            case NetworkStatus::CONNECTION_FAILED: statusText = "failed"; break;
            default: break;
        }
        const char* modeText = "unknown";
        switch (cfg.mode) {
            case NetworkMode::NETWORK_STA: modeText = "STA"; break;
            case NetworkMode::NETWORK_AP:  modeText = "AP"; break;
            default: break;
        }

        FastBee::setFixedJsonField(data, "network", "status", statusText);
        FastBee::setFixedJsonField(data, "network", "mode", modeText);
        FastBee::setFixedJsonField(data, "network", "ipAddress", info.ipAddress);
        FastBee::setFixedJsonField(data, "network", "apIPAddress", info.apIPAddress);
        FastBee::setFixedJsonField(data, "network", "ssid", info.ssid.isEmpty() ? cfg.staSSID : info.ssid);
        FastBee::setFixedJsonField(data, "network", "internetAvailable", info.internetAvailable);
        FastBee::setFixedJsonField(data, "network", "customDomain", cfg.customDomain);
        auto* dns = netMgr->getDNSManager();
        if (dns) {
            FastBee::setFixedJsonField(data, "network", "mdnsDomain", dns->getActualHostname());
        }
    }

    FastBee::setFixedJsonField(data, "compact", compactRuntime);
    doc["success"] = true;
    HandlerUtils::sendJsonStream(request, doc);
}

void SystemRouteHandler::handleGetCapabilities(AsyncWebServerRequest* request) {
    JsonDocument doc;

    FastBee::setFixedJsonField(doc, "data", "mqtt", (bool)FASTBEE_ENABLE_MQTT);
    FastBee::setFixedJsonField(doc, "data", "modbus", (bool)FASTBEE_ENABLE_MODBUS);

    FastBee::setFixedJsonField(doc, "data", "periphExec", (bool)FASTBEE_ENABLE_PERIPH_EXEC);
    FastBee::setFixedJsonField(doc, "data", "screenPanel", (bool)FASTBEE_ENABLE_SCREEN_PANEL);
    FastBee::setFixedJsonField(doc, "data", "screenUi", (bool)FASTBEE_ENABLE_SCREEN_UI);
    FastBee::setFixedJsonField(doc, "data", "pointHistory", (bool)FASTBEE_ENABLE_POINT_HISTORY);
    FastBee::setFixedJsonField(doc, "data", "remoteResources", (bool)FASTBEE_ENABLE_REMOTE_RESOURCES);
    FastBee::setFixedJsonField(doc, "data", "ruleScript", (bool)FASTBEE_ENABLE_RULE_SCRIPT);
    FastBee::setFixedJsonField(doc, "data", "lcd", (bool)FASTBEE_ENABLE_LCD);
    FastBee::setFixedJsonField(doc, "data", "ethernet", (bool)FASTBEE_ENABLE_ETHERNET);
    FastBee::setFixedJsonField(doc, "data", "cellular", (bool)FASTBEE_ENABLE_CELLULAR);
    FastBee::setFixedJsonField(doc, "data", "ota", (bool)FASTBEE_ENABLE_OTA);
    FastBee::setFixedJsonField(doc, "data", "auth", (bool)FASTBEE_ENABLE_AUTH);
    FastBee::setFixedJsonField(doc, "data", "webServer", (bool)FASTBEE_ENABLE_WEB_SERVER);
    FastBee::setFixedJsonField(doc, "data", "healthMonitor", (bool)FASTBEE_ENABLE_HEALTH_MONITOR);
    FastBee::setFixedJsonField(doc, "data", "logger", (bool)FASTBEE_ENABLE_LOGGER);
    FastBee::setFixedJsonField(doc, "data", "logViewer", (bool)FASTBEE_ENABLE_LOG_VIEWER);
    FastBee::setFixedJsonField(doc, "data", "fileLogging", (bool)FASTBEE_ENABLE_FILE_LOGGING);
    FastBee::setFixedJsonField(doc, "data", "taskManager", (bool)FASTBEE_ENABLE_TASK_MANAGER);

    doc["success"] = true;
    HandlerUtils::sendJsonStream(request, doc);
}

// Memory profile feature disabled - not registered in routes
// void SystemRouteHandler::handleGetMemoryProfile(AsyncWebServerRequest* request) {
//     FastBee::MemoryProfileDB& db = FastBee::MemoryProfileDB::instance();
//     String json = db.toJson();
//
//     JsonDocument doc;
//     doc["success"] = true;
//     doc["data"].set(serialized(json));
//
//     HandlerUtils::sendJsonStream(request, doc);
// }

// ---------------------------------------------------------------------------
// 路由业务实现（setupRoutes 注册表一行注册对应的 handle* 下沉方法，便于单测）
// ---------------------------------------------------------------------------

void SystemRouteHandler::handleNetworkStatus(AsyncWebServerRequest* request) {
    // Network status handler - critical endpoint, bypass heavy memory guard
    if (!ctx->requireAuth(request)) return;
    auto doc = FastBee::makeJsonDocument();
    doc["success"] = true;
    if (ctx->networkManager) {
        FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
        netMgr->updateStatusInfo();
        NetworkStatusInfo info = netMgr->getStatusInfo();
        WiFiConfig cfg = netMgr->getConfig();
        netMgr->patchStatusInfoWithWiFiLink(info);
        const char* statusText = "unknown";
        switch (info.status) {
            case NetworkStatus::CONNECTED:        statusText = "connected";   break;
            case NetworkStatus::DISCONNECTED:     statusText = "disconnected"; break;
            case NetworkStatus::CONNECTING:       statusText = "connecting";  break;
            case NetworkStatus::AP_MODE:          statusText = "ap_mode";     break;
            case NetworkStatus::CONNECTION_FAILED:statusText = "failed";      break;
            default: break;
        }
        FastBee::setFixedJsonField(doc, "data", "status", statusText);
        FastBee::setFixedJsonField(doc, "data", "statusCode", static_cast<uint8_t>(info.status));
        FastBee::setFixedJsonField(doc, "data", "connected", (info.status == NetworkStatus::CONNECTED));
        FastBee::setFixedJsonField(doc, "data", "deviceNetworkType", static_cast<uint8_t>(cfg.networkType));
        FastBee::setFixedJsonField(doc, "data", "ipAddress", info.ipAddress);
        FastBee::setFixedJsonField(doc, "data", "macAddress", info.macAddress.isEmpty() ? WiFi.macAddress() : info.macAddress);
        FastBee::setFixedJsonField(doc, "data", "connectedTime", info.lastConnectionTime > 0 ? (millis() - info.lastConnectionTime) / 1000 : 0);
        FastBee::setFixedJsonField(doc, "data", "apIPAddress", info.apIPAddress);
        FastBee::setFixedJsonField(doc, "data", "apClientCount", info.apClientCount);
        FastBee::setFixedJsonField(doc, "data", "apSSID", cfg.apSSID);
        FastBee::setFixedJsonField(doc, "data", "internetAvailable", info.internetAvailable);

        // WiFi 鐗规湁瀛楁
        if (cfg.networkType == NetworkType::NET_WIFI) {
            FastBee::setFixedJsonField(doc, "data", "ssid", info.ssid.isEmpty() ? cfg.staSSID : info.ssid);
            FastBee::setFixedJsonField(doc, "data", "rssi", info.rssi);
            FastBee::setFixedJsonField(doc, "data", "signalStrength", NetworkUtils::rssiToPercentage(info.rssi));
            FastBee::setFixedJsonField(doc, "data", "gateway", info.currentGateway);
            FastBee::setFixedJsonField(doc, "data", "subnet", info.currentSubnet);
            FastBee::setFixedJsonField(doc, "data", "dnsServer", info.dnsServer);
            FastBee::setFixedJsonField(doc, "data", "reconnectAttempts", info.reconnectAttempts);
        }

        // 4G 铚傜獫缃戠粶鐘舵€侊紙浠呭湪 4G 妯″紡鏃惰繑鍥烇級
        if (cfg.networkType == NetworkType::NET_4G) {
            FastBee::setFixedJsonField(doc, "data", "simStatus", info.simStatus);
            FastBee::setFixedJsonField(doc, "data", "operator", info.operatorName);
            FastBee::setFixedJsonField(doc, "data", "networkType", info.cellularNetworkType);
            FastBee::setFixedJsonField(doc, "data", "apn", info.apn);
            FastBee::setFixedJsonField(doc, "data", "imei", info.imei);
            FastBee::setFixedJsonField(doc, "data", "iccId", info.iccid);
            FastBee::setFixedJsonField(doc, "data", "signalQuality", info.cellularSignalQuality);
            FastBee::setFixedJsonField(doc, "data", "rssi", info.rssi);
            FastBee::setFixedJsonField(doc, "data", "signalStrength", NetworkUtils::rssiToPercentage(info.rssi));
        }

        // 浠ュお缃戠姸鎬?
        if (cfg.networkType == NetworkType::NET_ETHERNET) {
            FastBee::setFixedJsonField(doc, "data", "gateway", info.currentGateway);
        }

        const char* modeText = "unknown";
        switch (cfg.mode) {
            case NetworkMode::NETWORK_STA:    modeText = "STA"; break;
            case NetworkMode::NETWORK_AP:     modeText = "AP";  break;
            default: break;
        }
        FastBee::setFixedJsonField(doc, "data", "mode", modeText);
        FastBee::setFixedJsonField(doc, "data", "modeCode", static_cast<uint8_t>(cfg.mode));
        FastBee::setFixedJsonField(doc, "data", "enableMDNS", cfg.enableMDNS);
        FastBee::setFixedJsonField(doc, "data", "customDomain", cfg.customDomain);
        DNSManager* dns = netMgr->getDNSManager();
        if (dns) {
            FastBee::setFixedJsonField(doc, "data", "mdnsDomain", dns->getActualHostname());
        }
        unsigned long uptimeSec = millis() / 1000;
        char uptimeBuf[32];
        snprintf(uptimeBuf, sizeof(uptimeBuf), "%lud %02lu:%02lu:%02lu",
            uptimeSec / 86400, (uptimeSec % 86400) / 3600,
            (uptimeSec % 3600) / 60, uptimeSec % 60);
        FastBee::setFixedJsonField(doc, "data", "uptimeFormatted", uptimeBuf);
    } else {
        doc["success"] = false;
        doc["error"] = "Network service unavailable";
    }
    // 缃戠粶鐘舵€佹槸UI鍏抽敭绔偣锛屽搷搴斾綋杈冨皬锛?1KB锛夛紝鐩存帴鍙戦€佷笉妫€鏌ュ唴瀛樺畧鍗?
    HandlerUtils::sendJsonStream(request, doc);
}

void SystemRouteHandler::handleUpdateNetworkConfig(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->networkManager) {
        ctx->sendError(request, 500, "Network service unavailable");
        return;
    }
    FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
    WiFiConfig cfg = netMgr->getConfig();
    JsonObject obj = json.as<JsonObject>();
    if (obj["mode"].is<String>()) cfg.mode = static_cast<NetworkMode>(obj["mode"].as<String>().toInt());
    if (obj["staSSID"].is<String>()) cfg.staSSID = obj["staSSID"].as<String>();
    if (obj["staPassword"].is<String>()) { String pwd = obj["staPassword"].as<String>(); if (!pwd.isEmpty() && pwd != "********") cfg.staPassword = pwd; }
    if (obj["ipConfigType"].is<String>()) cfg.ipConfigType = static_cast<IPConfigType>(obj["ipConfigType"].as<String>().toInt());
    if (obj["staticIP"].is<String>()) cfg.staticIP = obj["staticIP"].as<String>();
    if (obj["gateway"].is<String>()) cfg.gateway = obj["gateway"].as<String>();
    if (obj["subnet"].is<String>()) cfg.subnet = obj["subnet"].as<String>();
    if (obj["apSSID"].is<String>()) cfg.apSSID = obj["apSSID"].as<String>();
    if (obj["apPassword"].is<String>()) { String pwd = obj["apPassword"].as<String>(); if (!pwd.isEmpty() && pwd != "********") cfg.apPassword = pwd; }
    if (obj["apIP"].is<String>()) cfg.apIP = obj["apIP"].as<String>();
    if (obj["apChannel"].is<String>() || obj["apChannel"].is<int>()) cfg.apChannel = obj["apChannel"].as<int>();
    if (obj["apHidden"].is<String>() || obj["apHidden"].is<bool>()) { String h = obj["apHidden"].as<String>(); cfg.apHidden = (h == "1" || h == "true" || obj["apHidden"].as<bool>()); }
    if (obj["apMaxConnections"].is<String>() || obj["apMaxConnections"].is<int>()) cfg.apMaxConnections = obj["apMaxConnections"].as<int>();
    if (obj["dns1"].is<String>()) cfg.dns1 = obj["dns1"].as<String>();
    if (obj["dns2"].is<String>()) cfg.dns2 = obj["dns2"].as<String>();
    if (obj["enableMDNS"].is<String>() || obj["enableMDNS"].is<bool>()) { String m = obj["enableMDNS"].as<String>(); cfg.enableMDNS = (m == "1" || m == "true" || obj["enableMDNS"].as<bool>()); }
    if (obj["customDomain"].is<String>()) cfg.customDomain = obj["customDomain"].as<String>();
    if (obj["connectTimeout"].is<String>() || obj["connectTimeout"].is<int>()) cfg.connectTimeout = obj["connectTimeout"].as<int>();
    if (obj["reconnectInterval"].is<String>() || obj["reconnectInterval"].is<int>()) cfg.reconnectInterval = obj["reconnectInterval"].as<int>();
    if (obj["maxReconnectAttempts"].is<String>() || obj["maxReconnectAttempts"].is<int>()) cfg.maxReconnectAttempts = obj["maxReconnectAttempts"].as<int>();
    if (obj["conflictDetection"].is<String>() || obj["conflictDetection"].is<int>()) cfg.conflictDetection = static_cast<IPConflictMode>(obj["conflictDetection"].as<int>());
    // 鑱旂綉鏂瑰紡
    if (obj["networkType"].is<String>() || obj["networkType"].is<int>()) {
        uint8_t rawNetworkType = obj["networkType"].as<String>().toInt();
        cfg.networkType = rawNetworkType <= static_cast<uint8_t>(NetworkType::NET_4G)
            ? static_cast<NetworkType>(rawNetworkType)
            : NetworkType::NET_WIFI;
    }
    // 浠ュお缃戦厤缃?
    if (obj.containsKey("ethernet")) {
        String pinError;
        if (!obj["ethernet"].is<JsonObject>() ||
            !FastBee::parseEthernetResourcePins(obj["ethernet"].as<JsonObjectConst>(),
                                               cfg.ethernet, pinError)) {
            ctx->sendError(request, 400, pinError.isEmpty() ? String("NETWORK_PIN_INVALID: ethernet object required") : pinError);
            return;
        }
    }
    // 4G 铚傜獫閰嶇疆
    if (obj.containsKey("cellular")) {
        String pinError;
        if (!obj["cellular"].is<JsonObject>() ||
            !FastBee::parseCellularResourcePins(obj["cellular"].as<JsonObjectConst>(),
                                               cfg.cellular, pinError)) {
            ctx->sendError(request, 400, pinError.isEmpty() ? String("NETWORK_PIN_INVALID: cellular object required") : pinError);
            return;
        }
        JsonObject cell = obj["cellular"];
        if (cell.containsKey("baudRate")) cfg.cellular.baudRate = cell["baudRate"].as<uint32_t>();
        if (cell.containsKey("apn"))      cfg.cellular.apn      = cell["apn"].as<String>();
    }
    String resourceError;
    if (netMgr->updateConfig(cfg, true, &resourceError)) {
        LOGGER.info("Network configuration updated via web");

        // 配置保存成功，取消运行时网络重启（改为设备重启）
        // updateConfig() 可能已设置 pendingRestart=true，这里清除它
        // 设备重启比运行时 destroy/rebuild 更可靠：干净堆状态，无内存碎片
        netMgr->clearPendingRestart();

        // 构建详细响应，包含网络模式和访问信息
        auto respDoc = FastBee::makeJsonDocument();
        respDoc["success"] = true;
        respDoc["message"] = "Network configuration saved, device will reboot";

        // 网络模式信息
        FastBee::setFixedJsonField(respDoc, "data", "networkType", static_cast<uint8_t>(cfg.networkType));
        FastBee::setFixedJsonField(respDoc, "data", "mode", static_cast<uint8_t>(cfg.mode));
        const char* modeText = "unknown";
        switch (cfg.mode) {
            case NetworkMode::NETWORK_STA:    modeText = "STA"; break;
            case NetworkMode::NETWORK_AP:     modeText = "AP";  break;
            default: break;
        }
        FastBee::setFixedJsonField(respDoc, "data", "modeText", modeText);

        // AP信息（AP模式需要）
        if (cfg.mode == NetworkMode::NETWORK_AP) {
            FastBee::setFixedJsonField(respDoc, "data", "apSSID", cfg.apSSID);
            FastBee::setFixedJsonField(respDoc, "data", "apPassword", cfg.apPassword);
            FastBee::setFixedJsonField(respDoc, "data", "apIP", cfg.apIP);
        }

        // mDNS信息（STA模式需要）
        if (cfg.mode == NetworkMode::NETWORK_STA && cfg.enableMDNS) {
            String domain = cfg.customDomain.length() > 0 ? cfg.customDomain : "fastbee";
            FastBee::setFixedJsonField(respDoc, "data", "mdnsDomain", domain + ".local");
        }

        // 提示设备将重启
        FastBee::setFixedJsonField(respDoc, "data", "restartRequired", true);

        HandlerUtils::sendJsonStream(request, respDoc);

        // HTTP 响应已发送，调度延迟重启（2秒后 ESP.restart）
        SystemRebooter::scheduleConfigReboot("Network config changed");
    } else {
        const bool busy = resourceError.startsWith("NETWORK_RESOURCE_BUSY") ||
                          resourceError.startsWith("NETWORK_RESOURCE_MEMORY");
        ctx->sendError(request, resourceError.isEmpty() ? 500 : (busy ? 503 : 422),
                       resourceError.isEmpty() ? String("Failed to save network configuration") : resourceError);
    }
}

#if FASTBEE_ENABLE_CONFIG_TRANSFER
void SystemRouteHandler::handleConfigTransferList(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;
    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Config list", MemoryGuardLevel::SEVERE, 8)) return;
    if (HandlerUtils::checkLowMemory(request, 8192)) return;

    JsonDocument doc;
    doc["success"] = true;
    JsonArray files = doc["data"]["files"].to<JsonArray>();

    File root = LittleFS.open("/config");
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        uint8_t count = 0;
        while (file && count < 32) {
            if (!file.isDirectory()) {
                String name = basenameOfFile(file.name());
                if (isAllowedConfigTransferFileName(name) && file.size() <= kMaxConfigTransferBytes) {
                    JsonObject item = files.add<JsonObject>();
                    item["name"] = name;
                    item["size"] = file.size();
                    count++;
                }
            }
            file = root.openNextFile();
        }
        root.close();
    }

    HandlerUtils::sendJsonStream(request, doc);
}

void SystemRouteHandler::handleConfigTransferExport(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;
    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Config export", MemoryGuardLevel::SEVERE, 8)) return;
    if (!request->hasParam("name")) { ctx->sendError(request, 400, "Missing name parameter"); return; }
    String name = request->getParam("name")->value();
    if (!isAllowedConfigTransferFileName(name)) { ctx->sendError(request, 403, "Invalid config file name"); return; }

    String path = configPathForName(name);
    if (!LittleFS.exists(path)) { ctx->sendError(request, 404, "Config file not found"); return; }
    File file = LittleFS.open(path, "r");
    if (!file) { ctx->sendError(request, 500, "Failed to open config file"); return; }
    if (file.size() > kMaxConfigTransferBytes) {
        file.close();
        ctx->sendError(request, 413, "Config file too large");
        return;
    }

    auto state = std::make_shared<File>(file);
    AsyncWebServerResponse* resp = request->beginChunkedResponse(
        "application/json",
        [state](uint8_t* buffer, size_t maxLen, size_t /*index*/) -> size_t {
            if (!state || !(*state)) return 0;
            if (!state->available()) {
                state->close();
                return 0;
            }
            return state->read(buffer, maxLen);
        });
    if (!resp) {
        file.close();
        HandlerUtils::sendJsonError(request, 500, "Failed to create export response");
        return;
    }
    resp->addHeader("Content-Disposition", String("attachment; filename=\"") + name + "\"");
    resp->addHeader("Cache-Control", "no-store");
    resp->addHeader("Connection", "close");
    request->send(resp);
}

void SystemRouteHandler::handleConfigTransferImport(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;
    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Config import", MemoryGuardLevel::SEVERE, 8)) return;
    if (!request->hasParam("name", true) || !request->hasParam("content", true)) {
        ctx->sendError(request, 400, "Missing name or content parameter");
        return;
    }
    String name = request->getParam("name", true)->value();
    const String& content = request->getParam("content", true)->value();
    if (!isSafeConfigFileName(name)) { ctx->sendError(request, 403, "文件名非法（只允许字母、数字、下划线、短横线和 .json 后缀）"); return; }
    if (isManagedRuntimeConfigFileName(name)) {
        ctx->sendError(request, 409, "该运行配置必须通过专用 API 导入，以保证校验、事务提交和运行态一致");
        return;
    }
    if (isIgnoredConfigImportFileName(name)) {
        JsonDocument skipped;
        skipped["name"] = name;
        skipped["skipped"] = true;
        ctx->sendSuccess(request, skipped);
        return;
    }
    // 白名单检查：只允许导入已知的配置文件，防止在 /config/ 下生成非预期文件
    if (!isAllowedConfigImportFileName(name)) {
        ctx->sendError(request, 403,
            ("不允许导入配置文件：" + name + "。网络、协议和认证配置必须通过各自的专用页面/API 修改").c_str());
        return;
    }
    if (content.isEmpty()) {
        ctx->sendError(request, 413, "配置文件内容为空");
        return;
    }
    if (content.length() > kMaxConfigInlineImportBytes) {
        ctx->sendError(request, 413,
            ("配置文件过大（" + String(content.length()) + " 字节），单文件最大 " + String(kMaxConfigInlineImportBytes / 1024) + " KB，请使用分片导入").c_str());
        return;
    }
    if (HandlerUtils::checkLowMemory(request, 24576)) return;

    FastBee::ConfigImportSession::Guard importGuard(ordinaryImport);
    if (!importGuard) { ctx->sendError(request, 503, "配置导入忙，请稍后核验"); return; }
    if (ordinaryImport.busy(millis())) {
        ctx->sendError(request, 409, "已有分片导入进行中"); return;
    }

    JsonDocument importedConfig;
    DeserializationError importError = deserializeJson(importedConfig, content.c_str(), content.length());
    if (importError || (!importedConfig.is<JsonObject>() && name != "ir_keys.json")) {
        ctx->sendError(request, 422, "配置文件内容不是有效的 JSON 对象，请检查文件内容");
        return;
    }

    // Flash 空间检查：确保有足够空间写入，防止存储空间耗尽
    size_t _fsTotal, _fsUsed;
    getCachedFsStats(_fsTotal, _fsUsed);
    size_t freeSpace = _fsTotal > _fsUsed ? _fsTotal - _fsUsed : 0;
    if (freeSpace < content.length() * 2 + 4096) {  // 2x 安全余量 + 4KB 缓冲
        ctx->sendError(request, 507,
            ("Flash 存储空间不足（剩余 " + String(freeSpace) + " 字节），请先清理不需要的配置").c_str());
        return;
    }

    if (!LittleFS.exists("/config") && !LittleFS.mkdir("/config")) {
        ctx->sendError(request, 500, "Failed to create config directory");
        return;
    }

    String path = configPathForName(name);
    ConfigStorage& configStorage = ConfigStorage::getInstance();
    if (!configStorage.saveImportedJSONConfig(path, importedConfig)) {
        ctx->sendError(request, 500, "配置文件原子提交失败，已保留原配置");
        return;
    }
    const size_t written = measureJson(importedConfig);
    ordinaryImport.invalidate();

    if (name == "device.json" || name == "protocol.json") {
        FastBeeFramework* fw = FastBeeFramework::getInstance();
        if (fw) fw->ensureDeviceIdentity();
    }

    JsonDocument doc;
    doc["name"] = name;
    doc["size"] = written;
    doc["saved"] = true;
    doc["applied"] = false;
    doc["restartRequired"] = true;
    ctx->sendSuccess(request, doc);
}

void SystemRouteHandler::handleConfigTransferImportBegin(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;
    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Config import begin", MemoryGuardLevel::SEVERE, 8)) return;
    if (!request->hasParam("name", true) || !request->hasParam("total", true) ||
        !request->hasParam("size", true)) {
        ctx->sendError(request, 400, "缺少导入会话参数"); return;
    }
    const String& name = request->getParam("name", true)->value();
    if (!isSafeConfigFileName(name)) { ctx->sendError(request, 403, "文件名非法"); return; }
    if (isManagedRuntimeConfigFileName(name)) {
        ctx->sendError(request, 409, "该运行配置必须通过专用 API 导入"); return;
    }
    if (!isAllowedConfigImportFileName(name)) {
        ctx->sendError(request, 403, "不允许导入该配置文件"); return;
    }
    uint32_t total = 0, size = 0;
    if (!parseConfigImportNumber(request->getParam("total", true)->value(), kMaxConfigTransferChunks, total) ||
        !parseConfigImportNumber(request->getParam("size", true)->value(), kMaxConfigInlineImportBytes, size) ||
        !total || !size || total > size) {
        ctx->sendError(request, 400, "导入大小或分片数量无效，普通配置最大24KB"); return;
    }
    FastBee::ConfigImportSession::Guard importGuard(ordinaryImport);
    if (!importGuard) { ctx->sendError(request, 503, "配置导入忙，请稍后核验"); return; }
    if (ordinaryImport.busy(millis())) {
        ctx->sendError(request, 409, "已有分片导入进行中，请完成或等待60秒空闲后核验"); return;
    }
    if (!LittleFS.exists("/config") && !LittleFS.mkdir("/config")) {
        ctx->sendError(request, 500, "Failed to create config directory"); return;
    }
    // Only retire our own previous staging file. Never sweep other uploads.
    if (ordinaryImport.name[0]) {
        const String previousStage = configPathForName(ordinaryImport.name) + ".import.tmp";
        if (LittleFS.exists(previousStage) && !LittleFS.remove(previousStage)) {
            ctx->sendError(request, 500, "无法释放上一导入暂存文件"); return;
        }
    }
    ordinaryImport.invalidate();
    String stagedPath = configPathForName(name) + ".import.tmp";
    File staged = LittleFS.open(stagedPath, "w");
    if (!staged) { ctx->sendError(request, 500, "无法建立导入暂存文件"); return; }
    staged.close();
    uint8_t random[16];
    char session[33];
    static const char hex[] = "0123456789abcdef";
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); ++i) {
        session[i * 2] = hex[random[i] >> 4]; session[i * 2 + 1] = hex[random[i] & 15];
    }
    session[32] = '\0';
    if (!ordinaryImport.begin(session, name.c_str(), total, size, millis())) {
        ctx->sendError(request, 500, "无法建立导入会话"); return;
    }
    JsonDocument doc;
    doc["name"] = name; doc["session"] = session;
    doc["total"] = total; doc["expectedSize"] = size; doc["size"] = 0;
    doc["saved"] = false; doc["applied"] = false; doc["restartRequired"] = false;
    ctx->sendSuccess(request, doc);
}

void SystemRouteHandler::handleConfigTransferImportChunk(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;
    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Config chunk import", MemoryGuardLevel::SEVERE, 8)) return;
    if (!request->hasParam("name", true) || !request->hasParam("chunk", true)
        || !request->hasParam("index", true) || !request->hasParam("total", true)
        || !request->hasParam("session", true)) {
        ctx->sendError(request, 400, "缺少分片导入参数，须先建立导入会话");
        return;
    }

    String name = request->getParam("name", true)->value();
    const String& chunk = request->getParam("chunk", true)->value();
    const String& session = request->getParam("session", true)->value();
    uint32_t index = 0, total = 0;
    if (!parseConfigImportNumber(request->getParam("index", true)->value(), kMaxConfigTransferChunks, index) ||
        !parseConfigImportNumber(request->getParam("total", true)->value(), kMaxConfigTransferChunks, total)) {
        ctx->sendError(request, 400, "分片索引必须是规范十进制整数"); return;
    }
    if (session.length() != 32) { ctx->sendError(request, 400, "导入会话无效"); return; }

    if (!isSafeConfigFileName(name)) { ctx->sendError(request, 403, "文件名非法"); return; }
    if (isManagedRuntimeConfigFileName(name)) {
        ctx->sendError(request, 409, "该运行配置必须通过专用 API 导入，以保证校验、事务提交和运行态一致");
        return;
    }
    if (isIgnoredConfigImportFileName(name)) {
        JsonDocument skipped;
        skipped["name"] = name;
        skipped["skipped"] = true;
        ctx->sendSuccess(request, skipped);
        return;
    }
    // 白名单检查（与 inline import 一致）
    if (!isAllowedConfigImportFileName(name)) {
        ctx->sendError(request, 403,
            ("不允许导入配置文件：" + name + "。网络、协议和认证配置必须通过各自的专用页面/API 修改").c_str());
        return;
    }
    if (total == 0 || total > kMaxConfigTransferChunks || index >= total) {
        ctx->sendError(request, 400, "分片索引无效");
        return;
    }
    if (chunk.isEmpty()) {
        // 空分片几乎都是退化链路下跨包 body 丢失所致（而非客户端真发空值），给出可行动的提示
        ctx->sendError(request, 400, "分片内容接收为空，可能是网络传输失败，请重试导入");
        return;
    }
    if (chunk.length() > kMaxConfigTransferChunkBytes) {
        ctx->sendError(request, 413,
            ("分片过大（" + String(chunk.length()) + " 字节），最大 " + String(kMaxConfigTransferChunkBytes / 1024) + " KB").c_str());
        return;
    }
    if (HandlerUtils::checkLowMemory(request, 24576)) return;

    FastBee::ConfigImportSession::Guard importGuard(ordinaryImport);
    if (!importGuard) { ctx->sendError(request, 503, "配置导入忙，请稍后核验"); return; }
    const auto admission = ordinaryImport.inspect(session.c_str(), name.c_str(), index, total, chunk.length(), millis());
    if (admission == FastBee::ConfigImportSession::Admission::Reject) {
        ctx->sendError(request, 409, "导入会话过期、不匹配或分片乱序，请核验后恢复"); return;
    }
    String path = configPathForName(name);
    String stagedPath = path + ".import.tmp";
    File existing = LittleFS.open(stagedPath, "r");
    if (!existing || existing.size() != ordinaryImport.received) {
        existing.close(); ordinaryImport.failed();
        ctx->sendError(request, 409, "导入暂存文件已变化，请核验后恢复"); return;
    }
    const bool replay = admission == FastBee::ConfigImportSession::Admission::ReplayLast;
    if (replay) {
        bool equal = existing.seek(ordinaryImport.lastOffset);
        for (size_t i = 0; equal && i < chunk.length(); ++i)
            equal = existing.read() == static_cast<uint8_t>(chunk[i]);
        existing.close();
        if (!equal) { ctx->sendError(request, 409, "重复分片内容不一致"); return; }
    } else {
        existing.close();
        File out = LittleFS.open(stagedPath, "a");
        if (!out) { ordinaryImport.failed(); ctx->sendError(request, 500, "无法打开导入暂存文件"); return; }
        const size_t written = out.print(chunk);
        out.flush();
        const size_t stored = out.size();
        out.close();
        if (written != chunk.length() || stored != ordinaryImport.received + written) {
            ordinaryImport.failed();
            ctx->sendError(request, 500, "分片写入不完整，已停止该会话"); return;
        }
        ordinaryImport.appended(written, millis());
    }

    if (!replay && index == total - 1) {
        File staged = LittleFS.open(stagedPath, "r");
        if (!staged) {
            ordinaryImport.failed();
            ctx->sendError(request, 500, "Failed to reopen staged config");
            return;
        }
        JsonDocument importedConfig;
        DeserializationError importError = deserializeJson(importedConfig, staged);
        staged.close();
        if (importError || (!importedConfig.is<JsonObject>() && name != "ir_keys.json")) {
            ordinaryImport.failed();
            ctx->sendError(request, 422, "完整配置不是有效的 JSON 对象，原配置未修改");
            return;
        }

        ConfigStorage& configStorage = ConfigStorage::getInstance();
        if (!configStorage.saveImportedJSONConfig(path, importedConfig)) {
            ordinaryImport.failed();
            ctx->sendError(request, 500, "配置文件原子提交失败，已保留原配置");
            return;
        }
        // Retain the exact input for a bounded final acknowledgement replay;
        // normalized active JSON is not byte-identical to the input.
        ordinaryImport.committed();

        if (name == "device.json") {
            FastBeeFramework* fw = FastBeeFramework::getInstance();
            if (fw) fw->ensureDeviceIdentity();
        }
    }

    JsonDocument doc;
    doc["name"] = name;
    doc["session"] = session;
    doc["index"] = index;
    doc["total"] = total;
    doc["size"] = ordinaryImport.received;
    doc["saved"] = index == total - 1;
    doc["applied"] = false;
    doc["restartRequired"] = index == total - 1;
    ctx->sendSuccess(request, doc);
}

#endif // FASTBEE_ENABLE_CONFIG_TRANSFER

#if FASTBEE_ENABLE_FILE_MANAGER
void SystemRouteHandler::handleGetFileContent(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;
    if (!request->hasParam("path")) { ctx->sendError(request, 400, "Missing path parameter"); return; }
    String path = request->getParam("path")->value();
    if (!path.startsWith("/")) path = "/" + path;
    if (!isSafeFileApiPath(path)) { ctx->sendError(request, 403, "Invalid path"); return; }
    if (path.startsWith("/config/")) {
        String name = basenameOfFile(path.c_str());
        if (!isAllowedConfigTransferFileName(name)) {
            ctx->sendError(request, 403, "Sensitive or unmanaged configuration cannot be read through the file API");
            return;
        }
    }
    if (!LittleFS.exists(path)) { ctx->sendError(request, 404, "File not found"); return; }
    File file = LittleFS.open(path, "r");
    if (!file) { ctx->sendError(request, 500, "Failed to open file"); return; }
    size_t fileSize = file.size();
    if (fileSize > 128 * 1024) { file.close(); ctx->sendError(request, 413, "File too large (max 128KB)"); return; }

    // 鈽?Raw 蹇€熻矾寰勶細raw=1 鏃剁洿鎺?chunked 娴佸紡鍥炰紶鏂囦欢鍘熷瀛楄妭锛堟棤 JSON 鍖呰銆佹棤杞箟锛?
    // 鐢ㄤ簬閰嶇疆鏂囦欢瀵煎嚭鍦烘櫙锛氬嵆浣胯澶囧浜?MemGuard CRITICAL 绾т篃鑳芥垚鍔熶笅杞斤紝
    // 鍐呭瓨寮€閿€浠厏256B TCP 鍙戦€佺紦鍐?+ File 鍙ユ焺锛岃劚鏁忕敱鍓嶇鍦ㄦ祻瑙堝櫒鏈湴瀹屾垚銆?
    bool rawDownload = request->hasParam("raw") && request->getParam("raw")->value() == "1";
    if (rawDownload) {
        auto state = std::make_shared<File>(file);
        AsyncWebServerResponse* resp = request->beginChunkedResponse(
            "application/octet-stream",
            [state](uint8_t* buffer, size_t maxLen, size_t /*index*/) -> size_t {
                if (!state || !(*state)) return 0;
                if (!state->available()) {
                    state->close();
                    return 0;
                }
                return state->read(buffer, maxLen);
            });
        if (!resp) {
            file.close();
            HandlerUtils::sendJsonError(request, 500, "Failed to create raw response");
            return;
        }
        String fileName = path;
        int lastSlash = fileName.lastIndexOf('/');
        if (lastSlash >= 0) fileName = fileName.substring(lastSlash + 1);
        resp->addHeader("Content-Disposition", String("attachment; filename=\"") + fileName + "\"");
        resp->addHeader("X-Content-Type-Options", "nosniff");
        resp->addHeader("Cache-Control", "no-store");
        resp->addHeader("Connection", "close");
        request->send(resp);
        return;
    }

    // 鏂囦欢鍒嗙骇锛堢敤浜?MemGuard 绛栫暐鍐崇瓥锛夛細
    // - isTinyFile (鈮?4KB)锛氭瀬灏忔枃浠讹紙device.json/network.json锛?
    // - isConfigFile (/config/ 涓?鈮?16KB)锛氬彈淇濇姢閰嶇疆鏂囦欢锛坧eripherals/protocol/periph_exec锛?
    // - 鍏跺畠锛氭櫘閫氭枃浠?
    const bool isTinyFile = (fileSize <= 4096);
    const bool isConfigFile = path.startsWith("/config/") && (fileSize <= 16 * 1024);
    const bool isProtectedFile = isTinyFile || isConfigFile;

    // MemGuard 绛栫暐锛坈hunked 娴佸紡鍝嶅簲锛屽唴瀛樺崰鐢ㄦ亽瀹?~256B 缂撳啿 + 灏戦噺 state锛夛細
    // - 鍙椾繚鎶ゆ枃浠讹紙config/tiny锛夛細鏃犺 MemGuard 绾у埆锛屽缁堟斁琛岋紙CRITICAL 绾т笅涔熻兘鏁戞彺瀵煎嚭锛?
    // - 闈炲彈淇濇姢鏂囦欢锛欳RITICAL 绾у叏鎷掞紱SEVERE 绾ф嫆缁濓紙閬垮厤澶ф枃浠舵嫋鍨凡婵掍复 OOM 鐨勭郴缁燂級
    if (!isProtectedFile && HandlerUtils::rejectHeavyRequestOnPressure(
            request,
            rawDownload ? "Raw file download" : "File content",
            MemoryGuardLevel::SEVERE,
            8)) {
        file.close();
        return;
    }

    // 鍫嗗唴瀛樻渶灏忛槇鍊硷紙chunked 娴佸紡浠呴渶鏋佸皯鍐呭瓨鍗冲彲鍙戦€侊級锛?
    // - 鍙椾繚鎶ゆ枃浠讹細free 鈮?6KB, maxAlloc 鈮?2KB锛堜粎闇€鐘舵€佺粨鏋?+ TCP 鍙戦€佺紦鍐诧級
    // - 闈炲彈淇濇姢澶ф枃浠讹細free 鈮?20KB, maxAlloc 鈮?8KB
    size_t freeHeap = ESP.getFreeHeap();
    size_t maxAlloc = ESP.getMaxAllocHeap();
    size_t minFreeHeap = isProtectedFile ? 6000  : 20000;
    size_t minMaxAlloc = isProtectedFile ? 2000  : 8000;
    if (freeHeap < minFreeHeap || maxAlloc < minMaxAlloc) {
        // Chunked streaming uses minimal internal heap (~300B state + TCP buffer).
        // If PSRAM is available and heap isn't critically low, safe to proceed.
        if (!FastBee::psramAvailableForJson(4096) || HandlerUtils::internalHeapCriticallyLow(freeHeap, maxAlloc)) {
            file.close();
            char msg[96];
            snprintf(msg, sizeof(msg), "Low memory: heap=%u maxAlloc=%u", (unsigned)freeHeap, (unsigned)maxAlloc);
            HandlerUtils::sendJsonError(request, 503, msg);
            return;
        }
    }

    // ============ Chunked 娴佸紡鍝嶅簲鐘舵€佹満 ============
    // 閬垮厤涓€娆℃€у垎閰嶅ぇ String锛岃 CRITICAL 绾т笅鐨勯厤缃枃浠朵篃鑳藉畬鏁村鍑恒€?
    // 闃舵锛?=header, 1=content锛堣鏂囦欢+杞箟锛? 2=trailer "\"}}", 3=done
    struct FileStreamState {
        File file;
        size_t fileSize = 0;
        size_t rawPos = 0;
        String header;
        size_t headerPos = 0;
        size_t trailerPos = 0;
        // pendingBuf 缂撳瓨锛氬綋 buffer 鍓╀綑绌洪棿涓嶈冻浠ュ啓鍏?2 瀛楄妭杞箟鏃讹紝鏆傚瓨鍒颁笅娆″洖璋?
        uint8_t pendingBuf[130] = {0};  // 64 raw bytes * 2 max escape = 128 worst case
        size_t pendingLen = 0;
        size_t pendingHead = 0;
        uint8_t phase = 0;
    };

    auto state = std::make_shared<FileStreamState>();
    state->file = file;  // 绉讳氦鏂囦欢鍙ユ焺鎵€鏈夋潈
    state->fileSize = fileSize;
    state->header = F("{\"success\":true,\"data\":{\"path\":\"");
    state->header += path;
    state->header += F("\",\"size\":");
    state->header += String((unsigned int)fileSize);
    state->header += F(",\"content\":\"");

    AsyncWebServerResponse* resp = request->beginChunkedResponse(
        "application/json",
        [state](uint8_t* buffer, size_t maxLen, size_t /*index*/) -> size_t {
            size_t written = 0;
            // ---- Phase 0: header ----
            if (state->phase == 0) {
                size_t headerLen = state->header.length();
                while (state->headerPos < headerLen && written < maxLen) {
                    size_t remain = headerLen - state->headerPos;
                    size_t toCopy = (remain < (maxLen - written)) ? remain : (maxLen - written);
                    memcpy(buffer + written, state->header.c_str() + state->headerPos, toCopy);
                    state->headerPos += toCopy;
                    written += toCopy;
                }
                if (state->headerPos >= headerLen) state->phase = 1;
                if (written > 0) return written;
            }
            // ---- Phase 1: 鏂囦欢鍐呭杞箟 ----
            if (state->phase == 1) {
                // 浼樺厛娑堣€椾笂娆℃湭鑳藉啓鍏ョ殑杞箟瀛楄妭
                while (state->pendingLen > 0 && written < maxLen) {
                    buffer[written++] = state->pendingBuf[state->pendingHead++];
                    state->pendingLen--;
                }
                if (state->pendingLen == 0) state->pendingHead = 0;

                uint8_t raw[64];
                while (written < maxLen && state->rawPos < state->fileSize && state->file.available()) {
                    size_t canRead = std::min((size_t)sizeof(raw), state->fileSize - state->rawPos);
                    int n = state->file.read(raw, canRead);
                    if (n <= 0) break;
                    state->rawPos += n;
                    for (int i = 0; i < n; i++) {
                        char c = (char)raw[i];
                        char esc[2]; size_t escLen = 0;
                        switch (c) {
                            case '"':  esc[0]='\\'; esc[1]='"'; escLen=2; break;
                            case '\\': esc[0]='\\'; esc[1]='\\'; escLen=2; break;
                            case '\n': esc[0]='\\'; esc[1]='n'; escLen=2; break;
                            case '\r': esc[0]='\\'; esc[1]='r'; escLen=2; break;
                            case '\t': esc[0]='\\'; esc[1]='t'; escLen=2; break;
                            default:
                                if ((uint8_t)c >= 0x20) { esc[0]=c; escLen=1; }
                                break;
                        }
                        if (escLen == 0) continue;  // 鎺у埗瀛楃鐩存帴涓㈠純
                        if (written + escLen <= maxLen) {
                            for (size_t j = 0; j < escLen; j++) buffer[written++] = esc[j];
                        } else {
                            // buffer 瑁呬笉涓嬪畬鏁磋浆涔夊簭鍒楋紝鏆傚瓨鍒?pendingBuf
                            for (size_t j = 0; j < escLen; j++) {
                                if (state->pendingLen < sizeof(state->pendingBuf)) {
                                    state->pendingBuf[state->pendingLen++] = esc[j];
                                }
                            }
                        }
                    }
                    // buffer 鍐欐弧涓?pending 闈炵┖锛氶€€鍑猴紝涓嬫鍐嶆潵
                    if (written >= maxLen) break;
                }
                if (state->rawPos >= state->fileSize && state->pendingLen == 0) {
                    state->phase = 2;
                }
                if (written > 0) return written;
            }
            // ---- Phase 2: trailer "\"}}" ----
            if (state->phase == 2) {
                static const char kTrailer[] = "\"}}";
                const size_t trailerLen = 3;
                while (state->trailerPos < trailerLen && written < maxLen) {
                    buffer[written++] = (uint8_t)kTrailer[state->trailerPos++];
                }
                if (state->trailerPos >= trailerLen) state->phase = 3;
                if (written > 0) return written;
            }
            // ---- Phase 3: done ----
            if (state->file) state->file.close();
            return 0;
        }
    );
    resp->addHeader("Connection", "close");
    request->send(resp);
}

void SystemRouteHandler::handleSaveFile(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;
    if (!request->hasParam("path", true)) { ctx->sendError(request, 400, "Missing path parameter"); return; }
    String path = request->getParam("path", true)->value();
    if (!path.startsWith("/")) path = "/" + path;
    if (!isSafeFileApiPath(path)) { ctx->sendError(request, 403, "Invalid path"); return; }
    if (isManagedRuntimeConfigPath(path)) {
        ctx->sendError(request, 409, "该运行配置不能通过文件编辑器直接覆盖，请使用对应配置页面");
        return;
    }
    if (isSensitiveRuntimeConfigPath(path)) {
        ctx->sendError(request, 409, "敏感配置不能通过文件编辑器直接覆盖，请使用对应配置页面");
        return;
    }
    if (path.startsWith("/config/")) {
        ctx->sendError(request, 409, "配置文件不能通过文件编辑器直接覆盖，请使用对应配置页面或配置导入接口");
        return;
    }
    if (!(path.endsWith(".json") || path.endsWith(".txt") || path.endsWith(".log") ||
          path.endsWith(".html") || path.endsWith(".js") || path.endsWith(".css"))) {
        ctx->sendError(request, 403, "File type not allowed for editing"); return;
    }
    String content = "";
    if (request->hasParam("content", true)) content = request->getParam("content", true)->value();
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash > 0) {
        String parentDir = path.substring(0, lastSlash);
        if (!LittleFS.exists(parentDir)) { ctx->sendError(request, 500, "Parent directory does not exist"); return; }
    }
    File file = LittleFS.open(path, "w");
    if (!file) { ctx->sendError(request, 500, "Failed to open file for writing"); return; }
    size_t written = file.print(content);
    file.close();
    if (written == content.length()) {
        LOGGER.infof("File saved via web: %s", path.c_str());

        // 瀵煎叆/淇濆瓨閰嶇疆鍚庡洖濉韩浠藉瓧娈碉細褰撳啓鍏?device.json 鎴?protocol.json 鏃讹紝
        // 澶嶇敤鍚姩闃舵鐨?ensureDeviceIdentity() 鑷姩濉厖绌虹殑 deviceId / mqtt.clientId锛?
        // 纭繚璺ㄨ澶囧鍏ヤ篃鑳芥纭敓鎴愪竴鑷寸殑璁惧鏍囪瘑銆?
        if (path == "/config/device.json" || path == "/config/protocol.json") {
            FastBeeFramework* fw = FastBeeFramework::getInstance();
            if (fw) {
                fw->ensureDeviceIdentity();
            }
        }

        ctx->sendSuccess(request, "File saved successfully");
    } else { ctx->sendError(request, 500, "Failed to write complete file"); }
}

void SystemRouteHandler::handleGetFilesystem(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (FastBee::MemoryBudget::dramFree() < 2048 || FastBee::MemoryBudget::dramLargest() < 1024) {
        HandlerUtils::sendJsonError(request, 503, "Critical memory - filesystem info unavailable", 5);
        return;
    }
    auto doc = FastBee::makeJsonDocument(2048);
    size_t total, used;
    getCachedFsStats(total, used);
    doc["totalBytes"] = total;
    doc["usedBytes"]  = used;
    doc["freeBytes"]  = total > used ? total - used : 0;
    doc["usedPercent"]= total > 0 ? (used * 100 / total) : 0;
    ctx->sendSuccess(request, doc);
}

#endif // FASTBEE_ENABLE_FILE_MANAGER

void SystemRouteHandler::handleFactoryReset(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    const String confirmation = ctx->getParamValue(request, "confirm", "");
    if (confirmation != "RESET") {
        ctx->sendError(request, 400, "Factory reset requires explicit RESET confirmation");
        return;
    }
    const String password = ctx->getParamValue(request, "password", "");
    if (!ctx->verifyCurrentUserPassword(request, password)) {
        ctx->sendError(request, 403, "Current password verification failed");
        return;
    }
    LOGGER.warning("Factory reset authorized by current-user password verification");

#if FASTBEE_ENABLE_POINT_HISTORY
    if (!FastBee::PointHistory::instance().pauseWrites(2000)) {
        ctx->sendError(request, 503, "Recent history storage is busy; retry factory reset");
        return;
    }
#endif

    int resetCount = 0;
    int resetFailureCount = 0;
    String failedFiles;
    auto recordFailure = [&](const char* path) {
        ++resetFailureCount;
        if (!failedFiles.isEmpty()) failedFiles += ',';
        failedFiles += path;
        LOGGER.errorf("Factory reset: failed to reset %s", path);
    };
    ConfigStorage& configStorage = ConfigStorage::getInstance();
    auto writeDefault = [&](const char* path, const char* content) -> bool {
        if (configStorage.saveFactoryJSONConfig(path, content)) {
            ++resetCount;
            return true;
        }
        recordFailure(path);
        return false;
    };

    // ── 核心配置文件：写入默认 JSON ──────────────────────────────────────
    // device.json - 设备基本信息（清除个性化配置，保留合理默认值）
    static const char DEFAULT_DEVICE[] PROGMEM =
        "{\"deviceId\":\"\",\"productNumber\":0,\"userId\":\"1\","
        "\"deviceName\":\"FastBee\",\"location\":\"\",\"description\":\"FastBee-Arduino\","
        "\"enableNTP\":true,\"ntpServer1\":\"http://iot.fastbee.cn/prod-api/iot/tool/ntp\",\"ntpServer2\":\"cn.pool.ntp.org\","
        "\"timezone\":\"CST-8\",\"syncInterval\":3600,\"logLevel\":\"INFO\","
        "\"developerModeEnabled\":false}";
    writeDefault("/config/device.json", DEFAULT_DEVICE);

    // network.json - 网络配置（清除WiFi凭据，恢复AP模式以便用户重新配网）
    static const char DEFAULT_NETWORK[] PROGMEM =
        "{\"mode\":0,\"networkType\":0,"
        "\"apSSID\":\"fastbee-ap\",\"apPassword\":\"admin123\",\"apIP\":\"192.168.4.1\","
        "\"apChannel\":1,\"apHidden\":false,\"apMaxConnections\":4,"
        "\"staSSID\":\"\",\"staPassword\":\"\",\"networks\":[],"
        "\"ipConfigType\":0,\"staticIP\":\"\",\"gateway\":\"\",\"subnet\":\"\","
        "\"dns1\":\"8.8.8.8\",\"dns2\":\"8.8.4.4\",\"connectTimeout\":20000,"
        "\"reconnectInterval\":5000,\"maxReconnectAttempts\":5,"
        "\"customDomain\":\"fastbee\",\"enableMDNS\":true,"
        "\"conflictDetection\":2,\"failoverStrategy\":2,\"autoFailover\":true,"
        "\"conflictCheckInterval\":30000,\"maxFailoverAttempts\":3,"
        "\"conflictThreshold\":2,\"fallbackToDHCP\":true,"
        "\"ethernet\":{\"spiMosi\":11,\"spiMiso\":13,\"spiSck\":12,\"csPin\":47,\"rstPin\":48,\"intPin\":14},"
        "\"cellular\":{\"txPin\":39,\"rxPin\":40,\"pwrPin\":38,\"baudRate\":115200,\"apn\":\"CMNET\"}}";
    writeDefault("/config/network.json", DEFAULT_NETWORK);

    // users.json - 恢复默认用户（仅 admin）
    static const char DEFAULT_USERS[] PROGMEM =
        "{\"users\":["
        "{\"username\":\"admin\",\"passwordHash\":\"MMqroQgcopTMz5bU/x3brwufT38Ks6YAWGPRkmE5jDw=\","
        "\"salt\":\"RmFzdEJlZURlZmF1bHQhIQ==\",\"enabled\":true,"
        "\"createTime\":0,\"lastLogin\":0,\"lastModified\":0,"
        "\"description\":\"\"}],"
        "\"security\":{\"maxLoginAttempts\":5,\"loginLockoutTime\":300000,"
        "\"minPasswordLength\":6,\"requireStrongPasswords\":false}}";
    writeDefault("/config/users.json", DEFAULT_USERS);

    // protocol.json - MQTT 默认启用；未配置设备身份时连接层保持等待配置
    static const char DEFAULT_PROTOCOL[] PROGMEM =
        "{\"modbusRtu\":{\"enabled\":false,\"peripheralId\":\"\",\"mode\":\"master\",\"dePin\":14,\"transferType\":0,\"master\":{\"tasks\":[],\"devices\":[]}},"
        "\"mqtt\":{\"enabled\":true,\"scheme\":\"mqtt\",\"server\":\"iot.fastbee.cn\",\"otaBaseUrl\":\"\",\"port\":1883,\"clientId\":\"\",\"username\":\"\",\"password\":\"\","
        "\"keepAlive\":60,\"autoReconnect\":true,\"authType\":0,\"mqttSecret\":\"\",\"authCode\":\"\","
        "\"willTopic\":\"\",\"willPayload\":\"\",\"willQos\":0,\"willRetain\":false,"
        "\"longitude\":0,\"latitude\":0,\"iccid\":\"\",\"cardPlatformId\":0,"
        "\"summary\":\"{\\\"name\\\":\\\"fastbee\\\",\\\"chip\\\":\\\"ESP32\\\"}\","
        "\"publishTopics\":["
        "{\"topic\":\"/property/post\",\"qos\":0,\"retain\":false,\"enabled\":true,\"autoPrefix\":true,\"topicType\":0},"
        "{\"topic\":\"/info/post\",\"qos\":0,\"retain\":false,\"enabled\":true,\"autoPrefix\":true,\"topicType\":2},"
        "{\"topic\":\"/event/post\",\"qos\":0,\"retain\":false,\"enabled\":true,\"autoPrefix\":true,\"topicType\":4},"
        "{\"topic\":\"/monitor/post\",\"qos\":0,\"retain\":false,\"enabled\":true,\"autoPrefix\":true,\"topicType\":3},"
        "{\"topic\":\"/ntp/post\",\"qos\":0,\"retain\":false,\"enabled\":true,\"autoPrefix\":true,\"topicType\":7},"
        "{\"topic\":\"/{deviceNum}/http/upgrade/reply\",\"qos\":0,\"retain\":false,\"enabled\":true,\"autoPrefix\":false,\"topicType\":5}],"
        "\"subscribeTopics\":["
        "{\"topic\":\"/function/get\",\"qos\":0,\"enabled\":true,\"autoPrefix\":true,\"topicType\":1},"
        "{\"topic\":\"/info/get\",\"qos\":0,\"enabled\":true,\"autoPrefix\":true,\"topicType\":2},"
        "{\"topic\":\"/monitor/get\",\"qos\":0,\"enabled\":true,\"autoPrefix\":true,\"topicType\":3},"
        "{\"topic\":\"/ntp/get\",\"qos\":0,\"enabled\":true,\"autoPrefix\":true,\"topicType\":7},"
        "{\"topic\":\"/{deviceNum}/http/upgrade/set\",\"qos\":0,\"enabled\":true,\"autoPrefix\":false,\"topicType\":5}],"
        "\"http\":{\"enabled\":false,\"url\":\"\",\"port\":80,\"method\":\"POST\",\"timeout\":30,\"interval\":60,"
        "\"retry\":3,\"authType\":\"none\",\"authUser\":\"\",\"authToken\":\"\",\"contentType\":\"application/json\"},"
        "\"coap\":{\"enabled\":false,\"server\":\"\",\"port\":5683,\"method\":\"POST\","
        "\"path\":\"sensors/temperature\",\"msgType\":\"CON\",\"retransmit\":3,\"timeout\":5000}}}";
    writeDefault("/config/protocol.json", DEFAULT_PROTOCOL);

    // peripherals.json - 清空外设配置（用户按需添加）
    static const char DEFAULT_PERIPHERALS[] PROGMEM = "{\"peripherals\":[]}";
    writeDefault("/config/peripherals.json", DEFAULT_PERIPHERALS);

    // periph_exec.json - 清空执行规则
    static const char DEFAULT_PERIPH_EXEC[] PROGMEM = "{\"rules\":[]}";
    writeDefault("/config/periph_exec.json", DEFAULT_PERIPH_EXEC);

    // features.json - 恢复完整功能快照：Web 与 mDNS 默认启用，其余按需启用。
    // 通过管理器生成，避免这里维护第二份容易漂移的功能注册表。
    if (FastBee::RuntimeFeatureManager::instance().resetToDefaults()) {
        resetCount++;
    } else {
        recordFailure("/config/features.json");
    }

    // rule_scripts.json - 恢复默认规则脚本
    static const char DEFAULT_RULE_SCRIPTS[] PROGMEM =
        "{\"rules\":["
        "{\"id\":\"rs_mqtt_report\",\"name\":\"MQTT\xe4\xb8\x8a\xe6\x8a\xa5:\xe6\x95\xb0\xe7\xbb\x84\xe8\xbd\xac\xe5\xaf\xb9\xe8\xb1\xa1\","
        "\"enabled\":false,\"triggerType\":1,\"protocolType\":0,\"sourceTopic\":\"\",\"targetTopic\":\"\","
        "\"scriptContent\":\"{\\\"temperature\\\": ${temperature}, \\\"humidity\\\": ${humidity}}\"},"
        "{\"id\":\"rs_mqtt_recv\",\"name\":\"MQTT\xe6\x8e\xa5\xe6\x94\xb6:\xe5\xaf\xb9\xe8\xb1\xa1\xe8\xbd\xac\xe6\x95\xb0\xe7\xbb\x84\","
        "\"enabled\":false,\"triggerType\":0,\"protocolType\":0,\"sourceTopic\":\"\",\"targetTopic\":\"\","
        "\"scriptContent\":\"[{\\\"id\\\":\\\"temperature\\\",\\\"value\\\":\\\"${temperature}\\\",\\\"remark\\\":\\\"\\\"},{\\\"id\\\":\\\"humidity\\\",\\\"value\\\":\\\"${humidity}\\\",\\\"remark\\\":\\\"\\\"}]\"}"
        "]}";
    writeDefault("/config/rule_scripts.json", DEFAULT_RULE_SCRIPTS);

    // ── 运行时可选配置文件：直接删除（各模块已处理文件不存在的情况）────
    const char* optionalFiles[] = {
        "/config/auth.json", "/config/system.json", "/config/ai.json",
        "/config/http.json", "/config/mqtt.json", "/config/tcp.json",
        "/config/modbus.json", "/config/coap.json", "/config/mqtt-backup.json",
        "/config/provision.json", "/config/ble_provision.json",
        "/config/ir_keys.json", "/config/rf_keys.json",
        "/config/screen_panel.json", "/config/point_history.json",
        "/config/screen_panel.json.touch-v1.bak",
        "/config/protocol.pre-screen-weather.json", "/config/periph_exec.pre-screen-remote.json",
        "/data/point_history.json"
    };
    bool historyDeleted = true;
    for (const char* optFile : optionalFiles) {
        if (!configStorage.deleteJSONConfigGenerations(optFile)) {
            recordFailure(optFile);
            if (strstr(optFile, "point_history.json")) historyDeleted = false;
        }
    }
#if FASTBEE_ENABLE_POINT_HISTORY
    if (!FastBee::PointHistory::instance().resumeWrites(historyDeleted)) {
        recordFailure("point_history runtime state");
    }
#endif

    if (!ctx->networkManager || !ctx->networkManager->clearPersistentFallback()) {
        recordFailure("NVS:net_cfg");
    }

#if FASTBEE_ENABLE_FILE_LOGGING || FASTBEE_ENABLE_LOG_VIEWER
    if (LittleFS.exists("/logs/system.log")) {
        File logFile = LittleFS.open("/logs/system.log", "w");
        if (logFile) { logFile.close(); LOGGER.info("Cleared system log file"); }
    }
#endif

#if FASTBEE_ENABLE_STORAGE_CACHE
    // 清除配置缓存，确保重启前不会把旧缓存写回磁盘
    ConfigStorage::getInstance().clearCache();
#endif

    LOGGER.infof("Factory reset: %d config files restored to defaults", resetCount);

    if (resetFailureCount > 0) {
        JsonDocument errorDoc;
        errorDoc["success"] = false;
        errorDoc["error"] = "Factory reset incomplete; retry before rebooting";
        errorDoc["restoredFiles"] = resetCount;
        errorDoc["failedCount"] = resetFailureCount;
        errorDoc["failedFiles"] = failedFiles;
        errorDoc["timestamp"] = millis();
        String errorJson;
        serializeJson(errorDoc, errorJson);
        request->send(500, "application/json", errorJson);
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "Factory reset completed. Device will restart.";
    doc["restoredFiles"] = resetCount;
    doc["timestamp"] = millis();
    String jsonStr; serializeJson(doc, jsonStr);
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", jsonStr);
    response->addHeader("Connection", "close");
    request->onDisconnect([this]() {
        LOGGER.info("Factory reset response sent, scheduling restart via SystemRebooter");
        SystemRebooter::scheduleReboot("Factory reset completed", 2000,
                                       RestartReason::FACTORY_RESET);
    });
    request->send(response);
}

void SystemRouteHandler::handleGetWebConfig(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    JsonDocument doc;
    Preferences prefs;
    prefs.begin("webconfig", true);
    doc["webPort"] = prefs.getUInt("webPort", 80);
    doc["sessionTimeout"] = prefs.getUInt("sessionTimeout", 3600000);
    doc["maxLoginAttempts"] = prefs.getUInt("maxLoginAttempts", 5);
    doc["lockoutTime"] = prefs.getUInt("lockoutTime", 300000);
    prefs.end();
    ctx->sendSuccess(request, doc);
}

void SystemRouteHandler::handleSaveWebConfig(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;
    Preferences prefs;
    prefs.begin("webconfig", false);
    uint32_t webPort = ctx->getParamInt(request, "webPort", 0);
    uint32_t sessionTimeout = ctx->getParamInt(request, "sessionTimeout", 0);
    uint32_t maxLoginAttempts = ctx->getParamInt(request, "maxLoginAttempts", 0);
    uint32_t lockoutTime = ctx->getParamInt(request, "lockoutTime", 0);
    bool updated = false;
    if (webPort >= 1 && webPort <= 65535) { prefs.putUInt("webPort", webPort); updated = true; }
    if (sessionTimeout >= 60000 && sessionTimeout <= 86400000) { prefs.putUInt("sessionTimeout", sessionTimeout); updated = true; }
    if (maxLoginAttempts >= 1 && maxLoginAttempts <= 10) { prefs.putUInt("maxLoginAttempts", maxLoginAttempts); updated = true; }
    if (lockoutTime >= 60000 && lockoutTime <= 3600000) { prefs.putUInt("lockoutTime", lockoutTime); updated = true; }
    prefs.end();
    if (updated) ctx->sendSuccess(request, "Configuration updated successfully");
    else ctx->sendError(request, 400, "No valid configuration parameters provided");
}
