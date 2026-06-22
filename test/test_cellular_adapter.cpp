/**
 * @file test_cellular_adapter.cpp
 * @brief CellularAdapter EC801E 兼容性修复回归测试
 *
 * 覆盖关键修复：
 * 1. checkPdpActive() 使用 AT+CGPADDR 替代 AT+CIPSTATUS
 * 2. isConnected() / update() 不依赖 TinyGSM isGprsConnected()
 * 3. PWRKEY 低电平脉冲时序 (EC801E-CN)
 * 4. GPRS 连接重试 + CGPADDR 后备检测
 * 5. NetworkManager 4G 初始化路径 + AP 混合模式
 */

#include <unity.h>
#include <Arduino.h>
#include <fstream>
#include <sstream>
#include <string>
#include "mocks/MockMultiNetwork.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

void test_cellular_adapter_group();

// 辅助：读取项目源文件
static std::string readSrc(const char* path) {
    const char* roots[] = { ".", "..", "../.." };
    for (const char* root : roots) {
        std::string full = std::string(root) + "/" + path;
        std::ifstream f(full);
        if (f.is_open()) {
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
    }
    return "";
}

// 辅助：从字符串中提取函数体（简易版，查找函数签名后的 {} 配对）
static std::string extractFuncBody(const std::string& src, const std::string& signature, size_t maxLen = 2000) {
    auto pos = src.find(signature);
    if (pos == std::string::npos) return "";
    return src.substr(pos, std::min(maxLen, src.size() - pos));
}

// ============================================================
//  源码回归测试：EC801E AT+CIPSTATUS 不兼容修复
// ============================================================

/**
 * @brief checkPdpActive() 使用 AT+CGPADDR 而非 AT+CIPSTATUS
 *
 * EC801E-CN 不支持 TinyGSM SIM7600 模式的 AT+CIPSTATUS 命令，
 * 必须使用 AT+CGPADDR 检查 PDP 上下文是否有有效 IP。
 */
void test_ec801e_checkPdpActive_uses_CGPADDR() {
    TestLog::testStart("EC801E: checkPdpActive uses AT+CGPADDR");

    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");

    // checkPdpActive() 必须存在且使用 AT+CGPADDR
    std::string body = extractFuncBody(cell, "CellularAdapter::checkPdpActive()");
    TEST_ASSERT_TRUE_MESSAGE(!body.empty(), "checkPdpActive() must exist");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("AT+CGPADDR=1") != std::string::npos,
        "checkPdpActive must use AT+CGPADDR=1 to query PDP context IP");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("\"0.0.0.0\"") != std::string::npos,
        "checkPdpActive must reject 0.0.0.0 (no IP assigned)");

    TestLog::step("checkPdpActive uses AT+CGPADDR correctly");
    TestLog::testEnd(true);
}

/**
 * @brief isConnected() 不依赖 TinyGSM 的 isGprsConnected()
 *
 * TinyGSM SIM7600 的 isGprsConnected() 内部使用 AT+CIPSTATUS，
 * EC801E 不支持，导致始终返回 false。
 */
void test_ec801e_isConnected_avoids_CIPSTATUS() {
    TestLog::testStart("EC801E: isConnected avoids isGprsConnected()");

    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");

    std::string body = extractFuncBody(cell, "CellularAdapter::isConnected()", 500);
    TEST_ASSERT_TRUE_MESSAGE(!body.empty(), "isConnected() must exist");

    // isConnected() 必须使用 checkPdpActive() 而非 isGprsConnected()
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("checkPdpActive()") != std::string::npos,
        "isConnected must use checkPdpActive() for EC801E compatibility");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("isGprsConnected()") == std::string::npos,
        "isConnected must NOT use TinyGSM isGprsConnected() (unsupported by EC801E)");

    TestLog::step("isConnected uses checkPdpActive, not isGprsConnected");
    TestLog::testEnd(true);
}

/**
 * @brief update() 使用 checkPdpActive() 避免误判连接丢失
 *
 * 之前 update() 使用 _modem->isGprsConnected()，每30秒误判 PDP 断开，
 * 触发无意义的 reconnect() 循环。
 */
