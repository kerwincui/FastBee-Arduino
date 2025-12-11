#ifndef GPIO_MANAGER_H
#define GPIO_MANAGER_H

#include <Arduino.h>
#include <map>
#include <vector>
#include <functional>
#include "../systems/GpioConfig.h"

class GPIOManager {
public:
    static GPIOManager& getInstance();
    
    // 禁止拷贝
    GPIOManager(const GPIOManager&) = delete;
    GPIOManager& operator=(const GPIOManager&) = delete;
    
    // 初始化
    bool initialize();
    
    // GPIO配置管理
    bool configurePin(const GPIOConfig& config);
    bool configurePins(const std::vector<GPIOConfig>& configs);
    bool reconfigurePin(uint8_t pin, GPIOMode newMode);
    
    // 状态操作
    GPIOState readPin(uint8_t pin);
    GPIOState readPin(const String& name);
    bool writePin(uint8_t pin, GPIOState state);
    bool writePin(const String& name, GPIOState state);
    bool togglePin(uint8_t pin);
    bool togglePin(const String& name);
    
    // 模拟操作
    uint16_t readAnalog(uint8_t pin);
    uint16_t readAnalog(const String& name);
    bool writePWM(uint8_t pin, uint32_t dutyCycle);
    bool writePWM(const String& name, uint32_t dutyCycle);
    
    // 中断管理
    bool attachInterrupt(uint8_t pin, GPIOMode interruptMode, GPIOInterruptCallback callback);
    bool attachInterrupt(const String& name, GPIOMode interruptMode, GPIOInterruptCallback callback);
    bool detachInterrupt(uint8_t pin);
    bool detachInterrupt(const String& name);
    
    // 状态查询
    GPIOMode getPinMode(uint8_t pin) const;
    GPIOMode getPinMode(const String& name) const;
    String getPinName(uint8_t pin) const;
    bool isPinConfigured(uint8_t pin) const;
    bool isPinConfigured(const String& name) const;
    
    // 系统状态
    void printStatus() const;
    std::vector<String> getConfiguredPins() const;
    
    // 持久化
    bool saveConfiguration();
    bool loadConfiguration();

private:
    GPIOManager() = default;
    
    // 内部引脚状态
    struct PinState {
        GPIOConfig config;
        GPIOState currentState;
        unsigned long lastChangeTime;
        bool interruptAttached;
        
        PinState() : currentState(GPIOState::STATE_UNDEFINED), 
                    lastChangeTime(0), interruptAttached(false) {}
    };
    
    std::map<uint8_t, PinState> pinStates;
    std::map<String, uint8_t> nameToPin;
    
    // 内部方法
    bool isValidPin(uint8_t pin) const;
    bool isValidModeForPin(uint8_t pin, GPIOMode mode) const;
    bool setupPinMode(const GPIOConfig& config);
    bool setupPWM(const GPIOConfig& config);
    void handleInterrupt(uint8_t pin);
    
    // 静态中断处理函数
    static void IRAM_ATTR isrHandler(void* arg);
};

#endif