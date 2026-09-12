/**
 * @file test_peripheral_manager_opt.cpp
 * @brief PeripheralManager 优化项单元测试（内存池 / ISR 队列 / 去抖 / 线程安全）
 *
 * 覆盖本次优化的四个方向：
 * - T1/T3 内存管理：StaticSlabPool + PooledAllocator 真实实现（直接 include 生产头文件）
 *   * 池分配/回收/复用、耗尽回退 malloc、统计计数、跨 T 共享池、std::map 集成
 * - T3 引脚/中断验证：attach 前的引脚合法性决策（镜像）
 * - T4 ISR 队列：溢出计数语义（队列满丢事件必须可观测）
 * - T4 中断去抖：shouldDispatchIsrEvent 决策（镜像 PeripheralManager::shouldDispatchIsrEvent）
 * - T4 Ticker 回调安全：非阻塞尝试加锁语义（拿不到锁跳过本次，不阻塞定时器任务）
 */

#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include "helpers/TestLogger.h"
#include "core/PeripheralConfig.h"
#include "utils/StaticPoolAllocator.h"

#include <map>
#include <string>
#include <vector>
#include <cstring>
#include <chrono>
#include <climits>
#include <fstream>
#include <sstream>
#include <functional>

void test_peripheral_manager_opt_group();

// ============================================================
// TEST GROUP 1: StaticSlabPool 基础行为
// ============================================================

static void test_slabpool_constants() {
    FastBee::StaticSlabPool<48, 8> pool;
    TEST_ASSERT_EQUAL(8u, pool.capacity());
    TEST_ASSERT_EQUAL(48u, pool.blockSize());
    TEST_ASSERT_EQUAL(48u * 8u, pool.totalBytes());
    TEST_ASSERT_EQUAL(0u, pool.usedCount());
    TEST_ASSERT_EQUAL(0u, pool.peakUsed());
    TEST_ASSERT_EQUAL(0u, pool.overflowCount());
}

static void test_slabpool_distinct_addresses() {
    FastBee::StaticSlabPool<64, 4> pool;
    void* a = pool.allocate();
    void* b = pool.allocate();
    void* c = pool.allocate();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(a != b);
    TEST_ASSERT_TRUE(b != c);
    TEST_ASSERT_TRUE(a != c);
    TEST_ASSERT_EQUAL(3u, pool.usedCount());
    TEST_ASSERT_EQUAL(3u, pool.peakUsed());
    pool.deallocate(a);
    pool.deallocate(b);
    pool.deallocate(c);
}

static void test_slabpool_overflow_fallback_malloc() {
    FastBee::StaticSlabPool<32, 2> pool;
    void* a = pool.allocate();
    void* b = pool.allocate();
    TEST_ASSERT_EQUAL(0u, pool.overflowCount());
    // 池耗尽：必须回退到 malloc 而不是返回 nullptr（功能不降级）
    void* c = pool.allocate();
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL(1u, pool.overflowCount());
    TEST_ASSERT_EQUAL(2u, pool.usedCount());  // overflow 不计入 usedCount
    pool.deallocate(a);
    pool.deallocate(b);
    pool.deallocate(c);  // 池外指针走 ::free 路径，不崩溃
    TEST_ASSERT_EQUAL(0u, pool.usedCount());
}

static void test_slabpool_reuse_after_free() {
    FastBee::StaticSlabPool<96, 1> pool;
    void* a = pool.allocate();
    TEST_ASSERT_NOT_NULL(a);
    pool.deallocate(a);
    TEST_ASSERT_EQUAL(0u, pool.usedCount());
    // 归还的块必须立即可复用（freelist），且不再触发 overflow
    void* b = pool.allocate();
    TEST_ASSERT_EQUAL_PTR(a, b);
    TEST_ASSERT_EQUAL(0u, pool.overflowCount());
    pool.deallocate(b);
}

static void test_slabpool_peak_tracking() {
    FastBee::StaticSlabPool<40, 10> pool;
    std::vector<void*> blocks;
    for (int i = 0; i < 6; i++) blocks.push_back(pool.allocate());
    TEST_ASSERT_EQUAL(6u, pool.peakUsed());
    for (void* p : blocks) pool.deallocate(p);
    // 再次分配不超过峰值时，peakUsed 不应增长
    void* x = pool.allocate();
    TEST_ASSERT_EQUAL(6u, pool.peakUsed());
    pool.deallocate(x);
}

static void test_slabpool_deallocate_null_safe() {
    FastBee::StaticSlabPool<48, 4> pool;
    pool.deallocate(nullptr);  // 不允许崩溃
    TEST_ASSERT_EQUAL(0u, pool.usedCount());
}

// ============================================================
// TEST GROUP 2: PooledAllocator + std::map 集成
// ============================================================

// 每个用例使用独立的 (BlockSize, BlockCount) 组合，
// 避免共享单例池（sharedSlabPool 按 (B,N) 键控）造成用例间干扰

using MapAllocA = FastBee::PooledAllocator<std::pair<const int, uint64_t>, 128, 8>;
using TestMapA  = std::map<int, uint64_t, std::less<int>, MapAllocA>;

static void test_pooled_map_insert_uses_pool() {
    TestMapA m;
    for (int i = 0; i < 5; i++) m[i] = (uint64_t)i * 10;
    TEST_ASSERT_EQUAL(5u, m.size());
    TEST_ASSERT_EQUAL(5u, MapAllocA::pool().usedCount());
    TEST_ASSERT_EQUAL(0u, MapAllocA::pool().overflowCount());
    TEST_ASSERT_EQUAL_UINT64(30, m[3]);
    m.clear();
}

static void test_pooled_map_erase_reclaims_pool() {
    TestMapA m;
    for (int i = 0; i < 8; i++) m[i] = i;
    TEST_ASSERT_EQUAL(8u, MapAllocA::pool().usedCount());
    m.clear();
    // 全部归还 freelist：池占用归零（无泄漏）
    TEST_ASSERT_EQUAL(0u, MapAllocA::pool().usedCount());
    TEST_ASSERT_EQUAL(8u, MapAllocA::pool().peakUsed());
}

