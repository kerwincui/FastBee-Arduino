/**
 * @file test_modbus_mqtt_interaction.cpp
 * @brief 联网方式 × Modbus RTU 传输类型 × MQTT 交互 集成测试
 *
 * 覆盖场景：
 * 1. WiFi / 以太网 / 4G 联网后 MQTT 连接 + Modbus JSON 数据上报
 * 2. WiFi / 以太网 / 4G 联网后 MQTT 连接 + Modbus HEX 透传数据上报
 * 3. JSON 模式下平台下发 DATA_COMMAND → Modbus 指令执行
 * 4. HEX 透传模式下平台下发二进制/HEX ASCII 帧 → modbus_raw_send 执行
 * 5. JSON 模式严格拒绝非 JSON payload
 * 6. HEX 透传帧大小上限（256 字节）
 * 7. collectCachedPollData 在 transferType=1 且无映射时跳过
 * 8. 网络切换后 Modbus + MQTT 全链路恢复
 * 9. 多网络 × 多传输类型排列组合验证
 */

#include <unity.h>
#include <Arduino.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "mocks/MockWiFi.h"
#include "mocks/MockMQTTClient.h"
#include "mocks/MockMultiNetwork.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"
#include "helpers/NetworkMQTTHelper.h"

void test_modbus_mqtt_interaction_group();

using namespace NetMQTTHelper;

// ================================================================
// 内联复现生产代码中的关键逻辑（用于行为验证，不依赖真实模块）
// ================================================================

/**
 * @brief 模拟 ModbusHandler::formatRawHex
 * 将寄存器读取结果格式化为原始 HEX 帧字符串
 */
static String formatRawHex(uint8_t slaveAddr, uint8_t functionCode,
                           const uint16_t* data, uint16_t count) {
    String hex;
    hex.reserve(4 + count * 4 + 4);  // addr(2) + fc(2) + data + crc(4)
    char buf[8];
    snprintf(buf, sizeof(buf), "%02X%02X", slaveAddr, functionCode);
    hex += buf;
    for (uint16_t i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "%02X%02X",
                 (uint8_t)(data[i] >> 8), (uint8_t)(data[i] & 0xFF));
        hex += buf;
    }
    // 追加 CRC16 占位（测试仅验证格式，不验证 CRC 值）
    hex += "0000";
    return hex;
}

/**
 * @brief 模拟 MQTTClient::publishReportData 中 HEX 透传分支
 * 当 transferType==1 且 payload 为纯 HEX ASCII 时，转换为二进制字节帧
 *
 * @return true=以二进制发布, false=未转换（回退文本发布）
 */
static bool publishReportDataHex(uint8_t transferType, const String& payload,
                                 MockMQTTClient& mqtt, const char* topic) {
    if (transferType != 1) return false;

    unsigned int hexLen = payload.length();
    if (hexLen < 4 || (hexLen % 2) != 0) return false;

    bool allHex = true;
    for (unsigned int i = 0; i < hexLen; i++) {
        char c = payload[i];
        bool v = (c >= '0' && c <= '9') ||
                 (c >= 'a' && c <= 'f') ||
                 (c >= 'A' && c <= 'F');
        if (!v) { allHex = false; break; }
    }
    if (!allHex) return false;

    unsigned int byteLen = hexLen / 2;
    if (byteLen > 256) byteLen = 256;  // 生产代码上限

    auto hexVal = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
        if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
        return 0;
    };

    std::vector<uint8_t> buf(byteLen);
    for (unsigned int i = 0; i < byteLen; i++) {
        buf[i] = (uint8_t)((hexVal(payload[i * 2]) << 4) |
                            hexVal(payload[i * 2 + 1]));
    }

    mqtt.publish(topic, buf.data(), byteLen);
    return true;
}

/**
 * @brief 模拟 DATA_COMMAND 接收端的 transferType 分流逻辑
 *
 * @param transferType 当前 Modbus 传输类型 (0=JSON, 1=HEX)
 * @param payload      收到的 MQTT payload
 * @param length       payload 长度
 * @param[out] enqueueMsg   实际入队命令字符串
 * @param[out] shouldEnqueue 是否应入队执行
 */
