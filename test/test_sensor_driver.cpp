/**
 * @file test_sensor_driver.cpp
 * @brief SensorDriver 传感器驱动单元测试
 *
 * 测试内容（纯逻辑，不依赖硬件）：
 * - 引脚有效性校验（sensor_pin_valid）
 * - 读取间隔缓存逻辑（DHT 2s / DS18B20 1s / Ultrasonic 100ms / ADC 200ms）
 * - ADC 校准公式（电流/电压）
 * - 超声波距离公式与范围校验
 * - 传感器类型枚举值固定
 */

#include <unity.h>
#include <Arduino.h>
#include "helpers/TestLogger.h"
#include <cmath>
#include <climits>

void test_sensor_driver_group();

// ========== 镜像 sensor_pin_valid 逻辑 ==========
// 来自 SensorDriver.cpp：
//   if (pin == 255) return false;
//   if (pin > GPIO_NUM_MAX - 1) return false;
//   return GPIO_IS_VALID_GPIO(pin);
// native 环境 GPIO_NUM_MAX 定义为 48（ESP32-S3 最大）

#ifndef GPIO_NUM_MAX
#define GPIO_NUM_MAX 48
#endif

// 镜像：简化版 GPIO 有效性（忽略芯片差异，只验证数值边界）
static bool mirror_sensor_pin_valid(uint8_t pin) {
    if (pin == 255) return false;
    if (pin > GPIO_NUM_MAX - 1) return false;
    return true;  // 假设数值在范围内的引脚都是合法的
}

// ========== 镜像传感器类型枚举 ==========
enum class MirrorSensorDriverType : uint8_t {
    DHT11 = 0,
    DHT22 = 1,
    DS18B20 = 2,
    ULTRASONIC = 3,
    CURRENT = 4,
    VOLTAGE = 5
};

// ========== 镜像缓存间隔常量 ==========
namespace SensorIntervals {
    constexpr unsigned long DHT_MIN_INTERVAL_MS = 2000;
    constexpr unsigned long DS18B20_MIN_INTERVAL_MS = 1000;
    constexpr unsigned long ULTRASONIC_MIN_INTERVAL_MS = 100;
    constexpr unsigned long ADC_SENSOR_MIN_INTERVAL_MS = 200;
    // 读取失败时的旧缓存有效期
    constexpr unsigned long STALE_CACHE_WINDOW_MS = 10000;
}

// ========== 镜像 ADC 校准参数 ==========
struct MirrorADCCalibration {
    float vRef = 3.3f;
    float sensitivity = 0.100f;  // V/A（ACS712-20A）
    float offset = 1.65f;        // V（VCC/2）
    float ratio = 1.0f;          // 分压比
    uint16_t adcMax = 4095;      // 12-bit
};

// ============================================================
// TEST GROUP 1: 引脚有效性校验
// ============================================================

static void test_pin_valid_normal_pin() {
    TEST_ASSERT_TRUE(mirror_sensor_pin_valid(2));
    TEST_ASSERT_TRUE(mirror_sensor_pin_valid(4));
    TEST_ASSERT_TRUE(mirror_sensor_pin_valid(15));
    TEST_ASSERT_TRUE(mirror_sensor_pin_valid(25));
}

static void test_pin_valid_pin_255_rejected() {
    // 255 是"未配置"标志，必须拒绝
    TEST_ASSERT_FALSE(mirror_sensor_pin_valid(255));
}

static void test_pin_valid_pin_zero_accepted() {
    // GPIO 0 在大多数 ESP32 上是合法的（虽然用于 boot 模式）
    TEST_ASSERT_TRUE(mirror_sensor_pin_valid(0));
}

static void test_pin_valid_max_gpio_accepted() {
    // GPIO_NUM_MAX - 1 = 47（S3 最大引脚号）
    TEST_ASSERT_TRUE(mirror_sensor_pin_valid(GPIO_NUM_MAX - 1));
}

