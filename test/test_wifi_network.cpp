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
        bool asyncBoot;
    };
    const PathCheck paths[] = {
        { "src/network/NetworkManager.cpp",
          "FBNetworkManager::connectToWiFiBlocking()",
          "startBootStaRetryScan();",
          true },
        { "src/network/WiFiManager.cpp",
          "WiFiManager::connectToWiFi()",
          "WiFi.begin(targetSSID",
          false },
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
        if (p.asyncBoot) {
            TEST_ASSERT_TRUE_MESSAGE(
                pre.find("WiFi.scanNetworks(false, true)") == std::string::npos &&
                pre.find("while (WiFi.status()") == std::string::npos &&
                pre.find("delay(") == std::string::npos,
                "async boot initialization must not retain synchronous scan/wait logic");
        }
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
 * @brief 源码回归：开机 STA 候选扫描在所有编译模式下均为异步状态机，
 * 连接重试由 update() 统一推进，不能在初始化或 network_update 中同步等待。
 */
void test_source_boot_connect_retries_for_weak_signal() {
    TestLog::testStart("Source: boot STA candidate scan is async in every Web start mode");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read src/network/NetworkManager.cpp");

    // 初始化入口必须启动统一异步状态机，而非直接同步预扫描。
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("startBootStaRetryScan()") != std::string::npos &&
        content.find("WiFi.scanNetworks(false, true)") == std::string::npos,
        "boot candidate scan must be asynchronous and contain no blocking scan");
    size_t initStart = content.find("bool FBNetworkManager::connectToWiFiBlocking()");
    size_t initEnd = content.find("void FBNetworkManager::disconnectWiFi()", initStart);
    TEST_ASSERT_TRUE_MESSAGE(initStart != std::string::npos && initEnd != std::string::npos,
        "STA initialization hook not found");
    std::string initBlock = content.substr(initStart, initEnd - initStart);
    TEST_ASSERT_TRUE_MESSAGE(
        initBlock.find("while (WiFi.status()") == std::string::npos &&
        initBlock.find("delay(") == std::string::npos &&
        initBlock.find("return true;") != std::string::npos,
        "STA initialization must return immediately after scheduling async scan");
}

/**
 * @brief 源码回归：AP 模式探测必须先预扫描，SSID 不可见时禁止 WiFi.begin()
 * 旧逻辑对不存在的 SSID 仍每 2min 切 STA + WiFi.begin()，每次白耗 ~4KB
 * lwIP 缓冲且失败不回收，~10 次后 free8 跌破 6KB Web 门控，AP 模式下
 * 页面永远加载不出、探测被 16KB 门控锁死（只能重启逃生）。
 */
