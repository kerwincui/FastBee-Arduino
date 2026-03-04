/**
 * @file ErrorHandler.cpp
 * @brief 错误处理类实现
 * @author kerwincui
 * @date 2026-03-03
 */

#include "core/ErrorHandler.h"
#include "systems/LoggerSystem.h"

/**
 * @brief 获取错误码对应的错误消息
 * @param code 错误码
 * @return 错误消息
 */
String ErrorHandler::getErrorMessage(ErrorCode code) {
    switch (code) {
        // 系统错误
        case ErrorCode::SUCCESS: return "Success";
        case ErrorCode::UNKNOWN_ERROR: return "Unknown error";
        case ErrorCode::MEMORY_ERROR: return "Memory error";
        case ErrorCode::SYSTEM_ERROR: return "System error";
        case ErrorCode::INITIALIZATION_ERROR: return "Initialization error";
        
        // 网络错误
        case ErrorCode::NETWORK_ERROR: return "Network error";
        case ErrorCode::WIFI_CONNECTION_ERROR: return "WiFi connection error";
        case ErrorCode::IP_CONFIG_ERROR: return "IP configuration error";
        case ErrorCode::DNS_ERROR: return "DNS error";
        case ErrorCode::IP_CONFLICT_ERROR: return "IP conflict error";
        
        // 存储错误
        case ErrorCode::STORAGE_ERROR: return "Storage error";
        case ErrorCode::PREFERENCES_ERROR: return "Preferences error";
        case ErrorCode::FILE_SYSTEM_ERROR: return "File system error";
        case ErrorCode::JSON_PARSE_ERROR: return "JSON parse error";
        
        // 安全错误
        case ErrorCode::SECURITY_ERROR: return "Security error";
        case ErrorCode::AUTHENTICATION_ERROR: return "Authentication error";
        case ErrorCode::AUTHORIZATION_ERROR: return "Authorization error";
        case ErrorCode::USER_ERROR: return "User error";
        
        // 协议错误
        case ErrorCode::PROTOCOL_ERROR: return "Protocol error";
        case ErrorCode::MQTT_ERROR: return "MQTT error";
        case ErrorCode::HTTP_ERROR: return "HTTP error";
        case ErrorCode::MODBUS_ERROR: return "Modbus error";
        case ErrorCode::TCP_ERROR: return "TCP error";
        case ErrorCode::COAP_ERROR: return "CoAP error";
        
        // 任务错误
        case ErrorCode::TASK_ERROR: return "Task error";
        case ErrorCode::TASK_OVERFLOW: return "Task overflow";
        case ErrorCode::TASK_EXECUTION_ERROR: return "Task execution error";
        
        default: return "Unknown error";
    }
}

/**
 * @brief 创建错误信息
 * @param code 错误码
 * @param message 错误消息
 * @param details 详细信息
 * @return 错误信息结构
 */
ErrorInfo ErrorHandler::createError(ErrorCode code, const String& message, const String& details) {
    ErrorInfo error;
    error.code = code;
    error.message = message;
    error.details = details;
    error.timestamp = millis();
    return error;
}

/**
 * @brief 记录错误
 * @param error 错误信息
 */
void ErrorHandler::logError(const ErrorInfo& error) {
    String errorMessage = "Error [" + String(static_cast<uint16_t>(error.code)) + "]: " + error.message;
    if (!error.details.isEmpty()) {
        errorMessage += " - " + error.details;
    }
    LOG_ERROR(errorMessage.c_str());
}

/**
 * @brief 检查操作是否成功
 * @param code 错误码
 * @return 是否成功
 */
bool ErrorHandler::isSuccess(ErrorCode code) {
    return code == ErrorCode::SUCCESS;
}

/**
 * @brief 检查操作是否失败
 * @param code 错误码
 * @return 是否失败
 */
bool ErrorHandler::isError(ErrorCode code) {
    return code != ErrorCode::SUCCESS;
}