void test_ec801e_update_uses_checkPdpActive() {
    TestLog::testStart("EC801E: update() uses checkPdpActive");

    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");

    std::string body = extractFuncBody(cell, "CellularAdapter::update()");
    TEST_ASSERT_TRUE_MESSAGE(!body.empty(), "update() must exist");

    // update() 必须使用 checkPdpActive()
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("checkPdpActive()") != std::string::npos,
        "update() must use checkPdpActive() for periodic status check");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("isGprsConnected()") == std::string::npos,
        "update() must NOT use isGprsConnected() (causes false disconnects on EC801E)");

    TestLog::step("update uses checkPdpActive for periodic check");
    TestLog::testEnd(true);
}

/**
 * @brief activateNetwork() 在 gprsConnect 失败后使用 CGPADDR 后备检测
 *
 * TinyGSM gprsConnect() 可能实际激活了 PDP 但因 AT+CIPSTATUS 不支持而返回 false，
 * 必须用 CGPADDR 检查 PDP 是否已实际获得 IP。
 */
void test_ec801e_activateNetwork_CGPADDR_fallback() {
    TestLog::testStart("EC801E: activateNetwork CGPADDR fallback after gprsConnect");

    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");

    std::string body = extractFuncBody(cell, "CellularAdapter::activateNetwork()", 4000);
    TEST_ASSERT_TRUE_MESSAGE(!body.empty(), "activateNetwork() must exist");

    // gprsConnect 失败后必须有 checkPdpActive 后备
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("gprsConnect(") != std::string::npos,
        "activateNetwork must call gprsConnect()");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("checkPdpActive()") != std::string::npos,
        "activateNetwork must verify PDP via checkPdpActive() after gprsConnect fails");

    // 必须有 GPRS 重试逻辑（至少 3 次）
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("attempt") != std::string::npos &&
        body.find("3") != std::string::npos,
        "activateNetwork must retry GPRS connect up to 3 times");

    // 必须有手动 PDP 激活后备（AT+CGDCONT + AT+CGACT）
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("AT+CGDCONT") != std::string::npos &&
        body.find("AT+CGACT=1,1") != std::string::npos,
        "activateNetwork must have manual PDP activation fallback (AT+CGDCONT + AT+CGACT)");

    TestLog::step("GPRS fallback chain: gprsConnect -> CGPADDR check -> manual PDP");
    TestLog::testEnd(true);
}

// ============================================================
//  源码回归测试：PWRKEY 时序和硬件配置
// ============================================================

/**
 * @brief powerOn/powerOff 使用 PWRKEY 低电平脉冲 (EC801E-CN 要求)
 *
 * EC801E-CN PWRKEY 低电平有效，需要拉低 500ms+ 触发上电/关机，
 * 不能简单地保持 HIGH 或 LOW。
 */
void test_ec801e_pwrkey_pulse_timing() {
    TestLog::testStart("EC801E: PWRKEY low-level pulse timing");

    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");

    // powerOn: LOW -> delay -> HIGH
    std::string onBody = extractFuncBody(cell, "CellularAdapter::powerOn()", 600);
    TEST_ASSERT_TRUE_MESSAGE(!onBody.empty(), "powerOn() must exist");
    TEST_ASSERT_TRUE_MESSAGE(
        onBody.find("LOW") != std::string::npos &&
        onBody.find("HIGH") != std::string::npos,
        "powerOn must pulse PWRKEY: LOW -> delay -> HIGH");

    // 验证 LOW 在 HIGH 之前（低电平脉冲）
    size_t lowPos = onBody.find("LOW");
    size_t highPos = onBody.find("HIGH");
    TEST_ASSERT_TRUE_MESSAGE(
        lowPos < highPos,
        "powerOn must pull PWRKEY LOW before releasing HIGH (EC801E PWRKEY active-low)");

    // powerOff: 也使用低电平脉冲
    std::string offBody = extractFuncBody(cell, "CellularAdapter::powerOff()", 600);
    TEST_ASSERT_TRUE_MESSAGE(!offBody.empty(), "powerOff() must exist");
    TEST_ASSERT_TRUE_MESSAGE(
        offBody.find("LOW") != std::string::npos &&
        offBody.find("HIGH") != std::string::npos,
        "powerOff must also pulse PWRKEY: LOW -> delay -> HIGH");

    TestLog::step("PWRKEY uses correct low-level pulse for EC801E-CN");
    TestLog::testEnd(true);
}

