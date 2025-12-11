#ifndef SYSTEM_CONSTANTS_H
#define SYSTEM_CONSTANTS_H

#include <Arduino.h>

// ============================================================================
// 系统版本和信息常量
// ============================================================================
namespace SystemInfo {
    constexpr const char* VERSION = "1.0.0";
    constexpr const char* NAME = "FastBee IoT Platform";
    constexpr const char* MANUFACTURER = "FastBee Technology";
    constexpr const char* COPYRIGHT = "Copyright © 2024 FastBee IoT Platform";
    
    // 设备信息
    constexpr const char* DEFAULT_DEVICE_NAME = "FastBee-Device";
    constexpr const char* MODEL = "FB-ESP32-V1";
    constexpr const char* FIRMWARE_TYPE = "stable";
}

// ============================================================================
// 硬件配置常量
// ============================================================================
namespace Hardware {
    // ESP32 硬件特定配置
    constexpr uint32_t CPU_FREQUENCY = 240; // MHz
    
    // 引脚定义（根据实际硬件调整）
    constexpr uint8_t STATUS_LED_PIN = 2;    // 通常ESP32开发板上的内置LED
    constexpr uint8_t BOOT_BUTTON_PIN = 0;   // 通常ESP32的BOOT按钮
    constexpr uint8_t USER_BUTTON_PIN = 35;  // 用户按钮
    
    // 串口配置
    constexpr uint32_t SERIAL_BAUDRATE = 115200;
    
    // ADC配置
    constexpr uint8_t ADC_RESOLUTION = 12;   // 12位ADC
    constexpr uint16_t ADC_MAX_VALUE = 4095; // 2^12 - 1
    
    // I2C配置
    constexpr uint8_t I2C_SDA_PIN = 21;
    constexpr uint8_t I2C_SCL_PIN = 22;
    constexpr uint32_t I2C_FREQUENCY = 100000; // 100kHz
    
    // SPI配置
    constexpr uint8_t SPI_MISO_PIN = 19;
    constexpr uint8_t SPI_MOSI_PIN = 23;
    constexpr uint8_t SPI_SCK_PIN = 18;
    constexpr uint8_t SPI_SS_PIN = 5;
}

// ============================================================================
// 网络配置常量
// ============================================================================
namespace Network {
    // WiFi配置
    constexpr const char* DEFAULT_AP_SSID = "FastBee-Config";
    constexpr const char* DEFAULT_AP_PASSWORD = "fastbee123";
    constexpr uint8_t DEFAULT_AP_CHANNEL = 1;
    constexpr uint8_t DEFAULT_AP_MAX_CONNECTIONS = 4;
    constexpr bool DEFAULT_AP_HIDDEN = false;    
    
    // MQTT配置
    constexpr const char* DEFAULT_MQTT_SERVER = "mqtt.broker.com";
    constexpr uint16_t DEFAULT_MQTT_PORT = 1883;
    constexpr const char* DEFAULT_MQTT_USER = "";
    constexpr const char* DEFAULT_MQTT_PASSWORD = "";
    constexpr uint16_t MQTT_KEEPALIVE = 60; // 秒
    constexpr uint16_t MQTT_SOCKET_TIMEOUT = 15; // 秒
    constexpr uint16_t MQTT_BUFFER_SIZE = 1024;
    
    // TCP服务器配置
    constexpr uint16_t TCP_SERVER_PORT = 8080;
    constexpr uint16_t TCP_MAX_CLIENTS = 5;
    constexpr uint16_t TCP_BUFFER_SIZE = 1024;
    
    // HTTP客户端配置
    constexpr uint16_t HTTP_TIMEOUT = 10000; // 10秒
    constexpr uint16_t HTTP_RETRY_COUNT = 3;
    
    // CoAP配置
    constexpr uint16_t COAP_PORT = 5683;
    constexpr uint16_t COAP_BUFFER_SIZE = 512;
}

// ============================================================================
// 文件系统配置常量
// ============================================================================
namespace FileSystem {
    // 文件系统类型
    constexpr bool USE_LITTLEFS = true;
    
    // 路径常量
    constexpr const char* CONFIG_DIR = "/config";
    constexpr const char* LOGS_DIR = "/logs";
    constexpr const char* OTA_DIR = "/ota";
    constexpr const char* DATA_DIR = "/data";
    
