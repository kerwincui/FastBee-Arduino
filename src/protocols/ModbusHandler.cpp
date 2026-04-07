/**
 * @description: Modbus RTU 处理器 - 支持从站(Slave)和主站(Master)双模式
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:31:03
 *
 * Slave模式: 被动响应，支持功能码 0x01-0x06, 0x0F, 0x10
 * Master模式: 主动轮询，非阻塞状态机驱动，支持FC 0x01-0x04读取和FC 0x06写入
 * 支持异常响应: ILLEGAL_FUNCTION, ILLEGAL_DATA_ADDRESS, ILLEGAL_DATA_VALUE
 */

#include "protocols/ModbusHandler.h"
#include <ArduinoJson.h>

ModbusHandler::ModbusHandler() 
    : isInitialized(false), modbusSerial(&Serial2),
      pollState(POLL_IDLE), currentTaskIndex(0),
      pollStateTimestamp(0), currentRetryCount(0),
      responseIndex(0), lastByteTime(0), currentIsWrite(false),
      isOneShotReading(false) {
    memset(holdingRegisters, 0, sizeof(holdingRegisters));
    memset(inputRegisters, 0, sizeof(inputRegisters));
    memset(coils, 0, sizeof(coils));
    memset(discreteInputs, 0, sizeof(discreteInputs));
    memset(taskLastPollTime, 0, sizeof(taskLastPollTime));
    memset(responseBuffer, 0, sizeof(responseBuffer));
}

ModbusHandler::~ModbusHandler() {
    end();
}

bool ModbusHandler::begin(const ModbusConfig& cfg) {
    this->config = cfg;
    
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
        memset(taskLastPollTime, 0, sizeof(taskLastPollTime));
        pollState = POLL_IDLE;
        masterStats = MasterStats();
    }
    
    return true;
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
                const char* lbl   = t["label"] | "";
                strncpy(task.label, lbl, sizeof(task.label) - 1);
                task.label[sizeof(task.label) - 1] = '\0';
                
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
                        task.mappingCount++;
                    }
                }
                
                config.master.taskCount++;
            }
        }
    }
    
    LOG_INFO("Modbus: Config loaded successfully");
    return true;
}

bool ModbusHandler::saveConfigToFile(const String& configPath) {
    String actualPath = configPath.isEmpty() ? config.configFile : configPath;
    
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
        t["label"] = task.label;
        
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
            }
        }
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
        handleMaster();
    } else {
        handleSlave();
    }
}

// ============ Slave模式主循环 ============

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
                  ", OK:" + String(masterStats.successPolls) +
                  ", Fail:" + String(masterStats.failedPolls);
    }
    
    return status;
}

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

// ============ FC 0x01: 读线圈 ============

