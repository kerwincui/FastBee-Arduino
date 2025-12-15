#ifndef CONFIG_DEFINES_H
#define CONFIG_DEFINES_H

// 系统配置常量
#define FASTBEE_VERSION "1.0.0"
#define FASTBEE_DEVICE_NAME "FBE10000001"
#define FASTBEE_DEFAULT_SSID "CMCC-7mnN"
#define FASTBEE_DEFAULT_PASSWORD "eb66bcm9"

// DNS配置
#define MDNS_HOSTNAME "fastbee"
#define CUSTOM_DOMAIN "fastbee"

// 网络配置
#define WIFI_CONNECT_TIMEOUT 30000
#define WIFI_RECONNECT_INTERVAL 10000
#define MAX_WIFI_ATTEMPTS 3

// Web服务器配置
#define WEB_SERVER_PORT 80
#define WEB_SOCKET_PORT 81
#define WEB_UPLOAD_BUFFER_SIZE 4096
#define WEB_SESSION_TIMEOUT 3600000 // 1小时 (毫秒)

// OTA配置
// #define OTA_TIMEOUT 300000 // 5分钟
// #define OTA_BUFFER_SIZE 4096

// 存储配置
// #define PREFERENCES_NAMESPACE "fastbee"
#define PREFERENCES_NETWORK "network"
#define CONFIG_FILE_SYSTEM "/config/system.json"
#define CONFIG_FILE_NETWORK "/config/network.json"
#define CONFIG_FILE_USERS "/config/users.json"
#define CONFIG_FILE_MQTT "/config/mqtt.json"
#define CONFIG_FILE_MODBUS "/config/modbus.json"
#define CONFIG_FILE_TCP "/config/tcp.json"
#define CONFIG_FILE_HTTP "/config/http.json"
#define CONFIG_FILE_COAP "/config/coap.json"

// 任务调度配置
// #define MAX_TASKS 20
// #define TASK_QUEUE_SIZE 10

// 健康检查阈值
// #define MIN_FREE_HEAP 10000
// #define MAX_HEAP_FRAGMENTATION 50

#endif