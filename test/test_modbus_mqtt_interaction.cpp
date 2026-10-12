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
#include <ArduinoJson.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
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

static std::string readModbusInteractionSource(const char* relativePath) {
    const char* roots[] = { ".", "..", "../.." };
    for (const char* root : roots) {
        std::ifstream file(std::string(root) + "/" + relativePath);
        if (!file.is_open()) continue;
        std::ostringstream content;
        content << file.rdbuf();
        return content.str();
    }
    return "";
}

static uint16_t calculateTestModbusCrc(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFFU;
    for (size_t pos = 0; pos < length; ++pos) {
        crc ^= static_cast<uint16_t>(data[pos]);
        for (uint8_t bit = 0; bit < 8U; ++bit) {
            crc = (crc & 0x0001U) != 0U
                      ? static_cast<uint16_t>((crc >> 1U) ^ 0xA001U)
                      : static_cast<uint16_t>(crc >> 1U);
        }
    }
    return crc;
}

static std::string bytesToHex(const std::vector<uint8_t>& bytes) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(bytes.size() * 2U);
    for (uint8_t value : bytes) {
        result.push_back(HEX[value >> 4U]);
        result.push_back(HEX[value & 0x0FU]);
    }
    return result;
}

static std::string buildCompleteRtuAduHex(size_t bodyLength) {
    std::vector<uint8_t> frame(bodyLength);
    for (size_t i = 0; i < frame.size(); ++i) {
        frame[i] = static_cast<uint8_t>((i * 37U + 1U) & 0xFFU);
    }
    const uint16_t crc = calculateTestModbusCrc(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc & 0xFFU));
    frame.push_back(static_cast<uint8_t>((crc >> 8U) & 0xFFU));
    return bytesToHex(frame);
}

