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

static bool mockCheckPdpActiveResponse(const String& pip) {
    int cid1 = pip.indexOf("+CGPADDR: 1");
    if (cid1 < 0) return false;
    int quoteStart = pip.indexOf('"', cid1);
    int quoteEnd = quoteStart >= 0 ? pip.indexOf('"', quoteStart + 1) : -1;
    if (quoteStart < 0 || quoteEnd <= quoteStart + 1) return false;
    String ip = pip.substring(quoteStart + 1, quoteEnd);
    ip.trim();
    return !ip.isEmpty() && ip != "0.0.0.0" && ip != "::" && ip != "0:0:0:0:0:0:0:0";
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
        body.find("hasAssignedPdpAddress") != std::string::npos &&
        cell.find("\"0.0.0.0\"") != std::string::npos,
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
/**
 * @brief activateNetwork() must reject a stale PDP with local IP 0.0.0.0.
 *
 * EC801E can report an active PDP while TinyGSM still exposes localIP() as
 * 0.0.0.0. MQTT over software TLS needs a working TCP socket, so startup must
 * reset that stale PDP and retry instead of returning connected.
 */
void test_ec801e_activateNetwork_rejects_invalid_local_ip() {
    TestLog::testStart("EC801E: activateNetwork rejects invalid local IP");

    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");

    std::string body = extractFuncBody(cell, "CellularAdapter::activateNetwork()", 7000);
    TEST_ASSERT_TRUE_MESSAGE(!body.empty(), "activateNetwork() must exist");

    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("hasUsableLocalIpv4") != std::string::npos,
        "CellularAdapter must have a helper that rejects 0.0.0.0 local IP");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("hasUsableLocalIpv4(ip)") != std::string::npos,
        "activateNetwork must validate the modem local IP before reporting connected");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("PDP active but local IP invalid") != std::string::npos &&
        body.find("GPRS connected but local IP invalid") != std::string::npos &&
        body.find("PDP has CGPADDR but local IP invalid") != std::string::npos,
        "activateNetwork must log and retry stale PDP/local-IP mismatch states");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("_modem->gprsDisconnect()") != std::string::npos,
        "activateNetwork must reset stale PDP contexts before retrying");

    TestLog::step("Invalid local IP is rejected before TCP/MQTTS use");
    TestLog::testEnd(true);
}

void test_ec801e_clients_gate_io_on_pdp_state() {
    TestLog::testStart("EC801E: cellular clients gate IO on PDP state");

    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("CellularTrackedClient(Client* inner, bool& busyFlag, bool& networkReady)") != std::string::npos,
        "Plain cellular client must receive a PDP/network-ready gate");
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("CellularSoftwareTlsClient(Client* inner, bool& busyFlag, bool& networkReady)") != std::string::npos,
        "Software TLS cellular client must receive a PDP/network-ready gate");
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("if (!_networkReady) return 0;") != std::string::npos &&
        cell.find("if (!_networkReady) return -1;") != std::string::npos,
        "Cellular clients must reject connect/write/read attempts while PDP is offline");
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("return (_networkReady && _inner) ? _inner->connected() : 0;") != std::string::npos &&
        cell.find("return _networkReady ? SSLClient::connected() : 0;") != std::string::npos,
        "Cellular clients must report disconnected when PDP is offline");
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("CellularTrackedClient(_gsmClient, _clientBusy, _connected)") != std::string::npos &&
        cell.find("CellularSoftwareTlsClient(tlsBaseClient, _clientBusy, _connected)") != std::string::npos,
        "Cellular clients must be wired to the adapter connection state");

    TestLog::step("PDP gate protects MQTT and MQTTS transports");
    TestLog::testEnd(true);
}

void test_ec801e_activateNetwork_requires_cgpaddr_after_gprsConnect() {
    TestLog::testStart("EC801E: gprsConnect success must verify CGPADDR");

    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");

    std::string body = extractFuncBody(cell, "CellularAdapter::activateNetwork()", 7000);
    TEST_ASSERT_TRUE_MESSAGE(!body.empty(), "activateNetwork() must exist");

    TEST_ASSERT_TRUE_MESSAGE(
        body.find("hasUsableLocalIpv4(ip) && checkPdpActive()") != std::string::npos,
        "gprsConnect success path must require both localIP and CGPADDR before reporting connected");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("GPRS connected but CGPADDR inactive") != std::string::npos,
        "activateNetwork must log and retry when localIP exists but CGPADDR is inactive");

    TestLog::step("TinyGSM localIP is not trusted without CGPADDR");
    TestLog::testEnd(true);
}

