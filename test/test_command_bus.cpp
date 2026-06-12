/**
 * @file test_command_bus.cpp
 * @brief CommandBus 命令总线单元测试
 * 
 * 测试内容：
 * - 命令注册/注销
 * - 命令分派和执行
 * - 命令行解析
 * - 边界条件（满槽、空名、重复注册）
 * - listCommands 回调遍历
 */

#include <unity.h>
#include <Arduino.h>
#include "core/CommandBus.h"

void test_command_bus_group();

// ========== 辅助：重置 CommandBus 状态 ==========
static void resetCommandBus() {
    auto& bus = CommandBus::getInstance();
    // 注销所有命令以恢复干净状态
    const char* names[CommandBus::MAX_COMMANDS];
    size_t count = 0;
    bus.listCommands([&](const char* name, const char*) {
        if (count < CommandBus::MAX_COMMANDS) {
            names[count++] = name;
        }
    });
    for (size_t i = 0; i < count; i++) {
        bus.unregisterCommand(names[i]);
    }
}

// ========== 测试用例 ==========

static void test_register_and_dispatch() {
    resetCommandBus();
    auto& bus = CommandBus::getInstance();
    
    bool called = false;
    String receivedArgs;
    CommandSource receivedSource = CommandSource::CMD_INTERNAL;
    
    bool ok = bus.registerCommand("test_cmd", [&](const String& args, CommandSource src) {
        called = true;
        receivedArgs = args;
        receivedSource = src;
        return CommandResult::ok("done");
    }, "Test command");
    
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(bus.hasCommand("test_cmd"));
    TEST_ASSERT_EQUAL(1, bus.getCommandCount());
    
    auto result = bus.dispatch("test_cmd", "arg1,arg2", CommandSource::CMD_HTTP);
    TEST_ASSERT_TRUE(called);
    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_EQUAL_STRING("done", result.message.c_str());
    TEST_ASSERT_EQUAL_STRING("arg1,arg2", receivedArgs.c_str());
    TEST_ASSERT_EQUAL((int)CommandSource::CMD_HTTP, (int)receivedSource);
}

static void test_unregister_command() {
    resetCommandBus();
    auto& bus = CommandBus::getInstance();
    
    bus.registerCommand("removable", [](const String&, CommandSource) {
        return CommandResult::ok();
    });
    
    TEST_ASSERT_TRUE(bus.hasCommand("removable"));
    TEST_ASSERT_TRUE(bus.unregisterCommand("removable"));
    TEST_ASSERT_FALSE(bus.hasCommand("removable"));
    TEST_ASSERT_EQUAL(0, bus.getCommandCount());
}

static void test_dispatch_unknown_command() {
    resetCommandBus();
    auto& bus = CommandBus::getInstance();
    
    auto result = bus.dispatch("nonexistent", "", CommandSource::CMD_SERIAL);
    TEST_ASSERT_FALSE(result.success);
    TEST_ASSERT_TRUE(result.message.indexOf("Unknown") >= 0);
}

static void test_execute_with_args() {
    resetCommandBus();
    auto& bus = CommandBus::getInstance();
    
    String capturedArgs;
    bus.registerCommand("echo", [&](const String& args, CommandSource) {
        capturedArgs = args;
        return CommandResult::ok("echoed", args);
    });
    
    auto result = bus.execute("echo hello world", CommandSource::CMD_MQTT);
    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_EQUAL_STRING("hello world", capturedArgs.c_str());
    TEST_ASSERT_EQUAL_STRING("hello world", result.data.c_str());
}

static void test_execute_no_args() {
    resetCommandBus();
    auto& bus = CommandBus::getInstance();
    
    bus.registerCommand("noop", [](const String& args, CommandSource) {
        return CommandResult::ok("noop_ok");
    });
    
    auto result = bus.execute("noop");
    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_EQUAL_STRING("noop_ok", result.message.c_str());
}

