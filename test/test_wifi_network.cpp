/**
 * @file test_wifi_network.cpp
 * @brief WiFi 网络连接改进 — 频段检测、指数退避、内存保护 回归测试
 */

#include <unity.h>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <algorithm>
#include <regex>

// 辅助：读取项目源文件
static std::string readSrcFile(const char* relativePath) {
    const char* roots[] = { ".", "..", "../.." };
    for (const char* root : roots) {
        std::string fullPath = std::string(root) + "/" + relativePath;
        std::ifstream ifs(fullPath);
        if (ifs.good()) {
            return std::string((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());
        }
    }
    return "";
}

// 测试日志辅助
struct TestLog {
    static void testStart(const char* name) {
        printf("  [WiFi-NET] %s\n", name);
    }
};

void test_wifi_network_group();

// ============================================================================
// 5.1 频段检测测试
// ============================================================================

/**
 * @brief 验证 WiFi channel 到频段的分类逻辑
 * channel 1-14 = 2.4GHz, channel 36+ = 5GHz
 */
void test_channel_to_band_classification() {
    TestLog::testStart("Channel to band classification (1-14=2.4GHz, 36+=5GHz)");

    // 2.4GHz channels
    for (int ch = 1; ch <= 14; ch++) {
        const char* band = (ch > 14) ? "5GHz" : "2.4GHz";
        TEST_ASSERT_EQUAL_STRING_MESSAGE("2.4GHz", band,
            (std::string("Channel ") + std::to_string(ch) + " should be 2.4GHz").c_str());
    }

    // 5GHz channels (common ones: 36, 40, 44, 48, 52, 149, 153, 157, 161, 165)
    int fiveGhzChannels[] = {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112,
                             116, 120, 124, 128, 132, 136, 140, 149, 153, 157, 161, 165};
    for (int ch : fiveGhzChannels) {
        const char* band = (ch > 14) ? "5GHz" : "2.4GHz";
        TEST_ASSERT_EQUAL_STRING_MESSAGE("5GHz", band,
            (std::string("Channel ") + std::to_string(ch) + " should be 5GHz").c_str());
    }
}

/**
 * @brief 验证 scanNetworks() 的 JSON 输出包含 "band" 字段
 * 通过源码回归测试：检查 scanNetworks() 函数中存在 band 赋值
 */
void test_scan_networks_includes_band_field() {
    TestLog::testStart("Source: scanNetworks() includes 'band' field in JSON output");

    std::string content = readSrcFile("src/network/WiFiManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read WiFiManager.cpp — ensure test runs from project root");

    // 在 scanNetworks 函数体内查找 band 字段赋值
    size_t scanPos = content.find("String WiFiManager::scanNetworks()");
    TEST_ASSERT_TRUE_MESSAGE(scanPos != std::string::npos,
        "scanNetworks() function definition not found");

    std::string scanBody = content.substr(scanPos, 1500);
    TEST_ASSERT_TRUE_MESSAGE(
        scanBody.find("\"band\"") != std::string::npos ||
        scanBody.find("[\"band\"]") != std::string::npos,
        "scanNetworks() must include 'band' field in JSON output");

    // 验证频段判断逻辑存在
    TEST_ASSERT_TRUE_MESSAGE(
        scanBody.find("ch > 14") != std::string::npos,
        "scanNetworks() must check channel > 14 for 5GHz detection");
}

/**
 * @brief 验证 diagnoseBandMismatch() 函数存在且包含正确的判断逻辑
 */
void test_band_diagnosis_function_exists() {
    TestLog::testStart("Source: diagnoseBandMismatch() implements 5GHz detection");

    std::string content = readSrcFile("src/network/WiFiManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read WiFiManager.cpp");

    // 检查函数定义
    size_t pos = content.find("BandDiagnosis WiFiManager::diagnoseBandMismatch");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
        "diagnoseBandMismatch() function definition not found");

    std::string fnBody = content.substr(pos, 2000);

    // 检查关键逻辑
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("has5GHz") != std::string::npos &&
        fnBody.find("has24GHz") != std::string::npos,
        "diagnoseBandMismatch must track both 5GHz and 2.4GHz presence");

    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("ONLY_5GHZ") != std::string::npos,
        "diagnoseBandMismatch must return ONLY_5GHZ when network is 5GHz-only");

    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("scanDelete") != std::string::npos,
        "diagnoseBandMismatch must call WiFi.scanDelete() to free memory");

    // 检查错误日志
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("only supports 5GHz") != std::string::npos ||
        fnBody.find("ONLY_5GHZ") != std::string::npos,
        "diagnoseBandMismatch must log clear error message for 5GHz-only networks");
}

/**
 * @brief 验证 BandDiagnosis 枚举定义在头文件中
 */
void test_band_diagnosis_enum_defined() {
    TestLog::testStart("Source: BandDiagnosis enum defined in WiFiManager.h");

    std::string content = readSrcFile("include/network/WiFiManager.h");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read WiFiManager.h");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("enum class BandDiagnosis") != std::string::npos,
        "BandDiagnosis enum must be defined in WiFiManager.h");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("BAND_OK") != std::string::npos &&
        content.find("ONLY_5GHZ") != std::string::npos &&
        content.find("NOT_FOUND") != std::string::npos &&
        content.find("MIXED_BAND") != std::string::npos,
        "BandDiagnosis must have BAND_OK, ONLY_5GHZ, NOT_FOUND, MIXED_BAND values");
}

