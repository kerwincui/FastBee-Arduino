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

// Graphical display configuration is intentionally separate from the legacy
// U8g2 LCD parameters. Named pins avoid truncating wide display/touch wiring
// into PeripheralConfig::pins[8], while keeping the persisted schema explicit.
struct DisplayConfigParams {
    char backend[16];             // spi_tft / rgb
    char driver[16];              // st7789 / ili9341 / st7701
    char touchBackend[16];        // none / gt911
    uint16_t width;
    uint16_t height;
    uint8_t rotation;             // 0, 1, 2, 3
    uint8_t colorDepth;           // first implementation: RGB565 (16)
    uint8_t bufferLines;          // bounded renderer stripe hint
    uint8_t brightness;           // 0..100
    uint8_t backlightChannel;     // explicit LEDC channel for resource planning
    bool backlightActiveHigh;
    uint32_t spiFrequency;
    uint32_t i2cFrequency;
    int8_t mosi;
    int8_t sclk;
    int8_t cs;
    int8_t dc;
    int8_t rst;
    int8_t backlight;
    int8_t touchSda;
    int8_t touchScl;
    int8_t touchInt;
    int8_t touchRst;
    char rgbInitProfile[24];
    int8_t rgbDataPins[16];       // D0..15: B0..4, G0..5, R0..4
    int8_t rgbDe, rgbVsync, rgbHsync, rgbPclk;
    uint32_t rgbFrequency;
    uint16_t hsyncFrontPorch, hsyncPulseWidth, hsyncBackPorch;
    uint16_t vsyncFrontPorch, vsyncPulseWidth, vsyncBackPorch;
    bool hsyncIdleLow, vsyncIdleLow, pclkActiveNeg;
    char controlBackend[16];      // none / tca9554 / ch32v003
    int8_t controlSda, controlScl;
    uint8_t controlAddress, controlResetMask, controlBacklightMask, controlEnableMask;
    uint8_t controlBuzzerMask;   // Optional active-high buzzer output; CH32V003 only.
    uint8_t touchBuzzerDurationMs; // Touch feedback duration (20..150 ms), not volume.
    uint8_t controlInactiveMask; // Explicit outputs to hold low; never infer unclaimed pins.
    uint8_t controlTouchIntMask; // Optional GT911 INT on control expander; low during reset, then input.

    bool controlMasksDisjoint() const {
        return !(controlResetMask & controlEnableMask) &&
               !(controlBacklightMask & (controlResetMask | controlEnableMask)) &&
               !(controlBuzzerMask & (controlResetMask | controlEnableMask | controlBacklightMask)) &&
               !(controlInactiveMask & (controlResetMask | controlEnableMask |
                                         controlBacklightMask | controlBuzzerMask)) &&
               !(controlTouchIntMask & (controlResetMask | controlEnableMask |
                                         controlBacklightMask | controlBuzzerMask |
                                         controlInactiveMask));
    }

    bool controlTouchSyncValid() const {
        return controlTouchIntMask == 0 ||
            (!(controlTouchIntMask & (controlTouchIntMask - 1U)) && controlResetMask != 0 &&
             strcasecmp(backend, "rgb") == 0 && strcasecmp(touchBackend, "gt911") == 0 &&
             (strcasecmp(controlBackend, "ch32v003") == 0 || strcasecmp(controlBackend, "tca9554") == 0) &&
             touchInt < 0 && touchRst < 0);
    }

