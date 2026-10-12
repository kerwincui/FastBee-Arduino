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
 * 接线必须按实际板卡预检；未显式填写的总线脚使用编译板型的
 * SCK/MOSI/MISO 常量，不代表所有 ESP32/S3 载板有通用接线。
 * 
 * 编译条件：FASTBEE_ENABLE_RFID=1
 */
class RFIDDriver {
public:
    static RFIDDriver& getInstance();
    struct Status {
        bool initialized = false;
        bool cardPresent = false;
        String uid;
        String cardType;
        String lastError;
    };
    // Nonblocking coherent snapshot; failure leaves the supplied snapshot intact.
    bool getStatus(Status& status, String& errorMsg) const;

    /**
     * @brief 初始化 RFID 读卡器
     * @param ssPin SPI SS/SDA 引脚
     * @param rstPin RST 复位引脚
     * @return 初始化是否成功
     */
    bool begin(uint8_t ssPin = 5, uint8_t rstPin = 27);
    bool begin(uint8_t ssPin, uint8_t rstPin, uint8_t sckPin,
               uint8_t mosiPin, uint8_t misoPin);

    /**
     * @brief 轮询检测卡片（由调度器定期调用）
     * 检测到新卡时自动触发事件
     */
    void check();

    /**
     * @brief 获取最近检测到的卡片 UID（十六进制字符串）
     * @return UID 字符串（如 "A1B2C3D4"），无卡时返回空串
     */
    String getLastUID() const;
    String getLastCardType() const;
    String getLastError() const;

    /**
     * @brief 卡片是否当前在感应范围内
     */
    bool isCardPresent() const;

    bool scanNow(String& uid, String& cardType, String& errorMsg);
    bool readBlock(uint8_t block, const uint8_t key[6], uint8_t data[16],
                   String& errorMsg);
    bool writeBlockVerified(uint8_t block, const uint8_t key[6],
                            const uint8_t data[16], String& errorMsg);

    /**
     * @brief 停止 RFID 驱动
     */
    // Failure leaves the reader/lease intact; caller must not release its pins.
    bool stop(String* errorMsg = nullptr);

    bool isInitialized() const;

private:
    RFIDDriver() = default;
    ~RFIDDriver();
    RFIDDriver(const RFIDDriver&) = delete;
    RFIDDriver& operator=(const RFIDDriver&) = delete;

    void* _mfrc522 = nullptr;       // MFRC522* (避免头文件暴露)
    bool _initialized = false;
    bool _cardPresent = false;
    String _lastUID;
    String _lastCardType;
    String _lastError;
    uint8_t _sckPin = 255;
    uint8_t _mosiPin = 255;
    uint8_t _misoPin = 255;
    uint8_t _ssPin = 255;
    uint8_t _rstPin = 255;
    unsigned long _lastCheckTime = 0;
    unsigned long _cardDetectedTime = 0;

    static constexpr unsigned long CHECK_INTERVAL_MS = 200;        // 轮询间隔
    static constexpr unsigned long CARD_TIMEOUT_MS = 1000;         // 卡片超时（认为移除）

    // All mutable state and hardware operations use SpiBusOperation. Private
    // helpers require that lock; rule callbacks must run after it is released.
    bool selectCard(String& errorMsg);
    bool authenticateBlock(uint8_t block, const uint8_t key[6], String& errorMsg);
    void finishCardOperation();
    static bool isWritableDataBlock(uint8_t block);
};

#endif // FASTBEE_ENABLE_RFID
#endif // RFID_DRIVER_H
