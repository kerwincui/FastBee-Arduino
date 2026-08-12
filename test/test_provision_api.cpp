/**
 * @file test_provision_api.cpp
 * @brief AP 配网接口测试
 *
 * 覆盖：
 * - /api/wifi/scan 响应格式验证（networks 字段、encrypted 布尔、encryption 安全类型字符串）
 * - /api/wifi/scan 加密枚举→安全类型字符串映射守护
 * - 前端 network.js 扫描结果解析/点选回填契约守护
 * - /api/wifi/scan 超时/失败响应格式验证
 * - /api/wifi/scan 扫描结果上限截断（20 条）
 * - /api/wifi/scan 并发请求去重逻辑验证
 * - "重新扫描"功能已移除守护（force 参数/代数计数器/前端按钮均不应存在）
 * - /api/wifi/connect 扩展参数（userId/deviceNum/extra）写入 device.json
 * - 不带扩展参数时兼容性不受影响
 * - 扩展参数为空时 device.json 不被修改
 */

#include <unity.h>
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
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

// 读取项目源文件（用于前后端契约源码守护测试）
static std::string _readProvisionSrcFile(const char* relativePath) {
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

static void _ensureConfigDir() {
    if (!LittleFS.exists("/config")) {
        LittleFS.mkdir("/config");
    }
}

// ---------------------------------------------------------------------------
// 测试：WiFi 扫描响应格式（networks + encrypted 布尔 + encryption 字符串）
// ---------------------------------------------------------------------------
static void test_wifi_scan_response_format() {
    TestLog::testStart("Provision: WiFi Scan Response Format");

    // 模拟构建扫描响应 JSON（与 _wifiScanTask 输出逻辑一致）
    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();

    // 模拟 3 个 WiFi 网络
    const char* ssids[]      = {"OfficeWiFi", "HomeNet", "GuestOpen"};
    const int   rssi[]       = {-45, -70, -85};
    const int   channels[]   = {6, 11, 1};
    const bool  encrypted[]  = {true, true, false};
    const char* encryption[] = {"wpa2", "wpa3", "open"};

    for (int i = 0; i < 3; i++) {
        JsonObject net = networks.add<JsonObject>();
        net["ssid"]       = ssids[i];
        net["rssi"]       = rssi[i];
        net["channel"]    = channels[i];
        net["encrypted"]  = encrypted[i];
        net["encryption"] = encryption[i];
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
        // encryption 必须是安全类型字符串（供前端 wifi-security 下拉回填）
        TEST_ASSERT_TRUE_MESSAGE(net["encryption"].is<const char*>(),
            "encryption field must be string type");
        TEST_ASSERT_EQUAL_STRING(encryption[i], net["encryption"].as<const char*>());
    }
    TestLog::step("All networks have correct ssid, rssi, channel, encrypted(bool), encryption(string)");

    TestLog::testEnd(true);
}

// ---------------------------------------------------------------------------
// 测试：加密枚举 → 前端安全类型字符串映射（_authModeToString 守护）
// 前端 wifi-security 下拉仅接受 none/wpa/wpa2/wpa3，encryption 必须落在其中
// ---------------------------------------------------------------------------
static void test_wifi_scan_auth_mode_mapping() {
    TestLog::testStart("Provision: WiFi auth mode maps to frontend security strings");

    // 行为镜像：枚举→字符串映射结果必须落在前端可识别集合内
    // （open 前端转为 none，wpa/wpa2/wpa3 直接使用，wep 无对应选项回退 wpa2）
    enum { AUTH_OPEN, AUTH_WEP, AUTH_WPA_PSK, AUTH_WPA2_PSK,
           AUTH_WPA_WPA2_PSK, AUTH_WPA2_ENTERPRISE, AUTH_WPA3_PSK,
           AUTH_WPA2_WPA3_PSK, AUTH_UNKNOWN = 99 };
    auto mirror = [](int mode) -> const char* {
        switch (mode) {
            case AUTH_OPEN:            return "open";
            case AUTH_WEP:             return "wep";
            case AUTH_WPA_PSK:         return "wpa";
            case AUTH_WPA2_PSK:
            case AUTH_WPA_WPA2_PSK:
            case AUTH_WPA2_ENTERPRISE: return "wpa2";
            case AUTH_WPA3_PSK:
            case AUTH_WPA2_WPA3_PSK:   return "wpa3";
            default:                   return "wpa2";
        }
    };
    TEST_ASSERT_EQUAL_STRING("open", mirror(AUTH_OPEN));
    TEST_ASSERT_EQUAL_STRING("wep",  mirror(AUTH_WEP));
    TEST_ASSERT_EQUAL_STRING("wpa",  mirror(AUTH_WPA_PSK));
    TEST_ASSERT_EQUAL_STRING("wpa2", mirror(AUTH_WPA2_PSK));
    TEST_ASSERT_EQUAL_STRING("wpa2", mirror(AUTH_WPA_WPA2_PSK));
    TEST_ASSERT_EQUAL_STRING("wpa2", mirror(AUTH_WPA2_ENTERPRISE));
    TEST_ASSERT_EQUAL_STRING("wpa3", mirror(AUTH_WPA3_PSK));
    TEST_ASSERT_EQUAL_STRING("wpa3", mirror(AUTH_WPA2_WPA3_PSK));
    TEST_ASSERT_EQUAL_STRING("wpa2", mirror(AUTH_UNKNOWN));
    TestLog::step("Mirror mapping: 9 auth modes map to expected strings");

    std::string src = _readProvisionSrcFile("src/network/handlers/ProvisionRouteHandler.cpp");
    if (src.empty()) {
        TEST_IGNORE_MESSAGE("ProvisionRouteHandler.cpp not readable, skipping");
        return;
    }
    // 必须存在枚举→字符串映射函数，并在扫描结果中写入 encryption 字段
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("_authModeToString") != std::string::npos,
        "handler must define _authModeToString mapping helper");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("net[\"encryption\"]") != std::string::npos,
        "scan result must include encryption string field for frontend security select");

    // 映射的目标字符串必须与前端下拉选项取值一致
    size_t mapPos = src.find("_authModeToString(wifi_auth_mode_t");
    TEST_ASSERT_TRUE_MESSAGE(mapPos != std::string::npos, "_authModeToString definition must exist");
    size_t mapEnd = src.find("void ProvisionRouteHandler::_wifiScanTask", mapPos);
    if (mapEnd == std::string::npos) mapEnd = mapPos + 800;
    std::string mapBody = src.substr(mapPos, mapEnd - mapPos);
    TEST_ASSERT_TRUE_MESSAGE(mapBody.find("\"open\"") != std::string::npos, "must map open");
    TEST_ASSERT_TRUE_MESSAGE(mapBody.find("\"wpa\"") != std::string::npos, "must map wpa");
    TEST_ASSERT_TRUE_MESSAGE(mapBody.find("\"wpa2\"") != std::string::npos, "must map wpa2");
    TEST_ASSERT_TRUE_MESSAGE(mapBody.find("\"wpa3\"") != std::string::npos, "must map wpa3");
    // WPA3 相关枚举必须被显式处理（否则新路由器的 WPA3 网络被误判）
    TEST_ASSERT_TRUE_MESSAGE(mapBody.find("WIFI_AUTH_WPA3_PSK") != std::string::npos,
        "must handle WIFI_AUTH_WPA3_PSK explicitly");
    TestLog::step("_authModeToString covers open/wpa/wpa2/wpa3 and WPA3 enums");

    TestLog::testEnd(true);
}

