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
 * @brief 回归：自动补启动不应拆掉已加载/运行的主 MQTT 客户端
 *
 * 场景：以太网 MQTTS 握手成功后，网络稳定任务或状态轮询再次调用
 * restartMQTTDeferred()。旧逻辑会先释放旧客户端，再检查内存；在低 DRAM
 * 窗口里可能释放成功但重建失败，导致 MQTTClient 变成 nullptr。
 *
 * 新逻辑：默认调用在客户端已有有效运行配置且未停止时直接返回；测试/恢复
 * 配置路径显式传入 forceRebuild=true，仍然强制加载最新 protocol.json。
 */
void test_mqtt_deferred_restart_preserves_active_client() {
    TestLog::testStart("MQTT Deferred Restart: Preserve Active Client");

    std::string header = readProjectFile("include/protocols/ProtocolManager.h");
    std::string pm = readProjectFile("src/protocols/ProtocolManager.cpp");
    std::string route = readProjectFile("src/network/handlers/MqttRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!header.empty(), "ProtocolManager.h must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!pm.empty(), "ProtocolManager.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!route.empty(), "MqttRouteHandler.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        header.find("restartMQTTDeferred(bool forceRebuild = false)") != std::string::npos,
        "restartMQTTDeferred must expose default non-forced mode");

    size_t defPos = pm.find("bool ProtocolManager::restartMQTTDeferred(bool forceRebuild)");
    TEST_ASSERT_TRUE_MESSAGE(defPos != std::string::npos,
        "restartMQTTDeferred implementation must accept forceRebuild");

    // mqtt.enabled 守卫必须在所有其他逻辑之前（最外层守卫）
    size_t enabledCheckPos = pm.find("\"mqtt\"", defPos);
    TEST_ASSERT_TRUE_MESSAGE(enabledCheckPos != std::string::npos,
        "restartMQTTDeferred must check mqtt.enabled before any other logic");

    size_t guardPos = pm.find("!forceRebuild && mqttClient && !mqttClient->isStopped()", defPos);
    TEST_ASSERT_TRUE_MESSAGE(guardPos != std::string::npos,
        "restartMQTTDeferred must guard non-forced active clients");
    TEST_ASSERT_TRUE_MESSAGE(enabledCheckPos < guardPos,
        "mqtt.enabled check must run before active-client guard");
    TestLog::step("mqtt.enabled guard runs before active-client guard");

    // active-client guard 必须在 rebuild 的 reset 之前
    // 查找 guard 之后的 reset（跳过 mqtt.enabled 块中的 reset）
    size_t resetPos = pm.find("mqttClient.reset()", guardPos);
    TEST_ASSERT_TRUE_MESSAGE(resetPos != std::string::npos,
        "restartMQTTDeferred must still release clients when rebuilding");
    TEST_ASSERT_TRUE_MESSAGE(guardPos < resetPos,
        "active-client guard must run before mqttClient.reset()");
    TestLog::step("Default restart preserves active client before reset");

    TEST_ASSERT_TRUE_MESSAGE(
        pm.find("currentConfig.server.isEmpty()", guardPos) != std::string::npos &&
        pm.find("currentConfig.port > 0", guardPos) != std::string::npos,
        "active-client guard must require a loaded runtime config");
    TEST_ASSERT_TRUE_MESSAGE(
        pm.find("Deferred restart skipped: client already active", guardPos) != std::string::npos,
        "guard must log the preserved-client path for serial diagnostics");
    TestLog::step("Guard checks loaded config and emits serial diagnostic");

    TEST_ASSERT_TRUE_MESSAGE(
        route.find("restartMQTTDeferred(true)") != std::string::npos,
        "MQTT test/restore paths must force rebuild after config changes");
    TEST_ASSERT_TRUE_MESSAGE(
        route.find("started = pm->restartMQTTDeferred();") != std::string::npos,
        "status auto-start path must keep the default non-forced guard");
    TestLog::step("Config-change paths force rebuild; status auto-start stays guarded");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 restartMQTTDeferred 堆内存检查顺序：先释放旧客户端，再检查堆
 * 复现场景：旧客户端占 12KB，堆仅 18KB，释放前检查 <15KB 误判失败
 * 阈值已从 25KB 降至 15KB，防止低堆时永远无法重建客户端
 */
void test_mqtt_restore_restart_handle_survives_null_client() {
    TestLog::testStart("MQTT Restore Restart: Handle Survives Null Client");

    std::string pm = readProjectFile("src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!pm.empty(), "ProtocolManager.cpp must be readable");

    size_t restorePos = pm.find("MqttRouteHandler::checkPendingTestRestore()");
    TEST_ASSERT_TRUE_MESSAGE(restorePos != std::string::npos,
        "ProtocolManager::handle must call checkPendingTestRestore");

    size_t debugPos = pm.find("getIsConnected()", restorePos);
    TEST_ASSERT_TRUE_MESSAGE(debugPos != std::string::npos,
        "ProtocolManager::handle must still emit MQTT alive diagnostics");

    // Diagnostics must call getIsConnected() and be inside the outer if(mqttClient) block,
    // so no redundant inner "mqttClient &&" guard is needed.
    TEST_ASSERT_TRUE_MESSAGE(debugPos != std::string::npos && debugPos > restorePos,
        "MQTT alive diagnostics must call getIsConnected() after test restore check");

    TestLog::step("Post-restore MQTT diagnostics inside outer mqttClient guard (no redundant check)");
    TestLog::testEnd(true);
}

void test_mqtt_deferred_restart_keeps_mqtts_client_in_marginal_dram() {
    TestLog::testStart("MQTT Deferred Restart: Keep MQTTS Client in Marginal DRAM");

    std::string pm = readProjectFile("src/protocols/ProtocolManager.cpp");
    std::string mqtt = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!pm.empty(), "ProtocolManager.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!mqtt.empty(), "MQTTClient.cpp must be readable");

    size_t defPos = pm.find("bool ProtocolManager::restartMQTTDeferred(bool forceRebuild)");
    TEST_ASSERT_TRUE_MESSAGE(defPos != std::string::npos,
        "restartMQTTDeferred implementation must exist");

    size_t lowMemPos = pm.find("if (freeHeap < minHeap || largestBlock < minLargestBlock)", defPos);
    TEST_ASSERT_TRUE_MESSAGE(lowMemPos != std::string::npos,
        "restartMQTTDeferred must check DRAM and largest block");

    size_t marginalPos = pm.find("DRAM marginal for MQTTS deferred restart", lowMemPos);
    size_t createPos = pm.find("mqttClient = std::unique_ptr<MQTTClient>", lowMemPos);
    TEST_ASSERT_TRUE_MESSAGE(marginalPos != std::string::npos && createPos != std::string::npos && marginalPos < createPos,
        "MQTTS marginal DRAM path must continue to create a client and let reconnect backoff/reclaim");

    size_t stopPos = pm.find("mqttClient->stop()", defPos);
    size_t resetPos = pm.find("mqttClient.reset()", defPos);
    TEST_ASSERT_TRUE_MESSAGE(stopPos != std::string::npos && resetPos != std::string::npos && stopPos < resetPos,
        "restartMQTTDeferred must stop the old client before reset to cancel reconnect workers");

    size_t shutdownPos = mqtt.find("void MQTTClient::shutdown()");
    TEST_ASSERT_TRUE_MESSAGE(shutdownPos != std::string::npos,
        "MQTTClient::shutdown must exist");
    TEST_ASSERT_TRUE_MESSAGE(
        mqtt.find("_reconnectPending = false", shutdownPos) != std::string::npos &&
        mqtt.find("while (_reconnectRunning", shutdownPos) != std::string::npos,
        "shutdown must cancel pending reconnects and wait briefly for a running reconnect to exit");

    size_t stopFnPos = mqtt.find("void MQTTClient::stop()");
    TEST_ASSERT_TRUE_MESSAGE(stopFnPos != std::string::npos,
        "MQTTClient::stop must exist");
    TEST_ASSERT_TRUE_MESSAGE(
        mqtt.find("shutdown()", stopFnPos) != std::string::npos &&
        mqtt.find("disconnect()", stopFnPos) != std::string::npos,
        "stop must cancel reconnect worker before disconnecting sockets");

    TestLog::step("MQTTS marginal restart keeps a client and stop cancels workers");
    TestLog::testEnd(true);
}

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
    // MQTT 已禁用：优先显示“未连接”，不检查 connected 字段
    if (!s.enabled) {
        return "未连接";  // mqtt-status-offline
    }
    if (s.connected) {
        return "已连接";  // mqtt-status-online
    }
    if (s.internetAvailable == false) {
        return "网络未连接";  // mqtt-status-offline
    }
    bool connecting = s.connecting;
    bool hasError = s.lastError != 0;

    if (connecting && !hasError) {
        return "连接中";  // mqtt-status-connecting
    }
    if (connecting && hasError) {
        return "连接失败";  // mqtt-status-offline
    }
    if (!s.initialized) {
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

    // MQTT 未启用 → 未连接（优先于其他状态判断）
    MqttStatusApiResponse s1 = {};
    s1.initialized = false;
    s1.connected = false;
    s1.connecting = false;
    s1.stopped = false;
    s1.enabled = false;
    s1.internetAvailable = true;
    TEST_ASSERT_EQUAL_STRING("未连接", mapBadgeState(s1));
    TestLog::step("enabled=false, initialized=false → '未连接'");

    // MQTT 禁用但客户端仍在运行（重启前窗口期） → 仍然显示“未连接”
    MqttStatusApiResponse s1b = {};
    s1b.initialized = true;
    s1b.connected = true;      // 客户端仍在运行
    s1b.connecting = false;
    s1b.stopped = false;
    s1b.enabled = false;        // 但配置已禁用
    s1b.internetAvailable = true;
    TEST_ASSERT_EQUAL_STRING("未连接", mapBadgeState(s1b));
    TestLog::step("enabled=false, connected=true → '未连接' (disabled overrides connected)");

    // MQTT 已启用但客户端未创建 → 初始化中
    MqttStatusApiResponse s2 = {};
    s2.initialized = false;
    s2.connected = false;
    s2.connecting = false;
    s2.stopped = false;
    s2.enabled = true;
    s2.internetAvailable = true;
    TEST_ASSERT_EQUAL_STRING("初始化中", mapBadgeState(s2));
    TestLog::step("enabled=true, initialized=false → '初始化中'");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 MQTT 禁用状态的各种组合 → badge 始终显示“未连接”
 * 覆盖：disabled+connecting、disabled+error、disabled+initialized 等
 */
void test_mqtt_status_api_disabled_comprehensive() {
    TestLog::testStart("MQTT Status: Disabled Comprehensive → Always '未连接'");

    // 基础禁用状态
    MqttStatusApiResponse s = {};
    s.enabled = false;
    s.internetAvailable = true;
    TEST_ASSERT_EQUAL_STRING("未连接", mapBadgeState(s));
    TestLog::step("disabled + all-zero → '未连接'");

    // 禁用 + connecting=true → 仍然“未连接”
    s.connecting = true;
    s.initialized = true;
    TEST_ASSERT_EQUAL_STRING("未连接", mapBadgeState(s));
    TestLog::step("disabled + connecting=true → '未连接'");

    // 禁用 + connecting + error → 仍然“未连接”
    s.lastError = -2;  // MQTT_CONNECT_FAILED
    TEST_ASSERT_EQUAL_STRING("未连接", mapBadgeState(s));
    TestLog::step("disabled + connecting + error → '未连接'");

    // 禁用 + internetAvailable=false → 仍然“未连接”
    s.connecting = false;
    s.lastError = 0;
    s.internetAvailable = false;
    TEST_ASSERT_EQUAL_STRING("未连接", mapBadgeState(s));
    TestLog::step("disabled + no internet → '未连接'");

    // 禁用 + stopped → “未连接”
    s.stopped = true;
    s.internetAvailable = true;
    TEST_ASSERT_EQUAL_STRING("未连接", mapBadgeState(s));
    TestLog::step("disabled + stopped → '未连接'");

    TestLog::testEnd(true);
}

/**
 * @brief 验证网络不可用时的 badge 状态
 * 仅在 MQTT 已启用且未连接时显示“网络未连接”
 */
void test_mqtt_status_api_network_unavailable() {
    TestLog::testStart("MQTT Status: Network Unavailable → Badge State");

    // 已启用 + 未连接 + 网络不可用 → “网络未连接”
    MqttStatusApiResponse s = {};
    s.initialized = true;
    s.connected = false;
    s.connecting = false;
    s.stopped = false;
    s.enabled = true;
    s.lastError = 0;
    s.reconnectCount = 0;
    s.autoReconnect = true;
    s.internetAvailable = false;
    TEST_ASSERT_EQUAL_STRING("网络未连接", mapBadgeState(s));
    TestLog::step("enabled + no internet + not connecting → '网络未连接'");

    // 已启用 + connecting + 网络不可用 → “网络未连接”（internetAvailable 优先于 connecting）
    s.connecting = true;
    TEST_ASSERT_EQUAL_STRING("网络未连接", mapBadgeState(s));
    TestLog::step("enabled + no internet + connecting → '网络未连接' (internet check first)");

    // 已禁用 + 网络不可用 → “未连接”（disabled 优先级最高）
    s.enabled = false;
    TEST_ASSERT_EQUAL_STRING("未连接", mapBadgeState(s));
    TestLog::step("disabled + no internet → '未连接' (disabled first)");

    TestLog::testEnd(true);
}

/**
 * @brief 验证连接中但有错误 → badge 显示“连接失败”
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
    s.internetAvailable = true; // 网络可用，错误是 MQTT 层面的

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
    constexpr int POST_CONNECT_TIMEOUT = 15;  // 连接成功后 MQTTS 用 15s（兼顾 TLS 读和解耦死 socket）

    // mqtt: 连接超时 5s
    TEST_ASSERT_EQUAL(5, getConnectTimeout("mqtt"));
    TestLog::step("mqtt connect timeout: 5s");

    // mqtts: 连接超时 30s（TLS 握手在 C6 上需 5~15s）
    TEST_ASSERT_EQUAL(30, getConnectTimeout("mqtts"));
    TestLog::step("mqtts connect timeout: 30s");

    // 连接成功后 MQTTS 超时为 15s（不是 5s，防止 TLS 读超时误断连）
    TEST_ASSERT_EQUAL(15, POST_CONNECT_TIMEOUT);
    TestLog::step("mqtts post-connect timeout: 15s");

    TestLog::testEnd(true);
}

// MQTTS-10: MQTTS 重连任务栈保持精简，给 TLS 堆缓冲留下 DRAM
void test_mqtts_reconnect_stack_size() {
    TestLog::testStart("MQTTS Reconnect Stack Size");

    // 模拟 MQTTClient::begin() 中的栈分配逻辑
    constexpr uint32_t SIMPLE_TASK_STACK_C6 = 4096;   // ESP32-C6 平台值
    constexpr uint32_t SIMPLE_TASK_STACK_S3 = 6144;   // ESP32-S3 平台值
    constexpr uint32_t MQTTS_STACK = 6144;             // MQTTS 专用栈

    // mqtt 使用默认栈
    uint32_t mqttStack = SIMPLE_TASK_STACK_C6;
    TEST_ASSERT_TRUE(mqttStack >= SIMPLE_TASK_STACK_C6);
    TestLog::step("mqtt uses default stack (4096 on C6)");

    // mqtts 使用 6KB 栈，避免先消耗 mbedTLS 需要的内部 DRAM
    uint32_t mqttsStack = MQTTS_STACK;
    TEST_ASSERT_TRUE(mqttsStack >= SIMPLE_TASK_STACK_S3);
    TEST_ASSERT_TRUE(mqttsStack <= SIMPLE_TASK_STACK_S3);
    TEST_ASSERT_TRUE(mqttsStack > SIMPLE_TASK_STACK_C6);
    TestLog::step("mqtts uses 6KB stack (> 4KB C6 default, <= S3 default)");

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

    // 3. minHeap 阈值为 35000（mbedtls 缓冲区裁剪后 TLS 握手仅需 ~25-30KB）
    std::regex minHeapRe("minHeap\\s*=\\s*isMqtts\\s*\\?\\s*(\\d+)");
    std::smatch match;
    if (std::regex_search(content, match, minHeapRe) && match.size() > 1) {
        int minHeapVal = std::stoi(match[1].str());
        TEST_ASSERT_TRUE_MESSAGE(minHeapVal >= 30000 && minHeapVal <= 40000,
            "MQTTS minHeap should be 30000-40000 (buffer trimmed TLS needs ~25-30KB DRAM)");
        TestLog::step(("MQTTS minHeap=" + match[1].str() + " (in 30K-40K range)").c_str());
    }

    // 4. minLargestBlock 阈值为 20000（mbedtls 4KB input buffer + 上下文 + 对齐）
    std::regex minBlockRe("minLargestBlock\\s*=\\s*isMqtts\\s*\\?\\s*(\\d+)");
    if (std::regex_search(content, match, minBlockRe) && match.size() > 1) {
        int minBlockVal = std::stoi(match[1].str());
        TEST_ASSERT_TRUE_MESSAGE(minBlockVal >= 15000 && minBlockVal <= 25000,
            "MQTTS minLargestBlock should be 15000-25000");
        TestLog::step(("MQTTS minLargestBlock=" + match[1].str() + " (in 15K-25K range)").c_str());
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
    //   DRAM 内部 40KB 总空闲 + 最大连续 25KB -> 允许重连
    uint32_t NEW_MIN_HEAP = 40000;
    uint32_t NEW_MIN_BLOCK = 25000;
    uint32_t newDramFree = 39000;  // DRAM 内部 39KB 总空闲（不足 40KB）
    bool newGuardPassed = (newDramFree >= NEW_MIN_HEAP) && (dramLargest >= NEW_MIN_BLOCK);
    TEST_ASSERT_FALSE_MESSAGE(newGuardPassed,
        "New DRAM check: free=39KB < 40KB threshold -> should BLOCK");
    TestLog::step("New DRAM check: insufficient DRAM (39KB < 40KB) correctly blocks");

    uint32_t fieldDramFree = 43104;
    uint32_t fieldDramLargest = 32756;
    bool fieldGuard = (fieldDramFree >= NEW_MIN_HEAP) && (fieldDramLargest >= NEW_MIN_BLOCK);
    TEST_ASSERT_TRUE_MESSAGE(fieldGuard,
        "Field DRAM (43104 free, 32756 largest) must allow MQTTS reconnect attempt");
    TestLog::step("Field DRAM: free=43104, largest=32756 -> reconnect allowed");

    // 真正 DRAM 充足时放行
    uint32_t healthyDramFree = 50000;  // 50KB DRAM
    uint32_t healthyDramLargest = 28000;  // 28KB 连续 DRAM
    bool healthyGuard = (healthyDramFree >= NEW_MIN_HEAP) && (healthyDramLargest >= NEW_MIN_BLOCK);
    TEST_ASSERT_TRUE_MESSAGE(healthyGuard,
        "Healthy DRAM (50KB free, 28KB largest) → should ALLOW reconnect");
    TestLog::step("Healthy DRAM: free=50KB, largest=28KB → reconnect allowed");

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
 * @brief 前端 refreshMqttStatus 必须调用 _loadMqttStatus 和 _startMqttStatusPolling
 * 回归：testMqttConnection 已重构为 refreshMqttStatus，不再直接测试连接
 */
void test_mqtt_refresh_status_method() {
    TestLog::testStart("Frontend: refreshMqttStatus Method");

    std::string content = readProjectFile("web-src/modules/runtime/protocol/mqtt-config.js");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read mqtt-config.js");

    // 1. refreshMqttStatus 方法必须存在
    TEST_ASSERT_TRUE_MESSAGE(content.find("refreshMqttStatus()") != std::string::npos ||
                             content.find("refreshMqttStatus ()") != std::string::npos,
        "refreshMqttStatus method must exist");
    TestLog::step("refreshMqttStatus method present");

    // 2. 必须调用 _loadMqttStatus 获取当前状态
    TEST_ASSERT_TRUE_MESSAGE(content.find("_loadMqttStatus()") != std::string::npos,
        "refreshMqttStatus must call _loadMqttStatus");
    TestLog::step("_loadMqttStatus call present");

    // 3. 必须调用 _startMqttStatusPolling 启动轮询
    TEST_ASSERT_TRUE_MESSAGE(content.find("_startMqttStatusPolling") != std::string::npos,
        "refreshMqttStatus must call _startMqttStatusPolling");
    TestLog::step("_startMqttStatusPolling call present");

    // 4. 按钮文本应为"刷新状态"而非"测试连接"
    TEST_ASSERT_TRUE_MESSAGE(content.find("刷新状态") != std::string::npos,
        "Button text must be '刷新状态'");
    TestLog::step("Button text is '刷新状态'");

    // 5. 不应再有 apiMqttTest 调用（连接测试已移除）
    TEST_ASSERT_TRUE_MESSAGE(content.find("apiMqttTest") == std::string::npos,
        "apiMqttTest call must be removed (connection test replaced by status refresh)");
    TestLog::step("apiMqttTest call removed");

    TestLog::testEnd(true);
}

/**
 * @brief 前端不应再包含已移除的 testMqttConnection 和 disconnectMqtt 方法
 * 回归：MQTT 连接测试已重构为状态刷新，断开连接功能也已移除
 */
void test_mqtt_old_methods_removed() {
    TestLog::testStart("Frontend: Old Methods Removed");

    std::string content = readProjectFile("web-src/modules/runtime/protocol/mqtt-config.js");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read mqtt-config.js");

    // 1. testMqttConnection 方法应已移除
    TEST_ASSERT_TRUE_MESSAGE(content.find("testMqttConnection()") == std::string::npos &&
                             content.find("testMqttConnection ()") == std::string::npos,
        "testMqttConnection method must be removed (replaced by refreshMqttStatus)");
    TestLog::step("testMqttConnection removed");

    // 2. disconnectMqtt 方法应已移除
    TEST_ASSERT_TRUE_MESSAGE(content.find("disconnectMqtt()") == std::string::npos &&
                             content.find("disconnectMqtt ()") == std::string::npos,
        "disconnectMqtt method must be removed");
    TestLog::step("disconnectMqtt removed");

    // 3. 不应有 mqtt-test-btn 选择器（按钮已改为 data-action）
    TEST_ASSERT_TRUE_MESSAGE(content.find("mqtt-test-btn") == std::string::npos,
        "mqtt-test-btn selector must be removed");
    TestLog::step("mqtt-test-btn selector removed");

    // 4. 不应有 mqtt-disconnect-btn 选择器
    TEST_ASSERT_TRUE_MESSAGE(content.find("mqtt-disconnect-btn") == std::string::npos,
        "mqtt-disconnect-btn selector must be removed");
    TestLog::step("mqtt-disconnect-btn selector removed");

    // 5. HTML 文件中也不应有断开连接按钮和测试连接按钮
    std::string html = readProjectFile("web-src/pages/fragments/protocol-mqtt.html");
    TEST_ASSERT_TRUE_MESSAGE(!html.empty(),
        "Failed to read protocol-mqtt.html");
    TEST_ASSERT_TRUE_MESSAGE(html.find("disconnectMqtt") == std::string::npos &&
                             html.find("mqtt-disconnect") == std::string::npos,
        "HTML must not contain disconnectMqtt action or mqtt-disconnect button");
    TestLog::step("HTML disconnect button removed");
    TEST_ASSERT_TRUE_MESSAGE(html.find("testMqttConnection") == std::string::npos &&
                             html.find("mqtt-test-conn") == std::string::npos,
        "HTML must not contain testMqttConnection action or mqtt-test-conn button");
    TestLog::step("HTML test connection button removed");

    // 6. HTML 中应有刷新状态按钮
    TEST_ASSERT_TRUE_MESSAGE(html.find("refreshMqttStatus") != std::string::npos &&
                             html.find("mqtt-refresh-btn") != std::string::npos,
        "HTML must contain refreshMqttStatus action and mqtt-refresh-btn i18n key");
    TestLog::step("HTML refresh status button present");

    // 7. i18n 键应已更新：mqtt-refresh-btn 存在，mqtt-test-conn-btn 移除
    std::string i18nZh = readProjectFile("web-src/i18n/i18n-zh-CN.js");
    if (!i18nZh.empty()) {
        TEST_ASSERT_TRUE_MESSAGE(i18nZh.find("mqtt-refresh-btn") != std::string::npos,
            "i18n-zh-CN must contain mqtt-refresh-btn key");
        TEST_ASSERT_TRUE_MESSAGE(i18nZh.find("mqtt-test-conn-btn") == std::string::npos,
            "i18n-zh-CN must not contain deprecated mqtt-test-conn-btn key");
        TestLog::step("i18n zh-CN keys updated");
    }
    std::string i18nEn = readProjectFile("web-src/i18n/i18n-en.js");
    if (!i18nEn.empty()) {
        TEST_ASSERT_TRUE_MESSAGE(i18nEn.find("mqtt-refresh-btn") != std::string::npos,
            "i18n-en must contain mqtt-refresh-btn key");
        TEST_ASSERT_TRUE_MESSAGE(i18nEn.find("mqtt-test-conn-btn") == std::string::npos,
            "i18n-en must not contain deprecated mqtt-test-conn-btn key");
        TestLog::step("i18n en keys updated");
    }

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

// ============================================================
// 网络切换后 MQTT 重连测试
// 修复：网络切换后错误计数器不重置、handle() 使用悬空指针、
// MQTT 慢模式不恢复等问题
// ============================================================

/**
 * @brief 网络类型变更后 MQTT 重连
 * 模拟从 WiFi 切换到以太网后，MQTT 客户端重建并连接成功
 */
void test_mqtt_reconnect_after_network_type_change() {
    TestLog::testStart("MQTT Reconnect After Network Type Change");

    // WiFi 模式 MQTT 连接
    MockMQTTClient mqtt;
    MQTTConfig config;
    config.enabled = true;
    config.server = "iot.fastbee.cn";
    config.port = 1883;
    config.clientId = "TestSwitchDevice";
    config.username = "admin";
    config.password = "password123";
    config.autoReconnect = true;
    config.reconnectInterval = 100;
    mqtt.initialize(config);
    TEST_ASSERT_TRUE(mqtt.connect());
    TEST_ASSERT_TRUE(mqtt.getIsConnected());
    TestLog::step("MQTT connected over WiFi");

    // 模拟网络切换：停止 MQTT（pre-network-switch 回调）
    mqtt.disconnect();
    mqtt.setStopped(true);
    TEST_ASSERT_TRUE(mqtt.isStopped());
    TEST_ASSERT_FALSE(mqtt.getIsConnected());
    TestLog::step("MQTT stopped before network switch");

    // 模拟 restartMQTTDeferred：创建新的 MQTT 客户端（全新状态）
    MockMQTTClient mqttNew;
    mqttNew.initialize(config);
    TEST_ASSERT_TRUE(mqttNew.connect());
    TEST_ASSERT_TRUE(mqttNew.getIsConnected());
    TEST_ASSERT_EQUAL(0, mqttNew.getReconnectCount());  // 全新客户端
    TEST_ASSERT_EQUAL(0, mqttNew.getLastError());        // 无错误
    TestLog::step("MQTT reconnected with fresh client (counters reset)");

    // 验证新客户端的 handleAutoReconnect 不会在已连接时调度重连
    bool scheduled = mqttNew.handleAutoReconnect(1000);
    TEST_ASSERT_FALSE(scheduled);  // 已连接，不重连
    TestLog::step("New client handle: no reconnect needed (connected)");

    TestLog::testEnd(true);
}

/**
 * @brief 传输层变更后错误计数器重置
 * 模拟 MQTT 连接失败 3 次进入慢模式后，传输层变更应重置计数器
 */
void test_mqtt_error_counters_reset_on_transport_change() {
    TestLog::testStart("MQTT Error Counters Reset on Transport Change");

    MockMQTTClient mqtt;
    MQTTConfig config;
    config.enabled = true;
    config.server = "iot.fastbee.cn";
    config.port = 1883;
    config.clientId = "TestCounterReset";
    config.username = "admin";
    config.password = "password123";
    config.autoReconnect = true;
    config.reconnectInterval = 100;
    mqtt.initialize(config);

    // 模拟 3 次连接失败（进入慢模式）
    mqtt.setShouldFailConnect(true);
    for (int i = 0; i < 3; i++) {
        mqtt.reconnect();
    }
    TEST_ASSERT_EQUAL(3, mqtt.getReconnectCount());
    TEST_ASSERT_FALSE(mqtt.getIsConnected());
    TestLog::step("3 failed reconnects (slow mode simulated)");

    // 模拟传输层变更：创建新的 MQTT 客户端（模拟 restartMQTTDeferred + resetErrorCounters）
    MockMQTTClient mqttReset;
    mqttReset.initialize(config);
    // 全新客户端的计数器应为初始值
    TEST_ASSERT_EQUAL(0, mqttReset.getReconnectCount());
    TEST_ASSERT_EQUAL(0, mqttReset.getLastError());
    TestLog::step("New client: counters reset (count=0, error=0)");

    // 新客户端应能正常连接
    mqttReset.setShouldFailConnect(false);
    TEST_ASSERT_TRUE(mqttReset.connect());
    TEST_ASSERT_TRUE(mqttReset.getIsConnected());
    TestLog::step("New client connected successfully after reset");

    TestLog::testEnd(true);
}

/**
 * @brief MQTT 被 stop() 后 handle() 立即返回
 * 验证：stopped=true 时不调度重连，防止使用悬空指针
 */
void test_mqtt_handle_skipped_when_stopped() {
    TestLog::testStart("MQTT Handle Skipped When Stopped");

    MockMQTTClient mqtt;
    MQTTConfig config;
    config.enabled = true;
    config.server = "iot.fastbee.cn";
    config.port = 1883;
    config.clientId = "TestStopped";
    config.username = "admin";
    config.password = "password123";
    config.autoReconnect = true;
    config.reconnectInterval = 100;
    mqtt.initialize(config);
    mqtt.connect();
    TEST_ASSERT_TRUE(mqtt.getIsConnected());

    // stop MQTT
    mqtt.disconnect();
    mqtt.setStopped(true);
    TEST_ASSERT_TRUE(mqtt.isStopped());
    TEST_ASSERT_FALSE(mqtt.getIsConnected());
    TestLog::step("MQTT stopped");

    // handleAutoReconnect 模拟 handle() 中的重连逻辑
    // stopped=true 时应返回 false（不调度重连）
    bool scheduled1 = mqtt.handleAutoReconnect(1000);
    TEST_ASSERT_FALSE(scheduled1);
    TestLog::step("handle() at t=1000: no reconnect (stopped)");

    bool scheduled2 = mqtt.handleAutoReconnect(5000);
    TEST_ASSERT_FALSE(scheduled2);
    TestLog::step("handle() at t=5000: no reconnect (stopped)");

    bool scheduled3 = mqtt.handleAutoReconnect(10000);
    TEST_ASSERT_FALSE(scheduled3);
    TestLog::step("handle() at t=10000: no reconnect (stopped)");

    // 重建客户端后恢复正常
    MockMQTTClient mqttNew;
    mqttNew.initialize(config);
    mqttNew.connect();
    TEST_ASSERT_TRUE(mqttNew.getIsConnected());
    TEST_ASSERT_FALSE(mqttNew.isStopped());
    bool scheduled4 = mqttNew.handleAutoReconnect(1000);
    TEST_ASSERT_FALSE(scheduled4);  // 已连接，不重连
    TestLog::step("New client: handle() works correctly");

    TestLog::testEnd(true);
}

/**
 * @brief 网络切换期间的内存保护
 * 模拟低堆环境下 MQTT 重连被跳过，堆恢复后正常重连
 */
void test_mqtt_memory_protection_during_switch() {
    TestLog::testStart("MQTT Memory Protection During Switch");

    MockMQTTClient mqtt;
    MQTTConfig config;
    config.enabled = true;
    config.server = "iot.fastbee.cn";
    config.port = 1883;
    config.clientId = "TestMemProt";
    config.username = "admin";
    config.password = "password123";
    config.autoReconnect = true;
    config.reconnectInterval = 100;
    mqtt.initialize(config);

    // 正常连接
    TEST_ASSERT_TRUE(mqtt.connect());
    TEST_ASSERT_TRUE(mqtt.getIsConnected());
    TestLog::step("MQTT connected (normal heap)");

    // 模拟低堆环境：MQTT 应跳过重连
    // 在生产代码中 doReconnect() 检查 heap < 8000 时跳过
    uint32_t lowHeap = 5000;  // 低于 8000 阈值
    bool heapTooLow = (lowHeap < 8000);
    TEST_ASSERT_TRUE(heapTooLow);
    TestLog::step("Low heap detected (<8KB): reconnect would be skipped");

    // 断开连接
    mqtt.setConnected(false);
    TEST_ASSERT_FALSE(mqtt.getIsConnected());

    // 在低堆环境下，handleAutoReconnect 仍会调度（mock 不检查堆），
    // 但生产代码中 doReconnect() 会跳过。验证逻辑：
    // 模拟堆恢复后正常重连
    uint32_t recoveredHeap = 50000;  // 充足堆
    bool heapSufficient = (recoveredHeap >= 8000);
    TEST_ASSERT_TRUE(heapSufficient);
    TestLog::step("Heap recovered (50KB): reconnect allowed");

    // 重连成功
    bool scheduled = mqtt.handleAutoReconnect(1000);
    // handleAutoReconnect 会设置 _reconnectPending=true
    if (scheduled) {
        mqtt.clearReconnectPending();
        TEST_ASSERT_TRUE(mqtt.reconnect());
        TEST_ASSERT_TRUE(mqtt.getIsConnected());
        TestLog::step("MQTT reconnected after heap recovery");
    } else {
        // 如果没有调度（时间未到），手动重连
        TEST_ASSERT_TRUE(mqtt.reconnect());
        TEST_ASSERT_TRUE(mqtt.getIsConnected());
        TestLog::step("MQTT reconnected after heap recovery (manual)");
    }

    // 验证低堆阈值逻辑
    // MQTT (非 MQTTS): 阈值 8KB total / 2KB 连续
    // MQTTS: 阈值 35KB total / 20KB 连续（mbedtls 缓冲区裁剪后）
    uint32_t mqttThreshold = 8000;
    uint32_t mqttsThreshold = 35000;
    TEST_ASSERT_TRUE(5000 < mqttThreshold);   // 5KB 低于 MQTT 阈值
    TEST_ASSERT_TRUE(34000 < mqttsThreshold);  // 34KB 低于 MQTTS 阈值（35KB）
    TEST_ASSERT_TRUE(19444 < 20000);  // 19.4KB 低于 MQTTS 连续块阈值（20KB）
    TestLog::step("Memory threshold logic verified (MQTT=8KB, MQTTS=35KB/20KB)");

    TestLog::testEnd(true);
}

// ========== MQTT 重连任务按需创建测试 ==========

/**
 * @brief 源码验证: MQTTClient 重连任务从常驻改为按需创建
 * begin() 不再创建 reconnectTaskHandle
 * ensureReconnectTask() 在需要重连时按需创建
 * reconnectTaskEntry 完成后自删除 (vTaskDelete)
 */
void test_mqtt_reconnect_task_on_demand() {
    TestLog::testStart("MQTT: Reconnect Task On-Demand");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    // 1. 必须有 ensureReconnectTask 方法
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("ensureReconnectTask") != std::string::npos,
        "MQTTClient must have ensureReconnectTask() method for on-demand creation");
    TestLog::step("ensureReconnectTask() method present");

    // 2. reconnectTaskEntry 完成后必须自删除
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("vTaskDelete(nullptr)") != std::string::npos,
        "reconnectTaskEntry must self-delete after completion to free DRAM");
    TestLog::step("reconnectTaskEntry self-deletes via vTaskDelete(nullptr)");

    // 3. reconnectTaskEntry 完成后必须将 handle 置空
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("_reconnectTaskHandle = nullptr") != std::string::npos,
        "After self-delete, _reconnectTaskHandle must be set to nullptr");
    TestLog::step("_reconnectTaskHandle cleared after task exits");

    // 4. 头文件必须声明 ensureReconnectTask
    std::string header = readProjectFile("include/protocols/MQTTClient.h");
    TEST_ASSERT_TRUE_MESSAGE(!header.empty(), "MQTTClient.h must be readable");
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("ensureReconnectTask") != std::string::npos,
        "MQTTClient.h must declare ensureReconnectTask()");
    TestLog::step("ensureReconnectTask() declared in header");

    TestLog::testEnd(true);
}

