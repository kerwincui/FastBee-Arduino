#ifndef PERIPHERAL_CONFIG_H
#define PERIPHERAL_CONFIG_H

#include <Arduino.h>
#include "PeripheralTypes.h"
#include "../systems/GpioConfig.h"  // 引用已有的GPIOState定义

// GPIO中断回调函数类型
using GPIOInterruptCallback = void(*)(uint8_t pin, GPIOState state);

// 工业模拟量输入的简化信号类型。数值固定写入 JSON，便于旧固件/新固件
// 之间保持兼容；0 保持原始 ADC 计数，不改变既有配置行为。
enum class AnalogSignalMode : uint8_t {
    RAW = 0,
    VOLTAGE_0_10V = 1,
    CURRENT_0_20MA = 2,
    CURRENT_4_20MA = 3,
    CUSTOM = 4,
    // Source compatibility for the descriptive name used by newer paths.
    CUSTOM_LINEAR = CUSTOM
};

// AI quality is reported separately from the converted value so existing
// property identifiers and numeric payloads remain backward compatible.
enum class AnalogInputQuality : uint8_t {
    VALID = 0,
    BELOW_RANGE = 1,
    ABOVE_RANGE = 2,
    DISCONNECTED = 3,
    SHORT_CIRCUIT = 4,
    INVALID_CONFIG = 5
};

inline const char* analogInputQualityName(AnalogInputQuality quality) {
    switch (quality) {
        case AnalogInputQuality::VALID: return "valid";
        case AnalogInputQuality::BELOW_RANGE: return "low";
        case AnalogInputQuality::ABOVE_RANGE: return "high";
        case AnalogInputQuality::DISCONNECTED: return "disconnected";
        case AnalogInputQuality::SHORT_CIRCUIT: return "short";
        default: return "invalid_config";
    }
}

inline AnalogInputQuality classifyAnalogInputQuality(uint8_t signalMode,
                                                      float inputValue,
                                                      float inputMin,
                                                      float inputMax,
                                                      float boundaryTolerance = 0.0f) {
    if (boundaryTolerance < 0.0f || isnan(boundaryTolerance) || isinf(boundaryTolerance)) {
        boundaryTolerance = 0.0f;
    }
    if (inputValue >= inputMin - boundaryTolerance &&
        inputValue <= inputMax + boundaryTolerance) {
        return AnalogInputQuality::VALID;
    }

    // Keep the existing 4-20mA driver's 1mA fault tolerance: 3..4mA and
    // 20..21mA are ordinary under/over-range values, while readings beyond
    // those bands are treated as a broken loop or short/overdrive condition.
    if (signalMode == static_cast<uint8_t>(AnalogSignalMode::CURRENT_4_20MA)) {
        if (inputValue < inputMin - 1.0f - boundaryTolerance) {
            return AnalogInputQuality::DISCONNECTED;
        }
        if (inputValue > inputMax + 1.0f + boundaryTolerance) {
            return AnalogInputQuality::SHORT_CIRCUIT;
        }
    }
    return inputValue < inputMin ? AnalogInputQuality::BELOW_RANGE
                                 : AnalogInputQuality::ABOVE_RANGE;
}

// DI 输入模式。AUTO 根据外设类型(11/13/14)推导，避免旧配置必须迁移。
enum class DigitalInputMode : uint8_t {
    AUTO = 0,
    FLOATING = 1,
    // Avoid Arduino HAL's global PULLUP/PULLDOWN macros (0x04/0x08).
    PULL_UP = 2,
    PULL_DOWN = 3
};

enum class DigitalReportMode : uint8_t {
    ON_CHANGE = 0,
    PERIODIC = 1,
    EVENT_ONLY = 2
};

constexpr uint16_t DIGITAL_INPUT_DEFAULT_DEBOUNCE_MS = 50;

inline uint16_t effectiveDigitalInputDebounceMs(uint16_t configuredMs) {
    return configuredMs > 0 ? configuredMs : DIGITAL_INPUT_DEFAULT_DEBOUNCE_MS;
}

inline bool shouldReportDigitalInputChange(uint8_t reportMode) {
    return reportMode == static_cast<uint8_t>(DigitalReportMode::ON_CHANGE);
}

inline bool shouldReportDigitalInputPeriodically(uint8_t reportMode) {
    return reportMode == static_cast<uint8_t>(DigitalReportMode::PERIODIC);
}

inline bool digitalInputLogicalLevel(bool rawLevel, bool invertLogic) {
    return invertLogic ? !rawLevel : rawLevel;
}

