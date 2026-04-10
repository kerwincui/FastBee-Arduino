#ifndef MODBUS_HANDLER_H
#define MODBUS_HANDLER_H

#include <Arduino.h>
#include <HardwareSerial.h>
#include <functional>
#include <freertos/semphr.h>
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

// 轮询任务定义（由 PeriphExec POLL_TRIGGER 调度执行）
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

// 线圈延时任务（通用：到期后将指定线圈写为目标值）
struct CoilDelayTask {
    uint8_t  slaveAddress;
    uint16_t coilAddress;
    unsigned long triggerTime;  // millis() 到期时间
    bool active;
    bool coilValue;             // 到期后写入的值（NO模式=false断开, NC模式=true断开）

    CoilDelayTask() : slaveAddress(0), coilAddress(0), triggerTime(0), active(false), coilValue(false) {}
};

// Modbus 子设备（控制类设备：继电器/PWM/PID）
struct ModbusSubDevice {
    char     name[Protocols::MODBUS_DEVICE_NAME_MAX_LEN];
    char     deviceType[Protocols::MODBUS_DEVICE_TYPE_MAX_LEN]; // "relay","pwm","pid"
    uint8_t  slaveAddress;      // 从站地址 1-247
    uint8_t  channelCount;      // 通道数
    uint16_t coilBase;          // 线圈/寄存器基地址
    bool     ncMode;            // NC 常闭模式
    uint8_t  controlProtocol;   // 0=线圈(FC01/FC05), 1=寄存器(FC03/FC06)
    // PWM 扩展
    uint16_t pwmRegBase;        // PWM 寄存器基地址
    uint8_t  pwmResolution;     // PWM 分辨率(bits)
    // PID 扩展
    uint16_t pidAddrs[6];       // PID 寄存器地址 [PV,SV,OUT,P,I,D]
    uint8_t  pidDecimals;       // PID 小数位

    ModbusSubDevice()
        : slaveAddress(1), channelCount(2), coilBase(0),
          ncMode(false), controlProtocol(0),
          pwmRegBase(0), pwmResolution(8), pidDecimals(1) {
        memset(name, 0, sizeof(name));
        strncpy(name, "Device", sizeof(name) - 1);
        memset(deviceType, 0, sizeof(deviceType));
        strncpy(deviceType, "relay", sizeof(deviceType) - 1);
        memset(pidAddrs, 0, sizeof(pidAddrs));
    }
};

// Master模式专属配置
struct MasterConfig {
    uint16_t responseTimeout;     // 响应超时（毫秒）
    uint8_t  maxRetries;          // 最大重试次数
    uint16_t interPollDelay;      // 两次请求间最小间隔（毫秒）
    PollTask tasks[Protocols::MODBUS_MAX_POLL_TASKS];
    uint8_t  taskCount;
    ModbusSubDevice devices[Protocols::MODBUS_MAX_SUB_DEVICES];
    uint8_t  deviceCount;

    MasterConfig()
        : responseTimeout(1000), maxRetries(2),
          interPollDelay(100), taskCount(0), deviceCount(0) {}
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
    
    // 轮询任务只读访问（任务由配置文件定义，由 PeriphExec 调度执行）
    uint8_t getPollTaskCount() const { return config.master.taskCount; }
    PollTask getPollTask(uint8_t index) const;
    
    // 子设备管理
    uint8_t getSubDeviceCount() const { return config.master.deviceCount; }
    const ModbusSubDevice& getSubDevice(uint8_t index) const;

    // Master写操作（阻塞式）
    bool masterWriteSingleRegister(uint8_t slaveAddr, uint16_t regAddr, uint16_t value);
    
    // Master一次性读取（阻塞，用于MQTT指令触发的即时采集）
    OneShotResult readRegistersOnce(uint8_t slaveAddress, uint8_t functionCode,
                                    uint16_t startAddress, uint16_t quantity);
    
    // PeriphExec 调度的轮询任务执行（按索引读取并返回映射后的 JSON）
    String executePollTaskByIndex(uint8_t taskIdx, uint16_t timeout, uint8_t retries);
    
    // ========== 轮询统计接口（用于运行状态显示）==========
    // 单个轮询任务的缓存数据
    struct PollTaskCache {
        uint16_t values[Protocols::MODBUS_MAX_REGISTERS_PER_READ]; // 原始寄存器值
        uint8_t count;               // 有效数据数量
        unsigned long timestamp;     // 采集时间戳
        bool valid;                  // 数据是否有效
        uint8_t lastError;           // 最后一次错误码（0=成功）
        
        PollTaskCache() : count(0), timestamp(0), valid(false), lastError(0) {
            memset(values, 0, sizeof(values));
        }
    };
    // 获取统计数据的JSON字符串
    String getPollStatistics() const;
    // 获取指定任务的缓存数据（用于API返回）
    const PollTaskCache* getTaskCache(uint8_t taskIdx) const;
    // 重置统计数据
    void resetPollStatistics();
    // 统计数据 getter（用于API直接访问）
    uint32_t getTotalPollCount() const { return _totalPollCount; }
    uint32_t getSuccessPollCount() const { return _successPollCount; }
    uint32_t getFailedPollCount() const { return _failedPollCount; }
    uint32_t getTimeoutPollCount() const { return _timeoutPollCount; }
    
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

    // 线圈延时任务管理（到期后自动将线圈写为目标值，支持 NC 模式反转）
    bool addCoilDelayTask(uint8_t slaveAddr, uint16_t coilAddr, unsigned long delayMs, bool coilValue = false);
    
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
    
    // 一次性操作通用发送/接收辅助（提取自readRegistersOnce的通用逻辑）
    OneShotResult sendOneShotRequest(const uint8_t* request, uint8_t reqLen,
                                      uint8_t expectedSlaveAddr);
    
    // 线圈延时任务处理
    void processCoilDelayTasks();
    
    // 一次性操作互斥标志和信号量
    volatile bool isOneShotReading;
    SemaphoreHandle_t _oneShotSemaphore;
    
    // 线圈延时任务队列
    CoilDelayTask coilDelayTasks[Protocols::MODBUS_MAX_COIL_DELAY_TASKS];

    // 调试帧缓冲区（最近一次 OneShot TX/RX）
    String _lastTxHex;
    String _lastRxHex;
    
    uint32_t _totalPollCount;      // 总轮询次数
    uint32_t _successPollCount;    // 成功次数
    uint32_t _failedPollCount;     // 失败次数（含异常）
    uint32_t _timeoutPollCount;    // 超时次数
    unsigned long _lastPollTime;   // 最后轮询时间（millis）
    
    PollTaskCache _taskCache[Protocols::MODBUS_MAX_POLL_TASKS];
    
    // 寄存器模拟存储（从站模式使用）
    uint16_t holdingRegisters[100];
    uint16_t inputRegisters[100];
    uint8_t coils[20];
    uint8_t discreteInputs[20];
};

#endif