void test_source_ap_probe_prescan_fast_fail() {
    TestLog::testStart("Source: AP-mode WiFi probe pre-scans before WiFi.begin (no lwIP leak)");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read src/network/NetworkManager.cpp");

    size_t fnPos = content.find("void FBNetworkManager::handleApModeWifiProbe()");
    TEST_ASSERT_TRUE_MESSAGE(fnPos != std::string::npos,
        "handleApModeWifiProbe() definition not found in NetworkManager.cpp");

    // 提取函数体（到下一个顶层函数定义为止）
    size_t endPos = content.find("\nvoid FBNetworkManager::", fnPos + 20);
    if (endPos == std::string::npos) endPos = fnPos + 16000;
    std::string fnBody = content.substr(fnPos, endPos - fnPos);

    // 1) 探测内必须含预扫描快速失败日志与可见性判断
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("[WiFi-AP] Pre-scan:") != std::string::npos,
        "AP probe must pre-scan and log SSID visibility before any STA switch");
    // 2) 停 AP/切 STA/WiFi.begin() 之前必须先判 ssidVisible 并快速返回
    size_t checkPos = fnBody.find("if (!ssidVisible)");
    size_t stopApPos = fnBody.find("stopAPMode");
    TEST_ASSERT_TRUE_MESSAGE(checkPos != std::string::npos,
        "ssidVisible fast-fail check missing in AP probe");
    TEST_ASSERT_TRUE_MESSAGE(stopApPos != std::string::npos,
        "stopAPMode call missing in AP probe");
    TEST_ASSERT_TRUE_MESSAGE(checkPos < stopApPos,
        "ssidVisible check must run BEFORE stopping AP / switching STA");
    // 3) 预扫描必须异步、包含隐藏 SSID，并在完成后释放结果
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("WiFi.scanNetworks(true, true)") != std::string::npos &&
        fnBody.find("WiFi.scanComplete()") != std::string::npos,
        "AP probe pre-scan must be asynchronous and include hidden SSIDs");
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("WiFi.scanDelete()") != std::string::npos,
        "AP probe pre-scan must release scan results");
    // 4) 异步扫描跨多个 update tick，完成/失败路径必须显式关闭 scan 临时
    //    启用的 STA，恢复纯 AP。
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("WiFi.enableSTA(false)") != std::string::npos,
        "Async AP probe must explicitly restore pure AP after scan (scanNetworks flips AP->APSTA)");
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("while (") == std::string::npos &&
        fnBody.find("delay(") == std::string::npos,
        "AP probe must not block network_update with polling loops or delays");

    // 5) 探测失败路径必须用 WiFi.disconnect(true, true) 关闭 STA 接口，
    //    而非 disconnect(false)。仅断 AP 不断 STA 则 lwIP 缓冲驻留不回收，
    //    每次探测泄漏 ~4KB（实测 1.5h 累积 ~18KB → free8 跌破 6KB）。
    //    disconnect(true, true): 第1个true=断开连接，第2个true=关闭STA接口。
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("WiFi.disconnect(true, true)") != std::string::npos,
        "AP probe failure path must use WiFi.disconnect(true, true) to close STA interface and release lwIP buffers");
    // 确保不存在旧的 disconnect(false) 残留在失败路径
    // （成功路径不调 disconnect，所以函数体内 disconnect 只应出现在失败路径）
    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("WiFi.disconnect(false)") == std::string::npos,
        "AP probe must NOT use WiFi.disconnect(false) — STA interface stays open, leaking lwIP buffers");
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
 * @brief 异步开机连接失败路径不能调用同步频段扫描诊断。
 * 频段诊断函数仍由 WiFiManager 单独提供，避免在 network_update 中再次阻塞。
 */
void test_source_connect_failure_diagnoses_band() {
    TestLog::testStart("Source: async boot failure path avoids blocking band diagnosis");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read NetworkManager.cpp");

    size_t pos = content.find("FBNetworkManager::connectToWiFiBlocking()");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
        "connectToWiFiBlocking() not found");

    size_t end = content.find("void FBNetworkManager::disconnectWiFi()", pos);
    TEST_ASSERT_TRUE_MESSAGE(end != std::string::npos,
        "connectToWiFiBlocking() end not found");
    std::string fnBody = content.substr(pos, end - pos);

    TEST_ASSERT_TRUE_MESSAGE(
        fnBody.find("startBootStaRetryScan()") != std::string::npos &&
        fnBody.find("diagnoseBandMismatch") == std::string::npos &&
        fnBody.find("WiFi.scanNetworks(false, true)") == std::string::npos,
        "async boot initialization must not invoke a synchronous band scan on failure");
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

    const unsigned long threshold = 300000UL;   // 5 分钟，与 STA_LOST_REBOOT_MS 一致
    const unsigned long start = 1000000UL;     // 掉线起点

    // 弱信号或 Mesh 漫游造成的 2 分钟掉线不应重启
    TEST_ASSERT_FALSE_MESSAGE(
        shouldRebootForApFallback(true, false, false, true, start, start + 120000UL, threshold, false),
        "Transient 2min drop must NOT trigger reboot");

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

    const unsigned long threshold = 300000UL;

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

    const unsigned long threshold = 300000UL;
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
 * @brief Source regression: once AP fallback is available, retry STA by probe
 *        without periodically rebooting the whole device.
 */
void test_source_ap_fallback_uses_probe_without_periodic_reboot() {
    TestLog::testStart("Source: AP fallback remains stable while STA probes continue");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    std::string header = readSrcFile("include/network/NetworkManager.h");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!header.empty(), "Failed to read NetworkManager.h");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("handleApModeWifiProbe()") != std::string::npos,
        "AP fallback must keep the non-rebooting STA probe path");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("AP fallback stuck too long") == std::string::npos,
        "AP fallback must not force periodic whole-device reboots");
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("AP_STUCK_REBOOT_MS") == std::string::npos,
        "obsolete periodic AP reboot threshold must stay removed");
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
    // 5 分钟持续失联才重启：覆盖指数退避和 Mesh/弱信号短时波动
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("STA_LOST_REBOOT_MS = 300000") != std::string::npos,
        "STA_LOST_REBOOT_MS must be 300000 (5 minutes) to avoid transient-drop reboot loops");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("_staDisconnectedSince") != std::string::npos,
        "NetworkManager.h must declare _staDisconnectedSince member");
}

