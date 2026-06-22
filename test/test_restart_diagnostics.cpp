/**
 * @file test_restart_diagnostics.cpp
 * @brief RestartDiagnostics 重启诊断单元测试
 * 
 * 测试内容：
 * - PreRestartSnapshot 结构体完整性（magic/checksum）
 * - calculateChecksum XOR 校验算法
 * - RestartReason 枚举全覆盖
 * - getResetReasonString（ESP系统级重启原因）
 * - getRestartReasonString（FastBee自定义重启原因）
 * - wasAbnormalRestart 判定逻辑
 */

#include <unity.h>
#include <Arduino.h>
#include <cstdint>
#include <cstring>
#include <cstddef>

void test_restart_diagnostics_group();

// ========== 内联模拟 ESP-IDF 依赖 ==========

// 镜像 esp_system.h 中的 esp_reset_reason_t
enum esp_reset_reason_t {
    ESP_RST_UNKNOWN  = 0,
    ESP_RST_POWERON  = 1,
    ESP_RST_EXT      = 2,
    ESP_RST_SW       = 3,
    ESP_RST_PANIC    = 4,
    ESP_RST_INT_WDT  = 5,
    ESP_RST_TASK_WDT = 6,
    ESP_RST_WDT      = 7,
    ESP_RST_DEEPSLEEP= 8,
    ESP_RST_BROWNOUT = 9,
    ESP_RST_SDIO     = 10
};

// 测试用全局变量模拟 esp_reset_reason() 返回值
static esp_reset_reason_t g_mockResetReason = ESP_RST_POWERON;
static esp_reset_reason_t test_esp_reset_reason() { return g_mockResetReason; }

// ========== 内联复现 RestartDiagnostics 核心逻辑 ==========

// 镜像 RestartReason 枚举
enum class RestartReason : uint8_t {
    UNKNOWN             = 0,
    CRITICAL_LOW_MEMORY = 1,
    FRAMEWORK_LOW_MEMORY= 2,
    USER_COMMAND        = 3,
    OTA_UPDATE          = 4,
    UNCAUGHT_EXCEPTION  = 5,
    WATCHDOG_TIMEOUT    = 6,
    STACK_OVERFLOW      = 7,
    MEMORY_COMPACTION   = 8,
    PERIPHERAL_FAULT    = 9,
    CONFIG_CORRUPTION   = 10,
    CONFIG_CHANGE       = 11,
    WEB_RECOVERY        = 12,
    AP_FALLBACK         = 13,
    FACTORY_RESET       = 14,
};

// 镜像 PreRestartSnapshot 结构体（精简版）
struct PreRestartSnapshot {
    uint32_t magic;
    uint32_t timestamp;
    uint32_t freeHeap;
    uint32_t minFreeHeap;
    uint32_t largestFreeBlock;
    uint8_t  heapFragmentation;
    uint8_t  memGuardLevel;
    uint8_t  consecutiveLowMem;
    uint16_t loopTaskWatermark;
    uint16_t asyncTcpWatermark;
    uint16_t mqttReconnWatermark;
    uint8_t  wifiConnected;
    int8_t   wifiRssi;
    uint8_t  mqttQueueDepth;
    uint8_t  sseClientCount;
    uint8_t  activeRuleCount;
    uint8_t  restartReason;
    char     restartContext[48];
    uint32_t checksum;
};

static constexpr uint32_t SNAPSHOT_MAGIC = 0xFBD1A600;

// 镜像 calculateChecksum
static uint32_t calculateChecksum(const PreRestartSnapshot& snap) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&snap);
    size_t len = offsetof(PreRestartSnapshot, checksum);
    uint32_t xorVal = 0;
    for (size_t i = 0; i < len; i++) {
        xorVal ^= (uint32_t)data[i] << ((i % 4) * 8);
    }
    return xorVal ^ SNAPSHOT_MAGIC;
}

// 镜像 getResetReasonString
static const char* getResetReasonString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "Power-on reset";
        case ESP_RST_EXT:       return "External reset (pin)";
        case ESP_RST_SW:        return "Software restart (ESP.restart)";
        case ESP_RST_PANIC:     return "Exception/Panic (software fault)";
        case ESP_RST_INT_WDT:   return "Interrupt Watchdog (task deadlock)";
        case ESP_RST_TASK_WDT:  return "Task Watchdog (loop blocked >60s)";
        case ESP_RST_WDT:       return "Other Watchdog reset";
        case ESP_RST_DEEPSLEEP: return "Deep sleep wakeup";
        case ESP_RST_BROWNOUT:  return "Brownout (voltage drop)";
        case ESP_RST_SDIO:      return "SDIO reset";
        default:                return "Unknown reset reason";
    }
}

