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
    static constexpr uint32_t GUARD_WARN_LARGEST_BLOCK = 16384U;
    static constexpr uint32_t GUARD_SEVERE_LARGEST_BLOCK = 12288U;
    static constexpr uint32_t GUARD_CRITICAL_LARGEST_BLOCK = 8192U;

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

    static constexpr MemoryPressureLevel guardLevelForDram(uint32_t dramFree,
                                                           uint32_t largestBlock,
                                                           uint8_t fragmentation,
                                                           bool startupGrace) {
        MemoryPressureLevel level = MemoryPressureLevel::NORMAL;

        if (dramFree < GUARD_CRITICAL_DRAM_FREE) {
            level = MemoryPressureLevel::CRITICAL;
        } else if (dramFree < GUARD_SEVERE_DRAM_FREE) {
            level = MemoryPressureLevel::SEVERE;
        } else if (dramFree < GUARD_WARN_DRAM_FREE) {
            level = MemoryPressureLevel::WARN;
        }

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

    // 2026-08-05：8BIT 口径真实可 malloc DRAM。INTERNAL 含 IRAM/RTC 不可
    // malloc 内存，实测比 8BIT 虚高 ~28KB；malloc/new/lwIP pbuf 都从
    // 8BIT DRAM 分配，低内存门控必须用它否则守卫被误导（OOM abort 教训）。
    static uint32_t dram8Free() {
#if defined(ESP_PLATFORM)
        return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_8BIT));
#else
        return 0;
#endif
    }

    static uint32_t dram8Largest() {
#if defined(ESP_PLATFORM)
        return static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#else
        return 0;
#endif
    }
};

}  // namespace FastBee

#endif  // FASTBEE_MEMORY_BUDGET_H
