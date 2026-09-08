/**
 * @file test_memory_budget.cpp
 * @brief Central memory budget tests for WiFi/MQTTS stability.
 */

#include <unity.h>
#include <Arduino.h>
#include "core/MemoryBudget.h"
#include "helpers/TestLogger.h"
#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>

void test_memory_budget_group();

static std::string readProjectFile(const char* relativePath) {
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

void test_mqtts_budget_gate_boundaries() {
    TestLog::testStart("MemoryBudget: MQTTS Gate Boundaries");

    using FastBee::MemoryBudget;

    TEST_ASSERT_FALSE(MemoryBudget::canAttemptMqtts(
        MemoryBudget::MQTTS_MIN_DRAM_FREE - 1,
        MemoryBudget::MQTTS_MIN_LARGEST_BLOCK));
    TEST_ASSERT_FALSE(MemoryBudget::canAttemptMqtts(
        MemoryBudget::MQTTS_MIN_DRAM_FREE,
        MemoryBudget::MQTTS_MIN_LARGEST_BLOCK - 1));
    TEST_ASSERT_TRUE(MemoryBudget::canAttemptMqtts(
        MemoryBudget::MQTTS_MIN_DRAM_FREE,
        MemoryBudget::MQTTS_MIN_LARGEST_BLOCK));

    TEST_ASSERT_TRUE(MemoryBudget::shouldReclaimBeforeMqtts(
        MemoryBudget::MQTTS_READY_DRAM_FREE - 1,
        MemoryBudget::MQTTS_READY_LARGEST_BLOCK));
    TEST_ASSERT_TRUE(MemoryBudget::shouldReclaimBeforeMqtts(
        MemoryBudget::MQTTS_READY_DRAM_FREE,
        MemoryBudget::MQTTS_READY_LARGEST_BLOCK - 1));
    TEST_ASSERT_FALSE(MemoryBudget::shouldReclaimBeforeMqtts(
        MemoryBudget::MQTTS_READY_DRAM_FREE,
        MemoryBudget::MQTTS_READY_LARGEST_BLOCK));

    TEST_ASSERT_TRUE(MemoryBudget::shouldPauseWebBeforeMqtts(
        MemoryBudget::MQTTS_WEB_PAUSE_DRAM_FREE - 1,
        MemoryBudget::MQTTS_WEB_PAUSE_LARGEST_BLOCK));
    TEST_ASSERT_TRUE(MemoryBudget::shouldPauseWebBeforeMqtts(
        MemoryBudget::MQTTS_WEB_PAUSE_DRAM_FREE,
        MemoryBudget::MQTTS_WEB_PAUSE_LARGEST_BLOCK - 1));
    TEST_ASSERT_FALSE(MemoryBudget::shouldPauseWebBeforeMqtts(
        MemoryBudget::MQTTS_WEB_PAUSE_DRAM_FREE,
        MemoryBudget::MQTTS_WEB_PAUSE_LARGEST_BLOCK));

    TestLog::testEnd(true);
}

void test_mqtts_field_observed_s3_boot_budget() {
    TestLog::testStart("MemoryBudget: MQTTS Field Observed S3 Boot Budget");

    using FastBee::MemoryBudget;

    // Serial validation on esp32s3-F16R8 showed MQTTS boot attempts at roughly
    // 43KB internal DRAM free and a 32756-byte largest block. This state must
    // be allowed to attempt TLS, but it should still pause Web/SSE first
    // because Huawei Cloud's RSA path needs more BIGNUM headroom.
    TEST_ASSERT_TRUE(MemoryBudget::canAttemptMqtts(43492U, 32756U));
    TEST_ASSERT_TRUE(MemoryBudget::shouldReclaimBeforeMqtts(43492U, 32756U));
    TEST_ASSERT_FALSE(MemoryBudget::shouldReclaimBeforeMqtts(56000U, 43000U));

    TestLog::testEnd(true);
}

void test_mqtts_field_observed_post_mqtt_restore_budget() {
    TestLog::testStart("MemoryBudget: MQTTS Post-MQTT Restore Budget");

    using FastBee::MemoryBudget;

    // After a plain MQTT test restored the saved mqtts:// profile on
    // esp32s3-F16R8, serial logs showed 45416 bytes of internal DRAM and a
    // 28660-byte largest block. With mbedTLS large allocations routed to PSRAM,
    // this state should try TLS instead of entering a 300s low-DRAM backoff.
    TEST_ASSERT_TRUE(MemoryBudget::canAttemptMqtts(45416U, 28660U));
    TEST_ASSERT_TRUE(MemoryBudget::shouldReclaimBeforeMqtts(45416U, 28660U));

    TestLog::testEnd(true);
}

void test_mqtts_field_observed_ethernet_budget() {
    TestLog::testStart("MemoryBudget: MQTTS Ethernet Budget");

    using FastBee::MemoryBudget;

    // Ethernet/W5500 hybrid mode on esp32s3-F16R8 keeps the Web config AP
    // online and showed about 36KB internal DRAM with a 27636-byte largest
    // block. That is below the comfort zone, so it should reclaim first, but
    // it must not enter a 300s low-DRAM backoff before trying TLS.
    TEST_ASSERT_TRUE(MemoryBudget::canAttemptMqtts(36392U, 27636U));
    TEST_ASSERT_TRUE(MemoryBudget::shouldReclaimBeforeMqtts(36392U, 27636U));

    TestLog::testEnd(true);
}

void test_mqtts_field_observed_ethernet_restore_budget() {
    TestLog::testStart("MemoryBudget: MQTTS Ethernet Restore Budget");

    using FastBee::MemoryBudget;

    // After Ethernet plain MQTT 1883 test restore on esp32s3-F16R8, detailed
    // reconnect logs showed roughly 36KB internal DRAM and a 17396-byte largest
    // block. With large mbedTLS buffers routed to PSRAM this must attempt TLS
    // instead of parking MQTTS in a 300s low-DRAM backoff.
    TEST_ASSERT_TRUE(MemoryBudget::canAttemptMqtts(42980U, 23540U));
    TEST_ASSERT_TRUE(MemoryBudget::shouldReclaimBeforeMqtts(42980U, 23540U));
    TEST_ASSERT_TRUE(MemoryBudget::canAttemptMqtts(36212U, 17396U));
    TEST_ASSERT_TRUE(MemoryBudget::shouldReclaimBeforeMqtts(36212U, 17396U));
    TEST_ASSERT_TRUE(MemoryBudget::canRetryMqttsMemoryRecovery(36212U, 17396U));

    TestLog::testEnd(true);
}

void test_mqtts_reconnect_task_stack_budget() {
    TestLog::testStart("MemoryBudget: MQTTS Reconnect Task Stack Budget");

    using FastBee::MemoryBudget;

    // 审计 P1-3：6144 为边缘值，TLS 握手栈峰值可能突破，保守提升至 8192
    TEST_ASSERT_EQUAL_UINT32(8192U, MemoryBudget::MQTTS_RECONNECT_TASK_STACK);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(4096U, MemoryBudget::MQTTS_RECONNECT_TASK_STACK);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(8192U, MemoryBudget::MQTTS_RECONNECT_TASK_STACK);
    TEST_ASSERT_EQUAL_UINT32(10000UL, MemoryBudget::MQTTS_MEMORY_RECOVERY_RETRY_MS);

    TestLog::testEnd(true);
}

void test_guard_level_uses_internal_dram_budgets() {
    TestLog::testStart("MemoryBudget: Guard Level DRAM Budgets");

    using FastBee::MemoryBudget;
    using FastBee::MemoryPressureLevel;

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(MemoryPressureLevel::NORMAL),
        static_cast<uint8_t>(MemoryBudget::guardLevelForDram(60000, 45000, 5, false)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(MemoryPressureLevel::WARN),
        static_cast<uint8_t>(MemoryBudget::guardLevelForDram(28000, 20000, 5, false)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(MemoryPressureLevel::SEVERE),
        static_cast<uint8_t>(MemoryBudget::guardLevelForDram(24000, 20000, 5, false)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(MemoryPressureLevel::CRITICAL),
        static_cast<uint8_t>(MemoryBudget::guardLevelForDram(16000 - 1, 20000, 5, false)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(MemoryPressureLevel::CRITICAL),
        static_cast<uint8_t>(MemoryBudget::guardLevelForDram(60000, 8192 - 1, 5, false)));

    TestLog::testEnd(true);
}

void test_guard_level_uses_dynamic_critical_floor() {
    TestLog::testStart("MemoryBudget: Dynamic Critical DRAM Floor");

    using FastBee::MemoryBudget;
    using FastBee::MemoryPressureLevel;

    // ESP32-F4R0 full peripheral bundle observed this stable baseline on-device.
    // It must remain SEVERE (controlled degradation), not CRITICAL/rebooting.
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(MemoryPressureLevel::SEVERE),
        static_cast<uint8_t>(MemoryBudget::guardLevelForFreeDram(14232U, 8192U)));

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(MemoryPressureLevel::CRITICAL),
        static_cast<uint8_t>(MemoryBudget::guardLevelForFreeDram(8191U, 8192U)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(MemoryPressureLevel::SEVERE),
        static_cast<uint8_t>(MemoryBudget::guardLevelForFreeDram(8192U, 8192U)));

    // The PSRAM tier retains its stricter 12KB floor.
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(MemoryPressureLevel::CRITICAL),
        static_cast<uint8_t>(MemoryBudget::guardLevelForFreeDram(12287U, 12288U)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(MemoryPressureLevel::SEVERE),
        static_cast<uint8_t>(MemoryBudget::guardLevelForFreeDram(12288U, 12288U)));

    TestLog::testEnd(true);
}

// ========== Task 4 内存池管理优化测试 ==========

/**
 * @brief Modbus 缓冲池大小验证（基于 PSRAM 配置）
 * 验证 PSRAM 设备池大小为 8，无 PSRAM 为 4
 */
void test_modbus_buffer_pool_size() {
    TestLog::testStart("Modbus: Buffer Pool Size");

    // 验证常量定义
    std::string header;
    {
        std::ifstream f("include/core/PeriphExecExecutor.h");
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            header = ss.str();
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(!header.empty(), "PeriphExecExecutor.h must be readable");

    // 验证 PSRAM 条件编译
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("#if defined(BOARD_HAS_PSRAM)") != std::string::npos,
        "Modbus buffer pool must have PSRAM conditional compilation");
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("MODBUS_BUFFER_POOL_SIZE = 8") != std::string::npos,
        "PSRAM devices must have 8 buffers");
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("MODBUS_BUFFER_POOL_SIZE = 4") != std::string::npos,
        "Non-PSRAM devices must have 4 buffers");
    TestLog::step("PSRAM conditional pool sizes verified");

    TestLog::testEnd(true);
}

/**
 * @brief MQTT 命令队列内存限制验证
 * 验证 MQTTClient.h 中定义了队列内存上限常量和跟踪变量
 */
void test_mqtt_cmd_queue_memory_limit() {
    TestLog::testStart("MQTT: Command Queue Memory Limit");

    std::string header;
    {
        std::ifstream f("include/protocols/MQTTClient.h");
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            header = ss.str();
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(!header.empty(), "MQTTClient.h must be readable");

    // 1. 队列内存上限常量
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("MQTT_CMD_QUEUE_MAX_BYTES") != std::string::npos,
        "MQTTClient.h must define MQTT_CMD_QUEUE_MAX_BYTES");
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("4096U") != std::string::npos,
        "MQTT_CMD_QUEUE_MAX_BYTES must be 4096U");
    TestLog::step("MQTT_CMD_QUEUE_MAX_BYTES = 4096U defined");

    // 2. 队列内存跟踪变量
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("_cmdQueueTotalBytes") != std::string::npos,
        "MQTTClient.h must track queue memory usage");
    TestLog::step("_cmdQueueTotalBytes tracking variable exists");

    // 3. 验证入队时有内存限制检查
    std::string source;
    {
        std::ifstream f("src/protocols/MQTTClient.cpp");
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            source = ss.str();
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(
        source.find("_cmdQueueTotalBytes + msgLen > MQTT_CMD_QUEUE_MAX_BYTES") != std::string::npos,
        "MQTTClient.cpp must check queue memory limit before enqueue");
    TestLog::step("Enqueue memory limit check exists");

    TestLog::testEnd(true);
}