/**
 * @brief 网关 ICMP 失败最终只触发 STA 重连，不切 AP/不重启。
 */
void test_source_gateway_ping_failure_is_advisory() {
    TestLog::testStart("Source: gateway ping recovery is STA-only with bounded unknown baseline");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read NetworkManager.cpp");

    size_t probeBlockStart = content.find("// 网关 ICMP 使用会话内成功基线");
    size_t probeBlockEnd = content.find("// 处理连接超时", probeBlockStart);
    TEST_ASSERT_TRUE_MESSAGE(probeBlockStart != std::string::npos &&
                             probeBlockEnd != std::string::npos,
        "Gateway observation block not found");
    std::string probeBlock = content.substr(probeBlockStart, probeBlockEnd - probeBlockStart);
    TEST_ASSERT_TRUE_MESSAGE(
        probeBlock.find("const uint8_t recoveryThreshold = _gwProbeEverSucceeded") != std::string::npos &&
        probeBlock.find("GW_PROBE_RECOVERY_THRESHOLD") != std::string::npos,
        "Gateway recovery must retain a stricter threshold for confirmed sessions");
    TEST_ASSERT_TRUE_MESSAGE(
        probeBlock.find("GW_PROBE_UNKNOWN_RECOVERY_THRESHOLD") != std::string::npos &&
        probeBlock.find("reconnecting STA without reboot or AP fallback") != std::string::npos,
        "Unknown-baseline data-plane loss must recover with STA only, never AP fallback");
    TEST_ASSERT_TRUE_MESSAGE(
        probeBlock.find("probeResultReady") != std::string::npos &&
        probeBlock.find("if (!probeResultReady)") != std::string::npos,
        "Starting or waiting for an async ping must not clear the consecutive failure count");
    TEST_ASSERT_TRUE_MESSAGE(
        probeBlock.find("WiFi.disconnect(false)") != std::string::npos &&
        probeBlock.find("ESP.restart()") == std::string::npos,
        "A confirmed gateway path loss must reconnect STA without rebooting the device");
    TEST_ASSERT_TRUE_MESSAGE(
        probeBlock.find("_gwProbeEverSucceeded ? \"confirmed\" : \"unknown\"") != std::string::npos &&
        probeBlock.find("pending STA-only recovery") != std::string::npos,
        "Unanswered gateway probes must remain observable while bounded STA recovery is pending");

    size_t zombieBlockStart = content.find("// WiFi 僵尸连接检测");
    size_t zombieBlockEnd = content.find("// 清理冲突缓存", zombieBlockStart);
    TEST_ASSERT_TRUE_MESSAGE(zombieBlockStart != std::string::npos &&
                             zombieBlockEnd != std::string::npos,
        "UDP zombie recovery block not found");
    std::string zombieBlock = content.substr(zombieBlockStart, zombieBlockEnd - zombieBlockStart);
    TEST_ASSERT_TRUE_MESSAGE(
        zombieBlock.find("if (!isConnected)") != std::string::npos &&
        zombieBlock.find("zombieFailCount = 0") != std::string::npos,
        "Zombie failure count must reset while STA is disconnected");
    TEST_ASSERT_TRUE_MESSAGE(
        zombieBlock.find("WiFi.disconnect(false)") != std::string::npos,
        "Repeated local UDP send failure must retain WiFi self-healing");

    const std::string header = readSrcFile("include/network/NetworkManager.h");
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("GW_PROBE_INTERVAL_MS = 60000") != std::string::npos &&
        header.find("GW_PROBE_RECOVERY_THRESHOLD = 5") != std::string::npos &&
        header.find("GW_PROBE_UNKNOWN_RECOVERY_THRESHOLD = 5") != std::string::npos,
        "Gateway ICMP recovery must tolerate transient/router-filtered probes (60s x 5)");
}

/**
 * @brief 驱动重建后不能再用 disconnect(true) 关闭刚启动的 STA。
 */
