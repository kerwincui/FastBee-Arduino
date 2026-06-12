/**
 * @file test_string_utils.cpp
 * @brief StringUtils 工具函数单元测试
 *
 * 测试内容：
 * - 字符串分割 (split)
 * - 字符串连接 (join)
 * - 空白处理 (trim, trimLeft, trimRight)
 * - 大小写转换 (toLower, toUpper)
 * - 前缀/后缀检查 (startsWith, endsWith)
 * - 子串替换 (replace, replaceAll)
 * - 空值与数字检查 (isEmpty, isNumeric, isInteger, isFloat)
 * - 类型转换 (toInt, toFloat, toBool)
 * - URL / JSON 转义 (urlEncode, urlDecode, jsonEscape)
 * - Base64 编码 (base64Encode)
 * - MD5 / SHA256 哈希
 * - 其他辅助函数 (pad, repeat, reverse, substring)
 */

/**
 * 注意：StringUtils.cpp 在 native 测试中默认不编译（test_build_src = no）。
 * 此处直接包含实现文件以进行单元测试。MD5Builder 使用 test/mocks/MD5Builder.h 模拟。
 */
#include <unity.h>
#include <Arduino.h>
#include "utils/StringUtils.h"
#include "../src/utils/StringUtils.cpp"

void test_string_utils_group();

// ========== 分割与连接 ==========

static void test_split_by_char() {
    auto parts = StringUtils::split("a,b,c", ',');
    TEST_ASSERT_EQUAL(3, (int)parts.size());
    TEST_ASSERT_EQUAL_STRING("a", parts[0].c_str());
    TEST_ASSERT_EQUAL_STRING("b", parts[1].c_str());
    TEST_ASSERT_EQUAL_STRING("c", parts[2].c_str());

    auto empty = StringUtils::split("", ',');
    TEST_ASSERT_EQUAL(0, (int)empty.size());

    auto single = StringUtils::split("hello", ',');
    TEST_ASSERT_EQUAL(1, (int)single.size());
    TEST_ASSERT_EQUAL_STRING("hello", single[0].c_str());
}

static void test_split_by_string() {
    auto parts = StringUtils::split("a::b::c", String("::"));
    TEST_ASSERT_EQUAL(3, (int)parts.size());
    TEST_ASSERT_EQUAL_STRING("a", parts[0].c_str());

    auto emptyDelim = StringUtils::split("hello", String(""));
    TEST_ASSERT_EQUAL(1, (int)emptyDelim.size());
}

static void test_join() {
    std::vector<String> parts = {"a", "b", "c"};
    TEST_ASSERT_EQUAL_STRING("abc", StringUtils::join(parts).c_str());
    TEST_ASSERT_EQUAL_STRING("a,b,c", StringUtils::join(parts, ",").c_str());

    std::vector<String> empty;
    TEST_ASSERT_EQUAL_STRING("", StringUtils::join(empty).c_str());
}

// ========== 空白处理 ==========

static void test_trim() {
    TEST_ASSERT_EQUAL_STRING("hello", StringUtils::trim("  hello  ").c_str());
    TEST_ASSERT_EQUAL_STRING("hello", StringUtils::trim("\t\nhello\r\n").c_str());
    TEST_ASSERT_EQUAL_STRING("hello", StringUtils::trim("hello").c_str());
    TEST_ASSERT_EQUAL_STRING("", StringUtils::trim("   ").c_str());
}

static void test_trim_left() {
    TEST_ASSERT_EQUAL_STRING("hello  ", StringUtils::trimLeft("  hello  ").c_str());
}

static void test_trim_right() {
    TEST_ASSERT_EQUAL_STRING("  hello", StringUtils::trimRight("  hello  ").c_str());
}

// ========== 大小写转换 ==========

static void test_to_lower() {
    TEST_ASSERT_EQUAL_STRING("hello", StringUtils::toLower("HELLO").c_str());
    TEST_ASSERT_EQUAL_STRING("hello", StringUtils::toLower("HeLLo").c_str());
}

static void test_to_upper() {
    TEST_ASSERT_EQUAL_STRING("HELLO", StringUtils::toUpper("hello").c_str());
    TEST_ASSERT_EQUAL_STRING("HELLO", StringUtils::toUpper("HeLLo").c_str());
}

// ========== 前缀/后缀/包含 ==========

static void test_starts_with() {
    TEST_ASSERT_TRUE(StringUtils::startsWith("hello world", "hello"));
    TEST_ASSERT_FALSE(StringUtils::startsWith("hello world", "world"));
    TEST_ASSERT_TRUE(StringUtils::startsWith("Hello World", "hello", false));
}

