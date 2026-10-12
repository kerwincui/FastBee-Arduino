#ifndef IR_REMOTE_DRIVER_H
#define IR_REMOTE_DRIVER_H

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_IR_REMOTE

#include <Arduino.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "core/FeatureInstanceRegistry.h"

/**
 * @brief 红外遥控接收驱动（基于 IRremoteESP8266）
 *
 * 功能：
 *   - 使用硬件中断接收红外信号
 *   - 支持 NEC/RC5/RC6/SONY/Samsung/LG/Panasonic 等协议自动解码
 *   - 学习模式（对码）：用户主动触发的限时窗口，捕获下一个有效编码，期间不派发触发事件
 *   - 按键映射（按键名→编码表）：随 /config/periph_exec.json 的 irKeys 数组持久化，规则按名称引用
 *   - 解码成功后触发 ir_code_received 事件；命中按键映射时事件携带 key 字段
 *
 * 接线：
 *   - IR 接收器 OUT → 配置的 GPIO 引脚（外设配置必须按目标板选择）
 *   - IR 接收器 VCC → 3.3V
 *   - IR 接收器 GND → GND
 *
 * 编译条件：FASTBEE_ENABLE_IR_REMOTE=1（以实际 platformio 环境为准；标准矩阵 esp32s3-F8R8 显式关闭）
 */

/** @brief 学习模式状态 */
enum class IrLearnState : uint8_t {
    IDLE = 0,      // 空闲
    LEARNING = 1,  // 学习中（等待编码）
    CAPTURED = 2,  // 已捕获编码
    TIMEOUT = 3    // 学习超时
};

/** @brief 按键映射条目：按键名 → 编码 */
struct IrKeyEntry {
    String name;       // 按键名称（唯一标识，如 "power"）
    uint64_t value;    // 原始编码值
    String protocol;   // 协议名（可空；为空时匹配任意协议）
};

struct IrStatusSnapshot {
    bool initialized = false, stopping = false, keysLoaded = false;
    IrLearnState learnState = IrLearnState::IDLE;
    uint32_t remainingMs = 0;
    size_t keyCount = 0;
    String lastCode, lastProtocol, learnedCode, learnedProtocol;
    uint64_t lastValue = 0;
};

class IRRemoteDriver {
public:
    static IRRemoteDriver& getInstance();
    FastBee::FeatureInstanceSnapshot observeFeatureInstance() const;

    /**
     * @brief 初始化红外接收器
     * @param recvPin 接收器数据引脚
     * @param bufferSize 接收缓冲区大小（默认 1024）
     * @return 初始化是否成功
     */
    bool begin(uint8_t recvPin = 15, uint16_t bufferSize = 1024);

    /**
     * @brief 检查是否有新的红外编码接收（由调度器定期调用）
     * 解码成功时：学习模式下捕获编码；否则派发 ir_code_received 事件
     */
    void check();

    /**
     * @brief 获取最近接收到的红外编码信息
     * @return 格式 "PROTOCOL:0xCODE"（如 "NEC:0x00FF6897"）
     */
    bool getSnapshot(IrStatusSnapshot& snapshot) const;
    String getLastCode() const;

    /**
     * @brief 获取最近接收的协议名称
     */
    String getLastProtocol() const;

    /**
     * @brief 获取最近接收的原始编码值
     */
    uint64_t getLastValue() const;

    /**
     * @brief 停止红外接收器
     */
    bool stop(); // False while a decode event is still being dispatched; caller must retain and retry.

    bool isInitialized() const;

    // ============ 学习模式（对码）============

    /**
     * @brief 进入限时学习模式（用户主动触发的对码操作）
     * @param timeoutMs 超时时间毫秒（默认 10s，限制 1000~60000）
     */
    bool startLearn(uint32_t timeoutMs = 10000);

    /** @brief 取消学习模式 */
    bool stopLearn();

    /** @brief 是否处于学习中 */
    bool isLearning() const;

    /** @brief 获取学习状态（会同步刷新超时状态）*/
    IrLearnState getLearnState();

    /** @brief 学习状态名称（idle/learning/captured/timeout）*/
    String getLearnStateName();

    /** @brief 已捕获的编码（十六进制字符串 "0x...", 未捕获时为空）*/
    String getLearnedCode() const;

    /** @brief 已捕获编码的协议名 */
    String getLearnedProtocol() const;

    /** @brief 学习窗口剩余毫秒（非学习中返回 0）*/
    uint32_t getLearnRemainingMs();

    // ============ 按键映射（对码结果 / 清码）============

    /** @brief 从 PeriphExecManager 的统一配置 irKeys 加载按键映射到内存 */
    bool loadKeyMap();

    /** @brief 已保存的按键数量 */
    size_t getKeyCount() const;

    /**
     * @brief 根据编码解析按键名称
     * @return 命中返回按键名，未命中返回空串
     */
    String resolveKeyName(uint64_t value, const String& protocol) const;

    /**
     * @brief 输出按键映射的 JSON 数组字符串
     * 格式: [{"name":"power","code":"0x00FF6897","protocol":"NEC"}]
     */
    String getKeysJson() const;
    bool getKeySnapshot(String& json, size_t& count) const;

    /**
     * @brief 新增或更新一个按键（对码保存），并持久化
     * @param name 按键名
     * @param code 编码字符串（"0x00FF6897" 或 "00FF6897"）
     * @param protocol 协议名（可空）
     * @param err 失败原因（输出）
     */
    bool upsertKey(const String& name, const String& code, const String& protocol, String& err);

    /** @brief 删除单个按键（清码），并持久化 */
    bool deleteKey(const String& name, String* error = nullptr);

    /** @brief 清除全部按键（清码），并持久化 */
    bool clearKeys(String* error = nullptr);

private:
    IRRemoteDriver() = default;
    ~IRRemoteDriver();
    IRRemoteDriver(const IRRemoteDriver&) = delete;
    IRRemoteDriver& operator=(const IRRemoteDriver&) = delete;

    void _updateLearnTimeout();
    bool _editKeyMap(const String& name, uint64_t value, const String& protocol, uint8_t operation, String& error);
    String _resolveKeyNameLocked(uint64_t value, const String& protocol) const;
    bool ensureMutex();

    void* _irRecv = nullptr;        // IRrecv* (避免头文件暴露)
    void* _results = nullptr;       // decode_results*
    bool _initialized = false;
    bool _stopping = false, _dispatching = false;
    std::atomic_flag _keyMutation = ATOMIC_FLAG_INIT; // Serializes edits without holding receiver lock over storage.
    std::atomic<SemaphoreHandle_t> _mutex{nullptr};  // One lifetime lock, published by business getter.
    String _lastCode;
    String _lastProtocol;
    uint64_t _lastValue = 0;
    unsigned long _lastEventTime = 0;

    // 学习模式
    IrLearnState _learnState = IrLearnState::IDLE;
    unsigned long _learnDeadline = 0;
    String _learnedCode;
    String _learnedProtocol;
    uint64_t _learnedValue = 0;

    // 按键映射
    std::vector<IrKeyEntry> _keyMap;
    bool _keyMapLoaded = false;

    static constexpr unsigned long MIN_EVENT_INTERVAL_MS = 150;  // 防重复触发间隔
    static constexpr size_t MAX_IR_KEYS = 40;                    // 按键映射上限
};

#endif // FASTBEE_ENABLE_IR_REMOTE
#endif // IR_REMOTE_DRIVER_H
