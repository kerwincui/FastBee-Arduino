#ifndef PERIPHERAL_MANAGER_H
#define PERIPHERAL_MANAGER_H

#include <Arduino.h>
#include <map>
#include <vector>
#include <functional>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include "core/NativeOutputControl.h"
#include "core/ModbusOrdinaryControl.h"
#include "core/DisplayOutputTypes.h"
#include "core/FeatureFlags.h"
#include "core/CheckedActionTimer.h"
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include "PeripheralTypes.h"
#include "PeripheralConfig.h"
#include "ResourceProfile.h"
#include "UartPortLease.h"
#include "UartReceiveGeneration.h"
#include "UartReceiveFault.h"
#include "UartEventOrigin.h"
#include "ControlOutput.h"
#include "ManagedPinIo.h"
#include "StepperResourceSnapshot.h"
#include "NeoPixelResourceSnapshot.h"
#include "utils/StaticPoolAllocator.h"

namespace FastBee { struct PreflightContext; struct PreflightResult; }

// 外设容器静态池参数（T1：根治 std::map 节点堆碎片）
//   - BlockSize 256: 容纳 _Rb_tree_node<pair<const String, PeripheralConfig>>(~200B) 与
//                     _Rb_tree_node<pair<const String, PeripheralRuntimeState>>(~120B)
//   - BlockCount 32: 覆盖 ~16 个外设×2 容器，冷启动预分配 8KB DRAM
//   - 池耗尽会优雅回退 ::malloc（不限制外设上限）
static constexpr size_t PERIPH_NODE_BLOCK_SIZE  = 256;
static constexpr size_t PERIPH_NODE_BLOCK_COUNT = 32;

using PeripheralMapAllocator = FastBee::PooledAllocator<
    std::pair<const String, PeripheralConfig>,
    PERIPH_NODE_BLOCK_SIZE, PERIPH_NODE_BLOCK_COUNT>;
using RuntimeStateMapAllocator = FastBee::PooledAllocator<
    std::pair<const String, PeripheralRuntimeState>,
    PERIPH_NODE_BLOCK_SIZE, PERIPH_NODE_BLOCK_COUNT>;

using PeripheralMap   = std::map<String, PeripheralConfig,
                                  std::less<String>, PeripheralMapAllocator>;
using RuntimeStateMap = std::map<String, PeripheralRuntimeState,
                                  std::less<String>, RuntimeStateMapAllocator>;

// Modbus 通信委托回调类型（用于解耦 PeripheralManager 和 ModbusHandler）
using ModbusCoilWriteFunc = std::function<bool(uint8_t slaveAddr, uint16_t coilAddr, bool value,
    const FastBee::OrdinaryControlContext&, FastBee::OrdinaryControlResult&)>;
using ModbusRegWriteFunc  = std::function<bool(uint8_t slaveAddr, uint16_t regAddr, uint16_t value,
    const FastBee::OrdinaryControlContext&, FastBee::OrdinaryControlResult&)>;

// 外设配置文件路径
#define PERIPHERAL_CONFIG_FILE "/config/peripherals.json"
#define PERIPHERAL_CONFIG_BACKUP "/config/gpio.json.bak"

class TwoWire;
class ModbusHandler;
struct OneShotResult;
struct AnalogReadSample {
    bool sampled = false, conversionEnabled = false, calibratedVoltage = false;
    uint16_t raw = 0;
    uint8_t pin = 255, signalMode = 0, decimalPlaces = 0;
    float value = NAN, pinVoltage = NAN, calculatedCurrentMa = NAN;
    AnalogInputQuality quality = AnalogInputQuality::READ_FAILED;
};
class PeripheralManager {
public:
    static PeripheralManager& getInstance();

    // 禁止拷贝
    PeripheralManager(const PeripheralManager&) = delete;
    PeripheralManager& operator=(const PeripheralManager&) = delete;

    // 初始化
    bool initialize();
    // Boot-only opt-in RTC fallback, before network/background task startup.
    void seedSystemClockFromRtcAtBoot();
    // Best-effort bounded output shutdown. Blocks further IO until restart;
    // false means at least one physical safe-state write was not confirmed.
    bool shutdownIoExpanders();
    // PWM lifecycle is explicit: boot/enable never starts or enables outputs.
    bool preparePca9685(const String& id, JsonObject review, String& error);
    bool startPca9685(const String& id, const String& token, bool powerCycleConfirmed, String& error);
    bool commandPca9685(const String& id, const String& command, uint32_t channel, double value, String& error,
        const FastBee::OrdinaryControlContext* context = nullptr,
        FastBee::OrdinaryControlResult* result = nullptr);
    bool getPca9685Status(const String& id, JsonObject status) const;
    bool stopPca9685(const String& id);
    bool shutdownPca9685();
    bool pca9685Owns(const String& id) const;
    void servicePca9685();
    bool validatePca9685Dependencies(const PeripheralConfig& config, String& error) const;

    // ========== 外设管理（增删改查） ==========

    // 添加外设
    bool addPeripheral(const PeripheralConfig& config);
    // 添加外设（带错误信息输出，用于 API 返回具体失败原因）
    bool addPeripheral(const PeripheralConfig& config, String& errorMsg);
    bool addPeripheralAndSave(const PeripheralConfig& config, String& errorMsg);

