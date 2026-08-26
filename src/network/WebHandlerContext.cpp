#include "./network/WebHandlerContext.h"
#include "./security/AuthManager.h"
#include "core/MemoryBudget.h"
#include "./network/handlers/HandlerUtils.h"
#include "systems/LoggerSystem.h"
#include "systems/HealthMonitor.h"
#include "systems/TaskManager.h"
#include "core/FastBeeFramework.h"
#include "protocols/ProtocolManager.h"
#include "protocols/MQTTClient.h"
#include "utils/PsramJsonDocument.h"
#include "./network/WebConfigManager.h"
#include <esp_heap_caps.h>

// ROM 日志函数：不走 newlib / 不加锁 / 不分配内存。OOM 拒绝路径上
// Serial.printf 会堆分配并解引用堆字符串（vsnprintf -> strlen %s 参数），
// 实测在 free8=3544 时触发 LoadProhibited panic（serveStaticFile 拒绝分支）。
// 临界路径一律 ets_printf + 纯数字，不传 %s 堆指针。
extern "C" int ets_printf(const char* fmt, ...);

WebHandlerContext::WebHandlerContext(AsyncWebServer* srv, IAuthManager* authMgr, IUserManager* userMgr)
    : server(srv), authManager(authMgr), userManager(userMgr),
      networkManager(nullptr), otaManager(nullptr),
      protocolManager(nullptr), sseHandler(nullptr), bleProvisioner(nullptr), webRootPath("/www"),
      scheduleRestart(false), scheduledRestartTime(0), cacheDuration(86400),
      webForegroundModeActive(false), webForegroundUntilMs(0),
      devModeCacheValid(false), devModeCacheValue(false), devModeCacheExpiryMs(0),
      webConfigManager(nullptr) {
    loadCacheDuration();
}

// ============ 参数处理辅助方法 ============

String WebHandlerContext::getParamValue(AsyncWebServerRequest* request, const String& paramName,
                                        const String& defaultValue) {
    if (!request) return defaultValue;

    // 优先从 POST body（表单参数）取，再从 URL query string 取
    if (request->hasParam(paramName, true)) {
        const AsyncWebParameter* param = request->getParam(paramName, true);
        return param->value();
    }
    if (request->hasParam(paramName, false)) {
        const AsyncWebParameter* param = request->getParam(paramName, false);
        return param->value();
    }
    return defaultValue;
}

bool WebHandlerContext::getParamBool(AsyncWebServerRequest* request, const String& paramName,
                                     bool defaultValue) {
    String value = getParamValue(request, paramName, defaultValue ? "true" : "false");
    return value.equalsIgnoreCase("true") || value == "1";
}

int WebHandlerContext::getParamInt(AsyncWebServerRequest* request, const String& paramName,
                                   int defaultValue) {
    String value = getParamValue(request, paramName, String(defaultValue));
    return value.toInt();
}

// ============ 认证辅助方法 ============

bool WebHandlerContext::requiresAuth(AsyncWebServerRequest* request) {
    static const char* publicPaths[] = {
        "/api/auth/login",
        "/api/health",
        "/api/events",  // SSE 推送通道，EventSource 不支持 Cookie，需跳过 session 认证
        "/login",
        "/css/",
        "/js/",
        "/images/",
        "/assets/"
    };

    const char* path = request->url().c_str();

    for (const char* publicPath : publicPaths) {
        if (strncmp(path, publicPath, strlen(publicPath)) == 0) {
            return false;
        }
    }

    return true;
}

AuthResult WebHandlerContext::authenticateRequest(AsyncWebServerRequest* request) {
    if (!authManager) {
        return AuthResult();
    }

    String sessionId = AuthManager::extractSessionIdFromRequest(request);
    if (sessionId.isEmpty()) {
        return AuthResult();
    }

    return authManager->verifySession(sessionId, true);
}

String WebHandlerContext::getClientIP(AsyncWebServerRequest* request) {
    if (!request) return "";

    if (request->hasHeader("X-Forwarded-For")) {
        return request->header("X-Forwarded-For");
    }

    if (request->hasHeader("X-Real-IP")) {
        return request->header("X-Real-IP");
    }

    return request->client()->remoteIP().toString();
}

