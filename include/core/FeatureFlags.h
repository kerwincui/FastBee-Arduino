/**
 * @file FeatureFlags.h
 * @brief 功能编译开关配置
 * @author kerwincui
 * @date 2025-12-02
 * 
 * 通过条件编译控制各功能的启用/禁用，优化 Flash 占用。
 * 可在platformio.ini 的 build_flags 中配置，或在此修改默认值。
 */

#ifndef FEATURE_FLAGS_H
#define FEATURE_FLAGS_H

// ============================================================================
// 协议功能开关
// ============================================================================

/**
 * @brief MQTT 协议支持
 * 默认：启用
 * 占用：约 15KB Flash
 */
#ifndef FASTBEE_ENABLE_MQTT
#define FASTBEE_ENABLE_MQTT 1
#endif

/**
 * @brief Modbus 协议支持
 * 默认：启用
 * 占用：约 20KB Flash
 */
#ifndef FASTBEE_ENABLE_MODBUS
#define FASTBEE_ENABLE_MODBUS 1
#endif

/**
 * @brief Modbus 从站模式支持
 * 默认：启用（设为0可节省 ~440B RAM + ~10KB Flash）
 * 需要：FASTBEE_ENABLE_MODBUS=1
 */
#ifndef FASTBEE_MODBUS_SLAVE_ENABLE
#define FASTBEE_MODBUS_SLAVE_ENABLE 1
#endif

/**
 * @brief TCP 协议支持
 * 默认：启用
 * 占用：约 10KB Flash
 */
#ifndef FASTBEE_ENABLE_TCP
#define FASTBEE_ENABLE_TCP 1
#endif

/**
 * @brief HTTP 客户端支持
 * 默认：启用
 * 占用：约 8KB Flash
 */
#ifndef FASTBEE_ENABLE_HTTP
#define FASTBEE_ENABLE_HTTP 1
#endif

/**
 * @brief CoAP 协议支持
 * 默认：启用
 * 占用：约 12KB Flash
 * 建议：如不使用可禁用以节省空间
 */
#ifndef FASTBEE_ENABLE_COAP
#define FASTBEE_ENABLE_COAP 1
#endif

// ============================================================================
// 网络功能开关
// ============================================================================

/**
 * @brief mDNS 支持（本地域名访问）
 * 默认：启用
 * 占用：约 5KB Flash
 */
#ifndef FASTBEE_ENABLE_MDNS
#define FASTBEE_ENABLE_MDNS 1
#endif

/**
 * @brief DNS 服务器支持
 * 默认：启用
 * 占用：约 3KB Flash
 */
#ifndef FASTBEE_ENABLE_DNS
#define FASTBEE_ENABLE_DNS 1
#endif

/**
 * @brief AP 模式支持
 * 默认：启用
 * 占用：约 2KB Flash
 */
#ifndef FASTBEE_ENABLE_AP_MODE
#define FASTBEE_ENABLE_AP_MODE 1
#endif

/**
 * @brief BLE 蓝牙配网支持
 * 默认：禁用（功能尚未完全实现）
 * 占用：约 80KB Flash（NimBLE 库）
 * 需要：NimBLE-Arduino 库
 */
#ifndef FASTBEE_ENABLE_BLE
    #define FASTBEE_ENABLE_BLE 0
#endif

// ============================================================================
// Web 服务开关
// ============================================================================

/**
 * @brief Web 管理界面支持
 * 默认：启用
 * 占用：约 25KB Flash（不含静态资源）
 */
#ifndef FASTBEE_ENABLE_WEB_SERVER
#define FASTBEE_ENABLE_WEB_SERVER 1
#endif

/**
 * @brief Web 静态资源服务
 * 默认：启用
 * 需要：FASTBEE_ENABLE_WEB_SERVER=1
 */
#ifndef FASTBEE_ENABLE_WEB_STATIC
#define FASTBEE_ENABLE_WEB_STATIC 1
#endif

/**
 * @brief REST API 支持
 * 默认：启用
 * 需要：FASTBEE_ENABLE_WEB_SERVER=1
 */
