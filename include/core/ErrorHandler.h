#ifndef ERROR_HANDLER_H
#define ERROR_HANDLER_H

#include <Arduino.h>
#include <stdint.h>

/**
 * @brief 错误码枚举
 * 
 * 定义系统中可能出现的各种错误类型
 */
enum class ErrorCode : uint16_t {
    // 系统错误
    SUCCESS = 0,                // 成功
    UNKNOWN_ERROR = 1,          // 未知错误
    MEMORY_ERROR = 2,           // 内存错误
    SYSTEM_ERROR = 3,           // 系统错误
    INITIALIZATION_ERROR = 4,   // 初始化错误
    
    // 网络错误
    NETWORK_ERROR = 100,        // 网络错误
    WIFI_CONNECTION_ERROR = 101, // WiFi连接错误
    IP_CONFIG_ERROR = 102,      // IP配置错误
    DNS_ERROR = 103,            // DNS错误
    IP_CONFLICT_ERROR = 104,    // IP冲突错误
    
    // 存储错误
    STORAGE_ERROR = 200,        // 存储错误
    PREFERENCES_ERROR = 201,    // Preferences错误
    FILE_SYSTEM_ERROR = 202,    // 文件系统错误
    JSON_PARSE_ERROR = 203,     // JSON解析错误
    
    // 安全错误
    SECURITY_ERROR = 300,       // 安全错误
    AUTHENTICATION_ERROR = 301, // 认证错误
    AUTHORIZATION_ERROR = 302,  // 授权错误
    USER_ERROR = 303,           // 用户错误
    
    // 协议错误
    PROTOCOL_ERROR = 400,       // 协议错误
    MQTT_ERROR = 401,           // MQTT错误
    HTTP_ERROR = 402,            // HTTP错误
    MODBUS_ERROR = 403,          // Modbus错误
    TCP_ERROR = 404,             // TCP错误
    COAP_ERROR = 405,            // CoAP错误
    
    // 任务错误
    TASK_ERROR = 500,            // 任务错误
    TASK_OVERFLOW = 501,         // 任务溢出
    TASK_EXECUTION_ERROR = 502,  // 任务执行错误
};

/**
 * @brief 错误信息结构
 * 
 * 包含错误码和详细错误信息
 */
struct ErrorInfo {
    ErrorCode code;              // 错误码
    String message;              // 错误信息
    String details;              // 详细信息
    unsigned long timestamp;     // 错误发生时间
};

/**
 * @brief 错误处理类
 * 
 * 提供错误处理和错误信息管理功能
 */
class ErrorHandler {
public:
    // 禁止实例化，所有方法都是静态的
    ErrorHandler() = delete;
    ErrorHandler(const ErrorHandler&) = delete;
    ErrorHandler& operator=(const ErrorHandler&) = delete;

    /**
     * @brief 获取错误码对应的错误消息
     * @param code 错误码
     * @return 错误消息
     */
    static String getErrorMessage(ErrorCode code);

    /**
     * @brief 创建错误信息
     * @param code 错误码
     * @param message 错误消息
     * @param details 详细信息
     * @return 错误信息结构
     */
    static ErrorInfo createError(ErrorCode code, const String& message, const String& details = "");

    /**
     * @brief 记录错误
     * @param error 错误信息
     */
    static void logError(const ErrorInfo& error);

    /**
     * @brief 检查操作是否成功
     * @param code 错误码
     * @return 是否成功
     */
    static bool isSuccess(ErrorCode code);

    /**
     * @brief 检查操作是否失败
     * @param code 错误码
     * @return 是否失败
     */
    static bool isError(ErrorCode code);
};

#endif