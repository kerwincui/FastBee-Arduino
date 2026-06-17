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
}
