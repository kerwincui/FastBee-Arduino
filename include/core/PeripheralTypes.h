#ifndef PERIPHERAL_TYPES_H
#define PERIPHERAL_TYPES_H

#include <Arduino.h>

// 外设接口类型枚举
enum class PeripheralType {
    UNCONFIGURED = 0,
    
    // 通信接口 (1-10)
    UART = 1,           // 串口
    I2C,                // I2C总线
    SPI,                // SPI总线
    CAN,                // CAN总线
    USB,                // USB接口
    
    // GPIO接口 (11-25) - 保持兼容现有GPIO功能
    GPIO_DIGITAL_INPUT = 11,
    GPIO_DIGITAL_OUTPUT,
    GPIO_DIGITAL_INPUT_PULLUP,
    GPIO_DIGITAL_INPUT_PULLDOWN,
    GPIO_ANALOG_INPUT,
    GPIO_ANALOG_OUTPUT,
    GPIO_PWM_OUTPUT,
    GPIO_INTERRUPT_RISING,
    GPIO_INTERRUPT_FALLING,
    GPIO_INTERRUPT_CHANGE,
    GPIO_TOUCH,
    
    // 模拟信号接口 (26-30)
    ADC = 26,           // 模数转换
    DAC,                // 数模转换
    
    // 调试接口 (31-35)
    JTAG = 31,          // JTAG调试
    SWD,                // SWD调试
    
    // 专用外设接口 (36-50)
    LCD = 36,           // LCD显示
    SDIO,               // SD卡接口
    SENSOR,             // 通用传感器
    CAMERA,             // 摄像头接口
    ETHERNET,           // 以太网
    PWM_SERVO,          // 舵机PWM
    STEPPER_MOTOR,      // 步进电机
    ENCODER,            // 编码器
    ONE_WIRE,           // 单总线
    NEO_PIXEL,          // WS2812等LED
    BUZZER,             // 蜂鸣器（数字驱动，支持 beep/long/alarm/sos 预设节奏）
    
    // Modbus外设 (51-55)
    MODBUS_DEVICE = 51  // Modbus子设备（继电器/PWM/PID等）
};

// 外设状态枚举
enum class PeripheralStatus {
    PERIPHERAL_DISABLED = 0,    // 禁用 (避免与esp32-hal-gpio.h的DISABLED宏冲突)
    PERIPHERAL_ENABLED,         // 启用但未初始化
    PERIPHERAL_INITIALIZED,     // 已初始化
    PERIPHERAL_RUNNING,         // 运行中
    PERIPHERAL_ERROR            // 错误状态
};

// 外设类别（用于前端分组显示）
// 注意：避免使用ANALOG等被esp32-hal-gpio.h定义为宏的名称
enum class PeripheralCategory {
    CATEGORY_COMMUNICATION = 1,  // 通信接口
    CATEGORY_GPIO,               // GPIO接口
    CATEGORY_ANALOG_SIGNAL,      // 模拟信号
    CATEGORY_DEBUG,              // 调试接口
    CATEGORY_SPECIAL             // 专用外设
};

// 获取外设类别的字符串名称
inline const char* getCategoryName(PeripheralCategory category) {
    switch (category) {
        case PeripheralCategory::CATEGORY_COMMUNICATION: return "communication";
        case PeripheralCategory::CATEGORY_GPIO: return "gpio";
        case PeripheralCategory::CATEGORY_ANALOG_SIGNAL: return "analog";
        case PeripheralCategory::CATEGORY_DEBUG: return "debug";
        case PeripheralCategory::CATEGORY_SPECIAL: return "special";
        default: return "unknown";
    }
}