String WebHandlerContext::getUserAgent(AsyncWebServerRequest* request) {
    if (!request) return "";

    if (request->hasHeader("User-Agent")) {
        return request->header("User-Agent");
    }

    return "";
}

bool WebHandlerContext::requireAuth(AsyncWebServerRequest* request) {
    if (!authManager || !request) {
        sendUnauthorized(request);
        return false;
    }

    AuthResult authResult = authenticateRequest(request);
    if (!authResult.success) {
        sendUnauthorized(request);
        return false;
    }

    // 记录请求活动：大量处理器经由 HandlerUtils 的 chunked/stream 路径发送响应，
    // 那些路径不会更新 lastRequestSeenMs。统一在此记录，避免无请求看门狗误判
    // “120s 无人访问”而 abort 端口 80 上的存活连接（会切断浏览器进行中的请求链）。
    noteWebRequestActivity();

    return true;
}

bool WebHandlerContext::isDeveloperModeEnabled() {
    // SEVERE+ 内存压力时禁止 DevMode API（诊断序列化会加剧内存压力）
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    if (monitor && monitor->isMemorySevere()) {
        return false;
    }
    // 缓存优化：避免每次请求都读取+解析 JSON 文件
    unsigned long now = millis();
    if (devModeCacheValid && (long)(devModeCacheExpiryMs - now) > 0) {
        return devModeCacheValue;
    }

    File f = LittleFS.open("/config/device.json", "r");
    if (!f) {
        devModeCacheValue = false;  // 配置文件缺失时默认禁用开发者模式（安全优先）
        devModeCacheValid = true;
        devModeCacheExpiryMs = now + 30000UL;  // 缓存 30s
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        devModeCacheValue = false;  // JSON 解析失败时默认禁用开发者模式
        devModeCacheValid = true;
        devModeCacheExpiryMs = now + 10000UL;  // 解析失败缓存 10s
        return false;
    }
    if (!doc["developerModeEnabled"].is<bool>()) {
        devModeCacheValue = false;  // 字段缺失时默认禁用开发者模式
        devModeCacheValid = true;
        devModeCacheExpiryMs = now + 30000UL;
        return false;
    }
    devModeCacheValue = doc["developerModeEnabled"].as<bool>();
    devModeCacheValid = true;
    devModeCacheExpiryMs = now + 60000UL;  // 成功读取缓存 60s
    return devModeCacheValue;
}

bool WebHandlerContext::verifyCurrentUserPassword(AsyncWebServerRequest* request, const String& password) {
    if (!request || !userManager || password.isEmpty()) {
        return false;
    }

    AuthResult authResult = authenticateRequest(request);
    if (!authResult.success || authResult.username.isEmpty()) {
        return false;
    }

    return userManager->validateUser(authResult.username, password);
}

bool WebHandlerContext::requireDeveloperMode(AsyncWebServerRequest* request) {
    if (isDeveloperModeEnabled()) {
        return true;
    }
    sendError(request, 403, "Developer mode is disabled");
    return false;
}

// ============ 内存保护辅助 ============

static bool canAllocateResponse(size_t requiredSize) {
    auto* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    if (!monitor) return true;
    SystemHealth health = monitor->getHealthStatus();
    // 预留 512 字节安全余量，防止序列化过程中的临时分配
    size_t safeSize = requiredSize + 512;
    if (health.largestFreeBlock < safeSize) {
        if (FastBee::psramAvailableForJson(safeSize + 4096) &&
            FastBee::MemoryBudget::dramFree() >= 2048 && FastBee::MemoryBudget::dramLargest() >= 1024) {
            return true;
        }
        ets_printf("[MemGuard] Rejecting response: need ~%u bytes, largest block=%lu\n",
                   (unsigned)safeSize, (unsigned long)health.largestFreeBlock);
        return false;
    }
    return true;
}

