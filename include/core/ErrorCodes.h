/**
 * @file ErrorCodes.h
 * @brief 统一错误码定义
 * @author kerwincui
 * @date 2025-12-02
 * 
 * 提供系统级的错误码定义，便于错误诊断和处理。
 */

#ifndef ERROR_CODES_H
#define ERROR_CODES_H

#include <Arduino.h>

/**
 * @brief 错误码枚举
 * 
 * 错误码按模块分类：
 * - 0:      成功
 * - 1-99:   通用错误
 * - 100-199:存储相关
 * - 200-299:网络相关
 * - 300-399:协议相关
 * - 400-499:安全相关
 * - 500-599:Web 服务相关
 * - 600-699:系统服务相关
 */
enum class ErrorCode {
    // ============ 成功 ============
    OK = 0,
    
    // ============ 通用错误 (1-99) ============
    ERR_UNKNOWN = 1,
    ERR_INVALID_PARAM = 2,
    ERR_NOT_INITIALIZED = 3,
    ERR_TIMEOUT = 4,
    ERR_OUT_OF_MEMORY = 5,
    ERR_NOT_SUPPORTED = 6,
    ERR_ALREADY_EXISTS = 7,
    ERR_NOT_FOUND = 8,
    
    // ============ 存储相关 (100-199) ============
    ERR_FS_INIT_FAILED = 100,
    ERR_FS_MOUNT_FAILED = 101,
    ERR_FS_FORMAT_FAILED = 102,
    ERR_FILE_NOT_FOUND = 103,
    ERR_FILE_OPEN_FAILED = 104,
    ERR_FILE_READ_FAILED = 105,
    ERR_FILE_WRITE_FAILED = 106,
    ERR_FILE_CLOSE_FAILED = 107,
    ERR_FILE_DELETE_FAILED = 108,
    ERR_CONFIG_LOAD_FAILED = 109,
    ERR_CONFIG_SAVE_FAILED = 110,
    ERR_CONFIG_INVALID = 111,
    ERR_NVS_INIT_FAILED = 112,
    ERR_NVS_READ_FAILED = 113,
    ERR_NVS_WRITE_FAILED = 114,
    
    // ============ 网络相关 (200-299) ============
    ERR_WIFI_INIT_FAILED = 200,
    ERR_WIFI_CONNECT_FAILED = 201,
    ERR_WIFI_DISCONNECTED = 202,
    ERR_WIFI_TIMEOUT = 203,
    ERR_WIFI_NO_NETWORK = 204,
    ERR_WIFI_IP_CONFLICT = 205,
    ERR_WIFI_DNS_FAILED = 206,
    ERR_WIFI_MDNS_FAILED = 207,
    ERR_AP_START_FAILED = 208,
    ERR_NETWORK_UNREACHABLE = 209,
    
    // ============ 协议相关 (300-399) ============
    ERR_PROTOCOL_INIT_FAILED = 300,
    ERR_PROTOCOL_CONNECT_FAILED = 301,
    ERR_PROTOCOL_SEND_FAILED = 302,
    ERR_PROTOCOL_RECV_FAILED = 303,
    ERR_PROTOCOL_TIMEOUT = 304,
    ERR_MQTT_CONNECT_FAILED = 310,
    ERR_MQTT_PUBLISH_FAILED = 311,
    ERR_MQTT_SUBSCRIBE_FAILED = 312,
    ERR_MODBUS_INIT_FAILED = 320,
    ERR_MODBUS_SEND_FAILED = 321,
    ERR_MODBUS_RECV_FAILED = 322,
    ERR_TCP_INIT_FAILED = 330,
    ERR_TCP_CONNECT_FAILED = 331,
    ERR_TCP_SEND_FAILED = 332,
    ERR_HTTP_REQUEST_FAILED = 340,
    ERR_COAP_INIT_FAILED = 350,
    ERR_COAP_SEND_FAILED = 351,
    
