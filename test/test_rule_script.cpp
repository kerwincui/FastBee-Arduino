/**
 * @file test_rule_script.cpp
 * @brief RuleScriptManager/ScriptEngine 单元测试
 * 
 * 测试内容：
 * - 规则 CRUD 操作（添加、查询、更新、删除）
 * - 规则启用/禁用
 * - 规则计数
 * - 数据转换管道
 * - 边界条件（空ID、重复添加）
 * 
 * 注：native环境通过模拟RuleScript结构体测试逻辑
 */

#include <unity.h>
#include <Arduino.h>
#include "mocks/MockPeripheral.h"

void test_rule_script_group();

// ========== 模拟 RuleScript 结构（镜像自 RuleScript.h 核心字段） ==========
struct TestRuleScript {
    String id;
    String name;
    String description;
    bool enabled;
    uint8_t protocolType;  // 0=MQTT, 1=HTTP, 2=Modbus
    String receiveTemplate;
    String reportTemplate;
    
    TestRuleScript() : enabled(true), protocolType(0) {}
};

// ========== 简化的规则管理器（模拟核心逻辑） ==========
#include <map>

class TestRuleManager {
public:
    bool addRule(const TestRuleScript& rule) {
        if (rule.id.isEmpty()) return false;
        if (_rules.find(rule.id) != _rules.end()) return false;
        _rules[rule.id] = rule;
        return true;
    }
    
    bool updateRule(const String& id, const TestRuleScript& rule) {
        auto it = _rules.find(id);
        if (it == _rules.end()) return false;
        it->second = rule;
        it->second.id = id;  // 保持ID不变
        return true;
    }
    
    bool removeRule(const String& id) {
        return _rules.erase(id) > 0;
    }
    
    TestRuleScript* getRule(const String& id) {
        auto it = _rules.find(id);
        return (it != _rules.end()) ? &it->second : nullptr;
    }
    
    bool enableRule(const String& id) {
        auto it = _rules.find(id);
        if (it == _rules.end()) return false;
        it->second.enabled = true;
        return true;
    }
    
    bool disableRule(const String& id) {
        auto it = _rules.find(id);
        if (it == _rules.end()) return false;
        it->second.enabled = false;
        return true;
    }
    
    size_t getRuleCount() const { return _rules.size(); }
    
    // 简化模板引擎：替换 {{key}} 为 JSON 值
    static String applyTemplate(const String& tpl, const String& jsonInput) {
        if (tpl.isEmpty()) return jsonInput;
        // 简单替换：找 {{...}} 并用输入替换
        String result = tpl;
        int start = result.indexOf("{{data}}");
        if (start >= 0) {
            result = result.substring(0, start) + jsonInput + result.substring(start + 8);
        }
        return result;
    }
    
    String applyReceiveTransform(uint8_t protocolType, const String& rawData) {
        for (auto& pair : _rules) {
            auto& rule = pair.second;
            if (rule.enabled && rule.protocolType == protocolType && !rule.receiveTemplate.isEmpty()) {
                return applyTemplate(rule.receiveTemplate, rawData);
            }
        }
        return rawData;
    }
    
    String applyReportTransform(uint8_t protocolType, const String& rawData) {
        for (auto& pair : _rules) {
            auto& rule = pair.second;
            if (rule.enabled && rule.protocolType == protocolType && !rule.reportTemplate.isEmpty()) {
                return applyTemplate(rule.reportTemplate, rawData);
            }
        }
        return rawData;
    }

private:
    std::map<String, TestRuleScript> _rules;
};

// ========== 测试用例 ==========

static TestRuleManager mgr;

static void resetManager() {
    mgr = TestRuleManager();
}

static void test_add_rule() {
    resetManager();
    TestRuleScript rule;
    rule.id = "rule_001";
    rule.name = "MQTT Transform";
    rule.enabled = true;
    rule.protocolType = 0;
    
    TEST_ASSERT_TRUE(mgr.addRule(rule));
    TEST_ASSERT_EQUAL(1, mgr.getRuleCount());
    
    auto* found = mgr.getRule("rule_001");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_STRING("MQTT Transform", found->name.c_str());
}

static void test_add_rule_empty_id() {
    resetManager();
    TestRuleScript rule;
    rule.id = "";
    
    TEST_ASSERT_FALSE(mgr.addRule(rule));
    TEST_ASSERT_EQUAL(0, mgr.getRuleCount());
}

static void test_add_duplicate_rule() {
    resetManager();
    TestRuleScript rule;
    rule.id = "dup_001";
    rule.name = "First";
    
    TEST_ASSERT_TRUE(mgr.addRule(rule));
    
    rule.name = "Second";
    TEST_ASSERT_FALSE(mgr.addRule(rule));
    TEST_ASSERT_EQUAL(1, mgr.getRuleCount());
    
    // 原始规则应保持不变
    auto* found = mgr.getRule("dup_001");
    TEST_ASSERT_EQUAL_STRING("First", found->name.c_str());
}

static void test_update_rule() {
    resetManager();
    TestRuleScript rule;
    rule.id = "upd_001";
    rule.name = "Original";
    mgr.addRule(rule);
    
    TestRuleScript updated;
    updated.name = "Updated Name";
    updated.description = "New description";
    
    TEST_ASSERT_TRUE(mgr.updateRule("upd_001", updated));
    
    auto* found = mgr.getRule("upd_001");
    TEST_ASSERT_EQUAL_STRING("Updated Name", found->name.c_str());
    TEST_ASSERT_EQUAL_STRING("upd_001", found->id.c_str());  // ID不变
}