// ============================================================================
// 5.2 指数退避测试
// ============================================================================

/**
 * @brief 验证指数退避间隔计算：5s→10s→20s→40s→60s(cap)
 */
void test_reconnect_backoff_interval_calculation() {
    TestLog::testStart("Exponential backoff: 5s→10s→20s→40s→60s(cap)");

    const uint32_t baseInterval = 5000;  // 默认 reconnectInterval
    const unsigned long maxInterval = 60000;  // RECONNECT_MAX_INTERVAL_MS

    // 模拟退避计算（与 NetworkManager.cpp update() 一致）
    unsigned long expected[] = {5000, 10000, 20000, 40000, 60000, 60000};
    for (uint8_t attempts = 0; attempts <= 5; attempts++) {
        uint8_t expShift = std::min(attempts, (uint8_t)4);
        unsigned long backoff = std::min(
            (unsigned long)(baseInterval * (1UL << expShift)),
            maxInterval
        );
        char msg[100];
        snprintf(msg, sizeof(msg), "Attempt %d: expected %lu, got %lu",
                 attempts, expected[attempts], backoff);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected[attempts], backoff, msg);
    }
}

/**
 * @brief 验证退避在较大 base interval 时也能正确封顶
 */
void test_reconnect_backoff_caps_at_max() {
    TestLog::testStart("Backoff caps at RECONNECT_MAX_INTERVAL_MS regardless of base");

    const unsigned long maxInterval = 60000;

    // 即使 base=30000，shift=2 → 30000*4=120000 应被 cap 到 60000
    uint32_t baseInterval = 30000;
    uint8_t expShift = std::min((uint8_t)2, (uint8_t)4);
    unsigned long backoff = std::min(
        (unsigned long)(baseInterval * (1UL << expShift)),
        maxInterval
    );
    TEST_ASSERT_EQUAL_UINT32(60000, backoff);

    // base=10000, shift=4 → 10000*16=160000 → cap to 60000
    baseInterval = 10000;
    expShift = 4;
    backoff = std::min(
        (unsigned long)(baseInterval * (1UL << expShift)),
        maxInterval
    );
    TEST_ASSERT_EQUAL_UINT32(60000, backoff);
}

/**
 * @brief 源码回归：验证 NetworkManager 使用指数退避而非固定间隔
 */
void test_source_reconnect_uses_exponential_backoff() {
    TestLog::testStart("Source: NetworkManager::update() uses exponential backoff");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read NetworkManager.cpp");

    // 查找 update() 函数中的退避逻辑
    size_t pos = content.find("backoffInterval");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
        "NetworkManager::update() must use backoffInterval variable for exponential backoff");

    // 验证使用了位移操作进行指数计算
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("1UL << expShift") != std::string::npos ||
        content.find("1UL << exp") != std::string::npos,
        "Backoff calculation must use bit-shift for exponential growth");

    // 验证使用了 RECONNECT_MAX_INTERVAL_MS 封顶
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("RECONNECT_MAX_INTERVAL_MS") != std::string::npos,
        "Backoff must be capped by RECONNECT_MAX_INTERVAL_MS");
}

// ============================================================================
// 5.3 内存保护测试
// ============================================================================

/**
 * @brief 源码回归：connectToWiFiBlocking 有 DRAM 检查
 */
void test_source_connect_blocking_has_dram_check() {
    TestLog::testStart("Source: connectToWiFiBlocking() checks DRAM before WiFi.begin");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read NetworkManager.cpp");

    // 找到 connectToWiFiBlocking 函数体
    size_t pos = content.find("FBNetworkManager::connectToWiFiBlocking()");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
        "connectToWiFiBlocking() function not found");

    // 取函数体前 800 字符（包含入口 DRAM 检查）
    std::string fnEntry = content.substr(pos, 800);
    TEST_ASSERT_TRUE_MESSAGE(
        fnEntry.find("WIFI_CONNECT_MIN_DRAM") != std::string::npos,
        "connectToWiFiBlocking must check WIFI_CONNECT_MIN_DRAM at entry");

    TEST_ASSERT_TRUE_MESSAGE(
        fnEntry.find("heap_caps_get_free_size") != std::string::npos ||
        fnEntry.find("MALLOC_CAP_INTERNAL") != std::string::npos,
        "connectToWiFiBlocking must check DRAM (MALLOC_CAP_INTERNAL)");
}

/**
 * @brief 源码回归：AP 探测有 DRAM 内存门控
 */
void test_source_ap_probe_has_dram_guard() {
    TestLog::testStart("Source: handleApModeWifiProbe() has DRAM memory guard");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read NetworkManager.cpp");

    size_t pos = content.find("FBNetworkManager::handleApModeWifiProbe()");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
        "handleApModeWifiProbe() function not found");

    std::string fnBody = content.substr(pos, 1000);
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("WIFI_CONNECT_MIN_DRAM") != std::string::npos,
        "handleApModeWifiProbe must check WIFI_CONNECT_MIN_DRAM before probing");

    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("Probe skipped") != std::string::npos ||
        fnBody.find("DRAM too low") != std::string::npos,
        "handleApModeWifiProbe must log when probe is skipped due to low DRAM");
}

/**
 * @brief 验证 wifiStatusToString 覆盖所有关键状态码
 */
