#ifndef GPIO_CONFIG_H
#define GPIO_CONFIG_H

#include <Arduino.h>
#include <driver/gpio.h>

// GPIO功能枚举
enum class GPIOMode {
    UNCONFIGURED = 0,
    DIGITAL_INPUT,
    DIGITAL_OUTPUT,
    DIGITAL_INPUT_PULLUP,
    DIGITAL_INPUT_PULLDOWN,
    ANALOG_INPUT,
    ANALOG_OUTPUT,
    PWM_OUTPUT,
    INTERRUPT_RISING,
    INTERRUPT_FALLING,
    INTERRUPT_CHANGE,
    TOUCH,
    I2C_SDA,
    I2C_SCL,
    SPI_MISO,
    SPI_MOSI,
    SPI_SCK,
    UART_TX,
    UART_RX
};

// GPIO状态枚举
enum class GPIOState {
    STATE_LOW = 0,
    STATE_HIGH = 1,
    STATE_UNDEFINED = 2
};

// GPIO中断回调函数类型
using GPIOInterruptCallback = void(*)(uint8_t pin, GPIOState state);

// GPIO配置结构体
struct GPIOConfig {
    uint8_t pin;
    String name;
    GPIOMode mode;
    GPIOState initialState;
    bool inverted;
    uint8_t pwmChannel;      // PWM通道 (0-15)
    uint32_t pwmFrequency;   // PWM频率
    uint8_t pwmResolution;   // PWM分辨率 (1-16位)
    uint16_t debounceMs;     // 消抖时间
    GPIOInterruptCallback interruptCallback;
    
    // 默认构造函数
    GPIOConfig() 
        : pin(0), name(""), mode(GPIOMode::UNCONFIGURED), 
          initialState(GPIOState::STATE_LOW), inverted(false),
          pwmChannel(0), pwmFrequency(1000), pwmResolution(8),
          debounceMs(50), interruptCallback(nullptr) {}
};

// 预定义GPIO配置
namespace GPIO_PINS {
    // 系统LED
    extern const GPIOConfig SYSTEM_LED;
    
    // 用户按钮
    extern const GPIOConfig USER_BUTTON;
    
    // I2C总线
    extern const GPIOConfig I2C_SDA;
    extern const GPIOConfig I2C_SCL;
    
    // SPI总线
    extern const GPIOConfig SPI_MISO;
    extern const GPIOConfig SPI_MOSI;
    extern const GPIOConfig SPI_SCK;
    
    // 传感器引脚
    extern const GPIOConfig TEMP_SENSOR;
    extern const GPIOConfig HUMIDITY_SENSOR;
    
    // 执行器引脚
    extern const GPIOConfig RELAY_1;
    extern const GPIOConfig RELAY_2;
}

#endif