#ifndef PROTOCOL_MANAGER_H
#define PROTOCOL_MANAGER_H

#include <Arduino.h>
#include <memory>
#include <vector>
#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_MQTT
#include "MQTTClient.h"
#include <freertos/queue.h>
#endif
#if FASTBEE_ENABLE_MODBUS
#include "ModbusHandler.h"
#endif
#include "core/PeripheralManager.h"

// 前向声明 — 用于条件编译禁用时的 stub getter 返回类型
#if !FASTBEE_ENABLE_MODBUS
class ModbusHandler;
#endif
#if !FASTBEE_ENABLE_MQTT
class MQTTClient;
#endif

// 协议类型枚举
enum class ProtocolType {
    MQTT,
    MODBUS
};

// 协议配置结构体
struct ProtocolConfig {
    ProtocolType type;
    String name;
    bool enabled;
    void* config;  // 指向具体协议的配置
};

// 回调函数类型定义
typedef std::function<void(ProtocolType, const String&, const String&)> MessageCallback;
typedef std::function<void()> CounterCallback;
typedef std::function<void(uint8_t, const String&)> SSEBroadcastCallback;
typedef std::function<void(const String&)> SimpleSSECallback;  // 简化版 SSE 回调（无地址参数）

// Modbus 数据来源枚举（用于统一分发出口）
enum class ModbusDataSource {
    LiveCallback,    // 实时回调（MQTT直报 + SSE + 规则匹配）
    PeriphExecPoll   // PeriphExec轮询（仅MQTT上报，不分发到规则匹配）
};

#if FASTBEE_ENABLE_MODBUS
// Modbus 配置可以只包含尚未接线、默认禁用的设备模板。该状态不是启动
// 故障，调用方必须与解析、内存或 UART 初始化失败区分处理。
enum class ModbusRestartResult : uint8_t {
    Started,
    IdleNoEnabledTargets,
    DeferredBusy,
    Failed
};
#endif

class ProtocolManager {
public:
    ProtocolManager();
    ~ProtocolManager();

    // 初始化协议管理器
    bool initialize();
    
    // 添加协议
    bool addProtocol(ProtocolType type, const String& name, bool enabled = true);
    
    // 设置协议配置
    bool setProtocolConfig(ProtocolType type, void* config);
    
    // 启动所有协议
    bool startAll();
    
    // 停止所有协议
    void stopAll();
    
    // 发送数据
    bool sendData(ProtocolType type, const String& topic, const String& data);
    
    // 注册消息回调
    void setMessageCallback(MessageCallback callback);
    
    // 获取协议状态
    String getProtocolStatus(ProtocolType type);
    
    // 处理循环（需要在 loop 中调用）
    void handle();

    // 获取 ModbusHandler 指针（供路由处理器访问Master模式接口）
#if FASTBEE_ENABLE_MODBUS
    class ModbusHandlerLease {
    public:
        ModbusHandlerLease() : _owner(nullptr), _handler(nullptr) {}
        ~ModbusHandlerLease() { release(); }
        ModbusHandlerLease(ModbusHandlerLease&& other) noexcept
            : _owner(other._owner), _handler(other._handler) {
            other._owner = nullptr;
            other._handler = nullptr;
        }
        ModbusHandlerLease& operator=(ModbusHandlerLease&& other) noexcept {
            if (this != &other) {
                release();
                _owner = other._owner;
                _handler = other._handler;
                other._owner = nullptr;
                other._handler = nullptr;
            }
            return *this;
        }
        ModbusHandler* get() const { return _handler; }
        ModbusHandler* operator->() const { return _handler; }
        explicit operator bool() const { return _handler != nullptr; }

        ModbusHandlerLease(const ModbusHandlerLease&) = delete;
        ModbusHandlerLease& operator=(const ModbusHandlerLease&) = delete;

    private:
        friend class ProtocolManager;
        ModbusHandlerLease(ProtocolManager* owner, ModbusHandler* handler)
            : _owner(owner), _handler(handler) {}
        void release();
        ProtocolManager* _owner;
        ModbusHandler* _handler;
    };

    // 获取 handler 活跃引用。生命周期互斥只保护快照/计数，不会在整个
    // UART 操作期间持有；热重启看到活跃引用时延期，避免 end/reset 竞态。
    ModbusHandlerLease acquireModbusHandler(
        TickType_t timeout = pdMS_TO_TICKS(2000)) const;