// 2026-08-24 OOM 崩溃环取证：拒绝路径自身在临界堆上构建响应也会崩。
// backtrace 实锤：sendServiceUnavailable -> addHeader -> throwing operator new(40)
// 在 free8 耗尽时 malloc 失败直接 abort（-fno-exceptions，无 try/catch 可救）
// -> panic 重启 -> 重启后基线仍差 -> 再次触发，形成死环。
// 临界水位下放弃构建任何响应，也不 send：请求挂起后由客户端超时/RST 清理。
// 注意：不能在此处调 request->client()->close()——本函数运行在 async_tcp
// 任务的 _recv 回调链内，同步 close 会销毁 AsyncClient 而请求链仍在引用它，
// 实测直接触发 StoreProhibited panic（三次复现，PC 一致）。
static bool rejectPathSafe(AsyncWebServerRequest* request) {
    const uint32_t free8 = FastBee::MemoryBudget::dram8Free();
    const uint32_t blk8 = FastBee::MemoryBudget::dram8Largest();
    // 与静态文件门控（free8<6144 拒绝）对齐：拒绝后的链路（std::function
    // 拷贝、响应构建）仍需小分配，实测 blk8 碎到 28B 时 next() 拷贝 32B 即 abort
    if (free8 >= 6144 && blk8 >= 1536) {
        return true;  // 水位足够承担 beginResponse + addHeader 的小分配
    }
    ets_printf("[MemGuard] reject path unsafe (free8=%u blk8=%u), dropping request without response\n",
               (unsigned)free8, (unsigned)blk8);
    return false;
}

static void sendServiceUnavailable(AsyncWebServerRequest* request) {
    if (!rejectPathSafe(request)) return;
    AsyncWebServerResponse* response = request->beginResponse(
        503,
        "application/json",
        "{\"success\":false,\"message\":\"Service unavailable - low memory\",\"error\":\"Service unavailable - low memory\",\"code\":503,\"retryAfter\":5}");
    response->addHeader("Retry-After", "5");
    response->addHeader("Connection", "close");  // 立即释放 TCP pbuf
    request->send(response);
}

// ============ static file concurrency limiter ============
// 2026-08 field test: 4 concurrent streams of a 128KB CSS exhausted DRAM
// (largest block dropped to 220B) and operator new(40) abort() put the
// device into a crash loop (login page never finishes loading).
// Cap in-flight static file responses; excess gets fast 503 + Retry-After
// (front-end loader already retries), serialising large transfers.
//
// 2026-08-04: raised 1 -> 2. Login page diagnosis showed the strict cap of 1
// plus Connection: close turned the initial ~12-resource burst into a long
// serial chain, and a 503 on the document request itself left the browser on
// a blank JSON page. Two concurrent gz streams (each <=22KB gz) stay within
// the DRAM gates above; small uncompressed assets bypass the cap entirely.
static volatile uint8_t s_staticInFlight = 0;
static unsigned long s_staticInFlightTouchMs = 0;
static constexpr uint8_t STATIC_MAX_INFLIGHT = 3;
// 2026-08-10: 2→3。登录页首屏 ~12 个压缩资源，旧 cap=2 导致大部分请求被
// 503 拒绝后重试，串行化延迟严重（实测页面加载 >5s）。3 路并发每路
// send_buffer ≤2872B（含降级分配）+ lwIP PCB ≈ 4.5KB/路 × 3 ≈ 13.5KB，
// 稳态 free8=24KB 下仍留 10KB+ 余量给 AsyncTCP/其他任务。
// Shortened from 20s: a stuck counter blocked every static request (incl. the
// login page document) until the watchdog fired; 12s bounds the worst case
// while still covering any legitimate single-file transfer.
static constexpr unsigned long STATIC_INFLIGHT_WATCHDOG_MS = 12000UL;
// 2026-08-10 慢速修复复盘：槽位满时的短等待（delay 轮询 300ms）运行在
// AsyncTCP 服务任务上下文，会阻塞在途响应的发送与 onDisconnect 事件
// 处理——等待期间槽位永远等不到释放，实际退化为“先卡死全部连接
// 300ms 再拒绝”，比立即拒绝更糟（实测 conc>=2 出现 13s 慢请求）。
// 改回立即判定：单文件 gz ≤ 22KB 传输 <100ms，前端已有 300/800ms 快速
// 退避重试（index.html boot loader + module-loader），503 风暴不会放大。
static bool staticInFlightAllowed() {
    unsigned long now = millis();
    // watchdog: force-reset a stuck counter to avoid permanent 503
    if (s_staticInFlight > 0 &&
        (now - s_staticInFlightTouchMs) > STATIC_INFLIGHT_WATCHDOG_MS) {
        ets_printf("[MemGuard] Static in-flight counter stuck (%u), resetting\n",
                   (unsigned)s_staticInFlight);
        s_staticInFlight = 0;
    }
    if (s_staticInFlight < STATIC_MAX_INFLIGHT) {
        s_staticInFlight++;
        s_staticInFlightTouchMs = now;
        return true;
    }
    return false;
}

