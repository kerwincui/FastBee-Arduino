/**
 * @description: 外设接口管理器实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-03-03
 */

#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/FeatureFlags.h"
#include "core/ChipConfig.h"
#include "systems/LoggerSystem.h"
#include "peripherals/LCDManager.h"
#include <Wire.h>
#include <SPI.h>

// 单例实现
PeripheralManager& PeripheralManager::getInstance() {
    static PeripheralManager instance;
    return instance;
}

// 初始化
bool PeripheralManager::initialize() {
    LOG_INFO("Peripheral Manager: Initializing...");
    
    // 创建递归互斥量（支持同一任务嵌套加锁，如 togglePin → readPin → writePin）
    _mutex = xSemaphoreCreateRecursiveMutex();
    
    // 加载配置
    if (loadConfiguration()) {
        LOG_INFO("Peripheral Manager: Configuration loaded successfully");
        
        // 初始化所有启用的外设
        initAllEnabledPeripherals();
        
        LOG_INFO("Peripheral Manager: Initialization complete");
        return true;
    }
    
    LOG_WARNING("Peripheral Manager: Failed to load configuration, starting with empty config");
    return true;
}

// ========== 外设管理（增删改查） ==========

bool PeripheralManager::addPeripheral(const PeripheralConfig& config) {
    // 验证配置
    String errorMsg;
    if (!validateConfig(config, errorMsg)) {
        LOG_ERRORF("Peripheral Manager: Invalid config - %s", errorMsg.c_str());
        return false;
    }
    
    // 检查ID是否已存在
    if (hasPeripheral(config.id)) {
        LOG_ERRORF("Peripheral Manager: Peripheral with ID '%s' already exists", config.id.c_str());
        return false;
    }
    
    // 检查引脚冲突（Modbus 外设不使用 GPIO 引脚，跳过此检查）
    if (!config.isModbusPeripheral()) {
        for (int i = 0; i < config.pinCount && i < 8; i++) {
            if (config.pins[i] != 255 && checkPinConflict(config.pins[i])) {
                LOG_ERRORF("Peripheral Manager: Pin %d is already in use", config.pins[i]);
                return false;
            }
        }
    }
    
    // 添加外设
    peripherals[config.id] = config;
    
    // 初始化运行时状态
    PeripheralRuntimeState state;
    state.id = config.id;
    state.status = config.enabled ? PeripheralStatus::PERIPHERAL_ENABLED : PeripheralStatus::PERIPHERAL_DISABLED;
    runtimeStates[config.id] = state;
    
    // 更新引脚映射
    updatePinMapping(config.id, config);
    
    LOG_INFOF("Peripheral Manager: Added peripheral '%s' (%s)", 
              config.name.c_str(), config.id.c_str());
    
    // 如果启用，初始化硬件
    if (config.enabled) {
        initHardware(config.id);
    }
    
    return true;
}

bool PeripheralManager::updatePeripheral(const String& id, const PeripheralConfig& config) {
    if (!hasPeripheral(id)) {
        LOG_ERRORF("Peripheral Manager: Peripheral '%s' not found", id.c_str());
        return false;
    }
    
    // 如果ID改变，需要特殊处理
    if (id != config.id && !config.id.isEmpty()) {
        // 删除旧的
        removePeripheral(id);
        // 添加新的
        return addPeripheral(config);
    }
    
    // 先禁用并释放硬件
    bool wasEnabled = isPeripheralEnabled(id);
    if (wasEnabled) {
        deinitHardware(id);
    }
    
    // 移除旧引脚映射
    removePinMapping(id);
    
    // 更新配置
    peripherals[id] = config;
    
    // 更新引脚映射
    updatePinMapping(id, config);
    
    // 更新运行时状态
    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].status = config.enabled ? PeripheralStatus::PERIPHERAL_ENABLED : PeripheralStatus::PERIPHERAL_DISABLED;
    }
    
    LOG_INFOF("Peripheral Manager: Updated peripheral '%s'", id.c_str());
    
    // 如果之前是启用的，重新初始化
    if (wasEnabled && config.enabled) {
        initHardware(id);
    }
    
    return true;
}

bool PeripheralManager::removePeripheral(const String& id) {
    if (!hasPeripheral(id)) {
        return false;
    }
    
    // 先禁用并释放硬件
    deinitHardware(id);
    
    // 移除引脚映射
    removePinMapping(id);
    
    // 移除运行时状态
    runtimeStates.erase(id);
    
    // 移除配置
    peripherals.erase(id);
    
    LOG_INFOF("Peripheral Manager: Removed peripheral '%s'", id.c_str());
    return true;
}

PeripheralConfig* PeripheralManager::getPeripheral(const String& id) {
    auto it = peripherals.find(id);
    if (it != peripherals.end()) {
        return &(it->second);
    }
    return nullptr;
}

const PeripheralConfig* PeripheralManager::getPeripheral(const String& id) const {
    auto it = peripherals.find(id);
    if (it != peripherals.end()) {
        return &(it->second);
    }
    return nullptr;
}

std::vector<PeripheralConfig> PeripheralManager::getPeripheralsByType(PeripheralType type) const {
    std::vector<PeripheralConfig> result;
    for (const auto& pair : peripherals) {
        if (pair.second.type == type) {
            result.push_back(pair.second);
        }
    }
    return result;
}

std::vector<PeripheralConfig> PeripheralManager::getPeripheralsByCategory(PeripheralCategory category) const {
    std::vector<PeripheralConfig> result;
    for (const auto& pair : peripherals) {
        if (getPeripheralCategory(pair.second.type) == category) {
            result.push_back(pair.second);
        }
    }
    return result;
}

std::vector<PeripheralConfig> PeripheralManager::getAllPeripherals() const {
    std::vector<PeripheralConfig> result;
    for (const auto& pair : peripherals) {
        result.push_back(pair.second);
    }
    return result;
}

void PeripheralManager::forEachPeripheral(std::function<void(const PeripheralConfig&)> callback) const {
    for (const auto& pair : peripherals) {
        callback(pair.second);
    }
}

bool PeripheralManager::hasPeripheral(const String& id) const {
    return peripherals.find(id) != peripherals.end();
}

// ========== 外设操作 ==========

bool PeripheralManager::enablePeripheral(const String& id) {
    auto config = getPeripheral(id);
    if (!config) return false;
    
    config->enabled = true;
    
    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].status = PeripheralStatus::PERIPHERAL_ENABLED;
    }
    
    return initHardware(id);
}

bool PeripheralManager::disablePeripheral(const String& id) {
    auto config = getPeripheral(id);
    if (!config) return false;
    
    config->enabled = false;
    
    return deinitHardware(id);
}

bool PeripheralManager::isPeripheralEnabled(const String& id) const {
    auto config = getPeripheral(id);
    return config ? config->enabled : false;
}

PeripheralStatus PeripheralManager::getPeripheralStatus(const String& id) const {
    auto it = runtimeStates.find(id);
    if (it != runtimeStates.end()) {
        return it->second.status;
    }
    return PeripheralStatus::PERIPHERAL_DISABLED;
}

PeripheralRuntimeState* PeripheralManager::getRuntimeState(const String& id) {
    auto it = runtimeStates.find(id);
    if (it != runtimeStates.end()) {
        return &(it->second);
    }
    return nullptr;
}

