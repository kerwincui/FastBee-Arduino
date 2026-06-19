/**
 * @file run_tests.cpp
 * @brief Standalone combined test runner (MQTT + System Stability)
 *
 * 仅在定义 RUN_COMBINED_TESTS 宏时参与编译，
 * 避免与 test_main.cpp 的 setUp/tearDown/main 符号冲突。
 * 独立运行方式：在 platformio.ini 的 native 环境中添加
 *   build_flags = ... -DRUN_COMBINED_TESTS
 */
#if defined(UNIT_TEST) && defined(RUN_COMBINED_TESTS)

#include <unity.h>
#include <cstdio>

extern void test_mqtt_protocol_group();
extern void test_system_stability_group();

extern "C" void setUp() {}
extern "C" void tearDown() {}

int main(int, char**) {
    printf("\n=== Combined Test Suite ===\n\n");
    UNITY_BEGIN();
    test_mqtt_protocol_group();
    test_system_stability_group();
    int failures = UNITY_END();
    printf("\n=== Result: %d failure(s) ===\n", failures);
    return failures;
}

#endif
