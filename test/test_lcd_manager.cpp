/**
 * @file test_lcd_manager.cpp
 * @brief LCD/OLED 显示屏管理器单元测试
 * 
 * 测试内容（纯逻辑，不依赖硬件）：
 * - 分辨率 → 最大行数计算（128x64, 128x32, 72x40, 64x128）
 * - LCD 配置参数 JSON 序列化/反序列化
 * - 自定义文本行数截断逻辑（不同分辨率）
 * - OLED 显示动作类型配置（外设执行规则）
 * - ESP32-C3 引脚校验逻辑
 */

#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include "mocks/MockPeripheral.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"

void test_lcd_manager_group();

// ========== 镜像 LCDManager 行计算逻辑 ==========

/**
 * @brief 镜像 LCDManager::getFontHeight()
 * 
 * 字体索引 0 = 8px, 1 = 13px(WQY12), 2 = 16px(WQY15)
 */
static uint8_t mirrorGetFontHeight(uint8_t fontIndex) {
    switch (fontIndex) {
        case 0: return 8;
        case 1: return 13;
        case 2: return 16;
        default: return 13;
    }
}

/**
 * @brief 镜像 LCDManager::getMaxLines()
 */
static uint8_t mirrorGetMaxLines(uint8_t screenHeight, uint8_t fontIndex) {
    uint8_t fontH = mirrorGetFontHeight(fontIndex);
    if (fontH == 0) return 0;
    return screenHeight / fontH;
}

/**
 * @brief 镜像 showCustomText 行数截断逻辑
 * 
 * 返回实际可显示行数（受屏高和硬上限 MAX_CUSTOM_LINES=6 约束）
 */
static uint8_t mirrorGetMaxCustomLines(uint8_t screenHeight, uint8_t fontIndex) {
    static constexpr uint8_t MAX_CUSTOM_LINES = 6;
    uint8_t maxPixelLines = mirrorGetMaxLines(screenHeight, fontIndex);
    if (maxPixelLines == 0) maxPixelLines = 4;
    return (maxPixelLines < MAX_CUSTOM_LINES) ? maxPixelLines : MAX_CUSTOM_LINES;
}

/**
 * @brief 镜像 createDisplay 的分辨率匹配逻辑
 * 
 * 返回匹配结果：0=128x64 SSD1306, 1=128x32 SSD1306, 2=72x40 SSD1306,
 *                3=64x128 SH1106, 99=默认回退(128x64)
 */
static int mirrorResolutionMatch(uint8_t width, uint8_t height) {
    if (width == 128 && height == 64) return 0;
    if (width == 128 && height == 32) return 1;
    if (width == 72  && height == 40) return 2;
    if (width == 64  && height == 128) return 3;
    return 99; // fallback to 128x64
}

// ========== ESP32-C3 引脚校验镜像 ==========

static bool isValidGpioC3(int pin) {
    return (pin >= 0 && pin <= 21);
}

static bool validateI2CPinsC3(int sda, int scl) {
    return isValidGpioC3(sda) && isValidGpioC3(scl);
}

// ============================================================
//  TEST GROUP 1: 分辨率 → 最大行数计算
// ============================================================

static void test_lcd_max_lines_128x64_default_font() {
    // 128x64 + WQY12(font=1, h=13): 64/13 = 4 行
    uint8_t lines = mirrorGetMaxLines(64, 1);
    TEST_ASSERT_EQUAL(4, lines);
}

static void test_lcd_max_lines_128x32_default_font() {
    // 128x32 + WQY12(font=1, h=13): 32/13 = 2 行
    uint8_t lines = mirrorGetMaxLines(32, 1);
    TEST_ASSERT_EQUAL(2, lines);
}

static void test_lcd_max_lines_72x40_default_font() {
    // 72x40 + WQY12(font=1, h=13): 40/13 = 3 行
    uint8_t lines = mirrorGetMaxLines(40, 1);
    TEST_ASSERT_EQUAL(3, lines);
}

static void test_lcd_max_lines_64x128_default_font() {
    // 64x128 (SH1106 竖屏) + WQY12(font=1, h=13): 128/13 = 9 行
    uint8_t lines = mirrorGetMaxLines(128, 1);
    TEST_ASSERT_EQUAL(9, lines);
}

static void test_lcd_max_lines_small_font() {
    // 128x64 + small(font=0, h=8): 64/8 = 8 行
    uint8_t lines = mirrorGetMaxLines(64, 0);
    TEST_ASSERT_EQUAL(8, lines);
}