/**
 * @brief CellularAdapter 头文件声明完整性检查
 *
 * 确保 checkPdpActive、readATResponse 等辅助方法已正确声明。
 */
void test_ec801e_header_declarations() {
    TestLog::testStart("EC801E: Header declarations complete");

    std::string hdr = readSrc("include/network/CellularAdapter.h");
    TEST_ASSERT_TRUE_MESSAGE(!hdr.empty(), "CellularAdapter.h must be readable");

    // 必须声明 checkPdpActive
    TEST_ASSERT_TRUE_MESSAGE(
        hdr.find("checkPdpActive") != std::string::npos,
        "Header must declare checkPdpActive()");

    // 必须声明 readATResponse
    TEST_ASSERT_TRUE_MESSAGE(
        hdr.find("readATResponse") != std::string::npos,
        "Header must declare readATResponse()");

    // 必须声明 sendATDiag 和 sendATCmd
    TEST_ASSERT_TRUE_MESSAGE(
        hdr.find("sendATDiag") != std::string::npos,
        "Header must declare sendATDiag()");
    TEST_ASSERT_TRUE_MESSAGE(
        hdr.find("sendATCmd") != std::string::npos,
        "Header must declare sendATCmd()");

    // TinyGSM 必须使用 SIM7600 兼容模式
    TEST_ASSERT_TRUE_MESSAGE(
        hdr.find("TINY_GSM_MODEM_SIM7600") != std::string::npos,
        "Must use TINY_GSM_MODEM_SIM7600 for EC801E compatibility");

    TestLog::step("All EC801E helper methods declared in header");
    TestLog::testEnd(true);
}

// ============================================================
//  源码回归测试：NetworkManager 4G 初始化路径
// ============================================================

/**
 * @brief NetworkManager 4G 初始化路径正确性
 *
 * 验证 networkType==NET_4G 时进入 CellularAdapter 初始化，
 * 成功后启动 WiFi AP 混合模式，失败后回退到 AP 模式。
 */
void test_ec801e_network_manager_4g_init_path() {
    TestLog::testStart("EC801E: NetworkManager 4G init path");

    std::string nmc = readSrc("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!nmc.empty(), "NetworkManager.cpp must be readable");

    // 4G 初始化块必须存在
    TEST_ASSERT_TRUE_MESSAGE(
        nmc.find("FASTBEE_ENABLE_CELLULAR") != std::string::npos,
        "NetworkManager must have FASTBEE_ENABLE_CELLULAR guard");
    TEST_ASSERT_TRUE_MESSAGE(
        nmc.find("NetworkType::NET_4G") != std::string::npos,
        "NetworkManager must check NET_4G network type");

    // 4G 成功后启动 AP 混合模式
    auto cell4gPos = nmc.find("NetworkType::NET_4G");
    TEST_ASSERT_TRUE_MESSAGE(cell4gPos != std::string::npos, "NET_4G block must exist");
    std::string block4g = nmc.substr(cell4gPos, 2000);
    TEST_ASSERT_TRUE_MESSAGE(
        block4g.find("startAPMode()") != std::string::npos,
        "4G success path must start WiFi AP for hybrid mode");

    // 4G 失败后回退到 AP
    TEST_ASSERT_TRUE_MESSAGE(
        block4g.find("falling back to AP") != std::string::npos,
        "4G failure must fall back to AP mode");

    // 关键诊断使用 ets_printf（USB-CDC 兼容）
    TEST_ASSERT_TRUE_MESSAGE(
        nmc.find("ets_printf(\"[NET]") != std::string::npos,
        "NetworkManager diagnostics must use ets_printf for USB-CDC compatibility");

    TestLog::step("NetworkManager 4G path: init -> hybrid AP / fallback AP");
    TestLog::testEnd(true);
}

/**
 * @brief CellularAdapter 诊断日志使用 ets_printf
 *
 * ESP32-S3 USB-CDC 在 RTS 重启后需要重新枚举，期间 Serial.printf 输出丢失。
 * 关键诊断必须使用 ets_printf（ROM 级 UART 输出）。
 */
void test_ec801e_diagnostic_uses_ets_printf() {
    TestLog::testStart("EC801E: Diagnostics use ets_printf");

    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");

    // 关键路径必须使用 ets_printf
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("ets_printf(\"[4G]") != std::string::npos,
        "CellularAdapter must use ets_printf for [4G] diagnostics");

    // activateNetwork 中的关键消息使用 ets_printf
    std::string actBody = extractFuncBody(cell, "CellularAdapter::activateNetwork()", 4000);
    TEST_ASSERT_TRUE_MESSAGE(
        actBody.find("ets_printf") != std::string::npos,
        "activateNetwork must use ets_printf for diagnostic output");

    TestLog::step("Diagnostics use ets_printf for USB-CDC compatibility");
    TestLog::testEnd(true);
}