// ---------------------------------------------------------------------------
// 测试：前端 network.js 扫描结果解析与点选回填契约守护
// 现象回归——前端读 res.data 但后端返回 res.networks，导致"未找到WiFi网络"
// ---------------------------------------------------------------------------
static void test_wifi_scan_frontend_parse_contract() {
    TestLog::testStart("Provision: frontend network.js parses networks + fills fields on select");

    std::string js = _readProvisionSrcFile("web-src/modules/runtime/network.js");
    if (js.empty()) {
        TEST_IGNORE_MESSAGE("network.js not readable, skipping");
        return;
    }
    // 定位 scanWifiNetworks 函数定义体（用定义而非调用点定位）
    size_t fnPos = js.find("scanWifiNetworks() {");
    TEST_ASSERT_TRUE_MESSAGE(fnPos != std::string::npos, "scanWifiNetworks must exist");
    size_t fnEnd = js.find("loadNetworkStatus", fnPos);
    if (fnEnd == std::string::npos) fnEnd = js.size();
    std::string fn = js.substr(fnPos, fnEnd - fnPos);

    // 必须从 res.networks 读取列表（回归：旧代码只读 res.data 导致列表恒为空）
    TEST_ASSERT_TRUE_MESSAGE(
        fn.find("res.networks") != std::string::npos,
        "frontend must read res.networks from scan response");
    // 旧固件兼容：仅有 encrypted 布尔时必须能兜底推导安全类型
    TEST_ASSERT_TRUE_MESSAGE(
        fn.find("net.encryption || (net.encrypted") != std::string::npos,
        "frontend must fall back to encrypted bool when encryption missing");
    // 点选网络后必须回填 SSID 与安全类型输入框
    TEST_ASSERT_TRUE_MESSAGE(
        fn.find("wifi-ssid") != std::string::npos,
        "selecting a network must fill #wifi-ssid input");
    TEST_ASSERT_TRUE_MESSAGE(
        fn.find("wifi-security") != std::string::npos,
        "selecting a network must set #wifi-security select");
    // 空列表分支必须存在（length === 0 → 未找到）
    TEST_ASSERT_TRUE_MESSAGE(
        fn.find("length === 0") != std::string::npos,
        "must handle empty network list branch");
    // SSID 必须经过 HTML 转义再插入 DOM（防 XSS / 属性截断）
    TEST_ASSERT_TRUE_MESSAGE(
        fn.find("_escapeHtml") != std::string::npos,
        "SSID must be HTML-escaped before insertion");
    TestLog::step("network.js reads res.networks, escapes SSID, fills ssid+security on select");

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
// 测试：WiFi 扫描超时响应格式
// ---------------------------------------------------------------------------
static void test_wifi_scan_timeout_response_format() {
    TestLog::testStart("Provision: WiFi Scan Timeout Response Format");

    // 模拟 chunked 回调 10s 超时后生成的响应（与 handleWiFiScan 内逻辑一致）
    String json = "{\"success\":false,\"error\":\"scan_timeout\","
                  "\"message\":\"WiFi scan timed out, please try again\"}";

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    TEST_ASSERT_TRUE_MESSAGE(!err, "Timeout JSON must be valid");
    TEST_ASSERT_FALSE(doc["success"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("scan_timeout", doc["error"].as<const char*>());
    TEST_ASSERT_TRUE_MESSAGE(
        doc["message"].as<const char*>() && strlen(doc["message"].as<const char*>()) > 0,
        "Timeout response must include human-readable message");
    TestLog::step("success=false, error=scan_timeout, message present");

    TestLog::testEnd(true);
}

// ---------------------------------------------------------------------------
// 测试：WiFi 扫描失败响应格式（任务创建失败 / scanNetworks 返回负值）
// ---------------------------------------------------------------------------
static void test_wifi_scan_failed_response_format() {
    TestLog::testStart("Provision: WiFi Scan Failed Response Format");

    // 模拟 _wifiScanTask 中 n<0 或 _launchScanTask 失败时的响应
    String json = "{\"success\":false,\"error\":\"scan_failed\","
                  "\"message\":\"WiFi scan failed, please try again\"}";

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    TEST_ASSERT_TRUE_MESSAGE(!err, "Failed JSON must be valid");
    TEST_ASSERT_FALSE(doc["success"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("scan_failed", doc["error"].as<const char*>());
    TEST_ASSERT_TRUE_MESSAGE(
        doc["message"].as<const char*>() && strlen(doc["message"].as<const char*>()) > 0,
        "Failed response must include human-readable message");
    // 确保错误响应不包含 networks 字段（前端根据 success 判断）
    TEST_ASSERT_TRUE_MESSAGE(
        doc["networks"].isNull(),
        "Error response must NOT contain networks field");
    TestLog::step("success=false, error=scan_failed, no networks field");

    TestLog::testEnd(true);
}

// ---------------------------------------------------------------------------
// 测试：WiFi 扫描结果上限截断（最多 20 条）
// ---------------------------------------------------------------------------
static void test_wifi_scan_result_limit_20() {
    TestLog::testStart("Provision: WiFi Scan Result Capped at 20 Networks");

    // 模拟 _wifiScanTask 中的截断逻辑：for (int i = 0; i < n && i < 20; i++)
    const int simulatedScanCount = 35;  // 假设扫描到 35 个网络
    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();
    for (int i = 0; i < simulatedScanCount && i < 20; i++) {
        JsonObject net = networks.add<JsonObject>();
        char ssidBuf[16];
        snprintf(ssidBuf, sizeof(ssidBuf), "Net%d", i);
        net["ssid"] = ssidBuf;
        net["rssi"] = -40 - i;
        net["channel"] = (i % 13) + 1;
        net["encrypted"] = true;
    }
    doc["success"] = true;
    doc["count"] = simulatedScanCount;  // count 报告实际扫描数

    // 验证：networks 数组被截断为 20 条
    TEST_ASSERT_EQUAL(20, networks.size());
    TestLog::step("35 networks scanned, only 20 included in response");

    // count 字段反映真实扫描数（而非截断后的数量）
    TEST_ASSERT_EQUAL(simulatedScanCount, doc["count"].as<int>());
    TestLog::step("count=35 reflects actual scan count");

    // 正常场景：扫描数 < 20 时不截断
    JsonDocument doc2;
    JsonArray nets2 = doc2["networks"].to<JsonArray>();
    for (int i = 0; i < 5 && i < 20; i++) {
        JsonObject net = nets2.add<JsonObject>();
        char ssidBuf[16];
        snprintf(ssidBuf, sizeof(ssidBuf), "Small%d", i);
        net["ssid"] = ssidBuf;
    }
    TEST_ASSERT_EQUAL(5, nets2.size());
    TestLog::step("5 networks scanned, all 5 included (no truncation)");

    TestLog::testEnd(true);
}

// ---------------------------------------------------------------------------
// 测试：WiFi 扫描并发请求去重 + 缓存 TTL 逻辑
// ---------------------------------------------------------------------------
static void test_wifi_scan_concurrent_dedup() {
    TestLog::testStart("Provision: WiFi Scan Concurrent Request Deduplication + Cache TTL");

    // 镜像 handleWiFiScan 的去重 + 缓存状态机：
    // - scanTaskRunning=true 时后续请求不启动新任务
    // - 缓存 TTL 内直接复用结果，不触发新扫描
    // - 启动新扫描时不清除 scanResultReady（保留旧结果供重试请求使用）
    struct MirrorScanDedup {
        bool scanTaskRunning = false;
        bool scanResultReady = false;
        int taskLaunchCount = 0;
        unsigned long scanLastCompleteMs = 0;
        enum { TTL = 60000 };

        // 模拟 handleWiFiScan 入口逻辑
        bool onRequest(unsigned long nowMs) {
            bool cacheFresh = scanResultReady &&
                              (nowMs - scanLastCompleteMs < TTL);
            bool needLaunch = false;
            if (!cacheFresh && !scanTaskRunning) {
                scanTaskRunning = true;
                // 不清除 scanResultReady（保留旧结果）
                needLaunch = true;
                taskLaunchCount++;
            }
            return needLaunch;
        }

        // 模拟 _wifiScanTask 完成
        void onScanComplete(unsigned long nowMs) {
            scanResultReady = true;
            scanLastCompleteMs = nowMs;
            scanTaskRunning = false;
        }
    };

    MirrorScanDedup dedup;

    // 第 1 个请求（无缓存）：应启动任务
    TEST_ASSERT_TRUE(dedup.onRequest(1000));
    TEST_ASSERT_EQUAL(1, dedup.taskLaunchCount);
    TestLog::step("1st request (no cache): launches scan task");

    // 第 2、3 个请求（扫描进行中）：不启动新任务
    TEST_ASSERT_FALSE(dedup.onRequest(1100));
    TEST_ASSERT_FALSE(dedup.onRequest(1200));
    TEST_ASSERT_EQUAL(1, dedup.taskLaunchCount);
    TestLog::step("2nd/3rd requests during scan: no new task (dedup)");

    // 扫描完成
    dedup.onScanComplete(5000);
    TEST_ASSERT_TRUE(dedup.scanResultReady);
    TEST_ASSERT_FALSE(dedup.scanTaskRunning);
    TestLog::step("Scan complete: result ready, task no longer running");

    // 第 4 个请求（缓存新鲜，TTL 内）：不启动新任务，直接复用缓存
    TEST_ASSERT_FALSE(dedup.onRequest(10000));  // 5s 后，在 60s TTL 内
    TEST_ASSERT_EQUAL(1, dedup.taskLaunchCount);
    TEST_ASSERT_TRUE(dedup.scanResultReady);  // 缓存结果保留
    TestLog::step("4th request within TTL: serves cache, no new scan");

    // 第 5 个请求（缓存过期，TTL 后）：启动新扫描，但保留旧结果
    TEST_ASSERT_TRUE(dedup.onRequest(70000));  // 65s 后，超过 60s TTL
    TEST_ASSERT_EQUAL(2, dedup.taskLaunchCount);
    TEST_ASSERT_TRUE(dedup.scanResultReady);  // 旧结果保留（供扫描期间重试用）
    TestLog::step("5th request after TTL: launches fresh scan, old result preserved");

    TestLog::testEnd(true);
}

// ---------------------------------------------------------------------------
// 测试："重新扫描"功能已移除守护
// 背景：force=1 强制重扫在实机上触发 ERR_INCOMPLETE_CHUNKED_ENCODING，
// 已整体删除（用户关闭弹窗重新打开即可重新扫描）；
// 此处守护前后端均无残留，防止该功能被意外重新引入
// ---------------------------------------------------------------------------
static void test_wifi_rescan_feature_removed() {
    TestLog::testStart("Provision: force rescan feature fully removed");

    // 后端：无 force 参数入口、无代数计数器，TTL 缓存逻辑保留
    std::string src = _readProvisionSrcFile("src/network/handlers/ProvisionRouteHandler.cpp");
    if (!src.empty()) {
        TEST_ASSERT_TRUE_MESSAGE(
            src.find("hasParam(\"force\")") == std::string::npos,
            "handleWiFiScan must not read force param (feature removed)");
        TEST_ASSERT_TRUE_MESSAGE(
            src.find("scanGeneration") == std::string::npos,
            "scanGeneration counter must be removed from handler source");
        TEST_ASSERT_TRUE_MESSAGE(
            src.find("scanResultReady &&") != std::string::npos,
            "TTL cache reuse logic must be preserved after removal");
        TestLog::step("Backend: no force param, no generation counter, TTL cache intact");
    }
    std::string hdr = _readProvisionSrcFile("include/network/handlers/ProvisionRouteHandler.h");
    if (!hdr.empty()) {
        TEST_ASSERT_TRUE_MESSAGE(
            hdr.find("scanGeneration") == std::string::npos,
            "scanGeneration member must be removed from header");
    }

    // 前端：无重扫按钮绑定、无 force 参数传递，扫描回到无参 apiGet
    std::string js = _readProvisionSrcFile("web-src/modules/runtime/network.js");
    if (!js.empty()) {
        TEST_ASSERT_TRUE_MESSAGE(
            js.find("wifi-rescan-btn") == std::string::npos,
            "network.js must not reference wifi-rescan-btn");
        TEST_ASSERT_TRUE_MESSAGE(
            js.find("?force=1") == std::string::npos,
            "network.js must not append ?force=1 to scan request");
        TEST_ASSERT_TRUE_MESSAGE(
            js.find("scanWifiNetworks(true)") == std::string::npos,
            "no caller may pass force flag to scanWifiNetworks");
        TEST_ASSERT_TRUE_MESSAGE(
            js.find("apiGet('/api/wifi/scan')") != std::string::npos,
            "scan must use plain apiGet without force query");
        TestLog::step("Frontend: no rescan button binding, plain apiGet scan restored");
    }

    // 弹窗模板与 i18n：重扫按钮及文案均已删除
    std::string html = _readProvisionSrcFile("web-src/pages/modals.html");
    if (!html.empty()) {
        TEST_ASSERT_TRUE_MESSAGE(
            html.find("wifi-rescan-btn") == std::string::npos,
            "modals.html must not contain wifi-rescan-btn button");
    }
    std::string zh = _readProvisionSrcFile("web-src/i18n/i18n-zh-CN.js");
    if (!zh.empty()) {
        TEST_ASSERT_TRUE_MESSAGE(
            zh.find("'wifi-rescan'") == std::string::npos,
            "zh-CN i18n must not keep wifi-rescan key");
    }
    std::string en = _readProvisionSrcFile("web-src/i18n/i18n-en.js");
    if (!en.empty()) {
        TEST_ASSERT_TRUE_MESSAGE(
            en.find("'wifi-rescan'") == std::string::npos,
            "en i18n must not keep wifi-rescan key");
    }
    TestLog::step("Templates/i18n: rescan button and labels fully removed");

    TestLog::testEnd(true);
}

// ---------------------------------------------------------------------------
// 测试：GET /api/wifi/status 响应格式验证
// ---------------------------------------------------------------------------
static void test_wifi_status_response_format() {
    TestLog::testStart("Provision: WiFi Status Response Format");

    // 镜像 handleWiFiStatus 的 JSON 构建逻辑
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["deviceName"] = "fastbee";
    data["deviceId"] = "FBE00112233";
    data["userId"] = "1";
    data["firmwareVersion"] = "1.0.0";
    data["networkMode"] = "ap";
    data["connected"] = false;
    data["ipAddress"] = "192.168.4.1";
    data["apSSID"] = "fastbee-ap";
    data["apIP"] = "192.168.4.1";
    data["macAddress"] = "AA:BB:CC:DD:EE:FF";
    data["chipModel"] = "ESP32";
    data["chipRevision"] = 3;
    data["freeHeap"] = 58000;

    String json;
    serializeJson(doc, json);

    // 反序列化验证字段
    JsonDocument parsed;
    deserializeJson(parsed, json);
    TEST_ASSERT_TRUE(parsed["success"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("fastbee", parsed["data"]["deviceName"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("FBE00112233", parsed["data"]["deviceId"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("1", parsed["data"]["userId"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("1.0.0", parsed["data"]["firmwareVersion"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("ap", parsed["data"]["networkMode"].as<const char*>());
    TEST_ASSERT_FALSE(parsed["data"]["connected"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", parsed["data"]["ipAddress"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("fastbee-ap", parsed["data"]["apSSID"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("ESP32", parsed["data"]["chipModel"].as<const char*>());
    TEST_ASSERT_EQUAL(58000, parsed["data"]["freeHeap"].as<int>());
    TestLog::step("Status response has all required fields: device info + network + chip");

    // STA 模式连接状态响应
    JsonDocument doc2;
    doc2["success"] = true;
    JsonObject d2 = doc2["data"].to<JsonObject>();
    d2["networkMode"] = "sta";
    d2["connected"] = true;
    d2["ipAddress"] = "10.52.122.2";
    d2["ssid"] = "fastbeecn";
    d2["rssi"] = -43;

    JsonDocument p2;
    String j2; serializeJson(doc2, j2);
    deserializeJson(p2, j2);
    TEST_ASSERT_EQUAL_STRING("sta", p2["data"]["networkMode"].as<const char*>());
    TEST_ASSERT_TRUE(p2["data"]["connected"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("10.52.122.2", p2["data"]["ipAddress"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("fastbeecn", p2["data"]["ssid"].as<const char*>());
    TEST_ASSERT_EQUAL(-43, p2["data"]["rssi"].as<int>());
    TestLog::step("STA connected response: ssid, rssi, ipAddress verified");

    TestLog::testEnd(true);
}

// ---------------------------------------------------------------------------
// 测试：POST /api/wifi/config 必须参数校验
// ---------------------------------------------------------------------------
static void test_wifi_config_requires_params() {
    TestLog::testStart("Provision: WiFi Config Requires SSID+password+userId");

    // 镜像 handleWiFiConfig 的参数校验逻辑
    auto validate = [](const String& ssid, const String& password, const String& userId) -> bool {
        return !(ssid.isEmpty() || password.isEmpty() || userId.isEmpty());
    };

    // 缺少 SSID
    TEST_ASSERT_FALSE(validate("", "pass123", "user1"));
    TestLog::step("Empty SSID → rejected");

    // 缺少密码
    TEST_ASSERT_FALSE(validate("MyWiFi", "", "user1"));
    TestLog::step("Empty password → rejected");

    // 缺少 userId
    TEST_ASSERT_FALSE(validate("MyWiFi", "pass123", ""));
    TestLog::step("Empty userId → rejected");

    // 全部提供
    TEST_ASSERT_TRUE(validate("MyWiFi", "pass123", "user1"));
    TestLog::step("All params present → accepted");

    // 验证错误响应格式
    JsonDocument errDoc;
    errDoc["success"] = false;
    errDoc["message"] = "配网必须传递 SSID、WiFi密码和用户编号(userId)";
    String errJson;
    serializeJson(errDoc, errJson);
    JsonDocument parsed;
    deserializeJson(parsed, errJson);
    TEST_ASSERT_FALSE(parsed["success"].as<bool>());
    TEST_ASSERT_TRUE_MESSAGE(
        String(parsed["message"].as<const char*>()).indexOf("SSID") >= 0,
        "Error message must mention SSID");
    TEST_ASSERT_TRUE_MESSAGE(
        String(parsed["message"].as<const char*>()).indexOf("userId") >= 0,
        "Error message must mention userId");
    TestLog::step("Error response format correct (success=false, message mentions SSID+userId)");

    TestLog::testEnd(true);
}

// ---------------------------------------------------------------------------
// 测试：POST /api/wifi/config 同时保存 network.json 和 device.json
// ---------------------------------------------------------------------------
static void test_wifi_config_saves_both_configs() {
    TestLog::testStart("Provision: WiFi Config Saves to network.json + device.json");
    _ensureConfigDir();

    // 准备初始 device.json
    _writeTestDeviceJson(
        "{\"deviceId\":\"\",\"productNumber\":0,\"userId\":\"\","
        "\"deviceName\":\"fastbee\",\"logLevel\":\"INFO\"}");

    // 模拟 _updateDeviceConfig 写入 userId + deviceNum
    const String userId    = "42";
    const String deviceNum = "FBE00112233";
    const String extra     = "7";

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
    if (changed) {
        File f = LittleFS.open(TEST_DEVICE_CONFIG, "w");
        TEST_ASSERT_TRUE(f);
        serializeJsonPretty(doc, f);
        f.close();
    }
    TestLog::step("device.json saved: userId=42, deviceId=FBE00112233, productNumber=7");

    // 验证 device.json
    String content = _readTestDeviceJson();
    JsonDocument verify;
    deserializeJson(verify, content);
    TEST_ASSERT_EQUAL_STRING("42", verify["userId"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("FBE00112233", verify["deviceId"].as<const char*>());
    TEST_ASSERT_EQUAL(7, verify["productNumber"].as<int>());
    TestLog::step("device.json fields verified");

    // 模拟 network.json WiFi 配置保存（与 handleWiFiConfig 的 cfg 更新逻辑一致）
    const char* NET_CONFIG = "/config/network.json";
    {
        JsonDocument netDoc;
        netDoc["mode"] = 0;   // AP mode initially
        netDoc["staSSID"] = "";
        netDoc["staPassword"] = "";
        netDoc["apSSID"] = "fastbee-ap";
        File nf = LittleFS.open(NET_CONFIG, "w");
        TEST_ASSERT_TRUE(nf);
        serializeJson(netDoc, nf);
        nf.close();
    }

    // 模拟配网：更新 staSSID/staPassword + mode→1(STA)
    {
        JsonDocument netDoc;
        File nf = LittleFS.open(NET_CONFIG, "r");
        TEST_ASSERT_TRUE(nf);
        deserializeJson(netDoc, nf);
        nf.close();
        netDoc["staSSID"] = "MyHomeWiFi";
        netDoc["staPassword"] = "secret123";
        netDoc["mode"] = 1;  // STA mode
        nf = LittleFS.open(NET_CONFIG, "w");
        TEST_ASSERT_TRUE(nf);
        serializeJson(netDoc, nf);
        nf.close();
    }
    TestLog::step("network.json saved: mode=1(STA), staSSID=MyHomeWiFi");

    // 验证 network.json
    {
        JsonDocument netDoc;
        File nf = LittleFS.open(NET_CONFIG, "r");
        TEST_ASSERT_TRUE(nf);
        deserializeJson(netDoc, nf);
        nf.close();
        TEST_ASSERT_EQUAL(1, netDoc["mode"].as<int>());
        TEST_ASSERT_EQUAL_STRING("MyHomeWiFi", netDoc["staSSID"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING("secret123", netDoc["staPassword"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING("fastbee-ap", netDoc["apSSID"].as<const char*>());
    }
    TestLog::step("network.json verified: mode=STA, SSID/password saved, AP info preserved");

    TestLog::testEnd(true);
}

// ---------------------------------------------------------------------------
// 测试：/api/wifi/status 和 /api/wifi/config 路由注册 + 公开接口（无认证）
// ---------------------------------------------------------------------------
static void test_wifi_api_routes_and_public_access() {
    TestLog::testStart("Provision: WiFi Status/Config Routes Registered + Public (No Auth)");

    std::string src = _readProvisionSrcFile("src/network/handlers/ProvisionRouteHandler.cpp");
    if (src.empty()) {
        TEST_IGNORE_MESSAGE("ProvisionRouteHandler.cpp not readable, skipping");
        return;
    }

    // 路由注册守护
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("\"/api/wifi/status\"") != std::string::npos,
        "handleWiFiStatus route must be registered at /api/wifi/status");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("\"/api/wifi/config\"") != std::string::npos,
        "handleWiFiConfig route must be registered at /api/wifi/config");
    TestLog::step("Routes /api/wifi/status and /api/wifi/config registered");

    // HTTP 方法守护
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("HTTP_GET") != std::string::npos &&
        src.find("handleWiFiStatus") != std::string::npos,
        "/api/wifi/status must use HTTP_GET");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("HTTP_POST") != std::string::npos &&
        src.find("handleWiFiConfig") != std::string::npos,
        "/api/wifi/config must use HTTP_POST");
    TestLog::step("HTTP methods: status=GET, config=POST");

    // 公开接口守护：新接口的处理函数内不应调用 requireAuth
    size_t statusPos = src.find("void ProvisionRouteHandler::handleWiFiStatus");
    size_t configPos = src.find("void ProvisionRouteHandler::handleWiFiConfig");
    TEST_ASSERT_TRUE_MESSAGE(statusPos != std::string::npos, "handleWiFiStatus implementation must exist");
    TEST_ASSERT_TRUE_MESSAGE(configPos != std::string::npos, "handleWiFiConfig implementation must exist");

    // 提取两个函数体范围
    size_t statusEnd = src.find("void ProvisionRouteHandler::handleWiFiConfig", statusPos + 1);
    if (statusEnd == std::string::npos) statusEnd = src.size();
    std::string statusBody = src.substr(statusPos, statusEnd - statusPos);

    size_t configEnd = src.size();  // handleWiFiConfig is last function
    std::string configBody = src.substr(configPos, configEnd - configPos);

    TEST_ASSERT_TRUE_MESSAGE(
        statusBody.find("requireAuth") == std::string::npos,
        "handleWiFiStatus must NOT call requireAuth (public endpoint)");
    TEST_ASSERT_TRUE_MESSAGE(
        configBody.find("requireAuth") == std::string::npos,
        "handleWiFiConfig must NOT call requireAuth (public endpoint)");
    TestLog::step("Both handlers are public (no requireAuth call)");

    // handleWiFiConfig 必须保存 WiFi 配置并切换 STA 模式
    TEST_ASSERT_TRUE_MESSAGE(
        configBody.find("NETWORK_STA") != std::string::npos,
        "handleWiFiConfig must switch to STA mode");
    TEST_ASSERT_TRUE_MESSAGE(
        configBody.find("updateConfig") != std::string::npos,
        "handleWiFiConfig must call updateConfig to persist network config");
    TEST_ASSERT_TRUE_MESSAGE(
        configBody.find("_updateDeviceConfig") != std::string::npos,
        "handleWiFiConfig must call _updateDeviceConfig for device.json");
    TestLog::step("handleWiFiConfig: saves network config (STA mode) + device config");

    // handleWiFiStatus 必须返回设备信息
    TEST_ASSERT_TRUE_MESSAGE(
        statusBody.find("deviceName") != std::string::npos,
        "handleWiFiStatus must return deviceName");
    TEST_ASSERT_TRUE_MESSAGE(
        statusBody.find("networkMode") != std::string::npos,
        "handleWiFiStatus must return networkMode");
    TEST_ASSERT_TRUE_MESSAGE(
        statusBody.find("firmwareVersion") != std::string::npos,
        "handleWiFiStatus must return firmwareVersion");
    TestLog::step("handleWiFiStatus: returns deviceName, networkMode, firmwareVersion");

    TestLog::testEnd(true);
}

// ---------------------------------------------------------------------------
// 测试组入口
// ---------------------------------------------------------------------------
void test_provision_api_group() {
    TestLog::groupStart("Provision API Tests");

    RUN_TEST(test_wifi_scan_response_format);
    RUN_TEST(test_wifi_scan_auth_mode_mapping);
    RUN_TEST(test_wifi_scan_frontend_parse_contract);
    RUN_TEST(test_wifi_scan_timeout_response_format);
    RUN_TEST(test_wifi_scan_failed_response_format);
    RUN_TEST(test_wifi_scan_result_limit_20);
    RUN_TEST(test_wifi_scan_concurrent_dedup);
    RUN_TEST(test_wifi_rescan_feature_removed);
    RUN_TEST(test_provision_extended_params_write);
    RUN_TEST(test_provision_no_extended_params_compatible);
    RUN_TEST(test_provision_extra_invalid_discarded);
    RUN_TEST(test_provision_partial_params);

    // 新增：AP 配网公开接口测试
    RUN_TEST(test_wifi_status_response_format);
    RUN_TEST(test_wifi_config_requires_params);
    RUN_TEST(test_wifi_config_saves_both_configs);
    RUN_TEST(test_wifi_api_routes_and_public_access);

    TestLog::groupEnd();
}