#ifndef FASTBEE_ENABLE_WEB_API
#define FASTBEE_ENABLE_WEB_API 1
#endif

/**
 * @brief Web 服务提前启动
 * 默认：启用
 * 说明：为1时，Web服务器在WiFi连接完成前就启动（AP模式下直接可用）；
 *       为0时，保持原有行为（等WiFi连接后再启动Web服务器）
 * 需要：FASTBEE_ENABLE_WEB_SERVER=1
 */
#ifndef FASTBEE_WEB_START_EARLY
#define FASTBEE_WEB_START_EARLY 0
#endif

// ============================================================================
// 安全功能开关
// ============================================================================

/**
 * @brief 用户认证系统
 * 默认：启用
 * 占用：约 8KB Flash
 */
#ifndef FASTBEE_ENABLE_AUTH
#define FASTBEE_ENABLE_AUTH 1
#endif

/**
 * @brief 会话管理
 * 默认：启用
 * 需要：FASTBEE_ENABLE_AUTH=1
 */
#ifndef FASTBEE_ENABLE_SESSION
#define FASTBEE_ENABLE_SESSION 1
#endif

// ============================================================================
// OTA升级开关
// ============================================================================

/**
 * @brief OTA 固件升级
 * 默认：启用
 * 占用：约 10KB Flash
 */
#ifndef FASTBEE_ENABLE_OTA
#define FASTBEE_ENABLE_OTA 1
#endif

/**
 * @brief OTA 文件系统更新
 * 默认：启用
 * 需要：FASTBEE_ENABLE_OTA=1
 */
#ifndef FASTBEE_ENABLE_OTA_FS
#define FASTBEE_ENABLE_OTA_FS 1
#endif

// ============================================================================
// 系统服务开关
// ============================================================================

/**
 * @brief 任务调度器
 * 默认：启用
 * 占用：约 5KB Flash
 */
#ifndef FASTBEE_ENABLE_TASK_MANAGER
#define FASTBEE_ENABLE_TASK_MANAGER 1
#endif

/**
 * @brief 健康监控
 * 默认：启用
 * 占用：约 4KB Flash
 */
#ifndef FASTBEE_ENABLE_HEALTH_MONITOR
#define FASTBEE_ENABLE_HEALTH_MONITOR 1
#endif

/**
 * @brief 日志系统
 * 默认：启用
 * 占用：约 6KB Flash
 */
#ifndef FASTBEE_ENABLE_LOGGER
#define FASTBEE_ENABLE_LOGGER 1
#endif

/**
 * @brief 外设执行规则系统
 * 默认：启用
 * 占用：约 8KB Flash
 * 说明：包含外设执行定时器、设备触发轮询、按键事件检测等
 */
#ifndef FASTBEE_ENABLE_PERIPH_EXEC
    #define FASTBEE_ENABLE_PERIPH_EXEC 1
#endif

/**
 * @brief 规则脚本系统（RuleScript + ScriptEngine）
 * 默认：启用
 * 占用：约 8KB Flash
 * 说明：包含规则管理、模板引擎、命令序列脚本执行
 */
#ifndef FASTBEE_ENABLE_RULE_SCRIPT
  #define FASTBEE_ENABLE_RULE_SCRIPT 1
#endif

/**
 * @brief GPIO 管理
 * 默认：启用
 * 占用：约 3KB Flash
 */
#ifndef FASTBEE_ENABLE_GPIO
#define FASTBEE_ENABLE_GPIO 1
#endif

/**
 * @brief LED 屏幕支持（WS2812B/APA102 等 NeoPixel）
 * 默认：禁用
 * 占用：约 15KB Flash（驱动库~10KB + 管理器~5KB）
 * RAM：3 bytes × 像素数（帧缓冲区）+ ~200B 管理器开销
 * 需要：Adafruit NeoPixel 库
 * 说明：使用 ESP32 RMT 硬件外设发送时序，不占用 CPU
 */
#ifndef FASTBEE_ENABLE_LED_SCREEN
#define FASTBEE_ENABLE_LED_SCREEN 0
#endif

