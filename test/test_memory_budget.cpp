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

void test_memory_budget_group();

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

    TEST_ASSERT_EQUAL_UINT32(6144U, MemoryBudget::MQTTS_RECONNECT_TASK_STACK);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(4096U, MemoryBudget::MQTTS_RECONNECT_TASK_STACK);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(6144U, MemoryBudget::MQTTS_RECONNECT_TASK_STACK);
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

void test_memory_budget_group() {
    TestLog::groupStart("MemoryBudget Tests");
    RUN_TEST(test_mqtts_budget_gate_boundaries);
    RUN_TEST(test_mqtts_field_observed_s3_boot_budget);
    RUN_TEST(test_mqtts_field_observed_post_mqtt_restore_budget);
    RUN_TEST(test_mqtts_field_observed_ethernet_budget);
    RUN_TEST(test_mqtts_field_observed_ethernet_restore_budget);
    RUN_TEST(test_mqtts_reconnect_task_stack_budget);
    RUN_TEST(test_guard_level_uses_internal_dram_budgets);
    // Task 4: 内存池管理优化测试
    RUN_TEST(test_modbus_buffer_pool_size);
    RUN_TEST(test_mqtt_cmd_queue_memory_limit);
    // Task 3: 大对象延迟加载测试
    RUN_TEST(test_protocol_save_uses_psram_json);
    TestLog::groupEnd();
}
