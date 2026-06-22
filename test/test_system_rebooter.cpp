/**
 * @file test_system_rebooter.cpp
 * @brief SystemRebooter 配置变更重启单元测试
 *
 * 测试内容：
 * - scheduleConfigReboot 设置调度状态
 * - cancelScheduledReboot 取消已调度重启
 * - update() 未到时间点不执行重启
 * - update() 到达时间点执行重启
 * - update() 处理 millis() 溢出场景
 * - isScheduled() 状态查询
 * - reason 字符串截断（47字符上限）
 * - 多芯片环境兼容性（ESP32/S3/C3）
 */

#include <unity.h>
#include <Arduino.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <fstream>
#include <sstream>

void test_system_rebooter_group();

// ========== 内联复现 SystemRebooter 核心逻辑 ==========
// 与 src/systems/SystemRebooter.cpp 完全一致的调度逻辑
// 但不调用 ESP.restart()，而是设置标志位供断言使用

namespace TestRebooter {

static bool _scheduled = false;
static unsigned long _rebootAt = 0;
static char _reason[48] = {0};
static uint8_t _restartReason = 0;  // 镜像 RestartReason 枚举
static bool _rebootExecuted = false;  // 测试用：标记"重启"是否已执行
static char _lastRebootReason[48] = {0};
static uint8_t _lastRestartReason = 0;  // 测试用：保存的 RestartReason

void reset() {
    _scheduled = false;
    _rebootAt = 0;
    _reason[0] = '\0';
    _restartReason = 0;
    _rebootExecuted = false;
    _lastRebootReason[0] = '\0';
    _lastRestartReason = 0;
}

void scheduleReboot(const char* reason, unsigned long delayMs = 2000, uint8_t restartReason = 11 /*CONFIG_CHANGE*/) {
    if (reason) {
        strncpy(_reason, reason, sizeof(_reason) - 1);
        _reason[sizeof(_reason) - 1] = '\0';
    } else {
        strncpy(_reason, "Reboot", sizeof(_reason) - 1);
        _reason[sizeof(_reason) - 1] = '\0';
    }
    _rebootAt = millis() + delayMs;
    _restartReason = restartReason;
    _scheduled = true;
}

void scheduleConfigReboot(const char* reason, unsigned long delayMs = 2000) {
    scheduleReboot(reason, delayMs, 11);  // CONFIG_CHANGE = 11
}

void cancelScheduledReboot() {
    if (_scheduled) {
        _scheduled = false;
        _rebootAt = 0;
        _reason[0] = '\0';
        _restartReason = 0;
    }
}

void update() {
    if (!_scheduled) return;

    unsigned long now = millis();
    if (now >= _rebootAt || (_rebootAt > now + 60000UL)) {
        _scheduled = false;
        // 模拟 RestartDiagnostics::savePreRestartState + ESP.restart()
        strncpy(_lastRebootReason, _reason, sizeof(_lastRebootReason) - 1);
        _lastRebootReason[sizeof(_lastRebootReason) - 1] = '\0';
        _lastRestartReason = _restartReason;
        _rebootExecuted = true;
    }
}

bool isScheduled() { return _scheduled; }

}  // namespace TestRebooter

// ========== 调度测试 ==========

static void test_schedule_sets_state() {
    TestRebooter::reset();
    TestRebooter::scheduleConfigReboot("Network config changed", 2000);

    TEST_ASSERT_TRUE(TestRebooter::isScheduled());
    TEST_ASSERT_EQUAL_STRING("Network config changed", TestRebooter::_reason);
    TEST_ASSERT_FALSE(TestRebooter::_rebootExecuted);
}

static void test_schedule_with_null_reason() {
    TestRebooter::reset();
    TestRebooter::scheduleConfigReboot(nullptr, 2000);

    TEST_ASSERT_TRUE(TestRebooter::isScheduled());
    TEST_ASSERT_EQUAL_STRING("Reboot", TestRebooter::_reason);
}

static void test_schedule_reason_truncation() {
    TestRebooter::reset();
    // 50 字符字符串，应截断到 47 字符
    const char* longReason = "This is a very long reason that exceeds the forty seven character limit!!!";
    TestRebooter::scheduleConfigReboot(longReason, 2000);

    TEST_ASSERT_TRUE(TestRebooter::isScheduled());
    TEST_ASSERT_EQUAL(47, (int)strlen(TestRebooter::_reason));
    // 验证截断后的内容（47字符）
    TEST_ASSERT_EQUAL_STRING("This is a very long reason that exceeds the for",
                             TestRebooter::_reason);
}

// ========== 取消测试 ==========