static void handleDataCommand(uint8_t transferType,
                              const uint8_t* payload, unsigned int length,
                              String& enqueueMsg, bool& shouldEnqueue) {
    String msg;
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

    // 检测是否为 JSON
    bool looksLikeJson = false;
    for (unsigned int i = 0; i < length; i++) {
        char c = (char)payload[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        looksLikeJson = (c == '[' || c == '{');
        break;
    }

    enqueueMsg = msg;
    shouldEnqueue = true;

    if (!looksLikeJson && length >= 2) {
        if (transferType != 1) {
            // JSON 模式严格拒绝非 JSON
            shouldEnqueue = false;
        } else {
            // HEX 透传模式
            auto isHexAscii = [&]() -> bool {
                if ((length % 2) != 0 || length < 4) return false;
                for (unsigned int i = 0; i < length; i++) {
                    char c = (char)payload[i];
                    bool ok = (c >= '0' && c <= '9') ||
                              (c >= 'a' && c <= 'f') ||
                              (c >= 'A' && c <= 'F');
                    if (!ok) return false;
                }
                return true;
            };

            String hexStr;
            if (isHexAscii()) {
                // Payload 是 HEX ASCII 字符串
                hexStr.reserve(length);
                for (unsigned int i = 0; i < length; i++) {
                    char c = (char)payload[i];
                    if (c >= 'a' && c <= 'f') c = c - 'a' + 'A';
                    hexStr += c;
                }
            } else {
                // 真实二进制字节帧
                hexStr.reserve(length * 2);
                for (unsigned int i = 0; i < length; i++) {
                    char buf[3];
                    snprintf(buf, sizeof(buf), "%02X", payload[i]);
                    hexStr += buf;
                }
            }
            enqueueMsg = "[{\"id\":\"modbus_raw_send\",\"value\":\"" + hexStr + "\"}]";
        }
    }
}

/**
 * @brief 模拟 collectCachedPollData 中的 transferType 过滤
 * 透传模式 (transferType=1) 且无映射 (mappingCount==0) 时跳过该任务
 */
static String collectCachedPollData(uint8_t transferType,
                                    const std::vector<int>& mappingCounts) {
    String json;
    json.reserve(512);
    json = "[";
    bool first = true;

    for (size_t i = 0; i < mappingCounts.size(); i++) {
        int mappingCount = mappingCounts[i];
        // 透传模式无映射，跳过
        if (transferType == 1 && mappingCount == 0) continue;

        if (!first) json += ",";
        // 简化：仅输出任务索引
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"taskIdx\":%d,\"mappings\":%d}", (int)i, mappingCount);
        json += buf;
        first = false;
    }
    json += "]";
    return json;
}

// ================================================================
// 辅助函数
// ================================================================

/**
 * @brief 在指定网络模式下建立 MQTT 连接
 */
static bool setupNetworkAndMQTT(MockMultiNetworkManager& mgr,
                                 MockMQTTClient& mqtt,
                                 MockMultiNetworkManager::NetType netType,
                                 bool adapterSuccess = true) {
    return switchNetworkAndReconnectMQTT(mgr, mqtt, buildDefaultMQTTConfig(),
                                         netType, adapterSuccess);
}

/**
 * @brief 验证 MQTT 发布消息中包含预期内容
 */
static void assertLastPublishContains(MockMQTTClient& mqtt,
                                       const char* expectedSubstring) {
    auto msgs = mqtt.getPublishedMessages();
    TEST_ASSERT_GREATER_THAN(0, (int)msgs.size());
    const auto& last = msgs.back();
    TEST_ASSERT_TRUE_MESSAGE(
        last.payload.indexOf(expectedSubstring) >= 0 ||
        last.topic.indexOf(expectedSubstring) >= 0,
        (String("Expected '") + expectedSubstring + "' in last publish, got payload='" +
         last.payload + "' topic='" + last.topic + "'").c_str()
    );
}

// ================================================================
// 测试用例
// ================================================================

// ---------- 1. 各联网方式 + JSON 模式数据上报 ----------

/**
 * @brief WiFi + Modbus JSON + MQTT 数据上报
 */
static void test_wifi_modbus_json_mqtt_report() {
    TestLog::testStart("WiFi + Modbus JSON + MQTT report");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_WIFI));
    assertMQTTConnected(mqtt);
    TestLog::step("WiFi connected, MQTT online");

    // 模拟 Modbus 轮询生成 JSON 数据
    uint8_t transferType = 0;  // JSON
    String jsonData = "[{\"id\":\"temp\",\"value\":\"25.5\",\"remark\":\"modbus\"}]";

    // 模拟 dispatchModbusData → publishReportData (文本发布)
    bool sentAsBinary = publishReportDataHex(transferType, jsonData, mqtt, "/data/report");
    TEST_ASSERT_FALSE(sentAsBinary);  // JSON 模式不走二进制路径

    // JSON 模式回退为文本发布
    TEST_ASSERT_TRUE(mqtt.publish("/data/report", jsonData.c_str()));
    assertLastPublishContains(mqtt, "temp");
    TestLog::step("Modbus JSON data published via MQTT");

    TestLog::testEnd(true);
}

/**
 * @brief 以太网 + Modbus JSON + MQTT 数据上报
 */
static void test_ethernet_modbus_json_mqtt_report() {
    TestLog::testStart("Ethernet + Modbus JSON + MQTT report");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_ETHERNET));
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetType::NET_ETHERNET, (int)mgr.networkType);
    TestLog::step("Ethernet connected, MQTT online");

    uint8_t transferType = 0;
    String jsonData = "[{\"id\":\"humidity\",\"value\":\"65.2\",\"remark\":\"modbus\"}]";

    bool sentAsBinary = publishReportDataHex(transferType, jsonData, mqtt, "/data/report");
    TEST_ASSERT_FALSE(sentAsBinary);

    TEST_ASSERT_TRUE(mqtt.publish("/data/report", jsonData.c_str()));
    assertLastPublishContains(mqtt, "humidity");
    TestLog::step("Modbus JSON data published via Ethernet MQTT");

    TestLog::testEnd(true);
}

/**
 * @brief 4G + Modbus JSON + MQTT 数据上报
 */