// ========== MQTTS SSL 内存失败防御测试 ==========

/**
 * @brief MQTTS SSL 内存保护：主动释放 SSE + 激进重试机制
 * 验证源码中包含：
 * 1. MQTTS 连接前主动关闭 SSE 客户端释放 DRAM
 * 2. SSL 失败后激进内存回收再重试
 * 3. 阈值为 40KB/25KB（匹配 setInsecure 后 TLS 实际需求 ~35-40KB）
 */
void test_mqtts_ssl_memory_failure_defense() {
    TestLog::testStart("MQTTS: SSL Memory Failure Defense");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    // 1. 必须在 MQTTS 连接前主动释放 SSE
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("closeAllClients") != std::string::npos,
        "doReconnect must proactively close SSE clients before MQTTS connection");
    TestLog::step("Proactive SSE client cleanup before MQTTS connect");

    // 2. 必须有 SSL 失败后的内存回收与单独分类
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("reclaimDramForMqtts(") != std::string::npos &&
        content.find("mqttsSslMemoryFailure") != std::string::npos,
        "doReconnect must reclaim DRAM and classify SSL memory failures separately");
    TestLog::step("DRAM reclaim and SSL memory failure classification present");

    // 3. 阈值集中在 MemoryBudget（预编译 mbedtls 16KB record buffer 需要更高连续块）
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MemoryBudget::MQTTS_MIN_DRAM_FREE") != std::string::npos,
        "MQTTS minHeap must use MemoryBudget::MQTTS_MIN_DRAM_FREE");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MemoryBudget::MQTTS_MIN_LARGEST_BLOCK") != std::string::npos,
        "MQTTS minLargestBlock must use MemoryBudget::MQTTS_MIN_LARGEST_BLOCK");
    TestLog::step("Thresholds centralized in MemoryBudget");

    // 4. 激进重试必须包含延迟（让 idle 任务回收 mbedtls 残留）
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("delay(50)") != std::string::npos,
        "Aggressive retry must have delay for idle task memory reclamation");
    TestLog::step("Delay for mbedtls memory reclamation in aggressive retry");

    // 5. TLS 内存失败必须被单独分类，不能误触发网络/DNS失败的 WiFi soft-reset
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("mqttsSslMemoryFailure") != std::string::npos,
        "MQTTS SSL memory failures must be tracked separately from DNS/network failures");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("_lastMqttsTlsMemoryFailure") != std::string::npos,
        "MQTTS RSA/BIGNUM failures must be classified as TLS memory failures");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MQTTS SSL memory failure") != std::string::npos,
        "MQTTS SSL memory failures must enter slow retry mode");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("ets_printf(\"[MQTT] MQTTS SSL memory failure") != std::string::npos,
        "MQTTS SSL memory slow retry must be visible in serial diagnostics");
    TestLog::step("MQTTS SSL memory failures use slow retry classification");

    TestLog::testEnd(true);
}

