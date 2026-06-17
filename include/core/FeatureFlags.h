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
#define FASTBEE_MODBUS_SLAVE_ENABLE 0
#endif

/**
 * @brief TCP 协议支持
 * 默认：启用
 * 占用：约 10KB Flash
 */
#ifndef FASTBEE_ENABLE_TCP
#define FASTBEE_ENABLE_TCP 0
#endif

/**
 * @brief HTTP 客户端支持
 * 默认：启用
 * 占用：约 8KB Flash
 */
#ifndef FASTBEE_ENABLE_HTTP
#define FASTBEE_ENABLE_HTTP 0
#endif

/**
 * @brief CoAP 协议支持
 * 默认：启用
 * 占用：约 12KB Flash
 * 建议：如不使用可禁用以节省空间
 */
#ifndef FASTBEE_ENABLE_COAP
#define FASTBEE_ENABLE_COAP 0
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
 * @brief 以太网 W5500 SPI 支持
 * 默认：禁用（Standard/Full 构建启用；Lite 为稳定性默认关闭）
 * 占用：约 8KB Flash
 * 需要：硬件连接 W5500 芯片到 SPI2 接口
 */
#ifndef FASTBEE_ENABLE_ETHERNET
    #define FASTBEE_ENABLE_ETHERNET 0
#endif

/**
 * @brief 4G 蜂窝模块支持 (EC801E-CN)
 * 默认：禁用（Standard/Full 构建启用；Lite 为稳定性默认关闭）
 * 占用：约 15KB Flash
 * 需要：硬件连接 EC801E-CN 模块到 UART2
 * 依赖：TinyGSM 库
 */
#ifndef FASTBEE_ENABLE_CELLULAR
    #define FASTBEE_ENABLE_CELLULAR 0
#endif

/**
 * @brief LoRa 网关透传支持 (E22-400T22D)
 * 默认：禁用（仅 ESP32-S3 full 构建启用）
 * 占用：约 5KB Flash
 * 需要：硬件连接 E22-400T22D 模块到 UART
 */
#ifndef FASTBEE_ENABLE_LORA
    #define FASTBEE_ENABLE_LORA 0
#endif

/**
 * @brief BLE 蓝牙通信支持（保留通用 BLE 能力；配网已统一由 AP+STA 双模自动切换处理）
 * 默认：禁用（standard 预设已 lib_ignore NimBLE-Arduino）
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

// Web admin surface switches. Keep auth/session available, but allow small
// ESP32 production builds to remove rarely used management APIs and pages.
#ifndef FASTBEE_ENABLE_USER_ADMIN
#define FASTBEE_ENABLE_USER_ADMIN 1
#endif

#ifndef FASTBEE_ENABLE_FILE_MANAGER
#define FASTBEE_ENABLE_FILE_MANAGER 0
#endif

#ifndef FASTBEE_ENABLE_CONFIG_TRANSFER
#define FASTBEE_ENABLE_CONFIG_TRANSFER 1
#endif

#ifndef FASTBEE_SINGLE_ADMIN_MODE
#define FASTBEE_SINGLE_ADMIN_MODE 0
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
#define FASTBEE_ENABLE_OTA 0
#endif

/**
 * @brief OTA 文件系统更新
 * 默认：启用
 * 需要：FASTBEE_ENABLE_OTA=1
 */
#ifndef FASTBEE_ENABLE_OTA_FS
#define FASTBEE_ENABLE_OTA_FS 0
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

#ifndef FASTBEE_ENABLE_LOG_VIEWER
#define FASTBEE_ENABLE_LOG_VIEWER 0
#endif

#ifndef FASTBEE_ENABLE_FILE_LOGGING
#define FASTBEE_ENABLE_FILE_LOGGING 0
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
  #define FASTBEE_ENABLE_RULE_SCRIPT 0
#endif

/**
 * @brief Command script action for PeriphExec ACTION_SCRIPT.
 * This is independent from RuleScript pages/API so slim builds can keep
 * local command scripts without restoring the full rule-script module.
 */
#ifndef FASTBEE_ENABLE_COMMAND_SCRIPT
  #define FASTBEE_ENABLE_COMMAND_SCRIPT FASTBEE_ENABLE_RULE_SCRIPT
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
 * @brief 传感器驱动支持（DHT11/DHT22/DS18B20）
 * 默认：启用
 * 占用：约 8KB Flash（DHT 库 ~3KB + OneWire ~2KB + DallasTemperature ~3KB）
 * RAM：约 200B（驱动实例 + 缓存）
 * 需要：Adafruit DHT sensor library, OneWire, DallasTemperature
 * 说明：支持通过 ACTION_SENSOR_READ 读取温湿度传感器数据
 */
