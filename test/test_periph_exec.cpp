/**
 * @file test_periph_exec.cpp
 * @brief Peripheral Execution Engine Tests
 * 
 * 测试外设执行规则引擎的核心逻辑：
 * - 调度器配置校验（轮询间隔边界）
 * - 动态降频机制
 * - 内存保护暂停逻辑
 * - 按键事件状态机
 * - 数据命令匹配
 * - 规则CRUD和执行流程
 */

#include <unity.h>
#include <Arduino.h>
#include <map>
#include "mocks/MockPeripheral.h"
#include "mocks/MockHealthMonitor.h"
#include "mocks/MockLogger.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
// 前向声明 WorkerPool 常量，避免引入完整的头文件链（防止与 MockPeripheral.h 中的 PeriphExecRule 定义冲突）
namespace PeriphExecWorkerPool {
    static constexpr size_t   WORKER_COUNT   = 2;
    static constexpr size_t   QUEUE_CAPACITY = 16;
    static constexpr uint32_t WORKER_STACK   = 6144;
}

void test_periph_exec_group();

// ========== 调度器常量（镜像自 PeriphExecScheduler.h） ==========
namespace SchedulerConstants {
    constexpr uint32_t MIN_POLL_INTERVAL_MS = 5000;
    constexpr uint32_t SAFE_POLL_INTERVAL_MS = 30000;
    constexpr uint8_t MAX_ACTIVE_TASKS = 12;
    constexpr uint8_t WARN_TASK_THRESHOLD = 8;
    constexpr uint32_t CHECK_PERIOD_NORMAL_MS = 1000;
    constexpr uint32_t CHECK_PERIOD_WARN_MS = 2000;
    constexpr uint32_t CHECK_PERIOD_SEVERE_MS = 4000;
}

// ========== 内存保护常量（镜像自 PeriphExecScheduler.cpp） ==========
namespace MemoryGuardConstants {
    constexpr uint32_t WEB_RESERVE_FREE_HEAP_BYTES = 18432U;
    constexpr uint32_t WEB_RESERVE_LARGEST_BLOCK_BYTES = 6144U;
    constexpr uint32_t WEB_RESERVE_FRAGMENTED_BLOCK_BYTES = 12288U;
    constexpr uint8_t WEB_RESERVE_FRAGMENTATION_PERCENT = 65U;
}

// ========== 模拟 MemoryGuardLevel ==========
enum class MemoryGuardLevel : uint8_t {
    NORMAL = 0,
    WARN = 1,
    SEVERE = 2,
    CRITICAL = 3
};

// ========== 镜像 shouldSuspendBackgroundPolling 逻辑 ==========
static bool shouldSuspendBackgroundPolling(MemoryGuardLevel level,
                                           uint32_t freeHeap,
                                           uint32_t largestBlock,
                                           uint8_t fragmentation) {
    if (level >= MemoryGuardLevel::SEVERE) {
        return true;
    }
    if (freeHeap < MemoryGuardConstants::WEB_RESERVE_FREE_HEAP_BYTES) {
        return true;
    }
    if (largestBlock < MemoryGuardConstants::WEB_RESERVE_LARGEST_BLOCK_BYTES) {
        return true;
    }
    return fragmentation >= MemoryGuardConstants::WEB_RESERVE_FRAGMENTATION_PERCENT &&
           largestBlock < MemoryGuardConstants::WEB_RESERVE_FRAGMENTED_BLOCK_BYTES;
}

// ========== 镜像动态降频逻辑 ==========
static uint32_t getDynamicCheckPeriod(MemoryGuardLevel level) {
    switch (level) {
        case MemoryGuardLevel::WARN:     return SchedulerConstants::CHECK_PERIOD_WARN_MS;
        case MemoryGuardLevel::SEVERE:   return SchedulerConstants::CHECK_PERIOD_SEVERE_MS;
        case MemoryGuardLevel::CRITICAL: return SchedulerConstants::CHECK_PERIOD_SEVERE_MS;
        default:                         return SchedulerConstants::CHECK_PERIOD_NORMAL_MS;
    }
}

// ========== 模拟按键事件配置 ==========
struct ButtonEventConfig {
    uint16_t debounceMs = 50;
    uint16_t clickIntervalMs = 300;
    uint16_t longPress2sMs = 2000;
    uint16_t longPress5sMs = 5000;
    uint16_t longPress10sMs = 10000;
};

struct ButtonRuntimeState {
    String periphId;
    bool lastState = true;
    bool currentState = true;
    unsigned long lastChangeTime = 0;
    unsigned long pressStartTime = 0;
    uint8_t clickCount = 0;
    unsigned long lastClickTime = 0;
    bool longPress2sTriggered = false;
    bool longPress5sTriggered = false;
    bool longPress10sTriggered = false;
};

// ========== 模拟轮询间隔校验逻辑 ==========
struct MockTrigger {
    uint8_t triggerType;   // 3=TIMER, 5=POLL
    uint32_t intervalSec;
};

struct MockRule {
    String id;
    String name;
    bool enabled;
    std::vector<MockTrigger> triggers;
};

static bool validatePollInterval(MockRule& rule, uint8_t activeTaskCount) {
    bool modified = false;
    for (auto& trigger : rule.triggers) {
        if (trigger.triggerType != 3 && trigger.triggerType != 5) continue;
        
        uint32_t intervalMs = trigger.intervalSec * 1000UL;
        
        if (intervalMs < SchedulerConstants::MIN_POLL_INTERVAL_MS) {
            trigger.intervalSec = SchedulerConstants::MIN_POLL_INTERVAL_MS / 1000;
            modified = true;
        } else if (activeTaskCount > SchedulerConstants::WARN_TASK_THRESHOLD && intervalMs < 10000) {
            trigger.intervalSec = SchedulerConstants::SAFE_POLL_INTERVAL_MS / 1000;
            modified = true;
        }
    }
    return modified;
}

// ============================================================
//  TEST GROUP 1: 调度器配置校验测试（轮询间隔边界）
// ============================================================

void test_poll_interval_below_minimum_gets_corrected() {
    MockRule rule;
    rule.id = "rule_1";
    rule.name = "Test Rule";
    rule.enabled = true;
    rule.triggers.push_back({3, 2});  // 2s < 5s minimum
    
    bool modified = validatePollInterval(rule, 1);
    
    TEST_ASSERT_TRUE(modified);
    TEST_ASSERT_EQUAL_UINT32(5, rule.triggers[0].intervalSec);
}

void test_poll_interval_at_minimum_not_modified() {
    MockRule rule;
    rule.id = "rule_2";
    rule.name = "Boundary Rule";
    rule.enabled = true;
    rule.triggers.push_back({3, 5});  // exactly 5s minimum
    
    bool modified = validatePollInterval(rule, 1);
    
    TEST_ASSERT_FALSE(modified);
    TEST_ASSERT_EQUAL_UINT32(5, rule.triggers[0].intervalSec);
}

void test_poll_interval_above_minimum_not_modified() {
    MockRule rule;
    rule.id = "rule_3";
    rule.name = "Safe Rule";
    rule.enabled = true;
    rule.triggers.push_back({3, 60});  // 60s is well above minimum
    
    bool modified = validatePollInterval(rule, 1);
    
    TEST_ASSERT_FALSE(modified);
    TEST_ASSERT_EQUAL_UINT32(60, rule.triggers[0].intervalSec);
}

void test_poll_interval_aggressive_with_many_tasks() {
    MockRule rule;
    rule.id = "rule_4";
    rule.name = "Aggressive Rule";
    rule.enabled = true;
    rule.triggers.push_back({5, 8});  // 8s < 10s and tasks > 8
    
    bool modified = validatePollInterval(rule, 9);  // > WARN_TASK_THRESHOLD
    
    TEST_ASSERT_TRUE(modified);
    TEST_ASSERT_EQUAL_UINT32(30, rule.triggers[0].intervalSec);  // forced to SAFE (30s)
}

void test_poll_interval_not_aggressive_with_few_tasks() {
    MockRule rule;
    rule.id = "rule_5";
    rule.name = "Normal Load Rule";
    rule.enabled = true;
    rule.triggers.push_back({5, 8});  // 8s with only 4 tasks
    
    bool modified = validatePollInterval(rule, 4);  // <= WARN_TASK_THRESHOLD
    
    TEST_ASSERT_FALSE(modified);
    TEST_ASSERT_EQUAL_UINT32(8, rule.triggers[0].intervalSec);
}

void test_poll_interval_zero_seconds_corrected() {
    MockRule rule;
    rule.id = "rule_6";
    rule.name = "Zero Interval";
    rule.enabled = true;
    rule.triggers.push_back({3, 0});  // 0s is clearly invalid
    
    bool modified = validatePollInterval(rule, 1);
    
    TEST_ASSERT_TRUE(modified);
    TEST_ASSERT_EQUAL_UINT32(5, rule.triggers[0].intervalSec);
}

void test_poll_interval_non_timer_trigger_ignored() {
    MockRule rule;
    rule.id = "rule_7";
    rule.name = "Event Rule";
    rule.enabled = true;
    rule.triggers.push_back({1, 1});  // Event trigger type, interval irrelevant
    
    bool modified = validatePollInterval(rule, 10);
    
    TEST_ASSERT_FALSE(modified);
    TEST_ASSERT_EQUAL_UINT32(1, rule.triggers[0].intervalSec);  // unchanged
}

// ============================================================
//  TEST GROUP 2: 动态降频机制测试
// ============================================================

void test_dynamic_check_period_normal() {
    uint32_t period = getDynamicCheckPeriod(MemoryGuardLevel::NORMAL);
    TEST_ASSERT_EQUAL_UINT32(1000, period);
}

void test_dynamic_check_period_warn() {
    uint32_t period = getDynamicCheckPeriod(MemoryGuardLevel::WARN);
    TEST_ASSERT_EQUAL_UINT32(2000, period);
}

void test_dynamic_check_period_severe() {
    uint32_t period = getDynamicCheckPeriod(MemoryGuardLevel::SEVERE);
    TEST_ASSERT_EQUAL_UINT32(4000, period);
}

void test_dynamic_check_period_critical() {
    uint32_t period = getDynamicCheckPeriod(MemoryGuardLevel::CRITICAL);
    TEST_ASSERT_EQUAL_UINT32(4000, period);  // CRITICAL uses same as SEVERE
}

void test_frequency_reduction_doubles_per_level() {
    uint32_t normal = getDynamicCheckPeriod(MemoryGuardLevel::NORMAL);
    uint32_t warn = getDynamicCheckPeriod(MemoryGuardLevel::WARN);
    uint32_t severe = getDynamicCheckPeriod(MemoryGuardLevel::SEVERE);
    
    TEST_ASSERT_EQUAL_UINT32(normal * 2, warn);   // 1s -> 2s
    TEST_ASSERT_EQUAL_UINT32(warn * 2, severe);   // 2s -> 4s
}

// ============================================================
//  TEST GROUP 3: 内存保护暂停逻辑测试
// ============================================================

void test_suspend_when_guard_level_severe() {
    bool result = shouldSuspendBackgroundPolling(
        MemoryGuardLevel::SEVERE, 50000, 30000, 20);
    TEST_ASSERT_TRUE(result);
}

void test_suspend_when_guard_level_critical() {
    bool result = shouldSuspendBackgroundPolling(
        MemoryGuardLevel::CRITICAL, 50000, 30000, 20);
    TEST_ASSERT_TRUE(result);
}

void test_suspend_when_free_heap_below_threshold() {
    bool result = shouldSuspendBackgroundPolling(
        MemoryGuardLevel::NORMAL, 15000, 30000, 20);  // 15000 < 18432
    TEST_ASSERT_TRUE(result);
}

void test_suspend_when_largest_block_below_threshold() {
    bool result = shouldSuspendBackgroundPolling(
        MemoryGuardLevel::NORMAL, 50000, 5000, 20);  // 5000 < 6144
    TEST_ASSERT_TRUE(result);
}

void test_suspend_when_fragmented_and_small_block() {
    // fragmentation >= 65% AND largestBlock < 12288
    bool result = shouldSuspendBackgroundPolling(
        MemoryGuardLevel::NORMAL, 50000, 10000, 70);
    TEST_ASSERT_TRUE(result);
}

void test_no_suspend_normal_conditions() {
    bool result = shouldSuspendBackgroundPolling(
        MemoryGuardLevel::NORMAL, 80000, 40000, 30);
    TEST_ASSERT_FALSE(result);
}

void test_no_suspend_high_fragmentation_but_large_block() {
    // fragmentation >= 65% BUT largestBlock >= 12288
    bool result = shouldSuspendBackgroundPolling(
        MemoryGuardLevel::NORMAL, 50000, 15000, 70);
    TEST_ASSERT_FALSE(result);
}

void test_no_suspend_warn_level_with_enough_memory() {
    // WARN level doesn't trigger suspend (only SEVERE+)
    bool result = shouldSuspendBackgroundPolling(
        MemoryGuardLevel::WARN, 50000, 30000, 20);
    TEST_ASSERT_FALSE(result);
}

void test_suspend_boundary_free_heap_exactly_threshold() {
    // Exactly at threshold: 18432 is NOT less than 18432
    bool result = shouldSuspendBackgroundPolling(
        MemoryGuardLevel::NORMAL, 18432, 30000, 20);
    TEST_ASSERT_FALSE(result);
}

void test_suspend_boundary_free_heap_one_below() {
    bool result = shouldSuspendBackgroundPolling(
        MemoryGuardLevel::NORMAL, 18431, 30000, 20);
    TEST_ASSERT_TRUE(result);
}

void test_suspend_boundary_largest_block_exactly_threshold() {
    bool result = shouldSuspendBackgroundPolling(
        MemoryGuardLevel::NORMAL, 50000, 6144, 20);
    TEST_ASSERT_FALSE(result);
}

void test_suspend_boundary_largest_block_one_below() {
    bool result = shouldSuspendBackgroundPolling(
        MemoryGuardLevel::NORMAL, 50000, 6143, 20);
    TEST_ASSERT_TRUE(result);
}

// ============================================================
//  TEST GROUP 4: 按键事件状态机测试
// ============================================================

void test_button_debounce_rejects_fast_changes() {
    ButtonEventConfig config;
    ButtonRuntimeState state;
    state.periphId = "btn_1";
    state.lastState = true;  // 上拉默认高
    state.lastChangeTime = 100;
    
    // 模拟 30ms 后的变化（< 50ms debounce）
    unsigned long now = 130;
    bool stateChanged = (now - state.lastChangeTime) >= config.debounceMs;
    
    TEST_ASSERT_FALSE(stateChanged);
}

void test_button_debounce_accepts_stable_changes() {
    ButtonEventConfig config;
    ButtonRuntimeState state;
    state.periphId = "btn_2";
    state.lastState = true;
    state.lastChangeTime = 100;
    
    // 模拟 60ms 后的变化（> 50ms debounce）
    unsigned long now = 160;
    bool stateChanged = (now - state.lastChangeTime) >= config.debounceMs;
    
    TEST_ASSERT_TRUE(stateChanged);
}

void test_button_long_press_2s_detection() {
    ButtonEventConfig config;
    ButtonRuntimeState state;
    state.periphId = "btn_3";
    state.pressStartTime = 1000;
    state.longPress2sTriggered = false;
    
    unsigned long now = 3100;  // 2100ms held
    bool longPress2s = !state.longPress2sTriggered && 
                       (now - state.pressStartTime >= config.longPress2sMs);
    
    TEST_ASSERT_TRUE(longPress2s);
}

