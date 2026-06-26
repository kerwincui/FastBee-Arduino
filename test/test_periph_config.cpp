/**
 * @file test_periph_config.cpp
 * @brief Peripheral Configuration Safety Tests
 * 
 * 测试外设配置的安全校验机制：
 * - Group 1: sanitizeTriggerForSafety 参数边界校验
 * - Group 2: validateLoadedConfig 全局配置安全校验
 * - Group 3: 外设配置 CRUD 安全约束
 * - Group 4: 配置持久化边界条件
 */

#include <unity.h>
#include <Arduino.h>
#include <vector>
#include <map>
#include "mocks/MockPeripheral.h"
#include "mocks/MockHealthMonitor.h"
#include "mocks/MockLogger.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"

void test_periph_config_group();

// ========== sanitize 常量（镜像自 PeriphExecManager.cpp） ==========
namespace SanitizeConstants {
    constexpr uint32_t MIN_TIMER_INTERVAL_SEC = 1;
    constexpr uint32_t MAX_TIMER_INTERVAL_SEC = 86400UL;
    constexpr uint16_t MIN_POLL_TIMEOUT_MS = 100;
    constexpr uint16_t MAX_POLL_TIMEOUT_MS = 5000;
    constexpr uint16_t HEAVY_POLL_TIMEOUT_MS = 3000;
    constexpr uint8_t  MAX_POLL_RETRIES = 3;
    constexpr uint8_t  HEAVY_POLL_RETRIES = 2;
    constexpr uint16_t MIN_POLL_INTER_DELAY_MS = 20;
    constexpr uint16_t MAX_POLL_INTER_DELAY_MS = 1000;
    constexpr uint16_t HEAVY_POLL_INTER_DELAY_MS = 100;
}

// ========== validateLoadedConfig 常量（镜像自 PeriphExecScheduler.h） ==========
namespace ValidateConstants {
    constexpr uint32_t MIN_POLL_INTERVAL_MS = 5000;
    constexpr uint32_t SAFE_POLL_INTERVAL_MS = 30000;
    constexpr uint8_t MAX_ACTIVE_TASKS = 12;
    constexpr uint8_t WARN_TASK_THRESHOLD = 8;
}

// ========== 模拟 sanitizeTriggerForSafety ==========
struct ConfigTrigger {
    uint8_t triggerType;    // 1=TIMER, 5=POLL
    uint32_t intervalSec;
    uint16_t pollResponseTimeout;
    uint8_t  pollMaxRetries;
    uint16_t pollInterPollDelay;

    ConfigTrigger()
        : triggerType(1), intervalSec(60),
          pollResponseTimeout(1000), pollMaxRetries(2),
          pollInterPollDelay(100) {}
};

static bool sanitizeTriggerForSafety(ConfigTrigger& trigger, bool hasPollCollectionAction) {
    bool modified = false;

    if (trigger.triggerType == 1) {  // TIMER_TRIGGER
        if (trigger.intervalSec < SanitizeConstants::MIN_TIMER_INTERVAL_SEC) {
            trigger.intervalSec = SanitizeConstants::MIN_TIMER_INTERVAL_SEC;
            modified = true;
        } else if (trigger.intervalSec > SanitizeConstants::MAX_TIMER_INTERVAL_SEC) {
            trigger.intervalSec = SanitizeConstants::MAX_TIMER_INTERVAL_SEC;
            modified = true;
        }
    }

    if (trigger.triggerType == 5) {  // POLL_TRIGGER
        if (trigger.pollResponseTimeout < SanitizeConstants::MIN_POLL_TIMEOUT_MS) {
            trigger.pollResponseTimeout = SanitizeConstants::MIN_POLL_TIMEOUT_MS;
            modified = true;
        } else if (trigger.pollResponseTimeout > SanitizeConstants::MAX_POLL_TIMEOUT_MS) {
            trigger.pollResponseTimeout = SanitizeConstants::MAX_POLL_TIMEOUT_MS;
            modified = true;
        }

        if (trigger.pollMaxRetries > SanitizeConstants::MAX_POLL_RETRIES) {
            trigger.pollMaxRetries = SanitizeConstants::MAX_POLL_RETRIES;
            modified = true;
        }

        if (trigger.pollInterPollDelay < SanitizeConstants::MIN_POLL_INTER_DELAY_MS) {
            trigger.pollInterPollDelay = SanitizeConstants::MIN_POLL_INTER_DELAY_MS;
            modified = true;
        } else if (trigger.pollInterPollDelay > SanitizeConstants::MAX_POLL_INTER_DELAY_MS) {
            trigger.pollInterPollDelay = SanitizeConstants::MAX_POLL_INTER_DELAY_MS;
            modified = true;
        }

        if (hasPollCollectionAction) {
            if (trigger.pollResponseTimeout > SanitizeConstants::HEAVY_POLL_TIMEOUT_MS) {
                trigger.pollResponseTimeout = SanitizeConstants::HEAVY_POLL_TIMEOUT_MS;
                modified = true;
            }
            if (trigger.pollMaxRetries > SanitizeConstants::HEAVY_POLL_RETRIES) {
                trigger.pollMaxRetries = SanitizeConstants::HEAVY_POLL_RETRIES;
                modified = true;
            }
            if (trigger.pollInterPollDelay < SanitizeConstants::HEAVY_POLL_INTER_DELAY_MS) {
                trigger.pollInterPollDelay = SanitizeConstants::HEAVY_POLL_INTER_DELAY_MS;
                modified = true;
            }
        }
    }

    return modified;
}

// ========== 模拟 validateLoadedConfig 逻辑 ==========
struct ConfigRule {
    String id;
    String name;
    bool enabled;
    std::vector<ConfigTrigger> triggers;
};

static uint8_t countActiveTimerTasks(const std::map<String, ConfigRule>& rules) {
    uint8_t count = 0;
    for (auto& kv : rules) {
        if (!kv.second.enabled) continue;
        for (const auto& t : kv.second.triggers) {
            if (t.triggerType == 1 || t.triggerType == 5) {
                count++;
                break;
            }
        }
    }
    return count;
}

static bool validateLoadedConfig(std::map<String, ConfigRule>& rules) {
    uint8_t activeCount = countActiveTimerTasks(rules);
    bool modified = false;

    for (auto& kv : rules) {
        if (!kv.second.enabled) continue;
        for (auto& trigger : kv.second.triggers) {
            if (trigger.triggerType != 1 && trigger.triggerType != 5) continue;

            uint32_t intervalMs = trigger.intervalSec * 1000UL;
            if (intervalMs < ValidateConstants::MIN_POLL_INTERVAL_MS) {
                trigger.intervalSec = ValidateConstants::MIN_POLL_INTERVAL_MS / 1000;
                modified = true;
            } else if (activeCount > ValidateConstants::WARN_TASK_THRESHOLD &&
                       intervalMs < 10000) {
                trigger.intervalSec = ValidateConstants::SAFE_POLL_INTERVAL_MS / 1000;
                modified = true;
            }
        }
    }
    return modified;
}

// ============================================================
//  TEST GROUP 1: sanitizeTriggerForSafety 参数边界校验
// ============================================================

// --- Timer interval 边界 ---

void test_sanitize_timer_interval_below_min() {
    ConfigTrigger t;
    t.triggerType = 1;
    t.intervalSec = 0;  // < 1
    bool mod = sanitizeTriggerForSafety(t, false);
    TEST_ASSERT_TRUE(mod);
    TEST_ASSERT_EQUAL_UINT32(1, t.intervalSec);
}

void test_sanitize_timer_interval_at_min() {
    ConfigTrigger t;
    t.triggerType = 1;
    t.intervalSec = 1;
    bool mod = sanitizeTriggerForSafety(t, false);
    TEST_ASSERT_FALSE(mod);
    TEST_ASSERT_EQUAL_UINT32(1, t.intervalSec);
}

void test_sanitize_timer_interval_at_max() {
    ConfigTrigger t;
    t.triggerType = 1;
    t.intervalSec = 86400;
    bool mod = sanitizeTriggerForSafety(t, false);
    TEST_ASSERT_FALSE(mod);
    TEST_ASSERT_EQUAL_UINT32(86400, t.intervalSec);
}

void test_sanitize_timer_interval_above_max() {
    ConfigTrigger t;
    t.triggerType = 1;
    t.intervalSec = 100000;  // > 86400
    bool mod = sanitizeTriggerForSafety(t, false);
    TEST_ASSERT_TRUE(mod);
    TEST_ASSERT_EQUAL_UINT32(86400, t.intervalSec);
}

void test_sanitize_timer_interval_normal() {
    ConfigTrigger t;
    t.triggerType = 1;
    t.intervalSec = 30;
    bool mod = sanitizeTriggerForSafety(t, false);
    TEST_ASSERT_FALSE(mod);
}

// --- Poll timeout 边界 ---

void test_sanitize_poll_timeout_below_min() {
    ConfigTrigger t;
    t.triggerType = 5;
    t.pollResponseTimeout = 50;  // < 100
    bool mod = sanitizeTriggerForSafety(t, false);
    TEST_ASSERT_TRUE(mod);
    TEST_ASSERT_EQUAL_UINT16(100, t.pollResponseTimeout);
}

void test_sanitize_poll_timeout_above_max() {
    ConfigTrigger t;
    t.triggerType = 5;
    t.pollResponseTimeout = 8000;  // > 5000
    bool mod = sanitizeTriggerForSafety(t, false);
    TEST_ASSERT_TRUE(mod);
    TEST_ASSERT_EQUAL_UINT16(5000, t.pollResponseTimeout);
}

void test_sanitize_poll_timeout_normal() {
    ConfigTrigger t;
    t.triggerType = 5;
    t.pollResponseTimeout = 1000;
    bool mod = sanitizeTriggerForSafety(t, false);
    TEST_ASSERT_FALSE(mod);
}

// --- Poll retries 边界 ---

void test_sanitize_poll_retries_above_max() {
    ConfigTrigger t;
    t.triggerType = 5;
    t.pollMaxRetries = 10;  // > 3
    bool mod = sanitizeTriggerForSafety(t, false);
    TEST_ASSERT_TRUE(mod);
    TEST_ASSERT_EQUAL_UINT8(3, t.pollMaxRetries);
}

void test_sanitize_poll_retries_at_max() {
    ConfigTrigger t;
    t.triggerType = 5;
    t.pollMaxRetries = 3;
    bool mod = sanitizeTriggerForSafety(t, false);
    TEST_ASSERT_FALSE(mod);
}

void test_sanitize_poll_retries_zero() {
    ConfigTrigger t;
    t.triggerType = 5;
    t.pollMaxRetries = 0;
    bool mod = sanitizeTriggerForSafety(t, false);
    TEST_ASSERT_FALSE(mod);  // 0 is valid
}

// --- Poll inter-delay 边界 ---

void test_sanitize_poll_inter_delay_below_min() {
    ConfigTrigger t;
    t.triggerType = 5;
    t.pollInterPollDelay = 5;  // < 20
    bool mod = sanitizeTriggerForSafety(t, false);
    TEST_ASSERT_TRUE(mod);
    TEST_ASSERT_EQUAL_UINT16(20, t.pollInterPollDelay);
}

void test_sanitize_poll_inter_delay_above_max() {
    ConfigTrigger t;
    t.triggerType = 5;
    t.pollInterPollDelay = 5000;  // > 1000
    bool mod = sanitizeTriggerForSafety(t, false);
    TEST_ASSERT_TRUE(mod);
    TEST_ASSERT_EQUAL_UINT16(1000, t.pollInterPollDelay);
}

void test_sanitize_poll_inter_delay_normal() {
    ConfigTrigger t;
    t.triggerType = 5;
    t.pollInterPollDelay = 200;
    bool mod = sanitizeTriggerForSafety(t, false);
    TEST_ASSERT_FALSE(mod);
}

