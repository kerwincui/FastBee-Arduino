#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <esp_random.h>
#include <vector>
#include <atomic>
#include <freertos/semphr.h>
#include <core/SystemConstants.h>
#include "core/ReportInstanceBinding.h"
#include "protocols/MqttReconnectLifetime.h"
#include "protocols/MqttReportRetirement.h"
#include "network/TlsTransportObservation.h"

enum class MqttPayloadEncoding : uint8_t {
    TEXT = 0,
    MODBUS_RTU_HEX = 1,
    CONFIGURED_TOPIC_TEXT = 2 // Already transformed; topic row belongs to this immutable client config.
};

// 固定大小的 MQTT 上报槽（零堆分配）
struct MqttSlot {
    char payload[513];    // 最多512字节payload，额外1字节用于字符串终止符
    uint16_t length;      // 实际 payload 长度
    MqttPayloadEncoding encoding; // 入队时固化，禁止消费时根据协议瞬时状态猜测
    uint16_t topicIndex = UINT16_MAX; // Only CONFIGURED_TOPIC_TEXT uses this field.
    bool occupied;        // 槽位是否被占用
    
    MqttSlot() : length(0), encoding(MqttPayloadEncoding::TEXT), occupied(false) {
        payload[0] = '\0';
    }
    
    void clear() {
        length = 0;
        encoding = MqttPayloadEncoding::TEXT;
        topicIndex = UINT16_MAX;
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
    OTA_BINARY    = 6,  // OTA二进制（当前不支持，运行时强制禁用）
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
    MqttTopicType topicType;

    MqttPublishTopic() : qos(0), retain(false), enabled(true), autoPrefix(false), topicType(MqttTopicType::DATA_REPORT) {}
};

// 订阅主题配置结构体
struct MqttSubscribeTopic {
    String topic;       // 订阅主题
    uint8_t qos;        // QoS等级
    bool enabled;       // 是否启用此订阅
    bool autoPrefix;    // 是否启用自动前缀（拼接topicPrefix到主题前）
    MqttTopicType topicType;
    
    MqttSubscribeTopic() : qos(0), enabled(true), autoPrefix(false), topicType(MqttTopicType::DATA_COMMAND) {}
};

// MQTT配置结构体
struct MQTTConfig {
    String scheme;              // 协议: "mqtt"(默认) 或 "mqtts"(TLS)
    String server;
    uint16_t port;
    String clientId;
    String username;
    String password;
    String topicPrefix;
    String otaBaseUrl;          // Legacy storage: HTTP/HTTPS base edited in Device OTA
    String subscribeTopic;      // 兼容旧配置的单一订阅主题
    uint16_t keepAlive;
    bool autoReconnect;        // 自动重连
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
    MQTTConfig() : scheme("mqtt"), port(1883), keepAlive(60),
                   autoReconnect(true), authType(MqttAuthType::SIMPLE),
                   willQos(0), willRetain(false) {}
};

class MQTTClient {
public:
    MQTTClient();
    ~MQTTClient();

