/**
 * @file MockPeripheral.h
 * @brief 外设管理模拟对象
 * 
 * 提供外设管理功能的模拟实现，支持外设CRUD、GPIO控制、执行规则等
 */

#ifndef MOCK_PERIPHERAL_H
#define MOCK_PERIPHERAL_H

#include <Arduino.h>
#include <vector>
#include <map>
#include <functional>

// 外设类型枚举
enum class PeripheralType {
    GPIO_DIGITAL_INPUT = 0,
    GPIO_DIGITAL_OUTPUT = 1,
    GPIO_ANALOG_INPUT = 2,
    GPIO_PWM_OUTPUT = 3,
    GPIO_DAC_OUTPUT = 4,
    I2C_DEVICE = 10,
    SPI_DEVICE = 11,
    UART_DEVICE = 12,
    ONE_WIRE = 20,
    DHT_SENSOR = 21,
    DS18B20_SENSOR = 22,
    CUSTOM = 99
};

// GPIO状态枚举
enum class GPIOState {
    STATE_LOW = 0,
    STATE_HIGH = 1,
    STATE_PWM = 2,
    STATE_ANALOG = 3,
    STATE_UNKNOWN = -1
};

// 触发类型枚举
enum class TriggerType {
    PLATFORM_MQTT = 0,      // 平台触发（MQTT）
    DEVICE_LOCAL = 1,       // 设备触发
    TIMER_SCHEDULE = 2,     // 定时触发
    CONDITION_TRIGGER = 3   // 条件触发
};

// 动作类型枚举
enum class ActionType {
    SET_HIGH = 0,           // 设置高电平
    SET_LOW = 1,            // 设置低电平
    SET_PWM = 2,            // 设置PWM
    SET_DAC = 3,            // 设置DAC
    TOGGLE = 4,             // 翻转
    PULSE = 5,              // 脉冲
    SCRIPT_EXECUTE = 10,    // 执行脚本
    OLED_CUSTOM = 27        // OLED 自定义多行显示
};

// 外设配置结构
struct PeripheralConfig {
    String id;
    String name;
    PeripheralType type;
    int pin;
    bool enabled;
    std::map<String, String> properties;
    
    PeripheralConfig() : type(PeripheralType::GPIO_DIGITAL_OUTPUT), 
                         pin(-1), enabled(true) {}
};

// 执行规则结构
struct PeriphExecRule {
    String id;
    String name;
    bool enabled;
    TriggerType triggerType;
    ActionType actionType;
    String targetPeriphId;
    String triggerCondition;  // 触发条件表达式
    String actionValue;       // 动作值
    int priority;             // 优先级
    
    PeriphExecRule() : enabled(true), triggerType(TriggerType::PLATFORM_MQTT),
                       actionType(ActionType::SET_HIGH), priority(0) {
        // 生成唯一ID
        id = "rule_" + String(millis()) + "_" + String(random(1000));
    }
};

// 模拟外设类
class MockPeripheral {
public:
    MockPeripheral() : _pin(-1), _state(GPIOState::STATE_LOW), 
                       _pwmValue(0), _analogValue(0) {}

    MockPeripheral(int pin, PeripheralType type) 
        : _pin(pin), _type(type), _state(GPIOState::STATE_LOW),
          _pwmValue(0), _analogValue(0) {}

    // GPIO操作
    void digitalWrite(uint8_t value) {
        if (_type == PeripheralType::GPIO_DIGITAL_OUTPUT ||
            _type == PeripheralType::GPIO_PWM_OUTPUT) {
            _state = (value == HIGH) ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
            _digitalValue = value;
        }
    }

    int digitalRead() {
        if (_type == PeripheralType::GPIO_DIGITAL_INPUT) {
            return _digitalValue;
        }
        return (_state == GPIOState::STATE_HIGH) ? HIGH : LOW;
    }