using MapAllocB = FastBee::PooledAllocator<std::pair<const int, uint32_t>, 96, 6>;
using TestMapB  = std::map<int, uint32_t, std::less<int>, MapAllocB>;

static void test_pooled_map_churn_no_leak() {
    TestMapB m;
    // 高频增删 1000 轮：任意时刻池占用 <= 容量，且不产生 overflow
    for (int round = 0; round < 1000; round++) {
        for (int i = 0; i < 6; i++) m[round * 6 + i] = i;
        for (int i = 0; i < 6; i++) m.erase(round * 6 + i);
    }
    TEST_ASSERT_EQUAL(0u, m.size());
    TEST_ASSERT_EQUAL(0u, MapAllocB::pool().usedCount());
    TEST_ASSERT_TRUE(MapAllocB::pool().peakUsed() <= MapAllocB::pool().capacity());
    TEST_ASSERT_EQUAL(0u, MapAllocB::pool().overflowCount());
}

static void test_pooled_map_exhaustion_graceful() {
    TestMapB m;
    // 插入超过池容量：必须优雅回退 malloc，功能不受影响
    for (int i = 0; i < 16; i++) m[i] = i;
    TEST_ASSERT_EQUAL(16u, m.size());
    TEST_ASSERT_EQUAL(MapAllocB::pool().capacity(), MapAllocB::pool().usedCount());
    TEST_ASSERT_TRUE(MapAllocB::pool().overflowCount() >= 10u);
    TEST_ASSERT_EQUAL_UINT32(15, m[15]);
    m.clear();
    TEST_ASSERT_EQUAL(0u, MapAllocB::pool().usedCount());
}

// 值类型大于块大小：整节点走 malloc 回退路径，池保持空闲
using MapAllocC = FastBee::PooledAllocator<std::pair<const int, uint8_t>, 16, 4>;
using TestMapC  = std::map<int, uint8_t, std::less<int>, MapAllocC>;

static void test_pooled_map_oversized_node_fallback() {
    TestMapC m;
    // _Rb_tree_node<pair<const int, uint8_t>> 远大于 16B → 全部走 malloc
    for (int i = 0; i < 4; i++) m[i] = (uint8_t)i;
    TEST_ASSERT_EQUAL(4u, m.size());
    TEST_ASSERT_EQUAL(0u, MapAllocC::pool().usedCount());
    m.clear();
}

// 跨 T 共享池：两种不同 value_type 的 map 使用同 (B,N) 参数共享同一池
using MapAllocD1 = FastBee::PooledAllocator<std::pair<const int, uint32_t>, 72, 5>;
using MapAllocD2 = FastBee::PooledAllocator<std::pair<const std::string, uint8_t>, 72, 5>;
using TestMapD1  = std::map<int, uint32_t, std::less<int>, MapAllocD1>;
using TestMapD2  = std::map<std::string, uint8_t, std::less<std::string>, MapAllocD2>;

static void test_pooled_shared_pool_across_types() {
    TestMapD1 m1;
    TestMapD2 m2;
    m1[1] = 100;
    m2["key"] = 7;
    // 两个 map 的节点应计入同一个共享池
    TEST_ASSERT_EQUAL(2u, MapAllocD1::pool().usedCount());
    TEST_ASSERT_TRUE(&MapAllocD1::pool() == &MapAllocD2::pool());
    m1.clear();
    m2.clear();
    TEST_ASSERT_EQUAL(0u, MapAllocD1::pool().usedCount());
}

static void test_pooled_allocator_rebind_and_equality() {
    MapAllocA a1;
    MapAllocA a2;
    TEST_ASSERT_TRUE(a1 == a2);
    TEST_ASSERT_FALSE(a1 != a2);
    // rebind 到其他类型后仍是同参数池的分配器
    using Rebound = MapAllocA::rebind<std::pair<const int, uint64_t>>::other;
    Rebound rb;
    TEST_ASSERT_TRUE(&Rebound::pool() == &MapAllocA::pool());
}

// ============================================================
// TEST GROUP 3: SmallNodePool 预设参数（生产配置守护）
// ============================================================

static void test_smallnode_preset_constants() {
    TEST_ASSERT_EQUAL(64u, FastBee::SMALL_NODE_BLOCK_SIZE);
    TEST_ASSERT_EQUAL(64u, FastBee::SMALL_NODE_BLOCK_COUNT);
    TEST_ASSERT_EQUAL(4096u, FastBee::SMALL_NODE_BLOCK_SIZE * FastBee::SMALL_NODE_BLOCK_COUNT);
}

using SmallMap = std::map<int, uint8_t, std::less<int>,
                          FastBee::SmallNodeAllocator<std::pair<const int, uint8_t>>>;
using SmallAlloc = FastBee::SmallNodeAllocator<std::pair<const int, uint8_t>>;

static void test_smallnode_small_map_hits_pool() {
    SmallMap m;
    for (int i = 0; i < 10; i++) m[i] = (uint8_t)i;
    // 小节点（~40B）必须命中 64B 池路径，而非 malloc
    TEST_ASSERT_EQUAL(10u, SmallAlloc::pool().usedCount());
    m.clear();
    TEST_ASSERT_EQUAL(0u, SmallAlloc::pool().usedCount());
}

// ============================================================
// TEST GROUP 4: 池分配 vs malloc 性能基准（信息性指标）
// ============================================================

static void test_pool_vs_malloc_benchmark() {
    constexpr int ROUNDS = 20000;
    FastBee::StaticSlabPool<48, 32> pool;

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < ROUNDS; i++) {
        void* p = pool.allocate();
        pool.deallocate(p);
    }
    auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < ROUNDS; i++) {
        void* p = ::malloc(48);
        ::free(p);
    }
    auto t2 = std::chrono::steady_clock::now();

    double poolUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1.0;
    double mallocUs = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() / 1.0;
    Serial.printf("[PERF] pool alloc/dealloc x%d: %.1f us (%.2f ns/op)\n",
                  ROUNDS, poolUs, poolUs * 1000.0 / ROUNDS);
    Serial.printf("[PERF] malloc/free    x%d: %.1f us (%.2f ns/op)\n",
                  ROUNDS, mallocUs, mallocUs * 1000.0 / ROUNDS);
    // 守护断言：池路径性能不应劣于 malloc（允许 20% 容差防抖动）
    TEST_ASSERT_TRUE(poolUs <= mallocUs * 1.2);
}

