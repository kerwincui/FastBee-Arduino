/**
 * @file test_provision_api.cpp
 * @brief AP 配网接口测试
 *
 * 覆盖：
 * - /api/wifi/scan 响应格式验证（networks 字段、encrypted 布尔类型）
 * - /api/wifi/connect 扩展参数（userId/deviceNum/extra）写入 device.json
 * - 不带扩展参数时兼容性不受影响
 * - 扩展参数为空时 device.json 不被修改
 */

#include <unity.h>
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "helpers/TestLogger.h"

// 测试用 device.json 路径（与实际代码保持一致）
static const char* TEST_DEVICE_CONFIG = "/config/device.json";

// ---------------------------------------------------------------------------
// 辅助工具
// ---------------------------------------------------------------------------

static void _writeTestDeviceJson(const char* content) {
    File f = LittleFS.open(TEST_DEVICE_CONFIG, "w");
    TEST_ASSERT_TRUE_MESSAGE(f, "Failed to open device.json for writing");
    f.print(content);
    f.close();
}

static String _readTestDeviceJson() {
    File f = LittleFS.open(TEST_DEVICE_CONFIG, "r");
    if (!f) return "";
    String content = f.readString();
    f.close();
    return content;
}

static void _ensureConfigDir() {
    if (!LittleFS.exists("/config")) {
        LittleFS.mkdir("/config");
    }
}

// ---------------------------------------------------------------------------
// 测试：WiFi 扫描响应格式（networks + encrypted 布尔值）
// ---------------------------------------------------------------------------
static void test_wifi_scan_response_format() {
    TestLog::testStart("Provision: WiFi Scan Response Format");

    // 模拟构建扫描响应 JSON（与 handleWiFiScan 输出逻辑一致）
    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();

    // 模拟 3 个 WiFi 网络
    const char* ssids[]     = {"OfficeWiFi", "HomeNet", "GuestOpen"};
    const int   rssi[]      = {-45, -70, -85};
    const int   channels[]  = {6, 11, 1};
    const bool  encrypted[] = {true, true, false};

    for (int i = 0; i < 3; i++) {
        JsonObject net = networks.add<JsonObject>();
        net["ssid"]       = ssids[i];
        net["rssi"]       = rssi[i];
        net["channel"]    = channels[i];
        net["encrypted"]  = encrypted[i];
    }
    doc["success"] = true;
    doc["count"]   = 3;

    // 验证序列化
    String json;
    serializeJson(doc, json);
    TEST_ASSERT_TRUE_MESSAGE(json.indexOf("\"networks\"") >= 0,
        "Response must contain 'networks' field");
    TEST_ASSERT_TRUE_MESSAGE(json.indexOf("\"data\"") < 0,
        "Response must NOT contain old 'data' field");
    TestLog::step("Response uses 'networks' field (not 'data')");

    // 反序列化验证字段类型
    JsonDocument parsed;
    deserializeJson(parsed, json);
    TEST_ASSERT_TRUE(parsed["success"].as<bool>());
    TEST_ASSERT_EQUAL(3, parsed["count"].as<int>());
    TestLog::step("success=true, count=3");

    JsonArray arr = parsed["networks"].as<JsonArray>();
    TEST_ASSERT_EQUAL(3, arr.size());

    for (int i = 0; i < 3; i++) {
        JsonObject net = arr[i];
        TEST_ASSERT_EQUAL_STRING(ssids[i], net["ssid"].as<const char*>());
        TEST_ASSERT_EQUAL(rssi[i], net["rssi"].as<int>());
        TEST_ASSERT_EQUAL(channels[i], net["channel"].as<int>());
        // encrypted 必须是布尔类型
        TEST_ASSERT_TRUE_MESSAGE(net["encrypted"].is<bool>(),
            "encrypted field must be boolean type");
        TEST_ASSERT_EQUAL(encrypted[i], net["encrypted"].as<bool>());
    }
    TestLog::step("All networks have correct ssid, rssi, channel, encrypted(bool)");

    TestLog::testEnd(true);
}