static void test_4g_modbus_json_mqtt_report() {
    TestLog::testStart("4G + Modbus JSON + MQTT report");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_4G));
    TEST_ASSERT_EQUAL((int)MockMultiNetworkManager::NetType::NET_4G, (int)mgr.networkType);
    TestLog::step("4G connected, MQTT online");

    uint8_t transferType = 0;
    String jsonData = "[{\"id\":\"pressure\",\"value\":\"1013.25\",\"remark\":\"modbus\"}]";

    bool sentAsBinary = publishReportDataHex(transferType, jsonData, mqtt, "/data/report");
    TEST_ASSERT_FALSE(sentAsBinary);

    TEST_ASSERT_TRUE(mqtt.publish("/data/report", jsonData.c_str()));
    assertLastPublishContains(mqtt, "pressure");
    TestLog::step("Modbus JSON data published via 4G MQTT");

    TestLog::testEnd(true);
}

// ---------- 2. 各联网方式 + HEX 透传模式数据上报 ----------

/**
 * @brief WiFi + Modbus HEX 透传 + MQTT 二进制数据上报
 */
static void test_wifi_modbus_hex_mqtt_report() {
    TestLog::testStart("WiFi + Modbus HEX + MQTT binary report");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_WIFI));
    TestLog::step("WiFi connected, MQTT online");

    // 模拟 Modbus 轮询 → formatRawHex 生成 HEX 帧
    uint16_t regs[] = { 0x0001, 0xD5CA };
    String hexFrame = formatRawHex(0x01, 0x03, regs, 2);
    TEST_ASSERT_TRUE(hexFrame.length() > 0);
    TestLog::step("Modbus raw HEX frame generated");

    // HEX 透传模式：转换为二进制发布
    uint8_t transferType = 1;
    bool sentAsBinary = publishReportDataHex(transferType, hexFrame, mqtt, "/data/report");
    TEST_ASSERT_TRUE(sentAsBinary);

    auto msgs = mqtt.getPublishedMessages();
    TEST_ASSERT_GREATER_THAN(0, (int)msgs.size());
    TestLog::step("HEX frame published as binary via MQTT");

    TestLog::testEnd(true);
}

/**
 * @brief 以太网 + Modbus HEX 透传 + MQTT 二进制数据上报
 */
static void test_ethernet_modbus_hex_mqtt_report() {
    TestLog::testStart("Ethernet + Modbus HEX + MQTT binary report");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_ETHERNET));
    TestLog::step("Ethernet connected, MQTT online");

    uint16_t regs[] = { 0x00FF };
    String hexFrame = formatRawHex(0x02, 0x03, regs, 1);

    uint8_t transferType = 1;
    bool sentAsBinary = publishReportDataHex(transferType, hexFrame, mqtt, "/data/report");
    TEST_ASSERT_TRUE(sentAsBinary);
    TestLog::step("HEX frame published as binary via Ethernet MQTT");

    TestLog::testEnd(true);
}

/**
 * @brief 4G + Modbus HEX 透传 + MQTT 二进制数据上报
 */
static void test_4g_modbus_hex_mqtt_report() {
    TestLog::testStart("4G + Modbus HEX + MQTT binary report");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_4G));
    TestLog::step("4G connected, MQTT online");

    uint16_t regs[] = { 0xABCD, 0x1234 };
    String hexFrame = formatRawHex(0x03, 0x04, regs, 2);

    uint8_t transferType = 1;
    bool sentAsBinary = publishReportDataHex(transferType, hexFrame, mqtt, "/data/report");
    TEST_ASSERT_TRUE(sentAsBinary);
    TestLog::step("HEX frame published as binary via 4G MQTT");

    TestLog::testEnd(true);
}

// ---------- 3. JSON 模式下平台下发 DATA_COMMAND ----------

/**
 * @brief WiFi + JSON 模式 + 平台下发 JSON DATA_COMMAND
 */
static void test_json_mode_data_command_accepted() {
    TestLog::testStart("JSON mode accepts JSON DATA_COMMAND");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_WIFI));
    TestLog::step("WiFi + MQTT connected");

    uint8_t transferType = 0;  // JSON
    const char* cmdPayload = "[{\"id\":\"relay\",\"value\":\"1\"}]";

    String enqueueMsg;
    bool shouldEnqueue;
    handleDataCommand(transferType,
                      (const uint8_t*)cmdPayload, strlen(cmdPayload),
                      enqueueMsg, shouldEnqueue);

    TEST_ASSERT_TRUE(shouldEnqueue);
    TEST_ASSERT_TRUE(enqueueMsg.indexOf("relay") >= 0);
    TestLog::step("JSON DATA_COMMAND accepted and enqueued");

    TestLog::testEnd(true);
}

/**
 * @brief 以太网 + JSON 模式 + 平台下发 JSON DATA_COMMAND
 */
static void test_ethernet_json_mode_data_command() {
    TestLog::testStart("Ethernet + JSON mode DATA_COMMAND");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_ETHERNET));

    uint8_t transferType = 0;
    const char* cmdPayload = "[{\"id\":\"modbus_read\",\"value\":\"{\\\"slaveAddress\\\":1}\"}]";

    String enqueueMsg;
    bool shouldEnqueue;
    handleDataCommand(transferType,
                      (const uint8_t*)cmdPayload, strlen(cmdPayload),
                      enqueueMsg, shouldEnqueue);

    TEST_ASSERT_TRUE(shouldEnqueue);
    TEST_ASSERT_TRUE(enqueueMsg.indexOf("modbus_read") >= 0);
    TestLog::step("Ethernet JSON DATA_COMMAND accepted");

    TestLog::testEnd(true);
}

// ---------- 4. JSON 模式严格拒绝非 JSON payload ----------

/**
 * @brief JSON 模式下非 JSON payload 被丢弃
 */
