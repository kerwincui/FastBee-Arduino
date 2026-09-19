#ifndef FASTBEE_MEMORY_BUDGET_H
#define FASTBEE_MEMORY_BUDGET_H

#include <stdint.h>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace FastBee {

enum class MemoryPressureLevel : uint8_t {
    NORMAL = 0,
    WARN = 1,
    SEVERE = 2,
    CRITICAL = 3
};

struct MemoryBudget {
    // Network stacks and mbedTLS need internal DRAM. PSRAM must not be counted
    // here. Observed ESP32-S3 WiFi and Ethernet states can attempt TLS below
    // the comfort zone, but Huawei Cloud's RSA path still needs contiguous
    // control/socket headroom. Below the ready zone, reclaim lightweight Web
    // resources before the MQTTS handshake.
    static constexpr uint32_t MQTTS_READY_DRAM_FREE = 56000U;
    static constexpr uint32_t MQTTS_READY_LARGEST_BLOCK = 43000U;
    static constexpr uint32_t MQTTS_WEB_PAUSE_DRAM_FREE = 50000U;
    static constexpr uint32_t MQTTS_WEB_PAUSE_LARGEST_BLOCK = 38000U;
    static constexpr uint32_t MQTTS_MIN_DRAM_FREE = 36000U;
    // Large mbedTLS/RSA buffers are routed to PSRAM at runtime. Internal DRAM
    // still needs a contiguous socket/control block, but Ethernet restore logs
    // show stable MQTTS retry windows around 17KB after Web/SSE reclaim.
    static constexpr uint32_t MQTTS_MIN_LARGEST_BLOCK = 16384U;
    // 无 PSRAM 构建：mbedTLS 的 16KB in + 16KB out + ~10KB context 全部落内部
    // DRAM（~42KB 峰值）。准入门槛必须覆盖峰值 + lwIP/AsyncTCP 小分配余量，
    // 否则握手中途把 largest block 打穿，lwIP 的 nothrow 小分配失败直接 panic
    // （tcp_receive assert）。宁可进 300s backoff 也不做边缘尝试。
    static constexpr uint32_t MQTTS_NO_PSRAM_MIN_DRAM_FREE = 52224U;
    // 单个 record buffer 含分配器 overhead：16384 + 1024
    static constexpr uint32_t MQTTS_NO_PSRAM_MIN_LARGEST_BLOCK = 17408U;
    // Keep the reconnect worker small: mbedTLS consumes heap buffers, and the
    // task stack itself comes from the same scarce internal DRAM pool.
    // 审计 P1-3：6144 为边缘值，TLS 握手栈峰值可能突破；保守提升至 8192，
    // 待实机 uxTaskGetStackHighWaterMark 实测后再收紧。
    static constexpr uint32_t MQTTS_RECONNECT_TASK_STACK = 8192U;

    static constexpr uint32_t MQTT_MIN_HEAP = 8000U;
    static constexpr uint32_t MQTT_MIN_LARGEST_BLOCK = 2048U;

    // MQTT 接收指令处理：DRAM 低于此值时丢弃入站消息，防止低内存时 JSON 解析崩溃
    static constexpr uint32_t MQTT_RECEIVE_MIN_DRAM = 12000U;

    // 外设执行内存门控：使用 DRAM（非总堆），避免 PSRAM 干扰判断
    static constexpr uint32_t PERIPH_EXEC_MIN_DRAM = 15000U;
    static constexpr uint32_t PERIPH_EXEC_MIN_LARGEST = 4096U;
    // Modbus 轮询专用：DRAM 低于此值时跳过轮询
    static constexpr uint32_t MODBUS_POLL_MIN_DRAM = 12000U;
    // Modbus 配置重建会解析 JSON 并创建 UART/轮询对象，使用独立的
    // 最低准入线，避免在通用协议堆闸门早退时丢失延迟恢复请求。
    static constexpr uint32_t MODBUS_RESTART_MIN_DRAM = 15000U;
    static constexpr uint32_t MODBUS_RESTART_MIN_LARGEST = 4096U;

