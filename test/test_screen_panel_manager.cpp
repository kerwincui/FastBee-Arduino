#include <unity.h>

#ifdef UNIT_TEST

#define FASTBEE_ENABLE_POINT_HISTORY 0
#define FASTBEE_ENABLE_REMOTE_RESOURCES 0
#define FASTBEE_ENABLE_SCREEN_PANEL 1

#include "helpers/ScreenPanelNativeDependencies.h"

// Native tests do not build src/. Compile the implementation in this one test
// translation unit so the public validator is exercised rather than mirrored.
#include "../src/network/FixedJsonField.cpp"
#include "../src/core/ScreenPanelManager.cpp"

namespace {

JsonDocument baseConfig() {
    JsonDocument doc;
    const char* json = R"json({
        "schemaVersion":1,
        "displayRef":null,
        "settings":{
            "brightnessPercent":70,
            "idleSleepSeconds":120,
            "homePageId":"home",
            "themeId":"dark",
            "background":{"presetId":"solid"},
            "allowSystemRestart":false
        },
        "pages":[{
            "id":"home",
            "title":"Home",
            "layoutId":"mixed-grid",
            "layoutMode":"canvas",
            "logicalWidth":480,
            "logicalHeight":480,
            "rotation":0,
            "cards":[]
        }]
    })json";
    TEST_ASSERT_FALSE(static_cast<bool>(deserializeJson(doc, json)));
    return doc;
}

JsonObject addCard(JsonDocument& config, const char* id, const char* kind,
                   const char* visual, int x, int y, int w, int h) {
    JsonObject card = config["pages"][0]["cards"].add<JsonObject>();
    card["id"] = id;
    card["kind"] = kind;
    card["visualType"] = visual;
    card["title"] = id;
    card["rect"]["x"] = x;
    card["rect"]["y"] = y;
    card["rect"]["w"] = w;
    card["rect"]["h"] = h;
    return card;
}

void test_screen_panel_normalizes_a_valid_bounded_config() {
    JsonDocument config = baseConfig();
    JsonObject card = addCard(config, "temperature", "data", "value",
                              0, 0, 160, 80);
    card["sourceRef"] = "sensor.room.temperature";

    JsonDocument normalized;
    String error;
    TEST_ASSERT_TRUE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));
    TEST_ASSERT_TRUE(error.isEmpty());
    TEST_ASSERT_EQUAL_UINT32(70, normalized["settings"]["brightnessPercent"]);
    TEST_ASSERT_TRUE(normalized["settings"]["touchBeepEnabled"].as<bool>());
    TEST_ASSERT_EQUAL_UINT32(60, normalized["settings"]["controlConfirmIdleSeconds"]);
    TEST_ASSERT_EQUAL_STRING(
        "value", normalized["pages"][0]["cards"][0]["visualType"]);
}

void test_screen_panel_touch_preferences_and_widgets_are_bounded() {
    JsonDocument config = baseConfig(), normalized;
    String error;
    config["settings"]["touchBeepEnabled"] = false;
    for (int value : {15, 60, 600}) {
        config["settings"]["controlConfirmIdleSeconds"] = value;
        TEST_ASSERT_TRUE(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), normalized, error));
        TEST_ASSERT_EQUAL_INT(value, normalized["settings"]["controlConfirmIdleSeconds"].as<int>());
        TEST_ASSERT_FALSE(normalized["settings"]["touchBeepEnabled"].as<bool>());
    }
    for (int value : {-1, 0, 14, 601}) {
        config["settings"]["controlConfirmIdleSeconds"] = value;
        TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), normalized, error));
    }
    config["settings"]["controlConfirmIdleSeconds"] = 60;
    config["settings"]["touchBeepEnabled"] = 1;
    TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), normalized, error));
    config["settings"]["touchBeepEnabled"] = true;
    JsonObject card = addCard(config, "beep", "system", "switch", 16, 16, 448, 80);
    card["systemActionId"] = "touch-beep";
    TEST_ASSERT_TRUE(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), normalized, error));
    card["visualType"] = "action";
    TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), normalized, error));
    card["systemActionId"] = "confirm-idle"; card["visualType"] = "select";
    TEST_ASSERT_TRUE(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), normalized, error));
    card["visualType"] = "switch";
    TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), normalized, error));
    TEST_ASSERT_FALSE(ScreenPanelManager::instance().queueSetting("touchBeepEnabled", 2, error));
    TEST_ASSERT_EQUAL_STRING("Invalid screen setting", error.c_str());
    TEST_ASSERT_FALSE(ScreenPanelManager::instance().queueSetting("controlConfirmIdleSeconds", 14, error));
    TEST_ASSERT_EQUAL_STRING("Invalid screen setting", error.c_str());
    config = baseConfig();
    JsonObject motor = addCard(config, "motor", "control", "motor", 16, 16, 448, 400);
    motor["controlRef"] = "motor.test"; motor["requireConfirmation"] = false;
    TEST_ASSERT_TRUE(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), normalized, error));
    TEST_ASSERT_FALSE(normalized["pages"][0]["cards"][0]["requireConfirmation"].as<bool>());
    motor["requireConfirmation"] = true;
    TEST_ASSERT_TRUE(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), normalized, error));
    TEST_ASSERT_TRUE(normalized["pages"][0]["cards"][0]["requireConfirmation"].as<bool>());
    motor["requireConfirmation"] = "false";
    TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), normalized, error));
    TEST_ASSERT_TRUE(error.indexOf("requireConfirmation must be boolean") >= 0);
}