void test_ec801e_pdp_loss_stops_stale_clients() {
    TestLog::testStart("EC801E: PDP loss stops stale clients");

    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");

    std::string updateBody = extractFuncBody(cell, "CellularAdapter::update()", 1800);
    TEST_ASSERT_TRUE_MESSAGE(!updateBody.empty(), "update() must exist");
    TEST_ASSERT_TRUE_MESSAGE(
        updateBody.find("GPRS connection lost (PDP no IP)") != std::string::npos &&
        updateBody.find("_softwareTlsClient->stop()") != std::string::npos &&
        updateBody.find("_trackedClient->stop()") != std::string::npos &&
        updateBody.find("_gsmClient->stop()") != std::string::npos,
        "PDP loss must close stale MQTT/MQTTS sockets immediately");

    std::string reconnBody = extractFuncBody(cell, "CellularAdapter::reconnect()", 1200);
    TEST_ASSERT_TRUE_MESSAGE(!reconnBody.empty(), "reconnect() must exist");
    TEST_ASSERT_TRUE_MESSAGE(
        reconnBody.find("_softwareTlsClient->stop()") != std::string::npos &&
        reconnBody.find("_trackedClient->stop()") != std::string::npos &&
        reconnBody.find("_modem->gprsDisconnect()") != std::string::npos,
        "4G reconnect must stop stale clients before resetting PDP");

    TestLog::step("PDP loss closes stale sockets before reconnect");
    TestLog::testEnd(true);
}

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
        hdr.find("#define TINY_GSM_MODEM_BG96") != std::string::npos,
        "Must use TINY_GSM_MODEM_BG96 so EC801E TCP uses QIOPEN/QISEND/QIRD");
    TEST_ASSERT_TRUE_MESSAGE(
        hdr.find("#define TINY_GSM_MODEM_SIM7600") == std::string::npos,
        "Must not compile EC801E with SIM7600/CIP socket backend");

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
/**
 * @brief 4G + MQTTS must prefer software TLS, with EC801E QSSL retained as fallback.
 */
