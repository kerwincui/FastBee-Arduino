/**
 * @file test_time_utils.cpp
 * @brief TimeUtils 纯逻辑单元测试
 *
 * 覆盖范围：
 *  - 闰年判断 (isLeapYear)
 *  - 每月天数 (getDaysInMonth)
 *  - 时间间隔格式化 (formatDuration)
 *  - 时间算术 (addSeconds/Minutes/Hours/Days)
 *  - 时间差 (timeDifference)
 *  - 同一天判断 (isSameDay)
 *  - 时间范围检查 (isTimeInRange)
 *  - 预定义时区查询 (getTimeZoneByName)
 *  - ISO8601 解析 (parseTime)
 *  - 时间格式化 (formatTime)
 *  - 日起/日终 (getDayStart/getDayEnd)
 *  - 星期几 / 月日 / 年日 (getDayOfWeek/getDayOfMonth/getDayOfYear)
 */

#include <unity.h>
#include <Arduino.h>
#include "utils/TimeUtils.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

void test_time_utils_group();

// ========== 闰年判断 ==========

void test_leap_year_basic() {
    TestLog::testStart("Leap Year: Basic Cases");

    // 标准闰年：能被 4 整除但不能被 100 整除
    TEST_ASSERT_TRUE(TimeUtils::isLeapYear(2024));
    TEST_ASSERT_TRUE(TimeUtils::isLeapYear(2028));
    TEST_ASSERT_TRUE(TimeUtils::isLeapYear(2016));
    TestLog::step("Standard leap years verified (2024, 2028, 2016)");

    // 非闰年
    TEST_ASSERT_FALSE(TimeUtils::isLeapYear(2023));
    TEST_ASSERT_FALSE(TimeUtils::isLeapYear(2025));
    TEST_ASSERT_FALSE(TimeUtils::isLeapYear(2019));
    TestLog::step("Non-leap years verified (2023, 2025, 2019)");

    TestLog::testEnd(true);
}

void test_leap_year_century() {
    TestLog::testStart("Leap Year: Century Rules");

    // 能被 100 整除但不能被 400 整除 → 非闰年
    TEST_ASSERT_FALSE(TimeUtils::isLeapYear(1900));
    TEST_ASSERT_FALSE(TimeUtils::isLeapYear(2100));
    TEST_ASSERT_FALSE(TimeUtils::isLeapYear(2200));
    TestLog::step("Century non-leap years (1900, 2100, 2200)");

    // 能被 400 整除 → 闰年
    TEST_ASSERT_TRUE(TimeUtils::isLeapYear(2000));
    TEST_ASSERT_TRUE(TimeUtils::isLeapYear(1600));
    TEST_ASSERT_TRUE(TimeUtils::isLeapYear(2400));
    TestLog::step("400-year leap years (2000, 1600, 2400)");

    TestLog::testEnd(true);
}

// ========== 每月天数 ==========

void test_days_in_month_normal() {
    TestLog::testStart("Days In Month: Normal Year");

    TEST_ASSERT_EQUAL(31, TimeUtils::getDaysInMonth(2023, 1));   // 一月
    TEST_ASSERT_EQUAL(28, TimeUtils::getDaysInMonth(2023, 2));   // 二月（非闰年）
    TEST_ASSERT_EQUAL(31, TimeUtils::getDaysInMonth(2023, 3));
    TEST_ASSERT_EQUAL(30, TimeUtils::getDaysInMonth(2023, 4));
    TEST_ASSERT_EQUAL(31, TimeUtils::getDaysInMonth(2023, 5));
    TEST_ASSERT_EQUAL(30, TimeUtils::getDaysInMonth(2023, 6));
    TEST_ASSERT_EQUAL(31, TimeUtils::getDaysInMonth(2023, 7));
    TEST_ASSERT_EQUAL(31, TimeUtils::getDaysInMonth(2023, 8));
    TEST_ASSERT_EQUAL(30, TimeUtils::getDaysInMonth(2023, 9));
    TEST_ASSERT_EQUAL(31, TimeUtils::getDaysInMonth(2023, 10));
    TEST_ASSERT_EQUAL(30, TimeUtils::getDaysInMonth(2023, 11));
    TEST_ASSERT_EQUAL(31, TimeUtils::getDaysInMonth(2023, 12));
    TestLog::step("All 12 months verified for non-leap year");

    TestLog::testEnd(true);
}

