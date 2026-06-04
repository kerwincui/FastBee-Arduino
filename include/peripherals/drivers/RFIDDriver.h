#ifndef RFID_DRIVER_H
#define RFID_DRIVER_H

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RFID

#include <Arduino.h>

/**
 * @brief MFRC522 RFID 驱动（SPI 接口）
 * 
 * 功能：
 *   - 周期性轮询 RFID 卡片存在
 *   - 读取卡片 UID（4/7/10 字节）
 *   - 检测到新卡时触发 EVENT_RFID_CARD_DETECTED 事件
 *   - 卡片移除时触发 EVENT_RFID_CARD_REMOVED 事件
 * 
 * 接线（ESP32-S3 默认 SPI）：
 *   - SDA/SS: 可配置（默认 GPIO5）
 *   - SCK:    GPIO18
 *   - MOSI:   GPIO23
 *   - MISO:   GPIO19
 *   - RST:    可配置（默认 GPIO27）
 * 
 * 编译条件：FASTBEE_ENABLE_RFID=1（仅 esp32s3-full）
 */
class RFIDDriver {
public:
    static RFIDDriver& getInstance();

    /**
     * @brief 初始化 RFID 读卡器
     * @param ssPin SPI SS/SDA 引脚
     * @param rstPin RST 复位引脚
     * @return 初始化是否成功
     */
    bool begin(uint8_t ssPin = 5, uint8_t rstPin = 27);

    /**
     * @brief 轮询检测卡片（由调度器定期调用）
     * 检测到新卡时自动触发事件
     */
    void check();

    /**
     * @brief 获取最近检测到的卡片 UID（十六进制字符串）
     * @return UID 字符串（如 "A1B2C3D4"），无卡时返回空串
     */
    String getLastUID() const { return _lastUID; }

    /**
     * @brief 卡片是否当前在感应范围内
     */
    bool isCardPresent() const { return _cardPresent; }

    /**
     * @brief 停止 RFID 驱动
     */
    void stop();

    bool isInitialized() const { return _initialized; }

private:
    RFIDDriver() = default;
    ~RFIDDriver();
    RFIDDriver(const RFIDDriver&) = delete;
    RFIDDriver& operator=(const RFIDDriver&) = delete;

    void* _mfrc522 = nullptr;       // MFRC522* (避免头文件暴露)
    bool _initialized = false;
    bool _cardPresent = false;
    String _lastUID;
    unsigned long _lastCheckTime = 0;
    unsigned long _cardDetectedTime = 0;

    static constexpr unsigned long CHECK_INTERVAL_MS = 200;        // 轮询间隔
    static constexpr unsigned long CARD_TIMEOUT_MS = 1000;         // 卡片超时（认为移除）
};

#endif // FASTBEE_ENABLE_RFID
#endif // RFID_DRIVER_H