void test_source_sta_driver_restart_keeps_radio_started() {
    TestLog::testStart("Source: STA restart does not turn radio off before begin");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read NetworkManager.cpp");

    size_t retryStart = content.find("const uint32_t bootConnectTimeout");
    size_t retryEnd = content.find("#else", retryStart);
    TEST_ASSERT_TRUE_MESSAGE(retryStart != std::string::npos && retryEnd != std::string::npos,
        "Non-blocking boot retry block not found");
    std::string retryBlock = content.substr(retryStart, retryEnd - retryStart);
    TEST_ASSERT_TRUE_MESSAGE(
        retryBlock.find("WiFi.disconnect(true)") == std::string::npos,
        "Boot retry must not turn WiFi off immediately before begin");
    TEST_ASSERT_TRUE_MESSAGE(
        retryBlock.find("delay(") == std::string::npos,
        "Boot retry timeout path must not block on driver or scan delays");
    TEST_ASSERT_TRUE_MESSAGE(
        retryBlock.find("startBootStaRetryScan()") != std::string::npos &&
        retryBlock.find("WiFi.scanNetworks(false, true)") == std::string::npos,
        "The first boot retry must launch an asynchronous candidate scan");
    TEST_ASSERT_TRUE_MESSAGE(
        retryBlock.find("wifiConfig.connectTimeout < 45000UL") != std::string::npos,
        "Non-blocking boot connection must allow DHCP grace time on busy multi-AP networks");
    size_t scanStart = content.find("void FBNetworkManager::startBootStaRetryScan()");
    size_t scanEnd = content.find("void FBNetworkManager::handleBootStaRetryScan()", scanStart);
    TEST_ASSERT_TRUE_MESSAGE(scanStart != std::string::npos && scanEnd != std::string::npos,
        "Async boot retry scan starter not found");
    std::string scanBlock = content.substr(scanStart, scanEnd - scanStart);
    TEST_ASSERT_TRUE_MESSAGE(
        scanBlock.find("WiFi.mode(WIFI_MODE_NULL)") != std::string::npos &&
        scanBlock.find("DRIVER_NULL_SETTLE") != std::string::npos &&
        scanBlock.find("Async boot retry driver reset started") != std::string::npos,
        "Boot retry must start the driver reset state asynchronously");

    size_t scanHandleStart = scanEnd;
    size_t scanHandleEnd = content.find("void FBNetworkManager::update()", scanHandleStart);
    TEST_ASSERT_TRUE_MESSAGE(scanHandleEnd != std::string::npos,
        "Async boot retry scan handler block not found");
    std::string scanHandleBlock = content.substr(scanHandleStart, scanHandleEnd - scanHandleStart);
    TEST_ASSERT_TRUE_MESSAGE(
        scanHandleBlock.find("WiFi.mode(WIFI_MODE_STA)") != std::string::npos &&
        scanHandleBlock.find("WiFi.setAutoReconnect(false)") != std::string::npos &&
        scanHandleBlock.find("DRIVER_STA_SETTLE") != std::string::npos &&
        scanHandleBlock.find("WiFi.scanNetworks(true, true)") != std::string::npos &&
        scanHandleBlock.find("WIFI_SCAN_FAILED") != std::string::npos &&
        scanHandleBlock.find("WiFi.scanComplete()") != std::string::npos &&
        scanHandleBlock.find("WiFi.scanDelete()") != std::string::npos &&
        scanHandleBlock.find("memcmp(_bootStaRetryCandidates[j].bssid, bssid, 6) == 0") != std::string::npos &&
        scanHandleBlock.find("memcmp(previous.bssid, current.bssid, 6) > 0") != std::string::npos,
        "Async boot retry handler must consume, release, de-duplicate and stably sort scan results");

    size_t launchStart = content.find("void FBNetworkManager::launchBootStaRetryConnection()");
    size_t launchEnd = content.find("void FBNetworkManager::startBootStaRetryScan()", launchStart);
    TEST_ASSERT_TRUE_MESSAGE(launchStart != std::string::npos && launchEnd != std::string::npos,
        "Async boot retry connection launcher not found");
    std::string launchBlock = content.substr(launchStart, launchEnd - launchStart);
    TEST_ASSERT_TRUE_MESSAGE(
        launchBlock.find("using automatic AP selection") != std::string::npos &&
        launchBlock.find("WiFi.begin(wifiConfig.staSSID.c_str(), wifiConfig.staPassword.c_str())") != std::string::npos &&
        launchBlock.find("STA boot retry pinned BSSID") != std::string::npos &&
        launchBlock.find("(_bootConnectRetries - 1) % _bootStaRetryCandidateCount") != std::string::npos,
        "The first boot retry must be automatic, then rotate distinct BSSID candidates");

    size_t blockingStart = content.find("bool FBNetworkManager::connectToWiFiBlocking()");
    size_t blockingEnd = content.find("void FBNetworkManager::disconnectWiFi()", blockingStart);
    TEST_ASSERT_TRUE_MESSAGE(blockingStart != std::string::npos &&
                             blockingEnd != std::string::npos,
        "Blocking STA connection block not found");
    std::string blockingBlock = content.substr(blockingStart, blockingEnd - blockingStart);
    TEST_ASSERT_TRUE_MESSAGE(
        blockingBlock.find("startBootStaRetryScan()") != std::string::npos &&
        blockingBlock.find("WiFi.scanNetworks(false, true)") == std::string::npos &&
        blockingBlock.find("while (WiFi.status()") == std::string::npos &&
        blockingBlock.find("delay(") == std::string::npos,
        "The compatibility initialization path must schedule async scanning without blocking");

    size_t probeStart = content.find("void FBNetworkManager::handleApModeWifiProbe()");
    size_t probeEnd = content.find("void FBNetworkManager::triggerEvent", probeStart);
    TEST_ASSERT_TRUE_MESSAGE(probeStart != std::string::npos && probeEnd != std::string::npos,
        "AP recovery probe block not found");
    std::string probeBlock = content.substr(probeStart, probeEnd - probeStart);
    TEST_ASSERT_TRUE_MESSAGE(
        probeBlock.find("memcmp(_apProbeCandidates[j].bssid") != std::string::npos,
        "AP recovery candidates must de-duplicate repeated BSSID scan records");
    size_t firstBegin = probeBlock.find("WiFi.begin(");
    TEST_ASSERT_TRUE_MESSAGE(firstBegin != std::string::npos,
        "AP recovery probe must start a STA connection");
    std::string probePrepareBlock = probeBlock.substr(0, firstBegin);
    TEST_ASSERT_TRUE_MESSAGE(
        probePrepareBlock.find("WiFi.disconnect(true, true);") == std::string::npos &&
        probeBlock.find("WiFi.setAutoReconnect(false)") != std::string::npos &&
        probeBlock.find("WiFi.mode(WIFI_MODE_NULL)") != std::string::npos,
        "AP recovery must disable Arduino reconnect and reset STA before BSSID changes");
    TEST_ASSERT_TRUE_MESSAGE(
        probeBlock.find("usePinnedCandidate =") != std::string::npos &&
        probeBlock.find("(_apProbeAttempt - 1) % _apProbeCandidateCount") != std::string::npos &&
        probeBlock.find("ap.channel, ap.bssid") != std::string::npos,
        "AP recovery must use automatic selection first, then rotate sorted BSSID candidates");

    std::string wifiManager = readSrcFile("src/network/WiFiManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(
        wifiManager.find("WiFi.setAutoReconnect(false)") != std::string::npos &&
        wifiManager.find("WiFi.setAutoReconnect(true)") == std::string::npos,
        "WiFiManager must leave reconnect ownership to FBNetworkManager");
}