void test_ec801e_mqtts_uses_secure_client() {
    TestLog::testStart("EC801E: MQTTS uses software TLS client");

    std::string hdr = readSrc("include/network/CellularAdapter.h");
    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    std::string mqttcpp = readSrc("src/protocols/MQTTClient.cpp");
    std::string pm = readSrc("src/protocols/ProtocolManager.cpp");
    std::string route = readSrc("src/network/handlers/MqttRouteHandler.cpp");
    std::string pio = readSrc("platformio.ini");
    TEST_ASSERT_TRUE_MESSAGE(!hdr.empty(), "CellularAdapter.h must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!mqttcpp.empty(), "MQTTClient.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!pm.empty(), "ProtocolManager.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!route.empty(), "MqttRouteHandler.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!pio.empty(), "platformio.ini must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        hdr.find("class QuectelSslClient") != std::string::npos &&
        hdr.find("QuectelSslClient* _qsslClient") != std::string::npos &&
        hdr.find("getSecureClient()") != std::string::npos,
        "CellularAdapter must expose the EC801E QuectelSslClient via getSecureClient()");
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("new (std::nothrow) QuectelSslClient") != std::string::npos,
        "CellularAdapter must instantiate QuectelSslClient with nothrow (safe alloc)");
    TEST_ASSERT_TRUE_MESSAGE(
        hdr.find("CellularTrackedClient* _trackedClient") != std::string::npos &&
        hdr.find("CellularSoftwareTlsClient* _softwareTlsClient") != std::string::npos &&
        hdr.find("bool _clientBusy") != std::string::npos,
        "CellularAdapter must track 4G AT client usage for software TLS");
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("#include <SSLClient.h>") != std::string::npos &&
        cell.find("new (std::nothrow) CellularSoftwareTlsClient") != std::string::npos &&
        cell.find("setInsecure()") != std::string::npos &&
        cell.find("setCACertBundle(nullptr)") != std::string::npos &&
        cell.find("prepareHandshake();") != std::string::npos &&
        cell.find("TinyGSM TCP + ESP32 mbedTLS") != std::string::npos,
        "EC801E MQTTS must reset ESP32 software TLS to insecure/no CA bundle before every handshake");
    std::string secureBody = extractFuncBody(cell, "CellularAdapter::getSecureClient()", 500);
    TEST_ASSERT_TRUE_MESSAGE(!secureBody.empty(), "getSecureClient() must exist");
    size_t qsslPos = secureBody.find("if (_qsslClient) return static_cast<Client*>(_qsslClient)");
    size_t softwareTlsPos = secureBody.find("if (_softwareTlsClient) return static_cast<Client*>(_softwareTlsClient)");
    TEST_ASSERT_TRUE_MESSAGE(
        qsslPos != std::string::npos && softwareTlsPos != std::string::npos && softwareTlsPos < qsslPos,
        "getSecureClient() must prefer software TLS because tested EC801E firmware rejects QSSLCFG");
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("new (std::nothrow) QuectelSslClient(*_serial, 1, 4, 1)") != std::string::npos,
        "EC801E QSSL must use nothrow alloc with PDP context 1, client 4, and SSL context 1");
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("_contextId, _sslContextId, _connectId") != std::string::npos,
        "QSSLOPEN must pass context, SSL context, then client id");
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("AT+QSSLCFG") != std::string::npos &&
        cell.find("AT+QSSLOPEN") != std::string::npos &&
        cell.find("AT+QSSLSEND") != std::string::npos &&
        cell.find("AT+QSSLRECV") != std::string::npos,
        "Cellular MQTTS must use Quectel QSSL socket commands");
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("AT+QICSGP") != std::string::npos &&
        cell.find("AT+QIACT") != std::string::npos,
        "Quectel QSSL client must activate the Quectel PDP context before QSSLOPEN");
    TEST_ASSERT_TRUE_MESSAGE(
        hdr.find("isBusy() const") != std::string::npos &&
        cell.find("Skipping PDP check while cellular client AT session is busy") != std::string::npos &&
        cell.find("Skipping PDP check while QSSL AT session is busy") != std::string::npos,
        "CellularAdapter must not interleave PDP polling with MQTT/TLS AT sessions");
    TEST_ASSERT_TRUE_MESSAGE(
        mqttcpp.find("if (isMqttsReconnect)") != std::string::npos &&
        mqttcpp.find("ensureMbedtlsAllocatorForMqtts();") != std::string::npos &&
        mqttcpp.find("_externalClient->stop();") != std::string::npos &&
        mqttcpp.find("needsMqttsDramBudget") != std::string::npos,
        "MQTTS memory governance must also cover external 4G software TLS transports");
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("new TinyGsmClientSecure") == std::string::npos &&
        cell.find("+CCHOPEN=") == std::string::npos,
        "EC801E MQTTS must not use the SIMCom CCH secure-socket command path");
    TEST_ASSERT_TRUE_MESSAGE(
        pm.find("getSecureClient()") != std::string::npos &&
        pm.find("scheme") != std::string::npos &&
        pm.find("mqtts") != std::string::npos,
        "ProtocolManager must select cellular secure client for mqtts");
    TEST_ASSERT_TRUE_MESSAGE(
        route.find("getSecureClient()") != std::string::npos,
        "MQTT test route must use cellular secure client for mqtts");
    TEST_ASSERT_TRUE_MESSAGE(
        pio.find("patch-tinygsm-sim7600-secure.py") == std::string::npos,
        "Build must not apply the unused TinyGSM SIMCom CCH secure-client patch");
    TEST_ASSERT_TRUE_MESSAGE(
        pio.find("digitaldragon/SSLClient@1.3.2") != std::string::npos,
        "Cellular software TLS dependency must be pinned for 4G MQTTS");

    TestLog::step("4G MQTTS uses software TLS first, with EC801E QSSL fallback");
    TestLog::testEnd(true);
}

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
    auto cell4gPos = nmc.find("if (wifiConfig.networkType == NetworkType::NET_4G)");
    TEST_ASSERT_TRUE_MESSAGE(cell4gPos != std::string::npos, "NET_4G block must exist");
    auto cell4gEnd = nmc.find("#endif", cell4gPos);
    std::string block4g = nmc.substr(cell4gPos, cell4gEnd - cell4gPos);
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
    // 有效 IP（中国移动 4G 典型地址）
    TEST_ASSERT_TRUE(mockCheckPdpActiveResponse(String("+CGPADDR: 1,\"10.3.89.109\"")));
    TEST_ASSERT_TRUE(mockCheckPdpActiveResponse(String("+CGPADDR: 1,\"10.1.153.232\"")));
    TEST_ASSERT_TRUE(mockCheckPdpActiveResponse(String("+CGPADDR: 1,\"100.64.0.1\"")));
    TEST_ASSERT_TRUE(mockCheckPdpActiveResponse(String("+CGPADDR: 1,\"192.168.1.100\"")));

    TestLog::step("Valid IPs correctly detected as active");
    TestLog::testEnd(true);
}