    // 运行期小分配预检（审计 P2-7）：BLINK/BREATHE ticker 分配前水位检查，
    // 低内存时跳过单次动作而非抛出型 new OOM → abort 整机重启
    static constexpr uint32_t ACTION_TICKER_MIN_DRAM = 4096U;
    // 4G 适配器 begin() 创建 TinyGSM 对象族前水位检查（断线重建路径可反复触发）
    static constexpr uint32_t CELLULAR_BEGIN_MIN_DRAM = 8192U;

    // TCP 收包门控（审计 P1-1）：对齐 MQTT_RECEIVE_MIN_DRAM，低内存丢弃入站；
    // 单轮读取上限防止无界 String 累积把 DRAM 压到 lwIP 饿死水位
    static constexpr uint32_t TCP_RX_MIN_DRAM = 12000U;
    static constexpr uint32_t TCP_RX_MAX_FRAME = 2048U;

    // 文件读取闸门（审计 P2-1）：工具层 readFile 入口拒绝超限文件
    // （日志/膨胀配置），防止大文件 readString 把 DRAM 吃穿
    static constexpr uint32_t FILE_READ_MAX_SIZE = 16384U;

    static constexpr uint32_t GUARD_WARN_DRAM_FREE = 30720U;
    static constexpr uint32_t GUARD_SEVERE_DRAM_FREE = 24576U;
    static constexpr uint32_t GUARD_CRITICAL_DRAM_FREE = 16384U;
    // Runtime evidence across classic ESP32, C3, C6 and S3 shows a stable
    // internal 8-bit largest block around 10KB while Web, Modbus and
    // PeriphExec remain serviceable.  Their hard allocation floors are
    // 2.9-4KB, so larger guard bands create false SEVERE transitions during
    // a normal login and leave optional services suspended.  Keep an 8KB
    // early warning, but reserve SEVERE/CRITICAL for blocks that are actually
    // too small for those services.
    static constexpr uint32_t GUARD_WARN_LARGEST_BLOCK = 8192U;
    static constexpr uint32_t GUARD_SEVERE_LARGEST_BLOCK = 4096U;
    static constexpr uint32_t GUARD_CRITICAL_LARGEST_BLOCK = 3072U;

    static constexpr uint8_t FRAG_WARN_PERCENT = 65U;
    static constexpr uint8_t FRAG_SEVERE_PERCENT = 80U;
    static constexpr uint8_t FRAG_CRITICAL_PERCENT = 85U;

    static constexpr unsigned long MQTTS_WEB_PAUSE_HOLD_MS = 45000UL;
    static constexpr unsigned long MQTTS_WEB_RESUME_DELAY_MS = 300UL;
    static constexpr unsigned long MQTTS_MEMORY_RECOVERY_RETRY_MS = 10000UL;

    static constexpr bool canAttemptMqtts(uint32_t dramFree, uint32_t largestBlock) {
        return dramFree >= MQTTS_MIN_DRAM_FREE &&
               largestBlock >= MQTTS_MIN_LARGEST_BLOCK;
    }

    // PSRAM 感知重载：psramAvailable=false 时使用无 PSRAM 严格门槛，
    // 避免在 DRAM 必然耗尽的窗口内发起 TLS 握手
    static constexpr bool canAttemptMqtts(uint32_t dramFree, uint32_t largestBlock,
                                          bool psramAvailable) {
        return psramAvailable
            ? canAttemptMqtts(dramFree, largestBlock)
            : (dramFree >= MQTTS_NO_PSRAM_MIN_DRAM_FREE &&
               largestBlock >= MQTTS_NO_PSRAM_MIN_LARGEST_BLOCK);
    }