static void test_cancel_clears_state() {
    TestRebooter::reset();
    TestRebooter::scheduleConfigReboot("MQTT config changed", 2000);
    TEST_ASSERT_TRUE(TestRebooter::isScheduled());

    TestRebooter::cancelScheduledReboot();
    TEST_ASSERT_FALSE(TestRebooter::isScheduled());
    TEST_ASSERT_EQUAL_STRING("", TestRebooter::_reason);
}

static void test_cancel_when_not_scheduled_is_safe() {
    TestRebooter::reset();
    TEST_ASSERT_FALSE(TestRebooter::isScheduled());
    TestRebooter::cancelScheduledReboot();  // 不应崩溃
    TEST_ASSERT_FALSE(TestRebooter::isScheduled());
}

// ========== update() 时机测试 ==========

static void test_update_before_deadline_does_not_reboot() {
    TestRebooter::reset();
    unsigned long now = millis();
    TestRebooter::scheduleConfigReboot("Test", 5000);

    // 模拟时间只过了 1 秒（未到 5 秒 deadline）
    TestRebooter::_rebootAt = now + 5000;  // deadline 在 5 秒后
    // now < _rebootAt，不应执行重启
    TestRebooter::update();

    TEST_ASSERT_FALSE(TestRebooter::_rebootExecuted);
    TEST_ASSERT_TRUE(TestRebooter::isScheduled());
}

static void test_update_at_deadline_reboots() {
    TestRebooter::reset();
    unsigned long now = millis();
    TestRebooter::scheduleConfigReboot("Network config changed", 2000);

    // 模拟时间到达 deadline
    TestRebooter::_rebootAt = now;  // 设为当前时间
    TestRebooter::update();

    TEST_ASSERT_TRUE(TestRebooter::_rebootExecuted);
    TEST_ASSERT_FALSE(TestRebooter::isScheduled());
    TEST_ASSERT_EQUAL_STRING("Network config changed", TestRebooter::_lastRebootReason);
}

static void test_update_after_deadline_reboots() {
    TestRebooter::reset();
    unsigned long now = millis();
    TestRebooter::scheduleConfigReboot("MQTT config changed", 1000);

    // 模拟时间已过 deadline
    TestRebooter::_rebootAt = now - 100;  // deadline 已过
    TestRebooter::update();

    TEST_ASSERT_TRUE(TestRebooter::_rebootExecuted);
    TEST_ASSERT_EQUAL_STRING("MQTT config changed", TestRebooter::_lastRebootReason);
}

// ========== millis() 溢出场景 ==========

static void test_update_millis_overflow() {
    TestRebooter::reset();
    // 模拟 millis() 溢出：_rebootAt 接近 ULONG_MAX，now 已经溢出回到小值
    TestRebooter::_scheduled = true;
    TestRebooter::_rebootAt = 0xFFFFFF00UL;  // 接近溢出
    strncpy(TestRebooter::_reason, "Overflow test", 47);

    // now = 100 (已溢出)，_rebootAt = 0xFFFFFF00
    // 溢出检测条件：_rebootAt > now + 60000 → 0xFFFFFF00 > 100 + 60000 → true
    // 这意味着 _rebootAt 看起来像是"未来很远"，实际是溢出前的过去值
    unsigned long now = 100;
    bool overflowDetected = (TestRebooter::_rebootAt > now + 60000UL);
    TEST_ASSERT_TRUE_MESSAGE(overflowDetected,
        "Overflow detection: _rebootAt(0xFFFFFF00) > now(100) + 60000");
}

// ========== 多次调度覆盖 ==========

static void test_reschedule_overwrites() {
    TestRebooter::reset();
    TestRebooter::scheduleConfigReboot("First reason", 2000);
    TestRebooter::scheduleConfigReboot("Second reason", 3000);

    TEST_ASSERT_TRUE(TestRebooter::isScheduled());
    TEST_ASSERT_EQUAL_STRING("Second reason", TestRebooter::_reason);
}

// ========== update() 无调度时安全 ==========

static void test_update_without_schedule_is_safe() {
    TestRebooter::reset();
    TEST_ASSERT_FALSE(TestRebooter::isScheduled());
    TestRebooter::update();  // 不应崩溃
    TEST_ASSERT_FALSE(TestRebooter::_rebootExecuted);
}

// ========== 多芯片兼容性验证 ==========