// 镜像 getRestartReasonString
static const char* getRestartReasonString(RestartReason reason) {
    switch (reason) {
        case RestartReason::UNKNOWN:              return "Unknown/First boot";
        case RestartReason::CRITICAL_LOW_MEMORY:  return "Critical low memory (HealthMonitor)";
        case RestartReason::FRAMEWORK_LOW_MEMORY: return "Low memory (Framework guard)";
        case RestartReason::USER_COMMAND:         return "User command (reboot)";
        case RestartReason::OTA_UPDATE:           return "OTA update completed";
        case RestartReason::UNCAUGHT_EXCEPTION:   return "Uncaught C++ exception";
        case RestartReason::WATCHDOG_TIMEOUT:     return "Watchdog timeout";
        case RestartReason::STACK_OVERFLOW:       return "Stack overflow detected";
        case RestartReason::MEMORY_COMPACTION:    return "Irrecoverable fragmentation";
        case RestartReason::PERIPHERAL_FAULT:     return "Peripheral hardware fault";
        case RestartReason::CONFIG_CORRUPTION:    return "Configuration file corrupted";
        case RestartReason::CONFIG_CHANGE:        return "Configuration changed (network/MQTT)";
        case RestartReason::WEB_RECOVERY:         return "Web server recovery (TCP exhaustion)";
        case RestartReason::AP_FALLBACK:          return "Emergency AP fallback (all networks failed)";
        case RestartReason::FACTORY_RESET:        return "Factory reset completed";
        default:                                  return "Unknown reason code";
    }
}

// ========== Checksum 测试 ==========

static void test_checksum_deterministic() {
    // 相同数据应产生相同 checksum
    PreRestartSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.magic = SNAPSHOT_MAGIC;
    snap.freeHeap = 50000;
    snap.timestamp = 12345;
    
    uint32_t ck1 = calculateChecksum(snap);
    uint32_t ck2 = calculateChecksum(snap);
    TEST_ASSERT_EQUAL(ck1, ck2);
}

static void test_checksum_changes_with_data() {
    PreRestartSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.magic = SNAPSHOT_MAGIC;
    
    uint32_t ck1 = calculateChecksum(snap);
    
    snap.freeHeap = 99999;
    uint32_t ck2 = calculateChecksum(snap);
    
    TEST_ASSERT_NOT_EQUAL(ck1, ck2);
}

static void test_checksum_includes_magic() {
    PreRestartSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    
    snap.magic = SNAPSHOT_MAGIC;
    uint32_t ck1 = calculateChecksum(snap);
    
    snap.magic = 0xDEADBEEF;
    uint32_t ck2 = calculateChecksum(snap);
    
    TEST_ASSERT_NOT_EQUAL(ck1, ck2);
}

static void test_checksum_context_string_matters() {
    PreRestartSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.magic = SNAPSHOT_MAGIC;
    
    strcpy(snap.restartContext, "User reboot");
    uint32_t ck1 = calculateChecksum(snap);
    
    strcpy(snap.restartContext, "OTA update");
    uint32_t ck2 = calculateChecksum(snap);
    
    TEST_ASSERT_NOT_EQUAL(ck1, ck2);
}

static void test_checksum_validation_roundtrip() {
    // 写入 → 计算 checksum → 读出验证
    PreRestartSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.magic = SNAPSHOT_MAGIC;
    snap.freeHeap = 100000;
    snap.minFreeHeap = 8000;
    snap.largestFreeBlock = 32000;
    snap.heapFragmentation = 45;
    snap.memGuardLevel = 1;
    snap.restartReason = (uint8_t)RestartReason::CRITICAL_LOW_MEMORY;
    strcpy(snap.restartContext, "HealthMonitor triggered");
    snap.checksum = calculateChecksum(snap);
    
    // 验证 round-trip
    uint32_t verifyCk = calculateChecksum(snap);
    TEST_ASSERT_EQUAL(snap.checksum, verifyCk);
    
    // 篡改数据后验证失败
    snap.freeHeap = 0;
    verifyCk = calculateChecksum(snap);
    TEST_ASSERT_NOT_EQUAL(snap.checksum, verifyCk);
}

// ========== getResetReasonString 测试 ==========

static void test_reset_reason_poweron() {
    TEST_ASSERT_EQUAL_STRING("Power-on reset", getResetReasonString(ESP_RST_POWERON));
}