static void test_pin_valid_over_max_gpio_rejected() {
    // GPIO_NUM_MAX = 48 及以上应被拒绝
    TEST_ASSERT_FALSE(mirror_sensor_pin_valid(GPIO_NUM_MAX));
    TEST_ASSERT_FALSE(mirror_sensor_pin_valid(GPIO_NUM_MAX + 10));
}

// ============================================================
// TEST GROUP 2: 传感器类型枚举值固定
// ============================================================

static void test_sensor_type_dht11_value() {
    TEST_ASSERT_EQUAL(0, static_cast<int>(MirrorSensorDriverType::DHT11));
}

static void test_sensor_type_dht22_value() {
    TEST_ASSERT_EQUAL(1, static_cast<int>(MirrorSensorDriverType::DHT22));
}

static void test_sensor_type_ds18b20_value() {
    TEST_ASSERT_EQUAL(2, static_cast<int>(MirrorSensorDriverType::DS18B20));
}

static void test_sensor_type_ultrasonic_value() {
    TEST_ASSERT_EQUAL(3, static_cast<int>(MirrorSensorDriverType::ULTRASONIC));
}

static void test_sensor_type_current_value() {
    TEST_ASSERT_EQUAL(4, static_cast<int>(MirrorSensorDriverType::CURRENT));
}

static void test_sensor_type_voltage_value() {
    TEST_ASSERT_EQUAL(5, static_cast<int>(MirrorSensorDriverType::VOLTAGE));
}

// ============================================================
// TEST GROUP 3: 缓存间隔常量
// ============================================================

static void test_interval_dht_2s() {
    TEST_ASSERT_EQUAL(2000UL, SensorIntervals::DHT_MIN_INTERVAL_MS);
}

static void test_interval_ds18b20_1s() {
    TEST_ASSERT_EQUAL(1000UL, SensorIntervals::DS18B20_MIN_INTERVAL_MS);
}

static void test_interval_ultrasonic_100ms() {
    TEST_ASSERT_EQUAL(100UL, SensorIntervals::ULTRASONIC_MIN_INTERVAL_MS);
}

static void test_interval_adc_200ms() {
    TEST_ASSERT_EQUAL(200UL, SensorIntervals::ADC_SENSOR_MIN_INTERVAL_MS);
}

static void test_stale_cache_window_10s() {
    TEST_ASSERT_EQUAL(10000UL, SensorIntervals::STALE_CACHE_WINDOW_MS);
}

// ============================================================
// TEST GROUP 4: 缓存命中/失效逻辑（模拟时间戳判断）
// ============================================================

// 模拟缓存结构
struct MockSensorCache {
    bool success = false;
    float value = NAN;
    unsigned long timestamp = 0;
};

static bool isCacheValid(const MockSensorCache& cache, unsigned long now,
                          unsigned long intervalMs) {
    return cache.success && (now - cache.timestamp) < intervalMs;
}

static bool isStaleCacheUsable(const MockSensorCache& cache, unsigned long now) {
    return cache.success && (now - cache.timestamp) < SensorIntervals::STALE_CACHE_WINDOW_MS;
}

static void test_cache_hit_within_interval() {
    MockSensorCache cache{true, 25.5f, 1000};
    // 间隔 2000ms，当前时间 2500ms，差距 1500ms < 2000ms → 命中
    TEST_ASSERT_TRUE(isCacheValid(cache, 2500, SensorIntervals::DHT_MIN_INTERVAL_MS));
}

static void test_cache_miss_after_interval() {
    MockSensorCache cache{true, 25.5f, 1000};
    // 间隔 2000ms，当前时间 3500ms，差距 2500ms >= 2000ms → 未命中
    TEST_ASSERT_FALSE(isCacheValid(cache, 3500, SensorIntervals::DHT_MIN_INTERVAL_MS));
}

static void test_cache_miss_when_not_success() {
    MockSensorCache cache{false, NAN, 1000};
    TEST_ASSERT_FALSE(isCacheValid(cache, 1500, SensorIntervals::DHT_MIN_INTERVAL_MS));
}

