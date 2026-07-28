/**
 * @file test_longrun_stability.cpp
 * @brief Long-run (24h simulated) memory watermark & guard-level regression.
 *
 * These tests drive the pure decision function MemoryBudget::guardLevelForDram()
 * over a simulated 24-hour device lifetime to guarantee long-term stability
 * properties that are otherwise only observable after days on hardware:
 *
 *   1. Steady-state heap jitter never falsely escalates the memory guard level
 *      (no spurious MQTT pause / reboot under normal operation).
 *   2. A genuine slow leak is detected and escalates MONOTONICALLY
 *      NORMAL -> WARN -> SEVERE -> CRITICAL without ever regressing.
 *   3. A recovering heap de-escalates cleanly back to NORMAL (no "stuck" guard).
 *   4. The running watermark (minimum free DRAM) reflects the true trough, and a
 *      brief transient dip that recovers does not leave the guard latched.
 *
 * The guard function is stateless and constexpr, so a full simulated day runs in
 * microseconds on the native target while covering the full operating envelope.
 */

#include <unity.h>
#include <Arduino.h>
#include "core/MemoryBudget.h"
#include "helpers/TestLogger.h"

using FastBee::MemoryBudget;
using FastBee::MemoryPressureLevel;

void test_longrun_stability_group();

namespace {

// Deterministic LCG so simulated jitter is reproducible across CI runs.
uint32_t lcg(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

// Compressed but representative cadence: 1 sample / 10s over 24h => 8640 samples.
// This exercises a full day of operation while keeping native runtime trivial.
constexpr uint32_t SAMPLES_PER_DAY = 8640u;

// Symmetric jitter in [-span, +span] applied to a baseline value.
int32_t jitter(uint32_t& rng, uint32_t span) {
    if (span == 0) return 0;
    return static_cast<int32_t>(lcg(rng) % (2u * span + 1u)) - static_cast<int32_t>(span);
}

}  // namespace

/**
 * @brief 24h steady-state operation must never falsely escalate the guard.
 *
 * A healthy device idles well above every WARN threshold. Normal allocator
 * jitter (SSE buffers, JSON docs, MQTT frames) must not trip WARN/SEVERE/CRITICAL,
 * otherwise the device would needlessly pause services or reboot.
 */
void test_longrun_steady_state_no_false_escalation() {
    TestLog::testStart("LongRun: 24h Steady-State No False Escalation");

    uint32_t rng = 0xC0FFEEu;
    uint32_t watermarkMin = 0xFFFFFFFFu;
    uint32_t watermarkMax = 0u;
    uint32_t escalations = 0u;

    for (uint32_t i = 0; i < SAMPLES_PER_DAY; ++i) {
        // Baseline comfortably above WARN (dram<30720, largest<16384, frag>=65).
        uint32_t dramFree = static_cast<uint32_t>(60000 + jitter(rng, 4000));
        uint32_t largest  = static_cast<uint32_t>(45000 + jitter(rng, 3000));
        uint8_t  frag     = static_cast<uint8_t>(40 + (lcg(rng) % 11));  // 40..50%

        if (dramFree < watermarkMin) watermarkMin = dramFree;
        if (dramFree > watermarkMax) watermarkMax = dramFree;

        MemoryPressureLevel level =
            MemoryBudget::guardLevelForDram(dramFree, largest, frag, false);
        if (level != MemoryPressureLevel::NORMAL) {
            escalations++;
        }
    }

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, escalations,
        "Steady-state jitter must never escalate above NORMAL");
    // Watermark trough stays a wide margin above the WARN threshold.
    TEST_ASSERT_GREATER_THAN_UINT32(MemoryBudget::GUARD_WARN_DRAM_FREE + 20000u,
                                    watermarkMin);
    TestLog::step("8640 samples (24h @10s) stayed NORMAL, watermark stable");

    TestLog::testEnd(true);
}

/**
 * @brief A slow leak escalates monotonically to CRITICAL and never regresses.
 *
 * Models a ~70KB/day DRAM leak with proportional largest-block shrink and rising
 * fragmentation. Because guardLevelForDram() is monotonic in every argument, a
 * monotonically worsening heap must produce a non-decreasing guard level, must
 * transit WARN and SEVERE, and must end at CRITICAL.
 */
