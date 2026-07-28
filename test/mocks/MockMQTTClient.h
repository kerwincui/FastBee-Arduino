/**
 * @file MockMQTTClient.h
 * @brief MQTT客户端模拟对象
 * 
 * 提供MQTT协议功能的模拟实现，支持连接、发布、订阅、认证等
 */

#ifndef MOCK_MQTT_CLIENT_H
#define MOCK_MQTT_CLIENT_H

#include <Arduino.h>
#include <functional>
#include <map>
#include <vector>

// MQTT QoS级别
enum MQTTQoS {
    QOS0 = 0,
    QOS1 = 1,
    QOS2 = 2
};

// MQTT配置结构
struct MQTTConfig {
    bool enabled;
    String scheme;     // "mqtt" 或 "mqtts"
    String server;
    uint16_t port;
    String clientId;
    String username;
    String password;
    bool autoReconnect;
    uint16_t reconnectInterval;
    String willTopic;
    String willPayload;
    uint8_t willQos;
    bool willRetain;
    uint16_t keepAlive;
    uint16_t socketTimeout;
    
    MQTTConfig() : enabled(false), scheme("mqtt"), port(1883), autoReconnect(true),
                   reconnectInterval(5000), willQos(0), willRetain(false),
                   keepAlive(60), socketTimeout(15) {}
};

// 消息回调类型
typedef std::function<void(const char* topic, const uint8_t* payload, 
                           unsigned int length)> MQTTCallback;

// 模拟MQTT客户端类
class MockMQTTClient {
public:
    MockMQTTClient() : _connected(false), _stopped(false), _reconnectPending(false),
                       _reconnectCount(0),
                       _lastError(0), _messageId(0),
                       _shouldFailConnect(false), _shouldFailPublish(false),
                       _lastReconnectAttempt(0) {}

    // 初始化
    void initialize(const MQTTConfig& config) {
        _config = config;
        _reconnectCount = 0;
        _lastError = 0;
        _lastReconnectAttempt = 0;
        _reconnectPending = false;
        _reconnectRunning = false;
        _reconnectScheduledMs = 0;
        _reconnectTaskAlive = false;
        _consecutiveTimeouts = 0;
        _stopped = false;
    }

    // 连接管理
    bool connect() {
        if (_shouldFailConnect) {
            _connected = false;
            _lastError = -1;
            return false;
        }
        
        // 验证认证信息
        if (!validateAuth()) {
            _connected = false;
            _lastError = 5;  // 认证失败
            return false;
        }
        
        _connected = true;
        _lastError = 0;
        return true;
    }

    void disconnect() {
        _connected = false;
        _subscriptions.clear();
    }

    bool getIsConnected() { return _connected; }

    // 重连机制
    bool reconnect() {
        _reconnectCount++;
        return connect();
    }

    int getReconnectCount() { return _reconnectCount; }

    // 发布消息
    bool publish(const char* topic, const char* payload, 
                 bool retained = false, MQTTQoS qos = QOS0) {
        if (!_connected) return false;
        if (_shouldFailPublish) return false;
        
        PublishedMessage msg;
        msg.topic = String(topic);
        msg.payload = String(payload);
        msg.retained = retained;
        msg.qos = qos;
        msg.messageId = ++_messageId;
        
        _publishedMessages.push_back(msg);
        return true;
    }

    bool publish(const char* topic, const uint8_t* payload, unsigned int plength,
                 bool retained = false, MQTTQoS qos = QOS0) {
        if (!_connected) return false;
        
        String payloadStr;
        for (unsigned int i = 0; i < plength; i++) {
            payloadStr += (char)payload[i];
        }
        
        return publish(topic, payloadStr.c_str(), retained, qos);
    }

    // 订阅主题
    bool subscribe(const char* topic, MQTTQoS qos = QOS0) {
        if (!_connected) return false;
        
        Subscription sub;
        sub.topic = String(topic);
        sub.qos = qos;
        _subscriptions.push_back(sub);
        
        return true;
    }

    bool unsubscribe(const char* topic) {
        for (auto it = _subscriptions.begin(); it != _subscriptions.end(); ++it) {
            if (it->topic == topic) {
                _subscriptions.erase(it);
                return true;
            }
        }
        return false;
    }

    // 设置回调
    void setCallback(MQTTCallback callback) {
        _callback = callback;
    }