    void analogWrite(int value) {
        if (_type == PeripheralType::GPIO_PWM_OUTPUT) {
            _pwmValue = constrain(value, 0, 255);
            _state = GPIOState::STATE_PWM;
        } else if (_type == PeripheralType::GPIO_DAC_OUTPUT) {
            _analogValue = constrain(value, 0, 255);
            _state = GPIOState::STATE_ANALOG;
        }
    }

    int analogRead() {
        if (_type == PeripheralType::GPIO_ANALOG_INPUT) {
            return _analogValue;
        }
        return 0;
    }

    void setPWM(int dutyCycle, int frequency = 1000) {
        if (_type == PeripheralType::GPIO_PWM_OUTPUT) {
            _pwmValue = constrain(dutyCycle, 0, 255);
            _pwmFrequency = frequency;
            _state = GPIOState::STATE_PWM;
        }
    }

    // 状态获取
    GPIOState getState() { return _state; }
    int getPin() { return _pin; }
    PeripheralType getType() { return _type; }
    int getPWMValue() { return _pwmValue; }
    int getAnalogValue() { return _analogValue; }

    // 模拟输入变化（测试用）
    void simulateDigitalInput(int value) {
        if (_type == PeripheralType::GPIO_DIGITAL_INPUT) {
            _digitalValue = value;
        }
    }

    void simulateAnalogInput(int value) {
        if (_type == PeripheralType::GPIO_ANALOG_INPUT) {
            _analogValue = constrain(value, 0, 1023);
        }
    }

private:
    int _pin;
    PeripheralType _type;
    GPIOState _state;
    int _digitalValue;
    int _pwmValue;
    int _analogValue;
    int _pwmFrequency;
};

// 模拟外设管理器
class MockPeripheralManager {
public:
    static MockPeripheralManager& getInstance() {
        static MockPeripheralManager instance;
        return instance;
    }

    bool initialize() {
        _peripherals.clear();
        _initialized = true;
        return true;
    }

    // 外设CRUD
    bool addPeripheral(const PeripheralConfig& config) {
        if (config.id.isEmpty()) return false;
        if (_peripherals.find(config.id) != _peripherals.end()) return false;
        
        _configs[config.id] = config;
        _peripherals[config.id] = MockPeripheral(config.pin, config.type);
        return true;
    }

    bool removePeripheral(const String& id) {
        auto it = _peripherals.find(id);
        if (it == _peripherals.end()) return false;
        
        _peripherals.erase(it);
        _configs.erase(id);
        return true;
    }

    bool updatePeripheral(const String& id, const PeripheralConfig& config) {
        if (_peripherals.find(id) == _peripherals.end()) return false;
        
        _configs[id] = config;
        _peripherals[id] = MockPeripheral(config.pin, config.type);
        return true;
    }

    PeripheralConfig* getPeripheral(const String& id) {
        auto it = _configs.find(id);
        if (it != _configs.end()) {
            return &(it->second);
        }
        return nullptr;
    }

    std::vector<String> getPeripheralIds() {
        std::vector<String> ids;
        for (auto& entry : _peripherals) {
            ids.push_back(entry.first);
        }
        return ids;
    }

    // GPIO控制
    bool writePin(const String& periphId, int value) {
        auto it = _peripherals.find(periphId);
        if (it == _peripherals.end()) return false;
        
        it->second.digitalWrite(value);
        return true;
    }

    int readPin(const String& periphId) {
        auto it = _peripherals.find(periphId);
        if (it == _peripherals.end()) return -1;
        
        return it->second.digitalRead();
    }

    bool writePWM(const String& periphId, int dutyCycle) {
        auto it = _peripherals.find(periphId);
        if (it == _peripherals.end()) return false;
        
        it->second.setPWM(dutyCycle);
        return true;
    }

    int readAnalog(const String& periphId) {
        auto it = _peripherals.find(periphId);
        if (it == _peripherals.end()) return -1;
        
        return it->second.analogRead();
    }

    GPIOState getPinState(const String& periphId) {
        auto it = _peripherals.find(periphId);
        if (it == _peripherals.end()) return GPIOState::STATE_UNKNOWN;
        
        return it->second.getState();
    }

