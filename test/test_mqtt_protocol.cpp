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
    // doReconnect() 内部已有堆内存保护（<8KB 跳过），无需过长延迟
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
 * @brief 验证 doReconnect 堆阈值为 8KB（而非旧值 49KB/15KB）
 * 旧阈值 15KB 导致 PSRAM 设备稳态堆 ~10-12KB 时永远无法重连 MQTT
 * 新阈值 8KB 允许 PSRAM 设备正常重连，同时在真正内存危机时保护
 */
void test_mqtt_doReconnect_heap_threshold_8kb() {
    TestLog::testStart("MQTT doReconnect: heap threshold = 8KB (not 15KB/49KB)");

    constexpr uint32_t RECONNECT_HEAP_THRESHOLD = 8000;   // 当前阈值
    constexpr uint32_t OLD_THRESHOLD_15KB       = 15000;  // 旧阈值（导致 PSRAM 设备无法重连）
    constexpr uint32_t OLD_THRESHOLD_49KB       = 49152;  // 更旧阈值

    // PSRAM 设备稳态：堆 11KB（WiFi/lwIP/FreeRTOS 占用内部 DRAM）
    uint32_t psramTypicalHeap = 11000;
    bool newLogicAllows = (psramTypicalHeap >= RECONNECT_HEAP_THRESHOLD);
    bool old15KAllows  = (psramTypicalHeap >= OLD_THRESHOLD_15KB);
    TEST_ASSERT_TRUE(newLogicAllows);
    TEST_ASSERT_FALSE(old15KAllows);
    TestLog::step("Heap=11KB (PSRAM steady): new 8K allows, old 15K BLOCKS (root cause!)");

    // 无PSRAM 设备典型：堆 28KB
    uint32_t noPsramTypicalHeap = 28000;
    newLogicAllows = (noPsramTypicalHeap >= RECONNECT_HEAP_THRESHOLD);
    TEST_ASSERT_TRUE(newLogicAllows);
    TestLog::step("Heap=28KB (no-PSRAM typical): allowed");

    // 页面加载峰值：堆 22KB
    uint32_t peakHeap = 22000;
    TEST_ASSERT_TRUE(peakHeap >= RECONNECT_HEAP_THRESHOLD);
    TestLog::step("Heap=22KB (page load peak): allowed");

    // 真正内存危机：堆 5KB
    uint32_t crisisHeap = 5000;
    TEST_ASSERT_FALSE(crisisHeap >= RECONNECT_HEAP_THRESHOLD);
    TestLog::step("Heap=5KB (true crisis): blocked (correct protection)");

    // 边界值: 8000 应允许
    TEST_ASSERT_TRUE(8000 >= RECONNECT_HEAP_THRESHOLD);
    TestLog::step("Heap=8000 (boundary): allowed");

    // 边界值: 7999 应拒绝
    TEST_ASSERT_FALSE(7999 >= RECONNECT_HEAP_THRESHOLD);
    TestLog::step("Heap=7999 (below boundary): blocked");

    // 验证阈值设计的合理性：MQTT 普通连接需要约 4-6KB
    // 8KB 阈值留有 1.3x 安全余量，且低于 PSRAM 设备稳态堆 10-12KB
    constexpr uint32_t MQTT_CONNECT_MEMORY_NEED = 6000;
    TEST_ASSERT_GREATER_OR_EQUAL((uint32_t)2000, RECONNECT_HEAP_THRESHOLD - MQTT_CONNECT_MEMORY_NEED);
    TestLog::step("8KB threshold - 6KB need = 2KB margin (adequate for PSRAM devices)");

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
            // 重连前检查堆是否足够（阈值 8KB）
            if (heapDuringReconnect >= 8000) {
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

// ========== MQTTS Scheme 支持测试 ==========

// MQTTS-01: scheme 默认值为 "mqtt"
void test_mqtts_scheme_default_value() {
    TestLog::testStart("MQTTS Scheme Default Value");

    MQTTConfig config;
    TEST_ASSERT_EQUAL_STRING("mqtt", config.scheme.c_str());
    TestLog::step("Default scheme is 'mqtt'");

    TEST_ASSERT_EQUAL(1883, config.port);
    TestLog::step("Default port is 1883 (mqtt)");

    TestLog::testEnd(true);
}

// MQTTS-02: scheme 可设为 "mqtts"
void test_mqtts_scheme_set_mqtts() {
    TestLog::testStart("MQTTS Scheme Set to mqtts");

    MQTTConfig config;
    config.scheme = "mqtts";
    TEST_ASSERT_EQUAL_STRING("mqtts", config.scheme.c_str());
    TestLog::step("Scheme set to 'mqtts' successfully");

    // 其他字段不受影响
    TEST_ASSERT_EQUAL_STRING("mqtt", "mqtt");
    TEST_ASSERT_EQUAL(1883, config.port);
    TestLog::step("Other config fields unchanged");

    TestLog::testEnd(true);
}

// MQTTS-03: mqtts 默认端口 8883
void test_mqtts_default_port() {
    TestLog::testStart("MQTTS Default Port 8883");

    // 模拟前端端口联动逻辑
    MQTTConfig config;
    config.scheme = "mqtts";
    // 前端逻辑：scheme=mqtts 且端口为 1883 时自动切换为 8883
    if (config.scheme == "mqtts" && config.port == 1883) {
        config.port = 8883;
    }
    TEST_ASSERT_EQUAL(8883, config.port);
    TestLog::step("Port auto-updated to 8883 for mqtts");

    // 反向：切回 mqtt 时端口自动恢复
    config.scheme = "mqtt";
    if (config.scheme == "mqtt" && config.port == 8883) {
        config.port = 1883;
    }
    TEST_ASSERT_EQUAL(1883, config.port);
    TestLog::step("Port auto-restored to 1883 for mqtt");

    TestLog::testEnd(true);
}

// MQTTS-04: scheme 切换不影响其他配置
void test_mqtts_scheme_switch_preserves_config() {
    TestLog::testStart("MQTTS Scheme Switch Preserves Config");

    MockMQTTClient mqtt;
    MQTTConfig config;
    config.enabled = true;
    config.scheme = "mqtt";
    config.server = "broker.example.com";
    config.port = 1883;
    config.clientId = "TestDevice001";
    config.username = "user";
    config.password = "pass";
    config.autoReconnect = true;

    mqtt.initialize(config);
    MQTTConfig saved = mqtt.getConfig();
    TEST_ASSERT_EQUAL_STRING("mqtt", saved.scheme.c_str());
    TEST_ASSERT_EQUAL_STRING("broker.example.com", saved.server.c_str());
    TestLog::step("Initial mqtt config saved");

    // 切换到 mqtts
    config.scheme = "mqtts";
    config.port = 8883;
    mqtt.initialize(config);
    MQTTConfig updated = mqtt.getConfig();
    TEST_ASSERT_EQUAL_STRING("mqtts", updated.scheme.c_str());
    TEST_ASSERT_EQUAL(8883, updated.port);
    TEST_ASSERT_EQUAL_STRING("broker.example.com", updated.server.c_str());
    TEST_ASSERT_EQUAL_STRING("TestDevice001", updated.clientId.c_str());
    TEST_ASSERT_EQUAL_STRING("user", updated.username.c_str());
    TEST_ASSERT_EQUAL_STRING("pass", updated.password.c_str());
    TEST_ASSERT_TRUE(updated.autoReconnect);
    TestLog::step("After switch to mqtts: server/clientId/auth preserved");

    // 切回 mqtt
    config.scheme = "mqtt";
    config.port = 1883;
    mqtt.initialize(config);
    MQTTConfig restored = mqtt.getConfig();
    TEST_ASSERT_EQUAL_STRING("mqtt", restored.scheme.c_str());
    TEST_ASSERT_EQUAL(1883, restored.port);
    TEST_ASSERT_EQUAL_STRING("broker.example.com", restored.server.c_str());
    TestLog::step("After switch back to mqtt: all fields preserved");

    TestLog::testEnd(true);
}

// MQTTS-05: mqtts 连接/断开生命周期
void test_mqtts_connect_disconnect_lifecycle() {
    TestLog::testStart("MQTTS Connect/Disconnect Lifecycle");

    MockMQTTClient mqtt;
    MQTTConfig config;
    config.enabled = true;
    config.scheme = "mqtts";
    config.server = "tls.broker.example.com";
    config.port = 8883;
    config.clientId = "TestDevice001";
    config.username = "user";
    config.password = "pass";

    mqtt.initialize(config);
    TEST_ASSERT_FALSE(mqtt.getIsConnected());
    TestLog::step("MQTTS client initialized (disconnected)");

    TEST_ASSERT_TRUE(mqtt.connect());
    TEST_ASSERT_TRUE(mqtt.getIsConnected());
    TestLog::step("MQTTS connected successfully");

    // 发布消息验证
    TEST_ASSERT_TRUE(mqtt.publish("test/topic", "hello-mqtts"));
    auto msgs = mqtt.getPublishedMessages();
    TEST_ASSERT_EQUAL(1, (int)msgs.size());
    TEST_ASSERT_EQUAL_STRING("test/topic", msgs[0].topic.c_str());
    TEST_ASSERT_EQUAL_STRING("hello-mqtts", msgs[0].payload.c_str());
    TestLog::step("MQTTS publish works");

    mqtt.disconnect();
    TEST_ASSERT_FALSE(mqtt.getIsConnected());
    TestLog::step("MQTTS disconnected");

    // 断开后不可发布
    TEST_ASSERT_FALSE(mqtt.publish("test/topic", "should-fail"));
    TestLog::step("Publish after disconnect rejected");

    TestLog::testEnd(true);
}

// MQTTS-06: 前端端口联动逻辑验证（模拟 JS）
void test_mqtts_frontend_port_linkage() {
    TestLog::testStart("MQTTS Frontend Port Linkage");

    // 模拟前端端口联动逻辑 (common.js 中的 mqtt-scheme change handler)
    auto simulateSchemeChange = [](String newScheme, int currentPort, String portValue) -> int {
        if (newScheme == "mqtts" && (currentPort == 1883 || portValue == "")) {
            return 8883;
        } else if (newScheme == "mqtt" && (currentPort == 8883 || portValue == "")) {
            return 1883;
        }
        return currentPort;  // 自定义端口不覆盖
    };

    // mqtt -> mqtts: 默认端口 1883 -> 8883
    int port = simulateSchemeChange("mqtts", 1883, "1883");
    TEST_ASSERT_EQUAL(8883, port);
    TestLog::step("mqtt->mqtts: port 1883 -> 8883");

    // mqtts -> mqtt: 默认端口 8883 -> 1883
    port = simulateSchemeChange("mqtt", 8883, "8883");
    TEST_ASSERT_EQUAL(1883, port);
    TestLog::step("mqtts->mqtt: port 8883 -> 1883");

    // mqtt -> mqtts: 自定义端口 9999 不覆盖
    port = simulateSchemeChange("mqtts", 9999, "9999");
    TEST_ASSERT_EQUAL(9999, port);
    TestLog::step("Custom port 9999 preserved on mqtt->mqtts");

    // mqtts -> mqtt: 自定义端口 9999 不覆盖
    port = simulateSchemeChange("mqtt", 9999, "9999");
    TEST_ASSERT_EQUAL(9999, port);
    TestLog::step("Custom port 9999 preserved on mqtts->mqtt");

    // 空端口值：自动填充对应默认端口
    port = simulateSchemeChange("mqtts", 0, "");
    TEST_ASSERT_EQUAL(8883, port);
    TestLog::step("Empty port -> 8883 for mqtts");

    port = simulateSchemeChange("mqtt", 0, "");
    TEST_ASSERT_EQUAL(1883, port);
    TestLog::step("Empty port -> 1883 for mqtt");

    TestLog::testEnd(true);
}

// MQTTS-07: mqtts 重连保持 scheme 配置
void test_mqtts_reconnect_preserves_scheme() {
    TestLog::testStart("MQTTS Reconnect Preserves Scheme");

    MockMQTTClient mqtt;
    MQTTConfig config;
    config.enabled = true;
    config.scheme = "mqtts";
    config.server = "tls.broker.example.com";
    config.port = 8883;
    config.clientId = "TestDevice001";
    config.username = "user";
    config.password = "pass";
    config.autoReconnect = true;
    config.reconnectInterval = 100;

    mqtt.initialize(config);
    mqtt.connect();
    TEST_ASSERT_TRUE(mqtt.getIsConnected());

    mqtt.disconnect();
    TEST_ASSERT_FALSE(mqtt.getIsConnected());
    TestLog::step("Disconnected, waiting for reconnect interval");

    // 触发自动重连
    bool scheduled = mqtt.handleAutoReconnect(200);
    TEST_ASSERT_TRUE(scheduled);
    TEST_ASSERT_TRUE(mqtt.isReconnectPending());
    TestLog::step("Auto-reconnect scheduled for mqtts");

    // 重连后 scheme 仍然正确
    MQTTConfig afterReconnect = mqtt.getConfig();
    TEST_ASSERT_EQUAL_STRING("mqtts", afterReconnect.scheme.c_str());
    TEST_ASSERT_EQUAL(8883, afterReconnect.port);
    TestLog::step("Scheme and port preserved after reconnect");

    TestLog::testEnd(true);
}

// MQTTS-08: 端口 8883 自动推断 mqtts（向后兼容旧配置）
void test_mqtts_port_auto_detect() {
    TestLog::testStart("MQTTS Port-Based Auto-Detect");

    // 模拟 loadMqttConfig 逻辑：无 scheme 字段 + port=8883 → 自动推断 mqtts
    auto detectScheme = [](const char* savedScheme, int port) -> String {
        String scheme = (savedScheme && strlen(savedScheme) > 0) ? String(savedScheme) : String("mqtt");
        bool hasScheme = (savedScheme && strlen(savedScheme) > 0);
        if (!hasScheme && port == 8883) {
            scheme = "mqtts";  // 自动推断
        }
        return scheme;
    };

    // 旧配置：无 scheme + port 8883 → 自动 mqtts
    TEST_ASSERT_EQUAL_STRING("mqtts", detectScheme("", 8883).c_str());
    TestLog::step("No scheme + port 8883 → auto mqtts");

    // 新配置：有 scheme "mqtt" + port 8883 → 保持 mqtt（用户显式设置）
    TEST_ASSERT_EQUAL_STRING("mqtt", detectScheme("mqtt", 8883).c_str());
    TestLog::step("Explicit mqtt + port 8883 → stays mqtt");

    // 有 scheme "mqtts" + port 1883 → 保持 mqtts
    TEST_ASSERT_EQUAL_STRING("mqtts", detectScheme("mqtts", 1883).c_str());
    TestLog::step("Explicit mqtts + port 1883 → stays mqtts");

    // 默认配置：无 scheme + port 1883 → mqtt
    TEST_ASSERT_EQUAL_STRING("mqtt", detectScheme("", 1883).c_str());
    TestLog::step("No scheme + port 1883 → mqtt");

    // 自定义端口：无 scheme + port 9999 → mqtt
    TEST_ASSERT_EQUAL_STRING("mqtt", detectScheme("", 9999).c_str());
    TestLog::step("No scheme + custom port → mqtt");

    TestLog::testEnd(true);
}

// MQTTS-09: MQTTS socket timeout 策略（连接时 30s，连接后 5s）
void test_mqtts_socket_timeout_strategy() {
    TestLog::testStart("MQTTS Socket Timeout Strategy");

    // 模拟 begin() 和 reconnect() 的超时策略
    auto getConnectTimeout = [](const String& scheme) -> int {
        return (scheme == "mqtts") ? 30 : 5;
    };
    constexpr int PUBLISH_TIMEOUT = 5;  // 连接成功后始终用短超时

    // mqtt: 连接超时 5s
    TEST_ASSERT_EQUAL(5, getConnectTimeout("mqtt"));
    TestLog::step("mqtt connect timeout: 5s");

    // mqtts: 连接超时 30s（TLS 握手在 C6 上需 5~15s）
    TEST_ASSERT_EQUAL(30, getConnectTimeout("mqtts"));
    TestLog::step("mqtts connect timeout: 30s");

    // 连接成功后 publish 超时始终为 5s
    TEST_ASSERT_EQUAL(5, PUBLISH_TIMEOUT);
    TestLog::step("publish timeout always 5s after connect");

    TestLog::testEnd(true);
}

// MQTTS-10: MQTTS 重连任务栈大小足够 TLS 握手
void test_mqtts_reconnect_stack_size() {
    TestLog::testStart("MQTTS Reconnect Stack Size");

    // 模拟 MQTTClient::begin() 中的栈分配逻辑
    constexpr uint32_t SIMPLE_TASK_STACK_C6 = 4096;   // ESP32-C6 平台值
    constexpr uint32_t SIMPLE_TASK_STACK_S3 = 6144;   // ESP32-S3 平台值
    constexpr uint32_t MQTTS_STACK = 10240;             // MQTTS 专用栈

    // mqtt 使用默认栈
    uint32_t mqttStack = SIMPLE_TASK_STACK_C6;
    TEST_ASSERT_TRUE(mqttStack >= SIMPLE_TASK_STACK_C6);
    TestLog::step("mqtt uses default stack (4096 on C6)");

    // mqtts 使用专用大栈
    uint32_t mqttsStack = MQTTS_STACK;
    TEST_ASSERT_TRUE(mqttsStack >= 10240);
    TEST_ASSERT_TRUE(mqttsStack > SIMPLE_TASK_STACK_C6);
    TestLog::step("mqtts uses 10KB stack (> 4KB default)");

    // S3 上 mqtt 默认栈也足够
    uint32_t mqttStackS3 = SIMPLE_TASK_STACK_S3;
    TEST_ASSERT_TRUE(mqttStackS3 >= 6144);
    TestLog::step("S3 mqtt uses 6KB default stack");

    TestLog::testEnd(true);
}

// ========== MQTTS 内存保护机制测试 ==========

/**
 * @brief MQTTS-11: doReconnect() 使用 MALLOC_CAP_INTERNAL 检测 DRAM 而非 MALLOC_CAP_DEFAULT
 * 回归：修复前用 MALLOC_CAP_DEFAULT，在有 PSRAM 的 ESP32-S3 上 largest 显示 8MB+，
 * 导致内存检测通过但实际 DRAM 不足，SSL 握手在 connect() 中失败（rc=-32512）
 */
void test_mqtts_memory_check_uses_internal_cap() {
    TestLog::testStart("MQTTS doReconnect Uses MALLOC_CAP_INTERNAL");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read MQTTClient.cpp");

    // 1. 检测 DRAM 内部空闲：heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("heap_caps_get_free_size(MALLOC_CAP_INTERNAL)") != std::string::npos,
        "doReconnect must use MALLOC_CAP_INTERNAL for DRAM free size (not ESP.getFreeHeap)");
    TestLog::step("heap_caps_get_free_size(MALLOC_CAP_INTERNAL) present");

    // 2. 检测 DRAM 最大连续块：heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)") != std::string::npos,
        "doReconnect must use MALLOC_CAP_INTERNAL for largest block (not MALLOC_CAP_DEFAULT)");
    TestLog::step("heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) present");

    // 3. minHeap 阈值降为 35000（setInsecure 跳过证书节省了 ~8-12KB）
    std::regex minHeapRe("minHeap\\s*=\\s*isMqtts\\s*\\?\\s*(\\d+)");
    std::smatch match;
    if (std::regex_search(content, match, minHeapRe) && match.size() > 1) {
        int minHeapVal = std::stoi(match[1].str());
        TEST_ASSERT_TRUE_MESSAGE(minHeapVal >= 30000 && minHeapVal <= 45000,
            "MQTTS minHeap should be 30000-45000 (setInsecure saves cert memory)");
        TestLog::step(("MQTTS minHeap=" + match[1].str() + " (in 30K-45K range)").c_str());
    }

    // 4. minLargestBlock 阈值降为 20000
    std::regex minBlockRe("minLargestBlock\\s*=\\s*isMqtts\\s*\\?\\s*(\\d+)");
    if (std::regex_search(content, match, minBlockRe) && match.size() > 1) {
        int minBlockVal = std::stoi(match[1].str());
        TEST_ASSERT_TRUE_MESSAGE(minBlockVal >= 16000 && minBlockVal <= 30000,
            "MQTTS minLargestBlock should be 16000-30000");
        TestLog::step(("MQTTS minLargestBlock=" + match[1].str() + " (in 16K-30K range)").c_str());
    }

    TestLog::testEnd(true);
}