/**
 * @brief 有 STA 凭据且关闭自动故障转移时，失败只能留在 STA 重试，不能切 AP。
 */
void test_source_sta_only_policy_disables_ap_fallback() {
    TestLog::testStart("Source: STA-only recovery never switches to AP when auto failover is disabled");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read NetworkManager.cpp");

    TEST_ASSERT_TRUE_MESSAGE(
        content.find("STA retries exhausted; AP fallback disabled, continuing STA-only recovery") != std::string::npos &&
        content.find("STA reconnect budget exhausted; AP fallback disabled, continuing STA-only recovery") != std::string::npos,
        "Boot and reconnect exhaustion must have explicit STA-only branches");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("wifiConfig.autoFailover &&") != std::string::npos,
        "STA lost watchdog reboot/AP fallback must honor autoFailover");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("STA connection failed; AP fallback disabled, keeping STA retry mode") != std::string::npos,
        "Blocking initialization failure must keep STA retry mode when AP fallback is disabled");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("STA_ONLY_RETRY_INTERVAL_MS") != std::string::npos &&
        content.find("backoffInterval = max(backoffInterval, STA_ONLY_RETRY_INTERVAL_MS)") != std::string::npos,
        "STA-only recovery must enforce a long retry interval to avoid authentication storms");
}

