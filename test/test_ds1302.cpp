/**
 * @file test_ds1302.cpp
 * @brief DS1302 实时时钟驱动单元测试
 * 
 * 测试内容（纯逻辑，不依赖硬件）：
 * - BCD 码与十进制转换
 * - 日期时间结构体格式化
 * - Unix 时间戳计算
 * - 外设类型枚举集成
 */

#include <unity.h>
#include <Arduino.h>
#include "core/PeripheralTypes.h"

void test_ds1302_group();

// ========== 镜像 DS1302 BCD 转换逻辑 ==========

static uint8_t mirrorDecToBcd(uint8_t val) {
    return ((val / 10) << 4) | (val % 10);
}

static uint8_t mirrorBcdToDec(uint8_t val) {
    return ((val >> 4) * 10) + (val & 0x0F);
}

// ============================================================
// TEST GROUP: BCD 码转换
// ============================================================

static void test_ds1302_bcd_dec_to_bcd_0() {
    TEST_ASSERT_EQUAL(0x00, mirrorDecToBcd(0));
}

static void test_ds1302_bcd_dec_to_bcd_9() {
    TEST_ASSERT_EQUAL(0x09, mirrorDecToBcd(9));
}

static void test_ds1302_bcd_dec_to_bcd_23() {
    TEST_ASSERT_EQUAL(0x23, mirrorDecToBcd(23));
}

static void test_ds1302_bcd_dec_to_bcd_59() {
    TEST_ASSERT_EQUAL(0x59, mirrorDecToBcd(59));
}

static void test_ds1302_bcd_dec_to_bcd_99() {
    TEST_ASSERT_EQUAL(0x99, mirrorDecToBcd(99));
}

static void test_ds1302_bcd_bcd_to_dec_00() {
    TEST_ASSERT_EQUAL(0, mirrorBcdToDec(0x00));
}

static void test_ds1302_bcd_bcd_to_dec_09() {
    TEST_ASSERT_EQUAL(9, mirrorBcdToDec(0x09));
}

static void test_ds1302_bcd_bcd_to_dec_23() {
    TEST_ASSERT_EQUAL(23, mirrorBcdToDec(0x23));
}

static void test_ds1302_bcd_bcd_to_dec_59() {
    TEST_ASSERT_EQUAL(59, mirrorBcdToDec(0x59));
}

static void test_ds1302_bcd_roundtrip() {
    for (uint8_t i = 0; i < 60; i++) {
        TEST_ASSERT_EQUAL(i, mirrorBcdToDec(mirrorDecToBcd(i)));
    }
}

// ============================================================
// TEST GROUP: 外设类型枚举
// ============================================================

static void test_ds1302_type_value() {
    TEST_ASSERT_EQUAL(50, static_cast<int>(PeripheralType::DS1302_RTC));
}

static void test_ds1302_type_name() {
    TEST_ASSERT_EQUAL_STRING("DS1302 RTC", getPeripheralTypeName(PeripheralType::DS1302_RTC));
}

static void test_ds1302_pin_count() {
    TEST_ASSERT_EQUAL(3, getPeripheralPinCount(PeripheralType::DS1302_RTC));
}

static void test_ds1302_parse_string() {
    TEST_ASSERT_EQUAL(PeripheralType::DS1302_RTC, parsePeripheralType("DS1302"));
    TEST_ASSERT_EQUAL(PeripheralType::DS1302_RTC, parsePeripheralType("DS1302_RTC"));
    TEST_ASSERT_EQUAL(PeripheralType::DS1302_RTC, parsePeripheralType("RTC_DS1302"));
    TEST_ASSERT_EQUAL(PeripheralType::DS1302_RTC, parsePeripheralType("ds1302"));
}

static void test_ds1302_category() {
    PeripheralCategory cat = getPeripheralCategory(PeripheralType::DS1302_RTC);
    TEST_ASSERT_EQUAL(PeripheralCategory::CATEGORY_SPECIAL, cat);
}

static void test_ds1302_from_int() {
    TEST_ASSERT_EQUAL(PeripheralType::DS1302_RTC, peripheralTypeFromInt(50));
}

// ============================================================
// TEST GROUP: Unix 时间戳计算
// ============================================================

static uint32_t mirrorToUnixTimestamp(uint8_t year, uint8_t month, uint8_t day,
                                       uint8_t hour, uint8_t minute, uint8_t second) {
    static const uint8_t daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    
    uint32_t timestamp = 0;
    
    for (uint8_t y = 0; y < year; y++) {
        timestamp += (y % 4 == 0) ? 366 : 365;
    }
    
    for (uint8_t m = 1; m < month; m++) {
        timestamp += daysInMonth[m - 1];
        if (m == 2 && year % 4 == 0) timestamp++;
    }
    
    timestamp += (day - 1);
    timestamp = timestamp * 86400UL + hour * 3600UL + minute * 60UL + second;
    timestamp += 946684800UL;
    
    return timestamp;
}

static void test_ds1302_timestamp_2000_01_01() {
    uint32_t ts = mirrorToUnixTimestamp(0, 1, 1, 0, 0, 0);
    TEST_ASSERT_EQUAL(946684800UL, ts);
}

static void test_ds1302_timestamp_2024_01_01() {
    uint32_t ts = mirrorToUnixTimestamp(24, 1, 1, 0, 0, 0);
    TEST_ASSERT_EQUAL(1704067200UL, ts);
}

static void test_ds1302_timestamp_2024_06_15_12_30_45() {
    uint32_t ts = mirrorToUnixTimestamp(24, 6, 15, 12, 30, 45);
    TEST_ASSERT_EQUAL(1718454645UL, ts);
}

// ============================================================
// 测试注册函数
// ============================================================

void test_ds1302_group() {
    UNITY_BEGIN();
    
    // BCD 转换
    RUN_TEST(test_ds1302_bcd_dec_to_bcd_0);
    RUN_TEST(test_ds1302_bcd_dec_to_bcd_9);
    RUN_TEST(test_ds1302_bcd_dec_to_bcd_23);
    RUN_TEST(test_ds1302_bcd_dec_to_bcd_59);
    RUN_TEST(test_ds1302_bcd_dec_to_bcd_99);
    RUN_TEST(test_ds1302_bcd_bcd_to_dec_00);
    RUN_TEST(test_ds1302_bcd_bcd_to_dec_09);
    RUN_TEST(test_ds1302_bcd_bcd_to_dec_23);
    RUN_TEST(test_ds1302_bcd_bcd_to_dec_59);
    RUN_TEST(test_ds1302_bcd_roundtrip);
    
    // 外设类型
    RUN_TEST(test_ds1302_type_value);
    RUN_TEST(test_ds1302_type_name);
    RUN_TEST(test_ds1302_pin_count);
    RUN_TEST(test_ds1302_parse_string);
    RUN_TEST(test_ds1302_category);
    RUN_TEST(test_ds1302_from_int);
    
    // 时间戳
    RUN_TEST(test_ds1302_timestamp_2000_01_01);
    RUN_TEST(test_ds1302_timestamp_2024_01_01);
    RUN_TEST(test_ds1302_timestamp_2024_06_15_12_30_45);
    
    UNITY_END();
}