    ModbusHandler* getModbusHandler() const { return modbusHandler.get(); }
#else
    ModbusHandler* getModbusHandler() const { return nullptr; }
#endif

    // 获取 MQTTClient 指针（供路由处理器访问MQTT状态接口）
#if FASTBEE_ENABLE_MQTT
    MQTTClient* getMQTTClient() const { return mqttClient.get(); }
#else
    MQTTClient* getMQTTClient() const { return nullptr; }
#endif

#if FASTBEE_ENABLE_MQTT
    /**
     * @brief 重启MQTT连接（重新加载配置并连接）
     */
    bool restartMQTT();

    /**
     * @brief 非阻塞重启MQTT（重新加载配置，由loop自动重连）
     * 用于HTTP handler中避免阻塞Web服务器
     */
    bool restartMQTTDeferred(bool forceRebuild = false);

    /**
     * @brief 异步请求重启MQTT（仅置标志，由 handle() 在 loopTask 中执行）
     * 用于保存协议配置后热应用新配置，避免在 AsyncTCP 小栈任务中
     * 执行 TLS 级别的销毁/重建；若热重建内存门槛不过则自动回退调度设备重启
     */
    void requestMqttRestartAsync();

    /**
     * @brief 停止MQTT连接
     */
    void stopMQTT();

    /**
     * @brief 标记 MQTT 已被配置禁用（供调试打印可观测区分）
     * HealthMonitor 因内存危急通过配置永久禁用 MQTT 时调用以同步状态
     */
    void markMqttDisabledByConfig() { mqttDisabledByConfig = true; }
#endif

#if FASTBEE_ENABLE_MODBUS
    /**
     * @brief 重启Modbus（从protocol.json加载配置并初始化）
     * 注意：此方法栈开销大（~12KB），只能在 loopTask 中调用
     */
    ModbusRestartResult restartModbus();

    /**
     * @brief 延迟重启Modbus（设置标志，由 handle() 在 loopTask 中执行）
     * 用于 HTTP handler 中避免在 AsyncTCP 小栈任务中执行
     */
    bool restartModbusDeferred();

    // 延迟重启是否仍在等待 loopTask 消费。用于内存恢复路径避免在
    // handler 实际重建前重复排队或提前清除“Modbus 已停止”状态。
    bool isModbusRestartPending() const { return modbusRestartPending; }

    // 读取并清除最近一次实际重启结果。restartModbusDeferred() 只负责
    // 排队，HealthMonitor 通过该一次性结果区分“模板全部禁用”的正常
    // 空闲状态与需要退避重试的真实失败。
    bool consumeModbusRestartResult(ModbusRestartResult& result);

    /**
     * @brief 停止Modbus
     */
    void stopModbus();
#endif

    /**
     * @brief 完全关闭协议管理器，释放所有资源
     */
    void shutdown();

    // 设置 TX/RX 计数回调
    void setTxCallback(CounterCallback cb);
    void setRxCallback(CounterCallback cb);

    // 设置 SSE 广播回调
    void setSSECallback(SSEBroadcastCallback callback);

    // 设置 MQTT/Modbus 状态 SSE 回调
    void setMQTTStatusSSECallback(SimpleSSECallback callback);
    void setModbusStatusSSECallback(SimpleSSECallback callback);

    // Modbus 数据统一分发出口
    // LiveCallback: MQTT上报 + SSE广播 + 规则匹配
    // PeriphExecPoll: 仅MQTT上报（PeriphExec有自己的规则匹配流程）
    void dispatchModbusData(uint8_t slaveAddress, const String& data, ModbusDataSource source);

private:
    // unique_ptr 智能指针，主要用于自动管理动态内存，避免内存泄漏并确保资源安全释放
#if FASTBEE_ENABLE_MQTT
    std::unique_ptr<MQTTClient> mqttClient;
#endif
#if FASTBEE_ENABLE_MODBUS
    std::unique_ptr<ModbusHandler> modbusHandler;
    mutable SemaphoreHandle_t modbusLifecycleMutex = nullptr;
    mutable portMUX_TYPE modbusLeaseMux = portMUX_INITIALIZER_UNLOCKED;
    mutable uint16_t modbusActiveLeases = 0;
#endif
    
