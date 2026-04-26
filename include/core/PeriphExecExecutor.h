#ifndef PERIPH_EXEC_EXECUTOR_H
#define PERIPH_EXEC_EXECUTOR_H

#include <Arduino.h>
#include <vector>
#include <functional>
#include "PeripheralExecution.h"

// 前向声明
class PeriphExecManager;
class MQTTClient;

// ========== Modbus 寄存器缓冲池 — 消除轮询路径动态分配 ==========
static constexpr uint8_t MODBUS_BUFFER_POOL_SIZE = 4;   // 4 个缓冲区（与最大并发轮询任务数一致）
static constexpr uint8_t MODBUS_MAX_REGISTERS = 64;     // 每个缓冲区最大寄存器数

struct ModbusBuffer {
    uint16_t data[MODBUS_MAX_REGISTERS];  // 最大 64 个寄存器（128 字节）
    bool inUse;                            // 是否正在使用

    ModbusBuffer() : inUse(false) {
        memset(data, 0, sizeof(data));
    }

    void release() { inUse = false; }
};

// RAII 缓冲区守卫 — 确保异常路径也能释放缓冲区
class ModbusBufferGuard {
public:
    ModbusBufferGuard(ModbusBuffer* buf) : _buf(buf) {}
    ~ModbusBufferGuard() { if (_buf) _buf->release(); }
    ModbusBufferGuard(const ModbusBufferGuard&) = delete;
    ModbusBufferGuard& operator=(const ModbusBufferGuard&) = delete;
    uint16_t* data() { return _buf ? _buf->data : nullptr; }
    bool valid() const { return _buf != nullptr; }
private:
    ModbusBuffer* _buf;
};

// ========== 动作执行结果回调 ==========
typedef std::function<void(const std::vector<ActionExecResult>&)> ActionResultsCallback;

// ========== 执行引擎类 ==========
// 负责所有动作的执行，包括GPIO、Modbus、系统动作、脚本等
class PeriphExecExecutor {
public:
    PeriphExecExecutor();
    ~PeriphExecExecutor();

    // 禁止拷贝
    PeriphExecExecutor(const PeriphExecExecutor&) = delete;
    PeriphExecExecutor& operator=(const PeriphExecExecutor&) = delete;

    // 初始化
    void initialize(PeriphExecManager* manager);

    // ========== 动作执行接口 ==========

    // 执行单个动作项
    bool executeActionItem(const ExecAction& action, const String& effectiveValue);

    // 执行规则的所有动作（同步）
    std::vector<ActionExecResult> executeAllActions(const PeriphExecRule& rule, const String& receivedValue, bool suppressReport = false);

    // ========== 具体动作执行方法 ==========

    // 执行外设动作（GPIO、PWM、DAC、Modbus子设备等）
    bool executePeripheralAction(const ExecAction& action, const String& effectiveValue);

    // 执行Modbus轮询动作
    bool executeModbusPollAction(const ExecAction& action, const PeriphExecRule& rule,
                                 std::vector<ActionExecResult>* controlResults = nullptr);

    // 执行传感器读取动作
    bool executeSensorReadAction(const ExecAction& action, ActionExecResult& result);

    // 执行系统动作（重启、恢复出厂等）
    bool executeSystemAction(const ExecAction& action);

    // 执行脚本动作
    bool executeScriptAction(const ExecAction& action);

    // 执行调用其他外设动作
    bool executeCallPeripheralAction(const ExecAction& action, const String& effectiveValue);

    // ========== 回调设置 ==========

    // 设置动作执行结果上报回调
    void setReportCallback(ActionResultsCallback callback) { _reportCallback = callback; }

    // 设置Modbus读取回调
    void setModbusReadCallback(std::function<String(const String&)> callback) { _modbusReadCallback = callback; }

    // ========== 工具方法 ==========

    // 获取MQTTClient指针
    MQTTClient* getMqttClient();

    // 上报动作执行结果
    void reportActionResults(const std::vector<ActionExecResult>& results);

    // ========== Modbus 缓冲池管理 ==========
    ModbusBuffer* acquireBuffer();
    void releaseBuffer(ModbusBuffer* buf);

private:
    PeriphExecManager* _manager = nullptr;
    ActionResultsCallback _reportCallback = nullptr;
    std::function<String(const String&)> _modbusReadCallback = nullptr;

    // Modbus 寄存器缓冲池（4 x 128B = 512B 静态内存）
    ModbusBuffer _bufferPool[MODBUS_BUFFER_POOL_SIZE];
};

#endif // PERIPH_EXEC_EXECUTOR_H
