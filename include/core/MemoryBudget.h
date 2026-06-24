#ifndef FASTBEE_MEMORY_BUDGET_H
#define FASTBEE_MEMORY_BUDGET_H

#include <stdint.h>

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
    // Keep the reconnect worker small: mbedTLS consumes heap buffers, and the
    // task stack itself comes from the same scarce internal DRAM pool.
    static constexpr uint32_t MQTTS_RECONNECT_TASK_STACK = 6144U;

    static constexpr uint32_t MQTT_MIN_HEAP = 8000U;
    static constexpr uint32_t MQTT_MIN_LARGEST_BLOCK = 2048U;

    // MQTT 接收指令处理：DRAM 低于此值时丢弃入站消息，防止低内存时 JSON 解析崩溃
    static constexpr uint32_t MQTT_RECEIVE_MIN_DRAM = 12000U;

    // 外设执行内存门控：使用 DRAM（非总堆），避免 PSRAM 干扰判断
    static constexpr uint32_t PERIPH_EXEC_MIN_DRAM = 15000U;
    static constexpr uint32_t PERIPH_EXEC_MIN_LARGEST = 4096U;
    // Modbus 轮询专用：DRAM 低于此值时跳过轮询
    static constexpr uint32_t MODBUS_POLL_MIN_DRAM = 12000U;

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

    static constexpr bool canRetryMqttsMemoryRecovery(uint32_t dramFree,
                                                       uint32_t largestBlock) {
        return dramFree >= MQTTS_MIN_DRAM_FREE &&
               largestBlock >= GUARD_WARN_LARGEST_BLOCK;
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
};

}  // namespace FastBee

#endif  // FASTBEE_MEMORY_BUDGET_H