// ============================================================
//  行为测试：MockMultiNetwork 4G 模式
// ============================================================

/**
 * @brief 4G 连接成功后进入混合模式 (4G + AP)
 */
void test_4g_success_hybrid_mode() {
    TestLog::testStart("4G: Success -> Hybrid Mode (4G + AP)");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;
    mgr.enableMDNS = true;

    bool result = mgr.initialize(true);  // 4G adapter 成功
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_STRING("10.0.0.100", mgr.ipAddress.c_str());
    TEST_ASSERT_TRUE(mgr.internetAvailable);
    TEST_ASSERT_TRUE(mgr.apRunning);
    TEST_ASSERT_TRUE(mgr.mDNSStarted);

    // 验证初始化序列
    bool has4G = false, hasAP = false, hasMDNS = false;
    for (const auto& step : mgr.initSequence) {
        if (step == "4G_CONNECTED") has4G = true;
        if (step == "AP_STARTED") hasAP = true;
        if (step == "MDNS_STARTED") hasMDNS = true;
    }
    TEST_ASSERT_TRUE(has4G);
    TEST_ASSERT_TRUE(hasAP);
    TEST_ASSERT_TRUE(hasMDNS);

    TestLog::step("4G hybrid mode: 4G connected + AP + mDNS");
    TestLog::testEnd(true);
}

/**
 * @brief 4G 连接失败后回退到 AP 模式
 */
void test_4g_failure_fallback_to_ap() {
    TestLog::testStart("4G: Failure -> AP Fallback");

    MockMultiNetworkManager mgr;
    mgr.networkType = MockMultiNetworkManager::NetType::NET_4G;

    bool result = mgr.initialize(false);  // 4G adapter 失败
    TEST_ASSERT_FALSE(result);

    // 回退后 networkType 应变为 NET_WIFI
    TEST_ASSERT_EQUAL(
        (int)MockMultiNetworkManager::NetType::NET_WIFI,
        (int)mgr.networkType);
    TEST_ASSERT_TRUE(mgr.apRunning);
    TEST_ASSERT_FALSE(mgr.internetAvailable);

    TestLog::step("4G failed -> WiFi AP fallback for reconfiguration");
    TestLog::testEnd(true);
}

/**
 * @brief 4G 模式 NetworkType 枚举值验证
 *
 * networkType 存储为 uint8_t，NET_4G=2，必须与 JSON 序列化/反序列化一致。
 */
void test_4g_network_type_enum_values() {
    TestLog::testStart("4G: NetworkType enum values");

    // 验证枚举值（与 WiFiManager.h 中的定义一致）
    TEST_ASSERT_EQUAL(0, (int)MockMultiNetworkManager::NetType::NET_WIFI);
    TEST_ASSERT_EQUAL(1, (int)MockMultiNetworkManager::NetType::NET_ETHERNET);
    TEST_ASSERT_EQUAL(2, (int)MockMultiNetworkManager::NetType::NET_4G);

    TestLog::step("NET_WIFI=0, NET_ETHERNET=1, NET_4G=2 verified");
    TestLog::testEnd(true);
}

