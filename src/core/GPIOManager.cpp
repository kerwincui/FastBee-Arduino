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
    LOG_INFO("GPIO Manager: Initializing...");
    
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
        return false;
    }
    
    if (!isValidModeForPin(config.pin, config.mode)) {
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
    
    return true;
}

bool GPIOManager::configurePins(const std::vector<GPIOConfig>& configs) {
    bool allSuccess = true;
    for (const auto& config : configs) {
        if (!configurePin(config)) {
            allSuccess = false;
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
            break;
    }
    
    return true;
}

bool GPIOManager::setupPWM(const GPIOConfig& config) {
    if (config.pwmChannel > 15) {
        return false;
    }
    
    ledcSetup(config.pwmChannel, config.pwmFrequency, config.pwmResolution);
    ledcAttachPin(config.pin, config.pwmChannel);
    
    return true;
}

GPIOState GPIOManager::readPin(uint8_t pin) {
    auto it = pinStates.find(pin);
    if (it == pinStates.end()) {
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
        return GPIOState::STATE_UNDEFINED;
    }
    return readPin(it->second);
}

bool GPIOManager::writePin(uint8_t pin, GPIOState state) {
    auto it = pinStates.find(pin);
    if (it == pinStates.end()) {
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
            if (state == GPIOState::STATE_HIGH) {
                ledcWrite(config.pwmChannel, (1 << config.pwmResolution) - 1);
            } else {
                ledcWrite(config.pwmChannel, 0);
            }
            break;
            
        case GPIOMode::ANALOG_OUTPUT:
            // ANALOG_OUTPUT 使用 PWM 模拟，STATE_HIGH=满值，STATE_LOW=0
            ledcWrite(config.pwmChannel,
                physicalState == GPIOState::STATE_HIGH
                    ? (uint32_t)((1U << config.pwmResolution) - 1) : 0U);
            break;
            
        default:
            success = false;
            break;
    }
    
    if (success) {
        pinState.currentState = state;
        pinState.lastChangeTime = millis();
    }
    
    return success;
}

bool GPIOManager::writePin(const String& name, GPIOState state) {
    auto it = nameToPin.find(name);
    if (it == nameToPin.end()) {
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
    LOG_INFO("=== GPIO Status ===");
    for (const auto& pair : pinStates) {
        const auto& state = pair.second;
        char buf[80];
        snprintf(buf, sizeof(buf), "  Pin%2d [%-12s] mode=%d state=%d",
                 state.config.pin,
                 state.config.name.c_str(),
                 static_cast<int>(state.config.mode),
                 static_cast<int>(state.currentState));
        LOG_INFO(buf);
    }
}

// ============ 补全声明的缺失方法 ============

bool GPIOManager::isValidModeForPin(uint8_t pin, GPIOMode mode) const {
    if (!isValidPin(pin)) return false;
    // GPIO 34~39 是输入专用引脚，不能配置为输出
    if (pin >= 34 && pin <= 39) {
        return mode == GPIOMode::ANALOG_INPUT
            || mode == GPIOMode::DIGITAL_INPUT
            || mode == GPIOMode::DIGITAL_INPUT_PULLUP;
    }
    return true;
}

bool GPIOManager::reconfigurePin(uint8_t pin, GPIOMode newMode) {
    auto it = pinStates.find(pin);
    if (it == pinStates.end()) return false;

    GPIOConfig newConfig = it->second.config;
    newConfig.mode = newMode;

    // 先解绑中断
    detachInterrupt(pin);

    // 删除旧状态
    auto nameIt = nameToPin.begin();
    while (nameIt != nameToPin.end()) {
        if (nameIt->second == pin) {
            nameIt = nameToPin.erase(nameIt);
        } else {
            ++nameIt;
        }
    }
    pinStates.erase(pin);

    return configurePin(newConfig);
}

bool GPIOManager::togglePin(uint8_t pin) {
    GPIOState current = readPin(pin);
    if (current == GPIOState::STATE_UNDEFINED) return false;
    return writePin(pin, current == GPIOState::STATE_HIGH
                        ? GPIOState::STATE_LOW : GPIOState::STATE_HIGH);
}

bool GPIOManager::togglePin(const String& name) {
    auto it = nameToPin.find(name);
    if (it == nameToPin.end()) return false;
    return togglePin(it->second);
}

uint16_t GPIOManager::readAnalog(uint8_t pin) {
    auto it = pinStates.find(pin);
    if (it == pinStates.end()) return 0;
    if (it->second.config.mode != GPIOMode::ANALOG_INPUT) return 0;
    return (uint16_t)analogRead(pin);
}

uint16_t GPIOManager::readAnalog(const String& name) {
    auto it = nameToPin.find(name);
    if (it == nameToPin.end()) return 0;
    return readAnalog(it->second);
}

bool GPIOManager::writePWM(uint8_t pin, uint32_t dutyCycle) {
    auto it = pinStates.find(pin);
    if (it == pinStates.end()) return false;
    const auto& config = it->second.config;
    if (config.mode != GPIOMode::PWM_OUTPUT && config.mode != GPIOMode::ANALOG_OUTPUT) return false;
    uint32_t maxVal = (1U << config.pwmResolution) - 1;
    ledcWrite(config.pwmChannel, dutyCycle > maxVal ? maxVal : dutyCycle);
    return true;
}

bool GPIOManager::writePWM(const String& name, uint32_t dutyCycle) {
    auto it = nameToPin.find(name);
    if (it == nameToPin.end()) return false;
    return writePWM(it->second, dutyCycle);
}

bool GPIOManager::attachInterrupt(uint8_t pin, GPIOMode interruptMode, GPIOInterruptCallback callback) {
    auto it = pinStates.find(pin);
    if (it == pinStates.end()) return false;
    it->second.config.interruptCallback = callback;
    uint8_t mode = CHANGE;
    if (interruptMode == GPIOMode::DIGITAL_INPUT_PULLUP) mode = FALLING;
    else if (interruptMode == GPIOMode::DIGITAL_INPUT_PULLDOWN) mode = RISING;
    ::attachInterruptArg(digitalPinToInterrupt(pin), isrHandler, (void*)(uintptr_t)pin, mode);
    it->second.interruptAttached = true;
    return true;
}

bool GPIOManager::attachInterrupt(const String& name, GPIOMode interruptMode, GPIOInterruptCallback callback) {
    auto it = nameToPin.find(name);
    if (it == nameToPin.end()) return false;
    return attachInterrupt(it->second, interruptMode, callback);
}

bool GPIOManager::detachInterrupt(uint8_t pin) {
    auto it = pinStates.find(pin);
    if (it == pinStates.end()) return false;
    if (it->second.interruptAttached) {
        ::detachInterrupt(digitalPinToInterrupt(pin));
        it->second.interruptAttached = false;
    }
    return true;
}

bool GPIOManager::detachInterrupt(const String& name) {
    auto it = nameToPin.find(name);
    if (it == nameToPin.end()) return false;
    return detachInterrupt(it->second);
}

GPIOMode GPIOManager::getPinMode(uint8_t pin) const {
    auto it = pinStates.find(pin);
    return (it != pinStates.end()) ? it->second.config.mode : GPIOMode::DIGITAL_INPUT;
}

GPIOMode GPIOManager::getPinMode(const String& name) const {
    auto it = nameToPin.find(name);
    if (it == nameToPin.end()) return GPIOMode::DIGITAL_INPUT;
    return getPinMode(it->second);
}

String GPIOManager::getPinName(uint8_t pin) const {
    for (const auto& pair : nameToPin) {
        if (pair.second == pin) return pair.first;
    }
    return "";
}

bool GPIOManager::isPinConfigured(uint8_t pin) const {
    return pinStates.find(pin) != pinStates.end();
}

bool GPIOManager::isPinConfigured(const String& name) const {
    return nameToPin.find(name) != nameToPin.end();
}

std::vector<String> GPIOManager::getConfiguredPins() const {
    std::vector<String> names;
    names.reserve(nameToPin.size());
    for (const auto& pair : nameToPin) {
        names.push_back(pair.first);
    }
    return names;
}

bool GPIOManager::saveConfiguration() {
    // TODO: 使用 ConfigStorage::saveJSONConfig("/config/gpio.json") 持久化
    LOG_WARNING("GPIOManager::saveConfiguration() not yet implemented");
    return false;
}

bool GPIOManager::loadConfiguration() {
    // TODO: 使用 ConfigStorage::loadJSONConfig("/config/gpio.json") 恢复
    LOG_WARNING("GPIOManager::loadConfiguration() not yet implemented");
    return false;
}

void IRAM_ATTR GPIOManager::isrHandler(void* arg) {
    // 中断上下文：仅记录触发的引脚号，实际回调在 loop() 中处理
    // （完整实现需要 FreeRTOS 队列，此处保留桩以满足编译）
    (void)arg;
}

void GPIOManager::handleInterrupt(uint8_t pin) {
    auto it = pinStates.find(pin);
    if (it == pinStates.end()) return;
    if (it->second.config.interruptCallback) {
        GPIOState state = readPin(pin);
        it->second.config.interruptCallback(pin, state);
    }
}