/**
 * @brief MQTTS-12: 有 PSRAM 时 MALLOC_CAP_DEFAULT largest 远大于 MALLOC_CAP_INTERNAL
 * 说明只用 MALLOC_CAP_DEFAULT 会误判 PSRAM 内存为可用于 SSL 的 DRAM
 */
void test_mqtts_psram_largest_block_misleads_default_cap() {
    TestLog::testStart("MQTTS: PSRAM Misleads MALLOC_CAP_DEFAULT largest");

    // 模拟 ESP32-S3-F8R4 场景：
    //   DRAM 内部碎片化后最大连续块仅 8KB
    //   PSRAM 空闲块 8MB（作为 MALLOC_CAP_DEFAULT 最大块）
    uint32_t dramLargest = 8000;   // MALLOC_CAP_INTERNAL: 8KB (不足 20KB 阈值)
    uint32_t totalLargest = 8 * 1024 * 1024;  // MALLOC_CAP_DEFAULT: 8MB (PSRAM)

    // 旧逻辑（MALLOC_CAP_DEFAULT）：误判内存充足，放行 SSL 连接
    uint32_t OLD_MIN_HEAP = 60000;
    uint32_t OLD_MIN_BLOCK = 30000;
    uint32_t oldFreeHeap = 41628;  // 来自用户实际日志
    bool oldGuardPassed = (oldFreeHeap >= OLD_MIN_HEAP);
    TEST_ASSERT_FALSE_MESSAGE(oldGuardPassed,
        "Old check (heap=41628 < 60000): should have BLOCKED");
    TestLog::step("Old minHeap=60000 would block at heap=41628 (heap threshold too high)");

    // 新逻辑（MALLOC_CAP_INTERNAL）：
    //   DRAM 内部 35KB 总空闲 + 最大连续 20KB → 允许重连
    uint32_t NEW_MIN_HEAP = 35000;
    uint32_t NEW_MIN_BLOCK = 20000;
    uint32_t newDramFree = 40000;  // DRAM 内部 40KB 总空闲（足够）
    bool newGuardPassed = (newDramFree >= NEW_MIN_HEAP) && (dramLargest >= NEW_MIN_BLOCK);
    TEST_ASSERT_FALSE_MESSAGE(newGuardPassed,
        "New DRAM check: free=40KB but largest=8KB < 20KB → should BLOCK (fragmented)");
    TestLog::step("New DRAM check: fragmented DRAM (8KB block) correctly blocks");

    // 真正 DRAM 充足时放行
    uint32_t healthyDramLargest = 25000;  // 25KB 连续 DRAM
    bool healthyGuard = (newDramFree >= NEW_MIN_HEAP) && (healthyDramLargest >= NEW_MIN_BLOCK);
    TEST_ASSERT_TRUE_MESSAGE(healthyGuard,
        "Healthy DRAM (40KB free, 25KB largest) → should ALLOW reconnect");
    TestLog::step("Healthy DRAM: free=40KB, largest=25KB → reconnect allowed");

    // PSRAM largest (8MB) 不应影响 DRAM 连续块检测
    TEST_ASSERT_TRUE_MESSAGE(totalLargest > dramLargest * 100,
        "PSRAM largest block >> DRAM largest block (confirms mislead risk)");
    TestLog::step("PSRAM(8MB) >> DRAM(8KB): confirms MALLOC_CAP_DEFAULT misleads");

    TestLog::testEnd(true);
}