// ========== 硬件初始化 ==========

bool PeripheralManager::initHardware(const String& id) {
    auto config = getPeripheral(id);
    if (!config) return false;
    
    if (!config->enabled) {
        LOG_WARNINGF("Peripheral Manager: Cannot init disabled peripheral '%s'", id.c_str());
        return false;
    }
    
    bool success = setupHardware(*config);
    
    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].status = success ? PeripheralStatus::PERIPHERAL_INITIALIZED : PeripheralStatus::PERIPHERAL_ERROR;
        if (success) {
            runtimeStates[id].initTime = millis();
        }
    }
    
    return success;
}

bool PeripheralManager::deinitHardware(const String& id) {
    auto config = getPeripheral(id);
    if (!config) return false;
    
    bool success = teardownHardware(*config);
    
    if (runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].status = PeripheralStatus::PERIPHERAL_DISABLED;
    }
    
    return success;
}

bool PeripheralManager::initAllEnabledPeripherals() {
    int successCount = 0;
    int failCount = 0;
    
    for (auto& pair : peripherals) {
        if (pair.second.enabled) {
            if (initHardware(pair.first)) {
                successCount++;
            } else {
                failCount++;
            }
        }
    }
    
    LOG_INFOF("Peripheral Manager: Initialized %d peripherals, %d failed", 
              successCount, failCount);
    return failCount == 0;
}

// ========== GPIO兼容层 ==========

bool PeripheralManager::configurePin(uint8_t pin, PeripheralType type) {
    if (!isValidPin(pin)) return false;
    
    // 生成唯一ID
    String id = generateUniqueId(type);
    
    PeripheralConfig config;
    config.id = id;
    config.name = "Pin" + String(pin);
    config.type = type;
    config.enabled = true;
    config.pinCount = 1;
    config.pins[0] = pin;
    
    // 根据类型设置默认参数
    if (type == PeripheralType::GPIO_PWM_OUTPUT) {
        config.params.gpio.pwmChannel = pin % 16;  // 使用引脚号模16作为通道
        config.params.gpio.pwmFrequency = 1000;
        config.params.gpio.pwmResolution = 8;
    }
    
    return addPeripheral(config);
}

bool PeripheralManager::configurePin(const PeripheralConfig& config) {
    return addPeripheral(config);
}

GPIOState PeripheralManager::readPin(uint8_t pin) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return GPIOState::STATE_UNDEFINED;
    return readPin(id);
}

GPIOState PeripheralManager::readPin(const String& peripheralId) {
    RecursiveMutexGuard lock(_mutex);
    auto config = getPeripheral(peripheralId);
    if (!config) return GPIOState::STATE_UNDEFINED;
    
    // Modbus 外设：返回缓存的状态（无法实时读取远端设备）
    if (config->isModbusPeripheral()) {
        if (runtimeStates.find(peripheralId) != runtimeStates.end()) {
            return runtimeStates[peripheralId].state.gpio.currentState;
        }
        return GPIOState::STATE_UNDEFINED;
    }
    
    if (!config->isGPIOPeripheral()) {
        return GPIOState::STATE_UNDEFINED;
    }
    
    uint8_t pin = config->getPrimaryPin();
    if (pin == 255) return GPIOState::STATE_UNDEFINED;
    
    GPIOState state;
    
    switch (config->type) {
        case PeripheralType::GPIO_DIGITAL_INPUT:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLUP:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN:
            state = digitalRead(pin) ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
            break;
            
        case PeripheralType::GPIO_ANALOG_INPUT:
            state = analogRead(pin) > 2048 ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
            break;
            
        default:
            // 对于输出类型，返回最后设置的状态
            if (runtimeStates.find(peripheralId) != runtimeStates.end()) {
                state = runtimeStates[peripheralId].state.gpio.currentState;
            } else {
                state = GPIOState::STATE_UNDEFINED;
            }
            break;
    }
    
    // 返回物理状态（电平反转已迁移至外设执行）
    return state;
}

bool PeripheralManager::writePin(uint8_t pin, GPIOState state) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return false;
    return writePin(id, state);
}

bool PeripheralManager::writePin(const String& peripheralId, GPIOState state) {
    RecursiveMutexGuard lock(_mutex);
    auto config = getPeripheral(peripheralId);
    if (!config) return false;
    
    // Modbus 外设：通过委托回调写入
    if (config->isModbusPeripheral()) {
        return writeModbusPin(peripheralId, *config, state);
    }
    
    if (!config->isGPIOPeripheral()) {
        return false;
    }
    
    uint8_t pin = config->getPrimaryPin();
    if (pin == 255) return false;
    
    // 直接写入物理状态（电平反转已迁移至外设执行层处理）
    GPIOState physicalState = state;
    
    bool success = true;
    
    switch (config->type) {
        case PeripheralType::GPIO_DIGITAL_OUTPUT:
            digitalWrite(pin, physicalState == GPIOState::STATE_HIGH ? HIGH : LOW);
            break;
            
        case PeripheralType::GPIO_PWM_OUTPUT:
            if (state == GPIOState::STATE_HIGH) {
                ledcWrite(config->params.gpio.pwmChannel, (1 << config->params.gpio.pwmResolution) - 1);
            } else {
                ledcWrite(config->params.gpio.pwmChannel, 0);
            }
            break;
            
        case PeripheralType::GPIO_ANALOG_OUTPUT:
            ledcWrite(config->params.gpio.pwmChannel,
                physicalState == GPIOState::STATE_HIGH
                    ? (uint32_t)((1U << config->params.gpio.pwmResolution) - 1) : 0U);
            break;
            
        default:
            success = false;
            break;
    }
    
    if (success && runtimeStates.find(peripheralId) != runtimeStates.end()) {
        runtimeStates[peripheralId].state.gpio.currentState = state;
        runtimeStates[peripheralId].lastActivity = millis();
    }
    
    return success;
}

bool PeripheralManager::togglePin(uint8_t pin) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return false;
    return togglePin(id);
}

bool PeripheralManager::togglePin(const String& peripheralId) {
    RecursiveMutexGuard lock(_mutex);
    GPIOState current = readPin(peripheralId);
    if (current == GPIOState::STATE_UNDEFINED) return false;
    return writePin(peripheralId, current == GPIOState::STATE_HIGH 
                                ? GPIOState::STATE_LOW : GPIOState::STATE_HIGH);
}

bool PeripheralManager::writePWM(uint8_t pin, uint32_t dutyCycle) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return false;
    return writePWM(id, dutyCycle);
}

bool PeripheralManager::writePWM(const String& peripheralId, uint32_t dutyCycle) {
    RecursiveMutexGuard lock(_mutex);
    auto config = getPeripheral(peripheralId);
    if (!config) return false;
    
    // Modbus PWM 外设：通过委托回调写入寄存器
    if (config->isModbusPeripheral()) {
        return writeModbusPWM(peripheralId, *config, dutyCycle);
    }
    
    if (config->type != PeripheralType::GPIO_PWM_OUTPUT && 
        config->type != PeripheralType::GPIO_ANALOG_OUTPUT &&
        config->type != PeripheralType::PWM_SERVO) {
        return false;
    }
    
    uint32_t maxVal = (1U << config->params.gpio.pwmResolution) - 1;
    ledcWrite(config->params.gpio.pwmChannel, dutyCycle > maxVal ? maxVal : dutyCycle);
    
    return true;
}