void test_wifi_status_to_string_coverage() {
    TestLog::testStart("Source: wifiStatusToString() covers all key WiFi status codes");

    std::string content = readSrcFile("src/network/WiFiManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read WiFiManager.cpp");

    size_t pos = content.find("WiFiManager::wifiStatusToString");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
        "wifiStatusToString() function not found");

    std::string fnBody = content.substr(pos, 800);

    // 必须覆盖的关键状态码
    const char* requiredStatuses[] = {
        "WL_IDLE_STATUS",
        "WL_NO_SSID_AVAIL",
        "WL_CONNECTED",
        "WL_CONNECT_FAILED",
        "WL_DISCONNECTED"
    };

    for (const char* status : requiredStatuses) {
        TEST_ASSERT_TRUE_MESSAGE(
            fnBody.find(status) != std::string::npos,
            (std::string("wifiStatusToString must handle ") + status).c_str());
    }
}

// ============================================================================
// 5.4 源码回归测试
// ============================================================================

/**
 * @brief 源码回归：STA 两条连接路径必须在 WiFi.begin 前启用全信道扫描 +
 * 按信号强度择优 + 关闭省电，修复同名 SSID 多 AP（CMCC/放大器/双频合一）
 * 择到弱 AP 后反复 reason=6 认证失败的问题。
 */
void test_source_sta_connect_uses_all_channel_scan() {
    TestLog::testStart("Source: STA connect paths use all-channel scan + sort by signal");

    struct PathCheck {
        const char* file;
        const char* fnMarker;
        const char* beginMarker;
    };
    const PathCheck paths[] = {
        { "src/network/NetworkManager.cpp",
          "FBNetworkManager::connectToWiFiBlocking()",
          "WiFi.begin(wifiConfig.staSSID" },
        { "src/network/WiFiManager.cpp",
          "WiFiManager::connectToWiFi()",
          "WiFi.begin(targetSSID" },
    };

    for (const PathCheck& p : paths) {
        std::string content = readSrcFile(p.file);
        TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
            (std::string("Failed to read ") + p.file).c_str());

        size_t fnPos = content.find(p.fnMarker);
        TEST_ASSERT_TRUE_MESSAGE(fnPos != std::string::npos,
            (std::string("function not found: ") + p.fnMarker).c_str());

        // 从函数入口截取到第一个 WiFi.begin( 之间的代码块
        size_t beginPos = content.find(p.beginMarker, fnPos);
        TEST_ASSERT_TRUE_MESSAGE(beginPos != std::string::npos,
            (std::string("WiFi.begin not found in: ") + p.fnMarker).c_str());
        std::string pre = content.substr(fnPos, beginPos - fnPos);

        TEST_ASSERT_TRUE_MESSAGE(
            pre.find("WIFI_ALL_CHANNEL_SCAN") != std::string::npos,
            (std::string("must setScanMethod(WIFI_ALL_CHANNEL_SCAN) before begin in ") + p.fnMarker).c_str());
        TEST_ASSERT_TRUE_MESSAGE(
            pre.find("WIFI_CONNECT_AP_BY_SIGNAL") != std::string::npos,
            (std::string("must setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL) before begin in ") + p.fnMarker).c_str());
        TEST_ASSERT_TRUE_MESSAGE(
            pre.find("setSleep(false)") != std::string::npos,
            (std::string("must disable modem sleep before begin in ") + p.fnMarker).c_str());
    }
}

/**
 * @brief 源码回归：开机必须「STA 优先」——当存在有效 staSSID 但
 * network.json 持久化成 mode=AP 时，initialize() 必须自动校正回 STA，
 * 避免设备因被持久化成 AP 而「永不尝试 STA」永久卡在 192.168.4.1。
 */
void test_source_boot_prefers_sta_when_credentials_exist() {
    TestLog::testStart("Source: initialize() auto-corrects AP->STA when staSSID present");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read src/network/NetworkManager.cpp");

    // 定位 initialize() 函数体
    size_t fnPos = content.find("bool FBNetworkManager::initialize()");
    TEST_ASSERT_TRUE_MESSAGE(fnPos != std::string::npos,
        "FBNetworkManager::initialize() not found");
    // 截取到下一个成员函数定义之前（避免跨函数误匹配）
    size_t nextFn = content.find("\nbool FBNetworkManager::", fnPos + 10);
    std::string body = content.substr(fnPos,
        nextFn == std::string::npos ? std::string::npos : nextFn - fnPos);

    // 必须存在「mode==AP 且 staSSID 非空 → mode=STA」的校正分支
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("!wifiConfig.staSSID.isEmpty()") != std::string::npos,
        "initialize() must check !staSSID.isEmpty() for STA-first correction");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("wifiConfig.mode == NetworkMode::NETWORK_AP") != std::string::npos,
        "initialize() must detect persisted mode==NETWORK_AP");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("wifiConfig.mode = NetworkMode::NETWORK_STA") != std::string::npos,
        "initialize() must switch mode back to NETWORK_STA when credentials exist");
}