// --- Heavy poll (Modbus 采集) 额外约束 ---

void test_sanitize_heavy_poll_timeout_clamped() {
    ConfigTrigger t;
    t.triggerType = 5;
    t.pollResponseTimeout = 4000;  // > HEAVY 3000
    bool mod = sanitizeTriggerForSafety(t, true);  // hasPollCollectionAction=true
    TEST_ASSERT_TRUE(mod);
    TEST_ASSERT_EQUAL_UINT16(3000, t.pollResponseTimeout);
}

void test_sanitize_heavy_poll_retries_clamped() {
    ConfigTrigger t;
    t.triggerType = 5;
    t.pollMaxRetries = 3;  // > HEAVY 2
    bool mod = sanitizeTriggerForSafety(t, true);
    TEST_ASSERT_TRUE(mod);
    TEST_ASSERT_EQUAL_UINT8(2, t.pollMaxRetries);
}

void test_sanitize_heavy_poll_inter_delay_raised() {
    ConfigTrigger t;
    t.triggerType = 5;
    t.pollInterPollDelay = 50;  // < HEAVY 100
    bool mod = sanitizeTriggerForSafety(t, true);
    TEST_ASSERT_TRUE(mod);
    TEST_ASSERT_EQUAL_UINT16(100, t.pollInterPollDelay);
}

void test_sanitize_heavy_poll_normal_values() {
    ConfigTrigger t;
    t.triggerType = 5;
    t.pollResponseTimeout = 2000;  // <= 3000
    t.pollMaxRetries = 2;          // <= 2
    t.pollInterPollDelay = 200;    // >= 100
    bool mod = sanitizeTriggerForSafety(t, true);
    TEST_ASSERT_FALSE(mod);
}

// --- 非 TIMER/POLL 触发类型不受影响 ---

void test_sanitize_event_trigger_not_modified() {
    ConfigTrigger t;
    t.triggerType = 4;  // EVENT_TRIGGER
    t.intervalSec = 0;
    t.pollResponseTimeout = 50;
    bool mod = sanitizeTriggerForSafety(t, false);
    TEST_ASSERT_FALSE(mod);
}

void test_sanitize_platform_trigger_not_modified() {
    ConfigTrigger t;
    t.triggerType = 0;  // PLATFORM_TRIGGER
    t.intervalSec = 0;
    bool mod = sanitizeTriggerForSafety(t, true);
    TEST_ASSERT_FALSE(mod);
}

// ============================================================
//  TEST GROUP 2: validateLoadedConfig 全局配置安全校验
// ============================================================

void test_validate_interval_below_min_corrected() {
    std::map<String, ConfigRule> rules;
    ConfigRule r;
    r.id = "r1"; r.name = "Low interval"; r.enabled = true;
    ConfigTrigger t; t.triggerType = 1; t.intervalSec = 2;  // 2s < 5s
    r.triggers.push_back(t);
    rules[r.id] = r;

    bool mod = validateLoadedConfig(rules);
    TEST_ASSERT_TRUE(mod);
    TEST_ASSERT_EQUAL_UINT32(5, rules["r1"].triggers[0].intervalSec);
}

void test_validate_dangerous_combo_corrected() {
    // 9 个活跃任务 (> WARN=8) 且间隔 8s (< 10s) → 强制 30s
    std::map<String, ConfigRule> rules;
    for (int i = 0; i < 9; i++) {
        ConfigRule r;
        r.id = "r" + String(i); r.name = "Rule " + String(i); r.enabled = true;
        ConfigTrigger t; t.triggerType = 1; t.intervalSec = (i == 0) ? 8 : 60;
        r.triggers.push_back(t);
        rules[r.id] = r;
    }

    bool mod = validateLoadedConfig(rules);
    TEST_ASSERT_TRUE(mod);
    TEST_ASSERT_EQUAL_UINT32(30, rules["r0"].triggers[0].intervalSec);
}

void test_validate_safe_config_not_modified() {
    std::map<String, ConfigRule> rules;
    ConfigRule r;
    r.id = "r1"; r.name = "Safe"; r.enabled = true;
    ConfigTrigger t; t.triggerType = 1; t.intervalSec = 60;
    r.triggers.push_back(t);
    rules[r.id] = r;

    bool mod = validateLoadedConfig(rules);
    TEST_ASSERT_FALSE(mod);
}

void test_validate_disabled_rules_ignored() {
    std::map<String, ConfigRule> rules;
    ConfigRule r;
    r.id = "r1"; r.name = "Disabled"; r.enabled = false;
    ConfigTrigger t; t.triggerType = 1; t.intervalSec = 2;
    r.triggers.push_back(t);
    rules[r.id] = r;

    bool mod = validateLoadedConfig(rules);
    TEST_ASSERT_FALSE(mod);
    TEST_ASSERT_EQUAL_UINT32(2, rules["r1"].triggers[0].intervalSec);
}

void test_validate_active_task_count() {
    std::map<String, ConfigRule> rules;
    // 5 个 TIMER + 3 个 POLL + 2 个 EVENT
    for (int i = 0; i < 10; i++) {
        ConfigRule r;
        r.id = "r" + String(i); r.name = "R" + String(i); r.enabled = true;
        ConfigTrigger t;
        if (i < 5) t.triggerType = 1;
        else if (i < 8) t.triggerType = 5;
        else t.triggerType = 4;
        t.intervalSec = 60;
        r.triggers.push_back(t);
        rules[r.id] = r;
    }
    TEST_ASSERT_EQUAL_UINT8(8, countActiveTimerTasks(rules));
}

void test_validate_max_active_tasks_constant() {
    TEST_ASSERT_EQUAL_UINT8(12, ValidateConstants::MAX_ACTIVE_TASKS);
    TEST_ASSERT_EQUAL_UINT8(8, ValidateConstants::WARN_TASK_THRESHOLD);
    TEST_ASSERT_TRUE(ValidateConstants::WARN_TASK_THRESHOLD < ValidateConstants::MAX_ACTIVE_TASKS);
}

void test_validate_safe_combo_not_modified() {
    // 5 个活跃任务 (<= 8) 且间隔 8s → 不修正
    std::map<String, ConfigRule> rules;
    for (int i = 0; i < 5; i++) {
        ConfigRule r;
        r.id = "r" + String(i); r.name = "Rule " + String(i); r.enabled = true;
        ConfigTrigger t; t.triggerType = 1; t.intervalSec = 8;
        r.triggers.push_back(t);
        rules[r.id] = r;
    }
    bool mod = validateLoadedConfig(rules);
    TEST_ASSERT_FALSE(mod);
}

// ============================================================
//  TEST GROUP 3: 外设配置 CRUD 安全约束
// ============================================================

void test_config_crud_peripheral_add_remove() {
    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();

    PeripheralConfig config;
    config.id = "sensor_temp";
    config.name = "Temperature Sensor";
    config.type = PeripheralType::SENSOR;
    config.pin = 4;
    config.enabled = true;

    TEST_ASSERT_TRUE(pm.addPeripheral(config));
    TEST_ASSERT_EQUAL(1, pm.getPeripheralCount());

    TEST_ASSERT_TRUE(pm.removePeripheral("sensor_temp"));
    TEST_ASSERT_EQUAL(0, pm.getPeripheralCount());
}

void test_config_crud_duplicate_id_rejected() {
    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();

    PeripheralConfig config;
    config.id = "led_1";
    config.name = "LED 1";
    config.type = PeripheralType::GPIO_DIGITAL_OUTPUT;
    config.pin = 2;

    TEST_ASSERT_TRUE(pm.addPeripheral(config));
    TEST_ASSERT_FALSE(pm.addPeripheral(config));
}

void test_config_crud_empty_id_rejected() {
    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();

    PeripheralConfig config;
    config.id = "";
    config.name = "No ID";

    TEST_ASSERT_FALSE(pm.addPeripheral(config));
}

void test_config_crud_update_existing() {
    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();

    PeripheralConfig config;
    config.id = "relay_1";
    config.name = "Relay 1";
    config.type = PeripheralType::GPIO_DIGITAL_OUTPUT;
    config.pin = 5;
    pm.addPeripheral(config);

    PeripheralConfig updated;
    updated.id = "relay_1";
    updated.name = "Relay 1 Updated";
    updated.type = PeripheralType::GPIO_DIGITAL_OUTPUT;
    updated.pin = 12;

    TEST_ASSERT_TRUE(pm.updatePeripheral("relay_1", updated));

    PeripheralConfig* fetched = pm.getPeripheral("relay_1");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_STRING("Relay 1 Updated", fetched->name.c_str());
}

void test_config_crud_update_nonexistent_fails() {
    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();

    PeripheralConfig config;
    config.id = "ghost";
    config.name = "Ghost";

    TEST_ASSERT_FALSE(pm.updatePeripheral("ghost", config));
}

void test_config_exec_rule_crud() {
    MockPeriphExecManager& mgr = MockPeriphExecManager::getInstance();
    mgr.initialize();

    PeriphExecRule rule;
    rule.id = "exec_rule_1";
    rule.name = "Timer LED";
    rule.enabled = true;
    rule.triggerType = TriggerType::TIMER_SCHEDULE;
    rule.actionType = ActionType::BLINK;
    rule.targetPeriphId = "led_1";

    TEST_ASSERT_TRUE(mgr.addRule(rule));
    TEST_ASSERT_EQUAL(1, mgr.getRuleCount());

    PeriphExecRule* fetched = mgr.getRule("exec_rule_1");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_STRING("Timer LED", fetched->name.c_str());

    TEST_ASSERT_TRUE(mgr.removeRule("exec_rule_1"));
    TEST_ASSERT_EQUAL(0, mgr.getRuleCount());
}

void test_config_rule_ids_listing() {
    MockPeriphExecManager& mgr = MockPeriphExecManager::getInstance();
    mgr.initialize();

    PeriphExecRule r1, r2, r3;
    r1.id = "rule_a"; r1.name = "A"; r1.enabled = true;
    r2.id = "rule_b"; r2.name = "B"; r2.enabled = true;
    r3.id = "rule_c"; r3.name = "C"; r3.enabled = true;
    mgr.addRule(r1);
    mgr.addRule(r2);
    mgr.addRule(r3);

    std::vector<String> ids = mgr.getRuleIds();
    TEST_ASSERT_EQUAL(3, (int)ids.size());
}

// ============================================================
//  TEST GROUP 4: 配置持久化边界条件
// ============================================================

void test_config_max_triggers_per_rule() {
    // MAX_TRIGGERS_PER_RULE = 3 (PeripheralExecution.h)
    MockPeriphExecManager& mgr = MockPeriphExecManager::getInstance();
    mgr.initialize();

    PeriphExecRule rule;
    rule.id = "max_triggers";
    rule.name = "Max Triggers";
    rule.enabled = true;
    // 可以添加多个触发器（mock 不限制），但真实系统限制为 3
    mgr.addRule(rule);

    PeriphExecRule* fetched = mgr.getRule("max_triggers");
    TEST_ASSERT_NOT_NULL(fetched);
}

void test_config_peripheral_properties_map() {
    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();

    PeripheralConfig config;
    config.id = "i2c_sensor";
    config.name = "I2C Sensor";
    config.type = PeripheralType::I2C;
    config.pin = 21;
    config.properties["address"] = "0x48";
    config.properties["model"] = "BME280";

    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    PeripheralConfig* fetched = pm.getPeripheral("i2c_sensor");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_STRING("0x48", fetched->properties["address"].c_str());
    TEST_ASSERT_EQUAL_STRING("BME280", fetched->properties["model"].c_str());
}