static void test_register_null_handler() {
    resetCommandBus();
    auto& bus = CommandBus::getInstance();
    
    bool ok = bus.registerCommand("bad", nullptr, "");
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(0, bus.getCommandCount());
}

static void test_register_null_name() {
    resetCommandBus();
    auto& bus = CommandBus::getInstance();
    
    bool ok = bus.registerCommand(nullptr, [](const String&, CommandSource) {
        return CommandResult::ok();
    });
    TEST_ASSERT_FALSE(ok);
}

static void test_register_overwrite() {
    resetCommandBus();
    auto& bus = CommandBus::getInstance();
    
    int callCount = 0;
    bus.registerCommand("ow", [&](const String&, CommandSource) {
        callCount = 1;
        return CommandResult::ok();
    });
    
    // 覆盖注册
    bus.registerCommand("ow", [&](const String&, CommandSource) {
        callCount = 2;
        return CommandResult::ok();
    });
    
    TEST_ASSERT_EQUAL(1, bus.getCommandCount());  // 不增加计数
    bus.dispatch("ow", "", CommandSource::CMD_INTERNAL);
    TEST_ASSERT_EQUAL(2, callCount);  // 调用新handler
}

static void test_register_full_slots() {
    resetCommandBus();
    auto& bus = CommandBus::getInstance();
    
    // 用静态字符串数组避免悬空指针
    static char names[32][16];
    for (size_t i = 0; i < CommandBus::MAX_COMMANDS; i++) {
        snprintf(names[i], sizeof(names[i]), "cmd_%zu", i);
        bool ok = bus.registerCommand(names[i], [](const String&, CommandSource) {
            return CommandResult::ok();
        });
        TEST_ASSERT_TRUE(ok);
    }
    
    TEST_ASSERT_EQUAL(CommandBus::MAX_COMMANDS, bus.getCommandCount());
    
    // 第33个应该失败
    bool ok = bus.registerCommand("overflow", [](const String&, CommandSource) {
        return CommandResult::ok();
    });
    TEST_ASSERT_FALSE(ok);
}

static void test_list_commands() {
    resetCommandBus();
    auto& bus = CommandBus::getInstance();
    
    bus.registerCommand("alpha", [](const String&, CommandSource) { return CommandResult::ok(); }, "Alpha desc");
    bus.registerCommand("beta", [](const String&, CommandSource) { return CommandResult::ok(); }, "Beta desc");
    
    int count = 0;
    bus.listCommands([&](const char* name, const char* desc) {
        count++;
        if (count == 1) {
            TEST_ASSERT_EQUAL_STRING("alpha", name);
            TEST_ASSERT_EQUAL_STRING("Alpha desc", desc);
        }
    });
    TEST_ASSERT_EQUAL(2, count);
}

static void test_command_result_factories() {
    auto ok = CommandResult::ok("success");
    TEST_ASSERT_TRUE(ok.success);
    TEST_ASSERT_EQUAL_STRING("success", ok.message.c_str());
    TEST_ASSERT_TRUE(ok.data.isEmpty());
    
    auto okData = CommandResult::ok("msg", "{\"key\":1}");
    TEST_ASSERT_TRUE(okData.success);
    TEST_ASSERT_EQUAL_STRING("{\"key\":1}", okData.data.c_str());
    
    auto fail = CommandResult::fail("error reason");
    TEST_ASSERT_FALSE(fail.success);
    TEST_ASSERT_EQUAL_STRING("error reason", fail.message.c_str());
}

// ========== 测试组入口 ==========

void test_command_bus_group() {
    RUN_TEST(test_register_and_dispatch);
    RUN_TEST(test_unregister_command);
    RUN_TEST(test_dispatch_unknown_command);
    RUN_TEST(test_execute_with_args);
    RUN_TEST(test_execute_no_args);
    RUN_TEST(test_register_null_handler);
    RUN_TEST(test_register_null_name);
    RUN_TEST(test_register_overwrite);
    RUN_TEST(test_register_full_slots);
    RUN_TEST(test_list_commands);
    RUN_TEST(test_command_result_factories);
}
