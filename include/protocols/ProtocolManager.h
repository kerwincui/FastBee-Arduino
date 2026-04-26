#ifndef PROTOCOL_MANAGER_H
#define PROTOCOL_MANAGER_H

#include <Arduino.h>
#include <memory>
#include <vector>
#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_MQTT
#include "MQTTClient.h"
#endif
#if FASTBEE_ENABLE_MODBUS
#include "ModbusHandler.h"
#endif
#include "TCPHandler.h"
#include "CoAPHandler.h"
#include "HTTPClientWrapper.h"
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
    MODBUS,
    TCP,
    HTTP,
    COAP
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
    bool restartMQTTDeferred();

    /**
     * @brief 停止MQTT连接
     */
    void stopMQTT();
#endif

#if FASTBEE_ENABLE_MODBUS
    /**
     * @brief 重启Modbus（从protocol.json加载配置并初始化）
     * 注意：此方法栈开销大（~12KB），只能在 loopTask 中调用
     */
    bool restartModbus();

    /**
     * @brief 延迟重启Modbus（设置标志，由 handle() 在 loopTask 中执行）
     * 用于 HTTP handler 中避免在 AsyncTCP 小栈任务中执行
     */
    bool restartModbusDeferred();

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
#endif
#if FASTBEE_ENABLE_TCP
    std::unique_ptr<TCPHandler> tcpHandler;
#endif
#if FASTBEE_ENABLE_HTTP
    std::unique_ptr<HTTPClientWrapper> httpClientWrapper;
#endif
#if FASTBEE_ENABLE_COAP
    std::unique_ptr<CoAPHandler> coapHandler;
#endif
    
    std::vector<ProtocolConfig> protocols;
    MessageCallback messageCallback;
    CounterCallback txCallback;
    CounterCallback rxCallback;
    SSEBroadcastCallback sseCallback;
    SimpleSSECallback mqttStatusSSECallback;
    SimpleSSECallback modbusStatusSSECallback;
    
    bool isInitialized;
#if FASTBEE_ENABLE_MODBUS
    volatile bool modbusRestartPending;  // 延迟重启标志，由 handle() 在 loopTask 中检查
#endif
    
    // 初始化具体协议
#if FASTBEE_ENABLE_MQTT
    bool initMQTT(void* config);
#endif
#if FASTBEE_ENABLE_MODBUS
    bool initModbus(void* config);
#endif
#if FASTBEE_ENABLE_TCP
    bool initTCP(void* config);
#endif
#if FASTBEE_ENABLE_HTTP
    bool initHTTP(void* config);
#endif
#if FASTBEE_ENABLE_COAP
    bool initCoAP(void* config);
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
