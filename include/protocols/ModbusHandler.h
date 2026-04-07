#ifndef MODBUS_HANDLER_H
#define MODBUS_HANDLER_H

#include <Arduino.h>
#include <HardwareSerial.h>
#include <functional>
#include "utils/FileUtils.h"
#include <core/SystemConstants.h>
#include "systems/LoggerSystem.h"

// Modbus异常码
enum ModbusException : uint8_t {
    MODBUS_EX_ILLEGAL_FUNCTION      = 0x01,
    MODBUS_EX_ILLEGAL_DATA_ADDRESS  = 0x02,
    MODBUS_EX_ILLEGAL_DATA_VALUE    = 0x03,
    MODBUS_EX_SLAVE_DEVICE_FAILURE  = 0x04
};

// 一次性读取错误码
enum OneShotError : uint8_t {
    ONESHOT_SUCCESS          = 0,   // 读取成功
    ONESHOT_TIMEOUT          = 1,   // 从站无响应（超时）
    ONESHOT_CRC_ERROR        = 2,   // CRC校验失败
    ONESHOT_EXCEPTION        = 3,   // 从站返回异常响应
    ONESHOT_NOT_INITIALIZED  = 4,   // Modbus未初始化
    ONESHOT_BUSY             = 5    // 正在执行另一次读取
};

// 一次性读取结果
struct OneShotResult {
    OneShotError error;
    uint16_t data[Protocols::MODBUS_MAX_REGISTERS_PER_READ]; // 原始寄存器数据
    uint16_t count;          // 实际读取的寄存器数量
    uint8_t exceptionCode;   // error==ONESHOT_EXCEPTION时有效

    OneShotResult() : error(ONESHOT_NOT_INITIALIZED), count(0), exceptionCode(0) {
        memset(data, 0, sizeof(data));
    }
};

// Modbus工作模式
enum ModbusMode : uint8_t {
    MODBUS_SLAVE  = 0,   // 从站模式（被动响应）
    MODBUS_MASTER = 1    // 主站模式（主动轮询）
};

// Master轮询状态机
enum MasterPollState : uint8_t {
    POLL_IDLE     = 0,   // 空闲，等待下一轮询时机
    POLL_SENDING  = 1,   // 正在发送请求帧
    POLL_WAITING  = 2,   // 等待从站响应
    POLL_COMPLETE = 3,   // 响应接收完成
    POLL_ERROR    = 4    // 通信错误
};

// 寄存器映射定义（将原始寄存器值转换为传感器数据）
struct RegisterMapping {
    uint8_t  regOffset;      // 寄存器偏移量（相对于PollTask.startAddress）
    uint8_t  dataType;       // 数据类型: 0=uint16, 1=int16, 2=uint32, 3=int32, 4=float32
    float    scaleFactor;    // 缩放因子（原始值 * scaleFactor = 最终值）
    uint8_t  decimalPlaces;  // 小数位数（JSON格式化用）
    char     sensorId[Protocols::MODBUS_SENSOR_ID_MAX_LEN]; // 传感器标识符

    RegisterMapping()
        : regOffset(0), dataType(0), scaleFactor(1.0f), decimalPlaces(1) {
        memset(sensorId, 0, sizeof(sensorId));
    }
};

// 轮询任务定义
struct PollTask {
    uint8_t  slaveAddress;   // 目标从站地址 (1-247)
    uint8_t  functionCode;   // 功能码 (0x01-0x04)
    uint16_t startAddress;   // 起始寄存器地址
    uint16_t quantity;       // 寄存器/线圈数量
    uint16_t pollInterval;   // 此任务轮询间隔（秒）
    bool     enabled;        // 是否启用
    char     label[Protocols::MODBUS_POLL_LABEL_MAX_LEN]; // 可读标签
    
    // 寄存器映射配置（JSON模式使用）
    RegisterMapping mappings[Protocols::MODBUS_MAX_MAPPINGS_PER_TASK];
    uint8_t mappingCount;    // 实际映射数量

    PollTask()
        : slaveAddress(1), functionCode(0x03), startAddress(0),
          quantity(10), pollInterval(Protocols::MODBUS_DEFAULT_POLL_INTERVAL),
          enabled(true), mappingCount(0) {
        memset(label, 0, sizeof(label));
    }
};

// Master写请求
struct WriteRequest {
    uint8_t  slaveAddress;
    uint16_t regAddress;
    uint16_t value;
    bool     pending;

    WriteRequest() : slaveAddress(0), regAddress(0), value(0), pending(false) {}
};