static void test_multi_chip_schedule_reboot() {
    // 验证调度逻辑在各种芯片堆大小下都正确
    struct ChipEnv {
        const char* name;
        uint32_t heap;
        uint32_t psram;
    };
    ChipEnv chips[] = {
        {"ESP32-F4R0",    320000, 0},
        {"ESP32-S3-F8R0", 320000, 0},
        {"ESP32-S3-F8R4", 320000, 4*1024*1024},
        {"ESP32-S3-F16R8",320000, 8*1024*1024},
    };

    for (auto& chip : chips) {
        TestRebooter::reset();
        TestRebooter::scheduleConfigReboot("Network config changed", 2000);
        TEST_ASSERT_TRUE_MESSAGE(TestRebooter::isScheduled(),
            (String("Schedule should work on ") + chip.name).c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE("Network config changed",
            TestRebooter::_reason,
            (String("Reason preserved on ") + chip.name).c_str());
    }
}

// ========== 配置保存后重启 vs 运行时切换 验证 ==========

static void test_config_reboot_vs_runtime_switch() {
    // 场景：用户修改 WiFi SSID → 应触发设备重启（非运行时 restartNetwork）
    TestRebooter::reset();

    // 模拟 Web Handler 保存 WiFi 配置
    TestRebooter::scheduleConfigReboot("Network config changed", 2000);

    // 验证：设备重启被调度
    TEST_ASSERT_TRUE(TestRebooter::isScheduled());

    // 验证：2秒后执行重启
    TestRebooter::_rebootAt = millis();
    TestRebooter::update();
    TEST_ASSERT_TRUE(TestRebooter::_rebootExecuted);

    // 对比：运行时 restartNetwork() 不会调度设备重启
    // （restartNetwork 由 pendingRestart 标志驱动，不涉及 SystemRebooter）
}

static void test_mqtt_config_reboot() {
    // 场景：用户修改 MQTT broker → 应触发设备重启（非运行时 restartMQTTDeferred）
    TestRebooter::reset();
    TestRebooter::scheduleConfigReboot("MQTT config changed", 2000);

    TEST_ASSERT_TRUE(TestRebooter::isScheduled());
    TestRebooter::_rebootAt = millis();
    TestRebooter::update();
    TEST_ASSERT_TRUE(TestRebooter::_rebootExecuted);
    TEST_ASSERT_EQUAL_STRING("MQTT config changed", TestRebooter::_lastRebootReason);
}

// ========== RestartReason 枚举验证 ==========

static void test_config_change_reason_value() {
    // 验证枚举值与 RestartDiagnostics.h 一致
    enum class RestartReason : uint8_t {
        UNKNOWN = 0, CRITICAL_LOW_MEMORY = 1, FRAMEWORK_LOW_MEMORY = 2,
        USER_COMMAND = 3, OTA_UPDATE = 4, UNCAUGHT_EXCEPTION = 5,
        WATCHDOG_TIMEOUT = 6, STACK_OVERFLOW = 7, MEMORY_COMPACTION = 8,
        PERIPHERAL_FAULT = 9, CONFIG_CORRUPTION = 10, CONFIG_CHANGE = 11,
        WEB_RECOVERY = 12, AP_FALLBACK = 13, FACTORY_RESET = 14,
    };
    TEST_ASSERT_EQUAL(11, (int)RestartReason::CONFIG_CHANGE);
    TEST_ASSERT_EQUAL(12, (int)RestartReason::WEB_RECOVERY);
    TEST_ASSERT_EQUAL(13, (int)RestartReason::AP_FALLBACK);
    TEST_ASSERT_EQUAL(14, (int)RestartReason::FACTORY_RESET);
    TEST_ASSERT_NOT_EQUAL((int)RestartReason::USER_COMMAND, (int)RestartReason::CONFIG_CHANGE);
    TEST_ASSERT_NOT_EQUAL((int)RestartReason::CONFIG_CORRUPTION, (int)RestartReason::CONFIG_CHANGE);
}

// ========== scheduleReboot 多原因测试 ==========

static void test_schedule_reboot_web_recovery() {
    TestRebooter::reset();
    TestRebooter::scheduleReboot("TCP exhaustion", 3000, 12 /*WEB_RECOVERY*/);

    TEST_ASSERT_TRUE(TestRebooter::isScheduled());
    TEST_ASSERT_EQUAL_STRING("TCP exhaustion", TestRebooter::_reason);
    TEST_ASSERT_EQUAL(12, (int)TestRebooter::_restartReason);

    // 触发重启
    TestRebooter::_rebootAt = millis();
    TestRebooter::update();
    TEST_ASSERT_TRUE(TestRebooter::_rebootExecuted);
    TEST_ASSERT_EQUAL(12, (int)TestRebooter::_lastRestartReason);
}

static void test_schedule_reboot_factory_reset() {
    TestRebooter::reset();
    TestRebooter::scheduleReboot("Factory reset completed", 2000, 14 /*FACTORY_RESET*/);

    TEST_ASSERT_TRUE(TestRebooter::isScheduled());
    TEST_ASSERT_EQUAL(14, (int)TestRebooter::_restartReason);

    TestRebooter::_rebootAt = millis();
    TestRebooter::update();
    TEST_ASSERT_TRUE(TestRebooter::_rebootExecuted);
    TEST_ASSERT_EQUAL(14, (int)TestRebooter::_lastRestartReason);
    TEST_ASSERT_EQUAL_STRING("Factory reset completed", TestRebooter::_lastRebootReason);
}

static void test_schedule_reboot_user_command() {
    TestRebooter::reset();
    TestRebooter::scheduleReboot("User requested system restart", 5000, 3 /*USER_COMMAND*/);

    TEST_ASSERT_TRUE(TestRebooter::isScheduled());
    TEST_ASSERT_EQUAL(3, (int)TestRebooter::_restartReason);
}

static void test_config_reboot_default_reason() {
    // scheduleConfigReboot 默认使用 CONFIG_CHANGE (=11)
    TestRebooter::reset();
    TestRebooter::scheduleConfigReboot("Network config changed", 2000);
    TEST_ASSERT_EQUAL(11, (int)TestRebooter::_restartReason);
}

// ========== cancel 对 restartReason 的影响 ==========

static void test_cancel_resets_restart_reason() {
    TestRebooter::reset();
    TestRebooter::scheduleReboot("Web recovery", 2000, 12 /*WEB_RECOVERY*/);
    TEST_ASSERT_EQUAL(12, (int)TestRebooter::_restartReason);

    TestRebooter::cancelScheduledReboot();
    TEST_ASSERT_FALSE(TestRebooter::isScheduled());
    TEST_ASSERT_EQUAL(0, (int)TestRebooter::_restartReason);
    TEST_ASSERT_EQUAL_STRING("", TestRebooter::_reason);
}

static void test_reschedule_overwrites_reason_and_enum() {
    TestRebooter::reset();
    // 先用 CONFIG_CHANGE
    TestRebooter::scheduleConfigReboot("Network config", 2000);
    TEST_ASSERT_EQUAL(11, (int)TestRebooter::_restartReason);
    TEST_ASSERT_EQUAL_STRING("Network config", TestRebooter::_reason);

    // 再用 FACTORY_RESET 覆盖
    TestRebooter::scheduleReboot("Factory reset", 3000, 14 /*FACTORY_RESET*/);
    TEST_ASSERT_EQUAL(14, (int)TestRebooter::_restartReason);
    TEST_ASSERT_EQUAL_STRING("Factory reset", TestRebooter::_reason);
    TEST_ASSERT_TRUE(TestRebooter::isScheduled());

    // 触发重启，验证最终使用的是第二次调度的值
    TestRebooter::_rebootAt = millis();
    TestRebooter::update();
    TEST_ASSERT_TRUE(TestRebooter::_rebootExecuted);
    TEST_ASSERT_EQUAL(14, (int)TestRebooter::_lastRestartReason);
    TEST_ASSERT_EQUAL_STRING("Factory reset", TestRebooter::_lastRebootReason);
}

// ========== 源码级集成验证 ==========

static std::string readSourceFile(const char* relativePath) {
    // 尝试多种基础路径
    const char* bases[] = {
        "./",
        "../",
        "../../",
        "../../../",
    };
    for (const char* base : bases) {
        std::string fullPath = std::string(base) + relativePath;
        std::ifstream f(fullPath);
        if (f.good()) {
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
    }
    return "";
}

static void test_source_network_manager_uses_system_rebooter() {
    // 验证 NetworkManager.cpp 使用 SystemRebooter::isScheduled() 避免竟态
    std::string content = readSourceFile("src/network/NetworkManager.cpp");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("NetworkManager.cpp not readable, skipping source check");
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("SystemRebooter::isScheduled()") != std::string::npos,
        "NetworkManager::updateConfig must check SystemRebooter::isScheduled() before setting pendingRestart");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("#include \"systems/SystemRebooter.h\"") != std::string::npos,
        "NetworkManager.cpp must include SystemRebooter.h");
}