static void test_stale_cache_usable_within_10s() {
    MockSensorCache cache{true, 25.5f, 1000};
    // 当前时间 8000ms，差距 7000ms，超出 2000ms 间隔但在 10000ms 旧缓存窗口内
    TEST_ASSERT_FALSE(isCacheValid(cache, 8000, SensorIntervals::DHT_MIN_INTERVAL_MS));
    TEST_ASSERT_TRUE(isStaleCacheUsable(cache, 8000));
}

static void test_stale_cache_expired_after_10s() {
    MockSensorCache cache{true, 25.5f, 1000};
    // 当前时间 12000ms，差距 11000ms > 10000ms → 旧缓存也失效
    TEST_ASSERT_FALSE(isStaleCacheUsable(cache, 12000));
}

static void test_cache_ds18b20_interval_boundary() {
    MockSensorCache cache{true, 22.3f, 5000};
    // 正好在 1000ms 边界
    TEST_ASSERT_FALSE(isCacheValid(cache, 6000, SensorIntervals::DS18B20_MIN_INTERVAL_MS));
    // 在 999ms 内
    TEST_ASSERT_TRUE(isCacheValid(cache, 5999, SensorIntervals::DS18B20_MIN_INTERVAL_MS));
}

// ============================================================
// TEST GROUP 5: ADC 校准公式
// ============================================================

// 镜像电流计算：I = (V - Voffset) / sensitivity
static float mirrorCalcCurrent(float adcRaw, const MirrorADCCalibration& cal) {
    float voltage = adcRaw * cal.vRef / cal.adcMax;
    if (cal.sensitivity > 0.001f) {
        return (voltage - cal.offset) / cal.sensitivity;
    }
    return 0.0f;
}

// 镜像电压计算：actualV = measuredV * ratio
static float mirrorCalcVoltage(float adcRaw, const MirrorADCCalibration& cal) {
    float measuredVoltage = adcRaw * cal.vRef / cal.adcMax;
    return measuredVoltage * cal.ratio;
}

static void test_adc_current_zero_current() {
    // ACS712: offset=1.65V，当 ADC 读到 1.65V 时电流为 0
    MirrorADCCalibration cal;
    // ADC raw 对应 1.65V: 1.65 / 3.3 * 4095 ≈ 2048
    float raw = 1.65f / 3.3f * 4095.0f;
    float current = mirrorCalcCurrent(raw, cal);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, current);
}

static void test_adc_current_positive_1a() {
    // ACS712-20A: sensitivity=0.100 V/A
    // 1A → voltage = 1.65 + 1.0 * 0.100 = 1.75V
    MirrorADCCalibration cal;
    float raw = 1.75f / 3.3f * 4095.0f;
    float current = mirrorCalcCurrent(raw, cal);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f, current);
}

static void test_adc_current_negative_1a() {
    // -1A → voltage = 1.65 + (-1.0) * 0.100 = 1.55V
    MirrorADCCalibration cal;
    float raw = 1.55f / 3.3f * 4095.0f;
    float current = mirrorCalcCurrent(raw, cal);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, -1.0f, current);
}

static void test_adc_current_sensitivity_zero_protected() {
    // sensitivity 接近 0 时应返回 0，不崩溃
    MirrorADCCalibration cal;
    cal.sensitivity = 0.0f;
    float current = mirrorCalcCurrent(2048.0f, cal);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, current);
}

static void test_adc_voltage_no_divider() {
    // ratio=1.0（无分压），ADC 读 1.65V → 实际 1.65V
    MirrorADCCalibration cal;
    float raw = 1.65f / 3.3f * 4095.0f;
    float voltage = mirrorCalcVoltage(raw, cal);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 1.65f, voltage);
}

static void test_adc_voltage_with_divider() {
    // 分压比 ratio=5.0（R1=30k, R2=7.5k）
    // ADC 读 1.0V → 实际 5.0V
    MirrorADCCalibration cal;
    cal.ratio = 5.0f;
    float raw = 1.0f / 3.3f * 4095.0f;
    float voltage = mirrorCalcVoltage(raw, cal);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 5.0f, voltage);
}