static void registerStaticInFlightRelease(AsyncWebServerRequest* request) {
    // Connection: close guarantees _onDisconnect on finish/timeout/abort
    request->onDisconnect([]() {
        if (s_staticInFlight > 0) s_staticInFlight--;
    });
}

bool WebHandlerContext::isStaticTransferActive() const {
    return s_staticInFlight > 0;
}

// 浏览器导航请求（登录页文档）被限流/内存门控拒绝时，返回带自动刷新的轻量
// HTML 页而不是 JSON：避免用户看到 503 JSON 白屏，页面每秒自动重试直至恢复。
static void sendStaticBusyRetryPage(AsyncWebServerRequest* request) {
    if (!rejectPathSafe(request)) return;
    AsyncWebServerResponse* response = request->beginResponse(
        503,
        "text/html",
        "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta http-equiv=\"refresh\" content=\"1\">"
        "<title>FastBee</title></head>"
        "<body style=\"margin:0;min-height:100vh;display:flex;align-items:center;"
        "justify-content:center;font-family:system-ui,sans-serif;background:#f5f5f5\">"
        "<div style=\"text-align:center\">"
        "<h2 style=\"color:#1677ff\">\xe8\xae\xbe\xe5\xa4\x87\xe7\xb9\x81\xe5\xbf\x99\xef\xbc\x8c\xe6\xad\xa3\xe5\x9c\xa8\xe8\x87\xaa\xe5\x8a\xa8\xe9\x87\x8d\xe8\xaf\x95\xe2\x80\xa6</h2>"
        "<p>\xe5\xa6\x82\xe9\x95\xbf\xe6\x97\xb6\xe9\x97\xb4\xe6\x9c\xaa\xe8\xb7\xb3\xe8\xbd\xac\xef\xbc\x8c\xe8\xaf\xb7<a href=\"/\">\xe7\x82\xb9\xe5\x87\xbb\xe9\x87\x8d\xe8\xaf\x95</a></p>"
        "</div></body></html>");
    response->addHeader("Retry-After", "1");
    response->addHeader("Connection", "close");
    request->send(response);
}

// ============ 响应辅助方法 ============

void WebHandlerContext::sendJsonResponse(AsyncWebServerRequest* request, int code,
                                         const JsonDocument& doc) {
    if (!request) return;
    noteWebRequestActivity();

    size_t docSize = measureJson(doc);

    // MemGuard: CRITICAL 时仅拒绝大响应（>1KB），小响应放行以确保基础 API（如 auth/session）可用
    auto* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    if (monitor && monitor->isMemoryCritical() && docSize > 1024 &&
        !(FastBee::psramAvailableForJson(docSize + 4096) &&
          FastBee::MemoryBudget::dramFree() >= 2048 && FastBee::MemoryBudget::dramLargest() >= 1024)) {
        sendServiceUnavailable(request);
        return;
    }
    // MemGuard: 检查最大可分配块是否足够
    if (!canAllocateResponse(docSize)) {
        sendServiceUnavailable(request);
        return;
    }
    AsyncWebServerResponse* response;

    // 栈缓冲阈值 2048B：覆盖大多数小 JSON 响应（登录/状态/设备控制 < 2KB），
    // AsyncTCP 任务栈最小 12KB(C3/C6)，2048B 留 10KB 给调用链；
    // 超过阈值的响应走堆分配（String::reserve），避免碎片化。
    if (docSize < 2048) {
        char jsonBuffer[2048];
        serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
        response = request->beginResponse(code, "application/json", jsonBuffer);
    } else {
        String jsonStr;
        jsonStr.reserve(docSize + 1);
        serializeJson(doc, jsonStr);
        response = request->beginResponse(code, "application/json", jsonStr);
    }

    // CORS 头已由 DefaultHeaders 全局注入，无需重复设置
    // MAX_CONNECTIONS=2 + Connection:close 模式：响应后立即释放 TCP pbuf（避免 28KB heap 被 keep-alive 几个连接吃光）
    response->addHeader("Connection", "close");
    request->send(response);
}