    // 配置文件路径
    constexpr const char* SYSTEM_CONFIG_FILE = "/config/system.json";
    constexpr const char* NETWORK_CONFIG_FILE = "/config/network.json";
    constexpr const char* USER_CONFIG_FILE = "/config/users.json";
    constexpr const char* MQTT_CONFIG_FILE = "/config/mqtt.json";
    constexpr const char* MODBUS_CONFIG_FILE = "/config/modbus.json";
    constexpr const char* TCP_CONFIG_FILE = "/config/tcp.json";
    constexpr const char* HTTP_CONFIG_FILE = "/config/http.json";
    constexpr const char* COAP_CONFIG_FILE = "/config/coap.json";
    constexpr const char* TIMER_CONFIG_FILE = "/config/timers.json";
    
    // 日志文件配置
    constexpr const char* SYSTEM_LOG_FILE = "/logs/system.log";
    constexpr const char* ACCESS_LOG_FILE = "/logs/access.log";
    constexpr const size_t MAX_LOG_FILE_SIZE = 1048576; // 1MB
    constexpr const uint8_t LOG_RETENTION_DAYS = 7;
    
    // OTA文件
    constexpr const char* OTA_FIRMWARE_FILE = "/ota/firmware.bin";
}

// ============================================================================
// 存储配置常量
// ============================================================================
namespace Storage {
    // Preferences命名空间
    constexpr const char* PREFERENCES_NAMESPACE = "fastbee";
    
    // 系统配置键
    constexpr const char* KEY_DEVICE_ID = "device.id";
    constexpr const char* KEY_BOOT_COUNT = "system.boot_count";
    constexpr const char* KEY_LAST_RESET_REASON = "system.last_reset_reason";
    constexpr const char* KEY_FIRMWARE_VERSION = "system.fw_version";
    
    // 网络配置键
    constexpr const char* KEY_WIFI_MODE = "network.wifi_mode";
    constexpr const char* KEY_AP_ENABLED = "network.ap_enabled";
    constexpr const char* KEY_STA_ENABLED = "network.sta_enabled";
    constexpr const char* KEY_HOSTNAME = "network.hostname";
    
    // 安全配置键
    constexpr const char* KEY_SESSION_TOKEN = "auth.session_token";
    constexpr const char* KEY_LAST_LOGIN = "auth.last_login";
    constexpr const char* KEY_LOGIN_ATTEMPTS = "auth.login_attempts";
    
    // JSON配置文档大小
    constexpr size_t SYSTEM_CONFIG_DOC_SIZE = 2048;
    constexpr size_t NETWORK_CONFIG_DOC_SIZE = 2048;
    constexpr size_t USER_CONFIG_DOC_SIZE = 4096;
    constexpr size_t PROTOCOL_CONFIG_DOC_SIZE = 2048;
}

// ============================================================================
// 任务调度配置常量
// ============================================================================
namespace TaskScheduler {
    // 任务数量限制
    constexpr uint8_t MAX_TASKS = 20;
    constexpr uint8_t MAX_TASK_NAME_LENGTH = 32;
    constexpr uint8_t TASK_QUEUE_SIZE = 10;
    
    // 默认任务栈大小
    constexpr uint16_t DEFAULT_TASK_STACK_SIZE = 4096;
    
    // 任务优先级
    constexpr uint8_t TASK_PRIORITY_HIGH = 3;
    constexpr uint8_t TASK_PRIORITY_NORMAL = 2;
    constexpr uint8_t TASK_PRIORITY_LOW = 1;
    constexpr uint8_t TASK_PRIORITY_IDLE = 0;
    
    // 系统任务间隔（毫秒）
    constexpr uint32_t HEALTH_CHECK_INTERVAL = 30000;      // 30秒
    constexpr uint32_t WEB_CLIENT_INTERVAL = 100;          // 100ms
    constexpr uint32_t NETWORK_CHECK_INTERVAL = 10000;     // 10秒
    constexpr uint32_t DATA_SYNC_INTERVAL = 60000;         // 1分钟
    constexpr uint32_t STATUS_UPDATE_INTERVAL = 5000;      // 5秒
}