void test_screen_panel_rejects_unknown_fields_and_page_overflow() {
    JsonDocument config = baseConfig();
    config["unexpected"] = true;
    JsonDocument normalized;
    String error;
    TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));
    TEST_ASSERT_TRUE(error.indexOf("unknown field") >= 0);

    config = baseConfig();
    JsonArray pages = config["pages"].as<JsonArray>();
    for (int i = 1; i < 8; ++i) {
        JsonObject page = pages.add<JsonObject>();
        page["id"] = String("page-") + i;
        page["title"] = "Page";
        page["cards"].to<JsonArray>();
    }
    TEST_ASSERT_TRUE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));
    TEST_ASSERT_EQUAL_UINT32(8, normalized["pages"].size());
    JsonObject extra = pages.add<JsonObject>();
    extra["id"] = "page-overflow";
    extra["title"] = "Overflow";
    extra["cards"].to<JsonArray>();
    TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));
    TEST_ASSERT_TRUE(error.indexOf("1 to 8 pages") >= 0);
}

void test_screen_panel_allows_inert_templates_but_rejects_bad_references() {
    JsonDocument config = baseConfig();
    auto weather = addCard(config, "weather", "weather", "weather", 0, 0, 220, 120);
    weather["sourceRef"] = "";
    auto action = addCard(config, "action", "control", "action", 0, 140, 220, 80);
    action["controlRef"] = "";
    auto toggle = addCard(config, "switch", "control", "switch", 0, 240, 220, 80);
    toggle["controlRef"] = "";
    JsonDocument normalized;
    String error;
    TEST_ASSERT_TRUE(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), normalized, error));
    weather["sourceRef"] = "https://not-a-source";
    TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), normalized, error));
    weather["sourceRef"] = "";
    action["controlRef"] = 123;
    TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), normalized, error));
    action["controlRef"] = "";
    weather["kind"] = "data";
    weather["visualType"] = "value";
    TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), normalized, error));
}

void test_screen_panel_rejects_overlap_and_too_many_series() {
    JsonDocument config = baseConfig();
    addCard(config, "clock-a", "clock", "clock", 0, 0, 100, 100);
    addCard(config, "clock-b", "clock", "clock", 50, 50, 100, 100);
    JsonDocument normalized;
    String error;
    TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));
    TEST_ASSERT_TRUE(error.indexOf("overlaps") >= 0);

    config = baseConfig();
    JsonObject chart = addCard(config, "bars", "chart", "bar",
                               0, 0, 300, 180);
    chart["sourceRef"] = "source.primary";
    JsonArray sources = chart["sourceRefs"].to<JsonArray>();
    for (int i = 0; i < 5; ++i) sources.add(String("source.") + i);
    TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));
    TEST_ASSERT_TRUE(error.indexOf("at most 4") >= 0);
}

