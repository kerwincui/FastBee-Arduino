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
 * @brief GPIO 管理
 * 默认：启用
 * 占用：约 3KB Flash
 */
#ifndef FASTBEE_ENABLE_GPIO
#define FASTBEE_ENABLE_GPIO 1
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
 * 使用 StaticJsonDocument 避免堆内存分配失败导致崩溃
 * ArduinoJson v7 中推荐使用此类型替代 JsonDocument
 */
#include <ArduinoJson.h>
using FastBeeJsonDoc = StaticJsonDocument<FASTBEE_JSON_DOC_SIZE>;
using FastBeeJsonDocLarge = StaticJsonDocument<FASTBEE_JSON_DOC_SIZE_LARGE>;

#endif // FEATURE_FLAGS_H
