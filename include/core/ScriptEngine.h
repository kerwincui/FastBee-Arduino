#ifndef SCRIPT_ENGINE_H
#define SCRIPT_ENGINE_H

#include "core/FeatureFlags.h"
#include "core/UartEventOrigin.h"
#include "core/NativeOutputControl.h"
#include "core/ScriptMqttPublish.h"
#if FASTBEE_ENABLE_COMMAND_SCRIPT

#include <Arduino.h>
#include <vector>

// Synchronous callback only; context must outlive execute(). It acquires a
// short client lease per MQTT command instead of pinning one across delays.
struct ScriptMqttPublisher {
    void* context = nullptr;
    FastBee::ScriptPublishResult (*publish)(void*, size_t, const String&) = nullptr;
};

// 脚本命令类型
enum class ScriptCmdType : uint8_t {
    CMD_GPIO   = 0,   // GPIO <pin> HIGH/LOW
    CMD_DELAY  = 1,   // DELAY <ms>
    CMD_PWM    = 2,   // PWM <pin> <duty>
    CMD_DAC    = 3,   // DAC <pin> <value>
    CMD_LOG    = 4,   // LOG <message...>
    CMD_PERIPH = 5,   // PERIPH <id> HIGH/LOW/PWM/BLINK/BREATHE/STOP [param]
    CMD_MQTT   = 6    // MQTT <topicIndex> <message...> (支持 RANDOM/RANDOMF 表达式)
};

// 单条脚本命令
struct ScriptCommand {
    ScriptCmdType type;
    uint8_t pin;           // GPIO/PWM/DAC 引脚号
    int32_t intParam;      // delay ms / duty / dac value / sub-action param
    String strParam;       // 外设ID 或 LOG 消息
    String subAction;      // PERIPH 子动作: HIGH/LOW/PWM/BLINK/BREATHE/STOP/DISPLAY/TEXT/CLEAR/BRIGHTNESS
    String extraParam;     // PERIPH DISPLAY/TEXT 的显示内容（支持 ${periphId.field} 模板）
};

/**
 * 轻量级命令序列脚本引擎
 * 
 * 支持命令:
 *   GPIO <pin> HIGH|LOW       - 设置引脚电平
 *   DELAY <ms>                - 延时(最大10秒/条, 总计30秒)
 *   PWM <pin> <duty>          - 设置PWM占空比
 *   DAC <pin> <value>         - 设置DAC输出(0-255)
 *   LOG <message>             - 输出日志
 *   PERIPH <id> <action> [p]  - 通过外设ID控制
 *                               HIGH/LOW/PWM/BLINK/BREATHE/STOP
 *                               DISPLAY <value>   - TM1637 数码管显示数字（支持 ${id.field} 模板）
 *                               TEXT <value>      - TM1637 数码管显示文本
 *                               CLEAR             - TM1637 数码管清屏
 *                               BRIGHTNESS <0-7>  - 数码管亮度
 *   MQTT <idx> <message>      - 发布MQTT消息(支持RANDOM/RANDOMF表达式)
 * 
 * 随机数表达式(可用于 MQTT 消息和 LOG 消息):
 *   RANDOM(min,max)            - 生成 [min,max] 范围的随机整数
 *   RANDOMF(min,max,decimals)  - 生成 [min,max] 范围的随机浮点数
 * 
 * 注释行以 # 开头, 空行自动跳过
 */
class ScriptEngine {
public:
    static constexpr uint16_t MAX_SCRIPT_SIZE      = 1024;
    static constexpr uint16_t MAX_EXPANDED_MESSAGE_SIZE = 1024;
    static constexpr uint8_t  MAX_COMMANDS          = 50;
    static constexpr uint32_t MAX_TOTAL_DELAY_MS    = 30000;
    static constexpr uint32_t MAX_SINGLE_DELAY_MS   = 10000;
    static constexpr uint32_t MAX_EXECUTION_TIME_MS = 35000;

    // 解析脚本文本为命令列表, 失败返回空列表
    static std::vector<ScriptCommand> parse(const String& script);

    // 验证命令列表合法性
    static bool validate(const std::vector<ScriptCommand>& cmds, String& errorMsg);

    // MQTT不可用或发布未接受时返回false，停止剩余命令；不自动重放。
    static bool execute(const std::vector<ScriptCommand>& cmds, ScriptMqttPublisher mqtt = {},
                        const FastBee::UartEventOrigin& uartOrigin = {},
                        bool* sourceUnconfirmed = nullptr,
                        const FastBee::OrdinaryControlContext* control = nullptr,
                        FastBee::OrdinaryControlResult* controlResult = nullptr);

    // 处理字符串中的 RANDOM(min,max) 和 RANDOMF(min,max,decimals) 表达式
    // 输入输出须为不同对象。输出完整才返回true；非法表达式、超限或分配失败清空输出。
    static bool processRandomExpressions(const String& input, String& output);

private:
    // 解析单行命令
    static bool parseLine(const String& line, ScriptCommand& cmd);

    // 按空格分割为 token 列表
    static std::vector<String> tokenize(const String& line);

    // 安全延时(内部喂 WDT + 超时检测)
    static bool scriptDelay(uint32_t ms, unsigned long scriptStartTime,
                            const FastBee::UartEventOrigin& uartOrigin, bool* sourceUnconfirmed);
};

#endif // FASTBEE_ENABLE_COMMAND_SCRIPT

#endif // SCRIPT_ENGINE_H