    // Stable named-pin slots, including RGB and optional shared control I2C.
    int8_t pinAt(uint8_t index) const {
        const int8_t base[] = {mosi, sclk, cs, dc, rst, backlight,
                              touchSda, touchScl, touchInt, touchRst};
        if (index < 10) return base[index];
        if (strcasecmp(backend, "rgb") != 0) return -1;
        if (index < 26) return rgbDataPins[index - 10];
        const int8_t extra[] = {rgbDe, rgbVsync, rgbHsync, rgbPclk,
            controlSda == touchSda ? static_cast<int8_t>(-1) : controlSda,
            controlScl == touchScl ? static_cast<int8_t>(-1) : controlScl};
        return index < 32 ? extra[index - 26] : -1;
    }
};

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
            uint8_t stopBits;     // 1 or 2
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
        
        // GPIO 参数：保留硬件初始化所需字段。
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
            char pairedInputId[41];   // Optional complementary DI; configure on one input only
            uint16_t pairDebounceMs;  // 0=200ms, otherwise 20..5000ms
            uint8_t ioSource;         // 0=native GPIO, 1=IO expander channel
            char providerId[41];      // Parent peripheral ID; never an MCU pin
            uint8_t channel;          // PCF8574 P0..P7
        } gpio;

        struct {
            char driver[16];          // First supported model: PCF8574
            char busPeripheralId[41]; // Optional reference to an I2C peripheral
            uint8_t address;          // PCF8574: 0x20..0x27
            uint16_t sampleInterval;  // 20..1000ms; zero uses 20ms
        } ioExpander;
        
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

        // 舵机参数。舵机与通用 GPIO PWM 的语义不同，必须保持独立，
        // 避免 union 中读取未初始化的 gpio.pwm* 字段。
        struct {
            uint32_t frequency;       // PWM 频率，常用 50Hz
            uint16_t minPulse;        // 最小角对应脉宽 (us)
            uint16_t maxPulse;        // 最大角对应脉宽 (us)
            int16_t minAngle;         // 最小逻辑角度
            int16_t maxAngle;         // 最大逻辑角度
            int16_t initialAngle;     // 启用后的安全启动角度
            uint8_t pwmChannel;       // LEDC 通道
            uint8_t pwmResolution;    // LEDC 分辨率
            bool inverted;            // 反向映射角度
            bool releaseAfterMove;    // 到位后停止输出保持脉冲
        } servo;
        
        // 步进电机参数
        struct {
            uint16_t stepsPerRevolution;
            uint16_t speed;           // RPM
        } stepper;
        
        // 编码器参数
        struct {
            uint16_t resolution;      // 每转脉冲数
            bool useInterrupt;        // 当前实现必须为 true（GPIO ISR）
        } encoder;
        
        // SD卡参数
        struct {
            uint8_t interface;        // 0=SDMMC, 1=SPI模式
            uint32_t frequency;       // 通信频率 (默认 20MHz)
        } sdcard;

        // DS1302 RTC 参数（当前日期时间不持久化，仅保存运行模式）。
        struct {
            bool use24Hour;
            uint8_t trickleCharge;    // DS1302TrickleCharge 原始寄存器值，默认 0
        } ds1302;

        // LCD1602 + PCF8574 参数。
        struct {
            uint8_t cols;             // 16 或 20
            uint8_t rows;             // 2 或 4
            uint8_t address;          // 0=自动检测，或 0x20-0x27/0x38-0x3F
            bool backlight;
        } lcd1602;

        // MFRC522 当前没有可安全持久化的密钥参数。总线和复位引脚保存在 pins。
        struct {
            uint32_t frequency;       // 预留并持久化 SPI 频率，默认 4MHz
        } rfid;
        
        // LCD参数
        struct {
            uint8_t width;            // 宽度
            uint8_t height;           // 高度
            uint8_t interface;        // 0=Parallel, 1=SPI, 2=I2C
            uint8_t contrast;         // 对比度/亮度 0-255（默认 255）
        } lcd;

        // Graphical display and optional external touch controller. This does
        // not replace the legacy LCD member above.
        DisplayConfigParams display;
        
        // 通用传感器容器参数（逻辑数据源、外部传感器）
        struct {
            uint8_t sensorType;       // 传感器类型ID
            uint32_t sampleInterval;  // 采样间隔(ms)
            char driver[16];          // 注册驱动名，如 BMP280/MPU6050
            uint8_t address;          // I2C 地址
            uint8_t mode;             // 0=默认/加速度，1=陀螺仪
            uint32_t frequency;       // I2C 频率，默认 100kHz
            char busPeripheralId[41];
            float shuntResistance;   // INA226 ohms; zero means uncalibrated
            float maxCurrent;        // INA226 expected maximum current, A
            uint16_t conversionTimeUs;
            uint16_t averaging;
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

    bool isExpanderChannel() const {
        return isGPIOPeripheral() && params.gpio.ioSource == 1;
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
        if (type == PeripheralType::SCREEN_DISPLAY) {
            for (uint8_t i = 0; i < configuredPinCount(); ++i) {
                if (configuredPinAt(i) == pin) return true;
            }
            return false;
        }
        for (int i = 0; i < pinCount && i < 8; i++) {
            if (pins[i] == pin) {
                return true;
            }
        }
        return false;
    }

    uint8_t configuredPinCount() const {
        if (type != PeripheralType::SCREEN_DISPLAY) return pinCount > 8 ? 8 : pinCount;
        uint8_t count = 0;
        for (uint8_t i = 0; i < 32; ++i) if (params.display.pinAt(i) >= 0) ++count;
        return count;
    }

    uint8_t configuredPinAt(uint8_t configuredIndex) const {
        if (type != PeripheralType::SCREEN_DISPLAY) {
            return configuredIndex < pinCount && configuredIndex < 8
                ? pins[configuredIndex] : 255;
        }
        uint8_t seen = 0;
        for (uint8_t i = 0; i < 32; ++i) {
            const int8_t value = params.display.pinAt(i);
            if (value < 0) continue;
            if (seen++ == configuredIndex) return static_cast<uint8_t>(value);
        }
        return 255;
    }
};