// ============================================================================
// 日志系统配置常量
// ============================================================================
namespace Logging {
    // 日志级别
    enum LogLevel {
        LOG_LEVEL_ERROR = 0,
        LOG_LEVEL_WARNING = 1,
        LOG_LEVEL_INFO = 2,
        LOG_LEVEL_DEBUG = 3,
        LOG_LEVEL_VERBOSE = 4
    };
    
    // 默认日志级别
    constexpr LogLevel DEFAULT_LOG_LEVEL = LOG_LEVEL_INFO;
    
    // 日志模块名称
    constexpr const char* MODULE_SYSTEM = "SYSTEM";
    constexpr const char* MODULE_NETWORK = "NETWORK";
    constexpr const char* MODULE_WEB = "WEB";
    constexpr const char* MODULE_OTA = "OTA";
    constexpr const char* MODULE_MQTT = "MQTT";
    constexpr const char* MODULE_MODBUS = "MODBUS";
    constexpr const char* MODULE_TCP = "TCP";
    constexpr const char* MODULE_HTTP = "HTTP";
    constexpr const char* MODULE_COAP = "COAP";
    constexpr const char* MODULE_SECURITY = "SECURITY";
    constexpr const char* MODULE_STORAGE = "STORAGE";
    
    // 日志缓冲区大小
    constexpr uint16_t LOG_BUFFER_SIZE = 512;
    constexpr uint16_t MAX_LOG_MESSAGE_LENGTH = 256;
}

// ============================================================================
// OTA配置常量
// ============================================================================
namespace OTA {
    // OTA超时配置
    constexpr uint32_t OTA_TIMEOUT = 300000;           // 5分钟
    constexpr uint32_t OTA_BUFFER_SIZE = 4096;
    
    // OTA状态
    enum OTAStatus {
        OTA_IDLE = 0,
        OTA_IN_PROGRESS = 1,
        OTA_SUCCESS = 2,
        OTA_FAILED = 3
    };
    
    // OTA更新源
    enum OTASource {
        OTA_SOURCE_WEB = 0,
        OTA_SOURCE_HTTP = 1,
        OTA_SOURCE_MQTT = 2
    };
}

// ============================================================================
// 协议配置常量
// ============================================================================
namespace Protocols {
    // Modbus配置
    constexpr uint32_t MODBUS_BAUDRATE = 9600;
    constexpr uint8_t MODBUS_DATA_BITS = 8;
    constexpr uint8_t MODBUS_STOP_BITS = 1;
    constexpr uint8_t MODBUS_PARITY = 0; // 0=None, 1=Odd, 2=Even
    
    // 协议缓冲区大小
    constexpr uint16_t MQTT_BUFFER_SIZE = 1024;
    constexpr uint16_t MODBUS_BUFFER_SIZE = 256;
    constexpr uint16_t TCP_BUFFER_SIZE = 1024;
    constexpr uint16_t HTTP_BUFFER_SIZE = 2048;
    constexpr uint16_t COAP_BUFFER_SIZE = 512;
    
    // 协议超时
    constexpr uint16_t MQTT_TIMEOUT = 15000;
    constexpr uint16_t MODBUS_TIMEOUT = 1000;
    constexpr uint16_t TCP_TIMEOUT = 30000;
    constexpr uint16_t HTTP_TIMEOUT = 10000;
    constexpr uint16_t COAP_TIMEOUT = 5000;
}

// ============================================================================
// 安全配置常量
// ============================================================================
namespace Security {
    // 用户角色
    constexpr const char* ROLE_ADMIN = "admin";
    constexpr const char* ROLE_USER = "user";
    constexpr const char* ROLE_VIEWER = "viewer";
    
    // 认证配置
    constexpr uint8_t MAX_LOGIN_ATTEMPTS = 5;
    constexpr uint32_t LOGIN_LOCKOUT_TIME = 300000; // 5分钟
    constexpr uint16_t SESSION_TOKEN_LENGTH = 32;
    constexpr uint32_t SESSION_TIMEOUT = 3600000; // 1小时
    
    // 密码策略
    constexpr uint8_t MIN_PASSWORD_LENGTH = 6;
    constexpr uint8_t MAX_PASSWORD_LENGTH = 32;
    
    // 默认用户凭据
    constexpr const char* DEFAULT_ADMIN_USERNAME = "admin";
    constexpr const char* DEFAULT_ADMIN_PASSWORD = "admin";
}

