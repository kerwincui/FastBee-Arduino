/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:31:03
 */

#include "protocols/ModbusHandler.h"
#include <ArduinoJson.h>

ModbusHandler::ModbusHandler() 
    : isInitialized(false), modbusSerial(&Serial2) {
    // 初始化寄存器为0
    memset(holdingRegisters, 0, sizeof(holdingRegisters));
    memset(inputRegisters, 0, sizeof(inputRegisters));
    memset(coils, 0, sizeof(coils));
    memset(discreteInputs, 0, sizeof(discreteInputs));
}

ModbusHandler::~ModbusHandler() {
    end();
}

bool ModbusHandler::begin(const ModbusConfig& config) {
    this->config = config;
    
    if (!initializePins()) {
        Serial.println("Modbus Handler: Pin initialization failed");
        return false;
    }
    
    if (!initializeSerial()) {
        Serial.println("Modbus Handler: Serial initialization failed");
        return false;
    }
    
    isInitialized = true;
    Serial.println("Modbus Handler: Initialized successfully");
    Serial.printf("  Slave Address: %d\n", config.slaveAddress);
    Serial.printf("  Baud Rate: %d\n", config.baudRate);
    Serial.printf("  TX Pin: %d, RX Pin: %d\n", config.txPin, config.rxPin);
    Serial.printf("  DE Pin: %d\n", config.dePin);
    
    return true;
}

bool ModbusHandler::begin(const String& configPath) {
    if (!loadConfigFromFile(configPath)) {
        Serial.println("Modbus Handler: Failed to load config from file, using defaults");
        // 使用默认配置继续初始化
    }
    
    return begin(config);
}

void ModbusHandler::end() {
    if (isInitialized) {
        modbusSerial->end();
        isInitialized = false;
        Serial.println("Modbus Handler: Stopped");
    }
}

bool ModbusHandler::loadConfigFromFile(const String& configPath) {
    String actualPath = configPath.isEmpty() ? config.configFile : configPath;
    
    Serial.printf("Modbus Handler: Loading config from %s\n", actualPath.c_str());
    
    if (!FileUtils::exists(actualPath)) {
        Serial.println("Modbus Handler: Config file not found");
        return false;
    }
    
    String jsonContent = FileUtils::readFile(actualPath);
    if (jsonContent.isEmpty()) {
        Serial.println("Modbus Handler: Config file is empty");
        return false;
    }
    
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, jsonContent);
    if (error) {
        Serial.printf("Modbus Handler: JSON parse error: %s\n", error.c_str());
        return false;
    }
    
    // 读取配置值
    if (doc.containsKey("slaveAddress")) {
        config.slaveAddress = doc["slaveAddress"].as<uint8_t>();
    }
    
    if (doc.containsKey("baudRate")) {
        config.baudRate = doc["baudRate"].as<uint32_t>();
    }
    
    if (doc.containsKey("txPin")) {
        config.txPin = doc["txPin"].as<uint8_t>();
    }
    
    if (doc.containsKey("rxPin")) {
        config.rxPin = doc["rxPin"].as<uint8_t>();
    }
    
    if (doc.containsKey("dePin")) {
        config.dePin = doc["dePin"].as<uint8_t>();
    }
    
    if (doc.containsKey("responseTimeout")) {
        config.responseTimeout = doc["responseTimeout"].as<uint16_t>();
    }
    
    if (doc.containsKey("interFrameDelay")) {
        config.interFrameDelay = doc["interFrameDelay"].as<uint16_t>();
    }
    
    if (doc.containsKey("configFile")) {
        config.configFile = doc["configFile"].as<String>();
    }
    
    Serial.println("Modbus Handler: Config loaded successfully");
    return true;
}

bool ModbusHandler::saveConfigToFile(const String& configPath) {
    String actualPath = configPath.isEmpty() ? config.configFile : configPath;
    
    DynamicJsonDocument doc(1024);
    doc["slaveAddress"] = config.slaveAddress;
    doc["baudRate"] = config.baudRate;
    doc["txPin"] = config.txPin;
    doc["rxPin"] = config.rxPin;
    doc["dePin"] = config.dePin;
    doc["responseTimeout"] = config.responseTimeout;
    doc["interFrameDelay"] = config.interFrameDelay;
    doc["configFile"] = config.configFile;
    
    String jsonContent;
    serializeJsonPretty(doc, jsonContent);
    
    bool success = FileUtils::writeFile(actualPath, jsonContent);
    if (success) {
        Serial.printf("Modbus Handler: Config saved to %s\n", actualPath.c_str());
    } else {
        Serial.println("Modbus Handler: Failed to save config");
    }
    
    return success;
}

