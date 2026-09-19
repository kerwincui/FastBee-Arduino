#include "./network/WebMemoryGuard.h"

#if FASTBEE_WEB_MEM_GUARD

#include "./network/handlers/SSERouteHandler.h"
#include "systems/LoggerSystem.h"

// ROM 日志函数：不走 newlib / 不加锁 / 不分配内存，临界水位下唯一安全的
// 输出通道（Serial.printf/LOG_* 会堆分配并解引用堆字符串，实测 panic）
extern "C" int ets_printf(const char* fmt, ...);

using FastBee::MemoryBudget;

// 503 响应体：常量字符串 + AsyncProgmemResponse 直读，不复制进 DRAM String，
// 低内存下也能稳定发出（仅 addHeader 有数百字节级不可避免的小分配）
static const char WEB_GUARD_503_BODY[] PROGMEM =
    "{\"success\":false,\"code\":503,\"message\":\"Server low memory, request rejected\",\"retryAfter\":5}";

// 拒绝路径临界水位：503 构建本身仍有 throwing 分配（beginResponse +
// addHeader 各走 operator new），2026-08-24 backtrace 实锤 free8 耗尽时
// addHeader -> operator new(40) 失败直接 abort -> panic 重启环。
// 低于该水位时放弃自建响应：不调 next()、不 close（本函数运行在 async_tcp
// 任务的 _recv 回调链内，同步 client()->close() 会销毁 AsyncClient 而请求链
// 仍在引用它，实测直接触发 StoreProhibited panic，三次复现）。
// 实际兜底（2026-08-24 取证更正）：middleware 返回后库仍会执行 _send()，
// _response 未设置时发 501 "Handler did not handle the request"（非静默
// 挂起）。闸门已保证 blk8>=1536，该小响应（百字节级）分配安全，实测
// 501 正常送达，前端拿到可重试状态码后自动重试，行为可控。
//
// 阈值校准（2026-08-24 第二次取证）：静态文件在 free8<6144 时已被
// serveStaticFile 门控拒绝，但拒绝后的链路（next() 的 std::function 拷贝、
// 503 构建）仍需小分配；实测 free8=480/blk8=28 时 next() 拷贝 32B 即 abort。
// 阈值与静态门控对齐：free8<6144 或 blk8<1536 时连 next() 都不进入。
static constexpr uint32_t REJECT_MIN_FREE8 = 6144;
static constexpr uint32_t REJECT_MIN_BLK8 = 1536;

void WebMemoryGuardMiddleware::maintain() {
    // 每调用一次 8BIT 口径 DRAM 水位检查（真实可 malloc 内存，malloc/lwIP pbuf 同口径）；
    // blk8（最大可分配块）捕捉碎片化压力：free8 尚充裕但大块耗尽时
    // 多 KB 级响应构建会挂起（C3 full 镜像 soak 实测）
    const uint32_t free8 = MemoryBudget::dram8Free();
    const uint32_t blk8 = MemoryBudget::dram8Largest();
    if (active_ && MemoryBudget::hasWebStreamingHeadroom(free8, blk8)) {
        if (streamingRecoverySinceMs_ == 0) {
            streamingRecoverySinceMs_ = millis() ? millis() : 1;
        }
    } else {
        streamingRecoverySinceMs_ = 0;
    }

    // 状态迁移日志用 Serial.printf 而非 LOG_WARNINGF：MEMGUARD CRITICAL 降级
    // 会抬高日志等级，FORCE-RECOVER/re-ACTIVE 迁移被吞导致死环不可见
    // （2026-08-24 .34 取证：迁移分支条件均保证 free8>=11KB，printf 安全）

    // ── 状态机：带迟滞的触发 / 恢复（free8 与 blk8 双维度）──
    if (!active_) {
        // FORCE-RECOVER 冷却期内忽略 blk8 维度（陷阱带碎片只有重启能解，
        // 反复激活只会 ping-pong 自锁）；free8 真恶化仍可激活
        const bool inCooldown = forceRecoverMs_ != 0 &&
                                (millis() - forceRecoverMs_ < FORCE_RECOVER_COOLDOWN_MS);
        if (free8 < MemoryBudget::WEB_GUARD_MIN_DRAM_FREE ||
            (blk8 < MemoryBudget::WEB_GUARD_MIN_DRAM_BLK8 && !inCooldown)) {
            // 修复L：激活去抖动——低水位需持续 ACTIVATE_DELAY_MS 才激活，
            // 浏览瞬态凹陷（1-2s 内恢复）不再误伤菜单 API
            if (lowWaterSinceMs_ == 0) {
                lowWaterSinceMs_ = millis() ? millis() : 1;  // 避免与 0（无低水位）哨兵冲突
            } else if (millis() - lowWaterSinceMs_ >= ACTIVATE_DELAY_MS) {
                lowWaterSinceMs_ = 0;
                active_ = true;
                activeSinceMs_ = millis();
                Serial.printf("[WebGuard] ACTIVE: free8=%u blk8=%u (min %u/%u), rejecting API requests\n",
                              (unsigned)free8, (unsigned)blk8,
                              (unsigned)MemoryBudget::WEB_GUARD_MIN_DRAM_FREE,
                              (unsigned)MemoryBudget::WEB_GUARD_MIN_DRAM_BLK8);
                // 主动断开已连接的 SSE 客户端（优雅关闭），释放 lwIP/TCP 缓冲，
                // 浏览器 EventSource 按指数退避重连，恢复后自动重连成功
                if (sse_) {
                    sse_->closeAllClients();
                }
            }
        } else {
            lowWaterSinceMs_ = 0;  // 水位回升，去抖动计时清零
        }
    } else if ((free8 >= MemoryBudget::WEB_GUARD_RECOVERY_DRAM_FREE &&
                blk8 >= MemoryBudget::WEB_GUARD_RECOVERY_DRAM_BLK8) ||
               (streamingRecoverySinceMs_ != 0 &&
                millis() - streamingRecoverySinceMs_ >= STREAMING_RECOVERY_DELAY_MS)) {
        active_ = false;
        forceRecoverMs_ = 0;  // 真正恢复，解除冷却
        streamingRecoverySinceMs_ = 0;
        Serial.printf("[WebGuard] RECOVERED: free8=%u blk8=%u, fast-block=%u stable-block=%u, resuming API requests (rejected=%u)\n",
                      (unsigned)free8, (unsigned)blk8,
                      (unsigned)MemoryBudget::WEB_GUARD_RECOVERY_DRAM_BLK8,
                      (unsigned)MemoryBudget::WEB_STREAMING_RECOVERY_DRAM_BLK8,
                      (unsigned)rejectedCount_);
    } else if (free8 >= MemoryBudget::WEB_GUARD_RECOVERY_DRAM_FREE &&
               millis() - activeSinceMs_ >= MAX_ACTIVE_MS) {
        // 兜底强制放行：free8 已回到迟滞阈值之上，但 blk8 被探测流量稳态
        // 碎在触发/恢复阈值之间（永久达不到 blk8 恢复条件）。继续 503 只会
        // 换来更多重试进一步翻搅 lwIP，死环无法自解。此处打断死环；
        // 若内存真恶化，free8 维度仍会重新激活（blk8 维度进冷却期）。
        active_ = false;
        forceRecoverMs_ = millis();
        Serial.printf("[WebGuard] FORCE-RECOVER after %lus stuck: free8=%u ok but blk8=%u < %u, resuming (cooldown %lus, rejected=%u)\n",
                      (unsigned long)((millis() - activeSinceMs_) / 1000),
                      (unsigned)free8, (unsigned)blk8,
                      (unsigned)MemoryBudget::WEB_GUARD_RECOVERY_DRAM_BLK8,
                      (unsigned long)(FORCE_RECOVER_COOLDOWN_MS / 1000),
                      (unsigned)rejectedCount_);
    }
}