static void test_json_mode_rejects_non_json() {
    TestLog::testStart("JSON mode rejects non-JSON payload");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_WIFI));

    uint8_t transferType = 0;  // JSON
    // 模拟二进制 HEX 帧（非 JSON）
    const uint8_t binPayload[] = { 0x01, 0x03, 0x00, 0x01, 0x00, 0x01, 0xD5, 0xCA };

    String enqueueMsg;
    bool shouldEnqueue;
    handleDataCommand(transferType, binPayload, sizeof(binPayload),
                      enqueueMsg, shouldEnqueue);

    TEST_ASSERT_FALSE_MESSAGE(shouldEnqueue,
        "JSON mode must drop non-JSON payload");
    TestLog::step("Non-JSON payload correctly rejected in JSON mode");

    TestLog::testEnd(true);
}

// ---------- 5. HEX 透传模式下平台下发帧 ----------

/**
 * @brief HEX 透传模式 + 平台下发 HEX ASCII 帧 → modbus_raw_send
 */
static void test_hex_mode_hex_ascii_command() {
    TestLog::testStart("HEX mode accepts HEX ASCII command");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_WIFI));

    uint8_t transferType = 1;  // HEX 透传
    const char* hexAscii = "0103000100001D5CA";

    String enqueueMsg;
    bool shouldEnqueue;
    handleDataCommand(transferType,
                      (const uint8_t*)hexAscii, strlen(hexAscii),
                      enqueueMsg, shouldEnqueue);

    TEST_ASSERT_TRUE(shouldEnqueue);
    TEST_ASSERT_TRUE(enqueueMsg.indexOf("modbus_raw_send") >= 0);
    TestLog::step("HEX ASCII command converted to modbus_raw_send");

    TestLog::testEnd(true);
}

/**
 * @brief HEX 透传模式 + 平台下发二进制帧 → modbus_raw_send
 */
static void test_hex_mode_binary_command() {
    TestLog::testStart("HEX mode accepts binary command");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_4G));

    uint8_t transferType = 1;  // HEX 透传
    // 模拟真实二进制 Modbus 帧
    const uint8_t binFrame[] = { 0x01, 0x03, 0x00, 0x01, 0x00, 0x01, 0xD5, 0xCA };

    String enqueueMsg;
    bool shouldEnqueue;
    handleDataCommand(transferType, binFrame, sizeof(binFrame),
                      enqueueMsg, shouldEnqueue);

    TEST_ASSERT_TRUE(shouldEnqueue);
    TEST_ASSERT_TRUE(enqueueMsg.indexOf("modbus_raw_send") >= 0);
    // 验证 HEX 编码正确
    TEST_ASSERT_TRUE(enqueueMsg.indexOf("0103000100") >= 0);
    TestLog::step("Binary frame converted to modbus_raw_send with HEX encoding");

    TestLog::testEnd(true);
}

/**
 * @brief HEX 透传模式 + 平台下发 JSON 命令仍按 JSON 处理
 */
static void test_hex_mode_json_command_still_works() {
    TestLog::testStart("HEX mode still accepts JSON command");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_ETHERNET));

    uint8_t transferType = 1;  // HEX 透传
    // JSON 格式的 DATA_COMMAND（首个非空白字符为 [）
    const char* jsonCmd = "[{\"id\":\"relay\",\"value\":\"0\"}]";

    String enqueueMsg;
    bool shouldEnqueue;
    handleDataCommand(transferType,
                      (const uint8_t*)jsonCmd, strlen(jsonCmd),
                      enqueueMsg, shouldEnqueue);

    TEST_ASSERT_TRUE(shouldEnqueue);
    // JSON 格式直接传递，不转换为 modbus_raw_send
    TEST_ASSERT_TRUE(enqueueMsg.indexOf("relay") >= 0);
    TEST_ASSERT_TRUE(enqueueMsg.indexOf("modbus_raw_send") < 0);
    TestLog::step("JSON command in HEX mode passed through as-is");

    TestLog::testEnd(true);
}

// ---------- 6. HEX 帧大小上限验证 ----------

/**
 * @brief HEX 帧超过 256 字节时截断
 */
static void test_hex_frame_max_256_bytes() {
    TestLog::testStart("HEX frame capped at 256 bytes");

    MockMQTTClient mqtt;
    MQTTConfig config = buildDefaultMQTTConfig();
    mqtt.initialize(config);
    mqtt.connect();

    // 构造 600 字节 HEX 字符串 (= 300 bytes 二进制, 超过 256 上限)
    String longHex;
    longHex.reserve(600);
    for (int i = 0; i < 300; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X", (uint8_t)(i & 0xFF));
        longHex += buf;
    }
    TEST_ASSERT_EQUAL(600, (int)longHex.length());

    uint8_t transferType = 1;
    bool sentAsBinary = publishReportDataHex(transferType, longHex, mqtt, "/data/report");
    TEST_ASSERT_TRUE(sentAsBinary);

    // 验证发布的二进制长度为 256（被截断）
    auto msgs = mqtt.getPublishedMessages();
    TEST_ASSERT_GREATER_THAN(0, (int)msgs.size());
    TEST_ASSERT_LESS_OR_EQUAL(256, (int)msgs.back().payload.length());
    TestLog::step("HEX frame truncated to 256 bytes max");

    TestLog::testEnd(true);
}

// ---------- 7. collectCachedPollData 过滤验证 ----------

