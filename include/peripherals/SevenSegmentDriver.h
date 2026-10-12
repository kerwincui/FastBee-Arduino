/**
 * @file SevenSegmentDriver.h
 * @brief TM1637 4 位数码管驱动（bit-bang，无外部库依赖）
 * @author kerwincui
 * @date 2026-05
 *
 * 支持通过 2 个 GPIO（CLK + DIO）驱动多个 TM1637 模块。
 * 由 PeriphExecExecutor 的 ACTION_DISPLAY_NUMBER / ACTION_DISPLAY_TEXT / ACTION_DISPLAY_CLEAR 调用。
 */

#ifndef SEVEN_SEGMENT_DRIVER_H
#define SEVEN_SEGMENT_DRIVER_H

#include "core/FeatureFlags.h"

#if FASTBEE_ENABLE_SEVEN_SEGMENT

#include <Arduino.h>
#include <map>
#include <atomic>
#include "core/DisplayOutputTypes.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "core/FeatureInstanceRegistry.h"

class SevenSegmentDriver {
public:
    static SevenSegmentDriver& instance();
    FastBee::FeatureInstanceSnapshot observeFeatureInstance() const;

    // 注册并初始化一个 TM1637 实例
    // brightness: 0-7（默认 2，中等亮度）
    bool begin(const String& id, uint8_t clkPin, uint8_t dioPin, uint8_t brightness = 2);

    // Failed blanking keeps the instance for checked teardown; no physical state claim.
    bool release(const String& id);
    bool quiescent(const String& id) const;

    // 是否已注册
    bool exists(const String& id) const;

private:
    friend class PeripheralManager; // Every lifecycle/write caller holds the PM lock first.
    FastBee::DisplayWriteResult write(const String& id, uint8_t clk, uint8_t dio,
        FastBee::DisplayOutputCommand command, const String& text, int32_t value);
    SevenSegmentDriver() = default;
    SevenSegmentDriver(const SevenSegmentDriver&) = delete;
    SevenSegmentDriver& operator=(const SevenSegmentDriver&) = delete;
    bool ensureMutex();
    std::atomic<SemaphoreHandle_t> _mutex{nullptr};

    struct Instance {
        uint8_t clk;
        uint8_t dio;
        uint8_t brightness;  // 0-7
    };

    std::map<String, Instance> _instances;

    // ---- bit-bang TM1637 协议 ----
    void bitDelay() const;
    void start(const Instance& ins);
    void stop(const Instance& ins);
    // 发送单字节 LSB first，返回 ACK 状态（0 = ok）
    uint8_t writeByte(const Instance& ins, uint8_t b);
    // 按起始地址写入一组段码（addr 自增模式 0x40）
    FastBee::DisplayWriteResult writeSegments(const Instance& ins, const uint8_t* segs,
        uint8_t len, uint8_t startAddr = 0);
    // 下发 "显示开 + 亮度" 命令（0x88 | (bri & 0x07)）
    FastBee::DisplayWriteResult commitDisplayCtrl(const Instance& ins);
    FastBee::DisplayWriteResult displayNumber(const Instance& ins, const String& value);
    FastBee::DisplayWriteResult displayText(const Instance& ins, const String& text);

    // 字符映射到 TM1637 段码（不含 DP）
    static uint8_t charToSeg(char c);
};

#endif // FASTBEE_ENABLE_SEVEN_SEGMENT

#endif // SEVEN_SEGMENT_DRIVER_H