    // ============ 安全相关 (400-499) ============
    ERR_AUTH_FAILED = 400,
    ERR_AUTH_TOKEN_INVALID = 401,
    ERR_AUTH_TOKEN_EXPIRED = 402,
    ERR_AUTH_PERMISSION_DENIED = 403,
    ERR_USER_NOT_FOUND = 404,
    ERR_USER_ALREADY_EXISTS = 405,
    ERR_PASSWORD_INVALID = 406,
    ERR_ACCOUNT_LOCKED = 407,
    ERR_SESSION_INVALID = 408,
    ERR_SESSION_EXPIRED = 409,
    
    // ============ Web 服务相关 (500-599) ============
    ERR_WEB_SERVER_INIT_FAILED = 500,
    ERR_WEB_ROUTE_NOT_FOUND = 501,
    ERR_WEB_HANDLER_FAILED = 502,
    ERR_WEB_UPLOAD_FAILED = 503,
    ERR_WEB_PARSE_FAILED = 504,
    
    // ============ 系统服务相关 (600-699) ============
    ERR_TASK_INIT_FAILED = 600,
    ERR_TASK_CREATE_FAILED = 601,
    ERR_TASK_MAX_REACHED = 602,
    ERR_HEALTH_CHECK_FAILED = 610,
    ERR_LOW_MEMORY = 611,
    ERR_HIGH_CPU_USAGE = 612,
    ERR_OTA_INIT_FAILED = 620,
    ERR_OTA_DOWNLOAD_FAILED = 621,
    ERR_OTA_VERIFY_FAILED = 622,
    ERR_OTA_INSTALL_FAILED = 623,
    ERR_GPIO_INIT_FAILED = 630,
    ERR_GPIO_CONFIG_FAILED = 631,
    ERR_GPIO_READ_FAILED = 632,
    ERR_GPIO_WRITE_FAILED = 633
};

/**
 * @brief 错误结果结构体
 */
struct Result {
    ErrorCode code;
    String message;
    
    Result() : code(ErrorCode::OK), message("") {}
    Result(ErrorCode c, const String& msg) : code(c), message(msg) {}
    
    bool ok() const { return code == ErrorCode::OK; }
    
    const char* getCodeString() const {
        switch (code) {
            case ErrorCode::OK: return "OK";
            case ErrorCode::ERR_UNKNOWN: return "ERR_UNKNOWN";
            case ErrorCode::ERR_INVALID_PARAM: return "ERR_INVALID_PARAM";
            case ErrorCode::ERR_NOT_INITIALIZED: return "ERR_NOT_INITIALIZED";
            case ErrorCode::ERR_TIMEOUT: return "ERR_TIMEOUT";
            case ErrorCode::ERR_OUT_OF_MEMORY: return "ERR_OUT_OF_MEMORY";
            
            case ErrorCode::ERR_FS_INIT_FAILED: return "ERR_FS_INIT_FAILED";
            case ErrorCode::ERR_FILE_NOT_FOUND: return "ERR_FILE_NOT_FOUND";
            case ErrorCode::ERR_CONFIG_LOAD_FAILED: return "ERR_CONFIG_LOAD_FAILED";
            
            case ErrorCode::ERR_WIFI_CONNECT_FAILED: return "ERR_WIFI_CONNECT_FAILED";
            case ErrorCode::ERR_WIFI_TIMEOUT: return "ERR_WIFI_TIMEOUT";
            
            case ErrorCode::ERR_MQTT_CONNECT_FAILED: return "ERR_MQTT_CONNECT_FAILED";
            case ErrorCode::ERR_MODBUS_INIT_FAILED: return "ERR_MODBUS_INIT_FAILED";
            
            case ErrorCode::ERR_AUTH_FAILED: return "ERR_AUTH_FAILED";
            case ErrorCode::ERR_USER_NOT_FOUND: return "ERR_USER_NOT_FOUND";
            
            case ErrorCode::ERR_LOW_MEMORY: return "ERR_LOW_MEMORY";
            case ErrorCode::ERR_OTA_INIT_FAILED: return "ERR_OTA_INIT_FAILED";
            
            default: return "ERR_UNKNOWN_CODE";
        }
    }
    
    String toString() const {
        String result = String(getCodeString());
        if (message.length() > 0) {
            result += ": " + message;
        }
        return result;
    }
};

// 便捷宏定义
#define RESULT_OK Result(ErrorCode::OK, "")
#define RESULT_ERR(code, msg) Result(code, msg)

#endif // ERROR_CODES_H
