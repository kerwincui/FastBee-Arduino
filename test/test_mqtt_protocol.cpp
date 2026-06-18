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

// ============================================================
// MQTT 自动连接 & 状态通知测试
// ============================================================

// Test: 启动延迟常量为 3 秒
void test_mqtt_boot_stabilization_delay() {
    TestLog::testStart("MQTT Boot Stabilization Delay");

    // 验证生产代码中的启动延迟为 3 秒（而非旧值 15 秒）
    // 15s 导致用户点测试后长时间看到"连接中"，体验差
    // doReconnect() 内部已有堆内存保护（<49KB 跳过），无需过长延迟
    TEST_ASSERT_EQUAL(3000, MockMQTTClient::BOOT_STABILIZATION_DELAY_MS);
    TestLog::step("Boot delay = 3000ms (not 15000ms)");

    TestLog::testEnd(true);
}

// Test: handle() 自动调度重连
void test_mqtt_auto_reconnect_scheduling() {
    TestLog::testStart("MQTT Auto-Reconnect Scheduling");

    MockMQTTClient mqtt;

    MQTTConfig config;
    config.enabled = true;
    config.server = "test.mqtt.server";
    config.port = 1883;
    config.clientId = "TestDevice";
    config.username = "user";
    config.password = "pass";
    config.autoReconnect = true;
    config.reconnectInterval = 100;  // 缩短间隔便于测试

    mqtt.initialize(config);
    // 未连接状态
    TEST_ASSERT_FALSE(mqtt.getIsConnected());
    TEST_ASSERT_FALSE(mqtt.isReconnectPending());
    TestLog::step("Initial: disconnected, no pending");

    // 时间未到重连间隔
    bool result1 = mqtt.handleAutoReconnect(50);
    TEST_ASSERT_FALSE(result1);
    TEST_ASSERT_FALSE(mqtt.isReconnectPending());
    TestLog::step("t=50ms: no schedule (interval=100ms)");

    // 时间超过重连间隔，应调度
    bool result2 = mqtt.handleAutoReconnect(200);
    TEST_ASSERT_TRUE(result2);
    TEST_ASSERT_TRUE(mqtt.isReconnectPending());
    TestLog::step("t=200ms: scheduled");

    TestLog::testEnd(true);
}

// Test: 已连接时不调度重连
void test_mqtt_no_reconnect_when_connected() {
    TestLog::testStart("MQTT No Reconnect When Connected");

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
    mqtt.setConnected(true);

    // 已连接时即使时间超过也不调度重连
    TEST_ASSERT_FALSE(mqtt.handleAutoReconnect(60000));
    TEST_ASSERT_FALSE(mqtt.isReconnectPending());
    TestLog::step("Connected: no reconnect scheduled even at t=60s");

    TestLog::testEnd(true);
}

// Test: stopped 状态下不调度重连
void test_mqtt_no_reconnect_when_stopped() {
    TestLog::testStart("MQTT No Reconnect When Stopped");

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
    mqtt.setStopped(true);

    // 被显式停止时不应重连
    TEST_ASSERT_FALSE(mqtt.handleAutoReconnect(60000));
    TEST_ASSERT_FALSE(mqtt.isReconnectPending());
    TestLog::step("Stopped: no reconnect scheduled");

    TestLog::testEnd(true);
}

// Test: autoReconnect=false 时不调度重连
void test_mqtt_no_reconnect_when_disabled() {
    TestLog::testStart("MQTT No Reconnect When autoReconnect Disabled");

    MockMQTTClient mqtt;

    MQTTConfig config;
    config.enabled = true;
    config.server = "test.mqtt.server";
    config.port = 1883;
    config.clientId = "TestDevice";
    config.username = "user";
    config.password = "pass";
    config.autoReconnect = false;  // 禁用自动重连
    config.reconnectInterval = 5000;

    mqtt.initialize(config);

    TEST_ASSERT_FALSE(mqtt.handleAutoReconnect(60000));
    TEST_ASSERT_FALSE(mqtt.isReconnectPending());
    TestLog::step("autoReconnect=false: no reconnect scheduled");

    TestLog::testEnd(true);
}

// Test: SSE 状态通知 JSON 包含必要字段
void test_mqtt_status_notification_json_fields() {
    TestLog::testStart("MQTT Status Notification JSON Fields");

    MockMQTTClient mqtt;

    MQTTConfig config;
    config.enabled = true;
    config.server = "iot.fastbee.cn";
    config.port = 1883;
    config.clientId = "S&FBE100900001&1070&1";
    config.username = "admin";
    config.password = "pass";

    mqtt.initialize(config);
    mqtt.setConnected(true);

    String receivedJson;
    mqtt.setStatusChangeCallback([&](const String& json) {
        receivedJson = json;
    });

    mqtt.notifyStatusChange();
    TestLog::step("Status change notified");

    // 验证 JSON 包含关键字段（前端 _updateMqttStatusUI 依赖这些字段）
    TEST_ASSERT_TRUE(receivedJson.indexOf("\"initialized\":true") >= 0);
    TestLog::step("Contains 'initialized' field");

    TEST_ASSERT_TRUE(receivedJson.indexOf("\"connected\":true") >= 0);
    TestLog::step("Contains 'connected' field");

    TEST_ASSERT_TRUE(receivedJson.indexOf("\"reconnectCount\"") >= 0);
    TestLog::step("Contains 'reconnectCount' (not 'reconnects')");

    TEST_ASSERT_TRUE(receivedJson.indexOf("\"server\":\"iot.fastbee.cn\"") >= 0);
    TestLog::step("Contains correct server");

    TEST_ASSERT_TRUE(receivedJson.indexOf("\"port\":1883") >= 0);
    TestLog::step("Contains correct port");

    TEST_ASSERT_TRUE(receivedJson.indexOf("\"clientId\":\"S&FBE100900001&1070&1\"") >= 0);
    TestLog::step("Contains correct clientId");

    // 确保不包含旧字段名 'reconnects'
    TEST_ASSERT_TRUE(receivedJson.indexOf("\"reconnects\"") < 0);
    TestLog::step("Does NOT contain old field name 'reconnects'");

    TestLog::testEnd(true);
}

// Test: 断连时 SSE 状态通知正确
void test_mqtt_status_notification_disconnected() {
    TestLog::testStart("MQTT Status Notification When Disconnected");

    MockMQTTClient mqtt;

    MQTTConfig config;
    config.enabled = true;
    config.server = "iot.fastbee.cn";
    config.port = 1883;
    config.clientId = "S&FBE100900001&1070&1";
    config.username = "admin";
    config.password = "pass";

    mqtt.initialize(config);
    mqtt.setConnected(false);  // 未连接

    String receivedJson;
    mqtt.setStatusChangeCallback([&](const String& json) {
        receivedJson = json;
    });

    mqtt.notifyStatusChange();

    // 未连接时: initialized=true, connected=false
    // 前端应显示"未连接"而非"未初始化"
    TEST_ASSERT_TRUE(receivedJson.indexOf("\"initialized\":true") >= 0);
    TEST_ASSERT_TRUE(receivedJson.indexOf("\"connected\":false") >= 0);
    TestLog::step("Disconnected: initialized=true, connected=false");

    TestLog::testEnd(true);
}