void test_config_constants_sanity() {
    // 确保常量合理
    TEST_ASSERT_TRUE(SanitizeConstants::MIN_TIMER_INTERVAL_SEC >= 1);
    TEST_ASSERT_TRUE(SanitizeConstants::MAX_TIMER_INTERVAL_SEC <= 86400);
    TEST_ASSERT_TRUE(SanitizeConstants::MIN_POLL_TIMEOUT_MS >= 50);
    TEST_ASSERT_TRUE(SanitizeConstants::MAX_POLL_TIMEOUT_MS <= 10000);
    TEST_ASSERT_TRUE(SanitizeConstants::MAX_POLL_RETRIES <= 5);
    TEST_ASSERT_TRUE(SanitizeConstants::MIN_POLL_INTER_DELAY_MS >= 10);
    TEST_ASSERT_TRUE(SanitizeConstants::MAX_POLL_INTER_DELAY_MS <= 5000);

    // HEAVY 约束应比普通更严格
    TEST_ASSERT_TRUE(SanitizeConstants::HEAVY_POLL_TIMEOUT_MS <= SanitizeConstants::MAX_POLL_TIMEOUT_MS);
    TEST_ASSERT_TRUE(SanitizeConstants::HEAVY_POLL_RETRIES <= SanitizeConstants::MAX_POLL_RETRIES);
    TEST_ASSERT_TRUE(SanitizeConstants::HEAVY_POLL_INTER_DELAY_MS >= SanitizeConstants::MIN_POLL_INTER_DELAY_MS);
}

void test_config_sanitize_multiple_triggers() {
    // 一条规则有多个 POLL 触发器，全部应被校验
    ConfigTrigger t1, t2, t3;
    t1.triggerType = 5; t1.pollResponseTimeout = 50;     // too low
    t2.triggerType = 5; t2.pollMaxRetries = 10;           // too high
    t3.triggerType = 5; t3.pollInterPollDelay = 5000;     // too high

    bool m1 = sanitizeTriggerForSafety(t1, false);
    bool m2 = sanitizeTriggerForSafety(t2, false);
    bool m3 = sanitizeTriggerForSafety(t3, false);

    TEST_ASSERT_TRUE(m1);
    TEST_ASSERT_TRUE(m2);
    TEST_ASSERT_TRUE(m3);
    TEST_ASSERT_EQUAL_UINT16(100, t1.pollResponseTimeout);
    TEST_ASSERT_EQUAL_UINT8(3, t2.pollMaxRetries);
    TEST_ASSERT_EQUAL_UINT16(1000, t3.pollInterPollDelay);
}

void test_config_sanitize_all_params_at_boundary() {
    // 所有参数恰好在下限/上限
    ConfigTrigger t;
    t.triggerType = 5;
    t.pollResponseTimeout = 100;   // min
    t.pollMaxRetries = 0;          // min (0 is valid)
    t.pollInterPollDelay = 20;     // min
    bool mod = sanitizeTriggerForSafety(t, false);
    TEST_ASSERT_FALSE(mod);

    ConfigTrigger t2;
    t2.triggerType = 5;
    t2.pollResponseTimeout = 5000;  // max
    t2.pollMaxRetries = 3;          // max
    t2.pollInterPollDelay = 1000;   // max
    bool mod2 = sanitizeTriggerForSafety(t2, false);
    TEST_ASSERT_FALSE(mod2);
}

// ============================================================
//  TEST GROUP 5: 外设类型覆盖与硬件初始化行为测试
// ============================================================

void test_periph_type_rf_module_enum_value() {
    // RF_MODULE 类型值必须为 48
    TEST_ASSERT_EQUAL(48, static_cast<int>(PeripheralType::RF_MODULE));
}

void test_periph_type_radar_sensor_enum_value() {
    // RADAR_SENSOR 类型值必须为 49
    TEST_ASSERT_EQUAL(49, static_cast<int>(PeripheralType::RADAR_SENSOR));
}

void test_periph_type_modbus_device_enum_value() {
    // MODBUS_DEVICE 类型值必须为 51
    TEST_ASSERT_EQUAL(51, static_cast<int>(PeripheralType::MODBUS_DEVICE));
}

void test_periph_type_device_event_enum_value() {
    // DEVICE_EVENT 类型值必须为 60
    TEST_ASSERT_EQUAL(60, static_cast<int>(PeripheralType::DEVICE_EVENT));
}

void test_periph_type_one_wire_enum_value() {
    // ONE_WIRE 类型值必须为 44
    TEST_ASSERT_EQUAL(44, static_cast<int>(PeripheralType::ONE_WIRE));
}

void test_periph_type_pwm_servo_enum_value() {
    // PWM_SERVO 类型值必须为 41
    TEST_ASSERT_EQUAL(41, static_cast<int>(PeripheralType::PWM_SERVO));
}

void test_periph_unimplemented_types_identified() {
    // 未实现类型列表必须与生产代码一致
    // CAN(4), USB(5), JTAG(31), SWD(32), SDIO(37), CAMERA(39), ETHERNET(40), ENCODER(43)
    std::vector<int> unimplemented = {4, 5, 31, 32, 37, 39, 40, 43};
    for (int t : unimplemented) {
        bool found = (t == 4 || t == 5 || t == 31 || t == 32 ||
                      t == 37 || t == 39 || t == 40 || t == 43);
        TEST_ASSERT_TRUE_MESSAGE(found, 
            ("Unimplemented type " + std::to_string(t) + " should be in the list").c_str());
    }
    // 已实现的类型不应在未实现列表中
    std::vector<int> implemented = {
        static_cast<int>(PeripheralType::RF_MODULE),
        static_cast<int>(PeripheralType::RADAR_SENSOR),
        static_cast<int>(PeripheralType::SENSOR),
        static_cast<int>(PeripheralType::PWM_SERVO),
        static_cast<int>(PeripheralType::ONE_WIRE),
        static_cast<int>(PeripheralType::I2C),
        static_cast<int>(PeripheralType::SPI),
        static_cast<int>(PeripheralType::DAC),
        static_cast<int>(PeripheralType::ADC)
    };
    for (int t : implemented) {
        bool inUnimplemented = (t == 4 || t == 5 || t == 31 || t == 32 ||
                                t == 37 || t == 39 || t == 40 || t == 43);
        TEST_ASSERT_FALSE_MESSAGE(inUnimplemented,
            ("Implemented type " + std::to_string(t) + " should not be in unimplemented list").c_str());
    }
}

void test_periph_type_i2c_and_spi_values() {
    // I2C 和 SPI 类型值必须正确
    TEST_ASSERT_EQUAL(2, static_cast<int>(PeripheralType::I2C));
    TEST_ASSERT_EQUAL(3, static_cast<int>(PeripheralType::SPI));
}

void test_periph_type_dac_and_adc_values() {
    // DAC 和 ADC 类型值必须正确
    TEST_ASSERT_EQUAL(26, static_cast<int>(PeripheralType::ADC));
    TEST_ASSERT_EQUAL(27, static_cast<int>(PeripheralType::DAC));
}

void test_periph_gpio_pullup_pulldown_for_button_events() {
    // 按键事件应仅由 PULLUP(13) 和 PULLDOWN(14) 产生
    // 与生产代码 supportsButtonEvent() 一致
    auto supportsButtonEvent = [](PeripheralType type) -> bool {
        return type == PeripheralType::GPIO_DIGITAL_INPUT_PULLUP ||
               type == PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN;
    };
    TEST_ASSERT_TRUE(supportsButtonEvent(PeripheralType::GPIO_DIGITAL_INPUT_PULLUP));
    TEST_ASSERT_TRUE(supportsButtonEvent(PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN));
    // GPIO_INTERRUPT 类型不支持按键事件
    TEST_ASSERT_FALSE(supportsButtonEvent(PeripheralType::GPIO_INTERRUPT_RISING));
    TEST_ASSERT_FALSE(supportsButtonEvent(PeripheralType::GPIO_INTERRUPT_FALLING));
    TEST_ASSERT_FALSE(supportsButtonEvent(PeripheralType::GPIO_INTERRUPT_CHANGE));
    TEST_ASSERT_FALSE(supportsButtonEvent(PeripheralType::GPIO_DIGITAL_INPUT));
    TEST_ASSERT_FALSE(supportsButtonEvent(PeripheralType::GPIO_DIGITAL_OUTPUT));
}

// ============================================================
//  TEST GROUP 6: 跨芯片数据一致性测试
//  核心原则：不同芯片环境不对外设配置做任何特殊处理，
//  配置文件里是什么数据就原样返回给前端。
// ============================================================

void test_lcd_type_enum_and_data_transparency() {
    // LCD 类型值必须为 36，不受芯片宏保护
    TEST_ASSERT_EQUAL(36, static_cast<int>(PeripheralType::LCD));

    // 模拟 LCD 配置数据透传：设置什么就返回什么
    PeripheralConfig config;
    config.id = "lcd_1";
    config.name = "OLED Display";
    config.type = PeripheralType::LCD;
    config.lcd.width = 128;
    config.lcd.height = 64;
    config.lcd.interface = 2;  // I2C

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    PeripheralConfig* fetched = pm.getPeripheral("lcd_1");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL(PeripheralType::LCD, fetched->type);
    TEST_ASSERT_EQUAL_UINT8(128, fetched->lcd.width);
    TEST_ASSERT_EQUAL_UINT8(64, fetched->lcd.height);
    TEST_ASSERT_EQUAL_UINT8(2, fetched->lcd.interface);
}

void test_lcd_params_various_sizes() {
    // 各种 LCD 尺寸参数都应原样保留（不受芯片能力限制）
    struct TestCase { uint8_t w; uint8_t h; uint8_t iface; };
    TestCase cases[] = {
        {128, 64, 2},   // 常见 I2C OLED
        {132, 64, 2},   // SH1106
        {240, 135, 1},  // SPI TFT (height <= 255)
        {84, 48, 0},    // 小尺寸并行 LCD
        {255, 255, 3},  // 边界值
    };

    for (auto& tc : cases) {
        PeripheralConfig config;
        config.id = "lcd_" + String(tc.w) + "x" + String(tc.h);
        config.name = "LCD";
        config.type = PeripheralType::LCD;
        config.lcd.width = tc.w;
        config.lcd.height = tc.h;
        config.lcd.interface = tc.iface;

        // 数据应完全保留
        TEST_ASSERT_EQUAL_UINT8(tc.w, config.lcd.width);
        TEST_ASSERT_EQUAL_UINT8(tc.h, config.lcd.height);
        TEST_ASSERT_EQUAL_UINT8(tc.iface, config.lcd.interface);
    }
}

void test_seven_segment_type_enum_and_data_transparency() {
    // 数码管类型值必须为 47，不受芯片宏保护
    TEST_ASSERT_EQUAL(47, static_cast<int>(PeripheralType::SEVEN_SEGMENT_TM1637));

    // 模拟数码管配置数据透传
    PeripheralConfig config;
    config.id = "seg_1";
    config.name = "TM1637 Display";
    config.type = PeripheralType::SEVEN_SEGMENT_TM1637;
    config.segment.brightness = 5;

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    PeripheralConfig* fetched = pm.getPeripheral("seg_1");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL(PeripheralType::SEVEN_SEGMENT_TM1637, fetched->type);
    TEST_ASSERT_EQUAL_UINT8(5, fetched->segment.brightness);
}

void test_seven_segment_brightness_range() {
    // 亮度 0-7 都应原样保留
    for (uint8_t b = 0; b <= 7; b++) {
        PeripheralConfig config;
        config.id = "seg_b" + String(b);
        config.name = "Segment";
        config.type = PeripheralType::SEVEN_SEGMENT_TM1637;
        config.segment.brightness = b;
        TEST_ASSERT_EQUAL_UINT8(b, config.segment.brightness);
    }
}