/**
 * @brief checkPdpActive 解析逻辑：0.0.0.0 或无 IP 应返回 false
 */
void test_cgpaddr_parsing_no_ip() {
    TestLog::testStart("CGPADDR parsing: no IP -> inactive");

    // 无 IP（PDP 未激活或等待中）
    TEST_ASSERT_FALSE(mockCheckPdpActiveResponse(String("+CGPADDR: 1,\"0.0.0.0\"")));
    // 空响应
    TEST_ASSERT_FALSE(mockCheckPdpActiveResponse(String("")));
    TEST_ASSERT_FALSE(mockCheckPdpActiveResponse(String("ERROR")));
    // 无引号（格式异常）
    TEST_ASSERT_FALSE(mockCheckPdpActiveResponse(String("+CGPADDR: 1")));

    TestLog::step("No-IP and error responses correctly detected as inactive");
    TestLog::testEnd(true);
}

/**
 * @brief checkPdpActive 解析逻辑：边界情况
 */
void test_cgpaddr_parsing_edge_cases() {
    TestLog::testStart("CGPADDR parsing: edge cases");

    // 多个上下文（CID=1 和 CID=2）
    TEST_ASSERT_TRUE(mockCheckPdpActiveResponse(String("+CGPADDR: 1,\"10.0.0.1\"\r\n+CGPADDR: 2,\"0.0.0.0\"")));

    // IPv6 地址（EC801E 支持 IPv6）
    TEST_ASSERT_TRUE(mockCheckPdpActiveResponse(String("+CGPADDR: 1,\"2409:8a28::1\"")));

    // 仅 CID 无 IP（某些模块在 PDP 去激活时返回此格式）
    TEST_ASSERT_FALSE(mockCheckPdpActiveResponse(String("+CGPADDR: 1")));

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
//  源码回归测试：内存安全分配
// ============================================================

/**
 * @brief CellularAdapter 所有动态分配的客户端必须使用 std::nothrow
 *
 * ESP32 内存受限环境下，裸 new 在 DRAM 不足时调用 abort() 导致系统重启。
 * 所有客户端分配必须使用 new (std::nothrow) 并处理分配失败。
 */
void test_ec801e_all_clients_use_nothrow_alloc() {
    TestLog::testStart("EC801E: All cellular clients use nothrow allocation");

    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");

    // 1. CellularTrackedClient 必须使用 nothrow
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("new (std::nothrow) CellularTrackedClient") != std::string::npos,
        "CellularTrackedClient must use nothrow allocation");
    TestLog::step("CellularTrackedClient uses nothrow");

    // 2. CellularSoftwareTlsClient 必须使用 nothrow
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("new (std::nothrow) CellularSoftwareTlsClient") != std::string::npos,
        "CellularSoftwareTlsClient must use nothrow allocation");
    TestLog::step("CellularSoftwareTlsClient uses nothrow");

    // 3. QuectelSslClient 必须使用 nothrow（修复：之前使用裸 new）
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("new (std::nothrow) QuectelSslClient") != std::string::npos,
        "QuectelSslClient must use nothrow allocation (DRAM abort prevention)");
    TestLog::step("QuectelSslClient uses nothrow");

    // 4. 分配失败必须有日志警告
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("QuectelSslClient allocation failed") != std::string::npos,
        "QuectelSslClient allocation failure must be logged");
    TEST_ASSERT_TRUE_MESSAGE(
        cell.find("Software TLS client allocation failed") != std::string::npos,
        "Software TLS client allocation failure must be logged");
    TestLog::step("Allocation failures are logged");

    // 5. 不应存在裸 new 分配客户端（排除 TinyGsm 等第三方库内部）
    // 在 _qsslClient/_trackedClient/_softwareTlsClient 赋值行中不应有裸 new
    size_t qsslLine = cell.find("_qsslClient = new ");
    TEST_ASSERT_TRUE_MESSAGE(qsslLine != std::string::npos, "_qsslClient allocation must exist");
    std::string qsslAlloc = cell.substr(qsslLine, 80);
    TEST_ASSERT_TRUE_MESSAGE(
        qsslAlloc.find("std::nothrow") != std::string::npos,
        "_qsslClient must not use bare new (causes abort on ESP32 low DRAM)");
    TestLog::step("No bare new for cellular client allocation");

    TestLog::testEnd(true);
}

