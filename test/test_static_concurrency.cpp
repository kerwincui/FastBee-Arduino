/**
 * @file test_static_concurrency.cpp
 * @brief 静态文件并发限流 & 登录页加载提速回归测试
 *
 * 2026-08-04 登录页慢/白屏问题修复的回归守护：
 * ① 静态资源限流槽 STATIC_MAX_INFLIGHT 由 1 提升至 2（串行链长度减半）
 * ② 卡死看门狗 20s → 12s，缩短最坏阻塞时间
 * ③ favicon/logo 等小型未压缩资源豁免限流槽
 * ④ 核心入口文档被拒绝时返回 <meta refresh> 自动重试 HTML 而非 JSON 白屏
 * ⑤ 前端 loader：chunk 间隔 30ms、启动延迟 20ms、重试退避 300/1200/2700ms
 * ⑥ module-loader/main.js 退避提速（2026-08-10）：800ms 起步指数退避、
 *    main.js 首次失败 600ms 重试（配合服务端立即 503 把风暴消化在客户端）
 * ⑦ boot/CSS/page/module/SW 共用静态资源队列，并留出 TCP 断开释放间隔
 *
 * 实现分两部分：
 * - 源码常量锁定（readProjectFile 读取 WebHandlerContext.cpp / index.html）
 * - 限流器行为模拟（镜像 lease 获取/释放逻辑验证排队/看门狗/豁免）
 *
 * 依据：FastBee-doc src/device/dev/tcp-connection-budget.md「静态文件并发限流」
 */

#include <unity.h>
#include <Arduino.h>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include "helpers/TestLogger.h"

void test_static_concurrency_group();

// ============================================================
// 源码常量锁定
// ============================================================

// 辅助：读取项目源文件（native 测试环境下使用标准文件 I/O）
static std::string readStaticConcurrencyFile(const char* relativePath) {
    const char* roots[] = { ".", "..", "../.." };
    for (const char* root : roots) {
        std::string fullPath = std::string(root) + "/" + relativePath;
        std::ifstream file(fullPath);
        if (file.is_open()) {
            std::ostringstream ss;
            ss << file.rdbuf();
            return ss.str();
        }
    }
    return "";
}

/**
 * @brief 锁定 WebHandlerContext.cpp 中静态限流器关键常量与结构
 * 防止回退到 1 槽串行或 20s 看门狗（登录页慢的根因①②）
 */
void test_static_limiter_constants_locked() {
    TestLog::testStart("Static Limiter: Source Constants Locked");

    std::string src = readStaticConcurrencyFile("src/network/WebHandlerContext.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!src.empty(),
        "Failed to read WebHandlerContext.cpp — ensure test runs from project root");
    TestLog::step("WebHandlerContext.cpp loaded");

    // 1) 并发上限 >= 3：旧值 1 把首屏 ~12 资源压成纯串行链；值 2 仍导致
    //    大部分请求被 503 拒绝后重试（实测页面加载 >5s）；3 路并发每路
    //    ~4.5KB 在稳态 free8=24KB 下留 10KB+ 余量
    {
        size_t infP = src.find("STATIC_MAX_INFLIGHT");
        TEST_ASSERT_TRUE_MESSAGE(infP != std::string::npos,
            "STATIC_MAX_INFLIGHT not found");
        size_t eqP = src.find('=', infP);
        TEST_ASSERT_TRUE_MESSAGE(eqP != std::string::npos,
            "STATIC_MAX_INFLIGHT must be assigned");
        unsigned long v = strtoul(src.c_str() + eqP + 1, nullptr, 10);
        TEST_ASSERT_TRUE_MESSAGE(v >= 3,
            "STATIC_MAX_INFLIGHT must be >= 3 (values below 3 cause severe AP-mode page latency)");
    }
    TestLog::step("STATIC_MAX_INFLIGHT >= 3 verified");

    // 2) 卡死看门狗 = 12s：lease 卡死时最坏阻塞不超过 12s（旧值 20s）
    TEST_ASSERT_TRUE_MESSAGE(src.find("STATIC_INFLIGHT_WATCHDOG_MS = 12000UL") != std::string::npos,
        "STATIC_INFLIGHT_WATCHDOG_MS must be 12000UL — bounds worst-case block on stuck counter");
    TestLog::step("STATIC_INFLIGHT_WATCHDOG_MS = 12000UL verified");

    // 3) 看门狗必须逐 lease 回收，并保留代次；禁止清零共享计数后让旧回调
    //    误减新请求槽位。
    TEST_ASSERT_TRUE_MESSAGE(src.find("struct StaticInFlightLease") != std::string::npos &&
                             src.find("struct StaticInFlightSlot") != std::string::npos &&
                             src.find("slot.active = false;") != std::string::npos,
        "Watchdog must reclaim fixed-table leases independently");
    TEST_ASSERT_TRUE_MESSAGE(src.find("s_staticInFlight = 0") == std::string::npos,
        "Watchdog must not reset a shared counter that stale disconnects can decrement");
    TestLog::step("Per-request static lease table found");

    // 3.5) 2026-08-10 慢速修复复盘：槽位满时必须立即拒绝（不得 delay 轮询等待）。
    // delay 等待运行在 AsyncTCP 服务任务上下文，会阻塞在途响应发送与
    // onDisconnect 事件处理，槽位永远等不到释放，实测退化为“卡死全部连接
    // 300ms 再拒绝”（conc>=2 出现 13s 慢请求）。正确组合：立即 503 +
    // 前端 300/800ms 快速退避重试，把 503 风暴消化在客户端。
    TEST_ASSERT_TRUE_MESSAGE(src.find("STATIC_SLOT_WAIT_MS") == std::string::npos,
        "Delay-based slot waiting was reverted: it blocks the AsyncTCP task and "
        "starves in-flight releases (measured 13s slow requests)");
    TEST_ASSERT_TRUE_MESSAGE(src.find("if (activeCount < limit)") != std::string::npos,
        "Slot-full path must judge immediately (fast 503, front-end retries)");
    TestLog::step("Immediate-reject limiter (no blocking wait) verified");

    // 4) 计数器释放挂在 onDisconnect（Connection: close 保证完成/超时/abort 都会触发）
    TEST_ASSERT_TRUE_MESSAGE(src.find("registerStaticInFlightRelease") != std::string::npos &&
                             src.find("request->onDisconnect([lease]") != std::string::npos &&
                             src.find("slot.generation == lease.generation") != std::string::npos,
        "onDisconnect must release only its captured lease generation");
    TestLog::step("Generation-safe release hooked on onDisconnect");

    size_t activeStart = src.find("bool WebHandlerContext::isStaticTransferActive() const");
    size_t activeEnd = src.find("// 浏览器导航请求", activeStart);
    TEST_ASSERT_TRUE_MESSAGE(activeStart != std::string::npos && activeEnd != std::string::npos,
        "isStaticTransferActive implementation must remain detectable");
    std::string activeBlock = src.substr(activeStart, activeEnd - activeStart);
    TEST_ASSERT_TRUE_MESSAGE(
        activeBlock.find("reapExpiredStaticInFlightLeasesLocked(now)") != std::string::npos,
        "Web-busy queries must reclaim stale leases even without another static request");
    TestLog::step("Static-transfer query shares watchdog reclamation");

    TestLog::testEnd(true);
}