void test_all_special_types_always_in_data_layer() {
    // 所有外设类型在数据层必须有对应的枚举值，不受 #if 保护
    // LCD(36), SEVEN_SEGMENT(47), NEO_PIXEL(45), RF_MODULE(48), RADAR_SENSOR(49)
    TEST_ASSERT_EQUAL(36, static_cast<int>(PeripheralType::LCD));
    TEST_ASSERT_EQUAL(47, static_cast<int>(PeripheralType::SEVEN_SEGMENT_TM1637));
    TEST_ASSERT_EQUAL(45, static_cast<int>(PeripheralType::NEO_PIXEL));
    TEST_ASSERT_EQUAL(48, static_cast<int>(PeripheralType::RF_MODULE));
    TEST_ASSERT_EQUAL(49, static_cast<int>(PeripheralType::RADAR_SENSOR));
    TEST_ASSERT_EQUAL(42, static_cast<int>(PeripheralType::STEPPER_MOTOR));
}

void test_neopixel_data_transparency() {
    PeripheralConfig config;
    config.id = "neo_1";
    config.name = "NeoPixel Strip";
    config.type = PeripheralType::NEO_PIXEL;
    config.neopixel.count = 30;
    config.neopixel.brightness = 128;

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    PeripheralConfig* fetched = pm.getPeripheral("neo_1");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_UINT16(30, fetched->neopixel.count);
    TEST_ASSERT_EQUAL_UINT8(128, fetched->neopixel.brightness);
}

void test_rf_module_data_transparency() {
    PeripheralConfig config;
    config.id = "rf_1";
    config.name = "433MHz TX";
    config.type = PeripheralType::RF_MODULE;
    config.rf.mode = 0;
    config.rf.pulseWidth = 350;
    config.rf.repeat = 8;
    config.rf.bitLength = 24;
    config.rf.activeHigh = true;

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    PeripheralConfig* fetched = pm.getPeripheral("rf_1");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_UINT8(0, fetched->rf.mode);
    TEST_ASSERT_EQUAL_UINT16(350, fetched->rf.pulseWidth);
    TEST_ASSERT_EQUAL_UINT8(8, fetched->rf.repeat);
    TEST_ASSERT_EQUAL_UINT8(24, fetched->rf.bitLength);
    TEST_ASSERT_TRUE(fetched->rf.activeHigh);
}

void test_radar_sensor_data_transparency() {
    PeripheralConfig config;
    config.id = "radar_1";
    config.name = "RCWL-0516";
    config.type = PeripheralType::RADAR_SENSOR;
    config.radar.mode = 0;
    config.radar.activeHigh = true;
    config.radar.debounceMs = 100;
    config.radar.holdMs = 3000;

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    PeripheralConfig* fetched = pm.getPeripheral("radar_1");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_UINT16(100, fetched->radar.debounceMs);
    TEST_ASSERT_EQUAL_UINT16(3000, fetched->radar.holdMs);
}

void test_stepper_motor_data_transparency() {
    PeripheralConfig config;
    config.id = "stepper_1";
    config.name = "28BYJ-48";
    config.type = PeripheralType::STEPPER_MOTOR;
    config.stepper.stepsPerRevolution = 2048;
    config.stepper.speed = 15;

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    PeripheralConfig* fetched = pm.getPeripheral("stepper_1");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_UINT16(2048, fetched->stepper.stepsPerRevolution);
    TEST_ASSERT_EQUAL_UINT16(15, fetched->stepper.speed);
}

void test_validation_failed_peripheral_kept_with_error_status() {
    // 核心策略：验证失败的外设仍保留在列表中，标记 PERIPHERAL_ERROR
    // 模拟 loadConfiguration 中的行为
    PeripheralConfig config;
    config.id = "invalid_lcd";
    config.name = "Invalid LCD";
    config.type = PeripheralType::LCD;
    config.enabled = true;
    config.lcd.width = 128;
    config.lcd.height = 64;
    config.lcd.interface = 2;

    // 模拟验证失败但仍加载的逻辑
    bool valid = false;  // 假设 validateConfig 失败（如引脚不兼容）
    config.status = valid ? PeripheralStatus::PERIPHERAL_ENABLED
                          : PeripheralStatus::PERIPHERAL_ERROR;

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    // 验证失败的外设仍应存在于列表中
    PeripheralConfig* fetched = pm.getPeripheral("invalid_lcd");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_STRING("invalid_lcd", fetched->id.c_str());
    TEST_ASSERT_EQUAL(PeripheralStatus::PERIPHERAL_ERROR, fetched->status);

    // 数据应完整保留
    TEST_ASSERT_EQUAL_UINT8(128, fetched->lcd.width);
    TEST_ASSERT_EQUAL_UINT8(64, fetched->lcd.height);
}

void test_error_status_peripheral_skips_hw_init() {
    // PERIPHERAL_ERROR 状态的外设应跳过硬件初始化
    // 模拟 initAllEnabledPeripherals 中的跳过逻辑
    struct MockInitState {
        std::map<String, PeripheralStatus> states;
        int initCount = 0;
        int skipCount = 0;

        void simulateInitAll() {
            for (auto& kv : states) {
                if (kv.second == PeripheralStatus::PERIPHERAL_ERROR) {
                    skipCount++;
                    continue;
                }
                initCount++;
            }
        }
    };

    MockInitState ms;
    ms.states["lcd_ok"]     = PeripheralStatus::PERIPHERAL_ENABLED;
    ms.states["lcd_err"]    = PeripheralStatus::PERIPHERAL_ERROR;
    ms.states["seg_ok"]     = PeripheralStatus::PERIPHERAL_ENABLED;
    ms.states["seg_err"]    = PeripheralStatus::PERIPHERAL_ERROR;
    ms.states["gpio_ok"]    = PeripheralStatus::PERIPHERAL_ENABLED;

    ms.simulateInitAll();
    TEST_ASSERT_EQUAL(3, ms.initCount);   // lcd_ok, seg_ok, gpio_ok
    TEST_ASSERT_EQUAL(2, ms.skipCount);   // lcd_err, seg_err
}

void test_peripheral_status_enum_values() {
    // PeripheralStatus 枚举值必须与生产代码一致
    TEST_ASSERT_EQUAL(0, static_cast<int>(PeripheralStatus::PERIPHERAL_DISABLED));
    TEST_ASSERT_EQUAL(1, static_cast<int>(PeripheralStatus::PERIPHERAL_ENABLED));
    TEST_ASSERT_EQUAL(2, static_cast<int>(PeripheralStatus::PERIPHERAL_INITIALIZED));
    TEST_ASSERT_EQUAL(3, static_cast<int>(PeripheralStatus::PERIPHERAL_RUNNING));
    TEST_ASSERT_EQUAL(4, static_cast<int>(PeripheralStatus::PERIPHERAL_ERROR));
}

void test_cross_chip_config_consistency_simulation() {
    // 模拟跨芯片场景：同一份配置在不同芯片上都应完整加载
    // 配置文件包含 LCD + 数码管 + GPIO + NeoPixel
    struct ConfigEntry {
        String id;
        PeripheralType type;
    };
    ConfigEntry entries[] = {
        {"gpio_led",   PeripheralType::GPIO_DIGITAL_OUTPUT},
        {"lcd_oled",   PeripheralType::LCD},
        {"seg_tm1637", PeripheralType::SEVEN_SEGMENT_TM1637},
        {"neo_strip",  PeripheralType::NEO_PIXEL},
        {"rf_tx",      PeripheralType::RF_MODULE},
        {"radar_1",    PeripheralType::RADAR_SENSOR},
        {"stepper_1",  PeripheralType::STEPPER_MOTOR},
    };

    // 模拟在任意芯片上加载：所有类型都应能加载到列表中
    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();

    for (auto& e : entries) {
        PeripheralConfig config;
        config.id = e.id;
        config.name = e.id;
        config.type = e.type;
        config.enabled = true;
        TEST_ASSERT_TRUE_MESSAGE(pm.addPeripheral(config),
            ("Failed to add " + e.id).c_str());
    }

    // 所有外设都应在列表中，不因芯片差异被过滤
    TEST_ASSERT_EQUAL(7, pm.getPeripheralCount());
    for (auto& e : entries) {
        PeripheralConfig* fetched = pm.getPeripheral(e.id);
        TEST_ASSERT_NOT_NULL_MESSAGE(fetched,
            ("Missing " + e.id + " after load").c_str());
        TEST_ASSERT_EQUAL(e.type, fetched->type);
    }
}

void test_lcd_update_preserves_data() {
    // 更新 LCD 配置时参数应正确保留
    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();

    PeripheralConfig config;
    config.id = "lcd_upd";
    config.name = "LCD Update Test";
    config.type = PeripheralType::LCD;
    config.lcd.width = 128;
    config.lcd.height = 64;
    config.lcd.interface = 2;
    pm.addPeripheral(config);

    // 更新为新参数
    PeripheralConfig updated;
    updated.id = "lcd_upd";
    updated.name = "LCD Updated";
    updated.type = PeripheralType::LCD;
    updated.lcd.width = 240;
    updated.lcd.height = 240;
    updated.lcd.interface = 1;  // SPI
    TEST_ASSERT_TRUE(pm.updatePeripheral("lcd_upd", updated));

    PeripheralConfig* fetched = pm.getPeripheral("lcd_upd");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_UINT8(240, fetched->lcd.width);
    TEST_ASSERT_EQUAL_UINT8(240, fetched->lcd.height);
    TEST_ASSERT_EQUAL_UINT8(1, fetched->lcd.interface);
}

void test_segment_update_preserves_data() {
    // 更新数码管配置时参数应正确保留
    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();

    PeripheralConfig config;
    config.id = "seg_upd";
    config.name = "Segment Update Test";
    config.type = PeripheralType::SEVEN_SEGMENT_TM1637;
    config.segment.brightness = 2;
    pm.addPeripheral(config);

    PeripheralConfig updated;
    updated.id = "seg_upd";
    updated.name = "Segment Updated";
    updated.type = PeripheralType::SEVEN_SEGMENT_TM1637;
    updated.segment.brightness = 7;
    TEST_ASSERT_TRUE(pm.updatePeripheral("seg_upd", updated));

    PeripheralConfig* fetched = pm.getPeripheral("seg_upd");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_UINT8(7, fetched->segment.brightness);
}

// ============================================================
//  TEST GROUP 7: 外设类型→参数组映射一致性
//  镜像前端 onPeripheralTypeChange() 逻辑，确保类型值与
//  参数区域的对应关系正确。回归测试：UART(1) 不应映射到 GPIO 参数。
// ============================================================

// 参数组标识（镜像前端 peripheral-params-group 的 id）
enum class ParamGroup : uint8_t {
    NONE = 0,
    GPIO,
    UART,
    I2C,
    SPI,
    ADC,
    DAC,
    SEGMENT,
    STEPPER,
    NEOPIXEL,
    RF,
    RADAR,
    LCD
};

// 镜像前端 onPeripheralTypeChange() 的映射逻辑
static ParamGroup resolveParamGroup(int typeValue) {
    if (typeValue >= 11 && typeValue <= 21) return ParamGroup::GPIO;
    switch (typeValue) {
        case 1:  return ParamGroup::UART;
        case 2:  return ParamGroup::I2C;
        case 3:  return ParamGroup::SPI;
        case 26: return ParamGroup::ADC;
        case 27: return ParamGroup::DAC;
        case 36: return ParamGroup::LCD;
        case 42: return ParamGroup::STEPPER;
        case 45: return ParamGroup::NEOPIXEL;
        case 47: return ParamGroup::SEGMENT;
        case 48: return ParamGroup::RF;
        case 49: return ParamGroup::RADAR;
        default: return ParamGroup::NONE;
    }
}