    // 更新外设
    bool updatePeripheral(const String& id, const PeripheralConfig& config);
    bool updatePeripheral(const String& id, const PeripheralConfig& config, String& errorMsg);
    bool updatePeripheralAndSave(const String& id, const PeripheralConfig& config, String& errorMsg);

    // 删除外设
    bool removePeripheral(const String& id);
    bool removePeripheralAndSave(const String& id, String& errorMsg);

    // 获取外设配置
    PeripheralConfig* getPeripheral(const String& id);
    const PeripheralConfig* getPeripheral(const String& id) const;
    bool getPeripheralSnapshot(const String& id, PeripheralConfig& snapshot,
                               uint32_t* sensorConfigVersion = nullptr) const;
    // Configuration review identity, valid only within this boot. Zero refuses
    // activation after lock failure or counter exhaustion; never wraps/reuses.
    uint64_t getConfigurationReviewVersion() const;
    // Commit only bounded memory work here. Never call hardware, Web send,
    // display, SSE or rule callbacks while this guard is held.
    bool commitSensorRead(uint32_t sensorConfigVersion,
                          const std::function<bool()>& commit) const;
    bool releaseSensorCache(const String& id);

    // 根据类型获取外设列表
    std::vector<PeripheralConfig> getPeripheralsByType(PeripheralType type) const;
    std::vector<PeripheralConfig> getPeripheralsByCategory(PeripheralCategory category) const;

    // 获取所有外设
    std::vector<PeripheralConfig> getAllPeripherals() const;

    // 遍历所有外设（无拷贝，避免频繁内存分配）
    void forEachPeripheral(std::function<void(const PeripheralConfig&)> callback) const;

    // 检查外设是否存在
    bool hasPeripheral(const String& id) const;

    // ========== 外设操作 ==========

    // 启用/禁用外设
    bool enablePeripheral(const String& id);
    bool disablePeripheral(const String& id);
    bool enablePeripheralAndSave(const String& id, String& errorMsg);
    bool disablePeripheralAndSave(const String& id, String& errorMsg);
    bool enableIoExpanderGroupAndSave(const String& id, const std::vector<String>& channelIds, String& errorMsg);
    bool disableIoExpanderGroupAndSave(const String& id, String& errorMsg);
    bool isPeripheralEnabled(const String& id) const;
    String lastEnableError;  // 最近一次 enablePeripheral 失败原因

    // 获取外设状态
    PeripheralStatus getPeripheralStatus(const String& id) const;
    PeripheralRuntimeState* getRuntimeState(const String& id);
    bool getRuntimeStateSnapshot(const String& id, PeripheralRuntimeState& snapshot) const;

    // ========== 硬件初始化 ==========

    // 初始化外设硬件
    bool initHardware(const String& id);
    bool deinitHardware(const String& id);

    // 初始化所有启用的外设
    bool initAllEnabledPeripherals();
    // Task-context only: batch sample shared expanders and advance low-rate IO actions.
    void pollIoExpanders();
    uint32_t digitalInputGeneration(const PeripheralConfig& config) const;

    // 内存保护：仅在运行时挂起高占用的可选外设。配置文件中的 enabled
    // 值不会被写回；恢复到 NORMAL 后会重新初始化这些外设。
    size_t suspendOptionalForMemory();
    size_t restoreOptionalAfterMemory();
    size_t getMemorySuspendedCount() const { return _memorySuspendedCount; }

    // 延迟初始化重试：网络就绪后，重新初始化因 requiresNetwork 功能门控
    // 而在启动早期被跳过（标记为 PERIPHERAL_ERROR）的外设。仅针对类型映射到
    // 网络依赖功能的外设，避免误重试真实硬件故障。返回成功重试的数量。
    int retryDeferredPeripherals();

    // ========== 数据读写 ==========

    // 通用数据读写接口
    bool writeData(const String& id, const uint8_t* data, size_t len);
    bool readData(const String& id, uint8_t* buffer, size_t& len);
    // Nonzero only for a currently owned generic UART hardware initialization.
    uint32_t getUartReceiveGeneration(const String& id) const;
    bool isUartEventOriginCurrent(const FastBee::UartEventOrigin& origin) const;
    TwoWire* getManagedI2cWire(const String& id);
    bool resolveI2cConfig(const PeripheralConfig& config, uint8_t& sda, uint8_t& scl,
                          uint32_t& frequency, String& errorMsg) const;

    // 发送字符串（适用于通信接口）
    bool writeString(const String& id, const String& data);
    String readString(const String& id);

    // 编码器实时读数。delta 为自上次读取以来的增量；读取后原子清零。
    bool getEncoderReading(const String& id, EncoderReading& reading);
    bool resetEncoder(const String& id);

    // ========== GPIO兼容层（保持向后兼容） ==========

    // 配置单个GPIO引脚（内部转换为PeripheralConfig）
    bool configurePin(uint8_t pin, PeripheralType type);
    bool configurePin(const PeripheralConfig& config);

    // 读取GPIO状态
    GPIOState readPin(uint8_t pin);
    GPIOState readPin(const String& peripheralId);

    // 写入GPIO状态
    bool writePin(uint8_t pin, GPIOState state);
    bool writePin(const String& peripheralId, GPIOState state);