static void test_adc_voltage_full_scale() {
    // ADC 满量程 4095 → 3.3V，ratio=1
    MirrorADCCalibration cal;
    float voltage = mirrorCalcVoltage(4095.0f, cal);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.3f, voltage);
}

static void test_adc_voltage_zero() {
    MirrorADCCalibration cal;
    float voltage = mirrorCalcVoltage(0.0f, cal);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, voltage);
}

// ============================================================
// TEST GROUP 6: 超声波距离公式
// ============================================================

// 镜像超声波距离计算：distance = pulseDuration * 0.034 / 2
static float mirrorUltrasonicDistance(unsigned long durationUs) {
    return (float)durationUs * 0.034f / 2.0f;
}

// 镜像范围校验：有效 2~400cm
static bool mirrorUltrasonicRangeValid(float distanceCm) {
    return distanceCm >= 2.0f && distanceCm <= 400.0f;
}

static void test_ultrasonic_10cm() {
    // 10cm: duration = 10 * 2 / 0.034 ≈ 588 us
    float dist = mirrorUltrasonicDistance(588);
    TEST_ASSERT_FLOAT_WITHIN(0.2f, 10.0f, dist);
    TEST_ASSERT_TRUE(mirrorUltrasonicRangeValid(dist));
}

static void test_ultrasonic_100cm() {
    // 100cm: duration = 100 * 2 / 0.034 ≈ 5882 us
    float dist = mirrorUltrasonicDistance(5882);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 100.0f, dist);
    TEST_ASSERT_TRUE(mirrorUltrasonicRangeValid(dist));
}

static void test_ultrasonic_400cm_max() {
    // 400cm: duration = 400 * 2 / 0.034 ≈ 23529 us
    float dist = mirrorUltrasonicDistance(23529);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 400.0f, dist);
    TEST_ASSERT_TRUE(mirrorUltrasonicRangeValid(dist));
}

static void test_ultrasonic_below_min_rejected() {
    // 1cm: duration ≈ 59 us → distance ≈ 1.0cm < 2cm → 拒绝
    float dist = mirrorUltrasonicDistance(59);
    TEST_ASSERT_FALSE(mirrorUltrasonicRangeValid(dist));
}

static void test_ultrasonic_over_max_rejected() {
    // 500cm: duration ≈ 29412 us → distance ≈ 500cm > 400cm → 拒绝
    float dist = mirrorUltrasonicDistance(29412);
    TEST_ASSERT_FALSE(mirrorUltrasonicRangeValid(dist));
}

static void test_ultrasonic_timeout_returns_zero() {
    // pulseIn 超时返回 0 → distance = 0
    float dist = mirrorUltrasonicDistance(0);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, dist);
    TEST_ASSERT_FALSE(mirrorUltrasonicRangeValid(dist));  // 0cm < 2cm → 无效
}

static void test_ultrasonic_boundary_2cm() {
    // 正好 2cm 边界
    float dist = mirrorUltrasonicDistance(118);  // 2*2/0.034 ≈ 117.6
    TEST_ASSERT_FLOAT_WITHIN(0.2f, 2.0f, dist);
    TEST_ASSERT_TRUE(mirrorUltrasonicRangeValid(dist));
}

// ============================================================
// TEST GROUP 7: DHT 字段选择逻辑
// ============================================================

static float mirrorDHTFieldSelect(float temp, float humi, const String& field) {
    if (field == "humidity") return humi;
    return temp;
}

static void test_dht_field_temperature() {
    float val = mirrorDHTFieldSelect(25.5f, 60.0f, "temperature");
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.5f, val);
}

static void test_dht_field_humidity() {
    float val = mirrorDHTFieldSelect(25.5f, 60.0f, "humidity");
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, val);
}