// 获取外设类型的类别
inline PeripheralCategory getPeripheralCategory(PeripheralType type) {
    int typeValue = static_cast<int>(type);
    if (typeValue >= 1 && typeValue <= 10) return PeripheralCategory::CATEGORY_COMMUNICATION;
    if (typeValue >= 11 && typeValue <= 25) return PeripheralCategory::CATEGORY_GPIO;
    if (typeValue >= 26 && typeValue <= 30) return PeripheralCategory::CATEGORY_ANALOG_SIGNAL;
    if (typeValue >= 31 && typeValue <= 35) return PeripheralCategory::CATEGORY_DEBUG;
    if (typeValue >= 36 && typeValue <= 50) return PeripheralCategory::CATEGORY_SPECIAL;
    if (typeValue >= 51 && typeValue <= 55) return PeripheralCategory::CATEGORY_SPECIAL;
    return PeripheralCategory::CATEGORY_GPIO;
}

// 获取外设类型的字符串名称
inline const char* getPeripheralTypeName(PeripheralType type) {
    switch (type) {
        // 通信接口
        case PeripheralType::UART: return "UART";
        case PeripheralType::I2C: return "I2C";
        case PeripheralType::SPI: return "SPI";
        case PeripheralType::CAN: return "CAN";
        case PeripheralType::USB: return "USB";
        
        // GPIO接口
        case PeripheralType::GPIO_DIGITAL_INPUT: return "Digital Input";
        case PeripheralType::GPIO_DIGITAL_OUTPUT: return "Digital Output";
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLUP: return "Digital Input (Pull-up)";
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN: return "Digital Input (Pull-down)";
        case PeripheralType::GPIO_ANALOG_INPUT: return "Analog Input";
        case PeripheralType::GPIO_ANALOG_OUTPUT: return "Analog Output";
        case PeripheralType::GPIO_PWM_OUTPUT: return "PWM Output";
        case PeripheralType::GPIO_INTERRUPT_RISING: return "Interrupt (Rising)";
        case PeripheralType::GPIO_INTERRUPT_FALLING: return "Interrupt (Falling)";
        case PeripheralType::GPIO_INTERRUPT_CHANGE: return "Interrupt (Change)";
        case PeripheralType::GPIO_TOUCH: return "Touch";
        
        // 模拟信号
        case PeripheralType::ADC: return "ADC";
        case PeripheralType::DAC: return "DAC";
        
        // 调试接口
        case PeripheralType::JTAG: return "JTAG";
        case PeripheralType::SWD: return "SWD";
        
        // 专用外设
        case PeripheralType::LCD: return "LCD";
        case PeripheralType::SDIO: return "SDIO";
        case PeripheralType::SENSOR: return "Sensor";
        case PeripheralType::CAMERA: return "Camera";
        case PeripheralType::ETHERNET: return "Ethernet";
        case PeripheralType::PWM_SERVO: return "Servo";
        case PeripheralType::STEPPER_MOTOR: return "Stepper Motor";
        case PeripheralType::ENCODER: return "Encoder";
        case PeripheralType::ONE_WIRE: return "OneWire";
        case PeripheralType::NEO_PIXEL: return "NeoPixel";
        case PeripheralType::BUZZER: return "Buzzer";
        
        // Modbus外设
        case PeripheralType::MODBUS_DEVICE: return "Modbus Device";
        
        default: return "Unknown";
    }
}