static void test_lcd_max_lines_large_font_72x40() {
    // 72x40 + large(font=2, h=16): 40/16 = 2 行
    uint8_t lines = mirrorGetMaxLines(40, 2);
    TEST_ASSERT_EQUAL(2, lines);
}

// ============================================================
//  TEST GROUP 2: 分辨率匹配逻辑
// ============================================================

static void test_lcd_match_128x64_ssd1306() {
    TEST_ASSERT_EQUAL(0, mirrorResolutionMatch(128, 64));
}

static void test_lcd_match_128x32_ssd1306() {
    TEST_ASSERT_EQUAL(1, mirrorResolutionMatch(128, 32));
}

static void test_lcd_match_72x40_ssd1306_c3() {
    // ESP32-C3 OLED 开发板 72x40
    TEST_ASSERT_EQUAL(2, mirrorResolutionMatch(72, 40));
}

static void test_lcd_match_64x128_sh1106() {
    TEST_ASSERT_EQUAL(3, mirrorResolutionMatch(64, 128));
}

static void test_lcd_match_unknown_fallback() {
    // 未知分辨率应回退到默认 128x64
    TEST_ASSERT_EQUAL(99, mirrorResolutionMatch(100, 50));
    TEST_ASSERT_EQUAL(99, mirrorResolutionMatch(0, 0));
}

// ============================================================
//  TEST GROUP 3: 自定义文本行数截断
// ============================================================

static void test_custom_lines_128x64_max_4_plus_title() {
    // 128x64 默认字体最多 4 行，硬上限 6，实际取 min(4,6)=4
    uint8_t maxLines = mirrorGetMaxCustomLines(64, 1);
    TEST_ASSERT_EQUAL(4, maxLines);
}

static void test_custom_lines_128x32_max_2() {
    // 128x32 默认字体最多 2 行
    uint8_t maxLines = mirrorGetMaxCustomLines(32, 1);
    TEST_ASSERT_EQUAL(2, maxLines);
}

static void test_custom_lines_72x40_max_3() {
    // 72x40 默认字体最多 3 行
    uint8_t maxLines = mirrorGetMaxCustomLines(40, 1);
    TEST_ASSERT_EQUAL(3, maxLines);
}

static void test_custom_lines_64x128_capped_at_6() {
    // 64x128 默认字体最多 9 行，但硬上限 6，取 min(9,6)=6
    uint8_t maxLines = mirrorGetMaxCustomLines(128, 1);
    TEST_ASSERT_EQUAL(6, maxLines);
}

static void test_custom_lines_72x40_large_font_max_2() {
    // 72x40 大字体(h=16) 最多 2 行
    uint8_t maxLines = mirrorGetMaxCustomLines(40, 2);
    TEST_ASSERT_EQUAL(2, maxLines);
}

// ============================================================
//  TEST GROUP 4: LCD 配置参数 JSON 序列化/反序列化
// ============================================================

static void test_lcd_config_json_serialize_128x64() {
    JsonDocument doc;
    doc["width"] = 128;
    doc["height"] = 64;
    doc["interface"] = 2; // I2C
    
    int w = doc["width"] | 128;
    int h = doc["height"] | 64;
    int iface = doc["interface"] | 2;
    
    TEST_ASSERT_EQUAL(128, w);
    TEST_ASSERT_EQUAL(64, h);
    TEST_ASSERT_EQUAL(2, iface);
}

static void test_lcd_config_json_serialize_72x40() {
    JsonDocument doc;
    doc["width"] = 72;
    doc["height"] = 40;
    doc["interface"] = 2;
    
    int w = doc["width"] | 128;
    int h = doc["height"] | 64;
    int iface = doc["interface"] | 2;
    
    TEST_ASSERT_EQUAL(72, w);
    TEST_ASSERT_EQUAL(40, h);
    TEST_ASSERT_EQUAL(2, iface);
}

static void test_lcd_config_json_defaults() {
    // 空 JSON 应使用默认值 128x64 I2C
    JsonDocument doc;
    
    int w = doc["width"] | 128;
    int h = doc["height"] | 64;
    int iface = doc["interface"] | 2;
    
    TEST_ASSERT_EQUAL(128, w);
    TEST_ASSERT_EQUAL(64, h);
    TEST_ASSERT_EQUAL(2, iface);
}