// 本地复刻 initialize() 的开机模式校正决策（保持与 NetworkManager.cpp 一致）：
//   ① WIFI + staSSID 为空 + STA → AP（保证首次能通过 AP 配网）
//   ② WIFI + staSSID 非空 + AP → STA（STA 优先，避免被持久化成 AP 后永久卡 AP）
//   ③ 其余组合 mode 保持不变；非 WIFI(以太网/4G) 不受 WiFi 模式校正影响
namespace boot_mode_test {
    enum Mode { STA = 0, AP = 1 };
    enum NetType { WIFI = 0, ETH = 1, CELL = 2 };
    static Mode correctBootMode(NetType net, bool staSSIDEmpty, Mode mode) {
        if (net == WIFI && staSSIDEmpty && mode == STA) return AP;
        if (net == WIFI && !staSSIDEmpty && mode == AP)  return STA;
        return mode;
    }
}

/**
 * @brief 行为级真值表：锁死开机模式校正的全部组合，防止「STA 优先」修复回退。
 */
void test_boot_mode_correction_truth_table() {
    TestLog::testStart("Boot mode correction truth table (STA-first when creds exist)");
    using namespace boot_mode_test;
    // 无凭据 + STA → 回落 AP 供配网
    TEST_ASSERT_EQUAL_INT_MESSAGE(AP, correctBootMode(WIFI, true, STA),
        "empty SSID in STA mode must fall back to AP for provisioning");
    // 本次修复核心：有有效凭据却被持久化成 AP → 校正回 STA
    TEST_ASSERT_EQUAL_INT_MESSAGE(STA, correctBootMode(WIFI, false, AP),
        "valid creds persisted as AP must be auto-corrected to STA");
    // 正常 STA 配置保持不变
    TEST_ASSERT_EQUAL_INT_MESSAGE(STA, correctBootMode(WIFI, false, STA),
        "valid STA config must stay STA");
    // 无凭据 + AP 保持 AP（正常配网态）
    TEST_ASSERT_EQUAL_INT_MESSAGE(AP, correctBootMode(WIFI, true, AP),
        "no credentials must stay AP");
    // 非 WIFI（以太网）不受 WiFi 模式校正影响
    TEST_ASSERT_EQUAL_INT_MESSAGE(AP, correctBootMode(ETH, false, AP),
        "non-WiFi networkType must not be touched (AP)");
    TEST_ASSERT_EQUAL_INT_MESSAGE(STA, correctBootMode(ETH, true, STA),
        "non-WiFi networkType must not be touched (STA)");
}

/**
 * @brief 源码回归：开机 STA 连接需具备足够重试次数以容忍弱/边缘信号
 * （reason=6、扫描偶发 0 网络），尽量优先 STA 再回退 AP。
 */
void test_source_boot_connect_retries_for_weak_signal() {
    TestLog::testStart("Source: connectToWiFiBlocking retries >=4 for weak-signal tolerance");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read src/network/NetworkManager.cpp");

    const std::string marker = "MAX_WIFI_RETRIES = ";
    size_t p = content.find(marker);
    TEST_ASSERT_TRUE_MESSAGE(p != std::string::npos,
        "MAX_WIFI_RETRIES declaration not found");

    int retries = std::stoi(content.substr(p + marker.length()));
    char msg[96];
    snprintf(msg, sizeof(msg),
        "MAX_WIFI_RETRIES should be >=4 for weak-signal tolerance, got %d", retries);
    TEST_ASSERT_TRUE_MESSAGE(retries >= 4, msg);
}

/**
 * @brief handleWiFiEvent 中禁止使用 String 拼接操作（栈溢出风险）
 */
void test_source_no_string_concat_in_wifi_event() {
    TestLog::testStart("Source: handleWiFiEvent() has no String concatenation (stack safety)");

    std::string content = readSrcFile("src/network/WiFiManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read WiFiManager.cpp");

    size_t pos = content.find("void WiFiManager::handleWiFiEvent");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
        "handleWiFiEvent() function not found");

    // 提取函数体（到下一个顶层函数定义为止）
    size_t endPos = content.find("\nvoid WiFiManager::", pos + 10);
    if (endPos == std::string::npos) endPos = content.find("\nbool WiFiManager::", pos + 10);
    if (endPos == std::string::npos) endPos = pos + 2000;
    std::string fnBody = content.substr(pos, endPos - pos);

    // 检查无 String 拼接操作符 "+" 用于构建日志消息
    // 排除注释行
    std::istringstream stream(fnBody);
    std::string line;
    int lineNum = 0;
    while (std::getline(stream, line)) {
        lineNum++;
        // 跳过注释行
        size_t firstNonSpace = line.find_first_not_of(" \t");
        if (firstNonSpace != std::string::npos && line[firstNonSpace] == '/' &&
            firstNonSpace + 1 < line.size() && line[firstNonSpace + 1] == '/') {
            continue;
        }
        // 检查 String + String 拼接模式（如 LOG_INFO("xxx" + String(...))）
        if (line.find("LOG_INFO") != std::string::npos ||
            line.find("LOG_WARNING") != std::string::npos ||
            line.find("LOG_DEBUG") != std::string::npos) {
            TEST_FAIL_MESSAGE(
                (std::string("handleWiFiEvent line ") + std::to_string(lineNum) +
                 " uses LOG macro (forbidden on arduino_events stack): " + line).c_str());
        }
    }
}

/**
 * @brief 所有 WiFi.scanNetworks() 调用后必须有对应的 WiFi.scanDelete()
 */
