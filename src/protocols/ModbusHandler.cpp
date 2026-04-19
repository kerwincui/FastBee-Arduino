/**
 * @description: Modbus RTU 处理器 - 支持从站(Slave)和主站(Master)双模式
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:31:03
 *
 * Slave模式: 被动响应，支持功能码 0x01-0x06, 0x0F, 0x10
 * Master模式: 阻塞式一次性读写操作，由 PeriphExec POLL_TRIGGER 调度执行
 * 支持异常响应: ILLEGAL_FUNCTION, ILLEGAL_DATA_ADDRESS, ILLEGAL_DATA_VALUE
 */

#include "protocols/ModbusHandler.h"
#include "core/ChipConfig.h"
#include <ArduinoJson.h>

namespace {
constexpr uint16_t MODBUS_MIN_RESPONSE_TIMEOUT_MS = 100;
constexpr uint16_t MODBUS_MAX_RESPONSE_TIMEOUT_MS = 5000;
constexpr uint8_t MODBUS_MAX_MASTER_RETRIES = 3;
constexpr uint16_t MODBUS_MIN_INTER_POLL_DELAY_MS = 20;
constexpr uint16_t MODBUS_MAX_INTER_POLL_DELAY_MS = 1000;
constexpr uint16_t MODBUS_MIN_POLL_INTERVAL_SEC = 2;
constexpr uint16_t MODBUS_SAFE_POLL_INTERVAL_SEC = 5;
constexpr uint16_t MODBUS_MAX_POLL_INTERVAL_SEC = 3600;
constexpr uint8_t MODBUS_HEAVY_TASK_THRESHOLD = 4;

template <typename T>
T clampValue(T value, T minVal, T maxVal) {
    if (value < minVal) return minVal;
    if (value > maxVal) return maxVal;
    return value;
}

bool isReadFunctionCode(uint8_t functionCode) {
    return functionCode == 0x01 || functionCode == 0x02 ||
           functionCode == 0x03 || functionCode == 0x04;
}
}

ModbusHandler::ModbusHandler() 
#if CHIP_UART_COUNT >= 3
    : isInitialized(false), modbusSerial(&Serial2),
#else
    : isInitialized(false), modbusSerial(&Serial1),
#endif
      isOneShotReading(false), _oneShotSemaphore(nullptr), _controlPending(false),
      _totalPollCount(0), _successPollCount(0), _failedPollCount(0), _timeoutPollCount(0), 
      _lastPollTime(0), _lastStatusHash(0),
      _consecutiveTimeouts(0), _cooldownUntil(0) {
#if FASTBEE_MODBUS_SLAVE_ENABLE
    memset(holdingRegisters, 0, sizeof(holdingRegisters));
    memset(inputRegisters, 0, sizeof(inputRegisters));
    memset(coils, 0, sizeof(coils));
    memset(discreteInputs, 0, sizeof(discreteInputs));
#endif
    
    // 创建二值信号量（用于 OneShot 操作同步）
    _oneShotSemaphore = xSemaphoreCreateBinary();
    if (_oneShotSemaphore) {
        // 初始状态：信号量可用（OneShot 未进行中）
        xSemaphoreGive(_oneShotSemaphore);
    }
}

ModbusHandler::~ModbusHandler() {
    end();
    // 释放信号量
    if (_oneShotSemaphore) {
        vSemaphoreDelete(_oneShotSemaphore);
        _oneShotSemaphore = nullptr;
    }
}

bool ModbusHandler::begin(const ModbusConfig& cfg) {
    this->config = cfg;
    sanitizeConfig(this->config);
    
    if (!initializePins()) {
        LOG_ERROR("Modbus: Pin initialization failed");
        return false;
    }
    
    if (!initializeSerial()) {
        LOG_ERROR("Modbus: Serial initialization failed");
        return false;
    }
    
    isInitialized = true;
    
    const char* modeStr = (config.mode == MODBUS_MASTER) ? "Master" : "Slave";
    LOG_INFOF("Modbus: Initialized in %s mode", modeStr);
    LOG_INFOF("  Address: %d, Baud: %lu", config.slaveAddress, (unsigned long)config.baudRate);
    LOG_INFOF("  TX Pin: %d, RX Pin: %d, DE Pin: %d", config.txPin, config.rxPin, config.dePin);
    
    if (config.mode == MODBUS_MASTER) {
        LOG_INFOF("  Poll tasks: %d, Timeout: %dms",
                  config.master.taskCount,
                  config.master.responseTimeout);
    }
    
    return true;
}

void ModbusHandler::sanitizeConfig(ModbusConfig& config) {
    bool masterChanged = false;
    bool heavyLoadAdjusted = false;

    uint16_t originalResponseTimeout = config.responseTimeout;
    uint16_t originalMasterTimeout = config.master.responseTimeout;
    uint8_t originalRetries = config.master.maxRetries;
    uint16_t originalInterPollDelay = config.master.interPollDelay;

    config.responseTimeout = clampValue<uint16_t>(config.responseTimeout,
                                                  MODBUS_MIN_RESPONSE_TIMEOUT_MS,
                                                  MODBUS_MAX_RESPONSE_TIMEOUT_MS);
    config.master.responseTimeout = clampValue<uint16_t>(config.master.responseTimeout,
                                                         MODBUS_MIN_RESPONSE_TIMEOUT_MS,
                                                         MODBUS_MAX_RESPONSE_TIMEOUT_MS);
    config.master.maxRetries = clampValue<uint8_t>(config.master.maxRetries, 0, MODBUS_MAX_MASTER_RETRIES);
    config.master.interPollDelay = clampValue<uint16_t>(config.master.interPollDelay,
                                                        MODBUS_MIN_INTER_POLL_DELAY_MS,
                                                        MODBUS_MAX_INTER_POLL_DELAY_MS);

    if (originalResponseTimeout != config.responseTimeout ||
        originalMasterTimeout != config.master.responseTimeout ||
        originalRetries != config.master.maxRetries ||
        originalInterPollDelay != config.master.interPollDelay) {
        masterChanged = true;
    }

    uint8_t enabledTaskCount = 0;
    uint8_t boundedTaskCount = clampValue<uint8_t>(config.master.taskCount, 0, Protocols::MODBUS_MAX_POLL_TASKS);
    if (boundedTaskCount != config.master.taskCount) {
        masterChanged = true;
        config.master.taskCount = boundedTaskCount;
    }

    for (uint8_t i = 0; i < config.master.taskCount; i++) {
        PollTask& task = config.master.tasks[i];
        bool taskChanged = false;

        uint8_t originalSlave = task.slaveAddress;
        uint8_t originalFunctionCode = task.functionCode;
        uint16_t originalQuantity = task.quantity;
        uint16_t originalPollInterval = task.pollInterval;

        task.slaveAddress = clampValue<uint8_t>(task.slaveAddress, 1, 247);
        if (!isReadFunctionCode(task.functionCode)) {
            task.functionCode = 0x03;
        }
        task.quantity = clampValue<uint16_t>(task.quantity, 1, Protocols::MODBUS_MAX_REGISTERS_PER_READ);
        task.pollInterval = clampValue<uint16_t>(task.pollInterval,
                                                 MODBUS_MIN_POLL_INTERVAL_SEC,
                                                 MODBUS_MAX_POLL_INTERVAL_SEC);

        if (task.enabled) {
            enabledTaskCount++;
        }

        if (originalSlave != task.slaveAddress ||
            originalFunctionCode != task.functionCode ||
            originalQuantity != task.quantity ||
            originalPollInterval != task.pollInterval) {
            taskChanged = true;
        }

        if (task.mappingCount > Protocols::MODBUS_MAX_MAPPINGS_PER_TASK) {
            task.mappingCount = Protocols::MODBUS_MAX_MAPPINGS_PER_TASK;
            taskChanged = true;
        }

        if (taskChanged) {
            LOG_WARNINGF("[Modbus] Sanitized poll task[%d] (slave=%u, fc=%u, qty=%u, interval=%us)",
                         i,
                         static_cast<unsigned int>(task.slaveAddress),
                         static_cast<unsigned int>(task.functionCode),
                         static_cast<unsigned int>(task.quantity),
                         static_cast<unsigned int>(task.pollInterval));
        }
    }

    if (enabledTaskCount >= MODBUS_HEAVY_TASK_THRESHOLD) {
        for (uint8_t i = 0; i < config.master.taskCount; i++) {
            PollTask& task = config.master.tasks[i];
            if (!task.enabled) continue;
            if (task.pollInterval < MODBUS_SAFE_POLL_INTERVAL_SEC) {
                task.pollInterval = MODBUS_SAFE_POLL_INTERVAL_SEC;
                heavyLoadAdjusted = true;
                LOG_WARNINGF("[Modbus] Raised poll task[%d] interval to %us due to heavy master load (%u enabled tasks)",
                             i,
                             static_cast<unsigned int>(task.pollInterval),
                             static_cast<unsigned int>(enabledTaskCount));
            }
        }
    }

    if (masterChanged) {
        LOG_WARNINGF("[Modbus] Sanitized master config (timeout=%u, retries=%u, interDelay=%u, tasks=%u)",
                     static_cast<unsigned int>(config.master.responseTimeout),
                     static_cast<unsigned int>(config.master.maxRetries),
                     static_cast<unsigned int>(config.master.interPollDelay),
                     static_cast<unsigned int>(config.master.taskCount));
    }

    if (heavyLoadAdjusted) {
        LOG_WARNINGF("[Modbus] Heavy master polling detected, intervals below %us were raised",
                     static_cast<unsigned int>(MODBUS_SAFE_POLL_INTERVAL_SEC));
    }
}

bool ModbusHandler::begin(const String& configPath) {
    if (!loadConfigFromFile(configPath)) {
        LOG_WARNING("Modbus: Failed to load config from file, using defaults");
    }
    
    return begin(config);
}

void ModbusHandler::end() {
    if (isInitialized) {
        modbusSerial->end();
        isInitialized = false;
        LOG_INFO("Modbus: Stopped");
    }
}

// ============ 配置加载/保存 ============