uint16_t PeripheralManager::readAnalog(uint8_t pin) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return 0;
    return readAnalog(id);
}

uint16_t PeripheralManager::readAnalog(const String& peripheralId) {
    auto config = getPeripheral(peripheralId);
    if (!config) return 0;
    
    if (config->type != PeripheralType::GPIO_ANALOG_INPUT &&
        config->type != PeripheralType::ADC) {
        return 0;
    }
    
    uint8_t pin = config->getPrimaryPin();
    if (pin == 255) return 0;
    
    return (uint16_t)analogRead(pin);
}

// ========== 持久化 ==========

bool PeripheralManager::saveConfiguration() {
    FastBeeJsonDocLarge doc;
    JsonArray periphs = doc["peripherals"].to<JsonArray>();
    
    for (const auto& pair : peripherals) {
        const PeripheralConfig& config = pair.second;
        
        // Modbus 外设由 protocol.json 管理，不保存到 peripherals.json
        if (config.isModbusPeripheral()) continue;
        
        JsonObject obj = periphs.add<JsonObject>();
        obj["id"] = config.id;
        obj["name"] = config.name;
        obj["type"] = static_cast<int>(config.type);
        obj["enabled"] = config.enabled;
        
        // 引脚配置
        JsonArray pins = obj["pins"].to<JsonArray>();
        for (int i = 0; i < config.pinCount && i < 8; i++) {
            if (config.pins[i] != 255) {
                pins.add(config.pins[i]);
            }
        }
        
        // 类型特定参数
        JsonObject params = obj["params"].to<JsonObject>();
        
        if (config.type == PeripheralType::UART) {
            params["baudRate"] = config.params.uart.baudRate;
            params["dataBits"] = config.params.uart.dataBits;
            params["stopBits"] = config.params.uart.stopBits;
            params["parity"] = config.params.uart.parity;
        }
        else if (config.type == PeripheralType::I2C) {
            params["frequency"] = config.params.i2c.frequency;
            params["address"] = config.params.i2c.address;
            params["isMaster"] = config.params.i2c.isMaster;
        }
        else if (config.type == PeripheralType::SPI) {
            params["frequency"] = config.params.spi.frequency;
            params["mode"] = config.params.spi.mode;
            params["msbFirst"] = config.params.spi.msbFirst;
        }
        else if (config.isGPIOPeripheral()) {
            params["initialState"] = static_cast<int>(config.params.gpio.initialState);
            params["pwmChannel"] = config.params.gpio.pwmChannel;
            params["pwmFrequency"] = config.params.gpio.pwmFrequency;
            params["pwmResolution"] = config.params.gpio.pwmResolution;
            params["defaultDuty"] = config.params.gpio.defaultDuty;
        }
        else if (config.type == PeripheralType::ADC) {
            params["attenuation"] = config.params.adc.attenuation;
            params["resolution"] = config.params.adc.resolution;
            params["sampleRate"] = config.params.adc.sampleRate;
        }
        else if (config.type == PeripheralType::DAC) {
            params["channel"] = config.params.dac.channel;
        }
    }
    
    String jsonStr;
    serializeJson(doc, jsonStr);
    
    File file = LittleFS.open(PERIPHERAL_CONFIG_FILE, "w");
    if (!file) {
        LOG_ERROR("Peripheral Manager: Failed to open config file for writing");
        return false;
    }
    
    size_t written = file.print(jsonStr);
    file.close();
    
    if (written == jsonStr.length()) {
        LOG_INFO("Peripheral Manager: Configuration saved successfully");
        return true;
    } else {
        LOG_ERROR("Peripheral Manager: Failed to write complete configuration");
        return false;
    }
}

bool PeripheralManager::loadConfiguration() {
    if (!LittleFS.exists(PERIPHERAL_CONFIG_FILE)) {
        LOG_INFO("Peripheral Manager: No configuration file found");
        return true;
    }
    
    // 检查文件大小，避免一次性读取过大文件
    File file = LittleFS.open(PERIPHERAL_CONFIG_FILE, "r");
    if (!file) {
        LOG_ERROR("Peripheral Manager: Failed to open config file for reading");
        return false;
    }
    
    size_t fileSize = file.size();
    LOG_DEBUGF("Peripheral Manager: Config file size: %d bytes", fileSize);
    
    // 检查可用内存
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < fileSize + 4096) {
        LOG_ERRORF("Peripheral Manager: Insufficient memory (free: %d, needed: %d)", 
                   freeHeap, fileSize + 4096);
        file.close();
        return false;
    }
    
    // 使用流式解析，避免一次性读取整个文件
    FastBeeJsonDocLarge doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        LOG_ERRORF("Peripheral Manager: Failed to parse config - %s", error.c_str());
        return false;
    }
    
    JsonArray periphs = doc["peripherals"].as<JsonArray>();
    if (periphs.isNull()) {
        LOG_WARNING("Peripheral Manager: No peripherals found in configuration");
        return true;
    }
    
    peripherals.clear();
    runtimeStates.clear();
    pinToPeripheral.clear();
    
    int loadedCount = 0;
    for (JsonObject obj : periphs) {
        PeripheralConfig config;
        
        config.id = obj["id"] | "";
        config.name = obj["name"] | "";
        config.type = static_cast<PeripheralType>(obj["type"] | 0);
        config.enabled = obj["enabled"] | false;
        
        // 引脚配置
        JsonArray pins = obj["pins"].as<JsonArray>();
        config.pinCount = 0;
        for (int i = 0; i < 8 && i < pins.size(); i++) {
            config.pins[i] = pins[i] | 255;
            if (config.pins[i] != 255) {
                config.pinCount++;
            }
        }
        
        // 类型特定参数
        JsonObject params = obj["params"].as<JsonObject>();
        if (!params.isNull()) {
            if (config.type == PeripheralType::UART) {
                config.params.uart.baudRate = params["baudRate"] | 115200;
                config.params.uart.dataBits = params["dataBits"] | 8;
                config.params.uart.stopBits = params["stopBits"] | 1;
                config.params.uart.parity = params["parity"] | 0;
            }
            else if (config.type == PeripheralType::I2C) {
                config.params.i2c.frequency = params["frequency"] | 100000;
                config.params.i2c.address = params["address"] | 0;
                config.params.i2c.isMaster = params["isMaster"] | true;
            }
            else if (config.type == PeripheralType::SPI) {
                config.params.spi.frequency = params["frequency"] | 1000000;
                config.params.spi.mode = params["mode"] | 0;
                config.params.spi.msbFirst = params["msbFirst"] | true;
            }
            else if (config.isGPIOPeripheral()) {
                config.params.gpio.initialState = static_cast<GPIOState>(params["initialState"] | 0);
                config.params.gpio.pwmChannel = params["pwmChannel"] | 0;
                config.params.gpio.pwmFrequency = params["pwmFrequency"] | 1000;
                config.params.gpio.pwmResolution = params["pwmResolution"] | 8;
                config.params.gpio.defaultDuty = params["defaultDuty"] | 0;
            }
            else if (config.type == PeripheralType::ADC) {
                config.params.adc.attenuation = params["attenuation"] | 0;
                config.params.adc.resolution = params["resolution"] | 12;
                config.params.adc.sampleRate = params["sampleRate"] | 0;
            }
            else if (config.type == PeripheralType::DAC) {
                config.params.dac.channel = params["channel"] | 1;
            }
        }
        
        if (!config.id.isEmpty() && config.type != PeripheralType::UNCONFIGURED) {
            peripherals[config.id] = config;
            
            PeripheralRuntimeState state;
            state.id = config.id;
            state.status = config.enabled ? PeripheralStatus::PERIPHERAL_ENABLED : PeripheralStatus::PERIPHERAL_DISABLED;
            runtimeStates[config.id] = state;
            
            updatePinMapping(config.id, config);
            loadedCount++;
        }
    }
    
    LOG_INFOF("Peripheral Manager: Loaded %d peripheral configurations", loadedCount);
    return true;
}