bool ModbusHandler::initializeSerial() {
    // 停止之前的串口（如果正在运行）
    if (modbusSerial) {
        modbusSerial->end();
    }
    
    // 初始化Modbus串口
    modbusSerial->begin(config.baudRate, SERIAL_8N1, config.rxPin, config.txPin);
    modbusSerial->setTimeout(config.responseTimeout);
    
    Serial.printf("Modbus Handler: Serial initialized on pins RX:%d, TX:%d\n", 
                  config.rxPin, config.txPin);
    
    return true;
}

bool ModbusHandler::initializePins() {
    // 初始化方向控制引脚（如果使用RS485）
    if (config.dePin != 255) {
        pinMode(config.dePin, OUTPUT);
        digitalWrite(config.dePin, LOW);  // 默认接收模式
        Serial.printf("Modbus Handler: DE pin initialized on pin %d\n", config.dePin);
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

void ModbusHandler::handle() {
    if (!isInitialized) return;
    
    // 处理接收到的Modbus数据
    while (modbusSerial->available()) {
        static uint8_t buffer[256];
        static uint8_t index = 0;
        static unsigned long lastByteTime = 0;
        
        uint8_t byte = modbusSerial->read();
        unsigned long currentTime = millis();
        
        // 帧超时检测（3.5字符时间）
        if (index > 0 && (currentTime - lastByteTime) > (35000000 / config.baudRate)) {
            index = 0; // 超时，重置缓冲区
        }
        
        lastByteTime = currentTime;
        
        if (index < sizeof(buffer)) {
            buffer[index++] = byte;
            
            // 简单的帧检测
            if (index >= 6) { // 最小Modbus帧长度
                if (validateFrame(buffer, index)) {
                    processModbusFrame(buffer, index);
                    index = 0;
                } else if (index >= sizeof(buffer)) {
                    index = 0; // 缓冲区溢出，重置
                }
            }
        } else {
            index = 0; // 缓冲区溢出，重置
        }
    }
}

String ModbusHandler::getStatus() const {
    if (!isInitialized) {
        return "Not initialized";
    }
    
    String status = "Initialized - Addr:" + String(config.slaveAddress) +
                   ", Baud:" + String(config.baudRate) +
                   ", Pins:TX" + String(config.txPin) + "/RX" + String(config.rxPin);
    
    if (config.dePin != 255) {
        status += ", DE:" + String(config.dePin);
    }
    
    return status;
}

bool ModbusHandler::writeData(uint16_t address, const String& data) {
    // 将字符串数据写入保持寄存器
    uint16_t length = min(data.length(), static_cast<size_t>(100));
    
    for (uint16_t i = 0; i < length; i++) {
        holdingRegisters[address + i] = data[i];
    }
    
    Serial.printf("Modbus Handler: Written %d chars to address %d\n", length, address);
    return true;
}

bool ModbusHandler::writeData(const String& address, const String& data) {
    // 支持字符串地址（如"40001"）
    uint16_t addr = address.toInt();
    return writeData(addr, data);
}

// Modbus功能码实现
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

void ModbusHandler::processModbusFrame(const uint8_t* frame, uint8_t length) {
    uint8_t slaveAddress = frame[0];
    uint8_t functionCode = frame[1];
    
    // 检查是否是发给我们的
    if (slaveAddress != config.slaveAddress && slaveAddress != 0) {
        return; // 不是发给我们的广播帧或其它地址
    }
    
    Serial.printf("Modbus Handler: Received frame - Slave: %d, Function: 0x%02X\n", 
                  slaveAddress, functionCode);
    
    // 处理不同的功能码
    switch (functionCode) {
        case 0x01: // Read Coils
        case 0x02: // Read Discrete Inputs
        case 0x03: // Read Holding Registers
        case 0x04: // Read Input Registers
        case 0x05: // Write Single Coil
        case 0x06: // Write Single Register
        case 0x0F: // Write Multiple Coils
        case 0x10: // Write Multiple Registers
            // 这里可以实现具体的请求处理
            break;
            
        default:
            Serial.printf("Modbus Handler: Unsupported function code 0x%02X\n", functionCode);
            break;
    }
    
    if (dataCallback) {
        String dataStr = "Function: 0x" + String(functionCode, HEX);
        dataCallback(slaveAddress, dataStr);
    }
}

void ModbusHandler::sendResponse(const uint8_t* data, uint8_t length) {
    setTransmitMode(true);
    modbusSerial->write(data, length);
    modbusSerial->flush();
    setTransmitMode(false);
}

void ModbusHandler::setDataCallback(std::function<void(uint8_t, const String&)> callback) {
    this->dataCallback = callback;
}

void ModbusHandler::setRegisterReadCallback(std::function<uint16_t(uint16_t)> callback) {
    this->registerReadCallback = callback;
}

void ModbusHandler::setRegisterWriteCallback(std::function<void(uint16_t, uint16_t)> callback) {
    this->registerWriteCallback = callback;
}