// 线圈延时任务（通用：到期后将指定线圈写OFF）
struct CoilDelayTask {
    uint8_t  slaveAddress;
    uint16_t coilAddress;
    unsigned long triggerTime;  // millis() 到期时间
    bool active;

    CoilDelayTask() : slaveAddress(0), coilAddress(0), triggerTime(0), active(false) {}
};

// Master模式专属配置
struct MasterConfig {
    uint16_t responseTimeout;     // 响应超时（毫秒）
    uint8_t  maxRetries;          // 最大重试次数
    uint16_t interPollDelay;      // 两次请求间最小间隔（毫秒）
    PollTask tasks[Protocols::MODBUS_MAX_POLL_TASKS];
    uint8_t  taskCount;

    MasterConfig()
        : responseTimeout(1000), maxRetries(2),
          interPollDelay(100), taskCount(0) {}
};

// Master运行统计
struct MasterStats {
    uint32_t totalPolls;
    uint32_t successPolls;
    uint32_t failedPolls;
    uint32_t timeoutPolls;

    MasterStats() : totalPolls(0), successPolls(0), failedPolls(0), timeoutPolls(0) {}
};

// 单个轮询任务的最新数据缓存
struct TaskDataCache {
    uint16_t values[Protocols::MODBUS_MAX_REGISTERS_PER_READ];
    uint16_t count;          // 实际缓存的寄存器数量
    unsigned long timestamp; // millis() 采集时间
    bool valid;              // 是否有有效数据

    TaskDataCache() : count(0), timestamp(0), valid(false) {
        memset(values, 0, sizeof(values));
    }
};

// Modbus配置结构体
struct ModbusConfig {
    ModbusMode mode;          // 工作模式
    uint8_t slaveAddress;
    uint32_t baudRate;
    uint8_t txPin;
    uint8_t rxPin;
    uint8_t dePin;            // 方向控制引脚（RS485）
    uint16_t responseTimeout;
    uint16_t interFrameDelay;
    String configFile;
    uint8_t transferType;     // 传输类型: 0=JSON, 1=透传(RAW HEX)
    uint8_t workMode;         // 工作模式: 0=MQTT指令模式, 1=主动轮询模式
    MasterConfig master;      // Master模式配置
    
    // 默认构造函数
    ModbusConfig() 
        : mode(MODBUS_SLAVE),
          slaveAddress(1), 
          baudRate(9600), 
          txPin(17), 
          rxPin(16), 
          dePin(255),  // 255表示不使用方向控制
          responseTimeout(1000),
          interFrameDelay(5),
          transferType(0),
          workMode(1) {}  // 默认主动轮询模式
};

class ModbusHandler {
public:
    ModbusHandler();
    ~ModbusHandler();

    // 初始化方法
    bool begin(const ModbusConfig& config);
    bool begin(const String& configPath = FileSystem::PROTOCOL_CONFIG_FILE);
    void end();
    
    // 运行时处理
    void handle();
    String getStatus() const;
    
    // 数据操作
    bool writeData(uint16_t address, const String& data);
    bool writeData(const String& address, const String& data);
    
    // Modbus功能码实现（从站模式）
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

    // === Master模式公有接口 ===
    void setMode(ModbusMode mode);
    ModbusMode getMode() const { return config.mode; }
    uint8_t getWorkMode() const { return config.workMode; }
    
    // 轮询任务管理
    bool addPollTask(const PollTask& task);
    bool removePollTask(uint8_t index);
    bool updatePollTask(uint8_t index, const PollTask& task);
    uint8_t getPollTaskCount() const { return config.master.taskCount; }
    PollTask getPollTask(uint8_t index) const;
    
    // Master写操作（排队异步执行）
    bool masterWriteSingleRegister(uint8_t slaveAddr, uint16_t regAddr, uint16_t value);
    
    // Master一次性读取（阻塞，用于MQTT指令触发的即时采集）
    OneShotResult readRegistersOnce(uint8_t slaveAddress, uint8_t functionCode,
                                    uint16_t startAddress, uint16_t quantity);
    
    // Master一次性阻塞写操作（与 readRegistersOnce 对称，通用Modbus写能力）
    OneShotResult writeCoilOnce(uint8_t slaveAddr, uint16_t coilAddr, bool value);
    OneShotResult writeCoilOnce(uint8_t slaveAddr, uint16_t coilAddr, uint16_t rawValue);
    OneShotResult writeMultipleCoilsOnce(uint8_t slaveAddr, uint16_t startAddr,
                                          uint16_t quantity, const bool* values);
    OneShotResult writeRegisterOnce(uint8_t slaveAddr, uint16_t regAddr, uint16_t value);
    OneShotResult writeMultipleRegistersOnce(uint8_t slaveAddr, uint16_t startAddr,
                                              uint16_t quantity, const uint16_t* values);