void WebHandlerContext::sendSuccess(AsyncWebServerRequest* request, const JsonDocument& data) {
    noteWebRequestActivity();
    // MemGuard: CRITICAL 时仅拒绝大响应（>1KB），小响应放行
    auto* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    size_t estimatedSize = measureJson(data);
    if (monitor && monitor->isMemoryCritical() && estimatedSize > 1024 &&
        !(FastBee::psramAvailableForJson(estimatedSize + 4096) &&
          FastBee::MemoryBudget::dramFree() >= 2048 && FastBee::MemoryBudget::dramLargest() >= 1024)) {
        sendServiceUnavailable(request);
        return;
    }
    // MemGuard: 检查最大可分配块是否足够（含 wrapper 开销 ~48 字节 + 安全余量）
    if (!canAllocateResponse(estimatedSize + 64)) {
        sendServiceUnavailable(request);
        return;
    }

    // 先序列化 data 为字符串（避免深拷贝到 outDoc 造成 2x 内存峰值）
    String dataStr;
    dataStr.reserve(estimatedSize + 1);
    if (!data.isNull()) {
        serializeJson(data, dataStr);
    } else {
        dataStr = "null";
    }
    // 文本拼接构建响应，不产生第二个 JsonDocument
    String response;
    response.reserve(dataStr.length() + 64);
    response = "{\"success\":true,\"timestamp\":";
    response += String(millis());
    response += ",\"data\":";
    response += dataStr;
    response += "}";

    AsyncWebServerResponse* resp = request->beginResponse(200, "application/json", response);
    resp->addHeader("Connection", "close");
    request->send(resp);
}

// JSON 安全转义：就地处理到栈缓冲，避免 String 堆分配
static void jsonEscapeToBuffer(const char* src, char* dst, size_t dstSize) {
    size_t oi = 0;
    for (size_t si = 0; src[si] && oi < dstSize - 2; si++) {
        char c = src[si];
        if (c == '"' || c == '\\') { dst[oi++] = '\\'; dst[oi++] = c; }
        else if (c == '\n') { dst[oi++] = '\\'; dst[oi++] = 'n'; }
        else if (c == '\r') { dst[oi++] = '\\'; dst[oi++] = 'r'; }
        else if (c == '\t') { dst[oi++] = '\\'; dst[oi++] = 't'; }
        else if ((unsigned char)c < 0x20) { continue; }  // 跳过其他控制字符
        else { dst[oi++] = c; }
    }
    dst[oi] = '\0';
}

void WebHandlerContext::sendSuccess(AsyncWebServerRequest* request, const String& message) {
    noteWebRequestActivity();
    char jsonBuffer[512];
    if (message.isEmpty()) {
        snprintf(jsonBuffer, sizeof(jsonBuffer), "{\"success\":true,\"timestamp\":%lu}", millis());
    } else {
        char escapedMsg[384];
        jsonEscapeToBuffer(message.c_str(), escapedMsg, sizeof(escapedMsg));
        snprintf(jsonBuffer, sizeof(jsonBuffer),
                 "{\"success\":true,\"message\":\"%s\",\"timestamp\":%lu}",
                 escapedMsg, millis());
    }

    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", jsonBuffer);
    response->addHeader("Connection", "close");
    request->send(response);
}

void WebHandlerContext::sendError(AsyncWebServerRequest* request, int code, const String& message) {
    noteWebRequestActivity();
    char jsonBuffer[512];
    char escapedMsg[384];
    jsonEscapeToBuffer(message.c_str(), escapedMsg, sizeof(escapedMsg));
    snprintf(jsonBuffer, sizeof(jsonBuffer),
             "{\"success\":false,\"error\":\"%s\",\"code\":%d,\"timestamp\":%lu}",
             escapedMsg, code, millis());

    AsyncWebServerResponse* response = request->beginResponse(code, "application/json", jsonBuffer);
    response->addHeader("Connection", "close");
    request->send(response);
}

void WebHandlerContext::sendUnauthorized(AsyncWebServerRequest* request) {
    sendError(request, 401, "Unauthorized");
}

void WebHandlerContext::sendForbidden(AsyncWebServerRequest* request) {
    sendError(request, 403, "Forbidden");
}

void WebHandlerContext::sendNotFound(AsyncWebServerRequest* request) {
    sendError(request, 404, "Not Found");
}

