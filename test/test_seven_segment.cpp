/**
 * @file test_seven_segment.cpp
 * @brief TM1637 4位数码管驱动单元测试
 *
 * 测试内容（纯逻辑，不依赖硬件）：
 * - charToSeg 段码映射表
 * - displayNumber 小数点 DP、冒号、右对齐解析
 * - 亮度范围钳位（0~7）
 * - 文本截断（超过 4 字符取前 4）
 * - TM1637 协议常量
 */

#include <unity.h>
#include <Arduino.h>
#include "helpers/TestLogger.h"
#include <cstring>

void test_seven_segment_group();

// ========== 镜像 charToSeg 逻辑 ==========
// 来自 SevenSegmentDriver.cpp

// 数字段码表
static const uint8_t MIRROR_SEG_DIGITS[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

static uint8_t mirrorCharToSeg(char c) {
    if (c >= '0' && c <= '9') return MIRROR_SEG_DIGITS[c - '0'];
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    switch (c) {
        case 'A': return 0x77;
        case 'B': return 0x7C;
        case 'C': return 0x39;
        case 'D': return 0x5E;
        case 'E': return 0x79;
        case 'F': return 0x71;
        case 'G': return 0x3D;
        case 'H': return 0x76;
        case 'I': return 0x06;
        case 'J': return 0x1E;
        case 'L': return 0x38;
        case 'N': return 0x54;
        case 'O': return 0x3F;
        case 'P': return 0x73;
        case 'Q': return 0x67;
        case 'R': return 0x50;
        case 'S': return 0x6D;
        case 'T': return 0x78;
        case 'U': return 0x3E;
        case 'Y': return 0x6E;
        case '-': return 0x40;
        case '_': return 0x08;
        case ' ': return 0x00;
        default:  return 0x00;
    }
}

// TM1637 协议常量
#define TM1637_CMD_DATA     0x40
#define TM1637_CMD_ADDR     0xC0
#define TM1637_CMD_DISP_CTL 0x88
#define TM1637_BIT_DELAY_US 50

// 亮度范围
static uint8_t clampBrightness(uint8_t bri) {
    return (bri > 7) ? 7 : bri;
}

// ============================================================
// TEST GROUP 1: 数字段码映射
// ============================================================

static void test_seg_digit_0() {
    TEST_ASSERT_EQUAL_HEX8(0x3F, MIRROR_SEG_DIGITS[0]);
}

static void test_seg_digit_1() {
    TEST_ASSERT_EQUAL_HEX8(0x06, MIRROR_SEG_DIGITS[1]);
}

static void test_seg_digit_8() {
    // 8 点亮全部 7 段 = 0x7F
    TEST_ASSERT_EQUAL_HEX8(0x7F, MIRROR_SEG_DIGITS[8]);
}

static void test_seg_digit_9() {
    TEST_ASSERT_EQUAL_HEX8(0x6F, MIRROR_SEG_DIGITS[9]);
}

// ============================================================
// TEST GROUP 2: charToSeg 字母映射
// ============================================================

static void test_char_seg_uppercase_A() {
    TEST_ASSERT_EQUAL_HEX8(0x77, mirrorCharToSeg('A'));
}

static void test_char_seg_lowercase_a() {
    // 小写 a 应等同于大写 A
    TEST_ASSERT_EQUAL_HEX8(0x77, mirrorCharToSeg('a'));
}

static void test_char_seg_H() {
    TEST_ASSERT_EQUAL_HEX8(0x76, mirrorCharToSeg('H'));
}

static void test_char_seg_L() {
    TEST_ASSERT_EQUAL_HEX8(0x38, mirrorCharToSeg('L'));
}

static void test_char_seg_dash() {
    TEST_ASSERT_EQUAL_HEX8(0x40, mirrorCharToSeg('-'));
}

static void test_char_seg_underscore() {
    TEST_ASSERT_EQUAL_HEX8(0x08, mirrorCharToSeg('_'));
}

static void test_char_seg_space() {
    TEST_ASSERT_EQUAL_HEX8(0x00, mirrorCharToSeg(' '));
}

static void test_char_seg_unknown_char() {
    // 未定义字符返回 0x00（全灭）
    TEST_ASSERT_EQUAL_HEX8(0x00, mirrorCharToSeg('@'));
    TEST_ASSERT_EQUAL_HEX8(0x00, mirrorCharToSeg('#'));
}

static void test_char_seg_case_insensitive() {
    TEST_ASSERT_EQUAL_HEX8(mirrorCharToSeg('B'), mirrorCharToSeg('b'));
    TEST_ASSERT_EQUAL_HEX8(mirrorCharToSeg('C'), mirrorCharToSeg('c'));
    TEST_ASSERT_EQUAL_HEX8(mirrorCharToSeg('F'), mirrorCharToSeg('f'));
}

// ============================================================
// TEST GROUP 3: 小数点 DP 附加逻辑
// ============================================================

// 镜像 displayNumber 中的 DP 处理：
// '.' 附加到前一位的 DP 位（bit 7 = 0x80）

static void test_dp_bit_value() {
    // DP 位是 bit 7 (0x80)
    uint8_t seg = MIRROR_SEG_DIGITS[1];  // 0x06
    uint8_t withDP = seg | 0x80;
    TEST_ASSERT_EQUAL_HEX8(0x86, withDP);
}

static void test_dp_on_digit_5() {
    // "5." → digit 5 + DP = 0x6D | 0x80 = 0xED
    uint8_t seg = MIRROR_SEG_DIGITS[5] | 0x80;
    TEST_ASSERT_EQUAL_HEX8(0xED, seg);
}

// ============================================================
// TEST GROUP 4: displayNumber 右对齐逻辑
// ============================================================

// 镜像 displayNumber 的 token 解析逻辑
struct DisplayToken { char c; bool dp; };

// 返回解析后 token 数量
static int parseDisplayNumber(const String& value, DisplayToken tokens[], int maxTok) {
    int tokLen = 0;
    bool dummy_colon = false;
    for (size_t i = 0; i < value.length() && tokLen < maxTok; i++) {
        char c = value[i];
        if (c == '.') {
            if (tokLen > 0) tokens[tokLen - 1].dp = true;
        } else if (c == ':') {
            dummy_colon = true;
        } else {
            tokens[tokLen].c = c;
            tokens[tokLen].dp = false;
            tokLen++;
        }
    }
    return tokLen;
}

static void test_parse_integer_1234() {
    DisplayToken tokens[8];
    int len = parseDisplayNumber("1234", tokens, 8);
    TEST_ASSERT_EQUAL(4, len);
    TEST_ASSERT_EQUAL('1', tokens[0].c);
    TEST_ASSERT_EQUAL('4', tokens[3].c);
    TEST_ASSERT_FALSE(tokens[0].dp);
}

static void test_parse_decimal_12_34() {
    // "12.34" → tokens: [1,2(dp),3,4] = 4 tokens
    DisplayToken tokens[8];
    int len = parseDisplayNumber("12.34", tokens, 8);
    TEST_ASSERT_EQUAL(4, len);
    TEST_ASSERT_EQUAL('2', tokens[1].c);
    TEST_ASSERT_TRUE(tokens[1].dp);  // '.' 附加到前一位
}

static void test_parse_time_12_34() {
    // "12:34" → tokens: [1,2,3,4] + colon flag = 4 tokens
    DisplayToken tokens[8];
    int len = parseDisplayNumber("12:34", tokens, 8);
    TEST_ASSERT_EQUAL(4, len);
}

static void test_parse_short_number_42() {
    // "42" → 2 tokens，显示时右对齐，左侧填 2 个空格段
    DisplayToken tokens[8];
    int len = parseDisplayNumber("42", tokens, 8);
    TEST_ASSERT_EQUAL(2, len);
    // 右对齐后：segs = [0, 0, seg('4'), seg('2')]
    int padLeft = 4 - len;  // 2
    TEST_ASSERT_EQUAL(2, padLeft);
}

static void test_parse_overflow_truncated() {
    // "12345" → 5 tokens，但只显示最后 4 个
    DisplayToken tokens[8];
    int len = parseDisplayNumber("12345", tokens, 8);
    TEST_ASSERT_EQUAL(5, len);
    // startIdx = tokLen - 4 = 1，所以显示 "2345"
    int startIdx = len - 4;
    TEST_ASSERT_EQUAL(1, startIdx);
    TEST_ASSERT_EQUAL('2', tokens[startIdx].c);
}

// ============================================================
// TEST GROUP 5: 亮度钳位
// ============================================================

static void test_brightness_normal() {
    TEST_ASSERT_EQUAL(3, clampBrightness(3));
}

static void test_brightness_zero() {
    TEST_ASSERT_EQUAL(0, clampBrightness(0));
}

static void test_brightness_max() {
    TEST_ASSERT_EQUAL(7, clampBrightness(7));
}

static void test_brightness_over_clamped() {
    TEST_ASSERT_EQUAL(7, clampBrightness(8));
    TEST_ASSERT_EQUAL(7, clampBrightness(255));
}

// ============================================================
// TEST GROUP 6: 文本截断
// ============================================================

// 镜像 displayText 逻辑：最多 4 字符
static int mirrorTextLength(const String& text) {
    int len = (int)text.length();
    if (len > 4) len = 4;
    return len;
}

static void test_text_short() {
    TEST_ASSERT_EQUAL(2, mirrorTextLength("Hi"));
}

static void test_text_exact_4() {
    TEST_ASSERT_EQUAL(4, mirrorTextLength("HELP"));
}

static void test_text_over_4_truncated() {
    TEST_ASSERT_EQUAL(4, mirrorTextLength("HELLO"));
    TEST_ASSERT_EQUAL(4, mirrorTextLength("12345678"));
}

static void test_text_empty() {
    TEST_ASSERT_EQUAL(0, mirrorTextLength(""));
}

// ============================================================
// TEST GROUP 7: TM1637 协议常量
// ============================================================

static void test_cmd_data_value() {
    TEST_ASSERT_EQUAL_HEX8(0x40, TM1637_CMD_DATA);
}

static void test_cmd_addr_value() {
    TEST_ASSERT_EQUAL_HEX8(0xC0, TM1637_CMD_ADDR);
}

static void test_cmd_disp_ctl_value() {
    TEST_ASSERT_EQUAL_HEX8(0x88, TM1637_CMD_DISP_CTL);
}

static void test_disp_ctl_with_brightness() {
    // 显示控制命令：0x88 | (bri & 0x07)
    uint8_t bri = 5;
    uint8_t cmd = TM1637_CMD_DISP_CTL | (bri & 0x07);
    TEST_ASSERT_EQUAL_HEX8(0x8D, cmd);  // 0x88 | 0x05 = 0x8D
}

static void test_disp_ctl_brightness_mask() {
    // 亮度只取低 3 位
    uint8_t bri = 0xFF;  // 实际已被钳位为 7
    uint8_t cmd = TM1637_CMD_DISP_CTL | (clampBrightness(bri) & 0x07);
    TEST_ASSERT_EQUAL_HEX8(0x8F, cmd);  // 0x88 | 0x07 = 0x8F
}

static void test_bit_delay_value() {
    TEST_ASSERT_EQUAL(50, TM1637_BIT_DELAY_US);
}

// ============================================================
// TEST GROUP 8: 段码完整性检查
// ============================================================

static void test_all_digits_have_segments() {
    // 每个数字的段码不应为 0
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_NOT_EQUAL(0, MIRROR_SEG_DIGITS[i]);
    }
}