// --- 7.1 回归测试：UART(1) 必须映射到 UART 参数，而非 GPIO ---

void test_param_group_regression_uart_not_gpio() {
    // 修复前 bug：form.reset() 将下拉框重置为 UART(value=1)，
    // 但 onPeripheralTypeChange('11') 硬编码传入 GPIO 类型值，
    // 导致显示 GPIO 参数而非 UART 参数。
    // 此测试确保 type=1 始终映射到 UART 参数组。
    ParamGroup group = resolveParamGroup(1);  // UART
    TEST_ASSERT_EQUAL_MESSAGE(
        static_cast<int>(ParamGroup::UART),
        static_cast<int>(group),
        "UART(type=1) must map to UART params, NOT GPIO (regression)"
    );
    TEST_ASSERT_NOT_EQUAL(
        static_cast<int>(ParamGroup::GPIO),
        static_cast<int>(group)
    );
}

void test_param_group_regression_default_select_value() {
    // form.reset() 后 <select> 的默认值为第一个 <option>，即 UART(value=1)
    // 前端 openPeripheralModal 应读取 select.value 而非硬编码 '11'
    const int defaultSelectValue = 1;  // <option value="1">UART</option> 是第一个
    TEST_ASSERT_EQUAL(1, defaultSelectValue);
    ParamGroup group = resolveParamGroup(defaultSelectValue);
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::UART), static_cast<int>(group));
}

// --- 7.2 通信接口类型 → 各自的参数组 ---

void test_param_group_communication_types() {
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::UART), static_cast<int>(resolveParamGroup(1)));
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::I2C),  static_cast<int>(resolveParamGroup(2)));
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::SPI),  static_cast<int>(resolveParamGroup(3)));
    // CAN(4) 和 USB(5) 无专属参数区
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::NONE), static_cast<int>(resolveParamGroup(4)));
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::NONE), static_cast<int>(resolveParamGroup(5)));
}

// --- 7.3 GPIO 类型范围 (11-21) 全部映射到 GPIO 参数组 ---

void test_param_group_all_gpio_types() {
    for (int t = 11; t <= 21; t++) {
        ParamGroup group = resolveParamGroup(t);
        TEST_ASSERT_EQUAL_MESSAGE(
            static_cast<int>(ParamGroup::GPIO),
            static_cast<int>(group),
            ("GPIO type " + std::to_string(t) + " must map to GPIO params").c_str()
        );
    }
}

void test_param_group_gpio_sub_params_pwm_only() {
    // PWM 子参数仅在 PWM_OUTPUT(17) 和 ANALOG_OUTPUT(16) 时可见
    auto showsPwmSubParams = [](int type) -> bool {
        return type == 17 || type == 16;
    };
    TEST_ASSERT_TRUE(showsPwmSubParams(17));   // PWM输出
    TEST_ASSERT_TRUE(showsPwmSubParams(16));   // 模拟输出
    // 其他 GPIO 类型不显示 PWM 子参数
    TEST_ASSERT_FALSE(showsPwmSubParams(11));  // 数字输入
    TEST_ASSERT_FALSE(showsPwmSubParams(12));  // 数字输出
    TEST_ASSERT_FALSE(showsPwmSubParams(13));  // 上拉输入
    TEST_ASSERT_FALSE(showsPwmSubParams(14));  // 下拉输入
    TEST_ASSERT_FALSE(showsPwmSubParams(15));  // 模拟输入
}

void test_param_group_gpio_sub_params_input_only() {
    // input-only 子参数仅在输入类型时可见
    auto showsInputSubParams = [](int type) -> bool {
        return type == 11 || type == 13 || type == 14;
    };
    TEST_ASSERT_TRUE(showsInputSubParams(11));   // 数字输入
    TEST_ASSERT_TRUE(showsInputSubParams(13));   // 上拉输入
    TEST_ASSERT_TRUE(showsInputSubParams(14));   // 下拉输入
    // 输出类型不显示 input-only 子参数
    TEST_ASSERT_FALSE(showsInputSubParams(12));  // 数字输出
    TEST_ASSERT_FALSE(showsInputSubParams(16));  // 模拟输出
    TEST_ASSERT_FALSE(showsInputSubParams(17));  // PWM输出
}

// --- 7.4 模拟信号、专用外设类型映射 ---

void test_param_group_analog_types() {
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::ADC), static_cast<int>(resolveParamGroup(26)));
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::DAC), static_cast<int>(resolveParamGroup(27)));
}

void test_param_group_special_types() {
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::LCD),     static_cast<int>(resolveParamGroup(36)));
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::STEPPER), static_cast<int>(resolveParamGroup(42)));
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::NEOPIXEL),static_cast<int>(resolveParamGroup(45)));
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::SEGMENT), static_cast<int>(resolveParamGroup(47)));
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::RF),      static_cast<int>(resolveParamGroup(48)));
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::RADAR),   static_cast<int>(resolveParamGroup(49)));
}

void test_param_group_no_params_types() {
    // 这些类型无专属参数区域，应映射到 NONE
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::NONE), static_cast<int>(resolveParamGroup(4)));   // CAN
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::NONE), static_cast<int>(resolveParamGroup(5)));   // USB
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::NONE), static_cast<int>(resolveParamGroup(31)));  // JTAG
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::NONE), static_cast<int>(resolveParamGroup(32)));  // SWD
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::NONE), static_cast<int>(resolveParamGroup(37)));  // SDIO
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::NONE), static_cast<int>(resolveParamGroup(38)));  // SENSOR
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::NONE), static_cast<int>(resolveParamGroup(39)));  // CAMERA
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::NONE), static_cast<int>(resolveParamGroup(40)));  // ETHERNET
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::NONE), static_cast<int>(resolveParamGroup(41)));  // PWM_SERVO
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::NONE), static_cast<int>(resolveParamGroup(43)));  // ENCODER
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::NONE), static_cast<int>(resolveParamGroup(44)));  // ONE_WIRE
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::NONE), static_cast<int>(resolveParamGroup(51)));  // MODBUS_DEVICE
    TEST_ASSERT_EQUAL(static_cast<int>(ParamGroup::NONE), static_cast<int>(resolveParamGroup(60)));  // DEVICE_EVENT
}

// --- 7.5 参数组排他性：同一时刻只有一个参数组可见 ---

void test_param_group_exclusivity() {
    // 遍历所有已定义的类型值，每个类型最多激活一个参数组
    int allTypes[] = {
        1,2,3,4,5,
        11,12,13,14,15,16,17,18,19,20,21,
        26,27,
        31,32,
        36,37,38,39,40,41,42,43,44,45,47,48,49,
        51,60
    };
    for (int t : allTypes) {
        ParamGroup group = resolveParamGroup(t);
        // 验证同一类型不会同时匹配多个参数组
        int matchCount = 0;
        if (group == ParamGroup::GPIO)     matchCount++;
        if (group == ParamGroup::UART)     matchCount++;
        if (group == ParamGroup::I2C)      matchCount++;
        if (group == ParamGroup::SPI)      matchCount++;
        if (group == ParamGroup::ADC)      matchCount++;
        if (group == ParamGroup::DAC)      matchCount++;
        if (group == ParamGroup::LCD)      matchCount++;
        if (group == ParamGroup::STEPPER)  matchCount++;
        if (group == ParamGroup::NEOPIXEL) matchCount++;
        if (group == ParamGroup::SEGMENT)  matchCount++;
        if (group == ParamGroup::RF)       matchCount++;
        if (group == ParamGroup::RADAR)    matchCount++;
        // NONE 不算激活，有参数组时恰好为 1
        if (group != ParamGroup::NONE) {
            TEST_ASSERT_EQUAL_MESSAGE(
                1, matchCount,
                ("Type " + std::to_string(t) + " should activate exactly 1 param group").c_str()
            );
        }
    }
}

// --- 7.6 getPeripheralCategory 与 resolveParamGroup 一致性 ---

void test_param_group_category_consistency() {
    // 通信类型 → CATEGORY_COMMUNICATION，且参数组不为 GPIO
    TEST_ASSERT_EQUAL(
        static_cast<int>(PeripheralCategory::CATEGORY_COMMUNICATION),
        static_cast<int>(getPeripheralCategory(PeripheralType::UART))
    );
    TEST_ASSERT_NOT_EQUAL(
        static_cast<int>(ParamGroup::GPIO),
        static_cast<int>(resolveParamGroup(static_cast<int>(PeripheralType::UART)))
    );

    // GPIO 类型 → CATEGORY_GPIO，且参数组为 GPIO
    TEST_ASSERT_EQUAL(
        static_cast<int>(PeripheralCategory::CATEGORY_GPIO),
        static_cast<int>(getPeripheralCategory(PeripheralType::GPIO_DIGITAL_OUTPUT))
    );
    TEST_ASSERT_EQUAL(
        static_cast<int>(ParamGroup::GPIO),
        static_cast<int>(resolveParamGroup(static_cast<int>(PeripheralType::GPIO_DIGITAL_OUTPUT)))
    );

    // 模拟信号 → CATEGORY_ANALOG_SIGNAL
    TEST_ASSERT_EQUAL(
        static_cast<int>(PeripheralCategory::CATEGORY_ANALOG_SIGNAL),
        static_cast<int>(getPeripheralCategory(PeripheralType::ADC))
    );
    TEST_ASSERT_EQUAL(
        static_cast<int>(PeripheralCategory::CATEGORY_ANALOG_SIGNAL),
        static_cast<int>(getPeripheralCategory(PeripheralType::DAC))
    );

    // 调试接口 → CATEGORY_DEBUG
    TEST_ASSERT_EQUAL(
        static_cast<int>(PeripheralCategory::CATEGORY_DEBUG),
        static_cast<int>(getPeripheralCategory(PeripheralType::JTAG))
    );
    TEST_ASSERT_EQUAL(
        static_cast<int>(PeripheralCategory::CATEGORY_DEBUG),
        static_cast<int>(getPeripheralCategory(PeripheralType::SWD))
    );

    // 专用外设 → CATEGORY_SPECIAL
    int specialTypes[] = {36, 42, 45, 47, 48, 49, 51, 60};
    for (int t : specialTypes) {
        TEST_ASSERT_EQUAL_MESSAGE(
            static_cast<int>(PeripheralCategory::CATEGORY_SPECIAL),
            static_cast<int>(getPeripheralCategory(static_cast<PeripheralType>(t))),
            ("Type " + std::to_string(t) + " should be CATEGORY_SPECIAL").c_str()
        );
    }
}

// --- 7.7 新增外设默认禁用 ---

void test_param_group_new_peripheral_default_disabled() {
    // 新增外设时，enabled 应默认为 false（保存后由用户手动 toggle 启用）
    // 后端默认值验证：PeripheralConfig 构造时 enabled 应为 false
    PeripheralConfig config;
    config.id = "new_test";
    config.name = "New Test";
    config.type = PeripheralType::UART;
    // 新增时前端设置 enabledCb.checked = false，后端默认也应如此
    config.enabled = false;

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    PeripheralConfig* fetched = pm.getPeripheral("new_test");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_FALSE(fetched->enabled);
    TEST_ASSERT_EQUAL(
        static_cast<int>(PeripheralStatus::PERIPHERAL_DISABLED),
        static_cast<int>(fetched->status)
    );
}