    // 批量操作
    int getPeripheralCount() { return _peripherals.size(); }
    
    void clearAll() {
        _peripherals.clear();
        _configs.clear();
    }

    // 测试辅助方法
    void simulatePinChange(const String& periphId, int value) {
        auto it = _peripherals.find(periphId);
        if (it != _peripherals.end()) {
            it->second.simulateDigitalInput(value);
        }
    }

    MockPeripheral* getMockPeripheral(const String& id) {
        auto it = _peripherals.find(id);
        if (it != _peripherals.end()) {
            return &(it->second);
        }
        return nullptr;
    }

private:
    MockPeripheralManager() : _initialized(false) {}
    bool _initialized;
    std::map<String, MockPeripheral> _peripherals;
    std::map<String, PeripheralConfig> _configs;
};

// 模拟外设执行管理器
class MockPeriphExecManager {
public:
    static MockPeriphExecManager& getInstance() {
        static MockPeriphExecManager instance;
        return instance;
    }

    bool initialize() {
        _rules.clear();
        _executionCount = 0;
        return true;
    }

    // 规则CRUD
    bool addRule(const PeriphExecRule& rule) {
        if (rule.id.isEmpty()) return false;
        _rules[rule.id] = rule;
        return true;
    }

    bool removeRule(const String& ruleId) {
        auto it = _rules.find(ruleId);
        if (it == _rules.end()) return false;
        _rules.erase(it);
        return true;
    }

    bool updateRule(const String& ruleId, const PeriphExecRule& rule) {
        if (_rules.find(ruleId) == _rules.end()) return false;
        _rules[ruleId] = rule;
        return true;
    }

    PeriphExecRule* getRule(const String& ruleId) {
        auto it = _rules.find(ruleId);
        if (it != _rules.end()) {
            return &(it->second);
        }
        return nullptr;
    }

    std::vector<String> getRuleIds() {
        std::vector<String> ids;
        for (auto& entry : _rules) {
            ids.push_back(entry.first);
        }
        return ids;
    }

    // 规则执行
    bool executeRule(const String& ruleId) {
        auto it = _rules.find(ruleId);
        if (it == _rules.end()) return false;
        
        PeriphExecRule& rule = it->second;
        if (!rule.enabled) return false;
        
        // 执行动作
        MockPeripheralManager& pm = MockPeripheralManager::getInstance();
        
        switch (rule.actionType) {
            case ActionType::SET_HIGH:
                pm.writePin(rule.targetPeriphId, HIGH);
                break;
            case ActionType::SET_LOW:
                pm.writePin(rule.targetPeriphId, LOW);
                break;
            case ActionType::TOGGLE: {
                int current = pm.readPin(rule.targetPeriphId);
                pm.writePin(rule.targetPeriphId, current == HIGH ? LOW : HIGH);
                break;
            }
            case ActionType::SET_PWM: {
                int duty = rule.actionValue.toInt();
                pm.writePWM(rule.targetPeriphId, duty);
                break;
            }
            default:
                break;
        }
        
        _executionCount++;
        return true;
    }

    // MQTT消息处理
    bool handleMqttMessage(const String& topic, const String& payload) {
        bool executed = false;
        
        for (auto& entry : _rules) {
            PeriphExecRule& rule = entry.second;
            
            if (!rule.enabled) continue;
            if (rule.triggerType != TriggerType::PLATFORM_MQTT) continue;
            
            // 简单匹配：检查topic是否包含规则名称或条件
            if (topic.indexOf(rule.name) >= 0 || 
                evaluateCondition(payload, rule.triggerCondition)) {
                executeRule(rule.id);
                executed = true;
            }
        }
        
        return executed;
    }