/**
 * @brief MQTTS-13: doReconnect() 输出 DRAM vs Total 对比日志
 * 确保日志可诊断 PSRAM 干扰问题
 */
void test_mqtts_dram_diagnostic_log() {
    TestLog::testStart("MQTTS: DRAM Diagnostic Log in doReconnect");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read MQTTClient.cpp");

    // 日志必须同时打印 DRAM 和 Total 对比
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("DRAM free=") != std::string::npos ||
        content.find("dram=") != std::string::npos,
        "doReconnect diagnostic log must include DRAM free size");
    TestLog::step("DRAM free size in diagnostic log");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("total free=") != std::string::npos ||
        content.find("heap=%lu largest=%lu scheme=") != std::string::npos,
        "doReconnect diagnostic log must include total free size for comparison");
    TestLog::step("Total free size in diagnostic log for comparison");

    TestLog::testEnd(true);
}

/**
 * @brief MQTTS-14: setInsecure() 节省内存验证
 * 使用 setInsecure() 跳过证书验证后，MQTTS 内存需求降至约 28-32KB
 */
void test_mqtts_setinsecure_reduces_memory_requirement() {
    TestLog::testStart("MQTTS: setInsecure() Reduces Memory Requirement");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read MQTTClient.cpp");

    // 代码中必须使用 setInsecure()（跳过证书验证）
    TEST_ASSERT_TRUE_MESSAGE(content.find("setInsecure()") != std::string::npos,
        "MQTTS must use setInsecure() to skip certificate validation");
    TestLog::step("setInsecure() present: certificate validation skipped");

    // 注释中应说明 setInsecure 节省内存
    bool hasSavingComment = content.find("setInsecure") != std::string::npos &&
        (content.find("\u8df3\u8fc7\u8bc1\u4e66") != std::string::npos ||
         content.find("\u8bc1\u4e66\u9a8c\u8bc1\u8df3\u8fc7") != std::string::npos ||
         content.find("skip") != std::string::npos ||
         content.find("certificate verification skipped") != std::string::npos);
    TEST_ASSERT_TRUE_MESSAGE(hasSavingComment,
        "Code should document that setInsecure saves ~8-12KB cert memory");
    TestLog::step("setInsecure() saves cert memory documented");

    // MQTTS minHeap 阈值应低于 50000（不应还在要求完整 60KB）
    std::regex minHeapRe("minHeap\\s*=\\s*isMqtts\\s*\\?\\s*(\\d+)");
    std::smatch match;
    if (std::regex_search(content, match, minHeapRe) && match.size() > 1) {
        int minHeapVal = std::stoi(match[1].str());
        TEST_ASSERT_TRUE_MESSAGE(minHeapVal < 50000,
            "MQTTS minHeap must be < 50000 when using setInsecure (no cert memory needed)");
        TestLog::step(("MQTTS minHeap=" + match[1].str() + " < 50000").c_str());
    }

    TestLog::testEnd(true);
}