#ifndef FASTBEE_ENABLE_SENSOR_DRIVER
#define FASTBEE_ENABLE_SENSOR_DRIVER 1
#endif

/**
 * @brief LED 灯珠支持（WS2812B/NeoPixel）
 * 默认：按环境配置
 * 占用：约 10KB Flash（轻量 RMT 发送器）
 * RAM：发送时临时分配 24 × 灯珠数 × rmt_item32_t，默认限制 64 颗
 * 需要：ESP32 RMT 外设，无第三方 NeoPixel 库依赖
 * 说明：使用 ESP32 RMT 硬件外设发送 GRB 时序
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

/**
 * @brief TM1637 4位数码管支持
 * 默认：禁用（需硬件支持）
 * 占用：约 3KB Flash（自写 bit-bang 驱动，无外部库依赖）
 * RAM：约 32B × 实例数
 * 说明：通过 ACTION_DISPLAY_NUMBER / ACTION_DISPLAY_TEXT / ACTION_DISPLAY_CLEAR 驱动显示
 */
#ifndef FASTBEE_ENABLE_SEVEN_SEGMENT
    #define FASTBEE_ENABLE_SEVEN_SEGMENT 0
#endif

/**
 * @brief WS2812/NeoPixel RGB LED 灯带支持
 * 默认：启用（使用 Adafruit_NeoPixel 库，跨芯片兼容）
 * 占用：约 12KB Flash
 * RAM：约 3B × 灯珠数 + 2KB（库开销），64 颗约 212B
 * 需要：Adafruit NeoPixel Library
 * 说明：支持通过 ACTION_CALL_PERIPHERAL 控制（color/off/rainbow/brightness）
 * 兼容：ESP32/S3/C3/C6（统一使用新版 RMT 驱动）
 */
#ifndef FASTBEE_ENABLE_NEOPIXEL
    #define FASTBEE_ENABLE_NEOPIXEL 1
#endif

/**
 * @brief WS2812/NeoPixel 最大灯珠数
 * 默认：64（可在 platformio.ini 中覆盖）
 * 说明：内存受限芯片可设为 32 以节省内存
 */
#ifndef FASTBEE_NEOPIXEL_MAX_LEDS
    #define FASTBEE_NEOPIXEL_MAX_LEDS 64
#endif

/**
 * @brief I2C 高级传感器支持（BMP280 气压/MPU6050 陀螺仪等）
 * 默认：禁用（仅 ESP32-S3 full 构建启用）
 * 占用：约 15KB Flash（BMP280 ~5KB + MPU6050 ~10KB）
 * RAM：约 500B（驱动实例 + 数据缓存）
 * 需要：Adafruit BMP280 Library, Adafruit MPU6050 Library
 * 说明：支持通过 ACTION_SENSOR_READ 读取气压/温度/海拔/加速度/角速度数据
 */
#ifndef FASTBEE_ENABLE_I2C_SENSORS
    #define FASTBEE_ENABLE_I2C_SENSORS 0
#endif

/**
 * @brief MFRC522 RFID 射频卡模块支持
 * 默认：禁用（仅 ESP32-S3 full 构建启用）
 * 占用：约 12KB Flash
 * RAM：约 200B
 * 需要：MFRC522 Library (SPI)
 * 说明：支持卡片 UID 读取、事件触发（刷卡事件）
 */
#ifndef FASTBEE_ENABLE_RFID
    #define FASTBEE_ENABLE_RFID 0
#endif

/**
 * @brief 红外遥控收发支持
 * 默认：禁用（仅 ESP32-S3 full 构建启用）
 * 占用：约 8KB Flash
 * RAM：约 300B（接收缓冲区）
 * 需要：IRremoteESP8266 Library
 * 说明：支持 NEC/RC5/SONY 等协议的红外解码，可作为事件触发源
 */
#ifndef FASTBEE_ENABLE_IR_REMOTE
    #define FASTBEE_ENABLE_IR_REMOTE 0
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

// ============================================================================
// 国际化支持
// ============================================================================

/**
 * @brief Web UI 多语言(中英文切换)支持
 * 默认：禁用（仅显示中文）
 * 启用后：前端显示语言切换选择器，支持中/英文切换
 */
#ifndef FASTBEE_ENABLE_I18N
#define FASTBEE_ENABLE_I18N 0
#endif

#endif // FEATURE_FLAGS_H