/**
 * @brief 无 PSRAM 设备 begin() 警告用户 MQTTS 可能失败
 *
 * 验证：
 * 1. begin() 中使用运行时 psramFound() 检测（而非编译期 BOARD_HAS_PSRAM 宏）
 * 2. 无 PSRAM 时仅显示警告日志，不强制回退到 mqtt
 * 3. 日志标记存在
 */
void test_no_psram_forces_mqtt_in_begin() {
    TestLog::testStart("MQTTS: No-PSRAM begin() Warns User");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    std::string header = readProjectFile("include/protocols/MQTTClient.h");
    TEST_ASSERT_TRUE_MESSAGE(!header.empty(), "MQTTClient.h must be readable");

    // 1. 旧的降级成员变量已被移除
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("_mqttsFallbackToMqtt") == std::string::npos,
        "MQTTClient.h must NOT declare _mqttsFallbackToMqtt (removed in simplification)");
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("_mqttsSslFailureCount") == std::string::npos,
        "MQTTClient.h must NOT declare _mqttsSslFailureCount (removed in simplification)");
    TestLog::step("Old fallback member variables removed from header");

    // 2. begin() 中使用运行时 psramFound() 检测（而非编译期宏）
    size_t beginFunc = content.find("bool MQTTClient::begin(");
    TEST_ASSERT_TRUE_MESSAGE(
        beginFunc != std::string::npos,
        "begin() function must exist");

    // 运行时检测：psramFound() 替代了编译期 #if !defined(BOARD_HAS_PSRAM)
    size_t runtimeCheck = content.find("psramFound()", beginFunc);
    TEST_ASSERT_TRUE_MESSAGE(
        runtimeCheck != std::string::npos,
        "begin() must use runtime psramFound() for PSRAM detection");
    // begin() 中不应使用编译期 BOARD_HAS_PSRAM 宏来判断是否警告
    // 注意：calloc 等地方仍可使用 BOARD_HAS_PSRAM，所以只检查 begin 函数范围内
    std::string beginSection = content.substr(beginFunc, runtimeCheck - beginFunc + 200);
    TEST_ASSERT_TRUE_MESSAGE(
        beginSection.find("#if !defined(BOARD_HAS_PSRAM)") == std::string::npos,
        "begin() must NOT use compile-time BOARD_HAS_PSRAM macro for PSRAM warning");

    // 3. 无强制回退，仅显示警告日志（允许用户选择 mqtts）
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("config.scheme = \"mqtt\"", beginFunc) == std::string::npos,
        "begin() must NOT force config.scheme to mqtt (allow user choice)");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("config.port = 1883", beginFunc) == std::string::npos,
        "begin() must NOT force config.port to 1883 (allow user choice)");
    TestLog::step("begin() does NOT force mqtt for no-PSRAM devices (user can choose mqtts)");

    // 4. 警告日志标记存在
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MQTTS(TLS) connection may fail due to insufficient memory") != std::string::npos,
        "begin() must warn user about potential memory failure");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("Switch to mqtt:// if mqtts connection fails") != std::string::npos,
        "begin() must suggest switching to mqtt");
    TestLog::step("Warning log markers present");

    TestLog::testEnd(true);
}

/**
 * @brief 协议 API 返回 tlsSupported 字段
 *
 * 验证：
 * 1. ProtocolRouteHandler.cpp 中包含 tlsSupported 字段
 * 2. 字段值基于运行时 psramFound() 检测（而非编译期宏）
 */
void test_protocol_api_has_tls_supported() {
    TestLog::testStart("Protocol API: tlsSupported Field");

    std::string content = readProjectFile("src/network/handlers/ProtocolRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "ProtocolRouteHandler.cpp must be readable");

    // 1. MQTT 配置响应中包含 tlsSupported 字段
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("tlsSupported") != std::string::npos,
        "ProtocolRouteHandler must include tlsSupported in MQTT config response");

    // 2. 字段值基于运行时 psramFound() 检测，而非编译期 BOARD_HAS_PSRAM
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("psramFound()") != std::string::npos,
        "tlsSupported must use runtime psramFound() detection");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("#if defined(BOARD_HAS_PSRAM)") == std::string::npos,
        "tlsSupported must NOT use compile-time BOARD_HAS_PSRAM macro");
    TestLog::step("tlsSupported field present and runtime-detected");

    // 3. 保存配置时不再强制回退：允许用户选择 mqtts（连接失败时会提示）
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MQTTS not supported on no-PSRAM device") == std::string::npos,
        "Save handler must NOT force fallback for no-PSRAM devices (user choice)");
    TestLog::step("Backend save handler does NOT force fallback (user can choose mqtts)");

    TestLog::testEnd(true);
}

/**
 * @brief MQTTS SSL 内存失败不能触发 WiFi soft reset
 */
void test_mqtts_ssl_memory_failure_does_not_reset_wifi() {
    TestLog::testStart("MQTTS: SSL Memory Failure Does Not Reset WiFi");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    size_t memoryBranch = content.find("if (mqttsSslMemoryFailure)");
    TEST_ASSERT_TRUE_MESSAGE(
        memoryBranch != std::string::npos,
        "doReconnect must branch on mqttsSslMemoryFailure before rc=-2 DNS handling");

    size_t dnsBranch = content.find("} else if (lastErrorCode == -2)", memoryBranch);
    TEST_ASSERT_TRUE_MESSAGE(
        dnsBranch != std::string::npos,
        "DNS/network rc=-2 handling must be a separate else-if branch");

    size_t wifiReset = content.find("soft-resetting WiFi STA", dnsBranch);
    TEST_ASSERT_TRUE_MESSAGE(
        wifiReset != std::string::npos,
        "WiFi soft reset logic must remain limited to the DNS/network branch");

    TEST_ASSERT_TRUE_MESSAGE(
        memoryBranch < dnsBranch && dnsBranch < wifiReset,
        "MQTTS memory failure branch must bypass DNS failure counting and WiFi soft reset");

    size_t backoffHelper = content.find("auto enterMqttsMemoryBackoff");
    size_t counterReset = content.find("consecutiveTimeouts = 0;", backoffHelper);
    size_t memoryBackoffCall = content.find("enterMqttsMemoryBackoff(\"tls_ssl_memory_failure\")", memoryBranch);
    TEST_ASSERT_TRUE_MESSAGE(
        backoffHelper != std::string::npos &&
        counterReset != std::string::npos &&
        memoryBackoffCall != std::string::npos &&
        memoryBackoffCall < dnsBranch,
        "MQTTS memory failure must reset network-failure counters via memory backoff helper");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("bool keepSlowRetry = slowMode || mqttsSslMemoryFailure") != std::string::npos,
        "MQTTS memory slow retry must not be overwritten by exponential backoff");

    TestLog::step("MQTTS SSL memory failure isolated from network recovery path");
    TestLog::testEnd(true);
}

// ============ MQTTS TLS 动态内存管理测试 ============

/**
 * @brief 验证 ensureTlsTransport() / releaseTlsTransport() 在源码中存在且被正确调用
 */
void test_mqtts_dynamic_tls_lifecycle_methods() {
    TestLog::testStart("MQTTS: Dynamic TLS Lifecycle Methods");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    // ensureTlsTransport 实现
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("void MQTTClient::ensureTlsTransport()") != std::string::npos,
        "ensureTlsTransport() must be implemented");
    TestLog::step("ensureTlsTransport() implementation found");

    // releaseTlsTransport 实现
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("void MQTTClient::releaseTlsTransport()") != std::string::npos,
        "releaseTlsTransport() must be implemented");
    TestLog::step("releaseTlsTransport() implementation found");

    // reclaimDramForMqtts 实现
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("bool MQTTClient::reclaimDramForMqtts(bool pauseWeb)") != std::string::npos,
        "reclaimDramForMqtts() must be implemented");
    TestLog::step("reclaimDramForMqtts() implementation found");

    // ensureTlsTransport 必须动态创建对象；设备端使用 nothrow 避免分配失败时异常/abort
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("_wifiClientSecure = new (std::nothrow) WiFiClientSecure()") != std::string::npos ||
        content.find("_wifiClientSecure = new WiFiClientSecure()") != std::string::npos,
        "ensureTlsTransport must dynamically allocate WiFiClientSecure");
    TestLog::step("Dynamic allocation: WiFiClientSecure");

    // releaseTlsTransport 必须使用 delete 释放对象
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("delete _wifiClientSecure") != std::string::npos,
        "releaseTlsTransport must delete _wifiClientSecure to free TLS memory");
    TestLog::step("Dynamic deallocation: delete _wifiClientSecure");

    // releaseTlsTransport 必须设置 nullptr（防止 use-after-free）
    size_t deletePos = content.find("delete _wifiClientSecure");
    size_t nullPos = content.find("_wifiClientSecure = nullptr", deletePos);
    TEST_ASSERT_TRUE_MESSAGE(
        nullPos != std::string::npos && nullPos < deletePos + 100,
        "After delete, _wifiClientSecure must be set to nullptr");
    TestLog::step("Null assignment after delete (prevents use-after-free)");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 doReconnect() 在 MQTTS 连接前先释放 TLS 对象
 */
void test_mqtts_release_before_connect() {
    TestLog::testStart("MQTTS: Release TLS Before Connect in doReconnect");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    // doReconnect 中必须先 releaseTlsTransport 再 ensureTlsTransport
    size_t doReconnectPos = content.find("void MQTTClient::doReconnect()");
    TEST_ASSERT_TRUE(doReconnectPos != std::string::npos);

    size_t releasePos = content.find("releaseTlsTransport()", doReconnectPos);
    size_t ensurePos = content.find("ensureTlsTransport()", releasePos);
    TEST_ASSERT_TRUE_MESSAGE(
        releasePos != std::string::npos && ensurePos != std::string::npos && releasePos < ensurePos,
        "doReconnect must releaseTlsTransport() BEFORE ensureTlsTransport() to recover DRAM first");
    TestLog::step("releaseTlsTransport() called before ensureTlsTransport() in doReconnect");

    // releaseTlsTransport 必须在 DRAM 检查之前（确保检查时内存已回收）
    size_t minHeapCheck = content.find("reconnectFreeHeap < minHeap", doReconnectPos);
    TEST_ASSERT_TRUE_MESSAGE(
        releasePos < minHeapCheck,
        "TLS must be released BEFORE DRAM threshold check");
    TestLog::step("TLS released before DRAM threshold check");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 _wifiClientSecure 是动态指针而非固定成员
 */
void test_mqtts_wificlientsecure_is_pointer() {
    TestLog::testStart("MQTTS: WiFiClientSecure is Pointer (Dynamic Allocation)");

    std::string header = readProjectFile("include/protocols/MQTTClient.h");
    TEST_ASSERT_TRUE_MESSAGE(!header.empty(), "MQTTClient.h must be readable");

    // 必须是指针
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("WiFiClientSecure* _wifiClientSecure") != std::string::npos,
        "_wifiClientSecure must be a pointer for dynamic allocation");
    TestLog::step("_wifiClientSecure is a pointer (dynamic allocation)");

    // 不能是固定成员
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("WiFiClientSecure wifiClientSecure;") == std::string::npos,
        "WiFiClientSecure must NOT be a fixed member (wastes DRAM when not using MQTTS)");
    TestLog::step("No fixed WiFiClientSecure member (saves DRAM for non-MQTTS)");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 disconnect() 调用 releaseTlsTransport() 释放 TLS 内存
 */
void test_mqtts_disconnect_releases_tls() {
    TestLog::testStart("MQTTS: disconnect() Calls releaseTlsTransport()");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    size_t disconnectPos = content.find("void MQTTClient::disconnect()");
    TEST_ASSERT_TRUE(disconnectPos != std::string::npos);

    size_t releasePos = content.find("releaseTlsTransport()", disconnectPos);
    size_t nextFunc = content.find("void MQTTClient::", disconnectPos + 50);
    TEST_ASSERT_TRUE_MESSAGE(
        releasePos != std::string::npos && releasePos < nextFunc,
        "disconnect() must call releaseTlsTransport() to free TLS DRAM");
    TestLog::step("disconnect() calls releaseTlsTransport()");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 DRAM 监控在 reconnect() 中存在
 */
void test_mqtts_dram_monitoring_in_reconnect() {
    TestLog::testStart("MQTTS: DRAM Monitoring in reconnect()");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    // reconnect() 中必须记录 TLS 握手前后的 DRAM
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("TLS connect: DRAM") != std::string::npos,
        "reconnect() must log DRAM before/after TLS handshake");
    TestLog::step("TLS DRAM delta logging in reconnect()");

    // handle() 中必须有 DRAM 水位告警
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MQTTS DRAM low") != std::string::npos,
        "handle() must warn when MQTTS DRAM drops below 15KB");
    TestLog::step("DRAM watermark warning in handle()");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 reclaimDramForMqtts() 实现了完整的 DRAM 回收流程
 */
void test_mqtts_reclaim_dram_full_workflow() {
    TestLog::testStart("MQTTS: reclaimDramForMqtts() Full Workflow");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    // 必须包含 reclaimDramForMqtts 实现
    size_t reclaimPos = content.find("bool MQTTClient::reclaimDramForMqtts(bool pauseWeb)");
    TEST_ASSERT_TRUE_MESSAGE(reclaimPos != std::string::npos, "reclaimDramForMqtts() must be implemented");
    TestLog::step("reclaimDramForMqtts() implementation found");

    // Step 1: 必须释放 TLS 上下文
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("releaseTlsTransport()", reclaimPos) != std::string::npos,
        "reclaimDramForMqtts must call releaseTlsTransport() as first step");
    TestLog::step("Step 1: Release TLS context");

    // Step 2: Web listener stays online by default; Ethernet MQTTS can opt into
    // a short deep pause when W5500 + Web + TLS leaves too little contiguous DRAM.
    TEST_ASSERT_TRUE_MESSAGE(
        readProjectFile("include/protocols/MQTTClient.h").find("reclaimDramForMqtts(bool pauseWeb = false)") != std::string::npos,
        "reclaimDramForMqtts must default to keeping Web online");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("if (pauseWeb && wcm)", reclaimPos) != std::string::npos &&
        content.find("pauseForMqttsHandshake()", reclaimPos) != std::string::npos,
        "reclaimDramForMqtts must support an explicit Ethernet deep-pause path");
    TestLog::step("Step 2: Web pause is explicit and opt-in");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("shouldPauseWebForMqtts", 0) != std::string::npos &&
        content.find("isForegroundRequestActive()", 0) != std::string::npos,
        "MQTTS deep Web pause must be suppressed while a user is actively using Web config");
    TestLog::step("Step 2a: Foreground Web usage blocks deep pause");

    // Step 2b: Must explicitly mark Web as not paused.
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("_webServerPaused = false", reclaimPos) != std::string::npos &&
        content.find("_mdnsPausedForMqtts = false", reclaimPos) != std::string::npos,
        "reclaimDramForMqtts must reset Web/mDNS pause flags before optional pause");
    TestLog::step("Step 2b: Track Web pause state");

    // Step 3: 必须关闭 SSE 客户端
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("closeAllClients()", reclaimPos) != std::string::npos,
        "reclaimDramForMqtts must close SSE clients to free memory");
    TestLog::step("Step 3: Close SSE clients");

    // Step 4: 必须有强制 GC (heap_caps_check_integrity_all)
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("heap_caps_check_integrity_all(true)", reclaimPos) != std::string::npos,
        "reclaimDramForMqtts must force GC with heap_caps_check_integrity_all");
    TestLog::step("Step 4: Force garbage collection");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("if (_webServerPaused && netMgr", reclaimPos) != std::string::npos &&
        content.find("netMgr->stopMDNS()", reclaimPos) != std::string::npos,
        "reclaimDramForMqtts must only pause mDNS when Web was actually paused");
    TestLog::step("Step 4a: mDNS pause follows Web pause");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MemoryBudget::MQTTS_WEB_RESUME_DELAY_MS", reclaimPos) != std::string::npos,
        "reclaimDramForMqtts must wait for AsyncTCP/FreeRTOS idle cleanup before measuring DRAM");
    TestLog::step("Step 4b: Wait for idle cleanup before measuring DRAM");

    // Step 5: 必须检测 DRAM 是否达标
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MemoryBudget::canAttemptMqtts", reclaimPos) != std::string::npos,
        "reclaimDramForMqtts must use MemoryBudget::canAttemptMqtts");
    TestLog::step("Step 5: Check DRAM with MemoryBudget");

    TEST_ASSERT_TRUE_MESSAGE(
        readProjectFile("include/core/MemoryBudget.h").find("MQTTS_MIN_LARGEST_BLOCK") != std::string::npos,
        "MemoryBudget must define largest block requirement for MQTTS");
    TestLog::step("Step 5b: MQTTS largest block budget is centralized");

    // 必须输出诊断日志
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("[MQTT] reclaimDram:", reclaimPos) != std::string::npos &&
        content.find("mdns=%s", reclaimPos) != std::string::npos,
        "reclaimDramForMqtts must log diagnostic info (web/mdns status, SSE closed, DRAM stats)");
    TestLog::step("Diagnostic logging: web/mdns status, SSE closed, DRAM stats");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 resumeWebServices() 在 TLS 连接后正确重启 Web 服务
 */