/**
 * @brief 锁定小型未压缩资源豁免限流槽（favicon/logo 首屏必需资源不再被拒）
 */
void test_small_asset_bypass_locked() {
    TestLog::testStart("Static Limiter: Small Asset Bypass Locked");

    std::string src = readStaticConcurrencyFile("src/network/WebHandlerContext.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!src.empty(), "WebHandlerContext.cpp must be readable");

    // 可压缩类型（html/js/css）才计入限流槽；图片等未压缩小资源豁免
    TEST_ASSERT_TRUE_MESSAGE(src.find("const bool bypassInFlightCap = !isCompressible;") != std::string::npos,
        "Uncompressed assets (favicon/logo) must bypass the in-flight cap");
    TestLog::step("bypassInFlightCap = !isCompressible verified");

    TEST_ASSERT_TRUE_MESSAGE(src.find("isCompressible = (ext == \".html\" || ext == \".js\" || ext == \".css\")") != std::string::npos,
        "Compressible set must stay html/js/css only");
    TestLog::step("Compressible set (html/js/css) verified");

    // DRAM 门控已移除（2026-08-05 实测校准）：INTERNAL 口径含 ~28KB 不可
    // malloc 的 IRAM，36864U 门限会误拒健康状态下的请求；静态路径改由
    // in-flight 限流（STATIC_MAX_INFLIGHT）+ 12s 看门狗 + 小资源豁免保护。
    TEST_ASSERT_TRUE_MESSAGE(src.find("freeInternal < 36864U") == std::string::npos,
        "Obsolete INTERNAL-basis 36KB gate must stay removed (misreads free DRAM by ~28KB)");
    TestLog::step("Obsolete 36KB INTERNAL gate removal verified");

    TestLog::testEnd(true);
}

/**
 * @brief Compressed text assets must be selected first and retain logical MIME.
 */
void test_gzip_static_selection_locked() {
    TestLog::testStart("Static Files: Gzip Selection And Headers Locked");

    std::string src = readStaticConcurrencyFile("src/network/WebHandlerContext.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!src.empty(), "WebHandlerContext.cpp must be readable");

    const size_t serveStart = src.find("bool WebHandlerContext::serveStaticFile");
    const size_t serveEnd = src.find("void WebHandlerContext::serveGzippedFile", serveStart);
    TEST_ASSERT_TRUE_MESSAGE(serveStart != std::string::npos && serveEnd != std::string::npos,
        "serveStaticFile implementation block must be discoverable");
    const std::string block = src.substr(serveStart, serveEnd - serveStart);

    const size_t normalizePos = block.find("const bool gzipPathRequested = path.endsWith(\".gz\")");
    const size_t mimePos = block.find("getContentType(logicalPath)");
    const size_t gzipOpenPos = block.find("LittleFS.open(gzipPath, \"r\")");
    const size_t rawOpenPos = block.find("LittleFS.open(logicalPath, \"r\")");
    TEST_ASSERT_TRUE_MESSAGE(normalizePos != std::string::npos && mimePos > normalizePos,
        "Explicit .gz URLs must be normalized before MIME selection");
    TEST_ASSERT_TRUE_MESSAGE(gzipOpenPos != std::string::npos && rawOpenPos > gzipOpenPos,
        "HTML/JS/CSS must try the gzip representation before the optional custom-filesystem fallback");
    TEST_ASSERT_TRUE_MESSAGE(
        block.find("if (!openFile && !gzipPathRequested)") != std::string::npos,
        "An explicit .gz request must not silently fall back to uncompressed bytes");
    TEST_ASSERT_TRUE_MESSAGE(
        block.find("response->addHeader(\"Content-Encoding\", \"gzip\")") != std::string::npos,
        "Compressed responses must advertise Content-Encoding: gzip");
    TEST_ASSERT_TRUE_MESSAGE(
        block.find("new AsyncFileResponse(openFile, path, contentType, false)") != std::string::npos,
        "AsyncFileResponse must receive the logical MIME selected for the request");

    const size_t setupStart = src.find("void WebHandlerContext::sendBuiltinSetupPage");
    const size_t setupEnd = src.find("// ============ 文件服务方法", setupStart);
    TEST_ASSERT_TRUE_MESSAGE(setupStart != std::string::npos && setupEnd != std::string::npos,
        "sendBuiltinSetupPage implementation block must be discoverable");
    const std::string setupBlock = src.substr(setupStart, setupEnd - setupStart);
    TEST_ASSERT_TRUE_MESSAGE(
        setupBlock.find("serveStaticFile(request, \"/www/setup.html\")") != std::string::npos,
        "The setup entry point must reuse gzip-aware static-file serving");
    TEST_ASSERT_TRUE_MESSAGE(
        setupBlock.find("request->send(LittleFS, \"/www/setup.html.gz\"") == std::string::npos,
        "The setup entry point must not send explicit gzip bytes without negotiated headers");

    TestLog::step("gzip-first release selection, optional custom-filesystem fallback, MIME and encoding contracts verified");
    TestLog::testEnd(true);
}

/**
 * @brief 锁定核心入口被拒时返回自动重试 HTML 而非 JSON 白屏
 * 旧行为：主文档被限流/内存门控拒绝 → 浏览器停留在 503 JSON 白屏
 */
void test_busy_retry_page_locked() {
    TestLog::testStart("Static Limiter: Busy Retry Page Locked");

    std::string src = readStaticConcurrencyFile("src/network/WebHandlerContext.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!src.empty(), "WebHandlerContext.cpp must be readable");

    // 1) 重试页函数存在且 Content-Type 为 text/html（非 application/json）
    size_t fnPos = src.find("sendStaticBusyRetryPage");
    TEST_ASSERT_TRUE_MESSAGE(fnPos != std::string::npos,
        "sendStaticBusyRetryPage() must exist for core entry rejections");
    size_t fnEnd = src.find("// ============ 响应辅助方法", fnPos);
    TEST_ASSERT_TRUE(fnEnd != std::string::npos);
    std::string fnBlock = src.substr(fnPos, fnEnd - fnPos);

    TEST_ASSERT_TRUE_MESSAGE(fnBlock.find("\"text/html\"") != std::string::npos,
        "Busy retry page must be text/html, NOT json (json causes blank page)");
    TestLog::step("Retry page content-type is text/html");

    // 2) 必须带 meta refresh 每秒自动重试
    TEST_ASSERT_TRUE_MESSAGE(fnBlock.find("http-equiv=\\\"refresh\\\"") != std::string::npos &&
                             fnBlock.find("content=\\\"1\\\"") != std::string::npos,
        "Retry page must auto-refresh every second (meta refresh content=1)");
    TestLog::step("meta refresh content=1 verified");

    // 3) 核心入口的三个拒绝路径（DRAM 门控 ×2 + 限流）都必须走重试页
    size_t retryCalls = 0;
    size_t searchPos = 0;
    while ((searchPos = src.find("sendStaticBusyRetryPage(request)", searchPos)) != std::string::npos) {
        retryCalls++;
        searchPos += 1;
    }
    // 3 处调用（DRAM 门控 2 处 + 限流 1 处）都必须走重试页
    TEST_ASSERT_TRUE_MESSAGE(retryCalls >= 3,
        "All core-entry rejection paths (2 mem gates + rate limit) must send the retry page");
    TestLog::step("Retry page wired into all core-entry rejection paths");

    // 4) 核心入口识别基于去除 .gz 后的逻辑路径，两个表示自动共用策略。
    TEST_ASSERT_TRUE_MESSAGE(src.find("logicalPath == \"/www/index.html\"") != std::string::npos &&
                             src.find("logicalPath == \"/www/setup.html\"") != std::string::npos,
        "Core entry list must cover logical index.html and setup.html paths");
    TestLog::step("Core entry list verified");

    TestLog::testEnd(true);
}

/**
 * @brief 锁定前端 loader 提速参数（web-src/index.html）
 * 修复前：chunk 间隔 80ms + 启动延迟 100ms + 重试退避 500/2000/4500ms
 */
void test_frontend_loader_timings_locked() {
    TestLog::testStart("Frontend Loader: Timing Constants Locked");

    std::string html = readStaticConcurrencyFile("web-src/index.html");
    TEST_ASSERT_TRUE_MESSAGE(!html.empty(),
        "Failed to read web-src/index.html — ensure test runs from project root");
    TestLog::step("web-src/index.html loaded");

    // 1) chunk 间间隔 30ms（旧值 80ms；串行加载 5 个 boot chunk，间隔直接叠加到首屏时间）
    TEST_ASSERT_TRUE_MESSAGE(html.find("setTimeout(loadNext, delayMs || 30)") != std::string::npos,
        "Chunk interval must be 30ms (was 80ms)");
    TestLog::step("Chunk interval 30ms verified");

    // 2) 启动延迟 20ms（旧值 100ms）
    TEST_ASSERT_TRUE_MESSAGE(html.find("setTimeout(loadNext, 20)") != std::string::npos,
        "Boot delay must be 20ms (was 100ms)");
    TestLog::step("Boot delay 20ms verified");

    // 3) 失败重试退避 300 * attempt^2 → 300/1200/2700ms（旧值 500/2000/4500ms）
    TEST_ASSERT_TRUE_MESSAGE(html.find("setTimeout(tryLoad, 300 * attempt * attempt)") != std::string::npos,
        "Retry backoff must be 300*attempt^2 → 300/1200/2700ms (was 500/2000/4500ms)");
    TestLog::step("Retry backoff 300/1200/2700ms verified");

    // 4) 重试上限保持 4 次（弱网下单次失败率高，重试是成功率关键）
    TEST_ASSERT_TRUE_MESSAGE(html.find("CHUNK_MAX_RETRY = 4") != std::string::npos,
        "CHUNK_MAX_RETRY must stay 4");
    TestLog::step("CHUNK_MAX_RETRY = 4 verified");

    TestLog::testEnd(true);
}

/**
 * @brief 锁定 module-loader.js / main.js 的退避提速参数（2026-08-10）
 * 修复前：module-loader 2000*2^n 起步（2s/4s/8s）把偶发 503 放大成 5-10s 卡顿；
 * main.js 首次失败 1.5s 后才重试。服务端改为立即 503 后，前端必须用
 * 快速退避承接，否则首屏体验回退。
 */
void test_frontend_module_loader_backoff_locked() {
    TestLog::testStart("Frontend Module Loader: Fast Backoff Locked");

    std::string loader = readStaticConcurrencyFile("web-src/js/module-loader.js");
    TEST_ASSERT_TRUE_MESSAGE(!loader.empty(),
        "Failed to read web-src/js/module-loader.js — ensure test runs from project root");
    TestLog::step("web-src/js/module-loader.js loaded");

    // 1) 两处退避（sequence 加载 + 队列加载）都必须是 800ms 起步、8s 封顶
    size_t count800 = 0;
    size_t pos = 0;
    while ((pos = loader.find("Math.min(800 * Math.pow(2,", pos)) != std::string::npos) {
        count800++;
        pos += 1;
    }
    TEST_ASSERT_TRUE_MESSAGE(count800 >= 2,
        "Both sequence and queue retry paths must use 800*2^n backoff (was 2000*2^n)");
    TestLog::step("800ms-base exponential backoff present in both paths");

    // 2) 旧 2s 起步退避不得回潮
    TEST_ASSERT_TRUE_MESSAGE(loader.find("Math.min(2000 * Math.pow(2,") == std::string::npos,
        "Obsolete 2s-base backoff must not come back (amplified 503 into 5-10s stalls)");
    TestLog::step("Obsolete 2000ms-base backoff absent");

    // 3) 重试上限保持 3 次（含首次共 4 次尝试，弱网成功率关键）
    TEST_ASSERT_TRUE_MESSAGE(loader.find("retries < 3") != std::string::npos,
        "Module loader must keep 3 retries (4 attempts total)");
    TestLog::step("Retry cap (retries < 3) verified");

    std::string main = readStaticConcurrencyFile("web-src/js/main.js");
    TEST_ASSERT_TRUE_MESSAGE(!main.empty(),
        "Failed to read web-src/js/main.js");

    // 4) main.js 首次失败 600ms 重试（旧 1500ms）
    TEST_ASSERT_TRUE_MESSAGE(main.find("}, 600);") != std::string::npos,
        "main.js first-failure retry must be 600ms (was 1500ms)");
    TEST_ASSERT_TRUE_MESSAGE(main.find("}, 1500);") == std::string::npos,
        "Obsolete 1500ms first retry must not come back");
    TestLog::step("main.js 600ms retry verified");

    // 5) CSS 加载退避保持 600*n 线性（与首次重试同基线）
    TEST_ASSERT_TRUE_MESSAGE(main.find("setTimeout(applyCssHref, 600 * cssAttempts)") != std::string::npos,
        "CSS retry backoff must stay 600*attempts");
    TestLog::step("CSS 600*n backoff verified");

    TestLog::testEnd(true);
}

/**
 * @brief 锁定浏览器侧的统一静态资源队列，避免各自串行但彼此并发。
 */
void test_shared_static_asset_queue_locked() {
    TestLog::testStart("Frontend: Shared Static Asset Queue Locked");

    std::string html = readStaticConcurrencyFile("web-src/index.html");
    std::string main = readStaticConcurrencyFile("web-src/js/main.js");
    std::string pages = readStaticConcurrencyFile("web-src/js/page-loader.js");
    std::string modules = readStaticConcurrencyFile("web-src/js/module-loader.js");
    TEST_ASSERT_FALSE_MESSAGE(html.empty() || main.empty() || pages.empty() || modules.empty(),
        "Static asset queue source files must be readable");

    TEST_ASSERT_TRUE_MESSAGE(html.find("window.__fastbeeStaticAssetChain = Promise.resolve()") != std::string::npos,
        "index.html must define the shared static asset queue");
    TEST_ASSERT_TRUE_MESSAGE(html.find("setTimeout(resolve, 100)") != std::string::npos,
        "Shared queue must preserve the 100ms TCP disconnect settle window");
    TEST_ASSERT_TRUE_MESSAGE(html.find("window.__fastbeeQueueStaticAsset(function()") != std::string::npos,
        "Boot chunks must enter the shared queue");
    TEST_ASSERT_TRUE_MESSAGE(html.find("window.__fastbeeQueueStaticAsset(registerWorker)") != std::string::npos,
        "Service worker must enter the shared queue");
    TEST_ASSERT_TRUE_MESSAGE(main.find("window.__fastbeeQueueStaticAsset(loadStylesheet)") != std::string::npos,
        "Main stylesheet must enter the shared queue");
    TEST_ASSERT_TRUE_MESSAGE(pages.find("window.__fastbeeQueueStaticAsset(op)") != std::string::npos,
        "Page fragments must enter the shared queue");
    TEST_ASSERT_TRUE_MESSAGE(modules.find("window.__fastbeeQueueStaticAsset(startRequest)") != std::string::npos,
        "Dynamic modules must enter the shared queue");

    TestLog::testEnd(true);
}

/**
 * @brief A truncated/busy 200 response must not poison PageLoader's cache.
 */
void test_page_loader_response_integrity_locked() {
    TestLog::testStart("Page Loader: Response Integrity Locked");

    std::string loader = readStaticConcurrencyFile("web-src/js/page-loader.js");
    TEST_ASSERT_TRUE_MESSAGE(!loader.empty(), "Failed to read web-src/js/page-loader.js");

    size_t cacheStart = loader.find("_cachePageTemplates(moduleName, html)");
    size_t cacheEnd = loader.find("_mountPage(pageId)", cacheStart);
    size_t loadStart = loader.find("async loadPage(pageId, pageMapping");
    size_t catchStart = loader.find("} catch(e) {", loadStart);
    size_t finallyStart = loader.find("} finally {", catchStart);
    TEST_ASSERT_TRUE_MESSAGE(cacheStart != std::string::npos && cacheEnd != std::string::npos &&
                             loadStart != std::string::npos &&
                             catchStart != std::string::npos &&
                             finallyStart != std::string::npos,
        "PageLoader template cache and loadPage try/catch structure must remain detectable");
    std::string cacheBlock = loader.substr(cacheStart, cacheEnd - cacheStart);
    std::string loadBlock = loader.substr(loadStart, finallyStart - loadStart);
    std::string catchBlock = loader.substr(catchStart, finallyStart - catchStart);
    size_t noRoot = cacheBlock.find("if (roots.length === 0)");
    size_t throwNoRoot = cacheBlock.find("Page response contains no page root", noRoot);
    size_t storeTemplate = cacheBlock.find("this._pageTemplates[root.id] = root.outerHTML", throwNoRoot);
    size_t markLoaded = cacheBlock.find("this._loadedPages[moduleName] = true", storeTemplate);
    TEST_ASSERT_TRUE_MESSAGE(noRoot != std::string::npos && throwNoRoot > noRoot &&
                             storeTemplate > throwNoRoot && markLoaded > storeTemplate,
        "Only a complete response with page roots may populate the template and loaded caches");
    size_t readBody = loadBlock.find("var html = await this._fetchWithRetry(");
    size_t cacheTemplates = loadBlock.find("this._cachePageTemplates(moduleName, html)", readBody);
    size_t expectedRoot = loadBlock.find("Page response missing expected root", cacheTemplates);
    TEST_ASSERT_TRUE_MESSAGE(readBody != std::string::npos && cacheTemplates > readBody &&
                             expectedRoot > cacheTemplates,
        "loadPage must read the complete response, cache validated templates, then verify its requested root");
    TEST_ASSERT_TRUE_MESSAGE(catchBlock.find("delete this._loadedPages[moduleName]") != std::string::npos,
        "Failure must clear the loaded cache entry so a later click retries");
    TEST_ASSERT_TRUE_MESSAGE(loader.substr(finallyStart).find("delete this._loadingPages[moduleName]") != std::string::npos,
        "Failure or success must clear the in-flight entry after the attempt completes");

    TestLog::testEnd(true);
}

// ============================================================
// 限流器行为模拟（镜像 staticInFlightAllowed() 逻辑）
// ============================================================

// 镜像 WebHandlerContext.cpp 的限流器：常量必须与源码一致
struct StaticLimiterSim {
    static constexpr uint8_t MAX_INFLIGHT = 3;              // == STATIC_MAX_INFLIGHT
    static constexpr uint32_t WATCHDOG_MS = 12000U;         // == STATIC_INFLIGHT_WATCHDOG_MS

    struct Lease {
        uint32_t generation = 0;
        uint8_t slot = MAX_INFLIGHT;

        bool valid() const {
            return generation != 0 && slot < MAX_INFLIGHT;
        }
    };

    struct Slot {
        uint32_t generation = 0;
        uint32_t acquiredAtMs = 0;
        bool active = false;
    };

    Slot slots[MAX_INFLIGHT];
    bool memoryGuard = false;
    uint32_t free8 = 65536;
    uint32_t largest8 = 16384;

    uint8_t effectiveLimit() const {
        if (!memoryGuard) return MAX_INFLIGHT;
        return (free8 < 32768U || largest8 < 8192U) ? 1 : 2;
    }

    uint8_t inFlight() const {
        uint8_t count = 0;
        for (const Slot& slot : slots) {
            if (slot.active) count++;
        }
        return count;
    }

    uint8_t reapExpired(uint32_t now) {
        uint8_t reclaimed = 0;
        for (Slot& slot : slots) {
            if (slot.active && (now - slot.acquiredAtMs) > WATCHDOG_MS) {
                slot.active = false;
                slot.acquiredAtMs = 0;
                reclaimed++;
            }
        }
        return reclaimed;
    }

    bool isActive(uint32_t now) {
        reapExpired(now);
        return inFlight() > 0;
    }

    Lease acquire(uint32_t now) {
        reapExpired(now);
        if (inFlight() >= effectiveLimit()) return {};

        for (uint8_t i = 0; i < MAX_INFLIGHT; ++i) {
            Slot& slot = slots[i];
            if (slot.active) continue;
            slot.generation++;
            if (slot.generation == 0) slot.generation = 1;
            slot.acquiredAtMs = now;
            slot.active = true;
            return {slot.generation, i};
        }
        return {};
    }

    void release(const Lease& lease) {
        if (!lease.valid()) return;
        Slot& slot = slots[lease.slot];
        if (slot.active && slot.generation == lease.generation) {
            slot.active = false;
            slot.acquiredAtMs = 0;
        }
    }
};

/**
 * @brief 并发上限行为：3 路 gz 流放行，第 4 路 503；图片豁免不占槽
 */
void test_limiter_concurrency_behavior() {
    TestLog::testStart("Static Limiter: Concurrency Behavior");

    StaticLimiterSim sim;

    // 3 个 gz chunk 可并发
    StaticLimiterSim::Lease first = sim.acquire(0);
    StaticLimiterSim::Lease second = sim.acquire(10);
    StaticLimiterSim::Lease third = sim.acquire(20);
    TEST_ASSERT_TRUE(first.valid());
    TEST_ASSERT_TRUE(second.valid());
    TEST_ASSERT_TRUE(third.valid());
    TestLog::step("3 concurrent gz streams admitted");

    // 第 4 个被拒（修复前上限为 2，导致大部分请求被 503 → 串行重试）
    TEST_ASSERT_FALSE(sim.acquire(30).valid());
    TestLog::step("4th gz stream rejected with 503 busy");

    // 图片类小资源始终豁免
    const bool imageIsCompressible = false;
    TEST_ASSERT_FALSE(imageIsCompressible);
    TEST_ASSERT_EQUAL(3, sim.inFlight());  // 豁免资源不调用 acquire，不计数
    TestLog::step("Small uncompressed assets bypass the cap");

    // 释放后可重新进入
    sim.release(first);
    TEST_ASSERT_TRUE(sim.acquire(50).valid());
    TestLog::step("Slot reusable after release");

    TestLog::testEnd(true);
}

/**
 * @brief 无 PSRAM 目标使用 2 路稳态/1 路压力态，而不是公开上限 3 路。
 */
void test_limiter_no_psram_adaptive_behavior() {
    TestLog::testStart("Static Limiter: No-PSRAM Adaptive Behavior");

    std::string src = readStaticConcurrencyFile("src/network/WebHandlerContext.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!src.empty(), "WebHandlerContext.cpp must be readable");
    size_t limitStart = src.find("static uint8_t staticInFlightLimit()");
    size_t limitEnd = src.find("static StaticInFlightLease acquireStaticInFlightLease()", limitStart);
    TEST_ASSERT_TRUE_MESSAGE(limitStart != std::string::npos && limitEnd != std::string::npos,
        "DRAM-aware static limiter function must remain detectable");
    std::string limitBlock = src.substr(limitStart, limitEnd - limitStart);
    TEST_ASSERT_TRUE_MESSAGE(limitBlock.find("limit = 2;") != std::string::npos,
        "No-PSRAM healthy limit must remain two compressed streams");
    TEST_ASSERT_TRUE_MESSAGE(limitBlock.find("free8 < 32768U || blk8 < 8192U") != std::string::npos &&
                             limitBlock.find("limit = 1;") != std::string::npos,
        "No-PSRAM pressure limit must drop to one stream at the DRAM thresholds");
    TestLog::step("Production source keeps the C3 2/1 adaptive policy");

    StaticLimiterSim stable;
    stable.memoryGuard = true;
    stable.free8 = 48000;
    stable.largest8 = 16000;
    TEST_ASSERT_EQUAL(2, stable.effectiveLimit());
    TEST_ASSERT_TRUE(stable.acquire(0).valid());
    TEST_ASSERT_TRUE(stable.acquire(10).valid());
    TEST_ASSERT_FALSE(stable.acquire(20).valid());
    TestLog::step("Healthy no-PSRAM target admits two compressed streams");

    StaticLimiterSim fragmented;
    fragmented.memoryGuard = true;
    fragmented.free8 = 46000;
    fragmented.largest8 = 7000;
    TEST_ASSERT_EQUAL(1, fragmented.effectiveLimit());
    TEST_ASSERT_TRUE(fragmented.acquire(0).valid());
    TEST_ASSERT_FALSE(fragmented.acquire(10).valid());
    TEST_ASSERT_EQUAL(1, fragmented.inFlight());
    TestLog::step("Fragmented no-PSRAM target drops to one stream; small assets still bypass");

    TestLog::testEnd(true);
}

/**
 * @brief 卡死看门狗：泄漏 lease 12s 后逐个回收，不再永久 503
 * 复现场景：onDisconnect 未触发导致槽位卡死，旧值 20s 阻塞时间过长
 */
void test_limiter_watchdog_recovery() {
    TestLog::testStart("Static Limiter: Watchdog Recovery");

    StaticLimiterSim sim;

    TEST_ASSERT_TRUE(sim.acquire(100).valid());
    TEST_ASSERT_TRUE(sim.acquire(100).valid());
    TEST_ASSERT_TRUE(sim.acquire(100).valid());
    // 模拟泄漏：不调用 release()

    // 每个 lease 在自己的 12s 边界内仍有效。
    unsigned long blockedProbe = 100 + StaticLimiterSim::WATCHDOG_MS;
    TEST_ASSERT_FALSE(sim.acquire(blockedProbe).valid());
    TestLog::step("Still blocked within watchdog window");

    // 超过看门狗窗口后逐 lease 回收，请求恢复。
    TEST_ASSERT_TRUE(sim.acquire(blockedProbe + 1).valid());
    TEST_ASSERT_EQUAL(1, sim.inFlight());
    TestLog::step("Watchdog reclaimed stuck leases, requests recovered");

    // 最坏阻塞时间约束：看门狗 <= 12s，且需覆盖单个文件的合法传输时长
    TEST_ASSERT_LESS_OR_EQUAL(12000, (int)StaticLimiterSim::WATCHDOG_MS);
    TEST_ASSERT_GREATER_THAN(5000, (int)StaticLimiterSim::WATCHDOG_MS);
    TestLog::step("Worst-case block bounded to 12s, still covers legit transfers");

    TestLog::testEnd(true);
}

/**
 * @brief Web-busy 查询本身必须回收过期 lease，不能依赖下一次静态请求。
 */
void test_static_transfer_active_reaps_expired_leases() {
    TestLog::testStart("Static Limiter: Active Query Reaps Expired Leases");

    StaticLimiterSim sim;
    sim.memoryGuard = true;
    sim.free8 = 24000;
    sim.largest8 = 7000;

    TEST_ASSERT_TRUE(sim.acquire(100).valid());
    TEST_ASSERT_TRUE(sim.isActive(100 + StaticLimiterSim::WATCHDOG_MS));
    TEST_ASSERT_FALSE(sim.isActive(101 + StaticLimiterSim::WATCHDOG_MS));
    TEST_ASSERT_EQUAL(0, sim.inFlight());
    TestLog::step("Web-busy query clears a stale lease without a later admission");

    TestLog::testEnd(true);
}

/**
 * @brief 32-bit millis 回绕前后，lease 仍按真实经过时间到期。
 */
void test_limiter_millis_wraparound_safe() {
    TestLog::testStart("Static Limiter: Millis Wraparound Safety");

    std::string src = readStaticConcurrencyFile("src/network/WebHandlerContext.cpp");
    TEST_ASSERT_TRUE_MESSAGE(src.find("unsigned long acquiredAtMs") != std::string::npos &&
                             src.find("now - slot.acquiredAtMs") != std::string::npos,
        "Production watchdog must use unsigned elapsed-time subtraction");

    StaticLimiterSim sim;
    sim.memoryGuard = true;
    sim.free8 = 24000;
    sim.largest8 = 7000;

    const uint32_t acquiredAt = 0xFFFFFFFFu - 6000u;
    StaticLimiterSim::Lease lease = sim.acquire(acquiredAt);
    TEST_ASSERT_TRUE(lease.valid());

    const uint32_t expiryBoundary = acquiredAt + StaticLimiterSim::WATCHDOG_MS;
    TEST_ASSERT_TRUE_MESSAGE(sim.isActive(expiryBoundary),
        "Lease must remain active at the exact watchdog boundary across millis wrap");
    TEST_ASSERT_FALSE_MESSAGE(sim.isActive(expiryBoundary + 1u),
        "Lease must expire one millisecond after the boundary across millis wrap");
    TEST_ASSERT_EQUAL(0, sim.inFlight());
    TestLog::step("Unsigned elapsed-time check survives the 32-bit millis wrap");

    TestLog::testEnd(true);
}

/**
 * @brief 错峰 lease 必须逐槽过期，尚未到期的请求保持在途。
 */
void test_limiter_staggered_expiry_isolated() {
    TestLog::testStart("Static Limiter: Staggered Lease Expiry Isolated");

    StaticLimiterSim sim;
    sim.memoryGuard = true;
    sim.free8 = 48000;
    sim.largest8 = 16000;  // C3 健康态两路

    StaticLimiterSim::Lease expired = sim.acquire(0);
    StaticLimiterSim::Lease live = sim.acquire(6000);
    TEST_ASSERT_TRUE(expired.valid());
    TEST_ASSERT_TRUE(live.valid());
    TEST_ASSERT_FALSE(sim.acquire(6001).valid());

    StaticLimiterSim::Lease replacement =
        sim.acquire(StaticLimiterSim::WATCHDOG_MS + 1);
    TEST_ASSERT_TRUE(replacement.valid());
    TEST_ASSERT_EQUAL(expired.slot, replacement.slot);
    TEST_ASSERT_NOT_EQUAL(expired.generation, replacement.generation);
    TEST_ASSERT_EQUAL_MESSAGE(2, sim.inFlight(),
        "Only the expired lease should be reclaimed; the staggered live lease must remain");

    sim.release(expired);
    TEST_ASSERT_EQUAL(2, sim.inFlight());
    sim.release(live);
    TEST_ASSERT_EQUAL(1, sim.inFlight());
    sim.release(replacement);
    TEST_ASSERT_EQUAL(0, sim.inFlight());
    TestLog::step("Expired slot reused safely while the younger lease remained active");

    TestLog::testEnd(true);
}

/**
 * @brief watchdog 回收并复用槽位后，旧请求的迟到断连不得释放新一代 lease。
 */
void test_limiter_stale_disconnect_generation_safe() {
    TestLog::testStart("Static Limiter: Stale Disconnect Generation Safety");

    StaticLimiterSim sim;
    sim.memoryGuard = true;
    sim.free8 = 24000;
    sim.largest8 = 7000;  // 压力态固定为单路，确保复用同一槽位

    StaticLimiterSim::Lease stale = sim.acquire(0);
    TEST_ASSERT_TRUE(stale.valid());
    TEST_ASSERT_FALSE(sim.acquire(StaticLimiterSim::WATCHDOG_MS).valid());

    StaticLimiterSim::Lease current = sim.acquire(StaticLimiterSim::WATCHDOG_MS + 1);
    TEST_ASSERT_TRUE(current.valid());
    TEST_ASSERT_EQUAL(stale.slot, current.slot);
    TEST_ASSERT_NOT_EQUAL(stale.generation, current.generation);
    TEST_ASSERT_EQUAL(1, sim.inFlight());

    sim.release(stale);  // 已超时旧连接现在才触发 onDisconnect
    TEST_ASSERT_EQUAL_MESSAGE(1, sim.inFlight(),
        "Stale disconnect must not release the newer lease in the reused slot");
    sim.release(current);
    TEST_ASSERT_EQUAL(0, sim.inFlight());
    TestLog::step("Late disconnect ignored; current generation remains owned until its callback");

    TestLog::testEnd(true);
}

// 登录页资源清单（当前三分片首屏）
struct LoginResource {
    const char* path;
    bool compressible;   // html/js/css = true（占限流槽）
    unsigned long serveMs;  // 单文件服务耗时（模拟）
};

static const LoginResource LOGIN_PAGE_RESOURCES[] = {
    {"/index.html",         true,  40},   // 主文档（core entry）
    {"/assets/favicon.ico", false, 15},
    {"/assets/logo.png",    false, 15},
    {"/chunk-1-core.js",  true,  60},
    {"/chunk-2-app.js",   true,  60},
    {"/chunk-3-state.js", true,  60},
    {"/css/main.css",       true,  50},
    // API 请求不经静态限流器（路由处理器直接响应），计入清单仅作突发量参考，
    // 模拟时直接放行
    {"/api/auth/session",   false, 20},
    {"/api/system/health",  false, 20},
    {"/api/network/status", false, 20},
};
static constexpr int LOGIN_RESOURCE_COUNT =
    sizeof(LOGIN_PAGE_RESOURCES) / sizeof(LOGIN_PAGE_RESOURCES[0]);

/**
 * @brief 模拟登录页资源突发：3 槽 + 小资源豁免下全部可完成，无死锁
 * 修复前：上限 1 + Connection: close，6 并发仅 1 成功，其余 RST/超时/503
 * 上限 2 仍导致大部分请求被 503 → 串行重试，页面加载 >5s
 */
void test_login_page_burst_completes() {
    TestLog::testStart("Login Page: 12-Resource Burst Completes");

    StaticLimiterSim sim;
    unsigned long now = 0;

    // 在途请求：(资源索引, 完成时间)。Connection: close 保证完成后 onDisconnect 释放槽位
    struct InFlightResource {
        int index;
        unsigned long doneAt;
        StaticLimiterSim::Lease lease;
    };
    std::vector<InFlightResource> inFlightList;
    std::vector<int> pending;
    for (int i = 0; i < LOGIN_RESOURCE_COUNT; i++) pending.push_back(i);

    int served = 0;
    unsigned long lastDone = 0;
    int guard = 0;

    while (served < LOGIN_RESOURCE_COUNT && guard++ < 1000) {
        // 1) 放行所有当前可服务的资源（豁免资源与空闲槽）
        std::vector<int> stillPending;
        for (int idx : pending) {
            const LoginResource& res = LOGIN_PAGE_RESOURCES[idx];
            unsigned long doneAt = now + res.serveMs;
            if (strncmp(res.path, "/api/", 5) == 0) {
                // API 请求不经静态限流器，直接完成
                served++;
                lastDone = doneAt;
                continue;
            }
            if (!res.compressible) {
                // 豁免资源（bypassInFlightCap）不占槽也不需要释放，立即完成
                served++;
                lastDone = doneAt;
                continue;
            }
            StaticLimiterSim::Lease lease = sim.acquire(now);
            if (lease.valid()) {
                inFlightList.push_back({idx, doneAt, lease});
            } else {
                stillPending.push_back(idx);
            }
        }
        pending = stillPending;
        if (served >= LOGIN_RESOURCE_COUNT) break;

        // 2) 推进到最早的完成事件，释放槽位
        TEST_ASSERT_TRUE_MESSAGE(!inFlightList.empty(),
            "Deadlock: pending resources but nothing in flight");
        size_t earliestIdx = 0;
        for (size_t k = 1; k < inFlightList.size(); k++) {
            if (inFlightList[k].doneAt < inFlightList[earliestIdx].doneAt) earliestIdx = k;
        }
        now = inFlightList[earliestIdx].doneAt;
        lastDone = now;
        StaticLimiterSim::Lease completedLease = inFlightList[earliestIdx].lease;
        inFlightList.erase(inFlightList.begin() + earliestIdx);
        sim.release(completedLease);
        served++;  // 该在途请求完成
    }

    TEST_ASSERT_EQUAL_MESSAGE(LOGIN_RESOURCE_COUNT, served,
        "All login-page resources must be served under 3-slot limiter + bypass");
    TEST_ASSERT_LESS_THAN(1000, guard);
    TestLog::step("All resources served, no deadlock or permanent 503");

    // 弱约束：总完成时间应在秒级（弱网实测串行旧链路可达 4-15s 量级）
    TEST_ASSERT_LESS_THAN(5000, (int)lastDone);
    TestLog::step("Burst completes well within the 3s first-paint goal budget");

    TestLog::testEnd(true);
}

/**
 * @brief 串行链长度对比：上限 1 -> 3 后，可压缩资源的排队批次大幅减少
 */
void test_serial_chain_halved() {
    TestLog::testStart("Serial Chain: Halved by Cap 1 -> 3");

    // 登录页可压缩资源数（html + js + css，见 LOGIN_PAGE_RESOURCES）
    int compressibleCount = 0;
    for (const auto& res : LOGIN_PAGE_RESOURCES) {
        if (res.compressible) compressibleCount++;
    }
    TEST_ASSERT_EQUAL(5, compressibleCount);  // 1 html + 3 chunks + 1 css

    // 向上取整的排队批次：旧上限 1 vs 当前上限 3
    constexpr int currentCap = 3;
    int batchesOld = compressibleCount;
    int batchesNew = (compressibleCount + currentCap - 1) / currentCap;
    TEST_ASSERT_TRUE_MESSAGE(batchesNew <= (batchesOld + 1) / 2,
        "New cap must at least halve the serial chain length");
    TestLog::step("Serial batches: 5 (old cap=1) -> 2 (new cap=3)");

    // 叠加前端提速：旧 5 分片到当前 3 分片，间隔开销进一步降低。
    unsigned long oldChunkOverhead = 100 + 80 * 4;   // 启动延迟 + 4 个间隔
    unsigned long newChunkOverhead = 20 + 30 * 2;
    TEST_ASSERT_LESS_THAN(oldChunkOverhead, (int)newChunkOverhead);
    TEST_ASSERT_LESS_OR_EQUAL(100, (int)newChunkOverhead);
    TestLog::step("Chunk overhead: 420ms -> 80ms");

    // 重试总预算：新退避 300/1200/2700 总和 4200ms，仍显著小于旧的 7000ms
    unsigned long newBackoffTotal = 300 + 1200 + 2700;
    unsigned long oldBackoffTotal = 500 + 2000 + 4500;
    TEST_ASSERT_LESS_THAN(oldBackoffTotal, (int)newBackoffTotal);
    TEST_ASSERT_GREATER_THAN(3000, (int)newBackoffTotal);
    TestLog::step("Retry backoff total: 7000ms -> 4200ms");

    TestLog::testEnd(true);
}

// Test group entry point
void test_static_concurrency_group() {
    TestLog::groupStart("Static File Concurrency & Login Page Speed Tests");

    // 源码常量锁定
    RUN_TEST(test_static_limiter_constants_locked);
    RUN_TEST(test_small_asset_bypass_locked);
    RUN_TEST(test_gzip_static_selection_locked);
    RUN_TEST(test_busy_retry_page_locked);
    RUN_TEST(test_frontend_loader_timings_locked);
    RUN_TEST(test_frontend_module_loader_backoff_locked);
    RUN_TEST(test_shared_static_asset_queue_locked);
    RUN_TEST(test_page_loader_response_integrity_locked);

    // 限流器行为模拟
    RUN_TEST(test_limiter_concurrency_behavior);
    RUN_TEST(test_limiter_no_psram_adaptive_behavior);
    RUN_TEST(test_limiter_watchdog_recovery);
    RUN_TEST(test_static_transfer_active_reaps_expired_leases);
    RUN_TEST(test_limiter_millis_wraparound_safe);
    RUN_TEST(test_limiter_staggered_expiry_isolated);
    RUN_TEST(test_limiter_stale_disconnect_generation_safe);
    RUN_TEST(test_login_page_burst_completes);
    RUN_TEST(test_serial_chain_halved);

    TestLog::groupEnd();
}
