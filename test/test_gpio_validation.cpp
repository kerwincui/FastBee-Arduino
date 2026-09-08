/**
 * @file test_gpio_validation.cpp
 * @brief GPIO 冲突与引脚安全单元测试
 *
 * 测试内容（基于 ChipConfig.h 的硬件抽象层）：
 * - 各芯片保留引脚列表完整性
 * - 保留引脚不可分配给用户外设
 * - ESP32 GPIO 34-39 仅输入限制
 * - 各芯片 MAX_GPIO 边界值
 * - 双引脚设备引脚不可重叠
 * - 同一引脚不可同时分配给多个外设
 */

#include <unity.h>
#include <Arduino.h>
#include "helpers/TestLogger.h"
#include <vector>
#include <set>
#include <algorithm>

void test_gpio_validation_group();

// ========== 镜像 ChipConfig.h 硬件常量 ==========

struct ChipProfile {
    const char* name;
    uint8_t maxGpio;
    bool hasDac;
    bool hasTouch;
    uint8_t maxPwmCh;
    bool dualCore;
    uint8_t uartCount;
    std::vector<uint8_t> reservedPins;
    std::vector<uint8_t> inputOnlyPins;
};

static ChipProfile getESP32Profile() {
    return {"ESP32", 39, true, true, 16, true, 3,
            {0, 1, 3, 6, 7, 8, 9, 10, 11},
            {34, 35, 36, 39}};
}

// S3 无 PSRAM 变体（F8R0 等）；带 PSRAM 的 F8R8/F16R8 使用 Octal 保留脚
static ChipProfile getESP32S3Profile() {
    return {"ESP32-S3", 48, false, true, 8, true, 3,
            {0, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32},
            {}};
}

// S3 Octal PSRAM 变体（F16R8：CONFIG_SPIRAM_MODE_OCT）
// GPIO 33-37 被 PSRAM 总线独占（SPIIO4-7/SPIDQS），必须列为保留引脚
static ChipProfile getESP32S3OctalProfile() {
    return {"ESP32-S3-OCT", 48, false, true, 8, true, 3,
            {0, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37},
            {}};
}

static ChipProfile getESP32C3Profile() {
    return {"ESP32-C3", 21, false, false, 6, false, 2,
            {11, 12, 13, 14, 15, 16, 17},
            {}};
}

// C6 实际可用 GPIO 仅 0-23（24-30 为封装内部 Flash/PSRAM SPI，无外部焊盘）；
// 15/16/17 保留：GPIO16/17 = UART0 控制台 TX/RX，对其 pinMode 会切断串口输出，
// 设备看似挂死（2026-08-05 实测）。与 ChipConfig.h CHIP_RESERVED_PIN_COUNT=13 一致。
static ChipProfile getESP32C6Profile() {
    return {"ESP32-C6", 23, false, false, 6, false, 2,
            {12, 13, 14, 15, 16, 17, 24, 25, 26, 27, 28, 29, 30},
            {}};
}

// 验证引脚是否可分配（非保留且非仅输入）
// 注意：maxGpio 是最大合法 GPIO 编号（含），与生产代码 PeripheralManager::isValidPin
// 的 `pin > CHIP_MAX_GPIO` 语义一致，因此 pin == maxGpio 仍是合法引脚（如 ESP32 GPIO39）。
static bool isPinAssignable(const ChipProfile& chip, uint8_t pin) {
    if (pin > chip.maxGpio) return false;
    for (uint8_t r : chip.reservedPins) {
        if (r == pin) return false;
    }
    return true;
}

// 验证引脚是否可作输出
static bool isPinOutputCapable(const ChipProfile& chip, uint8_t pin) {
    if (!isPinAssignable(chip, pin)) return false;
    for (uint8_t io : chip.inputOnlyPins) {
        if (io == pin) return false;
    }
    return true;
}