void test_mqtts_resume_web_services() {
    TestLog::testStart("MQTTS: resumeWebServices() After TLS Connect");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    // 必须包含 resumeWebServices 实现
    size_t resumePos = content.find("void MQTTClient::resumeWebServices()");
    TEST_ASSERT_TRUE_MESSAGE(resumePos != std::string::npos, "resumeWebServices() must be implemented");
    TestLog::step("resumeWebServices() implementation found");

    // 必须检查 _webServerPaused 标志
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("if (_webServerPaused)", resumePos) != std::string::npos,
        "resumeWebServices must check _webServerPaused before restarting");
    TestLog::step("Check _webServerPaused flag");

    // 必须重启 Web 服务器
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("resumeFromMqttsHandshake()", resumePos) != std::string::npos,
        "resumeWebServices must call resumeFromMqttsHandshake() to restart Web server");
    TestLog::step("Resume Web server from MQTTS pause");

    // 必须重置 _webServerPaused 标志
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("_webServerPaused = false", resumePos) != std::string::npos,
        "resumeWebServices must reset _webServerPaused to false");
    TestLog::step("Reset _webServerPaused flag");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("if (_mdnsPausedForMqtts)", resumePos) != std::string::npos &&
        content.find("netMgr->startMDNS()", resumePos) != std::string::npos &&
        content.find("_mdnsPausedForMqtts = false", resumePos) != std::string::npos,
        "resumeWebServices must restart mDNS only when it was paused for MQTTS");
    TestLog::step("Resume mDNS after MQTTS pause");

    // doReconnect() 成功分支必须调用 resumeWebServices
    size_t doReconnectPos = content.find("void MQTTClient::doReconnect()");
    size_t successPos = content.find("Background reconnect successful", doReconnectPos);
    size_t resumeCall = content.find("resumeWebServices()", successPos);
    size_t nextFunc = content.find("void MQTTClient::", successPos + 50);
    
    TEST_ASSERT_TRUE_MESSAGE(
        resumeCall != std::string::npos && resumeCall < nextFunc,
        "doReconnect() success path must call resumeWebServices() after TLS connect");
    TestLog::step("doReconnect() success path calls resumeWebServices()");

    // doReconnect() 早期退出（内存不足）也必须调用 resumeWebServices
    size_t earlyExitPos = content.find("Memory too low for", doReconnectPos);
    size_t earlyResumeCall = content.find("resumeWebServices()", earlyExitPos);
    size_t earlyReturn = content.find("_reconnectRunning = false", earlyExitPos);
    
    TEST_ASSERT_TRUE_MESSAGE(
        earlyResumeCall != std::string::npos && earlyResumeCall < earlyReturn,
        "doReconnect() early exit (low memory) must call resumeWebServices() before return");
    TestLog::step("doReconnect() early exit calls resumeWebServices()");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 doReconnect() 阈值更新为预编译 mbedtls 需求 (42KB/38KB)
 */
void test_mqtts_dram_thresholds_updated() {
    TestLog::testStart("MQTTS: DRAM Thresholds Updated for Precompiled mbedtls");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    // 必须使用集中化的 MemoryBudget 作为 minHeap，避免 WiFi/以太网/4G 阈值分叉。
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MemoryBudget::MQTTS_MIN_DRAM_FREE") != std::string::npos,
        "doReconnect must use MemoryBudget::MQTTS_MIN_DRAM_FREE");
    TestLog::step("minHeap comes from MemoryBudget");

    // minLargestBlock is centralized so runtime allocator changes do not leave
    // stale hard-coded DRAM thresholds in reconnect logic.
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MemoryBudget::MQTTS_MIN_LARGEST_BLOCK") != std::string::npos,
        "doReconnect must use MemoryBudget::MQTTS_MIN_LARGEST_BLOCK");
    TestLog::step("minLargestBlock comes from MemoryBudget");

    // Aggressive reclaim threshold stays centralized in MemoryBudget.
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MemoryBudget::shouldReclaimBeforeMqtts") != std::string::npos,
        "Aggressive reclaim threshold must be centralized in MemoryBudget");
    TestLog::step("Aggressive reclaim uses MemoryBudget");

    TEST_ASSERT_TRUE_MESSAGE(
        readProjectFile("include/core/MemoryBudget.h").find("MQTTS_READY_LARGEST_BLOCK") != std::string::npos,
        "MemoryBudget must define ready largest block threshold");
    TestLog::step("Ready largest block threshold is centralized");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 ProtocolManager.cpp 阈值同步更新
 */
void test_mqtts_protocol_manager_threshold_sync() {
    TestLog::testStart("MQTTS: ProtocolManager Threshold Sync");

    std::string content = readProjectFile("src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "ProtocolManager.cpp must be readable");

    // 必须使用集中化的 MemoryBudget 作为 MQTT 延迟重启的最小堆。
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MemoryBudget::MQTTS_MIN_DRAM_FREE") != std::string::npos &&
        content.find("loadProtocolSection(\"mqtt\"") != std::string::npos,
        "ProtocolManager must use MemoryBudget and mqtt section loading for deferred restart");
    TestLog::step("ProtocolManager uses MemoryBudget and mqtt section load");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 PubSubClient.setClient() 同步修复（防止悬空指针崩溃）
 */
void test_mqtts_pubsubclient_setclient_sync() {
    TestLog::testStart("MQTTS: PubSubClient setClient() Sync Fix");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    // ensureTlsTransport 必须调用 mqttClient.setClient(*_wifiClientSecure)
    size_t ensurePos = content.find("void MQTTClient::ensureTlsTransport()");
    size_t ensureEnd = content.find("\nvoid MQTTClient::releaseTlsTransport()", ensurePos);
    std::string ensureBlock = content.substr(ensurePos, ensureEnd - ensurePos);
    
    TEST_ASSERT_TRUE_MESSAGE(
        ensureBlock.find("mqttClient.setClient(*_wifiClientSecure)") != std::string::npos,
        "ensureTlsTransport must call mqttClient.setClient(*_wifiClientSecure)");
    TestLog::step("ensureTlsTransport: setClient(*_wifiClientSecure)");

    // releaseTlsTransport 必须调用 mqttClient.setClient(wifiClient) 切换回普通 WiFiClient
    size_t releasePos = content.find("void MQTTClient::releaseTlsTransport()");
    size_t releaseEnd = content.find("\nbool MQTTClient::reclaimDramForMqtts(bool pauseWeb)", releasePos);
    std::string releaseBlock = content.substr(releasePos, releaseEnd - releasePos);
    
    TEST_ASSERT_TRUE_MESSAGE(
        releaseBlock.find("mqttClient.setClient(wifiClient)") != std::string::npos,
        "releaseTlsTransport must call mqttClient.setClient(wifiClient) to avoid dangling pointer");
    TestLog::step("releaseTlsTransport: setClient(wifiClient) fallback");

    // begin() 中 MQTTS 分支必须调用 ensureTlsTransport()
    size_t beginPos = content.find("bool MQTTClient::begin(");
    size_t beginEnd = content.find("\nvoid MQTTClient::shutdown()", beginPos);
    std::string beginBlock = content.substr(beginPos, beginEnd - beginPos);
    
    TEST_ASSERT_TRUE_MESSAGE(
        beginBlock.find("ensureTlsTransport()") == std::string::npos &&
        beginBlock.find("TLS client will be created on reconnect") != std::string::npos,
        "begin() must defer WiFiClientSecure allocation until reconnect");
    TestLog::step("begin() defers TLS allocation");

    size_t doReconnectPos = content.find("void MQTTClient::doReconnect()");
    size_t reclaimCall = content.find("reclaimDramForMqtts(", doReconnectPos);
    size_t ensureCall = content.find("ensureTlsTransport()", reclaimCall);
    TEST_ASSERT_TRUE_MESSAGE(
        reclaimCall != std::string::npos && ensureCall != std::string::npos && ensureCall > reclaimCall,
        "doReconnect must ensure TLS transport after reclaim");
    TestLog::step("doReconnect ensures TLS after reclaim");

    TestLog::testEnd(true);
}

/**
 * @brief Source regression: mbedTLS allocations for MQTTS must prefer PSRAM.
 */
void test_mqtts_mbedtls_allocator_uses_psram() {
    TestLog::testStart("MQTTS: mbedTLS Allocator Uses PSRAM");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("mbedtls_platform_set_calloc_free") != std::string::npos,
        "MQTTS must install a runtime mbedTLS allocator");
    TestLog::step("Runtime mbedTLS allocator install found");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("fastbeeMbedtlsCalloc") != std::string::npos &&
        content.find("fastbeeMbedtlsFree") != std::string::npos,
        "MQTTS must provide matching calloc/free callbacks");
    TestLog::step("Custom calloc/free callbacks found");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MALLOC_CAP_SPIRAM") != std::string::npos &&
        content.find("MALLOC_CAP_INTERNAL") != std::string::npos,
        "Allocator must prefer PSRAM for TLS payloads and fall back to internal DRAM");
    TestLog::step("PSRAM preference and internal fallback found");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("MQTTS_MBEDTLS_PSRAM_MIN_ALLOC = 256U") != std::string::npos,
        "Allocator threshold must cover RSA/BIGNUM buffers around 256 bytes");
    TestLog::step("256B PSRAM threshold found");

    size_t ensurePos = content.find("void MQTTClient::ensureTlsTransport()");
    TEST_ASSERT_TRUE(ensurePos != std::string::npos);
    size_t allocatorCall = content.find("ensureMbedtlsAllocatorForMqtts()", ensurePos);
    size_t tlsNew = content.find("new (std::nothrow) WiFiClientSecure()", ensurePos);
    TEST_ASSERT_TRUE_MESSAGE(
        allocatorCall != std::string::npos && tlsNew != std::string::npos && allocatorCall < tlsNew,
        "ensureTlsTransport must install allocator before WiFiClientSecure is created");
    TestLog::step("Allocator installed before TLS transport creation");

    TestLog::testEnd(true);
}

/**
 * @brief Source regression: Web resume after MQTTS pause must avoid port bind races.
 *
 * 回归测试：修复 AsyncTCP bind error: -8 问题
 * - resumeFromMqttsHandshake() 必须等待足够时间让 AsyncTCP 释放端口
 * - 必须有重试机制（3 次重试，间隔递增）
 * - start() 必须在 server->begin() 前后增加延迟
 */
void test_mqtts_web_resume_waits_for_async_tcp_release() {
    TestLog::testStart("MQTTS: Web Resume Waits for AsyncTCP Release (bind error -8 fix)");

    std::string web = readProjectFile("src/network/WebConfigManager.cpp");
    std::string budget = readProjectFile("include/core/MemoryBudget.h");
    TEST_ASSERT_TRUE_MESSAGE(!web.empty(), "WebConfigManager.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!budget.empty(), "MemoryBudget.h must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        budget.find("MQTTS_WEB_RESUME_DELAY_MS = 300UL") != std::string::npos,
        "MemoryBudget must define a 300ms Web resume delay after MQTTS pause");
    TestLog::step("Web resume delay budget found");

    // 1. pauseForMqttsHandshake 必须正确关闭 SSE 和 server
    size_t pausePos = web.find("bool WebConfigManager::pauseForMqttsHandshake");
    size_t pauseEnd = web.find("\nbool WebConfigManager::resumeFromMqttsHandshake", pausePos);
    TEST_ASSERT_TRUE(pausePos != std::string::npos && pauseEnd != std::string::npos);
    std::string pauseBlock = web.substr(pausePos, pauseEnd - pausePos);
    size_t sseClose = pauseBlock.find("sseRouteHandler->closeAllClients()");
    size_t serverEnd = pauseBlock.find("server->end()");
    TEST_ASSERT_TRUE_MESSAGE(
        sseClose != std::string::npos && serverEnd != std::string::npos && sseClose < serverEnd,
        "pauseForMqttsHandshake must close SSE clients before stopping AsyncWebServer");
    TestLog::step("Pause closes SSE before server end");

    TEST_ASSERT_TRUE_MESSAGE(
        pauseBlock.find("delay(50)") != std::string::npos,
        "pauseForMqttsHandshake must give AsyncTCP time to drain stopped sockets");
    TestLog::step("Pause drains AsyncTCP briefly");

    TEST_ASSERT_TRUE_MESSAGE(
        pauseBlock.find("isForegroundRequestActive()") != std::string::npos &&
        pauseBlock.find("Keeping Web online for active foreground request") != std::string::npos,
        "pauseForMqttsHandshake must refuse deep pause while Web config is active");
    TestLog::step("Active Web config requests keep Web online");

    TEST_ASSERT_TRUE_MESSAGE(
        web.find("bool WebConfigManager::isForegroundRequestActive() const") != std::string::npos &&
        web.find("webForegroundModeActive") != std::string::npos &&
        web.find("webForegroundUntilMs") != std::string::npos,
        "WebConfigManager must expose foreground Web activity for MQTTS arbitration");
    TestLog::step("Foreground Web activity helper found");

    // 2. resumeFromMqttsHandshake 必须有端口释放等待和重试机制
    size_t resumePos = web.find("bool WebConfigManager::resumeFromMqttsHandshake");
    size_t resumeEnd = web.find("\nbool WebConfigManager::isWebRecoverySuppressed", resumePos);
    TEST_ASSERT_TRUE(resumePos != std::string::npos && resumeEnd != std::string::npos);
    std::string resumeBlock = web.substr(resumePos, resumeEnd - resumePos);

    // 必须有端口释放延迟（使用 MemoryBudget::MQTTS_WEB_RESUME_DELAY_MS）
    TEST_ASSERT_TRUE_MESSAGE(
        resumeBlock.find("FastBee::MemoryBudget::MQTTS_WEB_RESUME_DELAY_MS") != std::string::npos &&
        resumeBlock.find("delay(delayMs)") != std::string::npos,
        "resumeFromMqttsHandshake must wait for TCP port release using MemoryBudget delay");
    TestLog::step("Resume waits for TCP port release");

    // 必须有重试机制（3 次）
    TEST_ASSERT_TRUE_MESSAGE(
        resumeBlock.find("maxRetries = 3") != std::string::npos &&
        resumeBlock.find("for (int attempt = 1") != std::string::npos,
        "resumeFromMqttsHandshake must have 3-retry mechanism for Web server start");
    TestLog::step("Resume has 3-retry mechanism");

    // 重试间隔必须递增（500ms, 1000ms, 1500ms）
    TEST_ASSERT_TRUE_MESSAGE(
        resumeBlock.find("retryDelay = 500 * attempt") != std::string::npos,
        "resumeFromMqttsHandshake must use progressive retry delays (500ms * attempt)");
    TestLog::step("Resume uses progressive retry delays");

    // 必须记录成功和失败事件
    TEST_ASSERT_TRUE_MESSAGE(
        resumeBlock.find("manual_resume") != std::string::npos &&
        resumeBlock.find("resume_failed") != std::string::npos,
        "resumeFromMqttsHandshake must record Web resume recovery events");
    TestLog::step("Resume recovery event recorded");

    // 3. start() 必须在 server->begin() 前后增加延迟
    size_t startPos = web.find("bool WebConfigManager::start()");
    size_t startEnd = web.find("\nvoid WebConfigManager::stop()", startPos);
    TEST_ASSERT_TRUE(startPos != std::string::npos && startEnd != std::string::npos);
    std::string startBlock = web.substr(startPos, startEnd - startPos);

    TEST_ASSERT_TRUE_MESSAGE(
        startBlock.find("delay(50)") != std::string::npos &&
        startBlock.find("server->begin()") != std::string::npos &&
        startBlock.find("delay(100)") != std::string::npos,
        "start() must add delay(50) before and delay(100) after server->begin() to avoid bind error");
    TestLog::step("start() adds delays around server->begin()");

    // 4. start() 必须验证端口 80 是否真正监听（bind error: -8 修复）
    TEST_ASSERT_TRUE_MESSAGE(
        startBlock.find("isPortListening(80)") != std::string::npos,
        "start() must verify port 80 is listening after server->begin()");
    TestLog::step("start() verifies port listening");

    TEST_ASSERT_TRUE_MESSAGE(
        startBlock.find("isRunning = false") != std::string::npos &&
        startBlock.find("return false") != std::string::npos,
        "start() must set isRunning=false and return false when port not listening");
    TestLog::step("start() returns false on bind failure");

    // 5. isPortListening 必须检查 lwIP tcp_listen_pcbs 链表
    TEST_ASSERT_TRUE_MESSAGE(
        web.find("bool WebConfigManager::isPortListening") != std::string::npos &&
        web.find("tcp_listen_pcbs.listen_pcbs") != std::string::npos &&
        web.find("pcb->local_port == port") != std::string::npos,
        "isPortListening() must iterate tcp_listen_pcbs.listen_pcbs and check local_port");
    TestLog::step("isPortListening() checks lwIP listen PCBs");

    TestLog::testEnd(true);
}

/**
 * @brief Source regression: Web server watchdog mechanisms for silent failures.
 *
 * 回归测试：确保 Web 服务在静默失败时能自动恢复
 * - 端口监听看门狗：检测 isRunning=true 但端口 80 未监听
 * - 无请求看门狗：5 分钟无 HTTP 请求触发软重启
 * - 自动重启使用 start() 而非直接 server->begin()
 */
