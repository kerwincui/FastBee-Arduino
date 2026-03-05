#ifndef CONFIG_DEFINES_H
#define CONFIG_DEFINES_H

/**
 * @file ConfigDefines.h
 * @brief 【已废弃】历史遗留宏定义文件
 *
 * 所有常量已迁移至 SystemConstants.h 中的对应 constexpr namespace：
 *   - 网络配置  →  namespace Network
 *   - 文件路径  →  namespace FileSystem
 *   - 存储配置  →  namespace Storage
 *   - 安全配置  →  namespace Security
 *   - 任务调度  →  namespace TaskScheduler
 *
 * 新代码请直接包含 <core/SystemConstants.h> 并使用命名空间常量。
 * 本文件保留仅为向后兼容，不得在此添加新的宏定义。
 */
#include "core/SystemConstants.h"

// ---------------------------------------------------------------------------
// 向后兼容别名（仅供已使用旧宏的遗留代码过渡期使用，后续将逐步移除）
// ---------------------------------------------------------------------------
#ifndef FASTBEE_VERSION
#define FASTBEE_VERSION SystemInfo::VERSION
#endif

#ifndef FASTBEE_DEVICE_NAME
#define FASTBEE_DEVICE_NAME SystemInfo::DEFAULT_DEVICE_NAME
#endif

// WiFi 默认 SSID/Password：留空，首次启动进入 AP 配网模式
#ifndef FASTBEE_DEFAULT_SSID
#define FASTBEE_DEFAULT_SSID     ""
#endif
#ifndef FASTBEE_DEFAULT_PASSWORD
#define FASTBEE_DEFAULT_PASSWORD ""
#endif

#ifndef MDNS_HOSTNAME
#define MDNS_HOSTNAME Network::DEFAULT_MDNS_HOSTNAME
#endif
#ifndef CUSTOM_DOMAIN
#define CUSTOM_DOMAIN Network::DEFAULT_CUSTOM_DOMAIN
#endif

#ifndef WIFI_CONNECT_TIMEOUT
#define WIFI_CONNECT_TIMEOUT Network::WIFI_CONNECT_TIMEOUT
#endif
#ifndef WIFI_RECONNECT_INTERVAL
#define WIFI_RECONNECT_INTERVAL Network::WIFI_RECONNECT_INTERVAL
#endif
#ifndef MAX_WIFI_ATTEMPTS
#define MAX_WIFI_ATTEMPTS Network::MAX_WIFI_ATTEMPTS
#endif

#ifndef WEB_SERVER_PORT
#define WEB_SERVER_PORT Network::WEB_SERVER_PORT
#endif
#ifndef WEB_SOCKET_PORT
#define WEB_SOCKET_PORT Network::WEB_SOCKET_PORT
#endif
#ifndef WEB_UPLOAD_BUFFER_SIZE
#define WEB_UPLOAD_BUFFER_SIZE Network::WEB_UPLOAD_BUFFER_SIZE
#endif
#ifndef WEB_SESSION_TIMEOUT
#define WEB_SESSION_TIMEOUT Network::WEB_SESSION_TIMEOUT
#endif

#ifndef PREFERENCES_NETWORK
#define PREFERENCES_NETWORK "network"
#endif

#ifndef CONFIG_FILE_DEVICE
#define CONFIG_FILE_DEVICE FileSystem::DEVICE_CONFIG_FILE
#endif
#ifndef CONFIG_FILE_NETWORK
#define CONFIG_FILE_NETWORK FileSystem::NETWORK_CONFIG_FILE
#endif
#ifndef CONFIG_FILE_USERS
#define CONFIG_FILE_USERS FileSystem::USER_CONFIG_FILE
#endif
#ifndef CONFIG_FILE_MQTT
#define CONFIG_FILE_MQTT FileSystem::MQTT_CONFIG_FILE
#endif
#ifndef CONFIG_FILE_MODBUS
#define CONFIG_FILE_MODBUS FileSystem::MODBUS_CONFIG_FILE
#endif
#ifndef CONFIG_FILE_TCP
#define CONFIG_FILE_TCP FileSystem::TCP_CONFIG_FILE
#endif
#ifndef CONFIG_FILE_HTTP
#define CONFIG_FILE_HTTP FileSystem::HTTP_CONFIG_FILE
#endif
#ifndef CONFIG_FILE_COAP
#define CONFIG_FILE_COAP FileSystem::COAP_CONFIG_FILE
#endif

#endif // CONFIG_DEFINES_H
