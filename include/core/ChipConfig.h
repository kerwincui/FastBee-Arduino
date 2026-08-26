#pragma once
/**
 * ChipConfig.h - ESP32 多芯片硬件抽象层
 * 
 * 集中定义各 ESP32 芯片型号的硬件差异常量。
 * 使用 ESP-IDF 内置宏 CONFIG_IDF_TARGET_* 进行芯片识别。
 */

// 确保 CONFIG_IDF_TARGET_* / CONFIG_SPIRAM_MODE_* 宏可见（native 测试环境无此文件）
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

// ========== 芯片能力定义 ==========

#if defined(CONFIG_IDF_TARGET_ESP32)
  #define CHIP_MAX_GPIO       39
  #define CHIP_HAS_DAC        1
  #define CHIP_HAS_TOUCH      1
  #define CHIP_MAX_PWM_CH     16
  #define CHIP_DUAL_CORE      1
  #define CHIP_UART_COUNT     3
  #define CHIP_HAS_CLASSIC_BT 1
  #define CHIP_NAME           "ESP32"
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  #define CHIP_MAX_GPIO       48
  #define CHIP_HAS_DAC        0
  #define CHIP_HAS_TOUCH      1
  #define CHIP_MAX_PWM_CH     8
  #define CHIP_DUAL_CORE      1
  #define CHIP_UART_COUNT     3
  #define CHIP_HAS_CLASSIC_BT 0
  #define CHIP_NAME           "ESP32-S3"
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  #define CHIP_MAX_GPIO       21
  #define CHIP_HAS_DAC        0
  #define CHIP_HAS_TOUCH      0
  #define CHIP_MAX_PWM_CH     6
  #define CHIP_DUAL_CORE      0
  #define CHIP_UART_COUNT     2
  #define CHIP_HAS_CLASSIC_BT 0
  #define CHIP_NAME           "ESP32-C3"
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  // C6 实际可用 GPIO 仅 0-23（24-30 为封装内部 Flash/PSRAM SPI，无外部焊盘）；
  // 旧值 30 会让 GPIO25-27 通过校验，对其寄存器操作直接触发 panic（2026-08-05 实测）。
  #define CHIP_MAX_GPIO       23
  #define CHIP_HAS_DAC        0
  #define CHIP_HAS_TOUCH      0
  #define CHIP_MAX_PWM_CH     6
  #define CHIP_DUAL_CORE      0
  #define CHIP_UART_COUNT     2
  #define CHIP_HAS_CLASSIC_BT 0
  #define CHIP_NAME           "ESP32-C6"
#else
  // 默认兼容经典 ESP32
  #define CHIP_MAX_GPIO       39
  #define CHIP_HAS_DAC        1
  #define CHIP_HAS_TOUCH      1
  #define CHIP_MAX_PWM_CH     16
  #define CHIP_DUAL_CORE      1
  #define CHIP_UART_COUNT     3
  #define CHIP_HAS_CLASSIC_BT 1
  #define CHIP_NAME           "ESP32"
#endif