// ============================================================
// TEST GROUP 5: ISR 去抖决策（镜像 PeripheralManager::shouldDispatchIsrEvent）
// ============================================================

// 镜像生产实现：无符号减法处理 millis() 回绕
static bool mirror_shouldDispatchIsrEvent(unsigned long nowMs, unsigned long lastDispatchMs,
                                          uint16_t debounceMs) {
    return (nowMs - lastDispatchMs) >= debounceMs;
}

static void test_debounce_first_event_dispatches() {
    // 空闲很久后的首次触发必须派发
    TEST_ASSERT_TRUE(mirror_shouldDispatchIsrEvent(10000, 0, 5));
}

static void test_debounce_within_window_suppressed() {
    // 窗口内（<5ms）的抖动事件必须被合并
    TEST_ASSERT_FALSE(mirror_shouldDispatchIsrEvent(1002, 1000, 5));
    TEST_ASSERT_FALSE(mirror_shouldDispatchIsrEvent(1004, 1000, 5));
}

static void test_debounce_boundary_dispatches() {
    // 恰好到达窗口边界（>=5ms）允许派发
    TEST_ASSERT_TRUE(mirror_shouldDispatchIsrEvent(1005, 1000, 5));
}

static void test_debounce_millis_wraparound() {
    // millis() 回绕：last 接近 ULONG_MAX，now 刚回绕到小数
    unsigned long last = ULONG_MAX - 15;  // 0xFFFFFFF0
    unsigned long now = 2;
    // now - last = 18 >= 5 → 必须派发，不能因回绕误判为"窗口内"
    TEST_ASSERT_TRUE(mirror_shouldDispatchIsrEvent(now, last, 5));
}

static void test_debounce_zero_window_always_dispatches() {
    TEST_ASSERT_TRUE(mirror_shouldDispatchIsrEvent(100, 100, 0));
}

static void test_debounce_storm_coalescing() {
    // 模拟机械按键抖动：100ms 内每 1ms 一次中断（共 100 次），
    // 5ms 去抖窗口下最多派发 21 次（t=0,5,10,...,100），负载降低 ~79%
    unsigned long last = 0;
    int dispatched = 0;
    bool first = true;
    for (unsigned long t = 0; t <= 100; t += 1) {
        if (first || mirror_shouldDispatchIsrEvent(t, last, 5)) {
            dispatched++;
            last = t;
            first = false;
        }
    }
    TEST_ASSERT_EQUAL(21, dispatched);
}

static void test_analog_quality_classification() {
    const uint8_t current4To20 = static_cast<uint8_t>(AnalogSignalMode::CURRENT_4_20MA);
    const uint8_t voltage0To10 = static_cast<uint8_t>(AnalogSignalMode::VOLTAGE_0_10V);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AnalogInputQuality::VALID),
        static_cast<uint8_t>(classifyAnalogInputQuality(current4To20, 4.0f, 4.0f, 20.0f)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AnalogInputQuality::BELOW_RANGE),
        static_cast<uint8_t>(classifyAnalogInputQuality(current4To20, 3.5f, 4.0f, 20.0f)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AnalogInputQuality::DISCONNECTED),
        static_cast<uint8_t>(classifyAnalogInputQuality(current4To20, 2.9f, 4.0f, 20.0f)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AnalogInputQuality::ABOVE_RANGE),
        static_cast<uint8_t>(classifyAnalogInputQuality(current4To20, 20.5f, 4.0f, 20.0f)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AnalogInputQuality::SHORT_CIRCUIT),
        static_cast<uint8_t>(classifyAnalogInputQuality(current4To20, 21.1f, 4.0f, 20.0f)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AnalogInputQuality::BELOW_RANGE),
        static_cast<uint8_t>(classifyAnalogInputQuality(voltage0To10, -0.1f, 0.0f, 10.0f)));

    // A tolerance applies to both configured range endpoints and the 4-20mA
    // 3mA/21mA fault thresholds, but must not hide a distinct out-of-band code.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AnalogInputQuality::BELOW_RANGE),
        static_cast<uint8_t>(classifyAnalogInputQuality(current4To20, 2.995f, 4.0f, 20.0f, 0.01f)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AnalogInputQuality::DISCONNECTED),
        static_cast<uint8_t>(classifyAnalogInputQuality(current4To20, 2.98f, 4.0f, 20.0f, 0.01f)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AnalogInputQuality::ABOVE_RANGE),
        static_cast<uint8_t>(classifyAnalogInputQuality(current4To20, 21.005f, 4.0f, 20.0f, 0.01f)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AnalogInputQuality::SHORT_CIRCUIT),
        static_cast<uint8_t>(classifyAnalogInputQuality(current4To20, 21.02f, 4.0f, 20.0f, 0.01f)));
}

static void test_peripheral_config_destructor_smoke() {
    PeripheralConfig config;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PeripheralType::UNCONFIGURED),
                            static_cast<uint8_t>(config.type));
}

static void initializeAnalogConversionConfig(PeripheralConfig& config, AnalogSignalMode mode) {
    config.type = PeripheralType::GPIO_ANALOG_INPUT;
    config.params.adc.resolution = 12;
    config.params.adc.signalMode = static_cast<uint8_t>(mode);
    config.params.adc.conversionEnabled = true;
    config.params.adc.clampEnabled = false;
    config.params.adc.engineeringMin = 0.0f;
    config.params.adc.engineeringMax = 100.0f;
    config.params.adc.engineeringOffset = 0.0f;
    config.params.adc.vRef = 3.3f;
    config.params.adc.inputMin = 0.0f;
    config.params.adc.inputMax = 0.0f;
    config.params.adc.voltageRatio = 0.0f;
    config.params.adc.shuntResistance = 0.0f;
    config.params.adc.inputGain = 1.0f;
    config.params.adc.inputOffset = 0.0f;
}