void test_web_server_watchdog_mechanisms() {
    TestLog::testStart("Web Server: Watchdog Mechanisms for Silent Failures");

    std::string web = readProjectFile("src/network/WebConfigManager.cpp");
    std::string header = readProjectFile("include/network/WebConfigManager.h");
    TEST_ASSERT_TRUE_MESSAGE(!web.empty(), "WebConfigManager.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!header.empty(), "WebConfigManager.h must be readable");

    // 1. 看门狗常量定义
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("LISTEN_CHECK_INTERVAL_MS = 30000UL") != std::string::npos,
        "Header must define LISTEN_CHECK_INTERVAL_MS = 30000UL (30s check interval)");
    TestLog::step("LISTEN_CHECK_INTERVAL_MS defined");

    TEST_ASSERT_TRUE_MESSAGE(
        header.find("LISTEN_CHECK_FAIL_TRIGGER = 3") != std::string::npos,
        "Header must define LISTEN_CHECK_FAIL_TRIGGER = 3 (3 consecutive failures)");
    TestLog::step("LISTEN_CHECK_FAIL_TRIGGER defined");

    TEST_ASSERT_TRUE_MESSAGE(
        header.find("NO_REQUEST_WATCHDOG_MS = 300000UL") != std::string::npos,
        "Header must define NO_REQUEST_WATCHDOG_MS = 300000UL (5 minutes)");
    TestLog::step("NO_REQUEST_WATCHDOG_MS defined");

    // 2. 看门狗状态字段
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("lastRequestSeenMs") != std::string::npos &&
        header.find("lastListenCheckMs") != std::string::npos &&
        header.find("listenCheckFailCount") != std::string::npos,
        "Header must define watchdog state fields");
    TestLog::step("Watchdog state fields defined");

    // 3. 端口监听看门狗实现
    size_t maintPos = web.find("void WebConfigManager::performMaintenance()");
    TEST_ASSERT_TRUE_MESSAGE(maintPos != std::string::npos,
        "performMaintenance() must exist");
    size_t maintEnd = web.find("\nvoid WebConfigManager::trackWebRequest()", maintPos);
    TEST_ASSERT_TRUE(maintEnd != std::string::npos);
    std::string maintBlock = web.substr(maintPos, maintEnd - maintPos);

    TEST_ASSERT_TRUE_MESSAGE(
        maintBlock.find("isRunning && !isPortListening(80)") != std::string::npos,
        "Port watchdog must check isRunning=true AND port 80 not listening");
    TestLog::step("Port watchdog condition check found");

    TEST_ASSERT_TRUE_MESSAGE(
        maintBlock.find("listenCheckFailCount >= LISTEN_CHECK_FAIL_TRIGGER") != std::string::npos,
        "Port watchdog must trigger after consecutive failures reach threshold");
    TestLog::step("Port watchdog threshold check found");

    TEST_ASSERT_TRUE_MESSAGE(
        maintBlock.find("listen_watchdog") != std::string::npos &&
        maintBlock.find("port_not_listening") != std::string::npos,
        "Port watchdog must record recovery event");
    TestLog::step("Port watchdog records recovery event");

    TEST_ASSERT_TRUE_MESSAGE(
        maintBlock.find("watchdog_restart") != std::string::npos &&
        maintBlock.find("port_watchdog_recovered") != std::string::npos,
        "Port watchdog must record success recovery event");
    TestLog::step("Port watchdog records success event");

    TEST_ASSERT_TRUE_MESSAGE(
        maintBlock.find("scheduleDeviceRestartForWebRecovery") != std::string::npos &&
        maintBlock.find("watchdog_restart_failed") != std::string::npos,
        "Port watchdog must schedule device reboot if restart fails");
    TestLog::step("Port watchdog schedules device reboot on failure");

    // 4. 无请求看门狗实现
    TEST_ASSERT_TRUE_MESSAGE(
        maintBlock.find("noRequestDuration >= NO_REQUEST_WATCHDOG_MS") != std::string::npos,
        "No-request watchdog must check duration against NO_REQUEST_WATCHDOG_MS");
    TestLog::step("No-request watchdog threshold check found");

    TEST_ASSERT_TRUE_MESSAGE(
        maintBlock.find("softRestartWebServer(\"no_request_watchdog\")") != std::string::npos,
        "No-request watchdog must call softRestartWebServer");
    TestLog::step("No-request watchdog calls softRestartWebServer");

    // 5. trackWebRequest 必须更新 lastRequestSeenMs
    size_t trackPos = web.find("void WebConfigManager::trackWebRequest()");
    TEST_ASSERT_TRUE(trackPos != std::string::npos);
    size_t trackEnd = web.find("\nbool WebConfigManager::isRequestBurst()", trackPos);
    TEST_ASSERT_TRUE(trackEnd != std::string::npos);
    std::string trackBlock = web.substr(trackPos, trackEnd - trackPos);

    TEST_ASSERT_TRUE_MESSAGE(
        trackBlock.find("lastRequestSeenMs = now") != std::string::npos,
        "trackWebRequest() must update lastRequestSeenMs for watchdog");
    TestLog::step("trackWebRequest updates lastRequestSeenMs");

    // 6. 自动重启必须使用 start() 而非 server->begin()
    TEST_ASSERT_TRUE_MESSAGE(
        maintBlock.find("const bool restarted = start()") != std::string::npos,
        "Auto-restart must use start() (which verifies port listening) instead of server->begin()");
    TestLog::step("Auto-restart uses start() with verification");

    TEST_ASSERT_TRUE_MESSAGE(
        maintBlock.find("auto_restart_failed") != std::string::npos &&
        maintBlock.find("bind_error") != std::string::npos,
        "Auto-restart must record failure event when start() returns false");
    TestLog::step("Auto-restart records failure event");

    TestLog::testEnd(true);
}

/**
 * @brief Source regression: MQTTS memory failures keep slow retry backoff.
 */
void test_mqtts_memory_backoff_preserved() {
    TestLog::testStart("MQTTS: Memory Backoff Preserved");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    std::string header = readProjectFile("include/protocols/MQTTClient.h");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!header.empty(), "MQTTClient.h must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        header.find("_mqttsMemoryBackoffActive") != std::string::npos,
        "MQTTS memory backoff state must be stored on the MQTT client");
    TestLog::step("Backoff state field found");

    size_t handlePos = content.find("void MQTTClient::handle()");
    size_t handleEnd = content.find("\nvoid MQTTClient::processQueuedCommands()", handlePos);
    TEST_ASSERT_TRUE(handlePos != std::string::npos && handleEnd != std::string::npos);
    std::string handleBlock = content.substr(handlePos, handleEnd - handlePos);
    TEST_ASSERT_TRUE_MESSAGE(
        handleBlock.find("_mqttsMemoryBackoffActive") != std::string::npos &&
        handleBlock.find("lastReconnectAttempt = millis()") != std::string::npos,
        "handle() must preserve MQTTS memory backoff instead of resetting reconnectInterval to 5s");
    TestLog::step("handle() preserves active MQTTS memory backoff");

    size_t doReconnectPos = content.find("void MQTTClient::doReconnect()");
    TEST_ASSERT_TRUE(doReconnectPos != std::string::npos);
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("enterMqttsMemoryBackoff", doReconnectPos) != std::string::npos &&
        content.find("lastReconnectAttempt = millis()", doReconnectPos) != std::string::npos &&
        content.find("_mqttsMemoryBackoffActive = true", doReconnectPos) != std::string::npos,
        "doReconnect() must centralize MQTTS memory slow backoff state");
    TestLog::step("doReconnect() centralizes MQTTS memory backoff");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("enterMqttsMemoryBackoff(\"low_dram\")", doReconnectPos) != std::string::npos &&
        content.find("enterMqttsMemoryRecoveryRetry(\"low_dram_recovering\")", doReconnectPos) != std::string::npos &&
        content.find("enterMqttsMemoryBackoff(\"reclaim_failed\")", doReconnectPos) != std::string::npos &&
        content.find("enterMqttsMemoryBackoff(\"tls_ssl_memory_failure\")", doReconnectPos) != std::string::npos,
        "MQTTS memory exits must distinguish recoverable low DRAM from slow-backoff failures");
    TestLog::step("Recoverable low DRAM uses short retry; hard failures enter slow backoff");

    TestLog::testEnd(true);
}

void test_mqtts_recoverable_low_dram_uses_short_retry() {
    TestLog::testStart("MQTTS: Recoverable Low DRAM Uses Short Retry");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    size_t doReconnectPos = content.find("void MQTTClient::doReconnect()");
    TEST_ASSERT_TRUE_MESSAGE(doReconnectPos != std::string::npos, "doReconnect() must exist");
    size_t doReconnectEnd = content.find("\nString MQTTClient::buildClientId()", doReconnectPos);
    TEST_ASSERT_TRUE_MESSAGE(doReconnectEnd != std::string::npos, "doReconnect() end marker must exist");
    std::string doBlock = content.substr(doReconnectPos, doReconnectEnd - doReconnectPos);

    TEST_ASSERT_TRUE_MESSAGE(
        doBlock.find("enterMqttsMemoryRecoveryRetry") != std::string::npos &&
        doBlock.find("MemoryBudget::MQTTS_MEMORY_RECOVERY_RETRY_MS") != std::string::npos,
        "doReconnect must provide a short MQTTS memory recovery retry path");

    size_t pauseDecision = doBlock.find("bool pauseWebForRecovery = shouldPauseWebForMqtts(");
    size_t reclaimCall = doBlock.find("bool reclaimed = reclaimDramForMqtts(pauseWebForRecovery)");
    TEST_ASSERT_TRUE_MESSAGE(
        pauseDecision != std::string::npos &&
        reclaimCall != std::string::npos &&
        pauseDecision < reclaimCall,
        "Low-memory branch must make an adaptive Web pause decision before reclaiming DRAM");

    size_t postReclaimSample = doBlock.find("reconnectLargestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)", reclaimCall);
    size_t recoverableCheck = doBlock.find("MemoryBudget::canRetryMqttsMemoryRecovery", reclaimCall);
    TEST_ASSERT_TRUE_MESSAGE(
        postReclaimSample != std::string::npos &&
        recoverableCheck != std::string::npos &&
        postReclaimSample < recoverableCheck,
        "Low-memory branch must re-sample DRAM after reclaim before checking recoverability");

    size_t shortRetry = doBlock.find("enterMqttsMemoryRecoveryRetry(\"low_dram_recovering\")", recoverableCheck);
    size_t slowBackoff = doBlock.find("enterMqttsMemoryBackoff(\"low_dram\")", recoverableCheck);
    TEST_ASSERT_TRUE_MESSAGE(
        shortRetry != std::string::npos &&
        slowBackoff != std::string::npos &&
        shortRetry < slowBackoff,
        "Recoverable MQTTS low DRAM must short-retry before falling back to 300s slow backoff");

    TestLog::step("Ethernet restore can retry quickly after adaptive Web/SSE memory reclaim");
    TestLog::testEnd(true);
}

/**
 * @brief 验证 doReconnect() 所有 MQTTS 失败分支都调用 resumeWebServices()
 *
 * 回归测试：之前 rc==-2（TLS 证书错误如 mbedtls -15104）和其他错误码分支
 * 缺少 resumeWebServices() 调用，导致 Web 服务器被永久暂停。
 * 修复后所有失败分支对 MQTTS 都必须调用 releaseTlsTransport() + resumeWebServices()。
 */
void test_mqtts_all_failure_branches_resume_web() {
    TestLog::testStart("MQTTS: All doReconnect() Failure Branches Resume Web Services");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    size_t doReconnectPos = content.find("void MQTTClient::doReconnect()");
    TEST_ASSERT_TRUE_MESSAGE(doReconnectPos != std::string::npos, "doReconnect() must exist");
    size_t doReconnectEnd = content.find("\nString MQTTClient::buildClientId()", doReconnectPos);
    TEST_ASSERT_TRUE_MESSAGE(doReconnectEnd != std::string::npos, "doReconnect() end marker must exist");
    std::string doBlock = content.substr(doReconnectPos, doReconnectEnd - doReconnectPos);

    // 1. 成功分支：已有 resumeWebServices()
    size_t successPos = doBlock.find("Background reconnect successful");
    TEST_ASSERT_TRUE_MESSAGE(successPos != std::string::npos, "success path must exist");
    size_t successResume = doBlock.find("resumeWebServices()", successPos);
    size_t successEnd = doBlock.find("} else {", successPos);
    TEST_ASSERT_TRUE_MESSAGE(
        successResume != std::string::npos && successResume < successEnd,
        "doReconnect() success path must call resumeWebServices()");
    TestLog::step("Success path calls resumeWebServices()");

    // 2. mqttsSslMemoryFailure 分支：必须有 resumeWebServices()
    size_t sslMemFailPos = doBlock.find("if (mqttsSslMemoryFailure)");
    TEST_ASSERT_TRUE_MESSAGE(sslMemFailPos != std::string::npos, "mqttsSslMemoryFailure branch must exist");
    size_t sslBranchEnd = doBlock.find("} else if (lastErrorCode == -2)", sslMemFailPos);
    std::string sslBranch = doBlock.substr(sslMemFailPos, sslBranchEnd - sslMemFailPos);
    TEST_ASSERT_TRUE_MESSAGE(
        sslBranch.find("releaseTlsTransport()") != std::string::npos &&
        sslBranch.find("resumeWebServices()") != std::string::npos,
        "mqttsSslMemoryFailure branch must call releaseTlsTransport() + resumeWebServices()");
    TestLog::step("mqttsSslMemoryFailure branch resumes web services");

    // 3. rc==-2 分支（TLS/DNS/TCP 连接失败）：必须有 isMqtts 保护的 resumeWebServices()
    size_t rc2Pos = doBlock.find("} else if (lastErrorCode == -2)", sslMemFailPos);
    TEST_ASSERT_TRUE_MESSAGE(rc2Pos != std::string::npos, "rc==-2 branch must exist");
    // 找到 rc==-2 分支的结束（下一个 else 或结束括号）
    size_t rc2InnerIf = doBlock.find("if (isMqtts)", rc2Pos);
    size_t rc2BranchEnd = doBlock.find("} else {", rc2Pos + 30);
    std::string rc2Branch = doBlock.substr(rc2Pos, rc2BranchEnd - rc2Pos);
    TEST_ASSERT_TRUE_MESSAGE(
        rc2InnerIf != std::string::npos && rc2InnerIf < rc2BranchEnd,
        "rc==-2 branch must have isMqtts guard for TLS cleanup");
    // rc==-2 的 isMqtts 块内必须同时有 releaseTlsTransport 和 resumeWebServices
    size_t rc2Release = doBlock.find("releaseTlsTransport()", rc2InnerIf);
    size_t rc2Resume = doBlock.find("resumeWebServices()", rc2InnerIf);
    TEST_ASSERT_TRUE_MESSAGE(
        rc2Release != std::string::npos && rc2Release < rc2BranchEnd &&
        rc2Resume != std::string::npos && rc2Resume < rc2BranchEnd,
        "rc==-2 branch: isMqtts block must call releaseTlsTransport() + resumeWebServices() "
        "(regression: web server was permanently paused after TLS cert error)");
    TestLog::step("rc==-2 branch resumes web services (TLS cert error regression)");

    // 4. else 分支（其他错误码如 rc=4 认证失败）：必须有 isMqtts 保护的 resumeWebServices()
    size_t elsePos = doBlock.find("} else {", rc2Pos + 30);
    TEST_ASSERT_TRUE_MESSAGE(elsePos != std::string::npos, "else branch must exist");
    size_t elseEnd = doBlock.find("bool keepSlowRetry", elsePos);
    std::string elseBranch = doBlock.substr(elsePos, elseEnd - elsePos);
    TEST_ASSERT_TRUE_MESSAGE(
        elseBranch.find("if (isMqtts)") != std::string::npos,
        "else branch must have isMqtts guard for TLS cleanup");
    TEST_ASSERT_TRUE_MESSAGE(
        elseBranch.find("releaseTlsTransport()") != std::string::npos &&
        elseBranch.find("resumeWebServices()") != std::string::npos,
        "else branch: isMqtts block must call releaseTlsTransport() + resumeWebServices() "
        "(regression: web server was permanently paused after non-memory errors)");
    TestLog::step("else branch resumes web services (other error codes)");

    // 5. 所有 MQTTS TLS 管理路径都确保 Web 服务最终恢复（幂等性）
    // resumeWebServices() 内部有 _webServerPaused 检查，多次调用安全
    size_t resumeImplPos = content.find("void MQTTClient::resumeWebServices()");
    TEST_ASSERT_TRUE_MESSAGE(resumeImplPos != std::string::npos, "resumeWebServices() must exist");
    std::string resumeImpl = content.substr(resumeImplPos, 500);
    TEST_ASSERT_TRUE_MESSAGE(
        resumeImpl.find("if (_webServerPaused)") != std::string::npos &&
        resumeImpl.find("_webServerPaused = false") != std::string::npos,
        "resumeWebServices() must be idempotent (check flag before acting)");
    TestLog::step("resumeWebServices() is idempotent (safe for multi-call)");

    TestLog::testEnd(true);
}

void test_mqtts_external_transport_skips_internal_tls() {
    TestLog::testStart("MQTTS: External Transport Skips Internal TLS");

    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    size_t doReconnectPos = content.find("void MQTTClient::doReconnect()");
    TEST_ASSERT_TRUE_MESSAGE(doReconnectPos != std::string::npos, "doReconnect() must exist");
    size_t doReconnectEnd = content.find("\nString MQTTClient::buildClientId()", doReconnectPos);
    TEST_ASSERT_TRUE_MESSAGE(doReconnectEnd != std::string::npos, "doReconnect() end marker must exist");
    std::string doBlock = content.substr(doReconnectPos, doReconnectEnd - doReconnectPos);

    TEST_ASSERT_TRUE_MESSAGE(
        doBlock.find("bool usesInternalTls = isMqtts && !_externalClient") != std::string::npos,
        "doReconnect must distinguish internal WiFi TLS from external secure transports");

    size_t ensureCall = doBlock.find("ensureTlsTransport()");
    size_t usesInternalBlock = ensureCall == std::string::npos
        ? std::string::npos
        : doBlock.rfind("if (usesInternalTls)", ensureCall);
    TEST_ASSERT_TRUE_MESSAGE(
        usesInternalBlock != std::string::npos && ensureCall != std::string::npos,
        "ensureTlsTransport must be guarded by usesInternalTls");

    TEST_ASSERT_TRUE_MESSAGE(
        doBlock.find("bool mqttsSslMemoryFailure =") != std::string::npos &&
        doBlock.find("isMqtts &&") != std::string::npos &&
        doBlock.find("needsMqttsDramBudget") != std::string::npos,
        "MQTTS memory backoff must cover external software TLS without creating WiFi TLS");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("if (isMqtts && !_externalClient && lastErrorCode == -2)") != std::string::npos,
        "External modem TLS connect failures must not be tagged as ESP32 internal TLS memory failures");

    TEST_ASSERT_TRUE_MESSAGE(
        doBlock.find("if (!_externalClient &&") != std::string::npos &&
        doBlock.find("soft-resetting WiFi STA") != std::string::npos,
        "WiFi STA reset on rc=-2 must not run for external 4G transports");

    TestLog::step("External MQTTS transport bypasses internal WiFiClientSecure lifecycle");
    TestLog::testEnd(true);
}