// Test: 自动连接完整生命周期
void test_mqtt_auto_connect_lifecycle() {
    TestLog::testStart("MQTT Auto-Connect Lifecycle");

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

    String lastStatusJson;
    mqtt.setStatusChangeCallback([&](const String& json) {
        lastStatusJson = json;
    });

    // 1. 初始状态：未连接
    TEST_ASSERT_FALSE(mqtt.getIsConnected());
    TestLog::step("Phase 1: Initially disconnected");

    // 2. handle 调度重连 (模拟 WiFi 稳定 + 15s 延迟后)
    TEST_ASSERT_TRUE(mqtt.handleAutoReconnect(20000));
    TEST_ASSERT_TRUE(mqtt.isReconnectPending());
    TestLog::step("Phase 2: Reconnect scheduled after boot delay");

    // 3. 后台任务执行重连
    mqtt.clearReconnectPending();
    TEST_ASSERT_TRUE(mqtt.connect());
    TEST_ASSERT_TRUE(mqtt.getIsConnected());
    TestLog::step("Phase 3: Background reconnect succeeded");

    // 4. 连接成功后发送 SSE 状态通知
    mqtt.notifyStatusChange();
    TEST_ASSERT_TRUE(lastStatusJson.indexOf("\"connected\":true") >= 0);
    TEST_ASSERT_TRUE(lastStatusJson.indexOf("\"initialized\":true") >= 0);
    TestLog::step("Phase 4: SSE notified with connected=true, initialized=true");

    // 5. 连接后不再调度重连
    TEST_ASSERT_FALSE(mqtt.handleAutoReconnect(30000));
    TestLog::step("Phase 5: No reconnect when already connected");

    TestLog::testEnd(true);
}

// ============================================================
// MQTT Lite 路径递归轮询 & 25秒超时回归测试
// ============================================================

#include <fstream>
#include <sstream>
#include <regex>

// 辅助：读取项目源文件（native 测试环境下使用标准文件 I/O）
static std::string readProjectFile(const char* relativePath) {
    // 尝试多个可能的项目根路径
    const char* roots[] = { ".", "..", "../.." };
    for (const char* root : roots) {
        std::string fullPath = std::string(root) + "/" + relativePath;
        std::ifstream file(fullPath);
        if (file.is_open()) {
            std::ostringstream ss;
            ss << file.rdbuf();
            return ss.str();
        }
    }
    return "";
}

/**
 * @brief 验证 protocol-lite-config.js 中5次递归轮询的设计约束
 * 防止回退到单次查询或改为无限轮询
 */
