#ifndef IR_REMOTE_DRIVER_H
#define IR_REMOTE_DRIVER_H

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_IR_REMOTE

#include <Arduino.h>

/**
 * @brief 红外遥控接收驱动（基于 IRremoteESP8266）
 * 
 * 功能：
 *   - 使用硬件中断接收红外信号
 *   - 支持 NEC/RC5/RC6/SONY/Samsung/LG/Panasonic 等协议自动解码
 *   - 解码成功后触发 EVENT_IR_CODE_RECEIVED 事件
 *   - 支持原始码和协议特定码输出
 * 
 * 接线：
 *   - IR 接收器 OUT → 配置的 GPIO 引脚（默认 GPIO15）
 *   - IR 接收器 VCC → 3.3V
 *   - IR 接收器 GND → GND
 * 
 * 编译条件：FASTBEE_ENABLE_IR_REMOTE=1（仅 esp32s3-full）
 */
class IRRemoteDriver {
public:
    static IRRemoteDriver& getInstance();

    /**
     * @brief 初始化红外接收器
     * @param recvPin 接收器数据引脚
     * @param bufferSize 接收缓冲区大小（默认 1024）
     * @return 初始化是否成功
     */
    bool begin(uint8_t recvPin = 15, uint16_t bufferSize = 1024);

    /**
     * @brief 检查是否有新的红外编码接收（由调度器定期调用）
     * 解码成功时自动触发事件
     */
    void check();

    /**
     * @brief 获取最近接收到的红外编码信息
     * @return 格式 "PROTOCOL:0xCODE"（如 "NEC:0x00FF6897"）
     */
    String getLastCode() const { return _lastCode; }

    /**
     * @brief 获取最近接收的协议名称
     */
    String getLastProtocol() const { return _lastProtocol; }

    /**
     * @brief 获取最近接收的原始编码值
     */
    uint64_t getLastValue() const { return _lastValue; }

    /**
     * @brief 停止红外接收器
     */
    void stop();

    bool isInitialized() const { return _initialized; }

private:
    IRRemoteDriver() = default;
    ~IRRemoteDriver();
    IRRemoteDriver(const IRRemoteDriver&) = delete;
    IRRemoteDriver& operator=(const IRRemoteDriver&) = delete;

    void* _irRecv = nullptr;        // IRrecv* (避免头文件暴露)
    void* _results = nullptr;       // decode_results*
    bool _initialized = false;
    String _lastCode;
    String _lastProtocol;
    uint64_t _lastValue = 0;
    unsigned long _lastEventTime = 0;

    static constexpr unsigned long MIN_EVENT_INTERVAL_MS = 150;  // 防重复触发间隔
};

#endif // FASTBEE_ENABLE_IR_REMOTE
#endif // IR_REMOTE_DRIVER_H