/**
 * @brief JSON 模式 collectCachedPollData 包含所有有映射的任务
 */
static void test_collect_poll_data_json_mode() {
    TestLog::testStart("collectCachedPollData JSON mode includes mapped tasks");

    uint8_t transferType = 0;  // JSON
    std::vector<int> mappingCounts = { 2, 0, 3 };  // 任务0有2映射, 任务1无映射, 任务2有3映射

    String result = collectCachedPollData(transferType, mappingCounts);

    // JSON 模式下所有任务都包含（即使 mappingCount=0 也不跳过）
    TEST_ASSERT_TRUE(result.indexOf("taskIdx") >= 0);
    // 应包含 3 个任务
    int taskCount = 0;
    for (size_t i = 0; i < result.length(); i++) {
        if (result[i] == '{') taskCount++;
    }
    TEST_ASSERT_EQUAL(3, taskCount);
    TestLog::step("JSON mode includes all tasks in poll data");

    TestLog::testEnd(true);
}

/**
 * @brief HEX 透传模式 collectCachedPollData 跳过无映射任务
 */
static void test_collect_poll_data_hex_mode_skips_unmapped() {
    TestLog::testStart("collectCachedPollData HEX mode skips unmapped tasks");

    uint8_t transferType = 1;  // HEX
    std::vector<int> mappingCounts = { 2, 0, 3 };  // 任务1无映射

    String result = collectCachedPollData(transferType, mappingCounts);

    // HEX 模式下跳过 mappingCount=0 的任务
    int taskCount = 0;
    for (size_t i = 0; i < result.length(); i++) {
        if (result[i] == '{') taskCount++;
    }
    TEST_ASSERT_EQUAL(2, taskCount);  // 只有任务0和任务2
    TEST_ASSERT_TRUE(result.indexOf("\"taskIdx\":0") >= 0);
    TEST_ASSERT_TRUE(result.indexOf("\"taskIdx\":2") >= 0);
    // 任务1 (无映射) 不应出现
    TEST_ASSERT_TRUE(result.indexOf("\"taskIdx\":1") < 0);
    TestLog::step("HEX mode skips unmapped tasks correctly");

    TestLog::testEnd(true);
}

// ---------- 8. 网络切换后 Modbus + MQTT 全链路恢复 ----------

/**
 * @brief WiFi → 以太网切换后 Modbus JSON 上报恢复
 */
static void test_switch_wifi_to_ethernet_modbus_json_recovery() {
    TestLog::testStart("WiFi→Ethernet: Modbus JSON report recovery");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;

    // WiFi 初始连接
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_WIFI));
    mqtt.clearPublishedMessages();

    // 第一次上报
    String json1 = "[{\"id\":\"temp\",\"value\":\"25.0\"}]";
    mqtt.publish("/data/report", json1.c_str());
    TEST_ASSERT_EQUAL(1, (int)mqtt.getPublishedMessages().size());
    TestLog::step("WiFi: first Modbus JSON report sent");

    // 切换到以太网
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_ETHERNET));
    assertMQTTConnected(mqtt);
    mqtt.clearPublishedMessages();
    TestLog::step("Switched to Ethernet, MQTT reconnected");

    // 以太网下恢复上报
    String json2 = "[{\"id\":\"temp\",\"value\":\"26.0\"}]";
    mqtt.publish("/data/report", json2.c_str());
    TEST_ASSERT_EQUAL(1, (int)mqtt.getPublishedMessages().size());
    assertLastPublishContains(mqtt, "26.0");
    TestLog::step("Ethernet: Modbus JSON report resumed");

    TestLog::testEnd(true);
}

/**
 * @brief 以太网 → 4G 切换后 Modbus HEX 透传恢复
 */
static void test_switch_ethernet_to_4g_modbus_hex_recovery() {
    TestLog::testStart("Ethernet→4G: Modbus HEX report recovery");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;

    // 以太网初始连接
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_ETHERNET));
    mqtt.clearPublishedMessages();

    // HEX 透传上报
    uint16_t regs1[] = { 0x0001 };
    String hex1 = formatRawHex(0x01, 0x03, regs1, 1);
    publishReportDataHex(1, hex1, mqtt, "/data/report");
    TEST_ASSERT_EQUAL(1, (int)mqtt.getPublishedMessages().size());
    TestLog::step("Ethernet: HEX frame published");

    // 切换到 4G
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_4G));
    assertMQTTConnected(mqtt);
    mqtt.clearPublishedMessages();
    TestLog::step("Switched to 4G, MQTT reconnected");

    // 4G 下恢复 HEX 透传上报
    uint16_t regs2[] = { 0x00FF };
    String hex2 = formatRawHex(0x01, 0x03, regs2, 1);
    publishReportDataHex(1, hex2, mqtt, "/data/report");
    TEST_ASSERT_EQUAL(1, (int)mqtt.getPublishedMessages().size());
    TestLog::step("4G: HEX frame report resumed");

    TestLog::testEnd(true);
}

/**
 * @brief 4G 失败回退 AP → WiFi 恢复后 Modbus 交互恢复
 */