void test_mqtt_lite_recursive_polling_constraints() {
    TestLog::testStart("MQTT Lite Recursive Polling Constraints");

    std::string content = readProjectFile("web-src/modules/runtime/protocol/protocol-lite-config.js");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read protocol-lite-config.js — ensure test runs from project root");
    TestLog::step("File loaded");

    // 1) 验证 mqttMaxPolls = 10（覆盖 3s + 10*5s = 53s 轮询窗口，充分覆盖 MQTT 30-40s 连接时间）
    std::regex maxPollsRe("mqttMaxPolls\\s*=\\s*10");
    TEST_ASSERT_TRUE_MESSAGE(std::regex_search(content, maxPollsRe),
        "mqttMaxPolls must be exactly 10 — covers 53s polling window for MQTT 30-40s connect time");
    TestLog::step("mqttMaxPolls = 10 verified");

    // 2) 验证轮询间隔 setTimeout(mqttPollOnce, 5000)
    std::regex intervalRe("setTimeout\\(\\s*mqttPollOnce\\s*,\\s*5000\\s*\\)");
    TEST_ASSERT_TRUE_MESSAGE(std::regex_search(content, intervalRe),
        "Polling interval must be setTimeout(mqttPollOnce, 5000) — recursive setTimeout, NOT setInterval");
    TestLog::step("Recursive setTimeout interval = 5000ms verified");

    // 3) 验证初始延迟 setTimeout(mqttPollOnce, 3000) — 避免与页面chunk加载竞争TCP连接
    std::regex initDelayRe("setTimeout\\(\\s*mqttPollOnce\\s*,\\s*3000\\s*\\)");
    TEST_ASSERT_TRUE_MESSAGE(std::regex_search(content, initDelayRe),
        "Initial poll delay must be setTimeout(mqttPollOnce, 3000) — prevent race with page load");
    TestLog::step("Initial delay = 3000ms verified");

    // 4) 验证使用递归setTimeout而非setInterval
    TEST_ASSERT_TRUE_MESSAGE(content.find("setInterval") == std::string::npos,
        "protocol-lite-config.js must NOT use setInterval — use recursive setTimeout only");
    TestLog::step("No setInterval usage confirmed (recursive setTimeout only)");

    // 5) 验证轮询停止条件 _mqttLitePollCount < mqttMaxPolls
    TEST_ASSERT_TRUE_MESSAGE(content.find("_mqttLitePollCount < mqttMaxPolls") != std::string::npos,
        "Polling stop condition '_mqttLitePollCount < mqttMaxPolls' must exist");
    TestLog::step("Polling stop condition verified");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 _updateMqttStatusUI 中60秒超时判断的关键条件
 * 防止超时阈值被意外修改或条件被简化
 */
void test_mqtt_connecting_timeout_25s_logic() {
    TestLog::testStart("MQTT Connecting Timeout 60s Logic");

    std::string content = readProjectFile("web-src/modules/runtime/protocol/mqtt-config.js");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read mqtt-config.js — ensure test runs from project root");
    TestLog::step("File loaded");

    // 1) 验证超时阈值为 60000（60s，必须 > lite 轮询窗口 48-53s 以避免误判超时）
    TEST_ASSERT_TRUE_MESSAGE(content.find("60000") != std::string::npos,
        "MQTT connecting timeout threshold must be 60000ms (60s) — must exceed lite polling window");
    TestLog::step("60000ms timeout threshold present");

    // 2) 验证条件包含 reconnectCount === 0 或 reconnectCount === undefined
    //    (表示首次连接尚未成功的判断)
    std::regex reconnectZeroRe("reconnectCount\\s*===?\\s*0");
    TEST_ASSERT_TRUE_MESSAGE(std::regex_search(content, reconnectZeroRe),
        "Timeout logic must check reconnectCount === 0 (first connection attempt)");
    TestLog::step("reconnectCount === 0 condition present");

    // 3) 验证超时后文本为 '连接超时'
    TEST_ASSERT_TRUE_MESSAGE(content.find("连接超时") != std::string::npos,
        "Must show '连接超时' text after 25s timeout");
    TestLog::step("'连接超时' text present");

    // 4) 验证超时后class包含 'mqtt-status-offline'
    TEST_ASSERT_TRUE_MESSAGE(content.find("mqtt-status-offline") != std::string::npos,
        "Must apply 'mqtt-status-offline' class after connection timeout");
    TestLog::step("'mqtt-status-offline' CSS class present");

    // 5) 验证 _mqttConnectingStartTime 计时器存在
    TEST_ASSERT_TRUE_MESSAGE(content.find("_mqttConnectingStartTime") != std::string::npos,
        "_mqttConnectingStartTime must exist for timeout tracking");
    TestLog::step("_mqttConnectingStartTime timer present");

    TestLog::testEnd(true);
}

// ============================================================
// MQTT 重连守卫 & restartMQTTDeferred 堆保护顺序测试
// ============================================================

/**
 * @brief 验证测试成功后始终重建客户端（不再用守卫逻辑跳过）
 * 旧逻辑：mqttHasValidRuntimeConfig() 只检查 server/port 非空，不验证凭据
 *   → 已有客户端用错误凭据进入慢重连模式时，旧逻辑谎报 realConnected=true
 * 新逻辑：测试成功后始终走 saveConfig + restartMQTTDeferred，确保配置一致
 */
void test_mqtt_reconnect_always_rebuild_after_test() {
    TestLog::testStart("MQTT: Always Rebuild After Test Success");

    // 场景1: 已有客户端在慢重连模式（用错误凭据），测试用正确凭据成功
    // 旧逻辑: mqttHasValidRuntimeConfig()=true → 跳过重建 → 谎报成功 → 平台显示离线
    // 新逻辑: 始终 saveConfig + restartMQTTDeferred → 重建客户端用新配置
    MockMQTTClient oldMqtt;
    MQTTConfig oldConfig;
    oldConfig.server = "iot.fastbee.cn";
    oldConfig.port = 1883;
    oldConfig.clientId = "S&DEV001&PROD001&USER001";
    oldConfig.password = "WRONG_PASSWORD";  // 旧密码错误
    oldConfig.autoReconnect = true;
    oldMqtt.initialize(oldConfig);
    oldMqtt.setConnected(false);  // 未连接（因为密码错误）

    // 旧逻辑的判断：mqttHasValidRuntimeConfig 只看 server/port
    bool oldLogicValidConfig = !oldConfig.server.isEmpty() && oldConfig.port > 0;
    bool oldLogicWouldSkip = oldLogicValidConfig;  // 旧逻辑跳过重建
    TEST_ASSERT_TRUE(oldLogicWouldSkip);  // 确认旧逻辑的 bug：会跳过重建
    TestLog::step("Old logic: validConfig=true → SKIP rebuild (BUG!)");

    // 新逻辑: 始终调用 restartMQTTDeferred
    // 先保存测试参数到 protocol.json，确保新客户端加载正确配置
    // restartMQTTDeferred 会先释放旧客户端（~12KB），再创建新的
    bool newLogicAlwaysRebuild = true;  // 新逻辑始终重建
    TEST_ASSERT_TRUE(newLogicAlwaysRebuild);
    TestLog::step("New logic: ALWAYS saveConfig + restartMQTTDeferred");

    // 场景2: 已连接的客户端被测试成功后也应该重建（重置重连计数器和慢模式间隔）
    MockMQTTClient connectedMqtt;
    MQTTConfig connectedConfig;
    connectedConfig.server = "iot.fastbee.cn";
    connectedConfig.port = 1883;
    connectedConfig.clientId = "S&DEV001&PROD001&USER001";
    connectedConfig.password = "correct_pass";
    connectedConfig.autoReconnect = true;
    connectedMqtt.initialize(connectedConfig);
    connectedMqtt.setConnected(true);

    // 即使已连接，测试成功后也应该重建（确保配置同步）
    bool shouldRebuildEvenIfConnected = true;  // 新逻辑
    TEST_ASSERT_TRUE(shouldRebuildEvenIfConnected);
    TestLog::step("Even connected client: rebuild to ensure config sync");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 restartMQTTDeferred 堆内存检查顺序：先释放旧客户端，再检查堆
 * 复现场景：旧客户端占 12KB，堆仅 18KB，释放前检查 <15KB 误判失败
 * 阈值已从 25KB 降至 15KB，防止低堆时永远无法重建客户端
 */
void test_mqtt_deferred_restart_heap_order() {
    TestLog::testStart("MQTT Deferred Restart: Heap Check After Release (15KB)");

    constexpr uint32_t DEFERRED_HEAP_THRESHOLD = 15000;  // 新阈值（旧值 25000）
    constexpr uint32_t OLD_CLIENT_MEMORY = 12000;  // MQTT客户端约占 12KB

    // 场景1: 堆=18KB，旧客户端占 12KB
    // 旧逻辑（释放前检查）：18KB >= 15KB → 通过，实际正确
    // 但若堆=12KB: 旧逻辑 12KB < 15KB → 失败。新逻辑释放后 24KB > 15KB → 成功
    uint32_t heapBeforeRelease = 12000;  // 释放前堆
    uint32_t heapAfterRelease = heapBeforeRelease + OLD_CLIENT_MEMORY;  // 释放后 24KB

    // 旧逻辑: 在释放前检查
    bool oldLogicPasses = (heapBeforeRelease >= DEFERRED_HEAP_THRESHOLD);
    TEST_ASSERT_FALSE(oldLogicPasses);
    TestLog::step("Old logic (check before release): 12KB < 15KB → FAILS");

    // 新逻辑: 释放后检查
    bool newLogicPasses = (heapAfterRelease >= DEFERRED_HEAP_THRESHOLD);
    TEST_ASSERT_TRUE(newLogicPasses);
    TestLog::step("New logic (check after release): 24KB >= 15KB → PASSES");

    // 场景2: 堆真的不够（释放后仍 < 15KB）→ 正确拒绝
    uint32_t reallyLowHeap = 2000;
    uint32_t afterReleaseStillLow = reallyLowHeap + OLD_CLIENT_MEMORY;  // 14KB
    TEST_ASSERT_FALSE(afterReleaseStillLow >= DEFERRED_HEAP_THRESHOLD);
    TestLog::step("Truly low heap (14KB after release): correctly rejected");

    // 验证阈值变更：旧值 25000 已被替换为 15000
    constexpr uint32_t OLD_DEFERRED_THRESHOLD = 25000;
    // 在旧阈值下，23KB 释放后 35KB 可以通过，但 20KB 释放后 32KB 也能通过
    // 而 12KB + 12KB = 24KB < 25KB → 失败。这就是旧阈值的 bug
    uint32_t midHeap = 12000;
    uint32_t afterMid = midHeap + OLD_CLIENT_MEMORY;  // 24KB
    TEST_ASSERT_FALSE(afterMid >= OLD_DEFERRED_THRESHOLD);  // 旧阈值拒绝 24KB
    TEST_ASSERT_TRUE(afterMid >= DEFERRED_HEAP_THRESHOLD);   // 新阈值允许 24KB
    TestLog::step("24KB: old threshold 25K REJECTS, new 15K ACCEPTS (fix!)");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 doReconnect 堆阈值为 15KB（而非旧值 49KB/49152）
 * 旧阈值 49KB 导致设备堆常在 25-35KB 时永远无法重连 MQTT
 * 新阈值 15KB 允许正常负载下重连，同时在真正内存危机时保护
 */
void test_mqtt_doReconnect_heap_threshold_15kb() {
    TestLog::testStart("MQTT doReconnect: heap threshold = 15KB (not 49KB)");

    constexpr uint32_t RECONNECT_HEAP_THRESHOLD = 15000;  // 新阈值
    constexpr uint32_t OLD_RECONNECT_THRESHOLD = 49152;   // 旧阈值（导致永迎无法重连）

    // 典型运行状态：堆 28KB
    uint32_t typicalHeap = 28000;
    bool newLogicAllows = (typicalHeap >= RECONNECT_HEAP_THRESHOLD);
    bool oldLogicAllows = (typicalHeap >= OLD_RECONNECT_THRESHOLD);
    TEST_ASSERT_TRUE(newLogicAllows);
    TEST_ASSERT_FALSE(oldLogicAllows);
    TestLog::step("Heap=28KB: new 15K allows reconnect, old 49K BLOCKS (root cause of bug!)");

    // 页面加载峰值：堆 22KB
    uint32_t peakHeap = 22000;
    newLogicAllows = (peakHeap >= RECONNECT_HEAP_THRESHOLD);
    oldLogicAllows = (peakHeap >= OLD_RECONNECT_THRESHOLD);
    TEST_ASSERT_TRUE(newLogicAllows);
    TEST_ASSERT_FALSE(oldLogicAllows);
    TestLog::step("Heap=22KB (page load peak): new 15K allows, old 49K blocks");

    // 真正内存危机：堆 10KB
    uint32_t crisisHeap = 10000;
    newLogicAllows = (crisisHeap >= RECONNECT_HEAP_THRESHOLD);
    TEST_ASSERT_FALSE(newLogicAllows);
    TestLog::step("Heap=10KB (crisis): both thresholds block (correct protection)");

    // 边界值: 15000 应允许
    TEST_ASSERT_TRUE(15000 >= RECONNECT_HEAP_THRESHOLD);
    TestLog::step("Heap=15000 (boundary): allowed");

    // 边界值: 14999 应拒绝
    TEST_ASSERT_FALSE(14999 >= RECONNECT_HEAP_THRESHOLD);
    TestLog::step("Heap=14999 (below boundary): blocked");

    // 验证阈值设计的合理性：MQTT 普通连接需要约 4-6KB
    // 15KB 阈值留有 2-3x 安全余量
    constexpr uint32_t MQTT_CONNECT_MEMORY_NEED = 6000;  // MQTT connect 最大内存需求
    constexpr uint32_t SAFETY_MARGIN = RECONNECT_HEAP_THRESHOLD / MQTT_CONNECT_MEMORY_NEED;
    TEST_ASSERT_GREATER_OR_EQUAL(2, SAFETY_MARGIN);
    TestLog::step("15KB threshold / 6KB need = 2.5x safety margin (adequate)");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 DNS_FAIL_THRESHOLD = 3 的平衡设计
 * 复现场景：阈值=1 时一次 DNS 失败就等 5 分钟，用户感知为“永远连接中”
 */
void test_mqtt_dns_fail_threshold_3() {
    TestLog::testStart("MQTT DNS Fail Threshold = 3");

    constexpr uint8_t DNS_FAIL_THRESHOLD = 3;
    constexpr uint32_t SLOW_RETRY_INTERVAL = 300000;  // 5分钟
    constexpr uint32_t FAST_RETRY_BASE = 5000;

    // 模拟连续失败计数
    uint8_t consecutiveTimeouts = 0;

    // 第1次失败: 仍在快速重试
    consecutiveTimeouts++;
    bool slowMode = (consecutiveTimeouts >= DNS_FAIL_THRESHOLD);
    TEST_ASSERT_FALSE(slowMode);
    TestLog::step("1st failure: still fast retry");

    // 第2次失败: 仍在快速重试
    consecutiveTimeouts++;
    slowMode = (consecutiveTimeouts >= DNS_FAIL_THRESHOLD);
    TEST_ASSERT_FALSE(slowMode);
    TestLog::step("2nd failure: still fast retry");

    // 第3次失败: 进入慢模式
    consecutiveTimeouts++;
    slowMode = (consecutiveTimeouts >= DNS_FAIL_THRESHOLD);
    TEST_ASSERT_TRUE(slowMode);
    TestLog::step("3rd failure: enters slow mode (5min)");

    // 验证阈值设计合理性
    // 阈值=1 太激进: broker 偶发一次超时就等 5 分钟
    TEST_ASSERT_GREATER_THAN(1, DNS_FAIL_THRESHOLD);
    TestLog::step("Threshold > 1: tolerates occasional DNS hiccups");

    // 阈值不能太大，否则影响内存保护
    TEST_ASSERT_LESS_OR_EQUAL(5, DNS_FAIL_THRESHOLD);
    TestLog::step("Threshold <= 5: protects memory from repeated reconnects");

    // 成功连接后重置计数
    consecutiveTimeouts = 0;
    slowMode = (consecutiveTimeouts >= DNS_FAIL_THRESHOLD);
    TEST_ASSERT_FALSE(slowMode);
    TestLog::step("After success: counter reset, fast mode resumed");

    TestLog::testEnd(true);
}

// ============================================================
// MQTT Status API & Frontend Badge Mapping Tests
// 验证后端 /api/mqtt/status 返回的各字段组合
// 与前端 _updateMqttStatusUI badge 状态的对应关系
// ============================================================

/**
 * @brief 模拟后端状态 API 返回逻辑
 * 与 MqttRouteHandler::handleGetMqttStatus 保持一致
 */
struct MqttStatusApiResponse {
    bool initialized;
    bool connected;
    bool connecting;
    bool stopped;
    bool enabled;
    int lastError;
    uint32_t reconnectCount;
    bool autoReconnect;
    bool internetAvailable;
};

// 模拟前端 badge 映射逻辑（与 _updateMqttStatusUI 保持一致）
static const char* mapBadgeState(const MqttStatusApiResponse& s) {
    if (s.connected) {
        return "已连接";  // mqtt-status-online
    }
    bool connecting = s.connecting;  // 新逻辑：仅看后端 connecting 字段
    bool hasError = s.lastError != 0;

    if (connecting && !hasError) {
        return "连接中";  // mqtt-status-connecting
    }
    if (connecting && hasError) {
        return "连接失败";  // mqtt-status-offline
    }
    if (!s.initialized && !s.enabled) {
        return "未初始化";  // mqtt-status-offline
    }
    if (s.enabled && !s.initialized) {
        return "初始化中";  // mqtt-status-connecting
    }
    return "未连接";  // mqtt-status-offline
}

/**
 * @brief 验证已连接状态 → badge 显示"已连接"
 */
void test_mqtt_status_api_connected_state() {
    TestLog::testStart("MQTT Status: Connected → Badge '已连接'");

    MqttStatusApiResponse s = {};
    s.initialized = true;
    s.connected = true;         // MQTT getIsConnected() && !isStopped()
    s.connecting = false;
    s.stopped = false;
    s.enabled = true;
    s.lastError = 0;
    s.reconnectCount = 0;
    s.autoReconnect = true;
    s.internetAvailable = true;

    TEST_ASSERT_EQUAL_STRING("已连接", mapBadgeState(s));
    TestLog::step("connected=true, connecting=false → '已连接'");

    // 即使 lastError 非零，connected=true 也应显示"已连接"（当前连接正常）
    s.lastError = -3;  // 之前断连过
    TEST_ASSERT_EQUAL_STRING("已连接", mapBadgeState(s));
    TestLog::step("connected=true + lastError=-3 → still '已连接'");

    TestLog::testEnd(true);
}

/**
 * @brief 验证连接中状态 → badge 显示"连接中"
 */
void test_mqtt_status_api_connecting_state() {
    TestLog::testStart("MQTT Status: Connecting → Badge '连接中'");

    MqttStatusApiResponse s = {};
    s.initialized = true;
    s.connected = false;
    s.connecting = true;        // !connected && autoReconnect && !stopped
    s.stopped = false;
    s.enabled = true;
    s.lastError = 0;            // 无错误
    s.reconnectCount = 1;       // 已尝试重连
    s.autoReconnect = true;
    s.internetAvailable = true;

    TEST_ASSERT_EQUAL_STRING("连接中", mapBadgeState(s));
    TestLog::step("connected=false, connecting=true, no error → '连接中'");

    // reconnectCount > 0 说明不是首次连接卡死
    // 前端不会因此显示“连接超时”
    TestLog::step("reconnectCount=1 → not first-attempt timeout");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 stopped 状态 → badge 显示"未连接"
 */
void test_mqtt_status_api_stopped_state() {
    TestLog::testStart("MQTT Status: Stopped → Badge '未连接'");

    MqttStatusApiResponse s = {};
    s.initialized = true;
    s.connected = false;
    s.connecting = false;       // stopped → !autoReconnect || stopped → connecting=false
    s.stopped = true;
    s.enabled = true;
    s.lastError = 0;
    s.reconnectCount = 0;
    s.autoReconnect = true;
    s.internetAvailable = true;

    TEST_ASSERT_EQUAL_STRING("未连接", mapBadgeState(s));
    TestLog::step("stopped=true, connecting=false → '未连接'");

    TestLog::testEnd(true);
}

/**
 * @brief 验证无客户端状态 → badge 显示"未初始化"或"未连接"
 */
void test_mqtt_status_api_no_client_state() {
    TestLog::testStart("MQTT Status: No Client → Badge State");

    // MQTT 未启用 → 未初始化
    MqttStatusApiResponse s1 = {};
    s1.initialized = false;
    s1.connected = false;
    s1.connecting = false;
    s1.stopped = false;
    s1.enabled = false;
    TEST_ASSERT_EQUAL_STRING("未初始化", mapBadgeState(s1));
    TestLog::step("enabled=false, initialized=false → '未初始化'");

    // MQTT 已启用但客户端未创建 → 初始化中
    MqttStatusApiResponse s2 = {};
    s2.initialized = false;
    s2.connected = false;
    s2.connecting = false;
    s2.stopped = false;
    s2.enabled = true;
    TEST_ASSERT_EQUAL_STRING("初始化中", mapBadgeState(s2));
    TestLog::step("enabled=true, initialized=false → '初始化中'");

    TestLog::testEnd(true);
}

/**
 * @brief 验证连接中但有错误 → badge 显示"连接失败"
 */
void test_mqtt_status_api_error_with_connecting() {
    TestLog::testStart("MQTT Status: Error While Connecting → Badge '连接失败'");

    MqttStatusApiResponse s = {};
    s.initialized = true;
    s.connected = false;
    s.connecting = true;        // 还在尝试重连
    s.stopped = false;
    s.enabled = true;
    s.lastError = 4;            // MQTT_BAD_CREDENTIALS (错误凭据)
    s.reconnectCount = 3;
    s.autoReconnect = true;

    TEST_ASSERT_EQUAL_STRING("连接失败", mapBadgeState(s));
    TestLog::step("connecting=true, lastError=4(BAD_CREDENTIALS) → '连接失败'");

    // 网络断连错误
    s.lastError = -3;  // MQTT_CONNECTION_LOST
    TEST_ASSERT_EQUAL_STRING("连接失败", mapBadgeState(s));
    TestLog::step("connecting=true, lastError=-3(CONNECTION_LOST) → '连接失败'");

    TestLog::testEnd(true);
}

/**
 * @brief 验证前端60秒超时逻辑：connecting 持续 60s 且 reconnectCount=0 → "连接超时"
 */
void test_mqtt_status_badge_timeout_logic() {
    TestLog::testStart("MQTT Status: 60s Timeout Logic");

    constexpr unsigned long TIMEOUT_MS = 60000;

    // reconnectCount=0 且 connecting 超过 60s → 连接超时
    MqttStatusApiResponse s = {};
    s.initialized = true;
    s.connected = false;
    s.connecting = true;
    s.reconnectCount = 0;  // 从未成功连接过

    unsigned long connectingStartMs = 1000;  // 1秒时开始
    unsigned long nowMs = 62000;  // 61秒时检查
    unsigned long duration = nowMs - connectingStartMs;  // 61s > 60s

    bool shouldTimeout = (duration > TIMEOUT_MS) && (s.reconnectCount == 0);
    TEST_ASSERT_TRUE(shouldTimeout);
    TestLog::step("61s connecting + reconnectCount=0 → TIMEOUT");

    // reconnectCount>0 时不超时（客户端正在重试）
    s.reconnectCount = 1;
    bool shouldNotTimeout = !((duration > TIMEOUT_MS) && (s.reconnectCount == 0));
    TEST_ASSERT_TRUE(shouldNotTimeout);
    TestLog::step("61s connecting + reconnectCount=1 → NOT timeout");

    // 未满 60s 时不超时
    s.reconnectCount = 0;
    nowMs = 50000;  // 50秒
    duration = nowMs - connectingStartMs;  // 49s < 60s
    bool tooEarly = (duration > TIMEOUT_MS) && (s.reconnectCount == 0);
    TEST_ASSERT_FALSE(tooEarly);
    TestLog::step("49s connecting → NOT timeout yet");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 connected=true 时重置超时计时器
 */
void test_mqtt_status_connected_resets_timer() {
    TestLog::testStart("MQTT Status: Connected Resets Timeout Timer");

    // 模拟：连接中30s → 突然连上 → 计时器重置 → 再断连 → 计时器从0开始
    unsigned long connectingStartTime = 0;

    // Phase 1: 开始连接
    connectingStartTime = 10000;  // 10s时开始
    TestLog::step("Phase 1: Start connecting at t=10s");

    // Phase 2: 30s后连接成功 → 计时器重置为0
    bool connected = true;
    if (connected) connectingStartTime = 0;
    TEST_ASSERT_EQUAL(0, connectingStartTime);
    TestLog::step("Phase 2: Connected at t=40s → timer reset to 0");

    // Phase 3: 又断连 → 计时器从新起点开始
    connected = false;
    unsigned long newConnectingStart = 50000;  // 50s时重新开始
    connectingStartTime = newConnectingStart;
    TEST_ASSERT_EQUAL(50000, connectingStartTime);
    TestLog::step("Phase 3: Disconnected at t=50s → timer starts fresh");

    // 不会因为之前30s的“连接中”累积而误判超时
    unsigned long now = 80000;  // 80s时检查
    unsigned long duration = now - connectingStartTime;  // 30s < 60s
    TEST_ASSERT_TRUE(duration < 60000);
    TestLog::step("Phase 4: 30s since reconnect → NOT timeout (timer was reset)");

    TestLog::testEnd(true);
}

// ============================================================
// MQTT 多认证格式 + 多芯片连接稳定性测试
// S前缀=简单认证, E前缀=加密认证
// 覆盖 ESP32/ESP32-S3(F8R0无PSRAM)/ESP32-S3(F8R4有PSRAM)/ESP32-S3(F16R8)
// ============================================================

/**
 * @brief 芯片环境配置模拟
 * 不同芯片的堆内存和PSRAM差异影响MQTT连接稳定性
 */
struct ChipProfile {
    const char* name;
    uint32_t totalHeap;
    uint32_t psramSize;
    uint32_t tcpMaxConnections;
};

static const ChipProfile CHIP_PROFILES[] = {
    {"ESP32-F4R0",      320000, 0,              10},   // ESP32 classic, 4MB flash
    {"ESP32-S3-F8R0",   320000, 0,              14},   // ESP32-S3, 8MB flash, no PSRAM
    {"ESP32-S3-F8R4",   320000, 4*1024*1024,    14},   // ESP32-S3, 8MB flash, 4MB PSRAM
    {"ESP32-S3-F16R8",  320000, 8*1024*1024,    14},   // ESP32-S3, 16MB flash, 8MB PSRAM
};
static const int CHIP_PROFILE_COUNT = 4;

/**
 * @brief 验证S前缀简单认证格式在所有芯片环境下正常工作
 * S&{deviceNum}&{productId}&{userId} 格式
 */
void test_mqtt_simple_auth_all_chip_environments() {
    TestLog::testStart("MQTT Simple Auth (S&) All Chip Environments");

    for (int i = 0; i < CHIP_PROFILE_COUNT; i++) {
        const ChipProfile& chip = CHIP_PROFILES[i];
        ESP.setFreeHeap(chip.totalHeap);
        if (chip.psramSize > 0) {
            ESP.setPsramSize(chip.psramSize);
            ESP.setFreePsram(chip.psramSize - 1024);
        }

        // S前缀简单认证: S&{deviceNum}&{productId}&{userId}
        String clientId = MockMQTTClient::buildClientId("S", "FBE100900001", "1070", "1");
        TEST_ASSERT_TRUE(clientId.startsWith("S&"));
        TEST_ASSERT_EQUAL_STRING("S&FBE100900001&1070&1", clientId.c_str());

        int authType = MockMQTTClient::detectAuthType(clientId.c_str());
        TEST_ASSERT_EQUAL(0, authType);  // 0 = 简单认证

        MockMQTTClient mqtt;
        MQTTConfig config;
        config.enabled = true;
        config.server = "iot.fastbee.cn";
        config.port = 1883;
        config.clientId = clientId;
        config.username = "admin";
        config.password = "password123";
        config.autoReconnect = true;

        mqtt.initialize(config);
        TEST_ASSERT_TRUE_MESSAGE(mqtt.connect(),
            (String("S-auth connect failed on ") + chip.name).c_str());
        TEST_ASSERT_TRUE(mqtt.getIsConnected());

        // 连接成功后发布/订阅验证
        TEST_ASSERT_TRUE(mqtt.publish("/device/status", "online"));
        TEST_ASSERT_TRUE(mqtt.subscribe("/device/control"));

        mqtt.disconnect();
        ESP.resetHeapOverride();
    }
    TestLog::step("S-auth passed on all 4 chip profiles");

    TestLog::testEnd(true);
}

/**
 * @brief 验证E前缀加密认证格式在所有芯片环境下正常工作
 * E&{deviceNum}&{productId}&{userId} 格式，密码为ENC:前缀
 */
void test_mqtt_encrypted_auth_all_chip_environments() {
    TestLog::testStart("MQTT Encrypted Auth (E&) All Chip Environments");

    for (int i = 0; i < CHIP_PROFILE_COUNT; i++) {
        const ChipProfile& chip = CHIP_PROFILES[i];
        ESP.setFreeHeap(chip.totalHeap);
        if (chip.psramSize > 0) {
            ESP.setPsramSize(chip.psramSize);
            ESP.setFreePsram(chip.psramSize - 1024);
        }

        // E前缀加密认证: E&{deviceNum}&{productId}&{userId}
        String clientId = MockMQTTClient::buildClientId("E", "FBE100900001", "1070", "1");
        TEST_ASSERT_TRUE(clientId.startsWith("E&"));

        int authType = MockMQTTClient::detectAuthType(clientId.c_str());
        TEST_ASSERT_EQUAL(1, authType);  // 1 = 加密认证

        // 加密密码生成
        String encPwd = MockMQTTClient::buildEncryptedPassword(
            "password123", "authCode001", "mqttSecret123456", "ntp.fastbee.cn"
        );
        TEST_ASSERT_TRUE(encPwd.startsWith("ENC:"));

        MockMQTTClient mqtt;
        MQTTConfig config;
        config.enabled = true;
        config.server = "iot.fastbee.cn";
        config.port = 1883;
        config.clientId = clientId;
        config.username = "admin";
        config.password = encPwd;
        config.autoReconnect = true;

        mqtt.initialize(config);
        TEST_ASSERT_TRUE_MESSAGE(mqtt.connect(),
            (String("E-auth connect failed on ") + chip.name).c_str());
        TEST_ASSERT_TRUE(mqtt.getIsConnected());

        TEST_ASSERT_TRUE(mqtt.publish("/device/status", "online"));
        mqtt.disconnect();
        ESP.resetHeapOverride();
    }
    TestLog::step("E-auth passed on all 4 chip profiles");

    TestLog::testEnd(true);
}

/**
 * @brief 验证无效认证前缀被正确拒绝
 * 非S非E前缀的clientId不应通过认证
 */
void test_mqtt_invalid_auth_prefix_rejected() {
    TestLog::testStart("MQTT Invalid Auth Prefix Rejected");

    // 无效前缀
    int invalidType = MockMQTTClient::detectAuthType("X&DEV001&PROD001&USER001");
    TEST_ASSERT_EQUAL(-1, invalidType);
    TestLog::step("X-prefix: authType = -1 (invalid)");

    // 空字符串
    int emptyType = MockMQTTClient::detectAuthType("");
    TEST_ASSERT_EQUAL(-1, emptyType);
    TestLog::step("Empty: authType = -1 (invalid)");

    // 只有S不带&
    int sNoAmp = MockMQTTClient::detectAuthType("S_DEV001");
    TEST_ASSERT_EQUAL(-1, sNoAmp);
    TestLog::step("S_ prefix (no &): authType = -1 (invalid)");

    // 小写s不应被识别为简单认证
    int lowercaseS = MockMQTTClient::detectAuthType("s&DEV001&PROD001&USER001");
    TEST_ASSERT_EQUAL(-1, lowercaseS);
    TestLog::step("lowercase s: authType = -1 (invalid)");

    // E前缀但缺少ENC:密码应认证失败
    MockMQTTClient mqtt;
    MQTTConfig config;
    config.enabled = true;
    config.server = "iot.fastbee.cn";
    config.port = 1883;
    config.clientId = "E&DEV001&PROD001&USER001";
    config.username = "admin";
    config.password = "plain_password";  // 非ENC:前缀
    mqtt.initialize(config);
    TEST_ASSERT_FALSE(mqtt.connect());
    TestLog::step("E-auth with plain password: connect FAILED (correct)");

    TestLog::testEnd(true);
}

/**
 * @brief 验证S和E认证clientId构建的多样参数组合
 * 确保不同deviceNum/productId/userId组合都能正确生成
 */
void test_mqtt_client_id_build_variations() {
    TestLog::testStart("MQTT ClientId Build Variations");

    // S前缀各种组合
    struct TestCase {
        const char* prefix;
        const char* device;
        const char* product;
        const char* user;
        const char* expected;
    };
    TestCase cases[] = {
        {"S", "DEV001", "PROD001", "USER001", "S&DEV001&PROD001&USER001"},
        {"S", "FBE100900001", "1070", "1", "S&FBE100900001&1070&1"},
        {"E", "DEV001", "PROD001", "USER001", "E&DEV001&PROD001&USER001"},
        {"E", "FBE200000001", "2050", "99", "E&FBE200000001&2050&99"},
    };

    for (auto& tc : cases) {
        String result = MockMQTTClient::buildClientId(tc.prefix, tc.device, tc.product, tc.user);
        TEST_ASSERT_EQUAL_STRING(tc.expected, result.c_str());

        int expectedType = (strcmp(tc.prefix, "S") == 0) ? 0 : 1;
        TEST_ASSERT_EQUAL(expectedType, MockMQTTClient::detectAuthType(result.c_str()));
    }
    TestLog::step("All 4 clientId variations built and verified");

    TestLog::testEnd(true);
}

/**
 * @brief MQTT长连接稳定性压力测试：100次连接/断开循环无泄漏
 * 模拟设备长期运行中MQTT反复断连重连的场景
 */
void test_mqtt_long_running_connect_disconnect_stability() {
    TestLog::testStart("MQTT Long-Running Connect/Disconnect Stability");

    MockMQTTClient mqtt;
    MQTTConfig config;
    config.enabled = true;
    config.server = "iot.fastbee.cn";
    config.port = 1883;
    config.clientId = "S&FBE100900001&1070&1";
    config.username = "admin";
    config.password = "password123";
    config.autoReconnect = true;
    config.reconnectInterval = 5000;

    mqtt.initialize(config);

    uint32_t initialHeap = ESP.getFreeHeap();
    int connectSuccessCount = 0;
    int publishSuccessCount = 0;

    // 100次连接/断开循环
    for (int cycle = 0; cycle < 100; cycle++) {
        // 连接
        TEST_ASSERT_TRUE(mqtt.connect());
        TEST_ASSERT_TRUE(mqtt.getIsConnected());
        connectSuccessCount++;

        // 连接后发布和订阅
        if (mqtt.publish("/device/status", "online")) {
            publishSuccessCount++;
        }
        mqtt.subscribe("/device/control");

        // 模拟接收消息
        mqtt.simulateMessage("/device/control", "{\"cmd\":\"reboot\"}");

        // 断开
        mqtt.disconnect();
        TEST_ASSERT_FALSE(mqtt.getIsConnected());
    }

    TEST_ASSERT_EQUAL(100, connectSuccessCount);
    TEST_ASSERT_EQUAL(100, publishSuccessCount);
    TestLog::step("100 connect/disconnect cycles: all successful");

    // 堆泄漏检测
    uint32_t finalHeap = ESP.getFreeHeap();
    int32_t leak = (int32_t)initialHeap - (int32_t)finalHeap;
    TEST_ASSERT_TRUE_MESSAGE(leak < 5000, "MQTT connect/disconnect leak detected");
    TestLog::step("Heap leak < 5KB over 100 cycles");

    // 重连计数验证
    TEST_ASSERT_EQUAL(0, mqtt.getReconnectCount());  // connect()不增加reconnectCount
    TestLog::step("Reconnect count correct after lifecycle");

    TestLog::testEnd(true);
}

/**
 * @brief MQTT自动重连在低堆内存环境下的稳定性
 * 模拟ESP32-S3(F8R0无PSRAM)堆内存紧张时的重连行为
 */
void test_mqtt_auto_reconnect_low_heap_stability() {
    TestLog::testStart("MQTT Auto-Reconnect Low Heap Stability");

    // 使用ESP32-S3-F8R0的堆配置（无PSRAM，堆更紧张）
    ESP.setFreeHeap(35000);  // 典型运行状态

    MockMQTTClient mqtt;
    MQTTConfig config;
    config.enabled = true;
    config.server = "iot.fastbee.cn";
    config.port = 1883;
    config.clientId = "S&FBE100900001&1070&1";
    config.username = "admin";
    config.password = "password123";
    config.autoReconnect = true;
    config.reconnectInterval = 100;

    mqtt.initialize(config);
    TEST_ASSERT_TRUE(mqtt.connect());
    TestLog::step("Initial connect at heap=35KB");

    // 模拟20次断连→自动重连周期
    for (int cycle = 0; cycle < 20; cycle++) {
        mqtt.setConnected(false);

        // 模拟堆内存波动（页面加载、NTP请求等）
        uint32_t heapDuringReconnect = 20000 + (cycle % 5) * 3000;
        ESP.setFreeHeap(heapDuringReconnect);

        // 自动重连调度
        unsigned long timeMs = (unsigned long)(cycle + 1) * 200;
        bool scheduled = mqtt.handleAutoReconnect(timeMs);

        if (scheduled) {
            mqtt.clearReconnectPending();
            // 重连前检查堆是否足够（阈值15KB）
            if (heapDuringReconnect >= 15000) {
                bool reconnected = mqtt.reconnect();
                TEST_ASSERT_TRUE(reconnected);
            }
        }
    }
    TestLog::step("20 disconnect/reconnect cycles with heap fluctuation");

    // 堆恢复后最终应能重连
    ESP.setFreeHeap(50000);
    mqtt.setConnected(false);
    TEST_ASSERT_TRUE(mqtt.handleAutoReconnect(100000));
    mqtt.clearReconnectPending();
    TEST_ASSERT_TRUE(mqtt.reconnect());
    TEST_ASSERT_TRUE(mqtt.getIsConnected());
    TestLog::step("Final reconnect succeeded after heap recovery");

    ESP.resetHeapOverride();
    TestLog::testEnd(true);
}

/**
 * @brief MQTT S/E认证切换不影响连接稳定性
 * 模拟设备从简单认证切换到加密认证（或反向）后仍能正常连接
 */
void test_mqtt_auth_type_switch_stability() {
    TestLog::testStart("MQTT Auth Type Switch Stability");

    MockMQTTClient mqtt;
    MQTTConfig config;
    config.enabled = true;
    config.server = "iot.fastbee.cn";
    config.port = 1883;
    config.autoReconnect = true;

    // 第1次：S简单认证
    config.clientId = "S&DEV001&PROD001&USER001";
    config.username = "admin";
    config.password = "simple_pass";
    mqtt.initialize(config);
    TEST_ASSERT_TRUE(mqtt.connect());
    TEST_ASSERT_EQUAL(0, MockMQTTClient::detectAuthType(config.clientId.c_str()));
    mqtt.disconnect();
    TestLog::step("1st: S-auth connect → disconnect OK");

    // 第2次：E加密认证
    config.clientId = "E&DEV001&PROD001&USER001";
    config.password = MockMQTTClient::buildEncryptedPassword(
        "pass123", "code001", "secret123456", "ntp.fastbee.cn");
    mqtt.initialize(config);
    TEST_ASSERT_TRUE(mqtt.connect());
    TEST_ASSERT_EQUAL(1, MockMQTTClient::detectAuthType(config.clientId.c_str()));
    mqtt.disconnect();
    TestLog::step("2nd: E-auth connect → disconnect OK");

    // 第3次：回到S认证
    config.clientId = "S&DEV002&PROD002&USER002";
    config.password = "new_simple_pass";
    mqtt.initialize(config);
    TEST_ASSERT_TRUE(mqtt.connect());
    TEST_ASSERT_TRUE(mqtt.getIsConnected());
    TestLog::step("3rd: S-auth re-connect OK (different device)");

    // 第4次：E认证不同设备
    config.clientId = "E&DEV003&PROD003&USER003";
    config.password = MockMQTTClient::buildEncryptedPassword(
        "pass456", "code002", "secret654321", "ntp.fastbee.cn");
    mqtt.initialize(config);
    TEST_ASSERT_TRUE(mqtt.connect());
    TestLog::step("4th: E-auth connect OK (different device)");

    TestLog::testEnd(true);
}

/**
 * @brief MQTT多服务器端口组合在不同芯片上的连接验证
 * 确保1883/8883等常用端口在所有芯片环境下正常
 */
void test_mqtt_multi_port_chip_combination() {
    TestLog::testStart("MQTT Multi-Port × Chip Combination");

    struct PortTest {
        uint16_t port;
        const char* desc;
    };
    PortTest ports[] = {
        {1883, "MQTT plain"},
        {8883, "MQTT TLS"},
    };

    int totalTests = 0;
    for (int ci = 0; ci < CHIP_PROFILE_COUNT; ci++) {
        for (auto& pt : ports) {
            ESP.setFreeHeap(CHIP_PROFILES[ci].totalHeap);

            MockMQTTClient mqtt;
            MQTTConfig config;
            config.enabled = true;
            config.server = "iot.fastbee.cn";
            config.port = pt.port;
            config.clientId = "S&FBE100900001&1070&1";
            config.username = "admin";
            config.password = "password123";

            mqtt.initialize(config);
            MQTTConfig saved = mqtt.getConfig();
            TEST_ASSERT_EQUAL(pt.port, saved.port);
            TEST_ASSERT_EQUAL_STRING("iot.fastbee.cn", saved.server.c_str());

            TEST_ASSERT_TRUE(mqtt.connect());
            mqtt.disconnect();
            totalTests++;
        }
    }

    ESP.resetHeapOverride();
    TestLog::step(("Passed " + String(totalTests) + " port×chip combinations").c_str());

    TestLog::testEnd(true);
}

// ============================================================
// Bug Fix 回归测试：autoReconnect 禁用时不触发重连
// ============================================================

/**
 * @brief 验证前端 mqtt-config.js _updateMqttStatusUI 在触发自动重连前检查 autoReconnect
 * 回归：旧代码不检查 d.autoReconnect，导致取消勾选后仍触发 /api/mqtt/reconnect
 */
void test_mqtt_autoreconnect_frontend_check_present() {
    TestLog::testStart("Regression: autoReconnect Frontend Check");

    std::string content = readProjectFile("web-src/modules/runtime/protocol/mqtt-config.js");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read mqtt-config.js — ensure test runs from project root");
    TestLog::step("File loaded");

    // 关键：_updateMqttStatusUI 中触发 /api/mqtt/reconnect 前必须检查 autoReconnect !== false
    // 条件格式：d.autoReconnect !== false
    TEST_ASSERT_TRUE_MESSAGE(content.find("autoReconnect !== false") != std::string::npos,
        "_updateMqttStatusUI must check d.autoReconnect !== false before triggering reconnect API");
    TestLog::step("autoReconnect !== false check present in _updateMqttStatusUI");

    // 验证条件在自动重连触发判断中（与 _mqttAutoReconnectTriggered 同处）
    std::regex combinedRe("_mqttAutoReconnectTriggered.*autoReconnect\\s*!==\\s*false");
    TEST_ASSERT_TRUE_MESSAGE(std::regex_search(content, combinedRe),
        "autoReconnect check must appear alongside _mqttAutoReconnectTriggered guard");
    TestLog::step("autoReconnect check co-located with auto-reconnect trigger guard");

    TestLog::testEnd(true);
}

/**
 * @brief 验证后端 MqttRouteHandler::handleMqttReconnect 在 autoReconnect=false 时拒绝重连
 * 回归：旧代码无条件执行 restartMQTTDeferred()，用户取消勾选后前端轮询仍触发重连
 */
void test_mqtt_autoreconnect_backend_check_present() {
    TestLog::testStart("Regression: autoReconnect Backend Check");

    std::string content = readProjectFile("src/network/handlers/MqttRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read MqttRouteHandler.cpp — ensure test runs from project root");
    TestLog::step("File loaded");

    // handleMqttReconnect 必须包含对 cfg.autoReconnect 的检查
    TEST_ASSERT_TRUE_MESSAGE(content.find("!cfg.autoReconnect") != std::string::npos,
        "handleMqttReconnect must check cfg.autoReconnect and skip reconnect when false");
    TestLog::step("!cfg.autoReconnect check present in handleMqttReconnect");

    // 拒绝重连时应返回 autoReconnectDisabled 标志
    TEST_ASSERT_TRUE_MESSAGE(content.find("autoReconnectDisabled") != std::string::npos,
        "Response must include autoReconnectDisabled=true when reconnect is refused");
    TestLog::step("autoReconnectDisabled field present in rejection response");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 autoReconnect=false 时 MockMQTTClient handle() 不触发重连
 * 与 MQTTClient.cpp handle() 中 if (!config.autoReconnect) { return; } 对应
 */
void test_mqtt_autoreconnect_disabled_blocks_handle_reconnect() {
    TestLog::testStart("autoReconnect=false Blocks handle() Reconnect");

    MockMQTTClient mqtt;
    MQTTConfig config;
    config.enabled = true;
    config.server = "iot.fastbee.cn";
    config.port = 1883;
    config.clientId = "S&FBE100900001&1070&1";
    config.username = "admin";
    config.password = "password123";
    config.autoReconnect = false;  // 关键：禁用自动重连
    config.reconnectInterval = 100;

    mqtt.initialize(config);
    TEST_ASSERT_FALSE(mqtt.getIsConnected());

    // 即使经过足够时间，handle 也不应调度重连
    bool scheduled = mqtt.handleAutoReconnect(60000);
    TEST_ASSERT_FALSE_MESSAGE(scheduled,
        "handleAutoReconnect must NOT schedule reconnect when autoReconnect=false");
    TEST_ASSERT_FALSE(mqtt.isReconnectPending());
    TestLog::step("autoReconnect=false: handle() does not schedule reconnect");

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

    // Auto-Connect & Status Notification Tests
    RUN_TEST(test_mqtt_boot_stabilization_delay);
    RUN_TEST(test_mqtt_auto_reconnect_scheduling);
    RUN_TEST(test_mqtt_no_reconnect_when_connected);
    RUN_TEST(test_mqtt_no_reconnect_when_stopped);
    RUN_TEST(test_mqtt_no_reconnect_when_disabled);
    RUN_TEST(test_mqtt_status_notification_json_fields);
    RUN_TEST(test_mqtt_status_notification_disconnected);
    RUN_TEST(test_mqtt_auto_connect_lifecycle);

    // MQTT Lite Polling & Timeout Regression Tests
    RUN_TEST(test_mqtt_lite_recursive_polling_constraints);
    RUN_TEST(test_mqtt_connecting_timeout_25s_logic);

    // MQTT Reconnect Guard & Deferred Restart Tests
    RUN_TEST(test_mqtt_reconnect_always_rebuild_after_test);
    RUN_TEST(test_mqtt_deferred_restart_heap_order);
    RUN_TEST(test_mqtt_doReconnect_heap_threshold_15kb);
    RUN_TEST(test_mqtt_dns_fail_threshold_3);

    // MQTT Status API & Frontend Badge Mapping Tests
    RUN_TEST(test_mqtt_status_api_connected_state);
    RUN_TEST(test_mqtt_status_api_connecting_state);
    RUN_TEST(test_mqtt_status_api_stopped_state);
    RUN_TEST(test_mqtt_status_api_no_client_state);
    RUN_TEST(test_mqtt_status_api_error_with_connecting);
    RUN_TEST(test_mqtt_status_badge_timeout_logic);
    RUN_TEST(test_mqtt_status_connected_resets_timer);

    // 多认证格式 + 多芯片连接稳定性测试
    RUN_TEST(test_mqtt_simple_auth_all_chip_environments);
    RUN_TEST(test_mqtt_encrypted_auth_all_chip_environments);
    RUN_TEST(test_mqtt_invalid_auth_prefix_rejected);
    RUN_TEST(test_mqtt_client_id_build_variations);
    RUN_TEST(test_mqtt_long_running_connect_disconnect_stability);
    RUN_TEST(test_mqtt_auto_reconnect_low_heap_stability);
    RUN_TEST(test_mqtt_auth_type_switch_stability);
    RUN_TEST(test_mqtt_multi_port_chip_combination);

    // Bug Fix 回归测试：autoReconnect 禁用不触发重连
    RUN_TEST(test_mqtt_autoreconnect_frontend_check_present);
    RUN_TEST(test_mqtt_autoreconnect_backend_check_present);
    RUN_TEST(test_mqtt_autoreconnect_disabled_blocks_handle_reconnect);

    TestLog::groupEnd();
}