static void test_analog_raw_conversion_boundaries() {
    // Use the production helper directly so header-only conversion changes are
    // exercised by the native binary rather than duplicated test arithmetic.
    AnalogInputQuality quality = AnalogInputQuality::INVALID_CONFIG;

    {
        PeripheralConfig linear;
        initializeAnalogConversionConfig(linear, AnalogSignalMode::CUSTOM_LINEAR);
        linear.params.adc.inputMin = 1000.0f;
        linear.params.adc.inputMax = 3000.0f;
        linear.params.adc.engineeringMin = -50.0f;
        linear.params.adc.engineeringMax = 150.0f;
        TEST_ASSERT_FLOAT_WITHIN(0.001f, -50.0f,
            convertAnalogInputRaw(linear, 1000, &quality));
        TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f,
            convertAnalogInputRaw(linear, 2000, &quality));
        TEST_ASSERT_FLOAT_WITHIN(0.001f, 150.0f,
            convertAnalogInputRaw(linear, 3000, &quality));
    }
    {
        PeripheralConfig voltage;
        initializeAnalogConversionConfig(voltage, AnalogSignalMode::VOLTAGE_0_10V);
        voltage.params.adc.voltageRatio = 10.0f / 3.3f;
        TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f,
            convertAnalogInputRaw(voltage, 0, &quality));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AnalogInputQuality::VALID),
                                static_cast<uint8_t>(quality));
        TEST_ASSERT_FLOAT_WITHIN(0.05f, 50.0f,
            convertAnalogInputRaw(voltage, 2048, &quality));
        TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f,
            convertAnalogInputRaw(voltage, 4095, &quality));
    }
    {
        PeripheralConfig current;
        initializeAnalogConversionConfig(current, AnalogSignalMode::CURRENT_4_20MA);
        current.params.adc.shuntResistance = 165.0f;
        current.params.adc.engineeringMin = -40.0f;
        current.params.adc.engineeringMax = 80.0f;
        current.params.adc.engineeringOffset = 2.0f;
        const uint16_t raw4mA = static_cast<uint16_t>((0.004f * 165.0f / 3.3f) * 4095.0f + 0.5f);
        const uint16_t raw12mA = static_cast<uint16_t>((0.012f * 165.0f / 3.3f) * 4095.0f + 0.5f);
        const uint16_t raw20mA = static_cast<uint16_t>((0.020f * 165.0f / 3.3f) * 4095.0f + 0.5f);
        TEST_ASSERT_FLOAT_WITHIN(0.001f, -38.0f,
            convertAnalogInputRaw(current, raw4mA, &quality));
        TEST_ASSERT_FLOAT_WITHIN(0.1f, 22.0f,
            convertAnalogInputRaw(current, raw12mA, &quality));
        TEST_ASSERT_FLOAT_WITHIN(0.001f, 82.0f,
            convertAnalogInputRaw(current, raw20mA, &quality));

        TEST_ASSERT_TRUE(isnan(convertAnalogInputRaw(current, 0, &quality)));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AnalogInputQuality::DISCONNECTED),
                                static_cast<uint8_t>(quality));
        current.params.adc.clampEnabled = true;
        TEST_ASSERT_FLOAT_WITHIN(0.001f, -38.0f,
            convertAnalogInputRaw(current, 0, &quality));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AnalogInputQuality::DISCONNECTED),
                                static_cast<uint8_t>(quality));

        current.params.adc.clampEnabled = false;
        current.params.adc.shuntResistance = 150.0f;
        TEST_ASSERT_TRUE(isnan(convertAnalogInputRaw(current, 4095, &quality)));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AnalogInputQuality::SHORT_CIRCUIT),
                                static_cast<uint8_t>(quality));

        current.params.adc.shuntResistance = 165.0f;
        current.params.adc.inputMin = 20.0f;
        current.params.adc.inputMax = 4.0f;
        TEST_ASSERT_TRUE(isnan(convertAnalogInputRaw(current, raw12mA, &quality)));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AnalogInputQuality::INVALID_CONFIG),
                                static_cast<uint8_t>(quality));
    }
}

static void test_inverted_interrupt_edges_use_logical_level() {
    TEST_ASSERT_TRUE(digitalInputLogicalLevel(false, true));
    TEST_ASSERT_FALSE(digitalInputLogicalLevel(true, true));
    TEST_ASSERT_TRUE(matchesDigitalInterruptEdge(PeripheralType::GPIO_INTERRUPT_RISING, true));
    TEST_ASSERT_FALSE(matchesDigitalInterruptEdge(PeripheralType::GPIO_INTERRUPT_RISING, false));
    TEST_ASSERT_TRUE(matchesDigitalInterruptEdge(PeripheralType::GPIO_INTERRUPT_FALLING, false));
    TEST_ASSERT_FALSE(matchesDigitalInterruptEdge(PeripheralType::GPIO_INTERRUPT_FALLING, true));
    TEST_ASSERT_TRUE(matchesDigitalInterruptEdge(PeripheralType::GPIO_INTERRUPT_CHANGE, true));
    TEST_ASSERT_TRUE(matchesDigitalInterruptEdge(PeripheralType::GPIO_INTERRUPT_CHANGE, false));
}

static void test_digital_input_defaults_and_report_modes_use_production_helpers() {
    TEST_ASSERT_EQUAL_UINT16(DIGITAL_INPUT_DEFAULT_DEBOUNCE_MS,
                             effectiveDigitalInputDebounceMs(0));
    TEST_ASSERT_EQUAL_UINT16(125, effectiveDigitalInputDebounceMs(125));
    TEST_ASSERT_TRUE(shouldReportDigitalInputChange(
        static_cast<uint8_t>(DigitalReportMode::ON_CHANGE)));
    TEST_ASSERT_FALSE(shouldReportDigitalInputChange(
        static_cast<uint8_t>(DigitalReportMode::EVENT_ONLY)));
    TEST_ASSERT_TRUE(shouldReportDigitalInputPeriodically(
        static_cast<uint8_t>(DigitalReportMode::PERIODIC)));
    TEST_ASSERT_FALSE(shouldReportDigitalInputPeriodically(
        static_cast<uint8_t>(DigitalReportMode::ON_CHANGE)));
}

