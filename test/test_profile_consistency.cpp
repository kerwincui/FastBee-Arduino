/**
 * @file test_profile_consistency.cpp
 * @brief 统一架构（Unified Architecture v2.0）配置矩阵一致性守卫
 *
 * 2026-08 环境整合后不再有 lite/standard/full 预设，所有出厂环境引用
 * [unified_flags] 编译全部功能模块，由运行时 FeatureBudget 门控。
 * 本文件防止后续改动无意中破坏整合后的矩阵：
 * 1. unified_flags 完整性守卫（重型功能必须为 1，外部网络适配器按环境选择）
 * 2. 已删除预设段守卫（lite/standard/full_flags 等不得复活）
 * 3. 统一 Web 镜像守卫（出厂一律 full 档，无 profile 区分）
 * 4. env lib_deps/lib_ignore 守卫（统一编译需要全部外设库；S3 排除红外库）
 * 5. OTA 策略守卫（无 PSRAM 的 4MB 环境必须显式关闭 OTA）
 * 6. Web 构建设备大屏门控守卫（保持不变）
 */

#include <unity.h>
#include <Arduino.h>
#include "helpers/TestLogger.h"
#include <string>
#include <fstream>
#include <sstream>

void test_profile_consistency_group();

static std::string readProjectFile(const char* relativePath) {
    const char* roots[] = { ".", "..", "../..", "../../..", "../../../.." };
    for (const char* root : roots) {
        std::string fullPath = std::string(root) + "/" + relativePath;
        std::ifstream file(fullPath);
        if (file.is_open()) {
            std::ostringstream ss;
            ss << file.rdbuf();
            return ss.str();
        }
    }
    return "";
}

// 提取 ini 中 [section] 到下一个 [section] 之间的内容
// 注意：只匹配行首的段头，避免命中注释里的 [unified_flags] 等字样
static std::string extractIniSection(const std::string& ini, const std::string& section) {
    const std::string header = "\n[" + section + "]";
    size_t start = ini.find(header);
    if (start == std::string::npos) return "";
    size_t contentStart = ini.find('\n', start + 1);
    if (contentStart == std::string::npos) return "";
    size_t end = ini.find("\n[", contentStart);
    if (end == std::string::npos) end = ini.size();
    return ini.substr(contentStart, end - contentStart);
}

// ========== 1. unified_flags 完整性守卫 ==========

void test_unified_flags_complete_feature_set() {
    TestLog::testStart("Unified: unified_flags keeps complete feature set");

    std::string pio = readProjectFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(pio.empty(), "platformio.ini must be readable");

    std::string section = extractIniSection(pio, "unified_flags");
    TEST_ASSERT_FALSE_MESSAGE(section.empty(), "[unified_flags] section must exist");

    // 统一架构编入全部功能模块（运行时 FeatureBudget 门控）
    const char* required[] = {
        "-DFASTBEE_ENABLE_MODBUS=1",
        "-DFASTBEE_ENABLE_RULE_SCRIPT=1",
        "-DFASTBEE_ENABLE_COMMAND_SCRIPT=1",
        "-DFASTBEE_ENABLE_IR_REMOTE=1",
        "-DFASTBEE_ENABLE_RFID=1",
        "-DFASTBEE_ENABLE_I2C_SENSORS=1",
        "-DFASTBEE_ENABLE_DEVICE_CONTROL=1",
        "-DFASTBEE_ENABLE_SENSOR_DRIVER=1",
        "-DFASTBEE_ENABLE_DS18B20=1",
        "-DFASTBEE_ENABLE_I18N=1",
    };
    for (const char* flag : required) {
        TEST_ASSERT_TRUE_MESSAGE(
            section.find(flag) != std::string::npos,
            (std::string("[unified_flags] must set ") + flag).c_str());
    }

    // 外部网络适配器不属于共享基线，避免小内存芯片被意外链接；
    // 具体环境在 test_network_adapter_compile_matrix() 中显式声明。
    const char* omitted[] = {
        "-DFASTBEE_ENABLE_ETHERNET=1",
        "-DFASTBEE_ENABLE_CELLULAR=1",
    };
    for (const char* flag : omitted) {
        TEST_ASSERT_TRUE_MESSAGE(
            section.find(flag) == std::string::npos,
            (std::string("[unified_flags] must not set ") + flag + " globally").c_str());
    }
    const char* disabled[] = {
        "-DFASTBEE_ENABLE_BLE=0",
    };
    for (const char* flag : disabled) {
        TEST_ASSERT_TRUE_MESSAGE(
            section.find(flag) != std::string::npos,
            (std::string("[unified_flags] must set ") + flag + " (opt-in via feature config)").c_str());
    }

    TestLog::testEnd(true);
}

