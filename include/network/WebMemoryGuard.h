/**
 * @file WebMemoryGuard.h
 * @brief Web 内存护栏中间件（无 PSRAM 机型：classic ESP32 / C3 / C6）
 *
 * 背景：无 PSRAM 设备（DRAM ~320KB / C3 ~400KB / C6 ~500KB）运行
 * unified 全功能固件时，重 Web 负载下 free8/blk8 水位紧张，
 * ESPAsyncWebServer/asynctcp 分配失败后请求挂起而非报错，行为不可控。
 *
 * 护栏策略（作为 server 级 middleware 位于请求分发最前端，每请求一次
 * heap_caps_get_free_size(MALLOC_CAP_8BIT) 检查，开销可接受）：
 *   - 触发：free8 < WEB_GUARD_MIN_DRAM_FREE(20KB)，或最大可分配块
 *     blk8 < WEB_GUARD_MIN_DRAM_BLK8(4KB，碎片化维度：C3 实测 free8 尚余
 *     22KB 时 blk8 已碎至 9.7KB 导致响应挂起) 时激活。
 *     2026-08-19 lite-soak 实测持续轮询下 free8 缓降至 11~18KB 后 lwIP
 *     饿死整体停摆，旧 16KB 阈值落在停摆区内，故上调提前背压；
 *     新 API/动态请求（/api/*）直接返回 503 + JSON（Retry-After: 5），
 *     不进入 handler 与响应构建分配；已连接的 SSE 客户端主动优雅断开，
 *     释放 lwIP/TCP 缓冲，给系统恢复窗口；
 *   - 放行：静态文件/SPA 页面（Flash/LittleFS 直读流式服务，几乎不占
 *     DRAM）不受护栏影响，保证登录页等基础资源始终可访问；
 *   - 恢复：free8 回到 WEB_GUARD_RECOVERY_DRAM_FREE(20KB+1KB 迟滞)
 *     且 blk8 >= WEB_GUARD_RECOVERY_DRAM_BLK8(4KB+5KB 迟滞)
 *     后自动解除，避免触发/恢复来回震荡。
 *
 * 编译开关 FASTBEE_WEB_MEM_GUARD：默认对无 PSRAM 的 classic ESP32 /
 * C3 / C6 生效（CONFIG_IDF_TARGET_* + FASTBEE_USE_PSRAM==0）；S3-F8R8/
 * F16R8 等有 PSRAM 的环境不受影响。可在 platformio.ini 用
 * -DFASTBEE_WEB_MEM_GUARD=1/0 强制覆盖。
 */

#ifndef WEB_MEMORY_GUARD_H
#define WEB_MEMORY_GUARD_H

#include "core/FeatureFlags.h"
#include "core/MemoryBudget.h"

#ifndef FASTBEE_WEB_MEM_GUARD
// 2026-08-14：护栏覆盖扩大到 C3/C6 无 PSRAM 机型——full 档 Web 镜像
// 统一化后实测 C3 heap 占用 84%、blk8 碎片化至 9.7KB 引发接口间歇挂起，
// 与 classic ESP32 同属"无 PSRAM + 重 Web 负载"风险面
#if (defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32C3) || \
     defined(CONFIG_IDF_TARGET_ESP32C6)) && \
    (FASTBEE_USE_PSRAM == 0)
#define FASTBEE_WEB_MEM_GUARD 1
#else
#define FASTBEE_WEB_MEM_GUARD 0
#endif
#endif

#if FASTBEE_WEB_MEM_GUARD

#include <ESPAsyncWebServer.h>

class SSERouteHandler;

/**
 * @brief Web 内存护栏中间件
 *
 * 由 WebConfigManager 持有并在 initialize() 中最先 addMiddleware，
 * 保证早于所有路由 handler 与 SSE(AsyncEventSource) 执行。
 */
class WebMemoryGuardMiddleware : public AsyncMiddleware {
public:
    /// 设置 SSE 处理器（护栏触发时主动断开所有 SSE 客户端）
    void setSseHandler(SSERouteHandler* sse) { sse_ = sse; }