static void test_analog_output_percent_mapping_uses_production_helper() {
    TEST_ASSERT_EQUAL_UINT32(51, mapAnalogOutputPercent(0, 20, 80, 8));
    TEST_ASSERT_EQUAL_UINT32(127, mapAnalogOutputPercent(50, 20, 80, 8));
    TEST_ASSERT_EQUAL_UINT32(204, mapAnalogOutputPercent(100, 20, 80, 8));
    TEST_ASSERT_EQUAL_UINT32(204, mapAnalogOutputPercent(150, 20, 80, 8));
    TEST_ASSERT_EQUAL_UINT32(127, mapAnalogOutputPercent(50, 80, 20, 8));
    TEST_ASSERT_EQUAL_UINT32(65535, mapAnalogOutputPercent(100, 0, 120, 16));
    TEST_ASSERT_EQUAL_UINT32(1, mapAnalogOutputPercent(100, 0, 100, 0));
}

// ============================================================
// TEST GROUP 6: ISR 队列溢出计数语义（镜像 ISR_QUEUE_SIZE=8）
// ============================================================

struct MirrorIsrQueue {
    static constexpr int CAPACITY = 8;  // 与生产 ISR_QUEUE_SIZE 一致
    uint8_t buf[CAPACITY];
    int count = 0;
    uint32_t overruns = 0;

    // 镜像 isrHandler：发送失败计入溢出统计
    bool sendFromISR(uint8_t pin) {
        if (count >= CAPACITY) {
            overruns++;
            return false;
        }
        buf[count++] = pin;
        return true;
    }
    bool receive(uint8_t& pin) {
        if (count == 0) return false;
        pin = buf[0];
        for (int i = 1; i < count; i++) buf[i - 1] = buf[i];
        count--;
        return true;
    }
};

static void test_isrqueue_no_loss_under_capacity() {
    MirrorIsrQueue q;
    for (uint8_t i = 0; i < 8; i++) {
        TEST_ASSERT_TRUE(q.sendFromISR(i));
    }
    TEST_ASSERT_EQUAL(0u, q.overruns);
}

static void test_isrqueue_overrun_counted_when_full() {
    MirrorIsrQueue q;
    for (uint8_t i = 0; i < 8; i++) q.sendFromISR(i);
    // 队列满后继续触发：事件丢失必须被计数（旧实现静默丢弃）
    for (uint8_t i = 0; i < 12; i++) {
        TEST_ASSERT_FALSE(q.sendFromISR(i));
    }
    TEST_ASSERT_EQUAL(12u, q.overruns);
}

static void test_isrqueue_drain_and_reset_stats() {
    MirrorIsrQueue q;
    for (uint8_t i = 0; i < 8; i++) q.sendFromISR(i);
    q.sendFromISR(99);  // 溢出一次
    TEST_ASSERT_EQUAL(1u, q.overruns);
    uint8_t pin;
    int drained = 0;
    while (q.receive(pin)) drained++;
    TEST_ASSERT_EQUAL(8, drained);
    // resetIsrQueueStats 语义：新采集周期计数清零
    q.overruns = 0;
    TEST_ASSERT_EQUAL(0u, q.overruns);
    // 排空后可继续接收新事件
    TEST_ASSERT_TRUE(q.sendFromISR(7));
}

// ============================================================
// TEST GROUP 7: Ticker 回调非阻塞加锁语义（镜像 blink/breathe 回调模式）
// ============================================================

struct MirrorRecursiveMutex {
    int holdCount = 0;         // 递归持锁计数
    bool busyByOtherTask = false;

    bool tryTakeRecursive() {  // xSemaphoreTakeRecursive(mtx, 0)
        if (busyByOtherTask) return false;
        holdCount++;
        return true;
    }
    void giveRecursive() {     // xSemaphoreGiveRecursive(mtx)
        if (holdCount > 0) holdCount--;
    }
};

static int mirror_blinkTick(MirrorRecursiveMutex& mtx, bool& pinState) {
    // 镜像 blinkTickerCallback：拿不到锁立即跳过，绝不阻塞定时器任务
    if (!mtx.tryTakeRecursive()) return 0;
    pinState = !pinState;
    mtx.giveRecursive();
    return 1;
}

static void test_ticker_skips_when_mutex_busy() {
    MirrorRecursiveMutex mtx;
    bool pin = false;
    mtx.busyByOtherTask = true;  // 模拟 loopTask 正持锁做长操作
    int acted = 0;
    for (int i = 0; i < 10; i++) acted += mirror_blinkTick(mtx, pin);
    // 锁忙期间：全部跳过（不阻塞、不动作），下一次重试再执行
    TEST_ASSERT_EQUAL(0, acted);
    TEST_ASSERT_FALSE(pin);
}

static void test_ticker_acts_when_mutex_free() {
    MirrorRecursiveMutex mtx;
    bool pin = false;
    int acted = 0;
    for (int i = 0; i < 10; i++) acted += mirror_blinkTick(mtx, pin);
    TEST_ASSERT_EQUAL(10, acted);
    TEST_ASSERT_FALSE(pin);  // 10 次翻转回到原状态
}

static void test_ticker_recursive_reentry_allowed() {
    // 镜像 blinkTickerCallback → togglePin 嵌套加锁：递归锁同任务重入必须成功
    MirrorRecursiveMutex mtx;
    TEST_ASSERT_TRUE(mtx.tryTakeRecursive());    // 回调外层加锁
    TEST_ASSERT_TRUE(mtx.tryTakeRecursive());    // togglePin 内部再加锁（递归）
    TEST_ASSERT_EQUAL(2, mtx.holdCount);
    mtx.giveRecursive();
    mtx.giveRecursive();
    TEST_ASSERT_EQUAL(0, mtx.holdCount);
}

// ============================================================
// TEST GROUP 8: attachInterrupt 引脚验证决策（镜像）
// ============================================================

// 镜像修复后的 attachInterrupt 前置检查：pin==255 或非法引脚拒绝注册
static bool mirror_attachInterruptAllowed(uint8_t pin, uint8_t maxGpio) {
    if (pin == 255) return false;
    if (pin > maxGpio) return false;
    return true;
}

static void test_attach_rejects_unassigned_pin() {
    TEST_ASSERT_FALSE(mirror_attachInterruptAllowed(255, 48));
}

