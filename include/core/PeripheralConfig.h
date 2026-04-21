#ifndef PERIPHERAL_CONFIG_H
#define PERIPHERAL_CONFIG_H

#include <Arduino.h>
#include "PeripheralTypes.h"
#include "../systems/GpioConfig.h"  // 引用已有的GPIOState定义

// GPIO中断回调函数类型
using GPIOInterruptCallback = void(*)(uint8_t pin, GPIOState state);

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
            GPIOInterruptCallback interruptCallback;
        } gpio;
        
        // ADC参数
        struct {
            uint8_t attenuation;      // 衰减系数 0-3
            uint8_t resolution;       // 分辨率 9-12位
            uint16_t sampleRate;      // 采样率
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
        
        // LCD参数
        struct {
            uint8_t width;            // 宽度
            uint8_t height;           // 高度
            uint8_t interface;        // 0=Parallel, 1=SPI, 2=I2C
        } lcd;
        
        // 通用传感器参数
        struct {
            uint8_t sensorType;       // 传感器类型ID
            uint32_t sampleInterval;  // 采样间隔(ms)
        } sensor;
        
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
        // 默认初始化联合体
        params.gpio.initialState = GPIOState::STATE_LOW;
        params.gpio.pwmChannel = 0;
        params.gpio.pwmFrequency = 1000;
        params.gpio.pwmResolution = 8;
        params.gpio.defaultDuty = 0;
        params.gpio.interruptCallback = nullptr;
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

// 外设运行时状态
struct PeripheralRuntimeState {
    String id;
    PeripheralStatus status;
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
        : status(PeripheralStatus::PERIPHERAL_DISABLED), initTime(0),
          lastActivity(0), errorCount(0) {}
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