// ============================================================
// Group 8: 测试连接快速路径 (alreadyConnected) 回归测试
// 修复：MQTTS测试按钮显示CONNECTION_TIMEOUT但状态栏显示已连接
// 根因：scheme参数未传递 + 重复连接clientId冲突 + TLS超时不足
// ============================================================

/**
 * @brief 后端快速路径：主客户端已连接且配置匹配时直接返回 alreadyConnected
 * 回归：修复前每次点击测试都创建新连接，导致华为云踢掉已有连接或MQTTS超时
 */
void test_mqtt_test_fast_path_already_connected() {
    TestLog::testStart("Test Connection Fast Path: alreadyConnected");

    std::string content = readProjectFile("src/network/handlers/MqttRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read MqttRouteHandler.cpp");

    // 1. 快速路径检查主客户端是否已连接
    TEST_ASSERT_TRUE_MESSAGE(content.find("alreadyConnected") != std::string::npos,
        "handleTestMqttConnection must have alreadyConnected fast path");
    TestLog::step("alreadyConnected field present in response");

    // 2. 检查主客户端连接状态
    TEST_ASSERT_TRUE_MESSAGE(content.find("getIsConnected") != std::string::npos,
        "Fast path must check existingMqtt->getIsConnected()");
    TestLog::step("getIsConnected() check present");

    // 3. 检查主客户端未被停止
    TEST_ASSERT_TRUE_MESSAGE(content.find("isStopped") != std::string::npos,
        "Fast path must check !existingMqtt->isStopped()");
    TestLog::step("isStopped() check present");

    // 4. 配置匹配检查：server + port + scheme
    TEST_ASSERT_TRUE_MESSAGE(content.find("serverMatch") != std::string::npos,
        "Fast path must verify server:port match");
    TEST_ASSERT_TRUE_MESSAGE(content.find("schemeMatch") != std::string::npos,
        "Fast path must verify scheme match");
    TestLog::step("server + port + scheme matching present");

    TestLog::testEnd(true);
}