void test_longrun_slow_leak_monotonic_escalation() {
    TestLog::testStart("LongRun: Slow Leak Monotonic Escalation");

    const uint32_t startDram = 80000u;
    const uint32_t endDram   = 10000u;   // below CRITICAL (16384)
    const uint32_t startBlk  = 50000u;
    const uint32_t endBlk    = 7000u;    // below CRITICAL largest (8192)

    MemoryPressureLevel prev = MemoryPressureLevel::NORMAL;
    bool sawWarn = false, sawSevere = false, sawCritical = false;
    uint32_t watermarkMin = startDram;

    for (uint32_t i = 0; i < SAMPLES_PER_DAY; ++i) {
        // Linear interpolation from start to end across the day.
        uint32_t dramFree = startDram - (startDram - endDram) * i / (SAMPLES_PER_DAY - 1);
        uint32_t largest  = startBlk  - (startBlk  - endBlk)  * i / (SAMPLES_PER_DAY - 1);
        uint8_t  frag     = static_cast<uint8_t>(40u + 48u * i / (SAMPLES_PER_DAY - 1));  // 40..88%

        if (dramFree < watermarkMin) watermarkMin = dramFree;

        MemoryPressureLevel level =
            MemoryBudget::guardLevelForDram(dramFree, largest, frag, false);

        // Monotonic non-decreasing: a worsening heap never de-escalates.
        TEST_ASSERT_GREATER_OR_EQUAL_UINT8(
            static_cast<uint8_t>(prev), static_cast<uint8_t>(level));
        prev = level;

        if (level == MemoryPressureLevel::WARN) sawWarn = true;
        if (level == MemoryPressureLevel::SEVERE) sawSevere = true;
        if (level == MemoryPressureLevel::CRITICAL) sawCritical = true;
    }

    TEST_ASSERT_TRUE_MESSAGE(sawWarn, "Leak must pass through WARN");
    TEST_ASSERT_TRUE_MESSAGE(sawSevere, "Leak must pass through SEVERE");
    TEST_ASSERT_TRUE_MESSAGE(sawCritical, "Leak must reach CRITICAL");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(
        static_cast<uint8_t>(MemoryPressureLevel::CRITICAL),
        static_cast<uint8_t>(prev), "Final guard level must be CRITICAL");
    TEST_ASSERT_LESS_THAN_UINT32(MemoryBudget::GUARD_CRITICAL_DRAM_FREE, watermarkMin);
    TestLog::step("Leak escalated NORMAL->WARN->SEVERE->CRITICAL monotonically");

    TestLog::testEnd(true);
}

/**
 * @brief A recovering heap de-escalates cleanly back to NORMAL.
 *
 * After a low-memory episode (e.g. TLS handshake burst) is reclaimed, the guard
 * must not stay latched at a high level; it must track the improving heap down
 * to NORMAL. Verifies the guard is not "sticky".
 */