static void test_attach_rejects_out_of_range_pin() {
    TEST_ASSERT_FALSE(mirror_attachInterruptAllowed(49, 48));
    TEST_ASSERT_FALSE(mirror_attachInterruptAllowed(200, 48));
}

static void test_attach_accepts_valid_pin() {
    TEST_ASSERT_TRUE(mirror_attachInterruptAllowed(0, 48));
    TEST_ASSERT_TRUE(mirror_attachInterruptAllowed(4, 48));
    TEST_ASSERT_TRUE(mirror_attachInterruptAllowed(48, 48));
}

// ============================================================
// TEST GROUP 9: 按类型差异化去抖（镜像 PeripheralManager::isrDebounceMsForType）
// ============================================================

// 镜像 PeripheralTypes.h 中的关键枚举值（避免 native 环境引入设备头文件）
namespace MirrorPeriphType {
    constexpr int GPIO_INTERRUPT_RISING  = 21;
    constexpr int GPIO_INTERRUPT_FALLING = 22;
    constexpr int GPIO_INTERRUPT_CHANGE  = 23;
    constexpr int RADAR_SENSOR           = 49;
    constexpr int RF_MODULE              = 48;
    constexpr int IR_RECEIVER            = 53;
}
constexpr uint16_t MIRROR_ISR_DEBOUNCE_MS = 5;

// 镜像生产实现：高速边沿外设返回 0 禁用去抖，其余默认 5ms
static uint16_t mirror_isrDebounceMsForType(int type) {
    switch (type) {
        case MirrorPeriphType::RF_MODULE:
        case MirrorPeriphType::IR_RECEIVER:
            return 0;
        default:
            return MIRROR_ISR_DEBOUNCE_MS;
    }
}

static void test_debounce_type_gpio_interrupt_default_window() {
    TEST_ASSERT_EQUAL_UINT32(5, mirror_isrDebounceMsForType(MirrorPeriphType::GPIO_INTERRUPT_RISING));
    TEST_ASSERT_EQUAL_UINT32(5, mirror_isrDebounceMsForType(MirrorPeriphType::GPIO_INTERRUPT_FALLING));
    TEST_ASSERT_EQUAL_UINT32(5, mirror_isrDebounceMsForType(MirrorPeriphType::GPIO_INTERRUPT_CHANGE));
}

static void test_debounce_type_radar_default_window() {
    // 雷达数字输出可去抖（状态型信号，非时序敏感）
    TEST_ASSERT_EQUAL_UINT32(5, mirror_isrDebounceMsForType(MirrorPeriphType::RADAR_SENSOR));
}

static void test_debounce_type_rf_ir_bypass() {
    // OOK/EV1527 与红外协议脉冲宽度在百 us 级，毫秒级合并会破坏解码
    TEST_ASSERT_EQUAL_UINT32(0, mirror_isrDebounceMsForType(MirrorPeriphType::RF_MODULE));
    TEST_ASSERT_EQUAL_UINT32(0, mirror_isrDebounceMsForType(MirrorPeriphType::IR_RECEIVER));
}

static void test_debounce_type_unknown_default_window() {
    TEST_ASSERT_EQUAL_UINT32(5, mirror_isrDebounceMsForType(0));
    TEST_ASSERT_EQUAL_UINT32(5, mirror_isrDebounceMsForType(99));
}

static void test_debounce_fast_type_dispatches_every_edge() {
    // RF 接收场景：1ms 内连续 8 个边沿，窗口=0 时全部派发（解码不丢边沿）
    uint16_t window = mirror_isrDebounceMsForType(MirrorPeriphType::RF_MODULE);
    unsigned long last = 0;
    int dispatched = 0;
    for (unsigned long t = 1; t <= 8; t++) {
        if (window == 0 || mirror_shouldDispatchIsrEvent(t, last, window)) {
            dispatched++;
            last = t;
        }
    }
    TEST_ASSERT_EQUAL(8, dispatched);
}

static void test_debounce_gpio_type_still_coalesces() {
    // 对照：同风暴下 GPIO 中断类（窗口 5ms）仍被合并
    uint16_t window = mirror_isrDebounceMsForType(MirrorPeriphType::GPIO_INTERRUPT_CHANGE);
    unsigned long last = 0;
    int dispatched = 0;
    bool first = true;
    for (unsigned long t = 0; t <= 8; t++) {
        if (first || mirror_shouldDispatchIsrEvent(t, last, window)) {
            dispatched++;
            last = t;
            first = false;
        }
    }
    TEST_ASSERT_EQUAL(2, dispatched);  // t=0 与 t=5
}

// ============================================================
// TEST GROUP 10: 回调快照语义（镜像 handleInterrupt 锁外执行回调）
// ============================================================

struct MirrorInterruptCtx {
    bool peripheralExists = true;
    bool hasCallback = true;
    bool callbackInvoked = false;
    bool callbackRanOutsideLock = false;
    int lockDepth = 0;

    // 镜像重构后的 handleInterrupt：锁内查找+去抖+快照，锁外执行回调
    void handleInterrupt() {
        std::function<void()> callback = nullptr;
        {
            lockDepth++;  // RecursiveMutexGuard 构造
            if (peripheralExists && hasCallback) {
                callback = [this]() {
                    callbackInvoked = true;
                    callbackRanOutsideLock = (lockDepth == 0);
                };
            }
            lockDepth--;  // Guard 析构释放锁
        }
        if (callback) callback();
    }
};

static void test_callback_snapshot_runs_outside_lock() {
    MirrorInterruptCtx ctx;
    ctx.handleInterrupt();
    TEST_ASSERT_TRUE(ctx.callbackInvoked);
    // 回调执行时锁必须已释放（避免用户回调长时间持锁阻塞其它任务）
    TEST_ASSERT_TRUE(ctx.callbackRanOutsideLock);
}

static void test_callback_snapshot_missing_peripheral_no_invoke() {
    MirrorInterruptCtx ctx;
    ctx.peripheralExists = false;
    ctx.handleInterrupt();
    TEST_ASSERT_FALSE(ctx.callbackInvoked);
    TEST_ASSERT_EQUAL(0, ctx.lockDepth);  // 锁必须平衡释放
}

