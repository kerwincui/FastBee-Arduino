#ifndef PERIPHERAL_MANAGER_H
#define PERIPHERAL_MANAGER_H

#include <Arduino.h>
#include <map>
#include <vector>
#include <functional>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "PeripheralTypes.h"
#include "PeripheralConfig.h"

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
    
    // 从旧版gpio.json迁移配置
    bool migrateFromGPIOConfig();
    
    // ========== 系统状态 ==========
    
    // 打印所有外设状态（调试用）
    void printStatus() const;
    
    // 获取已配置外设数量
    size_t getPeripheralCount() const { return peripherals.size(); }
    
    // 检查引脚冲突
    bool checkPinConflict(uint8_t pin, const String& excludeId = "") const;
    std::vector<String> getConflictingPeripherals(uint8_t pin) const;
    
    // 定期维护（在主循环中调用）
    void performMaintenance();

private:
    PeripheralManager() = default;
    
    // 外设存储
    std::map<String, PeripheralConfig> peripherals;
    std::map<String, PeripheralRuntimeState> runtimeStates;
    std::map<uint8_t, String> pinToPeripheral;  // 引脚到外设ID的映射
    
    // 内部方法
    bool validateConfig(const PeripheralConfig& config, String& errorMsg);
    bool setupHardware(const PeripheralConfig& config);
    bool teardownHardware(const PeripheralConfig& config);
    
    // GPIO硬件设置
    bool setupGPIOPin(const PeripheralConfig& config);
    bool setupPWMPin(const PeripheralConfig& config);
    
    // 生成唯一ID
    String generateUniqueId(PeripheralType type);
    
    // 更新引脚映射
    void updatePinMapping(const String& id, const PeripheralConfig& config);
    void removePinMapping(const String& id);
    
    // 中断处理
    static void IRAM_ATTR isrHandler(void* arg);
    void handleInterrupt(uint8_t pin);
    
    // 检查引脚有效性
    bool isValidPin(uint8_t pin) const;
    bool isValidPinForType(uint8_t pin, PeripheralType type) const;
};

#endif // PERIPHERAL_MANAGER_H