void test_screen_panel_restart_requires_permission_and_confirmation() {
    JsonDocument config = baseConfig();
    JsonObject restart = addCard(config, "restart", "system", "action",
                                 0, 0, 120, 64);
    restart["systemActionId"] = "restart";
    restart["requireConfirmation"] = true;
    JsonDocument normalized;
    String error;
    TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));

    config["settings"]["allowSystemRestart"] = true;
    TEST_ASSERT_TRUE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));
    config["pages"][0]["cards"][0]["requireConfirmation"] = false;
    TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));
}

void test_screen_panel_validates_fixed_system_actions_and_brightness_value() {
    JsonDocument config = baseConfig();
    JsonObject brightness = addCard(config, "brightness", "system", "action",
                                    0, 0, 120, 64);
    brightness["systemActionId"] = "brightness";
    JsonDocument normalized;
    String error;
    TEST_ASSERT_TRUE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));
    TEST_ASSERT_EQUAL_INT(70,
        normalized["pages"][0]["cards"][0]["actionValue"].as<int>());

    brightness["actionValue"] = 1;
    TEST_ASSERT_TRUE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));
    brightness["actionValue"] = 100;
    TEST_ASSERT_TRUE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));
    brightness["actionValue"] = 0;
    TEST_ASSERT_TRUE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));
    brightness["actionValue"] = 101;
    TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));

    config = baseConfig();
    JsonObject sleep = addCard(config, "sleep", "system", "action",
                               0, 0, 120, 64);
    sleep["systemActionId"] = "sleep";
    TEST_ASSERT_TRUE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));
    sleep["actionValue"] = 70;
    TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));
    sleep.remove("actionValue");
    sleep["systemActionId"] = "buzzer";
    TEST_ASSERT_TRUE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));
    sleep["systemActionId"] = "wifi-restart";
    TEST_ASSERT_FALSE(ScreenPanelManager::validateConfig(
        config.as<JsonVariantConst>(), normalized, error));
}