static void test_reset_reason_software() {
    TEST_ASSERT_EQUAL_STRING("Software restart (ESP.restart)", getResetReasonString(ESP_RST_SW));
}

static void test_reset_reason_panic() {
    TEST_ASSERT_EQUAL_STRING("Exception/Panic (software fault)", getResetReasonString(ESP_RST_PANIC));
}

static void test_reset_reason_task_wdt() {
    TEST_ASSERT_EQUAL_STRING("Task Watchdog (loop blocked >60s)", getResetReasonString(ESP_RST_TASK_WDT));
}

static void test_reset_reason_int_wdt() {
    TEST_ASSERT_EQUAL_STRING("Interrupt Watchdog (task deadlock)", getResetReasonString(ESP_RST_INT_WDT));
}

static void test_reset_reason_brownout() {
    TEST_ASSERT_EQUAL_STRING("Brownout (voltage drop)", getResetReasonString(ESP_RST_BROWNOUT));
}

static void test_reset_reason_deepsleep() {
    TEST_ASSERT_EQUAL_STRING("Deep sleep wakeup", getResetReasonString(ESP_RST_DEEPSLEEP));
}

static void test_reset_reason_unknown() {
    TEST_ASSERT_EQUAL_STRING("Unknown reset reason", getResetReasonString(ESP_RST_UNKNOWN));
    TEST_ASSERT_EQUAL_STRING("Unknown reset reason", getResetReasonString((esp_reset_reason_t)99));
}

// ========== getRestartReasonString 测试 ==========

static void test_restart_reason_unknown() {
    TEST_ASSERT_EQUAL_STRING("Unknown/First boot", getRestartReasonString(RestartReason::UNKNOWN));
}

static void test_restart_reason_low_memory() {
    TEST_ASSERT_EQUAL_STRING("Critical low memory (HealthMonitor)",
                             getRestartReasonString(RestartReason::CRITICAL_LOW_MEMORY));
}

static void test_restart_reason_user_command() {
    TEST_ASSERT_EQUAL_STRING("User command (reboot)",
                             getRestartReasonString(RestartReason::USER_COMMAND));
}

static void test_restart_reason_ota() {
    TEST_ASSERT_EQUAL_STRING("OTA update completed",
                             getRestartReasonString(RestartReason::OTA_UPDATE));
}

static void test_restart_reason_watchdog() {
    TEST_ASSERT_EQUAL_STRING("Watchdog timeout",
                             getRestartReasonString(RestartReason::WATCHDOG_TIMEOUT));
}

static void test_restart_reason_stack_overflow() {
    TEST_ASSERT_EQUAL_STRING("Stack overflow detected",
                             getRestartReasonString(RestartReason::STACK_OVERFLOW));
}

static void test_restart_reason_config_corruption() {
    TEST_ASSERT_EQUAL_STRING("Configuration file corrupted",
                             getRestartReasonString(RestartReason::CONFIG_CORRUPTION));
}

static void test_restart_reason_config_change() {
    TEST_ASSERT_EQUAL_STRING("Configuration changed (network/MQTT)",
                             getRestartReasonString(RestartReason::CONFIG_CHANGE));
}

static void test_restart_reason_web_recovery() {
    TEST_ASSERT_EQUAL_STRING("Web server recovery (TCP exhaustion)",
                             getRestartReasonString(RestartReason::WEB_RECOVERY));
}

static void test_restart_reason_ap_fallback() {
    TEST_ASSERT_EQUAL_STRING("Emergency AP fallback (all networks failed)",
                             getRestartReasonString(RestartReason::AP_FALLBACK));
}

static void test_restart_reason_factory_reset() {
    TEST_ASSERT_EQUAL_STRING("Factory reset completed",
                             getRestartReasonString(RestartReason::FACTORY_RESET));
}

static void test_restart_reason_invalid_code() {
    TEST_ASSERT_EQUAL_STRING("Unknown reason code",
                             getRestartReasonString((RestartReason)255));
}

// ========== 异常重启判定逻辑测试 ==========

// 镜像 wasAbnormalRestart 的判定逻辑
static bool isAbnormalResetReason(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
            return true;
        default:
            return false;
    }
}