    // 发送原始 Modbus 帧（自动追加 CRC，用于设备专有功能码如 0xB0）
    OneShotResult sendRawFrameOnce(uint8_t expectedSlaveAddr,
                                    const uint8_t* dataWithoutCRC, uint8_t dataLen);

    // 调试帧信息（最近一次 OneShot 操作的 TX/RX hex 字符串）
    const String& getLastTxHex() const { return _lastTxHex; }
    const String& getLastRxHex() const { return _lastRxHex; }

    // 线圈延时任务管理（到期后自动将线圈写OFF，通用定时操作）
    bool addCoilDelayTask(uint8_t slaveAddr, uint16_t coilAddr, unsigned long delayMs);
    
    // Master运行状态
    String getMasterStatus() const;
    MasterStats getMasterStats() const { return masterStats; }
    TaskDataCache getTaskDataCache(uint8_t index) const;
    
    // RAW模式辅助：将寄存器数据重构为Modbus响应帧的十六进制字符串
    String formatRawHex(uint8_t slaveAddr, uint8_t fc, const uint16_t* data, uint16_t count);

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
    
    // Slave模式FC处理函数
    void handleSlave();
    void handleReadCoils(const uint8_t* frame, uint8_t length);
    void handleReadDiscreteInputs(const uint8_t* frame, uint8_t length);
    void handleReadHoldingRegisters(const uint8_t* frame, uint8_t length);
    void handleReadInputRegisters(const uint8_t* frame, uint8_t length);
    void handleWriteSingleCoil(const uint8_t* frame, uint8_t length);
    void handleWriteSingleRegister(const uint8_t* frame, uint8_t length);
    void handleWriteMultipleCoils(const uint8_t* frame, uint8_t length);
    void handleWriteMultipleRegisters(const uint8_t* frame, uint8_t length);
    void sendExceptionResponse(uint8_t slaveAddr, uint8_t functionCode, uint8_t exceptionCode);
    
    // === Master模式私有方法 ===
    void handleMaster();
    uint8_t buildMasterRequest(const PollTask& task, uint8_t* buffer);
    bool parseMasterResponse(const uint8_t* buffer, uint8_t length, const PollTask& task);
    int8_t findNextPollTask();
    void reportPollData(const PollTask& task, const uint16_t* data, uint16_t count);
    void reportRawData(const PollTask& task, const uint8_t* frame, uint8_t frameLen);
    bool processWriteQueue();
    uint8_t buildWriteRequest(const WriteRequest& req, uint8_t* buffer);
    
    // 一次性操作通用发送/接收辅助（提取自readRegistersOnce的通用逻辑）
    OneShotResult sendOneShotRequest(const uint8_t* request, uint8_t reqLen,
                                      uint8_t expectedSlaveAddr);
    
    // 线圈延时任务处理
    void processCoilDelayTasks();
    
    // Master状态机变量
    MasterPollState pollState;
    uint8_t  currentTaskIndex;
    unsigned long pollStateTimestamp;
    unsigned long taskLastPollTime[Protocols::MODBUS_MAX_POLL_TASKS];
    uint8_t  currentRetryCount;
    uint8_t  responseBuffer[Protocols::MODBUS_BUFFER_SIZE];
    uint8_t  responseIndex;
    unsigned long lastByteTime;
    bool     currentIsWrite;   // 当前正在处理的是写请求还是轮询
    volatile bool isOneShotReading; // 一次性读取进行中，暂停轮询状态机
    
    // 写请求队列
    WriteRequest writeQueue[Protocols::MODBUS_MAX_WRITE_QUEUE];
    
    // 线圈延时任务队列
    CoilDelayTask coilDelayTasks[Protocols::MODBUS_MAX_COIL_DELAY_TASKS];
    
    // 运行统计
    MasterStats masterStats;

    // 每个轮询任务的最新数据缓存
    TaskDataCache taskDataCache[Protocols::MODBUS_MAX_POLL_TASKS];

    // 调试帧缓冲区（最近一次 OneShot TX/RX）
    String _lastTxHex;
    String _lastRxHex;
    
    // 寄存器模拟存储（从站模式使用）
    uint16_t holdingRegisters[100];
    uint16_t inputRegisters[100];
    uint8_t coils[20];
    uint8_t discreteInputs[20];
};

#endif