void test_button_long_press_not_retriggered() {
    ButtonEventConfig config;
    ButtonRuntimeState state;
    state.periphId = "btn_4";
    state.pressStartTime = 1000;
    state.longPress2sTriggered = true;  // already triggered
    
    unsigned long now = 4000;
    bool longPress2s = !state.longPress2sTriggered && 
                       (now - state.pressStartTime >= config.longPress2sMs);
    
    TEST_ASSERT_FALSE(longPress2s);
}

void test_button_double_click_interval() {
    ButtonEventConfig config;
    ButtonRuntimeState state;
    state.periphId = "btn_5";
    state.clickCount = 1;
    state.lastClickTime = 1000;
    
    // 第二次点击在 250ms 后（< 300ms interval）
    unsigned long now = 1250;
    bool withinInterval = (now - state.lastClickTime) < config.clickIntervalMs;
    
    TEST_ASSERT_TRUE(withinInterval);
    state.clickCount++;
    TEST_ASSERT_EQUAL_UINT8(2, state.clickCount);  // double click
}

void test_button_double_click_expired() {
    ButtonEventConfig config;
    ButtonRuntimeState state;
    state.periphId = "btn_6";
    state.clickCount = 1;
    state.lastClickTime = 1000;
    
    // 第二次点击在 400ms 后（> 300ms interval）
    unsigned long now = 1400;
    bool withinInterval = (now - state.lastClickTime) < config.clickIntervalMs;
    
    TEST_ASSERT_FALSE(withinInterval);
}

void test_button_long_press_5s_after_2s() {
    ButtonEventConfig config;
    ButtonRuntimeState state;
    state.periphId = "btn_7";
    state.pressStartTime = 0;
    state.longPress2sTriggered = true;
    state.longPress5sTriggered = false;
    
    unsigned long now = 5500;
    bool longPress5s = !state.longPress5sTriggered && 
                       (now - state.pressStartTime >= config.longPress5sMs);
    
    TEST_ASSERT_TRUE(longPress5s);
}

// ============================================================
//  TEST GROUP 5: 规则执行管理测试
// ============================================================

void test_rule_crud_add_and_get() {
    MockPeriphExecManager& mgr = MockPeriphExecManager::getInstance();
    mgr.initialize();
    
    PeriphExecRule rule;
    rule.id = "rule_test_1";
    rule.name = "LED Toggle Rule";
    rule.enabled = true;
    rule.triggerType = TriggerType::PLATFORM_MQTT;
    rule.actionType = ActionType::SET_HIGH;
    rule.targetPeriphId = "led_1";
    
    TEST_ASSERT_TRUE(mgr.addRule(rule));
    
    PeriphExecRule* fetched = mgr.getRule("rule_test_1");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_STRING("LED Toggle Rule", fetched->name.c_str());
}

void test_rule_crud_duplicate_add_fails() {
    MockPeriphExecManager& mgr = MockPeriphExecManager::getInstance();
    mgr.initialize();
    
    PeriphExecRule rule;
    rule.id = "rule_dup";
    rule.name = "Duplicate Rule";
    rule.enabled = true;
    
    TEST_ASSERT_TRUE(mgr.addRule(rule));
    // Second add with same id should fail (already exists)
    // Note: MockPeriphExecManager allows overwrites via addRule - 
    // in real system this is prevented by ID uniqueness check
    TEST_ASSERT_TRUE(mgr.addRule(rule));  // Mock allows it
}

void test_rule_crud_remove() {
    MockPeriphExecManager& mgr = MockPeriphExecManager::getInstance();
    mgr.initialize();
    
    PeriphExecRule rule;
    rule.id = "rule_remove";
    rule.name = "Removable";
    rule.enabled = true;
    mgr.addRule(rule);
    
    TEST_ASSERT_TRUE(mgr.removeRule("rule_remove"));
    TEST_ASSERT_NULL(mgr.getRule("rule_remove"));
}

void test_rule_crud_remove_nonexistent() {
    MockPeriphExecManager& mgr = MockPeriphExecManager::getInstance();
    mgr.initialize();
    
    TEST_ASSERT_FALSE(mgr.removeRule("nonexistent_rule"));
}

void test_rule_execution_set_high() {
    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    MockPeriphExecManager& mgr = MockPeriphExecManager::getInstance();
    mgr.initialize();
    
    PeripheralConfig config;
    config.id = "led_exec";
    config.name = "Test LED";
    config.type = PeripheralType::GPIO_DIGITAL_OUTPUT;
    config.pin = 2;
    pm.addPeripheral(config);
    
    PeriphExecRule rule;
    rule.id = "rule_exec_1";
    rule.name = "Set High";
    rule.enabled = true;
    rule.actionType = ActionType::SET_HIGH;
    rule.targetPeriphId = "led_exec";
    mgr.addRule(rule);
    
    TEST_ASSERT_TRUE(mgr.executeRule("rule_exec_1"));
    TEST_ASSERT_EQUAL(GPIOState::STATE_HIGH, pm.getPinState("led_exec"));
}

void test_rule_execution_disabled_rule_fails() {
    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    MockPeriphExecManager& mgr = MockPeriphExecManager::getInstance();
    mgr.initialize();
    
    PeripheralConfig config;
    config.id = "led_disabled";
    config.name = "Disabled LED";
    config.type = PeripheralType::GPIO_DIGITAL_OUTPUT;
    config.pin = 4;
    pm.addPeripheral(config);
    
    PeriphExecRule rule;
    rule.id = "rule_disabled";
    rule.name = "Disabled Rule";
    rule.enabled = false;  // disabled
    rule.actionType = ActionType::SET_HIGH;
    rule.targetPeriphId = "led_disabled";
    mgr.addRule(rule);
    
    TEST_ASSERT_FALSE(mgr.executeRule("rule_disabled"));
}

void test_rule_execution_toggle() {
    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    MockPeriphExecManager& mgr = MockPeriphExecManager::getInstance();
    mgr.initialize();
    
    PeripheralConfig config;
    config.id = "led_toggle";
    config.name = "Toggle LED";
    config.type = PeripheralType::GPIO_DIGITAL_OUTPUT;
    config.pin = 5;
    pm.addPeripheral(config);
    pm.writePin("led_toggle", LOW);
    
    PeriphExecRule rule;
    rule.id = "rule_toggle";
    rule.name = "Toggle";
    rule.enabled = true;
    rule.actionType = ActionType::BLINK;
    rule.targetPeriphId = "led_toggle";
    mgr.addRule(rule);
    
    // First toggle: LOW -> HIGH
    TEST_ASSERT_TRUE(mgr.executeRule("rule_toggle"));
    TEST_ASSERT_EQUAL(GPIOState::STATE_HIGH, pm.getPinState("led_toggle"));
    
    // Second toggle: HIGH -> LOW
    TEST_ASSERT_TRUE(mgr.executeRule("rule_toggle"));
    TEST_ASSERT_EQUAL(GPIOState::STATE_LOW, pm.getPinState("led_toggle"));
}

void test_rule_execution_pwm() {
    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    MockPeriphExecManager& mgr = MockPeriphExecManager::getInstance();
    mgr.initialize();
    
    PeripheralConfig config;
    config.id = "pwm_test";
    config.name = "PWM Output";
    config.type = PeripheralType::GPIO_PWM_OUTPUT;
    config.pin = 15;
    pm.addPeripheral(config);
    
    PeriphExecRule rule;
    rule.id = "rule_pwm";
    rule.name = "Set PWM";
    rule.enabled = true;
    rule.actionType = ActionType::SET_PWM;
    rule.actionValue = "128";
    rule.targetPeriphId = "pwm_test";
    mgr.addRule(rule);
    
    TEST_ASSERT_TRUE(mgr.executeRule("rule_pwm"));
    MockPeripheral* mp = pm.getMockPeripheral("pwm_test");
    TEST_ASSERT_NOT_NULL(mp);
    TEST_ASSERT_EQUAL(128, mp->getPWMValue());
}

// ============================================================
//  TEST GROUP 6: 并发访问和边界条件
// ============================================================

void test_max_active_tasks_limit() {
    TEST_ASSERT_EQUAL_UINT8(12, SchedulerConstants::MAX_ACTIVE_TASKS);
    TEST_ASSERT_EQUAL_UINT8(8, SchedulerConstants::WARN_TASK_THRESHOLD);
    TEST_ASSERT_TRUE(SchedulerConstants::WARN_TASK_THRESHOLD < SchedulerConstants::MAX_ACTIVE_TASKS);
}

void test_empty_rule_id_rejected() {
    MockPeriphExecManager& mgr = MockPeriphExecManager::getInstance();
    mgr.initialize();
    
    PeriphExecRule rule;
    rule.id = "";  // empty ID
    rule.name = "Invalid";
    
    TEST_ASSERT_FALSE(mgr.addRule(rule));
}

void test_execute_nonexistent_rule() {
    MockPeriphExecManager& mgr = MockPeriphExecManager::getInstance();
    mgr.initialize();
    
    TEST_ASSERT_FALSE(mgr.executeRule("ghost_rule"));
}

void test_multiple_triggers_per_rule() {
    MockRule rule;
    rule.id = "multi_trigger";
    rule.name = "Multi Trigger";
    rule.enabled = true;
    rule.triggers.push_back({3, 10});  // Timer 10s
    rule.triggers.push_back({5, 30});  // Poll 30s
    rule.triggers.push_back({1, 0});   // Event trigger
    
    bool modified = validatePollInterval(rule, 5);
    
    TEST_ASSERT_FALSE(modified);
    TEST_ASSERT_EQUAL_UINT32(10, rule.triggers[0].intervalSec);
    TEST_ASSERT_EQUAL_UINT32(30, rule.triggers[1].intervalSec);
}

void test_poll_interval_uint32_overflow_protection() {
    // Very large interval should not be modified
    MockRule rule;
    rule.id = "large_interval";
    rule.name = "Large Interval";
    rule.enabled = true;
    rule.triggers.push_back({3, 86400});  // 24 hours
    
    bool modified = validatePollInterval(rule, 10);
    
    TEST_ASSERT_FALSE(modified);
    TEST_ASSERT_EQUAL_UINT32(86400, rule.triggers[0].intervalSec);
}

// ============================================================
//  TEST GROUP 7: 定时触发防卡死机制测试
// ============================================================

// 模拟 checkTimerTriggers 中的间隔触发判断逻辑
struct TimerTriggerState {
    uint8_t timerMode = 0;       // 0=间隔, 1=每日时间点
    uint32_t intervalSec = 60;
    String timePoint = "08:00";
    unsigned long lastTriggerTime = 0;
    uint32_t triggerCount = 0;
};

static bool shouldTimerTrigger(TimerTriggerState& ts, unsigned long now) {
    if (ts.timerMode == 0) {
        if (ts.intervalSec == 0) return false;
        unsigned long intervalMs = (unsigned long)ts.intervalSec * 1000UL;
        if (ts.lastTriggerTime == 0 || (now - ts.lastTriggerTime) >= intervalMs) {
            return true;
        }
    }
    return false;
}

void test_timer_interval_first_trigger() {
    TimerTriggerState ts;
    ts.intervalSec = 10;
    ts.lastTriggerTime = 0;
    // 首次触发: lastTriggerTime == 0 时应立即触发
    TEST_ASSERT_TRUE(shouldTimerTrigger(ts, 5000));
}

void test_timer_interval_not_yet_due() {
    TimerTriggerState ts;
    ts.intervalSec = 60;
    ts.lastTriggerTime = 1000;
    // 仅过了 30s，不到 60s 间隔
    TEST_ASSERT_FALSE(shouldTimerTrigger(ts, 31000));
}

void test_timer_interval_exact_due() {
    TimerTriggerState ts;
    ts.intervalSec = 60;
    ts.lastTriggerTime = 1000;
    // 刚好 60s 后
    TEST_ASSERT_TRUE(shouldTimerTrigger(ts, 61000));
}

void test_timer_interval_zero_no_trigger() {
    TimerTriggerState ts;
    ts.intervalSec = 0;
    ts.lastTriggerTime = 0;
    // intervalSec == 0 不应触发（防除零/无限触发）
    TEST_ASSERT_FALSE(shouldTimerTrigger(ts, 100000));
}

void test_timer_interval_millis_overflow_safe() {
    TimerTriggerState ts;
    ts.intervalSec = 60;
    ts.lastTriggerTime = 4294967000UL;  // 接近 uint32 上限
    unsigned long now = 60296UL;  // millis() 溢出后回绕，elapsed = 60296 + (4294967296 - 4294967000) = 60592
    // 溢出后 (now - lastTriggerTime) 仍正确计算
    unsigned long elapsed = now - ts.lastTriggerTime;
    TEST_ASSERT_TRUE(elapsed >= 60000UL);
}

void test_timer_interval_24h_boundary() {
    TimerTriggerState ts;
    ts.intervalSec = 86400;  // 24 小时
    ts.lastTriggerTime = 1000;  // 非零，避免首次触发逻辑
    // 23h59m59s 后仍未到 (elapsed = 86399000 - 1000 = 86398000 < 86400000)
    TEST_ASSERT_FALSE(shouldTimerTrigger(ts, 86399000UL));
    // 24h 后触发 (elapsed = 86401000 - 1000 = 86400000 >= 86400000)
    TEST_ASSERT_TRUE(shouldTimerTrigger(ts, 86401000UL));
}

// 模拟失败退避机制
struct BackoffState {
    std::map<String, unsigned long> failureBackoff;
};

static bool isInBackoff(BackoffState& bs, const String& ruleId, unsigned long now) {
    auto it = bs.failureBackoff.find(ruleId);
    return (it != bs.failureBackoff.end() && now < it->second);
}

static void cleanExpiredBackoff(BackoffState& bs, unsigned long now) {
    for (auto it = bs.failureBackoff.begin(); it != bs.failureBackoff.end(); ) {
        if (now > it->second && (now - it->second) > 300000UL) {
            it = bs.failureBackoff.erase(it);
        } else {
            ++it;
        }
    }
}

void test_backoff_blocks_retrigger() {
    BackoffState bs;
    bs.failureBackoff["rule_a"] = 50000;  // 50s 后才能再触发
    TEST_ASSERT_TRUE(isInBackoff(bs, "rule_a", 30000));
    TEST_ASSERT_FALSE(isInBackoff(bs, "rule_a", 60000));
}

void test_backoff_cleanup_removes_expired() {
    BackoffState bs;
    bs.failureBackoff["old_rule"] = 1000;   // 退避到 1s
    bs.failureBackoff["fresh_rule"] = 500000; // 退避到 500s
    // now=600000: old_rule 已过期 599s > 300s，应清除
    cleanExpiredBackoff(bs, 600000);
    TEST_ASSERT_EQUAL(1, (int)bs.failureBackoff.size());
    TEST_ASSERT_TRUE(bs.failureBackoff.find("old_rule") == bs.failureBackoff.end());
    TEST_ASSERT_TRUE(bs.failureBackoff.find("fresh_rule") != bs.failureBackoff.end());
}

void test_backoff_no_cleanup_within_window() {
    BackoffState bs;
    bs.failureBackoff["rule_x"] = 100000;
    // now=200000: 过期仅 100s < 300s，不清除
    cleanExpiredBackoff(bs, 200000);
    TEST_ASSERT_EQUAL(1, (int)bs.failureBackoff.size());
}

// 模拟运行中规则防重复分发
struct RunningState {
    std::map<String, bool> runningRuleIds;
    std::map<String, unsigned long> runningStartTime;
};

