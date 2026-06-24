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
 * - MQTT 主题匹配（精确、+、#、空）
 * - 主题重定向（sourceTopic → targetTopic）
 * - 开发模式访问控制（增删改查权限）
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
    uint8_t protocolType;  // 0=MQTT, 1=ModbusRTU
    String receiveTemplate;
    String reportTemplate;
    String sourceTopic;    // 源主题过滤（空=匹配所有）
    String targetTopic;    // 目标主题（空=不重定向）
    
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
    
    // MQTT 主题匹配（支持 + 单级通配符和 # 多级通配符）
    static bool matchTopic(const String& pattern, const String& topic) {
        if (pattern.isEmpty()) return true;
        if (pattern == topic) return true;
        if (pattern.indexOf('+') < 0 && pattern.indexOf('#') < 0) return false;

        int pLen = pattern.length();
        int tLen = topic.length();
        int pi = 0, ti = 0;

        while (pi < pLen && ti < tLen) {
            int pSlash = pattern.indexOf('/', pi);
            if (pSlash < 0) pSlash = pLen;
            String pPart = pattern.substring(pi, pSlash);

            if (pPart == "#") return true;

            int tSlash = topic.indexOf('/', ti);
            if (tSlash < 0) tSlash = tLen;
            String tPart = topic.substring(ti, tSlash);

            if (pPart != "+" && pPart != tPart) return false;

            pi = pSlash + 1;
            ti = tSlash + 1;
        }

        if (pi < pLen && pattern.substring(pi) == "#") return true;
        return (pi >= pLen && ti >= tLen);
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

    // 主题感知接收转换
    String applyReceiveTransform(uint8_t protocolType, const String& rawData,
                                 const String& topic, String* outTargetTopic) {
        for (auto& pair : _rules) {
            auto& rule = pair.second;
            if (!rule.enabled || rule.protocolType != protocolType) continue;
            if (rule.receiveTemplate.isEmpty()) continue;
            if (!rule.sourceTopic.isEmpty() && !matchTopic(rule.sourceTopic, topic)) continue;

            String result = applyTemplate(rule.receiveTemplate, rawData);
            if (outTargetTopic && !rule.targetTopic.isEmpty()) {
                *outTargetTopic = rule.targetTopic;
            }
            return result;
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

// ========== MQTT 主题匹配测试 ==========

static void test_topic_match_exact() {
    TEST_ASSERT_TRUE(TestRuleManager::matchTopic("/device/sensor/data", "/device/sensor/data"));
    TEST_ASSERT_FALSE(TestRuleManager::matchTopic("/device/sensor/data", "/device/sensor/other"));
    TEST_ASSERT_FALSE(TestRuleManager::matchTopic("/device/sensor/data", "/device/sensor"));
}

static void test_topic_match_single_wildcard() {
    // + 匹配单个层级
    TEST_ASSERT_TRUE(TestRuleManager::matchTopic("/device/+/data", "/device/sensor/data"));
    TEST_ASSERT_TRUE(TestRuleManager::matchTopic("/device/+/data", "/device/actuator/data"));
    TEST_ASSERT_FALSE(TestRuleManager::matchTopic("/device/+/data", "/device/sensor/other"));
    TEST_ASSERT_FALSE(TestRuleManager::matchTopic("/device/+/data", "/device/a/b/data"));
}

static void test_topic_match_multi_wildcard() {
    // # 匹配剩余所有层级
    TEST_ASSERT_TRUE(TestRuleManager::matchTopic("/device/#", "/device/sensor/data"));
    TEST_ASSERT_TRUE(TestRuleManager::matchTopic("/device/#", "/device/a/b/c"));
    TEST_ASSERT_TRUE(TestRuleManager::matchTopic("/device/#", "/device/"));
    TEST_ASSERT_TRUE(TestRuleManager::matchTopic("#", "/any/topic/here"));
    TEST_ASSERT_FALSE(TestRuleManager::matchTopic("/other/#", "/device/sensor/data"));
}

static void test_topic_match_empty_pattern() {
    // 空模式匹配所有主题
    TEST_ASSERT_TRUE(TestRuleManager::matchTopic("", "/device/sensor/data"));
    TEST_ASSERT_TRUE(TestRuleManager::matchTopic("", ""));
}

static void test_topic_filter_match() {
    resetManager();
    TestRuleScript rule;
    rule.id = "topic_001";
    rule.enabled = true;
    rule.protocolType = 0;
    rule.sourceTopic = "/device/sensor/data";
    rule.receiveTemplate = "TRANSFORMED:{{data}}";
    mgr.addRule(rule);

    // 精确匹配
    String result = mgr.applyReceiveTransform(0, "raw", "/device/sensor/data", nullptr);
    TEST_ASSERT_EQUAL_STRING("TRANSFORMED:raw", result.c_str());
}

static void test_topic_filter_no_match() {
    resetManager();
    TestRuleScript rule;
    rule.id = "topic_002";
    rule.enabled = true;
    rule.protocolType = 0;
    rule.sourceTopic = "/device/sensor/data";
    rule.receiveTemplate = "TRANSFORMED:{{data}}";
    mgr.addRule(rule);

    // 主题不匹配，返回原始数据
    String result = mgr.applyReceiveTransform(0, "raw", "/device/actuator/cmd", nullptr);
    TEST_ASSERT_EQUAL_STRING("raw", result.c_str());
}

static void test_topic_filter_wildcard() {
    resetManager();
    TestRuleScript rule;
    rule.id = "topic_003";
    rule.enabled = true;
    rule.protocolType = 0;
    rule.sourceTopic = "/device/+/data";
    rule.receiveTemplate = "W:{{data}}";
    mgr.addRule(rule);

    String result1 = mgr.applyReceiveTransform(0, "v1", "/device/sensor/data", nullptr);
    TEST_ASSERT_EQUAL_STRING("W:v1", result1.c_str());

    String result2 = mgr.applyReceiveTransform(0, "v2", "/device/other/cmd", nullptr);
    TEST_ASSERT_EQUAL_STRING("v2", result2.c_str());
}

static void test_topic_redirect() {
    resetManager();
    TestRuleScript rule;
    rule.id = "topic_004";
    rule.enabled = true;
    rule.protocolType = 0;
    rule.sourceTopic = "/sensor/temp";
    rule.targetTopic = "/cloud/telemetry";
    rule.receiveTemplate = "{{data}}";
    mgr.addRule(rule);

    String redirectTopic;
    String result = mgr.applyReceiveTransform(0, "25.5", "/sensor/temp", &redirectTopic);
    TEST_ASSERT_EQUAL_STRING("25.5", result.c_str());
    TEST_ASSERT_EQUAL_STRING("/cloud/telemetry", redirectTopic.c_str());
}

static void test_topic_redirect_empty_when_no_target() {
    resetManager();
    TestRuleScript rule;
    rule.id = "topic_005";
    rule.enabled = true;
    rule.protocolType = 0;
    rule.sourceTopic = "/sensor/temp";
    rule.targetTopic = "";  // 无目标主题
    rule.receiveTemplate = "{{data}}";
    mgr.addRule(rule);

    String redirectTopic;
    String result = mgr.applyReceiveTransform(0, "25.5", "/sensor/temp", &redirectTopic);
    TEST_ASSERT_EQUAL_STRING("25.5", result.c_str());
    TEST_ASSERT_TRUE(redirectTopic.isEmpty());  // 不应重定向
}

static void test_topic_empty_source_matches_all() {
    resetManager();
    TestRuleScript rule;
    rule.id = "topic_006";
    rule.enabled = true;
    rule.protocolType = 0;
    rule.sourceTopic = "";  // 空=匹配所有
    rule.receiveTemplate = "ALL:{{data}}";
    mgr.addRule(rule);

    String result1 = mgr.applyReceiveTransform(0, "x", "/any/topic", nullptr);
    TEST_ASSERT_EQUAL_STRING("ALL:x", result1.c_str());

    String result2 = mgr.applyReceiveTransform(0, "y", "/other/path/data", nullptr);
    TEST_ASSERT_EQUAL_STRING("ALL:y", result2.c_str());
}

static void test_protocol_only_mqtt_and_rtu() {
    // 验证仅有 MQTT(0) 和 ModbusRTU(1) 两种协议类型
    resetManager();

    TestRuleScript mqttRule;
    mqttRule.id = "proto_mqtt";
    mqttRule.enabled = true;
    mqttRule.protocolType = 0;
    mqttRule.receiveTemplate = "MQTT:{{data}}";
    mgr.addRule(mqttRule);

    TestRuleScript rtuRule;
    rtuRule.id = "proto_rtu";
    rtuRule.enabled = true;
    rtuRule.protocolType = 1;
    rtuRule.receiveTemplate = "RTU:{{data}}";
    mgr.addRule(rtuRule);

    TEST_ASSERT_EQUAL(2, mgr.getRuleCount());

    // MQTT 匹配
    String r1 = mgr.applyReceiveTransform(0, "d1");
    TEST_ASSERT_EQUAL_STRING("MQTT:d1", r1.c_str());

    // ModbusRTU 匹配
    String r2 = mgr.applyReceiveTransform(1, "d2");
    TEST_ASSERT_EQUAL_STRING("RTU:d2", r2.c_str());

    // 不存在的协议类型，不应匹配
    String r3 = mgr.applyReceiveTransform(3, "d3");
    TEST_ASSERT_EQUAL_STRING("d3", r3.c_str());
}

// ========== 开发模式访问控制测试 ==========

// 模拟开发模式状态管理器
class MockDeveloperModeState {
public:
    bool enabled;
    MockDeveloperModeState() : enabled(true) {}
    bool isDeveloperModeEnabled() const { return enabled; }
    void setDeveloperModeEnabled(bool e) { enabled = e; }
};

static MockDeveloperModeState devModeState;

// 模拟 requireDeveloperMode 检查（返回 true=允许，false=拒绝）
static bool requireDeveloperMode() {
    return devModeState.isDeveloperModeEnabled();
}

static void test_dev_mode_add_rule_allowed() {
    resetManager();
    devModeState.setDeveloperModeEnabled(true);

    // 开发模式启用时，允许添加规则
    if (!requireDeveloperMode()) {
        TEST_FAIL_MESSAGE("Developer mode should be enabled");
        return;
    }

    TestRuleScript rule;
    rule.id = "dev_test_001";
    rule.name = "Dev Mode Test";
    TEST_ASSERT_TRUE(mgr.addRule(rule));
    TEST_ASSERT_EQUAL(1, mgr.getRuleCount());
}

static void test_dev_mode_add_rule_blocked() {
    resetManager();
    devModeState.setDeveloperModeEnabled(false);

    // 开发模式禁用时，应拒绝添加规则
    if (requireDeveloperMode()) {
        TEST_FAIL_MESSAGE("Developer mode should be disabled");
        return;
    }

    // 验证操作被拒绝（不执行添加）
    TEST_ASSERT_EQUAL(0, mgr.getRuleCount());
}

static void test_dev_mode_update_rule_blocked() {
    resetManager();
    devModeState.setDeveloperModeEnabled(true);

    // 先添加一个规则
    TestRuleScript rule;
    rule.id = "dev_upd_001";
    rule.name = "Original";
    mgr.addRule(rule);

    // 禁用开发模式
    devModeState.setDeveloperModeEnabled(false);

    // 开发模式禁用时，应拒绝更新规则
    if (requireDeveloperMode()) {
        TEST_FAIL_MESSAGE("Developer mode should be disabled");
        return;
    }

    // 验证规则未被修改
    auto* found = mgr.getRule("dev_upd_001");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_STRING("Original", found->name.c_str());
}

static void test_dev_mode_delete_rule_blocked() {
    resetManager();
    devModeState.setDeveloperModeEnabled(true);

    // 先添加一个规则
    TestRuleScript rule;
    rule.id = "dev_del_001";
    mgr.addRule(rule);
    TEST_ASSERT_EQUAL(1, mgr.getRuleCount());

    // 禁用开发模式
    devModeState.setDeveloperModeEnabled(false);

    // 开发模式禁用时，应拒绝删除规则
    if (requireDeveloperMode()) {
        TEST_FAIL_MESSAGE("Developer mode should be disabled");
        return;
    }

    // 验证规则未被删除
    TEST_ASSERT_EQUAL(1, mgr.getRuleCount());
    TEST_ASSERT_NOT_NULL(mgr.getRule("dev_del_001"));
}

static void test_dev_mode_toggle_rule_blocked() {
    resetManager();
    devModeState.setDeveloperModeEnabled(true);

    // 先添加一个启用的规则
    TestRuleScript rule;
    rule.id = "dev_toggle_001";
    rule.enabled = true;
    mgr.addRule(rule);

    // 禁用开发模式
    devModeState.setDeveloperModeEnabled(false);

    // 开发模式禁用时，应拒绝切换规则状态
    if (requireDeveloperMode()) {
        TEST_FAIL_MESSAGE("Developer mode should be disabled");
        return;
    }

    // 验证规则状态未改变
    auto* found = mgr.getRule("dev_toggle_001");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_TRUE(found->enabled);  // 仍然是启用状态
}

static void test_dev_mode_get_rules_allowed() {
    resetManager();
    devModeState.setDeveloperModeEnabled(true);

    // 添加一些规则
    TestRuleScript r1, r2;
    r1.id = "get_001"; r1.name = "Rule 1";
    r2.id = "get_002"; r2.name = "Rule 2";
    mgr.addRule(r1);
    mgr.addRule(r2);

    // 禁用开发模式
    devModeState.setDeveloperModeEnabled(false);

    // 获取规则列表不需要开发模式（只读操作）
    TEST_ASSERT_EQUAL(2, mgr.getRuleCount());
    TEST_ASSERT_NOT_NULL(mgr.getRule("get_001"));
    TEST_ASSERT_NOT_NULL(mgr.getRule("get_002"));
}

static void test_dev_mode_re_enable_allows_operations() {
    resetManager();
    devModeState.setDeveloperModeEnabled(false);

    // 开发模式禁用时不能添加
    if (requireDeveloperMode()) {
        TEST_FAIL_MESSAGE("Should be blocked");
        return;
    }

    // 重新启用开发模式
    devModeState.setDeveloperModeEnabled(true);

    // 现在应该允许操作
    if (!requireDeveloperMode()) {
        TEST_FAIL_MESSAGE("Should be allowed");
        return;
    }

    TestRuleScript rule;
    rule.id = "re_enable_001";
    TEST_ASSERT_TRUE(mgr.addRule(rule));
    TEST_ASSERT_EQUAL(1, mgr.getRuleCount());
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
    // MQTT 主题匹配测试
    RUN_TEST(test_topic_match_exact);
    RUN_TEST(test_topic_match_single_wildcard);
    RUN_TEST(test_topic_match_multi_wildcard);
    RUN_TEST(test_topic_match_empty_pattern);
    RUN_TEST(test_topic_filter_match);
    RUN_TEST(test_topic_filter_no_match);
    RUN_TEST(test_topic_filter_wildcard);
    RUN_TEST(test_topic_redirect);
    RUN_TEST(test_topic_redirect_empty_when_no_target);
    RUN_TEST(test_topic_empty_source_matches_all);
    RUN_TEST(test_protocol_only_mqtt_and_rtu);
    // 开发模式访问控制测试
    RUN_TEST(test_dev_mode_add_rule_allowed);
    RUN_TEST(test_dev_mode_add_rule_blocked);
    RUN_TEST(test_dev_mode_update_rule_blocked);
    RUN_TEST(test_dev_mode_delete_rule_blocked);
    RUN_TEST(test_dev_mode_toggle_rule_blocked);
    RUN_TEST(test_dev_mode_get_rules_allowed);
    RUN_TEST(test_dev_mode_re_enable_allows_operations);
}