// 镜像生产代码 PeripheralManager::isValidPin 的 S3 绝对禁止区
// （与保留列表不同：isReservedPin 仅拦输出类型，isValidPin 对任何类型绝对禁止）
static bool isS3PinAbsolutelyRejected(bool octalPsram, uint8_t pin) {
    if (pin >= 19 && pin <= 20) return true;
    if (octalPsram) {
        return (pin >= 22 && pin <= 37);
    }
    return (pin >= 22 && pin <= 32);
}

// 检测引脚冲突（两个设备列表是否有重叠）
static bool hasPinConflict(const std::vector<uint8_t>& device1Pins,
                            const std::vector<uint8_t>& device2Pins) {
    for (uint8_t p1 : device1Pins) {
        for (uint8_t p2 : device2Pins) {
            if (p1 == p2) return true;
        }
    }
    return false;
}

// ============================================================
// TEST GROUP 1: 芯片保留引脚列表完整性
// ============================================================

static void test_esp32_reserved_count() {
    auto chip = getESP32Profile();
    TEST_ASSERT_EQUAL(9, (int)chip.reservedPins.size());
}

static void test_esp32s3_reserved_count() {
    auto chip = getESP32S3Profile();
    TEST_ASSERT_EQUAL(14, (int)chip.reservedPins.size());
}

static void test_esp32s3_octal_reserved_count() {
    // 与 ChipConfig.h OCT 分支的 CHIP_RESERVED_PIN_COUNT = 19 保持一致
    auto chip = getESP32S3OctalProfile();
    TEST_ASSERT_EQUAL(19, (int)chip.reservedPins.size());
}

static void test_esp32c3_reserved_count() {
    auto chip = getESP32C3Profile();
    TEST_ASSERT_EQUAL(7, (int)chip.reservedPins.size());
}

static void test_esp32c6_reserved_count() {
    auto chip = getESP32C6Profile();
    TEST_ASSERT_EQUAL(13, (int)chip.reservedPins.size());
}

static void test_esp32_reserved_has_flash_spi() {
    // ESP32 GPIO 6-11 = Flash SPI
    auto chip = getESP32Profile();
    for (uint8_t pin : {6, 7, 8, 9, 10, 11}) {
        bool found = false;
        for (uint8_t r : chip.reservedPins) {
            if (r == pin) { found = true; break; }
        }
        TEST_ASSERT_TRUE_MESSAGE(found, "Flash SPI pin must be reserved");
    }
}

static void test_esp32_reserved_has_uart0() {
    // ESP32 GPIO 1/3 = UART0
    auto chip = getESP32Profile();
    bool has1 = false, has3 = false;
    for (uint8_t r : chip.reservedPins) {
        if (r == 1) has1 = true;
        if (r == 3) has3 = true;
    }
    TEST_ASSERT_TRUE(has1);
    TEST_ASSERT_TRUE(has3);
}

static void test_esp32s3_reserved_has_usb() {
    // S3 GPIO 19/20 = USB
    auto chip = getESP32S3Profile();
    bool has19 = false, has20 = false;
    for (uint8_t r : chip.reservedPins) {
        if (r == 19) has19 = true;
        if (r == 20) has20 = true;
    }
    TEST_ASSERT_TRUE(has19);
    TEST_ASSERT_TRUE(has20);
}