    // 热路径零分配重载（const char*）：轮询执行器每周期调用，
    // 避免 String(periphId) 临时对象反复堆分配。
    // 普通 GPIO DI/DO/AI 快路径完全无堆操作；Modbus/雷达/编码器等
    // 特例外设内部回退一次 String 构造后委托原实现。
    GPIOState readPinByCStr(const char* peripheralId);
    bool writePinByCStr(const char* peripheralId, GPIOState state);
    uint16_t readAnalogByCStr(const char* peripheralId);
    bool readAnalogSample(const String& peripheralId, AnalogReadSample& sample);
    bool readAnalogSampleByCStr(const char* peripheralId, AnalogReadSample& sample);
    // AI 工程量读取：未启用转换时返回与 readAnalog 相同的原始计数。
    // 返回 NAN 表示配置非法或输入超出未钳位范围；调用方可据此跳过上报。
    float readAnalogValue(const String& peripheralId);
    float readAnalogValueByCStr(const char* peripheralId);
    // 使用调用方已经取得的同一份 ADC 原始值执行工程量转换，避免重复采样。
    float convertAnalogRawValue(const String& peripheralId, uint16_t rawValue,
                                AnalogInputQuality* quality = nullptr, float* pinVoltage = nullptr);

    // 切换GPIO状态
    bool togglePin(uint8_t pin);
    bool togglePin(const String& peripheralId);

    // PWM操作
    bool writePWM(uint8_t pin, uint32_t dutyCycle);
    bool writePWM(const String& peripheralId, uint32_t dutyCycle);
    bool writeServoAngle(const String& peripheralId, int angle,
        const FastBee::OrdinaryControlContext* context = nullptr,
        FastBee::OrdinaryControlResult* result = nullptr);
    bool submitOrdinaryServo(const String& id, int64_t value, bool rawDuty,
        const FastBee::OrdinaryControlContext* context,
        FastBee::OrdinaryControlResult* result = nullptr);

    // 模拟读取
    uint16_t readAnalog(uint8_t pin);
    uint16_t readAnalog(const String& peripheralId);

    // 中断管理
    bool attachInterrupt(uint8_t pin, GPIOInterruptCallback callback);
    bool attachInterrupt(const String& peripheralId, GPIOInterruptCallback callback);
    bool detachInterrupt(uint8_t pin);
    bool detachInterrupt(const String& peripheralId);
    // Consume captured logical GPIO interrupt events. Events are copied from
    // the ISR into fixed-capacity buffers and therefore do not allocate heap.
    bool popDigitalInputInterruptEvent(uint8_t& pin, GPIOState& state);

    // GPIO查询
    bool isPinConfigured(uint8_t pin) const;
    PeripheralType getPinType(uint8_t pin) const;
    String getPinPeripheralId(uint8_t pin) const;
    std::vector<uint8_t> getConfiguredPins() const;

    // ========== 持久化 ==========

    // 保存配置到文件
    bool saveConfiguration();

    // 从文件加载配置
    bool loadConfiguration();

    // Full-file import validates a separate candidate graph. No live hardware
    // changes; a committed graph is loaded on restart, and old saves are blocked.
    bool configurationImportPending() const;
    bool prepareConfigurationImport(JsonDocument& document,
        std::vector<PeripheralConfig>& candidates, const FastBee::PreflightContext& context,
        FastBee::PreflightResult& result, String& error);
    bool commitConfigurationImport(JsonDocument& document, uint64_t expectedVersion, String& error);

    // ========== 系统状态 ==========

    // 打印所有外设状态（调试用）
    void printStatus() const;

    // 获取已配置外设数量
    size_t getPeripheralCount() const { return peripherals.size(); }

    // 检查引脚冲突
    bool checkPinConflict(uint8_t pin, const String& excludeId = "") const;
    std::vector<String> getConflictingPeripherals(uint8_t pin) const;

    // 引脚验证（增强版）
    String getPinConflictInfo(uint8_t pin, const String& excludeId = "") const;
    bool validatePinForType(uint8_t pin, PeripheralType type, String& errorMsg) const;
    bool isReservedPin(uint8_t pin) const;
    bool isInputOnlyPin(uint8_t pin) const;

    // 定期维护（在主循环中调用）
    void performMaintenance();

    // ========== Modbus 外设委托 ==========

    // 设置 Modbus 通信回调（由 ProtocolManager 注入，解耦协议层依赖）
    void setModbusCallbacks(ModbusCoilWriteFunc coilWrite, ModbusRegWriteFunc regWrite);

    // 清除 Modbus 通信回调（Modbus 停止时调用）
    void clearModbusCallbacks();

    // Modbus 线圈写入（指定地址，供高级控制使用）
    bool writeModbusCoil(const String& id, uint16_t coilAddr, bool value);

    // Modbus 寄存器写入（指定地址，供高级控制使用）
    bool writeModbusReg(const String& id, uint16_t regAddr, uint16_t value);
    bool writeModbusCoil(const String& id,uint16_t address,bool value,
        const FastBee::OrdinaryControlContext&,FastBee::OrdinaryControlResult&);
    bool writeModbusReg(const String& id,uint16_t address,uint16_t value,
        const FastBee::OrdinaryControlContext&,FastBee::OrdinaryControlResult&);
    bool writeModbusPin(const String& id,GPIOState state,
        const FastBee::OrdinaryControlContext&,FastBee::OrdinaryControlResult&);
    bool writeModbusPWM(const String& id,uint32_t value,
        const FastBee::OrdinaryControlContext&,FastBee::OrdinaryControlResult&);