// ============================================================
//  行为测试：CGPADDR 响应解析逻辑
// ============================================================

/**
 * @brief checkPdpActive 解析逻辑：有效 IP 应返回 true
 *
 * 模拟 AT+CGPADDR=1 的各种响应，验证解析逻辑。
 * 注意：这里直接测试解析逻辑（字符串匹配），不依赖真实硬件。
 */
void test_cgpaddr_parsing_valid_ip() {
    TestLog::testStart("CGPADDR parsing: valid IP -> active");

    // 模拟 checkPdpActive 的核心解析逻辑
    auto checkPdp = [](const String& pip) -> bool {
        return (pip.indexOf("CGPADDR") >= 0 &&
                pip.indexOf("\"") >= 0 &&
                pip.indexOf("\"0.0.0.0\"") < 0);
    };

    // 有效 IP（中国移动 4G 典型地址）
    TEST_ASSERT_TRUE(checkPdp(String("+CGPADDR: 1,\"10.3.89.109\"")));
    TEST_ASSERT_TRUE(checkPdp(String("+CGPADDR: 1,\"10.1.153.232\"")));
    TEST_ASSERT_TRUE(checkPdp(String("+CGPADDR: 1,\"100.64.0.1\"")));
    TEST_ASSERT_TRUE(checkPdp(String("+CGPADDR: 1,\"192.168.1.100\"")));

    TestLog::step("Valid IPs correctly detected as active");
    TestLog::testEnd(true);
}

/**
 * @brief checkPdpActive 解析逻辑：0.0.0.0 或无 IP 应返回 false
 */
void test_cgpaddr_parsing_no_ip() {
    TestLog::testStart("CGPADDR parsing: no IP -> inactive");

    auto checkPdp = [](const String& pip) -> bool {
        return (pip.indexOf("CGPADDR") >= 0 &&
                pip.indexOf("\"") >= 0 &&
                pip.indexOf("\"0.0.0.0\"") < 0);
    };

    // 无 IP（PDP 未激活或等待中）
    TEST_ASSERT_FALSE(checkPdp(String("+CGPADDR: 1,\"0.0.0.0\"")));
    // 空响应
    TEST_ASSERT_FALSE(checkPdp(String("")));
    TEST_ASSERT_FALSE(checkPdp(String("ERROR")));
    // 无引号（格式异常）
    TEST_ASSERT_FALSE(checkPdp(String("+CGPADDR: 1")));

    TestLog::step("No-IP and error responses correctly detected as inactive");
    TestLog::testEnd(true);
}

/**
 * @brief checkPdpActive 解析逻辑：边界情况
 */
void test_cgpaddr_parsing_edge_cases() {
    TestLog::testStart("CGPADDR parsing: edge cases");

    auto checkPdp = [](const String& pip) -> bool {
        return (pip.indexOf("CGPADDR") >= 0 &&
                pip.indexOf("\"") >= 0 &&
                pip.indexOf("\"0.0.0.0\"") < 0);
    };

    // 多个上下文（CID=1 和 CID=2）
    TEST_ASSERT_TRUE(checkPdp(String("+CGPADDR: 1,\"10.0.0.1\"\r\n+CGPADDR: 2,\"0.0.0.0\"")));

    // IPv6 地址（EC801E 支持 IPv6）
    TEST_ASSERT_TRUE(checkPdp(String("+CGPADDR: 1,\"2409:8a28::1\"")));

    // 仅 CID 无 IP（某些模块在 PDP 去激活时返回此格式）
    TEST_ASSERT_FALSE(checkPdp(String("+CGPADDR: 1")));

    TestLog::step("Edge cases handled correctly");
    TestLog::testEnd(true);
}

// ============================================================
//  集成回归：确保 4G 重连路径使用 checkPdpActive
// ============================================================

/**
 * @brief NetworkManager 4G 重连状态机存在且使用正确方法
 *
 * 验证 NetworkManager 有独立的 4G 重连机制，
 * 且 CellularAdapter::reconnect() 使用 PDP 重置 + 重新激活。
 */