static void test_esp32c6_reserved_has_uart0_console() {
    // C6 UART0 控制台 TX=GPIO16 / RX=GPIO17（CDC_ON_BOOT=0 时）。
    // 对 GPIO16 做 pinMode(OUTPUT) 会切断串口 TX，后续日志全部丢失，
    // 设备看似挂死（2026-08-05 实测挂死根因），必须保留。
    auto chip = getESP32C6Profile();
    bool has16 = false, has17 = false;
    for (uint8_t r : chip.reservedPins) {
        if (r == 16) has16 = true;
        if (r == 17) has17 = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(has16, "C6 UART0 TX (GPIO16) must be reserved");
    TEST_ASSERT_TRUE_MESSAGE(has17, "C6 UART0 RX (GPIO17) must be reserved");
    TEST_ASSERT_FALSE(isPinAssignable(chip, 16));
    TEST_ASSERT_FALSE(isPinAssignable(chip, 17));
}

static void test_esp32c6_reserved_has_usb() {
    // C6 GPIO 12/13 = USB D-/D+
    auto chip = getESP32C6Profile();
    bool has12 = false, has13 = false;
    for (uint8_t r : chip.reservedPins) {
        if (r == 12) has12 = true;
        if (r == 13) has13 = true;
    }
    TEST_ASSERT_TRUE(has12);
    TEST_ASSERT_TRUE(has13);
}

static void test_esp32c6_max_gpio_boundary() {
    auto chip = getESP32C6Profile();  // maxGpio=23
    // GPIO 22/23 为合法引脚（23 == maxGpio，与生产 isValidPin 语义一致）
    TEST_ASSERT_TRUE(isPinAssignable(chip, 22));
    TEST_ASSERT_TRUE(isPinAssignable(chip, 23));
    // GPIO 24-30 为封装内 Flash/PSRAM SPI：既超出 maxGpio 又在保留列表
    for (uint8_t pin = 24; pin <= 30; pin++) {
        TEST_ASSERT_FALSE_MESSAGE(isPinAssignable(chip, pin),
            "C6 GPIO24-30 (package Flash/PSRAM SPI) must be rejected");
    }
}

static void test_esp32s3_octal_reserved_has_psram_bus() {
    // Octal PSRAM 总线 GPIO 33-37 必须在保留列表（真机事故回归保护：
    // 配置 pin 33/34/35 通过校验后驱动 pinMode/analogRead 踩坏 PSRAM 总线，
    // 导致随机内存损坏崩溃循环）
    auto chip = getESP32S3OctalProfile();
    for (uint8_t pin : {33, 34, 35, 36, 37}) {
        bool found = false;
        for (uint8_t r : chip.reservedPins) {
            if (r == pin) { found = true; break; }
        }
        TEST_ASSERT_TRUE_MESSAGE(found, "Octal PSRAM bus pin must be reserved");
    }
}

static void test_esp32s3_non_octal_psram_pins_not_reserved() {
    // 无 PSRAM 变体（F8R0）GPIO 33-37 是普通可用引脚，不应误伤
    auto chip = getESP32S3Profile();
    for (uint8_t pin : {33, 34, 35, 36, 37}) {
        TEST_ASSERT_TRUE_MESSAGE(isPinAssignable(chip, pin),
            "Non-octal S3 must keep GPIO 33-37 assignable");
    }
}

static void test_esp32c3_reserved_has_flash() {
    // C3 GPIO 11 = Flash VDD, 12-17 = Flash SPI
    auto chip = getESP32C3Profile();
    for (uint8_t pin = 11; pin <= 17; pin++) {
        bool found = false;
        for (uint8_t r : chip.reservedPins) {
            if (r == pin) { found = true; break; }
        }
        TEST_ASSERT_TRUE_MESSAGE(found, "C3 Flash pin must be reserved");
    }
}

// ============================================================
// TEST GROUP 2: 保留引脚不可分配
// ============================================================

static void test_esp32_reserved_not_assignable() {
    auto chip = getESP32Profile();
    for (uint8_t r : chip.reservedPins) {
        TEST_ASSERT_FALSE_MESSAGE(isPinAssignable(chip, r),
            "Reserved pin must not be assignable");
    }
}

static void test_esp32s3_reserved_not_assignable() {
    auto chip = getESP32S3Profile();
    for (uint8_t r : chip.reservedPins) {
        TEST_ASSERT_FALSE_MESSAGE(isPinAssignable(chip, r),
            "Reserved pin must not be assignable");
    }
}

static void test_esp32s3_octal_reserved_not_assignable() {
    auto chip = getESP32S3OctalProfile();
    for (uint8_t r : chip.reservedPins) {
        TEST_ASSERT_FALSE_MESSAGE(isPinAssignable(chip, r),
            "Octal PSRAM reserved pin must not be assignable");
    }
}

// ============================================================
// TEST GROUP 2b: S3 isValidPin 绝对禁止区（镜像生产逻辑）
// ============================================================

static void test_s3_octal_absolute_reject_psram_bus() {
    // OCT 下 GPIO 22-37 全部绝对禁止（含输入类型，如 ADC/IR_RECEIVER）
    for (uint8_t pin = 22; pin <= 37; pin++) {
        TEST_ASSERT_TRUE_MESSAGE(isS3PinAbsolutelyRejected(true, pin),
            "Octal S3 must absolutely reject GPIO 22-37 for any type");
    }
}

static void test_s3_octal_absolute_reject_boundaries() {
    // 边界：GPIO 21 和 GPIO 38 在 OCT 下仍可用
    TEST_ASSERT_FALSE(isS3PinAbsolutelyRejected(true, 21));
    TEST_ASSERT_FALSE(isS3PinAbsolutelyRejected(true, 38));
    // GPIO 22 和 GPIO 37 是禁止区两端
    TEST_ASSERT_TRUE(isS3PinAbsolutelyRejected(true, 22));
    TEST_ASSERT_TRUE(isS3PinAbsolutelyRejected(true, 37));
}

static void test_s3_non_octal_absolute_reject_boundaries() {
    // 非 OCT：禁止区止于 GPIO 32，GPIO 33-37 合法
    TEST_ASSERT_TRUE(isS3PinAbsolutelyRejected(false, 32));
    for (uint8_t pin = 33; pin <= 37; pin++) {
        TEST_ASSERT_FALSE_MESSAGE(isS3PinAbsolutelyRejected(false, pin),
            "Non-octal S3 must accept GPIO 33-37");
    }
}

static void test_s3_usb_pins_rejected_both_variants() {
    // GPIO 19/20 = USB，两种变体都禁止
    for (bool oct : {false, true}) {
        TEST_ASSERT_TRUE(isS3PinAbsolutelyRejected(oct, 19));
        TEST_ASSERT_TRUE(isS3PinAbsolutelyRejected(oct, 20));
    }
}

static void test_s3_accident_pins_rejected_under_octal() {
    // 真机事故回归：ESP32 夹具的 GPIO 32/33/34/35 刷到 S3-F16R8 后
    // 必须全部被 isValidPin 拦住（事故前 33/34/35 可通过校验）
    for (uint8_t pin : {32, 33, 34, 35}) {
        TEST_ASSERT_TRUE_MESSAGE(isS3PinAbsolutelyRejected(true, pin),
            "Accident fixture pins must be rejected on octal S3");
    }
}

static void test_esp32c3_reserved_not_assignable() {
    auto chip = getESP32C3Profile();
    for (uint8_t r : chip.reservedPins) {
        TEST_ASSERT_FALSE_MESSAGE(isPinAssignable(chip, r),
            "Reserved pin must not be assignable");
    }
}

static void test_esp32c6_reserved_not_assignable() {
    auto chip = getESP32C6Profile();
    for (uint8_t r : chip.reservedPins) {
        TEST_ASSERT_FALSE_MESSAGE(isPinAssignable(chip, r),
            "Reserved pin must not be assignable");
    }
}

// ============================================================
// TEST GROUP 3: ESP32 GPIO 34-39 仅输入限制
// ============================================================

static void test_esp32_input_only_count() {
    auto chip = getESP32Profile();
    TEST_ASSERT_EQUAL(4, (int)chip.inputOnlyPins.size());
}

static void test_esp32_input_only_pins_values() {
    auto chip = getESP32Profile();
    std::vector<uint8_t> expected = {34, 35, 36, 39};
    TEST_ASSERT_EQUAL(expected.size(), chip.inputOnlyPins.size());
    for (size_t i = 0; i < expected.size(); i++) {
        TEST_ASSERT_EQUAL(expected[i], chip.inputOnlyPins[i]);
    }
}

static void test_esp32_input_only_not_output_capable() {
    auto chip = getESP32Profile();
    for (uint8_t pin : chip.inputOnlyPins) {
        TEST_ASSERT_FALSE_MESSAGE(isPinOutputCapable(chip, pin),
            "Input-only pin must not be output capable");
    }
}

static void test_esp32_input_only_still_assignable_for_input() {
    // 仅输入引脚仍然是"可分配"的（用作输入外设如按钮、ADC）
    auto chip = getESP32Profile();
    for (uint8_t pin : chip.inputOnlyPins) {
        TEST_ASSERT_TRUE_MESSAGE(isPinAssignable(chip, pin),
            "Input-only pin should still be assignable");
    }
}

static void test_s3_no_input_only_pins() {
    auto chip = getESP32S3Profile();
    TEST_ASSERT_EQUAL(0, (int)chip.inputOnlyPins.size());
}

static void test_c3_no_input_only_pins() {
    auto chip = getESP32C3Profile();
    TEST_ASSERT_EQUAL(0, (int)chip.inputOnlyPins.size());
}

// ============================================================
// TEST GROUP 4: MAX_GPIO 边界值
// ============================================================

static void test_esp32_max_gpio() {
    auto chip = getESP32Profile();
    TEST_ASSERT_EQUAL(39, chip.maxGpio);
}

static void test_esp32s3_max_gpio() {
    auto chip = getESP32S3Profile();
    TEST_ASSERT_EQUAL(48, chip.maxGpio);
}

static void test_esp32c3_max_gpio() {
    auto chip = getESP32C3Profile();
    TEST_ASSERT_EQUAL(21, chip.maxGpio);
}

static void test_esp32c6_max_gpio() {
    auto chip = getESP32C6Profile();
    TEST_ASSERT_EQUAL(23, chip.maxGpio);
}

static void test_pin_at_max_gpio_boundary() {
    auto chip = getESP32C3Profile();  // maxGpio=21
    // GPIO 20 应可分配（如果不在保留列表中）
    TEST_ASSERT_TRUE(isPinAssignable(chip, 20));
    // GPIO 21 应可分配（== maxGpio，为最大合法编号，与生产 isValidPin 语义一致）
    TEST_ASSERT_TRUE(isPinAssignable(chip, 21));
    // GPIO 22 不可分配（> maxGpio）
    TEST_ASSERT_FALSE(isPinAssignable(chip, 22));
}

// ============================================================
// TEST GROUP 5: 双引脚设备引脚不可重叠
// ============================================================

static void test_ultrasonic_trig_echo_must_differ() {
    // 超声波 HC-SR04：trigPin != echoPin
    std::vector<uint8_t> trig = {4};
    std::vector<uint8_t> echo = {4};  // 同一个引脚 → 冲突
    TEST_ASSERT_TRUE(hasPinConflict(trig, echo));
}

static void test_ultrasonic_different_pins_ok() {
    std::vector<uint8_t> trig = {4};
    std::vector<uint8_t> echo = {5};
    TEST_ASSERT_FALSE(hasPinConflict(trig, echo));
}

static void test_tm1637_clk_dio_must_differ() {
    // TM1637：CLK != DIO
    std::vector<uint8_t> clk = {21};
    std::vector<uint8_t> dio = {21};
    TEST_ASSERT_TRUE(hasPinConflict(clk, dio));
}

static void test_i2c_sda_scl_must_differ() {
    // I2C：SDA != SCL
    std::vector<uint8_t> sda = {21};
    std::vector<uint8_t> scl = {22};
    TEST_ASSERT_FALSE(hasPinConflict(sda, scl));
}

static void test_i2c_sda_scl_same_pin_conflict() {
    std::vector<uint8_t> sda = {21};
    std::vector<uint8_t> scl = {21};
    TEST_ASSERT_TRUE(hasPinConflict(sda, scl));
}

// ============================================================
// TEST GROUP 6: 同一引脚不可分配给多个外设
// ============================================================

static void test_multi_device_pin_conflict() {
    // 设备 A 使用引脚 4, 5
    std::vector<uint8_t> deviceA = {4, 5};
    // 设备 B 使用引脚 5, 6
    std::vector<uint8_t> deviceB = {5, 6};
    TEST_ASSERT_TRUE(hasPinConflict(deviceA, deviceB));
}

static void test_multi_device_no_conflict() {
    std::vector<uint8_t> deviceA = {4, 5};
    std::vector<uint8_t> deviceB = {12, 13};
    TEST_ASSERT_FALSE(hasPinConflict(deviceA, deviceB));
}

static void test_multi_device_complete_overlap() {
    std::vector<uint8_t> deviceA = {4, 5, 6};
    std::vector<uint8_t> deviceB = {4, 5, 6};
    TEST_ASSERT_TRUE(hasPinConflict(deviceA, deviceB));
}

static void test_pin_pool_allocation() {
    // 模拟引脚池分配：已用集合 vs 新请求
    std::set<uint8_t> usedPins = {2, 4, 5, 12, 13, 14, 15, 21, 22};
    std::vector<uint8_t> newRequest = {25, 26, 27};

    bool conflict = false;
    for (uint8_t p : newRequest) {
        if (usedPins.count(p) > 0) {
            conflict = true;
            break;
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(conflict, "New pins should not conflict with used pool");
}

static void test_pin_pool_conflict_detected() {
    std::set<uint8_t> usedPins = {2, 4, 5, 12, 13};
    std::vector<uint8_t> newRequest = {13, 14};  // 13 冲突

    bool conflict = false;
    for (uint8_t p : newRequest) {
        if (usedPins.count(p) > 0) {
            conflict = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(conflict);
}

// ============================================================
// TEST GROUP 7: 芯片能力标志
// ============================================================

static void test_esp32_has_dac() {
    auto chip = getESP32Profile();
    TEST_ASSERT_TRUE(chip.hasDac);
}

static void test_esp32s3_no_dac() {
    auto chip = getESP32S3Profile();
    TEST_ASSERT_FALSE(chip.hasDac);
}

static void test_esp32c3_no_dac_no_touch() {
    auto chip = getESP32C3Profile();
    TEST_ASSERT_FALSE(chip.hasDac);
    TEST_ASSERT_FALSE(chip.hasTouch);
}

static void test_esp32_dual_core() {
    auto chip = getESP32Profile();
    TEST_ASSERT_TRUE(chip.dualCore);
}

static void test_esp32c3_single_core() {
    auto chip = getESP32C3Profile();
    TEST_ASSERT_FALSE(chip.dualCore);
}

static void test_uart_count_esp32() {
    auto chip = getESP32Profile();
    TEST_ASSERT_EQUAL(3, chip.uartCount);
}

static void test_uart_count_c3() {
    auto chip = getESP32C3Profile();
    TEST_ASSERT_EQUAL(2, chip.uartCount);
}

// ============================================================
// TEST GROUP 8: 保留引脚无重复
// ============================================================

static void check_reserved_pins_no_duplicates(const ChipProfile& chip) {
    std::set<uint8_t> unique(chip.reservedPins.begin(), chip.reservedPins.end());
    TEST_ASSERT_EQUAL(chip.reservedPins.size(), unique.size());
}

static void test_esp32_reserved_no_duplicates() {
    check_reserved_pins_no_duplicates(getESP32Profile());
}

static void test_esp32s3_reserved_no_duplicates() {
    check_reserved_pins_no_duplicates(getESP32S3Profile());
}

static void test_esp32s3_octal_reserved_no_duplicates() {
    check_reserved_pins_no_duplicates(getESP32S3OctalProfile());
}

static void test_esp32c3_reserved_no_duplicates() {
    check_reserved_pins_no_duplicates(getESP32C3Profile());
}

static void test_esp32c6_reserved_no_duplicates() {
    check_reserved_pins_no_duplicates(getESP32C6Profile());
}

static void test_reserved_pins_sorted_ascending() {
    // 保留引脚列表应按升序排列（便于二分查找）
    auto chip = getESP32Profile();
    for (size_t i = 1; i < chip.reservedPins.size(); i++) {
        TEST_ASSERT_GREATER_OR_EQUAL(chip.reservedPins[i-1], chip.reservedPins[i]);
    }
}

// ============================================================
// 主入口
// ============================================================

void test_gpio_validation_group() {
    TestLog::groupStart("GPIO Validation Tests");

    // 保留引脚数量
    RUN_TEST(test_esp32_reserved_count);
    RUN_TEST(test_esp32s3_reserved_count);
    RUN_TEST(test_esp32s3_octal_reserved_count);
    RUN_TEST(test_esp32c3_reserved_count);
    RUN_TEST(test_esp32c6_reserved_count);
    RUN_TEST(test_esp32_reserved_has_flash_spi);
    RUN_TEST(test_esp32_reserved_has_uart0);
    RUN_TEST(test_esp32s3_reserved_has_usb);
    RUN_TEST(test_esp32c6_reserved_has_uart0_console);
    RUN_TEST(test_esp32c6_reserved_has_usb);
    RUN_TEST(test_esp32s3_octal_reserved_has_psram_bus);
    RUN_TEST(test_esp32s3_non_octal_psram_pins_not_reserved);
    RUN_TEST(test_esp32c3_reserved_has_flash);

    // 保留引脚不可分配
    RUN_TEST(test_esp32_reserved_not_assignable);
    RUN_TEST(test_esp32s3_reserved_not_assignable);
    RUN_TEST(test_esp32s3_octal_reserved_not_assignable);
    RUN_TEST(test_esp32c3_reserved_not_assignable);
    RUN_TEST(test_esp32c6_reserved_not_assignable);

    // S3 isValidPin 绝对禁止区（Octal PSRAM 防变砖回归保护）
    RUN_TEST(test_s3_octal_absolute_reject_psram_bus);
    RUN_TEST(test_s3_octal_absolute_reject_boundaries);
    RUN_TEST(test_s3_non_octal_absolute_reject_boundaries);
    RUN_TEST(test_s3_usb_pins_rejected_both_variants);
    RUN_TEST(test_s3_accident_pins_rejected_under_octal);

    // 仅输入引脚
    RUN_TEST(test_esp32_input_only_count);
    RUN_TEST(test_esp32_input_only_pins_values);
    RUN_TEST(test_esp32_input_only_not_output_capable);
    RUN_TEST(test_esp32_input_only_still_assignable_for_input);
    RUN_TEST(test_s3_no_input_only_pins);
    RUN_TEST(test_c3_no_input_only_pins);

    // MAX_GPIO 边界
    RUN_TEST(test_esp32_max_gpio);
    RUN_TEST(test_esp32s3_max_gpio);
    RUN_TEST(test_esp32c3_max_gpio);
    RUN_TEST(test_esp32c6_max_gpio);
    RUN_TEST(test_esp32c6_max_gpio_boundary);
    RUN_TEST(test_pin_at_max_gpio_boundary);

    // 双引脚设备冲突
    RUN_TEST(test_ultrasonic_trig_echo_must_differ);
    RUN_TEST(test_ultrasonic_different_pins_ok);
    RUN_TEST(test_tm1637_clk_dio_must_differ);
    RUN_TEST(test_i2c_sda_scl_must_differ);
    RUN_TEST(test_i2c_sda_scl_same_pin_conflict);

    // 多外设引脚冲突
    RUN_TEST(test_multi_device_pin_conflict);
    RUN_TEST(test_multi_device_no_conflict);
    RUN_TEST(test_multi_device_complete_overlap);
    RUN_TEST(test_pin_pool_allocation);
    RUN_TEST(test_pin_pool_conflict_detected);

    // 芯片能力
    RUN_TEST(test_esp32_has_dac);
    RUN_TEST(test_esp32s3_no_dac);
    RUN_TEST(test_esp32c3_no_dac_no_touch);
    RUN_TEST(test_esp32_dual_core);
    RUN_TEST(test_esp32c3_single_core);
    RUN_TEST(test_uart_count_esp32);
    RUN_TEST(test_uart_count_c3);

    // 保留引脚无重复
    RUN_TEST(test_esp32_reserved_no_duplicates);
    RUN_TEST(test_esp32s3_reserved_no_duplicates);
    RUN_TEST(test_esp32s3_octal_reserved_no_duplicates);
    RUN_TEST(test_esp32c3_reserved_no_duplicates);
    RUN_TEST(test_esp32c6_reserved_no_duplicates);
    RUN_TEST(test_reserved_pins_sorted_ascending);

    TestLog::groupEnd();
}