    static constexpr bool canRetryMqttsMemoryRecovery(uint32_t dramFree,
                                                       uint32_t largestBlock) {
        return dramFree >= MQTTS_MIN_DRAM_FREE &&
               largestBlock >= GUARD_WARN_LARGEST_BLOCK;
    }

    // 无 PSRAM 时：离严格门槛太远就不要 10s 高频重试（反复 pause/resume Web
    // 只会把堆越切越碎），直接交给 300s backoff
    static constexpr bool canRetryMqttsMemoryRecovery(uint32_t dramFree,
                                                       uint32_t largestBlock,
                                                       bool psramAvailable) {
        return psramAvailable
            ? canRetryMqttsMemoryRecovery(dramFree, largestBlock)
            : (dramFree >= MQTTS_NO_PSRAM_MIN_DRAM_FREE - 8192U &&
               largestBlock >= GUARD_WARN_LARGEST_BLOCK);
    }

    static constexpr bool shouldReclaimBeforeMqtts(uint32_t dramFree, uint32_t largestBlock) {
        return dramFree < MQTTS_READY_DRAM_FREE ||
               largestBlock < MQTTS_READY_LARGEST_BLOCK;
    }

    static constexpr bool shouldPauseWebBeforeMqtts(uint32_t dramFree, uint32_t largestBlock) {
        return dramFree < MQTTS_WEB_PAUSE_DRAM_FREE ||
               largestBlock < MQTTS_WEB_PAUSE_LARGEST_BLOCK;
    }

    // Runtime profiles may need different guard bands because the same
    // feature bundle leaves very different steady-state DRAM on each chip.
    // Keep the legacy two-argument overload below unchanged for callers that
    // rely on the project-wide defaults.
    static constexpr MemoryPressureLevel guardLevelForFreeDram(
        uint32_t dramFree,
        uint32_t criticalDramFree,
        uint32_t severeDramFree,
        uint32_t warnDramFree) {
        if (dramFree < criticalDramFree) {
            return MemoryPressureLevel::CRITICAL;
        }
        if (dramFree < severeDramFree) {
            return MemoryPressureLevel::SEVERE;
        }
        if (dramFree < warnDramFree) {
            return MemoryPressureLevel::WARN;
        }
        return MemoryPressureLevel::NORMAL;
    }

    static constexpr MemoryPressureLevel guardLevelForFreeDram(
        uint32_t dramFree,
        uint32_t criticalDramFree = GUARD_CRITICAL_DRAM_FREE) {
        return guardLevelForFreeDram(dramFree, criticalDramFree,
                                     GUARD_SEVERE_DRAM_FREE,
                                     GUARD_WARN_DRAM_FREE);
    }

    static constexpr MemoryPressureLevel guardLevelForDram(uint32_t dramFree,
                                                           uint32_t largestBlock,
                                                           uint8_t fragmentation,
                                                           bool startupGrace) {
        MemoryPressureLevel level = guardLevelForFreeDram(dramFree);

        if (!startupGrace) {
            if (largestBlock < GUARD_CRITICAL_LARGEST_BLOCK ||
                (fragmentation >= FRAG_CRITICAL_PERCENT &&
                 largestBlock < GUARD_CRITICAL_LARGEST_BLOCK)) {
                return MemoryPressureLevel::CRITICAL;
            }
            if (largestBlock < GUARD_SEVERE_LARGEST_BLOCK ||
                (fragmentation >= FRAG_SEVERE_PERCENT &&
                 largestBlock < GUARD_SEVERE_LARGEST_BLOCK)) {
                if (level < MemoryPressureLevel::SEVERE) {
                    level = MemoryPressureLevel::SEVERE;
                }
            } else if (largestBlock < GUARD_WARN_LARGEST_BLOCK ||
                       fragmentation >= FRAG_WARN_PERCENT) {
                if (level < MemoryPressureLevel::WARN) {
                    level = MemoryPressureLevel::WARN;
                }
            }
        }

        return level;
    }