/**
 * @brief LCD/OLED 显示屏支持（SSD1306/SH1106 等）
 * 默认：禁用（需硬件支持）
 * 占用：约 20KB Flash（U8g2 库 ~15KB + 管理器 ~5KB）
 * RAM：1KB 帧缓冲区 + ~200B 管理器开销
 * 需要：U8g2 库
 * 说明：独立于 LED_SCREEN（NeoPixel），用于 I2C/SPI 接口的字符/图形显示屏
 */
#ifndef FASTBEE_ENABLE_LCD
    #define FASTBEE_ENABLE_LCD 0
#endif

// ============================================================================
// 调试功能开关
// ============================================================================

/**
 * @brief 调试日志输出
 * 默认：启用（DEBUG 版本）
 * 建议：发布版本禁用
 */
#ifndef FASTBEE_DEBUG_LOG
#ifdef DEBUG
#define FASTBEE_DEBUG_LOG 1
#else
#define FASTBEE_DEBUG_LOG 0
#endif
#endif

/**
 * @brief 剥离 INFO 级别日志字符串
 * 默认：禁用（保留 INFO 日志）
 * 建议：minimal 配置启用以节省 Flash
 * 节省：约 2-5KB Flash（取决于 INFO 日志数量）
 */
#ifndef FASTBEE_STRIP_INFO_LOGS
#define FASTBEE_STRIP_INFO_LOGS 0
#endif

/**
 * @brief 详细错误信息
 * 默认：启用（DEBUG 版本）
 * 建议：发布版本禁用以节省空间
 */
#ifndef FASTBEE_VERBOSE_ERROR
#ifdef DEBUG
#define FASTBEE_VERBOSE_ERROR 1
#else
#define FASTBEE_VERBOSE_ERROR 0
#endif
#endif

// ============================================================================
// 性能优化选项
// ============================================================================

/**
 * @brief 使用 PSRAM
 * 默认：启用
 * 说明：ESP32 带 PSRAM 的设备可启用
 */
#ifndef FASTBEE_USE_PSRAM
#define FASTBEE_USE_PSRAM 1
#endif

/**
 * @brief 日志缓冲区大小（字节）
 * 默认：256
 * 说明：减小可节省 RAM，但可能影响长日志
 */
#ifndef FASTBEE_LOG_BUFFER_SIZE
#define FASTBEE_LOG_BUFFER_SIZE 256
#endif

/**
 * @brief JSON 文档最大大小（字节）
 * 默认：4096
 * 说明：根据实际需求调整
 * 注意：此大小用于 StaticJsonDocument，在栈上分配，避免堆内存碎片
 */
#ifndef FASTBEE_JSON_DOC_SIZE
#define FASTBEE_JSON_DOC_SIZE 4096
#endif

/**
 * @brief 大型 JSON 文档大小（用于配置文件加载等）
 * 默认：8192
 * 说明：用于加载较大的配置文件如 peripherals.json
 */
#ifndef FASTBEE_JSON_DOC_SIZE_LARGE
#define FASTBEE_JSON_DOC_SIZE_LARGE 8192
#endif

/**
 * @brief 安全的 JSON 文档类型
 * ArduinoJson v7 中 JsonDocument 自动管理内存，无需模板参数
 * FASTBEE_JSON_DOC_SIZE 宏仅保留供业务层手动分配缓冲区时参考
 */
#include <ArduinoJson.h>
using FastBeeJsonDoc = JsonDocument;
using FastBeeJsonDocLarge = JsonDocument;

// ============================================================================
// 配置缓存
// ============================================================================

/**
 * @brief ConfigStorage 内存缓存层
 * 默认：启用
 * 说明：避免重复文件 I/O 和 JSON 反序列化，含 LRU 淘汰和 debounce 写入
 */
#ifndef FASTBEE_ENABLE_STORAGE_CACHE
    #define FASTBEE_ENABLE_STORAGE_CACHE 1
#endif

#endif // FEATURE_FLAGS_H