void test_longrun_recovery_deescalation() {
    TestLog::testStart("LongRun: Recovery De-escalation To NORMAL");

    const uint32_t startDram = 15000u;   // CRITICAL
    const uint32_t endDram   = 62000u;   // healthy
    const uint32_t startBlk  = 7000u;    // CRITICAL largest
    const uint32_t endBlk    = 46000u;

    MemoryPressureLevel prev = MemoryPressureLevel::CRITICAL;
    const uint32_t STEPS = 2000u;

    for (uint32_t i = 0; i < STEPS; ++i) {
        uint32_t dramFree = startDram + (endDram - startDram) * i / (STEPS - 1);
        uint32_t largest  = startBlk  + (endBlk  - startBlk)  * i / (STEPS - 1);
        uint8_t  frag     = static_cast<uint8_t>(88u - 48u * i / (STEPS - 1));  // 88..40%

        MemoryPressureLevel level =
            MemoryBudget::guardLevelForDram(dramFree, largest, frag, false);

        // Monotonic non-increasing: an improving heap never re-escalates.
        TEST_ASSERT_LESS_OR_EQUAL_UINT8(
            static_cast<uint8_t>(prev), static_cast<uint8_t>(level));
        prev = level;
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(
        static_cast<uint8_t>(MemoryPressureLevel::NORMAL),
        static_cast<uint8_t>(prev), "Recovered heap must return to NORMAL");
    TestLog::step("Guard tracked recovery down to NORMAL (not latched)");

    TestLog::testEnd(true);
}

/**
 * @brief A brief transient dip is captured by the watermark but does not latch.
 *
 * A single short trough (deep allocation spike that is immediately freed) must:
 *   - be reflected in the running watermark minimum, and
 *   - momentarily raise the guard level, then
 *   - return to NORMAL once the spike is freed (stateless guard, no latch).
 */
void test_longrun_transient_dip_watermark_tracking() {
    TestLog::testStart("LongRun: Transient Dip Watermark Tracking");

    const uint32_t healthyDram = 58000u;
    const uint32_t troughDram  = 14000u;   // one-sample CRITICAL spike
    const uint32_t dipIndex    = 4000u;

    uint32_t watermarkMin = 0xFFFFFFFFu;
    bool dipEscalated = false;
    MemoryPressureLevel afterDip = MemoryPressureLevel::CRITICAL;

    for (uint32_t i = 0; i < SAMPLES_PER_DAY; ++i) {
        uint32_t dramFree = (i == dipIndex) ? troughDram : healthyDram;
        uint32_t largest  = (i == dipIndex) ? 7000u : 44000u;
        uint8_t  frag     = (i == dipIndex) ? 88u : 42u;

        if (dramFree < watermarkMin) watermarkMin = dramFree;

        MemoryPressureLevel level =
            MemoryBudget::guardLevelForDram(dramFree, largest, frag, false);

        if (i == dipIndex && level != MemoryPressureLevel::NORMAL) {
            dipEscalated = true;
        }
        if (i == dipIndex + 1) {
            afterDip = level;  // sample immediately after the spike is freed
        }
    }

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(troughDram, watermarkMin,
        "Watermark must capture the transient trough");
    TEST_ASSERT_TRUE_MESSAGE(dipEscalated, "Transient dip must raise the guard");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(
        static_cast<uint8_t>(MemoryPressureLevel::NORMAL),
        static_cast<uint8_t>(afterDip),
        "Guard must return to NORMAL after the spike is freed (no latch)");
    TestLog::step("Watermark captured trough; guard recovered next sample");

    TestLog::testEnd(true);
}

/**
 * @brief 周期性 TLS 握手突发不得让无 PSRAM 准入闸门在低谷放行。
 *
 * 模拟 24h 内每小时一次 MQTTS 重连窗口：握手期 DRAM 凹陷 ~42KB，
 * 握手后回升。无 PSRAM 门槛必须：
 *   - 低谷期（largest 被打穿）全部拒绝（放行即复现 tcp_receive panic）；
 *   - 健康窗口全部放行（门槛不得误拒致 MQTTS 永久不可用）；
 *   - 守卫等级每轮回到 NORMAL（无闩锁累积）。
 */
void test_longrun_tls_burst_gate_and_recovery() {
    TestLog::testStart("LongRun: Hourly TLS Burst Gate & Guard Recovery");

    constexpr uint32_t HEALTHY_DRAM = 60000u, HEALTHY_BLK = 45000u;
    constexpr uint32_t TROUGH_DRAM  = 18000u, TROUGH_BLK  = 9000u;

    uint32_t troughAllowed = 0, healthyRejected = 0, latchedAfterBurst = 0;

    for (uint32_t hour = 0; hour < 24; ++hour) {
        // 握手凹陷期（6 个采样点，逐步下探再回升）
        const uint32_t dip[6][2] = {
            {45000u, 30000u}, {30000u, 18000u}, {TROUGH_DRAM, TROUGH_BLK},
            {TROUGH_DRAM, TROUGH_BLK}, {30000u, 20000u}, {48000u, 34000u}};
        for (const auto& s : dip) {
            if (MemoryBudget::canAttemptMqtts(s[0], s[1], false)) troughAllowed++;
        }
        // 握手窗口外的健康稳态：门槛必须放行
        if (!MemoryBudget::canAttemptMqtts(HEALTHY_DRAM, HEALTHY_BLK, false)) {
            healthyRejected++;
        }
        // 突发结束后守卫必须回 NORMAL（无状态，不得闩锁）
        MemoryPressureLevel after = MemoryBudget::guardLevelForDram(
            HEALTHY_DRAM, HEALTHY_BLK, 42u, false);
        if (after != MemoryPressureLevel::NORMAL) latchedAfterBurst++;
    }

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, troughAllowed,
        "no-PSRAM gate must reject every marginal handshake window");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, healthyRejected,
        "no-PSRAM gate must allow handshakes from healthy steady-state");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, latchedAfterBurst,
        "Guard must return to NORMAL after every burst cycle");
    TestLog::step("24 hourly TLS bursts: gate airtight in troughs, open when healthy");

    TestLog::testEnd(true);
}

