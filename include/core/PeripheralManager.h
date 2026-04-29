#ifndef PERIPHERAL_MANAGER_H
#define PERIPHERAL_MANAGER_H

#include <Arduino.h>
#include <map>
#include <vector>
#include <functional>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include <freertos/semphr.h>
#include <functional>
#include "PeripheralTypes.h"
#include "PeripheralConfig.h"

// Modbus 通信委托回调类型（用于解耦 PeripheralManager 和 ModbusHandler）
using ModbusCoilWriteFunc = std::function<bool(uint8_t slaveAddr, uint16_t coilAddr, bool value)>;
using ModbusRegWriteFunc  = std::function<bool(uint8_t slaveAddr, uint16_t regAddr, uint16_t value)>;

// 外设配置文件路径
#define PERIPHERAL_CONFIG_FILE "/config/peripherals.json"
#define PERIPHERAL_CONFIG_BACKUP "/config/gpio.json.bak"

class PeripheralManager {
public:
    static PeripheralManager& getInstance();
    
    // 禁止拷贝
    PeripheralManager(const PeripheralManager&) = delete;
    PeripheralManager& operator=(const PeripheralManager&) = delete;
    
    // 初始化
    bool initialize();
    
    // ========== 外设管理（增删改查） ==========
    
    // 添加外设
    bool addPeripheral(const PeripheralConfig& config);
    // 添加外设（带错误信息输出，用于 API 返回具体失败原因）
    bool addPeripheral(const PeripheralConfig& config, String& errorMsg);
    
    // 更新外设
    bool updatePeripheral(const String& id, const PeripheralConfig& config);
    
    // 删除外设
    bool removePeripheral(const String& id);
    
    // 获取外设配置
    PeripheralConfig* getPeripheral(const String& id);
    const PeripheralConfig* getPeripheral(const String& id) const;
    
    // 根据类型获取外设列表
    std::vector<PeripheralConfig> getPeripheralsByType(PeripheralType type) const;
    std::vector<PeripheralConfig> getPeripheralsByCategory(PeripheralCategory category) const;
    
    // 获取所有外设
    std::vector<PeripheralConfig> getAllPeripherals() const;
    
    // 遍历所有外设（无拷贝，避免频繁内存分配）
    void forEachPeripheral(std::function<void(const PeripheralConfig&)> callback) const;
    
    // 检查外设是否存在
    bool hasPeripheral(const String& id) const;
    
    // ========== 外设操作 ==========
    
    // 启用/禁用外设
    bool enablePeripheral(const String& id);
    bool disablePeripheral(const String& id);
    bool isPeripheralEnabled(const String& id) const;
    
    // 获取外设状态
    PeripheralStatus getPeripheralStatus(const String& id) const;
    PeripheralRuntimeState* getRuntimeState(const String& id);
    
    // ========== 硬件初始化 ==========
    
    // 初始化外设硬件
    bool initHardware(const String& id);
    bool deinitHardware(const String& id);
    
    // 初始化所有启用的外设
    bool initAllEnabledPeripherals();
    
    // ========== 数据读写 ==========
    
    // 通用数据读写接口
    bool writeData(const String& id, const uint8_t* data, size_t len);
    bool readData(const String& id, uint8_t* buffer, size_t& len);
    
    // 发送字符串（适用于通信接口）
    bool writeString(const String& id, const String& data);
    String readString(const String& id);
    
    // ========== GPIO兼容层（保持向后兼容） ==========
    
    // 配置单个GPIO引脚（内部转换为PeripheralConfig）
    bool configurePin(uint8_t pin, PeripheralType type);
    bool configurePin(const PeripheralConfig& config);
    
    // 读取GPIO状态
    GPIOState readPin(uint8_t pin);
    GPIOState readPin(const String& peripheralId);
    
    // 写入GPIO状态
    bool writePin(uint8_t pin, GPIOState state);
    bool writePin(const String& peripheralId, GPIOState state);
    
    // 切换GPIO状态
    bool togglePin(uint8_t pin);
    bool togglePin(const String& peripheralId);
    
    // PWM操作
    bool writePWM(uint8_t pin, uint32_t dutyCycle);
    bool writePWM(const String& peripheralId, uint32_t dutyCycle);
    
    // 模拟读取
    uint16_t readAnalog(uint8_t pin);
    uint16_t readAnalog(const String& peripheralId);
    
    // 中断管理
    bool attachInterrupt(uint8_t pin, GPIOInterruptCallback callback);
    bool attachInterrupt(const String& peripheralId, GPIOInterruptCallback callback);
    bool detachInterrupt(uint8_t pin);
    bool detachInterrupt(const String& peripheralId);
    