inline bool matchesDigitalInterruptEdge(PeripheralType type, bool logicalLevel) {
    if (type == PeripheralType::GPIO_INTERRUPT_RISING) return logicalLevel;
    if (type == PeripheralType::GPIO_INTERRUPT_FALLING) return !logicalLevel;
    return type == PeripheralType::GPIO_INTERRUPT_CHANGE;
}

inline uint32_t mapAnalogOutputPercent(uint32_t percent, uint8_t minimumPercent,
                                       uint8_t maximumPercent, uint8_t resolution) {
    if (minimumPercent > maximumPercent) {
        const uint8_t temporary = minimumPercent;
        minimumPercent = maximumPercent;
        maximumPercent = temporary;
    }
    if (minimumPercent > 100) minimumPercent = 100;
    if (maximumPercent > 100) maximumPercent = 100;
    if (resolution < 1) resolution = 1;
    if (resolution > 16) resolution = 16;
    if (percent > 100) percent = 100;

    const uint32_t maximumDuty = (1UL << resolution) - 1UL;
    const uint32_t mappedPercent = minimumPercent +
        (static_cast<uint32_t>(maximumPercent - minimumPercent) * percent / 100UL);
    return static_cast<uint32_t>(
        static_cast<uint64_t>(maximumDuty) * mappedPercent / 100ULL);
}

// 外设配置结构体
struct PeripheralConfig {
    String id;                    // 唯一标识符
    String name;                  // 显示名称
    PeripheralType type;          // 外设类型
    bool enabled;                 // 是否启用
    
    // 引脚配置
    uint8_t pinCount;             // 使用引脚数量
    uint8_t pins[8];              // 引脚列表（最多8个）
    
    // 类型特定参数
    union {
        // UART参数
        struct {
            uint32_t baudRate;
            uint8_t dataBits;     // 5, 6, 7, 8
            uint8_t stopBits;     // 1, 1.5, 2
            uint8_t parity;       // 0=None, 1=Odd, 2=Even
        } uart;
        
        // I2C参数
        struct {
            uint32_t frequency;   // 100000, 400000
            uint8_t address;      // 从机地址（0=主机模式）
            bool isMaster;        // 是否主机模式
        } i2c;
        
        // SPI参数
        struct {
            uint32_t frequency;
            uint8_t mode;         // SPI模式0-3
            bool msbFirst;        // true=MSB, false=LSB
        } spi;
        
        // GPIO参数（精简版：仅保留硬件初始化参数）
        struct {
            GPIOState initialState;
            uint8_t pwmChannel;       // PWM通道 (0-15)
            uint32_t pwmFrequency;    // PWM频率
            uint8_t pwmResolution;    // PWM分辨率 (1-16位)
            uint16_t defaultDuty;     // PWM默认占空比(0~2^resolution-1)
            bool passive;             // 无源蜂鸣器模式：GPIO_DIGITAL_OUTPUT 使用 PWM 驱动
            GPIOInterruptCallback interruptCallback;
            bool aoMode;              // 模拟量输出模式：PWM 输出按 0-100% 映射（无 DAC 芯片的 AO 方案）
            uint8_t aoMinPct;         // AO 映射最小百分比（默认 0）
            uint8_t aoMaxPct;         // AO 映射最大百分比（默认 100）
            // DI 配置：0/false 表示兼容旧行为，运行时按外设类型补默认值。
            uint8_t inputMode;        // 0=自动, 1=浮空, 2=上拉, 3=下拉
            bool invertLogic;         // 逻辑反相（用户看到的高低电平）
            uint16_t debounceMs;      // 0=使用安全默认 50ms
            uint8_t reportMode;       // 0=状态变化, 1=周期, 2=仅规则事件
        } gpio;
        
        // ADC参数
        struct {
            uint8_t attenuation;      // 衰减系数 0-3
            uint8_t resolution;       // 分辨率 9-12位
            uint16_t sampleRate;      // 采样率
            uint8_t sampleCount;      // 多采样次数 1-16（0/1=单次，默认1；工业滤波用8）
            uint8_t filterMode;       // 滤波模式 0=均值, 1=中值
            // AI 工程量转换（全部为可选字段，缺省保持 raw ADC 上报）
            uint8_t signalMode;       // AnalogSignalMode
            bool conversionEnabled;   // 是否输出工程量
            bool clampEnabled;        // 越界时钳位；关闭则返回无效值
            uint8_t decimalPlaces;    // 上报小数位 0-6
            float inputMin;           // 输入信号下限(V/mA/raw)，0=按信号类型推导
            float inputMax;           // 输入信号上限(V/mA/raw)，0=按信号类型推导
            float engineeringMin;     // 工程量下限
            float engineeringMax;     // 工程量上限
            float engineeringOffset;  // 结果偏移修正
            float vRef;               // ADC 参考电压，0=3.3V
            float voltageRatio;       // 0-10V 分压还原倍数，0=1
            float shuntResistance;    // 电流环分流电阻(ohm)，0=165
            float inputGain;          // 前端增益，0=1
            float inputOffset;        // 前端电压偏移(V)
            uint16_t rawMin;          // 自定义原始下限，0=自动
            uint16_t rawMax;          // 自定义原始上限，0=按分辨率推导
        } adc;
        