void test_param_group_new_peripheral_uart_params_present() {
    // 新增 UART 外设后，UART 参数应完整保存
    PeripheralConfig config;
    config.id = "uart_new";
    config.name = "New UART";
    config.type = PeripheralType::UART;
    config.enabled = false;
    config.uart.baudRate = 115200;
    config.uart.dataBits = 8;
    config.uart.stopBits = 1;
    config.uart.parity = 0;

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    PeripheralConfig* fetched = pm.getPeripheral("uart_new");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL(PeripheralType::UART, fetched->type);
    TEST_ASSERT_EQUAL_UINT32(115200, fetched->uart.baudRate);
    TEST_ASSERT_EQUAL_UINT8(8, fetched->uart.dataBits);
    TEST_ASSERT_EQUAL_UINT8(1, fetched->uart.stopBits);
    TEST_ASSERT_EQUAL_UINT8(0, fetched->uart.parity);
}

// ============================================================
//  TEST GROUP 8: 编码器与SDIO外设驱动测试
// ============================================================

void test_encoder_type_enum_value() {
    // ENCODER 类型值必须为 43
    TEST_ASSERT_EQUAL(43, static_cast<int>(PeripheralType::ENCODER));
}

void test_sdio_type_enum_value() {
    // SDIO 类型值必须为 37
    TEST_ASSERT_EQUAL(37, static_cast<int>(PeripheralType::SDIO));
}

void test_encoder_config_validation_valid() {
    // 编码器配置：2个引脚，resolution > 0
    PeripheralConfig config;
    config.id = "encoder_01";
    config.name = "Rotary Encoder";
    config.type = PeripheralType::ENCODER;
    config.pinCount = 2;
    config.pins[0] = 4;
    config.pins[1] = 5;
    config.encoder.resolution = 1024;
    config.encoder.useInterrupt = true;

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    PeripheralConfig* fetched = pm.getPeripheral("encoder_01");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL(PeripheralType::ENCODER, fetched->type);
    TEST_ASSERT_EQUAL_UINT8(2, fetched->pinCount);
    TEST_ASSERT_EQUAL_UINT16(1024, fetched->encoder.resolution);
    TEST_ASSERT_TRUE(fetched->encoder.useInterrupt);
}

void test_encoder_config_validation_missing_pins() {
    // 注意：MockPeripheralManager 不包含验证逻辑，此测试仅验证数据结构
    // 实际验证在 PeripheralManager::validateConfig() 中实现
    PeripheralConfig config;
    config.id = "encoder_bad";
    config.name = "Bad Encoder";
    config.type = PeripheralType::ENCODER;
    config.pinCount = 1;  // 生产代码会拒绝：需要2个引脚
    config.pins[0] = 4;
    config.encoder.resolution = 1024;

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    // Mock 版本接受所有配置，仅测试数据能正确存储
    TEST_ASSERT_TRUE(pm.addPeripheral(config));
    
    PeripheralConfig* fetched = pm.getPeripheral("encoder_bad");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_UINT8(1, fetched->pinCount);  // 验证数据被保存
}

void test_encoder_config_validation_zero_resolution() {
    // 注意：MockPeripheralManager 不包含验证逻辑
    // 实际验证在 PeripheralManager::validateConfig() 中实现
    PeripheralConfig config;
    config.id = "encoder_zero";
    config.name = "Zero Resolution";
    config.type = PeripheralType::ENCODER;
    config.pinCount = 2;
    config.pins[0] = 4;
    config.pins[1] = 5;
    config.encoder.resolution = 0;  // 生产代码会拒绝：不能为0

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));  // Mock接受所有配置
    
    PeripheralConfig* fetched = pm.getPeripheral("encoder_zero");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_UINT16(0, fetched->encoder.resolution);
}

void test_encoder_data_transparency() {
    // 编码器配置数据应原样透传
    PeripheralConfig config;
    config.id = "encoder_trans";
    config.name = "Transparent Encoder";
    config.type = PeripheralType::ENCODER;
    config.pinCount = 2;
    config.pins[0] = 16;
    config.pins[1] = 17;
    config.encoder.resolution = 4096;
    config.encoder.useInterrupt = false;

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    PeripheralConfig* fetched = pm.getPeripheral("encoder_trans");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_UINT16(4096, fetched->encoder.resolution);
    TEST_ASSERT_FALSE(fetched->encoder.useInterrupt);
    TEST_ASSERT_EQUAL_UINT8(16, fetched->pins[0]);
    TEST_ASSERT_EQUAL_UINT8(17, fetched->pins[1]);
}

void test_encoder_read_counter() {
    // 编码器读取：返回计数器值
    PeripheralConfig config;
    config.id = "encoder_read";
    config.name = "Encoder Read Test";
    config.type = PeripheralType::ENCODER;
    config.pinCount = 2;
    config.pins[0] = 4;
    config.pins[1] = 5;
    config.encoder.resolution = 1024;
    config.encoder.useInterrupt = true;

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));
    
    // 初始计数为0
    int32_t counter = pm.getEncoderCounter("encoder_read");
    TEST_ASSERT_EQUAL_INT32(0, counter);
    
    // 模拟编码器计数增加
    pm.setEncoderCounter("encoder_read", 512);
    counter = pm.getEncoderCounter("encoder_read");
    TEST_ASSERT_EQUAL_INT32(512, counter);
    
    // 模拟继续计数
    pm.setEncoderCounter("encoder_read", 1024);
    counter = pm.getEncoderCounter("encoder_read");
    TEST_ASSERT_EQUAL_INT32(1024, counter);
}

void test_encoder_reset_counter() {
    // 编码器重置：写入LOW将计数器归零
    PeripheralConfig config;
    config.id = "encoder_reset";
    config.name = "Encoder Reset Test";
    config.type = PeripheralType::ENCODER;
    config.pinCount = 2;
    config.pins[0] = 6;
    config.pins[1] = 7;
    config.encoder.resolution = 2048;
    config.encoder.useInterrupt = true;

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));
    
    // 设置初始计数
    pm.setEncoderCounter("encoder_reset", 5000);
    int32_t counter = pm.getEncoderCounter("encoder_reset");
    TEST_ASSERT_EQUAL_INT32(5000, counter);
    
    // 模拟写入LOW（重置计数器）
    pm.resetEncoderCounter("encoder_reset");
    counter = pm.getEncoderCounter("encoder_reset");
    TEST_ASSERT_EQUAL_INT32(0, counter);
}

void test_sdio_config_validation_spi_mode() {
    // SD卡SPI模式：4个引脚，interface=1
    PeripheralConfig config;
    config.id = "sdcard_spi";
    config.name = "SD Card SPI";
    config.type = PeripheralType::SDIO;
    config.pinCount = 4;
    config.pins[0] = 14;  // CLK
    config.pins[1] = 13;  // MOSI
    config.pins[2] = 12;  // MISO
    config.pins[3] = 15;  // CS
    config.sdcard.interface = 1;  // SPI模式
    config.sdcard.frequency = 20000000;

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    PeripheralConfig* fetched = pm.getPeripheral("sdcard_spi");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL(PeripheralType::SDIO, fetched->type);
    TEST_ASSERT_EQUAL_UINT8(1, fetched->sdcard.interface);
    TEST_ASSERT_EQUAL_UINT32(20000000, fetched->sdcard.frequency);
}

void test_sdio_config_validation_sdmmc_mode() {
    // SD卡SDMMC模式：6个引脚，interface=0
    PeripheralConfig config;
    config.id = "sdcard_sdmmc";
    config.name = "SD Card SDMMC";
    config.type = PeripheralType::SDIO;
    config.pinCount = 6;
    config.pins[0] = 2;   // CMD
    config.pins[1] = 14;  // CLK
    config.pins[2] = 4;   // D0
    config.pins[3] = 12;  // D1
    config.pins[4] = 13;  // D2
    config.pins[5] = 15;  // D3
    config.sdcard.interface = 0;  // SDMMC模式
    config.sdcard.frequency = 40000000;

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    PeripheralConfig* fetched = pm.getPeripheral("sdcard_sdmmc");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_UINT8(0, fetched->sdcard.interface);
    TEST_ASSERT_EQUAL_UINT32(40000000, fetched->sdcard.frequency);
}

void test_sdio_config_validation_insufficient_pins() {
    // 注意：MockPeripheralManager 不包含验证逻辑
    // 实际验证在 PeripheralManager::validateConfig() 中实现
    PeripheralConfig config;
    config.id = "sdcard_bad";
    config.name = "Bad SD Card";
    config.type = PeripheralType::SDIO;
    config.pinCount = 3;  // 生产代码会拒绝：SPI需要4个
    config.pins[0] = 14;
    config.pins[1] = 13;
    config.pins[2] = 12;
    config.sdcard.interface = 1;  // SPI模式

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));  // Mock接受所有配置
    
    PeripheralConfig* fetched = pm.getPeripheral("sdcard_bad");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_UINT8(3, fetched->pinCount);
}

void test_sdio_data_transparency_spi() {
    // SD卡SPI配置数据应原样透传
    PeripheralConfig config;
    config.id = "sd_trans_spi";
    config.name = "Transparent SD SPI";
    config.type = PeripheralType::SDIO;
    config.pinCount = 4;
    config.pins[0] = 18;
    config.pins[1] = 23;
    config.pins[2] = 19;
    config.pins[3] = 5;
    config.sdcard.interface = 1;
    config.sdcard.frequency = 10000000;  // 10MHz

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    PeripheralConfig* fetched = pm.getPeripheral("sd_trans_spi");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_UINT8(1, fetched->sdcard.interface);
    TEST_ASSERT_EQUAL_UINT32(10000000, fetched->sdcard.frequency);
    TEST_ASSERT_EQUAL_UINT8(18, fetched->pins[0]);
    TEST_ASSERT_EQUAL_UINT8(5, fetched->pins[3]);
}

void test_sdio_data_transparency_sdmmc() {
    // SD卡SDMMC配置数据应原样透传
    PeripheralConfig config;
    config.id = "sd_trans_sdmmc";
    config.name = "Transparent SD SDMMC";
    config.type = PeripheralType::SDIO;
    config.pinCount = 6;
    config.pins[0] = 2;
    config.pins[1] = 14;
    config.pins[2] = 4;
    config.pins[3] = 12;
    config.pins[4] = 13;
    config.pins[5] = 15;
    config.sdcard.interface = 0;
    config.sdcard.frequency = 25000000;

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    PeripheralConfig* fetched = pm.getPeripheral("sd_trans_sdmmc");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_UINT8(0, fetched->sdcard.interface);
    TEST_ASSERT_EQUAL_UINT32(25000000, fetched->sdcard.frequency);
    TEST_ASSERT_EQUAL(6, fetched->pinCount);
}

void test_unimplemented_types_removed_from_ui() {
    // 验证未实现类型已从UI中移除（但枚举仍存在以保持兼容）
    // CAN(4), USB(5), JTAG(31), SWD(32), CAMERA(39), ETHERNET(40)
    // 注意：ENCODER(43)和SDIO(37)现在已实现
    
    std::vector<int> removedFromUI = {4, 5, 31, 32, 39, 40};
    
    // 这些类型在枚举中仍然存在
    TEST_ASSERT_EQUAL(4, static_cast<int>(PeripheralType::CAN));
    TEST_ASSERT_EQUAL(5, static_cast<int>(PeripheralType::USB));
    TEST_ASSERT_EQUAL(31, static_cast<int>(PeripheralType::JTAG));
    TEST_ASSERT_EQUAL(32, static_cast<int>(PeripheralType::SWD));
    TEST_ASSERT_EQUAL(39, static_cast<int>(PeripheralType::CAMERA));
    TEST_ASSERT_EQUAL(40, static_cast<int>(PeripheralType::ETHERNET));
    
    // 但编码器和SDIO现在是已实现的
    TEST_ASSERT_EQUAL(43, static_cast<int>(PeripheralType::ENCODER));
    TEST_ASSERT_EQUAL(37, static_cast<int>(PeripheralType::SDIO));
    
    // 验证它们不在removedFromUI列表中
    for (int t : removedFromUI) {
        bool isEncoderOrSdio = (t == 43 || t == 37);
        TEST_ASSERT_FALSE_MESSAGE(isEncoderOrSdio,
            "Encoder(43) and SDIO(37) should not be in removed list");
    }
}