    // 动作定时器上下文（公开供静态回调函数访问）
    struct ActionTickerData {
        String id;
        Ticker ticker;
        FastBee::CheckedActionTimer ordinaryTimer;
        FastBee::OrdinaryControl::Permit ordinaryPermit;
        uint32_t ordinaryConfigEpoch = 0;
        uint32_t ordinaryRuleEpoch = 0;
        FastBee::UartEventOrigin uartOrigin;
        uint32_t token = 0;
        uint32_t maxDuty = 0;
        uint32_t stepSize = 0;
        uint32_t duty = 0;
        uint8_t pin = 255;
        uint8_t mode = 0;
        bool increasing = true;
    };

    // 动作定时器（公开供 PeriphExecManager 调用）
    // actionMode: 1=闪烁, 2=呼吸灯; paramValue: 闪烁间隔ms或呼吸周期ms
    bool startActionTicker(const String& id, uint8_t actionMode, uint16_t paramValue,
                           const FastBee::UartEventOrigin& uartOrigin = {});
    void stopActionTicker(const String& id);
    void serviceActionTicker(uint32_t token); // Opaque callback token; no borrowed context pointer.

    // ULN2003/4 相步进电机控制（非阻塞半步序列）
    struct StepperTickerData {
        String id;
        FastBee::CheckedActionTimer timer;
        FastBee::OrdinaryControlContext ordinaryContext;
        FastBee::OrdinaryControl::Permit ordinaryPermit;
        FastBee::UartEventOrigin uartOrigin;
        uint32_t token = 0;
        uint8_t pins[4] = {255, 255, 255, 255};
        uint8_t phase = 0;
        int8_t direction = 0;      // 1=正转, -1=反转, 0=停止
        uint16_t rpm = 0;
        uint16_t stepsPerRev = 0;
        bool running = false;
        uint8_t retainedMask = 0;  // Attempted GPIO acquisitions, including uncertain SDK failures.
        bool pinsReady = false;
        bool cleanupPending = false;
    };

    struct ServoReleaseTickerData {
        bool releaseDone = false;
        String id;
        FastBee::CheckedActionTimer timer;
        FastBee::OrdinaryControlContext ordinaryContext;
        FastBee::OrdinaryControl::Permit ordinaryPermit;
        uint32_t token = 0;
        uint8_t pin = 255;
    };
    bool controlStepper(const String& id, const String& action, int value = 0,
                         const FastBee::UartEventOrigin& uartOrigin = {},
                         const FastBee::OrdinaryControlContext* context = nullptr,
                         FastBee::OrdinaryControlResult* result = nullptr);
    bool stopStepper(const String& id, bool* attempted = nullptr);
    void serviceStepperTicker(uint32_t token);
    void snapshotStepperResources(FastBee::StepperResourceSnapshot& result) const;

    // WS2812B / NeoPixel 控制（RMT 非阻塞发送，规则层定时调用可实现循环）
    bool controlNeoPixel(const String& id, const String& action, const String& value = "",
        const FastBee::OrdinaryControlContext* context = nullptr,
        FastBee::OrdinaryControlResult* result = nullptr);

    // 433MHz RF module helpers
    bool sendRfCode(const String& id, const String& code,
                    uint32_t bitLength = 0, uint32_t pulseWidth = 0, uint32_t repeat = 0,
                    const FastBee::OrdinaryControlContext* context = nullptr,
                    FastBee::OrdinaryControlResult* result = nullptr);
    bool setupRfTransmit(const PeripheralConfig& config);
    bool releaseRfTransmit(const PeripheralConfig& config);
    bool retireOrdinaryRfLocked(const String& id, bool* attempted = nullptr);
    bool ordinaryRfQuiescentLocked(const String& id) const;
    bool readRfLevel(const String& id, bool& level);

    // Radar/presence module helper
    bool readRadarState(const String& id, bool& detected);

    // 线程安全：获取互斥量句柄（供 Ticker 回调非阻塞尝试加锁）
    SemaphoreHandle_t getMutex() const { return _mutex; }