static void test_ends_with() {
    TEST_ASSERT_TRUE(StringUtils::endsWith("hello world", "world"));
    TEST_ASSERT_FALSE(StringUtils::endsWith("hello world", "hello"));
    TEST_ASSERT_TRUE(StringUtils::endsWith("Hello World", "WORLD", false));
}

static void test_contains() {
    TEST_ASSERT_TRUE(StringUtils::contains("hello world", "lo wo"));
    TEST_ASSERT_FALSE(StringUtils::contains("hello world", "xyz"));
    TEST_ASSERT_TRUE(StringUtils::contains("Hello World", "HELLO", false));
}

// ========== 替换 ==========

static void test_replace() {
    TEST_ASSERT_EQUAL_STRING("heXXo world", StringUtils::replace("hello world", "ll", "XX").c_str());
    TEST_ASSERT_EQUAL_STRING("hello world", StringUtils::replace("hello world", "xyz", "XX").c_str());
}

static void test_replace_all() {
    TEST_ASSERT_EQUAL_STRING("heXXo worXd", StringUtils::replaceAll("hello world", "l", "X").c_str());
}

// ========== 空值与数字检查 ==========

static void test_is_empty() {
    TEST_ASSERT_TRUE(StringUtils::isEmpty(""));
    TEST_ASSERT_TRUE(StringUtils::isEmpty("   "));
    TEST_ASSERT_TRUE(StringUtils::isEmpty("\t\n"));
    TEST_ASSERT_FALSE(StringUtils::isEmpty("hello"));
}

static void test_is_numeric() {
    TEST_ASSERT_TRUE(StringUtils::isNumeric("123"));
    TEST_ASSERT_TRUE(StringUtils::isNumeric("-123.45"));
    TEST_ASSERT_TRUE(StringUtils::isNumeric("+0.5"));
    TEST_ASSERT_FALSE(StringUtils::isNumeric("abc"));
    TEST_ASSERT_FALSE(StringUtils::isNumeric("12a3"));
}

static void test_is_integer() {
    TEST_ASSERT_TRUE(StringUtils::isInteger("123"));
    TEST_ASSERT_TRUE(StringUtils::isInteger("-456"));
    TEST_ASSERT_FALSE(StringUtils::isInteger("123.45"));
    TEST_ASSERT_FALSE(StringUtils::isInteger("abc"));
}

static void test_is_float() {
    TEST_ASSERT_TRUE(StringUtils::isFloat("123.45"));
    TEST_ASSERT_TRUE(StringUtils::isFloat("-0.5"));
    // "123" is an integer, isFloat requires a decimal point
    TEST_ASSERT_FALSE(StringUtils::isFloat("abc"));
}

// ========== 类型转换 ==========

static void test_to_int() {
    TEST_ASSERT_EQUAL(123, StringUtils::toInt("123"));
    TEST_ASSERT_EQUAL(-456, StringUtils::toInt("-456"));
    TEST_ASSERT_EQUAL(0, StringUtils::toInt("abc"));
    TEST_ASSERT_EQUAL(99, StringUtils::toInt("abc", 99));
}

static void test_to_float() {
    TEST_ASSERT_FLOAT_WITHIN(0.001, 123.45, StringUtils::toFloat("123.45"));
    TEST_ASSERT_FLOAT_WITHIN(0.001, -0.5, StringUtils::toFloat("-0.5"));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.0, StringUtils::toFloat("abc"));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 99.9, StringUtils::toFloat("abc", 99.9f));
}

static void test_to_bool() {
    TEST_ASSERT_TRUE(StringUtils::toBool("true"));
    TEST_ASSERT_TRUE(StringUtils::toBool("TRUE"));
    TEST_ASSERT_TRUE(StringUtils::toBool("1"));
    TEST_ASSERT_TRUE(StringUtils::toBool("yes"));
    TEST_ASSERT_FALSE(StringUtils::toBool("false"));
    TEST_ASSERT_FALSE(StringUtils::toBool("0"));
    TEST_ASSERT_FALSE(StringUtils::toBool("no"));
    TEST_ASSERT_FALSE(StringUtils::toBool("abc"));
    TEST_ASSERT_TRUE(StringUtils::toBool("abc", true));
}

// ========== URL / JSON ==========

static void test_url_encode() {
    TEST_ASSERT_EQUAL_STRING("hello%20world", StringUtils::urlEncode("hello world").c_str());
    TEST_ASSERT_EQUAL_STRING("hello", StringUtils::urlEncode("hello").c_str());
}