    bool begin(uint32_t taskStartupDelayMs = 3000);
    void shutdown();  // Terminal drain; never call from this client's worker callback.
    bool connect();
    bool disconnect(); // False: transport owner busy; retain the instance.
    bool stop();           // false: worker cancellation queued; keep object/transport alive.
    bool publish(const String& topic, const String& message);
    bool publishToTopic(size_t topicIndex, const String& message);
    bool queueTopicMessage(size_t topicIndex, const String& message); // Local retention, never delivery confirmation.
    bool publishDeviceInfo();  // 发布设备信息到 topicType=DEVICE_INFO 的主题
    bool publishMonitorData(); // 发布一次实时监测数据到 topicType=REALTIME_MON 的主题
    bool publishReportData(const String& payload); // 发布数据上报到 topicType=DATA_REPORT 的主题
    bool publishModbusRtuReport(const String& hexPayload); // 显式二进制 RTU 上报，不依赖 handler 生命周期
    bool publishOtaReply(const String& payload);   // 发布 FastBee HTTPS OTA 状态（不经过规则脚本）
    bool hasUsableFastBeeOtaReplyTopic() const;    // 规范 reply 主题是否已启用（OTA 写 Flash 前预检）
    bool publishDeviceEvent(const String& eventId, const String& eventName, const String& eventData); // 发布设备事件到 topicType=DEVICE_EVENT 主题（格式同 DATA_REPORT）
    bool queueReportData(const String& payload);   // 线程安全入队上报数据（异步任务调用）
    bool queueReportData(const char* payload, uint16_t length); // 零拷贝版本
    bool queueModbusRtuReport(const String& hexPayload); // 固化 RAW 编码后入队
    uint8_t getQueueDepth() const; // Ring plus retained consumer slot.
    // ── 队列背压统计 ──
    struct QueueStats {
        uint32_t enqueueTotal;    // Accepted ring admissions, including replacement.
        uint32_t dropTotal;       // Invalid rejections + ring evictions; excludes throttling.
        uint32_t publishTotal;    // Local publish acceptance, not broker/platform receipt.
        uint32_t throttleDrops;   // 背压限流丢弃数
        uint32_t invalidDrops;
        uint32_t evictedOldest;
        uint32_t publishRejected;
        uint32_t allocationDeferred;
        uint32_t testModeRejected;
        uint32_t configuredTopicAccepted;
    };
    struct QueueSnapshot {
        uint8_t depth = 0, ringDepth = 0;
        bool pendingReport = false, consumerActive = false;
        uint32_t pendingAttempts = 0, retryDelayMs = 0;
        QueueStats stats{};
    };
    QueueSnapshot getReportQueueSnapshot() const;
    QueueStats getQueueStats() const;
    static FastBee::MqttReportRetirement::Snapshot getReportRetirementSnapshot();
    // Manager write lease required; annotates destruction, never clears/replays.
    void setReportRetirementReason(FastBee::MqttRetirementReason reason) { _reportRetirementReason = reason; }
    bool publishNtpSync();             // 发布 NTP 时间同步请求到 topicType=NTP_SYNC 的主题
    bool subscribe(const String& topic);
    // Dedicated configured remote channels: exact topics, no transforms/prefix,
    // no retained commands and no offline queue/replay.
    bool subscribeExact(const String& topic);
    bool unsubscribeExact(const String& topic);
    bool publishExact(const String& topic, const String& message);
    bool subscribeAll();  // 订阅所有配置的主题
    void handle();
    String getStatus() const;
    
    // 获取配置引用
    const MQTTConfig& getConfig() const { return config; }
    uint32_t getReportInstanceId() const { return _connectionTestOnly ? 0 : _reportInstanceId; }
    bool isConnectionTestOnly() const { return _connectionTestOnly; }
    const String& getConnectionTestId() const { return _connectionTestId; }
    
    // 详细状态信息
    bool getIsConnected() const { return isConnected; }
    bool isStopped() const { return stopped; }  // 是否被显式停止
    bool isReconnectWorkerActive() const { return _reconnectLifetime.active(); }
    bool isReconnectCancellationRequested() const { return _reconnectLifetime.stopping(); }
    int  getLastErrorCode() const { return lastErrorCode; }
    // 最近一次 MQTTS 失败是否由 TLS 内存准入/分配失败引起。
    // 供状态 API 区分网络超时与内存保护，不分配额外缓冲。
    bool isLastMqttsTlsMemoryFailure() const { return _lastMqttsTlsMemoryFailure; }
    // Runtime marker written by HealthMonitor when CRITICAL memory pressure
    // disables MQTT.  It is included in asynchronous SSE status events.
    bool isDisabledByMemory() const { return _disabledByMemory; }
    void setDisabledByMemory(bool disabled) { _disabledByMemory = disabled; }
    // Temporary memory-pressure stop, distinct from an explicit user stop.
    bool isStoppedByMemory() const { return _stoppedByMemory; }
    void setStoppedByMemory(bool stopped) { _stoppedByMemory = stopped; }
    uint32_t getReconnectCount() const { return reconnectCount; }
    unsigned long getLastConnectedTime() const { return lastConnectedTime; }
    bool isTlsTransportAllocated() const { return _tlsTransportAllocated.load(); }
    bool isWebPausedForMqtts() const { return _webServerPaused; }
    
    // MQTT 上报降采样：设置最小上报间隔（毫秒），0 表示无限制
    void setMinReportInterval(uint32_t ms);

    // 设置外部 Transport Client（由 NetworkManager 注入，用于非 WiFi 联网方式）
    bool setTransportClient(Client* client, bool secure = false); // Trusted provider only; before begin.

    // 重置错误计数器（网络类型切换后调用，清除慢模式状态）
    // 重置 consecutiveTimeouts、reconnectInterval、reconnectCount、lastErrorCode
    void resetErrorCounters();

    // Web memory pressure requests transport yielding. Try immediately; if a
    // loop/handshake owns the socket, defer to handle instead of stopping it concurrently.
    void yieldToWeb();

    // 设置消息回调（topic, message, topicType）
    void setMessageCallback(std::function<void(const String&, const String&, MqttTopicType)> callback);

    // 设置状态变化回调（SSE 推送用）
    typedef std::function<void(const String&)> StatusChangeCallback;
    void setStatusChangeCallback(StatusChangeCallback cb);

