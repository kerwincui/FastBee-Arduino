#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_random.h>
#include <vector>
#include <freertos/semphr.h>
#include <core/SystemConstants.h>

// 固定大小的 MQTT 上报槽（零堆分配）
struct MqttSlot {
    char payload[512];    // 固定 512 字节 payload 缓冲
    uint16_t length;      // 实际 payload 长度
    bool occupied;        // 槽位是否被占用
    
    MqttSlot() : length(0), occupied(false) {
        payload[0] = '\0';
    }
    
    void clear() {
        length = 0;
        occupied = false;
        payload[0] = '\0';
    }
};

static constexpr uint8_t MQTT_REPORT_SLOTS = 8;         // 8 个槽位
static constexpr uint16_t MQTT_SLOT_MAX_PAYLOAD = 512;   // 单个槽位最大 payload

// 主题类型枚举
enum class MqttTopicType : uint8_t {
    DATA_REPORT   = 0,  // 数据上报
    DATA_COMMAND  = 1,  // 数据下发
    DEVICE_INFO   = 2,  // 设备信息
    REALTIME_MON  = 3,  // 实时监测
    DEVICE_EVENT  = 4,  // 设备事件
    OTA_UPGRADE   = 5,  // OTA升级
    OTA_BINARY    = 6,  // OTA二进制
    NTP_SYNC      = 7   // NTP时间同步
};

// MQTT认证类型枚举
enum class MqttAuthType : uint8_t {
    SIMPLE    = 0,  // 简单认证 (S) - 密码直接传输
    ENCRYPTED = 1   // 加密认证 (E) - AES-CBC-128加密密码
};

// 发布主题配置结构体
struct MqttPublishTopic {
    String topic;
    uint8_t qos;
    bool retain;
    bool enabled;
    bool autoPrefix;    // 是否启用自动前缀（拼接topicPrefix到主题前）
    String content;
    MqttTopicType topicType;
    
    MqttPublishTopic() : qos(0), retain(false), enabled(true), autoPrefix(false), topicType(MqttTopicType::DATA_REPORT) {}
};

// 订阅主题配置结构体
struct MqttSubscribeTopic {
    String topic;       // 订阅主题
    uint8_t qos;        // QoS等级
    bool enabled;       // 是否启用此订阅
    bool autoPrefix;    // 是否启用自动前缀（拼接topicPrefix到主题前）
    String action;      // 执行字段，定义接收到消息时的处理逻辑
    MqttTopicType topicType;
    
    MqttSubscribeTopic() : qos(0), enabled(true), autoPrefix(false), topicType(MqttTopicType::DATA_COMMAND) {}
};

// MQTT配置结构体
struct MQTTConfig {
    String server;
    uint16_t port;
    String clientId;
    String username;
    String password;
    String topicPrefix;
    String subscribeTopic;      // 兼容旧配置的单一订阅主题
    uint16_t keepAlive;
    bool autoReconnect;        // 自动重连
    uint32_t connectionTimeout;// 连接超时（毫秒）
    // 认证配置
    MqttAuthType authType;     // 认证类型: 简单认证(S) / 加密认证(E)
    String deviceNum;          // 设备编号
    String productId;          // 产品ID
    String userId;             // 用户ID
    String mqttSecret;         // 产品秘钥（AES加密密钥，16字节）
    String authCode;           // 设备授权码（可选）
    String ntpServer;          // NTP服务器地址
    // 遗嘱消息配置
    String willTopic;
    String willPayload;
    uint8_t willQos = 0;
    bool willRetain = false;
    // Card 高级配置（设备信息发布用）
    double longitude = 0;        // 经度
    double latitude = 0;         // 纬度
    String iccid;                // 物联网卡 ICCID
    int cardPlatformId = 0;      // 卡平台编号
    String summary;              // 摘要 JSON 字符串
    // 发布主题配置（支持多组）
    std::vector<MqttPublishTopic> publishTopics;
    // 订阅主题配置（支持多组）
    std::vector<MqttSubscribeTopic> subscribeTopics;

    // 默认构造函数
    MQTTConfig() : port(1883), keepAlive(60),
                   autoReconnect(true), 
                   connectionTimeout(30000), authType(MqttAuthType::SIMPLE),
                   willQos(0), willRetain(false) {}
};

class MQTTClient {
public:
    MQTTClient();
    ~MQTTClient();

    bool loadMqttConfig(const String& filename = FileSystem::PROTOCOL_CONFIG_FILE); 
    bool begin();
    void shutdown();  // 关闭后台任务（在系统关闭前调用）
    bool connect();
    void disconnect();
    void stop();           // 显式停止MQTT（断开并阻止自动重连）
    bool publish(const String& topic, const String& message);
    bool publishToTopic(size_t topicIndex, const String& message);
    bool publishDeviceInfo();  // 发布设备信息到 topicType=DEVICE_INFO 的主题
    bool publishMonitorData(); // 发布一次实时监测数据到 topicType=REALTIME_MON 的主题
    bool publishReportData(const String& payload); // 发布数据上报到 topicType=DATA_REPORT 的主题
    bool queueReportData(const String& payload);   // 线程安全入队上报数据（异步任务调用）
    bool queueReportData(const char* payload, uint16_t length); // 零拷贝版本
    uint8_t getQueueDepth() const { return _slotCount; } // 获取上报队列深度
    bool publishNtpSync();             // 发布 NTP 时间同步请求到 topicType=NTP_SYNC 的主题
    bool subscribe(const String& topic);
    bool subscribeAll();  // 订阅所有配置的主题
    void handle();
    String getStatus() const;
    