void test_days_in_month_leap() {
    TestLog::testStart("Days In Month: Leap Year February");

    TEST_ASSERT_EQUAL(29, TimeUtils::getDaysInMonth(2024, 2));
    TEST_ASSERT_EQUAL(29, TimeUtils::getDaysInMonth(2000, 2));
    TestLog::step("Leap year February = 29 days");

    TEST_ASSERT_EQUAL(28, TimeUtils::getDaysInMonth(1900, 2));
    TestLog::step("Century non-leap February = 28 days");

    TestLog::testEnd(true);
}

void test_days_in_month_invalid() {
    TestLog::testStart("Days In Month: Invalid Input");

    TEST_ASSERT_EQUAL(0, TimeUtils::getDaysInMonth(2023, 0));
    TEST_ASSERT_EQUAL(0, TimeUtils::getDaysInMonth(2023, 13));
    TEST_ASSERT_EQUAL(0, TimeUtils::getDaysInMonth(2023, -1));
    TestLog::step("Invalid months return 0");

    TestLog::testEnd(true);
}

// ========== 时间间隔格式化 ==========

void test_format_duration() {
    TestLog::testStart("Format Duration");

    // 纯秒
    String r1 = TimeUtils::formatDuration(5000);
    TEST_ASSERT_STRING_CONTAINS("5s", r1.c_str());
    TestLog::step("5000ms → contains '5s'");

    // 分+秒
    String r2 = TimeUtils::formatDuration(125000);  // 2m 5s
    TEST_ASSERT_STRING_CONTAINS("2m", r2.c_str());
    TEST_ASSERT_STRING_CONTAINS("5s", r2.c_str());
    TestLog::step("125000ms → contains '2m' and '5s'");

    // 时+分+秒
    String r3 = TimeUtils::formatDuration(3661000);  // 1h 1m 1s
    TEST_ASSERT_STRING_CONTAINS("1h", r3.c_str());
    TEST_ASSERT_STRING_CONTAINS("1m", r3.c_str());
    TEST_ASSERT_STRING_CONTAINS("1s", r3.c_str());
    TestLog::step("3661000ms → contains '1h', '1m', '1s'");

    // 天+时+分+秒
    String r4 = TimeUtils::formatDuration(90061000);  // 1d 1h 1m 1s
    TEST_ASSERT_STRING_CONTAINS("1d", r4.c_str());
    TestLog::step("90061000ms → contains '1d'");

    // 零
    String r5 = TimeUtils::formatDuration(0);
    TEST_ASSERT_STRING_CONTAINS("0s", r5.c_str());
    TestLog::step("0ms → contains '0s'");

    TestLog::testEnd(true);
}

void test_format_duration_with_ms() {
    TestLog::testStart("Format Duration: With Milliseconds");

    String r = TimeUtils::formatDuration(5432, true);
    TEST_ASSERT_STRING_CONTAINS("5", r.c_str());
    TestLog::step("5432ms with showMilliseconds=true formatted");

    TestLog::testEnd(true);
}

// ========== 时间算术 ==========

void test_time_arithmetic() {
    TestLog::testStart("Time Arithmetic");

    time_t base = 1700000000;  // 2023-11-14 22:13:20 UTC

    // 加秒
    TEST_ASSERT_EQUAL(base + 60, TimeUtils::addSeconds(base, 60));
    TestLog::step("addSeconds(base, 60) correct");

    // 加分
    TEST_ASSERT_EQUAL(base + 120, TimeUtils::addMinutes(base, 2));
    TestLog::step("addMinutes(base, 2) = +120s");

    // 加小时
    TEST_ASSERT_EQUAL(base + 7200, TimeUtils::addHours(base, 2));
    TestLog::step("addHours(base, 2) = +7200s");

    // 加天
    TEST_ASSERT_EQUAL(base + 172800, TimeUtils::addDays(base, 2));
    TestLog::step("addDays(base, 2) = +172800s");

    // 负值
    TEST_ASSERT_EQUAL(base - 3600, TimeUtils::addHours(base, -1));
    TestLog::step("addHours(base, -1) = -3600s");

    TestLog::testEnd(true);
}

// ========== 时间差 ==========

void test_time_difference() {
    TestLog::testStart("Time Difference");

    time_t t1 = 1700000000;
    time_t t2 = 1700003600;  // +1h

    long diff = TimeUtils::timeDifference(t2, t1);
    TEST_ASSERT_EQUAL(3600, diff);
    TestLog::step("1 hour difference = 3600s");

    long diffNeg = TimeUtils::timeDifference(t1, t2);
    TEST_ASSERT_EQUAL(-3600, diffNeg);
    TestLog::step("Reverse difference = -3600s");

    long same = TimeUtils::timeDifference(t1, t1);
    TEST_ASSERT_EQUAL(0, same);
    TestLog::step("Same time difference = 0");

    TestLog::testEnd(true);
}