static void test_callback_snapshot_null_callback_safe() {
    MirrorInterruptCtx ctx;
    ctx.hasCallback = false;
    ctx.handleInterrupt();
    TEST_ASSERT_FALSE(ctx.callbackInvoked);
    TEST_ASSERT_EQUAL(0, ctx.lockDepth);
}

// ============================================================
// TEST GROUP 11: 源码锁定防回归（HealthMonitor 接入 + handleInterrupt 结构）
// ============================================================

static std::string readOptSrcFile(const std::string& relativePath) {
    const char* roots[] = { ".", "..", "../..", "../../..", "../../../.." };
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

static void test_srclk_healthmonitor_exposes_isr_stats() {
    std::string src = readOptSrcFile("src/systems/HealthMonitor.cpp");
    if (src.empty()) {
        TEST_IGNORE_MESSAGE("HealthMonitor.cpp not readable, skipping source check");
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("core/PeripheralManager.h") != std::string::npos,
        "HealthMonitor.cpp must include core/PeripheralManager.h");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("queue_overruns") != std::string::npos,
        "getMetricsJson must expose isr.queue_overruns for storm diagnostics");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("queue_waiting") != std::string::npos,
        "getMetricsJson must expose isr.queue_waiting for storm diagnostics");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("getIsrQueueOverrunCount()") != std::string::npos,
        "HealthMonitor must read PeripheralManager::getIsrQueueOverrunCount()");
}

static void test_srclk_periphmanager_isr_hardening() {
    std::string src = readOptSrcFile("src/core/PeripheralManager.cpp");
    if (src.empty()) {
        TEST_IGNORE_MESSAGE("PeripheralManager.cpp not readable, skipping source check");
        return;
    }
    // 队列溢出必须被计数（不得回到静默丢弃）
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("_isrQueueOverruns++") != std::string::npos,
        "isrHandler must count queue overrun instead of silently dropping events");
    // 按类型去抖函数必须存在且 RF/IR 旁路
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("PeripheralManager::isrDebounceMsForType") != std::string::npos,
        "isrDebounceMsForType must be implemented");
    size_t fnPos = src.find("PeripheralManager::isrDebounceMsForType");
    std::string fnBody = src.substr(fnPos, 800);
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("PeripheralType::RF_MODULE") != std::string::npos &&
        fnBody.find("PeripheralType::IR_RECEIVER") != std::string::npos,
        "isrDebounceMsForType must bypass debounce for RF_MODULE and IR_RECEIVER");
    // handleInterrupt 必须持锁查找 + 快照回调后锁外执行
    size_t hiPos = src.find("void PeripheralManager::handleInterrupt");
    TEST_ASSERT_TRUE_MESSAGE(hiPos != std::string::npos, "handleInterrupt must exist");
    size_t hiEnd = src.find("\nbool PeripheralManager::", hiPos);
    std::string hiBody = src.substr(
        hiPos, hiEnd == std::string::npos ? std::string::npos : hiEnd - hiPos);
    TEST_ASSERT_TRUE_MESSAGE(
        hiBody.find("RecursiveMutexGuard lock(_mutex)") != std::string::npos,
        "handleInterrupt must hold recursive mutex during lookup/debounce");
    TEST_ASSERT_TRUE_MESSAGE(
        hiBody.find("callback = config.params.gpio.interruptCallback") != std::string::npos,
        "handleInterrupt must snapshot callback before releasing the lock");
    // attach/detach 线程安全：不得再使用锁外裸指针模式
    size_t attachPos = src.find("bool PeripheralManager::attachInterrupt(const String&");
    TEST_ASSERT_TRUE_MESSAGE(attachPos != std::string::npos, "attachInterrupt(String) must exist");
    std::string attachBody = src.substr(attachPos, 1400);
    TEST_ASSERT_TRUE_MESSAGE(
        attachBody.find("RecursiveMutexGuard lock(_mutex)") != std::string::npos,
        "attachInterrupt must hold the recursive mutex for the whole operation");
    TEST_ASSERT_TRUE_MESSAGE(
        attachBody.find("getPeripheral(peripheralId)") == std::string::npos,
        "attachInterrupt must not use out-of-lock raw pointer from getPeripheral");
    TEST_ASSERT_TRUE_MESSAGE(
        attachBody.find("isValidPin(pin)") != std::string::npos,
        "attachInterrupt must validate pin before registering the ISR");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("gpio_get_level") != std::string::npos &&
        src.find("sizeof(GpioIsrEvent)") != std::string::npos,
        "GPIO ISR queue must capture the edge level, not only the pin");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("popDigitalInputInterruptEvent") != std::string::npos &&
        src.find("digitalInputLogicalLevel") != std::string::npos,
        "captured GPIO interrupt events must be consumable in logical polarity");
    TEST_ASSERT_TRUE_MESSAGE(
        attachBody.find("invertLogic ? FALLING : RISING") != std::string::npos &&
        attachBody.find("invertLogic ? RISING : FALLING") != std::string::npos,
        "hardware edge selection must follow the configured logical polarity");
}