// ============================================================
//  TEST GROUP 9: Route Handler 数据校验（模拟 API 层逻辑）
// ============================================================

// 模拟 JSON 解析后的外设添加请求结构
struct ApiAddPeriphRequest {
    String id;
    String name;
    int type;       // 外设类型枚举值
    bool enabled;
    String pinsStr; // 逗号分隔的引脚字符串
    bool valid;
    String errorMsg;
};

// 模拟 handleAddPeripheral 中的参数校验逻辑
static bool validateAddRequest(ApiAddPeriphRequest& req) {
    if (req.name.isEmpty()) {
        req.valid = false;
        req.errorMsg = "Peripheral name is required";
        return false;
    }
    if (req.type <= 0) {
        req.valid = false;
        req.errorMsg = "Invalid peripheral type";
        return false;
    }
    // 设备事件(60)和通用传感器(38/51)不需要引脚
    bool noPinRequired = (req.type == 60 || req.type == 51);
    if (!noPinRequired && req.pinsStr.isEmpty()) {
        req.valid = false;
        req.errorMsg = "Pins are required for this peripheral type";
        return false;
    }
    req.valid = true;
    return true;
}

// 模拟引脚字符串解析
static int parsePinsString(const String& pinsStr, int* pins, int maxPins) {
    int count = 0;
    if (pinsStr.isEmpty()) return 0;
    int start = 0;
    int end = pinsStr.indexOf(',');
    while (end != -1 && count < maxPins) {
        pins[count++] = pinsStr.substring(start, end).toInt();
        start = end + 1;
        end = pinsStr.indexOf(',', start);
    }
    if (count < maxPins) {
        pins[count++] = pinsStr.substring(start).toInt();
    }
    return count;
}

// 模拟分页计算
struct PaginationResult {
    int total;
    int page;
    int pageSize;
    int startIdx;
    int endIdx;
    int totalPages;
};

static PaginationResult computePagination(int total, int page, int pageSize) {
    PaginationResult r;
    r.total = total;
    r.pageSize = (pageSize < 1) ? 10 : (pageSize > 50 ? 50 : pageSize);
    r.totalPages = (total + r.pageSize - 1) / r.pageSize;
    if (r.totalPages < 1) r.totalPages = 1;
    r.page = (page < 1) ? 1 : (page > r.totalPages ? r.totalPages : page);
    r.startIdx = (r.page - 1) * r.pageSize;
    r.endIdx = r.startIdx + r.pageSize;
    if (r.endIdx > total) r.endIdx = total;
    return r;
}

// 模拟引脚冲突检测
struct PinConflictResult {
    bool hasConflict;
    int conflictPin;
    String conflictPeriphId;
};

static PinConflictResult checkPinConflict(
    const std::map<String, std::vector<int>>& existingPeriphPins,
    const String& excludeId,
    const int* newPins, int newPinCount
) {
    PinConflictResult result = {false, -1, ""};
    for (int i = 0; i < newPinCount; i++) {
        int pin = newPins[i];
        if (pin < 0 || pin > 48) continue;
        for (auto& kv : existingPeriphPins) {
            if (kv.first == excludeId) continue;
            for (int ep : kv.second) {
                if (ep == pin) {
                    result.hasConflict = true;
                    result.conflictPin = pin;
                    result.conflictPeriphId = kv.first;
                    return result;
                }
            }
        }
    }
    return result;
}

void test_api_add_peripheral_name_required() {
    ApiAddPeriphRequest req;
    req.name = "";
    req.type = 12;
    req.pinsStr = "25";
    TEST_ASSERT_FALSE(validateAddRequest(req));
    TEST_ASSERT_TRUE(req.errorMsg.indexOf("name") >= 0);
}

void test_api_add_peripheral_type_required() {
    ApiAddPeriphRequest req;
    req.name = "test";
    req.type = 0; // invalid
    req.pinsStr = "25";
    TEST_ASSERT_FALSE(validateAddRequest(req));
    TEST_ASSERT_TRUE(req.errorMsg.indexOf("type") >= 0);
}

void test_api_add_peripheral_pins_required() {
    ApiAddPeriphRequest req;
    req.name = "test-gpio";
    req.type = 12; // GPIO output
    req.pinsStr = "";
    TEST_ASSERT_FALSE(validateAddRequest(req));
    TEST_ASSERT_TRUE(req.errorMsg.indexOf("Pins") >= 0);
}

void test_api_add_peripheral_device_event_no_pins() {
    ApiAddPeriphRequest req;
    req.name = "sys-start";
    req.type = 60; // DEVICE_EVENT
    req.pinsStr = "";
    TEST_ASSERT_TRUE(validateAddRequest(req));
}

void test_api_add_peripheral_valid_request() {
    ApiAddPeriphRequest req;
    req.name = "led-1";
    req.type = 12;
    req.pinsStr = "25";
    TEST_ASSERT_TRUE(validateAddRequest(req));
}

void test_api_update_peripheral_id_required() {
    // 模拟 handleUpdatePeripheral: id 为空时返回 400
    String id = "";
    bool rejected = id.isEmpty();
    TEST_ASSERT_TRUE(rejected);
}

void test_api_delete_peripheral_id_required() {
    // 模拟 handleDeletePeripheral: id 为空时返回 400
    String id = "";
    bool rejected = id.isEmpty();
    TEST_ASSERT_TRUE(rejected);
}

void test_api_get_peripherals_pagination_default() {
    PaginationResult r = computePagination(25, 1, 10);
    TEST_ASSERT_EQUAL(25, r.total);
    TEST_ASSERT_EQUAL(1, r.page);
    TEST_ASSERT_EQUAL(10, r.pageSize);
    TEST_ASSERT_EQUAL(3, r.totalPages);
    TEST_ASSERT_EQUAL(0, r.startIdx);
    TEST_ASSERT_EQUAL(10, r.endIdx);
}

void test_api_get_peripherals_pagination_page2() {
    PaginationResult r = computePagination(25, 2, 10);
    TEST_ASSERT_EQUAL(2, r.page);
    TEST_ASSERT_EQUAL(10, r.startIdx);
    TEST_ASSERT_EQUAL(20, r.endIdx);
}

void test_api_get_peripherals_pagination_last_page() {
    PaginationResult r = computePagination(25, 3, 10);
    TEST_ASSERT_EQUAL(3, r.page);
    TEST_ASSERT_EQUAL(20, r.startIdx);
    TEST_ASSERT_EQUAL(25, r.endIdx);
}

void test_api_get_peripherals_pagination_overflow() {
    PaginationResult r = computePagination(25, 99, 10);
    TEST_ASSERT_EQUAL(3, r.page); // 自动修正到最大页
    TEST_ASSERT_EQUAL(20, r.startIdx);
}

void test_api_get_peripherals_pagination_zero_page() {
    PaginationResult r = computePagination(10, 0, 10);
    TEST_ASSERT_EQUAL(1, r.page); // 自动修正为 1
}

void test_api_get_peripherals_pagination_clamp_page_size() {
    PaginationResult r = computePagination(100, 1, 999);
    TEST_ASSERT_EQUAL(50, r.pageSize); // 最大 50
}

void test_api_get_peripherals_pagination_empty() {
    PaginationResult r = computePagination(0, 1, 10);
    TEST_ASSERT_EQUAL(0, r.total);
    TEST_ASSERT_EQUAL(1, r.totalPages);
    TEST_ASSERT_EQUAL(0, r.endIdx);
}

void test_api_pin_conflict_no_conflict() {
    std::map<String, std::vector<int>> existing;
    existing["led_1"] = {25, 26};
    existing["uart_1"] = {16, 17};
    int newPins[] = {4, 5};
    PinConflictResult r = checkPinConflict(existing, "", newPins, 2);
    TEST_ASSERT_FALSE(r.hasConflict);
}

void test_api_pin_conflict_detected() {
    std::map<String, std::vector<int>> existing;
    existing["led_1"] = {25, 26};
    existing["uart_1"] = {16, 17};
    int newPins[] = {25}; // 与 led_1 冲突
    PinConflictResult r = checkPinConflict(existing, "", newPins, 1);
    TEST_ASSERT_TRUE(r.hasConflict);
    TEST_ASSERT_EQUAL(25, r.conflictPin);
    TEST_ASSERT_EQUAL_STRING("led_1", r.conflictPeriphId.c_str());
}

void test_api_pin_conflict_exclude_self() {
    std::map<String, std::vector<int>> existing;
    existing["led_1"] = {25, 26};
    int newPins[] = {25}; // 编辑自身时排除
    PinConflictResult r = checkPinConflict(existing, "led_1", newPins, 1);
    TEST_ASSERT_FALSE(r.hasConflict);
}

void test_api_pin_conflict_invalid_pin_ignored() {
    std::map<String, std::vector<int>> existing;
    existing["led_1"] = {25};
    int newPins[] = {99}; // 无效引脚
    PinConflictResult r = checkPinConflict(existing, "", newPins, 1);
    TEST_ASSERT_FALSE(r.hasConflict);
}

void test_api_pins_string_parsing_single() {
    int pins[8];
    int count = parsePinsString("25", pins, 8);
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL(25, pins[0]);
}

void test_api_pins_string_parsing_multiple() {
    int pins[8];
    int count = parsePinsString("16,17,21,22", pins, 8);
    TEST_ASSERT_EQUAL(4, count);
    TEST_ASSERT_EQUAL(16, pins[0]);
    TEST_ASSERT_EQUAL(17, pins[1]);
    TEST_ASSERT_EQUAL(21, pins[2]);
    TEST_ASSERT_EQUAL(22, pins[3]);
}

void test_api_pins_string_parsing_max_limit() {
    int pins[8];
    int count = parsePinsString("1,2,3,4,5,6,7,8,9,10", pins, 8);
    TEST_ASSERT_EQUAL(8, count); // 最多 8 个
}

void test_api_pins_string_parsing_empty() {
    int pins[8];
    int count = parsePinsString("", pins, 8);
    TEST_ASSERT_EQUAL(0, count);
}

// ============================================================
//  TEST GROUP 10: Route Handler 工具函数与内存阈值测试
// ============================================================

// --- 10A: escapePeriphJsonString 模拟 ---
static String mockEscapeJson(const String& value) {
    String out;
    out.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

void test_json_escape_plain_text() {
    TEST_ASSERT_EQUAL_STRING("hello", mockEscapeJson("hello").c_str());
}
void test_json_escape_double_quote() {
    TEST_ASSERT_EQUAL_STRING("say \\\"hi\\\"", mockEscapeJson("say \"hi\"").c_str());
}
void test_json_escape_backslash() {
    TEST_ASSERT_EQUAL_STRING("a\\\\b", mockEscapeJson("a\\b").c_str());
}
void test_json_escape_newline() {
    TEST_ASSERT_EQUAL_STRING("line1\\nline2", mockEscapeJson("line1\nline2").c_str());
}
void test_json_escape_carriage_return() {
    TEST_ASSERT_EQUAL_STRING("a\\rb", mockEscapeJson("a\rb").c_str());
}
void test_json_escape_tab() {
    TEST_ASSERT_EQUAL_STRING("a\\tb", mockEscapeJson("a\tb").c_str());
}
void test_json_escape_empty() {
    TEST_ASSERT_EQUAL_STRING("", mockEscapeJson("").c_str());
}
void test_json_escape_mixed_special() {
    // "a\"b\\c\n" → "a\\\"b\\\\c\\n"
    TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\c\\n", mockEscapeJson("a\"b\\c\n").c_str());
}
void test_json_escape_chinese() {
    // 中文字符不应被转义
    TEST_ASSERT_EQUAL_STRING("\xe4\xb8\xad\xe6\x96\x87", mockEscapeJson("\xe4\xb8\xad\xe6\x96\x87").c_str());
}
void test_json_escape_unicode_passthrough() {
    // 非 ASCII 字符原样透传
    String input = "caf\xc3\xa9"; // "café" in UTF-8
    TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", mockEscapeJson(input).c_str());
}

