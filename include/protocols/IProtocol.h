/**
 * @file IProtocol.h
 * @brief 协议接口抽象基类
 * @author kerwincui
 * @date 2025-12-02
 * 
 * 定义统一的协议接口，所有协议实现类都应继承此接口。
 * 解决 ProtocolManager 中 void* 类型不安全的问题。
 */

#ifndef IPROTOCOL_H
#define IPROTOCOL_H

#include <Arduino.h>
#include <functional>

/**
 * @brief 协议类型枚举
 */
enum class ProtocolType {
    MQTT = 0,
    MODBUS,
    TCP,
    HTTP,
    COAP,
    UNKNOWN
};

/**
 * @brief 协议状态枚举
 */
enum class ProtocolStatus {
    DISCONNECTED = 0,   // 未连接
    CONNECTING,         // 连接中
    CONNECTED,          // 已连接
    ERROR,              // 错误状态
    PROTOCOL_DISABLED   // 已禁用 (避免与 ESP32 HAL 的 DISABLED 宏冲突)
};

/**
 * @brief 协议消息回调类型
 */
using ProtocolMessageCallback = std::function<void(const String& topic, const String& message)>;

/**
 * @brief 协议接口抽象基类
 * 
 * 所有协议实现类都应继承此接口，提供统一的生命周期管理和数据操作接口。
 */
class IProtocol {
public:
    virtual ~IProtocol() = default;
    
    // ============ 生命周期管理 ============
    
    /**
     * @brief 初始化协议
     * @return 是否成功
     */
    virtual bool begin() = 0;
    
    /**
     * @brief 停止协议
     */
    virtual void stop() = 0;
    
    /**
     * @brief 主循环处理（需要在 loop 中调用）
     */
    virtual void loop() = 0;
    
    // ============ 数据操作 ============
    
    /**
     * @brief 发送数据
     * @param topic 主题/地址
     * @param data 数据内容
     * @return 是否成功
     */
    virtual bool send(const String& topic, const String& data) = 0;
    
    /**
     * @brief 设置消息回调
     * @param callback 回调函数
     */
    virtual void setMessageCallback(ProtocolMessageCallback callback) = 0;
    
    // ============ 状态查询 ============
    
    /**
     * @brief 获取协议类型
     * @return 协议类型
     */
    virtual ProtocolType getType() const = 0;
    
    /**
     * @brief 获取协议名称
     * @return 协议名称
     */
    virtual const char* getName() const = 0;
    
    /**
     * @brief 获取协议状态
     * @return 协议状态
     */
    virtual ProtocolStatus getStatus() const = 0;
    
    /**
     * @brief 获取状态描述字符串
     * @return 状态描述
     */
    virtual String getStatusString() const = 0;
    
    /**
     * @brief 检查是否已连接
     * @return 是否已连接
     */
    virtual bool isConnected() const = 0;
    
    // ============ 静态工具方法 ============
    
    /**
     * @brief 协议类型转字符串
     */
    static const char* typeToString(ProtocolType type) {
        switch (type) {
            case ProtocolType::MQTT:   return "MQTT";
            case ProtocolType::MODBUS: return "Modbus";
            case ProtocolType::TCP:    return "TCP";
            case ProtocolType::HTTP:   return "HTTP";
            case ProtocolType::COAP:   return "CoAP";
            default:                   return "Unknown";
        }
    }
    
    /**
     * @brief 协议状态转字符串
     */
    static const char* statusToString(ProtocolStatus status) {
        switch (status) {
            case ProtocolStatus::DISCONNECTED: return "Disconnected";
            case ProtocolStatus::CONNECTING:   return "Connecting";
            case ProtocolStatus::CONNECTED:    return "Connected";
            case ProtocolStatus::ERROR:        return "Error";
            case ProtocolStatus::PROTOCOL_DISABLED:     return "Disabled";
            default:                           return "Unknown";
        }
    }
};

#endif // IPROTOCOL_H