// ========== 内部辅助方法 ==========

bool PeripheralManager::validateConfig(const PeripheralConfig& config, String& errorMsg) {
    // 基本字段验证
    if (config.id.isEmpty()) {
        errorMsg = "ID 不能为空";
        return false;
    }
    
    if (config.name.isEmpty()) {
        errorMsg = "名称不能为空";
        return false;
    }
    
    if (config.type == PeripheralType::UNCONFIGURED) {
        errorMsg = "外设类型不能为空";
        return false;
    }
    
    // Modbus 外设不需要引脚配置
    if (config.isModbusPeripheral()) {
        if (config.params.modbus.slaveAddress < 1 || config.params.modbus.slaveAddress > 247) {
            errorMsg = "Modbus 从站地址无效 (有效范围: 1-247)";
            return false;
        }
        return true;
    }
    
    if (config.pinCount == 0) {
        errorMsg = "至少需要配置一个引脚";
        return false;
    }
    
    // 引脚验证
    for (int i = 0; i < config.pinCount && i < 8; i++) {
        if (config.pins[i] != 255) {
            // 使用增强的引脚验证
            if (!validatePinForType(config.pins[i], config.type, errorMsg)) {
                return false;
            }
        }
    }
    
    // 类型特定参数验证
    switch (config.type) {
        case PeripheralType::UART:
            // UART 波特率验证
            if (config.params.uart.baudRate == 0 || config.params.uart.baudRate > 5000000) {
                errorMsg = "UART 波特率无效 (有效范围: 1-5000000)";
                return false;
            }
            if (config.params.uart.dataBits < 5 || config.params.uart.dataBits > 8) {
                errorMsg = "UART 数据位无效 (有效值: 5-8)";
                return false;
            }
            if (config.params.uart.stopBits < 1 || config.params.uart.stopBits > 2) {
                errorMsg = "UART 停止位无效 (有效值: 1, 2)";
                return false;
            }
            if (config.params.uart.parity > 2) {
                errorMsg = "UART 校验位无效 (0=无, 1=奇, 2=偶)";
                return false;
            }
            break;
            
        case PeripheralType::I2C:
            // I2C 频率验证
            if (config.params.i2c.frequency != 100000 && 
                config.params.i2c.frequency != 400000 &&
                config.params.i2c.frequency != 1000000) {
                errorMsg = "I2C 频率无效 (支持: 100000, 400000, 1000000)";
                return false;
            }
            if (!config.params.i2c.isMaster && config.params.i2c.address == 0) {
                errorMsg = "I2C 从机模式需要设置地址";
                return false;
            }
            if (config.params.i2c.address > 127) {
                errorMsg = "I2C 地址无效 (有效范围: 0-127)";
                return false;
            }
            break;
            
        case PeripheralType::SPI:
            // SPI 频率验证
            if (config.params.spi.frequency == 0 || config.params.spi.frequency > 80000000) {
                errorMsg = "SPI 频率无效 (有效范围: 1-80000000)";
                return false;
            }
            if (config.params.spi.mode > 3) {
                errorMsg = "SPI 模式无效 (有效值: 0-3)";
                return false;
            }
            break;
            
        case PeripheralType::GPIO_PWM_OUTPUT:
        case PeripheralType::GPIO_ANALOG_OUTPUT:
        case PeripheralType::PWM_SERVO:
            // PWM 参数验证
            if (config.params.gpio.pwmChannel >= CHIP_MAX_PWM_CH) {
                errorMsg = "PWM 通道无效 (有效范围: 0-" + String(CHIP_MAX_PWM_CH - 1) + ")";
                return false;
            }
            if (config.params.gpio.pwmFrequency == 0 || config.params.gpio.pwmFrequency > 40000000) {
                errorMsg = "PWM 频率无效 (有效范围: 1-40000000)";
                return false;
            }
            if (config.params.gpio.pwmResolution < 1 || config.params.gpio.pwmResolution > 16) {
                errorMsg = "PWM 分辨率无效 (有效范围: 1-16 位)";
                return false;
            }
            // 检查频率和分辨率组合是否有效
            // ESP32: freq * (2^resolution) <= 80MHz
            {
                uint64_t maxFreqResProduct = 80000000ULL;
                uint64_t freqResProduct = (uint64_t)config.params.gpio.pwmFrequency * (1ULL << config.params.gpio.pwmResolution);
                if (freqResProduct > maxFreqResProduct) {
                    errorMsg = "PWM 频率和分辨率组合无效 (freq * 2^resolution 不能超过 80MHz)";
                    return false;
                }
            }
            break;
            
        case PeripheralType::ADC:
        case PeripheralType::GPIO_ANALOG_INPUT:
            // ADC 参数验证
            if (config.params.adc.attenuation > 3) {
                errorMsg = "ADC 衰减系数无效 (有效值: 0-3)";
                return false;
            }
            if (config.params.adc.resolution < 9 || config.params.adc.resolution > 12) {
                errorMsg = "ADC 分辨率无效 (有效范围: 9-12 位)";
                return false;
            }
            break;
            
        case PeripheralType::DAC:
            // DAC 参数验证
            if (config.params.dac.channel != 1 && config.params.dac.channel != 2) {
                errorMsg = "DAC 通道无效 (有效值: 1, 2)";
                return false;
            }
            break;
            
        default:
            // 其他类型暂不需要特殊验证
            break;
    }
    
    return true;
}

bool PeripheralManager::setupHardware(const PeripheralConfig& config) {
    if (config.isGPIOPeripheral()) {
        return setupGPIOPin(config);
    }
    
    // Modbus 外设不需要本地硬件初始化（通过 RS485 总线通信）
    if (config.isModbusPeripheral()) {
        LOG_INFOF("Peripheral Manager: Modbus device '%s' (slave=%d) registered, no local HW init needed",
                  config.name.c_str(), config.params.modbus.slaveAddress);
        return true;
    }
    
    // LCD/OLED 显示屏初始化
    if (config.type == PeripheralType::LCD) {
        return LCDManager::getInstance().initialize(config);
    }
    
    // TODO: 实现其他类型外设的硬件初始化
    LOG_INFOF("Peripheral Manager: Hardware setup for type %d not yet implemented", 
              static_cast<int>(config.type));
    return true;
}