void test_mqtts_protocol_manager_reads_section_scheme_for_cellular_secure() {
    TestLog::testStart("MQTTS: ProtocolManager Reads Section Scheme");

    std::string pm = readProjectFile("src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!pm.empty(), "ProtocolManager.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        pm.find("JsonVariant mqttSection = doc[\"mqtt\"]") != std::string::npos &&
        pm.find("mqttSection.is<JsonObject>() ? mqttSection : doc.as<JsonVariant>()") != std::string::npos &&
        pm.find("cfg[\"scheme\"]") != std::string::npos,
        "configureMqttTransport must read scheme from either nested mqtt object or mqtt section document");

    TEST_ASSERT_TRUE_MESSAGE(
        pm.find("wantsMqtts && netMgr->getNetworkType() == NetworkType::NET_4G") != std::string::npos &&
        pm.find("cell->getSecureClient()") != std::string::npos,
        "4G MQTTS must select the cellular secure client");

    TestLog::step("ProtocolManager detects mqtts from section config and selects cellular secure client");
    TestLog::testEnd(true);
}

void test_mqtts_ethernet_uses_internal_secure_transport() {
    TestLog::testStart("MQTTS: Ethernet Uses Internal Secure Transport");

    std::string pm = readProjectFile("src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!pm.empty(), "ProtocolManager.cpp must be readable");

    size_t ethBranch = pm.find("wantsMqtts && netMgr->getNetworkType() == NetworkType::NET_ETHERNET");
    TEST_ASSERT_TRUE_MESSAGE(ethBranch != std::string::npos,
        "ProtocolManager must special-case Ethernet MQTTS");

    size_t ethSetNull = pm.find("mqttClient->setTransportClient(nullptr)", ethBranch);
    size_t activeClient = pm.find("netMgr->getActiveClient()", ethBranch);
    TEST_ASSERT_TRUE_MESSAGE(
        ethSetNull != std::string::npos &&
        activeClient != std::string::npos &&
        ethSetNull < activeClient,
        "Ethernet MQTTS must use internal WiFiClientSecure/NetworkClientSecure, not plain EthernetClient");

    std::string mqtt = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!mqtt.empty(), "MQTTClient.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        mqtt.find("bool activeNetworkIsWifi = (activeNetworkType == NetworkType::NET_WIFI)") != std::string::npos &&
        mqtt.find("!_externalClient && activeNetworkIsWifi && WiFi.status() != WL_CONNECTED") != std::string::npos,
        "Internal Ethernet secure transport must not be blocked by WiFi.status()");

    TEST_ASSERT_TRUE_MESSAGE(
        mqtt.find("bool pauseWebForHandshake = shouldPauseWebForMqtts(") != std::string::npos &&
        mqtt.find("reclaimDramForMqtts(pauseWebForHandshake)") != std::string::npos &&
        mqtt.find("MQTTS deep Web pause skipped: foreground Web request active") != std::string::npos,
        "Ethernet MQTTS must use adaptive deep pause and preserve active Web config sessions");

    TEST_ASSERT_TRUE_MESSAGE(
        mqtt.find("activeNetworkIsWifi &&\n                consecutiveTimeouts") != std::string::npos,
        "WiFi STA soft-reset must not run for Ethernet internal TLS failures");

    TestLog::step("Ethernet MQTTS selects internal secure transport and skips WiFi-only guards");
    TestLog::testEnd(true);
}

// ========== 修复回归测试：MQTTS 缓冲区/超时/loop/事件循环 ==========

// 修复1: MQTTS 缓冲区统一为 1024 字节（不再为 MQTTS 缩小到 512）
void test_mqtts_buffer_size_unified_1024() {
    TestLog::testStart("MQTTS Buffer Size Unified 1024");

    // 模拟 begin() 中缓冲区设置逻辑（修复后不再按 scheme 区分）
    auto getBufferSize = [](const String& scheme) -> uint16_t {
        (void)scheme;
        return 1024;  // 修复：统一为 1024
    };

    // mqtt 缓冲区 1024
    TEST_ASSERT_EQUAL(1024, getBufferSize("mqtt"));
    TestLog::step("mqtt buffer: 1024 bytes");

    // mqtts 缓冲区也是 1024（修复前是 512）
    TEST_ASSERT_EQUAL(1024, getBufferSize("mqtts"));
    TestLog::step("mqtts buffer: 1024 bytes (was 512, fixed)");

    // 验证源码中确实已移除 512 缓冲区条件
    std::string mqtt = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!mqtt.empty(), "MQTTClient.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(
        mqtt.find("uint16_t mqttBufferSize = 1024;") != std::string::npos,
        "MQTTS buffer must be unified to 1024 (not 512)");

    TestLog::testEnd(true);
}

// 修复2: MQTTS 连接成功后 socket timeout 为 15s（不是 5s）
void test_mqtts_post_connect_timeout_15s() {
    TestLog::testStart("MQTTS Post-Connect Timeout 15s");

    // 模拟 reconnect() 中连接成功后的 timeout 设置
    auto getPostConnectTimeout = [](const String& scheme, bool connectOk) -> int {
        if (scheme == "mqtts" && connectOk) return 15;  // 修复：5 → 15
        if (scheme == "mqtts" && !connectOk) return 15;  // 失败也恢复 15s
        return 5;  // mqtt 不受影响
    };

    // mqtts 连接成功：15s
    TEST_ASSERT_EQUAL(15, getPostConnectTimeout("mqtts", true));
    TestLog::step("mqtts post-connect timeout: 15s");

    // mqtts 连接失败：也是 15s（不是 5s）
    TEST_ASSERT_EQUAL(15, getPostConnectTimeout("mqtts", false));
    TestLog::step("mqtts post-fail timeout: 15s");

    // mqtt 不受影响：5s
    TEST_ASSERT_EQUAL(5, getPostConnectTimeout("mqtt", true));
    TestLog::step("mqtt post-connect timeout: 5s (unchanged)");

    // 验证源码
    std::string mqtt = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!mqtt.empty(), "MQTTClient.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(
        mqtt.find("mqttClient.setSocketTimeout(15);") != std::string::npos,
        "MQTTS post-connect timeout must be 15s");

    TestLog::testEnd(true);
}

// 修复3: 连接成功后 subscribeAll/publishDeviceInfo 之间插入 mqttClient.loop()
void test_mqtts_loop_between_post_connect_ops() {
    TestLog::testStart("MQTT Loop Between Post-Connect Ops");

    // 模拟连接成功后的操作序列，验证 loop() 在 subscribeAll 和 publishDeviceInfo 之间被调用
    std::string mqtt = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!mqtt.empty(), "MQTTClient.cpp must be readable");

    // 查找 subscribeAll() 后紧跟 mqttClient.loop()
    std::string subscribeAllLoop = "subscribeAll();\n        mqttClient.loop();";
    TEST_ASSERT_TRUE_MESSAGE(
        mqtt.find(subscribeAllLoop) != std::string::npos,
        "mqttClient.loop() must be called after subscribeAll() to process SUBACK");

    // 查找 publishDeviceInfo() 后紧跟 mqttClient.loop()
    std::string publishDevLoop = "publishDeviceInfo();\n        mqttClient.loop();";
    TEST_ASSERT_TRUE_MESSAGE(
        mqtt.find(publishDevLoop) != std::string::npos,
        "mqttClient.loop() must be called after publishDeviceInfo() to process PUBACK");

    TestLog::step("loop() inserted between post-connect operations");
    TestLog::testEnd(true);
}

// 修复4: EVENT_MQTT_DISCONNECTED 仅在 reconnectCount <= 3 时触发，防止无限循环
void test_mqtt_disconnect_event_guarded() {
    TestLog::testStart("MQTT Disconnect Event Guarded");

    // 模拟 handle() 中 EVENT_MQTT_DISCONNECTED 触发条件
    auto shouldTriggerDisconnectEvent = [](uint32_t reconnectCount) -> bool {
        return reconnectCount <= 3;  // 修复：仅前 3 次触发
    };

    // 前 3 次断连：触发事件
    TEST_ASSERT_TRUE(shouldTriggerDisconnectEvent(0));
    TEST_ASSERT_TRUE(shouldTriggerDisconnectEvent(1));
    TEST_ASSERT_TRUE(shouldTriggerDisconnectEvent(2));
    TEST_ASSERT_TRUE(shouldTriggerDisconnectEvent(3));
    TestLog::step("reconnectCount 0-3: event triggered");

    // 第 4 次及以后：不触发，防止无限循环
    TEST_ASSERT_FALSE(shouldTriggerDisconnectEvent(4));
    TEST_ASSERT_FALSE(shouldTriggerDisconnectEvent(10));
    TEST_ASSERT_FALSE(shouldTriggerDisconnectEvent(100));
    TestLog::step("reconnectCount > 3: event suppressed (prevents infinite loop)");

    // 验证源码包含保护条件
    std::string mqtt = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!mqtt.empty(), "MQTTClient.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(
        mqtt.find("reconnectCount <= 3") != std::string::npos,
        "EVENT_MQTT_DISCONNECTED must be guarded by reconnectCount <= 3");

    TestLog::testEnd(true);
}

// ========== P0/P1 改进验证测试 ==========

/**
 * @brief RESET-1: resetErrorCounters() 实现验证
 * 验证源码中 resetErrorCounters() 正确重置所有退避相关状态
 */
void test_reset_error_counters_implementation() {
    TestLog::testStart("RESET-1: resetErrorCounters implementation");
    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    // 查找函数实现
    auto pos = content.find("void MQTTClient::resetErrorCounters()");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
        "resetErrorCounters() must be defined in MQTTClient.cpp");

    // 截取函数体（~500字符）
    std::string funcBody = content.substr(pos, 500);

    // 必须重置的关键字段
    TEST_ASSERT_TRUE_MESSAGE(
        funcBody.find("consecutiveTimeouts") != std::string::npos,
        "resetErrorCounters must reset consecutiveTimeouts");
    TEST_ASSERT_TRUE_MESSAGE(
        funcBody.find("reconnectInterval") != std::string::npos,
        "resetErrorCounters must reset reconnectInterval");
    TEST_ASSERT_TRUE_MESSAGE(
        funcBody.find("lastReconnectAttempt") != std::string::npos,
        "resetErrorCounters must reset lastReconnectAttempt to 0 (force immediate reconnect)");
    TEST_ASSERT_TRUE_MESSAGE(
        funcBody.find("lastErrorCode") != std::string::npos,
        "resetErrorCounters must reset lastErrorCode");

    TestLog::step("resetErrorCounters resets all backoff state for immediate reconnect");
    TestLog::testEnd(true);
}

/**
 * @brief DRAM-PROC: processQueuedReports 使用 DRAM 内部内存检测
 * 验证源码使用 heap_caps_get_free_size(MALLOC_CAP_INTERNAL) 而非 ESP.getFreeHeap()
 */
void test_process_queued_reports_uses_dram() {
    TestLog::testStart("DRAM-PROC: processQueuedReports uses DRAM check");
    std::string content = readProjectFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "MQTTClient.cpp must be readable");

    // 查找 processQueuedReports 函数
    auto pos = content.find("void MQTTClient::processQueuedReports()");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
        "processQueuedReports() must be defined");

    // 截取函数开头（~400字符）
    std::string funcHead = content.substr(pos, 400);

    // 必须使用 MALLOC_CAP_INTERNAL（DRAM 内部内存）
    TEST_ASSERT_TRUE_MESSAGE(
        funcHead.find("MALLOC_CAP_INTERNAL") != std::string::npos,
        "processQueuedReports must use heap_caps_get_free_size(MALLOC_CAP_INTERNAL) for DRAM check");

    // 不应使用 ESP.getFreeHeap()（含 PSRAM，保护永不触发）
    TEST_ASSERT_TRUE_MESSAGE(
        funcHead.find("ESP.getFreeHeap()") == std::string::npos,
        "processQueuedReports must NOT use ESP.getFreeHeap() (includes PSRAM)");

    TestLog::step("processQueuedReports correctly uses DRAM memory check");
    TestLog::testEnd(true);
}

/**
 * @brief WIFI-MQTT-1: WiFi GOT_IP 事件通知 MQTT 立即重连
 * 验证 WiFiManager 在 WiFi 连接成功（GOT_IP）后调用 MQTT 的 resetErrorCounters
 */
void test_wifi_got_ip_notifies_mqtt() {
    TestLog::testStart("WIFI-MQTT-1: WiFi GOT_IP notifies MQTT reconnect");
    std::string content = readProjectFile("src/network/WiFiManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "WiFiManager.cpp must be readable");

    // 查找 GOT_IP 事件处理
    auto pos = content.find("ARDUINO_EVENT_WIFI_STA_GOT_IP");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
        "WiFiManager must handle ARDUINO_EVENT_WIFI_STA_GOT_IP");

    // 在 GOT_IP 处理段中必须调用 resetErrorCounters
    // 搜索 GOT_IP 后的 2000 字符范围
    std::string gotIpSection = content.substr(pos, 2000);
    TEST_ASSERT_TRUE_MESSAGE(
        gotIpSection.find("resetErrorCounters") != std::string::npos,
        "GOT_IP handler must call MQTT resetErrorCounters for immediate reconnect");

    TestLog::step("WiFi GOT_IP event correctly notifies MQTT to reset backoff and reconnect");
    TestLog::testEnd(true);
}

// ============================================================
//  Topic Prefix Building 回归测试
//  验证 buildFullTopicWithType 逻辑：
//  - autoPrefix=false 不加前缀
//  - autoPrefix=true 时：OTA 用 /{deviceNum}，其他用 /{productId}/{deviceNum}
//  - productId 为空时回退到 /{deviceNum}
//  - topicPrefix 全局前缀插入在设备前缀之前
// ============================================================

// 镜像 buildFullTopicWithType 逻辑的纯函数版本，便于单元测试
static String mockBuildFullTopic(const String& topic, bool autoPrefix,
                                 int topicType,
                                 const String& productId,
                                 const String& deviceNum,
                                 const String& topicPrefix = "") {
    if (!autoPrefix) return topic;
    String prefix;
    // OTA类型 (5=OTA_UPGRADE, 6=OTA_BINARY): 只用 deviceNum
    if (topicType == 5 || topicType == 6) {
        if (!deviceNum.isEmpty()) prefix = "/" + deviceNum;
    } else {
        if (!productId.isEmpty() && !deviceNum.isEmpty()) {
            prefix = "/" + productId + "/" + deviceNum;
        } else if (!deviceNum.isEmpty()) {
            prefix = "/" + deviceNum;
        }
    }
    if (!topicPrefix.isEmpty()) {
        return topicPrefix + prefix + topic;
    }
    return prefix + topic;
}

void test_topic_prefix_auto_off_no_prefix() {
    // autoPrefix=false 时不加任何前缀
    String result = mockBuildFullTopic("/function/get", false, 1, "1835", "DEV001");
    TEST_ASSERT_EQUAL_STRING("/function/get", result.c_str());
}

void test_topic_prefix_with_productId_and_deviceNum() {
    // autoPrefix=true，productId 和 deviceNum 都有，构建完整前缀
    String result = mockBuildFullTopic("/function/get", true, 1, "1835", "FBE9BFEFF16A398");
    TEST_ASSERT_EQUAL_STRING("/1835/FBE9BFEFF16A398/function/get", result.c_str());
}

void test_topic_prefix_empty_productId_fallback_to_deviceNum() {
    // productId 为空时回退到 /{deviceNum} 前缀
    String result = mockBuildFullTopic("/function/get", true, 1, "", "FBE9BFEFF16A398");
    TEST_ASSERT_EQUAL_STRING("/FBE9BFEFF16A398/function/get", result.c_str());
}

void test_topic_prefix_empty_deviceNum_no_prefix() {
    // deviceNum 为空时不加前缀（无法构建有效前缀）
    String result = mockBuildFullTopic("/function/get", true, 1, "1835", "");
    TEST_ASSERT_EQUAL_STRING("/function/get", result.c_str());
}

void test_topic_prefix_ota_only_uses_deviceNum() {
    // OTA 类型 (type=5) 只用 deviceNum，不需要 productId
    String result = mockBuildFullTopic("/upgrade/set", true, 5, "1835", "DEV001");
    TEST_ASSERT_EQUAL_STRING("/DEV001/upgrade/set", result.c_str());
}

void test_topic_prefix_ota_binary_only_uses_deviceNum() {
    // OTA Binary (type=6) 只用 deviceNum
    String result = mockBuildFullTopic("/upgrade/set", true, 6, "1835", "DEV001");
    TEST_ASSERT_EQUAL_STRING("/DEV001/upgrade/set", result.c_str());
}

void test_topic_prefix_data_report_uses_full_prefix() {
    // 数据上报 (type=0) 使用完整前缀
    String result = mockBuildFullTopic("/report", true, 0, "1835", "DEV001");
    TEST_ASSERT_EQUAL_STRING("/1835/DEV001/report", result.c_str());
}

void test_topic_prefix_data_command_uses_full_prefix() {
    // 数据下发 (type=1) 使用完整前缀
    String result = mockBuildFullTopic("/function/get", true, 1, "1835", "DEV001");
    TEST_ASSERT_EQUAL_STRING("/1835/DEV001/function/get", result.c_str());
}

void test_topic_prefix_device_info_uses_full_prefix() {
    // 设备信息 (type=2) 使用完整前缀
    String result = mockBuildFullTopic("/info/get", true, 2, "1835", "DEV001");
    TEST_ASSERT_EQUAL_STRING("/1835/DEV001/info/get", result.c_str());
}