// ============================================================
//  源码回归测试：4G 状态轮询实时性修复
// ============================================================

/**
 * @brief forceUpdate() 方法存在且使用 checkPdpActive
 */
void test_ec801e_forceUpdate_exists() {
    TestLog::testStart("EC801E: forceUpdate() exists and uses checkPdpActive");

    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    std::string hdr = readSrc("include/network/CellularAdapter.h");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");
    TEST_ASSERT_TRUE_MESSAGE(!hdr.empty(), "CellularAdapter.h must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        hdr.find("forceUpdate()") != std::string::npos,
        "Header must declare forceUpdate()");

    std::string body = extractFuncBody(cell, "CellularAdapter::forceUpdate()", 1500);
    TEST_ASSERT_TRUE_MESSAGE(!body.empty(), "forceUpdate() implementation must exist");

    TEST_ASSERT_TRUE_MESSAGE(
        body.find("checkPdpActive()") != std::string::npos,
        "forceUpdate must use checkPdpActive() for real-time PDP check");

    TEST_ASSERT_TRUE_MESSAGE(
        body.find("_lastCheckTime = millis()") != std::string::npos,
        "forceUpdate must update _lastCheckTime to avoid rapid duplicate checks");

    TEST_ASSERT_TRUE_MESSAGE(
        body.find("_connected = true") != std::string::npos,
        "forceUpdate must set _connected = true when PDP is active");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("_connected = false") != std::string::npos,
        "forceUpdate must set _connected = false when PDP is inactive");

    TestLog::step("forceUpdate uses checkPdpActive and updates _connected flag");
    TestLog::testEnd(true);
}

/**
 * @brief update() 使用 10 秒检查间隔（而非之前的 30 秒）
 */
void test_ec801e_update_interval_10s() {
    TestLog::testStart("EC801E: update() uses 10s check interval");

    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");

    std::string body = extractFuncBody(cell, "CellularAdapter::update()", 2000);
    TEST_ASSERT_TRUE_MESSAGE(!body.empty(), "update() must exist");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("10000") != std::string::npos,
        "update() must use 10000ms (10s) check interval for faster status updates");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("30000") == std::string::npos,
        "update() must NOT use 30000ms interval (reduced to 10s)");

    TestLog::step("update() uses 10s interval (reduced from 30s)");
    TestLog::testEnd(true);
}

/**
 * @brief updateStatusInfo() 在读取 isConnected() 前调用 forceUpdate()
 */