bool PeripheralManager::teardownHardware(const PeripheralConfig& config) {
    if (config.isGPIOPeripheral()) {
        uint8_t pin = config.getPrimaryPin();
        if (pin != 255) {
            detachInterrupt(pin);
        }
    }
    
    // LCD/OLED 显示屏清理
    if (config.type == PeripheralType::LCD) {
        LCDManager::getInstance().deinitialize();
    }
    
    // TODO: 实现其他类型外设的硬件释放
    return true;
}

bool PeripheralManager::setupGPIOPin(const PeripheralConfig& config) {
    uint8_t pin = config.getPrimaryPin();
    if (pin == 255) return false;
    
    switch (config.type) {
        case PeripheralType::GPIO_DIGITAL_INPUT:
            pinMode(pin, INPUT);
            break;
            
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLUP:
            pinMode(pin, INPUT_PULLUP);
            break;
            
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN:
            pinMode(pin, INPUT_PULLDOWN);
            break;
            
        case PeripheralType::GPIO_DIGITAL_OUTPUT:
            pinMode(pin, OUTPUT);
            writePin(config.id, config.params.gpio.initialState);
            break;
            
        case PeripheralType::GPIO_ANALOG_INPUT:
            // ESP32模拟输入不需要特殊设置
            break;
            
        case PeripheralType::GPIO_PWM_OUTPUT:
        case PeripheralType::GPIO_ANALOG_OUTPUT:
        case PeripheralType::PWM_SERVO:
            return setupPWMPin(config);
            
        default:
            break;
    }
    
    return true;
}

bool PeripheralManager::setupPWMPin(const PeripheralConfig& config) {
    uint8_t pin = config.getPrimaryPin();
    if (pin == 255) return false;
    
    uint8_t channel = config.params.gpio.pwmChannel;
    if (channel >= CHIP_MAX_PWM_CH) {
        LOG_ERRORF("Peripheral Manager: Invalid PWM channel %d (max: %d)", channel, CHIP_MAX_PWM_CH - 1);
        return false;
    }
    
    ledcSetup(channel, config.params.gpio.pwmFrequency, config.params.gpio.pwmResolution);
    ledcAttachPin(pin, channel);
    
    // 设置初始值
    if (config.params.gpio.initialState == GPIOState::STATE_HIGH) {
        ledcWrite(channel, (1 << config.params.gpio.pwmResolution) - 1);
    } else {
        ledcWrite(channel, 0);
    }
    
    return true;
}

// ========== 动作定时器管理 ==========

void PeripheralManager::stopActionTicker(const String& id) {
    RecursiveMutexGuard lock(_mutex);
    auto it = actionTickers.find(id);
    if (it != actionTickers.end()) {
        it->second->ticker.detach();
        delete it->second;
        actionTickers.erase(it);
    }
}

// Ticker 回调：非阻塞尝试加锁，获取失败则跳过本次（下次重试）
static void blinkTickerCallback(PeripheralManager::ActionTickerData* data) {
    if (!data || !data->mgr) return;
    SemaphoreHandle_t mtx = data->mgr->getMutex();
    if (!mtx || xSemaphoreTakeRecursive(mtx, 0) != pdTRUE) return;
    data->mgr->togglePin(data->id);
    xSemaphoreGiveRecursive(mtx);
}

static void breatheTickerCallback(PeripheralManager::ActionTickerData* data) {
    if (!data || !data->mgr) return;
    SemaphoreHandle_t mtx = data->mgr->getMutex();
    if (!mtx || xSemaphoreTakeRecursive(mtx, 0) != pdTRUE) return;

    int16_t current = data->breatheState;
    bool increasing = (current >= 0);
    uint16_t duty = increasing ? current : (-current);
    
    if (increasing) {
        duty += data->stepSize;
        if (duty >= data->maxDuty) { duty = data->maxDuty; data->breatheState = -duty; }
        else { data->breatheState = duty; }
    } else {
        if (duty < data->stepSize) { duty = 0; data->breatheState = 0; }
        else { duty -= data->stepSize; data->breatheState = -duty; }
    }
    ledcWrite(data->channel, duty);
    xSemaphoreGiveRecursive(mtx);
}

void PeripheralManager::startActionTicker(const String& id, uint8_t actionMode, uint16_t paramValue) {
    RecursiveMutexGuard lock(_mutex);
    stopActionTicker(id);  // 先清理已有的
    
    auto config = getPeripheral(id);
    if (!config) return;

    if (actionMode == 1) {  // BLINK
        ActionTickerData* data = new ActionTickerData();
        data->mgr = this;
        data->id = id;
        actionTickers[id] = data;
        
        float intervalSec = paramValue / 1000.0f;
        data->ticker.attach(intervalSec, blinkTickerCallback, data);
        LOG_INFOF("Peripheral Manager: Blink ticker started for '%s' (interval=%dms)", 
                  id.c_str(), paramValue);
    }
    else if (actionMode == 2) {  // BREATHE
        ActionTickerData* data = new ActionTickerData();
        data->mgr = this;
        data->id = id;
        data->channel = config->params.gpio.pwmChannel;
        data->maxDuty = (1 << config->params.gpio.pwmResolution) - 1;
        data->breatheState = 0;
        
        uint16_t speedMs = paramValue;
        uint16_t steps = speedMs / 40;  // 半周期步数
        if (steps == 0) steps = 1;
        data->stepSize = data->maxDuty / steps;
        if (data->stepSize == 0) data->stepSize = 1;
        
        actionTickers[id] = data;
        data->ticker.attach_ms(20, breatheTickerCallback, data);
        LOG_INFOF("Peripheral Manager: Breathe ticker started for '%s' (speed=%dms)", 
                  id.c_str(), speedMs);
    }
}

// ========== DAC 硬件初始化 ==========

bool PeripheralManager::setupDACPin(const PeripheralConfig& config) {
#if CHIP_HAS_DAC
    uint8_t pin = config.getPrimaryPin();
    if (pin == 255) return false;
    if (pin != 25 && pin != 26) {
        LOG_ERROR("Peripheral Manager: DAC only supports GPIO 25 and 26");
        return false;
    }
    dacWrite(pin, config.params.dac.defaultValue);
    LOG_INFOF("Peripheral Manager: DAC pin %d set to %d", pin, config.params.dac.defaultValue);
    return true;
#else
    LOG_WARNING("DAC not supported on this chip");
    return false;
#endif
}

String PeripheralManager::generateUniqueId(PeripheralType type) {
    static int counter = 0;
    const char* typeName = getPeripheralTypeName(type);
    return String(typeName) + "_" + String(millis()) + "_" + String(counter++);
}

void PeripheralManager::updatePinMapping(const String& id, const PeripheralConfig& config) {
    for (int i = 0; i < config.pinCount && i < 8; i++) {
        if (config.pins[i] != 255) {
            pinToPeripheral[config.pins[i]] = id;
        }
    }
}

void PeripheralManager::removePinMapping(const String& id) {
    auto it = pinToPeripheral.begin();
    while (it != pinToPeripheral.end()) {
        if (it->second == id) {
            it = pinToPeripheral.erase(it);
        } else {
            ++it;
        }
    }
}

// ========== 引脚验证与冲突检测 ==========