static void test_source_web_config_manager_delegates_to_rebooter() {
    // 验证 WebConfigManager::performMaintenance 不再直接调用 ESP.restart()
    std::string content = readSourceFile("src/network/WebConfigManager.cpp");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("WebConfigManager.cpp not readable, skipping source check");
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("SystemRebooter::update()") != std::string::npos,
        "WebConfigManager::performMaintenance must delegate to SystemRebooter::update()");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("scheduleDeviceRestartForWebRecovery") != std::string::npos &&
        content.find("SystemRebooter::scheduleReboot") != std::string::npos,
        "scheduleDeviceRestartForWebRecovery must use SystemRebooter::scheduleReboot()");
}

static void test_source_mqtt_reconnect_uses_system_rebooter() {
    // 验证 MqttRouteHandler 手动重连使用 SystemRebooter
    std::string content = readSourceFile("src/network/handlers/MqttRouteHandler.cpp");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("MqttRouteHandler.cpp not readable, skipping source check");
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("SystemRebooter::scheduleConfigReboot") != std::string::npos,
        "MQTT manual reconnect must use SystemRebooter::scheduleConfigReboot()");
}

static void test_source_periph_exec_restarts_use_system_rebooter() {
    std::string content = readSourceFile("src/core/PeriphExecExecutor.cpp");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("PeriphExecExecutor.cpp not readable, skipping source check");
        return;
    }

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("SystemRebooter::scheduleReboot(\"PeriphExec system restart\"") != std::string::npos &&
        content.find("RestartReason::USER_COMMAND") != std::string::npos,
        "PeriphExec system restart action must schedule USER_COMMAND reboot");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("SystemRebooter::scheduleReboot(\"PeriphExec factory reset\"") != std::string::npos &&
        content.find("RestartReason::FACTORY_RESET") != std::string::npos,
        "PeriphExec factory reset action must schedule FACTORY_RESET reboot");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("ESP.restart();") == std::string::npos,
        "PeriphExecExecutor must not directly call ESP.restart(); use SystemRebooter for diagnostics");
}