    // 模拟接收消息（测试用）
    void simulateMessage(const char* topic, const char* payload) {
        if (_callback && _connected) {
            uint8_t* data = (uint8_t*)payload;
            _callback(topic, data, strlen(payload));
        }
    }

    // 循环处理
    void loop() {
        if (!_connected && _config.autoReconnect) {
            if (millis() - _lastReconnectAttempt > _config.reconnectInterval) {
                _lastReconnectAttempt = millis();
                reconnect();
            }
        }
    }

    // 客户端ID构建
    static String buildClientId(const char* prefix, const char* deviceNum,
                                const char* productId, const char* userId) {
        String clientId = String(prefix) + "&" + deviceNum + "&" + 
                         productId + "&" + userId;
        return clientId;
    }

    // 加密密码构建（模拟）
    static String buildEncryptedPassword(const char* password, 
                                          const char* authCode,
                                          const char* mqttSecret,
                                          const char* ntpServer) {
        // 模拟AES加密
        String encrypted = "ENC:";
        for (int i = 0; i < strlen(password); i++) {
            encrypted += String((int)password[i] ^ (int)mqttSecret[i % strlen(mqttSecret)], HEX);
        }
        return encrypted;
    }

    // 认证类型检测
    static int detectAuthType(const char* clientId) {
        if (strncmp(clientId, "S&", 2) == 0) return 0;  // 简单认证
        if (strncmp(clientId, "E&", 2) == 0) return 1;  // 加密认证
        return -1;  // 无效
    }

    // 获取配置
    MQTTConfig getConfig() { return _config; }

    // 获取错误码
    int getLastError() { return _lastError; }

    // 获取已发布消息列表（测试验证用）
    struct PublishedMessage {
        String topic;
        String payload;
        bool retained;
        MQTTQoS qos;
        int messageId;
    };
    std::vector<PublishedMessage> getPublishedMessages() {
        return _publishedMessages;
    }

    // 获取订阅列表
    struct Subscription {
        String topic;
        MQTTQoS qos;
    };
    std::vector<Subscription> getSubscriptions() {
        return _subscriptions;
    }

    // 测试控制方法
    void setShouldFailConnect(bool fail) { _shouldFailConnect = fail; }
    void setShouldFailPublish(bool fail) { _shouldFailPublish = fail; }
    void setConnected(bool connected) { _connected = connected; }
    void clearPublishedMessages() { _publishedMessages.clear(); }

    // ========== 自动连接 & 状态通知模拟 ==========

    // 状态变更回调
    typedef std::function<void(const String& json)> StatusChangeCallback;
    void setStatusChangeCallback(StatusChangeCallback cb) { _statusChangeCallback = cb; }

    // 模拟 _notifyStatusChange (与生产代码保持字段一致)
    void notifyStatusChange() {
        if (!_statusChangeCallback) return;
        // 生产代码中：initialized=true, connected, stopped, server, port, clientId, reconnectCount, lastError
        String json = "{\"initialized\":true,\"connected\":";
        json += _connected ? "true" : "false";
        json += ",\"stopped\":";
        json += _stopped ? "true" : "false";
        json += ",\"server\":\"";
        json += _config.server;
        json += "\",\"port\":";
        json += String(_config.port);
        json += ",\"clientId\":\"";
        json += _config.clientId;
        json += "\",\"reconnectCount\":";
        json += String(_reconnectCount);
        json += ",\"lastError\":";
        json += String(_lastError);
        json += "}";
        _statusChangeCallback(json);
    }

    // 自动重连调度模拟（镜像生产 handle() 断开分支：看门狗自愈 + 调度门控）
    // 返回是否调度了重连
    bool handleAutoReconnect(unsigned long currentMillis) {
        if (_stopped) return false;
        if (_connected) return false;
        if (!_config.autoReconnect) return false;

        // 自愈看门狗：标志卡死超过最大执行窗口时强制重置（镜像 recoverStuckReconnect）
        if ((_reconnectPending || _reconnectRunning) &&
            _reconnectScheduledMs != 0 &&
            (currentMillis - _reconnectScheduledMs > RECONNECT_WATCHDOG_TIMEOUT_MS)) {
            recoverStuckReconnect();
        }

        unsigned long elapsed = currentMillis - _lastReconnectAttempt;
        unsigned long interval = (unsigned long)_config.reconnectInterval;
        if (elapsed >= interval) {
            _lastReconnectAttempt = currentMillis;
            // 调度门控：pending/running 任一为真都不再调度（镜像生产代码）
            if (!_reconnectPending && !_reconnectRunning) {
                _reconnectPending = true;
                _reconnectScheduledMs = currentMillis;  // 记录调度时间戳（看门狗用）
                _reconnectTaskAlive = true;             // 模拟任务创建成功
                return true;
            }
        }
        return false;
    }