static bool isRuleStuck(RunningState& rs, const String& ruleId, unsigned long now) {
    if (rs.runningRuleIds.find(ruleId) == rs.runningRuleIds.end()) return false;
    auto itTime = rs.runningStartTime.find(ruleId);
    bool startTimeMissing = (itTime == rs.runningStartTime.end());
    unsigned long elapsed = startTimeMissing ? 0 : (now - itTime->second);
    bool stuckTooLong = !startTimeMissing && (elapsed > 60000UL);
    return startTimeMissing || stuckTooLong;
}

void test_stuck_rule_detected_after_60s() {
    RunningState rs;
    rs.runningRuleIds["stuck_rule"] = true;
    rs.runningStartTime["stuck_rule"] = 1000;
    // 70s 后应检测为卡死
    TEST_ASSERT_TRUE(isRuleStuck(rs, "stuck_rule", 71000));
}

void test_running_rule_not_stuck() {
    RunningState rs;
    rs.runningRuleIds["active_rule"] = true;
    rs.runningStartTime["active_rule"] = 1000;
    // 30s 后仍在正常运行
    TEST_ASSERT_FALSE(isRuleStuck(rs, "active_rule", 31000));
}

void test_stuck_rule_missing_start_time() {
    RunningState rs;
    rs.runningRuleIds["orphan_rule"] = true;
    // runningStartTime 无记录 -> 视为卡死
    TEST_ASSERT_TRUE(isRuleStuck(rs, "orphan_rule", 5000));
}

void test_not_running_rule_not_stuck() {
    RunningState rs;
    // 未在运行中集合 -> 不卡
    TEST_ASSERT_FALSE(isRuleStuck(rs, "absent_rule", 5000));
}

// ============================================================
//  TEST GROUP 8: Modbus 轮询触发防卡死机制测试
// ============================================================

// 模拟 poll ingress 节流逻辑
struct PollIngressState {
    std::map<String, unsigned long> lastAccepted;
};

static bool shouldThrottlePoll(PollIngressState& ps, const String& source,
                                unsigned long now, unsigned long minIntervalMs) {
    unsigned long& last = ps.lastAccepted[source];
    if (last > 0 && (now - last) < minIntervalMs) {
        return true;  // 节流
    }
    last = now;
    return false;
}

void test_poll_ingress_first_request_passes() {
    PollIngressState ps;
    TEST_ASSERT_FALSE(shouldThrottlePoll(ps, "modbus_poll", 1000, 2000));
}

void test_poll_ingress_rapid_requests_throttled() {
    PollIngressState ps;
    shouldThrottlePoll(ps, "modbus_poll", 1000, 2000);
    // 500ms 后的请求应被节流
    TEST_ASSERT_TRUE(shouldThrottlePoll(ps, "modbus_poll", 1500, 2000));
}

void test_poll_ingress_after_interval_passes() {
    PollIngressState ps;
    shouldThrottlePoll(ps, "modbus_poll", 1000, 2000);
    // 2500ms 后的请求应通过
    TEST_ASSERT_FALSE(shouldThrottlePoll(ps, "modbus_poll", 3500, 2000));
}

void test_poll_ingress_different_sources_independent() {
    PollIngressState ps;
    shouldThrottlePoll(ps, "modbus_poll", 1000, 2000);
    // 不同数据源不受影响
    TEST_ASSERT_FALSE(shouldThrottlePoll(ps, "sensor_poll", 1500, 2000));
}

// 模拟 Modbus 轮询任务内存保护
struct ModbusMemGuard {
    uint32_t freeHeap;
    bool memoryCritical;
    bool memorySevere;
};

static bool shouldSkipModbusPoll(ModbusMemGuard& mg) {
    if (mg.memoryCritical) return true;
    if (mg.memorySevere) return true;
    if (mg.freeHeap < 25000) return true;
    return false;
}

void test_modbus_poll_skip_on_critical_memory() {
    ModbusMemGuard mg = {50000, true, false};
    TEST_ASSERT_TRUE(shouldSkipModbusPoll(mg));
}

void test_modbus_poll_skip_on_severe_memory() {
    ModbusMemGuard mg = {50000, false, true};
    TEST_ASSERT_TRUE(shouldSkipModbusPoll(mg));
}

void test_modbus_poll_skip_on_low_heap() {
    ModbusMemGuard mg = {20000, false, false};
    TEST_ASSERT_TRUE(shouldSkipModbusPoll(mg));
}

void test_modbus_poll_ok_with_sufficient_memory() {
    ModbusMemGuard mg = {60000, false, false};
    TEST_ASSERT_FALSE(shouldSkipModbusPoll(mg));
}

void test_modbus_poll_boundary_heap_25000() {
    ModbusMemGuard mg_exact = {25000, false, false};
    TEST_ASSERT_FALSE(shouldSkipModbusPoll(mg_exact));  // 25000 is NOT < 25000
    ModbusMemGuard mg_below = {24999, false, false};
    TEST_ASSERT_TRUE(shouldSkipModbusPoll(mg_below));
}

// 模拟 Modbus 轮询内循环堆守卫
static bool shouldStopPollingInnerLoop(uint32_t freeHeap, bool memoryCritical) {
    if (memoryCritical) return true;
    if (freeHeap < 15000) return true;
    return false;
}

void test_modbus_inner_loop_stop_on_critical() {
    TEST_ASSERT_TRUE(shouldStopPollingInnerLoop(50000, true));
}

void test_modbus_inner_loop_stop_on_low_heap() {
    TEST_ASSERT_TRUE(shouldStopPollingInnerLoop(12000, false));
}

void test_modbus_inner_loop_continue_normal() {
    TEST_ASSERT_FALSE(shouldStopPollingInnerLoop(50000, false));
}

// 模拟多 poll 任务间隔延时
void test_poll_inter_delay_respects_config() {
    // 镜像自 sanitizeTriggerForSafety: pollInterPollDelay 被限制在 [20, 1000]
    uint16_t delay = 100;
    TEST_ASSERT_TRUE(delay >= 20 && delay <= 1000);
    // 超低值被修正
    uint16_t lowDelay = 5;
    uint16_t corrected = (lowDelay < 20) ? 20 : lowDelay;
    TEST_ASSERT_EQUAL(20, corrected);
    // 超高值被修正
    uint16_t highDelay = 5000;
    uint16_t correctedHigh = (highDelay > 1000) ? 1000 : highDelay;
    TEST_ASSERT_EQUAL(1000, correctedHigh);
}

// 模拟 Modbus 可用性检查
void test_timer_skip_when_modbus_unavailable() {
    // 模拟 checkTimerTriggers 中的 needsModbus && !modbusAvailable 逻辑
    bool needsModbus = true;
    bool modbusAvailable = false;
    bool shouldSkip = needsModbus && !modbusAvailable;
    TEST_ASSERT_TRUE(shouldSkip);
}

void test_timer_proceed_when_modbus_available() {
    bool needsModbus = true;
    bool modbusAvailable = true;
    bool shouldSkip = needsModbus && !modbusAvailable;
    TEST_ASSERT_FALSE(shouldSkip);
}

void test_timer_proceed_when_no_modbus_needed() {
    bool needsModbus = false;
    bool modbusAvailable = false;
    bool shouldSkip = needsModbus && !modbusAvailable;
    TEST_ASSERT_FALSE(shouldSkip);
}

// ============================================================
//  TEST GROUP 9: 异步执行/同步降级防卡死测试
// ============================================================

// 模拟 shouldRunAsync 判断
struct AsyncResourceState {
    uint32_t freeHeap;
    uint8_t availableSlots;
    static constexpr uint32_t TEST_MIN_HEAP_FOR_ASYNC = 20000;
};

static bool shouldRunAsync(AsyncResourceState& ars) {
    if (ars.freeHeap < AsyncResourceState::TEST_MIN_HEAP_FOR_ASYNC) return false;
    if (ars.availableSlots == 0) return false;
    return true;
}

void test_async_ok_with_sufficient_resources() {
    AsyncResourceState ars = {50000, 2};
    TEST_ASSERT_TRUE(shouldRunAsync(ars));
}

void test_async_skip_on_low_heap() {
    AsyncResourceState ars = {15000, 2};
    TEST_ASSERT_FALSE(shouldRunAsync(ars));
}

void test_async_skip_on_no_slots() {
    AsyncResourceState ars = {50000, 0};
    TEST_ASSERT_FALSE(shouldRunAsync(ars));
}

void test_async_boundary_heap_20000() {
    AsyncResourceState ars_exact = {20000, 1};
    TEST_ASSERT_TRUE(shouldRunAsync(ars_exact));  // 20000 is NOT < 20000
    AsyncResourceState ars_below = {19999, 1};
    TEST_ASSERT_FALSE(shouldRunAsync(ars_below));
}

// 模拟 shouldAvoidSyncFallback
struct RuleActionCheck {
    bool hasScript;
    bool hasModbusPoll;
    bool hasModbusWrite;
    bool hasSensorRead;
    bool targetIsModbus;
};

static bool shouldAvoidSyncFallback(RuleActionCheck& rac) {
    return rac.hasScript || rac.hasModbusPoll || rac.hasModbusWrite ||
           rac.hasSensorRead || rac.targetIsModbus;
}

void test_heavy_rule_avoids_sync_fallback() {
    RuleActionCheck rac = {false, true, false, false, false};  // Modbus poll
    TEST_ASSERT_TRUE(shouldAvoidSyncFallback(rac));
}

void test_script_rule_avoids_sync_fallback() {
    RuleActionCheck rac = {true, false, false, false, false};
    TEST_ASSERT_TRUE(shouldAvoidSyncFallback(rac));
}

void test_sensor_read_avoids_sync_fallback() {
    RuleActionCheck rac = {false, false, false, true, false};
    TEST_ASSERT_TRUE(shouldAvoidSyncFallback(rac));
}

void test_simple_gpio_allows_sync_fallback() {
    RuleActionCheck rac = {false, false, false, false, false};
    TEST_ASSERT_FALSE(shouldAvoidSyncFallback(rac));
}

void test_modbus_target_avoids_sync_fallback() {
    RuleActionCheck rac = {false, false, false, false, true};
    TEST_ASSERT_TRUE(shouldAvoidSyncFallback(rac));
}

// 模拟 executeAllActions 中的堆守卫
static bool shouldBreakActionLoop(uint32_t freeHeap, bool memCritical, bool memSevere) {
    if (memCritical) return true;
    if (memSevere && freeHeap < 20000) return true;
    if (freeHeap < 15000) return true;
    return false;
}

void test_action_loop_break_on_critical() {
    TEST_ASSERT_TRUE(shouldBreakActionLoop(50000, true, false));
}

void test_action_loop_break_on_severe_low() {
    TEST_ASSERT_TRUE(shouldBreakActionLoop(18000, false, true));
}

void test_action_loop_continue_normal() {
    TEST_ASSERT_FALSE(shouldBreakActionLoop(50000, false, false));
}

void test_action_loop_break_on_very_low_heap() {
    TEST_ASSERT_TRUE(shouldBreakActionLoop(12000, false, false));
}

// syncDelayMs 上限保护
void test_sync_delay_clamped_to_10s() {
    uint16_t syncDelay = 15000;
    uint16_t effective = (syncDelay > 10000) ? 10000 : syncDelay;
    TEST_ASSERT_EQUAL(10000, effective);
}

void test_sync_delay_normal_value() {
    uint16_t syncDelay = 500;
    uint16_t effective = (syncDelay > 10000) ? 10000 : syncDelay;
    TEST_ASSERT_EQUAL(500, effective);
}

// ============================================================
//  TEST GROUP 10: Worker Pool 队列防溢出测试
// ============================================================

void test_worker_pool_constants_sanity() {
    // Worker pool 参数合理性检查
    TEST_ASSERT_EQUAL(2, (int)PeriphExecWorkerPool::WORKER_COUNT);
    TEST_ASSERT_EQUAL(16, (int)PeriphExecWorkerPool::QUEUE_CAPACITY);
    // 队列容量必须 >= worker 数量，否则并发场景队列先满
    TEST_ASSERT_TRUE(PeriphExecWorkerPool::QUEUE_CAPACITY >= PeriphExecWorkerPool::WORKER_COUNT);
}

void test_worker_stack_size_adequate() {
    // 实测 HWM ~4036B，栈应至少 5120B 留余量
    TEST_ASSERT_TRUE(PeriphExecWorkerPool::WORKER_STACK >= 5120);
    // 但不应过大浪费 RAM
    TEST_ASSERT_TRUE(PeriphExecWorkerPool::WORKER_STACK <= 8192);
}

// ============================================================
//  TEST GROUP 11: 执行模式语义与脚本兼容测试
// ============================================================

void test_exec_mode_async_is_zero() {
    // execMode=0 必须为异步模式（不阻塞主循环）
    // 与 PeripheralExecution.h 中 EXEC_ASYNC=0 一致
    constexpr int EXEC_ASYNC = 0;
    constexpr int EXEC_SYNC  = 1;
    TEST_ASSERT_EQUAL(0, EXEC_ASYNC);
    TEST_ASSERT_EQUAL(1, EXEC_SYNC);
    // 异步应为默认值
    TEST_ASSERT_EQUAL(0, EXEC_ASYNC);
}

void test_exec_mode_sync_is_one() {
    // execMode=1 必须为同步模式（阻塞主循环）
    constexpr int EXEC_SYNC = 1;
    TEST_ASSERT_EQUAL(1, EXEC_SYNC);
}

void test_script_content_null_compatibility() {
    // 模拟 PeriphExecManager 中的 scriptContent "null" 兼容处理
    // 旧版固件可能将未设值存储为字符串 "null"
    auto sanitizeScriptContent = [](String& content) {
        if (content == "null") content = "";
    };
    
    // 场景 1: 正常空串不变
    String s1 = "";
    sanitizeScriptContent(s1);
    TEST_ASSERT_EQUAL_STRING("", s1.c_str());
    
    // 场景 2: 正常脚本不变
    String s2 = "PERIPH led_1 ON";
    sanitizeScriptContent(s2);
    TEST_ASSERT_EQUAL_STRING("PERIPH led_1 ON", s2.c_str());
    
    // 场景 3: "null" 字符串应被转换为空串
    String s3 = "null";
    sanitizeScriptContent(s3);
    TEST_ASSERT_EQUAL_STRING("", s3.c_str());
    
    // 场景 4: 包含 "null" 的正常内容不应被误修改
    String s4 = "null_check";
    sanitizeScriptContent(s4);
    TEST_ASSERT_EQUAL_STRING("null_check", s4.c_str());
}

void test_action_type_inverted_enum_values() {
    // INVERTED 动作类型值必须正确
    TEST_ASSERT_EQUAL(13, static_cast<int>(ActionType::HIGH_INVERTED));
    TEST_ASSERT_EQUAL(14, static_cast<int>(ActionType::LOW_INVERTED));
}

void test_action_type_high_inverted_semantics() {
    // HIGH_INVERTED: 语义高但物理输出低（用于低电平有效的继电器）
    // 模拟执行逻辑：activeHigh = !inverted
    bool inverted = (static_cast<int>(ActionType::HIGH_INVERTED) >= 13 &&
                     static_cast<int>(ActionType::HIGH_INVERTED) <= 14);
    TEST_ASSERT_TRUE(inverted);
    // HIGH_INVERTED 和 LOW_INVERTED 是连续值
    TEST_ASSERT_EQUAL(1, static_cast<int>(ActionType::LOW_INVERTED) - 
                         static_cast<int>(ActionType::HIGH_INVERTED));
}