bool ModbusHandler::loadConfigFromFile(const String& configPath) {
    String actualPath = configPath.isEmpty() ? config.configFile : configPath;
    
    LOG_INFOF("Modbus: Loading config from %s", actualPath.c_str());
    
    if (!FileUtils::exists(actualPath)) {
        LOG_WARNING("Modbus: Config file not found");
        return false;
    }
    
    String jsonContent = FileUtils::readFile(actualPath);
    if (jsonContent.isEmpty()) {
        LOG_WARNING("Modbus: Config file is empty");
        return false;
    }
    
    DynamicJsonDocument doc(8192);
    DeserializationError error = deserializeJson(doc, jsonContent);
    if (error) {
        LOG_ERRORF("Modbus: JSON parse error: %s", error.c_str());
        return false;
    }
    
    if (doc.containsKey("slaveAddress"))
        config.slaveAddress = doc["slaveAddress"].as<uint8_t>();
    if (doc.containsKey("baudRate"))
        config.baudRate = doc["baudRate"].as<uint32_t>();
    if (doc.containsKey("txPin"))
        config.txPin = doc["txPin"].as<uint8_t>();
    if (doc.containsKey("rxPin"))
        config.rxPin = doc["rxPin"].as<uint8_t>();
    if (doc.containsKey("dePin"))
        config.dePin = doc["dePin"].as<uint8_t>();
    if (doc.containsKey("responseTimeout"))
        config.responseTimeout = doc["responseTimeout"].as<uint16_t>();
    if (doc.containsKey("interFrameDelay"))
        config.interFrameDelay = doc["interFrameDelay"].as<uint16_t>();
    if (doc.containsKey("configFile"))
        config.configFile = doc["configFile"].as<String>();
    
    // 解析工作模式
    if (doc.containsKey("mode")) {
        String modeStr = doc["mode"].as<String>();
        config.mode = (modeStr == "master") ? MODBUS_MASTER : MODBUS_SLAVE;
    }
    
    // 解析传输类型
    if (doc.containsKey("transferType")) {
        config.transferType = doc["transferType"].as<uint8_t>();
    }
    
    // 解析工作模式
    if (doc.containsKey("workMode")) {
        config.workMode = doc["workMode"].as<uint8_t>();
    }
    
    // 解析Master模式配置
    if (doc.containsKey("master")) {
        JsonObject masterObj = doc["master"];
        if (masterObj.containsKey("responseTimeout"))
            config.master.responseTimeout = masterObj["responseTimeout"].as<uint16_t>();
        if (masterObj.containsKey("maxRetries"))
            config.master.maxRetries = masterObj["maxRetries"].as<uint8_t>();
        if (masterObj.containsKey("interPollDelay"))
            config.master.interPollDelay = masterObj["interPollDelay"].as<uint16_t>();
        
        if (masterObj.containsKey("tasks")) {
            JsonArray tasksArr = masterObj["tasks"];
            config.master.taskCount = 0;
            for (JsonVariant taskVar : tasksArr) {
                if (config.master.taskCount >= Protocols::MODBUS_MAX_POLL_TASKS) break;
                JsonObject t = taskVar.as<JsonObject>();
                PollTask& task = config.master.tasks[config.master.taskCount];
                task.slaveAddress = t["slaveAddress"] | (uint8_t)1;
                task.functionCode = t["functionCode"] | (uint8_t)0x03;
                task.startAddress = t["startAddress"] | (uint16_t)0;
                task.quantity     = t["quantity"] | (uint16_t)10;
                task.pollInterval = t["pollInterval"] | (uint16_t)Protocols::MODBUS_DEFAULT_POLL_INTERVAL;
                task.enabled      = t["enabled"] | true;
                // 向后兼容：优先读取 name，回退到 label
                const char* taskName = t["name"] | (t["label"] | "");
                strlcpy(task.name, taskName, sizeof(task.name));
                
                // 解析寄存器映射
                task.mappingCount = 0;
                if (t.containsKey("mappings")) {
                    JsonArray mappingsArr = t["mappings"];
                    for (JsonVariant mv : mappingsArr) {
                        if (task.mappingCount >= Protocols::MODBUS_MAX_MAPPINGS_PER_TASK) break;
                        JsonObject m = mv.as<JsonObject>();
                        RegisterMapping& mapping = task.mappings[task.mappingCount];
                        mapping.regOffset = m["regOffset"] | (uint8_t)0;
                        mapping.dataType = m["dataType"] | (uint8_t)0;
                        mapping.scaleFactor = m["scaleFactor"] | 1.0f;
                        mapping.decimalPlaces = m["decimalPlaces"] | (uint8_t)1;
                        const char* sid = m["sensorId"] | "";
                        strncpy(mapping.sensorId, sid, sizeof(mapping.sensorId) - 1);
                        mapping.sensorId[sizeof(mapping.sensorId) - 1] = '\0';
                        const char* u = m["unit"] | "";
                        strncpy(mapping.unit, u, sizeof(mapping.unit) - 1);
                        mapping.unit[sizeof(mapping.unit) - 1] = '\0';
                        task.mappingCount++;
                    }
                }
                
                config.master.taskCount++;
            }
        }
        
        // 解析子设备
        if (masterObj.containsKey("devices")) {
            JsonArray devicesArr = masterObj["devices"];
            config.master.deviceCount = 0;
            for (JsonVariant dv : devicesArr) {
                if (config.master.deviceCount >= Protocols::MODBUS_MAX_SUB_DEVICES) break;
                JsonObject d = dv.as<JsonObject>();
                ModbusSubDevice& dev = config.master.devices[config.master.deviceCount];
                const char* dName = d["name"] | "Device";
                strncpy(dev.name, dName, sizeof(dev.name) - 1);
                dev.name[sizeof(dev.name) - 1] = '\0';
                const char* sid = d["sensorId"] | "";
                strncpy(dev.sensorId, sid, sizeof(dev.sensorId) - 1);
                dev.sensorId[sizeof(dev.sensorId) - 1] = '\0';
                const char* dType = d["deviceType"] | "relay";
                strncpy(dev.deviceType, dType, sizeof(dev.deviceType) - 1);
                dev.deviceType[sizeof(dev.deviceType) - 1] = '\0';
                dev.slaveAddress    = d["slaveAddress"] | (uint8_t)1;
                dev.channelCount    = d["channelCount"] | (uint8_t)2;
                dev.coilBase        = d["coilBase"] | (uint16_t)0;
                dev.ncMode          = d["ncMode"] | false;
                dev.controlProtocol = d["controlProtocol"] | (uint8_t)0;
                dev.batchRegister   = d["batchRegister"] | (uint16_t)0;
                dev.pwmRegBase      = d["pwmRegBase"] | (uint16_t)0;
                dev.pwmResolution   = d["pwmResolution"] | (uint8_t)8;
                dev.pidDecimals     = d["pidDecimals"] | (uint8_t)1;
                dev.enabled         = d["enabled"] | true;
                if (d.containsKey("pidAddrs") && d["pidAddrs"].is<JsonArray>()) {
                    JsonArray pa = d["pidAddrs"].as<JsonArray>();
                    for (int i = 0; i < 6 && i < (int)pa.size(); i++)
                        dev.pidAddrs[i] = pa[i] | (uint16_t)0;
                }
                config.master.deviceCount++;
            }
        }
    }

    sanitizeConfig(config);
    
    LOG_INFO("Modbus: Config loaded successfully");
    return true;
}

bool ModbusHandler::saveConfigToFile(const String& configPath) {
    String actualPath = configPath.isEmpty() ? config.configFile : configPath;
    sanitizeConfig(config);
    
    DynamicJsonDocument doc(8192);
    doc["slaveAddress"] = config.slaveAddress;
    doc["baudRate"] = config.baudRate;
    doc["txPin"] = config.txPin;
    doc["rxPin"] = config.rxPin;
    doc["dePin"] = config.dePin;
    doc["responseTimeout"] = config.responseTimeout;
    doc["interFrameDelay"] = config.interFrameDelay;
    doc["configFile"] = config.configFile;
    doc["mode"] = (config.mode == MODBUS_MASTER) ? "master" : "slave";
    doc["transferType"] = config.transferType;
    doc["workMode"] = config.workMode;
    
    // 序列化Master配置
    JsonObject masterObj = doc.createNestedObject("master");
    masterObj["responseTimeout"] = config.master.responseTimeout;
    masterObj["maxRetries"] = config.master.maxRetries;
    masterObj["interPollDelay"] = config.master.interPollDelay;
    
    JsonArray tasksArr = masterObj.createNestedArray("tasks");
    for (uint8_t i = 0; i < config.master.taskCount; i++) {
        const PollTask& task = config.master.tasks[i];
        JsonObject t = tasksArr.createNestedObject();
        t["slaveAddress"] = task.slaveAddress;
        t["functionCode"] = task.functionCode;
        t["startAddress"] = task.startAddress;
        t["quantity"] = task.quantity;
        t["pollInterval"] = task.pollInterval;
        t["enabled"] = task.enabled;
        t["name"] = task.name;
        
        // 序列化寄存器映射
        if (task.mappingCount > 0) {
            JsonArray mappingsArr = t.createNestedArray("mappings");
            for (uint8_t j = 0; j < task.mappingCount; j++) {
                const RegisterMapping& mapping = task.mappings[j];
                JsonObject mo = mappingsArr.createNestedObject();
                mo["regOffset"] = mapping.regOffset;
                mo["dataType"] = mapping.dataType;
                mo["scaleFactor"] = mapping.scaleFactor;
                mo["decimalPlaces"] = mapping.decimalPlaces;
                mo["sensorId"] = mapping.sensorId;
                if (mapping.unit[0] != '\0') mo["unit"] = mapping.unit;
            }
        }
    }
    
    // 序列化子设备
    JsonArray devicesArr = masterObj.createNestedArray("devices");
    for (uint8_t i = 0; i < config.master.deviceCount; i++) {
        const ModbusSubDevice& dev = config.master.devices[i];
        JsonObject d = devicesArr.createNestedObject();
        d["name"]            = dev.name;
        d["sensorId"]        = dev.sensorId;
        d["deviceType"]      = dev.deviceType;
        d["slaveAddress"]    = dev.slaveAddress;
        d["channelCount"]    = dev.channelCount;
        d["coilBase"]        = dev.coilBase;
        d["ncMode"]          = dev.ncMode;
        d["controlProtocol"] = dev.controlProtocol;
        d["batchRegister"]   = dev.batchRegister;
        d["pwmRegBase"]      = dev.pwmRegBase;
        d["pwmResolution"]   = dev.pwmResolution;
        d["pidDecimals"]     = dev.pidDecimals;
        d["enabled"]         = dev.enabled;
        JsonArray pa = d.createNestedArray("pidAddrs");
        for (int j = 0; j < 6; j++) pa.add(dev.pidAddrs[j]);
    }
    
    String jsonContent;
    serializeJsonPretty(doc, jsonContent);
    
    bool success = FileUtils::writeFile(actualPath, jsonContent);
    if (success) {
        LOG_INFOF("Modbus: Config saved to %s", actualPath.c_str());
    } else {
        LOG_ERROR("Modbus: Failed to save config");
    }
    
    return success;
}

// ============ 硬件初始化 ============

bool ModbusHandler::initializeSerial() {
    if (modbusSerial) {
        modbusSerial->end();
    }
    
    modbusSerial->begin(config.baudRate, SERIAL_8N1, config.rxPin, config.txPin);
    modbusSerial->setTimeout(config.responseTimeout);
    
    LOG_INFOF("Modbus: Serial initialized on pins RX:%d, TX:%d", config.rxPin, config.txPin);
    return true;
}

bool ModbusHandler::initializePins() {
    if (config.dePin != 255) {
        pinMode(config.dePin, OUTPUT);
        digitalWrite(config.dePin, LOW);  // 默认接收模式
        LOG_INFOF("Modbus: DE pin initialized on pin %d", config.dePin);
    }
    return true;
}