constexpr uint32_t UART_DEFAULT_BAUD_RATE = 115200;
constexpr uint8_t UART_DEFAULT_DATA_BITS = 8;
constexpr uint8_t UART_DEFAULT_STOP_BITS = 1;
constexpr uint8_t UART_DEFAULT_PARITY = 0;

inline void initializeUartDefaults(PeripheralConfig& config) {
    memset(&config.params, 0, sizeof(config.params));
    config.params.uart.baudRate = UART_DEFAULT_BAUD_RATE;
    config.params.uart.dataBits = UART_DEFAULT_DATA_BITS;
    config.params.uart.stopBits = UART_DEFAULT_STOP_BITS;
    config.params.uart.parity = UART_DEFAULT_PARITY;
}

inline char uartParityCode(uint8_t parity) {
    return parity == 1 ? 'O' : (parity == 2 ? 'E' : 'N');
}

inline String uartFramingName(const PeripheralConfig& config) {
    char framing[4] = {
        static_cast<char>('0' + config.params.uart.dataBits),
        uartParityCode(config.params.uart.parity),
        static_cast<char>('0' + config.params.uart.stopBits),
        '\0'
    };
    return String(framing);
}

constexpr uint16_t ENCODER_DEFAULT_RESOLUTION = 20;

inline void initializeEncoderDefaults(PeripheralConfig& config) {
    memset(&config.params, 0, sizeof(config.params));
    config.params.encoder.resolution = ENCODER_DEFAULT_RESOLUTION;
    config.params.encoder.useInterrupt = true;
}

constexpr uint8_t SDCARD_INTERFACE_SPI = 1;
constexpr uint32_t SDCARD_DEFAULT_FREQUENCY = 20000000UL;
constexpr uint32_t RFID_DEFAULT_FREQUENCY = 4000000UL;
constexpr uint8_t LCD1602_DEFAULT_COLS = 16;
constexpr uint8_t LCD1602_DEFAULT_ROWS = 2;
constexpr uint8_t LCD1602_DEFAULT_ADDRESS = 0x27;
constexpr uint16_t DISPLAY_DEFAULT_WIDTH = 240;
constexpr uint16_t DISPLAY_DEFAULT_HEIGHT = 320;
constexpr uint8_t DISPLAY_DEFAULT_COLOR_DEPTH = 16;
constexpr uint8_t DISPLAY_DEFAULT_BUFFER_LINES = 16;
constexpr uint8_t DISPLAY_DEFAULT_BRIGHTNESS = 70;
constexpr uint32_t DISPLAY_DEFAULT_SPI_FREQUENCY = 40000000UL;
constexpr uint32_t DISPLAY_DEFAULT_I2C_FREQUENCY = 400000UL;

template <size_t N>
inline void copyPeripheralConfigText(char (&destination)[N], const char* source) {
    static_assert(N > 0, "fixed configuration text must have storage");
    size_t length = 0;
    if (source) {
        while (length + 1 < N && source[length] != '\0') {
            destination[length] = source[length];
            ++length;
        }
    }
    destination[length] = '\0';
}