    // Internal H1 control capability. Native GPIO config owns the pin; the
    // temporary control lease excludes normal writes and configuration changes.
    bool controlOutput(const String& id, const void* owner,
                       FastBee::ControlOutputCommand command, int& pin,
                       std::atomic<bool>** retirement = nullptr,
                       const FastBee::ResourceConsumerIdentity* consumer = nullptr);
    bool controlOutputIsClaimed(const String& id = "") const;
    bool controlOutputPinIsClaimed(uint8_t pin) const;
    void retireControlOutputs();
    // Native non-passive DO arbitration. Context identity is supplied only by
    // trusted internal boundaries; clients never choose source/actor/operation.
    // Transaction-only maintenance authority: no manual hold, takeover or ordinary output.
    bool beginOrdinaryModbusMaintenance(uint64_t actorIdentity, uint32_t expectedConfigEpoch,
        FastBee::OrdinaryControlContext& context);
    bool beginOrdinaryControl(FastBee::OrdinaryControl::Source source,
        uint64_t actorIdentity, uint32_t holdMs, bool overrideConfirmed,
        uint64_t reviewedGeneration, const String& reviewedTargetId,
        uint32_t expectedConfigEpoch, const FastBee::UartEventOrigin& uartOrigin,
        FastBee::OrdinaryControlContext& context);
    bool submitOrdinaryDigital(const String& id, FastBee::DigitalOutputCommand command,
        uint32_t intervalMs, const FastBee::OrdinaryControlContext& context,
        FastBee::OrdinaryControlResult& result);
    bool submitOrdinaryScalar(const String& id, FastBee::ScalarOutputCommand command,
        uint32_t value, const FastBee::OrdinaryControlContext& context,
        FastBee::OrdinaryControlResult& result);
    void finishOrdinaryControl(const FastBee::OrdinaryControlContext& context, bool cancelled = false);
    bool submitOrdinaryDisplay(const String& id, FastBee::DisplayOutputCommand command,
        const String& text, int32_t value, const FastBee::OrdinaryControlContext* context,
        FastBee::OrdinaryControlResult* result = nullptr);
    bool ordinaryDisplayQuiescentLocked(const String& id) const;
    bool submitOrdinaryLcd(const String& id, const FastBee::LcdOutputRequest& output,
        const FastBee::OrdinaryControlContext* context,
        FastBee::OrdinaryControlResult* result = nullptr);
    bool getActiveLcdTarget(String& id, uint32_t& configEpoch) const;
    void performLcdMaintenance();
    OneShotResult submitOrdinaryModbusRelay(ModbusHandler& handler,
        const FastBee::ModbusControlTarget& target, uint8_t function, uint16_t address, uint16_t value,
        const FastBee::OrdinaryControlContext& context, FastBee::OrdinaryControlResult& result,
        uint16_t timeout = 0, bool toggle = false, uint64_t workerGeneration = 0);
    OneShotResult submitOrdinaryModbusFrame(ModbusHandler& handler,
        const FastBee::ModbusControlTarget& target, const FastBee::ModbusOutputFrame& frame,
        const FastBee::OrdinaryControlContext& context, FastBee::OrdinaryControlResult& result,
        uint16_t timeout = 0, uint8_t* rawResponse = nullptr, size_t rawCapacity = 0,
        size_t* rawLength = nullptr, uint64_t workerGeneration = 0);
    bool reserveOrdinaryModbusWork(const FastBee::ModbusControlTarget& target,
        const FastBee::OrdinaryControlContext& context, uint64_t& workGeneration,
        FastBee::OrdinaryControlResult& result, bool emergencyStop = false);
    bool activateOrdinaryModbusWork(const FastBee::ModbusControlTarget& target,
        const FastBee::OrdinaryControlContext& context, uint64_t workGeneration, const char** rejectionReason=nullptr);
    bool ordinaryModbusWorkCurrent(const FastBee::ModbusControlTarget& target,
        const FastBee::OrdinaryControlContext& context, uint64_t workGeneration, const char** rejectionReason=nullptr);
    bool finishOrdinaryModbusWork(const FastBee::ModbusControlTarget& target,
        const FastBee::OrdinaryControlContext& context, uint64_t workGeneration, bool failed);
    bool getOrdinaryControlSnapshot(const String& id, FastBee::OrdinaryControlResult& result) const;
    void serviceOrdinaryControlTicker(uint32_t token);
    void serviceOrdinaryControlExpiry();
    void snapshotControlOutputResources(FastBee::ControlOutputResourceSnapshot& result) const;
    void snapshotEncoderResources(FastBee::EncoderResourceSnapshot& result) const;
    void snapshotLedcResources(FastBee::LedcResourceSnapshot& result) const;
    bool writeScriptPwm(uint8_t pin, uint32_t duty);
    bool releaseScriptPwm(uint8_t pin);
    void snapshotGpioResources(FastBee::GpioResourceSnapshot& result) const;
    void snapshotDacResources(FastBee::DacResourceSnapshot& result) const;
    void snapshotNeoPixelResources(FastBee::NeoPixelResourceSnapshot& result) const;
    bool prepareScriptPin(uint8_t pin, uint8_t kind); // 0 GPIO, 1 PWM, 2 DAC; release only own prior function.
    bool writeScriptGpio(uint8_t pin, bool high);
    bool writeScriptDac(uint8_t pin, uint8_t value);
    bool managedPinAvailableForAdc(uint8_t pin) const;

    // ========== ISR 队列诊断（T4：事件丢失可观测） ==========

    // 队列溢出计数：队列满时 ISR 事件被丢弃的次数（旧实现静默丢事件，无法诊断）
    uint32_t getIsrQueueOverrunCount() const { return _isrQueueOverruns; }
    // 当前队列中待处理的事件数
    size_t getIsrQueueWaiting() const;
    // 复位溢出计数（诊断采集周期开始时调用）
    void resetIsrQueueStats();

    // 中断去抖决策（纯函数，便于单元测试）：
    // 同一引脚两次派发间隔 < debounceMs 时合并，抑制机械按键/CHANGE 中断风暴
    static bool shouldDispatchIsrEvent(unsigned long nowMs, unsigned long lastDispatchMs,
                                       uint16_t debounceMs);