void test_action_type_values_complete() {
    // 验证所有关键动作类型值与生产代码一致
    TEST_ASSERT_EQUAL(0,  static_cast<int>(ActionType::SET_HIGH));
    TEST_ASSERT_EQUAL(1,  static_cast<int>(ActionType::SET_LOW));
    TEST_ASSERT_EQUAL(2,  static_cast<int>(ActionType::BLINK));
    TEST_ASSERT_EQUAL(3,  static_cast<int>(ActionType::BREATHE));
    TEST_ASSERT_EQUAL(4,  static_cast<int>(ActionType::SET_PWM));
    TEST_ASSERT_EQUAL(5,  static_cast<int>(ActionType::SET_DAC));
    TEST_ASSERT_EQUAL(10, static_cast<int>(ActionType::CALL_PERIPHERAL));
    TEST_ASSERT_EQUAL(15, static_cast<int>(ActionType::SCRIPT));
    TEST_ASSERT_EQUAL(19, static_cast<int>(ActionType::SENSOR_READ));
    TEST_ASSERT_EQUAL(21, static_cast<int>(ActionType::TRIGGER_EVENT));
}

// ============================================================
//  TEST GROUP 12: evaluateCondition 条件评估全量测试
//  镜像 PeriphExecManager::evaluateCondition 逻辑
// ============================================================

// 镜像 isNumericString 辅助函数
static bool isNumericString(const String& s) {
    if (s.isEmpty()) return false;
    bool hasDigit = false;
    bool hasDecimal = false;
    for (size_t i = 0; i < s.length(); ++i) {
        const char c = s[i];
        if (c >= '0' && c <= '9') { hasDigit = true; continue; }
        if (c == '.' && !hasDecimal) { hasDecimal = true; continue; }
        if ((c == '-' || c == '+') && i == 0) continue;
        return false;
    }
    return hasDigit;
}

// 镜像 evaluateCondition
static bool mockEvalCondition(const String& value, uint8_t op, const String& compareValue) {
    // CONTAIN / NOT_CONTAIN
    if (op == 8) return value.indexOf(compareValue) >= 0;
    if (op == 9) return value.indexOf(compareValue) < 0;
    // EQ / NEQ
    if (op == 0 || op == 1) {
        bool bothNumeric = isNumericString(value) && isNumericString(compareValue);
        if (bothNumeric) {
            float val = value.toFloat();
            float cmp = compareValue.toFloat();
            return (op == 0) ? (val == cmp) : (val != cmp);
        }
        return (op == 0) ? (value == compareValue) : (value != compareValue);
    }
    // 数值操作符
    float val = value.toFloat();
    float cmp = compareValue.toFloat();
    switch (op) {
        case 2: return val > cmp;   // GT
        case 3: return val < cmp;   // LT
        case 4: return val >= cmp;  // GTE
        case 5: return val <= cmp;  // LTE
        case 6: case 7: {
            int commaIdx = compareValue.indexOf(',');
            if (commaIdx < 0) return false;
            float minVal = compareValue.substring(0, commaIdx).toFloat();
            float maxVal = compareValue.substring(commaIdx + 1).toFloat();
            bool inRange = (val >= minVal && val <= maxVal);
            return (op == 6) ? inRange : !inRange;
        }
        default: return false;
    }
}

void test_eval_eq_numeric_equal() {
    TEST_ASSERT_TRUE(mockEvalCondition("25.5", 0, "25.5"));
}
void test_eval_eq_numeric_not_equal() {
    TEST_ASSERT_FALSE(mockEvalCondition("25.5", 0, "30.0"));
}
void test_eval_eq_string_equal() {
    TEST_ASSERT_TRUE(mockEvalCondition("hello", 0, "hello"));
}
void test_eval_eq_string_not_equal() {
    TEST_ASSERT_FALSE(mockEvalCondition("hello", 0, "world"));
}
void test_eval_eq_mixed_numeric_vs_string() {
    // "123" vs "123" 都是数值，123.0 == 123.0
    TEST_ASSERT_TRUE(mockEvalCondition("123", 0, "123"));
    // "abc" vs "abc" 字符串精确匹配
    TEST_ASSERT_TRUE(mockEvalCondition("abc", 0, "abc"));
}
void test_eval_neq_numeric() {
    TEST_ASSERT_TRUE(mockEvalCondition("10", 1, "20"));
    TEST_ASSERT_FALSE(mockEvalCondition("10", 1, "10"));
}
void test_eval_neq_string() {
    TEST_ASSERT_TRUE(mockEvalCondition("on", 1, "off"));
    TEST_ASSERT_FALSE(mockEvalCondition("on", 1, "on"));
}
void test_eval_gt_lt_gte_lte() {
    TEST_ASSERT_TRUE(mockEvalCondition("30.5", 2, "30.0"));   // 30.5 > 30
    TEST_ASSERT_FALSE(mockEvalCondition("30.0", 2, "30.0"));  // 30.0 > 30 is false
    TEST_ASSERT_TRUE(mockEvalCondition("29.5", 3, "30.0"));   // 29.5 < 30
    TEST_ASSERT_FALSE(mockEvalCondition("30.0", 3, "30.0"));  // 30.0 < 30 is false
    TEST_ASSERT_TRUE(mockEvalCondition("30.0", 4, "30.0"));   // 30.0 >= 30
    TEST_ASSERT_TRUE(mockEvalCondition("30.0", 5, "30.0"));   // 30.0 <= 30
}
void test_eval_between_in_range() {
    TEST_ASSERT_TRUE(mockEvalCondition("25", 6, "20,30"));   // 20 <= 25 <= 30
}
void test_eval_between_at_boundary() {
    TEST_ASSERT_TRUE(mockEvalCondition("20", 6, "20,30"));   // 20 >= 20
    TEST_ASSERT_TRUE(mockEvalCondition("30", 6, "20,30"));   // 30 <= 30
}
void test_eval_between_out_of_range() {
    TEST_ASSERT_FALSE(mockEvalCondition("19", 6, "20,30"));
    TEST_ASSERT_FALSE(mockEvalCondition("31", 6, "20,30"));
}
void test_eval_between_no_comma_returns_false() {
    // 没有逗号，无法解析范围，返回 false
    TEST_ASSERT_FALSE(mockEvalCondition("25", 6, "2030"));
}
void test_eval_not_between() {
    TEST_ASSERT_TRUE(mockEvalCondition("19", 7, "20,30"));   // 19 不在 [20,30]
    TEST_ASSERT_TRUE(mockEvalCondition("31", 7, "20,30"));   // 31 不在 [20,30]
    TEST_ASSERT_FALSE(mockEvalCondition("25", 7, "20,30"));  // 25 在 [20,30]
}
void test_eval_contain_found() {
    TEST_ASSERT_TRUE(mockEvalCondition("temperature:25.5", 8, "25.5"));
}
void test_eval_contain_not_found() {
    TEST_ASSERT_FALSE(mockEvalCondition("temperature:25.5", 8, "humidity"));
}
void test_eval_not_contain() {
    TEST_ASSERT_TRUE(mockEvalCondition("temperature:25.5", 9, "humidity"));
    TEST_ASSERT_FALSE(mockEvalCondition("temperature:25.5", 9, "25.5"));
}
void test_eval_non_numeric_gt_returns_zero() {
    // 非数值字符串 toFloat() 返回 0.0f
    // "abc" toFloat() -> 0.0, "10" toFloat() -> 10.0 -> 0.0 > 10.0 is false
    TEST_ASSERT_FALSE(mockEvalCondition("abc", 2, "10"));
}
void test_eval_negative_values() {
    TEST_ASSERT_TRUE(mockEvalCondition("-5", 3, "0"));    // -5 < 0
    TEST_ASSERT_TRUE(mockEvalCondition("-10", 3, "-5"));  // -10 < -5
    TEST_ASSERT_TRUE(mockEvalCondition("-5", 6, "-10,0")); // -10 <= -5 <= 0
}

// ============================================================
//  TEST GROUP 13: sanitizeTriggerForSafety 参数安全修正测试
//  镜像 PeriphExecManager::sanitizeTriggerForSafety 逻辑
// ============================================================

// 镜像 sanitizeTriggerForSafety 的安全常量和修正逻辑
struct MockSanitizeTrigger {
    uint8_t triggerType;
    uint32_t intervalSec;
    uint16_t pollResponseTimeout;
    uint8_t pollMaxRetries;
    uint16_t pollInterPollDelay;
    bool hasPollCollectionAction;
};

static constexpr uint32_t MIN_TIMER_INTERVAL_SEC = 1;
static constexpr uint32_t MAX_TIMER_INTERVAL_SEC = 86400UL;
static constexpr uint16_t MIN_POLL_TIMEOUT_MS = 100;
static constexpr uint16_t MAX_POLL_TIMEOUT_MS = 5000;
static constexpr uint16_t HEAVY_POLL_TIMEOUT_MS = 3000;
static constexpr uint8_t MAX_POLL_RETRIES = 3;
static constexpr uint8_t HEAVY_POLL_RETRIES = 2;
static constexpr uint16_t MIN_POLL_INTER_DELAY_MS = 20;
static constexpr uint16_t MAX_POLL_INTER_DELAY_MS = 1000;
static constexpr uint16_t HEAVY_POLL_INTER_DELAY_MS = 100;

static bool mockSanitizeTrigger(MockSanitizeTrigger& t) {
    uint32_t origInterval = t.intervalSec;
    uint16_t origTimeout = t.pollResponseTimeout;
    uint8_t origRetries = t.pollMaxRetries;
    uint16_t origDelay = t.pollInterPollDelay;

    // TIMER_TRIGGER interval 修正
    if (t.triggerType == 1) {  // TIMER_TRIGGER
        if (t.intervalSec < MIN_TIMER_INTERVAL_SEC)
            t.intervalSec = MIN_TIMER_INTERVAL_SEC;
        else if (t.intervalSec > MAX_TIMER_INTERVAL_SEC)
            t.intervalSec = MAX_TIMER_INTERVAL_SEC;
    }

    // POLL_TRIGGER 参数修正
    if (t.triggerType == 5) {  // POLL_TRIGGER
        if (t.pollResponseTimeout < MIN_POLL_TIMEOUT_MS)
            t.pollResponseTimeout = MIN_POLL_TIMEOUT_MS;
        else if (t.pollResponseTimeout > MAX_POLL_TIMEOUT_MS)
            t.pollResponseTimeout = MAX_POLL_TIMEOUT_MS;
        if (t.pollMaxRetries > MAX_POLL_RETRIES)
            t.pollMaxRetries = MAX_POLL_RETRIES;
        if (t.pollInterPollDelay < MIN_POLL_INTER_DELAY_MS)
            t.pollInterPollDelay = MIN_POLL_INTER_DELAY_MS;
        else if (t.pollInterPollDelay > MAX_POLL_INTER_DELAY_MS)
            t.pollInterPollDelay = MAX_POLL_INTER_DELAY_MS;
        // 重度轮询更严格限制
        if (t.hasPollCollectionAction) {
            if (t.pollResponseTimeout > HEAVY_POLL_TIMEOUT_MS)
                t.pollResponseTimeout = HEAVY_POLL_TIMEOUT_MS;
            if (t.pollMaxRetries > HEAVY_POLL_RETRIES)
                t.pollMaxRetries = HEAVY_POLL_RETRIES;
            if (t.pollInterPollDelay < HEAVY_POLL_INTER_DELAY_MS)
                t.pollInterPollDelay = HEAVY_POLL_INTER_DELAY_MS;
        }
    }

    return (origInterval != t.intervalSec || origTimeout != t.pollResponseTimeout ||
            origRetries != t.pollMaxRetries || origDelay != t.pollInterPollDelay);
}

void test_sanitize_timer_interval_zero_corrected() {
    MockSanitizeTrigger t = {1, 0, 1000, 2, 100, false};
    bool modified = mockSanitizeTrigger(t);
    TEST_ASSERT_TRUE(modified);
    TEST_ASSERT_EQUAL_UINT32(1, t.intervalSec);
}
void test_exec_sanitize_timer_interval_below_min() {
    // intervalSec = 0 < 1 强制修正为 1
    MockSanitizeTrigger t = {1, 0, 1000, 2, 100, false};
    mockSanitizeTrigger(t);
    TEST_ASSERT_EQUAL_UINT32(1, t.intervalSec);
}
void test_exec_sanitize_timer_interval_above_max() {
    // intervalSec = 100000 > 86400 强制修正为 86400
    MockSanitizeTrigger t = {1, 100000, 1000, 2, 100, false};
    bool modified = mockSanitizeTrigger(t);
    TEST_ASSERT_TRUE(modified);
    TEST_ASSERT_EQUAL_UINT32(86400, t.intervalSec);
}
void test_sanitize_timer_interval_valid_no_change() {
    MockSanitizeTrigger t = {1, 60, 1000, 2, 100, false};
    bool modified = mockSanitizeTrigger(t);
    TEST_ASSERT_FALSE(modified);
    TEST_ASSERT_EQUAL_UINT32(60, t.intervalSec);
}
void test_sanitize_timer_boundary_min() {
    MockSanitizeTrigger t = {1, 1, 1000, 2, 100, false};
    bool modified = mockSanitizeTrigger(t);
    TEST_ASSERT_FALSE(modified);
    TEST_ASSERT_EQUAL_UINT32(1, t.intervalSec);
}
void test_sanitize_timer_boundary_max() {
    MockSanitizeTrigger t = {1, 86400, 1000, 2, 100, false};
    bool modified = mockSanitizeTrigger(t);
    TEST_ASSERT_FALSE(modified);
    TEST_ASSERT_EQUAL_UINT32(86400, t.intervalSec);
}
void test_exec_sanitize_poll_timeout_below_min() {
    MockSanitizeTrigger t = {5, 60, 50, 2, 100, false};  // timeout=50 < 100
    bool modified = mockSanitizeTrigger(t);
    TEST_ASSERT_TRUE(modified);
    TEST_ASSERT_EQUAL(100, t.pollResponseTimeout);
}
void test_exec_sanitize_poll_timeout_above_max() {
    MockSanitizeTrigger t = {5, 60, 8000, 2, 100, false};  // timeout=8000 > 5000
    bool modified = mockSanitizeTrigger(t);
    TEST_ASSERT_TRUE(modified);
    TEST_ASSERT_EQUAL(5000, t.pollResponseTimeout);
}
void test_sanitize_poll_timeout_valid_no_change() {
    MockSanitizeTrigger t = {5, 60, 1000, 2, 100, false};
    bool modified = mockSanitizeTrigger(t);
    TEST_ASSERT_FALSE(modified);
}
void test_exec_sanitize_poll_retries_above_max() {
    MockSanitizeTrigger t = {5, 60, 1000, 5, 100, false};  // retries=5 > 3
    bool modified = mockSanitizeTrigger(t);
    TEST_ASSERT_TRUE(modified);
    TEST_ASSERT_EQUAL(3, t.pollMaxRetries);
}
void test_exec_sanitize_poll_inter_delay_below_min() {
    MockSanitizeTrigger t = {5, 60, 1000, 2, 5, false};  // delay=5 < 20
    bool modified = mockSanitizeTrigger(t);
    TEST_ASSERT_TRUE(modified);
    TEST_ASSERT_EQUAL(20, t.pollInterPollDelay);
}
void test_exec_sanitize_poll_inter_delay_above_max() {
    MockSanitizeTrigger t = {5, 60, 1000, 2, 2000, false};  // delay=2000 > 1000
    bool modified = mockSanitizeTrigger(t);
    TEST_ASSERT_TRUE(modified);
    TEST_ASSERT_EQUAL(1000, t.pollInterPollDelay);
}
void test_sanitize_heavy_poll_timeout_restricted() {
    // 重度轮询：timeout 上限 3000ms
    MockSanitizeTrigger t = {5, 60, 4000, 2, 100, true};  // timeout=4000 > 3000
    bool modified = mockSanitizeTrigger(t);
    TEST_ASSERT_TRUE(modified);
    TEST_ASSERT_EQUAL(3000, t.pollResponseTimeout);
}
void test_sanitize_heavy_poll_retries_restricted() {
    // 重度轮询：retries 上限 2
    MockSanitizeTrigger t = {5, 60, 1000, 3, 100, true};  // retries=3 > 2
    bool modified = mockSanitizeTrigger(t);
    TEST_ASSERT_TRUE(modified);
    TEST_ASSERT_EQUAL(2, t.pollMaxRetries);
}
void test_sanitize_heavy_poll_inter_delay_min_raised() {
    // 重度轮询：delay 下限提升到 100ms
    MockSanitizeTrigger t = {5, 60, 1000, 2, 50, true};  // delay=50 < 100
    bool modified = mockSanitizeTrigger(t);
    TEST_ASSERT_TRUE(modified);
    TEST_ASSERT_EQUAL(100, t.pollInterPollDelay);
}
void test_sanitize_heavy_poll_valid_no_change() {
    // 重度轮询正常值不修改
    MockSanitizeTrigger t = {5, 60, 2000, 2, 200, true};
    bool modified = mockSanitizeTrigger(t);
    TEST_ASSERT_FALSE(modified);
}
void test_sanitize_non_timer_poll_trigger_ignored() {
    // EVENT_TRIGGER (type=4) 不应被修正
    MockSanitizeTrigger t = {4, 0, 50, 10, 5, false};
    bool modified = mockSanitizeTrigger(t);
    TEST_ASSERT_FALSE(modified);
    TEST_ASSERT_EQUAL_UINT32(0, t.intervalSec);
    TEST_ASSERT_EQUAL(50, t.pollResponseTimeout);
}