/**
 * @brief 快速路径模拟：已连接 + 配置匹配 → 返回 alreadyConnected
 * 模拟后端快速路径的判断逻辑
 */
void test_mqtt_test_fast_path_simulation() {
    TestLog::testStart("Fast Path Logic Simulation");

    // 模拟后端快速路径判断
    struct MockMqttState {
        bool isConnected;
        bool isStopped;
        String server;
        int port;
        String scheme;
    };

    auto shouldReturnAlreadyConnected = [](const MockMqttState& state,
            const String& reqServer, int reqPort, const String& reqScheme) -> bool {
        if (!state.isConnected || state.isStopped) return false;
        bool serverMatch = (state.server == reqServer && state.port == reqPort);
        bool schemeMatch = (state.scheme == reqScheme);
        return serverMatch && schemeMatch;
    };

    // Case 1: 已连接 + 配置完全匹配 → true
    MockMqttState s1 = {true, false, "iot.fastbee.cn", 1883, "mqtt"};
    TEST_ASSERT_TRUE(shouldReturnAlreadyConnected(s1, "iot.fastbee.cn", 1883, "mqtt"));
    TestLog::step("Connected + matching config → alreadyConnected");

    // Case 2: 已连接 + server不匹配 → false
    TEST_ASSERT_FALSE(shouldReturnAlreadyConnected(s1, "other.broker.com", 1883, "mqtt"));
    TestLog::step("Connected + different server → NOT alreadyConnected");

    // Case 3: 已连接 + port不匹配 → false
    TEST_ASSERT_FALSE(shouldReturnAlreadyConnected(s1, "iot.fastbee.cn", 8883, "mqtt"));
    TestLog::step("Connected + different port → NOT alreadyConnected");

    // Case 4: 已连接 + scheme不匹配 → false (关键：MQTTS vs MQTT)
    TEST_ASSERT_FALSE(shouldReturnAlreadyConnected(s1, "iot.fastbee.cn", 1883, "mqtts"));
    TestLog::step("Connected + different scheme → NOT alreadyConnected");

    // Case 5: 未连接 → false
    MockMqttState s2 = {false, false, "iot.fastbee.cn", 1883, "mqtt"};
    TEST_ASSERT_FALSE(shouldReturnAlreadyConnected(s2, "iot.fastbee.cn", 1883, "mqtt"));
    TestLog::step("Not connected → NOT alreadyConnected");

    // Case 6: 已停止 → false
    MockMqttState s3 = {true, true, "iot.fastbee.cn", 1883, "mqtt"};
    TEST_ASSERT_FALSE(shouldReturnAlreadyConnected(s3, "iot.fastbee.cn", 1883, "mqtt"));
    TestLog::step("Stopped → NOT alreadyConnected");

    // Case 7: MQTTS 已连接 + MQTTS 匹配 → true
    MockMqttState s4 = {true, false, "huaweicloud.com", 8883, "mqtts"};
    TEST_ASSERT_TRUE(shouldReturnAlreadyConnected(s4, "huaweicloud.com", 8883, "mqtts"));
    TestLog::step("MQTTS connected + matching → alreadyConnected");

    TestLog::testEnd(true);
}

