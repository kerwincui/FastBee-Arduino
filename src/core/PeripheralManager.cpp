/**
 * @description: 外设接口管理器实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-03-03
 */

#include "core/PeripheralManager.h"
#include "systems/LoggerSystem.h"

// 单例实现
PeripheralManager& PeripheralManager::getInstance() {
    static PeripheralManager instance;
    return instance;
}

// 初始化
bool PeripheralManager::initialize() {
    LOG_INFO("Peripheral Manager: Initializing...");
    
    // 尝试从旧版配置迁移
    if (LittleFS.exists("/config/gpio.json") && !LittleFS.exists(PERIPHERAL_CONFIG_FILE)) {
        LOG_INFO("Peripheral Manager: Migrating from gpio.json");
        migrateFromGPIOConfig();
    }
    
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
    
    // 检查引脚冲突
    for (int i = 0; i < config.pinCount && i < 8; i++) {
        if (config.pins[i] != 255 && checkPinConflict(config.pins[i])) {
            LOG_ERRORF("Peripheral Manager: Pin %d is already in use", config.pins[i]);
            return false;
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
    auto config = getPeripheral(peripheralId);
    if (!config || !config->isGPIOPeripheral()) {
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
    
    // 处理反向逻辑
    if (config->params.gpio.inverted) {
        state = (state == GPIOState::STATE_HIGH) ? GPIOState::STATE_LOW : GPIOState::STATE_HIGH;
    }
    
    return state;
}

bool PeripheralManager::writePin(uint8_t pin, GPIOState state) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return false;
    return writePin(id, state);
}

bool PeripheralManager::writePin(const String& peripheralId, GPIOState state) {
    auto config = getPeripheral(peripheralId);
    if (!config || !config->isGPIOPeripheral()) {
        return false;
    }
    
    uint8_t pin = config->getPrimaryPin();
    if (pin == 255) return false;
    
    // 处理反向逻辑
    GPIOState physicalState = state;
    if (config->params.gpio.inverted) {
        physicalState = (physicalState == GPIOState::STATE_HIGH) ? GPIOState::STATE_LOW : GPIOState::STATE_HIGH;
    }
    
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
    auto config = getPeripheral(peripheralId);
    if (!config) return false;
    
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
    JsonDocument doc;
    JsonArray periphs = doc["peripherals"].to<JsonArray>();
    
    for (const auto& pair : peripherals) {
        const PeripheralConfig& config = pair.second;
        
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
            params["inverted"] = config.params.gpio.inverted;
            params["pwmChannel"] = config.params.gpio.pwmChannel;
            params["pwmFrequency"] = config.params.gpio.pwmFrequency;
            params["pwmResolution"] = config.params.gpio.pwmResolution;
            params["debounceMs"] = config.params.gpio.debounceMs;
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
    
    File file = LittleFS.open(PERIPHERAL_CONFIG_FILE, "r");
    if (!file) {
        LOG_ERROR("Peripheral Manager: Failed to open config file for reading");
        return false;
    }
    
    String jsonStr = file.readString();
    file.close();
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonStr);
    
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
                config.params.gpio.inverted = params["inverted"] | false;
                config.params.gpio.pwmChannel = params["pwmChannel"] | 0;
                config.params.gpio.pwmFrequency = params["pwmFrequency"] | 1000;
                config.params.gpio.pwmResolution = params["pwmResolution"] | 8;
                config.params.gpio.debounceMs = params["debounceMs"] | 50;
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

bool PeripheralManager::migrateFromGPIOConfig() {
    if (!LittleFS.exists("/config/gpio.json")) {
        return false;
    }
    
    LOG_INFO("Peripheral Manager: Migrating from gpio.json...");
    
    File file = LittleFS.open("/config/gpio.json", "r");
    if (!file) {
        LOG_ERROR("Peripheral Manager: Failed to open gpio.json");
        return false;
    }
    
    String jsonStr = file.readString();
    file.close();
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonStr);
    
    if (error) {
        LOG_ERRORF("Peripheral Manager: Failed to parse gpio.json - %s", error.c_str());
        return false;
    }
    
    JsonArray pins = doc["pins"].as<JsonArray>();
    if (pins.isNull()) {
        LOG_WARNING("Peripheral Manager: No pins found in gpio.json");
        return false;
    }
    
    int migratedCount = 0;
    for (JsonObject pinObj : pins) {
        PeripheralConfig config;
        
        uint8_t pin = pinObj["pin"] | 255;
        String name = pinObj["name"] | "";
        int mode = pinObj["mode"] | 0;
        
        if (pin == 255 || name.isEmpty()) continue;
        
        // 将旧模式转换为新类型
        switch (mode) {
            case 1: config.type = PeripheralType::GPIO_DIGITAL_INPUT; break;
            case 2: config.type = PeripheralType::GPIO_DIGITAL_OUTPUT; break;
            case 3: config.type = PeripheralType::GPIO_DIGITAL_INPUT_PULLUP; break;
            case 4: config.type = PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN; break;
            case 5: config.type = PeripheralType::GPIO_ANALOG_INPUT; break;
            case 6: config.type = PeripheralType::GPIO_ANALOG_OUTPUT; break;
            case 7: config.type = PeripheralType::GPIO_PWM_OUTPUT; break;
            default: config.type = PeripheralType::GPIO_DIGITAL_OUTPUT; break;
        }
        
        config.id = "gpio_" + String(pin);
        config.name = name;
        config.enabled = true;
        config.pinCount = 1;
        config.pins[0] = pin;
        
        config.params.gpio.initialState = static_cast<GPIOState>(pinObj["initialState"] | 0);
        config.params.gpio.inverted = pinObj["inverted"] | false;
        config.params.gpio.pwmChannel = pin % 16;
        config.params.gpio.pwmFrequency = pinObj["pwmFrequency"] | 1000;
        config.params.gpio.pwmResolution = pinObj["pwmResolution"] | 8;
        config.params.gpio.debounceMs = pinObj["debounceMs"] | 50;
        
        if (addPeripheral(config)) {
            migratedCount++;
        }
    }
    
    LOG_INFOF("Peripheral Manager: Migrated %d GPIO configurations", migratedCount);
    
    // 备份旧文件
    LittleFS.rename("/config/gpio.json", PERIPHERAL_CONFIG_BACKUP);
    LOG_INFO("Peripheral Manager: Backed up gpio.json to gpio.json.bak");
    
    // 保存新配置
    saveConfiguration();
    
    return true;
}

// ========== 内部辅助方法 ==========

bool PeripheralManager::validateConfig(const PeripheralConfig& config, String& errorMsg) {
    if (config.id.isEmpty()) {
        errorMsg = "ID cannot be empty";
        return false;
    }
    
    if (config.name.isEmpty()) {
        errorMsg = "Name cannot be empty";
        return false;
    }
    
    if (config.type == PeripheralType::UNCONFIGURED) {
        errorMsg = "Type cannot be unconfigured";
        return false;
    }
    
    if (config.pinCount == 0) {
        errorMsg = "At least one pin is required";
        return false;
    }
    
    for (int i = 0; i < config.pinCount && i < 8; i++) {
        if (config.pins[i] != 255 && !isValidPin(config.pins[i])) {
            errorMsg = "Invalid pin number: " + String(config.pins[i]);
            return false;
        }
    }
    
    return true;
}

bool PeripheralManager::setupHardware(const PeripheralConfig& config) {
    if (config.isGPIOPeripheral()) {
        return setupGPIOPin(config);
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
    if (channel > 15) {
        LOG_ERRORF("Peripheral Manager: Invalid PWM channel %d", channel);
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

bool PeripheralManager::isValidPin(uint8_t pin) const {
    // ESP32的有效GPIO引脚范围
    if (pin > 39) return false;
    
    // 内部Flash使用的引脚
    if (pin == 6 || pin == 7 || pin == 8 || pin == 9 || pin == 10 || pin == 11) {
        return false;
    }
    
    return true;
}

bool PeripheralManager::checkPinConflict(uint8_t pin, const String& excludeId) const {
    auto it = pinToPeripheral.find(pin);
    if (it != pinToPeripheral.end()) {
        return excludeId.isEmpty() || it->second != excludeId;
    }
    return false;
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

// 占位符实现（需要在后续完善）
bool PeripheralManager::writeData(const String& id, const uint8_t* data, size_t len) {
    // TODO: 实现通用数据写入
    (void)id; (void)data; (void)len;
    return false;
}

bool PeripheralManager::readData(const String& id, uint8_t* buffer, size_t& len) {
    // TODO: 实现通用数据读取
    (void)id; (void)buffer; (void)len;
    return false;
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