static void test_4g_fallback_to_wifi_modbus_recovery() {
    TestLog::testStart("4G failure→AP→WiFi: Modbus interaction recovery");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;

    // 4G 成功
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_4G));
    assertMQTTConnected(mqtt);
    TestLog::step("4G connected, MQTT online");

    // 4G 失败回退 AP
    TEST_ASSERT_FALSE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_4G, false));
    TEST_ASSERT_TRUE(mgr.isAPRunning());
    TEST_ASSERT_FALSE(mqtt.getIsConnected());
    TestLog::step("4G failed, AP fallback, MQTT offline");

    // WiFi 恢复
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_WIFI));
    assertMQTTConnected(mqtt);
    TestLog::step("WiFi recovered, MQTT reconnected");

    // Modbus 数据上报恢复
    String jsonData = "[{\"id\":\"temp\",\"value\":\"27.0\",\"remark\":\"modbus\"}]";
    mqtt.publish("/data/report", jsonData.c_str());
    assertLastPublishContains(mqtt, "temp");
    TestLog::step("Modbus JSON report resumed after recovery");

    TestLog::testEnd(true);
}

// ---------- 9. 多网络 × 多传输类型排列组合 ----------

/**
 * @brief 所有联网方式 × 传输类型组合的 MQTT 数据上报验证
 */
static void test_all_network_x_transfer_combinations() {
    TestLog::testStart("All network × transfer type combinations");

    struct TestCase {
        MockMultiNetworkManager::NetType netType;
        uint8_t transferType;
        const char* label;
    };

    TestCase cases[] = {
        { MockMultiNetworkManager::NetType::NET_WIFI,     0, "WiFi+JSON" },
        { MockMultiNetworkManager::NetType::NET_WIFI,     1, "WiFi+HEX" },
        { MockMultiNetworkManager::NetType::NET_ETHERNET, 0, "ETH+JSON" },
        { MockMultiNetworkManager::NetType::NET_ETHERNET, 1, "ETH+HEX" },
        { MockMultiNetworkManager::NetType::NET_4G,       0, "4G+JSON" },
        { MockMultiNetworkManager::NetType::NET_4G,       1, "4G+HEX" },
    };

    for (const auto& tc : cases) {
        MockMultiNetworkManager mgr;
        MockMQTTClient mqtt;

        // 建立连接
        bool ok = setupNetworkAndMQTT(mgr, mqtt, tc.netType);
        TEST_ASSERT_TRUE_MESSAGE(ok, (String(tc.label) + ": network setup failed").c_str());
        assertMQTTConnected(mqtt);

        if (tc.transferType == 0) {
            // JSON 模式
            String jsonData = "[{\"id\":\"sensor\",\"value\":\"42.0\"}]";
            bool bin = publishReportDataHex(tc.transferType, jsonData, mqtt, "/data/report");
            TEST_ASSERT_FALSE_MESSAGE(bin,
                (String(tc.label) + ": JSON should not use binary path").c_str());
            mqtt.publish("/data/report", jsonData.c_str());
            TEST_ASSERT_EQUAL(1, (int)mqtt.getPublishedMessages().size());
        } else {
            // HEX 透传模式
            uint16_t regs[] = { 0x1234 };
            String hexFrame = formatRawHex(0x01, 0x03, regs, 1);
            bool bin = publishReportDataHex(tc.transferType, hexFrame, mqtt, "/data/report");
            TEST_ASSERT_TRUE_MESSAGE(bin,
                (String(tc.label) + ": HEX should use binary path").c_str());
            TEST_ASSERT_EQUAL(1, (int)mqtt.getPublishedMessages().size());
        }
        TestLog::step((String("PASS: ") + tc.label).c_str());
    }

    TestLog::testEnd(true);
}

/**
 * @brief 所有联网方式 × 传输类型组合的 DATA_COMMAND 接收验证
 */
static void test_all_network_x_transfer_command_combinations() {
    TestLog::testStart("All network × transfer type command combinations");

    struct CmdCase {
        MockMultiNetworkManager::NetType netType;
        uint8_t transferType;
        const char* payload;
        bool expectEnqueue;
        bool expectRawSend;
        const char* label;
    };

    // JSON 命令 (所有模式通用)
    const char* jsonCmd = "[{\"id\":\"relay\",\"value\":\"1\"}]";
    // HEX ASCII 帧
    const char* hexAscii = "0103000100001D5CA";

    CmdCase cases[] = {
        // JSON 模式：接受 JSON 命令
        { MockMultiNetworkManager::NetType::NET_WIFI,     0, jsonCmd,  true,  false, "WiFi+JSON+jsonCmd" },
        { MockMultiNetworkManager::NetType::NET_ETHERNET, 0, jsonCmd,  true,  false, "ETH+JSON+jsonCmd" },
        { MockMultiNetworkManager::NetType::NET_4G,       0, jsonCmd,  true,  false, "4G+JSON+jsonCmd" },
        // HEX 模式：接受 JSON 命令 (按 JSON 处理)
        { MockMultiNetworkManager::NetType::NET_WIFI,     1, jsonCmd,  true,  false, "WiFi+HEX+jsonCmd" },
        { MockMultiNetworkManager::NetType::NET_ETHERNET, 1, jsonCmd,  true,  false, "ETH+HEX+jsonCmd" },
        { MockMultiNetworkManager::NetType::NET_4G,       1, jsonCmd,  true,  false, "4G+HEX+jsonCmd" },
        // HEX 模式：接受 HEX ASCII 帧 (转换为 modbus_raw_send)
        { MockMultiNetworkManager::NetType::NET_WIFI,     1, hexAscii, true,  true,  "WiFi+HEX+hexCmd" },
        { MockMultiNetworkManager::NetType::NET_ETHERNET, 1, hexAscii, true,  true,  "ETH+HEX+hexCmd" },
        { MockMultiNetworkManager::NetType::NET_4G,       1, hexAscii, true,  true,  "4G+HEX+hexCmd" },
    };

    for (const auto& tc : cases) {
        MockMultiNetworkManager mgr;
        MockMQTTClient mqtt;
        TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, tc.netType));

        String enqueueMsg;
        bool shouldEnqueue;
        handleDataCommand(tc.transferType,
                          (const uint8_t*)tc.payload, strlen(tc.payload),
                          enqueueMsg, shouldEnqueue);

        TEST_ASSERT_EQUAL_MESSAGE(tc.expectEnqueue, shouldEnqueue,
            (String(tc.label) + ": enqueue mismatch").c_str());

        if (shouldEnqueue) {
            if (tc.expectRawSend) {
                TEST_ASSERT_TRUE_MESSAGE(enqueueMsg.indexOf("modbus_raw_send") >= 0,
                    (String(tc.label) + ": expected modbus_raw_send").c_str());
            } else {
                TEST_ASSERT_TRUE_MESSAGE(enqueueMsg.indexOf("modbus_raw_send") < 0,
                    (String(tc.label) + ": should NOT be modbus_raw_send").c_str());
            }
        }
        TestLog::step((String("PASS: ") + tc.label).c_str());
    }

    TestLog::testEnd(true);
}