    // 按外设类型返回去抖窗口（纯函数，便于单元测试）：
    // - RF_MODULE/IR_RECEIVER 等高速边沿外设的脉冲时序敏感（OOK/NEC 解码），返回 0 禁用去抖
    // - GPIO 中断/雷达等类型默认 5ms 去抖抑制机械抖动
    static uint16_t isrDebounceMsForType(PeripheralType type);

private:
    PeripheralManager() = default;
    uint64_t _nextOrdinaryControlOperation = 0;
#if FASTBEE_ENABLE_MODBUS
    FastBee::ModbusOrdinarySlots _ordinaryModbus;
    uint64_t _ordinaryModbusRetiredFloor = 0;
#endif
    void finishOrdinaryModbusLocked(const FastBee::OrdinaryControlContext& context, bool cancelled);
    void serviceOrdinaryModbusLocked();
#if FASTBEE_ENABLE_MODBUS
    FastBee::ModbusOrdinarySlot* acquireOrdinaryModbusLocked(const FastBee::ModbusControlTarget& target,
        const FastBee::OrdinaryControlContext& context, FastBee::OrdinaryControlResult& result,
        uint64_t now, uint64_t workerGeneration = 0);
#endif
    uint64_t _nextOrdinaryControlGeneration = 0;
    // IOX bridge: callers hold the PM mutex across allocation, retirement and I2C.
    bool ordinaryIoProducerPresentLocked(const String& id) const;
    bool prepareOrdinaryIoProducerLocked(const String& id, bool& inserted);
    void discardOrdinaryIoReservationLocked(const String& id, bool inserted);
    bool retireOrdinaryIoProducerLocked(const String& id, bool keepSlot = false);
    bool startOrdinaryIoProducerLocked(const String& id, uint16_t interval,
        const FastBee::OrdinaryControlContext& context,
        const FastBee::OrdinaryControl::Permit& permit);
    bool retireOrdinaryIoGroupLocked(const String& providerId);
    void serviceOrdinaryIoProducerLocked(const String& id);
    bool ordinaryDigitalClaimedLocked(const String& id) const;
    bool ordinaryPermitCurrentLocked(const String& id, const FastBee::OrdinaryControl::Permit& permit) const;
    bool writeOrdinaryScalarLocked(const String& id, FastBee::ScalarOutputCommand command,
        uint32_t value, const FastBee::OrdinaryControl::Permit& permit);
    // All these helpers require the caller to retain the same PM mutex from
    // inspect through candidate preparation, commit and the bounded SDK call.
    bool ordinaryBackendSupportedLocked(const PeripheralConfig& config) const;
    bool ordinaryProducerPresentLocked(const String& id) const;
    bool ordinaryBackendQuiescentLocked(const String& id) const;
    bool ordinaryMotionProducerPresentLocked(const String& id) const;
    bool ordinaryMotionQuiescentLocked(const String& id) const;
    bool retireOrdinaryMotionLocked(const String& id, bool keepSlot, bool* attempted);
    bool ordinaryPca9685QuiescentLocked(const String& id) const;
    void invalidateOrdinaryPca9685Locked(const String& id);
    bool ordinaryNeoPixelQuiescentLocked(const String& id) const;
    bool controlNeoPixelFrameLocked(const String& id, const String& action, const String& value);
    bool inspectOrdinaryControlLocked(const String& id,
        const FastBee::OrdinaryControlContext& context, FastBee::OrdinaryControl::Intent intent,
        FastBee::OrdinaryControlResult& result, uint64_t& now);
    bool commitOrdinaryControlLocked(const String& id,
        const FastBee::OrdinaryControlContext& context, FastBee::OrdinaryControl::Intent intent,
        FastBee::OrdinaryControlResult& result, uint64_t& now, bool keepTickerSlot,
        FastBee::OrdinaryControl::Permit& permit);
    void updateOrdinaryControlResultLocked(const String& id, uint64_t now,
        FastBee::OrdinaryControlResult& result) const;
    bool completeOrdinaryControlLocked(const String& id, uint64_t now,
        FastBee::OrdinaryControlResult& result, bool accepted,
        const char* failureReason = "CONTROL_OUTPUT_UNCONFIRMED");
    bool retireOrdinaryDigitalLocked(const String& id, uint64_t cutoff,
        bool keepTickerSlot = false, bool keepInvocation = false, bool* attempted = nullptr);

    // 线程安全互斥量（递归，支持嵌套调用）
    SemaphoreHandle_t _mutex = nullptr;
    uint64_t _configurationReviewVersion = 1; // Access only under _mutex.
    bool _configurationImportPending = false;

    // 外设存储（T1：使用静态池 allocator，节点不再碎片化系统堆）
    PeripheralMap   peripherals;
    RuntimeStateMap runtimeStates;
    size_t _memorySuspendedCount = 0;
    // Protected by _mutex. Screen start waits outside this mutex; mutations
    // of that display are rejected until the two-phase recovery completes.
    String _memoryRestoringDisplayId;
    struct ControlOutputClaim {
        const void* owner = nullptr; // Identity only, never dereferenced.
        char id[41]{};
        FastBee::ResourceConsumerIdentity consumer;
        int pin = -1;
        bool fault = false;
        std::atomic<bool> retirement{false};
        bool retirementAttempted = false;
        uint32_t lastRetirementAttempt = 0;
    };
    ControlOutputClaim _controlOutputClaims[4]{}; // Fixed, no per-claim allocation.
    FastBee::LedcResourceSnapshot::Lease _ledcResourceLeases[FastBee::LedcResourceSnapshot::CAPACITY]{};
    // 引脚到外设ID的映射（T2：高频重建，走 SmallNodePool）
    std::map<uint8_t, String, std::less<uint8_t>,
             FastBee::SmallNodeAllocator<std::pair<const uint8_t, String>>> pinToPeripheral;

