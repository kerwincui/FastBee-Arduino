#ifndef RESOURCE_PROFILE_H
#define RESOURCE_PROFILE_H

#include <Arduino.h>
#include "FeatureFlags.h"

namespace FastBee {
namespace ResourceProfile {

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_IDF_TARGET_ESP32C3)
static constexpr const char* NAME = "esp32c3-slim";
static constexpr size_t MAX_PERIPHERALS = 16;             // 硬性上限（受 GPIO/内存限制）
static constexpr size_t MAX_PERIPH_EXEC_RULES = 12;       // 推荐值（软性限制，超限只警告）
static constexpr size_t SENSOR_CACHE_MAX_ENTRIES = 16;
#elif FASTBEE_ENABLE_RULE_SCRIPT || FASTBEE_ENABLE_OTA || FASTBEE_ENABLE_ETHERNET || FASTBEE_ENABLE_CELLULAR || FASTBEE_ENABLE_LORA
static constexpr const char* NAME = "esp32s3-full";
static constexpr size_t MAX_PERIPHERALS = 32;             // 硬性上限
static constexpr size_t MAX_PERIPH_EXEC_RULES = 32;       // 推荐值（软性限制，超限只警告）
static constexpr size_t SENSOR_CACHE_MAX_ENTRIES = 32;
#else
static constexpr const char* NAME = "esp32-slim";
static constexpr size_t MAX_PERIPHERALS = 24;             // 硬性上限
static constexpr size_t MAX_PERIPH_EXEC_RULES = 16;       // 推荐值（软性限制，超限只警告）
static constexpr size_t SENSOR_CACHE_MAX_ENTRIES = 24;
#endif

static constexpr size_t MAX_PERIPHERAL_NAME_LEN = 48;
static constexpr size_t MAX_PERIPHERAL_ID_LEN = 40;
static constexpr size_t MAX_RULE_NAME_LEN = 48;
static constexpr size_t MAX_RULE_ID_LEN = 40;
static constexpr size_t MAX_ACTION_VALUE_LEN = 1024;
static constexpr size_t MAX_SCRIPT_CONTENT_LEN = 2048;

}  // namespace ResourceProfile
}  // namespace FastBee

#endif // RESOURCE_PROFILE_H