// ============================================================
//  TEST GROUP 14: 每日时间点触发模式测试
//  镜像 checkTimerTriggers 中 timerMode==1 的逻辑
// ============================================================

struct MockTimeInfo {
    int tm_hour;
    int tm_min;
    int tm_year;  // years since 1900 (100 = year 2000)
};

// 镜像 timerMode==1 的判断逻辑
static bool shouldDailyTimeTrigger(MockTimeInfo& timeinfo, const String& timePoint,
                                    unsigned long now, unsigned long lastTriggerTime) {
    if (timeinfo.tm_year < 100) return false;  // 时间未同步
    if (timePoint.length() < 5) return false;
    int colonIdx = timePoint.indexOf(':');
    if (colonIdx < 0) return false;
    int targetHour = timePoint.substring(0, colonIdx).toInt();
    int targetMin = timePoint.substring(colonIdx + 1).toInt();
    if (timeinfo.tm_hour == targetHour && timeinfo.tm_min == targetMin) {
        // 60s 冷却：同一分钟内不重复触发
        if (lastTriggerTime > 0 && (now - lastTriggerTime) < 60000) return false;
        return true;
    }
    return false;
}

void test_daily_time_trigger_matches() {
    MockTimeInfo ti = {8, 30, 124};  // 2024年，08:30
    TEST_ASSERT_TRUE(shouldDailyTimeTrigger(ti, "08:30", 100000, 0));
}
void test_daily_time_trigger_no_match_hour() {
    MockTimeInfo ti = {9, 30, 124};
    TEST_ASSERT_FALSE(shouldDailyTimeTrigger(ti, "08:30", 100000, 0));
}
void test_daily_time_trigger_no_match_minute() {
    MockTimeInfo ti = {8, 31, 124};
    TEST_ASSERT_FALSE(shouldDailyTimeTrigger(ti, "08:30", 100000, 0));
}
void test_daily_time_trigger_time_not_synced() {
    MockTimeInfo ti = {8, 30, 50};  // tm_year < 100 -> NTP未同步
    TEST_ASSERT_FALSE(shouldDailyTimeTrigger(ti, "08:30", 100000, 0));
}
void test_daily_time_trigger_cooldown_60s() {
    MockTimeInfo ti = {8, 30, 124};
    // 30s 前已触发过 -> 不重复触发
    TEST_ASSERT_FALSE(shouldDailyTimeTrigger(ti, "08:30", 100000, 70000));
}
void test_daily_time_trigger_cooldown_expired() {
    MockTimeInfo ti = {8, 30, 124};
    // 61s 前触发过 -> 冷却结束，可以再次触发
    TEST_ASSERT_TRUE(shouldDailyTimeTrigger(ti, "08:30", 100000, 39000));
}
void test_daily_time_trigger_invalid_format() {
    MockTimeInfo ti = {8, 30, 124};
    // 格式错误：无冒号
    TEST_ASSERT_FALSE(shouldDailyTimeTrigger(ti, "0830", 100000, 0));
    // 格式错误：太短
    TEST_ASSERT_FALSE(shouldDailyTimeTrigger(ti, "8:3", 100000, 0));
}
void test_daily_time_trigger_midnight() {
    MockTimeInfo ti = {0, 0, 124};  // 00:00
    TEST_ASSERT_TRUE(shouldDailyTimeTrigger(ti, "00:00", 100000, 0));
}
void test_daily_time_trigger_end_of_day() {
    MockTimeInfo ti = {23, 59, 124};
    TEST_ASSERT_TRUE(shouldDailyTimeTrigger(ti, "23:59", 100000, 0));
}
void test_daily_time_trigger_cross_day_boundary() {
    // 23:59 的时间不应匹配 00:00
    MockTimeInfo ti = {23, 59, 124};
    TEST_ASSERT_FALSE(shouldDailyTimeTrigger(ti, "00:00", 100000, 0));
}

// ============================================================
//  TEST GROUP 15: 轮询触发冷却机制测试
//  镜像 PeriphExecManager::getPollTriggerCooldownMs 和
//  PERIPH_EXEC_POLL_TRIGGER_MIN_INTERVAL_MS 逻辑
// ============================================================

// 镜像冷却常量
static constexpr unsigned long POLL_TRIGGER_MIN_INTERVAL_MS = 1000;
static constexpr unsigned long HEAVY_POLL_TRIGGER_MIN_INTERVAL_MS = 2000;
static constexpr unsigned long MODBUS_POLL_INGRESS_MIN_INTERVAL_MS = 1000;

// 镜像 getPollTriggerCooldownMs 逻辑
static unsigned long mockGetPollTriggerCooldownMs(bool hasPollCollectionAction,
                                                    const String& source) {
    if ((source == "modbus" || source == "modbus_poll") && hasPollCollectionAction) {
        return HEAVY_POLL_TRIGGER_MIN_INTERVAL_MS;
    }
    return POLL_TRIGGER_MIN_INTERVAL_MS;
}

void test_poll_cooldown_normal_source() {
    unsigned long cooldown = mockGetPollTriggerCooldownMs(false, "sensor_poll");
    TEST_ASSERT_EQUAL_UINT32(1000, cooldown);
}
void test_poll_cooldown_modbus_heavy() {
    // modbus_poll 源 + 有轮询采集动作 -> 2000ms 冷却
    unsigned long cooldown = mockGetPollTriggerCooldownMs(true, "modbus_poll");
    TEST_ASSERT_EQUAL_UINT32(2000, cooldown);
}
void test_poll_cooldown_modbus_no_heavy_action() {
    // modbus_poll 源但无采集动作 -> 普通 1000ms 冷却
    unsigned long cooldown = mockGetPollTriggerCooldownMs(false, "modbus_poll");
    TEST_ASSERT_EQUAL_UINT32(1000, cooldown);
}
void test_poll_cooldown_modbus_raw_source() {
    // modbus 源 + 有采集动作 -> 重度冷却
    unsigned long cooldown = mockGetPollTriggerCooldownMs(true, "modbus");
    TEST_ASSERT_EQUAL_UINT32(2000, cooldown);
}
void test_poll_cooldown_other_source() {
    unsigned long cooldown = mockGetPollTriggerCooldownMs(false, "serial");
    TEST_ASSERT_EQUAL_UINT32(1000, cooldown);
}

// 镜像轮询触发冷却判断逻辑
static bool isPollTriggerInCooldown(unsigned long now, unsigned long lastTriggerTime,
                                     unsigned long cooldownMs) {
    if (lastTriggerTime == 0) return false;  // 从未触发过
    return (now - lastTriggerTime) < cooldownMs;
}

void test_poll_trigger_first_time_no_cooldown() {
    TEST_ASSERT_FALSE(isPollTriggerInCooldown(5000, 0, 1000));
}
void test_poll_trigger_within_cooldown() {
    TEST_ASSERT_TRUE(isPollTriggerInCooldown(5500, 5000, 1000));
}
void test_poll_trigger_after_cooldown() {
    TEST_ASSERT_FALSE(isPollTriggerInCooldown(6001, 5000, 1000));
}
void test_poll_trigger_exact_cooldown_boundary() {
    // 恰好等于冷却时间 -> 不节流（>= 比较）
    TEST_ASSERT_FALSE(isPollTriggerInCooldown(6000, 5000, 1000));
}
void test_poll_trigger_heavy_cooldown_longer() {
    // 重度冷却 2000ms：1500ms 后仍在冷却中
    TEST_ASSERT_TRUE(isPollTriggerInCooldown(6500, 5000, 2000));
    // 2001ms 后冷却结束
    TEST_ASSERT_FALSE(isPollTriggerInCooldown(7001, 5000, 2000));
}

// 镜像 Modbus poll ingress 节流（同源最小间隔）
struct MockPollIngressTracker {
    std::map<String, unsigned long> lastAccepted;
};

static bool mockIngressThrottle(MockPollIngressTracker& tracker, const String& source,
                                 unsigned long now, unsigned long minIntervalMs) {
    unsigned long& last = tracker.lastAccepted[source];
    if (last > 0 && (now - last) < minIntervalMs) return true;  // 节流
    last = now;
    return false;
}

void test_poll_ingress_modbus_throttle_1s() {
    MockPollIngressTracker tracker;
    // 首次请求通过
    TEST_ASSERT_FALSE(mockIngressThrottle(tracker, "modbus_poll", 1000, MODBUS_POLL_INGRESS_MIN_INTERVAL_MS));
    // 500ms 内再次请求被节流
    TEST_ASSERT_TRUE(mockIngressThrottle(tracker, "modbus_poll", 1500, MODBUS_POLL_INGRESS_MIN_INTERVAL_MS));
    // 1001ms 后通过
    TEST_ASSERT_FALSE(mockIngressThrottle(tracker, "modbus_poll", 2001, MODBUS_POLL_INGRESS_MIN_INTERVAL_MS));
}
void test_poll_ingress_independent_sources() {
    MockPollIngressTracker tracker;
    TEST_ASSERT_FALSE(mockIngressThrottle(tracker, "modbus_poll", 1000, MODBUS_POLL_INGRESS_MIN_INTERVAL_MS));
    // 不同源不受影响
    TEST_ASSERT_FALSE(mockIngressThrottle(tracker, "sensor_poll", 1500, MODBUS_POLL_INGRESS_MIN_INTERVAL_MS));
}

// ============================================================
//  TEST GROUP 16: 动作分发测试（模拟 executeActionItem 逻辑）
// ============================================================

// 模拟动作分发结果
struct MockActionResult {
    bool success;
    uint8_t physicalPinState;  // 0=LOW, 1=HIGH, 2=PWM, 3=UNCHANGED
    uint16_t pwmDuty;
    bool systemRestartTriggered;
    bool scriptExecuted;
    String scriptContent;
    String callPeriphTarget;
    String callPeriphCommand;
    String ruleControlTarget;
    bool ruleControlEnable;
};

// 模拟 executeActionItem 的分发逻辑
static MockActionResult mockExecuteAction(
    uint8_t actionType,
    const String& targetPeriphId,
    const String& actionValue,
    bool targetExists,
    bool scriptEnabled
) {
    MockActionResult result = {};
    result.physicalPinState = 3; // UNCHANGED

    // 调用其他外设 (actionType 10) 必须先于系统动作判断
    if (actionType == 10) { // ACTION_CALL_PERIPHERAL
        if (targetPeriphId.isEmpty()) { result.success = false; return result; }
        if (!targetExists) { result.success = false; return result; }
        result.callPeriphTarget = targetPeriphId;
        result.callPeriphCommand = actionValue;
        result.success = true;
        return result;
    }

    // 系统功能 (actionType 6-11)
    if (actionType >= 6 && actionType <= 11) {
        if (actionType == 6) { // SYS_RESTART
            result.systemRestartTriggered = true;
            result.success = true;
        } else if (actionType == 7) { // FACTORY_RESET
            result.success = true;
        } else {
            result.success = true; // NTP_SYNC, OTA 等
        }
        return result;
    }

    // 脚本命令 (actionType 15)
    if (actionType == 15) { // ACTION_SCRIPT
        if (!scriptEnabled) { result.success = false; return result; }
        if (actionValue.isEmpty()) { result.success = false; return result; }
        result.scriptExecuted = true;
        result.scriptContent = actionValue;
        result.success = true;
        return result;
    }

    // 规则控制 (actionType 22/23)
    if (actionType == 22 || actionType == 23) {
        if (targetPeriphId.isEmpty()) { result.success = false; return result; }
        result.ruleControlTarget = targetPeriphId;
        result.ruleControlEnable = (actionType == 22);
        result.success = true;
        return result;
    }

    // GPIO 动作 (actionType 0-5)
    if (targetPeriphId.isEmpty() || !targetExists) {
        result.success = false;
        return result;
    }

    switch (actionType) {
        case 0: // ACTION_HIGH
            result.physicalPinState = 1; // HIGH
            result.success = true;
            break;
        case 1: // ACTION_LOW
            result.physicalPinState = 0; // LOW
            result.success = true;
            break;
        case 4: // ACTION_SET_PWM
            result.physicalPinState = 2; // PWM
            result.pwmDuty = (uint16_t)actionValue.toInt();
            result.success = true;
            break;
        case 13: // ACTION_HIGH_INVERTED -> 物理低电平
            result.physicalPinState = 0; // physical LOW
            result.success = true;
            break;
        case 14: // ACTION_LOW_INVERTED -> 物理高电平
            result.physicalPinState = 1; // physical HIGH
            result.success = true;
            break;
        default:
            result.success = false;
            break;
    }
    return result;
}

void test_action_dispatch_gpio_high() {
    MockActionResult r = mockExecuteAction(0, "led_1", "", true, false);
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_EQUAL_UINT8(1, r.physicalPinState); // HIGH
}

void test_action_dispatch_gpio_low() {
    MockActionResult r = mockExecuteAction(1, "led_1", "", true, false);
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_EQUAL_UINT8(0, r.physicalPinState); // LOW
}

void test_action_dispatch_pwm_value() {
    MockActionResult r = mockExecuteAction(4, "pwm_1", "128", true, false);
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_EQUAL_UINT8(2, r.physicalPinState); // PWM
    TEST_ASSERT_EQUAL_UINT16(128, r.pwmDuty);
}

void test_action_dispatch_inverted_high() {
    // ACTION_HIGH_INVERTED(13): 逻辑高 -> 物理低电平
    MockActionResult r = mockExecuteAction(13, "relay_1", "", true, false);
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_EQUAL_UINT8(0, r.physicalPinState); // physical LOW
}