        // DAC参数
        struct {
            uint8_t channel;          // DAC通道 1或2
            uint8_t defaultValue;     // DAC默认输出值 (0-255)
        } dac;
        
        // PWM/Servo参数
        struct {
            uint32_t frequency;
            uint16_t minPulse;        // 最小脉宽 (微秒)
            uint16_t maxPulse;        // 最大脉宽 (微秒)
        } pwm;
        
        // 步进电机参数
        struct {
            uint16_t stepsPerRevolution;
            uint16_t speed;           // RPM
        } stepper;
        
        // 编码器参数
        struct {
            uint16_t resolution;      // 每转脉冲数
            bool useInterrupt;        // 是否使用中断
        } encoder;
        
        // SD卡参数
        struct {
            uint8_t interface;        // 0=SDMMC, 1=SPI模式
            uint32_t frequency;       // 通信频率 (默认 20MHz)
        } sdcard;
        
        // LCD参数
        struct {
            uint8_t width;            // 宽度
            uint8_t height;           // 高度
            uint8_t interface;        // 0=Parallel, 1=SPI, 2=I2C
            uint8_t contrast;         // 对比度/亮度 0-255（默认 255）
        } lcd;
        
        // 通用传感器容器参数（逻辑数据源、外部传感器）
        struct {
            uint8_t sensorType;       // 传感器类型ID
            uint32_t sampleInterval;  // 采样间隔(ms)
        } sensor;

        // 数码管参数（TM1637 等）
        struct {
            uint8_t brightness;       // 亮度 0-7
        } segment;

        // WS2812B / NeoPixel 参数
        struct {
            uint16_t count;           // 灯珠数量
            uint8_t brightness;       // 全局亮度 0-255
        } neopixel;

        // 433MHz RF OOK module parameters
        struct {
            uint8_t mode;             // 0=TX, 1=RX level monitor
            uint16_t pulseWidth;      // microseconds, default 350
            uint8_t repeat;           // send repeat count
            uint8_t bitLength;        // code bit length, usually 24
            bool activeHigh;          // true: HIGH is RF on / active
        } rf;

        // Radar/presence module parameters
        struct {
            uint8_t mode;             // 0=digital OUT pin
            bool activeHigh;          // true: HIGH means detected
            uint16_t debounceMs;      // UI/documented debounce hint
            uint16_t holdMs;          // UI/documented stable hold hint
        } radar;
        
        // Modbus子设备参数
        struct {
            uint8_t  slaveAddress;     // 从站地址 1-247
            uint8_t  channelCount;     // 通道数
            uint16_t coilBase;         // 线圈/寄存器基地址
            bool     ncMode;           // NC 常闭模式
            uint8_t  controlProtocol;  // 0=线圈(FC05), 1=寄存器(FC06)
            uint8_t  deviceType;       // 0=relay, 1=pwm, 2=pid, 3=motor
            uint8_t  deviceIndex;      // ModbusHandler config 中的索引
            uint16_t batchRegister;    // 位图批量寄存器地址，0表示不使用
            uint16_t pwmRegBase;       // PWM 寄存器基地址
            uint8_t  pwmResolution;    // PWM 分辨率(bits)
            uint16_t motorRegs[5];     // 电机寄存器地址 [正转,反转,停止,速度,脉冲数]
            uint8_t  motorDecimals;    // 电机参数小数位
            int32_t  motorMinPosition; // 软限位最小位置（步数）
            int32_t  motorMaxPosition; // 软限位最大位置（步数，> min 时启用）
            int32_t  motorCurrentPosition; // 当前估算位置（步数）
            int32_t  motorMoveStep;    // forward/reverse 默认相对移动步数
            uint16_t motorLastPulse;   // 最近一次 setPulse 值（运行时）
            char     sensorId[32];     // 传感器标识符 (同 MODBUS_DEVICE_SENSOR_ID_MAX)
        } modbus;
    } params;
    
    // 通用用户数据
    void* userData;
    
