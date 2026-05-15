/**
 * @file CommandBus.cpp
 * @brief 内置命令注册（系统核心命令）
 * 
 * 通过 CommandAutoRegister 在静态初始化阶段自动注册，
 * 无需修改 FastBeeFramework 初始化流程。
 */

#include "core/CommandBus.h"
#include "core/FeatureFlags.h"
#include <ESP.h>

// ============ 内置命令 ============

// reboot - 重启设备
static CommandResult cmdReboot(const String& args, CommandSource source) {
    (void)args; (void)source;
    ESP.restart();
    return CommandResult::ok("Rebooting...");
}
static CommandAutoRegister _regReboot("reboot", cmdReboot, "Restart device");

// status - 获取系统状态摘要
static CommandResult cmdStatus(const String& args, CommandSource source) {
    (void)args; (void)source;
    String info;
    info += "heap_free=";
    info += String(ESP.getFreeHeap());
    info += ",heap_max_alloc=";
    info += String(ESP.getMaxAllocHeap());
    info += ",uptime_ms=";
    info += String(millis());
    return CommandResult::ok("OK", info);
}
static CommandAutoRegister _regStatus("status", cmdStatus, "System status summary");

// help - 列出所有已注册命令
static CommandResult cmdHelp(const String& args, CommandSource source) {
    (void)args; (void)source;
    String result = "[";
    bool first = true;
    CommandBus::getInstance().listCommands([&](const char* name, const char* desc) {
        if (!first) result += ",";
        result += "{\"cmd\":\"";
        result += name;
        result += "\",\"desc\":\"";
        result += desc;
        result += "\"}";
        first = false;
    });
    result += "]";
    return CommandResult::ok("OK", result);
}
static CommandAutoRegister _regHelp("help", cmdHelp, "List all commands");

// heap - 详细堆信息
static CommandResult cmdHeap(const String& args, CommandSource source) {
    (void)args; (void)source;
    String info;
    info += "free=";
    info += String(ESP.getFreeHeap());
    info += ",min_free=";
    info += String(ESP.getMinFreeHeap());
    info += ",max_alloc=";
    info += String(ESP.getMaxAllocHeap());
    return CommandResult::ok("OK", info);
}
static CommandAutoRegister _regHeap("heap", cmdHeap, "Heap memory info");