inline void initializeSdCardDefaults(PeripheralConfig& config) {
    memset(&config.params, 0, sizeof(config.params));
    config.params.sdcard.interface = SDCARD_INTERFACE_SPI;
    config.params.sdcard.frequency = SDCARD_DEFAULT_FREQUENCY;
}

inline void initializeDs1302Defaults(PeripheralConfig& config) {
    memset(&config.params, 0, sizeof(config.params));
    config.params.ds1302.use24Hour = true;
    config.params.ds1302.trickleCharge = 0;
}

inline void initializeLcd1602Defaults(PeripheralConfig& config) {
    memset(&config.params, 0, sizeof(config.params));
    config.params.lcd1602.cols = LCD1602_DEFAULT_COLS;
    config.params.lcd1602.rows = LCD1602_DEFAULT_ROWS;
    config.params.lcd1602.address = LCD1602_DEFAULT_ADDRESS;
    config.params.lcd1602.backlight = true;
}

inline void initializeRfidDefaults(PeripheralConfig& config) {
    memset(&config.params, 0, sizeof(config.params));
    config.params.rfid.frequency = RFID_DEFAULT_FREQUENCY;
}

inline void initializeDisplayDefaults(PeripheralConfig& config) {
    memset(&config.params, 0, sizeof(config.params));
    copyPeripheralConfigText(config.params.display.backend, "spi_tft");
    copyPeripheralConfigText(config.params.display.driver, "st7789");
    copyPeripheralConfigText(config.params.display.touchBackend, "none");
    config.params.display.width = DISPLAY_DEFAULT_WIDTH;
    config.params.display.height = DISPLAY_DEFAULT_HEIGHT;
    config.params.display.rotation = 0;
    config.params.display.colorDepth = DISPLAY_DEFAULT_COLOR_DEPTH;
    config.params.display.bufferLines = DISPLAY_DEFAULT_BUFFER_LINES;
    config.params.display.brightness = DISPLAY_DEFAULT_BRIGHTNESS;
    config.params.display.backlightChannel = 0;
    config.params.display.backlightActiveHigh = true;
    config.params.display.spiFrequency = DISPLAY_DEFAULT_SPI_FREQUENCY;
    config.params.display.i2cFrequency = DISPLAY_DEFAULT_I2C_FREQUENCY;
    config.params.display.mosi = -1;
    config.params.display.sclk = -1;
    config.params.display.cs = -1;
    config.params.display.dc = -1;
    config.params.display.rst = -1;
    config.params.display.backlight = -1;
    config.params.display.touchSda = -1;
    config.params.display.touchScl = -1;
    config.params.display.touchInt = -1;
    config.params.display.touchRst = -1;
    copyPeripheralConfigText(config.params.display.rgbInitProfile, "st7701_type1");
    copyPeripheralConfigText(config.params.display.controlBackend, "none");
    config.params.display.controlBuzzerMask = 0;
    config.params.display.touchBuzzerDurationMs = 30;
    config.params.display.controlInactiveMask = 0;
    config.params.display.controlTouchIntMask = 0;
    memset(config.params.display.rgbDataPins, -1, sizeof(config.params.display.rgbDataPins));
    config.params.display.rgbDe = config.params.display.rgbVsync = -1;
    config.params.display.rgbHsync = config.params.display.rgbPclk = -1;
    config.params.display.controlSda = config.params.display.controlScl = -1;
}

inline void initializeSensorDefaults(PeripheralConfig& config) {
    memset(&config.params, 0, sizeof(config.params));
    config.params.sensor.sampleInterval = 1000;
    config.params.sensor.frequency = 100000;
    config.params.sensor.conversionTimeUs = 1100;
    config.params.sensor.averaging = 16;
}

struct EncoderReading {
    int64_t count = 0;
    int64_t delta = 0;
    int8_t direction = 0;
    float frequencyHz = 0.0f;
    float rpm = 0.0f;
};

