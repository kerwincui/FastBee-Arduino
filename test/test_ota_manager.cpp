/**
 * @file test_ota_manager.cpp
 * @brief OTAManager 边界条件单元测试
 * 
 * 测试内容：
 * - OTA状态机初始状态
 * - 进度百分比边界
 * - 并发更新保护
 * - 错误处理与取消
 * - 大固件溢出保护
 * - 分片上传流程
 */

#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include "mocks/MockOTA.h"
#include "protocols/FastBeeOTAProtocol.h"

void test_ota_manager_group();

// ========== 测试用例 ==========

static void test_ota_initial_state() {
    MockOTAManager ota;
    
    TEST_ASSERT_FALSE(ota.isOTAInProgress());
    TEST_ASSERT_EQUAL(0, ota.getProgress());
    TEST_ASSERT_EQUAL((int)OTAState::OTA_IDLE, (int)ota.getState());
    TEST_ASSERT_EQUAL((int)OTAError::OTA_OK, (int)ota.getError());
}

static void test_ota_begin_and_progress() {
    MockOTAManager ota;
    
    // 开始100KB固件升级
    bool ok = ota.beginOTA(100000);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(ota.isOTAInProgress());
    TEST_ASSERT_EQUAL(0, ota.getProgress());
    TEST_ASSERT_EQUAL((int)OTAState::OTA_WRITING, (int)ota.getState());
    
    // 写入50%
    uint8_t buf[1000];
    memset(buf, 0xAA, sizeof(buf));
    for (int i = 0; i < 50; i++) {
        size_t written = ota.writeData(buf, 1000);
        TEST_ASSERT_EQUAL(1000, written);
    }
    TEST_ASSERT_EQUAL(50, ota.getProgress());
    
    // 写入剩余50%
    for (int i = 0; i < 50; i++) {
        ota.writeData(buf, 1000);
    }
    TEST_ASSERT_EQUAL(100, ota.getProgress());
}

static void test_ota_concurrent_protection() {
    MockOTAManager ota;
    
    ota.beginOTA(50000);
    TEST_ASSERT_TRUE(ota.isOTAInProgress());
    
    // 正在进行中不应允许第二次启动
    bool secondStart = ota.beginOTA(60000);
    TEST_ASSERT_FALSE(secondStart);
}

static void test_ota_cancel() {
    MockOTAManager ota;
    
    ota.beginOTA(100000);
    uint8_t buf[1000];
    ota.writeData(buf, 1000);
    TEST_ASSERT_TRUE(ota.isOTAInProgress());
    
    ota.cancelOTA();
    TEST_ASSERT_FALSE(ota.isOTAInProgress());
    TEST_ASSERT_EQUAL((int)OTAState::OTA_FAILED, (int)ota.getState());
    TEST_ASSERT_EQUAL((int)OTAError::OTA_ERROR_CANCELED, (int)ota.getError());
}

static void test_ota_size_overflow() {
    MockOTAManager ota;
    
    // 超过最大可用空间
    size_t maxSpace = ota.getMaxSketchSpace();
    bool ok = ota.beginOTA(maxSpace + 1);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_FALSE(ota.isOTAInProgress());
    TEST_ASSERT_EQUAL((int)OTAError::OTA_ERROR_NO_SPACE, (int)ota.getError());
}

static void test_ota_fail_at_percent() {
    MockOTAManager ota;
    
    ota.setFailAtPercent(30);
    ota.beginOTA(10000);
    
    uint8_t buf[1000];
    memset(buf, 0, sizeof(buf));
    // 写入到3000字节(30%)时应该失败
    ota.writeData(buf, 1000);
    ota.writeData(buf, 1000);
    size_t written = ota.writeData(buf, 1000);  // 30%
    TEST_ASSERT_EQUAL(0, written);
    TEST_ASSERT_FALSE(ota.isOTAInProgress());
    TEST_ASSERT_EQUAL((int)OTAError::OTA_ERROR_WRITE_FAILED, (int)ota.getError());
}