void test_screen_panel_received_input_requires_actual_boolean_capability() {
    JsonDocument choices;
    auto controls = choices.to<JsonArray>();
    auto remoteSwitch = controls.add<JsonObject>();
    remoteSwitch["controlRef"] = "remote.lamp";
    remoteSwitch["type"] = "switch";
    auto scene = controls.add<JsonObject>();
    scene["controlRef"] = "remote.scene";
    scene["type"] = "action";
    auto remoteNumber = controls.add<JsonObject>();
    remoteNumber["controlRef"] = "remote.level";
    remoteNumber["type"] = "number";
    remoteNumber["writable"] = true;
    PeriphExecRule rule;
    rule.allowPanelControl = true;
    ExecAction action;
    action.actionType = static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_REG_WRITE);
    action.actionValue = "5:100";
    rule.actions.push_back(action);
    String error;
    TEST_ASSERT_TRUE(panelRuleInputKind(rule, controls) == PanelRuleInputKind::TRIGGER);
    TEST_ASSERT_TRUE(validatePanelControlInput(panelRuleInputKind(rule, controls), "action", error));
    TEST_ASSERT_FALSE(validatePanelControlInput(panelRuleInputKind(rule, controls), "switch", error));
    for (auto type : {ExecActionType::ACTION_MODBUS_COIL_WRITE,
                      ExecActionType::ACTION_STEPPER_CONTROL,
                      ExecActionType::ACTION_HIGH}) {
        rule.actions[0].actionType = static_cast<uint8_t>(type);
        rule.actions[0].useReceivedValue = true;
        TEST_ASSERT_TRUE(panelRuleInputKind(rule, controls) == PanelRuleInputKind::UNSUPPORTED);
        TEST_ASSERT_FALSE(validatePanelControlInput(panelRuleInputKind(rule, controls), "switch", error));
        TEST_ASSERT_FALSE(validatePanelControlInput(panelRuleInputKind(rule, controls), "action", error));
        TEST_ASSERT_TRUE(error.indexOf("fixed-value") >= 0);
    }
    PanelNumericConstraints numeric;
    rule.actions[0].actionType = static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_REG_WRITE);
    rule.actions[0].actionValue = "5:$value";
    TEST_ASSERT_TRUE(panelRuleInputKind(rule, controls, &numeric) == PanelRuleInputKind::NUMBER);
    TEST_ASSERT_TRUE(numeric.bounded && numeric.integerOnly);
    TEST_ASSERT_TRUE(numeric.minimum == 0.0);
    TEST_ASSERT_TRUE(numeric.maximum == 65535.0);
    TEST_ASSERT_TRUE(validatePanelControlInput(PanelRuleInputKind::NUMBER, "number", error));
    rule.actions[0].actionValue = "5:100";
    TEST_ASSERT_TRUE(panelRuleInputKind(rule, controls) == PanelRuleInputKind::UNSUPPORTED);

    for (auto type : {ExecActionType::ACTION_BLINK, ExecActionType::ACTION_BREATHE,
                      ExecActionType::ACTION_SET_PWM, ExecActionType::ACTION_SET_DAC,
                      ExecActionType::ACTION_DISPLAY_NUMBER}) {
        rule.actions[0].actionType = static_cast<uint8_t>(type);
        rule.actions[0].actionValue = "$value";
        TEST_ASSERT_TRUE(panelRuleInputKind(rule, controls) == PanelRuleInputKind::NUMBER);
    }
    rule.actions[0].actionType = static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_CHANNEL);
    rule.actions[0].actionValue = "";
    rule.actions[0].targetPeriphId = "lamp";
    TEST_ASSERT_TRUE(panelRuleInputKind(rule, controls) == PanelRuleInputKind::BOOLEAN);
    TEST_ASSERT_TRUE(validatePanelControlInput(panelRuleInputKind(rule, controls), "switch", error));
    TEST_ASSERT_FALSE(validatePanelControlInput(panelRuleInputKind(rule, controls), "action", error));
    rule.actions[0].targetPeriphId = "remote.lamp";
    TEST_ASSERT_TRUE(panelRuleInputKind(rule, controls) == PanelRuleInputKind::BOOLEAN);
    for (const char* target : {"scene", "missing"}) {
        rule.actions[0].targetPeriphId = target;
        TEST_ASSERT_TRUE(panelRuleInputKind(rule, controls) == PanelRuleInputKind::UNSUPPORTED);
    }
    rule.actions[0].targetPeriphId = "level";
    TEST_ASSERT_TRUE(panelRuleInputKind(rule, controls) == PanelRuleInputKind::NUMBER);
    remoteNumber["writable"] = false;
    TEST_ASSERT_TRUE(panelRuleInputKind(rule, controls) == PanelRuleInputKind::UNSUPPORTED);
    remoteNumber["writable"] = true;
    rule.actions[0].useReceivedValue = false;
    TEST_ASSERT_TRUE(panelRuleInputKind(rule, controls) == PanelRuleInputKind::TRIGGER);
    rule.actions[0].useReceivedValue = true;
    rule.actions[0].targetPeriphId = "lamp";
    // A boolean action must not hide another action that expects a numeric value.
    action.actionValue = "5:$value";
    action.useReceivedValue = true;
    rule.actions.push_back(action);
    TEST_ASSERT_TRUE(panelRuleInputKind(rule, controls) == PanelRuleInputKind::UNSUPPORTED);
}

void test_screen_panel_dynamic_refresh_clears_old_payload_without_changing_layout() {
    ScreenPanelCardSnapshot output{};
    snprintf(output.id, sizeof(output.id), "%s", "temperature");
    snprintf(output.title, sizeof(output.title), "%s", "Room");
    output.rect = {10, 20, 120, 80};
    output.kind = ScreenCardKind::CHART;
    output.visualType = ScreenVisualType::LINE;
    output.hasRange = true;
    output.rangeMin = -20;
    output.rangeMax = 80;
    output.interactive = true;
    output.requiresConfirmation = true;
    snprintf(output.value, sizeof(output.value), "%s", "123.45");
    snprintf(output.error, sizeof(output.error), "%s", "previous error");
    output.historyCount = 2;
    output.history[1].value = 99;
    output.historyStale = true;
    output.pending = true;
    output.operationId = 42;

    ScreenPanelCardSnapshot fresh{};
    fresh.quality = ScreenValueQuality::UNKNOWN;
    snprintf(fresh.unit, sizeof(fresh.unit), "%s", "C");
    updatePanelDynamicFields(output, fresh);
    TEST_ASSERT_EQUAL_STRING("temperature", output.id);
    TEST_ASSERT_EQUAL_STRING("Room", output.title);
    TEST_ASSERT_EQUAL_UINT16(10, output.rect.x);
    TEST_ASSERT_TRUE(output.kind == ScreenCardKind::CHART);
    TEST_ASSERT_TRUE(output.visualType == ScreenVisualType::LINE);
    TEST_ASSERT_TRUE(output.hasRange && output.interactive && output.requiresConfirmation);
    TEST_ASSERT_EQUAL_FLOAT(-20, output.rangeMin);
    TEST_ASSERT_EQUAL_FLOAT(80, output.rangeMax);
    TEST_ASSERT_EQUAL_STRING("C", output.unit);
    TEST_ASSERT_EQUAL_STRING("", output.value);
    TEST_ASSERT_EQUAL_STRING("", output.error);
    TEST_ASSERT_FALSE(output.pending || output.historyStale);
    TEST_ASSERT_EQUAL_UINT32(0, output.operationId);
    TEST_ASSERT_EQUAL_UINT8(0, output.historyCount);
    TEST_ASSERT_EQUAL_FLOAT(0, output.history[1].value);
}