// ========== 2. 已删除预设段守卫 ==========

void test_legacy_profile_sections_removed() {
    TestLog::testStart("Unified: legacy profile sections must stay removed");

    std::string pio = readProjectFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(pio.empty(), "platformio.ini must be readable");

    // 2026-08-27 环境整合删除的段，复活会重新引入双份配置漂移
    const char* removed[] = {
        "[lite_flags]",
        "[standard_flags]",
        "[full_flags]",
    };
    for (const char* section : removed) {
        TEST_ASSERT_TRUE_MESSAGE(
            pio.find(section) == std::string::npos,
            (std::string(section) + " was removed by env consolidation and must not be re-added").c_str());
    }

    // Shared source filters are intentionally retained for all unified envs.
    TEST_ASSERT_TRUE_MESSAGE(
        pio.find("[standard_src_filter]") != std::string::npos &&
        pio.find("[standard_ota_src_filter]") != std::string::npos,
        "shared source filters must remain available to unified environments");

    // -unified 后缀环境已并入同名基础环境
    TEST_ASSERT_TRUE_MESSAGE(
        pio.find("-unified]") == std::string::npos,
        "-unified environments were merged into base environments and must not be re-added");

    TestLog::testEnd(true);
}

// ========== 3. 统一 Web 镜像守卫 ==========

void test_unified_web_image_full_profile() {
    TestLog::testStart("Unified: factory web image is full profile for all envs");

    std::string py = readProjectFile("scripts/fastbee-artifacts.py");
    TEST_ASSERT_FALSE_MESSAGE(py.empty(), "scripts/fastbee-artifacts.py must be readable");

    // 出厂镜像统一 full 档（运行时功能门控），不再按环境区分 profile
    TEST_ASSERT_TRUE_MESSAGE(
        py.find("--web-profile=full") != std::string::npos,
        "fastbee-artifacts.py must build web image with --web-profile=full");
    TEST_ASSERT_TRUE_MESSAGE(
        py.find("ENV_TO_PROFILE") == std::string::npos,
        "ENV_TO_PROFILE mapping must stay removed (unified web image)");

    std::string gz = readProjectFile("scripts/gzip-www.js");
    TEST_ASSERT_FALSE_MESSAGE(gz.empty(), "scripts/gzip-www.js must be readable");
    TEST_ASSERT_TRUE_MESSAGE(
        gz.find("'esp32-F4R0'") != std::string::npos,
        "gzip-www.js fallback environment must be esp32-F4R0 (default_envs)");

    TestLog::testEnd(true);
}

// ========== 4. env lib_deps/lib_ignore 守卫 ==========

void test_env_lib_deps_unified_build() {
    TestLog::testStart("Unified: env lib_deps/lib_ignore guards");

    std::string pio = readProjectFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(pio.empty(), "platformio.ini must be readable");

    // 统一架构编译全部驱动：每个出厂环境都必须声明外设库
    const char* envs[] = {
        "env:esp32-F4R0", "env:esp32-F8R0", "env:esp32-F8R8",
        "env:esp32c3-F4R0", "env:esp32c6-F4R0", "env:esp32c6-F8R0",
        "env:esp32s3-F8R0", "env:esp32s3-F8R8", "env:esp32s3-F16R8"
    };
    const char* periphLibs[] = { "MFRC522", "BMP280", "MPU6050" };
    for (const char* env : envs) {
        std::string section = extractIniSection(pio, env);
        TEST_ASSERT_FALSE_MESSAGE(section.empty(),
            (std::string("[") + env + "] section must exist").c_str());
        for (const char* lib : periphLibs) {
            TEST_ASSERT_TRUE_MESSAGE(
                section.find(lib) != std::string::npos,
                (std::string("[") + env + "] must declare " + lib +
                 " (unified build compiles all drivers)").c_str());
        }
    }

    // DS18B20 作为统一镜像的一部分：C3/C6 不得再排除 OneWire/DallasTemperature，
    // 也不得通过编译宏关闭；实际是否读取由功能配置中的 ds18b20 开关决定。
    const char* sensorEnvs[] = { "env:esp32c3-F4R0", "env:esp32c6-F4R0" };
    for (const char* env : sensorEnvs) {
        std::string section = extractIniSection(pio, env);
        TEST_ASSERT_TRUE_MESSAGE(
            section.find("OneWire") == std::string::npos &&
            section.find("DallasTemperature") == std::string::npos,
            (std::string("[") + env + "] must not ignore OneWire/DallasTemperature").c_str());
        TEST_ASSERT_TRUE_MESSAGE(
            section.find("-DFASTBEE_ENABLE_DS18B20=0") == std::string::npos,
            (std::string("[") + env + "] must keep DS18B20 in the unified image").c_str());
    }

    // ESP32-S3 的 RMT 驱动与 IRremoteESP8266 冲突：lib_ignore 排除 + 宏关闭
    const char* s3LiteEnvs[] = { "env:esp32s3-F8R8" };
    for (const char* env : s3LiteEnvs) {
        std::string section = extractIniSection(pio, env);
        TEST_ASSERT_TRUE_MESSAGE(
            section.find("IRremoteESP8266") != std::string::npos &&
            section.find("lib_ignore") != std::string::npos,
            (std::string("[") + env + "] must lib_ignore IRremoteESP8266 (RMT conflict)").c_str());
        TEST_ASSERT_TRUE_MESSAGE(
            section.find("-DFASTBEE_ENABLE_IR_REMOTE=0") != std::string::npos,
            (std::string("[") + env + "] must set -DFASTBEE_ENABLE_IR_REMOTE=0").c_str());
    }

    // F16R8 has enough flash/PSRAM for the optional IR/RF adapters.
    std::string f16r8 = extractIniSection(pio, "env:esp32s3-F16R8");
    TEST_ASSERT_TRUE_MESSAGE(
        f16r8.find("crankyoldgit/IRremoteESP8266") != std::string::npos &&
        f16r8.find("-DFASTBEE_ENABLE_IR_REMOTE=1") != std::string::npos &&
        f16r8.find("-DFASTBEE_ENABLE_RF_LEARN=1") != std::string::npos,
        "[env:esp32s3-F16R8] must enable IR/RF adapters");

    TestLog::testEnd(true);
}