    std::vector<ProtocolConfig> protocols;
    MessageCallback messageCallback;
    CounterCallback txCallback;
    CounterCallback rxCallback;
    SSEBroadcastCallback sseCallback;
    SimpleSSECallback mqttStatusSSECallback;
    SimpleSSECallback modbusStatusSSECallback;
    
    bool isInitialized;
    bool shutdownComplete = false;
#if FASTBEE_ENABLE_MODBUS
    volatile bool modbusRestartPending;  // 延迟重启标志，由 handle() 在 loopTask 中检查
    volatile bool modbusStopPending = false;
    volatile bool modbusShutdownPending = false;
    volatile bool modbusRestartResultAvailable = false;
    ModbusRestartResult modbusLastRestartResult = ModbusRestartResult::Failed;
#endif
#if FASTBEE_ENABLE_MQTT
    volatile bool mqttRestartPending = false;  // MQTT 热重建标志，由 handle() 在 loopTask 中消费
    bool mqttDisabledByConfig = false;         // 配置 mqtt.enabled=false，由统一守卫置位，调试打印区分“禁用”与“启动失败”

    struct FastBeeOtaReplyEvent {
        int32_t taskId;
        uint8_t status;
        uint8_t progress;
        char version[32];
        char error[96];
    };
    QueueHandle_t fastBeeOtaActiveTerminalQueue = nullptr; // active task status 3/4
    QueueHandle_t fastBeeOtaReplyQueue = nullptr;          // non-owner terminal replies
    QueueHandle_t fastBeeOtaProgressQueue = nullptr;       // latest status 2 only
    FastBeeOtaReplyEvent fastBeeOtaPendingReply{};
    bool fastBeeOtaHasPendingReply = false;
    unsigned long fastBeeOtaLastPublishAttemptMs = 0;
    portMUX_TYPE fastBeeOtaStateMux = portMUX_INITIALIZER_UNLOCKED;
    int32_t fastBeeOtaActiveTaskId = 0;
    int32_t fastBeeOtaLastTerminalTaskId = 0;
    uint8_t fastBeeOtaLastTerminalStatus = 0;
    char fastBeeOtaLastTerminalVersion[32] = {};
#endif
    
    // 网络类型追踪：用于在 handle() 中检测网络类型变更并立即重启 MQTT
    // 存储为 uint8_t 避免在头文件中引入 NetworkType 依赖
    uint8_t _lastNetworkType = 0;  // 默认 NET_WIFI=0
    bool _mqttRestartOnTypeChange = false;  // 网络类型变更后需要重启 MQTT 的标志

    // 初始化具体协议
#if FASTBEE_ENABLE_MQTT
    bool initMQTT(void* config);
    void handleMqttMessage(const String& topic, const String& message, MqttTopicType topicType);
    void handleFastBeeOtaCommand(const String& message, MqttTopicType topicType);
    void queueFastBeeOtaReply(int32_t taskId, const String& version,
                              uint8_t status, uint8_t progress,
                              const String& error = "");
    void processFastBeeOtaReplies();
#endif
#if FASTBEE_ENABLE_MODBUS
    bool initModbus(void* config);
    ModbusHandlerLease acquireModbusHandlerInternal(TickType_t timeout,
                                                    bool allowPending) const;
    void releaseModbusHandlerLease();
    uint16_t getActiveModbusLeaseCount() const;
#endif
    
    // 内部消息处理
    void handleMessage(ProtocolType type, const String& topic, const String& message);

#if FASTBEE_ENABLE_MODBUS
    // MQTT触发的Modbus一次性读取
    String executeModbusRead(const String& paramsJson);
    
    // MQTT触发的Modbus原始HEX帧透传（平台下发 → 设备转发 → 上报响应）
    String executeModbusRawSend(const String& hexPayload);
    
    // Modbus 子设备注册/注销到 PeripheralManager
    void registerModbusSubDevices(const ModbusConfig& config);
    void unregisterModbusSubDevices();
#endif

    // 收集本地传感器类外设数据（GPIO/ADC），返回 JSON 数组字符串
    String collectLocalSensorData() const;
};

#endif