// 获取引脚保留原因（使用 ChipConfig.h 中的定义）
static String getPinReservedReason(uint8_t pin) {
    // 检查是否为保留引脚
    for (uint8_t i = 0; i < CHIP_RESERVED_PIN_COUNT; i++) {
        if (CHIP_RESERVED_PINS[i] == pin) {
            switch (pin) {
                case 0: return "Boot 模式选择引脚，建议保留";
                case 1: return "TX0 调试串口";
                case 3: return "RX0 调试串口";
                case 6: case 7: case 8: case 9: case 10: case 11:
                    return "内部 Flash SPI，禁止使用";
                case 19: case 20:
                    return "USB 接口，建议保留";
                case 26: case 27: case 28: case 29: case 30: case 31: case 32:
                    return "Octal Flash/PSRAM，建议保留";
                default: return "系统保留引脚";
            }
        }
    }
    // 检查是否为输入专用引脚
    for (uint8_t i = 0; i < CHIP_INPUT_ONLY_PIN_COUNT; i++) {
        if (CHIP_INPUT_ONLY_PINS[i] == pin) {
            return "仅支持输入模式";
        }
    }
    return "";
}

bool PeripheralManager::isValidPin(uint8_t pin) const {
    // 使用 ChipConfig.h 中的最大 GPIO 编号
    if (pin > CHIP_MAX_GPIO) return false;
    
    // 内部Flash使用的引脚（绝对禁止）- 检查是否为保留引脚
    for (uint8_t i = 0; i < CHIP_RESERVED_PIN_COUNT; i++) {
        if (CHIP_RESERVED_PINS[i] == pin) {
            // GPIO 6-11 是内部 Flash SPI，绝对禁止
            #if defined(CONFIG_IDF_TARGET_ESP32)
            if (pin >= 6 && pin <= 11) return false;
            #elif defined(CONFIG_IDF_TARGET_ESP32S3)
            // ESP32-S3: GPIO 26-32 是 Octal Flash/PSRAM
            if (pin >= 26 && pin <= 32) return false;
            #elif defined(CONFIG_IDF_TARGET_ESP32C3)
            // ESP32-C3: GPIO 12-17 是 Flash SPI
            if (pin >= 12 && pin <= 17) return false;
            #else
            if (pin >= 6 && pin <= 11) return false;
            #endif
        }
    }
    
    return true;
}

// 检查引脚是否为系统保留引脚
bool PeripheralManager::isReservedPin(uint8_t pin) const {
    for (uint8_t i = 0; i < CHIP_RESERVED_PIN_COUNT; i++) {
        if (CHIP_RESERVED_PINS[i] == pin) {
            return true;
        }
    }
    return false;
}

// 检查引脚是否只能用于输入
bool PeripheralManager::isInputOnlyPin(uint8_t pin) const {
    for (uint8_t i = 0; i < CHIP_INPUT_ONLY_PIN_COUNT; i++) {
        if (CHIP_INPUT_ONLY_PINS[i] == pin) {
            return true;
        }
    }
    return false;
}

bool PeripheralManager::checkPinConflict(uint8_t pin, const String& excludeId) const {
    auto it = pinToPeripheral.find(pin);
    if (it != pinToPeripheral.end()) {
        return excludeId.isEmpty() || it->second != excludeId;
    }
    return false;
}

// 获取引脚冲突详细信息
String PeripheralManager::getPinConflictInfo(uint8_t pin, const String& excludeId) const {
    // 检查是否为无效引脚
    if (!isValidPin(pin)) {
        return String("GPIO") + String(pin) + " 不是有效的 GPIO 引脚";
    }
    
    // 检查系统保留引脚
    String reservedReason = getPinReservedReason(pin);
    if (!reservedReason.isEmpty() && (pin >= 6 && pin <= 11)) {
        return String("GPIO") + String(pin) + ": " + reservedReason;
    }
    
    // 检查是否与现有外设冲突
    auto it = pinToPeripheral.find(pin);
    if (it != pinToPeripheral.end() && (excludeId.isEmpty() || it->second != excludeId)) {
        auto config = getPeripheral(it->second);
        if (config) {
            return String("GPIO") + String(pin) + " 已被外设 \"" + config->name + "\" (" + it->second + ") 使用";
        }
        return String("GPIO") + String(pin) + " 已被外设 " + it->second + " 使用";
    }
    
    // 系统保留引脚警告（非错误）
    if (!reservedReason.isEmpty()) {
        return String("警告: GPIO") + String(pin) + " - " + reservedReason;
    }
    
    return "";  // 无冲突
}

// 验证引脚配置是否与外设类型兼容
bool PeripheralManager::validatePinForType(uint8_t pin, PeripheralType type, String& errorMsg) const {
    if (!isValidPin(pin)) {
        errorMsg = String("GPIO") + String(pin) + " 不是有效的引脚";
        return false;
    }
    
    // 检查输入专用引脚
    if (isInputOnlyPin(pin)) {
        // GPIO34-39 只能用于输入类型
        int typeVal = static_cast<int>(type);
        bool isInputType = (type == PeripheralType::GPIO_DIGITAL_INPUT ||
                           type == PeripheralType::GPIO_DIGITAL_INPUT_PULLUP ||
                           type == PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN ||
                           type == PeripheralType::GPIO_ANALOG_INPUT ||
                           type == PeripheralType::ADC ||
                           type == PeripheralType::GPIO_INTERRUPT_RISING ||
                           type == PeripheralType::GPIO_INTERRUPT_FALLING ||
                           type == PeripheralType::GPIO_INTERRUPT_CHANGE);
        
        if (!isInputType) {
            errorMsg = String("GPIO") + String(pin) + " 只能用于输入模式，不能配置为 " + getPeripheralTypeName(type);
            return false;
        }
    }
    
    // DAC 引脚验证
#if CHIP_HAS_DAC
    if (type == PeripheralType::DAC && pin != 25 && pin != 26) {
        errorMsg = "DAC 只能使用 GPIO25 或 GPIO26";
        return false;
    }
#else
    if (type == PeripheralType::DAC) {
        errorMsg = "此芯片不支持 DAC 功能";
        return false;
    }
#endif
    
    // 触摸引脚验证
#if CHIP_HAS_TOUCH
    if (type == PeripheralType::GPIO_TOUCH) {
        bool isTouchPin = false;
        for (uint8_t i = 0; i < CHIP_TOUCH_PIN_COUNT; i++) {
            if (pin == CHIP_TOUCH_PINS[i]) { isTouchPin = true; break; }
        }
        if (!isTouchPin) {
            errorMsg = String("GPIO") + String(pin) + " 不支持触摸功能";
            return false;
        }
    }
#else
    if (type == PeripheralType::GPIO_TOUCH) {
        errorMsg = "此芯片不支持触摸功能";
        return false;
    }
#endif
    
    return true;
}

String PeripheralManager::getPinPeripheralId(uint8_t pin) const {
    auto it = pinToPeripheral.find(pin);
    if (it != pinToPeripheral.end()) {
        return it->second;
    }
    return "";
}

bool PeripheralManager::isPinConfigured(uint8_t pin) const {
    return pinToPeripheral.find(pin) != pinToPeripheral.end();
}

std::vector<uint8_t> PeripheralManager::getConfiguredPins() const {
    std::vector<uint8_t> pins;
    for (const auto& pair : pinToPeripheral) {
        pins.push_back(pair.first);
    }
    return pins;
}