/**
 * @brief 周期任务调度器跨 millis() 回绕必须不丢拍、不多发。
 *
 * 模拟约 100 天运行（跨越两次 uint32 回绕），1 小时周期任务用
 * 修复后的有符号差值写法调度：每周期恰触发一次。同步对照旧的
 * 绝对比较写法，它在回绕后立即连环误触发（回归的根因守护）。
 */
void test_longrun_scheduler_survives_millis_wrap() {
    TestLog::testStart("LongRun: Periodic Scheduler Across millis() Wraparound");

    constexpr uint32_t PERIOD_MS = 3600000u;   // 1h
    constexpr uint32_t TICK_MS   = 60000u;     // 1min 调度粒度
    constexpr uint64_t TOTAL_MS  = 100ull * 24 * 3600 * 1000;  // ~100 天，回绕 2 次

    uint32_t nowFixed = 0, deadlineFixed = PERIOD_MS;
    uint32_t firesFixed = 0;
    uint32_t nowLegacy = 0, deadlineLegacy = PERIOD_MS;
    uint64_t firesLegacy = 0;
    bool legacyMisfired = false;

    for (uint64_t t = 0; t < TOTAL_MS; t += TICK_MS) {
        nowFixed += TICK_MS;    // uint32 自然回绕
        nowLegacy += TICK_MS;

        // 修复后：有符号差值，回绕透明
        if ((int32_t)(nowFixed - deadlineFixed) >= 0) {
            firesFixed++;
            deadlineFixed += PERIOD_MS;
        }
        // 旧写法：绝对比较，回绕后 deadline 遗留在高位 → 长期不触发；
        // deadline 溢出到低位时 → 连环误触发
        if (nowLegacy >= deadlineLegacy) {
            firesLegacy++;
            uint32_t next = deadlineLegacy + PERIOD_MS;
            if (next < deadlineLegacy) legacyMisfired = true;  // 溢出即失真
            deadlineLegacy = next;
        }
    }

    const uint32_t expectedFires = (uint32_t)(TOTAL_MS / PERIOD_MS);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(expectedFires, firesFixed,
        "Signed-diff scheduler must fire exactly once per period across wraps");
    TEST_ASSERT_TRUE_MESSAGE(legacyMisfired || firesLegacy != expectedFires,
        "Legacy absolute comparison must exhibit wraparound misfire (guards the fix)");
    TestLog::step("2400 periods across 2 wraps: fixed=exact, legacy=misfires");

    TestLog::testEnd(true);
}

void test_longrun_stability_group() {
    TestLog::groupStart("Long-Run Stability Tests");
    RUN_TEST(test_longrun_steady_state_no_false_escalation);
    RUN_TEST(test_longrun_slow_leak_monotonic_escalation);
    RUN_TEST(test_longrun_recovery_deescalation);
    RUN_TEST(test_longrun_transient_dip_watermark_tracking);
    RUN_TEST(test_longrun_tls_burst_gate_and_recovery);
    RUN_TEST(test_longrun_scheduler_survives_millis_wrap);
    TestLog::groupEnd();
}