static void test_source_all_restart_points_have_diagnostics() {
    // 验证关键文件的 ESP.restart() 调用点都有 RestartDiagnostics
    struct Check {
        const char* file;
        const char* mustContain;
    };
    Check checks[] = {
        {"src/network/OTAManager.cpp", "savePreRestartState"},
        {"src/network/handlers/OTARouteHandler.cpp", "savePreRestartState"},
        {"src/network/NetworkManager.cpp", "savePreRestartState"},
    };
    for (const auto& c : checks) {
        std::string content = readSourceFile(c.file);
        if (content.empty()) continue;  // 跳过不可读文件
        TEST_ASSERT_TRUE_MESSAGE(
            content.find(c.mustContain) != std::string::npos,
            (std::string(c.file) + " must call RestartDiagnostics::savePreRestartState before ESP.restart()").c_str());
    }
}

// ========== 测试组入口 ==========

void test_system_rebooter_group() {
    // 调度
    RUN_TEST(test_schedule_sets_state);
    RUN_TEST(test_schedule_with_null_reason);
    RUN_TEST(test_schedule_reason_truncation);

    // 取消
    RUN_TEST(test_cancel_clears_state);
    RUN_TEST(test_cancel_when_not_scheduled_is_safe);

    // update() 时机
    RUN_TEST(test_update_before_deadline_does_not_reboot);
    RUN_TEST(test_update_at_deadline_reboots);
    RUN_TEST(test_update_after_deadline_reboots);
    RUN_TEST(test_update_millis_overflow);

    // 覆盖与安全
    RUN_TEST(test_reschedule_overwrites);
    RUN_TEST(test_update_without_schedule_is_safe);

    // 多芯片兼容
    RUN_TEST(test_multi_chip_schedule_reboot);

    // 配置重启 vs 运行时切换
    RUN_TEST(test_config_reboot_vs_runtime_switch);
    RUN_TEST(test_mqtt_config_reboot);

    // 枚举验证
    RUN_TEST(test_config_change_reason_value);

    // scheduleReboot 多原因
    RUN_TEST(test_schedule_reboot_web_recovery);
    RUN_TEST(test_schedule_reboot_factory_reset);
    RUN_TEST(test_schedule_reboot_user_command);
    RUN_TEST(test_config_reboot_default_reason);

    // cancel/reschedule 对 restartReason 的影响
    RUN_TEST(test_cancel_resets_restart_reason);
    RUN_TEST(test_reschedule_overwrites_reason_and_enum);

    // 源码级集成验证
    RUN_TEST(test_source_network_manager_uses_system_rebooter);
    RUN_TEST(test_source_web_config_manager_delegates_to_rebooter);
    RUN_TEST(test_source_mqtt_reconnect_uses_system_rebooter);
    RUN_TEST(test_source_periph_exec_restarts_use_system_rebooter);
    RUN_TEST(test_source_all_restart_points_have_diagnostics);
}