static void test_gpio_string_and_cstr_paths_share_core() {
    std::string src = readOptSrcFile("src/core/PeripheralManager.cpp");
    if (src.empty()) {
        TEST_IGNORE_MESSAGE("PeripheralManager.cpp not readable, skipping source check");
        return;
    }

    const size_t readStringPos = src.find("GPIOState PeripheralManager::readPin(const String&", 0);
    const size_t readCStrPos = src.find("GPIOState PeripheralManager::readPinByCStr", readStringPos);
    const size_t writeStringPos = src.find("bool PeripheralManager::writePin(const String&", 0);
    const size_t writeCStrPos = src.find("bool PeripheralManager::writePinByCStr", writeStringPos);

    TEST_ASSERT_TRUE_MESSAGE(readStringPos != std::string::npos && readCStrPos != std::string::npos,
                             "both GPIO read entry points must exist");
    TEST_ASSERT_TRUE_MESSAGE(writeStringPos != std::string::npos && writeCStrPos != std::string::npos,
                             "both GPIO write entry points must exist");

    const size_t nextRead = src.find("GPIOState PeripheralManager::readPinByCStr", readStringPos);
    const size_t nextWrite = src.find("bool PeripheralManager::writePinByCStr", writeStringPos);
    const std::string readStringBody = src.substr(readStringPos, nextRead - readStringPos);
    const std::string readCStrBody = src.substr(readCStrPos, writeStringPos - readCStrPos);
    const std::string writeStringBody = src.substr(writeStringPos, nextWrite - writeStringPos);
    const std::string writeCStrBody = src.substr(writeCStrPos, 1800);

    TEST_ASSERT_TRUE_MESSAGE(
        readStringBody.find("readGpioStateLocked(*config, &peripheralId, nullptr)") != std::string::npos &&
        readCStrBody.find("readGpioStateLocked(*config, nullptr, peripheralId)") != std::string::npos,
        "String and C-string read paths must share the locked GPIO core");
    TEST_ASSERT_TRUE_MESSAGE(
        writeStringBody.find("writeGpioStateLocked(*config, &peripheralId, nullptr, state)") != std::string::npos &&
        writeCStrBody.find("writeGpioStateLocked(*config, nullptr, peripheralId, state)") != std::string::npos,
        "String and C-string write paths must share the locked GPIO core");

    const size_t readCorePos = src.find("GPIOState PeripheralManager::readGpioStateLocked");
    const size_t writeCorePos = src.find("bool PeripheralManager::writeGpioStateLocked");
    TEST_ASSERT_TRUE_MESSAGE(readCorePos != std::string::npos && writeCorePos != std::string::npos,
                             "shared GPIO cores must be implemented");
    const std::string readCore = src.substr(readCorePos, writeCorePos - readCorePos);
    const std::string writeCore = src.substr(writeCorePos, 3200);
    TEST_ASSERT_TRUE_MESSAGE(
        readCore.find("isReservedPin(pin)") != std::string::npos &&
        readCore.find("digitalRead(pin)") != std::string::npos &&
        readCore.find("analogRead(pin)") != std::string::npos &&
        readCore.find("getRuntimeStateCStrLocked") != std::string::npos,
        "shared read core must preserve GPIO input, reserved-pin and runtime-state behavior");
    TEST_ASSERT_TRUE_MESSAGE(
        writeCore.find("digitalWrite(pin") != std::string::npos &&
        writeCore.find("ledcWrite") != std::string::npos &&
        writeCore.find("getRuntimeStateCStrLocked") != std::string::npos,
        "shared write core must preserve digital, PWM and runtime-state behavior");
}

// ============================================================
// 主入口
// ============================================================

void test_peripheral_manager_opt_group() {
    TestLog::groupStart("PeripheralManager Optimization Tests");

    // StaticSlabPool
    RUN_TEST(test_slabpool_constants);
    RUN_TEST(test_slabpool_distinct_addresses);
    RUN_TEST(test_slabpool_overflow_fallback_malloc);
    RUN_TEST(test_slabpool_reuse_after_free);
    RUN_TEST(test_slabpool_peak_tracking);
    RUN_TEST(test_slabpool_deallocate_null_safe);

    // PooledAllocator + std::map
    RUN_TEST(test_pooled_map_insert_uses_pool);
    RUN_TEST(test_pooled_map_erase_reclaims_pool);
    RUN_TEST(test_pooled_map_churn_no_leak);
    RUN_TEST(test_pooled_map_exhaustion_graceful);
    RUN_TEST(test_pooled_map_oversized_node_fallback);
    RUN_TEST(test_pooled_shared_pool_across_types);
    RUN_TEST(test_pooled_allocator_rebind_and_equality);

    // SmallNodePool 预设
    RUN_TEST(test_smallnode_preset_constants);
    RUN_TEST(test_smallnode_small_map_hits_pool);

    // 性能基准
    RUN_TEST(test_pool_vs_malloc_benchmark);

    // ISR 去抖
    RUN_TEST(test_debounce_first_event_dispatches);
    RUN_TEST(test_debounce_within_window_suppressed);
    RUN_TEST(test_debounce_boundary_dispatches);
    RUN_TEST(test_debounce_millis_wraparound);
    RUN_TEST(test_debounce_zero_window_always_dispatches);
    RUN_TEST(test_debounce_storm_coalescing);
    RUN_TEST(test_analog_quality_classification);
    RUN_TEST(test_peripheral_config_destructor_smoke);
    RUN_TEST(test_analog_raw_conversion_boundaries);
    RUN_TEST(test_inverted_interrupt_edges_use_logical_level);
    RUN_TEST(test_digital_input_defaults_and_report_modes_use_production_helpers);
    RUN_TEST(test_analog_output_percent_mapping_uses_production_helper);

    // ISR 队列溢出
    RUN_TEST(test_isrqueue_no_loss_under_capacity);
    RUN_TEST(test_isrqueue_overrun_counted_when_full);
    RUN_TEST(test_isrqueue_drain_and_reset_stats);

    // Ticker 非阻塞加锁
    RUN_TEST(test_ticker_skips_when_mutex_busy);
    RUN_TEST(test_ticker_acts_when_mutex_free);
    RUN_TEST(test_ticker_recursive_reentry_allowed);

    // 引脚验证
    RUN_TEST(test_attach_rejects_unassigned_pin);
    RUN_TEST(test_attach_rejects_out_of_range_pin);
    RUN_TEST(test_attach_accepts_valid_pin);

    // 按类型差异化去抖
    RUN_TEST(test_debounce_type_gpio_interrupt_default_window);
    RUN_TEST(test_debounce_type_radar_default_window);
    RUN_TEST(test_debounce_type_rf_ir_bypass);
    RUN_TEST(test_debounce_type_unknown_default_window);
    RUN_TEST(test_debounce_fast_type_dispatches_every_edge);
    RUN_TEST(test_debounce_gpio_type_still_coalesces);

    // 回调快照语义
    RUN_TEST(test_callback_snapshot_runs_outside_lock);
    RUN_TEST(test_callback_snapshot_missing_peripheral_no_invoke);
    RUN_TEST(test_callback_snapshot_null_callback_safe);

    // 源码锁定防回归
    RUN_TEST(test_srclk_healthmonitor_exposes_isr_stats);
    RUN_TEST(test_srclk_periphmanager_isr_hardening);
    RUN_TEST(test_gpio_string_and_cstr_paths_share_core);

    TestLog::groupEnd();
}