static void test_ota_end_incomplete() {
    MockOTAManager ota;
    
    ota.beginOTA(10000);
    uint8_t buf[5000];
    ota.writeData(buf, 5000);  // 只写了一半
    
    bool ok = ota.endOTA();  // 不完整应该失败
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL((int)OTAError::OTA_ERROR_VERIFY_FAILED, (int)ota.getError());
}

static void test_ota_end_complete() {
    MockOTAManager ota;
    
    ota.beginOTA(5000);
    uint8_t buf[5000];
    ota.writeData(buf, 5000);  // 写完整
    
    bool ok = ota.endOTA();
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FALSE(ota.isOTAInProgress());
    TEST_ASSERT_EQUAL((int)OTAState::OTA_COMPLETED, (int)ota.getState());
}

static void test_ota_url_start() {
    MockOTAManager ota;
    
    bool ok = ota.startOTAFromURL("http://example.com/fw.bin");
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(ota.isOTAInProgress());
}

static void test_ota_url_fail() {
    MockOTAManager ota;
    ota.setShouldFail(true);
    
    bool ok = ota.startOTAFromURL("http://example.com/fw.bin");
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_FALSE(ota.isOTAInProgress());
    TEST_ASSERT_EQUAL((int)OTAError::OTA_ERROR_NETWORK, (int)ota.getError());
}

static void test_ota_status_json() {
    MockOTAManager ota;
    
    String status = ota.getStatusJSON();
    TEST_ASSERT_TRUE(status.indexOf("state") >= 0);
    TEST_ASSERT_TRUE(status.indexOf("progress") >= 0);
    TEST_ASSERT_TRUE(status.indexOf("inProgress") >= 0);
}

static void test_ota_reset() {
    MockOTAManager ota;
    
    ota.beginOTA(10000);
    uint8_t buf[5000];
    ota.writeData(buf, 5000);
    
    ota.reset();
    TEST_ASSERT_FALSE(ota.isOTAInProgress());
    TEST_ASSERT_EQUAL(0, ota.getProgress());
    TEST_ASSERT_EQUAL((int)OTAState::OTA_IDLE, (int)ota.getState());
}

static void test_fastbee_ota_parses_https_upgrade_command() {
    FastBeeOtaProtocol::Command command;
    String error;

    bool ok = FastBeeOtaProtocol::parseCommand(
        "{\"taskId\":26,\"url\":\"/profile/iot/1/fw.bin\",\"version\":1.2,\"status\":1}",
        command, error);

    TEST_ASSERT_TRUE_MESSAGE(ok, error.c_str());
    TEST_ASSERT_EQUAL_INT32(26, command.taskId);
    TEST_ASSERT_EQUAL_STRING("/profile/iot/1/fw.bin", command.url.c_str());
    TEST_ASSERT_EQUAL_STRING("1.2", command.version.c_str());
}

static void test_fastbee_ota_rejects_invalid_commands() {
    const char* invalidPayloads[] = {
        "not-json",
        "[]",
        "{\"taskId\":0,\"url\":\"/fw.bin\",\"version\":\"1.0\",\"status\":1}",
        "{\"taskId\":26,\"url\":\"/fw.bin\",\"version\":\"1.0\",\"status\":2}",
        "{\"taskId\":26,\"version\":\"1.0\",\"status\":1}",
        "{\"taskId\":26,\"url\":\"/fw.bin\",\"status\":1}"
    };

    for (const char* payload : invalidPayloads) {
        FastBeeOtaProtocol::Command command;
        String error;
        TEST_ASSERT_FALSE_MESSAGE(
            FastBeeOtaProtocol::parseCommand(payload, command, error), payload);
        TEST_ASSERT_FALSE_MESSAGE(error.isEmpty(), payload);
    }
}