    // 动作定时器（T2：高频增删，走 SmallNodePool）
    std::map<String, ActionTickerData*, std::less<String>,
             FastBee::SmallNodeAllocator<std::pair<const String, ActionTickerData*>>> actionTickers;
    uint32_t _nextActionTickerToken = 0; // Saturates; a detached timer token is never reused.

    std::map<String, StepperTickerData*, std::less<String>,
             FastBee::SmallNodeAllocator<std::pair<const String, StepperTickerData*>>> stepperTickers;

    std::map<String, ServoReleaseTickerData*, std::less<String>,
             FastBee::SmallNodeAllocator<std::pair<const String, ServoReleaseTickerData*>>> servoReleaseTickers;

    std::map<String, uint8_t, std::less<String>,
             FastBee::SmallNodeAllocator<std::pair<const String, uint8_t>>> neopixelRainbowIndex;

    std::map<String, uint8_t, std::less<String>,
             FastBee::SmallNodeAllocator<std::pair<const String, uint8_t>>> uartPortById;
    FastBee::UartPortLease uartPortLeases[FastBee::UartPortLease::PORT_COUNT];
    FastBee::UartReceiveGeneration uartReceiveGenerations;

    // Fixed ISR storage. The argument is an immutable generation token, never
    // a slot pointer that a late callback could mistake for a replacement.
    static constexpr size_t MAX_ENCODER_SLOTS = 4;
    struct EncoderIsrSlot {
        volatile bool used = false;
        volatile bool active = false;
        uint32_t token = 0;
        bool fault = false, cleanupPending = false;
        uint8_t pinA = 255;
        uint8_t pinB = 255;
        volatile int64_t count = 0;
        volatile int64_t delta = 0;
        volatile int8_t direction = 0;
        volatile uint32_t lastEdgeMicros = 0;
        volatile uint32_t edgePeriodMicros = 0;
        String id;   // 仅任务上下文读写，ISR 不触碰
    };
    static EncoderIsrSlot _encoderSlots[MAX_ENCODER_SLOTS];
    static portMUX_TYPE _encoderMux;   // ISR 与任务间的自旋锁临界区
    static uint32_t _nextEncoderGeneration; // Saturates; generations never wrap.
    EncoderIsrSlot* findEncoderSlot(const String& id);
    EncoderIsrSlot* allocEncoderSlot(const String& id, uint8_t pinA, uint8_t pinB);
    void releaseEncoderSlot(const String& id);

    bool setupEncoderHardware(const PeripheralConfig& config);
    bool teardownEncoderHardware(const PeripheralConfig& config);
    bool encoderSlotReady(const EncoderIsrSlot* slot) const;

    // arg encodes a nonzero generation and the fixed two-bit slot index.
    static void IRAM_ATTR handleEncoderInterrupt(void* arg);

    // Modbus 通信委托
    ModbusCoilWriteFunc _modbusCoilWrite = nullptr;
    ModbusRegWriteFunc  _modbusRegWrite  = nullptr;

    // 内部方法
    bool validateConfig(const PeripheralConfig& config, String& errorMsg, bool checkRunningGraph = true) const;
    bool setupHardware(const PeripheralConfig& config);
    bool setupSpiHardware(const PeripheralConfig& config);
    bool teardownSpiHardware(const PeripheralConfig& config);
    bool validateIoDependencies(const PeripheralConfig& config, const String& excludeId,
                                String& errorMsg) const;
    bool syncIoExpander(const String& providerId, const String& excludeChild = "");
    bool setupIoExpander(const PeripheralConfig& config);
    bool teardownIoExpander(const String& providerId);
    bool canShareI2cPin(const PeripheralConfig& candidate, uint8_t pin, const String& excludeId) const;
    bool teardownHardware(const PeripheralConfig& config);
    bool setupStepperHardware(const PeripheralConfig& config);
    bool teardownStepperHardware(const String& id);
    bool writeStepperPhase(StepperTickerData& data, uint8_t phase);
    void invalidateSensorConfigurationLocked();
    bool setupUartHardware(const PeripheralConfig& config);
    HardwareSerial* getUartSerial(const String& id);
    bool validateUartCapacity(const PeripheralConfig& candidate,
                              const String& excludeId, String& errorMsg) const;