static void test_abnormal_reasons_detected() {
    g_mockResetReason = ESP_RST_PANIC;
    TEST_ASSERT_TRUE(isAbnormalResetReason(g_mockResetReason));
    
    g_mockResetReason = ESP_RST_INT_WDT;
    TEST_ASSERT_TRUE(isAbnormalResetReason(g_mockResetReason));
    
    g_mockResetReason = ESP_RST_TASK_WDT;
    TEST_ASSERT_TRUE(isAbnormalResetReason(g_mockResetReason));
    
    g_mockResetReason = ESP_RST_WDT;
    TEST_ASSERT_TRUE(isAbnormalResetReason(g_mockResetReason));
    
    g_mockResetReason = ESP_RST_BROWNOUT;
    TEST_ASSERT_TRUE(isAbnormalResetReason(g_mockResetReason));
}

static void test_normal_reasons_not_abnormal() {
    g_mockResetReason = ESP_RST_POWERON;
    TEST_ASSERT_FALSE(isAbnormalResetReason(g_mockResetReason));
    
    g_mockResetReason = ESP_RST_SW;
    TEST_ASSERT_FALSE(isAbnormalResetReason(g_mockResetReason));
    
    g_mockResetReason = ESP_RST_DEEPSLEEP;
    TEST_ASSERT_FALSE(isAbnormalResetReason(g_mockResetReason));
    
    g_mockResetReason = ESP_RST_EXT;
    TEST_ASSERT_FALSE(isAbnormalResetReason(g_mockResetReason));
}

// 验证新增 reason 的异常判定分类
static void test_new_reasons_abnormal_classification() {
    // 镜像 wasAbnormalRestart 中 ESP_RST_SW 时的自定义原因判定逻辑
    auto isAbnormalCustomReason = [](RestartReason r) -> bool {
        return r == RestartReason::CRITICAL_LOW_MEMORY ||
               r == RestartReason::FRAMEWORK_LOW_MEMORY ||
               r == RestartReason::UNCAUGHT_EXCEPTION ||
               r == RestartReason::STACK_OVERFLOW ||
               r == RestartReason::MEMORY_COMPACTION ||
               r == RestartReason::PERIPHERAL_FAULT;
    };

    // 新增 reason 应属于“正常”重启（非异常）
    TEST_ASSERT_FALSE(isAbnormalCustomReason(RestartReason::CONFIG_CHANGE));
    TEST_ASSERT_FALSE(isAbnormalCustomReason(RestartReason::WEB_RECOVERY));
    TEST_ASSERT_FALSE(isAbnormalCustomReason(RestartReason::AP_FALLBACK));
    TEST_ASSERT_FALSE(isAbnormalCustomReason(RestartReason::FACTORY_RESET));

    // 已有 reason 分类确认
    TEST_ASSERT_FALSE(isAbnormalCustomReason(RestartReason::USER_COMMAND));
    TEST_ASSERT_FALSE(isAbnormalCustomReason(RestartReason::OTA_UPDATE));
    TEST_ASSERT_FALSE(isAbnormalCustomReason(RestartReason::WATCHDOG_TIMEOUT));
    TEST_ASSERT_FALSE(isAbnormalCustomReason(RestartReason::CONFIG_CORRUPTION));

    // 异常 reason 确认
    TEST_ASSERT_TRUE(isAbnormalCustomReason(RestartReason::CRITICAL_LOW_MEMORY));
    TEST_ASSERT_TRUE(isAbnormalCustomReason(RestartReason::FRAMEWORK_LOW_MEMORY));
    TEST_ASSERT_TRUE(isAbnormalCustomReason(RestartReason::UNCAUGHT_EXCEPTION));
    TEST_ASSERT_TRUE(isAbnormalCustomReason(RestartReason::STACK_OVERFLOW));
    TEST_ASSERT_TRUE(isAbnormalCustomReason(RestartReason::MEMORY_COMPACTION));
    TEST_ASSERT_TRUE(isAbnormalCustomReason(RestartReason::PERIPHERAL_FAULT));
}

// 验证所有枚举值都有非空字符串映射
static void test_all_reasons_have_string_mapping() {
    // 确保每个枚举值映射到非 "Unknown" 字符串
    for (uint8_t i = 0; i <= 14; i++) {
        const char* str = getRestartReasonString(static_cast<RestartReason>(i));
        TEST_ASSERT_NOT_NULL(str);
        TEST_ASSERT_TRUE_MESSAGE(
            strlen(str) > 0,
            "All RestartReason values must have non-empty string mapping");
        TEST_ASSERT_FALSE_MESSAGE(
            strcmp(str, "Unknown reason code") == 0,
            "All RestartReason values must have a specific description");
    }
}

// ========== RestartReason 枚举完整性 ==========

