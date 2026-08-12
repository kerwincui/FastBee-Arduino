/**
 * @file test_feature_budget.cpp
 * @brief FeatureBudgetRegistry 单元测试（计划 7.1）
 *
 * 覆盖: 拒绝表记录与查询 / kMaxDenied=16 容量上限（G5 缺陷修复）/
 *       checked 标记 / 空键安全
 */

#include <unity.h>
#include <Arduino.h>

#include "core/FeatureBudget.h"

using FastBee::FeatureBudgetRegistry;
using FastBee::FeatureBudgetRegistryAccess;

// Test 1: deny/isDenied 基本记录与查询
void test_budget_deny_and_query() {
    auto& reg = FeatureBudgetRegistry::instance();
    FeatureBudgetRegistryAccess::deny("budgetTestA");
    TEST_ASSERT_TRUE(reg.isDenied("budgetTestA"));
    TEST_ASSERT_FALSE(reg.isDenied("budgetTestB"));
}

// Test 2: 拒绝表容量上限（G5: kMaxDenied 由 8 扩展为 16，超限不越界）
void test_budget_denied_table_capacity() {
    auto& reg = FeatureBudgetRegistry::instance();
    // 灌入超过 kMaxDenied 的条目，验证不发生数组越界
    for (int i = 0; i < 24; ++i) {
        FeatureBudgetRegistryAccess::deny("budgetOverflowItem");
    }
    // 表仍可正常查询，且容量受控（遍历不崩溃即通过）
    TEST_ASSERT_TRUE(reg.isDenied("budgetOverflowItem"));
    TEST_ASSERT_FALSE(reg.isDenied("budgetNeverDenied"));
}

// Test 3: markChecked 标记启动预算校验已完成
void test_budget_checked_flag() {
    auto& reg = FeatureBudgetRegistry::instance();
    FeatureBudgetRegistryAccess::markChecked();
    TEST_ASSERT_TRUE(reg.checked());
}

// Test 4: 空指针安全
void test_budget_null_key_safe() {
    auto& reg = FeatureBudgetRegistry::instance();
    TEST_ASSERT_FALSE(reg.isDenied(nullptr));
    // deny(nullptr) 不应崩溃
    FeatureBudgetRegistryAccess::deny(nullptr);
    TEST_ASSERT_FALSE(reg.isDenied(""));
}

// Test 5: FeatureRuntimeState 枚举值稳定（/api/system/status 依赖）
void test_budget_runtime_state_values() {
    TEST_ASSERT_EQUAL(0, static_cast<int>(FastBee::FeatureRuntimeState::Disabled));
    TEST_ASSERT_EQUAL(1, static_cast<int>(FastBee::FeatureRuntimeState::Active));
    TEST_ASSERT_EQUAL(2, static_cast<int>(FastBee::FeatureRuntimeState::NoMemory));
}

void test_feature_budget_group() {
    RUN_TEST(test_budget_deny_and_query);
    RUN_TEST(test_budget_denied_table_capacity);
    RUN_TEST(test_budget_checked_flag);
    RUN_TEST(test_budget_null_key_safe);
    RUN_TEST(test_budget_runtime_state_values);
}