void ModbusHandler::setTransmitMode(bool transmit) {
    if (config.dePin != 255) {
        digitalWrite(config.dePin, transmit ? HIGH : LOW);
        if (config.interFrameDelay > 0) {
            delayMicroseconds(config.interFrameDelay);
        }
    }
}

// ============ 主循环分发 ============

void ModbusHandler::handle() {
    if (!isInitialized) return;
    
    if (config.mode == MODBUS_MASTER) {
        processCoilDelayTasks();
    }
#if FASTBEE_MODBUS_SLAVE_ENABLE
    else {
        handleSlave();
    }
#endif
}

// ============ Slave模式主循环 ============

#if FASTBEE_MODBUS_SLAVE_ENABLE

void ModbusHandler::handleSlave() {
    while (modbusSerial->available()) {
        static uint8_t buffer[256];
        static uint8_t index = 0;
        static unsigned long slaveLastByteTime = 0;
        
        uint8_t byte = modbusSerial->read();
        unsigned long currentTime = millis();
        
        // 帧超时检测（3.5字符时间）
        if (index > 0 && (currentTime - slaveLastByteTime) > (35000000 / config.baudRate)) {
            index = 0;
        }
        
        slaveLastByteTime = currentTime;
        
        if (index < sizeof(buffer)) {
            buffer[index++] = byte;
            
            if (index >= 6) { // 最小Modbus帧长度
                if (validateFrame(buffer, index)) {
                    processModbusFrame(buffer, index);
                    index = 0;
                } else if (index >= sizeof(buffer)) {
                    index = 0;
                }
            }
        } else {
            index = 0;
        }
    }
}

#endif // FASTBEE_MODBUS_SLAVE_ENABLE (handleSlave)

String ModbusHandler::getStatus() const {
    if (!isInitialized) {
        return "Not initialized";
    }
    
    String mode = (config.mode == MODBUS_MASTER) ? "Master" : "Slave";
    String status = mode + " - Addr:" + String(config.slaveAddress) +
                   ", Baud:" + String(config.baudRate) +
                   ", Pins:TX" + String(config.txPin) + "/RX" + String(config.rxPin);
    
    if (config.dePin != 255) {
        status += ", DE:" + String(config.dePin);
    }
    
    if (config.mode == MODBUS_MASTER) {
        status += ", Tasks:" + String(config.master.taskCount) +
                  ", Devices:" + String(config.master.deviceCount);
    }
    
    return status;
}

#if FASTBEE_MODBUS_SLAVE_ENABLE

// ============ 数据操作 ============

bool ModbusHandler::writeData(uint16_t address, const String& data) {
    uint16_t length = min(data.length(), static_cast<size_t>(100));
    
    for (uint16_t i = 0; i < length; i++) {
        holdingRegisters[address + i] = data[i];
    }
    
    LOG_INFOF("Modbus: Written %d chars to address %d", length, address);
    return true;
}

bool ModbusHandler::writeData(const String& address, const String& data) {
    uint16_t addr = address.toInt();
    return writeData(addr, data);
}

// ============ Modbus 寄存器读写函数 ============

bool ModbusHandler::readCoils(uint16_t startAddr, uint16_t quantity, uint8_t* data) {
    if (startAddr + quantity > sizeof(coils) * 8) {
        return false;
    }
    
    memset(data, 0, (quantity + 7) / 8);
    
    for (uint16_t i = 0; i < quantity; i++) {
        uint8_t byteIndex = (startAddr + i) / 8;
        uint8_t bitIndex = (startAddr + i) % 8;
        
        if (coils[byteIndex] & (1 << bitIndex)) {
            data[i / 8] |= (1 << (i % 8));
        }
    }
    
    return true;
}

bool ModbusHandler::readDiscreteInputs(uint16_t startAddr, uint16_t quantity, uint8_t* data) {
    if (startAddr + quantity > sizeof(discreteInputs) * 8) {
        return false;
    }
    
    memset(data, 0, (quantity + 7) / 8);
    
    for (uint16_t i = 0; i < quantity; i++) {
        uint8_t byteIndex = (startAddr + i) / 8;
        uint8_t bitIndex = (startAddr + i) % 8;
        
        if (discreteInputs[byteIndex] & (1 << bitIndex)) {
            data[i / 8] |= (1 << (i % 8));
        }
    }
    
    return true;
}

bool ModbusHandler::readHoldingRegisters(uint16_t startAddr, uint16_t quantity, uint16_t* data) {
    if (startAddr + quantity > sizeof(holdingRegisters) / sizeof(holdingRegisters[0])) {
        return false;
    }
    
    for (uint16_t i = 0; i < quantity; i++) {
        if (registerReadCallback) {
            data[i] = registerReadCallback(startAddr + i);
        } else {
            data[i] = holdingRegisters[startAddr + i];
        }
    }
    
    return true;
}

bool ModbusHandler::readInputRegisters(uint16_t startAddr, uint16_t quantity, uint16_t* data) {
    if (startAddr + quantity > sizeof(inputRegisters) / sizeof(inputRegisters[0])) {
        return false;
    }
    
    for (uint16_t i = 0; i < quantity; i++) {
        data[i] = inputRegisters[startAddr + i];
    }
    
    return true;
}

bool ModbusHandler::writeSingleCoil(uint16_t addr, bool value) {
    if (addr >= sizeof(coils) * 8) {
        return false;
    }
    
    uint8_t byteIndex = addr / 8;
    uint8_t bitIndex = addr % 8;
    
    if (value) {
        coils[byteIndex] |= (1 << bitIndex);
    } else {
        coils[byteIndex] &= ~(1 << bitIndex);
    }
    
    return true;
}

bool ModbusHandler::writeSingleRegister(uint16_t addr, uint16_t value) {
    if (addr >= sizeof(holdingRegisters) / sizeof(holdingRegisters[0])) {
        return false;
    }
    
    if (registerWriteCallback) {
        registerWriteCallback(addr, value);
    } else {
        holdingRegisters[addr] = value;
    }
    
    return true;
}

bool ModbusHandler::writeMultipleCoils(uint16_t startAddr, uint16_t quantity, uint8_t* data) {
    if (startAddr + quantity > sizeof(coils) * 8) {
        return false;
    }
    
    for (uint16_t i = 0; i < quantity; i++) {
        uint8_t byteIndex = (startAddr + i) / 8;
        uint8_t bitIndex = (startAddr + i) % 8;
        
        if (data[i / 8] & (1 << (i % 8))) {
            coils[byteIndex] |= (1 << bitIndex);
        } else {
            coils[byteIndex] &= ~(1 << bitIndex);
        }
    }
    
    return true;
}

bool ModbusHandler::writeMultipleRegisters(uint16_t startAddr, uint16_t quantity, uint16_t* data) {
    if (startAddr + quantity > sizeof(holdingRegisters) / sizeof(holdingRegisters[0])) {
        return false;
    }
    
    for (uint16_t i = 0; i < quantity; i++) {
        if (registerWriteCallback) {
            registerWriteCallback(startAddr + i, data[i]);
        } else {
            holdingRegisters[startAddr + i] = data[i];
        }
    }
    
    return true;
}

// ============ CRC 与帧验证 ============

#endif // FASTBEE_MODBUS_SLAVE_ENABLE (slave register ops)