    static constexpr bool isSevereWebPressure(uint32_t dramFree,
                                              uint32_t largestBlock,
                                              uint8_t fragmentation) {
        return dramFree < GUARD_SEVERE_DRAM_FREE ||
               largestBlock < GUARD_SEVERE_LARGEST_BLOCK ||
               (fragmentation >= FRAG_WARN_PERCENT &&
                largestBlock < GUARD_WARN_LARGEST_BLOCK);
    }

    // ── 8BIT 口径 Web 压力检测 ───────────────────────────────────────────
    // INTERNAL 口径含 IRAM/RTC 不可 malloc 内存，实测比 8BIT 虚高 ~28KB，
    // 导致 isSevereWebPressure() 在 8BIT DRAM 已耗尽时仍报"健康"，
    // WebConfigManager 的自动恢复（软重启 / 设备重启）永远不触发。
    // 此函数使用 MALLOC_CAP_8BIT 校准的阈值，确保真实 DRAM 压力被检测到。
    // 阈值校准依据（ESP32-F4R0 实测）：
    //   开机 free8≈26KB/blk8≈13.5KB → 稳态 MQTT 连接后 free8≈14-18KB
    //   free8<8KB → 响应发送降级到 256B 缓冲仍可能失败
    //   blk8<3KB → AsyncTCP 2872B 主缓冲分配失败，进入降级 fallback
    static constexpr uint32_t GUARD8_SEVERE_DRAM_FREE     = 8192U;   // 8KB
    static constexpr uint32_t GUARD8_SEVERE_LARGEST_BLOCK = 3072U;   // 3KB
    static constexpr uint32_t GUARD8_WARN_LARGEST_BLOCK   = 8192U;   // 8KB

    static constexpr bool isSevereWebPressure8(uint32_t free8,
                                               uint32_t blk8,
                                               uint8_t fragmentation) {
        return free8 < GUARD8_SEVERE_DRAM_FREE ||
               blk8 < GUARD8_SEVERE_LARGEST_BLOCK ||
               (fragmentation >= FRAG_WARN_PERCENT &&
                blk8 < GUARD8_WARN_LARGEST_BLOCK);
    }

