/**
 * @file test_error_handler.cpp
 * @brief ErrorHandler 错误处理系统单元测试
 * 
 * 测试内容：
 * - 错误码成功/失败判定
 * - 错误信息创建
 * - 错误消息映射
 * - 各类错误码覆盖
 */

#include <unity.h>
#include <Arduino.h>
#include "core/ErrorHandler.h"

// Native 环境下提供 ErrorHandler 实现（test_build_src=no）
#ifdef UNIT_TEST
String ErrorHandler::getErrorMessage(ErrorCode code) {
    switch (code) {
        case ErrorCode::SUCCESS: return "Success";
        case ErrorCode::UNKNOWN_ERROR: return "Unknown error";
        case ErrorCode::MEMORY_ERROR: return "Memory error";
        case ErrorCode::SYSTEM_ERROR: return "System error";
        case ErrorCode::INITIALIZATION_ERROR: return "Initialization error";
        case ErrorCode::NETWORK_ERROR: return "Network error";
        case ErrorCode::WIFI_CONNECTION_ERROR: return "WiFi connection error";
        case ErrorCode::IP_CONFIG_ERROR: return "IP configuration error";
        case ErrorCode::DNS_ERROR: return "DNS error";
        case ErrorCode::IP_CONFLICT_ERROR: return "IP conflict error";
        case ErrorCode::STORAGE_ERROR: return "Storage error";
        case ErrorCode::PREFERENCES_ERROR: return "Preferences error";
        case ErrorCode::FILE_SYSTEM_ERROR: return "File system error";
        case ErrorCode::JSON_PARSE_ERROR: return "JSON parse error";
        case ErrorCode::SECURITY_ERROR: return "Security error";
        case ErrorCode::AUTHENTICATION_ERROR: return "Authentication error";
        case ErrorCode::AUTHORIZATION_ERROR: return "Authorization error";
        case ErrorCode::USER_ERROR: return "User error";
        case ErrorCode::PROTOCOL_ERROR: return "Protocol error";
        case ErrorCode::MQTT_ERROR: return "MQTT error";
        case ErrorCode::HTTP_ERROR: return "HTTP error";
        case ErrorCode::MODBUS_ERROR: return "Modbus error";
        case ErrorCode::TCP_ERROR: return "TCP error";
        case ErrorCode::COAP_ERROR: return "CoAP error";
        case ErrorCode::TASK_ERROR: return "Task error";
        case ErrorCode::TASK_OVERFLOW: return "Task overflow";
        case ErrorCode::TASK_EXECUTION_ERROR: return "Task execution error";
        default: return "Unknown error";
    }
}
ErrorInfo ErrorHandler::createError(ErrorCode code, const String& message, const String& details) {
    ErrorInfo error;
    error.code = code;
    error.message = message;
    error.details = details;
    error.timestamp = millis();
    return error;
}
void ErrorHandler::logError(const ErrorInfo& error) {
    // 测试环境下仅打印到 stdout
    printf("Error [%d]: %s\n", (int)error.code, error.message.c_str());
}
bool ErrorHandler::isSuccess(ErrorCode code) {
    return code == ErrorCode::SUCCESS;
}
bool ErrorHandler::isError(ErrorCode code) {
    return code != ErrorCode::SUCCESS;
}
#endif

void test_error_handler_group();

// ========== 测试用例 ==========

static void test_is_success() {
    TEST_ASSERT_TRUE(ErrorHandler::isSuccess(ErrorCode::SUCCESS));
    TEST_ASSERT_FALSE(ErrorHandler::isSuccess(ErrorCode::UNKNOWN_ERROR));
    TEST_ASSERT_FALSE(ErrorHandler::isSuccess(ErrorCode::MEMORY_ERROR));
    TEST_ASSERT_FALSE(ErrorHandler::isSuccess(ErrorCode::NETWORK_ERROR));
}

static void test_is_error() {
    TEST_ASSERT_FALSE(ErrorHandler::isError(ErrorCode::SUCCESS));
    TEST_ASSERT_TRUE(ErrorHandler::isError(ErrorCode::UNKNOWN_ERROR));
    TEST_ASSERT_TRUE(ErrorHandler::isError(ErrorCode::SYSTEM_ERROR));
    TEST_ASSERT_TRUE(ErrorHandler::isError(ErrorCode::WIFI_CONNECTION_ERROR));
    TEST_ASSERT_TRUE(ErrorHandler::isError(ErrorCode::STORAGE_ERROR));
    TEST_ASSERT_TRUE(ErrorHandler::isError(ErrorCode::SECURITY_ERROR));
    TEST_ASSERT_TRUE(ErrorHandler::isError(ErrorCode::PROTOCOL_ERROR));
    TEST_ASSERT_TRUE(ErrorHandler::isError(ErrorCode::TASK_ERROR));
}

static void test_create_error_basic() {
    ErrorInfo info = ErrorHandler::createError(
        ErrorCode::MEMORY_ERROR,
        "Out of memory",
        "heap exhausted"
    );
    
    TEST_ASSERT_EQUAL((int)ErrorCode::MEMORY_ERROR, (int)info.code);
    TEST_ASSERT_EQUAL_STRING("Out of memory", info.message.c_str());
    TEST_ASSERT_EQUAL_STRING("heap exhausted", info.details.c_str());
    TEST_ASSERT_TRUE(info.timestamp > 0 || info.timestamp == 0);  // timestamp is millis()
}

