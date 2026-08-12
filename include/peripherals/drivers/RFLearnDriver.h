#ifndef RF_LEARN_DRIVER_H
#define RF_LEARN_DRIVER_H

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RF_LEARN

#include <Arduino.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/**
 * @brief 433MHz OOK 遥控学习驱动（EV1527 兼容）
 *
 * 功能：
 *   - 基于 GPIO 中断的脉冲时序捕获（无需外部库）
 *   - 自动解码 EV1527/PT2262 风格 OOK 编码（同步头 + 数据位）
 *   - 学习模式（对码）：限时窗口捕获下一个有效编码
 *   - 按键映射（名称→编码+脉宽+位长）：持久化到 /config/rf_keys.json
 *   - 解码成功后触发 rf_code_received 事件；命中按键映射时携带 key 字段
 *
 * 接线：
 *   - 433MHz RX DATA → 配置的 GPIO 引脚
 *   - RX VCC → 3.3V / 5V（视模块）
 *   - RX GND → GND
 *
 * 编译条件：FASTBEE_ENABLE_RF_LEARN=1
 */

/** @brief 学习模式状态 */
enum class RfLearnState : uint8_t {
    IDLE = 0,      // 空闲
    LEARNING = 1,  // 学习中（等待编码）
    CAPTURED = 2,  // 已捕获编码
    TIMEOUT = 3    // 学习超时
};

/** @brief RF 按键映射条目 */
struct RfKeyEntry {
    String name;          // 按键名称（唯一标识）
    uint32_t code;        // 编码值
    uint8_t bitLength;    // 位长（通常 24）
    uint16_t pulseWidth;  // 脉宽 μs
};

class RFLearnDriver {
public:
    static RFLearnDriver& getInstance();

    /**
     * @brief 初始化 RF 接收器
     * @param rxPin 接收器数据引脚
     * @return 初始化是否成功
     */
    bool begin(uint8_t rxPin);

    /**
     * @brief 定期检查是否有解码成功的编码（由调度器调用）
     * 学习模式下捕获编码；否则派发 rf_code_received 事件
     */
    void check();

    /** @brief 停止接收 */
    void stop();

    bool isInitialized() const { return _initialized; }
    uint8_t getRxPin() const { return _rxPin; }

    /** @brief 最近解码的编码（十六进制 "0x..."） */
    String getLastCode() const { return _lastCode; }
    uint32_t getLastValue() const { return _lastValue; }
    uint8_t getLastBitLength() const { return _lastBitLength; }
    uint16_t getLastPulseWidth() const { return _lastPulseWidth; }

    // ============ 学习模式（对码）============

    void startLearn(uint32_t timeoutMs = 10000);
    void stopLearn();
    bool isLearning() const { return _learnState == RfLearnState::LEARNING; }
    RfLearnState getLearnState();
    String getLearnStateName();
    String getLearnedCode() const { return _learnedCode; }
    uint8_t getLearnedBitLength() const { return _learnedBitLength; }
    uint16_t getLearnedPulseWidth() const { return _learnedPulseWidth; }
    uint32_t getLearnRemainingMs();

    // ============ 按键映射 ============

    bool loadKeyMap();
    size_t getKeyCount() const { return _keyMap.size(); }
    String resolveKeyName(uint32_t code, uint8_t bitLength) const;
    String getKeysJson() const;
    bool upsertKey(const String& name, const String& code, uint8_t bitLength, uint16_t pulseWidth, String& err);
    bool deleteKey(const String& name);
    bool clearKeys();

    // ============ ISR 数据（供中断服务程序写入）============
    static volatile uint32_t _isrLastEdgeTime;
    static volatile uint16_t _isrPulseBuffer[];
    static volatile uint8_t _isrPulseCount;
    static volatile bool _isrFrameReady;

private:
    RFLearnDriver() = default;
    ~RFLearnDriver();
    RFLearnDriver(const RFLearnDriver&) = delete;
    RFLearnDriver& operator=(const RFLearnDriver&) = delete;

    static void IRAM_ATTR _isrHandler();
    bool _decodeFrame();
    void _updateLearnTimeout();
    bool _persistKeyMap();

    uint8_t _rxPin = 255;
    bool _initialized = false;
    SemaphoreHandle_t _mutex = nullptr;  // 保护 check/stop/begin 并发安全

    // 最近解码结果
    String _lastCode;
    uint32_t _lastValue = 0;
    uint8_t _lastBitLength = 0;
    uint16_t _lastPulseWidth = 0;
    unsigned long _lastEventTime = 0;

    // 学习模式
    RfLearnState _learnState = RfLearnState::IDLE;
    unsigned long _learnDeadline = 0;
    String _learnedCode;
    uint8_t _learnedBitLength = 0;
    uint16_t _learnedPulseWidth = 0;

    // 按键映射
    std::vector<RfKeyEntry> _keyMap;
    bool _keyMapLoaded = false;

    static constexpr unsigned long MIN_EVENT_INTERVAL_MS = 300;  // 防重复触发间隔
    static constexpr size_t MAX_RF_KEYS = 40;
    static constexpr uint8_t ISR_BUFFER_SIZE = 128;              // 脉冲缓冲（sync + 24*2 + margin）
    static constexpr uint16_t SYNC_MIN_US = 3000;               // 同步头最小持续时间
    static constexpr uint8_t MIN_DATA_BITS = 12;
    static constexpr uint8_t MAX_DATA_BITS = 32;
};

#endif // FASTBEE_ENABLE_RF_LEARN
#endif // RF_LEARN_DRIVER_H