    // GPIO查询
    bool isPinConfigured(uint8_t pin) const;
    PeripheralType getPinType(uint8_t pin) const;
    String getPinPeripheralId(uint8_t pin) const;
    std::vector<uint8_t> getConfiguredPins() const;
    
    // ========== 持久化 ==========
    
    // 保存配置到文件
    bool saveConfiguration();
    
    // 从文件加载配置
    bool loadConfiguration();
    
    // ========== 系统状态 ==========
    
    // 打印所有外设状态（调试用）
    void printStatus() const;
    
    // 获取已配置外设数量
    size_t getPeripheralCount() const { return peripherals.size(); }
    
    // 检查引脚冲突
    bool checkPinConflict(uint8_t pin, const String& excludeId = "") const;
    std::vector<String> getConflictingPeripherals(uint8_t pin) const;
    
    // 引脚验证（增强版）
    String getPinConflictInfo(uint8_t pin, const String& excludeId = "") const;
    bool validatePinForType(uint8_t pin, PeripheralType type, String& errorMsg) const;
    bool isReservedPin(uint8_t pin) const;
    bool isInputOnlyPin(uint8_t pin) const;
    
    // 定期维护（在主循环中调用）
    void performMaintenance();

    // ========== Modbus 外设委托 ==========
    
    // 设置 Modbus 通信回调（由 ProtocolManager 注入，解耦协议层依赖）
    void setModbusCallbacks(ModbusCoilWriteFunc coilWrite, ModbusRegWriteFunc regWrite);
    
    // 清除 Modbus 通信回调（Modbus 停止时调用）
    void clearModbusCallbacks();
    
    // Modbus 线圈写入（指定地址，供高级控制使用）
    bool writeModbusCoil(const String& id, uint16_t coilAddr, bool value);
    
    // Modbus 寄存器写入（指定地址，供高级控制使用）
    bool writeModbusReg(const String& id, uint16_t regAddr, uint16_t value);

    // 动作定时器上下文（公开供静态回调函数访问）
    struct ActionTickerData {
        PeripheralManager* mgr;
        String id;
        Ticker ticker;
        uint8_t channel;
        uint16_t maxDuty;
        int16_t stepSize;
        int16_t breatheState;  // 正值=递增(当前duty), 负值=递减
    };

    // 动作定时器（公开供 PeriphExecManager 调用）
    // actionMode: 1=闪烁, 2=呼吸灯; paramValue: 闪烁间隔ms或呼吸周期ms
    void startActionTicker(const String& id, uint8_t actionMode, uint16_t paramValue);
    void stopActionTicker(const String& id);

    // 线程安全：获取互斥量句柄（供 Ticker 回调非阻塞尝试加锁）
    SemaphoreHandle_t getMutex() const { return _mutex; }

private:
    PeripheralManager() = default;
    
    // 线程安全互斥量（递归，支持嵌套调用）
    SemaphoreHandle_t _mutex = nullptr;

    // 外设存储
    std::map<String, PeripheralConfig> peripherals;
    std::map<String, PeripheralRuntimeState> runtimeStates;
    std::map<uint8_t, String> pinToPeripheral;  // 引脚到外设ID的映射
    
    // 动作定时器
    std::map<String, ActionTickerData*> actionTickers;
    
    // Modbus 通信委托
    ModbusCoilWriteFunc _modbusCoilWrite = nullptr;
    ModbusRegWriteFunc  _modbusRegWrite  = nullptr;
    
    // 内部方法
    bool validateConfig(const PeripheralConfig& config, String& errorMsg);
    bool setupHardware(const PeripheralConfig& config);
    bool teardownHardware(const PeripheralConfig& config);
    
    // GPIO硬件设置
    bool setupGPIOPin(const PeripheralConfig& config);
    bool setupPWMPin(const PeripheralConfig& config);
    
    // Modbus外设写入（内部实现）
    bool writeModbusPin(const String& id, const PeripheralConfig& config, GPIOState state);
    bool writeModbusPWM(const String& id, const PeripheralConfig& config, uint32_t dutyCycle);
    
    // 生成唯一ID
    String generateUniqueId(PeripheralType type);
    
    // 更新引脚映射
    void updatePinMapping(const String& id, const PeripheralConfig& config);
    void removePinMapping(const String& id);
    // 完全从 peripherals 真实数据重建 pinToPeripheral 缓存（消除残留/错乱）
    void rebuildPinMapping();
    
    // 中断处理
    static void IRAM_ATTR isrHandler(void* arg);
    void handleInterrupt(uint8_t pin);
    
    // 检查引脚有效性
    bool isValidPin(uint8_t pin) const;
    bool isValidPinForType(uint8_t pin, PeripheralType type) const;
    
    // DAC硬件初始化
    bool setupDACPin(const PeripheralConfig& config);
};

#endif // PERIPHERAL_MANAGER_H