void test_ec801e_updateStatusInfo_calls_forceUpdate() {
    TestLog::testStart("EC801E: updateStatusInfo calls forceUpdate before isConnected");

    std::string nmc = readSrc("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!nmc.empty(), "NetworkManager.cpp must be readable");

    // 锚定 updateStatusInfo 函数，而非第一个 NET_4G 块
    auto funcPos = nmc.find("FBNetworkManager::updateStatusInfo()");
    TEST_ASSERT_TRUE_MESSAGE(funcPos != std::string::npos, "updateStatusInfo() must exist");

    auto cell4gPos = nmc.find("if (wifiConfig.networkType == NetworkType::NET_4G && cellularAdapter)", funcPos);
    TEST_ASSERT_TRUE_MESSAGE(cell4gPos != std::string::npos, "NET_4G block must exist after updateStatusInfo");

    auto blockEnd = nmc.find("#endif", cell4gPos);
    std::string block4g = nmc.substr(cell4gPos, blockEnd - cell4gPos);

    size_t forceUpdatePos = block4g.find("forceUpdate()");
    size_t isConnectedPos = block4g.find("isConnected()");

    TEST_ASSERT_TRUE_MESSAGE(
        forceUpdatePos != std::string::npos,
        "updateStatusInfo NET_4G block must call cellularAdapter->forceUpdate()");
    TEST_ASSERT_TRUE_MESSAGE(
        isConnectedPos != std::string::npos,
        "updateStatusInfo NET_4G block must call cellularAdapter->isConnected()");
    TEST_ASSERT_TRUE_MESSAGE(
        forceUpdatePos < isConnectedPos,
        "forceUpdate() must be called BEFORE isConnected() for real-time status");

    TestLog::step("forceUpdate() called before isConnected() in updateStatusInfo");
    TestLog::testEnd(true);
}

/**
 * @brief 前端 saveCellularConfig 保存后启动状态自动轮询
 */
void test_frontend_cellular_save_polling() {
    TestLog::testStart("Frontend: saveCellularConfig starts status polling");

    std::string js = readSrc("web-src/modules/runtime/network.js");
    TEST_ASSERT_TRUE_MESSAGE(!js.empty(), "network.js must be readable");

    TEST_ASSERT_TRUE_MESSAGE(
        js.find("_startNetworkStatusPolling(") != std::string::npos,
        "network.js must have _startNetworkStatusPolling() method");

    TEST_ASSERT_TRUE_MESSAGE(
        js.find("_stopNetworkStatusPolling(") != std::string::npos,
        "network.js must have _stopNetworkStatusPolling() method");

    // 在整个文件中搜索 _startNetworkStatusPolling(240000 调用
    TEST_ASSERT_TRUE_MESSAGE(
        js.find("_startNetworkStatusPolling(240000") != std::string::npos,
        "saveCellularConfig must start 4-minute polling after successful save");

    // 验证 240000 调用在 _startSaveBtnCountdown 之后（即不是旧代码）
    auto pollingPos = js.find("_startNetworkStatusPolling(240000");
    auto cellSavePos = js.find("saveCellularConfig()");
    TEST_ASSERT_TRUE_MESSAGE(
        cellSavePos != std::string::npos && pollingPos > cellSavePos,
        "The 240000 polling call must appear after saveCellularConfig");

    TestLog::step("saveCellularConfig starts 4-minute status polling");
    TestLog::testEnd(true);
}

/**
 * @brief 前端联网方式切换到 4G/以太网时启动状态轮询
 */
void test_frontend_network_type_change_polling() {
    TestLog::testStart("Frontend: _onNetworkTypeChange starts polling for 4G/Ethernet");

    std::string js = readSrc("web-src/modules/runtime/network.js");
    TEST_ASSERT_TRUE_MESSAGE(!js.empty(), "network.js must be readable");

    // 验证 _onNetworkTypeChange 定义中存在 240000 和 _startNetworkStatusPolling
    auto defPos = js.find("_onNetworkTypeChange(value)");
    TEST_ASSERT_TRUE_MESSAGE(defPos != std::string::npos, "_onNetworkTypeChange definition must exist");

    // 找到下一个函数定义边界（“        _renderWifiNote” 或 “        _setWifiMode”）
    auto nextFunc = js.find("_renderWifiNote(", defPos);
    TEST_ASSERT_TRUE_MESSAGE(nextFunc != std::string::npos, "Next function after _onNetworkTypeChange must exist");
    std::string changeBlock = js.substr(defPos, nextFunc - defPos);

    TEST_ASSERT_TRUE_MESSAGE(
        changeBlock.find("_startNetworkStatusPolling") != std::string::npos,
        "_onNetworkTypeChange must start polling for 4G/ethernet modes");
    TEST_ASSERT_TRUE_MESSAGE(
        changeBlock.find("240000") != std::string::npos,
        "4G mode must use 240000ms (4 min) polling duration");

    TestLog::step("Network type change starts polling for 4G/ethernet");
    TestLog::testEnd(true);
}

/**
 * @brief 前端 _isAnyStatusPanelConnected 检测方法
 */
void test_frontend_status_panel_connected_detection() {
    TestLog::testStart("Frontend: _isAnyStatusPanelConnected detects connected state");

    std::string js = readSrc("web-src/modules/runtime/network.js");
    TEST_ASSERT_TRUE_MESSAGE(!js.empty(), "network.js must be readable");

    // 搜索函数定义而非调用点
    auto defPos = js.find("_isAnyStatusPanelConnected() {");
    TEST_ASSERT_TRUE_MESSAGE(
        defPos != std::string::npos,
        "network.js must have _isAnyStatusPanelConnected() method definition");

    // 提取足够大的块覆盖完整函数体
    auto blockEnd = js.find("\n        },", defPos);
    std::string block = js.substr(defPos, blockEnd - defPos);
    TEST_ASSERT_TRUE_MESSAGE(
        block.find("status-connected") != std::string::npos,
        "_isAnyStatusPanelConnected must check for status-connected class");

    TEST_ASSERT_TRUE_MESSAGE(
        block.find("wifi-status-badge") != std::string::npos,
        "Must check WiFi status badge");
    TEST_ASSERT_TRUE_MESSAGE(
        block.find("eth-status-badge") != std::string::npos,
        "Must check Ethernet status badge");
    TEST_ASSERT_TRUE_MESSAGE(
        block.find("cell-status-badge") != std::string::npos,
        "Must check Cellular status badge");

    TestLog::step("_isAnyStatusPanelConnected checks all three badges");
    TestLog::testEnd(true);
}

/**
 * @brief forceUpdate() 安全守卫：未初始化/模块忙时不执行 PDP 检查
 */
void test_ec801e_forceUpdate_safety_guards() {
    TestLog::testStart("EC801E: forceUpdate() has safety guards");

    std::string cell = readSrc("src/network/CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!cell.empty(), "CellularAdapter.cpp must be readable");

    std::string body = extractFuncBody(cell, "CellularAdapter::forceUpdate()", 1500);
    TEST_ASSERT_TRUE_MESSAGE(!body.empty(), "forceUpdate() must exist");

    // 必须有 _initialized 和 _modem 守卫
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("!_initialized") != std::string::npos &&
        body.find("!_modem") != std::string::npos,
        "forceUpdate must guard against uninitialized state and null modem");

    // 必须有 _clientBusy 守卫（避免与 MQTT AT 会话冲突）
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("_clientBusy") != std::string::npos,
        "forceUpdate must skip when AT client is busy");

    // 必须有 _qsslClient->isBusy() 守卫
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("_qsslClient") != std::string::npos &&
        body.find("isBusy()") != std::string::npos,
        "forceUpdate must skip when QSSL AT session is busy");

    // PDP 丢失时必须清理所有客户端
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("_softwareTlsClient->stop()") != std::string::npos &&
        body.find("_trackedClient->stop()") != std::string::npos &&
        body.find("_gsmClient->stop()") != std::string::npos &&
        body.find("_qsslClient->stop()") != std::string::npos,
        "forceUpdate must stop all cellular clients when PDP is lost");

    TestLog::step("forceUpdate has init/busy guards and cleans up all clients on PDP loss");
    TestLog::testEnd(true);
}