    // 设置实时监测数据提供者回调（返回 JSON 数组字符串）
    void setMonitorDataProvider(std::function<String()> provider);

    // 根据主题路径查找对应的主题类型（可选输出订阅主题索引）
    MqttTopicType getTopicTypeByPath(const String& topicPath, int8_t* outSubIndex = nullptr) const;

    // 获取数据上报主题的完整路径（透传模式调试日志用）
    String getReportTopic() const;

private:
    WiFiClient wifiClient;
    FastBee::TlsTransportObservation _tlsOwner;
    WiFiClientSecure* _wifiClientSecure = nullptr;  // MQTTS (TLS) 传输层（动态分配，失败时销毁释放 ~20KB TLS 内存）
    std::atomic<bool> _tlsTransportAllocated{false}; // Status never reads a concurrently deleted pointer.
    Client* _externalClient = nullptr;  // 外部注入的 Client（非 WiFi 时使用）
    bool _externalClientSecure = false; // Published with transport before begin; never inferred from scheme.
    PubSubClient mqttClient;
    MQTTConfig config;
    // Loaded once, before ProtocolManager publishes the instance to readers.
    bool loadMqttConfig(const String& filename = FileSystem::PROTOCOL_CONFIG_FILE);
    bool publishToTopicImpl(size_t topicIndex, const String& message, bool prepared);
    bool publishPreparedTopic(size_t topicIndex, const String& message);
    std::atomic<bool> _beginAttempted{false};
    bool _connectionTestOnly = false;
    String _connectionTestId;
    const uint32_t _reportInstanceId = FastBee::nextReportInstanceId();
    std::atomic<bool> isConnected;
    std::atomic<bool> stopped;         // Cancellation is visible to the reconnect worker.
    std::atomic<bool> _stopCompleted{false};
    unsigned long lastReconnectAttempt;
    std::atomic<unsigned long> lastConnectedTime;  // Individually sampled status observations.
    std::atomic<int> lastErrorCode;
    std::atomic<uint32_t> reconnectCount;
    uint32_t reconnectInterval;       // 当前重连间隔（指数退避，毫秒）
    uint8_t  consecutiveTimeouts;     // 连续超时次数（用于检测 DNS 故障）
    unsigned long lastLoopTime;       // 上次loop()调用的时间（用于检测连接健康）
    
    // 实时监测状态
    bool monitorActive = false;       // 是否正在执行实时监测
    int monitorRemaining = 0;         // 剩余发布次数
    unsigned long monitorInterval = 1000; // 发布间隔（毫秒）
    unsigned long lastMonitorTime = 0;    // 上次发布监测数据的时间
    
    // NTP 时间同步状态
    unsigned long long ntpDeviceSendTime = 0;  // Monotonic milliseconds, no 49-day token reuse
    uint32_t ntpTimeSequence = 0;
    uint32_t ntpRequestConnection = 0;
    std::atomic<uint32_t> ntpConnectionEpoch{0};
    bool ntpRequestPending = false;
    std::atomic_flag ntpRequestBusy = ATOMIC_FLAG_INIT;
    
    std::function<void(const String&, const String&, MqttTopicType)> messageCallback;
    std::function<String()> _monitorDataProvider;  // 实时监测数据提供者

    // 状态变化回调（SSE 推送用）
    StatusChangeCallback _statusChangeCallback;
    void _notifyStatusChange();  // 内部辅助方法

    // 后台重连任务（避免 reconnect() 阻塞 loopTask）
    std::atomic<bool> _reconnectPending;
    std::atomic<bool> _reconnectRunning;
    FastBee::MqttReconnectLifetime _reconnectLifetime;
    uint32_t _taskStartupDelayMs;  // 重连任务启动延迟（ms），首次启动 3000ms，deferred 重启 500ms
    // Watchdog requests cooperative cancellation; it never kills a task that
    // may own TLS/socket/allocator locks or resumes a second worker over it.
    std::atomic<unsigned long> _reconnectScheduledMs{0};
    static constexpr uint32_t RECONNECT_WATCHDOG_TIMEOUT_MS = 90000;  // 重连执行最大允许时长（ms）
    // 8BIT 卡滞救援：MQTT 断开后 free8 <8KB 的起始时间 + 上次 WiFi 救援时间
    unsigned long _low8SinceMs = 0;
    unsigned long _lastWifiRescueMs = 0;
    std::atomic<bool> _lastMqttsTlsMemoryFailure{false};
    std::atomic<bool> _disabledByMemory{false};
    std::atomic<bool> _stoppedByMemory{false};
    bool _mqttsMemoryBackoffActive = false;
    static void reconnectTaskEntry(void* param);
    bool ensureReconnectTask();  // 按需创建重连任务（连接时任务已删除，断开时重新创建）
    void doReconnect();  // 实际执行重连（在后台任务中调用）
    void recoverStuckReconnect(const char* reason);  // Request cancellation, retain lifetime.

