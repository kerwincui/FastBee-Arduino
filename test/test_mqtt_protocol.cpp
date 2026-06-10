/**
 * @file test_mqtt_protocol.cpp
 * @brief MQTT Protocol Tests
 */

#include <unity.h>
#include <Arduino.h>
#include "mocks/MockMQTTClient.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

void test_mqtt_protocol_group();

// Test MQTT connection
void test_mqtt_connection() {
    TestLog::testStart("MQTT Connection");
    
    MockMQTTClient mqtt;
    
    MQTTConfig config;
    config.enabled = true;
    config.server = "test.mqtt.server";
    config.port = 1883;
    config.clientId = "TestDevice001";
    config.username = "testuser";
    config.password = "testpass";
    config.autoReconnect = true;
    
    mqtt.initialize(config);
    TestLog::step("MQTT client initialized");
    
    TEST_ASSERT_TRUE(mqtt.connect());
    TEST_ASSERT_TRUE(mqtt.getIsConnected());
    TestLog::step("MQTT connected successfully");
    
    mqtt.disconnect();
    TEST_ASSERT_FALSE(mqtt.getIsConnected());
    TestLog::step("MQTT disconnected successfully");
    
    TestLog::testEnd(true);
}

// Test MQTT simple authentication
void test_mqtt_simple_auth() {
    TestLog::testStart("MQTT Simple Authentication");
    
    MockMQTTClient mqtt;
    
    String clientId = MockMQTTClient::buildClientId("S", "DEV001", "PROD001", "USER001");
    TEST_ASSERT_TRUE(clientId.startsWith("S&"));
    TEST_ASSERT_EQUAL_STRING("S&DEV001&PROD001&USER001", clientId.c_str());
    TestLog::step("Simple auth clientId built");
    
    int authType = MockMQTTClient::detectAuthType(clientId.c_str());
    TEST_ASSERT_EQUAL(0, authType);
    TestLog::step("Auth type detected: 0 (Simple)");
    
    MQTTConfig config;
    config.enabled = true;
    config.server = "test.mqtt.server";
    config.port = 1883;
    config.clientId = clientId;
    config.username = "testuser";
    config.password = "testpass";
    
    mqtt.initialize(config);
    TEST_ASSERT_TRUE(mqtt.connect());
    TestLog::step("Connected with simple auth");
    
    TestLog::testEnd(true);
}

// Test MQTT encrypted authentication
void test_mqtt_encrypted_auth() {
    TestLog::testStart("MQTT Encrypted Authentication");
    
    MockMQTTClient mqtt;
    
    String clientId = MockMQTTClient::buildClientId("E", "DEV001", "PROD001", "USER001");
    TEST_ASSERT_TRUE(clientId.startsWith("E&"));
    TestLog::step("Encrypted auth clientId built");
    
    int authType = MockMQTTClient::detectAuthType(clientId.c_str());
    TEST_ASSERT_EQUAL(1, authType);
    TestLog::step("Auth type detected: 1 (Encrypted)");
    
    String encryptedPwd = MockMQTTClient::buildEncryptedPassword(
        "password123", "authCode123", "mqttSecret123456", "ntp.fastbee.cn"
    );
    TEST_ASSERT_TRUE(encryptedPwd.startsWith("ENC:"));
    TEST_ASSERT_FALSE(encryptedPwd.isEmpty());
    TestLog::step("Encrypted password generated");
    
    MQTTConfig config;
    config.enabled = true;
    config.server = "test.mqtt.server";
    config.port = 1883;
    config.clientId = clientId;
    config.username = "testuser";
    config.password = encryptedPwd;
    
    mqtt.initialize(config);
    TEST_ASSERT_TRUE(mqtt.connect());
    TestLog::step("Connected with encrypted auth");
    
    TestLog::testEnd(true);
}

// Test MQTT auth failure
void test_mqtt_auth_failure() {
    TestLog::testStart("MQTT Authentication Failure");
    
    MockMQTTClient mqtt;
    
    MQTTConfig config;
    config.enabled = true;
    config.server = "test.mqtt.server";
    config.port = 1883;
    config.clientId = "InvalidClient";
    config.username = "";
    config.password = "";
    
    mqtt.initialize(config);
    
    TEST_ASSERT_FALSE(mqtt.connect());
    TEST_ASSERT_FALSE(mqtt.getIsConnected());
    TEST_ASSERT_NOT_EQUAL(0, mqtt.getLastError());
    TestLog::step("Auth failure detected");
    
    TestLog::testEnd(true);
}