void PeripheralManager::printStatus() const {
    LOG_INFO("=== Peripheral Status ===");
    LOG_INFOF("Total peripherals: %d", peripherals.size());
    
    for (const auto& pair : peripherals) {
        const auto& config = pair.second;
        const auto& state = runtimeStates.find(config.id);
        
        char buf[128];
        snprintf(buf, sizeof(buf), "  [%s] %s (Type: %s, Enabled: %s, Status: %d)",
                 config.id.c_str(),
                 config.name.c_str(),
                 getPeripheralTypeName(config.type),
                 config.enabled ? "Yes" : "No",
                 state != runtimeStates.end() ? static_cast<int>(state->second.status) : -1);
        LOG_INFO(buf);
    }
}

void PeripheralManager::performMaintenance() {
    // 定期维护任务，如检查外设健康状态等
    // TODO: 实现定期维护逻辑
}

// 中断处理
void IRAM_ATTR PeripheralManager::isrHandler(void* arg) {
    // 中断上下文：仅记录触发的引脚号
    uint8_t pin = (uint8_t)(uintptr_t)arg;
    // TODO: 使用FreeRTOS队列将中断事件传递给主循环处理
    (void)pin;
}

void PeripheralManager::handleInterrupt(uint8_t pin) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return;
    
    auto config = getPeripheral(id);
    if (!config) return;
    
    if (config->params.gpio.interruptCallback) {
        GPIOState state = readPin(id);
        config->params.gpio.interruptCallback(pin, state);
    }
}

// ========== 通用数据读写接口 ==========

bool PeripheralManager::writeData(const String& id, const uint8_t* data, size_t len) {
    auto config = getPeripheral(id);
    if (!config || !config->enabled) {
        LOG_WARNINGF("writeData: Peripheral '%s' not found or disabled", id.c_str());
        return false;
    }
    
    bool success = false;
    
    switch (config->type) {
        // GPIO 数字输出
        case PeripheralType::GPIO_DIGITAL_OUTPUT:
            if (len >= 1) {
                GPIOState state = (data[0] != 0) ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
                success = writePin(id, state);
            }
            break;
            
        // PWM / 模拟输出
        case PeripheralType::GPIO_PWM_OUTPUT:
        case PeripheralType::GPIO_ANALOG_OUTPUT:
        case PeripheralType::PWM_SERVO:
            if (len >= 2) {
                uint32_t dutyCycle = data[0] | (data[1] << 8);
                if (len >= 4) {
                    dutyCycle |= (data[2] << 16) | (data[3] << 24);
                }
                success = writePWM(id, dutyCycle);
            } else if (len == 1) {
                // 单字节作为 0-255 占空比
                uint32_t maxVal = (1U << config->params.gpio.pwmResolution) - 1;
                uint32_t dutyCycle = (data[0] * maxVal) / 255;
                success = writePWM(id, dutyCycle);
            }
            break;
            
        // DAC 输出
        case PeripheralType::DAC:
#if CHIP_HAS_DAC
            if (len >= 1) {
                uint8_t pin = config->getPrimaryPin();
                if (pin == 25 || pin == 26) {
                    dacWrite(pin, data[0]);
                    success = true;
                }
            }
#else
            LOG_WARNING("DAC not supported on this chip");
#endif
            break;
            
        // UART 发送
        case PeripheralType::UART:
            // 需要根据配置的 UART 端口发送数据
            // 这里使用 Serial（UART0）作为示例
            if (config->pins[0] == 1 && config->pins[1] == 3) {
                Serial.write(data, len);
                success = true;
            }
            // TODO: 支持 Serial1/Serial2
            break;
            
        // I2C 写入
        case PeripheralType::I2C:
            if (config->params.i2c.isMaster && config->params.i2c.address > 0) {
                Wire.beginTransmission(config->params.i2c.address);
                Wire.write(data, len);
                success = (Wire.endTransmission() == 0);
            }
            break;
            
        // SPI 传输
        case PeripheralType::SPI:
            SPI.beginTransaction(SPISettings(config->params.spi.frequency, 
                config->params.spi.msbFirst ? MSBFIRST : LSBFIRST, 
                config->params.spi.mode));
            for (size_t i = 0; i < len; i++) {
                SPI.transfer(data[i]);
            }
            SPI.endTransaction();
            success = true;
            break;
            
        default:
            LOG_WARNINGF("writeData: Unsupported peripheral type %d", static_cast<int>(config->type));
            break;
    }
    
    // 更新运行时状态
    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
        runtimeStates[id].state.comm.bytesSent += len;
    }
    
    return success;
}

bool PeripheralManager::readData(const String& id, uint8_t* buffer, size_t& len) {
    auto config = getPeripheral(id);
    if (!config || !config->enabled) {
        LOG_WARNINGF("readData: Peripheral '%s' not found or disabled", id.c_str());
        len = 0;
        return false;
    }
    
    bool success = false;
    size_t maxLen = len;
    len = 0;
    
    switch (config->type) {
        // GPIO 数字输入
        case PeripheralType::GPIO_DIGITAL_INPUT:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLUP:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN:
        case PeripheralType::GPIO_TOUCH:
            if (maxLen >= 1) {
                GPIOState state = readPin(id);
                buffer[0] = (state == GPIOState::STATE_HIGH) ? 1 : 0;
                len = 1;
                success = true;
            }
            break;
            
        // ADC / 模拟输入
        case PeripheralType::GPIO_ANALOG_INPUT:
        case PeripheralType::ADC:
            if (maxLen >= 2) {
                uint16_t value = readAnalog(id);
                buffer[0] = value & 0xFF;
                buffer[1] = (value >> 8) & 0xFF;
                len = 2;
                success = true;
            }
            break;
            
        // UART 接收
        case PeripheralType::UART:
            if (config->pins[0] == 1 && config->pins[1] == 3) {
                len = 0;
                while (Serial.available() && len < maxLen) {
                    buffer[len++] = Serial.read();
                }
                success = true;
            }
            // TODO: 支持 Serial1/Serial2
            break;
            
        // I2C 读取
        case PeripheralType::I2C:
            if (config->params.i2c.isMaster && config->params.i2c.address > 0) {
                size_t requestLen = (maxLen < 32) ? maxLen : 32;  // I2C 一次最多读取 32 字节
                Wire.requestFrom(config->params.i2c.address, (uint8_t)requestLen);
                len = 0;
                while (Wire.available() && len < maxLen) {
                    buffer[len++] = Wire.read();
                }
                success = (len > 0);
            }
            break;
            
        // SPI 读取（需要先发送才能读取）
        case PeripheralType::SPI:
            SPI.beginTransaction(SPISettings(config->params.spi.frequency, 
                config->params.spi.msbFirst ? MSBFIRST : LSBFIRST, 
                config->params.spi.mode));
            for (size_t i = 0; i < maxLen; i++) {
                buffer[i] = SPI.transfer(0xFF);  // 发送 dummy 字节读取数据
            }
            SPI.endTransaction();
            len = maxLen;
            success = true;
            break;
            
        default:
            LOG_WARNINGF("readData: Unsupported peripheral type %d", static_cast<int>(config->type));
            break;
    }
    
    // 更新运行时状态
    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
        runtimeStates[id].state.comm.bytesReceived += len;
    }
    
    return success;
}