static void test_dht_field_unknown_returns_temperature() {
    // 未知字段默认返回温度（与生产代码一致）
    float val = mirrorDHTFieldSelect(25.5f, 60.0f, "unknown");
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.5f, val);
}

// ============================================================
// TEST GROUP 8: ADC 校准默认值
// ============================================================

static void test_adc_calibration_defaults() {
    MirrorADCCalibration cal;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.3f, cal.vRef);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.100f, cal.sensitivity);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.65f, cal.offset);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, cal.ratio);
    TEST_ASSERT_EQUAL(4095, cal.adcMax);
}

static void test_adc_calibration_custom_acs712_30a() {
    // ACS712-30A: sensitivity = 0.066 V/A
    MirrorADCCalibration cal;
    cal.sensitivity = 0.066f;
    float raw = 1.75f / 3.3f * 4095.0f;
    float current = mirrorCalcCurrent(raw, cal);
    // I = (1.75 - 1.65) / 0.066 ≈ 1.515A
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 1.515f, current);
}

// ============================================================
// 主入口
// ============================================================

void test_sensor_driver_group() {
    TestLog::groupStart("SensorDriver Tests");

    // 引脚有效性
    RUN_TEST(test_pin_valid_normal_pin);
    RUN_TEST(test_pin_valid_pin_255_rejected);
    RUN_TEST(test_pin_valid_pin_zero_accepted);
    RUN_TEST(test_pin_valid_max_gpio_accepted);
    RUN_TEST(test_pin_valid_over_max_gpio_rejected);

    // 传感器类型枚举
    RUN_TEST(test_sensor_type_dht11_value);
    RUN_TEST(test_sensor_type_dht22_value);
    RUN_TEST(test_sensor_type_ds18b20_value);
    RUN_TEST(test_sensor_type_ultrasonic_value);
    RUN_TEST(test_sensor_type_current_value);
    RUN_TEST(test_sensor_type_voltage_value);

    // 缓存间隔常量
    RUN_TEST(test_interval_dht_2s);
    RUN_TEST(test_interval_ds18b20_1s);
    RUN_TEST(test_interval_ultrasonic_100ms);
    RUN_TEST(test_interval_adc_200ms);
    RUN_TEST(test_stale_cache_window_10s);

    // 缓存命中/失效
    RUN_TEST(test_cache_hit_within_interval);
    RUN_TEST(test_cache_miss_after_interval);
    RUN_TEST(test_cache_miss_when_not_success);
    RUN_TEST(test_stale_cache_usable_within_10s);
    RUN_TEST(test_stale_cache_expired_after_10s);
    RUN_TEST(test_cache_ds18b20_interval_boundary);

    // ADC 校准公式
    RUN_TEST(test_adc_current_zero_current);
    RUN_TEST(test_adc_current_positive_1a);
    RUN_TEST(test_adc_current_negative_1a);
    RUN_TEST(test_adc_current_sensitivity_zero_protected);
    RUN_TEST(test_adc_voltage_no_divider);
    RUN_TEST(test_adc_voltage_with_divider);
    RUN_TEST(test_adc_voltage_full_scale);
    RUN_TEST(test_adc_voltage_zero);
    RUN_TEST(test_adc_calibration_defaults);
    RUN_TEST(test_adc_calibration_custom_acs712_30a);

    // 超声波距离
    RUN_TEST(test_ultrasonic_10cm);
    RUN_TEST(test_ultrasonic_100cm);
    RUN_TEST(test_ultrasonic_400cm_max);
    RUN_TEST(test_ultrasonic_below_min_rejected);
    RUN_TEST(test_ultrasonic_over_max_rejected);
    RUN_TEST(test_ultrasonic_timeout_returns_zero);
    RUN_TEST(test_ultrasonic_boundary_2cm);

    // DHT 字段选择
    RUN_TEST(test_dht_field_temperature);
    RUN_TEST(test_dht_field_humidity);
    RUN_TEST(test_dht_field_unknown_returns_temperature);

    TestLog::groupEnd();
}