static void test_restart_reason_enum_values() {
    // 验证枚举值与预期一致（防止意外改动）
    TEST_ASSERT_EQUAL(0,  (int)RestartReason::UNKNOWN);
    TEST_ASSERT_EQUAL(1,  (int)RestartReason::CRITICAL_LOW_MEMORY);
    TEST_ASSERT_EQUAL(2,  (int)RestartReason::FRAMEWORK_LOW_MEMORY);
    TEST_ASSERT_EQUAL(3,  (int)RestartReason::USER_COMMAND);
    TEST_ASSERT_EQUAL(4,  (int)RestartReason::OTA_UPDATE);
    TEST_ASSERT_EQUAL(5,  (int)RestartReason::UNCAUGHT_EXCEPTION);
    TEST_ASSERT_EQUAL(6,  (int)RestartReason::WATCHDOG_TIMEOUT);
    TEST_ASSERT_EQUAL(7,  (int)RestartReason::STACK_OVERFLOW);
    TEST_ASSERT_EQUAL(8,  (int)RestartReason::MEMORY_COMPACTION);
    TEST_ASSERT_EQUAL(9,  (int)RestartReason::PERIPHERAL_FAULT);
    TEST_ASSERT_EQUAL(10, (int)RestartReason::CONFIG_CORRUPTION);
    TEST_ASSERT_EQUAL(11, (int)RestartReason::CONFIG_CHANGE);
    TEST_ASSERT_EQUAL(12, (int)RestartReason::WEB_RECOVERY);
    TEST_ASSERT_EQUAL(13, (int)RestartReason::AP_FALLBACK);
    TEST_ASSERT_EQUAL(14, (int)RestartReason::FACTORY_RESET);
}

// ========== PreRestartSnapshot 结构体 ==========

static void test_snapshot_magic_value() {
    TEST_ASSERT_EQUAL_HEX32(0xFBD1A600, SNAPSHOT_MAGIC);
}

static void test_snapshot_context_string_truncation() {
    // restartContext 最多 47 字符（48字节含 null terminator）
    PreRestartSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    
    const char* longStr = "This is a very long context string that exceeds the maximum allowed length for the restart context field";
    strncpy(snap.restartContext, longStr, sizeof(snap.restartContext) - 1);
    snap.restartContext[sizeof(snap.restartContext) - 1] = '\0';
    
    TEST_ASSERT_EQUAL(47, (int)strlen(snap.restartContext));
}

// ========== 测试组入口 ==========

void test_restart_diagnostics_group() {
    // Checksum
    RUN_TEST(test_checksum_deterministic);
    RUN_TEST(test_checksum_changes_with_data);
    RUN_TEST(test_checksum_includes_magic);
    RUN_TEST(test_checksum_context_string_matters);
    RUN_TEST(test_checksum_validation_roundtrip);
    
    // ESP 重启原因字符串
    RUN_TEST(test_reset_reason_poweron);
    RUN_TEST(test_reset_reason_software);
    RUN_TEST(test_reset_reason_panic);
    RUN_TEST(test_reset_reason_task_wdt);
    RUN_TEST(test_reset_reason_int_wdt);
    RUN_TEST(test_reset_reason_brownout);
    RUN_TEST(test_reset_reason_deepsleep);
    RUN_TEST(test_reset_reason_unknown);
    
    // FastBee 自定义重启原因字符串
    RUN_TEST(test_restart_reason_unknown);
    RUN_TEST(test_restart_reason_low_memory);
    RUN_TEST(test_restart_reason_user_command);
    RUN_TEST(test_restart_reason_ota);
    RUN_TEST(test_restart_reason_watchdog);
    RUN_TEST(test_restart_reason_stack_overflow);
    RUN_TEST(test_restart_reason_config_corruption);
    RUN_TEST(test_restart_reason_config_change);
    RUN_TEST(test_restart_reason_web_recovery);
    RUN_TEST(test_restart_reason_ap_fallback);
    RUN_TEST(test_restart_reason_factory_reset);
    RUN_TEST(test_restart_reason_invalid_code);
    
    // 异常重启判定
    RUN_TEST(test_abnormal_reasons_detected);
    RUN_TEST(test_normal_reasons_not_abnormal);
    RUN_TEST(test_new_reasons_abnormal_classification);
    RUN_TEST(test_all_reasons_have_string_mapping);
    
    // 枚举完整性
    RUN_TEST(test_restart_reason_enum_values);
    
    // 结构体
    RUN_TEST(test_snapshot_magic_value);
    RUN_TEST(test_snapshot_context_string_truncation);
}