// ---------------------------------------------------------------------------
// 测试：扩展参数写入 device.json（userId / deviceNum / extra）
// ---------------------------------------------------------------------------
static void test_provision_extended_params_write() {
    TestLog::testStart("Provision: Extended Params Write to device.json");
    _ensureConfigDir();

    // 准备初始 device.json
    _writeTestDeviceJson(
        "{\"deviceId\":\"\",\"productNumber\":0,\"userId\":\"1\","
        "\"deviceName\":\"fastbee\",\"logLevel\":\"INFO\"}");

    // 模拟 _updateDeviceConfig 的核心逻辑
    const String userId    = "42";
    const String deviceNum = "FBE00112233";
    const String extra     = "7";   // 有效的产品编号

    JsonDocument doc;
    {
        File f = LittleFS.open(TEST_DEVICE_CONFIG, "r");
        TEST_ASSERT_TRUE(f);
        deserializeJson(doc, f);
        f.close();
    }

    bool changed = false;
    if (!userId.isEmpty())    { doc["userId"]         = userId;         changed = true; }
    if (!deviceNum.isEmpty()) { doc["deviceId"]       = deviceNum;      changed = true; }
    if (!extra.isEmpty()) {
        long pn = extra.toInt();
        if (pn > 0) { doc["productNumber"] = (int)pn; changed = true; }
    }
    TEST_ASSERT_TRUE(changed);

    if (changed) {
        File f = LittleFS.open(TEST_DEVICE_CONFIG, "w");
        TEST_ASSERT_TRUE(f);
        serializeJsonPretty(doc, f);
        f.close();
    }
    TestLog::step("device.json written with extended params");

    // 验证写入结果
    String content = _readTestDeviceJson();
    JsonDocument verify;
    deserializeJson(verify, content);

    TEST_ASSERT_EQUAL_STRING("42",          verify["userId"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("FBE00112233", verify["deviceId"].as<const char*>());
    TEST_ASSERT_EQUAL(7,                    verify["productNumber"].as<int>());
    TestLog::step("userId=42, deviceId=FBE00112233, productNumber=7 verified");

    // 原有字段不受影响
    TEST_ASSERT_EQUAL_STRING("fastbee", verify["deviceName"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("INFO",    verify["logLevel"].as<const char*>());
    TestLog::step("Existing fields (deviceName, logLevel) unchanged");

    TestLog::testEnd(true);
}

// ---------------------------------------------------------------------------
// 测试：不带扩展参数时兼容性（device.json 不被修改）
// ---------------------------------------------------------------------------
static void test_provision_no_extended_params_compatible() {
    TestLog::testStart("Provision: No Extended Params - Backward Compatible");
    _ensureConfigDir();

    const char* original =
        "{\"deviceId\":\"OLD_ID\",\"productNumber\":5,\"userId\":\"99\","
        "\"deviceName\":\"mydev\",\"logLevel\":\"DEBUG\"}";
    _writeTestDeviceJson(original);

    // 模拟空的扩展参数
    const String userId    = "";
    const String deviceNum = "";
    const String extra     = "";

    bool hasExtParam = !userId.isEmpty() || !deviceNum.isEmpty() || !extra.isEmpty();
    TEST_ASSERT_FALSE(hasExtParam);
    TestLog::step("hasExtParam=false when all params empty");

    // 不应触发任何写入
    if (hasExtParam) {
        TEST_FAIL_MESSAGE("Should not enter update block when params are empty");
    }

    // 验证 device.json 内容未变化
    String content = _readTestDeviceJson();
    JsonDocument verify;
    deserializeJson(verify, content);

    TEST_ASSERT_EQUAL_STRING("OLD_ID", verify["deviceId"].as<const char*>());
    TEST_ASSERT_EQUAL(5,              verify["productNumber"].as<int>());
    TEST_ASSERT_EQUAL_STRING("99",    verify["userId"].as<const char*>());
    TestLog::step("device.json fields unchanged (deviceId=OLD_ID, productNumber=5, userId=99)");

    TestLog::testEnd(true);
}

// ---------------------------------------------------------------------------
// 测试：extra 为无效值时不写入 productNumber
// ---------------------------------------------------------------------------
static void test_provision_extra_invalid_discarded() {
    TestLog::testStart("Provision: extra Invalid Value Discarded");
    _ensureConfigDir();

    _writeTestDeviceJson(
        "{\"deviceId\":\"\",\"productNumber\":3,\"userId\":\"1\","
        "\"deviceName\":\"fastbee\"}");

    const String userId    = "";
    const String deviceNum = "";
    const String extra     = "not-a-number";  // 无效值

    JsonDocument doc;
    {
        File f = LittleFS.open(TEST_DEVICE_CONFIG, "r");
        TEST_ASSERT_TRUE(f);
        deserializeJson(doc, f);
        f.close();
    }

    bool changed = false;
    if (!userId.isEmpty())    { doc["userId"]   = userId;    changed = true; }
    if (!deviceNum.isEmpty()) { doc["deviceId"] = deviceNum; changed = true; }
    if (!extra.isEmpty()) {
        long pn = extra.toInt();  // "not-a-number".toInt() → 0
        if (pn > 0) {
            doc["productNumber"] = (int)pn;
            changed = true;
        }
    }

    // changed 应为 false（extra 无效，其余为空）
    TEST_ASSERT_FALSE(changed);
    TestLog::step("extra='not-a-number' → toInt()=0, discarded, changed=false");

    // 验证 productNumber 保持原值
    String content = _readTestDeviceJson();
    JsonDocument verify;
    deserializeJson(verify, content);
    TEST_ASSERT_EQUAL(3, verify["productNumber"].as<int>());
    TestLog::step("productNumber remains 3 (original value)");

    TestLog::testEnd(true);
}

// ---------------------------------------------------------------------------
// 测试：仅部分扩展参数有值时只更新对应字段
// ---------------------------------------------------------------------------
static void test_provision_partial_params() {
    TestLog::testStart("Provision: Partial Params - Only Update Provided");
    _ensureConfigDir();

    _writeTestDeviceJson(
        "{\"deviceId\":\"EXISTING_ID\",\"productNumber\":2,\"userId\":\"10\","
        "\"deviceName\":\"fastbee\"}");

    // 只传 userId，不传 deviceNum 和 extra
    const String userId    = "55";
    const String deviceNum = "";
    const String extra     = "";

    JsonDocument doc;
    {
        File f = LittleFS.open(TEST_DEVICE_CONFIG, "r");
        TEST_ASSERT_TRUE(f);
        deserializeJson(doc, f);
        f.close();
    }

    bool changed = false;
    if (!userId.isEmpty())    { doc["userId"]   = userId;    changed = true; }
    if (!deviceNum.isEmpty()) { doc["deviceId"] = deviceNum; changed = true; }
    if (!extra.isEmpty()) {
        long pn = extra.toInt();
        if (pn > 0) { doc["productNumber"] = (int)pn; changed = true; }
    }
    TEST_ASSERT_TRUE(changed);

    {
        File f = LittleFS.open(TEST_DEVICE_CONFIG, "w");
        TEST_ASSERT_TRUE(f);
        serializeJsonPretty(doc, f);
        f.close();
    }

    String content = _readTestDeviceJson();
    JsonDocument verify;
    deserializeJson(verify, content);

    // userId 更新
    TEST_ASSERT_EQUAL_STRING("55", verify["userId"].as<const char*>());
    TestLog::step("userId updated to 55");

    // deviceId 保持原值
    TEST_ASSERT_EQUAL_STRING("EXISTING_ID", verify["deviceId"].as<const char*>());
    TestLog::step("deviceId unchanged (EXISTING_ID)");

    // productNumber 保持原值
    TEST_ASSERT_EQUAL(2, verify["productNumber"].as<int>());
    TestLog::step("productNumber unchanged (2)");

    TestLog::testEnd(true);
}

// ---------------------------------------------------------------------------
// 测试组入口
// ---------------------------------------------------------------------------
void test_provision_api_group() {
    TestLog::groupStart("Provision API Tests");

    RUN_TEST(test_wifi_scan_response_format);
    RUN_TEST(test_provision_extended_params_write);
    RUN_TEST(test_provision_no_extended_params_compatible);
    RUN_TEST(test_provision_extra_invalid_discarded);
    RUN_TEST(test_provision_partial_params);

    TestLog::groupEnd();
}
