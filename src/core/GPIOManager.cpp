/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:29:30
 */

#include "core/GPIOManager.h"
#include "systems/LoggerSystem.h"

GPIOManager& GPIOManager::getInstance() {
    static GPIOManager instance;
    return instance;
}

bool GPIOManager::initialize() {
    Serial.println("Initializing GPIO Manager");
    
    // 初始化默认引脚配置
    std::vector<GPIOConfig> defaultPins = {
        GPIO_PINS::SYSTEM_LED,
        GPIO_PINS::USER_BUTTON,
        GPIO_PINS::I2C_SDA,
        GPIO_PINS::I2C_SCL,
        GPIO_PINS::SPI_MISO,
        GPIO_PINS::SPI_MOSI,
        GPIO_PINS::SPI_SCK,
        GPIO_PINS::TEMP_SENSOR,
        GPIO_PINS::HUMIDITY_SENSOR,
        GPIO_PINS::RELAY_1,
        GPIO_PINS::RELAY_2
    };
    
    return configurePins(defaultPins);
}

bool GPIOManager::configurePin(const GPIOConfig& config) {
    if (!isValidPin(config.pin)) {
        // Serial.println("Invalid pin: %d", config.pin);
        return false;
    }
    
    if (!isValidModeForPin(config.pin, config.mode)) {
        // Serial.println("Invalid mode for pin %d", config.pin);
        return false;
    }
    
    // 如果引脚已配置，先清理
    if (pinStates.find(config.pin) != pinStates.end()) {
        detachInterrupt(config.pin);
    }
    
    // 配置引脚
    PinState pinState;
    pinState.config = config;
    
    if (!setupPinMode(config)) {
        // Serial.println("Failed to setup pin mode for pin %d", config.pin);
        return false;
    }
    
    // 设置初始状态
    if (config.mode == GPIOMode::DIGITAL_OUTPUT || 
        config.mode == GPIOMode::PWM_OUTPUT ||
        config.mode == GPIOMode::ANALOG_OUTPUT) {
        writePin(config.pin, config.initialState);
    }
    
    pinState.currentState = config.initialState;
    pinState.lastChangeTime = millis();
    
    // 保存状态
    pinStates[config.pin] = pinState;
    nameToPin[config.name] = config.pin;
    
    // Serial.println("Configured pin %d (%s) as %d", 
    //          config.pin, config.name.c_str(), static_cast<int>(config.mode));
    
    return true;
}

bool GPIOManager::configurePins(const std::vector<GPIOConfig>& configs) {
    bool allSuccess = true;
    
    for (const auto& config : configs) {
        if (!configurePin(config)) {
            allSuccess = false;
            // Serial.println("Failed to configure pin %d", config.pin);
        }
    }
    
    return allSuccess;
}

bool GPIOManager::setupPinMode(const GPIOConfig& config) {
    switch (config.mode) {
        case GPIOMode::DIGITAL_INPUT:
            pinMode(config.pin, INPUT);
            break;
            
        case GPIOMode::DIGITAL_INPUT_PULLUP:
            pinMode(config.pin, INPUT_PULLUP);
            break;
            
        case GPIOMode::DIGITAL_INPUT_PULLDOWN:
            pinMode(config.pin, INPUT_PULLDOWN);
            break;
            
        case GPIOMode::DIGITAL_OUTPUT:
            pinMode(config.pin, OUTPUT);
            break;
            
        case GPIOMode::ANALOG_INPUT:
            // ESP32模拟输入不需要特殊设置
            break;
            
        case GPIOMode::PWM_OUTPUT:
            return setupPWM(config);
            
        case GPIOMode::I2C_SDA:
        case GPIOMode::I2C_SCL:
            // I2C在Wire库中设置
            break;
            
        case GPIOMode::SPI_MISO:
        case GPIOMode::SPI_MOSI:
        case GPIOMode::SPI_SCK:
            // SPI在SPI库中设置
            break;
            
        default:
            // Serial.println("Unhandled pin mode for pin %d", config.pin);
            break;
    }
    
    return true;
}

bool GPIOManager::setupPWM(const GPIOConfig& config) {
    if (config.pwmChannel > 15) {
        // Serial.println("Invalid PWM channel: %d", config.pwmChannel);
        return false;
    }
    
    ledcSetup(config.pwmChannel, config.pwmFrequency, config.pwmResolution);
    ledcAttachPin(config.pin, config.pwmChannel);
    
    // Serial.println("PWM configured: pin=%d, channel=%d, freq=%d, resolution=%d",
    //           config.pin, config.pwmChannel, config.pwmFrequency, config.pwmResolution);
    
    return true;
}