/**
 * @brief saveEthernetConfig 保存后启动 60 秒状态轮询
 */
void test_frontend_ethernet_save_polling() {
    TestLog::testStart("Frontend: saveEthernetConfig starts 60s status polling");

    std::string js = readSrc("web-src/modules/runtime/network.js");
    TEST_ASSERT_TRUE_MESSAGE(!js.empty(), "network.js must be readable");

    // 验证 saveEthernetConfig 保存成功后调用 _startNetworkStatusPolling(60000
    TEST_ASSERT_TRUE_MESSAGE(
        js.find("_startNetworkStatusPolling(60000") != std::string::npos,
        "saveEthernetConfig must start 60s polling after successful save");

    // 验证 60000 调用在 saveEthernetConfig 函数范围内
    auto ethSavePos = js.find("saveEthernetConfig()");
    auto polling60Pos = js.find("_startNetworkStatusPolling(60000");
    TEST_ASSERT_TRUE_MESSAGE(
        ethSavePos != std::string::npos && polling60Pos != std::string::npos,
        "Both saveEthernetConfig and 60000 polling must exist");

    TestLog::step("saveEthernetConfig starts 60s status polling");
    TestLog::testEnd(true);
}

/**
 * @brief _startNetworkStatusPolling 检测到 connected 后自动停止
 */