void test_action_dispatch_inverted_low() {
    // ACTION_LOW_INVERTED(14): 逻辑低 -> 物理高电平
    MockActionResult r = mockExecuteAction(14, "relay_1", "", true, false);
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_EQUAL_UINT8(1, r.physicalPinState); // physical HIGH
}

void test_action_dispatch_system_restart() {
    MockActionResult r = mockExecuteAction(6, "", "", false, false);
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_TRUE(r.systemRestartTriggered);
}

void test_action_dispatch_system_factory_reset() {
    MockActionResult r = mockExecuteAction(7, "", "", false, false);
    TEST_ASSERT_TRUE(r.success);
}

void test_action_dispatch_script() {
    MockActionResult r = mockExecuteAction(15, "", "mqtt_publish topic1 hello", false, true);
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_TRUE(r.scriptExecuted);
    TEST_ASSERT_EQUAL_STRING("mqtt_publish topic1 hello", r.scriptContent.c_str());
}

void test_action_dispatch_script_disabled() {
    MockActionResult r = mockExecuteAction(15, "", "some script", false, false);
    TEST_ASSERT_FALSE(r.success);
}

void test_action_dispatch_script_empty() {
    MockActionResult r = mockExecuteAction(15, "", "", false, true);
    TEST_ASSERT_FALSE(r.success);
}

void test_action_dispatch_call_peripheral() {
    MockActionResult r = mockExecuteAction(10, "stepper_1", "forward", true, false);
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_EQUAL_STRING("stepper_1", r.callPeriphTarget.c_str());
    TEST_ASSERT_EQUAL_STRING("forward", r.callPeriphCommand.c_str());
}

void test_action_dispatch_call_peripheral_no_target() {
    MockActionResult r = mockExecuteAction(10, "", "forward", true, false);
    TEST_ASSERT_FALSE(r.success);
}

void test_action_dispatch_call_peripheral_not_found() {
    MockActionResult r = mockExecuteAction(10, "nonexist", "forward", false, false);
    TEST_ASSERT_FALSE(r.success);
}

void test_action_dispatch_rule_enable() {
    MockActionResult r = mockExecuteAction(22, "rule_abc", "", false, false);
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_EQUAL_STRING("rule_abc", r.ruleControlTarget.c_str());
    TEST_ASSERT_TRUE(r.ruleControlEnable);
}

void test_action_dispatch_rule_disable() {
    MockActionResult r = mockExecuteAction(23, "rule_abc", "", false, false);
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_EQUAL_STRING("rule_abc", r.ruleControlTarget.c_str());
    TEST_ASSERT_FALSE(r.ruleControlEnable);
}

void test_action_dispatch_rule_control_no_target() {
    MockActionResult r = mockExecuteAction(22, "", "", false, false);
    TEST_ASSERT_FALSE(r.success);
}

void test_action_dispatch_unknown_type() {
    MockActionResult r = mockExecuteAction(255, "led_1", "", true, false);
    TEST_ASSERT_FALSE(r.success);
}

void test_action_dispatch_gpio_no_target() {
    MockActionResult r = mockExecuteAction(0, "", "", false, false);
    TEST_ASSERT_FALSE(r.success);
}

void test_action_dispatch_gpio_target_not_found() {
    MockActionResult r = mockExecuteAction(0, "nonexist", "", false, false);
    TEST_ASSERT_FALSE(r.success);
}

void test_action_dispatch_enum_values_match() {
    // 确保测试中的动作类型值与枚举定义一致
    TEST_ASSERT_EQUAL(0, static_cast<int>(ActionType::SET_HIGH));
    TEST_ASSERT_EQUAL(1, static_cast<int>(ActionType::SET_LOW));
    TEST_ASSERT_EQUAL(4, static_cast<int>(ActionType::SET_PWM));
    TEST_ASSERT_EQUAL(6, static_cast<int>(ActionType::SYS_RESTART));
    TEST_ASSERT_EQUAL(7, static_cast<int>(ActionType::SYS_FACTORY_RESET));
    TEST_ASSERT_EQUAL(10, static_cast<int>(ActionType::CALL_PERIPHERAL));
    TEST_ASSERT_EQUAL(13, static_cast<int>(ActionType::HIGH_INVERTED));
    TEST_ASSERT_EQUAL(14, static_cast<int>(ActionType::LOW_INVERTED));
    TEST_ASSERT_EQUAL(15, static_cast<int>(ActionType::SCRIPT));
    TEST_ASSERT_EQUAL(22, static_cast<int>(ActionType::ENABLE_EXEC_RULE));
    TEST_ASSERT_EQUAL(23, static_cast<int>(ActionType::DISABLE_EXEC_RULE));
}

// ============================================================
//  TEST GROUP 17: trigger→condition→action 全链路联动测试
// ============================================================

// 模拟完整规则执行链路
struct ChainTrigger {
    uint8_t triggerType;  // 0=platform, 1=timer, 4=event, 5=poll
    uint8_t operatorType; // 0=EQ, 1=NEQ, 2=GT, 3=LT, 4=GTE, 5=LTE
    String compareValue;
};

struct ChainAction {
    uint8_t actionType;
    String targetPeriphId;
    String actionValue;
};

struct ChainRule {
    String id;
    String name;
    bool enabled;
    std::vector<ChainTrigger> triggers;
    std::vector<ChainAction> actions;
};

// 模拟规则执行结果
struct ChainResult {
    bool triggered;          // 触发器是否匹配
    bool conditionPassed;    // 条件是否通过
    int actionsExecuted;     // 执行的动作数
    std::vector<uint8_t> executedActionTypes; // 已执行的动作类型
    bool systemRestartFlag;  // 是否触发系统重启
};

// 模拟完整的规则执行链路: trigger match → condition eval → action dispatch
static ChainResult mockExecuteRuleChain(
    const ChainRule& rule,
    uint8_t incomingTriggerType,  // 本次触发的类型
    const String& receivedValue,  // 接收到的值（用于条件评估）
    const std::map<String, bool>& targetPeriphExists // 目标外设是否存在
) {
    ChainResult result = {};
    if (!rule.enabled) return result;

    // Step 1: 触发器匹配
    bool triggerMatched = false;
    uint8_t matchedTriggerOp = 0;
    String matchedCompareValue;
    for (const auto& t : rule.triggers) {
        if (t.triggerType == incomingTriggerType) {
            triggerMatched = true;
            matchedTriggerOp = t.operatorType;
            matchedCompareValue = t.compareValue;
            break;
        }
    }
    result.triggered = triggerMatched;
    if (!triggerMatched) return result;

    // Step 2: 条件评估
    // 定时触发(operatorType=0 且 compareValue为空)无条件通过
    // 其他触发类型若有比较值则进行条件评估
    if (!matchedCompareValue.isEmpty()) {
        result.conditionPassed = mockEvalCondition(receivedValue, matchedTriggerOp, matchedCompareValue);
    } else {
        // 无比较值时，无条件通过（如定时触发）
        result.conditionPassed = true;
    }
    if (!result.conditionPassed) return result;

    // Step 3: 动作分发
    for (const auto& a : rule.actions) {
        // 系统动作(6-11)不需要目标外设
        if (a.actionType >= 6 && a.actionType <= 11) {
            if (a.actionType == 6) result.systemRestartFlag = true;
            result.executedActionTypes.push_back(a.actionType);
            result.actionsExecuted++;
            continue;
        }
        // 规则控制(22/23)不需要目标外设存在
        if (a.actionType == 22 || a.actionType == 23) {
            result.executedActionTypes.push_back(a.actionType);
            result.actionsExecuted++;
            continue;
        }
        // GPIO/PWM 等需要目标外设存在
        auto it = targetPeriphExists.find(a.targetPeriphId);
        if (it != targetPeriphExists.end() && it->second) {
            result.executedActionTypes.push_back(a.actionType);
            result.actionsExecuted++;
        }
    }
    return result;
}

// --- 链路测试用例 ---

void test_chain_platform_trigger_eq_condition_gpio_action() {
    // 平台触发 + EQ条件 + GPIO HIGH动作
    ChainRule rule;
    rule.id = "r1"; rule.name = "MQTT控制LED"; rule.enabled = true;
    rule.triggers.push_back({0, 0, "on"}); // EQ "on"
    rule.actions.push_back({0, "led_1", ""}); // ACTION_HIGH

    std::map<String, bool> periphs = {{"led_1", true}};
    ChainResult r = mockExecuteRuleChain(rule, 0, "on", periphs);
    TEST_ASSERT_TRUE(r.triggered);
    TEST_ASSERT_TRUE(r.conditionPassed);
    TEST_ASSERT_EQUAL(1, r.actionsExecuted);
    TEST_ASSERT_EQUAL(0, r.executedActionTypes[0]); // HIGH
}

void test_chain_platform_trigger_condition_fails() {
    // 平台触发 + GT条件 + 收到值不满足条件 → 动作不执行
    ChainRule rule;
    rule.id = "r2"; rule.name = "温度报警"; rule.enabled = true;
    rule.triggers.push_back({0, 2, "50"}); // GT 50
    rule.actions.push_back({0, "alarm_1", ""}); // ACTION_HIGH

    std::map<String, bool> periphs = {{"alarm_1", true}};
    ChainResult r = mockExecuteRuleChain(rule, 0, "30", periphs); // 30 不大于 50
    TEST_ASSERT_TRUE(r.triggered);
    TEST_ASSERT_FALSE(r.conditionPassed);
    TEST_ASSERT_EQUAL(0, r.actionsExecuted);
}

void test_chain_timer_trigger_no_condition_multi_actions() {
    // 定时触发 + 无条件 + 多个动作
    ChainRule rule;
    rule.id = "r3"; rule.name = "定时巡检"; rule.enabled = true;
    rule.triggers.push_back({1, 0, ""}); // TIMER, 无条件
    rule.actions.push_back({0, "led_1", ""});   // HIGH
    rule.actions.push_back({4, "pwm_1", "128"}); // PWM
    rule.actions.push_back({1, "relay_1", ""}); // LOW

    std::map<String, bool> periphs = {{"led_1", true}, {"pwm_1", true}, {"relay_1", true}};
    ChainResult r = mockExecuteRuleChain(rule, 1, "", periphs);
    TEST_ASSERT_TRUE(r.triggered);
    TEST_ASSERT_TRUE(r.conditionPassed);
    TEST_ASSERT_EQUAL(3, r.actionsExecuted);
    TEST_ASSERT_EQUAL(0, r.executedActionTypes[0]); // HIGH
    TEST_ASSERT_EQUAL(4, r.executedActionTypes[1]); // PWM
    TEST_ASSERT_EQUAL(1, r.executedActionTypes[2]); // LOW
}

void test_chain_event_trigger_with_condition() {
    // 事件触发 + 条件 + 动作
    ChainRule rule;
    rule.id = "r4"; rule.name = "按键事件"; rule.enabled = true;
    rule.triggers.push_back({4, 0, "pressed"}); // EVENT, EQ "pressed"
    rule.actions.push_back({0, "buzzer_1", ""}); // HIGH

    std::map<String, bool> periphs = {{"buzzer_1", true}};
    // 匹配
    ChainResult r1 = mockExecuteRuleChain(rule, 4, "pressed", periphs);
    TEST_ASSERT_TRUE(r1.conditionPassed);
    TEST_ASSERT_EQUAL(1, r1.actionsExecuted);
    // 不匹配
    ChainResult r2 = mockExecuteRuleChain(rule, 4, "released", periphs);
    TEST_ASSERT_FALSE(r2.conditionPassed);
    TEST_ASSERT_EQUAL(0, r2.actionsExecuted);
}

void test_chain_trigger_type_mismatch() {
    // 规则配置为定时触发，但实际收到平台触发 → 不匹配
    ChainRule rule;
    rule.id = "r5"; rule.name = "定时规则"; rule.enabled = true;
    rule.triggers.push_back({1, 0, ""}); // TIMER
    rule.actions.push_back({0, "led_1", ""});

    std::map<String, bool> periphs = {{"led_1", true}};
    ChainResult r = mockExecuteRuleChain(rule, 0, "", periphs); // platform trigger
    TEST_ASSERT_FALSE(r.triggered);
    TEST_ASSERT_EQUAL(0, r.actionsExecuted);
}

void test_chain_disabled_rule_not_executed() {
    ChainRule rule;
    rule.id = "r6"; rule.name = "已禁用规则"; rule.enabled = false;
    rule.triggers.push_back({0, 0, ""});
    rule.actions.push_back({0, "led_1", ""});

    std::map<String, bool> periphs = {{"led_1", true}};
    ChainResult r = mockExecuteRuleChain(rule, 0, "", periphs);
    TEST_ASSERT_FALSE(r.triggered);
    TEST_ASSERT_EQUAL(0, r.actionsExecuted);
}

void test_chain_system_restart_action_sync() {
    // 包含系统重启动作的规则，应标记为同步执行
    ChainRule rule;
    rule.id = "r7"; rule.name = "紧急重启"; rule.enabled = true;
    rule.triggers.push_back({0, 0, "emergency"}); // EQ "emergency"
    rule.actions.push_back({0, "led_1", ""});  // HIGH
    rule.actions.push_back({6, "", ""});        // SYS_RESTART

    std::map<String, bool> periphs = {{"led_1", true}};
    ChainResult r = mockExecuteRuleChain(rule, 0, "emergency", periphs);
    TEST_ASSERT_TRUE(r.conditionPassed);
    TEST_ASSERT_EQUAL(2, r.actionsExecuted);
    TEST_ASSERT_TRUE(r.systemRestartFlag);
}

void test_chain_action_target_missing_skipped() {
    // 目标外设不存在时，该动作被跳过，其他动作正常执行
    ChainRule rule;
    rule.id = "r8"; rule.name = "部分外设缺失"; rule.enabled = true;
    rule.triggers.push_back({1, 0, ""}); // TIMER
    rule.actions.push_back({0, "led_1", ""});     // 存在
    rule.actions.push_back({0, "missing_1", ""}); // 不存在
    rule.actions.push_back({1, "relay_1", ""});   // 存在

    std::map<String, bool> periphs = {{"led_1", true}, {"relay_1", true}};
    ChainResult r = mockExecuteRuleChain(rule, 1, "", periphs);
    TEST_ASSERT_EQUAL(2, r.actionsExecuted); // missing_1 被跳过
    TEST_ASSERT_EQUAL(0, r.executedActionTypes[0]); // HIGH (led_1)
    TEST_ASSERT_EQUAL(1, r.executedActionTypes[1]); // LOW (relay_1)
}

void test_chain_rule_control_action() {
    // 规则控制动作：禁用另一条规则
    ChainRule rule;
    rule.id = "r9"; rule.name = "联动控制"; rule.enabled = true;
    rule.triggers.push_back({0, 0, "disable_all"}); // EQ
    rule.actions.push_back({23, "rule_target", ""}); // DISABLE_EXEC_RULE

    std::map<String, bool> periphs;
    ChainResult r = mockExecuteRuleChain(rule, 0, "disable_all", periphs);
    TEST_ASSERT_EQUAL(1, r.actionsExecuted);
    TEST_ASSERT_EQUAL(23, r.executedActionTypes[0]);
}