// ============================================================================
// 系统健康检查阈值
// ============================================================================
namespace HealthCheck {
    // 内存阈值
    constexpr uint32_t MIN_FREE_HEAP = 10000;              // 10KB 最小空闲堆
    constexpr uint32_t CRITICAL_FREE_HEAP = 5000;          // 5KB 临界空闲堆
    constexpr uint8_t MAX_HEAP_FRAGMENTATION = 50;         // 50% 最大堆碎片
    constexpr uint8_t CRITICAL_HEAP_FRAGMENTATION = 80;    // 80% 临界堆碎片
    
    // 文件系统阈值
    constexpr uint32_t MIN_FREE_FS_SPACE = 1048576;        // 1MB 最小空闲文件系统空间
    constexpr uint32_t CRITICAL_FREE_FS_SPACE = 524288;    // 512KB 临界空闲文件系统空间
    
    // 网络阈值
    constexpr int8_t MIN_WIFI_RSSI = -80;                  // -80dBm 最小WiFi信号强度
    constexpr int8_t CRITICAL_WIFI_RSSI = -90;             // -90dBm 临界WiFi信号强度
    
    // 温度阈值（如果支持温度监控）
    constexpr float MAX_CPU_TEMPERATURE = 85.0;            // 85°C 最大CPU温度
    constexpr float CRITICAL_CPU_TEMPERATURE = 95.0;       // 95°C 临界CPU温度
}

// ============================================================================
// 定时器配置常量
// ============================================================================
namespace Timers {
    // 最大定时器数量
    constexpr uint8_t MAX_TIMERS = 10;
    
    // 定时器类型
    enum TimerType {
        TIMER_ONCE = 0,
        TIMER_REPEAT = 1,
        TIMER_CRON = 2
    };
    
    // 定时器动作
    enum TimerAction {
        ACTION_RESTART = 0,
        ACTION_REBOOT = 1,
        ACTION_EXECUTE_TASK = 2,
        ACTION_SEND_MQTT = 3,
        ACTION_CALL_URL = 4
    };
}

// ============================================================================
// Web界面配置常量
// ============================================================================
namespace WebUI {
    // 页面标题和品牌
    constexpr const char* PAGE_TITLE = "FastBee IoT Platform";
    constexpr const char* BRAND_NAME = "FastBee";
    
    // API端点
    constexpr const char* API_BASE = "/api";
    constexpr const char* API_LOGIN = "/api/login";
    constexpr const char* API_LOGOUT = "/api/logout";
    constexpr const char* API_STATUS = "/api/status";
    constexpr const char* API_SYSTEM_CONFIG = "/api/system/config";
    constexpr const char* API_NETWORK_CONFIG = "/api/network/config";
    constexpr const char* API_USER_MANAGEMENT = "/api/users";
    constexpr const char* API_OTA_UPDATE = "/api/ota/update";
    constexpr const char* API_PROTOCOL_CONFIG = "/api/protocols";
    
    // WebSocket端点
    constexpr const char* WS_ENDPOINT = "/ws";
    
    // 静态文件路径
    constexpr const char* WEB_ROOT = "/web";
    constexpr const char* DEFAULT_PAGE = "/index.html";
}

// ============================================================================
// 错误代码定义
// ============================================================================
namespace ErrorCodes {
    // 系统错误
    constexpr int SUCCESS = 0;
    constexpr int UNKNOWN_ERROR = -1;
    constexpr int OUT_OF_MEMORY = -2;
    constexpr int FILE_SYSTEM_ERROR = -3;
    constexpr int NETWORK_ERROR = -4;
    
    // 配置错误
    constexpr int CONFIG_LOAD_FAILED = -100;
    constexpr int CONFIG_SAVE_FAILED = -101;
    constexpr int CONFIG_INVALID = -102;
    
    // 网络错误
    constexpr int WIFI_CONNECT_FAILED = -200;
    constexpr int MQTT_CONNECT_FAILED = -201;
    constexpr int HTTP_REQUEST_FAILED = -202;
    
    // 安全错误
    constexpr int AUTH_FAILED = -300;
    constexpr int PERMISSION_DENIED = -301;
    constexpr int SESSION_EXPIRED = -302;
    
    // OTA错误
    constexpr int OTA_START_FAILED = -400;
    constexpr int OTA_WRITE_FAILED = -401;
    constexpr int OTA_VERIFY_FAILED = -402;
}

#endif