constexpr uint32_t SERVO_DEFAULT_FREQUENCY_HZ = 50;
constexpr uint16_t SERVO_DEFAULT_MIN_PULSE_US = 500;
constexpr uint16_t SERVO_DEFAULT_MAX_PULSE_US = 2500;
constexpr int16_t SERVO_DEFAULT_MIN_ANGLE = 0;
constexpr int16_t SERVO_DEFAULT_MAX_ANGLE = 180;
constexpr int16_t SERVO_DEFAULT_INITIAL_ANGLE = 90;
constexpr uint8_t SERVO_DEFAULT_PWM_CHANNEL = 0;
constexpr uint8_t SERVO_DEFAULT_PWM_RESOLUTION = 16;

inline void initializeServoDefaults(PeripheralConfig& config) {
    memset(&config.params, 0, sizeof(config.params));
    config.params.servo.frequency = SERVO_DEFAULT_FREQUENCY_HZ;
    config.params.servo.minPulse = SERVO_DEFAULT_MIN_PULSE_US;
    config.params.servo.maxPulse = SERVO_DEFAULT_MAX_PULSE_US;
    config.params.servo.minAngle = SERVO_DEFAULT_MIN_ANGLE;
    config.params.servo.maxAngle = SERVO_DEFAULT_MAX_ANGLE;
    config.params.servo.initialAngle = SERVO_DEFAULT_INITIAL_ANGLE;
    config.params.servo.pwmChannel = SERVO_DEFAULT_PWM_CHANNEL;
    config.params.servo.pwmResolution = SERVO_DEFAULT_PWM_RESOLUTION;
    config.params.servo.inverted = false;
    config.params.servo.releaseAfterMove = false;
}

inline bool isServoConfigValid(const PeripheralConfig& config) {
    const auto& servo = config.params.servo;
    if (servo.frequency < 40 || servo.frequency > 400 ||
        servo.minPulse < 100 || servo.maxPulse > 5000 ||
        servo.minPulse >= servo.maxPulse ||
        servo.minAngle < 0 || servo.maxAngle > 360 ||
        servo.minAngle >= servo.maxAngle ||
        servo.initialAngle < servo.minAngle || servo.initialAngle > servo.maxAngle ||
        servo.pwmResolution < 1 || servo.pwmResolution > 16) {
        return false;
    }
    const uint32_t periodUs = 1000000UL / servo.frequency;
    return servo.maxPulse < periodUs;
}

inline uint16_t servoAngleToPulseUs(int angle, int minAngle, int maxAngle,
                                    uint16_t minPulse, uint16_t maxPulse,
                                    bool inverted) {
    if (maxAngle <= minAngle || maxPulse <= minPulse) return minPulse;
    if (angle < minAngle) angle = minAngle;
    if (angle > maxAngle) angle = maxAngle;

    int32_t offset = static_cast<int32_t>(angle) - minAngle;
    const int32_t angleSpan = static_cast<int32_t>(maxAngle) - minAngle;
    if (inverted) offset = angleSpan - offset;
    const uint32_t pulseSpan = static_cast<uint32_t>(maxPulse - minPulse);
    return static_cast<uint16_t>(minPulse +
        (pulseSpan * static_cast<uint32_t>(offset) + static_cast<uint32_t>(angleSpan / 2)) /
            static_cast<uint32_t>(angleSpan));
}

inline uint32_t servoPulseUsToDuty(uint16_t pulseUs, uint32_t frequency,
                                   uint8_t resolution) {
    if (frequency == 0) frequency = SERVO_DEFAULT_FREQUENCY_HZ;
    if (resolution < 1 || resolution > 16) resolution = SERVO_DEFAULT_PWM_RESOLUTION;
    const uint32_t maxDuty = (1UL << resolution) - 1UL;
    const uint64_t scaled = static_cast<uint64_t>(pulseUs) * frequency * maxDuty;
    const uint64_t rounded = (scaled + 500000ULL) / 1000000ULL;
    return static_cast<uint32_t>(rounded > maxDuty ? maxDuty : rounded);
}

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
            uint8_t port;
            uint16_t rxAvailable;
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

        struct {
            int16_t currentAngle;
            uint16_t currentPulseUs;
            bool signalAttached;
        } servo;

        struct {
            int64_t count;
            int64_t delta;
            int8_t direction;
            float frequencyHz;
            float rpm;
        } encoder;
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