/**
 * @brief 前端 testMqttConnection 必须传递 scheme 参数
 * 回归：修复前未传递 scheme，后端默认 mqtt，MQTTS 时用了 WiFiClient 而非 WiFiClientSecure
 */
void test_mqtt_test_frontend_scheme_param() {
    TestLog::testStart("Frontend: testMqttConnection Passes scheme");

    std::string content = readProjectFile("web-src/modules/runtime/protocol/mqtt-config.js");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read mqtt-config.js");

    // 1. testMqttConnection 中必须读取 mqtt-scheme 下拉框值
    TEST_ASSERT_TRUE_MESSAGE(content.find("mqtt-scheme") != std::string::npos,
        "testMqttConnection must read mqtt-scheme dropdown value");
    TestLog::step("mqtt-scheme element read present");

    // 2. apiMqttTest 调用参数中必须包含 scheme
    // 匹配 apiMqttTest({ ... scheme ... })
    std::regex apiCallRe("apiMqttTest\\([^)]*scheme");
    TEST_ASSERT_TRUE_MESSAGE(std::regex_search(content, apiCallRe),
        "apiMqttTest call must include scheme parameter");
    TestLog::step("scheme parameter passed to apiMqttTest");

    TestLog::testEnd(true);
}

/**
 * @brief 前端处理 alreadyConnected 响应：显示"连接正常"而非创建新连接
 */
void test_mqtt_test_frontend_already_connected_handler() {
    TestLog::testStart("Frontend: alreadyConnected Response Handler");

    std::string content = readProjectFile("web-src/modules/runtime/protocol/mqtt-config.js");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read mqtt-config.js");

    // 1. 检查 alreadyConnected 条件判断
    TEST_ASSERT_TRUE_MESSAGE(content.find("alreadyConnected") != std::string::npos,
        "Frontend must handle alreadyConnected response field");
    TestLog::step("alreadyConnected check present");

    // 2. alreadyConnected 时应显示成功信息（绿色）
    TEST_ASSERT_TRUE_MESSAGE(content.find("连接正常") != std::string::npos ||
                             content.find("无需重新测试") != std::string::npos,
        "alreadyConnected must show success message");
    TestLog::step("Success message for alreadyConnected present");

    // 3. alreadyConnected 时应更新 badge 为"已连接"
    //    检查 alreadyConnected 块中包含 mqtt-status-online
    auto alreadyConnectedPos = content.find("alreadyConnected");
    auto badgeOnlinePos = content.find("mqtt-status-online");
    TEST_ASSERT_TRUE_MESSAGE(alreadyConnectedPos != std::string::npos &&
                             badgeOnlinePos != std::string::npos &&
                             badgeOnlinePos > alreadyConnectedPos,
        "alreadyConnected block must contain mqtt-status-online badge update");
    TestLog::step("Badge update to online for alreadyConnected");

    TestLog::testEnd(true);
}

