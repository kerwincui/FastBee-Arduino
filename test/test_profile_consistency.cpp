/**
 * @file test_profile_consistency.cpp
 * @brief Profile 配置矩阵一致性守卫（功能模块可配置化重构）
 *
 * 防止后续改动无意中破坏 lite/standard/full 的功能裁剪矩阵：
 * 1. standard_flags 裁剪守卫（IR/RFID/I2C/DEVICE_CONTROL 必须为 0）
 * 2. full_flags 完整性守卫（重型功能必须为 1）
 * 3. Web profile 映射守卫（F4R0→standard, C3/C6→lite）
 * 4. env lib_deps 守卫（F4R0 不含重型库，S3-F8R0 包含）
 * 5. Web 构建守卫（hasDeviceControl() 已接入门控）
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
static std::string extractIniSection(const std::string& ini, const std::string& section) {
    const std::string header = "[" + section + "]";
    size_t start = ini.find(header);
    if (start == std::string::npos) return "";
    size_t contentStart = ini.find('\n', start);
    if (contentStart == std::string::npos) return "";
    size_t end = ini.find("\n[", contentStart);
    if (end == std::string::npos) end = ini.size();
    return ini.substr(contentStart, end - contentStart);
}

// ========== 1. standard_flags 裁剪守卫 ==========

void test_standard_flags_trimmed_heavy_features() {
    TestLog::testStart("Profile: standard_flags keeps heavy features trimmed");

    std::string pio = readProjectFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(pio.empty(), "platformio.ini must be readable");

    std::string section = extractIniSection(pio, "standard_flags");
    TEST_ASSERT_FALSE_MESSAGE(section.empty(), "[standard_flags] section must exist");

    const char* trimmed[] = {
        "-DFASTBEE_ENABLE_IR_REMOTE=0",
        "-DFASTBEE_ENABLE_RFID=0",
        "-DFASTBEE_ENABLE_I2C_SENSORS=0",
        "-DFASTBEE_ENABLE_DEVICE_CONTROL=0",
    };
    for (const char* flag : trimmed) {
        TEST_ASSERT_TRUE_MESSAGE(
            section.find(flag) != std::string::npos,
            (std::string("[standard_flags] must set ") + flag +
             " (F4R0 4MB Flash cannot fit heavy libs)").c_str());
    }

    // standard 保留项：Modbus / 规则脚本 / RF 学习
    const char* kept[] = {
        "-DFASTBEE_ENABLE_MODBUS=1",
        "-DFASTBEE_ENABLE_RULE_SCRIPT=1",
        "-DFASTBEE_ENABLE_RF_LEARN=1",
        "-DFASTBEE_ENABLE_CONFIG_TRANSFER=1",
    };
    for (const char* flag : kept) {
        TEST_ASSERT_TRUE_MESSAGE(
            section.find(flag) != std::string::npos,
            (std::string("[standard_flags] must set ") + flag).c_str());
    }

    TestLog::testEnd(true);
}

// ========== 2. full_flags 完整性守卫 ==========

void test_full_flags_complete_feature_set() {
    TestLog::testStart("Profile: full_flags keeps complete feature set");

    std::string pio = readProjectFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(pio.empty(), "platformio.ini must be readable");

    std::string section = extractIniSection(pio, "full_flags");
    TEST_ASSERT_FALSE_MESSAGE(section.empty(), "[full_flags] section must exist");

    const char* required[] = {
        "-DFASTBEE_ENABLE_IR_REMOTE=1",
        "-DFASTBEE_ENABLE_RFID=1",
        "-DFASTBEE_ENABLE_I2C_SENSORS=1",
        "-DFASTBEE_ENABLE_DEVICE_CONTROL=1",
        "-DFASTBEE_ENABLE_OTA=1",
        "-DFASTBEE_ENABLE_I18N=1",
    };
    for (const char* flag : required) {
        TEST_ASSERT_TRUE_MESSAGE(
            section.find(flag) != std::string::npos,
            (std::string("[full_flags] must set ") + flag).c_str());
    }

    TestLog::testEnd(true);
}

// ========== 3. Web profile 映射守卫 ==========

void test_web_profile_env_mapping() {
    TestLog::testStart("Profile: ENV_TO_PROFILE mapping");

    std::string py = readProjectFile("scripts/fastbee-artifacts.py");
    TEST_ASSERT_FALSE_MESSAGE(py.empty(), "scripts/fastbee-artifacts.py must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        py.find("\"esp32-F4R0\": \"standard\"") != std::string::npos,
        "esp32-F4R0 must map to standard profile");
    TEST_ASSERT_TRUE_MESSAGE(
        py.find("\"esp32c3-F4R0\": \"lite\"") != std::string::npos,
        "esp32c3-F4R0 must map to lite profile");
    TEST_ASSERT_TRUE_MESSAGE(
        py.find("\"esp32c6-F4R0\": \"lite\"") != std::string::npos,
        "esp32c6-F4R0 must map to lite profile");
    TEST_ASSERT_TRUE_MESSAGE(
        py.find("\"esp32s3-F8R0\": \"standard\"") != std::string::npos,
        "esp32s3-F8R0 must map to standard profile");

    TestLog::testEnd(true);
}

// ========== 4. env lib_deps 守卫 ==========

void test_env_lib_deps_heavy_libs() {
    TestLog::testStart("Profile: env lib_deps heavy lib guard");

    std::string pio = readProjectFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(pio.empty(), "platformio.ini must be readable");

    std::string f4r0 = extractIniSection(pio, "env:esp32-F4R0");
    TEST_ASSERT_FALSE_MESSAGE(f4r0.empty(), "[env:esp32-F4R0] section must exist");
    const char* heavyLibs[] = { "IRremoteESP8266", "MFRC522", "BMP280", "MPU6050" };
    for (const char* lib : heavyLibs) {
        TEST_ASSERT_TRUE_MESSAGE(
            f4r0.find(lib) == std::string::npos,
            (std::string("[env:esp32-F4R0] must not depend on ") + lib +
             " (standard profile trims heavy libs)").c_str());
    }

    std::string s3 = extractIniSection(pio, "env:esp32s3-F8R0");
    TEST_ASSERT_FALSE_MESSAGE(s3.empty(), "[env:esp32s3-F8R0] section must exist");
    for (const char* lib : heavyLibs) {
        TEST_ASSERT_TRUE_MESSAGE(
            s3.find(lib) != std::string::npos,
            (std::string("[env:esp32s3-F8R0] must depend on ") + lib +
             " (explicitly re-enables trimmed heavy features)").c_str());
    }
    // S3-F8R0 显式启用被 standard 裁剪的功能宏
    const char* overrides[] = {
        "-DFASTBEE_ENABLE_I2C_SENSORS=1",
        "-DFASTBEE_ENABLE_RFID=1",
        "-DFASTBEE_ENABLE_IR_REMOTE=1",
    };
    for (const char* flag : overrides) {
        TEST_ASSERT_TRUE_MESSAGE(
            s3.find(flag) != std::string::npos,
            (std::string("[env:esp32s3-F8R0] must set ") + flag).c_str());
    }

    TestLog::testEnd(true);
}

// ========== 5. Web 构建设备大屏门控守卫 ==========

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
    RUN_TEST(test_standard_flags_trimmed_heavy_features);
    RUN_TEST(test_full_flags_complete_feature_set);
    RUN_TEST(test_web_profile_env_mapping);
    RUN_TEST(test_env_lib_deps_heavy_libs);
    RUN_TEST(test_web_build_device_control_gate);
    TestLog::groupEnd();
}