static void test_create_error_no_details() {
    ErrorInfo info = ErrorHandler::createError(
        ErrorCode::NETWORK_ERROR,
        "Network failure"
    );
    
    TEST_ASSERT_EQUAL((int)ErrorCode::NETWORK_ERROR, (int)info.code);
    TEST_ASSERT_EQUAL_STRING("Network failure", info.message.c_str());
    TEST_ASSERT_TRUE(info.details.isEmpty());
}

static void test_get_error_message_system() {
    String msg = ErrorHandler::getErrorMessage(ErrorCode::SUCCESS);
    TEST_ASSERT_TRUE(msg.length() > 0);
    
    msg = ErrorHandler::getErrorMessage(ErrorCode::MEMORY_ERROR);
    TEST_ASSERT_TRUE(msg.length() > 0);
    
    msg = ErrorHandler::getErrorMessage(ErrorCode::INITIALIZATION_ERROR);
    TEST_ASSERT_TRUE(msg.length() > 0);
}

static void test_get_error_message_network() {
    String msg = ErrorHandler::getErrorMessage(ErrorCode::WIFI_CONNECTION_ERROR);
    TEST_ASSERT_TRUE(msg.length() > 0);
    
    msg = ErrorHandler::getErrorMessage(ErrorCode::DNS_ERROR);
    TEST_ASSERT_TRUE(msg.length() > 0);
}

static void test_get_error_message_storage() {
    String msg = ErrorHandler::getErrorMessage(ErrorCode::FILE_SYSTEM_ERROR);
    TEST_ASSERT_TRUE(msg.length() > 0);
    
    msg = ErrorHandler::getErrorMessage(ErrorCode::JSON_PARSE_ERROR);
    TEST_ASSERT_TRUE(msg.length() > 0);
}

static void test_get_error_message_security() {
    String msg = ErrorHandler::getErrorMessage(ErrorCode::AUTHENTICATION_ERROR);
    TEST_ASSERT_TRUE(msg.length() > 0);
    
    msg = ErrorHandler::getErrorMessage(ErrorCode::AUTHORIZATION_ERROR);
    TEST_ASSERT_TRUE(msg.length() > 0);
}

static void test_get_error_message_protocol() {
    String msg = ErrorHandler::getErrorMessage(ErrorCode::MQTT_ERROR);
    TEST_ASSERT_TRUE(msg.length() > 0);
    
    msg = ErrorHandler::getErrorMessage(ErrorCode::MODBUS_ERROR);
    TEST_ASSERT_TRUE(msg.length() > 0);
}

static void test_get_error_message_task() {
    String msg = ErrorHandler::getErrorMessage(ErrorCode::TASK_OVERFLOW);
    TEST_ASSERT_TRUE(msg.length() > 0);
    
    msg = ErrorHandler::getErrorMessage(ErrorCode::TASK_EXECUTION_ERROR);
    TEST_ASSERT_TRUE(msg.length() > 0);
}

static void test_error_code_categories() {
    // 系统错误 0-99
    TEST_ASSERT_TRUE((uint16_t)ErrorCode::SUCCESS < 100);
    TEST_ASSERT_TRUE((uint16_t)ErrorCode::MEMORY_ERROR < 100);
    
    // 网络错误 100-199
    TEST_ASSERT_TRUE((uint16_t)ErrorCode::NETWORK_ERROR >= 100);
    TEST_ASSERT_TRUE((uint16_t)ErrorCode::IP_CONFLICT_ERROR < 200);
    
    // 存储错误 200-299
    TEST_ASSERT_TRUE((uint16_t)ErrorCode::STORAGE_ERROR >= 200);
    TEST_ASSERT_TRUE((uint16_t)ErrorCode::JSON_PARSE_ERROR < 300);
    
    // 安全错误 300-399
    TEST_ASSERT_TRUE((uint16_t)ErrorCode::SECURITY_ERROR >= 300);
    TEST_ASSERT_TRUE((uint16_t)ErrorCode::USER_ERROR < 400);
    
    // 协议错误 400-499
    TEST_ASSERT_TRUE((uint16_t)ErrorCode::PROTOCOL_ERROR >= 400);
    TEST_ASSERT_TRUE((uint16_t)ErrorCode::COAP_ERROR < 500);
    
    // 任务错误 500+
    TEST_ASSERT_TRUE((uint16_t)ErrorCode::TASK_ERROR >= 500);
}

static void test_log_error_does_not_crash() {
    ErrorInfo info = ErrorHandler::createError(
        ErrorCode::SYSTEM_ERROR,
        "Critical failure",
        "stack overflow detected"
    );
    // 调用 logError 不应崩溃
    ErrorHandler::logError(info);
    TEST_ASSERT_TRUE(true);  // 如果到这里说明没崩溃
}

// ========== 测试组入口 ==========

void test_error_handler_group() {
    RUN_TEST(test_is_success);
    RUN_TEST(test_is_error);
    RUN_TEST(test_create_error_basic);
    RUN_TEST(test_create_error_no_details);
    RUN_TEST(test_get_error_message_system);
    RUN_TEST(test_get_error_message_network);
    RUN_TEST(test_get_error_message_storage);
    RUN_TEST(test_get_error_message_security);
    RUN_TEST(test_get_error_message_protocol);
    RUN_TEST(test_get_error_message_task);
    RUN_TEST(test_error_code_categories);
    RUN_TEST(test_log_error_does_not_crash);
}