    // 默认构造函数
    PeripheralConfig() 
        : id(""), name(""), type(PeripheralType::UNCONFIGURED), 
          enabled(false), pinCount(0), userData(nullptr) {
        // 初始化引脚数组
        for (int i = 0; i < 8; i++) {
            pins[i] = 255;  // 255表示未使用
        }
        // params is a union: initializing more than one member corrupts the
        // previously written defaults. Start from a deterministic zero state;
        // parsers/factories then apply defaults for the selected type.
        memset(&params, 0, sizeof(params));
    }
    
    // 检查是否为GPIO类型
    bool isGPIOPeripheral() const {
        int typeValue = static_cast<int>(type);
        return typeValue >= 11 && typeValue <= 25;
    }
    
    // 检查是否为Modbus外设
    bool isModbusPeripheral() const {
        return type == PeripheralType::MODBUS_DEVICE;
    }
    
    // 检查是否为通信接口
    bool isCommunicationPeripheral() const {
        int typeValue = static_cast<int>(type);
        return typeValue >= 1 && typeValue <= 10;
    }
    
    // 获取第一个有效引脚（用于单引脚外设）
    uint8_t getPrimaryPin() const {
        for (int i = 0; i < pinCount && i < 8; i++) {
            if (pins[i] != 255) {
                return pins[i];
            }
        }
        return 255;
    }
    
    // 检查引脚是否被使用
    bool usesPin(uint8_t pin) const {
        for (int i = 0; i < pinCount && i < 8; i++) {
            if (pins[i] == pin) {
                return true;
            }
        }
        return false;
    }
};

inline float positiveAnalogConfigValue(float value, float fallback) {
    return (!isnan(value) && !isinf(value) && value > 0.0f) ? value : fallback;
}

// Pure, allocation-free AI conversion shared by runtime reporting and native
// boundary tests. A disabled conversion deliberately preserves the raw count.
inline float convertAnalogInputRaw(const PeripheralConfig& config, uint16_t raw,
                                   AnalogInputQuality* quality = nullptr) {
    const auto& adc = config.params.adc;
    const uint8_t mode = adc.signalMode;
    if (!adc.conversionEnabled || mode == static_cast<uint8_t>(AnalogSignalMode::RAW)) {
        if (quality) *quality = AnalogInputQuality::VALID;
        return static_cast<float>(raw);
    }
    if (quality) *quality = AnalogInputQuality::INVALID_CONFIG;
    if (mode > static_cast<uint8_t>(AnalogSignalMode::CUSTOM)) return NAN;

    uint8_t resolution = adc.resolution;
    if (resolution < 9 || resolution > 12) resolution = 12;
    const float adcMax = static_cast<float>(1UL << resolution) - 1.0f;
    const float vRef = positiveAnalogConfigValue(adc.vRef, 3.3f);
    float inputValue = 0.0f;
    float inputMin = adc.inputMin;
    float inputMax = adc.inputMax;
    float inputLsb = 1.0f;

    if (mode == static_cast<uint8_t>(AnalogSignalMode::CUSTOM_LINEAR)) {
        inputValue = static_cast<float>(raw);
        if (inputMin == 0.0f && inputMax == 0.0f) {
            inputMin = adc.rawMin;
            inputMax = adc.rawMax > adc.rawMin ? adc.rawMax : adcMax;
        }
    } else {
        const float pinVoltage = static_cast<float>(raw) * vRef / adcMax;
        if (mode == static_cast<uint8_t>(AnalogSignalMode::VOLTAGE_0_10V)) {
            const float voltageRatio = positiveAnalogConfigValue(adc.voltageRatio, 1.0f);
            inputValue = pinVoltage * voltageRatio;
            inputLsb = (vRef / adcMax) * voltageRatio;
            if (inputMin == 0.0f && inputMax == 0.0f) {
                inputMin = 0.0f;
                inputMax = 10.0f;
            }
        } else {
            const float gain = positiveAnalogConfigValue(adc.inputGain, 1.0f);
            const float shunt = positiveAnalogConfigValue(adc.shuntResistance, 165.0f);
            inputValue = ((pinVoltage - adc.inputOffset) / gain) * 1000.0f / shunt;
            inputLsb = (vRef / adcMax / gain) * 1000.0f / shunt;
            if (inputMin == 0.0f && inputMax == 0.0f) {
                inputMin = mode == static_cast<uint8_t>(AnalogSignalMode::CURRENT_4_20MA) ? 4.0f : 0.0f;
                inputMax = 20.0f;
            }
        }
    }

    if (isnan(inputValue) || isinf(inputValue) ||
        isnan(inputMin) || isinf(inputMin) || isnan(inputMax) || isinf(inputMax) ||
        inputMax <= inputMin ||
        isnan(adc.engineeringMin) || isinf(adc.engineeringMin) ||
        isnan(adc.engineeringMax) || isinf(adc.engineeringMax) ||
        adc.engineeringMax == adc.engineeringMin) {
        return NAN;
    }

    // ADC values are quantized. Half an input LSB prevents the exact configured
    // endpoint from becoming an artificial under/over-range fault after a
    // float roundtrip, without masking a physically distinct ADC code.
    const float boundaryTolerance = inputLsb * 0.5f;
    const AnalogInputQuality measuredQuality = classifyAnalogInputQuality(
        mode, inputValue, inputMin, inputMax, boundaryTolerance);
    if (quality) *quality = measuredQuality;
    if (measuredQuality == AnalogInputQuality::VALID) {
        // Tolerance only affects classification. Normalize the tiny quantized
        // overshoot before mapping so engineering endpoints remain exact.
        if (inputValue < inputMin) inputValue = inputMin;
        if (inputValue > inputMax) inputValue = inputMax;
    } else {
        if (!adc.clampEnabled) return NAN;
        inputValue = inputValue < inputMin ? inputMin : inputMax;
    }

    const float ratio = (inputValue - inputMin) / (inputMax - inputMin);
    return adc.engineeringMin +
           ratio * (adc.engineeringMax - adc.engineeringMin) +
           adc.engineeringOffset;
}

