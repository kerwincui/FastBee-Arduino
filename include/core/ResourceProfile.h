#ifndef RESOURCE_PROFILE_H
#define RESOURCE_PROFILE_H

#include <Arduino.h>
#include "ChipConfig.h"
#include "FeatureFlags.h"

namespace FastBee {
namespace ResourceProfile {

#if defined(CONFIG_IDF_TARGET_ESP32C3)
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

// --- TCP 连接预算（按芯片硬件差异化，详见 docs/tcp-connection-budget.md）---
// 硬约束：MEMP_NUM_TCP_PCB=16，浏览器单 host 最多 6 并发
// 预算 = 设计目标并发数（不含 MQTT/TIME_WAIT）
#if defined(CONFIG_IDF_TARGET_ESP32C3)
// C3: 400KB SRAM，可用堆~140KB，4×12KB=48KB(34%)
static constexpr size_t TCP_TOTAL_BUDGET  = 4;
static constexpr size_t TCP_SSE_BUDGET    = 1;
static constexpr size_t TCP_HTTP_BUDGET   = 3;
static constexpr uint16_t TCP_CONN_EXHAUSTION_THRESHOLD = 10;  // C3: 内存紧张，早触发
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
// S3: 512KB + PSRAM 卸载，可用堆~310KB，支持多标签页
static constexpr size_t TCP_TOTAL_BUDGET  = 8;
static constexpr size_t TCP_SSE_BUDGET    = 2;
static constexpr size_t TCP_HTTP_BUDGET   = 6;
static constexpr uint16_t TCP_CONN_EXHAUSTION_THRESHOLD = 14;  // S3: 内存充裕，容忍更多
#else
// ESP32 classic / C6: ~220KB 可用堆，单用户场景
static constexpr size_t TCP_TOTAL_BUDGET  = 6;
static constexpr size_t TCP_SSE_BUDGET    = 1;
static constexpr size_t TCP_HTTP_BUDGET   = 5;
static constexpr uint16_t TCP_CONN_EXHAUSTION_THRESHOLD = 12;  // classic/C6: 标准值
#endif

// 编译期断言：耗尽阈值必须 < lwIP 硬上限 16
static_assert(TCP_CONN_EXHAUSTION_THRESHOLD < 16,
              "TCP_CONN_EXHAUSTION_THRESHOLD must be < MEMP_NUM_TCP_PCB(16)");
// 编译期断言：SSE 预算不超过 TCP 总预算
static_assert(TCP_SSE_BUDGET <= TCP_TOTAL_BUDGET,
              "TCP_SSE_BUDGET must not exceed TCP_TOTAL_BUDGET");

static constexpr size_t MAX_PERIPHERAL_NAME_LEN = 48;
static constexpr size_t MAX_PERIPHERAL_ID_LEN = 40;
static constexpr size_t MAX_RULE_NAME_LEN = 48;
static constexpr size_t MAX_RULE_ID_LEN = 40;
static constexpr size_t MAX_ACTION_VALUE_LEN = 1024;
static constexpr size_t MAX_SCRIPT_CONTENT_LEN = 2048;

}  // namespace ResourceProfile
}  // namespace FastBee

#endif // RESOURCE_PROFILE_H