// ========== 同一天判断 ==========

void test_is_same_day() {
    TestLog::testStart("Is Same Day");

    // 构造同一天的两个时间
    time_t t1 = 1700000000;  // 某个时刻
    time_t t2 = TimeUtils::addHours(t1, 5);  // 同日 +5h

    // 注意：是否同一天取决于时区，这里使用系统时区
    // 至少确保跨天的一定不是同一天
    time_t t3 = TimeUtils::addDays(t1, 1);
    TEST_ASSERT_FALSE(TimeUtils::isSameDay(t1, t3));
    TestLog::step("Different days → isSameDay=false");

    // 相同时间一定是同一天
    TEST_ASSERT_TRUE(TimeUtils::isSameDay(t1, t1));
    TestLog::step("Same timestamp → isSameDay=true");

    TestLog::testEnd(true);
}

// ========== 时间范围检查 ==========

void test_is_time_in_range() {
    TestLog::testStart("Is Time In Range");

    time_t start = 1700000000;
    time_t end   = 1700010000;
    time_t mid   = 1700005000;

    TEST_ASSERT_TRUE(TimeUtils::isTimeInRange(mid, start, end));
    TestLog::step("Middle time is in range");

    TEST_ASSERT_TRUE(TimeUtils::isTimeInRange(start, start, end));
    TestLog::step("Start boundary is in range (inclusive)");

    TEST_ASSERT_TRUE(TimeUtils::isTimeInRange(end, start, end));
    TestLog::step("End boundary is in range (inclusive)");

    TEST_ASSERT_FALSE(TimeUtils::isTimeInRange(start - 1, start, end));
    TestLog::step("Before start is out of range");

    TEST_ASSERT_FALSE(TimeUtils::isTimeInRange(end + 1, start, end));
    TestLog::step("After end is out of range");

    TestLog::testEnd(true);
}

// ========== 预定义时区查询 ==========

void test_get_timezone_by_name() {
    TestLog::testStart("Get TimeZone By Name");

    auto utc = TimeUtils::getTimeZoneByName("UTC");
    TEST_ASSERT_EQUAL_STRING("UTC", utc.name.c_str());
    TEST_ASSERT_EQUAL(0, utc.offset);
    TestLog::step("UTC: offset=0");

    auto cst8 = TimeUtils::getTimeZoneByName("CST8");
    TEST_ASSERT_EQUAL(480, cst8.offset);  // 8 * 60
    TestLog::step("CST8: offset=480 minutes");

    auto est = TimeUtils::getTimeZoneByName("EST");
    TEST_ASSERT_EQUAL(-300, est.offset);  // -5 * 60
    TestLog::step("EST: offset=-300 minutes");

    auto cet = TimeUtils::getTimeZoneByName("CET");
    TEST_ASSERT_EQUAL(60, cet.offset);
    TEST_ASSERT_TRUE(cet.dst);
    TestLog::step("CET: offset=60, DST=true");

    // 未知时区回退到 UTC
    auto unknown = TimeUtils::getTimeZoneByName("UNKNOWN");
    TEST_ASSERT_EQUAL_STRING("UTC", unknown.name.c_str());
    TEST_ASSERT_EQUAL(0, unknown.offset);
    TestLog::step("Unknown timezone falls back to UTC");

    TestLog::testEnd(true);
}

// ========== ISO8601 解析 ==========

void test_parse_time_iso8601() {
    TestLog::testStart("Parse Time: ISO8601");

    // 解析一个已知的 ISO8601 时间
    time_t t = TimeUtils::parseTime("2023-11-14T22:13:20Z", TimeUtils::ISO8601);
    TEST_ASSERT_GREATER_THAN(0, (int)t);
    TestLog::step("ISO8601 parse returned valid timestamp");

    // 解析后格式化回去，验证往返一致性
    String formatted = TimeUtils::formatTime(t, TimeUtils::ISO8601);
    TEST_ASSERT_STRING_CONTAINS("2023", formatted.c_str());
    TEST_ASSERT_STRING_CONTAINS("11", formatted.c_str());
    TEST_ASSERT_STRING_CONTAINS("14", formatted.c_str());
    TestLog::step("Round-trip format verified: contains 2023-11-14");

    TestLog::testEnd(true);
}

// ========== 时间格式化各模式 ==========