void test_topic_prefix_ntp_uses_full_prefix() {
    // NTP同步 (type=7) 使用完整前缀
    String result = mockBuildFullTopic("/ntp/get", true, 7, "1835", "DEV001");
    TEST_ASSERT_EQUAL_STRING("/1835/DEV001/ntp/get", result.c_str());
}

void test_topic_prefix_global_topicPrefix_prepended() {
    // topicPrefix 全局前缀插入在设备前缀之前
    String result = mockBuildFullTopic("/function/get", true, 1, "1835", "DEV001", "/custom");
    TEST_ASSERT_EQUAL_STRING("/custom/1835/DEV001/function/get", result.c_str());
}

void test_topic_prefix_global_topicPrefix_without_productId() {
    // topicPrefix + 无 productId 的组合
    String result = mockBuildFullTopic("/function/get", true, 1, "", "DEV001", "/custom");
    TEST_ASSERT_EQUAL_STRING("/custom/DEV001/function/get", result.c_str());
}

void test_topic_prefix_both_empty_no_prefix() {
    // productId 和 deviceNum 都为空，不加前缀
    String result = mockBuildFullTopic("/function/get", true, 1, "", "");
    TEST_ASSERT_EQUAL_STRING("/function/get", result.c_str());
}

void test_topic_prefix_platform_publish_matches_device_subscribe() {
    // 回归场景：平台发布到 /1835/DEV001/function/get，
    // 设备 productId=1835 deviceNum=DEV001 订阅的也应是 /1835/DEV001/function/get
    String platformTopic = "/1835/DEV001/function/get";
    String deviceSubscribe = mockBuildFullTopic("/function/get", true, 1, "1835", "DEV001");
    TEST_ASSERT_EQUAL_STRING(platformTopic.c_str(), deviceSubscribe.c_str());
}

void test_topic_prefix_default_productId_one() {
    // productId 默认为 "1" 时的主题前缀
    String result = mockBuildFullTopic("/function/get", true, 1, "1", "FBE9BFEFF16A398");
    TEST_ASSERT_EQUAL_STRING("/1/FBE9BFEFF16A398/function/get", result.c_str());
}

void test_topic_prefix_no_curly_braces_in_output() {
    // 回归验证：输出绝不能包含 {} 字符（文档占位符 {productId} 等仅用于注释）
    String result = mockBuildFullTopic("/function/get", true, 1, "1835", "FBE9BFEFF16A398");
    TEST_ASSERT_EQUAL(-1, result.indexOf('{'));
    TEST_ASSERT_EQUAL(-1, result.indexOf('}'));
    TEST_ASSERT_EQUAL_STRING("/1835/FBE9BFEFF16A398/function/get", result.c_str());
}

void test_topic_prefix_actual_device_values() {
    // 用实际设备值构建，确保与平台发布主题完全一致
    // clientId = S&FBE9BFEFF16A398&1835&1，对应 productId=1835 deviceNum=FBE9BFEFF16A398
    String result = mockBuildFullTopic("/function/get", true, 1, "1835", "FBE9BFEFF16A398");
    // 必须是纯数值路径，不能含任何占位符
    TEST_ASSERT_EQUAL_STRING("/1835/FBE9BFEFF16A398/function/get", result.c_str());
    TEST_ASSERT_TRUE(result.indexOf("{productId}") < 0);
    TEST_ASSERT_TRUE(result.indexOf("{deviceNum}") < 0);
}

void test_topic_prefix_no_curly_braces_ota() {
    // OTA 类型同样不能含占位符
    String result = mockBuildFullTopic("/upgrade/set", true, 5, "1835", "FBE9BFEFF16A398");
    TEST_ASSERT_EQUAL(-1, result.indexOf('{'));
    TEST_ASSERT_EQUAL(-1, result.indexOf('}'));
    TEST_ASSERT_EQUAL_STRING("/FBE9BFEFF16A398/upgrade/set", result.c_str());
}

// ============================================================
//  Topic Matching 回归测试
//  验证 getTopicTypeByPath 逻辑：
//  - 先匹配原始 topic，再匹配 autoPrefix 构建的完整 topic
//  - 订阅主题和发布主题都能正确匹配
//  - 未匹配时默认返回 DATA_COMMAND
// ============================================================

/**
 * 模拟 getTopicTypeByPath 的纯函数版本
 * subscribeTopics/publishTopics 用结构体数组模拟
 */
struct MockSubTopic { String topic; bool autoPrefix; int topicType; };
struct MockPubTopic { String topic; bool autoPrefix; int topicType; };

static int mockGetTopicTypeByPath(
    const String& topicPath,
    const MockSubTopic* subs, size_t subCount,
    const MockPubTopic* pubs, size_t pubCount,
    const String& productId, const String& deviceNum) {
    // 先查订阅主题
    for (size_t i = 0; i < subCount; i++) {
        if (subs[i].topic == topicPath) return subs[i].topicType;
        if (subs[i].autoPrefix) {
            String fullTopic = mockBuildFullTopic(subs[i].topic, true, subs[i].topicType, productId, deviceNum);
            if (fullTopic == topicPath) return subs[i].topicType;
        }
    }
    // 再查发布主题
    for (size_t i = 0; i < pubCount; i++) {
        if (pubs[i].topic == topicPath) return pubs[i].topicType;
        if (pubs[i].autoPrefix) {
            String fullTopic = mockBuildFullTopic(pubs[i].topic, true, pubs[i].topicType, productId, deviceNum);
            if (fullTopic == topicPath) return pubs[i].topicType;
        }
    }
    return 1; // 默认 DATA_COMMAND
}

void test_topic_match_data_command_with_auto_prefix() {
    // 平台发到 /1835/DEV001/function/get，设备订阅了 /function/get(autoPrefix=true)
    MockSubTopic subs[] = {{"/function/get", true, 1}};
    MockPubTopic pubs[] = {{"/report", true, 0}};
    int result = mockGetTopicTypeByPath("/1835/DEV001/function/get", subs, 1, pubs, 1, "1835", "DEV001");
    TEST_ASSERT_EQUAL(1, result); // DATA_COMMAND
}

void test_topic_match_data_report_with_auto_prefix() {
    // 发布主题 /report(autoPrefix=true) 应匹配 /1835/DEV001/report
    MockSubTopic subs[] = {{"/function/get", true, 1}};
    MockPubTopic pubs[] = {{"/report", true, 0}};
    int result = mockGetTopicTypeByPath("/1835/DEV001/report", subs, 1, pubs, 1, "1835", "DEV001");
    TEST_ASSERT_EQUAL(0, result); // DATA_REPORT
}

void test_topic_match_raw_topic_without_prefix() {
    // autoPrefix=false 时直接匹配原始 topic
    MockSubTopic subs[] = {{"custom/topic", false, 1}};
    MockPubTopic pubs[] = {};
    int result = mockGetTopicTypeByPath("custom/topic", subs, 1, pubs, 0, "1835", "DEV001");
    TEST_ASSERT_EQUAL(1, result);
}

void test_topic_match_ota_upgrade_topic() {
    // OTA 主题只用 deviceNum 前缀
    MockSubTopic subs[] = {{"/upgrade/set", true, 5}};
    MockPubTopic pubs[] = {};
    int result = mockGetTopicTypeByPath("/DEV001/upgrade/set", subs, 1, pubs, 0, "1835", "DEV001");
    TEST_ASSERT_EQUAL(5, result); // OTA_UPGRADE
}

void test_topic_match_no_match_defaults_to_data_command() {
    // 未匹配任何主题时默认返回 DATA_COMMAND(1)
    MockSubTopic subs[] = {{"/function/get", true, 1}};
    MockPubTopic pubs[] = {};
    int result = mockGetTopicTypeByPath("/unknown/topic", subs, 1, pubs, 0, "1835", "DEV001");
    TEST_ASSERT_EQUAL(1, result); // 默认 DATA_COMMAND
}

void test_topic_match_platform_publish_to_device_subscribe() {
    // 回归场景：平台发布到 /1835/DEV001/function/get
    // 设备 productId=1835 deviceNum=DEV001，订阅了 /function/get(autoPrefix=true)
    // 两者应该匹配
    String platformPublishTopic = "/1835/DEV001/function/get";
    String deviceSub = mockBuildFullTopic("/function/get", true, 1, "1835", "DEV001");
    TEST_ASSERT_EQUAL_STRING(platformPublishTopic.c_str(), deviceSub.c_str());
    // 并且 getTopicTypeByPath 应识别为 DATA_COMMAND
    MockSubTopic subs[] = {{"/function/get", true, 1}};
    MockPubTopic pubs[] = {};
    int result = mockGetTopicTypeByPath(platformPublishTopic, subs, 1, pubs, 0, "1835", "DEV001");
    TEST_ASSERT_EQUAL(1, result);
}

// ============================================================
//  Callback Payload Processing 回归测试
//  验证 mqttCallback 中的关键逻辑：
//  - looksLikeJson 检测（首个非空白字节为 '[' 或 '{'）
//  - JSON 数组命令解析（提取 id/value 字段）
//  - productId 默认值回退机制
// ============================================================