static void test_fastbee_ota_resolves_https_and_relative_urls() {
    String resolved;
    String error;

    TEST_ASSERT_TRUE(FastBeeOtaProtocol::resolveFirmwareUrl(
        "https://cdn.fastbee.cn/firmware/fw.bin", "https://fastbee.cn", "unused",
        resolved, error));
    TEST_ASSERT_EQUAL_STRING("https://cdn.fastbee.cn/firmware/fw.bin", resolved.c_str());

    TEST_ASSERT_TRUE(FastBeeOtaProtocol::resolveFirmwareUrl(
        "/profile/iot/1/fw.bin", "https://fastbee.cn/", "unused",
        resolved, error));
    TEST_ASSERT_EQUAL_STRING("https://fastbee.cn/profile/iot/1/fw.bin", resolved.c_str());

    TEST_ASSERT_TRUE(FastBeeOtaProtocol::resolveFirmwareUrl(
        "profile/iot/1/fw.bin", "", "mqtt.fastbee.cn", resolved, error));
    TEST_ASSERT_EQUAL_STRING("https://mqtt.fastbee.cn/profile/iot/1/fw.bin", resolved.c_str());
}

static void test_fastbee_ota_rejects_non_https_urls() {
    String resolved;
    String error;

    TEST_ASSERT_FALSE(FastBeeOtaProtocol::resolveFirmwareUrl(
        "http://fastbee.cn/fw.bin", "https://fastbee.cn", "mqtt.fastbee.cn",
        resolved, error));
    TEST_ASSERT_TRUE(error.indexOf("HTTPS") >= 0);

    TEST_ASSERT_FALSE(FastBeeOtaProtocol::resolveFirmwareUrl(
        "/fw.bin", "http://fastbee.cn", "mqtt.fastbee.cn", resolved, error));
    TEST_ASSERT_TRUE(error.indexOf("https://") >= 0);

    TEST_ASSERT_FALSE(FastBeeOtaProtocol::resolveFirmwareUrl(
        "//untrusted.example/fw.bin", "https://fastbee.cn", "mqtt.fastbee.cn",
        resolved, error));
}

static void test_fastbee_ota_reply_matches_platform_schema() {
    String upgrading = FastBeeOtaProtocol::buildReply(
        26, "1.2", FastBeeOtaProtocol::STATUS_UPGRADING, 37);
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, upgrading.c_str(), upgrading.length()));
    TEST_ASSERT_EQUAL_INT32(26, doc["taskId"].as<int32_t>());
    TEST_ASSERT_EQUAL_STRING("1.2", doc["version"].as<const char*>());
    TEST_ASSERT_EQUAL_UINT8(FastBeeOtaProtocol::STATUS_UPGRADING,
                            doc["status"].as<uint8_t>());
    TEST_ASSERT_EQUAL_UINT8(37, doc["progress"].as<uint8_t>());

    String completed = FastBeeOtaProtocol::buildReply(
        26, "1.2", FastBeeOtaProtocol::STATUS_SUCCESS, 100);
    doc.clear();
    TEST_ASSERT_FALSE(deserializeJson(doc, completed.c_str(), completed.length()));
    TEST_ASSERT_EQUAL_UINT8(FastBeeOtaProtocol::STATUS_SUCCESS,
                            doc["status"].as<uint8_t>());
    TEST_ASSERT_TRUE_MESSAGE(doc["progress"].isNull(),
                             "FastBee success reply must omit progress");
}

// ========== 测试组入口 ==========

void test_ota_manager_group() {
    RUN_TEST(test_ota_initial_state);
    RUN_TEST(test_ota_begin_and_progress);
    RUN_TEST(test_ota_concurrent_protection);
    RUN_TEST(test_ota_cancel);
    RUN_TEST(test_ota_size_overflow);
    RUN_TEST(test_ota_fail_at_percent);
    RUN_TEST(test_ota_end_incomplete);
    RUN_TEST(test_ota_end_complete);
    RUN_TEST(test_ota_url_start);
    RUN_TEST(test_ota_url_fail);
    RUN_TEST(test_ota_status_json);
    RUN_TEST(test_ota_reset);
    RUN_TEST(test_fastbee_ota_parses_https_upgrade_command);
    RUN_TEST(test_fastbee_ota_rejects_invalid_commands);
    RUN_TEST(test_fastbee_ota_resolves_https_and_relative_urls);
    RUN_TEST(test_fastbee_ota_rejects_non_https_urls);
    RUN_TEST(test_fastbee_ota_reply_matches_platform_schema);
}