// Test MQTT reconnect
void test_mqtt_reconnect() {
    TestLog::testStart("MQTT Reconnect Mechanism");
    
    MockMQTTClient mqtt;
    
    MQTTConfig config;
    config.enabled = true;
    config.server = "test.mqtt.server";
    config.port = 1883;
    config.clientId = "TestDevice";
    config.username = "user";
    config.password = "pass";
    config.autoReconnect = true;
    config.reconnectInterval = 5000;
    
    mqtt.initialize(config);
    
    TEST_ASSERT_TRUE(mqtt.connect());
    TestLog::step("Initial connection established");
    
    mqtt.setConnected(false);
    TEST_ASSERT_FALSE(mqtt.getIsConnected());
    TestLog::step("Connection lost");
    
    TEST_ASSERT_TRUE(mqtt.reconnect());
    TEST_ASSERT_EQUAL(1, mqtt.getReconnectCount());
    TestLog::step("Reconnected, count: 1");
    
    mqtt.setConnected(false);
    TEST_ASSERT_TRUE(mqtt.reconnect());
    TEST_ASSERT_EQUAL(2, mqtt.getReconnectCount());
    TestLog::step("Reconnected again, count: 2");
    
    TestLog::testEnd(true);
}

// Test MQTT publish
void test_mqtt_publish() {
    TestLog::testStart("MQTT Publish");
    
    MockMQTTClient mqtt;
    
    MQTTConfig config;
    config.enabled = true;
    config.server = "test.mqtt.server";
    config.port = 1883;
    config.clientId = "TestDevice";
    
    mqtt.initialize(config);
    TEST_ASSERT_TRUE(mqtt.connect());
    TestLog::step("Connected");
    
    TEST_ASSERT_TRUE(mqtt.publish("test/topic", "Hello MQTT"));
    TestLog::step("Published to test/topic");
    
    TEST_ASSERT_TRUE(mqtt.publish("test/qos1", "QoS 1 message", false, QOS1));
    TestLog::step("Published with QoS1");
    
    auto messages = mqtt.getPublishedMessages();
    TEST_ASSERT_EQUAL(2, messages.size());
    TestLog::step("Published messages count: 2");
    
    TestLog::testEnd(true);
}

// Test MQTT subscribe
void test_mqtt_subscribe() {
    TestLog::testStart("MQTT Subscribe");
    
    MockMQTTClient mqtt;
    
    MQTTConfig config;
    config.enabled = true;
    config.server = "test.mqtt.server";
    config.port = 1883;
    config.clientId = "TestDevice";
    
    mqtt.initialize(config);
    TEST_ASSERT_TRUE(mqtt.connect());
    TestLog::step("Connected");
    
    TEST_ASSERT_TRUE(mqtt.subscribe("test/subscribe"));
    TestLog::step("Subscribed to test/subscribe");
    
    TEST_ASSERT_TRUE(mqtt.subscribe("test/qos1", QOS1));
    TestLog::step("Subscribed to test/qos1 with QoS1");
    
    auto subs = mqtt.getSubscriptions();
    TEST_ASSERT_EQUAL(2, subs.size());
    TestLog::step("Subscription count: 2");
    
    TEST_ASSERT_TRUE(mqtt.unsubscribe("test/subscribe"));
    TestLog::step("Unsubscribed from test/subscribe");
    
    subs = mqtt.getSubscriptions();
    TEST_ASSERT_EQUAL(1, subs.size());
    TestLog::step("Subscription count after unsubscribe: 1");
    
    TestLog::testEnd(true);
}