static void test_lcd_config_json_clamp_overflow() {
    // 镜像 PeripheralManager 中的截断逻辑
    JsonDocument doc;
    doc["width"] = 300;   // > 255
    doc["height"] = -10;  // < 0
    
    int w = doc["width"] | 128;
    int h = doc["height"] | 64;
    
    uint8_t clampedW = (uint8_t)(w > 255 ? 255 : (w < 0 ? 0 : w));
    uint8_t clampedH = (uint8_t)(h > 255 ? 255 : (h < 0 ? 0 : h));
    
    TEST_ASSERT_EQUAL(255, clampedW);
    TEST_ASSERT_EQUAL(0, clampedH);
}

static void test_lcd_config_json_spi_interface() {
    JsonDocument doc;
    doc["width"] = 128;
    doc["height"] = 64;
    doc["interface"] = 1; // SPI
    
    int iface = doc["interface"] | 2;
    TEST_ASSERT_EQUAL(1, iface);
}

// ============================================================
//  TEST GROUP 5: ESP32-C3 I2C 引脚校验
// ============================================================

static void test_c3_i2c_pins_valid_5_6() {
    // ESP32-C3 OLED 标准接线：SDA=5, SCL=6
    TEST_ASSERT_TRUE(validateI2CPinsC3(5, 6));
}

static void test_c3_i2c_pins_valid_boundary() {
    // GPIO 0 和 21 均合法
    TEST_ASSERT_TRUE(validateI2CPinsC3(0, 21));
    TEST_ASSERT_TRUE(validateI2CPinsC3(21, 0));
}

static void test_c3_i2c_pins_invalid_22() {
    // GPIO 22 在 C3 上不合法（有效范围 0-21）
    TEST_ASSERT_FALSE(validateI2CPinsC3(5, 22));
    TEST_ASSERT_FALSE(validateI2CPinsC3(22, 6));
}

static void test_c3_i2c_pins_invalid_negative() {
    TEST_ASSERT_FALSE(validateI2CPinsC3(-1, 6));
    TEST_ASSERT_FALSE(validateI2CPinsC3(5, -1));
}

// ============================================================
//  TEST GROUP 6: OLED 外设执行动作配置
// ============================================================

static void test_oled_exec_action_type_exists() {
    // 验证 ActionType 包含 OLED_DISPLAY（镜像自定义显示动作）
    // ActionType::OLED_DISPLAY 在真实代码中的值
    // 这里通过 MockPeriphExecManager 验证规则可添加
    MockPeriphExecManager& mgr = MockPeriphExecManager::getInstance();
    mgr.initialize();
    
    PeriphExecRule rule;
    rule.id = "oled_display_rule";
    rule.name = "OLED Show Temp";
    rule.enabled = true;
    rule.triggerType = TriggerType::TIMER_SCHEDULE;
    rule.actionType = ActionType::OLED_DISPLAY;  // OLED 自定义显示
    rule.actionValue = "# Env\nTemp:${dht_01.temperature}";
    rule.targetPeriphId = "oled_display";
    
    TEST_ASSERT_TRUE(mgr.addRule(rule));
    
    PeriphExecRule* fetched = mgr.getRule("oled_display_rule");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL(ActionType::OLED_DISPLAY, fetched->actionType);
    TEST_ASSERT_TRUE(fetched->actionValue.indexOf("temperature") >= 0);
}

static void test_oled_exec_multiline_content() {
    // 验证多行 OLED 内容解析
    String content = "# 环境监测\n温度:25.5°C\n湿度:60%";
    
    // 统计 \n 分隔的行数
    int lineCount = 1;
    for (int i = 0; i < (int)content.length(); i++) {
        if (content[i] == '\n') lineCount++;
    }
    
    TEST_ASSERT_EQUAL(3, lineCount);
    
    // 首行以 # 开头 → 标题行
    int firstNewline = content.indexOf('\n');
    String firstLine = content.substring(0, firstNewline);
    TEST_ASSERT_TRUE(firstLine.startsWith("#"));
}

static void test_oled_exec_content_truncation_72x40() {
    // 72x40 屏最多 3 行（默认字体），超出部分应被截断
    String content = "# Title\nLine1\nLine2\nLine3\nLine4";
    uint8_t maxLines = mirrorGetMaxCustomLines(40, 1); // = 3
    
    // 统计实际行数
    int totalLines = 1;
    for (int i = 0; i < (int)content.length(); i++) {
        if (content[i] == '\n') totalLines++;
    }
    TEST_ASSERT_EQUAL(5, totalLines);  // 5 行内容
    
    // 截断后应只保留 maxLines 行
    TEST_ASSERT_EQUAL(3, maxLines);
    TEST_ASSERT_TRUE(totalLines > maxLines);  // 需要截断
}

// ============================================================
//  TEST GROUP 7: 传感器分页显示计算
// ============================================================