static bool isCompleteRtuAduHex(const std::string& hex) {
    if (hex.size() < 8U || (hex.size() % 2U) != 0U || hex.size() > 512U) {
        return false;
    }

    auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::vector<uint8_t> frame(hex.size() / 2U);
    for (size_t i = 0; i < frame.size(); ++i) {
        const int high = hexValue(hex[i * 2U]);
        const int low = hexValue(hex[i * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        frame[i] = static_cast<uint8_t>((high << 4) | low);
    }

    const uint16_t crc = calculateTestModbusCrc(frame.data(), frame.size() - 2U);
    return frame[frame.size() - 2U] == static_cast<uint8_t>(crc & 0xFFU) &&
           frame[frame.size() - 1U] == static_cast<uint8_t>((crc >> 8U) & 0xFFU);
}

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
    if (byteLen > 256) return false;  // 完整 RTU 帧不可静默截断

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

enum class SimulatedRawError : uint8_t {
    Success,
    Timeout,
    Exception
};

static bool publishCapturedRawResponse(SimulatedRawError error,
                                       const uint8_t* response, size_t responseLength,
                                       MockMQTTClient& mqtt, const char* topic) {
    const bool forwardableException =
        error == SimulatedRawError::Exception &&
        responseLength >= 5U && response != nullptr &&
        (response[1] & 0x80U) != 0U;
    if (error != SimulatedRawError::Success && !forwardableException) {
        return false;
    }
    if (!response || responseLength == 0) return false;

    String responseHex;
    responseHex.reserve(responseLength * 2U);
    for (size_t i = 0; i < responseLength; ++i) {
        char byteHex[3];
        snprintf(byteHex, sizeof(byteHex), "%02X", response[i]);
        responseHex += byteHex;
    }
    return publishReportDataHex(1, responseHex, mqtt, topic);
}

static bool isStructuredJsonPayload(const uint8_t* payload, unsigned int length) {
    if (!payload || length == 0) return false;

    unsigned int first = 0;
    while (first < length) {
        char c = static_cast<char>(payload[first]);
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++first;
    }
    if (first >= length || (payload[first] != '[' && payload[first] != '{')) {
        return false;
    }

    JsonDocument probe;
    DeserializationError error = deserializeJson(probe, payload, length);
    return !error && (probe.is<JsonArray>() || probe.is<JsonObject>());
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

    // 必须成功反序列化为对象/数组才属于结构化 JSON。原始 RTU 帧可能
    // 恰好以 '[' 或 '{' 开头，不能只凭首字节判断。
    bool structuredJson = isStructuredJsonPayload(payload, length);

    enqueueMsg = msg;
    shouldEnqueue = true;

    if (!structuredJson && length >= 2) {
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
    const char* hexAscii = "010300010001D5CA";

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

static void test_hex_mode_json_like_binary_command_is_raw_frame() {
    TestLog::testStart("HEX mode treats invalid JSON prefix as raw RTU frame");

    // 0x5B ('[') is a valid Modbus slave address. The remaining bytes do not
    // form JSON, so the complete binary frame must still take the raw path.
    const uint8_t binFrame[] = { 0x5B, 0x03, 0x00, 0x01, 0x00, 0x01, 0xD9, 0x30 };
    String enqueueMsg;
    bool shouldEnqueue = false;
    handleDataCommand(1, binFrame, sizeof(binFrame), enqueueMsg, shouldEnqueue);

    TEST_ASSERT_TRUE(shouldEnqueue);
    TEST_ASSERT_TRUE(enqueueMsg.indexOf("modbus_raw_send") >= 0);
    TEST_ASSERT_TRUE(enqueueMsg.indexOf("5B0300010001D930") >= 0);

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
 * @brief HEX 帧超过 256 字节时整帧拒绝，不能截断后发布无效 CRC 帧
 */
static void test_hex_frame_over_256_bytes_is_rejected() {
    TestLog::testStart("HEX frame over 256 bytes is rejected");

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
    TEST_ASSERT_FALSE(sentAsBinary);
    TEST_ASSERT_EQUAL(0, (int)mqtt.getPublishedMessages().size());
    TestLog::step("Oversized RTU frame rejected without publishing a truncated frame");

    TestLog::testEnd(true);
}

static void test_hex_frame_exactly_256_bytes_is_accepted() {
    TestLog::testStart("HEX frame at 256-byte boundary is accepted");

    MockMQTTClient mqtt;
    MQTTConfig config = buildDefaultMQTTConfig();
    mqtt.initialize(config);
    mqtt.connect();

    String boundaryHex;
    boundaryHex.reserve(512);
    for (int i = 0; i < 256; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02X", (uint8_t)(i & 0xFF));
        boundaryHex += buf;
    }

    TEST_ASSERT_TRUE(publishReportDataHex(1, boundaryHex, mqtt, "/data/report"));
    TEST_ASSERT_EQUAL(1, (int)mqtt.getPublishedMessages().size());

    TestLog::testEnd(true);
}

static void test_modbus_raw_transaction_lengths_are_256_safe() {
    TestLog::testStart("Modbus raw transaction uses 256-safe lengths");

    const std::string header = readModbusInteractionSource(
        "include/protocols/ModbusHandler.h");
    const std::string implementation = readModbusInteractionSource(
        "src/protocols/ModbusHandler.cpp");
    const std::string manager = readModbusInteractionSource(
        "src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(header.empty(), "ModbusHandler.h must be readable");
    TEST_ASSERT_FALSE_MESSAGE(implementation.empty(), "ModbusHandler.cpp must be readable");
    TEST_ASSERT_FALSE_MESSAGE(manager.empty(), "ProtocolManager.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        header.find("sendRawFrameBytesOnce") != std::string::npos &&
        header.find("size_t& responseLength") != std::string::npos,
        "raw API must return its byte length through size_t");
    TEST_ASSERT_TRUE_MESSAGE(
        implementation.find("size_t respIdx = 0;") != std::string::npos &&
        implementation.find("bool responseOverflow = false;") != std::string::npos &&
        implementation.find("respIdx > rawCap") != std::string::npos,
        "receive index must not wrap and oversized responses must not be truncated");
    TEST_ASSERT_TRUE_MESSAGE(
        implementation.find("uint8_t rawResponse[Protocols::MODBUS_BUFFER_SIZE]") != std::string::npos &&
        implementation.find("result.error = _sendOneShotRequestRaw(") != std::string::npos,
        "32-register reads must parse their 69-byte response outside OneShotResult");
    TEST_ASSERT_TRUE_MESSAGE(
        manager.find("sendRawFrameBytesOnce") != std::string::npos &&
        manager.find("uint8_t response[Protocols::MODBUS_BUFFER_SIZE]") != std::string::npos,
        "MQTT raw command path must use the complete byte-response API");

    constexpr size_t maxDataWithoutCrc = 256U - 2U;
    TEST_ASSERT_EQUAL_UINT32(256U, maxDataWithoutCrc + 2U);
    TEST_ASSERT_GREATER_THAN(256U, maxDataWithoutCrc + 3U);
    TestLog::step("254-byte body + CRC remains a 256-byte ADU without wraparound");

    TestLog::testEnd(true);
}

static void test_modbus_mqtt_raw_command_requires_complete_crc_adu() {
    TestLog::testStart("Modbus MQTT raw command requires complete CRC-valid ADU");

    TEST_ASSERT_TRUE(isCompleteRtuAduHex("010300010001D5CA"));
    TEST_ASSERT_FALSE(isCompleteRtuAduHex("010300010001D5CB"));
    TEST_ASSERT_FALSE(isCompleteRtuAduHex("010300010001"));
    TEST_ASSERT_FALSE(isCompleteRtuAduHex("010300010001D5C"));

    const std::string maxAdu = buildCompleteRtuAduHex(254U);
    const std::string oversizedAdu = buildCompleteRtuAduHex(255U);
    TEST_ASSERT_EQUAL_UINT32(512U, maxAdu.size());
    TEST_ASSERT_TRUE(isCompleteRtuAduHex(maxAdu));
    TEST_ASSERT_EQUAL_UINT32(514U, oversizedAdu.size());
    TEST_ASSERT_FALSE(isCompleteRtuAduHex(oversizedAdu));

    const std::string manager = readModbusInteractionSource(
        "src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(manager.empty(), "ProtocolManager.cpp must be readable");
    const auto rawStart = manager.find(
        "String ProtocolManager::executeModbusRawSend(const String& hexPayload)");
    const auto rawEnd = manager.find("#endif // FASTBEE_ENABLE_MODBUS", rawStart);
    TEST_ASSERT_TRUE_MESSAGE(rawStart != std::string::npos && rawEnd != std::string::npos,
                             "MQTT raw command implementation must be discoverable");
    const std::string rawBlock = manager.substr(rawStart, rawEnd - rawStart);
    const auto minLengthPos = rawBlock.find("hex.length() < 8U");
    const auto lengthRejectPos = rawBlock.find(
        "return failed(\"MODBUS_HEX_INVALID\");", minLengthPos);
    const auto crcPos = rawBlock.find(
        "calculateCRC(byteArray, byteLen - 2U)");
    const auto mismatchPos = rawBlock.find(
        "byteArray[byteLen - 2U] != crcLo");
    const auto highMismatchPos = rawBlock.find(
        "byteArray[byteLen - 1U] != crcHi", mismatchPos);
    const auto rejectPos = rawBlock.find(
        "return failed(\"MODBUS_CRC_INVALID\");", highMismatchPos);
    const auto sendPos = rawBlock.find("sendRawFrameBytesOnce");
    const auto submitPos = rawBlock.find("pm.submitOrdinaryModbusFrame");
    TEST_ASSERT_TRUE_MESSAGE(
        minLengthPos != std::string::npos && lengthRejectPos != std::string::npos &&
        crcPos != std::string::npos && highMismatchPos != std::string::npos &&
        mismatchPos != std::string::npos && rejectPos != std::string::npos &&
        sendPos != std::string::npos && submitPos != std::string::npos &&
        minLengthPos < lengthRejectPos && lengthRejectPos < crcPos &&
        crcPos < mismatchPos && mismatchPos < highMismatchPos &&
        highMismatchPos < rejectPos && rejectPos < sendPos && rejectPos < submitPos,
        "CRC and minimum-ADU failures must report rejection before read UART or PM write submission");
    TEST_ASSERT_TRUE_MESSAGE(
        rawBlock.find("size_t effectiveLen = byteLen;") == std::string::npos,
        "MQTT passthrough must not reinterpret an invalid ADU as a CRC-less body");

    TestLog::testEnd(true);
}

static void test_modbus_legacy_oneshot_count_matches_copied_capacity() {
    TestLog::testStart("Modbus legacy OneShot count is capacity bounded");

    const std::string implementation = readModbusInteractionSource(
        "src/protocols/ModbusHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(implementation.empty(), "ModbusHandler.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(
        implementation.find("rawLen < Protocols::MODBUS_ONESHOT_BUFFER_SIZE") != std::string::npos &&
        implementation.find("result.count = static_cast<uint16_t>(copyLen);") != std::string::npos,
        "legacy result count must describe only bytes copied into its 32 slots");
    TEST_ASSERT_TRUE_MESSAGE(
        implementation.find("result.count = rawLen") == std::string::npos,
        "legacy result must never advertise an uncopied raw response length");

    TestLog::testEnd(true);
}

static void test_modbus_raw_exception_forwarding_contract() {
    TestLog::testStart("Modbus raw exception frame forwarding contract");

    const std::string implementation = readModbusInteractionSource(
        "src/protocols/ModbusHandler.cpp");
    const std::string manager = readModbusInteractionSource(
        "src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(implementation.empty(), "ModbusHandler.cpp must be readable");
    TEST_ASSERT_FALSE_MESSAGE(manager.empty(), "ProtocolManager.cpp must be readable");

    const auto rawStart = implementation.find(
        "OneShotError ModbusHandler::_executeModbusTransactionRaw(");
    const auto rawEnd = implementation.find("return error;", rawStart);
    TEST_ASSERT_TRUE_MESSAGE(rawStart != std::string::npos && rawEnd != std::string::npos,
                             "raw transaction implementation must be discoverable");
    const std::string rawBlock = implementation.substr(rawStart, rawEnd - rawStart);
    const auto capacityPos = rawBlock.find("if (respIdx > rawCap)");
    const auto copyPos = rawBlock.find("memcpy(rawResp, respBuf, respIdx);");
    const auto lengthPos = rawBlock.find("rawLen = respIdx;");
    const auto exceptionPos = rawBlock.find("if (respBuf[1] & 0x80)");
    TEST_ASSERT_TRUE_MESSAGE(
        capacityPos != std::string::npos && copyPos != std::string::npos &&
        lengthPos != std::string::npos && exceptionPos != std::string::npos &&
        capacityPos < copyPos && copyPos < lengthPos && lengthPos < exceptionPos,
        "CRC-valid raw response must be copied before exception classification");

    const auto legacyStart = implementation.find(
        "OneShotResult ModbusHandler::_executeModbusTransaction(");
    TEST_ASSERT_TRUE_MESSAGE(legacyStart != std::string::npos && legacyStart < rawStart,
                             "legacy transaction wrapper must be discoverable");
    const std::string legacyBlock = implementation.substr(legacyStart, rawStart - legacyStart);
    const auto successOnlyPos = legacyBlock.find("if (result.error == ONESHOT_SUCCESS)");
    const auto legacyCopyPos = legacyBlock.find("result.data[i] = rawBuf[i];");
    TEST_ASSERT_TRUE_MESSAGE(
        successOnlyPos != std::string::npos && legacyCopyPos != std::string::npos &&
        successOnlyPos < legacyCopyPos,
        "ordinary OneShot transactions must keep exception responses as errors without copied data");

    TEST_ASSERT_TRUE_MESSAGE(
        manager.find("const bool forwardableException =") != std::string::npos &&
        manager.find("rawError == ONESHOT_EXCEPTION") != std::string::npos &&
        manager.find("responseLength >= 5U") != std::string::npos &&
        manager.find("(response[1] & 0x80U) != 0U") != std::string::npos &&
        manager.find("rawError != ONESHOT_SUCCESS && !forwardableException") != std::string::npos,
        "MQTT raw path must only admit a captured Modbus exception response");

    TestLog::testEnd(true);
}

static void test_modbus_exception_response_is_binary_mqtt_payload() {
    TestLog::testStart("Modbus exception response is preserved as binary MQTT payload");

    MockMQTTClient mqtt;
    MQTTConfig config = buildDefaultMQTTConfig();
    mqtt.initialize(config);
    mqtt.connect();

    const uint8_t exceptionFrame[] = { 0x01, 0x83, 0x02, 0xC0, 0xF1 };
    TEST_ASSERT_TRUE(publishCapturedRawResponse(
        SimulatedRawError::Exception, exceptionFrame, sizeof(exceptionFrame),
        mqtt, "/data/report"));

    const auto messages = mqtt.getPublishedMessages();
    TEST_ASSERT_EQUAL(1, static_cast<int>(messages.size()));
    TEST_ASSERT_EQUAL_UINT32(sizeof(exceptionFrame), messages.back().payload.length());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(
        exceptionFrame,
        reinterpret_cast<const uint8_t*>(messages.back().payload.c_str()),
        sizeof(exceptionFrame));

    mqtt.clearPublishedMessages();
    TEST_ASSERT_FALSE(publishCapturedRawResponse(
        SimulatedRawError::Timeout, nullptr, 0, mqtt, "/data/report"));
    TEST_ASSERT_FALSE(publishCapturedRawResponse(
        SimulatedRawError::Exception, nullptr, 0, mqtt, "/data/report"));
    const uint8_t nonExceptionFrame[] = { 0x01, 0x03, 0x02, 0x00, 0x00 };
    TEST_ASSERT_FALSE(publishCapturedRawResponse(
        SimulatedRawError::Exception, nonExceptionFrame, sizeof(nonExceptionFrame),
        mqtt, "/data/report"));
    TEST_ASSERT_EQUAL(0, static_cast<int>(mqtt.getPublishedMessages().size()));

    TestLog::testEnd(true);
}

static void test_modbus_raw_mode_starts_without_poll_targets() {
    TestLog::testStart("Modbus raw mode starts without polling targets");

    const std::string manager = readModbusInteractionSource(
        "src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(manager.empty(), "ProtocolManager.cpp must be readable");
    const auto restartStart = manager.find(
        "ModbusRestartResult ProtocolManager::restartModbus()");
    const auto restartEnd = manager.find("void ProtocolManager::stopModbus()", restartStart);
    TEST_ASSERT_TRUE_MESSAGE(restartStart != std::string::npos && restartEnd != std::string::npos,
                             "restartModbus implementation must be discoverable");
    const std::string restartBlock = manager.substr(restartStart, restartEnd - restartStart);
    const auto rawModeGuard = restartBlock.find("modbusConfig.transferType != 1");
    const auto noTaskGuard = restartBlock.find("!hasEnabledPollTask && !hasEnabledSubDevice");
    const auto idleReturn = restartBlock.find("ModbusRestartResult::IdleNoEnabledTargets");
    const auto handlerCreate = restartBlock.find("new (std::nothrow) ModbusHandler()");
    TEST_ASSERT_TRUE_MESSAGE(
        rawModeGuard != std::string::npos && noTaskGuard != std::string::npos &&
        idleReturn != std::string::npos && handlerCreate != std::string::npos &&
        rawModeGuard < noTaskGuard && noTaskGuard < idleReturn && idleReturn < handlerCreate,
        "only structured mode may idle when no tasks/devices are enabled; raw mode must start its handler");

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
    const char* hexAscii = "010300010001D5CA";

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
    const uint8_t dlFrame[] = { 0x01, 0x06, 0x00, 0x01, 0x00, 0x05, 0x18, 0x09 };
    String enqueueMsg;
    bool shouldEnqueue;
    handleDataCommand(1, dlFrame, sizeof(dlFrame), enqueueMsg, shouldEnqueue);
    TEST_ASSERT_TRUE(shouldEnqueue);
    TEST_ASSERT_TRUE(enqueueMsg.indexOf("modbus_raw_send") >= 0);
    // 验证帧内容被正确编码
    TEST_ASSERT_TRUE(enqueueMsg.indexOf("0106000100051809") >= 0);
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
    RUN_TEST(test_hex_mode_json_like_binary_command_is_raw_frame);
    RUN_TEST(test_hex_mode_json_command_still_works);

    // 6. HEX 帧大小上限验证
    RUN_TEST(test_hex_frame_over_256_bytes_is_rejected);
    RUN_TEST(test_hex_frame_exactly_256_bytes_is_accepted);
    RUN_TEST(test_modbus_raw_transaction_lengths_are_256_safe);
    RUN_TEST(test_modbus_mqtt_raw_command_requires_complete_crc_adu);
    RUN_TEST(test_modbus_legacy_oneshot_count_matches_copied_capacity);
    RUN_TEST(test_modbus_raw_exception_forwarding_contract);
    RUN_TEST(test_modbus_exception_response_is_binary_mqtt_payload);
    RUN_TEST(test_modbus_raw_mode_starts_without_poll_targets);

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