void test_screen_panel_reports_only_live_system_action_capabilities() {
    allowPanelNativeRuntimeDependencies() = true;
    auto& caps = panelNativeSystemActionCapabilities();
    caps = {};
    caps.displayReady = caps.touchConfigured = caps.touchReady = true;
    caps.brightness = caps.sleep = caps.restart = true;

    JsonDocument capabilityDocument;
    ScreenPanelManager::instance().appendCapabilities(
        capabilityDocument.to<JsonObject>());
    TEST_ASSERT_TRUE(capabilityDocument["brightness"]);
    TEST_ASSERT_TRUE(capabilityDocument["sleep"]);
    TEST_ASSERT_FALSE(capabilityDocument["buzzer"]);
    TEST_ASSERT_TRUE(capabilityDocument["systemRestart"]);
    JsonArrayConst actions = capabilityDocument["systemActions"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL_UINT32(5, actions.size());
    TEST_ASSERT_EQUAL_STRING("brightness", actions[0]);
    TEST_ASSERT_EQUAL_STRING("sleep", actions[1]);
    TEST_ASSERT_EQUAL_STRING("sleep-time", actions[2]);
    TEST_ASSERT_EQUAL_STRING("confirm-idle", actions[3]);
    TEST_ASSERT_EQUAL_STRING("restart", actions[4]);

    panelNativeSystemActionCapabilities() = {};
    capabilityDocument.clear();
    ScreenPanelManager::instance().appendCapabilities(
        capabilityDocument.to<JsonObject>());
    TEST_ASSERT_FALSE(capabilityDocument["brightness"]);
    TEST_ASSERT_FALSE(capabilityDocument["sleep"]);
    TEST_ASSERT_FALSE(capabilityDocument["buzzer"]);
    TEST_ASSERT_FALSE(capabilityDocument["systemRestart"]);
    TEST_ASSERT_FALSE(capabilityDocument["touchReady"]);
    TEST_ASSERT_TRUE(capabilityDocument["disabledReasons"]["systemRestart"].is<const char*>());
    TEST_ASSERT_EQUAL_UINT32(
        1, capabilityDocument["systemActions"].as<JsonArrayConst>().size());
    TEST_ASSERT_EQUAL_STRING("confirm-idle", capabilityDocument["systemActions"][0]);
    allowPanelNativeRuntimeDependencies() = false;
}

}  // namespace

#endif  // UNIT_TEST

void test_screen_panel_manager_group() {
#ifdef UNIT_TEST
    RUN_TEST(test_screen_panel_normalizes_a_valid_bounded_config);
    RUN_TEST(test_screen_panel_touch_preferences_and_widgets_are_bounded);
    RUN_TEST(test_screen_panel_allows_inert_templates_but_rejects_bad_references);
    RUN_TEST(test_screen_panel_rejects_unknown_fields_and_page_overflow);
    RUN_TEST(test_screen_panel_rejects_overlap_and_too_many_series);
    RUN_TEST(test_screen_panel_restart_requires_permission_and_confirmation);
    RUN_TEST(test_screen_panel_validates_fixed_system_actions_and_brightness_value);
    RUN_TEST(test_screen_panel_received_input_requires_actual_boolean_capability);
    RUN_TEST(test_screen_panel_dynamic_refresh_clears_old_payload_without_changing_layout);
    RUN_TEST(test_screen_panel_reports_only_live_system_action_capabilities);
#endif
}