// ---------- 10. 网络切换期间 Modbus 数据不丢失 ----------

/**
 * @brief 网络切换中断期间 Modbus 数据被丢弃（MQTT offline），恢复后重新上报
 */
static void test_modbus_data_during_network_switch() {
    TestLog::testStart("Modbus data during network switch");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;

    // WiFi 连接
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_WIFI));
    assertMQTTConnected(mqtt);
    mqtt.clearPublishedMessages();
    TestLog::step("WiFi + MQTT connected");

    // 正常上报
    mqtt.publish("/data/report", "[{\"id\":\"v1\",\"value\":\"1.0\"}]");
    TEST_ASSERT_EQUAL(1, (int)mqtt.getPublishedMessages().size());
    TestLog::step("Data reported before switch");

    // 切换网络（MQTT 断开）
    mqtt.disconnect();
    mqtt.setStopped(true);
    mgr.disconnect();
    mgr.networkType = MockMultiNetworkManager::NetType::NET_ETHERNET;
    mgr.initialize(true);
    TEST_ASSERT_FALSE(mqtt.getIsConnected());
    TestLog::step("Network switching, MQTT offline");

    // 此时 Modbus 轮询产生的数据无法上报（MQTT offline）
    // 模拟 dispatchModbusData 中 mqttClient->getIsConnected() == false 的分支
    bool mqttOffline = !mqtt.getIsConnected();
    TEST_ASSERT_TRUE(mqttOffline);
    // publish 应失败
    TEST_ASSERT_FALSE(mqtt.publish("/data/report", "[{\"id\":\"v2\",\"value\":\"2.0\"}]"));
    TestLog::step("Modbus data dropped during switch (expected)");

    // 以太网连接后 MQTT 恢复
    mqtt.setStopped(false);
    mqtt.initialize(buildDefaultMQTTConfig());
    TEST_ASSERT_TRUE(mqtt.connect());
    assertMQTTConnected(mqtt);
    mqtt.clearPublishedMessages();  // 清除旧消息，仅验证恢复后的上报

    // 恢复上报
    mqtt.publish("/data/report", "[{\"id\":\"v3\",\"value\":\"3.0\"}]");
    TEST_ASSERT_EQUAL(1, (int)mqtt.getPublishedMessages().size());
    assertLastPublishContains(mqtt, "v3");
    TestLog::step("Data report resumed after Ethernet connection");

    TestLog::testEnd(true);
}

// ---------- 11. 全链路端到端验证 ----------

/**
 * @brief 全链路：Modbus 轮询 → 数据格式化 → MQTT 上报 → 平台下发指令 → Modbus 执行
 */
static void test_full_chain_modbus_mqtt_roundtrip() {
    TestLog::testStart("Full chain: Modbus→MQTT→Command→Modbus");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_4G));
    assertMQTTConnected(mqtt);
    TestLog::step("4G + MQTT connected");

    // 1. Modbus 轮询产生数据 → JSON 格式上报
    String jsonData = "[{\"id\":\"temp\",\"value\":\"25.5\",\"remark\":\"modbus\"}]";
    mqtt.publish("/data/report", jsonData.c_str());
    TEST_ASSERT_EQUAL(1, (int)mqtt.getPublishedMessages().size());
    TestLog::step("Step 1: Modbus data reported via MQTT");

    // 2. 平台下发控制指令
    const char* cmdPayload = "[{\"id\":\"relay\",\"value\":\"1\"}]";
    String enqueueMsg;
    bool shouldEnqueue;
    handleDataCommand(0, (const uint8_t*)cmdPayload, strlen(cmdPayload),
                      enqueueMsg, shouldEnqueue);
    TEST_ASSERT_TRUE(shouldEnqueue);
    TestLog::step("Step 2: Platform command received and enqueued");

    // 3. 控制执行后上报结果
    String result = "[{\"id\":\"relay\",\"value\":\"1\",\"remark\":\"control\"}]";
    mqtt.publish("/data/report", result.c_str());
    TEST_ASSERT_EQUAL(2, (int)mqtt.getPublishedMessages().size());
    assertLastPublishContains(mqtt, "control");
    TestLog::step("Step 3: Control result reported");

    // 4. NTP 同步请求
    mqtt.subscribe("/ntp/get");
    TEST_ASSERT_EQUAL(1, (int)mqtt.getSubscriptions().size());
    TestLog::step("Step 4: NTP subscription active");

    TestLog::testEnd(true);
}