// Test MQTT callback
void test_mqtt_callback() {
    TestLog::testStart("MQTT Message Callback");
    
    MockMQTTClient mqtt;
    
    MQTTConfig config;
    config.enabled = true;
    config.server = "test.mqtt.server";
    config.port = 1883;
    config.clientId = "TestDevice";
    
    mqtt.initialize(config);
    TEST_ASSERT_TRUE(mqtt.connect());
    
    volatile bool callbackReceived = false;
    String receivedTopic;
    String receivedPayload;
    
    mqtt.setCallback([&](const char* topic, const uint8_t* payload, unsigned int length) {
        callbackReceived = true;
        receivedTopic = String(topic);
        receivedPayload = String((const char*)payload, length);
    });
    TestLog::step("Callback set");
    
    mqtt.simulateMessage("test/callback", "Test message");
    
    TEST_ASSERT_TRUE(callbackReceived);
    TEST_ASSERT_EQUAL_STRING("test/callback", receivedTopic.c_str());
    TestLog::step("Callback received message");
    
    TestLog::testEnd(true);
}

// Test MQTT will message
void test_mqtt_will_message() {
    TestLog::testStart("MQTT Will Message");
    
    MockMQTTClient mqtt;
    
    MQTTConfig config;
    config.enabled = true;
    config.server = "test.mqtt.server";
    config.port = 1883;
    config.clientId = "TestDevice";
    config.willTopic = "device/status";
    config.willPayload = "offline";
    config.willQos = 1;
    config.willRetain = true;
    
    mqtt.initialize(config);
    
    MQTTConfig savedConfig = mqtt.getConfig();
    TEST_ASSERT_EQUAL_STRING("device/status", savedConfig.willTopic.c_str());
    TEST_ASSERT_EQUAL_STRING("offline", savedConfig.willPayload.c_str());
    TEST_ASSERT_EQUAL(1, savedConfig.willQos);
    TEST_ASSERT_TRUE(savedConfig.willRetain);
    TestLog::step("Will message configured");
    
    TestLog::testEnd(true);
}

// Test MQTT connection failure
void test_mqtt_connection_failure() {
    TestLog::testStart("MQTT Connection Failure Handling");
    
    MockMQTTClient mqtt;
    
    MQTTConfig config;
    config.enabled = true;
    config.server = "invalid.server";
    config.port = 1883;
    config.clientId = "TestDevice";
    
    mqtt.initialize(config);
    
    mqtt.setShouldFailConnect(true);
    
    TEST_ASSERT_FALSE(mqtt.connect());
    TEST_ASSERT_FALSE(mqtt.getIsConnected());
    TEST_ASSERT_NOT_EQUAL(0, mqtt.getLastError());
    TestLog::step("Connection failure handled");
    
    TEST_ASSERT_FALSE(mqtt.publish("test/topic", "message"));
    TestLog::step("Publish rejected when disconnected");
    
    TEST_ASSERT_FALSE(mqtt.subscribe("test/topic"));
    TestLog::step("Subscribe rejected when disconnected");
    
    TestLog::testEnd(true);
}

// Test MQTT keepalive
void test_mqtt_keepalive() {
    TestLog::testStart("MQTT KeepAlive");
    
    MockMQTTClient mqtt;
    
    MQTTConfig config;
    config.enabled = true;
    config.server = "test.mqtt.server";
    config.port = 1883;
    config.clientId = "TestDevice";
    config.keepAlive = 60;
    
    mqtt.initialize(config);
    
    MQTTConfig savedConfig = mqtt.getConfig();
    TEST_ASSERT_EQUAL(60, savedConfig.keepAlive);
    TestLog::step("KeepAlive configured: 60s");
    
    TestLog::testEnd(true);
}

// ============================================================
// MQTT 堆保护测试（模拟低内存场景下的防护逻辑）
// ============================================================

// 模拟 MQTT 堆保护逻辑（复用生产代码中的阈值）
class HeapProtectedMQTTClient {
public:
    static constexpr uint32_t PUBLISH_INFO_THRESHOLD = 30000;
    static constexpr uint32_t MONITOR_DATA_THRESHOLD = 25000;
    static constexpr uint32_t QUEUED_COMMANDS_THRESHOLD = 30000;
    static constexpr uint32_t QUEUED_REPORTS_THRESHOLD = 20000;

    HeapProtectedMQTTClient() : _connected(true) {}

    bool publishDeviceInfo() {
        if (!_connected) return false;
        if (ESP.getFreeHeap() < PUBLISH_INFO_THRESHOLD) {
            _skippedOps.push_back("publishDeviceInfo");
            return false;
        }
        _executedOps.push_back("publishDeviceInfo");
        return true;
    }