// 获取外设类型所需引脚数量
inline uint8_t getPeripheralPinCount(PeripheralType type) {
    switch (type) {
        // 单引脚
        case PeripheralType::GPIO_DIGITAL_INPUT:
        case PeripheralType::GPIO_DIGITAL_OUTPUT:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLUP:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN:
        case PeripheralType::GPIO_ANALOG_INPUT:
        case PeripheralType::GPIO_ANALOG_OUTPUT:
        case PeripheralType::GPIO_PWM_OUTPUT:
        case PeripheralType::GPIO_INTERRUPT_RISING:
        case PeripheralType::GPIO_INTERRUPT_FALLING:
        case PeripheralType::GPIO_INTERRUPT_CHANGE:
        case PeripheralType::GPIO_TOUCH:
        case PeripheralType::ADC:
        case PeripheralType::DAC:
        case PeripheralType::ONE_WIRE:
        case PeripheralType::NEO_PIXEL:
        case PeripheralType::BUZZER:
            return 1;
            
        // 双引脚
        case PeripheralType::UART:
        case PeripheralType::I2C:
        case PeripheralType::ENCODER:
            return 2;
            
        // 三引脚
        case PeripheralType::SPI:
            return 4;  // MISO, MOSI, SCK, CS
            
        // 四引脚
        case PeripheralType::STEPPER_MOTOR:
            return 4;
            
        // 多引脚（可变）
        case PeripheralType::LCD:
        case PeripheralType::SDIO:
        case PeripheralType::CAMERA:
        case PeripheralType::ETHERNET:
        case PeripheralType::JTAG:
        case PeripheralType::SWD:
        case PeripheralType::CAN:
        case PeripheralType::USB:
        case PeripheralType::SENSOR:
        case PeripheralType::PWM_SERVO:
            return 8;  // 最大支持8个引脚
        
        // Modbus外设不使用GPIO引脚
        case PeripheralType::MODBUS_DEVICE:
            return 0;
            
        default:
            return 1;
    }
}

// 从字符串解析外设类型
inline PeripheralType parsePeripheralType(const char* typeStr) {
    if (!typeStr) return PeripheralType::UNCONFIGURED;
    
    // 通信接口
    if (strcasecmp(typeStr, "UART") == 0) return PeripheralType::UART;
    if (strcasecmp(typeStr, "I2C") == 0) return PeripheralType::I2C;
    if (strcasecmp(typeStr, "SPI") == 0) return PeripheralType::SPI;
    if (strcasecmp(typeStr, "CAN") == 0) return PeripheralType::CAN;
    if (strcasecmp(typeStr, "USB") == 0) return PeripheralType::USB;
    
    // GPIO接口
    if (strcasecmp(typeStr, "GPIO_DIGITAL_INPUT") == 0) return PeripheralType::GPIO_DIGITAL_INPUT;
    if (strcasecmp(typeStr, "GPIO_DIGITAL_OUTPUT") == 0) return PeripheralType::GPIO_DIGITAL_OUTPUT;
    if (strcasecmp(typeStr, "GPIO_DIGITAL_INPUT_PULLUP") == 0) return PeripheralType::GPIO_DIGITAL_INPUT_PULLUP;
    if (strcasecmp(typeStr, "GPIO_DIGITAL_INPUT_PULLDOWN") == 0) return PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN;
    if (strcasecmp(typeStr, "GPIO_ANALOG_INPUT") == 0) return PeripheralType::GPIO_ANALOG_INPUT;
    if (strcasecmp(typeStr, "GPIO_ANALOG_OUTPUT") == 0) return PeripheralType::GPIO_ANALOG_OUTPUT;
    if (strcasecmp(typeStr, "GPIO_PWM_OUTPUT") == 0) return PeripheralType::GPIO_PWM_OUTPUT;
    if (strcasecmp(typeStr, "GPIO_INTERRUPT_RISING") == 0) return PeripheralType::GPIO_INTERRUPT_RISING;
    if (strcasecmp(typeStr, "GPIO_INTERRUPT_FALLING") == 0) return PeripheralType::GPIO_INTERRUPT_FALLING;
    if (strcasecmp(typeStr, "GPIO_INTERRUPT_CHANGE") == 0) return PeripheralType::GPIO_INTERRUPT_CHANGE;
    if (strcasecmp(typeStr, "GPIO_TOUCH") == 0) return PeripheralType::GPIO_TOUCH;
    
    // 模拟信号
    if (strcasecmp(typeStr, "ADC") == 0) return PeripheralType::ADC;
    if (strcasecmp(typeStr, "DAC") == 0) return PeripheralType::DAC;
    
    // 调试接口
    if (strcasecmp(typeStr, "JTAG") == 0) return PeripheralType::JTAG;
    if (strcasecmp(typeStr, "SWD") == 0) return PeripheralType::SWD;
    
    // 专用外设
    if (strcasecmp(typeStr, "LCD") == 0) return PeripheralType::LCD;
    if (strcasecmp(typeStr, "SDIO") == 0) return PeripheralType::SDIO;
    if (strcasecmp(typeStr, "SENSOR") == 0) return PeripheralType::SENSOR;
    if (strcasecmp(typeStr, "CAMERA") == 0) return PeripheralType::CAMERA;
    if (strcasecmp(typeStr, "ETHERNET") == 0) return PeripheralType::ETHERNET;
    if (strcasecmp(typeStr, "PWM_SERVO") == 0) return PeripheralType::PWM_SERVO;
    if (strcasecmp(typeStr, "STEPPER_MOTOR") == 0) return PeripheralType::STEPPER_MOTOR;
    if (strcasecmp(typeStr, "ENCODER") == 0) return PeripheralType::ENCODER;
    if (strcasecmp(typeStr, "ONE_WIRE") == 0) return PeripheralType::ONE_WIRE;
    if (strcasecmp(typeStr, "NEO_PIXEL") == 0) return PeripheralType::NEO_PIXEL;
    if (strcasecmp(typeStr, "BUZZER") == 0) return PeripheralType::BUZZER;
    
    // Modbus外设
    if (strcasecmp(typeStr, "MODBUS_DEVICE") == 0) return PeripheralType::MODBUS_DEVICE;
    
    return PeripheralType::UNCONFIGURED;
}