// ========== 4b. 外部网络适配器编译矩阵守卫 ==========

void test_network_adapter_compile_matrix() {
    TestLog::testStart("Profile: Ethernet/cellular adapters follow hardware matrix");

    std::string pio = readProjectFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(pio.empty(), "platformio.ini must be readable");

    struct AdapterProfile {
        const char* env;
        bool enabled;
    };
    const AdapterProfile profiles[] = {
        { "env:esp32-F4R0", false },
        { "env:esp32-F8R0", true },
        { "env:esp32-F8R8", true },
        { "env:esp32c3-F4R0", false },
        { "env:esp32c6-F4R0", false },
        { "env:esp32c6-F8R0", false },
        { "env:esp32s3-F8R0", true },
        { "env:esp32s3-F8R8", true },
        { "env:esp32s3-F16R8", true }
    };

    for (const AdapterProfile& profile : profiles) {
        std::string section = extractIniSection(pio, profile.env);
        TEST_ASSERT_FALSE_MESSAGE(section.empty(),
            (std::string("[") + profile.env + "] section must exist").c_str());

        const std::string expected = profile.enabled ? "=1" : "=0";
        TEST_ASSERT_TRUE_MESSAGE(
            section.find("-DFASTBEE_ENABLE_ETHERNET" + expected) != std::string::npos &&
            section.find("-DFASTBEE_ENABLE_CELLULAR" + expected) != std::string::npos,
            (std::string("[") + profile.env + "] must explicitly set adapter flags to " + expected).c_str());

        if (profile.enabled) {
            TEST_ASSERT_TRUE_MESSAGE(
                section.find("TinyGSM") != std::string::npos &&
                section.find("SSLClient") != std::string::npos,
                (std::string("[") + profile.env + "] enabled adapter profile needs TinyGSM and SSLClient").c_str());
            TEST_ASSERT_TRUE_MESSAGE(
                section.find("-<network/CellularAdapter.cpp>") == std::string::npos &&
                section.find("-<network/EthernetAdapter.cpp>") == std::string::npos,
                (std::string("[") + profile.env + "] must compile adapter sources").c_str());
        } else {
            TEST_ASSERT_TRUE_MESSAGE(
                section.find("TinyGSM") == std::string::npos &&
                section.find("SSLClient") == std::string::npos,
                (std::string("[") + profile.env + "] disabled adapter profile must omit cellular libraries").c_str());
            TEST_ASSERT_TRUE_MESSAGE(
                section.find("${lite_src_filter.build_src_filter}") != std::string::npos,
                (std::string("[") + profile.env + "] must use lite adapter source filter").c_str());
        }
    }

    TestLog::testEnd(true);
}

// ========== 5. OTA 策略守卫 ==========