// 外设运行时状态
struct PeripheralRuntimeState {
    String id;
    PeripheralStatus status;
    // Runtime-only memory protection flag. It is not persisted to
    // peripherals.json, so recovery never overwrites the user's choice.
    bool memorySuspended;
    unsigned long initTime;       // 初始化时间
    unsigned long lastActivity;   // 最后活动时间
    uint32_t errorCount;          // 错误计数
    String lastError;             // 最后错误信息
    
    // 类型特定状态
    union {
        struct {
            uint32_t bytesSent;
            uint32_t bytesReceived;
            bool isConnected;
        } comm;
        
        struct {
            GPIOState currentState;
            uint32_t toggleCount;
            bool interruptAttached;
        } gpio;
        
        struct {
            uint16_t lastValue;
            uint32_t sampleCount;
        } analog;
    } state;
    
    PeripheralRuntimeState()
        : status(PeripheralStatus::PERIPHERAL_DISABLED), memorySuspended(false), initTime(0),
          lastActivity(0), errorCount(0) {
        memset(&state, 0, sizeof(state));
    }
};

// 外设配置JSON序列化/反序列化辅助函数
namespace PeripheralConfigSerializer {
    // 将配置序列化为JSON对象
    void toJson(const PeripheralConfig& config, JsonObject& obj);
    
    // 从JSON对象反序列化配置
    bool fromJson(PeripheralConfig& config, const JsonObject& obj);
    
    // 将运行时状态序列化为JSON对象
    void stateToJson(const PeripheralRuntimeState& state, JsonObject& obj);
}

// 预定义外设配置（常用配置模板）
namespace PERIPHERAL_TEMPLATES {
    // 创建UART配置
    PeripheralConfig createUART(const String& name, uint8_t txPin, uint8_t rxPin, 
                                 uint32_t baudRate = 115200);
    
    // 创建I2C配置
    PeripheralConfig createI2C(const String& name, uint8_t sdaPin, uint8_t sclPin, 
                                uint32_t frequency = 100000);
    
    // 创建SPI配置
    PeripheralConfig createSPI(const String& name, uint8_t misoPin, uint8_t mosiPin, 
                                uint8_t sckPin, uint8_t csPin, uint32_t frequency = 1000000);
    
    // 创建GPIO配置（兼容现有）
    PeripheralConfig createGPIO(const String& name, uint8_t pin, 
                                 PeripheralType type = PeripheralType::GPIO_DIGITAL_OUTPUT,
                                 GPIOState initialState = GPIOState::STATE_LOW);
    
    // 创建ADC配置
    PeripheralConfig createADC(const String& name, uint8_t pin, 
                                uint8_t resolution = 12);
    
    // 创建DAC配置
    PeripheralConfig createDAC(const String& name, uint8_t channel);
    
    // 创建PWM配置
    PeripheralConfig createPWM(const String& name, uint8_t pin, 
                                uint32_t frequency = 1000, uint8_t channel = 0);
    
    // 创建Servo配置
    PeripheralConfig createServo(const String& name, uint8_t pin, 
                                  uint16_t minPulse = 544, uint16_t maxPulse = 2400);
}

#endif // PERIPHERAL_CONFIG_H