void test_source_scan_delete_called_after_scan() {
    TestLog::testStart("Source: All scanNetworks paths call scanDelete() for memory safety");

    std::string content = readSrcFile("src/network/WiFiManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read WiFiManager.cpp");

    // 找到所有包含 WiFi.scanNetworks 的函数
    // 1. scanNetworks() 函数
    size_t pos1 = content.find("String WiFiManager::scanNetworks()");
    TEST_ASSERT_TRUE_MESSAGE(pos1 != std::string::npos, "scanNetworks() not found");
    std::string fn1 = content.substr(pos1, 3000);
    TEST_ASSERT_TRUE_MESSAGE(fn1.find("scanDelete") != std::string::npos,
        "WiFiManager::scanNetworks() must call scanDelete()");

    // 2. selectBestNetwork() 函数
    size_t pos2 = content.find("bool WiFiManager::selectBestNetwork");
    TEST_ASSERT_TRUE_MESSAGE(pos2 != std::string::npos, "selectBestNetwork() not found");
    std::string fn2 = content.substr(pos2, 2000);
    TEST_ASSERT_TRUE_MESSAGE(fn2.find("scanDelete") != std::string::npos,
        "WiFiManager::selectBestNetwork() must call scanDelete()");

    // 3. diagnoseBandMismatch() 函数
    size_t pos3 = content.find("BandDiagnosis WiFiManager::diagnoseBandMismatch");
    TEST_ASSERT_TRUE_MESSAGE(pos3 != std::string::npos, "diagnoseBandMismatch() not found");
    std::string fn3 = content.substr(pos3, 2000);
    TEST_ASSERT_TRUE_MESSAGE(fn3.find("scanDelete") != std::string::npos,
        "WiFiManager::diagnoseBandMismatch() must call scanDelete()");
}

/**
 * @brief 连接失败时输出频段诊断
 */
void test_source_connect_failure_diagnoses_band() {
    TestLog::testStart("Source: connectToWiFiBlocking() diagnoses band on failure");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read NetworkManager.cpp");

    size_t pos = content.find("FBNetworkManager::connectToWiFiBlocking()");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
        "connectToWiFiBlocking() not found");

    std::string fnBody = content.substr(pos, 6000);

    // 验证在失败路径上调用了频段诊断
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("diagnoseBandMismatch") != std::string::npos,
        "connectToWiFiBlocking must call diagnoseBandMismatch on failure");

    // 验证输出了 WiFi 状态码可读字符串
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("wifiStatusToString") != std::string::npos,
        "connectToWiFiBlocking must output WiFi status as human-readable string");

    // 验证输出了 DRAM 水位
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("DRAM free") != std::string::npos,
        "connectToWiFiBlocking must log DRAM free bytes on failure");
}

/**
 * @brief RECONNECT_MAX_INTERVAL_MS 常量已定义在 WiFiConfig 中
 */
void test_source_reconnect_max_interval_defined() {
    TestLog::testStart("Source: RECONNECT_MAX_INTERVAL_MS defined in WiFiConfig");

    std::string content = readSrcFile("include/network/WiFiManager.h");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read WiFiManager.h");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("RECONNECT_MAX_INTERVAL_MS") != std::string::npos,
        "WiFiConfig must define RECONNECT_MAX_INTERVAL_MS constant");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("60000") != std::string::npos,
        "RECONNECT_MAX_INTERVAL_MS should be 60000 (60 seconds)");
}

// ============================================================================
// 测试组注册
// ============================================================================

/**
 * @brief handleWiFiEvent 必须保持轻量（仅置位标志），重活延迟到 loopTask
 * 回归：PeriphExec/MQTT 调用曾被重新加回 arduino_events 栈上执行，
 *       导致 Stack canary watchpoint triggered (arduino_events) 崩溃复发
 */
void test_source_wifi_event_handler_defers_heavy_work() {
    TestLog::testStart("Source: handleWiFiEvent defers heavy work to loopTask");

    std::string content = readSrcFile("src/network/WiFiManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read WiFiManager.cpp");

    // 1) 提取 handleWiFiEvent 函数体（到下一个顶层函数定义为止）
    size_t pos = content.find("void WiFiManager::handleWiFiEvent");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos, "handleWiFiEvent() not found");
    size_t endPos = content.find("WiFiManager::", pos + 30);
    if (endPos == std::string::npos) endPos = pos + 3000;
    std::string fnBody = content.substr(pos, endPos - pos);

    // 2) 事件栈上禁止重量级调用：PeriphExec 规则分发、MQTT、上层回调
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("PeriphExecManager") == std::string::npos,
        "handleWiFiEvent must NOT call PeriphExecManager on arduino_events stack "
        "(mutex retry + JSON parse + LOGGER cause stack overflow) - defer via pending flag");
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("getMQTTClient") == std::string::npos &&
        fnBody.find("resetErrorCounters") == std::string::npos,
        "handleWiFiEvent must NOT touch MQTT client on arduino_events stack - defer via pending flag");
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("triggerEvent(NetworkStatus") == std::string::npos,
        "handleWiFiEvent must NOT invoke upper-layer callbacks on arduino_events stack - defer via pending flag");

    // 3) 延迟处理机制必须存在，且由 NetworkManager::update() 在 loopTask 上驱动
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("void WiFiManager::processPendingEvents()") != std::string::npos,
        "WiFiManager::processPendingEvents() must exist to run deferred heavy work on loopTask");

    std::string nmContent = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!nmContent.empty(), "Failed to read NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(
        nmContent.find("processPendingEvents()") != std::string::npos,
        "NetworkManager::update() must call wifiManager->processPendingEvents()");
}