    // GPIO硬件设置
    FastBee::ManagedPinIo _managedPinIo;
    uint32_t _nextRadarGeneration = 0; // Saturates; no stale session is reused.
    bool setupRadarHardware(const PeripheralConfig& config);
    bool teardownRadarHardware(const PeripheralConfig& config);
    bool managedPinConflict(const PeripheralConfig& config) const;
    bool scriptPinReady(uint8_t pin, uint8_t kind) const;
    bool setupManagedDigital(const PeripheralConfig& config);
    bool releaseManagedDigital(const PeripheralConfig& config);
    bool writeManagedDac(const String& id, uint8_t pin, uint8_t value);
    bool releaseManagedDac(const String& id, uint8_t pin);
    bool setupGPIOPin(const PeripheralConfig& config);
    bool setupPWMPin(const PeripheralConfig& config);
    bool attachManagedLedc(const String& id, uint8_t pin, uint32_t frequency,
                           uint8_t resolution, uint8_t channel, bool commandScript = false);
    bool detachManagedLedc(const String& id, uint8_t pin, bool commandScript = false);
    bool setupServoPin(const PeripheralConfig& config);
    bool attachServoSignal(const PeripheralConfig& config);
    bool scheduleServoRelease(const String& id);
    ServoReleaseTickerData* prepareServoReleaseLocked(const String& id, bool& inserted);
    bool startServoReleaseLocked(ServoReleaseTickerData& data);
    bool cancelServoRelease(const String& id, bool keepSlot = false);
    bool releaseServoSignal(const String& id, uint8_t capturedPin = 255, bool* attempted = nullptr);
    static void servoReleaseTickerCallback(void* token);
    void serviceServoRelease(uint32_t token);

    struct MutationSnapshot {
        String id;
        bool existed = false;
        bool hadRuntimeState = false;
        PeripheralConfig config;
        PeripheralRuntimeState runtimeState;
        size_t memorySuspendedCount = 0;
    };
    bool applyMutationTransactional(const String& snapshotId, const String& resultId,
                                    const std::function<bool(String&)>& mutation,
                                    String& errorMsg);
    bool restoreMutationSnapshot(const MutationSnapshot& snapshot,
                                 const String& resultId, String& errorMsg);

    // Modbus外设写入（内部实现）
    bool writeModbusPin(const String& id, const PeripheralConfig& config, GPIOState state);
    bool writeModbusPWM(const String& id, const PeripheralConfig& config, uint32_t dutyCycle);

    // 生成唯一ID
    String generateUniqueId(PeripheralType type);

    // 更新引脚映射
    void updatePinMapping(const String& id, const PeripheralConfig& config);
    void removePinMapping(const String& id);
    // 完全从 peripherals 真实数据重建 pinToPeripheral 缓存（消除残留/错乱）
    void rebuildPinMapping();

    // 中断事件队列
    static constexpr uint8_t ISR_QUEUE_SIZE = 16;
    static constexpr uint8_t ISR_MAX_EVENTS_PER_CYCLE = 8;
    static constexpr uint32_t ISR_MAINTENANCE_BUDGET_MS = 4;
    static constexpr uint32_t ISR_STATE_LOCK_TIMEOUT_MS = 2;
    struct GpioIsrEvent {
        uint8_t pin;
        uint8_t rawLevel;
    };
    static QueueHandle_t _isrQueue;
    // 队列溢出计数（静态：isrHandler 为静态 ISR 上下文，不能依赖实例指针）
    static volatile uint32_t _isrQueueOverruns;

    // 中断去抖：维护任务按引脚记录上次派发时刻。
    static constexpr uint16_t ISR_DEBOUNCE_MS = 5;
    static constexpr uint8_t ISR_TRACKED_PIN_COUNT = 64;
    unsigned long _lastIsrDispatch[ISR_TRACKED_PIN_COUNT] = {0};
    bool _hasIsrDispatch[ISR_TRACKED_PIN_COUNT] = {false};

    struct DigitalInputInterruptEvent {
        uint8_t pin;
        GPIOState state;
    };
    DigitalInputInterruptEvent _digitalInterruptEvents[ISR_QUEUE_SIZE];
    uint8_t _digitalInterruptHead = 0;
    uint8_t _digitalInterruptTail = 0;
    uint8_t _digitalInterruptCount = 0;

    // 中断处理
    static void IRAM_ATTR isrHandler(void* arg);
    // Consume one event only after bounded state-lock admission. False means
    // no event was consumed (empty queue or a busy configuration writer).
    bool handleInterrupt();
    void processInterruptQueue();

    // 检查引脚有效性
    bool isValidPin(uint8_t pin) const;
    bool isValidPinForType(uint8_t pin, PeripheralType type) const;

    // DAC硬件初始化
    bool setupDACPin(const PeripheralConfig& config);

    // ADC 多采样滤波读取（工业噪声抑制）
    // samples: 采样次数 1-16；filterMode: 0=均值, 1=中值
    bool setupAnalogInput(const PeripheralConfig& config);
    bool teardownAnalogInput(const PeripheralConfig& config);

    // C 字符串零分配查找（调用方须已持有 _mutex）：
    // 外设数量小（<32），线性扫描 strcmp 开销远低于 String 构造的堆分配
    // 以下两个 helper 也要求调用方已持有 _mutex，统一 String/C 字符串 GPIO 路径。
    GPIOState readGpioStateLocked(const PeripheralConfig& config,
                                  const String* peripheralId,
                                  const char* peripheralIdCStr);
    bool writeGpioStateLocked(const PeripheralConfig& config,
                              const String* peripheralId,
                              const char* peripheralIdCStr,
                              GPIOState state,
                              const FastBee::OrdinaryControl::Permit* permit = nullptr);
    PeripheralConfig* getPeripheralCStrLocked(const char* id);
    PeripheralRuntimeState* getRuntimeStateCStrLocked(const char* id);
};

#endif // PERIPHERAL_MANAGER_H