// ========== Web 预留内存阈值（8BIT DRAM 口径，按芯片 DRAM 基线差异化） ==========
// 各芯片 DRAM 总量不同，开机基线差异大，统一阈值会导致：
//   - S3(512KB DRAM, 基线~44KB)：阈值 12KB 太接近基线，频繁触发振荡
//   - C3(272KB DRAM, 基线~20KB)：阈值 12KB 太宽松，余量不足时规则切片导致 503
// 滞回设计（suspend < resume）防边界振荡；业务代码禁止再按 CONFIG_IDF_TARGET 自行派生。
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
// C3/C6: 基线 free8≈18-22KB，留 ~8KB 余量
#define CHIP_WEB_RESERVE_SUSPEND_FREE_HEAP_BYTES   10240U  // free8 < 10KB 暂停
#define CHIP_WEB_RESERVE_SUSPEND_LARGEST_BLOCK     6144U   // blk8 < 6KB 暂停
#define CHIP_WEB_RESERVE_RESUME_FREE_HEAP_BYTES    14336U  // free8 ≥ 14KB 恢复
#define CHIP_WEB_RESERVE_RESUME_LARGEST_BLOCK      10240U  // 且 blk8 ≥ 10KB
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
// S3: 基线 free8≈40-50KB，阈值宽松，避免小波动触发振荡
#define CHIP_WEB_RESERVE_SUSPEND_FREE_HEAP_BYTES   16384U  // free8 < 16KB 暂停
#define CHIP_WEB_RESERVE_SUSPEND_LARGEST_BLOCK     12288U  // blk8 < 12KB 暂停
#define CHIP_WEB_RESERVE_RESUME_FREE_HEAP_BYTES    20480U  // free8 ≥ 20KB 恢复
#define CHIP_WEB_RESERVE_RESUME_LARGEST_BLOCK      16384U  // 且 blk8 ≥ 16KB
#else
// ESP32 经典: 基线 free8≈25-28KB
#define CHIP_WEB_RESERVE_SUSPEND_FREE_HEAP_BYTES   12288U  // free8 < 12KB 暂停
#define CHIP_WEB_RESERVE_SUSPEND_LARGEST_BLOCK     8192U   // blk8 < 8KB 暂停
#define CHIP_WEB_RESERVE_RESUME_FREE_HEAP_BYTES    16384U  // free8 ≥ 16KB 恢复
#define CHIP_WEB_RESERVE_RESUME_LARGEST_BLOCK      12288U  // 且 blk8 ≥ 12KB
#endif

// ========== 保留引脚定义 ==========
// 这些引脚被 Flash SPI、调试串口等内部功能占用，不应分配给用户外设

#if defined(CONFIG_IDF_TARGET_ESP32)
  // ESP32: GPIO 6-11 = Flash SPI, GPIO 1/3 = UART0
  static const uint8_t CHIP_RESERVED_PINS[] = { 0, 1, 3, 6, 7, 8, 9, 10, 11 };
  static const uint8_t CHIP_RESERVED_PIN_COUNT = 9;
  // ESP32: GPIO 34-39 仅支持输入
  static const uint8_t CHIP_INPUT_ONLY_PINS[] = { 34, 35, 36, 39 };
  static const uint8_t CHIP_INPUT_ONLY_PIN_COUNT = 4;
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  // ESP32-S3: GPIO 19/20 = USB, GPIO 26-32 = Flash SPI
  #if defined(CONFIG_SPIRAM_MODE_OCT)
    // Octal PSRAM (R8): GPIO 33-37 被 PSRAM 总线独占（SPIIO4-7/SPIDQS），
    // 对其 pinMode/analogRead 会踩坏 PSRAM 总线导致随机内存损坏
    static const uint8_t CHIP_RESERVED_PINS[] = { 0, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37 };
    static const uint8_t CHIP_RESERVED_PIN_COUNT = 19;
  #else
    static const uint8_t CHIP_RESERVED_PINS[] = { 0, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 };
    static const uint8_t CHIP_RESERVED_PIN_COUNT = 14;
  #endif
  static const uint8_t CHIP_INPUT_ONLY_PINS[] = {};
  static const uint8_t CHIP_INPUT_ONLY_PIN_COUNT = 0;
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  // ESP32-C3: GPIO 11 = Flash VDD, GPIO 12-17 = Flash SPI (部分封装)
  static const uint8_t CHIP_RESERVED_PINS[] = { 11, 12, 13, 14, 15, 16, 17 };
  static const uint8_t CHIP_RESERVED_PIN_COUNT = 7;
  static const uint8_t CHIP_INPUT_ONLY_PINS[] = {};
  static const uint8_t CHIP_INPUT_ONLY_PIN_COUNT = 0;
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  // ESP32-C6: GPIO12/13 = USB D-/D+；GPIO16/17 = UART0 控制台 TX/RX
  // （对 16 做 pinMode 会切断串口 TX，后续日志全部丢失，设备看似挂死，2026-08-05 实测）；
  // GPIO24-30 = 封装内 Flash/PSRAM SPI。内嵌 Flash 变体（FH4）的 Flash SPI 占用 12-17。
  static const uint8_t CHIP_RESERVED_PINS[] = { 12, 13, 14, 15, 16, 17, 24, 25, 26, 27, 28, 29, 30 };
  static const uint8_t CHIP_RESERVED_PIN_COUNT = 13;
  static const uint8_t CHIP_INPUT_ONLY_PINS[] = {};
  static const uint8_t CHIP_INPUT_ONLY_PIN_COUNT = 0;