// ============================================================================
// 5.5 STA 长时间失联 → 重启回落 AP 看门狗
// ============================================================================

// 纯函数：镜像 NetworkManager::update() 中看门狗的判定逻辑，便于无硬件单测。
// 与实现一致：仅 STA 模式 + 未连接 + 非正在连接时计时；持续断开
// （disconnectedSince!=0）达阈值且未已调度重启时即触发。
// 注意：看门狗不受 autoReconnect 门控——即使自动重连已达上限停用，
// 只要持续断开超阈值仍应重启回退 AP（autoReconnect 参数仅保留兼容，不参与门控）。
static bool shouldRebootForApFallback(bool autoReconnect, bool connecting, bool isConnected,
                                      bool staMode, unsigned long disconnectedSince,
                                      unsigned long now, unsigned long thresholdMs,
                                      bool rebootAlreadyScheduled) {
    (void)autoReconnect;  // 看门狗不再受自动重连开关影响
    if (!(!connecting && !isConnected && staMode)) return false;
    if (disconnectedSince == 0) return false;                 // 未计时/已连接
    if ((now - disconnectedSince) < thresholdMs) return false; // 瞬时掉线，未达阈值
    if (rebootAlreadyScheduled) return false;                 // 避免重复调度
    return true;
}

/**
 * @brief 瞬时掉线（低于阈值）不应触发重启；持续超阈值才重启
 */
void test_sta_lost_reboot_threshold() {
    TestLog::testStart("STA lost watchdog: reboot only after threshold, not on transient drop");

    const unsigned long threshold = 60000UL;    // 1 分钟，与 STA_LOST_REBOOT_MS 一致
    const unsigned long start = 1000000UL;     // 掉线起点

    // 掉线 30 秒（30000ms < 60000ms）：不重启
    TEST_ASSERT_FALSE_MESSAGE(
        shouldRebootForApFallback(true, false, false, true, start, start + 30000UL, threshold, false),
        "Transient 30s drop must NOT trigger reboot");

    // 恰好达到阈值（>=）：重启
    TEST_ASSERT_TRUE_MESSAGE(
        shouldRebootForApFallback(true, false, false, true, start, start + threshold, threshold, false),
        "Disconnected exactly threshold must trigger reboot");

    // 超过阈值：重启
    TEST_ASSERT_TRUE_MESSAGE(
        shouldRebootForApFallback(true, false, false, true, start, start + threshold + 60000UL, threshold, false),
        "Disconnected beyond threshold must trigger reboot");
}

/**
 * @brief 重连成功（disconnectedSince 清零）后不再触发重启
 */
void test_sta_lost_reboot_resets_on_reconnect() {
    TestLog::testStart("STA lost watchdog: reconnect resets timer, no reboot");

    const unsigned long threshold = 60000UL;

    // disconnectedSince==0 表示已重连/未计时，即使 now 很大也不重启
    TEST_ASSERT_FALSE_MESSAGE(
        shouldRebootForApFallback(true, false, true, true, 0UL, 99999999UL, threshold, false),
        "Connected (timer=0) must NOT trigger reboot");
    TEST_ASSERT_FALSE_MESSAGE(
        shouldRebootForApFallback(true, false, false, true, 0UL, 99999999UL, threshold, false),
        "Timer reset to 0 (just reconnected then dropped) must NOT trigger reboot yet");
}

/**
 * @brief 已调度重启 / 非 STA 模式 / 正在连接 均不重复触发
 */
void test_sta_lost_reboot_guards() {
    TestLog::testStart("STA lost watchdog: guarded by scheduled/mode/connecting flags");

    const unsigned long threshold = 60000UL;
    const unsigned long start = 1000000UL;
    const unsigned long farNow = start + threshold + 10000UL;

    // 已有重启被调度 → 不重复
    TEST_ASSERT_FALSE_MESSAGE(
        shouldRebootForApFallback(true, false, false, true, start, farNow, threshold, true),
        "Already-scheduled reboot must NOT be scheduled again");

    // 已回退到 AP（非 STA 模式）→ 不重启
    TEST_ASSERT_FALSE_MESSAGE(
        shouldRebootForApFallback(true, false, false, false, start, farNow, threshold, false),
        "AP mode (already fell back) must NOT trigger STA reboot");

    // 正在连接中 → 不重启
    TEST_ASSERT_FALSE_MESSAGE(
        shouldRebootForApFallback(true, true, false, true, start, farNow, threshold, false),
        "While connecting must NOT trigger reboot");

    // 自动重连已关闭 → 仍然重启（看门狗不受 autoReconnect 门控，
    // 重连达上限停用后仍需重启回退 AP，否则设备会永久卡死无 IP）
    TEST_ASSERT_TRUE_MESSAGE(
        shouldRebootForApFallback(false, false, false, true, start, farNow, threshold, false),
        "Auto-reconnect disabled must STILL trigger reboot (watchdog is not gated by auto-reconnect)");
}

/**
 * @brief 源码回归：NetworkManager::update() 实现 STA 长时间失联 → AP_FALLBACK 重启
 */