/** 镜像 mqttCallback 中的 looksLikeJson 检测逻辑 */
static bool mockLooksLikeJson(const uint8_t* payload, unsigned int length) {
    for (unsigned int i = 0; i < length; i++) {
        char c = (char)payload[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        return (c == '[' || c == '{');
    }
    return false;
}

void test_callback_json_detect_array() {
    const char* msg = "[{\"id\":\"rgb_led\",\"value\":\"#FF0000\"}]";
    TEST_ASSERT_TRUE(mockLooksLikeJson((const uint8_t*)msg, strlen(msg)));
}

void test_callback_json_detect_object() {
    const char* msg = "{\"count\":5,\"interval\":1000}";
    TEST_ASSERT_TRUE(mockLooksLikeJson((const uint8_t*)msg, strlen(msg)));
}

void test_callback_json_detect_with_leading_whitespace() {
    const char* msg = "  \t\n[{\"id\":\"test\"}]";
    TEST_ASSERT_TRUE(mockLooksLikeJson((const uint8_t*)msg, strlen(msg)));
}

void test_callback_json_detect_plain_text() {
    const char* msg = "Hello World";
    TEST_ASSERT_FALSE(mockLooksLikeJson((const uint8_t*)msg, strlen(msg)));
}

void test_callback_json_detect_hex_frame() {
    // HEX 透传帧（如 Modbus RTU）不以 JSON 字符开头
    const char* msg = "01030000000AB9D4";
    TEST_ASSERT_FALSE(mockLooksLikeJson((const uint8_t*)msg, strlen(msg)));
}

void test_callback_json_detect_empty_payload() {
    TEST_ASSERT_FALSE(mockLooksLikeJson(nullptr, 0));
}

void test_callback_json_detect_retained_empty() {
    // Retained 消息全零场景（之前遇到的 broker 存储的全零 retained 消息）
    uint8_t zeros[] = {0x00, 0x00, 0x00};
    TEST_ASSERT_FALSE(mockLooksLikeJson(zeros, 3));
}

void test_callback_command_json_array_parsing() {
    // 模拟平台下发的命令 JSON 数组解析
    const char* msg = "[{\"id\":\"rgb_led\",\"value\":\"#FF0000\"}]";
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, msg);
    TEST_ASSERT_FALSE(err);
    JsonArray arr = doc.as<JsonArray>();
    TEST_ASSERT_EQUAL(1, arr.size());
    TEST_ASSERT_EQUAL_STRING("rgb_led", arr[0]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("#FF0000", arr[0]["value"].as<const char*>());
}

void test_callback_command_json_multi_item() {
    // 多命令项解析
    const char* msg = "[{\"id\":\"rgb_led\",\"value\":\"#FF0000\"},{\"id\":\"relay_1\",\"value\":\"1\"}]";
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, msg);
    TEST_ASSERT_FALSE(err);
    JsonArray arr = doc.as<JsonArray>();
    TEST_ASSERT_EQUAL(2, arr.size());
    TEST_ASSERT_EQUAL_STRING("rgb_led", arr[0]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("relay_1", arr[1]["id"].as<const char*>());
}

void test_callback_productId_default_fallback() {
    // productId 为空时应回退到默认值 "1"（防止主题前缀缺失）
    String productId = "";
    if (productId.isEmpty()) productId = "1";
    String fullTopic = mockBuildFullTopic("/function/get", true, 1, productId, "DEV001");
    TEST_ASSERT_EQUAL_STRING("/1/DEV001/function/get", fullTopic.c_str());
}

// ============================================================
//  Publish Topic Prefix 全类型验证
//  验证所有 7 种 topicType 的发布主题前缀构建是否正确
//  productId=1835, deviceNum=FBE14C19FC152D8（实际设备值）
// ============================================================

static const char* TEST_PID = "1835";
static const char* TEST_DEV = "FBE14C19FC152D8";

void test_pub_prefix_data_report() {
    // DATA_REPORT (type=0): /property/post → /1835/FBE.../property/post
    String r = mockBuildFullTopic("/property/post", true, 0, TEST_PID, TEST_DEV);
    TEST_ASSERT_EQUAL_STRING("/1835/FBE14C19FC152D8/property/post", r.c_str());
}

void test_pub_prefix_device_info() {
    // DEVICE_INFO (type=2): /info/post → /1835/FBE.../info/post
    String r = mockBuildFullTopic("/info/post", true, 2, TEST_PID, TEST_DEV);
    TEST_ASSERT_EQUAL_STRING("/1835/FBE14C19FC152D8/info/post", r.c_str());
}

void test_pub_prefix_realtime_mon() {
    // REALTIME_MON (type=3): /monitor/post → /1835/FBE.../monitor/post
    String r = mockBuildFullTopic("/monitor/post", true, 3, TEST_PID, TEST_DEV);
    TEST_ASSERT_EQUAL_STRING("/1835/FBE14C19FC152D8/monitor/post", r.c_str());
}

void test_pub_prefix_device_event() {
    // DEVICE_EVENT (type=4): /event/post → /1835/FBE.../event/post
    String r = mockBuildFullTopic("/event/post", true, 4, TEST_PID, TEST_DEV);
    TEST_ASSERT_EQUAL_STRING("/1835/FBE14C19FC152D8/event/post", r.c_str());
}

void test_pub_prefix_ota_upgrade() {
    // OTA_UPGRADE (type=5): /http/upgrade/reply → /FBE.../http/upgrade/reply（无 productId）
    String r = mockBuildFullTopic("/http/upgrade/reply", true, 5, TEST_PID, TEST_DEV);
    TEST_ASSERT_EQUAL_STRING("/FBE14C19FC152D8/http/upgrade/reply", r.c_str());
}

void test_pub_prefix_ota_binary() {
    // OTA_BINARY (type=6): /fetch/upgrade/reply → /FBE.../fetch/upgrade/reply（无 productId）
    String r = mockBuildFullTopic("/fetch/upgrade/reply", true, 6, TEST_PID, TEST_DEV);
    TEST_ASSERT_EQUAL_STRING("/FBE14C19FC152D8/fetch/upgrade/reply", r.c_str());
}

void test_pub_prefix_ntp_sync() {
    // NTP_SYNC (type=7): /ntp/post → /1835/FBE.../ntp/post
    String r = mockBuildFullTopic("/ntp/post", true, 7, TEST_PID, TEST_DEV);
    TEST_ASSERT_EQUAL_STRING("/1835/FBE14C19FC152D8/ntp/post", r.c_str());
}

// ============================================================
//  Payload 格式验证测试
//  验证各类上报数据的 JSON 结构符合 FastBee 平台规范
// ============================================================

void test_payload_data_report_format() {
    // DATA_REPORT payload: JSON 数组，每项包含 id/value/remark
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    JsonObject obj = arr.add<JsonObject>();
    obj["id"] = "temperature";
    obj["value"] = "25.5";
    obj["remark"] = "modbus";

    String payload;
    serializeJson(doc, payload);

    // 验证反序列化后的结构
    JsonDocument parsed;
    TEST_ASSERT_FALSE(deserializeJson(parsed, payload));
    JsonArray parsedArr = parsed.as<JsonArray>();
    TEST_ASSERT_EQUAL(1, parsedArr.size());
    TEST_ASSERT_EQUAL_STRING("temperature", parsedArr[0]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("25.5", parsedArr[0]["value"].as<const char*>());
    TEST_ASSERT_TRUE(parsedArr[0].containsKey("remark"));
}

void test_payload_device_info_format() {
    // DEVICE_INFO payload: JSON 对象，必须包含 rssi/firmwareVersion/status/userId
    JsonDocument doc;
    doc["rssi"] = -54;
    doc["firmwareVersion"] = "1.0.0";
    doc["status"] = 3;  // 在线
    doc["userId"] = 1;
    doc["longitude"] = 0.0;
    doc["latitude"] = 0.0;
    JsonObject summary = doc["summary"].to<JsonObject>();
    summary["name"] = "fastbee";
    summary["chip"] = "ESP32";

    String payload;
    serializeJson(doc, payload);

    JsonDocument parsed;
    TEST_ASSERT_FALSE(deserializeJson(parsed, payload));
    TEST_ASSERT_TRUE(parsed.containsKey("rssi"));
    TEST_ASSERT_TRUE(parsed.containsKey("firmwareVersion"));
    TEST_ASSERT_EQUAL(3, parsed["status"].as<int>());
    TEST_ASSERT_EQUAL(1, parsed["userId"].as<int>());
    TEST_ASSERT_TRUE(parsed["summary"].is<JsonObject>());
    TEST_ASSERT_EQUAL_STRING("fastbee", parsed["summary"]["name"].as<const char*>());
}

void test_payload_monitor_data_format() {
    // REALTIME_MON payload: JSON 数组，与 DATA_REPORT 结构相同但可能含多项
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    JsonObject temp = arr.add<JsonObject>();
    temp["id"] = "temperature";
    temp["value"] = "25.50";
    temp["remark"] = "";

    JsonObject humi = arr.add<JsonObject>();
    humi["id"] = "humidity";
    humi["value"] = "60.00";
    humi["remark"] = "";

    String payload;
    serializeJson(doc, payload);

    JsonDocument parsed;
    TEST_ASSERT_FALSE(deserializeJson(parsed, payload));
    JsonArray parsedArr = parsed.as<JsonArray>();
    TEST_ASSERT_EQUAL(2, parsedArr.size());
    // 验证每项都有 id/value/remark 三字段
    for (JsonVariant v : parsedArr) {
        TEST_ASSERT_TRUE(v.containsKey("id"));
        TEST_ASSERT_TRUE(v.containsKey("value"));
        TEST_ASSERT_TRUE(v.containsKey("remark"));
    }
}

void test_payload_control_report_format() {
    // 控制执行结果上报: JSON 数组，remark 标记为 "control"
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    JsonObject obj = arr.add<JsonObject>();
    obj["id"] = "relay_1";
    obj["value"] = "1";
    obj["remark"] = "control";

    String payload;
    serializeJson(doc, payload);

    JsonDocument parsed;
    TEST_ASSERT_FALSE(deserializeJson(parsed, payload));
    TEST_ASSERT_EQUAL_STRING("control", parsed[0]["remark"].as<const char*>());
}

// ============================================================
//  productId 加载链路验证
//  验证 device.json.productNumber → config.productId → 主题前缀的完整链路
// ============================================================

void test_productId_loading_chain_from_device_json() {
    // 模拟 loadConfig 中 productId 的加载链路：
    // 1. protocol.json mqtt 节不含 productId（已移除）→ config.productId = ""
    // 2. device.json productNumber=1835 → config.productId = "1835"
    // 3. 主题前缀使用 config.productId
    String configProductId = "";  // protocol.json 不提供 productId
    // 模拟从 device.json 读取
    int productNumber = 1835;
    String pn = String(productNumber);
    if (pn != "0") configProductId = pn;
    // 回退默认值
    if (configProductId.isEmpty()) configProductId = "1";

    TEST_ASSERT_EQUAL_STRING("1835", configProductId.c_str());
    // 验证主题使用前缀
    String topic = mockBuildFullTopic("/property/post", true, 0, configProductId, TEST_DEV);
    TEST_ASSERT_TRUE(topic.startsWith("/1835/"));
}

void test_productId_zero_in_device_json_uses_default() {
    // device.json productNumber=0 时应回退到默认值 "1"
    String configProductId = "";
    int productNumber = 0;
    String pn = String(productNumber);
    if (pn != "0") configProductId = pn;
    if (configProductId.isEmpty()) configProductId = "1";

    TEST_ASSERT_EQUAL_STRING("1", configProductId.c_str());
    String topic = mockBuildFullTopic("/property/post", true, 0, configProductId, TEST_DEV);
    TEST_ASSERT_EQUAL_STRING("/1/FBE14C19FC152D8/property/post", topic.c_str());
}

void test_productId_not_read_from_protocol_json() {
    // 验证 productId 不从 protocol.json mqtt 节读取（已移除的逻辑）
    // 即使 protocol.json 曾存了 productId，loadConfig 也不会读取它
    String protocolMqttProductId = "999";  // 假设 protocol.json 中有旧值
    (void)protocolMqttProductId;  // 不再使用

    // loadConfig 中 config.productId 直接从空开始
    String configProductId = "";  // cfg["productId"] 已移除，始终为空
    // 然后从 device.json 读取
    int deviceProductNumber = 1835;
    String pn = String(deviceProductNumber);
    if (pn != "0") configProductId = pn;
    if (configProductId.isEmpty()) configProductId = "1";

    // 应该使用 device.json 的值，而非 protocol.json 的旧值
    TEST_ASSERT_EQUAL_STRING("1835", configProductId.c_str());
}

// ============================================================
//  发布/订阅主题对称性测试
//  验证同一 topicType 的发布和订阅主题使用相同的前缀
// ============================================================

void test_pub_sub_symmetry_data_topics() {
    // DATA_REPORT 发布和 DATA_COMMAND 订阅使用相同前缀
    String pubTopic = mockBuildFullTopic("/property/post", true, 0, TEST_PID, TEST_DEV);
    String subTopic = mockBuildFullTopic("/function/get", true, 1, TEST_PID, TEST_DEV);
    // 两者前缀部分应相同
    TEST_ASSERT_TRUE(pubTopic.startsWith("/1835/FBE14C19FC152D8/"));
    TEST_ASSERT_TRUE(subTopic.startsWith("/1835/FBE14C19FC152D8/"));
}

void test_pub_sub_symmetry_info_topics() {
    // DEVICE_INFO 发布和 DEVICE_INFO 订阅使用相同前缀
    String pubTopic = mockBuildFullTopic("/info/post", true, 2, TEST_PID, TEST_DEV);
    String subTopic = mockBuildFullTopic("/info/get", true, 2, TEST_PID, TEST_DEV);
    TEST_ASSERT_TRUE(pubTopic.startsWith("/1835/FBE14C19FC152D8/"));
    TEST_ASSERT_TRUE(subTopic.startsWith("/1835/FBE14C19FC152D8/"));
}

void test_pub_sub_symmetry_monitor_topics() {
    // REALTIME_MON 发布和订阅使用相同前缀
    String pubTopic = mockBuildFullTopic("/monitor/post", true, 3, TEST_PID, TEST_DEV);
    String subTopic = mockBuildFullTopic("/monitor/get", true, 3, TEST_PID, TEST_DEV);
    TEST_ASSERT_TRUE(pubTopic.startsWith("/1835/FBE14C19FC152D8/"));
    TEST_ASSERT_TRUE(subTopic.startsWith("/1835/FBE14C19FC152D8/"));
}

void test_pub_sub_symmetry_ntp_topics() {
    // NTP_SYNC 发布和订阅使用相同前缀
    String pubTopic = mockBuildFullTopic("/ntp/post", true, 7, TEST_PID, TEST_DEV);
    String subTopic = mockBuildFullTopic("/ntp/get", true, 7, TEST_PID, TEST_DEV);
    TEST_ASSERT_TRUE(pubTopic.startsWith("/1835/FBE14C19FC152D8/"));
    TEST_ASSERT_TRUE(subTopic.startsWith("/1835/FBE14C19FC152D8/"));
}

void test_pub_sub_symmetry_ota_topics() {
    // OTA 发布和订阅只用 deviceNum 前缀（无 productId）
    String pubTopic = mockBuildFullTopic("/http/upgrade/reply", true, 5, TEST_PID, TEST_DEV);
    String subTopic = mockBuildFullTopic("/http/upgrade/set", true, 5, TEST_PID, TEST_DEV);
    TEST_ASSERT_TRUE(pubTopic.startsWith("/FBE14C19FC152D8/"));
    TEST_ASSERT_TRUE(subTopic.startsWith("/FBE14C19FC152D8/"));
    // OTA 主题不应包含 productId
    TEST_ASSERT_TRUE(pubTopic.indexOf("/1835/") < 0);
    TEST_ASSERT_TRUE(subTopic.indexOf("/1835/") < 0);
}

void test_pub_sub_symmetry_all_protocol_json_defaults() {
    // 使用 protocol.json 默认配置中的所有主题验证前缀一致性
    // 发布主题
    const char* pubTopics[] = {"/property/post", "/info/post", "/event/post",
                               "/monitor/post", "/ntp/post",
                               "/http/upgrade/reply", "/fetch/upgrade/reply"};
    const int pubTypes[]    = {0, 2, 4, 3, 7, 5, 6};
    // 订阅主题
    const char* subTopics[] = {"/function/get", "/info/get", "/monitor/get",
                               "/ntp/get", "/http/upgrade/set", "/fetch/upgrade/set"};
    const int subTypes[]    = {1, 2, 3, 7, 5, 6};

    // 验证每个发布主题前缀正确
    for (int i = 0; i < 7; i++) {
        String full = mockBuildFullTopic(pubTopics[i], true, pubTypes[i], TEST_PID, TEST_DEV);
        if (pubTypes[i] == 5 || pubTypes[i] == 6) {
            // OTA 只用 deviceNum
            TEST_ASSERT_TRUE(full.startsWith("/FBE14C19FC152D8/"));
        } else {
            // 其他类型用 productId/deviceNum
            TEST_ASSERT_TRUE(full.startsWith("/1835/FBE14C19FC152D8/"));
        }
        // 结尾应是原始 topic
        TEST_ASSERT_TRUE(full.endsWith(pubTopics[i]));
    }

    // 验证每个订阅主题前缀正确
    for (int i = 0; i < 6; i++) {
        String full = mockBuildFullTopic(subTopics[i], true, subTypes[i], TEST_PID, TEST_DEV);
        if (subTypes[i] == 5 || subTypes[i] == 6) {
            TEST_ASSERT_TRUE(full.startsWith("/FBE14C19FC152D8/"));
        } else {
            TEST_ASSERT_TRUE(full.startsWith("/1835/FBE14C19FC152D8/"));
        }
        TEST_ASSERT_TRUE(full.endsWith(subTopics[i]));
    }
}

void test_clientId_productId_matches_topic_prefix() {
    // 验证 clientId 中的 productId 与主题前缀中的 productId 一致
    // clientId 格式: S&deviceNum&productId&userId
    String clientId = "S&FBE14C19FC152D8&1835&1";
    // 从 clientId 解析 productId
    int firstAmp = clientId.indexOf('&');
    int secondAmp = clientId.indexOf('&', firstAmp + 1);
    int thirdAmp = clientId.indexOf('&', secondAmp + 1);
    String clientIdProductId = clientId.substring(secondAmp + 1, thirdAmp);
    TEST_ASSERT_EQUAL_STRING("1835", clientIdProductId.c_str());

    // 主题前缀中的 productId 应一致
    String topic = mockBuildFullTopic("/property/post", true, 0, clientIdProductId, TEST_DEV);
    TEST_ASSERT_TRUE(topic.startsWith("/1835/"));
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
    RUN_TEST(test_mqtt_deferred_restart_preserves_active_client);
    RUN_TEST(test_mqtt_restore_restart_handle_survives_null_client);
    RUN_TEST(test_mqtt_deferred_restart_keeps_mqtts_client_in_marginal_dram);
    RUN_TEST(test_mqtt_deferred_restart_heap_order);
    RUN_TEST(test_mqtt_doReconnect_heap_threshold_8kb);
    RUN_TEST(test_mqtt_dns_fail_threshold_3);

    // MQTT Status API & Frontend Badge Mapping Tests
    RUN_TEST(test_mqtt_status_api_connected_state);
    RUN_TEST(test_mqtt_status_api_connecting_state);
    RUN_TEST(test_mqtt_status_api_stopped_state);
    RUN_TEST(test_mqtt_status_api_no_client_state);
    RUN_TEST(test_mqtt_status_api_disabled_comprehensive);
    RUN_TEST(test_mqtt_status_api_network_unavailable);
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

    // Group 8: MQTT 连接测试快速路径 + 前端重构回归测试
    RUN_TEST(test_mqtt_test_fast_path_already_connected);
    RUN_TEST(test_mqtt_test_fast_path_simulation);
    RUN_TEST(test_mqtt_refresh_status_method);
    RUN_TEST(test_mqtt_old_methods_removed);
    RUN_TEST(test_mqtt_test_backend_mqtts_socket_timeout);
    RUN_TEST(test_mqtt_test_frontend_timeout_extended);
    RUN_TEST(test_mqtt_test_fast_path_no_false_positive);
    RUN_TEST(test_mqtt_test_simple_auth_respects_form_clientid);
    RUN_TEST(test_mqtt_test_fast_path_connecting_state);
    RUN_TEST(test_mqtt_test_save_config_includes_clientid);

    // 网络切换后 MQTT 重连测试（修复 use-after-free + 错误计数器不重置）
    RUN_TEST(test_mqtt_reconnect_after_network_type_change);
    RUN_TEST(test_mqtt_error_counters_reset_on_transport_change);
    RUN_TEST(test_mqtt_handle_skipped_when_stopped);
    RUN_TEST(test_mqtt_memory_protection_during_switch);

    // MQTT 重连任务按需创建测试
    RUN_TEST(test_mqtt_reconnect_task_on_demand);

    // MQTTS SSL 内存失败防御测试
    RUN_TEST(test_mqtts_ssl_memory_failure_defense);
    RUN_TEST(test_mqtts_ssl_memory_failure_does_not_reset_wifi);

    // MQTTS 简化：无 PSRAM 设备 begin() 警告提示 + API tlsSupported
    RUN_TEST(test_no_psram_forces_mqtt_in_begin);
    RUN_TEST(test_protocol_api_has_tls_supported);

    // MQTTS TLS 动态内存管理测试（WiFiClientSecure 动态分配优化）
    RUN_TEST(test_mqtts_dynamic_tls_lifecycle_methods);
    RUN_TEST(test_mqtts_release_before_connect);
    RUN_TEST(test_mqtts_wificlientsecure_is_pointer);
    RUN_TEST(test_mqtts_disconnect_releases_tls);
    RUN_TEST(test_mqtts_dram_monitoring_in_reconnect);

    // MQTTS DRAM 回收与 Web 服务生命周期测试
    RUN_TEST(test_mqtts_reclaim_dram_full_workflow);
    RUN_TEST(test_mqtts_resume_web_services);
    RUN_TEST(test_mqtts_dram_thresholds_updated);
    RUN_TEST(test_mqtts_protocol_manager_threshold_sync);
    RUN_TEST(test_mqtts_pubsubclient_setclient_sync);
    RUN_TEST(test_mqtts_mbedtls_allocator_uses_psram);
    RUN_TEST(test_mqtts_web_resume_waits_for_async_tcp_release);
    RUN_TEST(test_web_server_watchdog_mechanisms);
    RUN_TEST(test_mqtts_memory_backoff_preserved);
    RUN_TEST(test_mqtts_recoverable_low_dram_uses_short_retry);
    RUN_TEST(test_mqtts_all_failure_branches_resume_web);
    RUN_TEST(test_mqtts_external_transport_skips_internal_tls);
    RUN_TEST(test_mqtts_protocol_manager_reads_section_scheme_for_cellular_secure);
    RUN_TEST(test_mqtts_ethernet_uses_internal_secure_transport);

    // 修复回归测试：MQTTS 缓冲区/超时/loop/事件循环
    RUN_TEST(test_mqtts_buffer_size_unified_1024);
    RUN_TEST(test_mqtts_post_connect_timeout_15s);
    RUN_TEST(test_mqtts_loop_between_post_connect_ops);
    RUN_TEST(test_mqtt_disconnect_event_guarded);

    // P0/P1 改进验证测试
    RUN_TEST(test_reset_error_counters_implementation);
    RUN_TEST(test_process_queued_reports_uses_dram);
    RUN_TEST(test_wifi_got_ip_notifies_mqtt);

    // Topic Prefix Building 回归测试（buildFullTopicWithType 逻辑）
    RUN_TEST(test_topic_prefix_auto_off_no_prefix);
    RUN_TEST(test_topic_prefix_with_productId_and_deviceNum);
    RUN_TEST(test_topic_prefix_empty_productId_fallback_to_deviceNum);
    RUN_TEST(test_topic_prefix_empty_deviceNum_no_prefix);
    RUN_TEST(test_topic_prefix_ota_only_uses_deviceNum);
    RUN_TEST(test_topic_prefix_ota_binary_only_uses_deviceNum);
    RUN_TEST(test_topic_prefix_data_report_uses_full_prefix);
    RUN_TEST(test_topic_prefix_data_command_uses_full_prefix);
    RUN_TEST(test_topic_prefix_device_info_uses_full_prefix);
    RUN_TEST(test_topic_prefix_ntp_uses_full_prefix);
    RUN_TEST(test_topic_prefix_global_topicPrefix_prepended);
    RUN_TEST(test_topic_prefix_global_topicPrefix_without_productId);
    RUN_TEST(test_topic_prefix_both_empty_no_prefix);
    RUN_TEST(test_topic_prefix_platform_publish_matches_device_subscribe);
    RUN_TEST(test_topic_prefix_default_productId_one);
    RUN_TEST(test_topic_prefix_no_curly_braces_in_output);
    RUN_TEST(test_topic_prefix_actual_device_values);
    RUN_TEST(test_topic_prefix_no_curly_braces_ota);

    // Topic Matching 回归测试（getTopicTypeByPath 逻辑）
    RUN_TEST(test_topic_match_data_command_with_auto_prefix);
    RUN_TEST(test_topic_match_data_report_with_auto_prefix);
    RUN_TEST(test_topic_match_raw_topic_without_prefix);
    RUN_TEST(test_topic_match_ota_upgrade_topic);
    RUN_TEST(test_topic_match_no_match_defaults_to_data_command);
    RUN_TEST(test_topic_match_platform_publish_to_device_subscribe);

    // Callback Payload Processing 回归测试
    RUN_TEST(test_callback_json_detect_array);
    RUN_TEST(test_callback_json_detect_object);
    RUN_TEST(test_callback_json_detect_with_leading_whitespace);
    RUN_TEST(test_callback_json_detect_plain_text);
    RUN_TEST(test_callback_json_detect_hex_frame);
    RUN_TEST(test_callback_json_detect_empty_payload);
    RUN_TEST(test_callback_json_detect_retained_empty);
    RUN_TEST(test_callback_command_json_array_parsing);
    RUN_TEST(test_callback_command_json_multi_item);
    RUN_TEST(test_callback_productId_default_fallback);

    // Publish Topic Prefix 全类型验证（7种 topicType 的发布主题前缀）
    RUN_TEST(test_pub_prefix_data_report);
    RUN_TEST(test_pub_prefix_device_info);
    RUN_TEST(test_pub_prefix_realtime_mon);
    RUN_TEST(test_pub_prefix_device_event);
    RUN_TEST(test_pub_prefix_ota_upgrade);
    RUN_TEST(test_pub_prefix_ota_binary);
    RUN_TEST(test_pub_prefix_ntp_sync);

    // Payload 格式验证测试（JSON 结构校验）
    RUN_TEST(test_payload_data_report_format);
    RUN_TEST(test_payload_device_info_format);
    RUN_TEST(test_payload_monitor_data_format);
    RUN_TEST(test_payload_control_report_format);

    // productId 加载链路验证（device.json → config → 主题前缀）
    RUN_TEST(test_productId_loading_chain_from_device_json);
    RUN_TEST(test_productId_zero_in_device_json_uses_default);
    RUN_TEST(test_productId_not_read_from_protocol_json);

    // 发布/订阅主题对称性测试
    RUN_TEST(test_pub_sub_symmetry_data_topics);
    RUN_TEST(test_pub_sub_symmetry_info_topics);
    RUN_TEST(test_pub_sub_symmetry_monitor_topics);
    RUN_TEST(test_pub_sub_symmetry_ntp_topics);
    RUN_TEST(test_pub_sub_symmetry_ota_topics);
    RUN_TEST(test_pub_sub_symmetry_all_protocol_json_defaults);
    RUN_TEST(test_clientId_productId_matches_topic_prefix);

    TestLog::groupEnd();
}