void test_frontend_polling_auto_stop_on_connected() {
    TestLog::testStart("Frontend: polling auto-stops when connected detected");

    std::string js = readSrc("web-src/modules/runtime/network.js");
    TEST_ASSERT_TRUE_MESSAGE(!js.empty(), "network.js must be readable");

    // 锚定 _startNetworkStatusPolling 函数定义
    auto defPos = js.find("_startNetworkStatusPolling(durationMs");
    TEST_ASSERT_TRUE_MESSAGE(defPos != std::string::npos, "_startNetworkStatusPolling definition must exist");

    // 提取函数体：找到函数结束标记 "}," 后的下一个函数注释 "/**"
    auto commentAfter = js.find("/**", defPos + 50);
    TEST_ASSERT_TRUE_MESSAGE(commentAfter != std::string::npos, "Next function comment must exist");
    std::string pollingBlock = js.substr(defPos, commentAfter - defPos);

    // 必须在轮询回调中调用 _isAnyStatusPanelConnected 检测连接状态
    TEST_ASSERT_TRUE_MESSAGE(
        pollingBlock.find("_isAnyStatusPanelConnected()") != std::string::npos,
        "Polling must check _isAnyStatusPanelConnected() for auto-stop");

    // 检测到连接后必须调用 _stopNetworkStatusPolling
    TEST_ASSERT_TRUE_MESSAGE(
        pollingBlock.find("_stopNetworkStatusPolling()") != std::string::npos,
        "Polling must call _stopNetworkStatusPolling() when connected");

    // 必须恢复按钮状态
    TEST_ASSERT_TRUE_MESSAGE(
        pollingBlock.find("btn.disabled = false") != std::string::npos,
        "Polling must re-enable the save button after stopping");

    TestLog::step("Polling auto-stops on connected and restores button state");
    TestLog::testEnd(true);
}

/**
 * @brief _onNetworkTypeChange 切换到 WiFi 时停止轮询并加载状态
 */
void test_frontend_network_type_change_wifi_stops_polling() {
    TestLog::testStart("Frontend: _onNetworkTypeChange stops polling for WiFi");

    std::string js = readSrc("web-src/modules/runtime/network.js");
    TEST_ASSERT_TRUE_MESSAGE(!js.empty(), "network.js must be readable");

    auto defPos = js.find("_onNetworkTypeChange(value)");
    TEST_ASSERT_TRUE_MESSAGE(defPos != std::string::npos, "_onNetworkTypeChange definition must exist");

    auto nextFunc = js.find("_renderWifiNote(", defPos);
    std::string changeBlock = js.substr(defPos, nextFunc - defPos);

    // WiFi 模式下必须调用 loadNetworkStatus（即时加载，不轮询）
    TEST_ASSERT_TRUE_MESSAGE(
        changeBlock.find("loadNetworkStatus()") != std::string::npos,
        "WiFi mode must call loadNetworkStatus() for immediate status");

    TestLog::step("WiFi mode uses immediate loadNetworkStatus, not polling");
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
    RUN_TEST(test_ec801e_activateNetwork_rejects_invalid_local_ip);
    RUN_TEST(test_ec801e_clients_gate_io_on_pdp_state);
    RUN_TEST(test_ec801e_activateNetwork_requires_cgpaddr_after_gprsConnect);
    RUN_TEST(test_ec801e_pdp_loss_stops_stale_clients);

    // PWRKEY 时序和硬件配置
    RUN_TEST(test_ec801e_pwrkey_pulse_timing);
    RUN_TEST(test_ec801e_header_declarations);
    RUN_TEST(test_ec801e_mqtts_uses_secure_client);

    // NetworkManager 4G 初始化路径
    RUN_TEST(test_ec801e_network_manager_4g_init_path);
    RUN_TEST(test_ec801e_diagnostic_uses_ets_printf);

    // 4G 状态轮询实时性修复
    RUN_TEST(test_ec801e_forceUpdate_exists);
    RUN_TEST(test_ec801e_forceUpdate_safety_guards);
    RUN_TEST(test_ec801e_update_interval_10s);
    RUN_TEST(test_ec801e_updateStatusInfo_calls_forceUpdate);
    RUN_TEST(test_frontend_cellular_save_polling);
    RUN_TEST(test_frontend_ethernet_save_polling);
    RUN_TEST(test_frontend_network_type_change_polling);
    RUN_TEST(test_frontend_network_type_change_wifi_stops_polling);
    RUN_TEST(test_frontend_polling_auto_stop_on_connected);
    RUN_TEST(test_frontend_status_panel_connected_detection);

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

    // 内存安全分配
    RUN_TEST(test_ec801e_all_clients_use_nothrow_alloc);

    TestLog::groupEnd();
}