uint16_t ModbusHandler::calculateCRC(const uint8_t* data, uint8_t length) {
    uint16_t crc = 0xFFFF;
    for (uint8_t pos = 0; pos < length; pos++) {
        crc ^= (uint16_t)data[pos];
        for (uint8_t i = 8; i != 0; i--) {
            if ((crc & 0x0001) != 0) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

bool ModbusHandler::validateFrame(const uint8_t* frame, uint8_t length) {
    if (length < 4) return false;
    
    uint16_t calculatedCRC = calculateCRC(frame, length - 2);
    uint16_t receivedCRC = (frame[length - 1] << 8) | frame[length - 2];
    
    return calculatedCRC == receivedCRC;
}

#if FASTBEE_MODBUS_SLAVE_ENABLE

// ============ 帧处理与响应 ============

void ModbusHandler::sendResponse(const uint8_t* data, uint8_t length) {
    setTransmitMode(true);
    modbusSerial->write(data, length);
    modbusSerial->flush();
    setTransmitMode(false);
}

void ModbusHandler::sendExceptionResponse(uint8_t slaveAddr, uint8_t functionCode, uint8_t exceptionCode) {
    uint8_t response[5];
    response[0] = slaveAddr;
    response[1] = functionCode | 0x80;
    response[2] = exceptionCode;
    uint16_t crc = calculateCRC(response, 3);
    response[3] = crc & 0xFF;
    response[4] = (crc >> 8) & 0xFF;
    
    sendResponse(response, 5);
    LOG_WARNINGF("Modbus: Exception response FC=0x%02X, Ex=0x%02X", functionCode, exceptionCode);
}

void ModbusHandler::processModbusFrame(const uint8_t* frame, uint8_t length) {
    uint8_t slaveAddress = frame[0];
    uint8_t functionCode = frame[1];
    
    if (slaveAddress != config.slaveAddress && slaveAddress != 0) {
        return;
    }
    
    bool isBroadcast = (slaveAddress == 0);
    
    LOG_DEBUGF("Modbus: Received frame - Slave: %d, FC: 0x%02X, Len: %d",
               slaveAddress, functionCode, length);
    
    switch (functionCode) {
        case 0x01: if (!isBroadcast) handleReadCoils(frame, length); break;
        case 0x02: if (!isBroadcast) handleReadDiscreteInputs(frame, length); break;
        case 0x03: if (!isBroadcast) handleReadHoldingRegisters(frame, length); break;
        case 0x04: if (!isBroadcast) handleReadInputRegisters(frame, length); break;
        case 0x05: handleWriteSingleCoil(frame, length); break;
        case 0x06: handleWriteSingleRegister(frame, length); break;
        case 0x0F: handleWriteMultipleCoils(frame, length); break;
        case 0x10: handleWriteMultipleRegisters(frame, length); break;
        default:
            if (!isBroadcast) {
                sendExceptionResponse(config.slaveAddress, functionCode, MODBUS_EX_ILLEGAL_FUNCTION);
            }
            LOG_WARNINGF("Modbus: Unsupported function code 0x%02X", functionCode);
            break;
    }
    
    if (dataCallback) {
        String dataStr = "FC:0x" + String(functionCode, HEX);
        dataCallback(slaveAddress, dataStr);
    }
}

// ============ FC handler 通用辅助 ============

bool ModbusHandler::parseSlaveRequest(const uint8_t* frame, uint8_t length, uint8_t fc,
                                      uint16_t maxAddr, uint16_t maxQty,
                                      uint16_t& startAddr, uint16_t& quantity) {
    if (length < 8) {
        sendExceptionResponse(config.slaveAddress, fc, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return false;
    }
    startAddr = (frame[2] << 8) | frame[3];
    quantity  = (frame[4] << 8) | frame[5];
    if (quantity == 0 || quantity > maxQty) {
        sendExceptionResponse(config.slaveAddress, fc, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return false;
    }
    if (startAddr + quantity > maxAddr) {
        sendExceptionResponse(config.slaveAddress, fc, MODBUS_EX_ILLEGAL_DATA_ADDRESS);
        return false;
    }
    return true;
}

void ModbusHandler::sendSlaveReadResponse(uint8_t fc, const uint8_t* data, uint8_t byteCount) {
    uint8_t response[Protocols::MODBUS_BUFFER_SIZE];
    response[0] = config.slaveAddress;
    response[1] = fc;
    response[2] = byteCount;
    memcpy(&response[3], data, byteCount);
    uint8_t respLen = 3 + byteCount;
    uint16_t crc = calculateCRC(response, respLen);
    response[respLen]     = crc & 0xFF;
    response[respLen + 1] = (crc >> 8) & 0xFF;
    sendResponse(response, respLen + 2);
}

void ModbusHandler::sendSlaveWriteAck(uint8_t fc, uint16_t startAddr, uint16_t quantity) {
    uint8_t response[8];
    response[0] = config.slaveAddress;
    response[1] = fc;
    response[2] = (startAddr >> 8) & 0xFF;
    response[3] = startAddr & 0xFF;
    response[4] = (quantity >> 8) & 0xFF;
    response[5] = quantity & 0xFF;
    uint16_t crc = calculateCRC(response, 6);
    response[6] = crc & 0xFF;
    response[7] = (crc >> 8) & 0xFF;
    sendResponse(response, 8);
}

// ============ FC 0x01: 读线圈 ============

void ModbusHandler::handleReadCoils(const uint8_t* frame, uint8_t length) {
    uint16_t startAddr, quantity;
    if (!parseSlaveRequest(frame, length, 0x01, sizeof(coils) * 8, 2000, startAddr, quantity))
        return;
    uint8_t byteCount = (quantity + 7) / 8;
    uint8_t data[byteCount];
    if (!readCoils(startAddr, quantity, data)) {
        sendExceptionResponse(config.slaveAddress, 0x01, MODBUS_EX_SLAVE_DEVICE_FAILURE);
        return;
    }
    sendSlaveReadResponse(0x01, data, byteCount);
}

// ============ FC 0x02: 读离散输入 ============

void ModbusHandler::handleReadDiscreteInputs(const uint8_t* frame, uint8_t length) {
    uint16_t startAddr, quantity;
    if (!parseSlaveRequest(frame, length, 0x02, sizeof(discreteInputs) * 8, 2000, startAddr, quantity))
        return;
    uint8_t byteCount = (quantity + 7) / 8;
    uint8_t data[byteCount];
    if (!readDiscreteInputs(startAddr, quantity, data)) {
        sendExceptionResponse(config.slaveAddress, 0x02, MODBUS_EX_SLAVE_DEVICE_FAILURE);
        return;
    }
    sendSlaveReadResponse(0x02, data, byteCount);
}

// ============ FC 0x03: 读保持寄存器 ============

void ModbusHandler::handleReadHoldingRegisters(const uint8_t* frame, uint8_t length) {
    uint16_t startAddr, quantity;
    if (!parseSlaveRequest(frame, length, 0x03,
            sizeof(holdingRegisters) / sizeof(holdingRegisters[0]), 125, startAddr, quantity))
        return;
    uint16_t regData[125];
    if (!readHoldingRegisters(startAddr, quantity, regData)) {
        sendExceptionResponse(config.slaveAddress, 0x03, MODBUS_EX_SLAVE_DEVICE_FAILURE);
        return;
    }
    uint8_t byteCount = quantity * 2;
    uint8_t data[byteCount];
    for (uint16_t i = 0; i < quantity; i++) {
        data[i * 2]     = (regData[i] >> 8) & 0xFF;
        data[i * 2 + 1] = regData[i] & 0xFF;
    }
    sendSlaveReadResponse(0x03, data, byteCount);
}

// ============ FC 0x04: 读输入寄存器 ============

void ModbusHandler::handleReadInputRegisters(const uint8_t* frame, uint8_t length) {
    uint16_t startAddr, quantity;
    if (!parseSlaveRequest(frame, length, 0x04,
            sizeof(inputRegisters) / sizeof(inputRegisters[0]), 125, startAddr, quantity))
        return;
    uint16_t regData[125];
    if (!readInputRegisters(startAddr, quantity, regData)) {
        sendExceptionResponse(config.slaveAddress, 0x04, MODBUS_EX_SLAVE_DEVICE_FAILURE);
        return;
    }
    uint8_t byteCount = quantity * 2;
    uint8_t data[byteCount];
    for (uint16_t i = 0; i < quantity; i++) {
        data[i * 2]     = (regData[i] >> 8) & 0xFF;
        data[i * 2 + 1] = regData[i] & 0xFF;
    }
    sendSlaveReadResponse(0x04, data, byteCount);
}

// ============ FC 0x05: 写单个线圈 ============

void ModbusHandler::handleWriteSingleCoil(const uint8_t* frame, uint8_t length) {
    if (length < 8) {
        sendExceptionResponse(config.slaveAddress, 0x05, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    uint16_t addr  = (frame[2] << 8) | frame[3];
    uint16_t value = (frame[4] << 8) | frame[5];
    if (value != 0xFF00 && value != 0x0000) {
        sendExceptionResponse(config.slaveAddress, 0x05, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    if (addr >= sizeof(coils) * 8) {
        sendExceptionResponse(config.slaveAddress, 0x05, MODBUS_EX_ILLEGAL_DATA_ADDRESS);
        return;
    }
    if (!writeSingleCoil(addr, value == 0xFF00)) {
        sendExceptionResponse(config.slaveAddress, 0x05, MODBUS_EX_SLAVE_DEVICE_FAILURE);
        return;
    }
    if (frame[0] == 0) return;
    sendResponse(frame, length);
}

// ============ FC 0x06: 写单个寄存器 ============

void ModbusHandler::handleWriteSingleRegister(const uint8_t* frame, uint8_t length) {
    if (length < 8) {
        sendExceptionResponse(config.slaveAddress, 0x06, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    uint16_t addr  = (frame[2] << 8) | frame[3];
    uint16_t value = (frame[4] << 8) | frame[5];
    if (addr >= sizeof(holdingRegisters) / sizeof(holdingRegisters[0])) {
        sendExceptionResponse(config.slaveAddress, 0x06, MODBUS_EX_ILLEGAL_DATA_ADDRESS);
        return;
    }
    if (!writeSingleRegister(addr, value)) {
        sendExceptionResponse(config.slaveAddress, 0x06, MODBUS_EX_SLAVE_DEVICE_FAILURE);
        return;
    }
    if (frame[0] == 0) return;
    sendResponse(frame, length);
}

// ============ FC 0x0F: 写多个线圈 ============

void ModbusHandler::handleWriteMultipleCoils(const uint8_t* frame, uint8_t length) {
    if (length < 10) {
        sendExceptionResponse(config.slaveAddress, 0x0F, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    uint16_t startAddr = (frame[2] << 8) | frame[3];
    uint16_t quantity  = (frame[4] << 8) | frame[5];
    uint8_t byteCount  = frame[6];
    if (quantity == 0 || quantity > 1968 ||
        byteCount != (quantity + 7) / 8 ||
        length < (uint8_t)(7 + byteCount + 2)) {
        sendExceptionResponse(config.slaveAddress, 0x0F, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    if (startAddr + quantity > sizeof(coils) * 8) {
        sendExceptionResponse(config.slaveAddress, 0x0F, MODBUS_EX_ILLEGAL_DATA_ADDRESS);
        return;
    }
    if (!writeMultipleCoils(startAddr, quantity, (uint8_t*)&frame[7])) {
        sendExceptionResponse(config.slaveAddress, 0x0F, MODBUS_EX_SLAVE_DEVICE_FAILURE);
        return;
    }
    if (frame[0] == 0) return;
    sendSlaveWriteAck(0x0F, startAddr, quantity);
}

// ============ FC 0x10: 写多个寄存器 ============

void ModbusHandler::handleWriteMultipleRegisters(const uint8_t* frame, uint8_t length) {
    if (length < 11) {
        sendExceptionResponse(config.slaveAddress, 0x10, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    uint16_t startAddr = (frame[2] << 8) | frame[3];
    uint16_t quantity  = (frame[4] << 8) | frame[5];
    uint8_t byteCount  = frame[6];
    if (quantity == 0 || quantity > 123 ||
        byteCount != quantity * 2 ||
        length < (uint8_t)(7 + byteCount + 2)) {
        sendExceptionResponse(config.slaveAddress, 0x10, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    if (startAddr + quantity > sizeof(holdingRegisters) / sizeof(holdingRegisters[0])) {
        sendExceptionResponse(config.slaveAddress, 0x10, MODBUS_EX_ILLEGAL_DATA_ADDRESS);
        return;
    }
    uint16_t regData[123];
    for (uint16_t i = 0; i < quantity; i++) {
        regData[i] = (frame[7 + i * 2] << 8) | frame[7 + i * 2 + 1];
    }
    if (!writeMultipleRegisters(startAddr, quantity, regData)) {
        sendExceptionResponse(config.slaveAddress, 0x10, MODBUS_EX_SLAVE_DEVICE_FAILURE);
        return;
    }
    if (frame[0] == 0) return;
    sendSlaveWriteAck(0x10, startAddr, quantity);
}

#endif // FASTBEE_MODBUS_SLAVE_ENABLE

// ============ 回调设置 ============

void ModbusHandler::setDataCallback(std::function<void(uint8_t, const String&)> callback) {
    this->dataCallback = callback;
}

void ModbusHandler::setRegisterReadCallback(std::function<uint16_t(uint16_t)> callback) {
    this->registerReadCallback = callback;
}

void ModbusHandler::setRegisterWriteCallback(std::function<void(uint16_t, uint16_t)> callback) {
    this->registerWriteCallback = callback;
}

void ModbusHandler::setStatusChangeCallback(StatusChangeCallback cb) {
    _statusChangeCallback = cb;
}

void ModbusHandler::_notifyStatusChange() {
    if (!_statusChangeCallback || !isInitialized) return;

    // 计算状态 hash：基于统计计数判断是否有显著变化
    // 使用简单的 XOR hash，避免完整 JSON 序列化开销
    uint32_t newHash = _totalPollCount ^ _successPollCount ^ 
                       (_failedPollCount << 8) ^ (_timeoutPollCount << 16) ^
                       (isInitialized ? 0x80000000 : 0);

    // 只有 hash 变化时才推送（避免每次 poll 都推送）
    if (newHash == _lastStatusHash) return;
    _lastStatusHash = newHash;

    // 构建轻量级 JSON 状态字符串
    // 参考 ModbusRouteHandler::handleGetModbusStatus 的 JSON 结构
    JsonDocument doc;
    JsonObject data = doc.to<JsonObject>();

    data["mode"] = (config.mode == MODBUS_MASTER) ? "master" : "slave";
    data["status"] = getStatus();
    data["totalPolls"] = _totalPollCount;
    data["successPolls"] = _successPollCount;
    data["failedPolls"] = _failedPollCount;
    data["timeoutPolls"] = _timeoutPollCount;
    data["lastPollAgeSec"] = getLastPollAgeSec();
    data["taskCount"] = config.master.taskCount;

    String json;
    serializeJson(doc, json);

    _statusChangeCallback(json);
}

// ============================================================================
// Master模式实现
// ============================================================================

void ModbusHandler::setMode(ModbusMode newMode) {
    if (config.mode == newMode) return;
    
    bool wasInitialized = isInitialized;
    if (wasInitialized) {
        end();
    }
    
    config.mode = newMode;
    
    if (wasInitialized) {
        begin(config);
    }
    
    LOG_INFOF("Modbus: Mode changed to %s", (newMode == MODBUS_MASTER) ? "Master" : "Slave");
}

// ============ 轮询任务只读访问 ============

PollTask ModbusHandler::getPollTask(uint8_t index) const {
    if (index >= config.master.taskCount) return PollTask();
    return config.master.tasks[index];
}

const ModbusSubDevice& ModbusHandler::getSubDevice(uint8_t index) const {
    static const ModbusSubDevice emptyDevice;
    if (index >= config.master.deviceCount) return emptyDevice;
    return config.master.devices[index];
}

String ModbusHandler::buildSensorId(uint8_t deviceIndex, uint16_t channel) const {
    if (deviceIndex >= config.master.deviceCount) return String();
    const ModbusSubDevice& dev = config.master.devices[deviceIndex];
    if (dev.sensorId[0] == '\0') return String();
    if (dev.channelCount <= 1) return String(dev.sensorId);
    // 多通道格式: sensorId_chN (N 为通道号)
    return String(dev.sensorId) + "_ch" + String(channel);
}

bool ModbusHandler::findBySensorId(const String& sensorId, uint8_t& outDeviceIndex, uint16_t& outChannel) const {
    if (sensorId.isEmpty()) return false;
    for (uint8_t i = 0; i < config.master.deviceCount; i++) {
        const ModbusSubDevice& dev = config.master.devices[i];
        if (dev.sensorId[0] == '\0') continue;
        String baseSid(dev.sensorId);
        if (dev.channelCount <= 1) {
            if (sensorId == baseSid) {
                outDeviceIndex = i;
                outChannel = 0;
                return true;
            }
        } else {
            // 精确匹配基础 sensorId → channel 0
            if (sensorId == baseSid) {
                outDeviceIndex = i;
                outChannel = 0;
                return true;
            }
            // 匹配 sensorId_chN 模式 (N 为通道号)
            String prefix = baseSid + "_ch";
            if (sensorId.startsWith(prefix)) {
                String numPart = sensorId.substring(prefix.length());
                if (numPart.length() > 0 && numPart.length() <= 3) {
                    bool allDigits = true;
                    for (unsigned int k = 0; k < numPart.length(); k++) {
                        if (numPart[k] < '0' || numPart[k] > '9') { allDigits = false; break; }
                    }
                    if (allDigits) {
                        int ch = numPart.toInt();
                        if (ch >= 0 && ch < dev.channelCount) {
                            outDeviceIndex = i;
                            outChannel = (uint16_t)ch;
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

// Master写操作（阻塞式，通过 writeRegisterOnce 直接执行）
bool ModbusHandler::masterWriteSingleRegister(uint8_t slaveAddr, uint16_t regAddr, uint16_t value) {
    LOG_INFOF("Modbus Master: Write slave=%d, reg=%d, val=%d", slaveAddr, regAddr, value);
    OneShotResult result = writeRegisterOnce(slaveAddr, regAddr, value);
    return (result.error == ONESHOT_SUCCESS);
}

// ========== 一次性寄存器读取（阻塞式，用于MQTT指令触发） ==========

OneShotResult ModbusHandler::readRegistersOnce(uint8_t slaveAddress, uint8_t functionCode,
                                                uint16_t startAddress, uint16_t quantity, bool isControl) {
    OneShotResult result;

    // 参数校验（读操作特有）
    if (functionCode < 0x01 || functionCode > 0x04) {
        result.error = ONESHOT_EXCEPTION;
        result.exceptionCode = MODBUS_EX_ILLEGAL_FUNCTION;
        LOG_WARNINGF("[Modbus] OneShot read: invalid function code 0x%02X", functionCode);
        return result;
    }
    if (quantity == 0 || quantity > Protocols::MODBUS_ONESHOT_BUFFER_SIZE) {
        result.error = ONESHOT_EXCEPTION;
        result.exceptionCode = MODBUS_EX_ILLEGAL_DATA_VALUE;
        LOG_WARNINGF("[Modbus] OneShot read: invalid quantity %d", quantity);
        return result;
    }

    LOG_INFOF("[Modbus] OneShot read: slave=%d fc=0x%02X addr=%d qty=%d isControl=%d",
              slaveAddress, functionCode, startAddress, quantity, isControl ? 1 : 0);

    // 构建读请求帧（FC 0x01-0x04 均为 8 字节固定格式）
    uint8_t requestBuffer[8];
    requestBuffer[0] = slaveAddress;
    requestBuffer[1] = functionCode;
    requestBuffer[2] = (startAddress >> 8) & 0xFF;
    requestBuffer[3] = startAddress & 0xFF;
    requestBuffer[4] = (quantity >> 8) & 0xFF;
    requestBuffer[5] = quantity & 0xFF;
    uint16_t crc = calculateCRC(requestBuffer, 6);
    requestBuffer[6] = crc & 0xFF;
    requestBuffer[7] = (crc >> 8) & 0xFF;

    // 使用通用发送/接收（控制操作使用快速通道）
    OneShotResult rawResult = isControl ? sendControlRequest(requestBuffer, 8, slaveAddress)
                                        : sendOneShotRequest(requestBuffer, 8, slaveAddress);

    if (rawResult.error != ONESHOT_SUCCESS) {
        return rawResult;
    }

    // 从原始响应中提取数据
    // rawResult.data[] 中存储了原始响应字节（每个uint16存一个字节）
    // rawResult.count 是原始响应字节数
    uint8_t respLen = rawResult.count;

    // 功能码匹配
    uint8_t respFC = (uint8_t)rawResult.data[1];
    if (respFC != functionCode) {
        LOG_WARNINGF("[Modbus] OneShot read: FC mismatch (expected 0x%02X, got 0x%02X)", functionCode, respFC);
        result.error = ONESHOT_CRC_ERROR;
        return result;
    }

    // 提取寄存器/线圈数据
    if (functionCode == 0x03 || functionCode == 0x04) {
        uint8_t byteCount = (uint8_t)rawResult.data[2];
        if (byteCount != quantity * 2) {
            LOG_WARNINGF("[Modbus] OneShot read: byte count mismatch (expected %d, got %d)", quantity * 2, byteCount);
            result.error = ONESHOT_CRC_ERROR;
            return result;
        }
        // 防御性检查：确保响应数据不越界
        uint16_t maxIdx = 3 + (quantity - 1) * 2 + 1;  // 最后需要访问的索引
        if (maxIdx >= rawResult.count) {
            LOG_WARNINGF("[Modbus] OneShot read: response too short (need idx %d, have %d bytes)", maxIdx, rawResult.count);
            result.error = ONESHOT_CRC_ERROR;
            return result;
        }
        for (uint16_t i = 0; i < quantity; i++) {
            uint16_t idx1 = 3 + i * 2;
            uint16_t idx2 = 4 + i * 2;
            result.data[i] = ((uint16_t)(uint8_t)rawResult.data[idx1] << 8) | (uint8_t)rawResult.data[idx2];
        }
    } else if (functionCode == 0x01 || functionCode == 0x02) {
        // 标准 FC 0x01/0x02 响应: [addr, FC, byteCount, data..., CRC_L, CRC_H]
        // 非标准响应(部分继电器板): [addr, FC, data..., CRC_L, CRC_H] (无byteCount)
        uint8_t expectedDataBytes = (quantity + 7) / 8;
        uint8_t standardLen = 3 + expectedDataBytes + 2;  // addr+FC+byteCount+data+CRC
        uint8_t nonStdLen   = 2 + expectedDataBytes + 2;  // addr+FC+data+CRC (无byteCount)

        uint8_t dataOffset;
        uint8_t actualByteCount;
        if (respLen == standardLen) {
            dataOffset = 3;
            actualByteCount = (uint8_t)rawResult.data[2];
        } else if (respLen == nonStdLen) {
            dataOffset = 2;
            actualByteCount = expectedDataBytes;
            LOG_INFOF("[Modbus] OneShot read: non-standard FC 0x%02X response (no byteCount field, %d bytes)", functionCode, respLen);
        } else {
            dataOffset = 3;
            actualByteCount = (uint8_t)rawResult.data[2];
            LOG_WARNINGF("[Modbus] OneShot read: unexpected FC 0x%02X response length %d (expected %d or %d)", functionCode, respLen, standardLen, nonStdLen);
        }

        // 调试：打印原始响应帧 hex
        {
            String rxHex = "";
            for (uint8_t i = 0; i < respLen && i < 16; i++) {
                if (i > 0) rxHex += ' ';
                char h[4]; snprintf(h, sizeof(h), "%02X", (uint8_t)rawResult.data[i]);
                rxHex += h;
            }
            LOG_INFOF("[Modbus] FC 0x%02X raw resp: [%s] len=%d dataOffset=%d byteCount=%d",
                      functionCode, rxHex.c_str(), respLen, dataOffset, actualByteCount);
        }

        for (uint16_t i = 0; i < quantity && i < Protocols::MODBUS_MAX_REGISTERS_PER_READ; i++) {
            uint8_t byteIdx = i / 8;
            uint8_t bitIdx = i % 8;
            // 防御性检查：确保访问索引不越界
            uint16_t accessIdx = (uint16_t)dataOffset + byteIdx;
            uint8_t dataByte = 0;
            if (byteIdx < actualByteCount && accessIdx < rawResult.count) {
                dataByte = (uint8_t)rawResult.data[accessIdx];
            }
            result.data[i] = (dataByte & (1 << bitIdx)) ? 1 : 0;
            LOG_DEBUGF("[Modbus] FC 0x%02X CH%d: dataByte=0x%02X bitIdx=%d → value=%d",
                       functionCode, i, dataByte, bitIdx, result.data[i]);
        }
    }

    result.count = quantity;
    result.error = ONESHOT_SUCCESS;
    LOG_INFOF("[Modbus] OneShot read: success, read %d values from slave %d", quantity, slaveAddress);
    return result;
}

// ============================================================================
// 内部：执行实际的 Modbus 通信（不涉及信号量，纯通信逻辑）
// ============================================================================
OneShotResult ModbusHandler::_executeModbusTransaction(const uint8_t* request, uint8_t reqLen,
                                                        uint8_t expectedSlaveAddr) {
    OneShotResult result;
    result.error = ONESHOT_TIMEOUT;
    result.count = 0;

    // 连续超时冷却保护：防止持续超时耗尽堆内存导致 abort()
    if (_consecutiveTimeouts >= CONSECUTIVE_TIMEOUT_THRESHOLD) {
        unsigned long now = millis();
        if (now < _cooldownUntil) {
            result.error = ONESHOT_TIMEOUT;
            LOG_WARNINGF("[Modbus] In cooldown (%lus left), skip transaction",
                         (_cooldownUntil - now) / 1000);
            return result;
        }
        // 冷却结束，重置计数器，尝试一次
        _consecutiveTimeouts = 0;
        LOG_INFO("[Modbus] Cooldown ended, resuming communication");
    }

    // 超时参数
    uint16_t timeout = config.master.responseTimeout;
    if (timeout == 0) timeout = 1000;
    unsigned long charTimeout = 35000000UL / config.baudRate;
    if (charTimeout < 2) charTimeout = 2;
    uint8_t maxRetries = config.master.maxRetries;

    // 捕获 TX 帧 hex（用于调试面板显示）—— 低堆时跳过以减少分配压力
    bool heapSafe = ESP.getFreeHeap() > 15000;
    if (heapSafe) {
        _lastTxHex = "";
        _lastRxHex = "";
        for (uint8_t i = 0; i < reqLen; i++) {
            if (i > 0) _lastTxHex += ' ';
            char hex[4];
            snprintf(hex, sizeof(hex), "%02X", request[i]);
            _lastTxHex += hex;
        }
    }

    // 重试循环
    for (uint8_t attempt = 0; attempt <= maxRetries; attempt++) {
        if (attempt > 0) {
            LOG_INFOF("[Modbus] OneShot: retry %d/%d", attempt, maxRetries);
            delay(50);
            yield();
        }

        // 清空接收缓冲区
        while (modbusSerial->available()) modbusSerial->read();

        // 发送请求帧
        setTransmitMode(true);
        modbusSerial->write(request, reqLen);
        modbusSerial->flush();
        setTransmitMode(false);

        // 阻塞等待响应
        uint8_t respBuf[Protocols::MODBUS_BUFFER_SIZE];
        uint8_t respIdx = 0;
        unsigned long sendTime = millis();
        unsigned long lastRxTime = sendTime;
        bool frameComplete = false;
        bool timedOut = false;

        while (true) {
            while (modbusSerial->available() && respIdx < Protocols::MODBUS_BUFFER_SIZE) {
                respBuf[respIdx++] = modbusSerial->read();
                lastRxTime = millis();
            }
            if (respIdx > 0 && (millis() - lastRxTime) > charTimeout) {
                frameComplete = true;
                break;
            }
            if ((millis() - sendTime) > timeout) {
                timedOut = true;
                break;
            }
            yield();
        }

        // 超时且无数据
        if (timedOut && respIdx == 0) {
            LOG_WARNINGF("[Modbus] OneShot: timeout (attempt %d, no response)", attempt + 1);
            result.error = ONESHOT_TIMEOUT;
            continue;
        }

        // 帧过短
        if (respIdx < 4) {
            LOG_WARNINGF("[Modbus] OneShot: frame too short (%d bytes)", respIdx);
            result.error = ONESHOT_CRC_ERROR;
            continue;
        }

        // CRC 校验
        if (!validateFrame(respBuf, respIdx)) {
            LOG_WARNING("[Modbus] OneShot: CRC validation failed");
            result.error = ONESHOT_CRC_ERROR;
            continue;
        }

        // 捕获 RX 帧 hex（CRC 校验通过后）—— 低堆时跳过
        if (heapSafe) {
            _lastRxHex = "";
            for (uint8_t i = 0; i < respIdx; i++) {
                if (i > 0) _lastRxHex += ' ';
                char hex[4];
                snprintf(hex, sizeof(hex), "%02X", respBuf[i]);
                _lastRxHex += hex;
            }
        }

        // 地址匹配（广播地址 0x00 时跳过，允许任意从站响应）
        if (expectedSlaveAddr != 0 && respBuf[0] != expectedSlaveAddr) {
            LOG_WARNINGF("[Modbus] OneShot: address mismatch (expected %d, got %d)", expectedSlaveAddr, respBuf[0]);
            result.error = ONESHOT_CRC_ERROR;
            continue;
        }

        // 异常响应检测
        if (respBuf[1] & 0x80) {
            result.error = ONESHOT_EXCEPTION;
            result.exceptionCode = respBuf[2];
            LOG_WARNINGF("[Modbus] OneShot: exception response 0x%02X", respBuf[2]);
            break; // 异常是确定性错误，不重试
        }

        // 成功 — 将原始响应帧存入 result.data（按uint16打包，便于调用者解析）
        result.error = ONESHOT_SUCCESS;
        result.count = respIdx;  // 存储原始响应字节数
        // 将响应字节逐一存入data数组（每个uint16存一个字节）
        for (uint8_t i = 0; i < respIdx && i < Protocols::MODBUS_MAX_REGISTERS_PER_READ; i++) {
            result.data[i] = respBuf[i];
        }
        LOG_INFOF("[Modbus] OneShot: success, %d bytes response from slave %d", respIdx, expectedSlaveAddr);
        break;
    }

    // 连续超时追踪：成功时重置，超时时累加并触发冷却
    if (result.error == ONESHOT_SUCCESS || result.error == ONESHOT_EXCEPTION) {
        _consecutiveTimeouts = 0;
    } else if (result.error == ONESHOT_TIMEOUT) {
        _consecutiveTimeouts++;
        if (_consecutiveTimeouts >= CONSECUTIVE_TIMEOUT_THRESHOLD) {
            _cooldownUntil = millis() + COOLDOWN_DURATION_MS;
            LOG_WARNINGF("[Modbus] %d consecutive timeouts, entering %lus cooldown",
                         _consecutiveTimeouts, COOLDOWN_DURATION_MS / 1000);
        }
    }

    return result;
}

// ============================================================================
// 通用一次性发送/接收辅助方法
// ============================================================================
OneShotResult ModbusHandler::sendOneShotRequest(const uint8_t* request, uint8_t reqLen,
                                                 uint8_t expectedSlaveAddr) {
    OneShotResult result;

    // 前置检查
    if (!isInitialized || !modbusSerial) {
        result.error = ONESHOT_NOT_INITIALIZED;
        LOG_WARNING("[Modbus] OneShot: not initialized");
        return result;
    }
    if (isOneShotReading) {
        // 使用信号量等待总线空闲（FreeRTOS 友好）
        if (_oneShotSemaphore) {
            // 等待获取信号量，最多 2 秒
            if (xSemaphoreTake(_oneShotSemaphore, pdMS_TO_TICKS(2000)) == pdFALSE) {
                result.error = ONESHOT_BUSY;
                LOG_WARNING("[Modbus] OneShot: busy (timeout waiting for semaphore)");
                return result;
            }
            LOG_INFO("[Modbus] OneShot: bus was busy, now free after wait");
        } else {
            // 信号量未创建，使用旧的轮询方式（降级兼容）
            const unsigned long maxWaitMs = 2000;
            unsigned long startWait = millis();
            while (isOneShotReading) {
                if (millis() - startWait >= maxWaitMs) {
                    result.error = ONESHOT_BUSY;
                    LOG_WARNING("[Modbus] OneShot: busy (timeout waiting for bus)");
                    return result;
                }
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            LOG_INFO("[Modbus] OneShot: bus was busy, now free after wait");
        }
    } else {
        // 总线空闲，立即获取信号量
        if (_oneShotSemaphore) {
            // 非阻塞获取，确保独占访问
            if (xSemaphoreTake(_oneShotSemaphore, 0) == pdFALSE) {
                result.error = ONESHOT_BUSY;
                LOG_WARNING("[Modbus] OneShot: failed to acquire semaphore");
                return result;
            }
        }
    }
    if (expectedSlaveAddr > 247) {
        result.error = ONESHOT_EXCEPTION;
        result.exceptionCode = MODBUS_EX_ILLEGAL_DATA_ADDRESS;
        LOG_WARNINGF("[Modbus] OneShot: invalid slave address %d", expectedSlaveAddr);
        if (_oneShotSemaphore) {
            xSemaphoreGive(_oneShotSemaphore);
        }
        return result;
    }

    // 设置守卫标志
    isOneShotReading = true;

    // 执行实际的 Modbus 通信
    result = _executeModbusTransaction(request, reqLen, expectedSlaveAddr);

    isOneShotReading = false;
    // 释放信号量
    if (_oneShotSemaphore) {
        xSemaphoreGive(_oneShotSemaphore);
    }
    return result;
}

// ============================================================================
// 控制操作快速通道（高优先级，短超时）
// ============================================================================
OneShotResult ModbusHandler::sendControlRequest(uint8_t* requestBuffer, uint8_t requestLength, 
                                                 uint8_t expectedSlaveAddress) {
    OneShotResult result;
    result.error = ONESHOT_TIMEOUT;
    result.count = 0;
    
    if (!isInitialized || !modbusSerial) {
        result.error = ONESHOT_NOT_INITIALIZED;
        LOG_WARNING("[Modbus] ControlRequest: not initialized");
        return result;
    }
    
    // 设置控制挂起标志，通知轮询任务让路
    _controlPending = true;
    
    // 尝试获取信号量（2000ms超时，与普通请求一致）
    if (_oneShotSemaphore) {
        if (xSemaphoreTake(_oneShotSemaphore, pdMS_TO_TICKS(2000)) == pdFALSE) {
            _controlPending = false;
            result.error = ONESHOT_BUSY;
            LOG_WARNING("[Modbus] ControlRequest: busy (timeout waiting for semaphore)");
            return result;
        }
    }
    
    // 设置守卫标志
    isOneShotReading = true;
    
    // 执行实际的 Modbus 通信
    result = _executeModbusTransaction(requestBuffer, requestLength, expectedSlaveAddress);
    
    // 释放信号量和标志
    isOneShotReading = false;
    if (_oneShotSemaphore) {
        xSemaphoreGive(_oneShotSemaphore);
    }
    _controlPending = false;
    
    return result;
}

// ============================================================================
// FC 0x05 写单个线圈（阻塞）
// ============================================================================
OneShotResult ModbusHandler::writeCoilOnce(uint8_t slaveAddr, uint16_t coilAddr, bool value, bool isControl) {
    LOG_INFOF("[Modbus] writeCoilOnce: slave=%d coil=%d value=%d isControl=%d", 
              slaveAddr, coilAddr, value ? 1 : 0, isControl ? 1 : 0);

    uint8_t request[8];
    request[0] = slaveAddr;
    request[1] = 0x05;
    request[2] = (coilAddr >> 8) & 0xFF;
    request[3] = coilAddr & 0xFF;
    request[4] = value ? 0xFF : 0x00;
    request[5] = 0x00;
    uint16_t crc = calculateCRC(request, 6);
    request[6] = crc & 0xFF;
    request[7] = (crc >> 8) & 0xFF;

    OneShotResult result = isControl ? sendControlRequest(request, 8, slaveAddr) 
                                     : sendOneShotRequest(request, 8, slaveAddr);

    // 验证回显帧：FC 0x05 的正常响应是原请求的回显
    if (result.error == ONESHOT_SUCCESS && result.count >= 6) {
        uint8_t respFC = (uint8_t)result.data[1];
        if (respFC != 0x05) {
            result.error = ONESHOT_EXCEPTION;
            result.exceptionCode = MODBUS_EX_ILLEGAL_FUNCTION;
        }
    }
    return result;
}

// ============================================================================
// FC 0x05 写单个线圈 — 原始值版本（支持设备专有值如 0x5500 toggle）
// ============================================================================
OneShotResult ModbusHandler::writeCoilOnce(uint8_t slaveAddr, uint16_t coilAddr, uint16_t rawValue, bool isControl) {
    LOG_INFOF("[Modbus] writeCoilOnce(raw): slave=%d coil=%d rawValue=0x%04X isControl=%d", 
              slaveAddr, coilAddr, rawValue, isControl ? 1 : 0);

    uint8_t request[8];
    request[0] = slaveAddr;
    request[1] = 0x05;
    request[2] = (coilAddr >> 8) & 0xFF;
    request[3] = coilAddr & 0xFF;
    request[4] = (rawValue >> 8) & 0xFF;
    request[5] = rawValue & 0xFF;
    uint16_t crc = calculateCRC(request, 6);
    request[6] = crc & 0xFF;
    request[7] = (crc >> 8) & 0xFF;

    OneShotResult result = isControl ? sendControlRequest(request, 8, slaveAddr) 
                                     : sendOneShotRequest(request, 8, slaveAddr);

    if (result.error == ONESHOT_SUCCESS && result.count >= 6) {
        uint8_t respFC = (uint8_t)result.data[1];
        if (respFC != 0x05) {
            result.error = ONESHOT_EXCEPTION;
            result.exceptionCode = MODBUS_EX_ILLEGAL_FUNCTION;
        }
    }
    return result;
}

// ============================================================================
// 发送原始 Modbus 帧（自动追加 CRC，用于设备专有功能码如 0xB0）
// ============================================================================
OneShotResult ModbusHandler::sendRawFrameOnce(uint8_t expectedSlaveAddr,
                                               const uint8_t* dataWithoutCRC, uint8_t dataLen, bool isControl) {
    LOG_INFOF("[Modbus] sendRawFrameOnce: slave=%d dataLen=%d isControl=%d", 
              expectedSlaveAddr, dataLen, isControl ? 1 : 0);

    if (dataLen < 2 || dataLen > (Protocols::MODBUS_BUFFER_SIZE - 2)) {
        OneShotResult result;
        result.error = ONESHOT_EXCEPTION;
        result.exceptionCode = MODBUS_EX_ILLEGAL_DATA_VALUE;
        return result;
    }

    uint8_t frame[Protocols::MODBUS_BUFFER_SIZE];
    memcpy(frame, dataWithoutCRC, dataLen);
    uint16_t crc = calculateCRC(frame, dataLen);
    frame[dataLen] = crc & 0xFF;
    frame[dataLen + 1] = (crc >> 8) & 0xFF;

    return isControl ? sendControlRequest(frame, dataLen + 2, expectedSlaveAddr)
                     : sendOneShotRequest(frame, dataLen + 2, expectedSlaveAddr);
}

// ============================================================================
// FC 0x0F 写多个线圈（阻塞）
// ============================================================================
OneShotResult ModbusHandler::writeMultipleCoilsOnce(uint8_t slaveAddr, uint16_t startAddr,
                                                     uint16_t quantity, const bool* values, bool isControl) {
    OneShotResult result;

    if (quantity == 0 || quantity > Protocols::MODBUS_MAX_WRITE_COILS) {
        result.error = ONESHOT_EXCEPTION;
        result.exceptionCode = MODBUS_EX_ILLEGAL_DATA_VALUE;
        LOG_WARNINGF("[Modbus] writeMultipleCoilsOnce: invalid quantity %d", quantity);
        return result;
    }

    LOG_INFOF("[Modbus] writeMultipleCoilsOnce: slave=%d start=%d qty=%d", slaveAddr, startAddr, quantity);

    uint8_t byteCount = (quantity + 7) / 8;
    uint8_t reqLen = 7 + byteCount + 2; // addr+fc+startH+startL+qtyH+qtyL+byteCount+data+CRC

    // 使用栈缓冲（最大 7+4+2=13 字节，32线圈=4字节数据）
    uint8_t request[16];
    if (reqLen > sizeof(request)) {
        result.error = ONESHOT_EXCEPTION;
        result.exceptionCode = MODBUS_EX_ILLEGAL_DATA_VALUE;
        return result;
    }

    request[0] = slaveAddr;
    request[1] = 0x0F;
    request[2] = (startAddr >> 8) & 0xFF;
    request[3] = startAddr & 0xFF;
    request[4] = (quantity >> 8) & 0xFF;
    request[5] = quantity & 0xFF;
    request[6] = byteCount;

    // 将bool数组打包为字节
    memset(&request[7], 0, byteCount);
    for (uint16_t i = 0; i < quantity; i++) {
        if (values[i]) {
            request[7 + i / 8] |= (1 << (i % 8));
        }
    }

    uint16_t crc = calculateCRC(request, 7 + byteCount);
    request[7 + byteCount] = crc & 0xFF;
    request[7 + byteCount + 1] = (crc >> 8) & 0xFF;

    result = isControl ? sendControlRequest(request, reqLen, slaveAddr)
                       : sendOneShotRequest(request, reqLen, slaveAddr);

    // 验证响应：FC 0x0F 响应为 [addr, 0x0F, startH, startL, qtyH, qtyL, CRC]
    if (result.error == ONESHOT_SUCCESS && result.count >= 6) {
        uint8_t respFC = (uint8_t)result.data[1];
        if (respFC != 0x0F) {
            result.error = ONESHOT_EXCEPTION;
            result.exceptionCode = MODBUS_EX_ILLEGAL_FUNCTION;
        }
    }
    return result;
}

// ============================================================================
// FC 0x06 写单个寄存器（阻塞）
// ============================================================================
OneShotResult ModbusHandler::writeRegisterOnce(uint8_t slaveAddr, uint16_t regAddr, uint16_t value, bool isControl) {
    LOG_INFOF("[Modbus] writeRegisterOnce: slave=%d reg=%d value=%d isControl=%d", 
              slaveAddr, regAddr, value, isControl ? 1 : 0);

    uint8_t request[8];
    request[0] = slaveAddr;
    request[1] = 0x06;
    request[2] = (regAddr >> 8) & 0xFF;
    request[3] = regAddr & 0xFF;
    request[4] = (value >> 8) & 0xFF;
    request[5] = value & 0xFF;
    uint16_t crc = calculateCRC(request, 6);
    request[6] = crc & 0xFF;
    request[7] = (crc >> 8) & 0xFF;

    OneShotResult result = isControl ? sendControlRequest(request, 8, slaveAddr) 
                                     : sendOneShotRequest(request, 8, slaveAddr);

    if (result.error == ONESHOT_SUCCESS && result.count >= 6) {
        uint8_t respFC = (uint8_t)result.data[1];
        if (respFC != 0x06) {
            result.error = ONESHOT_EXCEPTION;
            result.exceptionCode = MODBUS_EX_ILLEGAL_FUNCTION;
        }
    }
    return result;
}

// ============================================================================
// FC 0x10 写多个寄存器（阻塞）
// ============================================================================
OneShotResult ModbusHandler::writeMultipleRegistersOnce(uint8_t slaveAddr, uint16_t startAddr,
                                                         uint16_t quantity, const uint16_t* values, bool isControl) {
    OneShotResult result;

    if (quantity == 0 || quantity > 123) {
        result.error = ONESHOT_EXCEPTION;
        result.exceptionCode = MODBUS_EX_ILLEGAL_DATA_VALUE;
        LOG_WARNINGF("[Modbus] writeMultipleRegistersOnce: invalid quantity %d", quantity);
        return result;
    }

    LOG_INFOF("[Modbus] writeMultipleRegistersOnce: slave=%d start=%d qty=%d", slaveAddr, startAddr, quantity);

    uint8_t byteCount = quantity * 2;
    uint8_t reqLen = 7 + byteCount + 2; // addr+fc+startH+startL+qtyH+qtyL+byteCount+data+CRC

    // 动态分配缓冲区（最大 7+246+2=255 字节）
    if (reqLen > Protocols::MODBUS_BUFFER_SIZE) {
        result.error = ONESHOT_EXCEPTION;
        result.exceptionCode = MODBUS_EX_ILLEGAL_DATA_VALUE;
        return result;
    }

    uint8_t request[Protocols::MODBUS_BUFFER_SIZE];
    request[0] = slaveAddr;
    request[1] = 0x10;
    request[2] = (startAddr >> 8) & 0xFF;
    request[3] = startAddr & 0xFF;
    request[4] = (quantity >> 8) & 0xFF;
    request[5] = quantity & 0xFF;
    request[6] = byteCount;

    for (uint16_t i = 0; i < quantity; i++) {
        request[7 + i * 2] = (values[i] >> 8) & 0xFF;
        request[7 + i * 2 + 1] = values[i] & 0xFF;
    }

    uint16_t crc = calculateCRC(request, 7 + byteCount);
    request[7 + byteCount] = crc & 0xFF;
    request[7 + byteCount + 1] = (crc >> 8) & 0xFF;

    result = isControl ? sendControlRequest(request, reqLen, slaveAddr)
                       : sendOneShotRequest(request, reqLen, slaveAddr);

    // 验证响应：FC 0x10 响应为 [addr, 0x10, startH, startL, qtyH, qtyL, CRC]
    if (result.error == ONESHOT_SUCCESS && result.count >= 6) {
        uint8_t respFC = (uint8_t)result.data[1];
        if (respFC != 0x10) {
            result.error = ONESHOT_EXCEPTION;
            result.exceptionCode = MODBUS_EX_ILLEGAL_FUNCTION;
        }
    }
    return result;
}

// ============================================================================
// 线圈延时任务管理
// ============================================================================
bool ModbusHandler::addCoilDelayTask(uint8_t slaveAddr, uint16_t coilAddr, unsigned long delayMs, bool coilValue) {
    return addCoilDelayTask(slaveAddr, coilAddr, delayMs, coilValue, false);
}

bool ModbusHandler::addCoilDelayTask(uint8_t slaveAddr, uint16_t coilAddr, 
                                      unsigned long delayMs, bool coilValue, bool isRegisterMode) {
    // 优先复用同一线圈地址的已有任务
    for (uint8_t i = 0; i < Protocols::MODBUS_MAX_COIL_DELAY_TASKS; i++) {
        if (coilDelayTasks[i].active &&
            coilDelayTasks[i].slaveAddress == slaveAddr &&
            coilDelayTasks[i].coilAddress == coilAddr) {
            coilDelayTasks[i].triggerTime = millis() + delayMs;
            coilDelayTasks[i].coilValue = coilValue;
            coilDelayTasks[i].isRegisterMode = isRegisterMode;
            LOG_INFOF("[Modbus] DelayTask: updated slot %d, slave=%d addr=%d delay=%lums val=%s mode=%s",
                      i, slaveAddr, coilAddr, delayMs, 
                      coilValue ? "ON" : "OFF",
                      isRegisterMode ? "register" : "coil");
            return true;
        }
    }
    // 查找空闲槽位
    for (uint8_t i = 0; i < Protocols::MODBUS_MAX_COIL_DELAY_TASKS; i++) {
        if (!coilDelayTasks[i].active) {
            coilDelayTasks[i].slaveAddress = slaveAddr;
            coilDelayTasks[i].coilAddress = coilAddr;
            coilDelayTasks[i].triggerTime = millis() + delayMs;
            coilDelayTasks[i].coilValue = coilValue;
            coilDelayTasks[i].isRegisterMode = isRegisterMode;
            coilDelayTasks[i].active = true;
            LOG_INFOF("[Modbus] DelayTask: added slot %d, slave=%d addr=%d delay=%lums val=%s mode=%s",
                      i, slaveAddr, coilAddr, delayMs, 
                      coilValue ? "ON" : "OFF",
                      isRegisterMode ? "register" : "coil");
            return true;
        }
    }
    LOG_WARNING("[Modbus] DelayTask: queue full, cannot add task");
    return false;
}

void ModbusHandler::processCoilDelayTasks() {
    unsigned long now = millis();
    for (uint8_t i = 0; i < Protocols::MODBUS_MAX_COIL_DELAY_TASKS; i++) {
        if (coilDelayTasks[i].active && now >= coilDelayTasks[i].triggerTime) {
            LOG_INFOF("[Modbus] DelayTask: executing slot %d, slave=%d addr=%d -> %s",
                      i, coilDelayTasks[i].slaveAddress, coilDelayTasks[i].coilAddress,
                      coilDelayTasks[i].coilValue ? "ON" : "OFF");
            if (coilDelayTasks[i].isRegisterMode) {
                uint16_t regValue = coilDelayTasks[i].coilValue ? 1 : 0;
                writeRegisterOnce(coilDelayTasks[i].slaveAddress, 
                                  coilDelayTasks[i].coilAddress, regValue);
            } else {
                writeCoilOnce(coilDelayTasks[i].slaveAddress, 
                              coilDelayTasks[i].coilAddress, coilDelayTasks[i].coilValue);
            }
            coilDelayTasks[i].active = false;
        }
    }
}

String ModbusHandler::formatRawHex(uint8_t slaveAddr, uint8_t fc, const uint16_t* data, uint16_t count) {
    // 重构Modbus响应帧: [slaveAddr][fc][byteCount][data_hi data_lo ...][crc_lo crc_hi]
    uint8_t byteCount = count * 2;
    uint8_t frameLen = 3 + byteCount + 2; // addr + fc + byteCount + data + CRC
    uint8_t frame[frameLen];
    frame[0] = slaveAddr;
    frame[1] = fc;
    frame[2] = byteCount;
    for (uint16_t i = 0; i < count; i++) {
        frame[3 + i * 2]     = (data[i] >> 8) & 0xFF;
        frame[3 + i * 2 + 1] = data[i] & 0xFF;
    }
    uint16_t crc = calculateCRC(frame, 3 + byteCount);
    frame[3 + byteCount]     = crc & 0xFF;        // CRC低字节
    frame[3 + byteCount + 1] = (crc >> 8) & 0xFF;  // CRC高字节

    String hexStr;
    hexStr.reserve(frameLen * 2);
    for (uint8_t i = 0; i < frameLen; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02X", frame[i]);
        hexStr += buf;
    }
    return hexStr;
}

// PeriphExec 调度的轮询任务执行
String ModbusHandler::executePollTaskByIndex(uint8_t taskIdx, uint16_t timeout, uint8_t retries) {
    if (taskIdx >= config.master.taskCount) {
        LOG_WARNINGF("[Modbus] executePollTaskByIndex: invalid idx %d (count=%d)", taskIdx, config.master.taskCount);
        return "[]";
    }

    const PollTask& task = config.master.tasks[taskIdx];
    if (!task.enabled) return "[]";

    // 检查堆内存是否充足
    if (ESP.getFreeHeap() < 10000) {
        LOG_WARNINGF("[Modbus] Insufficient heap for JSON: %d bytes", ESP.getFreeHeap());
        return "[]";
    }

    // 更新统计计数
    _totalPollCount++;
    _lastPollTime = millis();

    // 保存原始通信参数
    uint16_t origTimeout = config.master.responseTimeout;
    uint8_t origRetries = config.master.maxRetries;

    // 临时设置 PeriphExec 传入的参数
    config.master.responseTimeout = timeout;
    config.master.maxRetries = retries;

    LOG_DEBUGF("[Modbus] PollTask[%d]: slave=%d FC=%d start=%d qty=%d (timeout=%d retries=%d)",
              taskIdx, task.slaveAddress, task.functionCode, task.startAddress, task.quantity,
              timeout, retries);

    // 检查是否有控制操作挂起，如有则跳过本次轮询，让控制操作优先获取信号量
    if (_controlPending) {
        LOG_INFO("[Modbus] PollTask: control pending, skipping poll to yield bus");
        config.master.responseTimeout = origTimeout;
        config.master.maxRetries = origRetries;
        return "[]";
    }

    OneShotResult result = readRegistersOnce(task.slaveAddress, task.functionCode,
                                              task.startAddress, task.quantity);

    // 恢复原始参数
    config.master.responseTimeout = origTimeout;
    config.master.maxRetries = origRetries;

    // 更新任务缓存和统计计数
    PollTaskCache& cache = _taskCache[taskIdx];
    cache.timestamp = millis();
    cache.lastError = (uint8_t)result.error;
    
    if (result.error != ONESHOT_SUCCESS) {
        LOG_WARNINGF("[Modbus] PollTask[%d] failed: error=%d", taskIdx, result.error);
        
        // 分类统计失败原因
        if (result.error == ONESHOT_TIMEOUT) {
            _timeoutPollCount++;
        } else {
            _failedPollCount++;
        }
        
        // 标记缓存无效
        cache.valid = false;
        cache.count = 0;
        // 低堆时跳过状态通知（避免 JsonDocument 分配导致 abort）
        if (ESP.getFreeHeap() > 15000) {
            _notifyStatusChange();
        }
        return "[]";
    }
    
    // 成功：更新成功计数和缓存数据
    _successPollCount++;
    cache.valid = true;
    cache.count = result.count;
    memcpy(cache.values, result.data, result.count * sizeof(uint16_t));
    _notifyStatusChange();  // 通知状态变化：轮询成功

    // 应用寄存器映射生成 JSON
    if (task.mappingCount > 0) {
        // 预分配缓冲区
        String json;
        json.reserve(512);
        json = "[";
        bool first = true;

        for (uint8_t i = 0; i < task.mappingCount; i++) {
            const RegisterMapping& m = task.mappings[i];
            if (m.sensorId[0] == '\0') continue;

            float rawValue = 0.0f;
            switch (m.dataType) {
                case 0: // uint16
                    if (m.regOffset < result.count) rawValue = (float)result.data[m.regOffset];
                    break;
                case 1: // int16
                    if (m.regOffset < result.count) rawValue = (float)(int16_t)result.data[m.regOffset];
                    break;
                case 2: // uint32
                    if (m.regOffset + 1 < result.count) {
                        uint32_t u32 = ((uint32_t)result.data[m.regOffset] << 16) | result.data[m.regOffset + 1];
                        rawValue = (float)u32;
                    }
                    break;
                case 3: // int32
                    if (m.regOffset + 1 < result.count) {
                        uint32_t u32 = ((uint32_t)result.data[m.regOffset] << 16) | result.data[m.regOffset + 1];
                        rawValue = (float)(int32_t)u32;
                    }
                    break;
                case 4: // float32
                    if (m.regOffset + 1 < result.count) {
                        uint32_t u32 = ((uint32_t)result.data[m.regOffset] << 16) | result.data[m.regOffset + 1];
                        memcpy(&rawValue, &u32, sizeof(float));
                    }
                    break;
                default:
                    if (m.regOffset < result.count) rawValue = (float)result.data[m.regOffset];
                    break;
            }

            float scaledValue = rawValue * m.scaleFactor;
            char valBuf[16];
            dtostrf(scaledValue, 1, m.decimalPlaces, valBuf);

            if (!first) json += ",";
            json += "{\"id\":\"";
            json += m.sensorId;
            json += "\",\"value\":\"";
            json += valBuf;
            json += "\"}";
            first = false;
        }
        json += "]";

        // 同时通过 dataCallback 上报（如 MQTT）
        if (!first && dataCallback) {
            dataCallback(task.slaveAddress, json);
        }
        return first ? "[]" : json;
    }

    // 无映射配置: 返回原始寄存器数据 JSON
    // 预分配缓冲区
    String json;
    json.reserve(512);
    json = "[{\"id\":\"slave_" + String(task.slaveAddress) +
                  "_raw\",\"value\":\"";
    for (uint16_t i = 0; i < result.count; i++) {
        if (i > 0) json += ",";
        json += String(result.data[i]);
    }
    json += "\"}]";

    if (dataCallback) {
        dataCallback(task.slaveAddress, json);
    }
    return json;
}

// ============================================================================
// 轮询统计接口
// ============================================================================

String ModbusHandler::getPollStatistics() const {
    JsonDocument doc;
    doc["totalPolls"] = _totalPollCount;
    doc["successPolls"] = _successPollCount;
    doc["failedPolls"] = _failedPollCount;
    doc["timeoutPolls"] = _timeoutPollCount;
    doc["lastPollMs"] = _lastPollTime > 0 ? (millis() - _lastPollTime) / 1000 : 0;
    
    String out;
    serializeJson(doc, out);
    return out;
}

const ModbusHandler::PollTaskCache* ModbusHandler::getTaskCache(uint8_t taskIdx) const {
    if (taskIdx >= Protocols::MODBUS_MAX_POLL_TASKS) return nullptr;
    return &_taskCache[taskIdx];
}

void ModbusHandler::resetPollStatistics() {
    _totalPollCount = 0;
    _successPollCount = 0;
    _failedPollCount = 0;
    _timeoutPollCount = 0;
    _lastPollTime = 0;
    memset(_taskCache, 0, sizeof(_taskCache));
    LOG_INFO("[Modbus] Poll statistics reset");
}