    /// 护栏当前是否激活（供状态查询/诊断）
    bool isActive() const { return active_; }
    /// 累计被拒绝的请求数
    uint32_t getRejectedCount() const { return rejectedCount_; }

    void run(AsyncWebServerRequest* request, ArMiddlewareNext next) override;

    /// 周期性维护（请求无关）：评估激活/恢复/超时兜底状态机。
    /// 2026-08-24 OOM 死锁取证：旧实现只在请求到达时评估，客户端被 503
    /// 击退停止轮询后中间件不再运行，FORCE-RECOVER 永不触发，护栏永久
    /// 锁死 → API 持续不可用。由 HealthMonitor 周期任务调用本方法，
    /// 保证无请求时状态机仍持续推进。
    void maintain();

private:
    static constexpr unsigned long LOG_THROTTLE_MS = 10000UL;  // 拒绝日志限频间隔
    // 激活滞留兜底：护栏依赖 blk8 迟滞恢复，但无 PSRAM 机型在探测流量持续
    // 扰动下 blk8 会稳态碎在触发/恢复阈值之间（.34 实测稳态 7156B < 恢复
    // 阈值 9216B），护栏永久无法恢复 → 持续 503 → 客户端继续重试 → lwIP
    // 持续翻搅，形成自锁死环。free8 已真正恢复（>= 迟滞阈值）且激活超时
    // 时强制放行，打断死环（若真恶化会在下一请求按触发条件重新激活）。
    static constexpr unsigned long MAX_ACTIVE_MS = 180000UL;
    // FORCE-RECOVER 冷却期：强制放行后的窗口内，blk8 维度不再触发激活
    // （free8 恶化仍可激活）。2026-08-24 .34 取证：无冷却时 FORCE-RECOVER
    // 后下一次 maintain()（≤ 5s）因 blk8 仍在陷阱带立即重新激活并重置
    // activeSinceMs_，180s 超时永远攒不够，形成 ping-pong 自锁死环，
    // API 永久 503，最终只能靠 MEMGUARD 600s CRITICAL 看门狗重启解套。
    // 冷却期内硬保护仍在：入口临界闸门（blk8<1536 全丢弃）、tcp_accept
    // 三重门控、_onData 早断均与护栏状态无关。
    static constexpr unsigned long FORCE_RECOVER_COOLDOWN_MS = 300000UL;
    // 激活去抖动（2026-08-24 修复L，菜单报错取证）：浏览器正常浏览时
    // API 突发会把 free8 瞬态拉到 13~23KB（1-2s 内恢复 35KB+），旧实现
    // 单样本立即激活 → 瞬态凹陷全部拦成 503（日志实证 reject
    // /api/features/status，ACTIVE/RECOVERED 每几秒振荡）→ 用户看到菜单
    // 报错。要求低水位持续 ACTIVATE_DELAY_MS 才激活；真正抽干（第 10 轮
    // 负载 40s 级）不受影响，且入口临界闸门/tcp_accept 门控/_onData 早断
    // 等硬保护与激活状态无关。恢复方向仍单样本立即生效。
    // no-PSRAM C3 在多个静态资源/登录请求同时到达时，2s 内就可能从
    // 50KB 级水位跌入碎片化区；缩短去抖窗口优先保证不触发 throwing new。
    // 正常的 1~2s 瞬态仍由恢复迟滞和前端退避吸收。
    static constexpr unsigned long ACTIVATE_DELAY_MS = 500UL;

    bool active_ = false;
    uint32_t rejectedCount_ = 0;
    unsigned long lastLogMs_ = 0;
    unsigned long activeSinceMs_ = 0;
    unsigned long forceRecoverMs_ = 0;  // 0 = 无冷却；否则为最近一次 FORCE-RECOVER 时刻
    unsigned long lowWaterSinceMs_ = 0;  // 0 = 当前不在低水位；否则为首次采样到低水位的时刻
    SSERouteHandler* sse_ = nullptr;
};

#endif  // FASTBEE_WEB_MEM_GUARD

#endif  // WEB_MEMORY_GUARD_H