GPIOState GPIOManager::readPin(uint8_t pin) {
    auto it = pinStates.find(pin);
    if (it == pinStates.end()) {
        // Serial.println("Pin %d not configured", pin);
        return GPIOState::STATE_UNDEFINED;
    }
    
    const auto& config = it->second.config;
    GPIOState state;
    
    switch (config.mode) {
        case GPIOMode::DIGITAL_INPUT:
        case GPIOMode::DIGITAL_INPUT_PULLUP:
        case GPIOMode::DIGITAL_INPUT_PULLDOWN:
            state = digitalRead(pin) ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
            break;
            
        case GPIOMode::ANALOG_INPUT:
            // 模拟值需要转换为状态，这里简化处理
            state = analogRead(pin) > 2048 ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
            break;
            
        default:
            state = it->second.currentState;
            break;
    }
    
    // 处理反向逻辑
    if (config.inverted) {
        state = (state == GPIOState::STATE_HIGH) ? GPIOState::STATE_LOW : GPIOState::STATE_HIGH;
    }
    
    return state;
}

GPIOState GPIOManager::readPin(const String& name) {
    auto it = nameToPin.find(name);
    if (it == nameToPin.end()) {
        // Serial.println("Pin name '%s' not found", name.c_str());
        return GPIOState::STATE_UNDEFINED;
    }
    
    return readPin(it->second);
}

bool GPIOManager::writePin(uint8_t pin, GPIOState state) {
    auto it = pinStates.find(pin);
    if (it == pinStates.end()) {
        // Serial.println("Pin %d not configured", pin);
        return false;
    }
    
    auto& pinState = it->second;
    auto& config = pinState.config;
    
    // 处理反向逻辑
    GPIOState physicalState = state;
    if (config.inverted) {
        physicalState = (physicalState == GPIOState::STATE_HIGH) ? GPIOState::STATE_LOW : GPIOState::STATE_HIGH;
    }
    
    bool success = true;
    
    switch (config.mode) {
        case GPIOMode::DIGITAL_OUTPUT:
            digitalWrite(pin, physicalState == GPIOState::STATE_HIGH ? HIGH : LOW);
            break;
            
        case GPIOMode::PWM_OUTPUT:
            // 对于PWM，将状态转换为占空比
            if (state == GPIOState::STATE_HIGH) {
                ledcWrite(config.pwmChannel, (1 << config.pwmResolution) - 1);
            } else {
                ledcWrite(config.pwmChannel, 0);
            }
            break;
            
        case GPIOMode::ANALOG_OUTPUT:
            // ESP32不支持真正的模拟输出，使用PWM模拟
            ledcWrite(config.pwmChannel, (1 << config.pwmResolution) - 1);
            break;
            
        default:
            // Serial.println("Pin %d is not configured as output", pin);
            success = false;
            break;
    }
    
    if (success) {
        pinState.currentState = state;
        pinState.lastChangeTime = millis();
        // Serial.println("Pin %d set to %d", pin, static_cast<int>(state));
    }
    
    return success;
}

bool GPIOManager::writePin(const String& name, GPIOState state) {
    auto it = nameToPin.find(name);
    if (it == nameToPin.end()) {
        // Serial.println("Pin name '%s' not found", name.c_str());
        return false;
    }
    
    return writePin(it->second, state);
}

// 其他方法实现...
bool GPIOManager::isValidPin(uint8_t pin) const {
    // ESP32的有效GPIO引脚范围
    if (pin < 0 || pin > 39) return false;
    
    // 内部Flash使用的引脚
    if (pin == 6 || pin == 7 || pin == 8 || pin == 9 || pin == 10 || pin == 11) {
        return false;
    }
    
    return true;
}

void GPIOManager::printStatus() const {
    Serial.println("=== GPIO Status ===");
    // Serial.println("Configured pins: %d", pinStates.size());
    
    for (const auto& pair : pinStates) {
        const auto& state = pair.second;
        // Serial.println("Pin %2d: %-15s Mode:%-20s State:%-8s", 
        //          state.config.pin,
        //          state.config.name.c_str(),
        //          String(static_cast<int>(state.config.mode)).c_str(),
        //          String(static_cast<int>(state.currentState)).c_str());
    }
}