    // ── Web 内存护栏（无 PSRAM 机型，见 network/WebMemoryGuard.h）──
    // 阈值校准（2026-08-14 smoke 套件实测，esp32-F4R0 lite）：
    //   浏览器 API 突发时 free8 瞬态跌至 13~23KB，1-2 秒内恢复 35KB+；
    //   旧 24KB 触发把瞬态凹陷全部拦成 503（护栏 ACTIVE/RECOVERED 每 ~10s
    //   振荡，rejected 持续增长），套件大面积误伤。真正危险区是 free8<8KB
    //   （响应降级 256B）与 blk8<3KB（AsyncTCP 2872B 主缓冲失败），
    //   故触发下调至 16KB、恢复 16+4KB 迟滞，保留安全边际且消除瞬态误拦。
    // blk8 维度补充（2026-08-14 C3 full 镜像 soak 实测）：free8 尚余 22KB
    //   时 blk8 已碎至 9.7KB，多 KB 级 JSON 响应分配挂起引发接口间歇超时，
    //   单看 free8 会漏掉碎片化压力；触发 5KB（AsyncTCP 主缓冲 2872B +
    //   响应构建余量）、恢复 8KB 迟滞。
    // 提前背压（2026-08-19 classic ESP32 lite-soak 实测，缺陷 5.1 修复）：
    //   持续轮询下 free8 在约 5 分钟内从 ~30KB 缓降至 11~18KB，随后 lwIP
    //   饿死、HTTP 全挂 1~3 分钟（code=000，TCP 层失败，中间件无机会执行）。
    //   16KB 触发阈值落在停摆区间之内，为时已晚。上调至 20KB/blk8 4KB，
    //   在缓降过程中提前 503 背压，阻止连接积压进入死区；代价是突发负载
    //   的瞬态凹陷（13~23KB）会有少量 503，属可控降级，优于整体停摆。
    static constexpr uint32_t WEB_GUARD_MIN_DRAM_FREE = 20480U;      // 20KB 触发阈值（free8 口径）
    // 经典 ESP32 的完整外设配置在无请求稳态通常只有约 21~23KB free8。
    // 4KB 迟滞（24KB）会让护栏在设备已经稳定时永久保持 ACTIVE；保留
    // 1KB 迟滞，仍可过滤瞬态回升，同时允许低内存设备自愈恢复 API。
    static constexpr uint32_t WEB_GUARD_RECOVERY_DRAM_FREE =
        WEB_GUARD_MIN_DRAM_FREE + 1024U; // 20KB + 1KB 迟滞恢复阈值
    // 2026-08-24 .34 取证：重负载后 blk8 稳态碎在 ~5108B（free8 已恢复
    // 31KB），6144 触发阈值落在"陷阱带"内 → 护栏激活后永远达不到
    // 9216 恢复阈值，FORCE-RECOVER 后 5s 内又立即重新激活，ping-pong
    // 死环导致 API 永久 503（最终靠 MEMGUARD 600s 看门狗重启才解套）。
    // 下调至 4096：稳态碎片 5108 不再触发；真正压力期 blk8 实测 28~3400，
    // 保护力不受影响。响应构建硬保护另有入口临界闸门 blk8<1536 全丢弃。
    static constexpr uint32_t WEB_GUARD_MIN_DRAM_BLK8 = 4096U;       // 4KB 触发阈值（blk8 口径）
    static constexpr uint32_t WEB_GUARD_RECOVERY_DRAM_BLK8 = 9216U;  // 4KB + 5KB 迟滞恢复阈值
    // Bounded HTTP responses now stage at most one TCP MSS under low DRAM;
    // large JSON is streamed instead of requiring a contiguous serialized body.
    // Keep the 4KB response admission requirement plus 2KB block hysteresis.
    // C3 post-load evidence: 28KB free / 7668B largest is usable but can never
    // reach the legacy 9KB fast recovery threshold. The guard additionally
    // requires this headroom to remain stable for five seconds before recovery.
    static constexpr uint32_t WEB_STREAMING_RECOVERY_DRAM_BLK8 =
        WEB_GUARD_MIN_DRAM_BLK8 + 2048U;
    static constexpr bool hasWebStreamingHeadroom(uint32_t free8, uint32_t blk8) {
        return free8 >= WEB_GUARD_RECOVERY_DRAM_FREE &&
               blk8 >= WEB_STREAMING_RECOVERY_DRAM_BLK8;
    }

    // DRAM 水位辅助（审计 P2-4）：堆闸门必须统一用内部 DRAM 口径，
    // 而非 ESP.getFreeHeap()/getMaxAllocHeap() 总堆（PSRAM 板型上
    // PSRAM 计入导致闸门永远放行，DRAM 枯竭时 VFS/lwIP 分配直接 abort）。
    // native 测试环境无 esp_heap_caps，返回 0（测试仅锁定常量不调用）
    static uint32_t dramFree() {
#if defined(ESP_PLATFORM)
        return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#else
        return 0;
#endif
    }

    static uint32_t dramLargest() {
#if defined(ESP_PLATFORM)
        return static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#else
        return 0;
#endif
    }

    // 8-bit internal DRAM is the pool required by WiFi/lwIP/FreeRTOS.  A
    // bare MALLOC_CAP_8BIT query also includes PSRAM after extmem is enabled,
    // which can hide an exhausted internal pool on PSRAM boards.
    static uint32_t dram8Free() {
#if defined(ESP_PLATFORM)
        return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#else
        return 0;
#endif
    }

    static uint32_t dram8Largest() {
#if defined(ESP_PLATFORM)
        return static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#else
        return 0;
#endif
    }
};

}  // namespace FastBee

#endif  // FASTBEE_MEMORY_BUDGET_H