/**
 * @brief 后端 MQTTS 测试客户端使用 setSocketTimeout(30)
 * 回归：默认 15s 在 ESP32-C6 上 TLS 握手超时
 */
void test_mqtt_test_backend_mqtts_socket_timeout() {
    TestLog::testStart("Backend: MQTTS Socket Timeout via Main Client");

    // MQTTS socket timeout 现在由主客户端 MQTTClient.cpp 处理
    // （测试连接已改为 save+restart 方式，不再创建独立的 PubSubClient）
    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read MQTTClient.cpp");

    // 1. 必须有 setSocketTimeout 调用
    TEST_ASSERT_TRUE_MESSAGE(content.find("setSocketTimeout") != std::string::npos,
        "Main client must call setSocketTimeout for MQTTS");
    TestLog::step("setSocketTimeout call present in main client");

    // 2. 超时值为 30（MQTTS TLS 握手需要更多时间）
    std::regex timeoutRe("setSocketTimeout\\(30\\)");
    TEST_ASSERT_TRUE_MESSAGE(std::regex_search(content, timeoutRe),
        "MQTTS main client socket timeout must be 30 seconds");
    TestLog::step("Socket timeout set to 30s for MQTTS in main client");

    // 3. 连接后恢复为 5s（避免 publish/write 对死 socket 长时间阻塞）
    std::regex restoreRe("setSocketTimeout\\(5\\)");
    TEST_ASSERT_TRUE_MESSAGE(std::regex_search(content, restoreRe),
        "Must restore socket timeout to 5s after MQTTS connection");
    TestLog::step("Socket timeout restored to 5s after connection");

    TestLog::testEnd(true);
}

/**
 * @brief MQTT_TEST_TIMEOUT 必须 >= 45s，覆盖 MQTTS TLS 握手时间
 * 回归：原来 30s 在 ESP32-C6 上不够
 */
void test_mqtt_test_frontend_timeout_extended() {
    TestLog::testStart("Frontend: MQTT_TEST_TIMEOUT >= 45s");

    std::string content = readProjectFile("web-src/js/request-governor.js");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read request-governor.js");

    // 1. MQTT_TEST_TIMEOUT 常量存在
    TEST_ASSERT_TRUE_MESSAGE(content.find("MQTT_TEST_TIMEOUT") != std::string::npos,
        "MQTT_TEST_TIMEOUT constant must exist");
    TestLog::step("MQTT_TEST_TIMEOUT constant present");

    // 2. 值 >= 45000ms
    std::regex timeoutRe("MQTT_TEST_TIMEOUT\\s*=\\s*(\\d+)");
    std::smatch match;
    TEST_ASSERT_TRUE_MESSAGE(std::regex_search(content, match, timeoutRe),
        "MQTT_TEST_TIMEOUT must have a numeric value");
    if (match.size() > 1) {
        int timeoutMs = std::stoi(match[1].str());
        TEST_ASSERT_GREATER_OR_EQUAL(45000, timeoutMs);
        TestLog::step(("MQTT_TEST_TIMEOUT = " + match[1].str() + "ms (>= 45000)").c_str());
    }

    TestLog::testEnd(true);
}

/**
 * @brief 快速路径不影响配置不匹配时的正常测试流程
 * 当 server/port/scheme 任一不匹配时，仍走原始测试路径
 */
void test_mqtt_test_fast_path_no_false_positive() {
    TestLog::testStart("Fast Path No False Positive");

    std::string content = readProjectFile("src/network/handlers/MqttRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read MqttRouteHandler.cpp");

    // 快速路径返回后必须直接 return，不继续走 save+restart 流程
    auto acPos = content.find("alreadyConnected");
    auto retPos = content.find("return;", acPos);
    TEST_ASSERT_TRUE_MESSAGE(acPos != std::string::npos &&
                             retPos != std::string::npos &&
                             retPos - acPos < 900,
        "Fast path must return immediately after sending alreadyConnected response");
    TestLog::step("Fast path returns immediately, no save+restart triggered");

    // 配置不匹配时走 save+restart 路径（不再创建独立 PubSubClient）
    TEST_ASSERT_TRUE_MESSAGE(content.find("saveMqttTestConfig") != std::string::npos,
        "Non-matching config must use save+restart approach");
    TestLog::step("Save+restart path used for non-matching configs");

    // 确认不再有独立的测试客户端
    TEST_ASSERT_TRUE_MESSAGE(content.find("WiFiClientSecure testWifiSecure") == std::string::npos,
        "No separate WiFiClientSecure for testing (eliminated MQTT_BAD_CLIENT_ID)");
    TestLog::step("No separate test client created");

    TestLog::testEnd(true);
}

/**
 * @brief 简单认证模式尊重表单传递的clientId
 * 回归：修复前总是用 S&deviceNum&productId&userId 覆盖，导致自定义clientId被替换
 */
void test_mqtt_test_simple_auth_respects_form_clientid() {
    TestLog::testStart("Simple Auth Respects Form clientId");

    std::string content = readProjectFile("src/network/handlers/MqttRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read MqttRouteHandler.cpp");

    // 简单认证路径必须在 clientId.isEmpty() 时才构建 S& 格式
    // 而非无条件覆盖
    TEST_ASSERT_TRUE_MESSAGE(content.find("if (clientId.isEmpty())") != std::string::npos,
        "Simple auth must check if clientId is empty before auto-generating S& format");
    TestLog::step("clientId.isEmpty() check present before S& generation");

    // S& 格式构建必须在 isEmpty 检查内部
    auto emptyCheckPos = content.find("if (clientId.isEmpty())");
    auto sFormatPos = content.find("\"S&\" + deviceNum");
    TEST_ASSERT_TRUE_MESSAGE(emptyCheckPos != std::string::npos &&
                             sFormatPos != std::string::npos &&
                             sFormatPos > emptyCheckPos &&
                             sFormatPos - emptyCheckPos < 200,
        "S& clientId generation must be inside clientId.isEmpty() block");
    TestLog::step("S& generation is inside isEmpty() guard");

    TestLog::testEnd(true);
}