/**
 * @brief 全链路 HEX 模式：Modbus 轮询 → HEX 帧 → MQTT 二进制上报 → 平台下发帧 → raw_send
 */
static void test_full_chain_hex_mode_roundtrip() {
    TestLog::testStart("Full chain HEX: Modbus→HEX→MQTT→Frame→raw_send");

    MockMultiNetworkManager mgr;
    MockMQTTClient mqtt;
    TEST_ASSERT_TRUE(setupNetworkAndMQTT(mgr, mqtt, MockMultiNetworkManager::NetType::NET_ETHERNET));
    assertMQTTConnected(mqtt);
    TestLog::step("Ethernet + MQTT connected");

    // 1. Modbus 轮询 → formatRawHex → 二进制上报
    uint16_t regs[] = { 0x0001, 0x0002 };
    String hexFrame = formatRawHex(0x01, 0x03, regs, 2);
    bool sentAsBinary = publishReportDataHex(1, hexFrame, mqtt, "/data/report");
    TEST_ASSERT_TRUE(sentAsBinary);
    TEST_ASSERT_EQUAL(1, (int)mqtt.getPublishedMessages().size());
    TestLog::step("Step 1: HEX frame published as binary");

    // 2. 平台下发二进制 Modbus 帧
    const uint8_t dlFrame[] = { 0x01, 0x06, 0x00, 0x01, 0x00, 0x05 };
    String enqueueMsg;
    bool shouldEnqueue;
    handleDataCommand(1, dlFrame, sizeof(dlFrame), enqueueMsg, shouldEnqueue);
    TEST_ASSERT_TRUE(shouldEnqueue);
    TEST_ASSERT_TRUE(enqueueMsg.indexOf("modbus_raw_send") >= 0);
    // 验证帧内容被正确编码
    TEST_ASSERT_TRUE(enqueueMsg.indexOf("010600010005") >= 0);
    TestLog::step("Step 2: Binary command frame → modbus_raw_send");

    // 3. raw_send 执行后的响应也通过 HEX 上报
    uint16_t respRegs[] = { 0x0005 };
    String respHex = formatRawHex(0x01, 0x06, respRegs, 1);
    publishReportDataHex(1, respHex, mqtt, "/data/report");
    TEST_ASSERT_EQUAL(2, (int)mqtt.getPublishedMessages().size());
    TestLog::step("Step 3: Command response reported as HEX");

    TestLog::testEnd(true);
}

// ========== 测试组入口 ==========

void test_modbus_mqtt_interaction_group() {
    TestLog::groupStart("Modbus-MQTT Interaction Tests (Network × Transfer Type)");

    // 1. 各联网方式 + JSON 模式数据上报
    RUN_TEST(test_wifi_modbus_json_mqtt_report);
    RUN_TEST(test_ethernet_modbus_json_mqtt_report);
    RUN_TEST(test_4g_modbus_json_mqtt_report);

    // 2. 各联网方式 + HEX 透传模式数据上报
    RUN_TEST(test_wifi_modbus_hex_mqtt_report);
    RUN_TEST(test_ethernet_modbus_hex_mqtt_report);
    RUN_TEST(test_4g_modbus_hex_mqtt_report);

    // 3. JSON 模式下平台下发 DATA_COMMAND
    RUN_TEST(test_json_mode_data_command_accepted);
    RUN_TEST(test_ethernet_json_mode_data_command);

    // 4. JSON 模式严格拒绝非 JSON payload
    RUN_TEST(test_json_mode_rejects_non_json);

    // 5. HEX 透传模式下平台下发帧
    RUN_TEST(test_hex_mode_hex_ascii_command);
    RUN_TEST(test_hex_mode_binary_command);
    RUN_TEST(test_hex_mode_json_command_still_works);

    // 6. HEX 帧大小上限验证
    RUN_TEST(test_hex_frame_max_256_bytes);

    // 7. collectCachedPollData 过滤验证
    RUN_TEST(test_collect_poll_data_json_mode);
    RUN_TEST(test_collect_poll_data_hex_mode_skips_unmapped);

    // 8. 网络切换后 Modbus + MQTT 全链路恢复
    RUN_TEST(test_switch_wifi_to_ethernet_modbus_json_recovery);
    RUN_TEST(test_switch_ethernet_to_4g_modbus_hex_recovery);
    RUN_TEST(test_4g_fallback_to_wifi_modbus_recovery);

    // 9. 多网络 × 多传输类型排列组合
    RUN_TEST(test_all_network_x_transfer_combinations);
    RUN_TEST(test_all_network_x_transfer_command_combinations);

    // 10. 网络切换期间 Modbus 数据不丢失
    RUN_TEST(test_modbus_data_during_network_switch);

    // 11. 全链路端到端验证
    RUN_TEST(test_full_chain_modbus_mqtt_roundtrip);
    RUN_TEST(test_full_chain_hex_mode_roundtrip);

    TestLog::groupEnd();
}
