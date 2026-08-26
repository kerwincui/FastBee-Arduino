#ifndef IP_RATE_LIMITER_H
#define IP_RATE_LIMITER_H

#include <Arduino.h>
#include "systems/LoggerSystem.h"

/**
 * @brief IP 令牌桶限流器（固定大小数组 + LRU 替换）
 *
 * 以 16 槽固定数组记录 (IP, 分级) 维度的访问桶，槽满时替换
 * 最久未使用的槽位，避免无界容器的堆增长。
 *
 * 实现要点：
 * - 令牌数以毫单位（×1000）整数存储，避免 float
 * - 令牌补充由 millis() 差值整数运算驱动，无定时器依赖
 * - 补充速率 = 容量/分钟，静默 60s 后桶必然回满
 * - 内存占用：16 槽 × 20B ≈ 320B（静态分配，无堆分配）
 */
class IpRateLimiter {
public:
    enum class Tier : uint8_t {
        Strict = 0,  // 认证端点：10 次/分钟/IP
        Normal = 1   // 普通 API：120 次/分钟/IP
    };

    static constexpr size_t SLOT_COUNT = 16;

    IpRateLimiter() = default;

    IpRateLimiter(const IpRateLimiter&) = delete;
    IpRateLimiter& operator=(const IpRateLimiter&) = delete;

    void reset() {
        for (size_t i = 0; i < SLOT_COUNT; ++i) {
            slots[i] = Slot();
        }
    }

    /**
     * @brief 消耗 1 个令牌
     * @param ip           IPv4 客户端地址（uint32_t 主机序，0 表示无效）
     * @param tier         限流分级
     * @param nowMs        当前 millis()
     * @param retryAfterSec [out] 超限时建议的重试秒数（Retry-After）
     * @return true=放行；false=超限（应返回 429）
     */
    bool consume(uint32_t ip, Tier tier, unsigned long nowMs, uint32_t& retryAfterSec) {
        retryAfterSec = 1;
        if (ip == 0) return true;  // 无效 IP 不参与限流

        const uint8_t tierId = (uint8_t)tier;
        const uint32_t capacityMilli = (uint32_t)capacityOf(tier) * 1000UL;

        // 1. 查找已有桶，同时记录 LRU 替换候选（空槽 lastUseMs=0 天然优先）
        Slot* slot = nullptr;
        size_t lruIdx = 0;
        for (size_t i = 0; i < SLOT_COUNT; ++i) {
            if (slots[i].ip == ip && slots[i].tier == tierId) {
                slot = &slots[i];
                break;
            }
            if (slots[i].ip == 0 || slots[i].lastUseMs < slots[lruIdx].lastUseMs) {
                lruIdx = i;
            }
        }

        if (!slot) {
            // 2. 桶不存在：LRU 替换最久未用槽位，新桶满令牌
            slot = &slots[lruIdx];
            slot->ip = ip;
            slot->tier = tierId;
            slot->tokensMilli = capacityMilli;
            slot->lastUseMs = nowMs;
            slot->lastRejectLogMs = 0;
        } else {
            // 3. 令牌补充：整分钟窗口回满，其余按整数比例累加（uint64 防乘法溢出）
            const unsigned long elapsed = nowMs - slot->lastUseMs;
            if (elapsed >= 60000UL) {
                slot->tokensMilli = capacityMilli;
            } else if (slot->tokensMilli < capacityMilli) {
                const uint32_t add =
                    (uint32_t)(((uint64_t)elapsed * capacityMilli) / 60000UL);
                slot->tokensMilli = min(slot->tokensMilli + add, capacityMilli);
            }
            slot->lastUseMs = nowMs;
        }

        if (slot->tokensMilli >= 1000UL) {
            slot->tokensMilli -= 1000UL;
            return true;
        }

        // 4. 超限：计算 Retry-After（向上取整到秒，至少 1s）
        const uint32_t needMilli = 1000UL - slot->tokensMilli;
        const uint32_t waitMs =
            (uint32_t)(((uint64_t)needMilli * 60000UL + capacityMilli - 1UL) / capacityMilli);
        retryAfterSec = (waitMs + 999UL) / 1000UL;
        if (retryAfterSec == 0) retryAfterSec = 1;

        // 5. 限流日志限频（同一桶最多 5s 一条），避免被攻击时日志洪泛
        if (nowMs - slot->lastRejectLogMs >= 5000UL) {
            slot->lastRejectLogMs = nowMs;
            LOG_WARNINGF("[RateLimit] 429 ip=%u.%u.%u.%u tier=%s retryAfter=%us",
                         (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
                         (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF),
                         (tier == Tier::Strict) ? "strict" : "normal",
                         (unsigned)retryAfterSec);
        }
        return false;
    }

private:
    struct Slot {
        uint32_t ip = 0;               // IPv4 主机序，0=空槽
        uint32_t tokensMilli = 0;      // 当前令牌数（毫单位 ×1000，避免 float）
        unsigned long lastUseMs = 0;   // 最近访问时间（令牌补充 + LRU 依据）
        unsigned long lastRejectLogMs = 0;  // 上次拒绝日志时间（日志限频）
        uint8_t tier = 0;              // Tier 值
    };

    static constexpr uint8_t capacityOf(Tier tier) {
        return (tier == Tier::Strict) ? STRICT_PER_MIN : NORMAL_PER_MIN;
    }

    static constexpr uint8_t STRICT_PER_MIN = 10;   // 认证端点：10 次/分钟/IP
    static constexpr uint8_t NORMAL_PER_MIN = 120;  // 普通 API：120 次/分钟/IP

    Slot slots[SLOT_COUNT];
};

#endif // IP_RATE_LIMITER_H