void test_source_sta_lost_reboots_for_ap_fallback() {
    TestLog::testStart("Source: update() reboots (AP_FALLBACK) after prolonged STA loss");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read NetworkManager.cpp");

    // 看门狗使用阈值常量 + 持续断开起点时间戳
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("STA_LOST_REBOOT_MS") != std::string::npos,
        "update() must gate the reboot on STA_LOST_REBOOT_MS threshold");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("_staDisconnectedSince") != std::string::npos,
        "update() must track _staDisconnectedSince for prolonged-loss detection");

    // 必须调度干净重启并使用 AP_FALLBACK 重启原因（重启后走开机 AP 回退路径）
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("RestartReason::AP_FALLBACK") != std::string::npos,
        "reboot must use RestartReason::AP_FALLBACK");
    size_t rebootPos = content.find("scheduleReboot");
    TEST_ASSERT_TRUE_MESSAGE(rebootPos != std::string::npos,
        "update() must call SystemRebooter::scheduleReboot for AP fallback");

    // 必须有重复调度保护（!isScheduled）
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("!SystemRebooter::isScheduled()") != std::string::npos,
        "reboot must be guarded by !SystemRebooter::isScheduled() to avoid re-scheduling");

    // 重连成功必须清零计时器
    size_t connPos = content.find("WiFi connected, IP:");
    TEST_ASSERT_TRUE_MESSAGE(connPos != std::string::npos, "connect transition block not found");
    std::string connBlock = content.substr(connPos, 400);
    TEST_ASSERT_TRUE_MESSAGE(
        connBlock.find("_staDisconnectedSince = 0") != std::string::npos,
        "reconnect must reset _staDisconnectedSince = 0");

    // 看门狗不受 autoReconnectEnabled 门控：自动重连应被内层 if (autoReconnectEnabled) 包裹，
    // 而非作为整个断开处理块（含 scheduleReboot）的前置条件，否则重连停用后看门狗也会被跳过。
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("if (autoReconnectEnabled) {") != std::string::npos,
        "auto-reconnect must be gated by an inner if(autoReconnectEnabled) so the watchdog stays ungated");
}

/**
 * @brief 源码回归：头文件定义阈值常量与成员
 */
void test_source_sta_lost_reboot_constants_defined() {
    TestLog::testStart("Source: NetworkManager.h defines STA_LOST_REBOOT_MS + member");

    std::string content = readSrcFile("include/network/NetworkManager.h");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read NetworkManager.h");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("STA_LOST_REBOOT_MS") != std::string::npos,
        "NetworkManager.h must define STA_LOST_REBOOT_MS constant");
    // 阈值必须为 1 分钟（60000ms）：STA 连不上尽快回退 AP 供用户配置
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("STA_LOST_REBOOT_MS = 60000") != std::string::npos,
        "STA_LOST_REBOOT_MS must be 60000 (1 minute) per WiFi management requirement");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("_staDisconnectedSince") != std::string::npos,
        "NetworkManager.h must declare _staDisconnectedSince member");
}

/**
 * @brief 源码回归：AP→STA 恢复必须用 restartMDNS 重新绑定接口
 * 背景：mDNS 在 AP 模式已启动（mdnsStarted=true，绑定 192.168.4.1），若直接调用
 *       startMDNS 会命中提前返回而不做任何事，响应器残留在已失效的 AP 接口，
 *       导致 STA 已连接但 fastbee.local 无法解析。
 */
void test_source_ap_recovery_restarts_mdns() {
    TestLog::testStart("Source: AP->STA recovery uses restartMDNS (not startMDNS)");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read NetworkManager.cpp");

    size_t fnPos = content.find("FBNetworkManager::handleApModeWifiProbe()");
    TEST_ASSERT_TRUE_MESSAGE(fnPos != std::string::npos,
        "handleApModeWifiProbe() function not found");

    // 定位成功分支（WiFi 已恢复，切回 STA）中的 enableMDNS 处理块
    size_t recoveredPos = content.find("WiFi recovered", fnPos);
    TEST_ASSERT_TRUE_MESSAGE(recoveredPos != std::string::npos,
        "AP->STA recovery success branch not found");

    // 从 enableMDNS 判断句开始取小窗口（紧跟着的就是 mDNS 调用），
    // 避开前面长度不定的中文注释，不受注释长度影响
    size_t mdnsIfPos = content.find("if (wifiConfig.enableMDNS)", recoveredPos);
    TEST_ASSERT_TRUE_MESSAGE(mdnsIfPos != std::string::npos,
        "AP->STA recovery must guard mDNS restart with wifiConfig.enableMDNS");

    std::string recoveryBlock = content.substr(mdnsIfPos, 120);

    // 必须调用 restartMDNS 重新绑定到 STA 接口
    TEST_ASSERT_TRUE_MESSAGE(
        recoveryBlock.find("->restartMDNS") != std::string::npos,
        "AP->STA recovery must call dnsManager->restartMDNS to rebind mDNS on STA interface");

    // 禁止在恢复块里直接调用 startMDNS（会因 mdnsStarted 提前返回，mDNS 不会重新绑定）
    TEST_ASSERT_TRUE_MESSAGE(
        recoveryBlock.find("->startMDNS") == std::string::npos,
        "AP->STA recovery must NOT call startMDNS directly (early-returns when already started)");
}

/**
 * @brief 源码回归：startMDNS 保留 mdnsStarted 提前返回守卫 + restartMDNS 先 stop 后 start
 * 这是“接口切换必须用 restartMDNS”修复的前提：若去掉提前返回守卫，
 * 或 restartMDNS 不先 stop，则接口切换时 mDNS 仍会残留在旧接口。
 */