void WebHandlerContext::sendBadRequest(AsyncWebServerRequest* request, const String& message) {
    sendError(request, 400, message);
}

// ============ 内置页面 ============

void WebHandlerContext::sendBuiltinSetupPage(AsyncWebServerRequest* request) {
    noteWebRequestActivity();
    // 优先从 LittleFS 提供（节省 ~4KB 固件 Flash）
    if (LittleFS.exists("/www/setup.html.gz")) {
        request->send(LittleFS, "/www/setup.html.gz", "text/html");
    } else if (LittleFS.exists("/www/setup.html")) {
        request->send(LittleFS, "/www/setup.html", "text/html");
    } else {
        request->send(200, "text/html", "<html><body><h1>Setup page not found in filesystem</h1><p>Please upload LittleFS image first.</p></body></html>");
    }
}

// ============ 文件服务方法 ============

bool WebHandlerContext::serveStaticFile(AsyncWebServerRequest* request, const String& path) {
    String ext = path.substring(path.lastIndexOf('.'));
    String contentType = getContentType(path);
    bool isCompressible = (ext == ".html" || ext == ".js" || ext == ".css");
    bool isCoreEntry = (path == "/www/index.html" || path == "/www/index.html.gz" ||
                        path == "/www/setup.html" || path == "/www/setup.html.gz");
    bool isServiceWorker = (path == "/www/sw.js" || path == "/sw.js");
    bool isCacheableStatic = !isCoreEntry && !isServiceWorker;
    unsigned long staticHoldMs = isCoreEntry ? 3000UL : (isCompressible ? 5000UL : 1500UL);
    noteWebRequestActivity(staticHoldMs);

    // 入口内存门控：largestBlock < 2KB 时拒接静态文件请求（历史经验：不要设太高造成 503 风暴）
    // 避免 AsyncFileResponse 内部分配出错造成 lwIP 死锁
    {
        auto* fw = FastBeeFramework::getInstance();
        HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
        if (monitor) {
            size_t minLargestBlock = 2048;
            if (isCompressible && !isCoreEntry) {
                minLargestBlock = 3072;
            }
            // DRAM free gate (8BIT caliber): INTERNAL 口径含 IRAM/RTC 不可
            // malloc 内存，实测比真实可用 DRAM 虚高 ~28KB；而响应发送缓冲/
            // lwIP pbuf 都从 8BIT DRAM 分配。门限校准（实测）：MQTT 连接
            // 稳态 8BIT 常态 ~7KB，门限取 6KB 保证连接态下登录页资源仍可
            // 服务；已有 nothrow 降级缓冲（2872->256B）+ _fillBuffer 同步
            // 重试，6KB 余量足以推进响应（pbuf 随 ACK 滚动释放）。
            uint32_t freeDram = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            if (freeDram < 6144U) {
                // 临界水位：不传 path.c_str()（堆指针解引用实测 panic），
                // ets_printf 纯数字输出；资源路径可由前一跳访问日志反推
                ets_printf("[MemGuard] Reject static: DRAM(8bit) free=%lu < 6KB\n",
                           (unsigned long)freeDram);
                if (isCoreEntry) { sendStaticBusyRetryPage(request); }
                else { sendServiceUnavailable(request); }
                return true;
            }
            // largest gate 也用 8BIT 口径（INTERNAL largest 虚高，崩溃现场
            // blkint=27636 但 blk8 仅 116，旧门控完全失效）
            uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            if (largestBlock < minLargestBlock) {
                ets_printf("[MemGuard] Reject static: largest8=%lu < %uB\n",
                           (unsigned long)largestBlock,
                           static_cast<unsigned>(minLargestBlock));
                if (isCoreEntry) { sendStaticBusyRetryPage(request); }
                else { sendServiceUnavailable(request); }
                return true;
            }
        }
    }

    // Concurrency limit: cap in-flight static responses (crash-loop fix);
    // serialise large transfers instead of letting parallel streams drain DRAM.
    // 小尺寸未压缩资源（favicon/logo 等图片）不占限流槽：登录页首屏必须
    // 立即拿到它们，且单文件仅几 KB，不构成历史崩溃场景（大文件并发流）。
    // 2026-08-24 修复K：.gz 直链（如 /js/modules/*.js.gz）旧逻辑被判为
    // 不可压缩资源而绕过限流槽，实测 12 并发 gz 直链 4s 内把 free8 从
    // 31KB 抽到 3KB，90s 无恢复窗口触发 DRAM<8KB 看门狗重启。gz 正是
    // 最大的存储形态（单文件可达数十 KB），必须占槽。
    const bool bypassInFlightCap = !isCompressible && ext != ".gz";

    // 2026-08-04 fix: acquire the in-flight slot AFTER a successful open.
    // Previously the slot was taken before probing the filesystem, so a
    // missing file (gz miss + raw miss -> 404 fallback) leaked the slot and
    // every later compressible request got 503 until the 12s watchdog reset.
    // Open the file first (gz preferred for html/js/css), then take the slot.
    File openFile;
    if (isCompressible) {
        openFile = LittleFS.open(path + ".gz", "r");
    }
    if (!openFile) {
        openFile = LittleFS.open(path, "r");
    }
    if (!openFile) {
        return false;  // 404 fallback; no slot was taken
    }

    if (!bypassInFlightCap && !staticInFlightAllowed()) {
        openFile.close();
        if (isCoreEntry) {
            // 浏览器主文档被限流：返回自动重试页，避免 503 JSON 白屏
            sendStaticBusyRetryPage(request);
            return true;
        }
        // busy 响应的 MIME 与资源类型匹配：JS 请求收到 application/json 会触发
        // 浏览器严格 MIME 检查拒绝执行，产生 console 报错噪音；返回无害占位体
        // 保持 503 状态码不变，前端加载器仍按非 200 重试，语义一致
        const char* busyBody = "{\"success\":false,\"message\":\"Static file busy, retry soon\",\"code\":503,\"retryAfter\":1}";
        String busyType = "application/json";
        if (contentType == "application/javascript") {
            busyType = "application/javascript";
            busyBody = "// 503 busy - retry";
        } else if (contentType == "text/css") {
            busyType = "text/css";
            busyBody = "/* 503 busy - retry */";
        }
        AsyncWebServerResponse* busy = request->beginResponse(503, busyType, busyBody);
        busy->addHeader("Retry-After", "1");
        busy->addHeader("Connection", "close");
        request->send(busy);
        return true;
    }

    AsyncWebServerResponse *response = new AsyncFileResponse(openFile, path, contentType, false);

    // Keep the service worker uncached so clients can recover.
    if (isServiceWorker) {
        response->addHeader("Service-Worker-Allowed", "/");
        response->addHeader("Cache-Control", "no-cache");
    } else if (isCacheableStatic && cacheDuration > 0) {
        response->addHeader("Cache-Control", "public, max-age=" + String(cacheDuration));
    } else {
        response->addHeader("Cache-Control", "no-cache");
    }
    response->addHeader("Connection", "close");

    if (!bypassInFlightCap) registerStaticInFlightRelease(request);
    request->send(response);
    return true;
}