#else
  static const uint8_t CHIP_RESERVED_PINS[] = { 0, 1, 3, 6, 7, 8, 9, 10, 11 };
  static const uint8_t CHIP_RESERVED_PIN_COUNT = 9;
  static const uint8_t CHIP_INPUT_ONLY_PINS[] = { 34, 35, 36, 39 };
  static const uint8_t CHIP_INPUT_ONLY_PIN_COUNT = 4;
#endif

// ========== 绝对禁止引脚（Flash SPI / PSRAM 总线，硬件级禁区） ==========
// 与 CHIP_RESERVED_PINS（软保留，可被配置层放行）不同，这里列出的范围
// 任何方向的使用都会损坏 Flash/PSRAM 总线或切断控制台，业务代码一律拒绝。
// 统一收口于此，业务源码禁止再按 CONFIG_IDF_TARGET 自行派生范围。
static inline bool chipIsAbsoluteForbiddenPin(uint8_t pin) {
#if defined(CONFIG_IDF_TARGET_ESP32)
    // ESP32: GPIO 6-11 = 内部 Flash SPI
    return (pin >= 6 && pin <= 11);
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    // GPIO 19/20 = USB；26-32 = Flash SPI；Octal PSRAM (R8) 下 33-37
    // 被 PSRAM 总线独占，任何方向的使用都会导致内存损坏
  #if defined(CONFIG_SPIRAM_MODE_OCT)
    return (pin >= 19 && pin <= 20) || (pin >= 22 && pin <= 37);
  #else
    return (pin >= 19 && pin <= 20) || (pin >= 22 && pin <= 32);
  #endif
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
    // GPIO 12-17 = Flash SPI（内嵌 Flash 封装）
    return (pin >= 12 && pin <= 17);
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    // GPIO 12-17：内嵌 Flash 变体（FH4）的 Flash SPI；16/17 同时是 UART0 TX/RX；
    // 24-30 = 封装内 Flash/PSRAM SPI，寄存器操作直接 panic
    return (pin >= 12 && pin <= 17) || (pin >= 24 && pin <= 30);
#else
    return (pin >= 6 && pin <= 11);
#endif
}

// ========== 触摸引脚定义 ==========

#if CHIP_HAS_TOUCH
  #if defined(CONFIG_IDF_TARGET_ESP32)
    // ESP32 触摸引脚: T0=GPIO4, T1=GPIO0, T2=GPIO2, T3=GPIO15, T4=GPIO13, T5=GPIO12, T6=GPIO14, T7=GPIO27, T8=GPIO33, T9=GPIO32
    static const uint8_t CHIP_TOUCH_PINS[] = { 0, 2, 4, 12, 13, 14, 15, 27, 32, 33 };
    static const uint8_t CHIP_TOUCH_PIN_COUNT = 10;
  #elif defined(CONFIG_IDF_TARGET_ESP32S3)
    // ESP32-S3 触摸引脚: T1=GPIO1 ~ T14=GPIO14
    static const uint8_t CHIP_TOUCH_PINS[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };
    static const uint8_t CHIP_TOUCH_PIN_COUNT = 14;
  #else
    // 默认兼容经典 ESP32（含 native 测试环境）
    static const uint8_t CHIP_TOUCH_PINS[] = { 0, 2, 4, 12, 13, 14, 15, 27, 32, 33 };
    static const uint8_t CHIP_TOUCH_PIN_COUNT = 10;
  #endif
#else
  static const uint8_t CHIP_TOUCH_PINS[] = {};
  static const uint8_t CHIP_TOUCH_PIN_COUNT = 0;
#endif