static void test_update_nonexistent() {
    resetManager();
    TestRuleScript rule;
    rule.name = "Ghost";
    
    TEST_ASSERT_FALSE(mgr.updateRule("nonexistent", rule));
}

static void test_remove_rule() {
    resetManager();
    TestRuleScript rule;
    rule.id = "del_001";
    mgr.addRule(rule);
    
    TEST_ASSERT_TRUE(mgr.removeRule("del_001"));
    TEST_ASSERT_EQUAL(0, mgr.getRuleCount());
    TEST_ASSERT_NULL(mgr.getRule("del_001"));
}

static void test_remove_nonexistent() {
    resetManager();
    TEST_ASSERT_FALSE(mgr.removeRule("ghost"));
}

static void test_enable_disable_rule() {
    resetManager();
    TestRuleScript rule;
    rule.id = "toggle_001";
    rule.enabled = true;
    mgr.addRule(rule);
    
    TEST_ASSERT_TRUE(mgr.disableRule("toggle_001"));
    auto* found = mgr.getRule("toggle_001");
    TEST_ASSERT_FALSE(found->enabled);
    
    TEST_ASSERT_TRUE(mgr.enableRule("toggle_001"));
    TEST_ASSERT_TRUE(found->enabled);
}

static void test_enable_nonexistent() {
    resetManager();
    TEST_ASSERT_FALSE(mgr.enableRule("ghost"));
    TEST_ASSERT_FALSE(mgr.disableRule("ghost"));
}

static void test_template_apply_basic() {
    String tpl = "prefix_{{data}}_suffix";
    String result = TestRuleManager::applyTemplate(tpl, "hello");
    TEST_ASSERT_EQUAL_STRING("prefix_hello_suffix", result.c_str());
}

static void test_template_apply_empty() {
    // 空模板应透传原始数据
    String result = TestRuleManager::applyTemplate("", "raw_data");
    TEST_ASSERT_EQUAL_STRING("raw_data", result.c_str());
}

static void test_receive_transform() {
    resetManager();
    TestRuleScript rule;
    rule.id = "rx_001";
    rule.enabled = true;
    rule.protocolType = 0;  // MQTT
    rule.receiveTemplate = "{\"wrapped\":\"{{data}}\"}";
    mgr.addRule(rule);
    
    String result = mgr.applyReceiveTransform(0, "sensor_value");
    TEST_ASSERT_EQUAL_STRING("{\"wrapped\":\"sensor_value\"}", result.c_str());
}

static void test_report_transform() {
    resetManager();
    TestRuleScript rule;
    rule.id = "tx_001";
    rule.enabled = true;
    rule.protocolType = 1;  // HTTP
    rule.reportTemplate = "REPORT:{{data}}";
    mgr.addRule(rule);
    
    String result = mgr.applyReportTransform(1, "payload");
    TEST_ASSERT_EQUAL_STRING("REPORT:payload", result.c_str());
}

static void test_transform_disabled_rule() {
    resetManager();
    TestRuleScript rule;
    rule.id = "dis_001";
    rule.enabled = false;
    rule.protocolType = 0;
    rule.receiveTemplate = "SHOULD_NOT_APPLY:{{data}}";
    mgr.addRule(rule);
    
    // 禁用规则不应生效，返回原始数据
    String result = mgr.applyReceiveTransform(0, "original");
    TEST_ASSERT_EQUAL_STRING("original", result.c_str());
}

static void test_transform_protocol_mismatch() {
    resetManager();
    TestRuleScript rule;
    rule.id = "proto_001";
    rule.enabled = true;
    rule.protocolType = 0;  // MQTT
    rule.receiveTemplate = "MQTT:{{data}}";
    mgr.addRule(rule);
    
    // 用HTTP协议类型查找，不应匹配
    String result = mgr.applyReceiveTransform(1, "http_data");
    TEST_ASSERT_EQUAL_STRING("http_data", result.c_str());
}

static void test_multiple_rules() {
    resetManager();
    
    for (int i = 0; i < 10; i++) {
        TestRuleScript rule;
        rule.id = "batch_" + String(i);
        rule.name = "Rule " + String(i);
        rule.enabled = (i % 2 == 0);
        mgr.addRule(rule);
    }
    
    TEST_ASSERT_EQUAL(10, mgr.getRuleCount());
    
    // 删除奇数索引
    for (int i = 1; i < 10; i += 2) {
        mgr.removeRule("batch_" + String(i));
    }
    TEST_ASSERT_EQUAL(5, mgr.getRuleCount());
}

// ========== 测试组入口 ==========

void test_rule_script_group() {
    RUN_TEST(test_add_rule);
    RUN_TEST(test_add_rule_empty_id);
    RUN_TEST(test_add_duplicate_rule);
    RUN_TEST(test_update_rule);
    RUN_TEST(test_update_nonexistent);
    RUN_TEST(test_remove_rule);
    RUN_TEST(test_remove_nonexistent);
    RUN_TEST(test_enable_disable_rule);
    RUN_TEST(test_enable_nonexistent);
    RUN_TEST(test_template_apply_basic);
    RUN_TEST(test_template_apply_empty);
    RUN_TEST(test_receive_transform);
    RUN_TEST(test_report_transform);
    RUN_TEST(test_transform_disabled_rule);
    RUN_TEST(test_transform_protocol_mismatch);
    RUN_TEST(test_multiple_rules);
}