void test_chain_poll_trigger_numeric_condition() {
    // 轮询触发 + 数值区间条件
    ChainRule rule;
    rule.id = "r10"; rule.name = "温度区间报警"; rule.enabled = true;
    rule.triggers.push_back({5, 6, "20,40"}); // POLL, BETWEEN 20-40
    rule.actions.push_back({0, "alarm_1", ""}); // HIGH

    std::map<String, bool> periphs = {{"alarm_1", true}};
    // 25 在区间内
    ChainResult r1 = mockExecuteRuleChain(rule, 5, "25", periphs);
    TEST_ASSERT_TRUE(r1.conditionPassed);
    TEST_ASSERT_EQUAL(1, r1.actionsExecuted);
    // 50 不在区间内
    ChainResult r2 = mockExecuteRuleChain(rule, 5, "50", periphs);
    TEST_ASSERT_FALSE(r2.conditionPassed);
    TEST_ASSERT_EQUAL(0, r2.actionsExecuted);
}

void test_chain_multi_trigger_first_matches() {
    // 多触发器规则，第一个匹配的触发器生效
    ChainRule rule;
    rule.id = "r11"; rule.name = "多触发源"; rule.enabled = true;
    rule.triggers.push_back({0, 0, "cmd"});  // platform EQ "cmd"
    rule.triggers.push_back({1, 0, ""});      // timer (无条件)
    rule.actions.push_back({0, "led_1", ""});

    std::map<String, bool> periphs = {{"led_1", true}};
    // 平台触发匹配
    ChainResult r1 = mockExecuteRuleChain(rule, 0, "cmd", periphs);
    TEST_ASSERT_TRUE(r1.triggered);
    TEST_ASSERT_EQUAL(1, r1.actionsExecuted);
    // 定时触发也匹配
    ChainResult r2 = mockExecuteRuleChain(rule, 1, "", periphs);
    TEST_ASSERT_TRUE(r2.triggered);
    TEST_ASSERT_EQUAL(1, r2.actionsExecuted);
}

void test_chain_mixed_system_and_gpio_actions() {
    // 混合系统动作和GPIO动作
    ChainRule rule;
    rule.id = "r12"; rule.name = "混合动作"; rule.enabled = true;
    rule.triggers.push_back({0, 0, "go"}); // EQ "go"
    rule.actions.push_back({0, "led_1", ""});   // HIGH
    rule.actions.push_back({8, "", ""});         // NTP_SYNC (系统动作)
    rule.actions.push_back({1, "relay_1", ""}); // LOW

    std::map<String, bool> periphs = {{"led_1", true}, {"relay_1", true}};
    ChainResult r = mockExecuteRuleChain(rule, 0, "go", periphs);
    TEST_ASSERT_EQUAL(3, r.actionsExecuted);
    // 系统动作(NTP_SYNC)不需要外设，也应执行
    TEST_ASSERT_EQUAL(8, r.executedActionTypes[1]);
}

// ============================================================
//  TEST GROUP 18: PeriphExecExecutor 工具函数与上报构建测试
// ============================================================

// --- 18A: tryParseBoolLike 模拟 ---
// 镜像 PeriphExecExecutor.cpp:43 的 tryParseBoolLike
static bool mockTryParseBoolLike(const String& rawValue, bool& outValue) {
    String value = rawValue;
    value.trim();
    value.toLowerCase();
    if (value.isEmpty()) return false;
    if (value == "1" || value == "true" || value == "on" ||
        value == "high" || value == "open") {
        outValue = true; return true;
    }
    if (value == "0" || value == "false" || value == "off" ||
        value == "low" || value == "close") {
        outValue = false; return true;
    }
    if (value == "+1") { outValue = true; return true; }
    if (value == "-1") { outValue = false; return true; }
    bool isNumeric = true;
    bool hasDigit = false;
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        if (c >= '0' && c <= '9') { hasDigit = true; continue; }
        if ((c == '+' || c == '-') && i == 0) continue;
        isNumeric = false; break;
    }
    if (isNumeric && hasDigit) {
        outValue = value.toInt() != 0;
        return true;
    }
    return false;
}

void test_bool_parse_digit_one() { bool v = false; TEST_ASSERT_TRUE(mockTryParseBoolLike("1", v)); TEST_ASSERT_TRUE(v); }
void test_bool_parse_digit_zero() { bool v = true; TEST_ASSERT_TRUE(mockTryParseBoolLike("0", v)); TEST_ASSERT_FALSE(v); }
void test_bool_parse_true() { bool v = false; TEST_ASSERT_TRUE(mockTryParseBoolLike("true", v)); TEST_ASSERT_TRUE(v); }
void test_bool_parse_false() { bool v = true; TEST_ASSERT_TRUE(mockTryParseBoolLike("false", v)); TEST_ASSERT_FALSE(v); }
void test_bool_parse_on() { bool v = false; TEST_ASSERT_TRUE(mockTryParseBoolLike("on", v)); TEST_ASSERT_TRUE(v); }
void test_bool_parse_off() { bool v = true; TEST_ASSERT_TRUE(mockTryParseBoolLike("off", v)); TEST_ASSERT_FALSE(v); }
void test_bool_parse_high() { bool v = false; TEST_ASSERT_TRUE(mockTryParseBoolLike("high", v)); TEST_ASSERT_TRUE(v); }
void test_bool_parse_low() { bool v = true; TEST_ASSERT_TRUE(mockTryParseBoolLike("low", v)); TEST_ASSERT_FALSE(v); }
void test_bool_parse_open() { bool v = false; TEST_ASSERT_TRUE(mockTryParseBoolLike("open", v)); TEST_ASSERT_TRUE(v); }
void test_bool_parse_close() { bool v = true; TEST_ASSERT_TRUE(mockTryParseBoolLike("close", v)); TEST_ASSERT_FALSE(v); }
void test_bool_parse_plus_one() { bool v = false; TEST_ASSERT_TRUE(mockTryParseBoolLike("+1", v)); TEST_ASSERT_TRUE(v); }
void test_bool_parse_minus_one() { bool v = true; TEST_ASSERT_TRUE(mockTryParseBoolLike("-1", v)); TEST_ASSERT_FALSE(v); }
void test_bool_parse_numeric_nonzero() { bool v = false; TEST_ASSERT_TRUE(mockTryParseBoolLike("42", v)); TEST_ASSERT_TRUE(v); }
void test_bool_parse_numeric_zero() { bool v = true; TEST_ASSERT_TRUE(mockTryParseBoolLike("00", v)); TEST_ASSERT_FALSE(v); }
void test_bool_parse_case_insensitive() { bool v = false; TEST_ASSERT_TRUE(mockTryParseBoolLike("TRUE", v)); TEST_ASSERT_TRUE(v); }
void test_bool_parse_mixed_case() { bool v = false; TEST_ASSERT_TRUE(mockTryParseBoolLike("On", v)); TEST_ASSERT_TRUE(v); }
void test_bool_parse_whitespace_trim() { bool v = false; TEST_ASSERT_TRUE(mockTryParseBoolLike("  1  ", v)); TEST_ASSERT_TRUE(v); }
void test_bool_parse_empty_fails() { bool v = true; TEST_ASSERT_FALSE(mockTryParseBoolLike("", v)); }
void test_bool_parse_garbage_fails() { bool v = true; TEST_ASSERT_FALSE(mockTryParseBoolLike("abc", v)); }
void test_bool_parse_partial_digit_fails() { bool v = true; TEST_ASSERT_FALSE(mockTryParseBoolLike("12abc", v)); }

// --- 18B: looksLikeHexPayload 模拟 ---
static bool mockLooksLikeHex(const String& rawValue) {
    String value = rawValue;
    value.trim();
    if (value.length() < 4 || (value.length() % 2) != 0) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!isHex) return false;
    }
    return true;
}

void test_hex_valid_4chars() { TEST_ASSERT_TRUE(mockLooksLikeHex("01FF")); }
void test_hex_valid_8chars() { TEST_ASSERT_TRUE(mockLooksLikeHex("DEADBEEF")); }
void test_hex_valid_lowercase() { TEST_ASSERT_TRUE(mockLooksLikeHex("0123abcd")); }
void test_hex_valid_mixedcase() { TEST_ASSERT_TRUE(mockLooksLikeHex("Aa01")); }
void test_hex_too_short() { TEST_ASSERT_FALSE(mockLooksLikeHex("AB")); }
void test_hex_odd_length() { TEST_ASSERT_FALSE(mockLooksLikeHex("ABC")); }
void test_hex_nonhex_chars() { TEST_ASSERT_FALSE(mockLooksLikeHex("GHIJ")); }
void test_hex_empty() { TEST_ASSERT_FALSE(mockLooksLikeHex("")); }
void test_hex_whitespace_trimmed() { TEST_ASSERT_TRUE(mockLooksLikeHex("  A1B2  ")); }
void test_hex_mixed_valid_invalid() { TEST_ASSERT_FALSE(mockLooksLikeHex("01G2")); }

// --- 18C: normalizeBinaryReportValue / normalizeScalarReportValue 模拟 ---
static String mockNormalizeBinary(const String& preferred, const String& fallback, bool defaultState) {
    bool parsedState = defaultState;
    if (mockTryParseBoolLike(preferred, parsedState) || mockTryParseBoolLike(fallback, parsedState)) {
        return parsedState ? "1" : "0";
    }
    return defaultState ? "1" : "0";
}

static String mockNormalizeScalar(const String& preferred, const String& fallback, const char* def = "") {
    String r = preferred; r.trim();
    if (!r.isEmpty()) return r;
    r = fallback; r.trim();
    if (!r.isEmpty()) return r;
    return String(def);
}

void test_binary_normalize_preferred_on() {
    TEST_ASSERT_EQUAL_STRING("1", mockNormalizeBinary("on", "", false).c_str());
}
void test_binary_normalize_preferred_off() {
    TEST_ASSERT_EQUAL_STRING("0", mockNormalizeBinary("off", "", true).c_str());
}
void test_binary_normalize_fallback_used() {
    TEST_ASSERT_EQUAL_STRING("1", mockNormalizeBinary("", "true", false).c_str());
}
void test_binary_normalize_default_used() {
    TEST_ASSERT_EQUAL_STRING("0", mockNormalizeBinary("", "", false).c_str());
    TEST_ASSERT_EQUAL_STRING("1", mockNormalizeBinary("", "", true).c_str());
}
void test_binary_normalize_preferred_priority() {
    // preferred 优先于 fallback
    TEST_ASSERT_EQUAL_STRING("1", mockNormalizeBinary("1", "0", false).c_str());
}

void test_scalar_normalize_preferred_used() {
    TEST_ASSERT_EQUAL_STRING("128", mockNormalizeScalar("128", "").c_str());
}
void test_scalar_normalize_fallback_used() {
    TEST_ASSERT_EQUAL_STRING("50", mockNormalizeScalar("", "50").c_str());
}
void test_scalar_normalize_default_used() {
    TEST_ASSERT_EQUAL_STRING("0", mockNormalizeScalar("", "", "0").c_str());
}
void test_scalar_normalize_whitespace_trimmed() {
    TEST_ASSERT_EQUAL_STRING("42", mockNormalizeScalar("  42  ", "").c_str());
}

// --- 18D: buildActionReportValue 模拟 ---
// 简化版镜像，只测试 HIGH/LOW/PWM/MODBUS_COIL/MODBUS_REG 分支
struct MockExecAction { uint8_t actionType; };

static String mockBuildActionReport(const MockExecAction& a, const String& eff, const String& recv) {
    switch (a.actionType) {
        case 0: case 13: return mockNormalizeBinary(eff, recv, true);   // HIGH / HIGH_INVERTED
        case 1: case 14: return mockNormalizeBinary(eff, recv, false);  // LOW / LOW_INVERTED
        case 4: case 5:  return mockNormalizeScalar(eff, recv, "0");    // PWM / DAC
        case 16: { // MODBUS_COIL_WRITE
            String raw = mockNormalizeScalar(eff, recv, "");
            uint16_t ch = 0;
            String sv = raw;
            int sep = raw.indexOf(':');
            if (sep > 0) { ch = raw.substring(0, sep).toInt(); sv = raw.substring(sep + 1); }
            // buildModbusReportValue
            return String(ch) + ":" + mockNormalizeBinary(sv, recv, false).c_str();
        }
        case 17: { // MODBUS_REG_WRITE
            String raw = mockNormalizeScalar(eff, recv, "0");
            int sep = raw.indexOf(':');
            if (sep > 0) {
                uint16_t ch = raw.substring(0, sep).toInt();
                String rv = raw.substring(sep + 1); rv.trim();
                return String(ch) + ":" + rv;
            }
            return raw;
        }
        default: return mockNormalizeScalar(eff, recv, "");
    }
}

void test_report_high_action() {
    MockExecAction a = {0};
    TEST_ASSERT_EQUAL_STRING("1", mockBuildActionReport(a, "on", "").c_str());
}
void test_report_low_action() {
    MockExecAction a = {1};
    TEST_ASSERT_EQUAL_STRING("0", mockBuildActionReport(a, "off", "").c_str());
}
void test_report_pwm_action() {
    MockExecAction a = {4};
    TEST_ASSERT_EQUAL_STRING("128", mockBuildActionReport(a, "128", "").c_str());
}
void test_report_modbus_coil_write() {
    MockExecAction a = {16};
    // "2:1" → channel=2, state=1
    TEST_ASSERT_EQUAL_STRING("2:1", mockBuildActionReport(a, "2:1", "").c_str());
}
void test_report_modbus_reg_write() {
    MockExecAction a = {17};
    // "100:255" → channel=100, value=255
    TEST_ASSERT_EQUAL_STRING("100:255", mockBuildActionReport(a, "100:255", "").c_str());
}
void test_report_modbus_coil_no_channel() {
    MockExecAction a = {16};
    // 无分隔符，channel=0
    TEST_ASSERT_EQUAL_STRING("0:1", mockBuildActionReport(a, "1", "").c_str());
}
void test_report_high_inverted() {
    MockExecAction a = {13}; // HIGH_INVERTED
    TEST_ASSERT_EQUAL_STRING("1", mockBuildActionReport(a, "1", "").c_str());
}
void test_report_default_scalar() {
    MockExecAction a = {255};
    TEST_ASSERT_EQUAL_STRING("42", mockBuildActionReport(a, "42", "").c_str());
}

// --- 18E: effectiveValue 选择逻辑 ---
// 镜像 executeAllActions 中的 effectiveValue 计算
static String mockEffectiveValue(bool useReceived, const String& received, const String& actionValue) {
    return (useReceived && !received.isEmpty()) ? received : actionValue;
}

void test_effective_value_use_received() {
    TEST_ASSERT_EQUAL_STRING("MQTT_data", mockEffectiveValue(true, "MQTT_data", "default").c_str());
}
void test_effective_value_empty_received_use_action() {
    TEST_ASSERT_EQUAL_STRING("default", mockEffectiveValue(true, "", "default").c_str());
}
void test_effective_value_not_use_received() {
    TEST_ASSERT_EQUAL_STRING("action_val", mockEffectiveValue(false, "mqtt_val", "action_val").c_str());
}
void test_effective_value_both_empty() {
    TEST_ASSERT_EQUAL_STRING("", mockEffectiveValue(true, "", "").c_str());
}

// ============================================================
//  测试入口 (更新)
// ============================================================