void WebMemoryGuardMiddleware::run(AsyncWebServerRequest* request, ArMiddlewareNext next) {
    // 状态机推进（与请求到达解耦；HealthMonitor 也会周期调用 maintain()）
    maintain();

    // ── 入口临界水位闸门（对所有 URL，含静态放行路径）──
    // 任何 next() 都会执行中间件链下一跳：_runChain 的 lambda 按值捕获
    // finalizer(std::function)，每跳拷贝触发 32B throwing 分配。backtrace
    // 实锤（2026-08-24 free8=348 blk8=12）：护栏放行后下一跳 std::function
    // 拷贝 -> operator new(32) FATAL abort。低于水位时不调 next()、不自建
    // 响应、不 close（_recv 链内同步 close 会 panic）；middleware 返回后
    // 库 _send() 兜底发 501（blk8>=1536 保证小分配安全），前端重试。
    const uint32_t free8 = MemoryBudget::dram8Free();
    const uint32_t blk8 = MemoryBudget::dram8Largest();
    if (free8 < REJECT_MIN_FREE8 || blk8 < REJECT_MIN_BLK8) {
        rejectedCount_++;
        // 限频 + ets_printf 纯数字：临界水位下 LOG_WARNINGF 自身堆分配亦不安全
        const unsigned long now = millis();
        if (now - lastLogMs_ >= LOG_THROTTLE_MS) {
            lastLogMs_ = now;
            ets_printf("[WebGuard] critical water level (free8=%lu blk8=%lu), dropping request (lib 501 fallback)\n",
                       (unsigned long)free8, (unsigned long)blk8);
        }
        return;
    }

    if (!active_ || !request) {
        return next();
    }

    // 静态文件/SPA 页面（Flash/LittleFS 直读，几乎不占 DRAM）放行，
    // 仅拦截 API/动态请求（含 SSE /api/events 的新连接）
    const String& url = request->url();
    if (!url.startsWith("/api/")) {
        return next();
    }

    rejectedCount_++;
    // 限频日志：浏览器/前端高频重试下避免刷屏（入口闸门已保证水位 >=
    // REJECT_MIN，此处常规日志安全）
    const unsigned long now = millis();
    if (now - lastLogMs_ >= LOG_THROTTLE_MS) {
        lastLogMs_ = now;
        LOG_WARNINGF("[WebGuard] reject %s %s (free8=%u blk8=%u, rejected=%u)",
                     request->methodToString(), url.c_str(),
                     (unsigned)free8, (unsigned)blk8, (unsigned)rejectedCount_);
    }

    // 503 + JSON + Retry-After: 5，Progmem 响应不进入响应构建 DRAM 分配
    // （入口闸门已保证水位足以承担 beginResponse + addHeader 的小分配）
    AsyncWebServerResponse* response = request->beginResponse(
        503, "application/json",
        reinterpret_cast<const uint8_t*>(WEB_GUARD_503_BODY),
        sizeof(WEB_GUARD_503_BODY) - 1);
    response->addHeader("Retry-After", "5");
    response->addHeader("Connection", "close");
    request->send(response);
    // 不调用 next()：请求不进入 handler / JSON 构建 / 响应序列化
}

#endif  // FASTBEE_WEB_MEM_GUARD