static void test_digit_1_minimal_segments() {
    // 1 只点亮 b,c 段 = 0x06 (2 位)
    uint8_t seg = MIRROR_SEG_DIGITS[1];
    int bits = 0;
    for (int i = 0; i < 7; i++) {
        if (seg & (1 << i)) bits++;
    }
    TEST_ASSERT_EQUAL(2, bits);
}

static void test_digit_8_all_segments() {
    // 8 点亮全部 7 段
    uint8_t seg = MIRROR_SEG_DIGITS[8];
    int bits = 0;
    for (int i = 0; i < 7; i++) {
        if (seg & (1 << i)) bits++;
    }
    TEST_ASSERT_EQUAL(7, bits);
}

// ============================================================
// 主入口
// ============================================================

void test_seven_segment_group() {
    TestLog::groupStart("SevenSegment (TM1637) Tests");

    // 数字段码
    RUN_TEST(test_seg_digit_0);
    RUN_TEST(test_seg_digit_1);
    RUN_TEST(test_seg_digit_8);
    RUN_TEST(test_seg_digit_9);

    // 字母段码
    RUN_TEST(test_char_seg_uppercase_A);
    RUN_TEST(test_char_seg_lowercase_a);
    RUN_TEST(test_char_seg_H);
    RUN_TEST(test_char_seg_L);
    RUN_TEST(test_char_seg_dash);
    RUN_TEST(test_char_seg_underscore);
    RUN_TEST(test_char_seg_space);
    RUN_TEST(test_char_seg_unknown_char);
    RUN_TEST(test_char_seg_case_insensitive);

    // DP 附加
    RUN_TEST(test_dp_bit_value);
    RUN_TEST(test_dp_on_digit_5);

    // 数字解析
    RUN_TEST(test_parse_integer_1234);
    RUN_TEST(test_parse_decimal_12_34);
    RUN_TEST(test_parse_time_12_34);
    RUN_TEST(test_parse_short_number_42);
    RUN_TEST(test_parse_overflow_truncated);

    // 亮度
    RUN_TEST(test_brightness_normal);
    RUN_TEST(test_brightness_zero);
    RUN_TEST(test_brightness_max);
    RUN_TEST(test_brightness_over_clamped);

    // 文本截断
    RUN_TEST(test_text_short);
    RUN_TEST(test_text_exact_4);
    RUN_TEST(test_text_over_4_truncated);
    RUN_TEST(test_text_empty);

    // 协议常量
    RUN_TEST(test_cmd_data_value);
    RUN_TEST(test_cmd_addr_value);
    RUN_TEST(test_cmd_disp_ctl_value);
    RUN_TEST(test_disp_ctl_with_brightness);
    RUN_TEST(test_disp_ctl_brightness_mask);
    RUN_TEST(test_bit_delay_value);

    // 段码完整性
    RUN_TEST(test_all_digits_have_segments);
    RUN_TEST(test_digit_1_minimal_segments);
    RUN_TEST(test_digit_8_all_segments);

    TestLog::groupEnd();
}