/**
 * @brief Web/API 状态刷新不得用 WiFiManager 的传输层快照覆盖上层重连预算。
 * FBNetworkManager 独占退避、重试上限和 AP 回落决策；否则每次状态查询都可能
 * 把 reconnectAttempts 清零，使 maxReconnectAttempts 永远无法耗尽。
 */
void test_source_status_sync_preserves_network_manager_reconnect_count() {
    TestLog::testStart("Source: status sync preserves FBNetworkManager reconnect ownership");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read NetworkManager.cpp");

    size_t fnStart = content.find("void FBNetworkManager::updateStatusInfo()");
    size_t fnEnd = content.find("void FBNetworkManager::attemptReconnect()", fnStart);
    TEST_ASSERT_TRUE_MESSAGE(fnStart != std::string::npos && fnEnd != std::string::npos,
        "NetworkManager status synchronization block not found");
    std::string fnBody = content.substr(fnStart, fnEnd - fnStart);

    size_t snapshot = fnBody.find(
        "const uint16_t ownedReconnectAttempts = statusInfo.reconnectAttempts;");
    size_t transportSync = fnBody.find("statusInfo = wifiManager->getStatusInfo();");
    size_t restore = fnBody.find(
        "statusInfo.reconnectAttempts = ownedReconnectAttempts;");
    TEST_ASSERT_TRUE_MESSAGE(
        snapshot != std::string::npos &&
        transportSync != std::string::npos &&
        restore != std::string::npos,
        "WiFi status sync must snapshot and restore the NetworkManager-owned reconnect count");
    TEST_ASSERT_TRUE_MESSAGE(snapshot < transportSync && transportSync < restore,
        "Reconnect count must be saved before and restored after the transport status copy");
}

/**
 * @brief COM11 C6 临时测试配置保持 STA-only，避免弱网时切换到设备 AP。
 */
void test_c6_test_profile_disables_ap_fallback() {
    TestLog::testStart("Config: C6 test profile disables AP fallback");

    std::string config = readSrcFile("docs/c6-rgb-led-f4r0/network.json");
    TEST_ASSERT_TRUE_MESSAGE(!config.empty(), "Failed to read C6 network.json");
    TEST_ASSERT_TRUE_MESSAGE(
        std::regex_search(config, std::regex("\\\"autoFailover\\\"\\s*:\\s*false")),
        "C6 test profile must keep autoFailover=false during STA stability testing");
}

/**
 * @brief 源码回归：AP→STA 恢复必须在 WiFi 接口切换前停止 mDNS，
 *        切换完成后再启动，避免 mDNS 后台任务访问已销毁的 AP netif。
 */
void test_source_ap_recovery_restarts_mdns() {
    TestLog::testStart("Source: AP->STA recovery stops mDNS before driver switch");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read NetworkManager.cpp");

    size_t fnPos = content.find("FBNetworkManager::handleApModeWifiProbe()");
    TEST_ASSERT_TRUE_MESSAGE(fnPos != std::string::npos,
        "handleApModeWifiProbe() function not found");

    // 成功分支前必须先停止 mDNS，再切换 WiFi 驱动。
    size_t probeLogPos = content.find("[WiFi-AP] Probing WiFi", fnPos);
    size_t stopPos = content.find("dnsManager->stopMDNS()", probeLogPos);
    size_t modeResetPos = content.find("WiFi.mode(WIFI_MODE_NULL)", probeLogPos);
    TEST_ASSERT_TRUE_MESSAGE(stopPos != std::string::npos && modeResetPos != std::string::npos,
        "AP->STA recovery must stop mDNS before switching WiFi driver mode");
    TEST_ASSERT_TRUE_MESSAGE(stopPos < modeResetPos,
        "mDNS must stop before WiFi.mode(WIFI_MODE_NULL)");

    // 定位成功分支（WiFi 已恢复，切回 STA）中的统一功能门控处理块
    size_t recoveredPos = content.find("WiFi recovered", fnPos);
    TEST_ASSERT_TRUE_MESSAGE(recoveredPos != std::string::npos,
        "AP->STA recovery success branch not found");

    // 从 shouldRunMDNS 判断句开始取小窗口（紧跟着的就是 mDNS 调用），
    // 避开前面长度不定的中文注释，不受注释长度影响
    size_t mdnsIfPos = content.find("if (shouldRunMDNS()", recoveredPos);
    TEST_ASSERT_TRUE_MESSAGE(mdnsIfPos != std::string::npos,
        "AP->STA recovery must guard mDNS restart with the runtime feature gate");

    std::string recoveryBlock = content.substr(mdnsIfPos, 120);

    // mDNS 已提前停止，恢复后应在 STA netif 就绪时重新启动
    TEST_ASSERT_TRUE_MESSAGE(
        recoveryBlock.find("->startMDNS") != std::string::npos,
        "AP->STA recovery must call dnsManager->startMDNS after STA interface is ready");

    // 恢复块无需 restartMDNS；stop/start 的顺序已在接口切换前后明确拆开。
    TEST_ASSERT_TRUE_MESSAGE(
        recoveryBlock.find("->restartMDNS") == std::string::npos,
        "AP->STA recovery must not restart mDNS while the WiFi driver is transitioning");
}

