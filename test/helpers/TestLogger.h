/**
 * @file TestLogger.h
 * @brief 测试日志工具
 */

#ifndef TEST_LOGGER_H
#define TEST_LOGGER_H

#include <Arduino.h>

// 测试日志级别
enum TestLogLevel {
    TEST_LOG_DEBUG = 0,
    TEST_LOG_INFO = 1,
    TEST_LOG_WARNING = 2,
    TEST_LOG_ERROR = 3
};

class TestLogger {
public:
    static TestLogger& getInstance() {
        static TestLogger instance;
        return instance;
    }
    
    void setLevel(TestLogLevel level) {
        currentLevel = level;
    }
    
    void debug(const char* message) {
        log(TEST_LOG_DEBUG, "[DEBUG]", message);
    }
    
    void info(const char* message) {
        log(TEST_LOG_INFO, "[INFO]", message);
    }
    
    void warning(const char* message) {
        log(TEST_LOG_WARNING, "[WARN]", message);
    }
    
    void error(const char* message) {
        log(TEST_LOG_ERROR, "[ERROR]", message);
    }
    
    void testStart(const char* testName) {
        Serial.printf("\n[TEST] >>> Starting: %s\n", testName);
    }
    
    void testEnd(bool passed) {
        if (passed) {
            Serial.printf("[TEST] <<< PASSED\n");
        } else {
            Serial.printf("[TEST] <<< FAILED\n");
        }
    }
    
    void testEnd(const char* testName, bool passed) {
        if (passed) {
            Serial.printf("[TEST] <<< PASSED: %s\n", testName);
        } else {
            Serial.printf("[TEST] <<< FAILED: %s\n", testName);
        }
    }
    
    void section(const char* sectionName) {
        Serial.printf("\n[TEST] === %s ===\n", sectionName);
    }
    
    void step(const char* description) {
        Serial.printf("[TEST] Step: %s\n", description);
    }
    
    void step(int stepNum, const char* description) {
        Serial.printf("[TEST] Step %d: %s\n", stepNum, description);
    }
    
    void metric(const char* name, float value, const char* unit = "") {
        Serial.printf("[TEST] Metric - %s: %.2f %s\n", name, value, unit);
    }
    
    void metric(const char* name, int value, const char* unit = "") {
        Serial.printf("[TEST] Metric - %s: %d %s\n", name, value, unit);
    }
    
    void metric(const char* name, const char* value) {
        Serial.printf("[TEST] Metric - %s: %s\n", name, value);
    }

private:
    TestLogLevel currentLevel = TEST_LOG_INFO;
    
    void log(TestLogLevel level, const char* prefix, const char* message) {
        if (level >= currentLevel) {
            Serial.printf("[TEST] %s %s\n", prefix, message);
        }
    }
};

// 便捷宏
#define TEST_LOG_DEBUG(msg) TestLogger::getInstance().debug(msg)
#define TEST_LOG_INFO(msg) TestLogger::getInstance().info(msg)
#define TEST_LOG_WARNING(msg) TestLogger::getInstance().warning(msg)
#define TEST_LOG_ERROR(msg) TestLogger::getInstance().error(msg)
#define TEST_START(name) TestLogger::getInstance().testStart(name)
#define TEST_END(name, passed) TestLogger::getInstance().testEnd(name, passed)
#define TEST_SECTION(name) TestLogger::getInstance().section(name)
#define TEST_STEP(num, desc) TestLogger::getInstance().step(num, desc)
#define TEST_METRIC(name, value) TestLogger::getInstance().metric(name, value)

// 静态包装类
class TestLog {
public:
    static void testStart(const char* name) {
        TestLogger::getInstance().testStart(name);
    }
    static void testEnd(bool passed) {
        TestLogger::getInstance().testEnd(passed);
    }
    static void step(const char* desc) {
        TestLogger::getInstance().step(desc);
    }
    static void step(int num, const char* desc) {
        TestLogger::getInstance().step(num, desc);
    }
    static void groupStart(const char* name) {
        Serial.printf("\n[TEST] ===== Test Group: %s =====\n", name);
    }
    static void groupEnd() {
        Serial.printf("[TEST] ===== End of Test Group =====\n\n");
    }
};

#endif // TEST_LOGGER_H