    bool publishMonitorData() {
        if (!_connected) return false;
        if (ESP.getFreeHeap() < MONITOR_DATA_THRESHOLD) {
            _skippedOps.push_back("publishMonitorData");
            return false;
        }
        _executedOps.push_back("publishMonitorData");
        return true;
    }

    void processQueuedCommands() {
        if (ESP.getFreeHeap() < QUEUED_COMMANDS_THRESHOLD) {
            _skippedOps.push_back("processQueuedCommands");
            return;
        }
        _executedOps.push_back("processQueuedCommands");
    }

    void processQueuedReports() {
        if (ESP.getFreeHeap() < QUEUED_REPORTS_THRESHOLD) {
            _skippedOps.push_back("processQueuedReports");
            return;
        }
        _executedOps.push_back("processQueuedReports");
    }

    void setConnected(bool c) { _connected = c; }
    std::vector<String> getSkippedOps() { return _skippedOps; }
    std::vector<String> getExecutedOps() { return _executedOps; }
    void reset() { _skippedOps.clear(); _executedOps.clear(); }

private:
    bool _connected;
    std::vector<String> _skippedOps;
    std::vector<String> _executedOps;
};

// Test: publishDeviceInfo 堆保护
void test_mqtt_heap_protection_publish_device_info() {
    TestLog::testStart("MQTT Heap Protection: publishDeviceInfo");
    
    HeapProtectedMQTTClient mqtt;
    
    // 堆充足时正常发布
    ESP.setFreeHeap(50000);
    TEST_ASSERT_TRUE(mqtt.publishDeviceInfo());
    TestLog::step("Heap=50KB: publishDeviceInfo → success");
    
    // 堆低于 30KB 时跳过
    ESP.setFreeHeap(25000);
    TEST_ASSERT_FALSE(mqtt.publishDeviceInfo());
    TestLog::step("Heap=25KB: publishDeviceInfo → SKIPPED");
    
    // 边界值：刚好 30KB 应该正常
    ESP.setFreeHeap(30000);
    TEST_ASSERT_TRUE(mqtt.publishDeviceInfo());
    TestLog::step("Heap=30KB (boundary): publishDeviceInfo → success");
    
    // 边界值：29999 应该跳过
    ESP.setFreeHeap(29999);
    TEST_ASSERT_FALSE(mqtt.publishDeviceInfo());
    TestLog::step("Heap=29999 (below): publishDeviceInfo → SKIPPED");
    
    auto skipped = mqtt.getSkippedOps();
    TEST_ASSERT_EQUAL(2, skipped.size());
    TestLog::step("Total skipped: 2");
    
    ESP.resetHeapOverride();
    TestLog::testEnd(true);
}

// Test: publishMonitorData 堆保护
void test_mqtt_heap_protection_publish_monitor_data() {
    TestLog::testStart("MQTT Heap Protection: publishMonitorData");
    
    HeapProtectedMQTTClient mqtt;
    
    // 堆充足
    ESP.setFreeHeap(40000);
    TEST_ASSERT_TRUE(mqtt.publishMonitorData());
    TestLog::step("Heap=40KB: publishMonitorData → success");
    
    // 堆低于 25KB 跳过
    ESP.setFreeHeap(20000);
    TEST_ASSERT_FALSE(mqtt.publishMonitorData());
    TestLog::step("Heap=20KB: publishMonitorData → SKIPPED");
    
    // 边界值: 25000 应正常
    ESP.setFreeHeap(25000);
    TEST_ASSERT_TRUE(mqtt.publishMonitorData());
    TestLog::step("Heap=25KB (boundary): publishMonitorData → success");
    
    // 边界值: 24999 跳过
    ESP.setFreeHeap(24999);
    TEST_ASSERT_FALSE(mqtt.publishMonitorData());
    TestLog::step("Heap=24999 (below): publishMonitorData → SKIPPED");
    
    ESP.resetHeapOverride();
    TestLog::testEnd(true);
}