/**
 * @brief 源码回归：所有 STA/AP 回退入口统一在切换 Wi-Fi 驱动前停止 mDNS。
 * 经典 ESP32 的 mDNS 任务可能仍持有旧 STA netif；若直接切到 AP，
 * esp_netif_is_netif_up() 会在后台任务中解引用失效指针并触发 LoadProhibited。
 */
void test_source_ap_fallback_stops_mdns_before_driver_switch() {
    TestLog::testStart("Source: AP fallback stops mDNS before WiFi mode switch");

    std::string content = readSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read NetworkManager.cpp");

    size_t fnPos = content.find("bool FBNetworkManager::startAPMode()");
    TEST_ASSERT_TRUE_MESSAGE(fnPos != std::string::npos,
        "FBNetworkManager::startAPMode() not found");
    size_t fnEnd = content.find("\nvoid FBNetworkManager::stopAPMode()", fnPos);
    if (fnEnd == std::string::npos) fnEnd = fnPos + 1200;
    std::string fnBody = content.substr(fnPos, fnEnd - fnPos);

    size_t mdnsGuard = fnBody.find("isMDNSStarted()");
    size_t stopPos = fnBody.find("stopMDNS()", mdnsGuard);
    size_t wifiSwitch = fnBody.find("wifiManager->startAPMode()");
    TEST_ASSERT_TRUE_MESSAGE(mdnsGuard != std::string::npos &&
                             stopPos != std::string::npos &&
                             wifiSwitch != std::string::npos,
        "startAPMode must guard and stop mDNS before switching WiFi mode");
    TEST_ASSERT_TRUE_MESSAGE(stopPos < wifiSwitch,
        "mDNS must stop before WiFiManager::startAPMode() changes the driver mode");

    size_t restartPos = fnBody.find("dnsManager->startMDNS");
    TEST_ASSERT_TRUE_MESSAGE(restartPos != std::string::npos && restartPos > wifiSwitch,
        "startAPMode must restart mDNS after the AP interface is ready");
    TEST_ASSERT_TRUE_MESSAGE(fnBody.find("delay(200)", stopPos) != std::string::npos,
        "mDNS shutdown must include a settle window before netif replacement");
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
        size_t mdnsIfPos = content.find("if (shouldRunMDNS()", mpos);
        TEST_ASSERT_TRUE_MESSAGE(mdnsIfPos != std::string::npos,
            (std::string("runtime mDNS guard not found after: ") + marker).c_str());
        std::string block = content.substr(mdnsIfPos, 120);
        TEST_ASSERT_TRUE_MESSAGE(
            block.find("->restartMDNS") != std::string::npos,
            (std::string("must use restartMDNS after: ") + marker).c_str());
        TEST_ASSERT_TRUE_MESSAGE(
            block.find("->startMDNS") == std::string::npos,
            (std::string("must NOT use startMDNS after: ") + marker).c_str());
    }
}

