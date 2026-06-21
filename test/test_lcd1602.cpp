/**
 * @file test_lcd1602.cpp
 * @brief LCD1602 I2C 字符液晶驱动单元测试
 * 
 * 测试内容（纯逻辑，不依赖硬件）：
 * - 行偏移地址计算
 * - I2C 地址自动检测范围
 * - 文本截断逻辑
 * - 外设类型枚举集成
 */

#include <unity.h>
#include <Arduino.h>
#include "core/PeripheralTypes.h"

void test_lcd1602_group();

// ========== 镜像 LCD1602 行偏移逻辑 ==========

static const uint8_t MIRROR_ROW_OFFSETS[] = {0x00, 0x40, 0x14, 0x54};

static uint8_t mirrorGetRowOffset(uint8_t row, uint8_t maxRows) {
    if (row >= maxRows) row = maxRows - 1;
    return MIRROR_ROW_OFFSETS[row];
}

// ========== 镜像文本截断逻辑 ==========

static String mirrorTruncateText(const String& text, uint8_t maxCols) {
    if (text.length() > maxCols) {
        return text.substring(0, maxCols);
    }
    return text;
}

// ========== 镜像 I2C 地址扫描范围 ==========

static bool isCommonAddress(uint8_t addr) {
    uint8_t addresses[] = {0x27, 0x3F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E};
    for (uint8_t a : addresses) {
        if (a == addr) return true;
    }
    return false;
}

// ============================================================
// TEST GROUP: 行偏移地址计算
// ============================================================

static void test_lcd1602_row_offset_0() {
    TEST_ASSERT_EQUAL(0x00, mirrorGetRowOffset(0, 2));
}

static void test_lcd1602_row_offset_1() {
    TEST_ASSERT_EQUAL(0x40, mirrorGetRowOffset(1, 2));
}

static void test_lcd1602_row_offset_2_4line() {
    TEST_ASSERT_EQUAL(0x14, mirrorGetRowOffset(2, 4));
}

static void test_lcd1602_row_offset_3_4line() {
    TEST_ASSERT_EQUAL(0x54, mirrorGetRowOffset(3, 4));
}

static void test_lcd1602_row_overflow_clamp() {
    TEST_ASSERT_EQUAL(0x40, mirrorGetRowOffset(5, 2));
}

// ============================================================
// TEST GROUP: 文本截断
// ============================================================

static void test_lcd1602_truncate_short_text() {
    String result = mirrorTruncateText("Hello", 16);
    TEST_ASSERT_EQUAL_STRING("Hello", result.c_str());
}

static void test_lcd1602_truncate_exact_length() {
    String result = mirrorTruncateText("0123456789ABCDEF", 16);
    TEST_ASSERT_EQUAL_STRING("0123456789ABCDEF", result.c_str());
}

static void test_lcd1602_truncate_long_text() {
    String result = mirrorTruncateText("0123456789ABCDEFGH", 16);
    TEST_ASSERT_EQUAL_STRING("0123456789ABCDEF", result.c_str());
}

static void test_lcd1602_truncate_20cols() {
    String result = mirrorTruncateText("01234567890123456789ABCD", 20);
    TEST_ASSERT_EQUAL_STRING("01234567890123456789", result.c_str());
}

// ============================================================
// TEST GROUP: I2C 地址检测
// ============================================================

static void test_lcd1602_common_address_27() {
    TEST_ASSERT_TRUE(isCommonAddress(0x27));
}

static void test_lcd1602_common_address_3f() {
    TEST_ASSERT_TRUE(isCommonAddress(0x3F));
}

static void test_lcd1602_uncommon_address_10() {
    TEST_ASSERT_FALSE(isCommonAddress(0x10));
}

static void test_lcd1602_uncommon_address_50() {
    TEST_ASSERT_FALSE(isCommonAddress(0x50));
}

// ============================================================
// TEST GROUP: 外设类型枚举
// ============================================================

static void test_lcd1602_type_value() {
    TEST_ASSERT_EQUAL(52, static_cast<int>(PeripheralType::LCD1602));
}

static void test_lcd1602_type_name() {
    TEST_ASSERT_EQUAL_STRING("LCD1602", getPeripheralTypeName(PeripheralType::LCD1602));
}

static void test_lcd1602_pin_count() {
    TEST_ASSERT_EQUAL(2, getPeripheralPinCount(PeripheralType::LCD1602));
}

static void test_lcd1602_parse_string() {
    TEST_ASSERT_EQUAL(PeripheralType::LCD1602, parsePeripheralType("LCD1602"));
    TEST_ASSERT_EQUAL(PeripheralType::LCD1602, parsePeripheralType("LCD_1602"));
    TEST_ASSERT_EQUAL(PeripheralType::LCD1602, parsePeripheralType("lcd1602"));
}

static void test_lcd1602_category() {
    PeripheralCategory cat = getPeripheralCategory(PeripheralType::LCD1602);
    TEST_ASSERT_EQUAL(PeripheralCategory::CATEGORY_SPECIAL, cat);
}

static void test_lcd1602_from_int() {
    TEST_ASSERT_EQUAL(PeripheralType::LCD1602, peripheralTypeFromInt(52));
}

// ============================================================
// TEST GROUP: 配置参数默认值
// ============================================================

struct MirrorLCD1602Config {
    uint8_t cols = 16;
    uint8_t rows = 2;
    uint8_t i2cAddress = 0x27;
    bool backlight = true;
};

static void test_lcd1602_config_defaults() {
    MirrorLCD1602Config config;
    TEST_ASSERT_EQUAL(16, config.cols);
    TEST_ASSERT_EQUAL(2, config.rows);
    TEST_ASSERT_EQUAL(0x27, config.i2cAddress);
    TEST_ASSERT_TRUE(config.backlight);
}

static void test_lcd1602_config_20x4() {
    MirrorLCD1602Config config;
    config.cols = 20;
    config.rows = 4;
    TEST_ASSERT_EQUAL(20, config.cols);
    TEST_ASSERT_EQUAL(4, config.rows);
}

// ============================================================
// 测试注册函数
// ============================================================

void test_lcd1602_group() {
    UNITY_BEGIN();
    
    // 行偏移
    RUN_TEST(test_lcd1602_row_offset_0);
    RUN_TEST(test_lcd1602_row_offset_1);
    RUN_TEST(test_lcd1602_row_offset_2_4line);
    RUN_TEST(test_lcd1602_row_offset_3_4line);
    RUN_TEST(test_lcd1602_row_overflow_clamp);
    
    // 文本截断
    RUN_TEST(test_lcd1602_truncate_short_text);
    RUN_TEST(test_lcd1602_truncate_exact_length);
    RUN_TEST(test_lcd1602_truncate_long_text);
    RUN_TEST(test_lcd1602_truncate_20cols);
    
    // I2C 地址
    RUN_TEST(test_lcd1602_common_address_27);
    RUN_TEST(test_lcd1602_common_address_3f);
    RUN_TEST(test_lcd1602_uncommon_address_10);
    RUN_TEST(test_lcd1602_uncommon_address_50);
    
    // 外设类型
    RUN_TEST(test_lcd1602_type_value);
    RUN_TEST(test_lcd1602_type_name);
    RUN_TEST(test_lcd1602_pin_count);
    RUN_TEST(test_lcd1602_parse_string);
    RUN_TEST(test_lcd1602_category);
    RUN_TEST(test_lcd1602_from_int);
    
    // 配置参数
    RUN_TEST(test_lcd1602_config_defaults);
    RUN_TEST(test_lcd1602_config_20x4);
    
    UNITY_END();
}