    // MQTTS TLS 内存管理：动态创建/销毁 WiFiClientSecure，确保失败后释放全部 TLS DRAM
    void ensureTlsTransport();    // 按需创建 _wifiClientSecure（已存在则跳过）
    bool validateTransportSecurity(); // Reject untyped/plain external clients for MQTTS.
    void releaseTlsTransport();   // 销毁 _wifiClientSecure 释放 TLS 上下文和缓冲区
    bool reclaimDramForMqtts(bool pauseWeb = false);   // 回收 TLS/SSE 内存；以太网 MQTTS 可短暂暂停 Web 换取连续 DRAM
    void resumeWebServices();     // 显式暂停路径使用后恢复 Web 服务
    std::atomic<bool> _webServerPaused{false}; // MQTTS 深度回收时短暂停止 Web，握手后恢复
    bool _mdnsPausedForMqtts = false;
    // 前台推迟超限后的强制深度暂停标志（一次性，reclaimDramForMqtts 消费后清零）
    bool _mqttsForceWebPause = false;

    // One recursive owner for ALL PubSubClient, socket/TLS and mutable protocol
    // state. Public operations try without waiting; callbacks may reenter on
    // the owner task. Never hold this while waiting for worker acknowledgement.
    SemaphoreHandle_t _publishMutex = nullptr;
    std::atomic<bool> _webYieldPending{false};

    // 查找已启用的发布主题索引（按 topicType），找不到返回 -1
    int findEnabledPublishTopicIndex(MqttTopicType type) const;
    int findFastBeeOtaReplyTopicIndex() const;
    void normalizeFastBeeOtaTopics();

    // DATA_COMMAND 延迟处理队列（避免在 MQTT 回调中同步执行重操作）
    QueueHandle_t _dataCommandQueue = nullptr;
    uint32_t _cmdQueueTotalBytes = 0;  // Task 4.3: 队列当前总内存占用跟踪
    static constexpr uint32_t MQTT_CMD_QUEUE_MAX_BYTES = 4096U;  // 队列总内存上限
    void processQueuedCommands();

    // 异步任务上报环形缓冲区（PubSubClient 非线程安全，publish 必须在主循环执行）
    MqttSlot _reportSlots[MQTT_REPORT_SLOTS];  // 环形缓冲区 (~4KB)
    FastBee::MqttRetirementReason _reportRetirementReason = FastBee::MqttRetirementReason::Unspecified;
    bool _reportRetirementRecorded = false;
    void recordReportQueueRetirement(); // Destructor only, after all callers/workers drain.
    MqttSlot _pendingReportSlot;               // 发布失败时保留，避免异步 Modbus 响应丢失
    uint8_t _slotWriteIndex = 0;                // 写入位置
    uint8_t _slotReadIndex = 0;                 // 读取位置
    uint8_t _slotCount = 0;                     // 当前已占用的槽位数
    // 环形缓冲索引自旋锁：生产者(pexec worker任务)与消费者(loopTask)跨核并发，
    // 三个索引的读改写必须在同一临界区内完成（临界区仅含索引操作+memcpy，微秒级）
    mutable portMUX_TYPE _slotMux = portMUX_INITIALIZER_UNLOCKED;
    uint32_t _minReportInterval = 0;            // 最小上报间隔（ms），0=无限制
    uint32_t _lastReportQueueTime = 0;          // Last accepted admission.
    bool _reportQueueAttempted = false;
    bool _reportConsumerActive = false;
    bool _reportFlushAttempted = false;
    uint32_t _lastReportFlushMs = 0;
    uint32_t _pendingReportAttempts = 0;
    static void incrementQueueCounter(uint32_t& value) { if (value != UINT32_MAX) ++value; }
    bool rejectInvalidReport();
    void processQueuedReports();
    bool queueReportDataEncoded(const char* payload, uint16_t length,
                                MqttPayloadEncoding encoding, uint16_t topicIndex = UINT16_MAX);
    
    // ── 队列背压统计与限流 ──
    QueueStats _queueStats = {};
    static constexpr uint8_t BACKPRESSURE_HIGH_WATERMARK = 6;  // 队列深度 >= 6 时启用加速排空
    static constexpr uint8_t BACKPRESSURE_DRAIN_BATCH = 6;     // 背压模式下每次排空数量（正常为 4）

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