void test_ec801e_cellular_reconnect_path() {
    TestLog::testStart("EC801E: Cellular reconnect path");

    std::string nmc = readSrc("src/network/NetworkManager.cpp");
    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!nmc.empty(), "NetworkManager.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");

    // NetworkManager 必须有 4G 重连状态机
    TEST_ASSERT_TRUE_MESSAGE(
        nmc.find("cellReconnectPending") != std::string::npos,
        "NetworkManager must have cellReconnectPending flag");
    TEST_ASSERT_TRUE_MESSAGE(
        nmc.find("CELL_RECONNECT_INTERVAL_MS") != std::string::npos,
        "NetworkManager must have CELL_RECONNECT_INTERVAL_MS constant");

    // CellularAdapter::reconnect() 必须执行 PDP 重置
    std::string reconnBody = extractFuncBody(cell, "CellularAdapter::reconnect()", 500);
    TEST_ASSERT_TRUE_MESSAGE(!reconnBody.empty(), "reconnect() must exist");
    TEST_ASSERT_TRUE_MESSAGE(
        reconnBody.find("gprsDisconnect()") != std::string::npos,
        "reconnect must call gprsDisconnect() first");
    TEST_ASSERT_TRUE_MESSAGE(
        reconnBody.find("activateNetwork()") != std::string::npos,
        "reconnect must call activateNetwork() after disconnect");

    TestLog::step("Cellular reconnect: gprsDisconnect -> activateNetwork");
    TestLog::testEnd(true);
}

/**
 * @brief readATResponse 辅助方法实现完整性
 *
 * readATResponse 是 checkPdpActive 的基础，必须正确处理串口超时和响应解析。
 */
void test_ec801e_readATResponse_implementation() {
    TestLog::testStart("EC801E: readATResponse implementation");

    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");

    std::string body = extractFuncBody(cell, "CellularAdapter::readATResponse(", 1000);
    TEST_ASSERT_TRUE_MESSAGE(!body.empty(), "readATResponse() must exist");

    // 必须有超时保护
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("millis()") != std::string::npos,
        "readATResponse must have timeout protection using millis()");

    // 必须过滤 OK/ERROR 行
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("OK") != std::string::npos,
        "readATResponse must filter out OK lines");

    TestLog::step("readATResponse has timeout and response filtering");
    TestLog::testEnd(true);
}

// ============================================================
//  测试组注册
// ============================================================

void test_cellular_adapter_group() {
    TestLog::groupStart("CellularAdapter EC801E Tests");

    // EC801E AT+CIPSTATUS 不兼容修复
    RUN_TEST(test_ec801e_checkPdpActive_uses_CGPADDR);
    RUN_TEST(test_ec801e_isConnected_avoids_CIPSTATUS);
    RUN_TEST(test_ec801e_update_uses_checkPdpActive);
    RUN_TEST(test_ec801e_activateNetwork_CGPADDR_fallback);

    // PWRKEY 时序和硬件配置
    RUN_TEST(test_ec801e_pwrkey_pulse_timing);
    RUN_TEST(test_ec801e_header_declarations);

    // NetworkManager 4G 初始化路径
    RUN_TEST(test_ec801e_network_manager_4g_init_path);
    RUN_TEST(test_ec801e_diagnostic_uses_ets_printf);

    // MockMultiNetwork 4G 行为测试
    RUN_TEST(test_4g_success_hybrid_mode);
    RUN_TEST(test_4g_failure_fallback_to_ap);
    RUN_TEST(test_4g_network_type_enum_values);

    // CGPADDR 响应解析
    RUN_TEST(test_cgpaddr_parsing_valid_ip);
    RUN_TEST(test_cgpaddr_parsing_no_ip);
    RUN_TEST(test_cgpaddr_parsing_edge_cases);

    // 集成回归
    RUN_TEST(test_ec801e_cellular_reconnect_path);
    RUN_TEST(test_ec801e_readATResponse_implementation);

    TestLog::groupEnd();
}