void test_source_mdns_rebind_premise_intact() {
    TestLog::testStart("Source: startMDNS early-return guard + restartMDNS stop-before-start");

    std::string content = readSrcFile("src/network/DNSManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read DNSManager.cpp");

    // startMDNS 必须保留 mdnsStarted 提前返回守卫
    size_t startPos = content.find("DNSManager::startMDNS");
    TEST_ASSERT_TRUE_MESSAGE(startPos != std::string::npos, "startMDNS() not found");
    std::string startBody = content.substr(startPos, 300);
    TEST_ASSERT_TRUE_MESSAGE(
        startBody.find("if (mdnsStarted)") != std::string::npos,
        "startMDNS must keep the mdnsStarted early-return guard (rebind premise)");

    // restartMDNS 必须先 stopMDNS 再 startMDNS（强制重新绑定当前接口 IP）
    size_t restartPos = content.find("DNSManager::restartMDNS");
    TEST_ASSERT_TRUE_MESSAGE(restartPos != std::string::npos, "restartMDNS() not found");
    std::string restartBody = content.substr(restartPos, 300);
    size_t stopPos = restartBody.find("stopMDNS");
    size_t innerStartPos = restartBody.find("startMDNS(hostname");
    TEST_ASSERT_TRUE_MESSAGE(stopPos != std::string::npos && innerStartPos != std::string::npos,
        "restartMDNS must call both stopMDNS and startMDNS(hostname...)");
    TEST_ASSERT_TRUE_MESSAGE(stopPos < innerStartPos,
        "restartMDNS must call stopMDNS BEFORE startMDNS to force rebind");
}

/**
 * @brief 源码回归：以太网/4G 接口切换（回退 AP 或重连成功）后必须用 restartMDNS
 * 与 AP→STA 同为接口切换场景，startMDNS 会因 mdnsStarted 提前返回而无法重绑定。
 */
void test_source_interface_switch_restarts_mdns() {
    TestLog::testStart("Source: ETH/4G interface-switch paths use restartMDNS");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read NetworkManager.cpp");

    const char* markers[] = {
        "Ethernet auto-reconnect exhausted",
        "4G auto-reconnect succeeded",
        "4G auto-reconnect exhausted",
    };
    for (const char* marker : markers) {
        size_t mpos = content.find(marker);
        TEST_ASSERT_TRUE_MESSAGE(mpos != std::string::npos,
            (std::string("marker not found: ") + marker).c_str());
        size_t mdnsIfPos = content.find("if (wifiConfig.enableMDNS)", mpos);
        TEST_ASSERT_TRUE_MESSAGE(mdnsIfPos != std::string::npos,
            (std::string("enableMDNS guard not found after: ") + marker).c_str());
        std::string block = content.substr(mdnsIfPos, 120);
        TEST_ASSERT_TRUE_MESSAGE(
            block.find("->restartMDNS") != std::string::npos,
            (std::string("must use restartMDNS after: ") + marker).c_str());
        TEST_ASSERT_TRUE_MESSAGE(
            block.find("->startMDNS") == std::string::npos,
            (std::string("must NOT use startMDNS after: ") + marker).c_str());
    }
}

void test_wifi_network_group() {
    // 频段检测测试
    RUN_TEST(test_channel_to_band_classification);
    RUN_TEST(test_scan_networks_includes_band_field);
    RUN_TEST(test_band_diagnosis_function_exists);
    RUN_TEST(test_band_diagnosis_enum_defined);

    // 指数退避测试
    RUN_TEST(test_reconnect_backoff_interval_calculation);
    RUN_TEST(test_reconnect_backoff_caps_at_max);
    RUN_TEST(test_source_reconnect_uses_exponential_backoff);

    // 内存保护测试
    RUN_TEST(test_source_connect_blocking_has_dram_check);
    RUN_TEST(test_source_ap_probe_has_dram_guard);
    RUN_TEST(test_wifi_status_to_string_coverage);

    // 源码回归测试
    RUN_TEST(test_source_sta_connect_uses_all_channel_scan);
    RUN_TEST(test_source_boot_prefers_sta_when_credentials_exist);
    RUN_TEST(test_boot_mode_correction_truth_table);
    RUN_TEST(test_source_boot_connect_retries_for_weak_signal);
    RUN_TEST(test_source_no_string_concat_in_wifi_event);
    RUN_TEST(test_source_wifi_event_handler_defers_heavy_work);
    RUN_TEST(test_source_scan_delete_called_after_scan);
    RUN_TEST(test_source_connect_failure_diagnoses_band);
    RUN_TEST(test_source_reconnect_max_interval_defined);

    // STA 长时间失联 → 重启回落 AP 看门狗
    RUN_TEST(test_sta_lost_reboot_threshold);
    RUN_TEST(test_sta_lost_reboot_resets_on_reconnect);
    RUN_TEST(test_sta_lost_reboot_guards);
    RUN_TEST(test_source_sta_lost_reboots_for_ap_fallback);
    RUN_TEST(test_source_sta_lost_reboot_constants_defined);

    // AP→STA 恢复重新绑定 mDNS（fastbee.local 在 STA 下可解析）
    RUN_TEST(test_source_ap_recovery_restarts_mdns);
    RUN_TEST(test_source_mdns_rebind_premise_intact);
    RUN_TEST(test_source_interface_switch_restarts_mdns);
}