void WebHandlerContext::serveGzippedFile(AsyncWebServerRequest* request, const String& path) {
    // 统一使用 serveStaticFile，已包含 gzip 和缓存逻辑
    if (serveStaticFile(request, path)) return;
    sendNotFound(request);
}

String WebHandlerContext::getContentType(const String& filename) {
    if (filename.endsWith(".html")) return "text/html";
    if (filename.endsWith(".css")) return "text/css";
    if (filename.endsWith(".js")) return "application/javascript";
    if (filename.endsWith(".png")) return "image/png";
    if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) return "image/jpeg";
    if (filename.endsWith(".gif")) return "image/gif";
    if (filename.endsWith(".ico")) return "image/x-icon";
    if (filename.endsWith(".svg")) return "image/svg+xml";
    if (filename.endsWith(".json")) return "application/json";
    if (filename.endsWith(".xml")) return "application/xml";
    if (filename.endsWith(".pdf")) return "application/pdf";
    if (filename.endsWith(".zip")) return "application/zip";
    if (filename.endsWith(".gz")) return "application/x-gzip";
    if (filename.endsWith(".txt")) return "text/plain";
    return "application/octet-stream";
}

String WebHandlerContext::readFile(const String& path) {
    if (!LittleFS.exists(path)) {
        return "";
    }

    File file = LittleFS.open(path, "r");
    if (!file) {
        return "";
    }

    // 审计 P2-1：同 FileUtils::readFile 的文件读取闸门（超限/低 DRAM/reserve）
    size_t fileSize = file.size();
    if (fileSize > FastBee::MemoryBudget::FILE_READ_MAX_SIZE) {
        Serial.printf("[WebCtx] readFile rejects %s: size %u exceeds cap\n",
                      path.c_str(), (unsigned)fileSize);
        file.close();
        return "";
    }
    if (FastBee::MemoryBudget::dramFree() < fileSize + 8192U) {
        Serial.printf("[WebCtx] readFile rejects %s: DRAM too low\n", path.c_str());
        file.close();
        return "";
    }

    String content;
    if (!content.reserve(fileSize + 64)) {
        file.close();
        return "";
    }
    content = file.readString();
    file.close();
    return content;
}