void ModbusHandler::handleReadCoils(const uint8_t* frame, uint8_t length) {
    if (length < 8) {
        sendExceptionResponse(config.slaveAddress, 0x01, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    
    uint16_t startAddr = (frame[2] << 8) | frame[3];
    uint16_t quantity  = (frame[4] << 8) | frame[5];
    
    if (quantity == 0 || quantity > 2000) {
        sendExceptionResponse(config.slaveAddress, 0x01, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    
    if (startAddr + quantity > sizeof(coils) * 8) {
        sendExceptionResponse(config.slaveAddress, 0x01, MODBUS_EX_ILLEGAL_DATA_ADDRESS);
        return;
    }
    
    uint8_t byteCount = (quantity + 7) / 8;
    uint8_t response[Protocols::MODBUS_BUFFER_SIZE];
    response[0] = config.slaveAddress;
    response[1] = 0x01;
    response[2] = byteCount;
    
    if (!readCoils(startAddr, quantity, &response[3])) {
        sendExceptionResponse(config.slaveAddress, 0x01, MODBUS_EX_SLAVE_DEVICE_FAILURE);
        return;
    }
    
    uint8_t respLen = 3 + byteCount;
    uint16_t crc = calculateCRC(response, respLen);
    response[respLen]     = crc & 0xFF;
    response[respLen + 1] = (crc >> 8) & 0xFF;
    
    sendResponse(response, respLen + 2);
}

// ============ FC 0x02: 读离散输入 ============

void ModbusHandler::handleReadDiscreteInputs(const uint8_t* frame, uint8_t length) {
    if (length < 8) {
        sendExceptionResponse(config.slaveAddress, 0x02, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    
    uint16_t startAddr = (frame[2] << 8) | frame[3];
    uint16_t quantity  = (frame[4] << 8) | frame[5];
    
    if (quantity == 0 || quantity > 2000) {
        sendExceptionResponse(config.slaveAddress, 0x02, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    
    if (startAddr + quantity > sizeof(discreteInputs) * 8) {
        sendExceptionResponse(config.slaveAddress, 0x02, MODBUS_EX_ILLEGAL_DATA_ADDRESS);
        return;
    }
    
    uint8_t byteCount = (quantity + 7) / 8;
    uint8_t response[Protocols::MODBUS_BUFFER_SIZE];
    response[0] = config.slaveAddress;
    response[1] = 0x02;
    response[2] = byteCount;
    
    if (!readDiscreteInputs(startAddr, quantity, &response[3])) {
        sendExceptionResponse(config.slaveAddress, 0x02, MODBUS_EX_SLAVE_DEVICE_FAILURE);
        return;
    }
    
    uint8_t respLen = 3 + byteCount;
    uint16_t crc = calculateCRC(response, respLen);
    response[respLen]     = crc & 0xFF;
    response[respLen + 1] = (crc >> 8) & 0xFF;
    
    sendResponse(response, respLen + 2);
}

// ============ FC 0x03: 读保持寄存器 ============

void ModbusHandler::handleReadHoldingRegisters(const uint8_t* frame, uint8_t length) {
    if (length < 8) {
        sendExceptionResponse(config.slaveAddress, 0x03, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    
    uint16_t startAddr = (frame[2] << 8) | frame[3];
    uint16_t quantity  = (frame[4] << 8) | frame[5];
    
    if (quantity == 0 || quantity > 125) {
        sendExceptionResponse(config.slaveAddress, 0x03, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    
    if (startAddr + quantity > sizeof(holdingRegisters) / sizeof(holdingRegisters[0])) {
        sendExceptionResponse(config.slaveAddress, 0x03, MODBUS_EX_ILLEGAL_DATA_ADDRESS);
        return;
    }
    
    uint16_t regData[125];
    if (!readHoldingRegisters(startAddr, quantity, regData)) {
        sendExceptionResponse(config.slaveAddress, 0x03, MODBUS_EX_SLAVE_DEVICE_FAILURE);
        return;
    }
    
    uint8_t byteCount = quantity * 2;
    uint8_t response[Protocols::MODBUS_BUFFER_SIZE];
    response[0] = config.slaveAddress;
    response[1] = 0x03;
    response[2] = byteCount;
    
    for (uint16_t i = 0; i < quantity; i++) {
        response[3 + i * 2]     = (regData[i] >> 8) & 0xFF;
        response[3 + i * 2 + 1] = regData[i] & 0xFF;
    }
    
    uint8_t respLen = 3 + byteCount;
    uint16_t crc = calculateCRC(response, respLen);
    response[respLen]     = crc & 0xFF;
    response[respLen + 1] = (crc >> 8) & 0xFF;
    
    sendResponse(response, respLen + 2);
}

// ============ FC 0x04: 读输入寄存器 ============

void ModbusHandler::handleReadInputRegisters(const uint8_t* frame, uint8_t length) {
    if (length < 8) {
        sendExceptionResponse(config.slaveAddress, 0x04, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    
    uint16_t startAddr = (frame[2] << 8) | frame[3];
    uint16_t quantity  = (frame[4] << 8) | frame[5];
    
    if (quantity == 0 || quantity > 125) {
        sendExceptionResponse(config.slaveAddress, 0x04, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    
    if (startAddr + quantity > sizeof(inputRegisters) / sizeof(inputRegisters[0])) {
        sendExceptionResponse(config.slaveAddress, 0x04, MODBUS_EX_ILLEGAL_DATA_ADDRESS);
        return;
    }
    
    uint16_t regData[125];
    if (!readInputRegisters(startAddr, quantity, regData)) {
        sendExceptionResponse(config.slaveAddress, 0x04, MODBUS_EX_SLAVE_DEVICE_FAILURE);
        return;
    }
    
    uint8_t byteCount = quantity * 2;
    uint8_t response[Protocols::MODBUS_BUFFER_SIZE];
    response[0] = config.slaveAddress;
    response[1] = 0x04;
    response[2] = byteCount;
    
    for (uint16_t i = 0; i < quantity; i++) {
        response[3 + i * 2]     = (regData[i] >> 8) & 0xFF;
        response[3 + i * 2 + 1] = regData[i] & 0xFF;
    }
    
    uint8_t respLen = 3 + byteCount;
    uint16_t crc = calculateCRC(response, respLen);
    response[respLen]     = crc & 0xFF;
    response[respLen + 1] = (crc >> 8) & 0xFF;
    
    sendResponse(response, respLen + 2);
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
    
    if (quantity == 0 || quantity > 1968) {
        sendExceptionResponse(config.slaveAddress, 0x0F, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    
    uint8_t expectedByteCount = (quantity + 7) / 8;
    if (byteCount != expectedByteCount) {
        sendExceptionResponse(config.slaveAddress, 0x0F, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    
    if (length < (uint8_t)(7 + byteCount + 2)) {
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
    
    uint8_t response[8];
    response[0] = config.slaveAddress;
    response[1] = 0x0F;
    response[2] = (startAddr >> 8) & 0xFF;
    response[3] = startAddr & 0xFF;
    response[4] = (quantity >> 8) & 0xFF;
    response[5] = quantity & 0xFF;
    uint16_t crc = calculateCRC(response, 6);
    response[6] = crc & 0xFF;
    response[7] = (crc >> 8) & 0xFF;
    
    sendResponse(response, 8);
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
    
    if (quantity == 0 || quantity > 123) {
        sendExceptionResponse(config.slaveAddress, 0x10, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    
    if (byteCount != quantity * 2) {
        sendExceptionResponse(config.slaveAddress, 0x10, MODBUS_EX_ILLEGAL_DATA_VALUE);
        return;
    }
    
    if (length < (uint8_t)(7 + byteCount + 2)) {
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
    
    uint8_t response[8];
    response[0] = config.slaveAddress;
    response[1] = 0x10;
    response[2] = (startAddr >> 8) & 0xFF;
    response[3] = startAddr & 0xFF;
    response[4] = (quantity >> 8) & 0xFF;
    response[5] = quantity & 0xFF;
    uint16_t crc = calculateCRC(response, 6);
    response[6] = crc & 0xFF;
    response[7] = (crc >> 8) & 0xFF;
    
    sendResponse(response, 8);
}

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

// ============ 轮询任务管理 ============

bool ModbusHandler::addPollTask(const PollTask& task) {
    if (config.master.taskCount >= Protocols::MODBUS_MAX_POLL_TASKS) {
        LOG_WARNING("Modbus Master: Max poll tasks reached");
        return false;
    }
    config.master.tasks[config.master.taskCount] = task;
    config.master.taskCount++;
    return true;
}

bool ModbusHandler::removePollTask(uint8_t index) {
    if (index >= config.master.taskCount) return false;
    
    for (uint8_t i = index; i < config.master.taskCount - 1; i++) {
        config.master.tasks[i] = config.master.tasks[i + 1];
        taskLastPollTime[i] = taskLastPollTime[i + 1];
    }
    config.master.taskCount--;
    return true;
}

bool ModbusHandler::updatePollTask(uint8_t index, const PollTask& task) {
    if (index >= config.master.taskCount) return false;
    config.master.tasks[index] = task;
    return true;
}

PollTask ModbusHandler::getPollTask(uint8_t index) const {
    if (index >= config.master.taskCount) return PollTask();
    return config.master.tasks[index];
}

bool ModbusHandler::masterWriteSingleRegister(uint8_t slaveAddr, uint16_t regAddr, uint16_t value) {
    for (uint8_t i = 0; i < Protocols::MODBUS_MAX_WRITE_QUEUE; i++) {
        if (!writeQueue[i].pending) {
            writeQueue[i].slaveAddress = slaveAddr;
            writeQueue[i].regAddress = regAddr;
            writeQueue[i].value = value;
            writeQueue[i].pending = true;
            LOG_INFOF("Modbus Master: Write queued - slave=%d, reg=%d, val=%d", slaveAddr, regAddr, value);
            return true;
        }
    }
    LOG_WARNING("Modbus Master: Write queue full");
    return false;
}

String ModbusHandler::getMasterStatus() const {
    DynamicJsonDocument doc(256);
    doc["mode"] = (config.mode == MODBUS_MASTER) ? "master" : "slave";
    doc["state"] = (uint8_t)pollState;
    doc["taskCount"] = config.master.taskCount;
    doc["totalPolls"] = masterStats.totalPolls;
    doc["successPolls"] = masterStats.successPolls;
    doc["failedPolls"] = masterStats.failedPolls;
    doc["timeoutPolls"] = masterStats.timeoutPolls;
    
    String result;
    serializeJson(doc, result);
    return result;
}

TaskDataCache ModbusHandler::getTaskDataCache(uint8_t index) const {
    if (index < Protocols::MODBUS_MAX_POLL_TASKS) {
        return taskDataCache[index];
    }
    return TaskDataCache();
}

// ============ Master请求帧构建 ============

uint8_t ModbusHandler::buildMasterRequest(const PollTask& task, uint8_t* buffer) {
    // 读请求帧: [slaveAddr, FC, startAddrHi, startAddrLo, qtyHi, qtyLo, CRClo, CRChi]
    buffer[0] = task.slaveAddress;
    buffer[1] = task.functionCode;
    buffer[2] = (task.startAddress >> 8) & 0xFF;
    buffer[3] = task.startAddress & 0xFF;
    buffer[4] = (task.quantity >> 8) & 0xFF;
    buffer[5] = task.quantity & 0xFF;
    
    uint16_t crc = calculateCRC(buffer, 6);
    buffer[6] = crc & 0xFF;
    buffer[7] = (crc >> 8) & 0xFF;
    
    return 8;
}

uint8_t ModbusHandler::buildWriteRequest(const WriteRequest& req, uint8_t* buffer) {
    // FC 0x06: 写单寄存器 [slaveAddr, 0x06, regHi, regLo, valHi, valLo, CRClo, CRChi]
    buffer[0] = req.slaveAddress;
    buffer[1] = 0x06;
    buffer[2] = (req.regAddress >> 8) & 0xFF;
    buffer[3] = req.regAddress & 0xFF;
    buffer[4] = (req.value >> 8) & 0xFF;
    buffer[5] = req.value & 0xFF;
    
    uint16_t crc = calculateCRC(buffer, 6);
    buffer[6] = crc & 0xFF;
    buffer[7] = (crc >> 8) & 0xFF;
    
    return 8;
}

// ============ Master响应解析 ============

bool ModbusHandler::parseMasterResponse(const uint8_t* buffer, uint8_t length, const PollTask& task) {
    if (length < 5) return false;
    
    // 检查从站地址
    if (buffer[0] != task.slaveAddress) {
        LOG_WARNINGF("Modbus Master: Address mismatch: expected %d, got %d", task.slaveAddress, buffer[0]);
        return false;
    }
    
    // 检查异常响应
    if (buffer[1] & 0x80) {
        uint8_t exCode = buffer[2];
        LOG_WARNINGF("Modbus Master: Exception from slave %d, FC=0x%02X, Ex=0x%02X",
                     task.slaveAddress, task.functionCode, exCode);
        return false;
    }
    
    // 检查功能码匹配
    if (buffer[1] != task.functionCode) {
        LOG_WARNINGF("Modbus Master: FC mismatch: expected 0x%02X, got 0x%02X", task.functionCode, buffer[1]);
        return false;
    }
    
    // CRC校验
    if (!validateFrame(buffer, length)) {
        LOG_WARNING("Modbus Master: CRC error in response");
        return false;
    }
    
    // 透传模式: 直接上报原始十六进制帧
    if (config.transferType == 1) {
        reportRawData(task, buffer, length);
        return true;
    }
    
    // JSON模式: 解析寄存器读响应数据
    uint8_t byteCount = buffer[2];
    
    if (task.functionCode == 0x03 || task.functionCode == 0x04) {
        // 寄存器读响应: [addr, FC, byteCount, data..., CRC]
        if (byteCount != task.quantity * 2) {
            LOG_WARNING("Modbus Master: Byte count mismatch in response");
            return false;
        }
        
        uint16_t regData[Protocols::MODBUS_MAX_REGISTERS_PER_READ];
        for (uint16_t i = 0; i < task.quantity && i < Protocols::MODBUS_MAX_REGISTERS_PER_READ; i++) {
            regData[i] = (buffer[3 + i * 2] << 8) | buffer[3 + i * 2 + 1];
        }
        
        reportPollData(task, regData, task.quantity);
    } else if (task.functionCode == 0x01 || task.functionCode == 0x02) {
        // 线圈/离散输入读响应: [addr, FC, byteCount, data..., CRC]
        // 将位数据转为寄存器格式上报
        uint16_t regData[Protocols::MODBUS_MAX_REGISTERS_PER_READ];
        uint16_t count = min((uint16_t)byteCount, (uint16_t)Protocols::MODBUS_MAX_REGISTERS_PER_READ);
        for (uint16_t i = 0; i < count; i++) {
            regData[i] = buffer[3 + i];
        }
        reportPollData(task, regData, count);
    }
    
    return true;
}

// ============ 数据上报 ============

void ModbusHandler::reportPollData(const PollTask& task, const uint16_t* data, uint16_t count) {
    // 缓存最新轮询数据（无论是否有 dataCallback）
    if (currentTaskIndex < Protocols::MODBUS_MAX_POLL_TASKS) {
        TaskDataCache& cache = taskDataCache[currentTaskIndex];
        uint16_t copyCount = min(count, (uint16_t)Protocols::MODBUS_MAX_REGISTERS_PER_READ);
        memcpy(cache.values, data, copyCount * sizeof(uint16_t));
        cache.count = copyCount;
        cache.timestamp = millis();
        cache.valid = true;
    }

    if (!dataCallback) return;
    
    // JSON模式且有映射配置: 生成 [{id,value}] 格式
    if (config.transferType == 0 && task.mappingCount > 0) {
        String json = "[";
        bool first = true;
        
        for (uint8_t i = 0; i < task.mappingCount; i++) {
            const RegisterMapping& m = task.mappings[i];
            if (m.sensorId[0] == '\0') continue;
            
            float rawValue = 0.0f;
            
            switch (m.dataType) {
                case 0: // uint16
                    if (m.regOffset < count) {
                        rawValue = (float)data[m.regOffset];
                    }
                    break;
                case 1: // int16
                    if (m.regOffset < count) {
                        rawValue = (float)(int16_t)data[m.regOffset];
                    }
                    break;
                case 2: // uint32 (两个寄存器)
                    if (m.regOffset + 1 < count) {
                        uint32_t u32 = ((uint32_t)data[m.regOffset] << 16) | data[m.regOffset + 1];
                        rawValue = (float)u32;
                    }
                    break;
                case 3: // int32 (两个寄存器)
                    if (m.regOffset + 1 < count) {
                        uint32_t u32 = ((uint32_t)data[m.regOffset] << 16) | data[m.regOffset + 1];
                        rawValue = (float)(int32_t)u32;
                    }
                    break;
                case 4: // float32 (两个寄存器, IEEE 754)
                    if (m.regOffset + 1 < count) {
                        uint32_t u32 = ((uint32_t)data[m.regOffset] << 16) | data[m.regOffset + 1];
                        memcpy(&rawValue, &u32, sizeof(float));
                    }
                    break;
                default:
                    if (m.regOffset < count) {
                        rawValue = (float)data[m.regOffset];
                    }
                    break;
            }
            
            float scaledValue = rawValue * m.scaleFactor;
            
            // 格式化数值
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
        
        if (!first) { // 至少有一个映射输出
            dataCallback(task.slaveAddress, json);
            return;
        }
    }
    
    // 默认: 旧文本格式（向后兼容，mappingCount==0 时使用）
    String report = "MASTER:slave=" + String(task.slaveAddress) +
                    ",fc=" + String(task.functionCode) +
                    ",start=" + String(task.startAddress) +
                    ",count=" + String(count) +
                    ",data=[";
    
    for (uint16_t i = 0; i < count; i++) {
        if (i > 0) report += ",";
        report += String(data[i]);
    }
    report += "]";
    
    if (task.label[0] != '\0') {
        report += ",label=" + String(task.label);
    }
    
    dataCallback(task.slaveAddress, report);
}

void ModbusHandler::reportRawData(const PollTask& task, const uint8_t* frame, uint8_t frameLen) {
    if (!dataCallback) return;
    
    // 将原始响应帧转为十六进制字符串
    String hexStr;
    hexStr.reserve(frameLen * 2 + 1);
    for (uint8_t i = 0; i < frameLen; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02X", frame[i]);
        hexStr += buf;
    }
    
    dataCallback(task.slaveAddress, hexStr);
}

// ============ 轮询调度 ============

int8_t ModbusHandler::findNextPollTask() {
    unsigned long now = millis();
    int8_t bestTask = -1;
    unsigned long maxOverdue = 0;
    
    for (uint8_t i = 0; i < config.master.taskCount; i++) {
        if (!config.master.tasks[i].enabled) continue;
        
        unsigned long interval = (unsigned long)config.master.tasks[i].pollInterval * 1000UL;
        unsigned long elapsed = now - taskLastPollTime[i];
        
        if (elapsed >= interval) {
            unsigned long overdue = elapsed - interval;
            if (bestTask < 0 || overdue > maxOverdue) {
                bestTask = i;
                maxOverdue = overdue;
            }
        }
    }
    
    return bestTask;
}

bool ModbusHandler::processWriteQueue() {
    for (uint8_t i = 0; i < Protocols::MODBUS_MAX_WRITE_QUEUE; i++) {
        if (writeQueue[i].pending) {
            return true; // 有待处理的写请求
        }
    }
    return false;
}

// ============ Master状态机主循环 ============

void ModbusHandler::handleMaster() {
    // 一次性读取进行中，暂停轮询状态机
    if (isOneShotReading) return;
    
    unsigned long now = millis();
    
    switch (pollState) {
        case POLL_IDLE: {
            // 周期性诊断日志（每60秒）
            static unsigned long lastDiagLog = 0;
            if (now - lastDiagLog > 60000) {
                lastDiagLog = now;
                LOG_INFOF("Modbus Master stats: total=%lu ok=%lu fail=%lu timeout=%lu tasks=%d wm=%d",
                           (unsigned long)masterStats.totalPolls,
                           (unsigned long)masterStats.successPolls,
                           (unsigned long)masterStats.failedPolls,
                           (unsigned long)masterStats.timeoutPolls,
                           config.master.taskCount, config.workMode);
            }
            
            // 优先处理写请求队列
            for (uint8_t i = 0; i < Protocols::MODBUS_MAX_WRITE_QUEUE; i++) {
                if (writeQueue[i].pending) {
                    // 清空接收缓冲区
                    while (modbusSerial->available()) modbusSerial->read();
                    
                    uint8_t requestBuffer[8];
                    uint8_t reqLen = buildWriteRequest(writeQueue[i], requestBuffer);
                    
                    setTransmitMode(true);
                    modbusSerial->write(requestBuffer, reqLen);
                    modbusSerial->flush();
                    setTransmitMode(false);
                    
                    writeQueue[i].pending = false;
                    currentIsWrite = true;
                    responseIndex = 0;
                    pollStateTimestamp = now;
                    pollState = POLL_WAITING;
                    
                    LOG_DEBUGF("Modbus Master: Write sent to slave %d, reg %d",
                              writeQueue[i].slaveAddress, writeQueue[i].regAddress);
                    return;
                }
            }
            
            // MQTT指令模式：不执行主动轮询，只处理写请求队列和一次性读取
            if (config.workMode == 0) {
                processCoilDelayTasks();
                return;
            }
            
            // 处理线圈延时任务
            processCoilDelayTasks();
            
            // 查找到期的轮询任务
            int8_t nextTask = findNextPollTask();
            if (nextTask < 0) return; // 没有到期任务
            
            currentTaskIndex = nextTask;
            currentIsWrite = false;
            currentRetryCount = 0;
            
            // 确保满足interPollDelay
            if (now - pollStateTimestamp < config.master.interPollDelay) return;
            
            pollState = POLL_SENDING;
            break;
        }
        
        case POLL_SENDING: {
            const PollTask& task = config.master.tasks[currentTaskIndex];
            
            // 清空接收缓冲区
            while (modbusSerial->available()) modbusSerial->read();
            
            // 构建并发送请求
            uint8_t requestBuffer[8];
            uint8_t reqLen = buildMasterRequest(task, requestBuffer);
            
            setTransmitMode(true);
            modbusSerial->write(requestBuffer, reqLen);
            modbusSerial->flush();
            setTransmitMode(false);
            
            // 进入等待状态
            responseIndex = 0;
            lastByteTime = now;
            pollStateTimestamp = now;
            pollState = POLL_WAITING;
            masterStats.totalPolls++;
            
            LOG_DEBUGF("Modbus Master: Poll sent to slave %d, FC=0x%02X, start=%d, qty=%d",
                      task.slaveAddress, task.functionCode, task.startAddress, task.quantity);
            break;
        }
        
        case POLL_WAITING: {
            // 读取可用的串口数据
            while (modbusSerial->available()) {
                if (responseIndex < Protocols::MODBUS_BUFFER_SIZE) {
                    responseBuffer[responseIndex++] = modbusSerial->read();
                    lastByteTime = now;
                } else {
                    modbusSerial->read(); // 丢弃溢出数据
                }
            }
            
            // 检查帧超时（收到数据后3.5字符时间无新数据 = 一帧结束）
            // 3.5字符时间(ms) = 35000 / baudRate（每字符10位：start+8data+stop）
            // 低波特率(<=19200)按标准计算，高波特率固定1.75ms，最小保底2ms
            unsigned long charTimeout = (config.baudRate <= 19200) ? (35000UL / config.baudRate + 1) : 2;
            if (charTimeout < 2) charTimeout = 2;
            
            if (responseIndex > 0 && (now - lastByteTime) > charTimeout) {
                // 帧接收完成，验证
                if (responseIndex >= 5 && validateFrame(responseBuffer, responseIndex)) {
                    if (currentIsWrite) {
                        // 写请求响应 - 仅确认成功
                        LOG_DEBUG("Modbus Master: Write acknowledged");
                        pollState = POLL_COMPLETE;
                    } else {
                        const PollTask& task = config.master.tasks[currentTaskIndex];
                        if (parseMasterResponse(responseBuffer, responseIndex, task)) {
                            pollState = POLL_COMPLETE;
                        } else {
                            pollState = POLL_ERROR;
                        }
                    }
                } else {
                    LOG_WARNINGF("Modbus Master: Invalid frame, len=%d", responseIndex);
                    pollState = POLL_ERROR;
                }
                return;
            }
            
            // 检查响应超时
            uint16_t timeout = currentIsWrite ? config.responseTimeout : config.master.responseTimeout;
            if ((now - pollStateTimestamp) > timeout) {
                if (responseIndex == 0) {
                    LOG_WARNINGF("Modbus Master: Timeout - no response (attempt %d/%d)",
                                currentRetryCount + 1, config.master.maxRetries + 1);
                } else {
                    LOG_WARNINGF("Modbus Master: Timeout - incomplete frame (%d bytes)", responseIndex);
                }
                masterStats.timeoutPolls++;
                pollState = POLL_ERROR;
            }
            break;
        }
        
        case POLL_COMPLETE: {
            if (!currentIsWrite) {
                masterStats.successPolls++;
                taskLastPollTime[currentTaskIndex] = now;
            }
            
            pollStateTimestamp = now;
            pollState = POLL_IDLE;
            break;
        }
        
        case POLL_ERROR: {
            if (!currentIsWrite && currentRetryCount < config.master.maxRetries) {
                currentRetryCount++;
                LOG_INFOF("Modbus Master: Retrying task %d (attempt %d)", currentTaskIndex, currentRetryCount + 1);
                pollState = POLL_SENDING;
                return;
            }
            
            if (!currentIsWrite) {
                masterStats.failedPolls++;
                taskLastPollTime[currentTaskIndex] = now; // 避免立即重试
            }
            
            pollStateTimestamp = now;
            pollState = POLL_IDLE;
            break;
        }
    }
}

// ========== 一次性寄存器读取（阻塞式，用于MQTT指令触发） ==========

OneShotResult ModbusHandler::readRegistersOnce(uint8_t slaveAddress, uint8_t functionCode,
                                                uint16_t startAddress, uint16_t quantity) {
    OneShotResult result;

    // 参数校验（读操作特有）
    if (functionCode < 0x01 || functionCode > 0x04) {
        result.error = ONESHOT_EXCEPTION;
        result.exceptionCode = MODBUS_EX_ILLEGAL_FUNCTION;
        LOG_WARNINGF("[Modbus] OneShot read: invalid function code 0x%02X", functionCode);
        return result;
    }
    if (quantity == 0 || quantity > Protocols::MODBUS_MAX_REGISTERS_PER_READ) {
        result.error = ONESHOT_EXCEPTION;
        result.exceptionCode = MODBUS_EX_ILLEGAL_DATA_VALUE;
        LOG_WARNINGF("[Modbus] OneShot read: invalid quantity %d", quantity);
        return result;
    }

    LOG_INFOF("[Modbus] OneShot read: slave=%d fc=0x%02X addr=%d qty=%d",
              slaveAddress, functionCode, startAddress, quantity);

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

    // 使用通用发送/接收
    OneShotResult rawResult = sendOneShotRequest(requestBuffer, 8, slaveAddress);

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
        for (uint16_t i = 0; i < quantity; i++) {
            result.data[i] = ((uint16_t)(uint8_t)rawResult.data[3 + i * 2] << 8) | (uint8_t)rawResult.data[4 + i * 2];
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
            uint8_t dataByte = (byteIdx < actualByteCount) ? (uint8_t)rawResult.data[dataOffset + byteIdx] : 0;
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
        result.error = ONESHOT_BUSY;
        LOG_WARNING("[Modbus] OneShot: busy (another one-shot in progress)");
        return result;
    }
    if (expectedSlaveAddr > 247) {
        result.error = ONESHOT_EXCEPTION;
        result.exceptionCode = MODBUS_EX_ILLEGAL_DATA_ADDRESS;
        LOG_WARNINGF("[Modbus] OneShot: invalid slave address %d", expectedSlaveAddr);
        return result;
    }

    // 设置守卫标志
    isOneShotReading = true;

    // 等待状态机回到 POLL_IDLE
    unsigned long waitStart = millis();
    unsigned long maxWait = (unsigned long)config.master.responseTimeout * 2;
    while (pollState != POLL_IDLE && (millis() - waitStart) < maxWait) {
        yield();
        delay(1);
    }
    if (pollState != POLL_IDLE) {
        isOneShotReading = false;
        result.error = ONESHOT_BUSY;
        LOG_WARNING("[Modbus] OneShot: state machine not idle, giving up");
        return result;
    }

    // 超时参数
    uint16_t timeout = config.master.responseTimeout;
    if (timeout == 0) timeout = 1000;
    unsigned long charTimeout = 35000000UL / config.baudRate;
    if (charTimeout < 2) charTimeout = 2;
    uint8_t maxRetries = config.master.maxRetries;

    // 捕获 TX 帧 hex（用于调试面板显示）
    _lastTxHex = "";
    _lastRxHex = "";
    for (uint8_t i = 0; i < reqLen; i++) {
        if (i > 0) _lastTxHex += ' ';
        char hex[4];
        snprintf(hex, sizeof(hex), "%02X", request[i]);
        _lastTxHex += hex;
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

        // 捕获 RX 帧 hex（CRC 校验通过后）
        _lastRxHex = "";
        for (uint8_t i = 0; i < respIdx; i++) {
            if (i > 0) _lastRxHex += ' ';
            char hex[4];
            snprintf(hex, sizeof(hex), "%02X", respBuf[i]);
            _lastRxHex += hex;
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

    isOneShotReading = false;
    return result;
}

// ============================================================================
// FC 0x05 写单个线圈（阻塞）
// ============================================================================
OneShotResult ModbusHandler::writeCoilOnce(uint8_t slaveAddr, uint16_t coilAddr, bool value) {
    LOG_INFOF("[Modbus] writeCoilOnce: slave=%d coil=%d value=%d", slaveAddr, coilAddr, value ? 1 : 0);

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

    OneShotResult result = sendOneShotRequest(request, 8, slaveAddr);

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
OneShotResult ModbusHandler::writeCoilOnce(uint8_t slaveAddr, uint16_t coilAddr, uint16_t rawValue) {
    LOG_INFOF("[Modbus] writeCoilOnce(raw): slave=%d coil=%d rawValue=0x%04X", slaveAddr, coilAddr, rawValue);

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

    OneShotResult result = sendOneShotRequest(request, 8, slaveAddr);

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
                                               const uint8_t* dataWithoutCRC, uint8_t dataLen) {
    LOG_INFOF("[Modbus] sendRawFrameOnce: slave=%d dataLen=%d", expectedSlaveAddr, dataLen);

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

    return sendOneShotRequest(frame, dataLen + 2, expectedSlaveAddr);
}

// ============================================================================
// FC 0x0F 写多个线圈（阻塞）
// ============================================================================
OneShotResult ModbusHandler::writeMultipleCoilsOnce(uint8_t slaveAddr, uint16_t startAddr,
                                                     uint16_t quantity, const bool* values) {
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

    result = sendOneShotRequest(request, reqLen, slaveAddr);

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
OneShotResult ModbusHandler::writeRegisterOnce(uint8_t slaveAddr, uint16_t regAddr, uint16_t value) {
    LOG_INFOF("[Modbus] writeRegisterOnce: slave=%d reg=%d value=%d", slaveAddr, regAddr, value);

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

    OneShotResult result = sendOneShotRequest(request, 8, slaveAddr);

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
                                                         uint16_t quantity, const uint16_t* values) {
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

    result = sendOneShotRequest(request, reqLen, slaveAddr);

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
bool ModbusHandler::addCoilDelayTask(uint8_t slaveAddr, uint16_t coilAddr, unsigned long delayMs) {
    // 优先复用同一线圈地址的已有任务
    for (uint8_t i = 0; i < Protocols::MODBUS_MAX_COIL_DELAY_TASKS; i++) {
        if (coilDelayTasks[i].active &&
            coilDelayTasks[i].slaveAddress == slaveAddr &&
            coilDelayTasks[i].coilAddress == coilAddr) {
            coilDelayTasks[i].triggerTime = millis() + delayMs;
            LOG_INFOF("[Modbus] DelayTask: updated slot %d, slave=%d coil=%d delay=%lums",
                      i, slaveAddr, coilAddr, delayMs);
            return true;
        }
    }
    // 查找空闲槽位
    for (uint8_t i = 0; i < Protocols::MODBUS_MAX_COIL_DELAY_TASKS; i++) {
        if (!coilDelayTasks[i].active) {
            coilDelayTasks[i].slaveAddress = slaveAddr;
            coilDelayTasks[i].coilAddress = coilAddr;
            coilDelayTasks[i].triggerTime = millis() + delayMs;
            coilDelayTasks[i].active = true;
            LOG_INFOF("[Modbus] DelayTask: added slot %d, slave=%d coil=%d delay=%lums",
                      i, slaveAddr, coilAddr, delayMs);
            return true;
        }
    }
    LOG_WARNING("[Modbus] DelayTask: queue full");
    return false;
}

void ModbusHandler::processCoilDelayTasks() {
    unsigned long now = millis();
    for (uint8_t i = 0; i < Protocols::MODBUS_MAX_COIL_DELAY_TASKS; i++) {
        if (coilDelayTasks[i].active && now >= coilDelayTasks[i].triggerTime) {
            LOG_INFOF("[Modbus] DelayTask: executing slot %d, turning OFF slave=%d coil=%d",
                      i, coilDelayTasks[i].slaveAddress, coilDelayTasks[i].coilAddress);
            writeCoilOnce(coilDelayTasks[i].slaveAddress, coilDelayTasks[i].coilAddress, false);
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
