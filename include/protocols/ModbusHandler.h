#ifndef MODBUS_HANDLER_H
#define MODBUS_HANDLER_H

#include <Arduino.h>
#include <HardwareSerial.h>
#include <functional>
#include "utils/FileUtils.h"
#include <core/ConfigDefines.h>

// Modbus配置结构体
struct ModbusConfig {
    uint8_t slaveAddress;
    uint32_t baudRate;
    uint8_t txPin;
    uint8_t rxPin;
    uint8_t dePin;  // 方向控制引脚（RS485）
    uint16_t responseTimeout;
    uint16_t interFrameDelay;
    String configFile;
    
    // 默认构造函数
    ModbusConfig() 
        : slaveAddress(1), 
          baudRate(9600), 
          txPin(17), 
          rxPin(16), 
          dePin(255),  // 255表示不使用方向控制
          responseTimeout(1000),
          interFrameDelay(5) {}
};

class ModbusHandler {
public:
    ModbusHandler();
    ~ModbusHandler();

    // 初始化方法
    bool begin(const ModbusConfig& config);
    bool begin(const String& configPath = CONFIG_FILE_MODBUS);
    void end();
    
    // 运行时处理
    void handle();
    String getStatus() const;
    
    // 数据操作
    bool writeData(uint16_t address, const String& data);
    bool writeData(const String& address, const String& data);
    
    // Modbus功能码实现
    bool readCoils(uint16_t startAddr, uint16_t quantity, uint8_t* data);
    bool readDiscreteInputs(uint16_t startAddr, uint16_t quantity, uint8_t* data);
    bool readHoldingRegisters(uint16_t startAddr, uint16_t quantity, uint16_t* data);
    bool readInputRegisters(uint16_t startAddr, uint16_t quantity, uint16_t* data);
    bool writeSingleCoil(uint16_t addr, bool value);
    bool writeSingleRegister(uint16_t addr, uint16_t value);
    bool writeMultipleCoils(uint16_t startAddr, uint16_t quantity, uint8_t* data);
    bool writeMultipleRegisters(uint16_t startAddr, uint16_t quantity, uint16_t* data);
    
    // 回调设置
    void setDataCallback(std::function<void(uint8_t, const String&)> callback);
    void setRegisterReadCallback(std::function<uint16_t(uint16_t)> callback);
    void setRegisterWriteCallback(std::function<void(uint16_t, uint16_t)> callback);
    
    // 配置管理
    bool loadConfigFromFile(const String& configPath = "");
    bool saveConfigToFile(const String& configPath = "");
    ModbusConfig getConfig() const { return config; }

private:
    bool isInitialized;
    HardwareSerial* modbusSerial;
    ModbusConfig config;
    
    // 回调函数
    std::function<void(uint8_t, const String&)> dataCallback;
    std::function<uint16_t(uint16_t)> registerReadCallback;
    std::function<void(uint16_t, uint16_t)> registerWriteCallback;
    
    // 内部方法
    bool initializeSerial();
    bool initializePins();
    uint16_t calculateCRC(const uint8_t* data, uint8_t length);
    bool validateFrame(const uint8_t* frame, uint8_t length);
    void processModbusFrame(const uint8_t* frame, uint8_t length);
    void sendResponse(const uint8_t* data, uint8_t length);
    void setTransmitMode(bool transmit);
    
    // 寄存器模拟存储（实际应用中可能连接到真实设备）
    uint16_t holdingRegisters[100];
    uint16_t inputRegisters[100];
    uint8_t coils[20];
    uint8_t discreteInputs[20];
};

#endif