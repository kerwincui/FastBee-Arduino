/**
 * @file LoRaAdapter.h
 * @brief LoRa 网关透传适配器 (E22-400T22D)
 * @author kerwincui
 * @date 2026-06-02
 * 
 * LoRa 网关透传模式：ESP32 将 MQTT 数据包通过 LoRa 串口发送，
 * 网关端负责通过 TCP/IP 转发到 MQTT Broker。
 * E22-400T22D 单次最大 240 字节，大包需分帧处理。
 */

#ifndef LORA_ADAPTER_H
#define LORA_ADAPTER_H

#include "core/FeatureFlags.h"

#if FASTBEE_ENABLE_LORA

#include <Arduino.h>
#include <HardwareSerial.h>
#include <Client.h>
#include "network/WiFiManager.h"

// LoRa 帧大小限制 (E22-400T22D 透传模式最大 240 字节)
#define LORA_MAX_FRAME_SIZE 240
#define LORA_RX_BUFFER_SIZE 512

/**
 * @class LoRaClient
 * @brief LoRa Client 包装器（实现 Arduino Client 接口）
 * 
 * 将 Arduino Client 的 connect/write/read 接口映射到 LoRa 串口收发。
 * connect() 不做实际 TCP 连接，只标记为"已连接"。
 * MQTT 包直接通过 LoRa 射频发送给网关。
 */
class LoRaClient : public Client {
public:
    LoRaClient();
    
    void setSerial(HardwareSerial* serial);

    // Arduino Client 接口实现
    int connect(IPAddress ip, uint16_t port) override;
    int connect(const char* host, uint16_t port) override;
    size_t write(uint8_t b) override;
    size_t write(const uint8_t* buf, size_t size) override;
    int available() override;
    int read() override;
    int read(uint8_t* buf, size_t size) override;
    int peek() override;
    void flush() override;
    void stop() override;
    uint8_t connected() override;
    operator bool() override;

private:
    HardwareSerial* _serial = nullptr;
    bool _connected = false;
    
    // 接收缓冲区
    uint8_t _rxBuffer[LORA_RX_BUFFER_SIZE];
    size_t _rxHead = 0;
    size_t _rxTail = 0;
    
    void pollSerial();
};

/**
 * @class LoRaAdapter
 * @brief LoRa 网关透传适配器 (E22-400T22D)
 */
class LoRaAdapter {
public:
    LoRaAdapter();
    ~LoRaAdapter();

    /**
     * @brief 初始化 LoRa 模块
     * @param config 网络配置（含 LoRa 引脚配置）
     * @return 初始化是否成功
     */
    bool begin(const WiFiConfig& config);

    /**
     * @brief 检查 LoRa 模块是否在线
     */
    bool isConnected() const;

    /**
     * @brief 断开/停止 LoRa 模块
     */
    void disconnect();

    /**
     * @brief 获取 Arduino Client 指针（用于 PubSubClient）
     */
    Client* getClient();

    /**
     * @brief 获取连接状态字符串
     */
    String getStatusString() const;

    /**
     * @brief 检查是否为透传模式
     */
    bool isTransparentMode() const;

    /**
     * @brief 获取设备地址
     */
    uint16_t getAddress() const;

    /**
     * @brief 获取频率字符串
     */
    String getFrequencyString() const;

    /**
     * @brief 获取空中速率字符串
     */
    String getAirRateString() const;

    /**
     * @brief 获取信道
     */
    uint8_t getChannel() const;

    /**
     * @brief 更新状态
     */
    void update();

private:
    HardwareSerial* _serial = nullptr;
    LoRaClient _loraClient;
    LoRaConfig _pinConfig;
    bool _initialized = false;
    bool _transparentMode = true;
    
    // LoRa 参数缓存
    uint16_t _address = 0;
    uint8_t _channel = 0;
    uint8_t _airRate = 0;
    uint32_t _frequency = 0;

    /**
     * @brief 设置 LoRa 模块为透传模式
     */
    void setTransparentMode();
};

#endif // FASTBEE_ENABLE_LORA
#endif // LORA_ADAPTER_H
