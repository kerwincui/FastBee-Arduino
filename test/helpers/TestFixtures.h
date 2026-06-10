/**
 * @file TestFixtures.h
 * @brief 测试夹具基类
 */

#ifndef TEST_FIXTURES_H
#define TEST_FIXTURES_H

#include <Arduino.h>
#include <LittleFS.h>
#include <functional>
#include "TestConfig.h"

class TestFixture {
public:
    virtual void setUp() {
        Serial.println("[TEST] TestFixture::setUp()");
        // 确保文件系统已挂载
        if (!LittleFS.begin()) {
            LittleFS.format();
            LittleFS.begin();
        }
    }
    
    virtual void tearDown() {
        Serial.println("[TEST] TestFixture::tearDown()");
        // 清理测试文件
        cleanupTestFiles();
    }
    
    // 创建临时测试文件
    bool createTestFile(const char* path, const char* content) {
        File f = LittleFS.open(path, "w");
        if (!f) return false;
        f.print(content);
        f.close();
        return true;
    }
    
    // 创建JSON测试文件
    bool createTestJsonFile(const char* path, const JsonDocument& doc) {
        File f = LittleFS.open(path, "w");
        if (!f) return false;
        serializeJson(doc, f);
        f.close();
        return true;
    }
    
    // 删除测试文件
    bool removeTestFile(const char* path) {
        return LittleFS.remove(path);
    }
    
    // 等待条件满足（带超时）
    bool waitForCondition(std::function<bool()> condition, unsigned long timeout) {
        unsigned long start = millis();
        while (millis() - start < timeout) {
            if (condition()) return true;
            delay(10);
        }
        return false;
    }
    
    // 等待网络状态
    bool waitForNetworkStatus(std::function<bool()> condition, unsigned long timeout = TestConfig::NETWORK_TIMEOUT) {
        return waitForCondition(condition, timeout);
    }
    
    // 清理所有测试文件
    void cleanupTestFiles() {
        // 删除测试目录下的所有文件
        File root = LittleFS.open("/test");
        if (!root) return;
        
        File file = root.openNextFile();
        while (file) {
            String path = "/test/";
            path += file.name();
            LittleFS.remove(path);
            file = root.openNextFile();
        }
    }
    
    // 获取测试数据路径
    String getTestDataPath(const char* filename) {
        String path = TestConfig::TEST_DATA_DIR;
        path += "/";
        path += filename;
        return path;
    }
    
    // 模拟网络延迟
    void simulateNetworkDelay(unsigned long ms = 100) {
        delay(ms);
    }
    
    // 重置网络配置
    void resetNetworkConfig() {
        // 删除网络配置文件
        LittleFS.remove("/config/network.json");
    }
    
    // 重置设备配置
    void resetDeviceConfig() {
        // 删除设备配置文件
        LittleFS.remove("/config/device.json");
    }
    
    // 格式化文件系统（完全重置）
    void formatFilesystem() {
        LittleFS.format();
        LittleFS.begin();
    }
};

// 网络测试夹具
class NetworkTestFixture : public TestFixture {
public:
    void setUp() override {
        TestFixture::setUp();
        Serial.println("[TEST] NetworkTestFixture::setUp()");
    }
    
    void tearDown() override {
        Serial.println("[TEST] NetworkTestFixture::tearDown()");
        TestFixture::tearDown();
    }
};

// 安全测试夹具
class SecurityTestFixture : public TestFixture {
public:
    void setUp() override {
        TestFixture::setUp();
        Serial.println("[TEST] SecurityTestFixture::setUp()");
    }
    
    void tearDown() override {
        Serial.println("[TEST] SecurityTestFixture::tearDown()");
        TestFixture::tearDown();
    }
};

#endif // TEST_FIXTURES_H
