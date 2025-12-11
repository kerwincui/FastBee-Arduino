/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:32:31
 */

#include "systems/GpioConfig.h"

// 辅助函数用于创建配置
namespace GPIOConfigHelper {
    GPIOConfig createDigitalOutput(uint8_t pin, const String& name, 
                                   GPIOState initialState = GPIOState::STATE_LOW, 
                                   bool inverted = false) {
        GPIOConfig config;
        config.pin = pin;
        config.name = name;
        config.mode = GPIOMode::DIGITAL_OUTPUT;
        config.initialState = initialState;
        config.inverted = inverted;
        return config;
    }
    
    GPIOConfig createDigitalInput(uint8_t pin, const String& name, 
                                  GPIOMode mode = GPIOMode::DIGITAL_INPUT,
                                  uint16_t debounceMs = 50) {
        GPIOConfig config;
        config.pin = pin;
        config.name = name;
        config.mode = mode;
        config.debounceMs = debounceMs;
        return config;
    }
    
    GPIOConfig createAnalogInput(uint8_t pin, const String& name) {
        GPIOConfig config;
        config.pin = pin;
        config.name = name;
        config.mode = GPIOMode::ANALOG_INPUT;
        return config;
    }
    
    GPIOConfig createBusPin(uint8_t pin, const String& name, GPIOMode mode) {
        GPIOConfig config;
        config.pin = pin;
        config.name = name;
        config.mode = mode;
        return config;
    }
}

// 使用辅助函数初始化
const GPIOConfig GPIO_PINS::SYSTEM_LED = GPIOConfigHelper::createDigitalOutput(2, "SYSTEM_LED");
const GPIOConfig GPIO_PINS::USER_BUTTON = GPIOConfigHelper::createDigitalInput(0, "USER_BUTTON", GPIOMode::DIGITAL_INPUT_PULLUP, 100);
const GPIOConfig GPIO_PINS::I2C_SDA = GPIOConfigHelper::createBusPin(21, "I2C_SDA", GPIOMode::I2C_SDA);
const GPIOConfig GPIO_PINS::I2C_SCL = GPIOConfigHelper::createBusPin(22, "I2C_SCL", GPIOMode::I2C_SCL);
const GPIOConfig GPIO_PINS::SPI_MISO = GPIOConfigHelper::createBusPin(19, "SPI_MISO", GPIOMode::SPI_MISO);
const GPIOConfig GPIO_PINS::SPI_MOSI = GPIOConfigHelper::createBusPin(23, "SPI_MOSI", GPIOMode::SPI_MOSI);
const GPIOConfig GPIO_PINS::SPI_SCK = GPIOConfigHelper::createBusPin(18, "SPI_SCK", GPIOMode::SPI_SCK);
const GPIOConfig GPIO_PINS::TEMP_SENSOR = GPIOConfigHelper::createAnalogInput(34, "TEMP_SENSOR");
const GPIOConfig GPIO_PINS::HUMIDITY_SENSOR = GPIOConfigHelper::createAnalogInput(35, "HUMIDITY_SENSOR");
const GPIOConfig GPIO_PINS::RELAY_1 = GPIOConfigHelper::createDigitalOutput(4, "RELAY_1", GPIOState::STATE_LOW, true);
const GPIOConfig GPIO_PINS::RELAY_2 = GPIOConfigHelper::createDigitalOutput(5, "RELAY_2", GPIOState::STATE_LOW, true);