static void test_url_decode() {
    TEST_ASSERT_EQUAL_STRING("hello world", StringUtils::urlDecode("hello%20world").c_str());
    TEST_ASSERT_EQUAL_STRING("hello", StringUtils::urlDecode("hello").c_str());
}

static void test_json_escape() {
    TEST_ASSERT_EQUAL_STRING("hello\\nworld", StringUtils::jsonEscape("hello\nworld").c_str());
    TEST_ASSERT_EQUAL_STRING("hello\\tworld", StringUtils::jsonEscape("hello\tworld").c_str());
    TEST_ASSERT_EQUAL_STRING("quote\\\"test", StringUtils::jsonEscape("quote\"test").c_str());
}

// ========== Base64 / Hash ==========

static void test_base64_encode() {
    TEST_ASSERT_EQUAL_STRING("aGVsbG8=", StringUtils::base64Encode("hello").c_str());
    TEST_ASSERT_EQUAL_STRING("", StringUtils::base64Encode("").c_str());
}

static void test_md5() {
    String hash = StringUtils::md5("test");
    TEST_ASSERT_EQUAL(32, hash.length());
}

static void test_sha256() {
    String hash = StringUtils::sha256("test");
    TEST_ASSERT_EQUAL(16, hash.length());
}

// ========== 其他辅助函数 ==========

static void test_pad() {
    TEST_ASSERT_EQUAL_STRING("  hello", StringUtils::pad("hello", 7, ' ', true).c_str());
    TEST_ASSERT_EQUAL_STRING("hello  ", StringUtils::pad("hello", 7, ' ', false).c_str());
    TEST_ASSERT_EQUAL_STRING("hello", StringUtils::pad("hello", 3, ' ', true).c_str());
}

static void test_repeat() {
    TEST_ASSERT_EQUAL_STRING("abcabcabc", StringUtils::repeat("abc", 3).c_str());
    TEST_ASSERT_EQUAL_STRING("", StringUtils::repeat("abc", 0).c_str());
}

static void test_reverse() {
    TEST_ASSERT_EQUAL_STRING("olleh", StringUtils::reverse("hello").c_str());
    TEST_ASSERT_EQUAL_STRING("", StringUtils::reverse("").c_str());
}

static void test_substring() {
    TEST_ASSERT_EQUAL_STRING("lo", StringUtils::substring("hello", 3, 2).c_str());
    TEST_ASSERT_EQUAL_STRING("llo", StringUtils::substring("hello", 2).c_str());
}

static void test_compare_ignore_case() {
    TEST_ASSERT_EQUAL(0, StringUtils::compareIgnoreCase("Hello", "hello"));
    TEST_ASSERT_TRUE(StringUtils::compareIgnoreCase("abc", "def") < 0);
}

static void test_remove_whitespace() {
    TEST_ASSERT_EQUAL_STRING("hello", StringUtils::removeWhitespace(" h e l l o ").c_str());
    TEST_ASSERT_EQUAL_STRING("", StringUtils::removeWhitespace("   ").c_str());
}

static void test_build_json_response() {
    TEST_ASSERT_EQUAL_STRING("{\"status\":200,\"msg\":\"ok\",\"data\":\"result\"}",
                             StringUtils::buildJsonResponse(200, "ok", "\"result\"").c_str());
}

// ========== 测试主入口 ==========

void test_string_utils_group() {
    RUN_TEST(test_split_by_char);
    RUN_TEST(test_split_by_string);
    RUN_TEST(test_join);
    RUN_TEST(test_trim);
    RUN_TEST(test_trim_left);
    RUN_TEST(test_trim_right);
    RUN_TEST(test_to_lower);
    RUN_TEST(test_to_upper);
    RUN_TEST(test_starts_with);
    RUN_TEST(test_ends_with);
    RUN_TEST(test_contains);
    RUN_TEST(test_replace);
    RUN_TEST(test_replace_all);
    RUN_TEST(test_is_empty);
    RUN_TEST(test_is_numeric);
    RUN_TEST(test_is_integer);
    RUN_TEST(test_is_float);
    RUN_TEST(test_to_int);
    RUN_TEST(test_to_float);
    RUN_TEST(test_to_bool);
    RUN_TEST(test_url_encode);
    RUN_TEST(test_url_decode);
    RUN_TEST(test_json_escape);
    RUN_TEST(test_base64_encode);
    RUN_TEST(test_md5);
    RUN_TEST(test_sha256);
    RUN_TEST(test_pad);
    RUN_TEST(test_repeat);
    RUN_TEST(test_reverse);
    RUN_TEST(test_substring);
    RUN_TEST(test_compare_ignore_case);
    RUN_TEST(test_remove_whitespace);
    RUN_TEST(test_build_json_response);
}