    // 镜像生产 recoverStuckReconnect：强制重置卡死标志并回收僵死任务
    void recoverStuckReconnect() {
        _reconnectPending = false;
        _reconnectRunning = false;
        _reconnectScheduledMs = 0;
        _reconnectTaskAlive = false;   // 模拟 vTaskDelete 回收僵死任务
        _consecutiveTimeouts = 0;      // 卡死前的连续超时不延续到恢复后
    }

    // 镜像生产 resetErrorCounters：网络恢复后清除卡死调度标志、重置退避
    void resetErrorCounters() {
        _consecutiveTimeouts = 0;
        _reconnectPending = false;
        _reconnectScheduledMs = 0;
        _lastReconnectAttempt = 0;  // 强制下次 handle() 立即尝试重连
    }

    bool isReconnectPending() const { return _reconnectPending; }
    void clearReconnectPending() { _reconnectPending = false; }
    bool isReconnectRunning() const { return _reconnectRunning; }
    void setReconnectRunning(bool v) { _reconnectRunning = v; }
    bool isReconnectTaskAlive() const { return _reconnectTaskAlive; }
    unsigned long getReconnectScheduledMs() const { return _reconnectScheduledMs; }
    int getConsecutiveTimeouts() const { return _consecutiveTimeouts; }
    void setConsecutiveTimeouts(int v) { _consecutiveTimeouts = v; }
    // 模拟一次性重连任务异常终止（如栈溢出）后遗留的卡死状态
    void simulateStuckReconnect(unsigned long scheduledMs) {
        _reconnectPending = true;
        _reconnectRunning = true;
        _reconnectScheduledMs = scheduledMs;
        _reconnectTaskAlive = true;
    }
    // 模拟加固后的 reconnectTaskEntry 正常退出：始终清除标志
    void simulateTaskExit() {
        _reconnectPending = false;
        _reconnectRunning = false;
        _reconnectTaskAlive = false;
    }
    void setStopped(bool s) { _stopped = s; }
    bool isStopped() const { return _stopped; }

    // 后台重连延迟常量 (与生产代码同步: reconnectTaskEntry 中 vTaskDelay 3s)
    static constexpr uint32_t BOOT_STABILIZATION_DELAY_MS = 3000;
    // 重连自愈看门狗超时（与生产代码 MQTTClient::RECONNECT_WATCHDOG_TIMEOUT_MS 同步）
    static constexpr uint32_t RECONNECT_WATCHDOG_TIMEOUT_MS = 90000;

private:
    bool validateAuth() {
        // 简单认证验证
        if (_config.clientId.startsWith("S&")) {
            return !_config.username.isEmpty();
        }
        // 加密认证验证
        if (_config.clientId.startsWith("E&")) {
            return !_config.password.isEmpty() && _config.password.startsWith("ENC:");
        }
        return _config.clientId.startsWith("Test") ||
               (!_config.clientId.isEmpty() &&
                !_config.username.isEmpty() &&
                !_config.password.isEmpty());
    }

    MQTTConfig _config;
    bool _connected;
    bool _stopped = false;
    bool _reconnectPending = false;
    bool _reconnectRunning = false;
    unsigned long _reconnectScheduledMs = 0;
    bool _reconnectTaskAlive = false;
    int _consecutiveTimeouts = 0;
    int _reconnectCount;
    int _lastError;
    int _messageId;
    bool _shouldFailConnect;
    bool _shouldFailPublish;
    unsigned long _lastReconnectAttempt;
    
    MQTTCallback _callback;
    StatusChangeCallback _statusChangeCallback;
    std::vector<PublishedMessage> _publishedMessages;
    std::vector<Subscription> _subscriptions;
};

// 全局Mock MQTT客户端实例
inline MockMQTTClient MockMQTT;

#endif // MOCK_MQTT_CLIENT_H