bool PeripheralManager::writeString(const String& id, const String& data) {
    return writeData(id, (const uint8_t*)data.c_str(), data.length());
}

String PeripheralManager::readString(const String& id) {
    uint8_t buffer[256];
    size_t len = sizeof(buffer);
    if (readData(id, buffer, len)) {
        return String((char*)buffer, len);
    }
    return "";
}

bool PeripheralManager::attachInterrupt(uint8_t pin, GPIOInterruptCallback callback) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return false;
    return attachInterrupt(id, callback);
}

bool PeripheralManager::attachInterrupt(const String& peripheralId, GPIOInterruptCallback callback) {
    auto config = getPeripheral(peripheralId);
    if (!config || !config->isGPIOPeripheral()) return false;
    
    uint8_t pin = config->getPrimaryPin();
    if (pin == 255) return false;
    
    config->params.gpio.interruptCallback = callback;
    
    uint8_t mode = CHANGE;
    if (config->type == PeripheralType::GPIO_INTERRUPT_RISING) mode = RISING;
    else if (config->type == PeripheralType::GPIO_INTERRUPT_FALLING) mode = FALLING;
    
    ::attachInterruptArg(digitalPinToInterrupt(pin), isrHandler, (void*)(uintptr_t)pin, mode);
    
    if (runtimeStates.find(peripheralId) != runtimeStates.end()) {
        runtimeStates[peripheralId].state.gpio.interruptAttached = true;
    }
    
    return true;
}

bool PeripheralManager::detachInterrupt(uint8_t pin) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return false;
    return detachInterrupt(id);
}

bool PeripheralManager::detachInterrupt(const String& peripheralId) {
    auto config = getPeripheral(peripheralId);
    if (!config || !config->isGPIOPeripheral()) return false;
    
    uint8_t pin = config->getPrimaryPin();
    if (pin == 255) return false;
    
    ::detachInterrupt(digitalPinToInterrupt(pin));
    config->params.gpio.interruptCallback = nullptr;
    
    if (runtimeStates.find(peripheralId) != runtimeStates.end()) {
        runtimeStates[peripheralId].state.gpio.interruptAttached = false;
    }
    
    return true;
}

// ========== Modbus 外设委托 ==========

void PeripheralManager::setModbusCallbacks(ModbusCoilWriteFunc coilWrite, ModbusRegWriteFunc regWrite) {
    RecursiveMutexGuard lock(_mutex);
    _modbusCoilWrite = coilWrite;
    _modbusRegWrite  = regWrite;
    LOG_INFO("Peripheral Manager: Modbus write callbacks registered");
}

void PeripheralManager::clearModbusCallbacks() {
    RecursiveMutexGuard lock(_mutex);
    _modbusCoilWrite = nullptr;
    _modbusRegWrite  = nullptr;
    LOG_INFO("Peripheral Manager: Modbus write callbacks cleared");
}

bool PeripheralManager::writeModbusPin(const String& id, const PeripheralConfig& config, GPIOState state) {
    if (!_modbusCoilWrite && !_modbusRegWrite) {
        LOG_WARNING("Peripheral Manager: Modbus write callbacks not set");
        return false;
    }
    
    bool value = (state == GPIOState::STATE_HIGH);
    if (config.params.modbus.ncMode) value = !value;
    
    uint16_t addr = config.params.modbus.coilBase;
    bool success = false;
    
    if (config.params.modbus.controlProtocol == 0) {
        // 线圈模式 (FC05)
        if (_modbusCoilWrite) {
            success = _modbusCoilWrite(config.params.modbus.slaveAddress, addr, value);
        }
    } else {
        // 寄存器模式 (FC06)
        uint16_t regVal = value ? 0xFF00 : 0x0000;
        if (_modbusRegWrite) {
            success = _modbusRegWrite(config.params.modbus.slaveAddress, addr, regVal);
        }
    }
    
    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].state.gpio.currentState = state;
        runtimeStates[id].lastActivity = millis();
    }
    
    LOG_INFOF("Peripheral Manager: Modbus writePin '%s' slave=%d addr=%d state=%s %s",
              id.c_str(), config.params.modbus.slaveAddress, addr,
              state == GPIOState::STATE_HIGH ? "HIGH" : "LOW",
              success ? "OK" : "FAIL");
    return success;
}

bool PeripheralManager::writeModbusPWM(const String& id, const PeripheralConfig& config, uint32_t dutyCycle) {
    if (!_modbusRegWrite) {
        LOG_WARNING("Peripheral Manager: Modbus register write callback not set");
        return false;
    }
    
    // PWM 类型设备：写入 pwmRegBase 寄存器
    if (config.params.modbus.deviceType != 1) {
        LOG_WARNINGF("Peripheral Manager: '%s' is not a PWM Modbus device", id.c_str());
        return false;
    }
    
    uint16_t regAddr = config.params.modbus.pwmRegBase;
    uint16_t regVal = (uint16_t)dutyCycle;
    bool success = _modbusRegWrite(config.params.modbus.slaveAddress, regAddr, regVal);
    
    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
    }
    
    LOG_INFOF("Peripheral Manager: Modbus writePWM '%s' slave=%d reg=%d duty=%d %s",
              id.c_str(), config.params.modbus.slaveAddress, regAddr, (int)dutyCycle,
              success ? "OK" : "FAIL");
    return success;
}

bool PeripheralManager::writeModbusCoil(const String& id, uint16_t coilAddr, bool value) {
    RecursiveMutexGuard lock(_mutex);
    if (!_modbusCoilWrite) {
        LOG_WARNING("Peripheral Manager: Modbus coil write callback not set");
        return false;
    }
    
    auto config = getPeripheral(id);
    if (!config || !config->isModbusPeripheral()) {
        LOG_WARNINGF("Peripheral Manager: '%s' is not a Modbus peripheral", id.c_str());
        return false;
    }
    
    bool success = _modbusCoilWrite(config->params.modbus.slaveAddress, coilAddr, value);
    
    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
    }
    
    LOG_INFOF("Peripheral Manager: Modbus writeCoil '%s' slave=%d coil=%d val=%d %s",
              id.c_str(), config->params.modbus.slaveAddress, coilAddr, value,
              success ? "OK" : "FAIL");
    return success;
}

bool PeripheralManager::writeModbusReg(const String& id, uint16_t regAddr, uint16_t value) {
    RecursiveMutexGuard lock(_mutex);
    if (!_modbusRegWrite) {
        LOG_WARNING("Peripheral Manager: Modbus register write callback not set");
        return false;
    }
    
    auto config = getPeripheral(id);
    if (!config || !config->isModbusPeripheral()) {
        LOG_WARNINGF("Peripheral Manager: '%s' is not a Modbus peripheral", id.c_str());
        return false;
    }
    
    bool success = _modbusRegWrite(config->params.modbus.slaveAddress, regAddr, value);
    
    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
    }
    
    LOG_INFOF("Peripheral Manager: Modbus writeReg '%s' slave=%d reg=%d val=%d %s",
              id.c_str(), config->params.modbus.slaveAddress, regAddr, value,
              success ? "OK" : "FAIL");
    return success;
}