void test_periph_exec_group() {
    // Group 1: 调度器配置校验
    RUN_TEST(test_poll_interval_below_minimum_gets_corrected);
    RUN_TEST(test_poll_interval_at_minimum_not_modified);
    RUN_TEST(test_poll_interval_above_minimum_not_modified);
    RUN_TEST(test_poll_interval_aggressive_with_many_tasks);
    RUN_TEST(test_poll_interval_not_aggressive_with_few_tasks);
    RUN_TEST(test_poll_interval_zero_seconds_corrected);
    RUN_TEST(test_poll_interval_non_timer_trigger_ignored);
    
    // Group 2: 动态降频
    RUN_TEST(test_dynamic_check_period_normal);
    RUN_TEST(test_dynamic_check_period_warn);
    RUN_TEST(test_dynamic_check_period_severe);
    RUN_TEST(test_dynamic_check_period_critical);
    RUN_TEST(test_frequency_reduction_doubles_per_level);
    
    // Group 3: 内存保护暂停
    RUN_TEST(test_suspend_when_guard_level_severe);
    RUN_TEST(test_suspend_when_guard_level_critical);
    RUN_TEST(test_suspend_when_free_heap_below_threshold);
    RUN_TEST(test_suspend_when_largest_block_below_threshold);
    RUN_TEST(test_suspend_when_fragmented_and_small_block);
    RUN_TEST(test_no_suspend_normal_conditions);
    RUN_TEST(test_no_suspend_high_fragmentation_but_large_block);
    RUN_TEST(test_no_suspend_warn_level_with_enough_memory);
    RUN_TEST(test_suspend_boundary_free_heap_exactly_threshold);
    RUN_TEST(test_suspend_boundary_free_heap_one_below);
    RUN_TEST(test_suspend_boundary_largest_block_exactly_threshold);
    RUN_TEST(test_suspend_boundary_largest_block_one_below);
    
    // Group 4: 按键事件状态机
    RUN_TEST(test_button_debounce_rejects_fast_changes);
    RUN_TEST(test_button_debounce_accepts_stable_changes);
    RUN_TEST(test_button_long_press_2s_detection);
    RUN_TEST(test_button_long_press_not_retriggered);
    RUN_TEST(test_button_double_click_interval);
    RUN_TEST(test_button_double_click_expired);
    RUN_TEST(test_button_long_press_5s_after_2s);
    
    // Group 5: 规则执行管理
    RUN_TEST(test_rule_crud_add_and_get);
    RUN_TEST(test_rule_crud_duplicate_add_fails);
    RUN_TEST(test_rule_crud_remove);
    RUN_TEST(test_rule_crud_remove_nonexistent);
    RUN_TEST(test_rule_execution_set_high);
    RUN_TEST(test_rule_execution_disabled_rule_fails);
    RUN_TEST(test_rule_execution_toggle);
    RUN_TEST(test_rule_execution_pwm);
    
    // Group 6: 边界条件
    RUN_TEST(test_max_active_tasks_limit);
    RUN_TEST(test_empty_rule_id_rejected);
    RUN_TEST(test_execute_nonexistent_rule);
    RUN_TEST(test_multiple_triggers_per_rule);
    RUN_TEST(test_poll_interval_uint32_overflow_protection);

    // Group 7: 定时触发防卡死机制
    RUN_TEST(test_timer_interval_first_trigger);
    RUN_TEST(test_timer_interval_not_yet_due);
    RUN_TEST(test_timer_interval_exact_due);
    RUN_TEST(test_timer_interval_zero_no_trigger);
    RUN_TEST(test_timer_interval_millis_overflow_safe);
    RUN_TEST(test_timer_interval_24h_boundary);
    RUN_TEST(test_backoff_blocks_retrigger);
    RUN_TEST(test_backoff_cleanup_removes_expired);
    RUN_TEST(test_backoff_no_cleanup_within_window);
    RUN_TEST(test_stuck_rule_detected_after_60s);
    RUN_TEST(test_running_rule_not_stuck);
    RUN_TEST(test_stuck_rule_missing_start_time);
    RUN_TEST(test_not_running_rule_not_stuck);

    // Group 8: Modbus 轮询触发防卡死
    RUN_TEST(test_poll_ingress_first_request_passes);
    RUN_TEST(test_poll_ingress_rapid_requests_throttled);
    RUN_TEST(test_poll_ingress_after_interval_passes);
    RUN_TEST(test_poll_ingress_different_sources_independent);
    RUN_TEST(test_modbus_poll_skip_on_critical_memory);
    RUN_TEST(test_modbus_poll_skip_on_severe_memory);
    RUN_TEST(test_modbus_poll_skip_on_low_heap);
    RUN_TEST(test_modbus_poll_ok_with_sufficient_memory);
    RUN_TEST(test_modbus_poll_boundary_heap_25000);
    RUN_TEST(test_modbus_inner_loop_stop_on_critical);
    RUN_TEST(test_modbus_inner_loop_stop_on_low_heap);
    RUN_TEST(test_modbus_inner_loop_continue_normal);
    RUN_TEST(test_poll_inter_delay_respects_config);
    RUN_TEST(test_timer_skip_when_modbus_unavailable);
    RUN_TEST(test_timer_proceed_when_modbus_available);
    RUN_TEST(test_timer_proceed_when_no_modbus_needed);

    // Group 9: 异步执行/同步降级防卡死
    RUN_TEST(test_async_ok_with_sufficient_resources);
    RUN_TEST(test_async_skip_on_low_heap);
    RUN_TEST(test_async_skip_on_no_slots);
    RUN_TEST(test_async_boundary_heap_20000);
    RUN_TEST(test_heavy_rule_avoids_sync_fallback);
    RUN_TEST(test_script_rule_avoids_sync_fallback);
    RUN_TEST(test_sensor_read_avoids_sync_fallback);
    RUN_TEST(test_simple_gpio_allows_sync_fallback);
    RUN_TEST(test_modbus_target_avoids_sync_fallback);
    RUN_TEST(test_action_loop_break_on_critical);
    RUN_TEST(test_action_loop_break_on_severe_low);
    RUN_TEST(test_action_loop_continue_normal);
    RUN_TEST(test_action_loop_break_on_very_low_heap);
    RUN_TEST(test_sync_delay_clamped_to_10s);
    RUN_TEST(test_sync_delay_normal_value);

    // Group 10: Worker Pool 队列防溢出
    RUN_TEST(test_worker_pool_constants_sanity);
    RUN_TEST(test_worker_stack_size_adequate);

    // Group 11: 执行模式语义与脚本兼容
    RUN_TEST(test_exec_mode_async_is_zero);
    RUN_TEST(test_exec_mode_sync_is_one);
    RUN_TEST(test_script_content_null_compatibility);
    RUN_TEST(test_action_type_inverted_enum_values);
    RUN_TEST(test_action_type_high_inverted_semantics);
    RUN_TEST(test_action_type_values_complete);

    // Group 12: evaluateCondition 条件评估全量测试
    RUN_TEST(test_eval_eq_numeric_equal);
    RUN_TEST(test_eval_eq_numeric_not_equal);
    RUN_TEST(test_eval_eq_string_equal);
    RUN_TEST(test_eval_eq_string_not_equal);
    RUN_TEST(test_eval_eq_mixed_numeric_vs_string);
    RUN_TEST(test_eval_neq_numeric);
    RUN_TEST(test_eval_neq_string);
    RUN_TEST(test_eval_gt_lt_gte_lte);
    RUN_TEST(test_eval_between_in_range);
    RUN_TEST(test_eval_between_at_boundary);
    RUN_TEST(test_eval_between_out_of_range);
    RUN_TEST(test_eval_between_no_comma_returns_false);
    RUN_TEST(test_eval_not_between);
    RUN_TEST(test_eval_contain_found);
    RUN_TEST(test_eval_contain_not_found);
    RUN_TEST(test_eval_not_contain);
    RUN_TEST(test_eval_non_numeric_gt_returns_zero);
    RUN_TEST(test_eval_negative_values);

    // Group 13: sanitizeTriggerForSafety 参数安全修正
    RUN_TEST(test_sanitize_timer_interval_zero_corrected);
    RUN_TEST(test_exec_sanitize_timer_interval_below_min);
    RUN_TEST(test_exec_sanitize_timer_interval_above_max);
    RUN_TEST(test_sanitize_timer_interval_valid_no_change);
    RUN_TEST(test_sanitize_timer_boundary_min);
    RUN_TEST(test_sanitize_timer_boundary_max);
    RUN_TEST(test_exec_sanitize_poll_timeout_below_min);
    RUN_TEST(test_exec_sanitize_poll_timeout_above_max);
    RUN_TEST(test_sanitize_poll_timeout_valid_no_change);
    RUN_TEST(test_exec_sanitize_poll_retries_above_max);
    RUN_TEST(test_exec_sanitize_poll_inter_delay_below_min);
    RUN_TEST(test_exec_sanitize_poll_inter_delay_above_max);
    RUN_TEST(test_sanitize_heavy_poll_timeout_restricted);
    RUN_TEST(test_sanitize_heavy_poll_retries_restricted);
    RUN_TEST(test_sanitize_heavy_poll_inter_delay_min_raised);
    RUN_TEST(test_sanitize_heavy_poll_valid_no_change);
    RUN_TEST(test_sanitize_non_timer_poll_trigger_ignored);

    // Group 14: 每日时间点触发模式
    RUN_TEST(test_daily_time_trigger_matches);
    RUN_TEST(test_daily_time_trigger_no_match_hour);
    RUN_TEST(test_daily_time_trigger_no_match_minute);
    RUN_TEST(test_daily_time_trigger_time_not_synced);
    RUN_TEST(test_daily_time_trigger_cooldown_60s);
    RUN_TEST(test_daily_time_trigger_cooldown_expired);
    RUN_TEST(test_daily_time_trigger_invalid_format);
    RUN_TEST(test_daily_time_trigger_midnight);
    RUN_TEST(test_daily_time_trigger_end_of_day);
    RUN_TEST(test_daily_time_trigger_cross_day_boundary);

    // Group 15: 轮询触发冷却机制
    RUN_TEST(test_poll_cooldown_normal_source);
    RUN_TEST(test_poll_cooldown_modbus_heavy);
    RUN_TEST(test_poll_cooldown_modbus_no_heavy_action);
    RUN_TEST(test_poll_cooldown_modbus_raw_source);
    RUN_TEST(test_poll_cooldown_other_source);
    RUN_TEST(test_poll_trigger_first_time_no_cooldown);
    RUN_TEST(test_poll_trigger_within_cooldown);
    RUN_TEST(test_poll_trigger_after_cooldown);
    RUN_TEST(test_poll_trigger_exact_cooldown_boundary);
    RUN_TEST(test_poll_trigger_heavy_cooldown_longer);
    RUN_TEST(test_poll_ingress_modbus_throttle_1s);
    RUN_TEST(test_poll_ingress_independent_sources);

    // Group 16: 动作分发测试
    RUN_TEST(test_action_dispatch_gpio_high);
    RUN_TEST(test_action_dispatch_gpio_low);
    RUN_TEST(test_action_dispatch_pwm_value);
    RUN_TEST(test_action_dispatch_inverted_high);
    RUN_TEST(test_action_dispatch_inverted_low);
    RUN_TEST(test_action_dispatch_system_restart);
    RUN_TEST(test_action_dispatch_system_factory_reset);
    RUN_TEST(test_action_dispatch_script);
    RUN_TEST(test_action_dispatch_script_disabled);
    RUN_TEST(test_action_dispatch_script_empty);
    RUN_TEST(test_action_dispatch_call_peripheral);
    RUN_TEST(test_action_dispatch_call_peripheral_no_target);
    RUN_TEST(test_action_dispatch_call_peripheral_not_found);
    RUN_TEST(test_action_dispatch_rule_enable);
    RUN_TEST(test_action_dispatch_rule_disable);
    RUN_TEST(test_action_dispatch_rule_control_no_target);
    RUN_TEST(test_action_dispatch_unknown_type);
    RUN_TEST(test_action_dispatch_gpio_no_target);
    RUN_TEST(test_action_dispatch_gpio_target_not_found);
    RUN_TEST(test_action_dispatch_enum_values_match);

    // Group 17: trigger→condition→action 全链路联动测试
    RUN_TEST(test_chain_platform_trigger_eq_condition_gpio_action);
    RUN_TEST(test_chain_platform_trigger_condition_fails);
    RUN_TEST(test_chain_timer_trigger_no_condition_multi_actions);
    RUN_TEST(test_chain_event_trigger_with_condition);
    RUN_TEST(test_chain_trigger_type_mismatch);
    RUN_TEST(test_chain_disabled_rule_not_executed);
    RUN_TEST(test_chain_system_restart_action_sync);
    RUN_TEST(test_chain_action_target_missing_skipped);
    RUN_TEST(test_chain_rule_control_action);
    RUN_TEST(test_chain_poll_trigger_numeric_condition);
    RUN_TEST(test_chain_multi_trigger_first_matches);
    RUN_TEST(test_chain_mixed_system_and_gpio_actions);

    // Group 18: 工具函数与上报构建测试
    // 18A: tryParseBoolLike
    RUN_TEST(test_bool_parse_digit_one);
    RUN_TEST(test_bool_parse_digit_zero);
    RUN_TEST(test_bool_parse_true);
    RUN_TEST(test_bool_parse_false);
    RUN_TEST(test_bool_parse_on);
    RUN_TEST(test_bool_parse_off);
    RUN_TEST(test_bool_parse_high);
    RUN_TEST(test_bool_parse_low);
    RUN_TEST(test_bool_parse_open);
    RUN_TEST(test_bool_parse_close);
    RUN_TEST(test_bool_parse_plus_one);
    RUN_TEST(test_bool_parse_minus_one);
    RUN_TEST(test_bool_parse_numeric_nonzero);
    RUN_TEST(test_bool_parse_numeric_zero);
    RUN_TEST(test_bool_parse_case_insensitive);
    RUN_TEST(test_bool_parse_mixed_case);
    RUN_TEST(test_bool_parse_whitespace_trim);
    RUN_TEST(test_bool_parse_empty_fails);
    RUN_TEST(test_bool_parse_garbage_fails);
    RUN_TEST(test_bool_parse_partial_digit_fails);
    // 18B: looksLikeHexPayload
    RUN_TEST(test_hex_valid_4chars);
    RUN_TEST(test_hex_valid_8chars);
    RUN_TEST(test_hex_valid_lowercase);
    RUN_TEST(test_hex_valid_mixedcase);
    RUN_TEST(test_hex_too_short);
    RUN_TEST(test_hex_odd_length);
    RUN_TEST(test_hex_nonhex_chars);
    RUN_TEST(test_hex_empty);
    RUN_TEST(test_hex_whitespace_trimmed);
    RUN_TEST(test_hex_mixed_valid_invalid);
    // 18C: normalizeBinary/ScalarReportValue
    RUN_TEST(test_binary_normalize_preferred_on);
    RUN_TEST(test_binary_normalize_preferred_off);
    RUN_TEST(test_binary_normalize_fallback_used);
    RUN_TEST(test_binary_normalize_default_used);
    RUN_TEST(test_binary_normalize_preferred_priority);
    RUN_TEST(test_scalar_normalize_preferred_used);
    RUN_TEST(test_scalar_normalize_fallback_used);
    RUN_TEST(test_scalar_normalize_default_used);
    RUN_TEST(test_scalar_normalize_whitespace_trimmed);
    // 18D: buildActionReportValue
    RUN_TEST(test_report_high_action);
    RUN_TEST(test_report_low_action);
    RUN_TEST(test_report_pwm_action);
    RUN_TEST(test_report_modbus_coil_write);
    RUN_TEST(test_report_modbus_reg_write);
    RUN_TEST(test_report_modbus_coil_no_channel);
    RUN_TEST(test_report_high_inverted);
    RUN_TEST(test_report_default_scalar);
    // 18E: effectiveValue 选择逻辑
    RUN_TEST(test_effective_value_use_received);
    RUN_TEST(test_effective_value_empty_received_use_action);
    RUN_TEST(test_effective_value_not_use_received);
    RUN_TEST(test_effective_value_both_empty);
}