// Test: processQueuedCommands 堆保护
void test_mqtt_heap_protection_queued_commands() {
    TestLog::testStart("MQTT Heap Protection: processQueuedCommands");
    
    HeapProtectedMQTTClient mqtt;
    
    // 堆充足时处理队列
    ESP.setFreeHeap(50000);
    mqtt.processQueuedCommands();
    TEST_ASSERT_EQUAL(1, mqtt.getExecutedOps().size());
    TestLog::step("Heap=50KB: processQueuedCommands → executed");
    
    // 堆低于 30KB 跳过
    ESP.setFreeHeap(20000);
    mqtt.processQueuedCommands();
    TEST_ASSERT_EQUAL(1, mqtt.getSkippedOps().size());
    TestLog::step("Heap=20KB: processQueuedCommands → SKIPPED");
    
    ESP.resetHeapOverride();
    TestLog::testEnd(true);
}

// Test: processQueuedReports 堆保护
void test_mqtt_heap_protection_queued_reports() {
    TestLog::testStart("MQTT Heap Protection: processQueuedReports");
    
    HeapProtectedMQTTClient mqtt;
    
    // 堆充足
    ESP.setFreeHeap(50000);
    mqtt.processQueuedReports();
    TEST_ASSERT_EQUAL(1, mqtt.getExecutedOps().size());
    TestLog::step("Heap=50KB: processQueuedReports → executed");
    
    // 堆低于 20KB 跳过
    ESP.setFreeHeap(15000);
    mqtt.processQueuedReports();
    TEST_ASSERT_EQUAL(1, mqtt.getSkippedOps().size());
    TestLog::step("Heap=15KB: processQueuedReports → SKIPPED");
    
    // 边界: 20000 应正常
    ESP.setFreeHeap(20000);
    mqtt.processQueuedReports();
    TEST_ASSERT_EQUAL(2, mqtt.getExecutedOps().size());
    TestLog::step("Heap=20KB (boundary): processQueuedReports → executed");
    
    // 边界: 19999 跳过
    ESP.setFreeHeap(19999);
    mqtt.processQueuedReports();
    TEST_ASSERT_EQUAL(2, mqtt.getSkippedOps().size());
    TestLog::step("Heap=19999 (below): processQueuedReports → SKIPPED");
    
    ESP.resetHeapOverride();
    TestLog::testEnd(true);
}

// Test: 断连时堆保护不应被触发（先检查连接状态）
void test_mqtt_heap_protection_disconnected_priority() {
    TestLog::testStart("MQTT Heap Protection: Disconnected Priority");
    
    HeapProtectedMQTTClient mqtt;
    mqtt.setConnected(false);
    
    // 即使堆充足，断连时也应返回 false
    ESP.setFreeHeap(100000);
    TEST_ASSERT_FALSE(mqtt.publishDeviceInfo());
    TEST_ASSERT_FALSE(mqtt.publishMonitorData());
    TestLog::step("Disconnected: publish returns false regardless of heap");
    
    // 确认是因为断连而不是堆保护
    TEST_ASSERT_EQUAL(0, mqtt.getSkippedOps().size());
    TestLog::step("No heap-skip recorded (connection check is first)");
    
    ESP.resetHeapOverride();
    TestLog::testEnd(true);
}

// Test group entry point
void test_mqtt_protocol_group() {
    TestLog::groupStart("MQTT Protocol Tests");
    
    RUN_TEST(test_mqtt_connection);
    RUN_TEST(test_mqtt_simple_auth);
    RUN_TEST(test_mqtt_encrypted_auth);
    RUN_TEST(test_mqtt_auth_failure);
    RUN_TEST(test_mqtt_reconnect);
    RUN_TEST(test_mqtt_publish);
    RUN_TEST(test_mqtt_subscribe);
    RUN_TEST(test_mqtt_callback);
    RUN_TEST(test_mqtt_will_message);
    RUN_TEST(test_mqtt_connection_failure);
    RUN_TEST(test_mqtt_keepalive);
    
    // Heap Protection Tests
    RUN_TEST(test_mqtt_heap_protection_publish_device_info);
    RUN_TEST(test_mqtt_heap_protection_publish_monitor_data);
    RUN_TEST(test_mqtt_heap_protection_queued_commands);
    RUN_TEST(test_mqtt_heap_protection_queued_reports);
    RUN_TEST(test_mqtt_heap_protection_disconnected_priority);
    
    TestLog::groupEnd();
}