/**
 * @brief ProtocolRouteHandler 使用 PSRAM JSON 分配器验证
 * 验证保存配置时使用 makeJsonDocument 而非栈上 JsonDocument
 */
void test_protocol_save_uses_psram_json() {
    TestLog::testStart("Protocol: Save Uses PSRAM JSON");

    std::string source;
    {
        std::ifstream f("src/network/handlers/ProtocolRouteHandler.cpp");
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            source = ss.str();
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(!source.empty(), "ProtocolRouteHandler.cpp must be readable");

    // 验证 handleSaveProtocolConfig 使用 makeJsonDocument
    TEST_ASSERT_TRUE_MESSAGE(
        source.find("FastBee::makeJsonDocument") != std::string::npos,
        "handleSaveProtocolConfig must use FastBee::makeJsonDocument for PSRAM allocation");
    TestLog::step("Protocol save uses PSRAM JSON allocator");

    TestLog::testEnd(true);
}

// ========== 8BIT 口径 Web 压力检测 ==========

/**
 * @brief isSevereWebPressure8() 边界值测试
 * 验证 8BIT 口径的 Web 压力检测函数在各阈值组合下返回正确结果。
 * 背景：旧 isSevereWebPressure() 使用 INTERNAL 口径阈值（24KB/12KB），
 * INTERNAL 含 IRAM/RTC 不可 malloc 内存虚高 ~28KB，导致 8BIT DRAM 已耗尽
 * 时仍报"健康"，WebConfigManager 自动恢复永不触发。
 */
void test_severe_web_pressure8_boundary_values() {
    TestLog::testStart("MemoryBudget: isSevereWebPressure8() boundary values");

    using FastBee::MemoryBudget;

    // ── 常量值校验 ──
    TEST_ASSERT_EQUAL_UINT32(8192U, MemoryBudget::GUARD8_SEVERE_DRAM_FREE);
    TEST_ASSERT_EQUAL_UINT32(3072U, MemoryBudget::GUARD8_SEVERE_LARGEST_BLOCK);
    TEST_ASSERT_EQUAL_UINT32(8192U, MemoryBudget::GUARD8_WARN_LARGEST_BLOCK);
    TestLog::step("GUARD8_* constants: 8KB/3KB/8KB");

    // ── free8 < 8KB → 严重压力（不管 blk8 多大） ──
    TEST_ASSERT_TRUE(MemoryBudget::isSevereWebPressure8(
        8191, 50000, 10));   // free8 差 1 字节
    TEST_ASSERT_TRUE(MemoryBudget::isSevereWebPressure8(
        0, 50000, 0));       // 完全耗尽
    TestLog::step("free8 < 8KB → severe (even with large blk8)");

    // ── blk8 < 3KB → 严重压力（不管 free8 多大） ──
    TEST_ASSERT_TRUE(MemoryBudget::isSevereWebPressure8(
        50000, 3071, 10));   // blk8 差 1 字节
    TEST_ASSERT_TRUE(MemoryBudget::isSevereWebPressure8(
        50000, 0, 0));       // 无连续块
    TestLog::step("blk8 < 3KB → severe (even with large free8)");

    // ── 碎片率 ≥ 65% 且 blk8 < 8KB → 严重压力 ──
    TEST_ASSERT_TRUE(MemoryBudget::isSevereWebPressure8(
        20000, 7000, 65));   // 碎片率刚达阈值 + blk8 < 8KB
    TEST_ASSERT_TRUE(MemoryBudget::isSevereWebPressure8(
        20000, 4000, 90));   // 高碎片 + 小块
    TestLog::step("frag≥65% AND blk8<8KB → severe");

    // ── 碎片率 ≥ 65% 但 blk8 ≥ 8KB → 不触发 ──
    TEST_ASSERT_FALSE(MemoryBudget::isSevereWebPressure8(
        20000, 8192, 65));   // 碎片率高但块够大
    TestLog::step("frag≥65% but blk8≥8KB → NOT severe");

    // ── 碎片率 < 65% 且 free8/blk8 都充足 → 正常 ──
    TEST_ASSERT_FALSE(MemoryBudget::isSevereWebPressure8(
        26000, 13500, 48));  // 开机稳态
    TEST_ASSERT_FALSE(MemoryBudget::isSevereWebPressure8(
        14000, 8500, 39));   // MQTT 稳态
    TestLog::step("healthy state → NOT severe");

    // ── 精确边界：free8 刚好 = 8KB → 不触发（< 而非 ≤） ──
    TEST_ASSERT_FALSE(MemoryBudget::isSevereWebPressure8(
        8192, 50000, 10));
    TestLog::step("free8=8192 exactly → NOT severe (< not <=)");

    // ── 精确边界：blk8 刚好 = 3KB → 不触发 ──
    TEST_ASSERT_FALSE(MemoryBudget::isSevereWebPressure8(
        50000, 3072, 10));
    TestLog::step("blk8=3072 exactly → NOT severe (< not <=)");

    // ── 实测故障场景回归：AP 模式 1.5h 泄漏后 ──
    // free8=7920, blk8=1460, frag≈82% → 必须检测到严重压力
    TEST_ASSERT_TRUE(MemoryBudget::isSevereWebPressure8(7920, 1460, 82));
    TestLog::step("regression: AP 1.5h leak (free8=7.7KB blk8=1.4KB) → severe");

    // ── 对比旧函数在 INTERNAL 口径下漏报 ──
    // INTERNAL 值：free=36KB, largest=29KB → 旧函数返回 false（漏报）
    TEST_ASSERT_FALSE(MemoryBudget::isSevereWebPressure(36000, 29000, 20));
    // 同一设备的 8BIT 值：free=7.9KB, blk8=1.4KB → 新函数返回 true（正确）
    TEST_ASSERT_TRUE(MemoryBudget::isSevereWebPressure8(7900, 1400, 82));
    TestLog::step("old INTERNAL=false (miss), new 8BIT=true (correct)");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：WebConfigManager 维护函数使用 MALLOC_CAP_8BIT 口径
 * 验证 severe pressure 检测已切换为 8BIT，不再用虚高的 INTERNAL。
 */
void test_source_webconfig_maintenance_uses_8bit() {
    TestLog::testStart("Source: WebConfigManager maintenance uses MALLOC_CAP_8BIT");

    std::string content = readProjectFile("src/network/WebConfigManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "WebConfigManager.cpp must be readable");

    // 1. 维护函数必须调用 isSevereWebPressure8（非旧的 isSevereWebPressure）
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("isSevereWebPressure8") != std::string::npos,
        "maintenance must call isSevereWebPressure8 (8BIT caliber)");
    TestLog::step("isSevereWebPressure8 called in maintenance");

    // 2. 堆测量必须使用 MALLOC_CAP_8BIT
    //    在 severe pressure 检测附近找 MALLOC_CAP_8BIT
    auto pressurePos = content.find("isSevereWebPressure8");
    TEST_ASSERT_TRUE_MESSAGE(pressurePos != std::string::npos,
        "isSevereWebPressure8 must be present");
    // 向前搜索 500 字符内的堆测量调用
    std::string context = content.substr(
        pressurePos > 500 ? pressurePos - 500 : 0,
        pressurePos > 500 ? 600 : pressurePos + 100);
    TEST_ASSERT_TRUE_MESSAGE(
        context.find("MALLOC_CAP_8BIT") != std::string::npos,
        "heap measurement near isSevereWebPressure8 must use MALLOC_CAP_8BIT");
    TestLog::step("MALLOC_CAP_8BIT used for heap measurement");

    // 3. 不应在 isSevereWebPressure8 附近使用 MALLOC_CAP_INTERNAL
    TEST_ASSERT_TRUE_MESSAGE(
        context.find("MALLOC_CAP_INTERNAL") == std::string::npos,
        "maintenance pressure check must NOT use MALLOC_CAP_INTERNAL (over-reports ~28KB)");
    TestLog::step("MALLOC_CAP_INTERNAL not used near pressure check");

    TestLog::testEnd(true);
}

// ========== Profile 可配置化重构守卫（计划五.2） ==========

/**
 * ASYNC_TCP 参数守卫：连接数/队列被调大会在并发连接风暴下抽干 DRAM。
 * ESP32: MAX=6（浏览器每 host 6 连接，多出的只是小请求堆占）QUEUE=32；
 * C3: MAX=4（内存更小）。
 */
void test_async_tcp_platformio_guard() {
    TestLog::testStart("MemoryBudget: ASYNC_TCP platformio guard");

    std::string pio = readProjectFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(pio.empty(), "platformio.ini must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        pio.find("-DCONFIG_ASYNC_TCP_MAX_CONNECTIONS=6") != std::string::npos,
        "esp32/c6 runtime flags must keep CONFIG_ASYNC_TCP_MAX_CONNECTIONS=6");
    TEST_ASSERT_TRUE_MESSAGE(
        pio.find("-DCONFIG_ASYNC_TCP_QUEUE_SIZE=32") != std::string::npos,
        "esp32 runtime flags must keep CONFIG_ASYNC_TCP_QUEUE_SIZE=32");
    TEST_ASSERT_TRUE_MESSAGE(
        pio.find("-DCONFIG_ASYNC_TCP_MAX_CONNECTIONS=4") != std::string::npos,
        "esp32c3 runtime flags must keep CONFIG_ASYNC_TCP_MAX_CONNECTIONS=4");

    TestLog::testEnd(true);
}

/**
 * PeriphExec 资源守卫：MAX_ACTIVE_TASKS 被放宽或检查周期被缩短都会
 * 推高后台任务的 DRAM/CPU 压力。
 */
void test_periph_exec_resource_guard() {
    TestLog::testStart("MemoryBudget: PeriphExec resource guard");

    std::string header = readProjectFile("include/core/PeriphExecScheduler.h");
    TEST_ASSERT_FALSE_MESSAGE(header.empty(), "PeriphExecScheduler.h must be readable");

    auto parseConst = [&header](const char* name) -> long {
        size_t pos = header.find(name);
        if (pos == std::string::npos) return -1;
        pos = header.find('=', pos);
        if (pos == std::string::npos) return -1;
        return strtol(header.c_str() + pos + 1, nullptr, 10);
    };

    long maxTasks = parseConst("MAX_ACTIVE_TASKS");
    TEST_ASSERT_TRUE_MESSAGE(
        maxTasks > 0 && maxTasks <= 12,
        "MAX_ACTIVE_TASKS must stay within (0, 12] to bound PeriphExec DRAM");

    long checkPeriod = parseConst("CHECK_PERIOD_NORMAL_MS");
    TEST_ASSERT_TRUE_MESSAGE(
        checkPeriod >= 1000,
        "CHECK_PERIOD_NORMAL_MS must stay >= 1000ms to limit background churn");

    TestLog::testEnd(true);
}

/**
 * 静态资源并发门控守卫：STATIC_MAX_INFLIGHT=3 是登录页首屏加载与
 * DRAM 占用之间的实测平衡值（2->3 修复首屏 >5s，继续调大将压穿 DRAM）。
 */
void test_static_max_inflight_guard() {
    TestLog::testStart("MemoryBudget: STATIC_MAX_INFLIGHT guard");

    std::string src = readProjectFile("src/network/WebHandlerContext.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "WebHandlerContext.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("STATIC_MAX_INFLIGHT = 3") != std::string::npos,
        "STATIC_MAX_INFLIGHT must stay 3 (static gz transfer concurrency cap)");

    TestLog::testEnd(true);
}

/**
 * FeatureFlags 完整性守卫：新增宏必须在 platformio.ini 中至少一个
 * profile/base 段显式配置，避免新宏遗漏导致各版本行为漂移。
 * 例外：按需覆盖宏（DS1302/LCD1602）设计上不写入 ini。
 */
void test_feature_flags_ini_coverage() {
    TestLog::testStart("MemoryBudget: FeatureFlags platformio coverage");

    std::string flags = readProjectFile("include/core/FeatureFlags.h");
    std::string pio = readProjectFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(flags.empty(), "FeatureFlags.h must be readable");
    TEST_ASSERT_FALSE_MESSAGE(pio.empty(), "platformio.ini must be readable");

    static const char* kOnDemand[] = { "FASTBEE_ENABLE_DS1302", "FASTBEE_ENABLE_LCD1602" };

    std::istringstream ss(flags);
    std::string line;
    while (std::getline(ss, line)) {
        size_t pos = line.find("#ifndef FASTBEE_ENABLE_");
        if (pos == std::string::npos) continue;
        std::string macro = line.substr(pos + 8);
        while (!macro.empty() && (macro.back() == '\r' || macro.back() == ' ')) {
            macro.pop_back();
        }
        bool onDemand = false;
        for (const char* name : kOnDemand) {
            if (macro == name) { onDemand = true; break; }
        }
        if (onDemand) continue;
        std::string flag = "-D" + macro + "=";
        TEST_ASSERT_TRUE_MESSAGE(
            pio.find(flag) != std::string::npos,
            (macro + " must be explicitly configured in platformio.ini").c_str());
    }

    TestLog::testEnd(true);
}

void test_memory_budget_group() {
    TestLog::groupStart("MemoryBudget Tests");
    RUN_TEST(test_mqtts_budget_gate_boundaries);
    RUN_TEST(test_mqtts_field_observed_s3_boot_budget);
    RUN_TEST(test_mqtts_field_observed_post_mqtt_restore_budget);
    RUN_TEST(test_mqtts_field_observed_ethernet_budget);
    RUN_TEST(test_mqtts_field_observed_ethernet_restore_budget);
    RUN_TEST(test_mqtts_reconnect_task_stack_budget);
    RUN_TEST(test_guard_level_uses_internal_dram_budgets);
    RUN_TEST(test_guard_level_uses_dynamic_critical_floor);
    // Task 4: 内存池管理优化测试
    RUN_TEST(test_modbus_buffer_pool_size);
    RUN_TEST(test_mqtt_cmd_queue_memory_limit);
    // Task 3: 大对象延迟加载测试
    RUN_TEST(test_protocol_save_uses_psram_json);
    // 8BIT 口径 Web 压力检测
    RUN_TEST(test_severe_web_pressure8_boundary_values);
    RUN_TEST(test_source_webconfig_maintenance_uses_8bit);
    // Profile 可配置化重构守卫（计划五.2）
    RUN_TEST(test_async_tcp_platformio_guard);
    RUN_TEST(test_periph_exec_resource_guard);
    RUN_TEST(test_static_max_inflight_guard);
    RUN_TEST(test_feature_flags_ini_coverage);
    TestLog::groupEnd();
}