static void test_sensor_page_count_128x64() {
    // 128x64: maxLines=4, 每页显示 3 条（预留标题行）
    uint8_t linesPerPage = mirrorGetMaxLines(64, 1) - 1;  // = 3
    
    // 5 条传感器数据 → 2 页
    uint8_t entryCount = 5;
    uint8_t pages = (entryCount + linesPerPage - 1) / linesPerPage;
    TEST_ASSERT_EQUAL(2, pages);
}

static void test_sensor_page_count_72x40() {
    // 72x40: maxLines=3, 每页显示 2 条（预留标题行）
    uint8_t linesPerPage = mirrorGetMaxLines(40, 1) - 1;  // = 2
    
    // 5 条传感器数据 → 3 页
    uint8_t entryCount = 5;
    uint8_t pages = (entryCount + linesPerPage - 1) / linesPerPage;
    TEST_ASSERT_EQUAL(3, pages);
}

static void test_sensor_page_count_128x32() {
    // 128x32: maxLines=2, 每页显示 1 条
    uint8_t linesPerPage = mirrorGetMaxLines(32, 1) - 1;  // = 1
    
    // 3 条传感器数据 → 3 页
    uint8_t entryCount = 3;
    uint8_t pages = (entryCount + linesPerPage - 1) / linesPerPage;
    TEST_ASSERT_EQUAL(3, pages);
}

static void test_sensor_page_count_single_entry() {
    // 只有 1 条数据时，任何屏幕都只需 1 页
    uint8_t linesPerPage64 = mirrorGetMaxLines(64, 1) - 1;
    uint8_t linesPerPage40 = mirrorGetMaxLines(40, 1) - 1;
    
    TEST_ASSERT_EQUAL(1, (1 + linesPerPage64 - 1) / linesPerPage64);
    TEST_ASSERT_EQUAL(1, (1 + linesPerPage40 - 1) / linesPerPage40);
}

// ============================================================
//  测试入口
// ============================================================

void test_lcd_manager_group() {
    // Group 1: 分辨率 → 最大行数
    RUN_TEST(test_lcd_max_lines_128x64_default_font);
    RUN_TEST(test_lcd_max_lines_128x32_default_font);
    RUN_TEST(test_lcd_max_lines_72x40_default_font);
    RUN_TEST(test_lcd_max_lines_64x128_default_font);
    RUN_TEST(test_lcd_max_lines_small_font);
    RUN_TEST(test_lcd_max_lines_large_font_72x40);
    
    // Group 2: 分辨率匹配
    RUN_TEST(test_lcd_match_128x64_ssd1306);
    RUN_TEST(test_lcd_match_128x32_ssd1306);
    RUN_TEST(test_lcd_match_72x40_ssd1306_c3);
    RUN_TEST(test_lcd_match_64x128_sh1106);
    RUN_TEST(test_lcd_match_unknown_fallback);
    
    // Group 3: 自定义文本行数截断
    RUN_TEST(test_custom_lines_128x64_max_4_plus_title);
    RUN_TEST(test_custom_lines_128x32_max_2);
    RUN_TEST(test_custom_lines_72x40_max_3);
    RUN_TEST(test_custom_lines_64x128_capped_at_6);
    RUN_TEST(test_custom_lines_72x40_large_font_max_2);
    
    // Group 4: 配置参数 JSON 序列化/反序列化
    RUN_TEST(test_lcd_config_json_serialize_128x64);
    RUN_TEST(test_lcd_config_json_serialize_72x40);
    RUN_TEST(test_lcd_config_json_defaults);
    RUN_TEST(test_lcd_config_json_clamp_overflow);
    RUN_TEST(test_lcd_config_json_spi_interface);
    
    // Group 5: ESP32-C3 引脚校验
    RUN_TEST(test_c3_i2c_pins_valid_5_6);
    RUN_TEST(test_c3_i2c_pins_valid_boundary);
    RUN_TEST(test_c3_i2c_pins_invalid_22);
    RUN_TEST(test_c3_i2c_pins_invalid_negative);
    
    // Group 6: OLED 外设执行动作
    RUN_TEST(test_oled_exec_action_type_exists);
    RUN_TEST(test_oled_exec_multiline_content);
    RUN_TEST(test_oled_exec_content_truncation_72x40);
    
    // Group 7: 传感器分页显示
    RUN_TEST(test_sensor_page_count_128x64);
    RUN_TEST(test_sensor_page_count_72x40);
    RUN_TEST(test_sensor_page_count_128x32);
    RUN_TEST(test_sensor_page_count_single_entry);
}