bool WebHandlerContext::fileExists(const String& path) {
    return LittleFS.exists(path);
}

// ============ 工具方法 ============

String WebHandlerContext::formatUptime(unsigned long ms) {
    unsigned long seconds = ms / 1000;
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;
    unsigned long days = hours / 24;

    char buf[64];
    if (days > 0) {
        snprintf(buf, sizeof(buf), "%lu天 %02lu:%02lu:%02lu", days, hours % 24, minutes % 60, seconds % 60);
    } else {
        snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", hours, minutes % 60, seconds % 60);
    }
    return String(buf);
}

void WebHandlerContext::loadCacheDuration() {
    cacheDuration = 86400; // 默认24小时
    File f = LittleFS.open("/config/device.json", "r");
    if (f) {
        JsonDocument doc;
        if (!deserializeJson(doc, f)) {
            if (doc["cacheDuration"].is<int>()) {
                cacheDuration = (uint32_t)doc["cacheDuration"].as<int>();
            }
        }
        f.close();
    }
}

void WebHandlerContext::noteWebRequestActivity(unsigned long holdMs) {
    unsigned long now = millis();
    unsigned long nextUntil = now + holdMs;
    if (nextUntil > webForegroundUntilMs) {
        webForegroundUntilMs = nextUntil;
    }

    // 请求突增跟踪
    if (webConfigManager) {
        webConfigManager->trackWebRequest();
    }

    FastBeeFramework* framework = FastBeeFramework::getInstance();

    // MQTT 同步让位：首个前台请求到达时若 free8 已被 MQTT 挤压（<12KB），
    // 立即断开 MQTT 再服务响应——handle() 的让位要等下一个 tick，
    // 届时响应已入队卡在拥塞的 TX 通道（soak 首请求 1/40 挂起根因）。
    {
        ProtocolManager* pm = framework ? framework->getProtocolManager() : nullptr;
        MQTTClient* mqtt = pm ? pm->getMQTTClient() : nullptr;
        if (mqtt) {
            mqtt->yieldToWeb();
        }
    }

    TaskManager* taskManager = framework ? framework->getTaskManager() : nullptr;
    if (!taskManager || webForegroundModeActive) {
        return;
    }

    bool changed = false;
    changed = taskManager->disableTask("periph_exec_timer") || changed;
    // 注意：protocol_handle 不再禁用——MQTT 前台让位逻辑在 MQTTClient::handle()
    // 内，禁用它会让让位永远无法触发（实测死锁：前台时 MQTT 保持连接
    // 挤压 Web，free8=21KB 仍发生响应截断）。
    changed = taskManager->disableTask("network_update") || changed;

    if (changed) {
        webForegroundModeActive = true;
        LOG_INFO("[WebConfig] Foreground request protection enabled");
    }
}

void WebHandlerContext::maintainWebForegroundTaskBudget() {
    if (!webForegroundModeActive) {
        return;
    }

    unsigned long now = millis();
    if ((long)(webForegroundUntilMs - now) > 0) {
        return;
    }

    FastBeeFramework* framework = FastBeeFramework::getInstance();
    TaskManager* taskManager = framework ? framework->getTaskManager() : nullptr;
    if (!taskManager) {
        webForegroundModeActive = false;
        return;
    }

    taskManager->enableTask("network_update");
    taskManager->enableTask("periph_exec_timer");
    webForegroundModeActive = false;
    LOG_INFO("[WebConfig] Foreground request protection cleared");
}