    // 条件评估
    bool evaluateCondition(const String& value, const String& condition) {
        if (condition.isEmpty()) return true;
        
        // 简化实现：支持基本的比较操作
        // 格式: "operator,threshold" 例如: ">,20" 或 "==,ON"
        
        int commaIndex = condition.indexOf(',');
        if (commaIndex < 0) {
            // 简单相等比较
            return value == condition;
        }
        
        String op = condition.substring(0, commaIndex);
        String threshold = condition.substring(commaIndex + 1);
        
        // 数值比较
        float val = value.toFloat();
        float thresh = threshold.toFloat();
        
        if (op == ">") return val > thresh;
        if (op == ">=") return val >= thresh;
        if (op == "<") return val < thresh;
        if (op == "<=") return val <= thresh;
        if (op == "==") return val == thresh;
        if (op == "!=") return val != thresh;
        
        return false;
    }

    // 统计信息
    int getRuleCount() { return _rules.size(); }
    int getExecutionCount() { return _executionCount; }
    void resetExecutionCount() { _executionCount = 0; }

    // 清理
    void clearAll() {
        _rules.clear();
        _executionCount = 0;
    }

private:
    MockPeriphExecManager() : _executionCount(0) {}
    std::map<String, PeriphExecRule> _rules;
    int _executionCount;
};

// 脚本引擎模拟
class MockScriptEngine {
public:
    // 脚本解析
    static std::vector<String> parse(const String& script) {
        std::vector<String> commands;
        
        int start = 0;
        int end = script.indexOf('\n');
        
        while (end >= 0) {
            String cmd = script.substring(start, end);
            cmd.trim();
            if (cmd.length() > 0) {
                commands.push_back(cmd);
            }
            start = end + 1;
            end = script.indexOf('\n', start);
        }
        
        // 处理最后一行
        if (start < script.length()) {
            String cmd = script.substring(start);
            cmd.trim();
            if (cmd.length() > 0) {
                commands.push_back(cmd);
            }
        }
        
        return commands;
    }

    // 命令执行
    static bool executeCommand(const String& command, 
                               MockPeripheralManager& pm) {
        // 解析命令
        // 格式: "COMMAND param1 param2 ..."
        
        std::vector<String> parts = splitCommand(command);
        if (parts.empty()) return false;
        
        String cmd = parts[0];
        cmd.toUpperCase();
        
        if (cmd == "GPIO" && parts.size() >= 3) {
            // GPIO <pin> <HIGH/LOW>
            String periphId = parts[1];
            int value = (parts[2] == "HIGH" || parts[2] == "1") ? HIGH : LOW;
            return pm.writePin(periphId, value);
        }
        else if (cmd == "PWM" && parts.size() >= 3) {
            // PWM <pin> <duty>
            String periphId = parts[1];
            int duty = parts[2].toInt();
            return pm.writePWM(periphId, duty);
        }
        else if (cmd == "DELAY" && parts.size() >= 2) {
            // DELAY <ms>
            int ms = parts[1].toInt();
            delay(ms);
            return true;
        }
        else if (cmd == "LOG" && parts.size() >= 2) {
            // LOG <message>
            String msg = command.substring(4);
            Serial.println("[SCRIPT] " + msg);
            return true;
        }
        
        return false;
    }

    // 执行完整脚本
    static bool executeScript(const String& script, 
                              MockPeripheralManager& pm) {
        std::vector<String> commands = parse(script);
        
        for (auto& cmd : commands) {
            if (!executeCommand(cmd, pm)) {
                return false;
            }
        }
        
        return true;
    }

private:
    static std::vector<String> splitCommand(const String& command) {
        std::vector<String> parts;
        
        int start = 0;
        bool inQuote = false;
        
        for (int i = 0; i < command.length(); i++) {
            char c = command[i];
            
            if (c == '"') {
                inQuote = !inQuote;
            } else if (c == ' ' && !inQuote) {
                if (i > start) {
                    parts.push_back(command.substring(start, i));
                }
                start = i + 1;
            }
        }
        
        if (start < command.length()) {
            parts.push_back(command.substring(start));
        }
        
        return parts;
    }
};

// 全局实例引用
#define MockPeripheralMgr MockPeripheralManager::getInstance()
#define MockPeriphExecMgr MockPeriphExecManager::getInstance()

#endif // MOCK_PERIPHERAL_H