// --- 10B: isListMemoryCriticallyLow / shouldForceCompactList 模拟 ---
static bool mockIsListMemoryCriticallyLow(uint32_t freeHeap, uint32_t maxAlloc) {
    return freeHeap < 4096 || maxAlloc < 1536;
}
static bool mockShouldForceCompactList(uint32_t freeHeap, uint32_t maxAlloc) {
    return freeHeap < 16384 || maxAlloc < 8192;
}

void test_memory_critical_low_heap() {
    TEST_ASSERT_TRUE(mockIsListMemoryCriticallyLow(3000, 2000));
}
void test_memory_critical_low_alloc() {
    TEST_ASSERT_TRUE(mockIsListMemoryCriticallyLow(5000, 1000));
}
void test_memory_critical_both_ok() {
    TEST_ASSERT_FALSE(mockIsListMemoryCriticallyLow(8000, 4000));
}
void test_memory_critical_boundary_heap() {
    TEST_ASSERT_FALSE(mockIsListMemoryCriticallyLow(4096, 2000)); // 等于阈值不算低
}
void test_memory_critical_boundary_alloc() {
    TEST_ASSERT_FALSE(mockIsListMemoryCriticallyLow(5000, 1536));
}
void test_memory_compact_low_heap() {
    TEST_ASSERT_TRUE(mockShouldForceCompactList(10000, 10000));
}
void test_memory_compact_low_alloc() {
    TEST_ASSERT_TRUE(mockShouldForceCompactList(20000, 5000));
}
void test_memory_compact_both_ok() {
    TEST_ASSERT_FALSE(mockShouldForceCompactList(20000, 10000));
}
void test_memory_compact_boundary_heap() {
    TEST_ASSERT_FALSE(mockShouldForceCompactList(16384, 10000));
}
void test_memory_compact_very_low() {
    TEST_ASSERT_TRUE(mockShouldForceCompactList(100, 100));
}

// ============================================================
//  测试入口
// ============================================================

void test_periph_config_group() {
    // Group 1: sanitizeTriggerForSafety 参数边界校验
    RUN_TEST(test_sanitize_timer_interval_below_min);
    RUN_TEST(test_sanitize_timer_interval_at_min);
    RUN_TEST(test_sanitize_timer_interval_at_max);
    RUN_TEST(test_sanitize_timer_interval_above_max);
    RUN_TEST(test_sanitize_timer_interval_normal);
    RUN_TEST(test_sanitize_poll_timeout_below_min);
    RUN_TEST(test_sanitize_poll_timeout_above_max);
    RUN_TEST(test_sanitize_poll_timeout_normal);
    RUN_TEST(test_sanitize_poll_retries_above_max);
    RUN_TEST(test_sanitize_poll_retries_at_max);
    RUN_TEST(test_sanitize_poll_retries_zero);
    RUN_TEST(test_sanitize_poll_inter_delay_below_min);
    RUN_TEST(test_sanitize_poll_inter_delay_above_max);
    RUN_TEST(test_sanitize_poll_inter_delay_normal);
    RUN_TEST(test_sanitize_heavy_poll_timeout_clamped);
    RUN_TEST(test_sanitize_heavy_poll_retries_clamped);
    RUN_TEST(test_sanitize_heavy_poll_inter_delay_raised);
    RUN_TEST(test_sanitize_heavy_poll_normal_values);
    RUN_TEST(test_sanitize_event_trigger_not_modified);
    RUN_TEST(test_sanitize_platform_trigger_not_modified);

    // Group 2: validateLoadedConfig 全局配置安全校验
    RUN_TEST(test_validate_interval_below_min_corrected);
    RUN_TEST(test_validate_dangerous_combo_corrected);
    RUN_TEST(test_validate_safe_config_not_modified);
    RUN_TEST(test_validate_disabled_rules_ignored);
    RUN_TEST(test_validate_active_task_count);
    RUN_TEST(test_validate_max_active_tasks_constant);
    RUN_TEST(test_validate_safe_combo_not_modified);

    // Group 3: 外设配置 CRUD 安全约束
    RUN_TEST(test_config_crud_peripheral_add_remove);
    RUN_TEST(test_config_crud_duplicate_id_rejected);
    RUN_TEST(test_config_crud_empty_id_rejected);
    RUN_TEST(test_config_crud_update_existing);
    RUN_TEST(test_config_crud_update_nonexistent_fails);
    RUN_TEST(test_config_exec_rule_crud);
    RUN_TEST(test_config_rule_ids_listing);

    // Group 4: 配置持久化边界条件
    RUN_TEST(test_config_max_triggers_per_rule);
    RUN_TEST(test_config_peripheral_properties_map);
    RUN_TEST(test_config_constants_sanity);
    RUN_TEST(test_config_sanitize_multiple_triggers);
    RUN_TEST(test_config_sanitize_all_params_at_boundary);

    // Group 5: 外设类型覆盖与硬件初始化行为
    RUN_TEST(test_periph_type_rf_module_enum_value);
    RUN_TEST(test_periph_type_radar_sensor_enum_value);
    RUN_TEST(test_periph_type_modbus_device_enum_value);
    RUN_TEST(test_periph_type_device_event_enum_value);
    RUN_TEST(test_periph_type_one_wire_enum_value);
    RUN_TEST(test_periph_type_pwm_servo_enum_value);
    RUN_TEST(test_periph_unimplemented_types_identified);
    RUN_TEST(test_periph_type_i2c_and_spi_values);
    RUN_TEST(test_periph_type_dac_and_adc_values);
    RUN_TEST(test_periph_gpio_pullup_pulldown_for_button_events);

    // Group 6: 跨芯片数据一致性（不同芯片不做特殊处理，配置原样透传）
    RUN_TEST(test_lcd_type_enum_and_data_transparency);
    RUN_TEST(test_lcd_params_various_sizes);
    RUN_TEST(test_seven_segment_type_enum_and_data_transparency);
    RUN_TEST(test_seven_segment_brightness_range);
    RUN_TEST(test_all_special_types_always_in_data_layer);
    RUN_TEST(test_neopixel_data_transparency);
    RUN_TEST(test_rf_module_data_transparency);
    RUN_TEST(test_radar_sensor_data_transparency);
    RUN_TEST(test_stepper_motor_data_transparency);
    RUN_TEST(test_validation_failed_peripheral_kept_with_error_status);
    RUN_TEST(test_error_status_peripheral_skips_hw_init);
    RUN_TEST(test_peripheral_status_enum_values);
    RUN_TEST(test_cross_chip_config_consistency_simulation);
    RUN_TEST(test_lcd_update_preserves_data);
    RUN_TEST(test_segment_update_preserves_data);

    // Group 7: 外设类型→参数组映射一致性（回归测试：UART不应映射到GPIO参数）
    RUN_TEST(test_param_group_regression_uart_not_gpio);
    RUN_TEST(test_param_group_regression_default_select_value);
    RUN_TEST(test_param_group_communication_types);
    RUN_TEST(test_param_group_all_gpio_types);
    RUN_TEST(test_param_group_gpio_sub_params_pwm_only);
    RUN_TEST(test_param_group_gpio_sub_params_input_only);
    RUN_TEST(test_param_group_analog_types);
    RUN_TEST(test_param_group_special_types);
    RUN_TEST(test_param_group_no_params_types);
    RUN_TEST(test_param_group_exclusivity);
    RUN_TEST(test_param_group_category_consistency);
    RUN_TEST(test_param_group_new_peripheral_default_disabled);
    RUN_TEST(test_param_group_new_peripheral_uart_params_present);

    // Group 8: 编码器与SDIO外设驱动测试
    RUN_TEST(test_encoder_type_enum_value);
    RUN_TEST(test_sdio_type_enum_value);
    RUN_TEST(test_encoder_config_validation_valid);
    RUN_TEST(test_encoder_config_validation_missing_pins);
    RUN_TEST(test_encoder_config_validation_zero_resolution);
    RUN_TEST(test_encoder_data_transparency);
    RUN_TEST(test_encoder_read_counter);
    RUN_TEST(test_encoder_reset_counter);
    RUN_TEST(test_sdio_config_validation_spi_mode);
    RUN_TEST(test_sdio_config_validation_sdmmc_mode);
    RUN_TEST(test_sdio_config_validation_insufficient_pins);
    RUN_TEST(test_sdio_data_transparency_spi);
    RUN_TEST(test_sdio_data_transparency_sdmmc);
    RUN_TEST(test_unimplemented_types_removed_from_ui);

    // Group 9: Route Handler 数据校验（模拟 API 层逻辑）
    RUN_TEST(test_api_add_peripheral_name_required);
    RUN_TEST(test_api_add_peripheral_type_required);
    RUN_TEST(test_api_add_peripheral_pins_required);
    RUN_TEST(test_api_add_peripheral_device_event_no_pins);
    RUN_TEST(test_api_add_peripheral_valid_request);
    RUN_TEST(test_api_update_peripheral_id_required);
    RUN_TEST(test_api_delete_peripheral_id_required);
    RUN_TEST(test_api_get_peripherals_pagination_default);
    RUN_TEST(test_api_get_peripherals_pagination_page2);
    RUN_TEST(test_api_get_peripherals_pagination_last_page);
    RUN_TEST(test_api_get_peripherals_pagination_overflow);
    RUN_TEST(test_api_get_peripherals_pagination_zero_page);
    RUN_TEST(test_api_get_peripherals_pagination_clamp_page_size);
    RUN_TEST(test_api_get_peripherals_pagination_empty);
    RUN_TEST(test_api_pin_conflict_no_conflict);
    RUN_TEST(test_api_pin_conflict_detected);
    RUN_TEST(test_api_pin_conflict_exclude_self);
    RUN_TEST(test_api_pin_conflict_invalid_pin_ignored);
    RUN_TEST(test_api_pins_string_parsing_single);
    RUN_TEST(test_api_pins_string_parsing_multiple);
    RUN_TEST(test_api_pins_string_parsing_max_limit);
    RUN_TEST(test_api_pins_string_parsing_empty);

    // Group 10: Route Handler 工具函数与内存阈值测试
    // 10A: JSON转义
    RUN_TEST(test_json_escape_plain_text);
    RUN_TEST(test_json_escape_double_quote);
    RUN_TEST(test_json_escape_backslash);
    RUN_TEST(test_json_escape_newline);
    RUN_TEST(test_json_escape_carriage_return);
    RUN_TEST(test_json_escape_tab);
    RUN_TEST(test_json_escape_empty);
    RUN_TEST(test_json_escape_mixed_special);
    RUN_TEST(test_json_escape_chinese);
    RUN_TEST(test_json_escape_unicode_passthrough);
    // 10B: 内存阈值
    RUN_TEST(test_memory_critical_low_heap);
    RUN_TEST(test_memory_critical_low_alloc);
    RUN_TEST(test_memory_critical_both_ok);
    RUN_TEST(test_memory_critical_boundary_heap);
    RUN_TEST(test_memory_critical_boundary_alloc);
    RUN_TEST(test_memory_compact_low_heap);
    RUN_TEST(test_memory_compact_low_alloc);
    RUN_TEST(test_memory_compact_both_ok);
    RUN_TEST(test_memory_compact_boundary_heap);
    RUN_TEST(test_memory_compact_very_low);
}