void test_ota_policy_follows_hardware() {
    TestLog::testStart("Unified: OTA disabled on non-PSRAM single-slot devices");

    std::string pio = readProjectFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(pio.empty(), "platformio.ini must be readable");

    // 无 PSRAM + 4MB 单 app 分区：必须在 env 段显式关闭 OTA（不依赖默认值）。
    // DS18B20 不在此处关闭，统一镜像由功能配置运行时门控。
    const char* noOtaEnvs[] = { "env:esp32-F4R0", "env:esp32c3-F4R0", "env:esp32c6-F4R0" };
    for (const char* env : noOtaEnvs) {
        std::string section = extractIniSection(pio, env);
        TEST_ASSERT_FALSE_MESSAGE(section.empty(),
            (std::string("[") + env + "] section must exist").c_str());
        TEST_ASSERT_TRUE_MESSAGE(
            section.find("-DFASTBEE_ENABLE_OTA=0") != std::string::npos &&
            section.find("-DFASTBEE_ENABLE_OTA_FS=0") != std::string::npos,
            (std::string("[") + env + "] must explicitly set OTA/OTA_FS=0 "
             "(4MB single-slot partition cannot do dual-bank OTA)").c_str());
        TEST_ASSERT_TRUE_MESSAGE(
            section.find("partitions/4MB.csv") != std::string::npos ||
            section.find("${esp32_base.board_build.partitions}") != std::string::npos,
            (std::string("[") + env + "] must use the 4MB single-slot partition table").c_str());
    }

    // 8MB/16MB 双 app 分区：环境必须显式开启 OTA，分区表必须是双槽
    const char* otaEnvs[] = {
        "env:esp32-F8R0", "env:esp32-F8R8", "env:esp32c6-F8R0",
        "env:esp32s3-F8R0", "env:esp32s3-F8R8", "env:esp32s3-F16R8"
    };
    for (const char* env : otaEnvs) {
        std::string section = extractIniSection(pio, env);
        TEST_ASSERT_FALSE_MESSAGE(section.empty(),
            (std::string("[") + env + "] section must exist").c_str());
        TEST_ASSERT_TRUE_MESSAGE(
            section.find("partitions/8MB.csv") != std::string::npos ||
            section.find("partitions/16MB.csv") != std::string::npos,
            (std::string("[") + env + "] must use a dual-slot (A/B) partition table for OTA").c_str());
        TEST_ASSERT_TRUE_MESSAGE(
            section.find("-DFASTBEE_ENABLE_OTA=1") != std::string::npos &&
            section.find("-DFASTBEE_ENABLE_OTA_FS=1") != std::string::npos,
            (std::string("[") + env + "] must explicitly enable OTA/OTA_FS "
             "(dual-slot capable)").c_str());
    }

    TestLog::testEnd(true);
}

// ========== 6. Web 构建设备大屏门控守卫 ==========

void test_web_build_device_control_gate() {
    TestLog::testStart("Profile: web build hasDeviceControl() gate");

    std::string profileJs = readProjectFile("scripts/web-profile.js");
    TEST_ASSERT_FALSE_MESSAGE(profileJs.empty(), "scripts/web-profile.js must be readable");
    TEST_ASSERT_TRUE_MESSAGE(
        profileJs.find("hasDeviceControl") != std::string::npos,
        "web-profile.js must export hasDeviceControl()");

    std::string buildJs = readProjectFile("scripts/build-web-modules.js");
    TEST_ASSERT_FALSE_MESSAGE(buildJs.empty(), "scripts/build-web-modules.js must be readable");
    TEST_ASSERT_TRUE_MESSAGE(
        buildJs.find("hasDeviceControl()") != std::string::npos,
        "build-web-modules.js must gate device-control via hasDeviceControl()");
    TEST_ASSERT_TRUE_MESSAGE(
        buildJs.find("skipWithoutDC") != std::string::npos,
        "device-control bundles must carry skipWithoutDC marker");

    std::string artifactsPy = readProjectFile("scripts/fastbee-artifacts.py");
    TEST_ASSERT_FALSE_MESSAGE(artifactsPy.empty(), "scripts/fastbee-artifacts.py must be readable");
    TEST_ASSERT_TRUE_MESSAGE(
        artifactsPy.find("FASTBEE_ENABLE_DEVICE_CONTROL") != std::string::npos,
        "fastbee-artifacts.py must forward FASTBEE_ENABLE_DEVICE_CONTROL");

    TestLog::testEnd(true);
}

void test_profile_consistency_group() {
    TestLog::groupStart("Profile Consistency Tests");
    RUN_TEST(test_unified_flags_complete_feature_set);
    RUN_TEST(test_legacy_profile_sections_removed);
    RUN_TEST(test_unified_web_image_full_profile);
    RUN_TEST(test_env_lib_deps_unified_build);
    RUN_TEST(test_network_adapter_compile_matrix);
    RUN_TEST(test_ota_policy_follows_hardware);
    RUN_TEST(test_web_build_device_control_gate);
    TestLog::groupEnd();
}
