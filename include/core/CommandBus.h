#ifndef COMMAND_BUS_H
#define COMMAND_BUS_H

#include <Arduino.h>
#include <array>
#include <functional>

/**
 * @brief 命令来源枚举
 */
enum class CommandSource : uint8_t {
    CMD_HTTP = 0,       // Web API 调用
    CMD_MQTT = 1,       // MQTT 消息
    CMD_SERIAL = 2,     // 串口命令
    CMD_RULE = 3,       // 规则引擎触发
    CMD_INTERNAL = 4    // 内部系统调用
};

/**
 * @brief 命令执行结果
 */
struct CommandResult {
    bool success = false;
    String message;
    String data;    // 可选返回数据（JSON 格式）
    
    CommandResult() = default;
    CommandResult(bool s, const String& m, const String& d) : success(s), message(m), data(d) {}
    
    static CommandResult ok(const String& msg = "OK") {
        return CommandResult(true, msg, "");
    }
    static CommandResult ok(const String& msg, const String& data) {
        return CommandResult(true, msg, data);
    }
    static CommandResult fail(const String& msg) {
        return CommandResult(false, msg, "");
    }
};

/**
 * @brief 命令处理函数类型
 * @param args 命令参数（逗号分隔或 JSON 格式）
 * @param source 命令来源
 * @return 执行结果
 */
using CommandHandler = std::function<CommandResult(const String& args, CommandSource source)>;

/**
 * @brief 命令注册条目
 */
struct CommandEntry {
    const char* name = nullptr;     // 命令名（如 "reboot", "gpio", "status"）
    CommandHandler handler;         // 处理函数
    const char* description = nullptr; // 简要描述（用于 help 输出）
    bool active = false;            // 是否已注册
};

/**
 * @brief 统一命令总线（定长数组，零运行时分配）
 * 
 * 设计原则：
 * - 使用固定 32 槽数组，避免 std::map 红黑树节点分配
 * - HTTP/MQTT/规则/串口统一通过 dispatch() 入口执行命令
 * - 支持静态注册（编译期）和动态注册（运行时）
 * 
 * 用法：
 *   CommandBus::getInstance().registerCommand("reboot", handler, "Restart device");
 *   CommandBus::getInstance().dispatch("reboot", "", CommandSource::HTTP);
 */
class CommandBus {
public:
    static constexpr size_t MAX_COMMANDS = 32;

    static CommandBus& getInstance() {
        static CommandBus instance;
        return instance;
    }

    /**
     * @brief 注册命令
     * @param name 命令名（必须是字面常量或生命周期足够长的字符串）
     * @param handler 处理函数
     * @param description 命令描述
     * @return 是否注册成功
     */
    bool registerCommand(const char* name, CommandHandler handler, const char* description = "") {
        if (!name || !handler) return false;
        // 检查重名
        for (size_t i = 0; i < MAX_COMMANDS; i++) {
            if (_commands[i].active && _commands[i].name && strcmp(_commands[i].name, name) == 0) {
                // 允许覆盖注册
                _commands[i].handler = handler;
                _commands[i].description = description;
                return true;
            }
        }
        // 找空槽
        for (size_t i = 0; i < MAX_COMMANDS; i++) {
            if (!_commands[i].active) {
                _commands[i].name = name;
                _commands[i].handler = handler;
                _commands[i].description = description;
                _commands[i].active = true;
                _count++;
                return true;
            }
        }
        return false;  // 槽满
    }

    /**
     * @brief 注销命令
     * @param name 命令名
     * @return 是否注销成功
     */
    bool unregisterCommand(const char* name) {
        for (size_t i = 0; i < MAX_COMMANDS; i++) {
            if (_commands[i].active && _commands[i].name && strcmp(_commands[i].name, name) == 0) {
                _commands[i] = CommandEntry{};
                _count--;
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 分派命令执行
     * @param name 命令名
     * @param args 参数
     * @param source 来源
     * @return 执行结果
     */
    CommandResult dispatch(const String& name, const String& args, CommandSource source = CommandSource::CMD_INTERNAL) {
        for (size_t i = 0; i < MAX_COMMANDS; i++) {
            if (_commands[i].active && _commands[i].name && name.equals(_commands[i].name)) {
                if (_commands[i].handler) {
                    return _commands[i].handler(args, source);
                }
                return CommandResult::fail("Handler is null");
            }
        }
        return CommandResult::fail("Unknown command: " + name);
    }

    /**
     * @brief 解析并执行命令字符串（格式: "command arg1,arg2,..."）
     * @param cmdLine 命令行字符串
     * @param source 来源
     * @return 执行结果
     */
    CommandResult execute(const String& cmdLine, CommandSource source = CommandSource::CMD_INTERNAL) {
        int spaceIdx = cmdLine.indexOf(' ');
        String name, args;
        if (spaceIdx > 0) {
            name = cmdLine.substring(0, spaceIdx);
            args = cmdLine.substring(spaceIdx + 1);
            args.trim();
        } else {
            name = cmdLine;
        }
        name.trim();
        return dispatch(name, args, source);
    }

    /**
     * @brief 检查命令是否已注册
     */
    bool hasCommand(const char* name) const {
        for (size_t i = 0; i < MAX_COMMANDS; i++) {
            if (_commands[i].active && _commands[i].name && strcmp(_commands[i].name, name) == 0) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 获取已注册命令数
     */
    size_t getCommandCount() const { return _count; }

    /**
     * @brief 获取所有命令列表（用于 help 输出）
     * @param callback 回调函数 (name, description)
     */
    void listCommands(std::function<void(const char*, const char*)> callback) const {
        if (!callback) return;
        for (size_t i = 0; i < MAX_COMMANDS; i++) {
            if (_commands[i].active && _commands[i].name) {
                callback(_commands[i].name, _commands[i].description ? _commands[i].description : "");
            }
        }
    }

private:
    CommandBus() = default;
    std::array<CommandEntry, MAX_COMMANDS> _commands = {};
    size_t _count = 0;
};

/**
 * @brief 命令自动注册辅助类
 * 
 * 用法（在 .cpp 文件中）：
 *   static CommandResult handleReboot(const String& args, CommandSource src) { ... }
 *   static CommandAutoRegister _reg("reboot", handleReboot, "Restart device");
 */
struct CommandAutoRegister {
    CommandAutoRegister(const char* name, CommandHandler handler, const char* desc = "") {
        CommandBus::getInstance().registerCommand(name, handler, desc);
    }
};

#endif // COMMAND_BUS_H