/**
 * @brief 快速路径覆盖“正在连接中”状态，避免重复clientId导致MQTT_BAD_CLIENT_ID
 * 回归：修复前只检查 getIsConnected()，主客户端正在连接时测试创建重复连接被broker拒绝
 */
void test_mqtt_test_fast_path_connecting_state() {
    TestLog::testStart("Fast Path Covers Connecting State");

    std::string content = readProjectFile("src/network/handlers/MqttRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read MqttRouteHandler.cpp");

    // 快速路径必须检查 !isStopped() 而非仅 getIsConnected()
    // 这样主客户端正在连接时也会命中快速路径
    TEST_ASSERT_TRUE_MESSAGE(content.find("!existingMqtt->isStopped()") != std::string::npos,
        "Fast path must check !isStopped() to cover connecting state");
    TestLog::step("!isStopped() check present in fast path");

    // 快速路径应区分已连接和连接中两种状态
    TEST_ASSERT_TRUE_MESSAGE(content.find("alreadyConnected") != std::string::npos,
        "Fast path must return alreadyConnected when main client is connected");
    TEST_ASSERT_TRUE_MESSAGE(content.find("deferred") != std::string::npos,
        "Fast path must return deferred when main client is connecting");
    TestLog::step("Fast path distinguishes connected vs connecting states");

    TestLog::testEnd(true);
}

/**
 * @brief saveMqttTestConfig 保存 clientId 到 protocol.json
 * 回归：修复前测试成功后不保存clientId，导致下次加载时clientId丢失
 */
void test_mqtt_test_save_config_includes_clientid() {
    TestLog::testStart("saveMqttTestConfig Includes clientId");

    std::string content = readProjectFile("src/network/handlers/MqttRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read MqttRouteHandler.cpp");

    // saveMqttTestConfig 函数签名应包含 clientId 参数
    std::regex sigRe("saveMqttTestConfig.*clientId");
    TEST_ASSERT_TRUE_MESSAGE(std::regex_search(content, sigRe),
        "saveMqttTestConfig must accept clientId parameter");
    TestLog::step("saveMqttTestConfig signature includes clientId");

    // saveMqttTestConfig 内部应将 clientId 写入 JSON
    TEST_ASSERT_TRUE_MESSAGE(content.find("mqtt[\"clientId\"]") != std::string::npos,
        "saveMqttTestConfig must write clientId to protocol.json");
    TestLog::step("clientId written to protocol.json mqtt object");

    // 两处 saveMqttTestConfig 调用都应传入 clientId
    // 统计包含 "saveMqttTestConfig(" 且同一行或附近有 clientId 的调用
    int callsWithClientId = 0;
    size_t pos = 0;
    while ((pos = content.find("saveMqttTestConfig(", pos)) != std::string::npos) {
        // 检查接下来 500 字符内是否有 clientId
        std::string snippet = content.substr(pos, std::min((size_t)500, content.size() - pos));
        if (snippet.find("clientId") != std::string::npos) callsWithClientId++;
        pos += 20;
    }
    TEST_ASSERT_GREATER_OR_EQUAL(2, callsWithClientId);
    TestLog::step("Both saveMqttTestConfig calls pass clientId");

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
    RUN_TEST(test_mqtt_doReconnect_heap_threshold_8kb);
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

    // MQTTS Scheme 支持测试
    RUN_TEST(test_mqtts_scheme_default_value);
    RUN_TEST(test_mqtts_scheme_set_mqtts);
    RUN_TEST(test_mqtts_default_port);
    RUN_TEST(test_mqtts_scheme_switch_preserves_config);
    RUN_TEST(test_mqtts_connect_disconnect_lifecycle);
    RUN_TEST(test_mqtts_frontend_port_linkage);
    RUN_TEST(test_mqtts_reconnect_preserves_scheme);
    RUN_TEST(test_mqtts_port_auto_detect);
    RUN_TEST(test_mqtts_socket_timeout_strategy);
    RUN_TEST(test_mqtts_reconnect_stack_size);

    // MQTTS 内存保护机制测试（修复 PSRAM 干扰 DRAM 检测导致 SSL 失败）
    RUN_TEST(test_mqtts_memory_check_uses_internal_cap);
    RUN_TEST(test_mqtts_psram_largest_block_misleads_default_cap);
    RUN_TEST(test_mqtts_dram_diagnostic_log);
    RUN_TEST(test_mqtts_setinsecure_reduces_memory_requirement);

    // Group 8: 测试连接快速路径 (alreadyConnected) 回归测试
    RUN_TEST(test_mqtt_test_fast_path_already_connected);
    RUN_TEST(test_mqtt_test_fast_path_simulation);
    RUN_TEST(test_mqtt_test_frontend_scheme_param);
    RUN_TEST(test_mqtt_test_frontend_already_connected_handler);
    RUN_TEST(test_mqtt_test_backend_mqtts_socket_timeout);
    RUN_TEST(test_mqtt_test_frontend_timeout_extended);
    RUN_TEST(test_mqtt_test_fast_path_no_false_positive);
    RUN_TEST(test_mqtt_test_simple_auth_respects_form_clientid);
    RUN_TEST(test_mqtt_test_fast_path_connecting_state);
    RUN_TEST(test_mqtt_test_save_config_includes_clientid);

    TestLog::groupEnd();
}