    // 获取配置引用
    const MQTTConfig& getConfig() const { return config; }
    
    // 详细状态信息
    bool getIsConnected() const { return isConnected; }
    bool isStopped() const { return stopped; }  // 是否被显式停止
    int  getLastErrorCode() const { return lastErrorCode; }
    uint32_t getReconnectCount() const { return reconnectCount; }
    unsigned long getLastConnectedTime() const { return lastConnectedTime; }
    
    // MQTT 上报降采样：设置最小上报间隔（毫秒），0 表示无限制
    void setMinReportInterval(uint32_t ms);

    // 设置消息回调（topic, message, topicType）
    void setMessageCallback(std::function<void(const String&, const String&, MqttTopicType)> callback);

    // 设置状态变化回调（SSE 推送用）
    typedef std::function<void(const String&)> StatusChangeCallback;
    void setStatusChangeCallback(StatusChangeCallback cb);

    // 设置实时监测数据提供者回调（返回 JSON 数组字符串）
    void setMonitorDataProvider(std::function<String()> provider);

    // 根据主题路径查找对应的主题类型（可选输出订阅主题索引）
    MqttTopicType getTopicTypeByPath(const String& topicPath, int8_t* outSubIndex = nullptr) const;

private:
    WiFiClient wifiClient;
    PubSubClient mqttClient;
    MQTTConfig config;
    bool isConnected;
    bool stopped;                     // 是否被显式停止（阻止自动重连）
    unsigned long lastReconnectAttempt;
    unsigned long lastConnectedTime;  // 上次连接成功时间
    int  lastErrorCode;               // 上次错误码
    uint32_t reconnectCount;          // 重连次数
    uint32_t reconnectInterval;       // 当前重连间隔（指数退避，毫秒）
    uint8_t  consecutiveTimeouts;     // 连续超时次数（用于检测 DNS 故障）
    unsigned long lastLoopTime;       // 上次loop()调用的时间（用于检测连接健康）
    
    // 实时监测状态
    bool monitorActive = false;       // 是否正在执行实时监测
    int monitorRemaining = 0;         // 剩余发布次数
    unsigned long monitorInterval = 1000; // 发布间隔（毫秒）
    unsigned long lastMonitorTime = 0;    // 上次发布监测数据的时间
    
    // NTP 时间同步状态
    unsigned long long ntpDeviceSendTime = 0;  // 发送 NTP 请求时的设备本地时间戳（millis）
    
    std::function<void(const String&, const String&, MqttTopicType)> messageCallback;
    std::function<String()> _monitorDataProvider;  // 实时监测数据提供者

    // 状态变化回调（SSE 推送用）
    StatusChangeCallback _statusChangeCallback;
    void _notifyStatusChange();  // 内部辅助方法

    // 后台重连任务（避免 reconnect() 阻塞 loopTask）
    volatile bool _reconnectPending;
    volatile bool _reconnectRunning;
    TaskHandle_t _reconnectTaskHandle;
    static void reconnectTaskEntry(void* param);
    void doReconnect();  // 实际执行重连（在后台任务中调用）

    // 线程安全：递归互斥量保护 publish 操作（PubSubClient 非线程安全）
    SemaphoreHandle_t _publishMutex = nullptr;

    // DATA_COMMAND 延迟处理队列（避免在 MQTT 回调中同步执行重操作）
    QueueHandle_t _dataCommandQueue = nullptr;
    void processQueuedCommands();

    // 异步任务上报环形缓冲区（PubSubClient 非线程安全，publish 必须在主循环执行）
    MqttSlot _reportSlots[MQTT_REPORT_SLOTS];  // 环形缓冲区 (~4KB)
    uint8_t _slotWriteIndex = 0;                // 写入位置
    uint8_t _slotReadIndex = 0;                 // 读取位置
    uint8_t _slotCount = 0;                     // 当前已占用的槽位数
    uint32_t _minReportInterval = 0;            // 最小上报间隔（ms），0=无限制
    unsigned long _lastReportQueueTime = 0;     // 上次入队上报数据的时间
    void processQueuedReports();

    void mqttCallback(char* topic, byte* payload, unsigned int length);
    bool reconnect();  // 同步重连（供 doReconnect 调用）
    String buildFullTopic(const String& topic, bool autoPrefix) const; // 根据主题级autoPrefix构建完整主题
    String buildFullTopicWithType(const String& topic, bool autoPrefix, MqttTopicType topicType) const; // 根据topicType和设备配置构建完整主题
    
    // FastBee认证相关方法
    String buildClientId();            // 构建认证clientId: 类型&设备编号&产品ID&用户ID
    String buildSimplePassword();      // 简单认证密码: password 或 password&authCode
    String buildEncryptedPassword();   // 加密认证密码: AES-CBC-128加密
    String getNtpTime(unsigned long& outDeviceSendTime);  // 通过HTTP获取NTP时间
    String aesEncrypt(const String& plainData, const String& key, const String& iv); // AES-CBC-128加密
};

#endif