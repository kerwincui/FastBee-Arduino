#pragma once
/**
 * ChipConfig.h - ESP32 多芯片硬件抽象层
 * 
 * 集中定义各 ESP32 芯片型号的硬件差异常量。
 * 使用 ESP-IDF 内置宏 CONFIG_IDF_TARGET_* 进行芯片识别。
 */

// ========== 芯片能力定义 ==========

#if defined(CONFIG_IDF_TARGET_ESP32)
  #define CHIP_MAX_GPIO       39
  #define CHIP_HAS_DAC        1
  #define CHIP_HAS_TOUCH      1
  #define CHIP_MAX_PWM_CH     16
  #define CHIP_DUAL_CORE      1
  #define CHIP_UART_COUNT     3
  #define CHIP_NAME           "ESP32"
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  #define CHIP_MAX_GPIO       48
  #define CHIP_HAS_DAC        0
  #define CHIP_HAS_TOUCH      1
  #define CHIP_MAX_PWM_CH     8
  #define CHIP_DUAL_CORE      1
  #define CHIP_UART_COUNT     3
  #define CHIP_NAME           "ESP32-S3"
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  #define CHIP_MAX_GPIO       21
  #define CHIP_HAS_DAC        0
  #define CHIP_HAS_TOUCH      0
  #define CHIP_MAX_PWM_CH     6
  #define CHIP_DUAL_CORE      0
  #define CHIP_UART_COUNT     2
  #define CHIP_NAME           "ESP32-C3"
#else
  // 默认兼容经典 ESP32
  #define CHIP_MAX_GPIO       39
  #define CHIP_HAS_DAC        1
  #define CHIP_HAS_TOUCH      1
  #define CHIP_MAX_PWM_CH     16
  #define CHIP_DUAL_CORE      1
  #define CHIP_UART_COUNT     3
  #define CHIP_NAME           "ESP32"
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
  // ESP32-S3: GPIO 19/20 = USB, GPIO 26-32 = Octal Flash/PSRAM (部分板子)
  static const uint8_t CHIP_RESERVED_PINS[] = { 0, 19, 20, 26, 27, 28, 29, 30, 31, 32 };
  static const uint8_t CHIP_RESERVED_PIN_COUNT = 10;
  static const uint8_t CHIP_INPUT_ONLY_PINS[] = {};
  static const uint8_t CHIP_INPUT_ONLY_PIN_COUNT = 0;
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  // ESP32-C3: GPIO 11 = Flash VDD, GPIO 12-17 = Flash SPI (部分封装)
  static const uint8_t CHIP_RESERVED_PINS[] = { 11, 12, 13, 14, 15, 16, 17 };
  static const uint8_t CHIP_RESERVED_PIN_COUNT = 7;
  static const uint8_t CHIP_INPUT_ONLY_PINS[] = {};
  static const uint8_t CHIP_INPUT_ONLY_PIN_COUNT = 0;
#else
  static const uint8_t CHIP_RESERVED_PINS[] = { 0, 1, 3, 6, 7, 8, 9, 10, 11 };
  static const uint8_t CHIP_RESERVED_PIN_COUNT = 9;
  static const uint8_t CHIP_INPUT_ONLY_PINS[] = { 34, 35, 36, 39 };
  static const uint8_t CHIP_INPUT_ONLY_PIN_COUNT = 4;
#endif

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
  #endif
#else
  static const uint8_t CHIP_TOUCH_PINS[] = {};
  static const uint8_t CHIP_TOUCH_PIN_COUNT = 0;
#endif