/**
 * @brief 源码回归：AP 模式 startAPMode() 必须调用 WiFi.setSleep(false)
 * 关闭省电。STA 路径已在 connectToWiFiBlocking()/connectToWiFi() 中设置，
 * 但 AP 路径曾经遗漏，导致 AP 省电模式下无线帧缓存等待 DTIM 周期，
 * Web 响应延迟翻倍（实测登录后页面加载 >5s）。
 */
void test_source_ap_mode_disables_wifi_sleep() {
    TestLog::testStart("Source: startAPMode() calls WiFi.setSleep(false)");

    std::string content = readSrcFile("src/network/WiFiManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read WiFiManager.cpp");

    // 定位 startAPMode() 函数体
    size_t fnPos = content.find("bool WiFiManager::startAPMode()");
    TEST_ASSERT_TRUE_MESSAGE(fnPos != std::string::npos,
        "startAPMode() function not found in WiFiManager.cpp");

    // 截取函数体（startAPMode 函数约 120 行 / 7000+ 字符）
    // 找到函数结束的 } 匹配（简单截取到下一个 bool/void 函数定义）
    size_t fnEnd = content.find("\nvoid WiFiManager::stopAPMode()", fnPos);
    if (fnEnd == std::string::npos) fnEnd = fnPos + 8000;
    std::string fnBody = content.substr(fnPos, fnEnd - fnPos);

    // 必须在 softAP() 之后调用 setSleep(false)
    size_t softAPPos = fnBody.find("WiFi.softAP(");
    TEST_ASSERT_TRUE_MESSAGE(softAPPos != std::string::npos,
        "WiFi.softAP() call not found in startAPMode()");

    size_t sleepPos = fnBody.find("setSleep(false)");
    TEST_ASSERT_TRUE_MESSAGE(sleepPos != std::string::npos,
        "WiFi.setSleep(false) must be called in startAPMode() to disable AP power save");
    TEST_ASSERT_TRUE_MESSAGE(sleepPos > softAPPos,
        "setSleep(false) must come AFTER softAP() to ensure AP radio is configured first");
}

/**
 * @brief 源码回归：轻量 /api/batch 系统信息必须携带内存护栏等级。
 * 仪表盘使用该批量接口，缺少 guardLevel 会导致前端无法提前降频，
 * 只能等重型请求返回 503 后才进入保护提示。
 */
void test_source_batch_system_info_reports_memory_guard() {
    TestLog::testStart("Source: compact batch system info reports memory guard level");

    std::string content = readSrcFile("src/network/handlers/BatchRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read BatchRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(content.find("core/FastBeeFramework.h") != std::string::npos,
        "BatchRouteHandler must access the framework health monitor");
    TEST_ASSERT_TRUE_MESSAGE(content.find("systems/HealthMonitor.h") != std::string::npos,
        "BatchRouteHandler must include HealthMonitor for guard level reporting");
    TEST_ASSERT_TRUE_MESSAGE(content.find("memory[\"guardLevel\"]") != std::string::npos,
        "Compact system info must expose memory.guardLevel");
    TEST_ASSERT_TRUE_MESSAGE(content.find("monitor->getMemoryGuardLevel()") != std::string::npos,
        "Compact system info must read the current guard level");
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
    RUN_TEST(test_source_ap_probe_prescan_fast_fail);
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
    RUN_TEST(test_source_ap_fallback_uses_probe_without_periodic_reboot);
    RUN_TEST(test_source_sta_lost_reboot_constants_defined);
    RUN_TEST(test_source_gateway_ping_failure_is_advisory);
    RUN_TEST(test_source_sta_driver_restart_keeps_radio_started);
    RUN_TEST(test_source_sta_only_policy_disables_ap_fallback);
    RUN_TEST(test_source_status_sync_preserves_network_manager_reconnect_count);
    RUN_TEST(test_c6_test_profile_disables_ap_fallback);

    // AP→STA 恢复重新绑定 mDNS（fastbee.local 在 STA 下可解析）
    RUN_TEST(test_source_ap_recovery_restarts_mdns);
    RUN_TEST(test_source_ap_fallback_stops_mdns_before_driver_switch);
    RUN_TEST(test_source_mdns_rebind_premise_intact);
    RUN_TEST(test_source_interface_switch_restarts_mdns);

    // AP 模式省电关闭回归保护
    RUN_TEST(test_source_ap_mode_disables_wifi_sleep);
    RUN_TEST(test_source_batch_system_info_reports_memory_guard);
}
