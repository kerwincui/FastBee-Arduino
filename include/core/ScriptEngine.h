#ifndef SCRIPT_ENGINE_H
#define SCRIPT_ENGINE_H

#include <Arduino.h>
#include <vector>

// 脚本命令类型
enum class ScriptCmdType : uint8_t {
    CMD_GPIO   = 0,   // GPIO <pin> HIGH/LOW
    CMD_DELAY  = 1,   // DELAY <ms>
    CMD_PWM    = 2,   // PWM <pin> <duty>
    CMD_DAC    = 3,   // DAC <pin> <value>
    CMD_LOG    = 4,   // LOG <message...>
    CMD_PERIPH = 5    // PERIPH <id> HIGH/LOW/PWM/BLINK/BREATHE/STOP [param]
};

// 单条脚本命令
struct ScriptCommand {
    ScriptCmdType type;
    uint8_t pin;           // GPIO/PWM/DAC 引脚号
    int32_t intParam;      // delay ms / duty / dac value / sub-action param
    String strParam;       // 外设ID 或 LOG 消息
    String subAction;      // PERIPH 子动作: HIGH/LOW/PWM/BLINK/BREATHE/STOP
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
 * 
 * 注释行以 # 开头, 空行自动跳过
 */
class ScriptEngine {
public:
    static constexpr uint16_t MAX_SCRIPT_SIZE      = 1024;
    static constexpr uint8_t  MAX_COMMANDS          = 50;
    static constexpr uint32_t MAX_TOTAL_DELAY_MS    = 30000;
    static constexpr uint32_t MAX_SINGLE_DELAY_MS   = 10000;
    static constexpr uint32_t MAX_EXECUTION_TIME_MS = 35000;

    // 解析脚本文本为命令列表, 失败返回空列表
    static std::vector<ScriptCommand> parse(const String& script);

    // 验证命令列表合法性
    static bool validate(const std::vector<ScriptCommand>& cmds, String& errorMsg);

    // 执行命令列表
    static bool execute(const std::vector<ScriptCommand>& cmds);

private:
    // 解析单行命令
    static bool parseLine(const String& line, ScriptCommand& cmd);

    // 按空格分割为 token 列表
    static std::vector<String> tokenize(const String& line);

    // 安全延时(内部喂 WDT + 超时检测)
    static bool scriptDelay(uint32_t ms, unsigned long scriptStartTime);
};

#endif // SCRIPT_ENGINE_H