// 从整数值解析外设类型
inline PeripheralType peripheralTypeFromInt(int value) {
    if ((value >= 0 && value <= 50) || (value >= 51 && value <= 55)) {
        return static_cast<PeripheralType>(value);
    }
    return PeripheralType::UNCONFIGURED;
}

// ========== 外设类型分类辅助方法 ==========

// 检查是否为数字输入类型（包括带上拉/下拉）
inline bool isDigitalInputType(PeripheralType type) {
    return type == PeripheralType::GPIO_DIGITAL_INPUT ||
           type == PeripheralType::GPIO_DIGITAL_INPUT_PULLUP ||
           type == PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN;
}

// 检查是否为模拟输入类型
inline bool isAnalogInputType(PeripheralType type) {
    return type == PeripheralType::GPIO_ANALOG_INPUT ||
           type == PeripheralType::ADC;
}

// 检查是否为输入类型（数字或模拟输入）
inline bool isInputType(PeripheralType type) {
    return isDigitalInputType(type) || isAnalogInputType(type);
}

// 检查是否为输出类型（数字输出、PWM、DAC、蜂鸣器等）
inline bool isOutputType(PeripheralType type) {
    int typeVal = static_cast<int>(type);
    // GPIO_DIGITAL_OUTPUT = 12, GPIO_ANALOG_OUTPUT = 16, GPIO_PWM_OUTPUT = 17
    // DAC = 27, BUZZER = 46（数字驱动输出）
    return typeVal == 12 || typeVal == 16 || typeVal == 17 || typeVal == 27 || typeVal == 46;
}

// 检查是否支持按键事件检测（仅上拉/下拉输入类型）
inline bool supportsButtonEvent(PeripheralType type) {
    return type == PeripheralType::GPIO_DIGITAL_INPUT_PULLUP ||
           type == PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN;
}

// 检查是否为纯输入类型（无内部上拉/下拉，用于编码器、UART RX等）
inline bool isPureInputType(PeripheralType type) {
    return type == PeripheralType::GPIO_DIGITAL_INPUT;
}

// 检查是否为GPIO类型外设
inline bool isGPIOType(PeripheralType type) {
    int typeVal = static_cast<int>(type);
    return typeVal >= 11 && typeVal <= 25;
}

// 检查是否为Modbus外设类型
inline bool isModbusType(PeripheralType type) {
    return type == PeripheralType::MODBUS_DEVICE;
}

#endif // PERIPHERAL_TYPES_H