void test_format_time_modes() {
    TestLog::testStart("Format Time: Multiple Modes");

    time_t t = TimeUtils::parseTime("2023-06-15T08:30:45Z", TimeUtils::ISO8601);
    TEST_ASSERT_GREATER_THAN(0, (int)t);

    String iso = TimeUtils::formatTime(t, TimeUtils::ISO8601);
    TEST_ASSERT_STRING_CONTAINS("T", iso.c_str());
    TEST_ASSERT_STRING_CONTAINS("Z", iso.c_str());
    TestLog::step("ISO8601 contains 'T' and 'Z'");

    String human = TimeUtils::formatTime(t, TimeUtils::HUMAN_READABLE);
    TEST_ASSERT_STRING_CONTAINS("2023", human.c_str());
    TEST_ASSERT_STRING_CONTAINS(":", human.c_str());
    TestLog::step("HUMAN_READABLE contains year and ':'");

    String dateOnly = TimeUtils::formatTime(t, TimeUtils::DATE_ONLY);
    TEST_ASSERT_STRING_CONTAINS("2023", dateOnly.c_str());
    TEST_ASSERT_FALSE(strstr(dateOnly.c_str(), ":") != nullptr);
    TestLog::step("DATE_ONLY contains year but no ':'");

    String timeOnly = TimeUtils::formatTime(t, TimeUtils::TIME_ONLY);
    TEST_ASSERT_STRING_CONTAINS(":", timeOnly.c_str());
    TEST_ASSERT_FALSE(strstr(timeOnly.c_str(), "2023") != nullptr);
    TestLog::step("TIME_ONLY contains ':' but no year");

    TestLog::testEnd(true);
}

// ========== 日起/日终 ==========

void test_day_start_end() {
    TestLog::testStart("Day Start and Day End");

    time_t mid = 1700050000;  // 某个中间时刻

    time_t start = TimeUtils::getDayStart(mid);
    time_t end   = TimeUtils::getDayEnd(mid);

    // 日终 > 日起
    TEST_ASSERT_GREATER_THAN(start, end);
    TestLog::step("getDayEnd > getDayStart");

    // 日起 <= mid <= 日终
    TEST_ASSERT_GREATER_OR_EQUAL(start, mid);
    TEST_ASSERT_LESS_OR_EQUAL(end, mid);
    TestLog::step("getDayStart <= mid <= getDayEnd");

    // 日终 - 日起 ≈ 86399 (23h 59m 59s)
    long span = TimeUtils::timeDifference(end, start);
    TEST_ASSERT_EQUAL(86399, span);
    TestLog::step("Day span = 86399 seconds (23h59m59s)");

    TestLog::testEnd(true);
}

// ========== 星期/月日/年日 ==========

void test_day_of_week_month_year() {
    TestLog::testStart("Day Of Week / Month / Year");

    // 2023-11-14 是星期二 (tm_wday=2)
    time_t t = TimeUtils::parseTime("2023-11-14T12:00:00Z", TimeUtils::ISO8601);
    TEST_ASSERT_GREATER_THAN(0, (int)t);

    int dow = TimeUtils::getDayOfWeek(t);
    TEST_ASSERT_TRUE(dow >= 0 && dow <= 6);
    TestLog::step("Day of week in valid range [0-6]");

    int dom = TimeUtils::getDayOfMonth(t);
    TEST_ASSERT_TRUE(dom >= 1 && dom <= 31);
    TestLog::step("Day of month in valid range [1-31]");

    int doy = TimeUtils::getDayOfYear(t);
    TEST_ASSERT_TRUE(doy >= 1 && doy <= 366);
    TestLog::step("Day of year in valid range [1-366]");

    TestLog::testEnd(true);
}

// ========== 测试组入口 ==========

void test_time_utils_group() {
    TestLog::groupStart("TimeUtils Tests");

    // 闰年
    RUN_TEST(test_leap_year_basic);
    RUN_TEST(test_leap_year_century);

    // 每月天数
    RUN_TEST(test_days_in_month_normal);
    RUN_TEST(test_days_in_month_leap);
    RUN_TEST(test_days_in_month_invalid);

    // 时间格式化
    RUN_TEST(test_format_duration);
    RUN_TEST(test_format_duration_with_ms);
    RUN_TEST(test_format_time_modes);

    // 时间算术
    RUN_TEST(test_time_arithmetic);
    RUN_TEST(test_time_difference);
    RUN_TEST(test_is_same_day);
    RUN_TEST(test_is_time_in_range);

    // 时区
    RUN_TEST(test_get_timezone_by_name);

    // 解析 & 格式化
    RUN_TEST(test_parse_time_iso8601);

    // 日计算
    RUN_TEST(test_day_start_end);
    RUN_TEST(test_day_of_week_month_year);

    TestLog::groupEnd();
}
